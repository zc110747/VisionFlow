# Video Server：中心视频服务器 + Web管理平台

## 一、项目目标

实现一个完整的中心视频服务器。

它接收多个 Camera Agent 的 RTSP 推流，并提供：

```text
RTSP接入
视频流管理
RTSP转发
WebRTC访问
HTTP Server
REST API
Web UI
Camera管理
状态监控
配置管理
```

最终用户只需要运行：

```text
video-server
```

然后访问：

```text
http://server:8080
```

即可使用完整系统。

不要求额外运行：

```text
npm run dev
```

不要求用户单独部署 Web Server。

---

# 二、总体架构

```text
Camera Agent
      │
      │ RTSP Push
      ▼
┌────────────────────────────────────┐
│           Video Server             │
│                                    │
│ ┌──────────────┐                   │
│ │   MediaMTX   │                   │
│ │              │                   │
│ │ RTSP Server  │                   │
│ │ RTSP Relay   │                   │
│ │ WebRTC       │                   │
│ └───────┬──────┘                   │
│         │                          │
│         ▼                          │
│    Stream Manager                  │
│                                    │
│ ┌────────────────────────────────┐ │
│ │ HTTP Server                    │ │
│ │                                │ │
│ │ REST API                       │ │
│ │ Web UI                         │ │
│ │ Static Assets                  │ │
│ └────────────────────────────────┘ │
└───────────────┬────────────────────┘
                │
       ┌────────┴─────────┐
       ▼                  ▼
    Browser            PC Client
    WebRTC               RTSP
```

---

# 三、技术栈

后端：

* Go
* MediaMTX
* SQLite
* HTTP
* REST API

前端：

* Vue 3
* TypeScript
* Vite
* Element Plus

媒体：

* RTSP
* RTP
* WebRTC
* H.264

禁止自行实现：

```text
RTSP协议
RTP协议
WebRTC Media Server
H264 RTP封装
```

底层媒体服务优先使用 MediaMTX。

---

# 四、Video Server必须是单体启动入口

最终：

```text
video-server
```

负责：

```text
启动数据库
启动MediaMTX
启动HTTP Server
加载Web UI
启动Camera监控
启动REST API
```

用户不需要分别启动：

```text
MediaMTX
Web Server
Vue
```

开发环境允许独立调试，但生产模式必须一条命令启动。

---

# 五、MediaMTX

将 MediaMTX 作为视频媒体核心。

Video Server负责管理它。

Camera Agent 推流：

```text
rtsp://video-server:8554/camera01
```

服务器自动识别：

```text
camera01
camera02
camera03
```

支持多个Camera同时在线。

---

# 六、RTSP访问

PC客户端可以直接访问：

```text
rtsp://server:8554/camera01
```

必须兼容：

```text
VLC
ffplay
FFmpeg
GStreamer
Qt客户端
```

Video Server不能破坏原始H.264码流。

原则：

```text
Camera Agent
    ↓
H264
    ↓
RTSP
    ↓
Video Server
    ↓
RTSP Client
```

默认不进行重新编码。

---

# 七、WebRTC

浏览器访问：

```text
http://server:8080
```

Web页面通过WebRTC播放视频。

流程：

```text
Browser
   ↓
HTTP REST API
   ↓
获取Camera Stream信息
   ↓
WebRTC
   ↓
MediaMTX
   ↓
RTSP Stream
```

浏览器不能直接访问RTSP。

WebRTC必须尽量复用Camera Agent原始H.264码流。

不要进行服务器二次编码。

---

# 八、Web UI

Web UI必须属于 Video Server。

最终编译：

```text
Vue
 ↓
npm run build
 ↓
dist/
 ↓
Go embed
 ↓
video-server
```

Go使用：

```go
//go:embed
```

嵌入静态文件。

最终：

```text
video-server
```

一个二进制即可提供：

```text
HTTP
REST API
Web UI
```

---

# 九、Web页面

首页：

```text
Video Server
```

显示：

```text
系统状态

Camera数量
Online数量
Offline数量

Camera列表
```

Camera卡片：

```text
┌────────────────────┐
│ Camera 01          │
│ ● ONLINE            │
│                    │
│     VIDEO          │
│                    │
│ 1280×720  30 FPS   │
└────────────────────┘
```

点击进入：

```text
/camera/camera01
```

显示：

```text
Camera 01
ONLINE

┌──────────────────────────────┐
│                              │
│            VIDEO             │
│                              │
└──────────────────────────────┘

Resolution: 1280x720
FPS: 30
Bitrate: 4 Mbps

[Fullscreen]
```

---

# 十、REST API

实现：

```text
GET    /api/health

GET    /api/cameras
GET    /api/cameras/{id}
POST   /api/cameras
PUT    /api/cameras/{id}
DELETE /api/cameras/{id}

GET    /api/cameras/{id}/status
GET    /api/cameras/{id}/stream
GET    /api/cameras/{id}/webrtc
```

返回JSON。

例如：

```json
{
  "id": "camera01",
  "name": "Camera 01",
  "status": "online",
  "stream_path": "camera01",
  "rtsp_url": "rtsp://server:8554/camera01"
}
```

---

# 十一、Camera自动注册

当新的Camera Agent开始：

```text
RTSP Push
```

Video Server检测到：

```text
camera01
```

如果数据库没有：

```text
camera01
```

自动创建。

默认：

```text
name = camera01
status = online
```

未来可增加人工绑定设备功能。

---

# 十二、Camera状态

状态：

```text
ONLINE
OFFLINE
CONNECTING
ERROR
```

检测：

```text
RTSP Path存在
+
收到视频数据
```

如果超过设定时间没有视频数据：

```text
ONLINE → OFFLINE
```

恢复：

```text
OFFLINE → ONLINE
```

Web页面自动刷新状态。

---

# 十三、SQLite

数据库：

```text
data/video.db
```

表：

```text
cameras
```

字段：

```text
id
name
stream_path
device_ip
status
resolution
fps
bitrate
created_at
updated_at
last_seen
```

未来预留：

```text
users
permissions
recordings
events
```

---

# 十四、API与Web分离

后端：

```text
/api/*
```

前端：

```text
/*
```

例如：

```text
GET /
GET /assets/*
GET /camera/camera01

GET /api/cameras
GET /api/cameras/camera01
```

不要让Web UI直接操作数据库。

---

# 十五、配置

使用：

```text
config/config.yaml
```

例如：

```yaml
server:
  http_port: 8080

rtsp:
  port: 8554

webrtc:
  port: 8889

database:
  path: data/video.db

mediamtx:
  binary: ./mediamtx/mediamtx
  config: ./config/mediamtx.yml
```

所有端口不能硬编码。

---

# 十六、跨平台

第一阶段支持：

```text
Windows
Linux
```

最终重点支持：

```text
Linux
```

未来可以部署：

```text
Ubuntu
Debian
Rocky Linux
Docker
```

---

# 十七、日志

必须记录：

```text
Server启动
MediaMTX启动
Camera上线
Camera离线
RTSP连接
RTSP断开
WebRTC客户端
REST API
异常
```

支持：

```text
INFO
WARN
ERROR
DEBUG
```

---

# 十八、健康检查

：

```text
GET /api/health
```

返回：

```json
{
  "status": "ok",
  "database": "ok",
  "media_server": "ok"
}
```

如果MediaMTX异常：

```json
{
  "status": "error",
  "database": "ok",
  "media_server": "error"
}
```

---

# 十九、WebRTC自动重连

Web页面：

```text
CONNECTED
 ↓
DISCONNECTED
 ↓
RECONNECTING
 ↓
CONNECTED
```

浏览器不能因为短暂网络异常要求用户刷新页面。

---

# 二十、多客户端

必须支持：

```text
Browser × N
RTSP Client × N
```

同时访问：

```text
camera01
```

不允许因为一个客户端断开导致Camera Stream停止。

---

# 二十一、三个Camera验收

同时运行：

```text
camera01
camera02
camera03
```

Video Server显示：

```text
Camera 01 ONLINE
Camera 02 ONLINE
Camera 03 ONLINE
```

浏览器：

```text
http://localhost:8080
```

显示三个Camera。

同时：

```text
ffplay rtsp://localhost:8554/camera01
```

可以播放Camera01。

浏览器也可以同时播放Camera01。

---

# 二十二、目录结构

```text
video-server/
├── go.mod
├── README.md
├── config/
│   ├── config.yaml
│   └── mediamtx.yml
├── cmd/
│   └── video-server/
│       └── main.go
├── internal/
│   ├── api/
│   ├── camera/
│   ├── stream/
│   ├── database/
│   ├── mediamtx/
│   ├── monitor/
│   ├── config/
│   └── server/
├── web/
│   ├── package.json
│   ├── vite.config.ts
│   └── src/
│       ├── api/
│       ├── components/
│       ├── views/
│       ├── router/
│       └── webrtc/
├── data/
├── mediamtx/
├── scripts/
└── tests/
```

---

# 二十三、开发顺序

严格按照：

```text
Phase 1
MediaMTX启动

Phase 2
Camera Agent RTSP接入

Phase 3
VLC/ffplay验证

Phase 4
Go HTTP Server

Phase 5
SQLite

Phase 6
Camera自动发现

Phase 7
Camera状态监控

Phase 8
REST API

Phase 9
Vue Web UI

Phase 10
WebRTC

Phase 11
WebRTC自动重连

Phase 12
多Camera

Phase 13
多客户端

Phase 14
完整集成测试
```

每完成一个阶段必须：

```text
编译
→ 运行
→ 测试
→ 修复
```

---

# 二十四、重要原则

Video Server是整个系统的中心。

Camera Agent不提供Web服务。

Browser不直接连接Camera Agent。

PC Client不直接连接Camera Agent。

所有访问统一：

```text
Camera Agent
       ↓
Video Server
       ↓
┌──────┴──────┐
Browser     PC Client
WebRTC         RTSP
```

未来增加：

```text
录像
截图
AI分析
用户权限
设备管理
告警
```

都应该在Video Server层扩展。
