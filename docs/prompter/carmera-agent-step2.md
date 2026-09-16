# Camera Agent AI 人员检测与跟踪——第一部分

## 1. 项目背景

当前 Camera Agent 已经实现稳定的视频采集和推流：

```text
Camera
  ↓
GStreamer
  ↓
720p@30fps
  ↓
H.264
  ↓
RTSP
```

该视频流已经能够正常提供给现有 Video Server / MediaMTX 使用。

本次任务只修改 **Camera Agent**，增加本地 AI 人员检测和人员跟踪能力。

### 本阶段明确不做：

* 不修改 Video Server
* 不修改 MediaMTX
* 不修改 Web
* 不实现 WebSocket
* 不实现报警
* 不实现微信通知
* 不实现邮件通知
* 不实现服务器 AI Metadata 接口
* 不改变现有 RTSP 协议
* 不改变现有视频编码参数

---

# 2. 核心目标

在现有视频 pipeline 基础上增加独立 AI pipeline：

```text
                    ┌──→ H.264 → RTSP
                    │
Camera / Video ─────┤
                    │
                    └──→ 5fps → YOLO → ByteTrack
                                      ↓
                                  AI Results
```

最终 Camera Agent 内部同时运行：

```text
Video Pipeline
    720p@30fps
        ↓
      H.264
        ↓
       RTSP


AI Pipeline
    720p@30fps source
        ↓
    sample 5fps
        ↓
      YOLO
        ↓
    ByteTrack
        ↓
    Detection/Tracking Results
```

低于10fps的摄像头，则每帧都进行计算。

---

# 3. 最重要的架构约束

## 3.1 不允许破坏原视频pipeline

现有：

```text
Camera
 ↓
GStreamer
 ↓
H264
 ↓
RTSP
```

必须保持正常运行。

AI加入以后：

```text
AI异常
AI模型加载失败
Tracker异常
AI处理超时
AI线程退出
```

都不能导致：

```text
视频停止
RTSP断开
GStreamer pipeline崩溃
Camera Agent退出
```

---

# 4. AI必须与视频解耦

禁止：

```text
Camera
 ↓
YOLO
 ↓
画框
 ↓
H264
 ↓
RTSP
```

禁止把AI作为H264编码前的同步阻塞处理。

必须：

```text
Camera
 │
 ├──────────────→ Video Pipeline
 │                   ↓
 │                  H264
 │                   ↓
 │                  RTSP
 │
 └──────────────→ AI Pipeline
                     ↓
                    5fps
                     ↓
                    YOLO
                     ↓
                 ByteTrack
```

AI不能成为视频pipeline的性能瓶颈。

---

# 5. AI处理频率

视频：

```text
1920×1080 @ 30fps
```

AI：

```text
5fps
```

第一版直接：

```text
30 / 6 = 5fps
```

每6帧取1帧。

不允许让YOLO处理30fps。

---

# 6. AI帧获取

优先复用现有GStreamer视频pipeline。

不要为了AI重新打开Camera设备：

```text
/dev/video0
```

避免：

```text
Video Pipeline → /dev/video0
AI Pipeline    → /dev/video0
```

造成设备抢占或USB摄像头兼容性问题。

优先考虑：

```text
Camera
 ↓
GStreamer
 ↓
tee
 ├── Video Branch
 │
 └── AI Branch
      ↓
   frame sample
      ↓
     5fps
```

如果现有pipeline结构不适合tee，则设计独立的帧共享机制，但不得破坏现有视频pipeline。

---

# 7. AI线程模型

AI不得运行在GStreamer视频主线程中。

推荐：

```text
Video Thread
    │
    └── 720p@30fps RTSP

AI Capture/Queue
    │
    └── 5fps

AI Inference Thread
    │
    └── YOLO

Tracker Thread
    │
    └── ByteTrack
```

可以根据现有工程合理合并AI线程，但必须保证：

```text
AI阻塞
    ↓
Video不阻塞
```

---

# 8. AI队列

AI队列禁止无限增长。

建议：

```text
queue_size = 1~2
```

如果YOLO处理速度低于5fps：

```text
丢弃旧帧
保留最新帧
```

禁止：

```text
Frame1
Frame2
Frame3
Frame4
...
无限排队
```

避免产生越来越大的AI延迟。

---

# 9. YOLO检测

使用YOLO作为目标检测器。

当前只需要：

```text
person
```

其他类别暂时可以忽略。

YOLO输出：

```text
class
confidence
bbox
```

例如：

```text
class = person
confidence = 0.93
bbox = [812,210,1040,850]
```

---

# 10. bbox坐标

YOLO输入尺寸可能为：

```text
640×640
```

但是最终检测结果必须转换回原始视频坐标：

```text
1920×1080
```

例如：

```text
bbox = [x1,y1,x2,y2]
```

必须表示：

```text
1920×1080视频坐标
```

不能直接使用640×640坐标。

必须正确处理：

* resize
* letterbox
* scale
* padding

---

# 11. Tracker

使用：

```text
ByteTrack
```

YOLO负责：

```text
Detection
```

ByteTrack负责：

```text
Tracking
```

最终产生：

```text
track_id
```

例如：

```text
Frame 100
person → track_id=1

Frame 106
person → track_id=1

Frame 112
person → track_id=1
```

同一个目标应尽可能保持相同track_id。

第一阶段禁止引入：

* DeepSORT
* ReID
* BoT-SORT
* 人脸识别

保持系统简单。

---

# 12. Camera Agent内部统一数据结构

虽然本阶段不上传服务器，但是必须提前定义统一内部数据结构。

建议：

```cpp
struct AIObject {
    std::string class_name;
    float confidence;

    int track_id;

    float x1;
    float y1;
    float x2;
    float y2;
};

struct AIFrameResult {
    uint64_t frame_id;
    uint64_t timestamp;

    int video_width;
    int video_height;

    std::vector<AIObject> objects;
};
```

如果项目语言不是C++，按照当前工程语言实现等价结构。

这个数据结构必须作为后续第二阶段 Metadata 输出的基础。

---

# 13. frame_id

必须为视频帧建立稳定的frame_id。

例如：

```text
Frame 0
Frame 1
Frame 2
...
Frame 1000
```

AI处理Frame 1000后：

```text
AIFrameResult.frame_id = 1000
```

不能使用：

```text
AI结果产生时间
```

代替frame_id。

---

# 14. timestamp

AI结果同时保存时间戳：

```text
timestamp
```

优先使用GStreamer PTS / pipeline clock等已有时间基准。

不要随意使用：

```text
time(NULL)
```

作为视频同步时间。

---

# 15. 本阶段调试输出

Camera Agent增加AI日志，例如：

```text
[AI] initialized
[AI] model loaded
[AI] detector started
[AI] tracker started

[AI] frame=120 objects=1
[AI] person confidence=0.93 track_id=1
[AI] bbox=(812,210,1040,850)
```

为了避免刷屏，正式运行时必须支持日志等级：

```text
DEBUG
INFO
WARN
ERROR
```

---

# 16. AI性能统计

增加统计信息：

```text
AI FPS
YOLO inference time
Tracker processing time
AI queue size
Dropped AI frames
Detected objects
```

例如：

```text
[AI] fps=5.0 inference=42ms tracker=2ms queue=1 dropped=0
```

---

# 17. 配置参数

不要把参数全部硬编码。

至少支持：

```text
AI_ENABLE=true
AI_FPS=5
AI_CONFIDENCE=0.5
AI_MODEL=...
AI_INPUT_WIDTH=640
AI_INPUT_HEIGHT=640
AI_QUEUE_SIZE=2
```

如果当前项目已有配置系统，必须复用。

---

# 18. AI_ENABLE行为

```text
AI_ENABLE=false
```

时：

```text
Video Pipeline
    ↓
正常运行
```

AI完全关闭。

```text
AI_ENABLE=true
```

时：

```text
Video Pipeline
    ↓
正常运行

AI Pipeline
    ↓
启动
```

---

# 19. AI异常隔离

必须保证：

```text
YOLO模型加载失败
```

不会：

```text
Camera Agent退出
```

而应该：

```text
[ERROR] AI model initialization failed
[WARN] AI disabled
```

然后：

```text
Video继续运行
```

Tracker异常同样处理。

---

# 20. PC阶段

当前先在PC环境验证。

PC上可以使用：

```text
YOLO
+
ByteTrack
```

进行验证。

但是必须进行模块抽象。

例如：

```text
IDetector
   │
   └── YOLODetector

ITracker
   │
   └── ByteTrackTracker
```

以后RK3568迁移：

```text
IDetector
   │
   └── RKNNYOLODetector
```

Server和Web完全不需要知道底层AI实现发生了变化。

---

# 21. 第一阶段验收标准

必须满足：

### 视频

```text
720p@30fps
RTSP正常
Web正常
```

### AI

```text
AI 5fps
YOLO正常
person检测正常
ByteTrack正常
track_id正常
```

### 并发

```text
AI运行
+
视频持续30fps
```

### 异常

```text
AI关闭
    ↓
视频正常

AI模型失败
    ↓
视频正常

AI处理超时
    ↓
视频正常
```

### 最终内部数据

能够产生：

```text
frame_id
timestamp
person
confidence
bbox
track_id
```

---

# 22. 本阶段禁止修改范围

禁止修改：

```text
Video Server
MediaMTX
Web Client
WebSocket
Alarm Engine
Email
微信
服务器API
```

禁止新增服务器依赖。

本阶段只允许修改：

```text
Camera Agent
AI相关依赖
Camera Agent配置
Camera Agent测试代码
```

---

# 23. 开发原则

先分析当前Camera Agent代码。

不要直接重写。

优先：

```text
复用现有Camera采集
复用现有GStreamer pipeline
复用现有线程模型
复用现有配置系统
复用现有日志系统
```

采用最小侵入式修改。

完成后输出：

1. 修改文件
2. 新增文件
3. AI pipeline结构
4. YOLO实现
5. ByteTrack实现
6. 帧采样方式
7. frame_id生成方式
8. timestamp生成方式
9. AI队列设计
10. 配置参数
11. 性能数据
12. 测试结果
13. 已知问题
14. 第二阶段建议

最终要求：

> Camera Agent在保持原720p@30fps RTSP视频流完全不变的情况下，增加独立的5fps YOLO人员检测 + ByteTrack人员跟踪能力，并在Camera Agent内部产生标准化AIFrameResult，为第二阶段AI Metadata输出做好准备。
