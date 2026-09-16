# video-server 端到端方案（Camera Agent AI Metadata 第二部分）

> 适用范围：本文件是相机 Agent 规范《Camera Agent AI Metadata输出——第二部分》在 **video-server 接收端** 的落地说明。
> 原始规范 §2 要求"本阶段只改 Camera Agent、服务器协议仅预留"；本文件描述**服务端按预留协议实现的接收/落库/查询**一侧，
> 与 camera-agent 侧互为镜像，共同构成 spec §20 的完整数据流。
>
> 配套源码：`internal/metadata/{model.go,repo.go,metadata_test.go}`、`internal/api/metadata.go`、`internal/api/handlers.go`。
> 验收脚本：`scripts/verify_metadata.py`、`scripts/verify_joint.py`、`scripts/verify_ai_resilience.py`、`scripts/verify_metadata_https.py`。

---

## 1. 端到端数据流

```text
Camera (UVC)
  └─ camera-agent.exe
       ├─ Video Pipeline  → GStreamer → H264(nvh264enc) → rtspclientsink
       │                                             │
       │                                             ▼
       │                                         MediaMTX :8554   ← video-server 拉起
       │                                             │
       └─ AI Pipeline (5fps) → YOLO → ByteTrack        │
            └─ AIFrameResult                           │
                 └─ Metadata Encoder (JSON, type 判别) │
                      └─ MetadataManager (有界队列)     │
                           └─ Sender Thread (WinHTTP POST)
                                │  HTTP POST /api/metadata (frame | status)
                                ▼
                         video-server (Go 单二进制)
                           ├─ POST /api/metadata  → 解析 type → 落库
                           ├─ SQLite: ai_status / ai_frame / ai_object
                           ├─ GET /api/cameras/{id}/metadata  → 单路快照
                           ├─ GET /api/metadata                → 全局概览
                           └─ monitor 每 3s 轮询 MediaMTX 控制 API → 自动注册摄像头
```

**两条铁律（与 agent 一致）**：
1. AI / Metadata 网络异常 **绝不** 影响 1080P30 H264 RTSP 视频流。
2. `frame_id` / `timestamp` 由采集侧产生，服务端**原样存储**，不重编号、不"修正"。

---

## 2. Metadata 协议（服务端接收契约）

`POST /api/metadata`，`Content-Type: application/json`，由 `type` 字段区分两类消息：

| `type` | 含义 | 频率 | 落表 |
|---|---|---|---|
| `frame` | 一次推理结果：帧号 + 检测框 | 默认 5/s | `ai_frame` + `ai_object` |
| `status` | AI 存活心跳 + 模型身份 | 默认 10s | `ai_status` |

```jsonc
// frame
{"version":1,"type":"frame","camera_id":"camera01","frame_id":719,"timestamp":25269,
 "video_width":1280,"video_height":720,
 "objects":[{"class":"person","confidence":0.87,"track_id":7,"bbox":[10,20,100,200]}]}

// status
{"version":1,"type":"status","camera_id":"camera01","wall_clock":1788356203728,
 "ai":{"enable":true,"running":true,"fps":5.01,"model":"models/yolov8n.onnx",
       "tracker":"bytetrack","last_frame_id":571,"last_timestamp":20265,"processed":99}}
```

字段语义见原始规范 §4/§5/§14/§15：bbox 为原始像素 `[x1,y1,x2,y2]`，需满足 `0<=x1<x2<=width`、
`0<=y1<y2<=height`；agent 已 clamp，服务端**再夹一次**做防御。

---

## 3. spec §23 交付物映射（服务端视角）

| # | 交付物 | 服务端实现 |
|---|---|---|
| 1 | 修改文件 | `internal/api/metadata.go`（新增接收端点）、`internal/api/handlers.go`、`internal/api/router.go`、`internal/server/server.go`、`internal/config/config.go`、`internal/database/db.go`、`cmd/video-server/main.go` |
| 2 | 新增文件 | `internal/metadata/model.go`、`internal/metadata/repo.go`、`internal/metadata/metadata_test.go`、`scripts/verify_metadata.py`、`scripts/verify_joint.py`、`scripts/verify_ai_resilience.py`、`scripts/verify_metadata_https.py` |
| 3 | Metadata 协议 | 见 §2：`POST /api/metadata`，`type ∈ {frame,status}`，`version` 字段必备；旧版无 `type` 由 `InferType()` 按载荷形状兜底（§10.5） |
| 4 | Transport 实现 | 服务端侧 Transport = HTTP `POST /api/metadata` + JSON 解码 + `type` 路由；agent 侧为 `HttpMetadataTransport`（WinHTTP）。协议契约统一，互不依赖实现细节 |
| 5 | Queue 设计 | 服务端不维护发送队列，而是**有界落库**：`ai_object` 按 `retention_rows`（默认 2000/路）滚动裁剪，`ai_frame` 仅留最新一帧。与 agent 侧有界队列（默认 8，丢最老保最新）解耦，断服时 agent 队列不膨胀 |
| 6 | 重连机制 | agent 侧指数退避重连；服务端 `metadata.enabled=false` 时返回 `503 + Retry-After:30`，agent 据此退避。服务端对重连透明，无需状态机 |
| 7 | 配置参数 | `metadata.enabled` / `retention_rows` / `max_body_bytes` / `require_known_camera`（见 README §10.3）。**无硬编码地址**，全部走 YAML |
| 8 | 性能测试 | `verify_metadata.py` 第 10 组：5 msg/s 持续 10s，44 条全收，最新帧即末帧 |
| 9 | 网络异常测试 | `verify_ai_resilience.py` scenario7：杀掉 video-server（含 MediaMTX）→ agent 进程存活且进入重连/退避 → 重启后 metadata 心跳自动恢复、`frame_id` 越过断点（48→50）；`verify_joint.py` 第 11 组佐证媒体路径不受影响 |
| 10 | AI 异常测试 | `verify_ai_resilience.py` scenario5（坏模型 → 视频 STREAMING + H264 + 心跳 `enable=true/running=false`）/ scenario6（`--no-ai` → 视频正常 + 心跳 `enable=false/running=false`） |
| 11 | 视频稳定性测试 | 全部 scenario 中 ffprobe 均解出 `h264 1280x720`、fps=30，证视频流不受 AI/Metadata 影响 |
| 12 | 后续服务器对接说明 | 见 §4：Web UI / 告警引擎 / 微信 / 邮件通过 `GET /api/cameras/{id}/metadata` 拉取快照，或 `GET /api/metadata` 拉全局概览；视频流走 RTSP/WebRTC/HLS，与 Metadata 通道正交 |

---

## 4. 后续服务器对接说明

- **Web UI / 客户端**：轮询 `GET /api/cameras/{id}/metadata` 拿 `{frame,status}` 渲染检测框；未知路返回空快照（`frame/status=null`），前端无需区分"没数据/没这路"。
- **Alarm Engine / 微信 / 邮件**：订阅 `ai_object` 明细（按 `retention_rows` 滚动保留），或读 `ai_status` 心跳判定 AI 在线。
- **视频通道独立**：RTSP :8554 / WebRTC :8889 / HLS :8888 由 MediaMTX 提供，与 Metadata HTTP :8081 完全正交；Metadata 写失败不会反映到 `/api/health` 的 `media_server` 字段。
- **安全**：MediaMTX 控制 API（:9997）仅本机监听、无鉴权；媒体端口无鉴权，只在可信网络开放。

---

## 5. 端到端验收结果（真实 camera-agent + 真实 video-server）

运行命令：

```bash
cd video-server
python scripts/verify_metadata.py       # 服务端 metadata 接入（独立起服务）
python scripts/verify_joint.py          # 全栈：真实 camera-agent --auto --ai --metadata
python scripts/verify_ai_resilience.py  # spec §22 测试5/6/7（AI 异常 / AI 关闭 / 杀服务重连）
python scripts/verify_metadata_https.py # HTTPS TLS 双态验收（--metadata-insecure 开/关）
```

实测（2026-09-02，对齐 HEAD 的二进制）：

| 脚本 | 结果 | 覆盖 |
|---|---|---|
| `verify_metadata.py` | **PASS=35 FAIL=0** | 接入 / 往返保真 / 心跳 / bbox 硬化 / 空结果 / 畸形拒绝 / 旧版兼容 / camera_id 映射 / 概览 / 5msg-s 压测 / 媒体路径不受影响 |
| `verify_joint.py` | **PASS=17 FAIL=0 INFO=3** | 全栈协商 1280×720@30、RTSP 解码、分辨率/fps/码率回传、frame+status 双落库；stage 6b 额外轮询真实检测帧并校验 objects 的 class/confidence/bbox 在帧内结构（无目标时为 INFO，不误判）（WebRTC 502 为预期 INFO） |
| `verify_ai_resilience.py` | **PASS=24 FAIL=0 INFO=3** | §22 测试5（坏模型→视频 STREAMING+H264+fps=30+心跳 enable=true/running=false）/ 测试6（`--no-ai`→视频正常+心跳 enable=false/running=false）/ 测试7（杀服务→agent 存活+重连→重启后 metadata 自动恢复且 `frame_id` 越断点 48→50） |
| `verify_metadata_https.py` | **PASS=4 FAIL=0 INFO=2** | §13 对齐的 TLS 双态：Run A（`--metadata-insecure` 开）→ 真实 agent 对自签 HTTPS 端点成功 POST（11 条 frame/status 落 mock 204）；Run B（默认）→ WinHTTP 证书链校验**拦截**自签证书（0 POST 到达），agent 存活不崩，安全默认保留 |

**合计 80 PASS / 0 FAIL。** spec §22 六类验收（测试1 全正常 / 2 服务关重连 / 3 网络断不崩 / 4 恢复发送 / 5 AI 异常 / 6 AI 关闭）全部真机通过；HTTPS TLS 开关（spec §13 隐含的传输安全项）双态真机闭环。

---

## 6. 约束回溯（对照原始规范 §21 禁止事项）

| 禁止项 | 服务端遵守情况 |
|---|---|
| 修改 RTSP / H264 / MediaMTX | 未改；video-server 仅新增 metadata 接收与落库 |
| 重复生成 frame_id | 服务端原样存储采集侧 `frame_id`，不重编号 |
| Metadata 失败影响视频 | 写失败不反映到 `/api/health`，视频流独立 |
| 修改 AIFrameResult 语义 | 仅读取，不改写 |
