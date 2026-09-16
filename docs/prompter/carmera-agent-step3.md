# Camera Agent AI Metadata输出——第二部分

## 1. 前置条件

Camera Agent已经完成第一阶段：

```text
Camera
 ↓
GStreamer
 ├── Video → H264 → RTSP
 │
 └── AI → 5fps → YOLO → ByteTrack
                         ↓
                    AIFrameResult
```

当前Camera Agent内部已经可以获得：

```text
frame_id
timestamp
video_width
video_height
objects
    class
    confidence
    track_id
    bbox
```

本阶段只增加：

```text
AIFrameResult
      ↓
Metadata Encoder
      ↓
Metadata Transport
      ↓
Server
```

---

# 2. 绝对约束

本阶段：

**只修改 Camera Agent。**

不得修改：

* Video Server
* MediaMTX
* Web
* 客户端
* Alarm Engine
* 微信通知
* 邮件通知

服务器端协议只是按照本阶段定义的格式预留。

---

# 3. 不允许修改视频流

现有：

```text
1080P30
 ↓
H264
 ↓
RTSP
```

必须完全保持。

Metadata发送异常：

```text
Server断开
网络异常
发送超时
JSON编码失败
```

都不能影响：

```text
Camera
GStreamer
H264
RTSP
```

---

# 4. Metadata设计

定义标准JSON：

```json
{
    "camera_id": "camera_001",
    "frame_id": 15230,
    "timestamp": 1756773210123,
    "video_width": 1920,
    "video_height": 1080,
    "objects": [
        {
            "class": "person",
            "confidence": 0.93,
            "track_id": 17,
            "bbox": [812, 210, 1040, 850]
        }
    ]
}
```

字段说明：

```text
camera_id
    摄像头唯一ID

frame_id
    对应视频帧编号

timestamp
    视频时间基准

video_width
    原始视频宽度

video_height
    原始视频高度

objects
    当前帧检测结果
```

Object：

```text
class
confidence
track_id
bbox
```

---

# 5. bbox规范

bbox必须使用：

```text
1920×1080
```

原始视频坐标。

格式：

```text
[x1, y1, x2, y2]
```

例如：

```text
[812,210,1040,850]
```

必须满足：

```text
0 <= x1 < x2 <= video_width
0 <= y1 < y2 <= video_height
```

如果检测结果超出范围，需要进行clamp。

---

# 6. Metadata发送线程

不能在AI推理线程中直接执行阻塞网络发送。

错误：

```text
YOLO
 ↓
JSON
 ↓
HTTP发送
 ↓
等待Server
 ↓
继续YOLO
```

正确：

```text
YOLO
 ↓
ByteTrack
 ↓
AIFrameResult
 ↓
Metadata Queue
 ↓
Metadata Sender Thread
 ↓
Server
```

网络发送必须异步化。

---

# 7. Metadata Queue

建议：

```text
queue_size = 5~10
```

不能无限增长。

如果Server网络异常：

```text
Metadata Queue
```

不能无限堆积。

超过限制：

```text
丢弃旧Metadata
```

或者根据最新帧优先原则丢弃旧数据。

不能影响：

```text
AI Pipeline
Video Pipeline
```

---

# 8. 网络发送

实现独立的：

```text
MetadataSender
```

不要将网络代码直接写进：

```text
Detector
Tracker
```

推荐结构：

```text
Detector
    ↓
Tracker
    ↓
AIFrameResult
    ↓
MetadataManager
    ↓
MetadataSender
```

---

# 9. Server地址配置

Camera Agent增加配置：

```text
METADATA_ENABLE=true
METADATA_SERVER_URL=http://server:port/...
CAMERA_ID=camera_001
METADATA_RETRY_INTERVAL=...
METADATA_QUEUE_SIZE=...
```

不得硬编码服务器地址。

---

# 10. 断线重连

Server断开时：

```text
AI继续运行
视频继续运行
```

MetadataSender：

```text
连接失败
 ↓
等待
 ↓
重连
 ↓
恢复发送
```

不要因为服务器断开导致Camera Agent退出。

---

# 11. 发送失败处理

网络发送失败：

```text
[WARN] metadata send failed
```

然后：

```text
retry
```

但不能：

```text
阻塞AI
```

也不能：

```text
阻塞视频
```

---

# 12. Metadata频率

AI当前：

```text
5fps
```

正常情况下：

```text
Metadata ≈ 5 messages/s
```

不能发送30fps AI数据。

如果当前帧没有检测目标：

```json
{
    "camera_id": "camera_001",
    "frame_id": 15236,
    "timestamp": 1756773210323,
    "video_width": 1920,
    "video_height": 1080,
    "objects": []
}
```

仍然允许发送空结果。

这样服务器可以知道：

```text
AI仍然在线
```

---

# 13. AI心跳

建议增加独立AI状态信息：

```text
AI_ENABLE
AI_RUNNING
AI_FPS
MODEL
TRACKER
LAST_FRAME_ID
LAST_TIMESTAMP
```

如果项目已有设备心跳机制，优先复用。

不要重复创建多套心跳系统。

---

# 14. Metadata版本

协议必须带版本信息。

例如：

```json
{
    "version": 1,
    "camera_id": "camera_001",
    "frame_id": 15230,
    ...
}
```

以后扩展：

```text
version=2
```

避免协议无法兼容。

---

# 15. 时间和视频同步

Metadata：

```text
frame_id
timestamp
```

必须来自第一阶段统一的AIFrameResult。

禁止MetadataSender自行重新生成：

```text
frame_id
timestamp
```

否则会导致：

```text
视频帧
≠
AI帧
```

---

# 16. Metadata传输协议

当前Camera Agent只需要实现一个清晰的Transport抽象：

```text
IMetadataTransport
```

例如：

```text
HttpMetadataTransport
```

后续可以扩展：

```text
WebSocketMetadataTransport
MqttMetadataTransport
```

但本阶段只实现项目实际需要的一种。

如果服务器协议尚未最终确定，优先实现：

```text
HTTP POST JSON
```

并将Transport接口抽象出来。

---

# 17. 日志

增加：

```text
[METADATA] sender started
[METADATA] connected
[METADATA] frame=15230 objects=1
[METADATA] send success
[METADATA] send failed
[METADATA] reconnecting
```

支持日志等级。

不要每帧默认打印完整JSON。

DEBUG模式才允许打印完整Metadata。

---

# 18. 性能监控

增加：

```text
metadata fps
send latency
send failures
queue size
dropped metadata
reconnect count
```

例如：

```text
[METADATA]
fps=5
latency=8ms
queue=0
failed=0
dropped=0
```

---

# 19. Camera Agent最终结构

最终形成：

```text
Camera Agent
│
├── Camera Manager
│
├── Video Pipeline
│     └── 1080P30 → H264 → RTSP
│
├── AI Pipeline
│     ├── Frame Sampler
│     ├── YOLO Detector
│     └── ByteTrack Tracker
│
├── AI Result
│     └── AIFrameResult
│
└── Metadata
      ├── Encoder
      ├── Queue
      └── Sender
             ↓
          Video Server
```

---

# 20. 第二阶段完成后的完整数据流

```text
Camera
 │
 ├─────────────────────────────┐
 │                             │
 ▼                             ▼
Video Pipeline              AI Pipeline
 │                             │
1080P30                       5fps
 │                             │
H264                          YOLO
 │                             │
RTSP                       ByteTrack
 │                             │
 │                         AIFrameResult
 │                             │
 │                         Metadata
 │                             │
 └──────────────┐       ┌──────┘
                │       │
                ▼       ▼
              Server / Video Server
```

---

# 21. 本阶段禁止事项

禁止：

1. 修改RTSP视频流。
2. 修改H264编码参数。
3. 修改MediaMTX。
4. 修改服务器代码。
5. 修改Web客户端。
6. 在AI线程同步发送网络数据。
7. 无限Metadata队列。
8. Server断开导致Camera Agent退出。
9. Metadata发送失败导致AI停止。
10. Metadata发送失败导致视频停止。
11. 重复生成frame_id。
12. 修改已经定义好的AIFrameResult语义。

---

# 22. 验收标准

必须验证：

### 测试1

```text
视频正常
RTSP正常
AI正常
Metadata正常
```

### 测试2

Server关闭：

```text
视频正常
AI正常
Metadata自动重连
```

### 测试3

网络断开：

```text
视频不崩溃
AI不崩溃
Metadata进入重连
```

### 测试4

Server恢复：

```text
Metadata自动恢复发送
```

### 测试5

AI异常：

```text
视频继续
Metadata停止
Camera Agent继续运行
```

### 测试6

AI关闭：

```text
视频正常
Camera Agent正常
```

---

# 23. 最终目标

本阶段完成后，Camera Agent具备：

```text
1080P30视频采集
        ↓
H264
        ↓
RTSP
```

以及独立：

```text
5fps
 ↓
YOLO
 ↓
ByteTrack
 ↓
AIFrameResult
 ↓
JSON Metadata
 ↓
异步发送
 ↓
Video Server
```

并保证：

> AI、Metadata网络通信出现任何异常，都不能影响原有1080P30 RTSP视频流。

完成后输出：

1. 修改文件
2. 新增文件
3. Metadata协议
4. Transport实现
5. Queue设计
6. 重连机制
7. 配置参数
8. 性能测试
9. 网络异常测试
10. AI异常测试
11. 视频稳定性测试
12. 后续服务器对接说明
