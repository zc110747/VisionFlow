# Camera Agent：PC 摄像头模拟嵌入式 Linux 摄像头设备

## 一、项目目标

实现一个运行在 Windows PC 上的 Camera Agent，用 PC 本地摄像头完整模拟未来 RK3568 等嵌入式 Linux 设备上的摄像头采集程序。

未来真实设备架构：

```text
RK3568
  ↓
Camera / V4L2
  ↓
ISP
  ↓
Hardware H.264 Encoder
  ↓
RTSP Push
  ↓
Video Server
```

当前 PC 模拟器：

```text
Windows PC
  ↓
USB / Integrated Camera
  ↓
GStreamer
  ↓
H.264 Encoder
  ↓
RTSP Push
  ↓
Video Server
```

本项目只负责：

```text
摄像头采集
视频编码
RTSP推流
设备状态
自动重连
配置管理
```

本项目不负责：

```text
RTSP Server
Web Server
Web UI
WebRTC Server
数据库
用户管理
视频转发
```

---

# 二、技术栈

必须优先使用：

* C++
* CMake
* GStreamer
* Windows
* H.264
* RTSP
* spdlog 或等效成熟日志库

不要自行实现：

* H.264 Encoder
* RTP
* RTSP协议

全部优先使用 GStreamer。

---

# 三、工程要求

工程必须支持：

```text
cmake
cmake --build
```

Windows 优先使用：

```text
MSVC
```

同时保持代码尽量跨平台，为以后移植 Linux / RK3568 做准备。

禁止将 Windows API 大量散落在业务代码中。

摄像头采集层必须独立封装。

---

# 四、目录结构

建议：

```text
camera-agent/
├── CMakeLists.txt
├── README.md
├── LICENSE
├── config/
│   └── camera-agent.yaml
├── include/
│   └── camera_agent/
│       ├── camera_manager.h
│       ├── video_pipeline.h
│       ├── rtsp_publisher.h
│       ├── config.h
│       └── types.h
├── src/
│   ├── main.cpp
│   ├── camera/
│   │   └── camera_manager.cpp
│   ├── pipeline/
│   │   └── video_pipeline.cpp
│   ├── rtsp/
│   │   └── rtsp_publisher.cpp
│   ├── config/
│   │   └── config.cpp
│   └── common/
├── tests/
└── scripts/
```

---

# 五、摄像头管理

启动：

```text
camera-agent --list
```

列出：

```text
ID
Name
Supported Resolution
Supported FPS
```

例如：

```text
Camera 0
  Name: USB Camera
  Resolution:
    640x480
    1280x720
    1920x1080
  FPS:
    15
    30
```

支持指定摄像头：

```text
--camera 0
```

---

# 六、视频参数

默认：

```yaml
camera:
  id: 0
  width: 1280
  height: 720
  fps: 30

encoder:
  codec: h264
  bitrate: 4000
  keyframe_interval: 30

stream:
  id: camera01
```

支持：

```text
--width
--height
--fps
--bitrate
--camera
--stream
```

例如：

```text
camera-agent ^
  --camera 0 ^
  --width 1280 ^
  --height 720 ^
  --fps 30 ^
  --bitrate 4000 ^
  --stream camera01
```

---

# 七、GStreamer Pipeline

使用 GStreamer 完成：

```text
Camera
 ↓
Video Convert
 ↓
H264 Encoder
 ↓
H264 Parse
 ↓
RTP H264
 ↓
RTSP Publisher
```

Windows 下根据实际安装环境选择：

```text
ksvideosrc
dshowvideosrc
```

以及可用的 H.264 encoder。

优先：

```text
硬件H264 Encoder
```

如果 PC 环境不存在合适硬件编码器：

```text
x264enc
```

作为开发环境 fallback。

程序启动时必须检查插件。

如果插件不存在，给出明确错误：

```text
Required GStreamer plugin xxx is not installed.
```

不要崩溃。

---

# 八、RTSP Push

默认：

```text
rtsp://127.0.0.1:8554/camera01
```

支持：

```text
--server
--port
--stream
```

例如：

```text
rtsp://192.168.1.100:8554/camera01
```

Camera Agent 必须作为 RTSP Publisher。

不要实现 RTSP Server。

---

# 九、断线重连

如果：

```text
Video Server停止
```

Camera Agent不能退出。

必须：

```text
Disconnected
    ↓
等待
    ↓
Reconnect
    ↓
Connected
```

采用指数退避或者固定退避：

```text
1s
2s
5s
10s
```

最大10秒。

Video Server恢复后自动重新推流。

---

# 十、运行状态

程序运行期间显示：

```text
Camera:
  USB Camera

Video:
  1280x720
  30 FPS
  H264
  4000 kbps

Stream:
  camera01

RTSP:
  rtsp://127.0.0.1:8554/camera01

Status:
  STREAMING

Statistics:
  Frames: xxxx
  Dropped: xxxx
  Bitrate: xxxx
```

---

# 十一、设备模拟能力

提供设备ID：

```text
--device-id camera01
```

未来真实 RK3568 使用相同概念。

状态接口可以预留：

```text
device_id
device_name
firmware_version
camera_status
stream_status
resolution
fps
bitrate
```

当前不需要实现HTTP状态服务。

---

# 十二、日志

实现：

```text
INFO
WARN
ERROR
DEBUG
```

日志内容包括：

* 摄像头初始化
* GStreamer初始化
* Encoder初始化
* RTSP连接
* RTSP断开
* 重连
* FPS
* 丢帧
* Pipeline错误

支持：

```text
--log-level debug
```

---

# 十三、异常处理

必须正确处理：

```text
摄像头不存在
摄像头被其他程序占用
分辨率不支持
FPS不支持
GStreamer不存在
Encoder不存在
RTSP Server不存在
网络断开
服务器断开
程序退出
```

程序不能因为普通网络错误直接崩溃。

---

# 十四、优雅退出

支持：

```text
Ctrl+C
```

退出流程：

```text
停止采集
 ↓
停止Encoder
 ↓
停止RTSP
 ↓
释放GStreamer
 ↓
释放Camera
 ↓
退出
```

---

# 十五、测试

必须提供：

```text
tests/
```

至少验证：

1. 摄像头枚举
2. Pipeline创建
3. H264编码
4. RTSP连接
5. RTSP断开
6. 自动重连
7. 参数错误
8. 正常退出

---

# 十六、完整验收

启动 Video Server 后：

```text
camera-agent --camera 0 --stream camera01
```

使用：

```text
ffplay rtsp://127.0.0.1:8554/camera01
```

必须能够看到 PC 摄像头实时画面。

关闭 Video Server：

```text
Camera Agent继续运行
```

重新启动 Video Server：

```text
Camera Agent自动恢复推流
```

---

# 十七、开发要求

不要一次生成所有代码。

严格执行：

```text
Phase 1
环境检查

Phase 2
Camera枚举

Phase 3
Camera采集

Phase 4
H264编码

Phase 5
RTSP Push

Phase 6
自动重连

Phase 7
参数配置

Phase 8
完整测试
```

每个阶段：

```text
修改
→ 编译
→ 测试
→ 修复
→ 再进入下一阶段
```

禁止在无法编译的情况下继续堆积代码。

---

# 十八、未来 RK3568 移植要求

代码架构必须允许未来替换：

```text
Windows Camera
```

为：

```text
V4L2 Camera
```

同时保持：

```text
H264
RTSP Push
Stream ID
```

接口不变。

最终真实设备：

```text
V4L2
 ↓
RK3568 MPP/GStreamer
 ↓
H264
 ↓
RTSP
```

不应影响 Video Server。
