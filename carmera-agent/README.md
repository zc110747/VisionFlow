# Camera Agent

在 Windows PC 上用本地摄像头完整模拟未来 **RK3568 等嵌入式 Linux 设备** 的摄像头采集程序。
当前 PC 模拟器链路：

```
Windows PC
  -> USB / Integrated Camera (UVC)
  -> GStreamer (mfvideosrc / dshowvideosrc / ksvideosrc)
  -> videoconvert (像素格式转换，不做缩放)
  -> H.264 Encoder (nvh264enc 硬件 / x264enc 软件)
  -> h264parse
  -> rtspclientsink (自带 RTP payloader)
  -> MediaMTX (RTSP Server)
  -> ffplay / 任意 RTSP 客户端
```

本项目只负责：**摄像头采集 · 视频编码 · RTSP 推流 · 设备状态 · 自动重连 · 配置管理 · 端侧 AI 推理（人检测 + 跟踪 / 人体姿态 / 手部 3D 关键点，可选） · AI 结果上报（Metadata，可选）**。
**不**实现 RTSP Server / Web Server / Web UI / WebRTC / 数据库 / 用户管理 / 视频转发。

---

## 关键设计：后端可插拔

`CameraManager` / `VideoPipeline` / `RtspPublisher` 均为抽象接口，由工厂按编译期后端选择具体实现：

| 后端 | 说明 | 编译条件 |
|------|------|----------|
| `gstreamer` | 真实管线（mfvideosrc → videoconvert → H264 → h264parse → rtspclientsink） | 系统安装 GStreamer 1.0 |
| `sim` | 不依赖 GStreamer，用内部伪帧驱动同一套状态机/重连/统计，用于无摄像头/无 GStreamer 环境下的编译与测试 | 默认（GStreamer 缺失时 auto 自动选中） |

> 未来真实 RK3568 设备只需新增 `camera_manager_v4l2.cpp` 替换 `camera_manager_gst.cpp`，
> 保持 `H264 / RTSP Push / Stream ID` 接口不变，不影响 Video Server。

### GStreamer 管线要点（gstreamer 后端）

- **采集源选择**：`--source auto`（默认，优先 `mfvideosrc`）/` mfvideosrc` / `dshowvideosrc` / `ksvideosrc` / `videotestsrc`（合成源，调试用）。本机 UVC 设备实测 `mfvideosrc`（Media Foundation）最稳，`dshowvideosrc`/`ksvideosrc` 对该设备只出 1 帧。
- **像素格式**：`videoconvert` 只转像素格式（色度/色彩空间），**不做缩放**。硬编 `nvh264enc`（NVENC）不支持 I420，故管线里不 pin format，交由 `videoconvert` 协商。
- **RTSP 推送**：`rtspclientsink` **自带 RTP payloader**，管线里不要串 `rtph264pay`（会让 rtspclientsink 无法 link）。
- **低延迟**：编码段按编码器补低延迟参数（`tune=ultra-low-latency zerolatency=true bframes=0 rc-lookahead=0 repeat-sequence-header=true` 等），每段间插 `queue max-size-buffers=2`，`rtspclientsink latency=0 rtx-time=0`。
- **自动协商（原生直通）**：`--auto` 模式下不在 `videoconvert` 后插入 `video/x-raw` 强制 caps，摄像头用其原生格式直接流；推流后查询源 pad 真实协商值并打印 `Negotiated capture format: WxH @ Ffps`。

---

## 目录结构

```
camera-agent/
├── CMakeLists.txt
├── README.md
├── build_oneclick.bat        # 一键构建 (MSVC cl.exe + Ninja)，自动探测 VS
├── start-camera-agent.bat    # 一键启动 demo：MediaMTX -> agent -> ffplay
├── mediamtx.yml              # MediaMTX 演示配置（强制 TCP、禁录制）
├── config/camera-agent.yaml  # YAML 配置（可选，CLI 优先）
├── models/                   # ONNX 模型：yolo11n / yolo11n-pose / MediaPipe 手部两级级联
│                             #   溯源、I/O 形状与 SHA-256 见 models/README.md
├── include/camera_agent/     # 公共头：types / config / camera_manager /
│                             #   video_pipeline / rtsp_publisher / stream_controller / backoff / logger
│                             #   ai/{ai_types,detector,tracker,ai_pipeline}.h
│                             #   ai/{hand_types,hand_decode,hand_detector,hand_analyzer}.h
├── src/
│   ├── main.cpp              # CLI 解析 + 运行循环 + SIGINT 优雅退出
│   ├── camera/               # camera_manager_sim.cpp / camera_manager_gst.cpp
│   ├── pipeline/             # video_pipeline_sim.cpp / video_pipeline_gst.cpp
│   ├── rtsp/                 # rtsp_publisher_sim.cpp / rtsp_publisher_gst.cpp
│   ├── ai/                   # yolo_detector.cpp (ORT) / byte_tracker.cpp / ai_pipeline.cpp
│   │                         #   hand_decode.cpp / hand_detector.cpp / hand_analyzer.cpp
│   ├── metadata/             # metadata_encoder.cpp / metadata_manager.cpp
│   │                         #   / http_transport_winhttp.cpp (WinHTTP，无额外依赖)
│   ├── config/config.cpp     # 极简 YAML 解析（无额外依赖）
│   └── common/stream_controller.cpp  # 编排 + 自动重连
├── third_party/
│   └── stb_image.h           # 单元测试用的单头 JPEG/PNG 解码（仅 tests 链接）
├── tests/                    # 轻量测试框架 + 88 个用例（可无头运行；需 PATH 含 GStreamer / ORT）
│   ├── golden/               # 手部黄金夹具 + 全量 anchor 参考表 + 生成脚本（见 golden/README.md）
│   └── finished/             # 运行/验证产物（日志、抓帧）落此处，已 gitignore
└── scripts/
    ├── build-msvc.ps1        # MSVC + Ninja 构建（推荐，原生 PowerShell）
    ├── build.ps1             # MSVC (Visual Studio 生成器，本机不可用，见下)
    ├── e2e-test.ps1          # 端到端验收（MediaMTX + 拉帧 + 断服重连 + auto-resume）
    ├── metadata-mock-server.py  # Metadata 验收用假服务端（仅标准库，可随时 kill 测重连）
    ├── analyze_hand_landmarks.py  # 手部阈值离线标定（复刻 hand_classify_gesture 公式）
    └── probe-*.ps1           # 历史调试探针脚本（排查延迟/链路各段，见调试章节）
```

---

## 环境依赖

- **Windows + Visual Studio 2022**（含 "Desktop development with C++" 工作负载，带 MSVC / Windows SDK / CMake / Ninja）。
- **GStreamer 1.0（MSVC 版 runtime + devel）**。仓库**不硬编码**路径：CMake 按 `-DGSTREAMER_ROOT` → `$ENV{GSTREAMER_ROOT}` → 官方安装程序写入的环境变量 → `PATH` 上的 `gst-launch-1.0`/`pkg-config` → 通用默认前缀的顺序定位，详见 `ENV_SETUP.md`。运行 agent 时需把其 `bin` 加入 `PATH` 以加载插件。
- **MediaMTX**（RTSP Server）：`mediamtx.exe` 加入 `PATH`。
- **ffmpeg/ffplay**（仅验证/观看用）：加入 `PATH`。
- 依赖仅 `spdlog`（CMake `FetchContent` 自动拉取），无其他第三方依赖。
- **可选**——**ONNX Runtime 1.x（Windows x64 CPU）**：AI 分支加载 `.onnx` 模型用。未安装时 AI 自动停用，编出 `NullDetector`，视频链路不受影响。`include\` + `lib\` 即可；运行时 DLL 由 CMake `POST_BUILD` 自动拷到 exe 旁。CMake 按 `-DONNXRUNTIME_ROOT` → `$ENV{ONNXRUNTIME_ROOT}` → `D:/Software/onnxruntime` → `C:/onnxruntime` → 其他通用前缀顺序定位。
- **可选**——YOLO11 模型（Ultralytics 官方 ONNX，下载地址与 SHA-256 见 `third_lib.md`）：`yolo11n.onnx`（检测，默认）与 `yolo11n-pose.onnx`（17 关键点姿态），放在项目根 `models/` 下；路径可在 `ai.model` 配置或 `--ai-model` 覆盖，检测/姿态模型自动识别。
- **可选**——MediaPipe 手部模型（Apache-2.0，**已入库**，见 `models/README.md`）：`palm_detection_mediapipe_2023feb.onnx`（掌心检测）+ `handpose_estimation_mediapipe_2023feb.onnx`（21 点），仅在 `--ai-mode hand` 时加载。

> 外部工具一律按**裸名称**调用，因此必须在本机 `PATH` 上。双击 `.bat` 时继承的是注册表里的系统/用户 PATH，**不是**已开终端的 PATH —— 改完 PATH 要重开终端才生效。

> 摄像头规格因机而异，推荐 `--auto` 让设备自己协商原生格式。本机实测：LRCP USB2.0（index 1）协商为 **1280×720@30**；内置 Integrated Camera（index 0）为 1280×720@10。实际索引请用 `camera-agent --list` 查看。

---

## 构建

### ⚠️ 关键约束（本机坑）

1. **不能用 MSBuild 生成器**：本机 MSBuild 一编译真实工程就 access violation 崩溃，故一律用 **Ninja + MSVC**，封装在 `scripts/build-msvc.ps1` / `build_oneclick.bat`。`scripts/build.ps1`（VS 生成器）在本机不可用。
2. **用 PowerShell / cmd 原生环境，不用 MSYS2 bash**：MSYS2 bash 会把冒号分隔的 PATH 交给 MSYS2 版 cmake，再传给 cmd.exe，导致 rc.exe/mt.exe 找不到、链接失败。从 PowerShell 跑保持 PATH 全程原生 Windows 形式。
3. **工具链指向 VS 自带原生 CMake/Ninja**（在 `Common7\IDE\CommonExtensions\Microsoft\CMake\...`），绝不用 MSYS2 的 cmake。
4. **源码 UTF-8 → MSVC 加 `/utf-8`** 消除 C4819 警告，目标零警告。

### 推荐：PowerShell 构建（原生）

```powershell
# 必须用原生 PowerShell 运行（非 MSYS2 bash），且以非沙箱方式跑（需写注册表/调 cl.exe）
.\scripts\build-msvc.ps1                  # gstreamer 后端（默认）
.\scripts\build-msvc.ps1 -Backend sim     # SIM 后端
.\scripts\build-msvc.ps1 -Clean           # 先清空对应构建目录再构建
```

**产物目录按后端分流，别搞混**：`-Backend gstreamer` 输出到 **`build-msvc-gst\src\camera-agent.exe`**
（约 628 KB），`-Backend sim` 才用 `build-msvc\`。分流是为了让 SIM 构建不覆盖 GStreamer 构建，
根目录 `start.bat` 两个都找、优先 `build-msvc-gst`。目标 0 warning / 0 error。

### 备选：一键构建 bat

```bat
build_oneclick.bat              :: gstreamer 后端（默认）
build_oneclick.bat sim          :: SIM 后端（无需 GStreamer）
build_oneclick.bat auto         :: 让 CMake 自动选 gstreamer 或 sim
build_oneclick.bat clean        :: 先清空 build-msvc
```

`build_oneclick.bat` 内部同样走 **vcvarsall.bat (x64) + VS 自带 CMake/Ninja + Ninja 生成器**，只是把流程打包成双击可用的 bat。

### 任意平台 / 无 GStreamer（SIM 后端，开发/CI/测试）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCAMERA_AGENT_BACKEND=sim
cmake --build build
```

---

## 配置

### YAML（`config/camera-agent.yaml`，可选）

优先级：**CLI 参数 > YAML > 内置默认**。支持的字段：

```yaml
camera:
  id: 0
  width: 1280          # 显式分辨率；与 --auto 互斥（--auto 时不强制）
  height: 720
  fps: 30
  # auto: true         # 可选；等同 --auto，原生直通自动协商
encoder:
  codec: h264
  bitrate: 4000        # kbps
  keyframe_interval: 30
stream:
  id: camera01
rtsp:
  server: 127.0.0.1
  port: 8554
device_id: camera01
log_level: info
# source: mfvideosrc   # 可选；等同 --source
# measure_latency: true # 可选；等同 --latency-probe

# ---- AI 分支（与视频链路完全独立；ai.enable=false 时管线字符串逐字节不变）----
ai:
  enable: false                # 启用人检测 + ByteTrack 跟踪
  fps: 5                       # 推理率（源 fps < full_rate_below_fps 时改为每帧都算）
  confidence: 0.5              # 检测+跟踪门限
  model: models/yolo11n.onnx   # ONNX 模型路径（相对工作目录）；yolo11n-pose.onnx 为姿态模式
  input_width: 640
  input_height: 640
  queue_size: 2                # 有界队列；满了丢最老帧保最新
  nms_threshold: 0.45
  match_threshold: 0.8         # ByteTrack 首次关联 IoU 门限
  track_buffer: 30             # 丢失轨迹保留时长（按 30fps 折算）
  low_confidence: 0.1          # 检测器底门槛（ByteTrack 二阶段用）
  full_rate_below_fps: 10      # 源 fps 低于此值则每帧都跑
  log_objects: true            # 每帧打印检出对象
  num_threads: 2               # ONNX Runtime intra-op 线程

# ---- 运行时 AI 模式切换（Web → video-server → agent 轮询）----
# 启动模式：detect | pose | hand（也可写 ai-y / ai-y-pose / ai-hand）。
# 仅决定第一个请求到达前跑什么，以及「没有服务端时怎么单独跑某模式」。
  initial_mode: detect
# model_pose：姿态模型路径；Web 选 ai-y-pose 时运行时换此模型，无需重启。
  model_pose: models/yolo11n-pose.onnx
  # agent 主动轮询 video-server 的 GET /api/cameras/{id}/aimode 拿期望模式：
  #   ai-off    -> 保持当前模型，仅 Web 隐藏 overlay（不通知 agent 停模型）
  #   ai-y      -> 换 yolo11n（人检测）
  #   ai-y-pose -> 换 yolo11n-pose（17 关键点姿态）
  #   ai-hand   -> 换掌心+21 点两级级联（手部 3D）
  aimode_poll: true                            # false = 固定启动模型，不接受 Web 切换
  aimode_poll_ms: 2000                         # 轮询周期
  aimode_base_url: ""                          # 留空=自动从 metadata.server_url 派生（建议，单端点配置）。显式填 http://127.0.0.1:8080 也行

# ---- 手部模式（Phase 1：单只手的 21 点 3D 关键点 + 运动 + 手势）----
# 同一条 AIPipeline、同一个 metadata 端点：选 hand 只是换掉加载的模型。
# 完整注释见 config/camera-agent.yaml；这里只列结构与默认值。
  hand:
    model_palm: models/palm_detection_mediapipe_2023feb.onnx
    model_landmark: models/handpose_estimation_mediapipe_2023feb.onnx
    palm_score_threshold: 0.5    # 掌心检测门限
    palm_nms_threshold: 0.3      # 掌心 NMS IoU
    landmark_confidence: 0.5     # 关键点模型门限
    max_hands: 1                 # Phase 1 单只手：取最高分掌心
    filter_alpha: 0.4            # EMA
    motion_deadzone: 0.02        # |delta| 低于此值判 STATIONARY
    reference_stable_frames: 3   # 参考点锁存前需连续检出的帧数
    lost_frames_to_reset: 10     # 丢失多少帧后状态机复位
    gesture_confirm_frames: 5    # 手势需连续保持多少帧才确认
    depth_source: size           # size | landmark_z（决定 logical Y=FORWARD 的来源）
    invert_x: false              # 相机装反时翻某个 logical 轴
    invert_y: false
    invert_z: false
    emit_landmarks: true        # false 则 metadata 不带 21 点，报文显著变小

# ---- Metadata 分支（Phase 2：AI 结果异步上报；失败绝不影响视频/AI）----
metadata:
  enable: false                                  # 启用结果上报
  server_url: http://127.0.0.1:8080/api/metadata # 服务端地址（必须配置，禁止硬编码；默认即 video-server 的 8080）
  camera_id: camera01                            # 上报用的摄像头标识
  version: 1                                     # 协议版本，随每条消息发出
  queue_size: 8                                  # 有界队列（5~10）；满了丢最老的
  timeout_ms: 2000                               # 单次 HTTP 超时
  retry_interval_ms: 1000                        # 重连退避起始值
  retry_max_interval_ms: 30000                   # 退避上限（指数增长封顶）
  heartbeat_interval_sec: 10                     # AI 状态心跳周期（0 = 关闭）
  log_payload: false                             # DEBUG 打印完整 JSON（默认关）
```

### MediaMTX（`mediamtx.yml`）

MediaMTX 从项目根目录启动时会自动加载本文件。`all_others` + 无 source 允许 agent 发布到任意 RTSP 路径（如 `rtsp://127.0.0.1:8554/camera01`）无需逐路径配置。已设 `rtspTransport: tcp`（匹配 rtspclientsink 的 TCP）与 `record: no`（不引入磁盘缓冲）。

### 自动协商（`--auto`）

开启后不在 `videoconvert` 后插入 `video/x-raw` 强制 caps，由 GStreamer 自动协商摄像头原生格式（原生直通，必然能链上）。推流后查询摄像头源 pad 的真实协商 caps 并打印：

```
[info] Auto-resolution: not forcing caps; camera negotiates native format
[info] Negotiated capture format: 240x240 @ 8fps
```

> `videoconvert` 不做缩放，原生直通 = 摄像头给什么就推什么。若要统一输出尺寸（如固定 1280×720）需另加 `videoscale`（当前未做）。

---

## 运行

### 命令行参数（全表）

| 参数 | 默认 | 说明 |
|------|------|------|
| `--list` | - | 列出摄像头 (id/name/resolution/fps) |
| `--camera <id>` | 0 | 摄像头 id |
| `--width/--height <px>` | 1280/720 | 采集分辨率（显式模式） |
| `--fps <n>` | 30 | 采集帧率（显式模式） |
| `--auto` | - | 自动协商分辨率/帧率：用摄像头原生格式，代替强制 `--width/--height/--fps` |
| `--source <elem>` | auto | 采集源元素（auto \| mfvideosrc \| dshowvideosrc \| ksvideosrc \| videotestsrc ...） |
| `--bitrate <kbps>` | 4000 | 编码码率 |
| `--stream <id>` | camera01 | 流 id |
| `--server/--port` | 127.0.0.1/8554 | RTSP 服务器 |
| `--device-id <id>` | camera01 | 设备状态上报 id |
| `--config <path>` | config/camera-agent.yaml（若存在） | YAML 配置 |
| `--duration <sec>` | 0 | 自动退出秒数（0 = 直到 Ctrl+C） |
| `--log-level <lvl>` | info | trace/debug/info/warn/error |
| `--latency-probe` | - | 测量 camera-agent 内部分段延迟（capture→encode→push） |
| `--ai` / `--no-ai` | `--no-ai` | 启/停 AI 分支（人检测 + 跟踪） |
| `--ai-fps <n>` | 8 | AI 推理率，可配范围 5-12（`source fps < full_rate_below_fps` 时改为每帧） |
| `--ai-confidence <f>` | 0.5 | 检测+跟踪置信度门限 |
| `--ai-model <path>` | `models/yolo11n.onnx` | ONNX 模型路径（检测/姿态自动识别） |
| `--ai-model-pose <path>` | `models/yolo11n-pose.onnx` | 姿态模型路径（Web 切 ai-y-pose 时运行时换） |
| `--ai-mode <mode>` | detect | 启动模式：detect \| pose \| hand（也接受 ai-y / ai-y-pose / ai-hand）；不需要服务端即可单跑某模式 |
| `--ai-hand-palm <path>` | `models/palm_detection_mediapipe_2023feb.onnx` | 手部掌心检测模型 |
| `--ai-hand-landmark <path>` | `models/handpose_estimation_mediapipe_2023feb.onnx` | 手部 21 点模型 |
| `--ai-hand-depth <src>` | size | 手部深度轴来源：size \| landmark_z |
| `--ai-hand-invert-x/-y/-z` | - | 翻转手部 logical 轴（相机装反时用） |
| `--no-ai-hand-landmarks` | - | 上报不带 21 点，报文显著变小 |
| `--ai-aimode-base <url>` | `http://127.0.0.1:8081` | 轮询 video-server 的基址（取 `/api/cameras/{id}/aimode`） |
| `--ai-aimode-poll-ms <ms>` | 2000 | AI 模式轮询周期 |
| `--no-ai-aimode` | - | 关闭 Web 驱动切换（固定启动模型，不接受 ai-y/ai-y-pose 切换） |
| `--ai-input <w> <h>` | 640 640 | 网络输入尺寸 |
| `--ai-queue <n>` | 2 | 有界队列深度 |
| `--ai-full-rate-below <n>` | 10 | 源 fps 低于此值时改为每帧都跑 |
| `--ai-log-objects` | on | 每帧打印检出对象 |
| `--metadata` / `--no-metadata` | `--no-metadata` | 启/停 Metadata 上报分支 |
| `--metadata-url <url>` | `http://127.0.0.1:8000/api/metadata` | 服务端地址 |
| `--metadata-camera-id <id>` | `camera01` | 上报用摄像头标识 |
| `--metadata-queue <n>` | 8 | 有界队列深度（5~10） |
| `--metadata-timeout <ms>` | 2000 | 单次 HTTP 超时 |
| `--metadata-heartbeat <s>` | 10 | AI 状态心跳周期（0 = 关闭） |
| `--metadata-log-payload` | off | DEBUG 打印完整 JSON |
| `--version` / `--help` | - | 版本 / 帮助 |

### 典型命令

```bash
# 列出摄像头能力
camera-agent --list

# 指定分辨率推流（强制 caps；设备不支持该模式会协商失败进重连）
camera-agent --camera 1 --width 640 --height 480 --fps 30 --stream camera01

# 自动协商（原生直通，无需手填分辨率）
camera-agent --auto --source mfvideosrc --stream camera01

# 自定义码率/服务器
camera-agent --auto --bitrate 2000 --server 192.168.1.100 --port 8554 --stream camera01

# 内部延迟探针（实测约 20ms，见验证章节）
camera-agent --auto --source mfvideosrc --latency-probe --duration 8

# AI 启用人检测（独立线程，5 fps，视频不受影响）
camera-agent --ai --width 1280 --height 720 --fps 30 --duration 30

# AI + Metadata 上报（另开一个终端跑假服务端）
python scripts/metadata-mock-server.py --port 8000
camera-agent --ai --metadata --metadata-url http://127.0.0.1:8000/api/metadata \
             --metadata-camera-id camera01 --metadata-heartbeat 10 --duration 30

# 手部 3D 模式（不需要服务端也能起：启动模式由 CLI 决定）
camera-agent --ai --metadata --metadata-url http://127.0.0.1:8000/api/metadata \
             --ai-mode hand --duration 30

# 手部方向实验：切换深度来源 / 翻转某个 logical 轴（结果记入文档，不要靠猜）
camera-agent --ai --ai-mode hand --ai-hand-depth landmark_z --duration 15
camera-agent --ai --ai-mode hand --ai-hand-invert-x --duration 15
```

> 手部模式启动时会加载两个 ONNX（实测约 100–200ms）。模型缺失或加载失败**不会**让 agent 退出：
> 会记一条 ERROR/WARN 并回落到人检测（`--ai-mode` 指定的模式被拒时同理，切换失败则保持原模型不变）。

### 一键启动（demo：MediaMTX + agent + ffplay）

```bat
start-camera-agent.bat            :: 默认启动（CAMERA_ID=0 + --auto）
start-camera-agent.bat --no-pause :: 不暂停
start-camera-agent.bat --latency-probe :: 透传延迟探针
start-camera-agent.bat --camera 3 --auto          :: 换摄像头
start-camera-agent.bat --camera 1 --no-auto --width 640 --height 480 --fps 30
```

所有参数既能用命令行传，也能用环境变量预设（`CAMERA_ID` / `CAMERA_AUTO` / `CAMERA_WIDTH` / `CAMERA_HEIGHT` / `CAMERA_FPS` / `CAMERA_SOURCE` / `STREAM_ID` / `RTSP_HOST` / `RTSP_PORT`），优先级：命令行 > 环境变量 > 默认值。

流程：(1) 预检工具与二进制；(2) `taskkill` 停掉残留 mediamtx/camera-agent/ffplay；(3) 顺序 `start` mediamtx → camera-agent；(4) 轮询 agent 日志确认 `STREAMING`、再确认 mediamtx 已接受发布者；(5) **确认就绪后才启动 ffplay**；(6) 打印 RTSP 地址与访问方式。

- 外部工具（mediamtx/ffplay/ffmpeg）按裸名称调用，**必须在 PATH 上**，脚本内不含任何机器路径。
- 日志重定向到 `tests\finished\`（agent.log / mediamtx.log）。
- 观看端 ffplay 低延迟旗标组：`-rtsp_transport tcp -fflags nobuffer -flags low_delay -probesize 32768 -analyzeduration 0 -framedrop -max_delay 0`。
  ⚠️ 不要加 `-rtsp_flags nobuffer`：`rtsp_flags` 不接受该值，ffplay 会直接 `Invalid argument` 退出、窗口永远不出现。
- **摄像头索引自动校验**：启动 agent 前先跑一次 `camera-agent --list` 枚举可用索引；若配置的 `CAMERA_ID` 不在其中，自动回退到第一个可用索引并打印 `[AUTO] camera index N is not available - using index M`。换机器/换 UVC 设备时无需手改脚本。若枚举失败（无摄像头或 GStreamer 不在 PATH），保留原索引并打印 `[note] could not enumerate cameras`，由后续诊断提示接手。

> 为什么必须"先等流就绪再开 ffplay"：agent 报 `STREAMING` 比 mediamtx 真正注册路径早约 1 秒，此时 ffplay 发 DESCRIBE 会拿到 `404 Not Found` 并立即退出。

### Web 驱动 AI 模式切换（四态）

控制链路：**Web UI → POST `/api/cameras/{id}/aimode` → video-server 落库 → agent 轮询 `GET /api/cameras/{id}/aimode` → 运行时换模型**。agent→server 是单向 WinHTTP POST（metadata），无反向通道，故采用 agent 主动轮询。

四种模式（默认 `ai-y`）：

| 模式 | 含义 | agent 行为 | Web 行为 |
|------|------|-----------|----------|
| `ai-off` | 关 AI 显示 | **不换模型**（保持启动模型），忽略该请求 | 仅隐藏检测框/骨架 overlay |
| `ai-y` | 人检测 | 运行时换 `yolo11n`（Detect） | 画检测框 |
| `ai-y-pose` | 姿态检测 | 运行时换 `yolo11n-pose`（Pose，17 关键点） | 画检测框 + COCO 骨架连线 |
| `ai-hand` | 手部 3D | 运行时换掌心 + 21 点两级级联（Hand） | 画手部骨架 + HUD（手势/运动） |

- 模式 token 只有**一个解析点**（`aimode_from_string`，CLI / YAML / 轮询器共用）；`ai-off` 与任何无法识别的 token 一律**不解析** —— 前者语义是"Web 停止绘制"，后者是别的构建才支持的模式，都不能被悄悄翻译成一次模型切换。
- 切换在 AI 线程内完成：ONNX 重新加载（慢，~数百 ms）在锁外进行，仅指针交换这一步加锁，因此**绝不阻塞视频/采集线程**。切换成功后才释放旧模型（Detect/Pose 与 Hand 互斥，两套同时挂着只是白占 ~20MB）。
- 切模式时 `HandAnalyzer` 被重置：参考点、EMA、手势锁存都不跨模式继承，否则第一次 delta 会是一个巨大的幽灵位移。
- `ai-off` 不通知 agent 停模型（符合需求：只关界面显示）；其余三种才会触发模型重建并重绘。
- 心跳里的 `model` 报的是**当前模式实际加载的模型**（hand 模式报掌心模型），不是配置里的 `ai.model` —— 否则 UI 会在 hand 模式下谎报 yolo11n。
- 单测 `ai_pipeline_switch_models`（Detect↔Pose）与 `ai_pipeline_hand_mode_round_trip`（Detect↔Hand，含 `keypoint_count()` 21↔0）验证运行时可切换；video-server 侧 `TestAIMode*`/`TestSaveFrameRoundTripsKeypoints` 覆盖落库、默认、非法拒绝、keypoints 透传。

---

## 手部 3D 识别（Phase 1：单只手，21 点 + 运动 + 手势）

> **边界**：这一章到"产出结构化 `hand` metadata"为止。**不包含**机器人、IK、舵机、MuJoCo、RobotCommand ——
> 那些属于另一个项目，`hand_types.h` 里连一个相关概念都不许出现。

### 位置：同一条 AI 链路里的一个新模式

```
camera ──tee──┬──► video pipeline ─────────► RTSP ─► MediaMTX ─► 浏览器 <video>   （完全不动）
              │
              └──► AIPipeline（同一个线程、同一个有界队列、同一个 MetadataManager）
                        │
                        ├─ AIMode::Detect → OnnxYoloDetector(yolo11n)
                        ├─ AIMode::Pose   → OnnxYoloDetector(yolo11n-pose)
                        └─ AIMode::Hand   → OnnxHandDetector ─► HandAnalyzer ─► HandState
                                                                                    │
                                                              AIFrameResult.hand ───┘
                                                              → 编码为 "hand" 加法字段
```

不新增服务、不新增第二套视频流、不新增第二套 metadata 协议；`IDetector` 抽象刻意**没有**被改（手部两级级联需要两个模型 + ROI 变换，塞进 `IDetector` 只会污染人检测/姿态两条既有路径），而是并列了一个 `IHandDetector`。

### 两级级联（MediaPipe 方案，Apache-2.0，模型已入库）

```
frame ──► palm detector (192x192, 2016 anchors) ──► 掌心框 + 7 个掌心关键点
                                          │
                                          ├─ 按掌心关键点算旋转角（复刻 _cropAndPadFromPalm）
                                          └─ 仿射裁切 224x224 ──► landmark model ──► 21 x (x,y,z) + handedness
```

- 21 点顺序为 MediaPipe 约定（0 手腕、1-4 拇指、5-8 食指、9-12 中指、13-16 无名指、17-20 小指）。
- **刻意不使用 OpenCV**：双线性重采样、`INTER_AREA`、仿射、letterbox、NMS 全部自实现，和 `yolo_detector.cpp` 保持同一策略，便于后续搬 RK3568。
- 高风险的解码原语（anchor 生成 / 逐行解码 / 真 IoU NMS）拆在 `hand_decode.h/.cpp`，**无需模型即可单测**。

### 后处理流水（`HandAnalyzer`，纯逻辑，可无模型单测）

```
HandDetection ─► 坐标映射(spec 17) ─► EMA 滤波 ─► 参考点状态机 ─► 运动判定(死区) ─► 手势 + 去抖 ─► HandState
```

- **logical 坐标系**（下游只说这一套，模型/相机的原始轴不外泄）：`X+` 右、`Y+` 前（朝相机）、`Z+` 上；位置/参考/位移全部**归一化**（x∈[0,1] 按帧宽，z∈[0,1] 按帧高且 0 在底部，y 是深度代理）。
- **参考点**：手必须连续稳定检出 `reference_stable_frames` 帧才锁存，且锁的是**滤波后**位置。锁存之前 `delta` 一律置零 —— 否则手刚入画那一帧的抖动会被永久烘焙进之后每一个 delta。
- **死区**：`|delta_axis| < motion_deadzone` 判 `STATIONARY`，这是唯一能压住"手抖导致 LEFT/RIGHT 反复横跳"的机制。
- **手势**：四指 `|tip-wrist| / |MCP-wrist|` 的比值投票（拇指不参与，它最噪且两种手势都含）。`OPEN_PALM` 阈值 `1.15` 有真实数据出处：官方样例图上实测张开手为 1.59–1.84。
- **Z 的语义**（实测钉死，不是假设）：模型输出的 `landmark.z` 是**相对手腕**的深度，手腕 z 恒为 0，与"手离相机多远"无关 —— 它描述**朝向**。因此 logical `Y = FORWARD/BACKWARD` 默认取**手的表观尺寸**（`depth_source: size`），`landmark_z` 保留为可选项供方向实验复现。

### 运行

```bash
camera-agent --ai --metadata --metadata-url http://127.0.0.1:8081/api/metadata --ai-mode hand
start.bat --ai-mode hand        # 联合运行（server + agent）直接以手部模式起
```

### 阈值怎么重新标定

`hand.landmarks` 上报的是**图像像素坐标（2 位小数）**，精度远高于要标定的 ratio
⇒ **报文本身就够标定，不用改代码、不用重编**。

```bash
# 1. 落全量报文（-u 关缓冲，否则重定向到文件看不到实时内容）
python scripts/metadata-mock-server.py --port 8099 --dump > mock.jsonl &
camera-agent --camera 0 --width 1280 --height 720 --fps 30 --ai --ai-mode hand \
             --no-ai-aimode --metadata \
             --metadata-url http://127.0.0.1:8099/api/metadata --duration 120

# 2. 离线复算（脚本逐字复刻 hand_classify_gesture 的公式）
python scripts/analyze_hand_landmarks.py mock.jsonl --timeline 60   # 先看时间线切段
python scripts/analyze_hand_landmarks.py mock.jsonl --tail 30       # 只看最新一段
python scripts/analyze_hand_landmarks.py mock.jsonl --hist          # 看双峰与谷底
```

⚠️ **标定时被标对象必须保持不动**：过渡帧（出入画、运动模糊）在分布上天然落在两簇之间的
谷底且数量占优，会把阈值往上拉 —— 实测对"每帧四指中位数"跑 Otsu 得到 **1.3546**，
甚至越过了 `kExtendedRatio`。脚本已把建议值钳到 `kExtendedRatio - 0.05`。

### 已知未验证项（显式标注，不含糊）

| 项 | 状态 |
|---|---|
| `kCurledRatio = 1.10`（握拳侧阈值） | **已实测**（2026-09-23，真机 LRCP USB2.0）。原先的 0.90 是未验证的占位值，实测发现它落在**真实握拳簇内部** —— 真握拳的 middle 指会冲到 0.983，导致四指里只有 2 指投 curled、拿不到 0.75 票，**14 帧真握拳里有 11 帧输出 `UNKNOWN`**。现取值 1.10：比握拳簇高 0.117、比开掌簇低 0.329，且仍在 `kExtendedRatio`(1.15) 之下。回归用例 `hand_gesture_real_capture_open_and_fist` 用本次实测的 landmark 逐位钉住 |
| **index 指是四指里最不可靠的一个** | **已知几何限制，非标定问题**。MCP5 是四指里离腕点最近的，同样的卷曲程度下 index 比值天生偏高。**同一只手的两次握拳实测：松拳 1.222–1.398（> `kExtendedRatio`，反而投 extended）、紧拳 0.614–1.971（中位 0.829，多数投 curled）** ⇒ index 是唯一跨越两个阈值的指头，"怎么握就怎么投"。而 middle/ring/pinky 两次都 ≤ 0.983，是可靠的三指。**FIST 实际只靠这三指，且恰好卡在 `kGestureVote=0.75` 的等号上** ⇒ 这三指若有一指被遮挡，手势就会丢失。根治需要按手指归一化（或用 PIP/DIP 关节），属分类器改动，Phase 1 不做 |
| `depth_source` 的方向（哪个符号是"靠近"） | **待真机实验**（`--ai-hand-depth` 可切换两种来源对比） |
| 真实 UVC 摄像头的 `invert_*` 取值 | **待真机实验**（`--ai-hand-invert-x/-y/-z`） |
| 画面疑似整体旋转 90° | **待确认**（真机帧里天花板灯板出现在画面左侧、玻璃隔断在右侧）。这会影响 §17 logical 坐标系与 P3 前端 overlay 朝向 |

模型溯源、I/O 实测形状与 SHA-256：`models/README.md`。
黄金夹具、全量 anchor 参考表与生成脚本：`tests/golden/README.md`。
架构分析与实证记录（含"为什么夹具必须是 PNG"）：`../docs/prompter/hand-3d-phase1-architecture.md`。

---

## 手势 → 机械臂（Phase 2：TCP 控制 MeArmPilot）

> **边界**：这一段**消费** `HandState`，反向不成立 —— `hand_types.h` 明确写着"Phase 2 才把
> HandState→RobotCommand 放到**另一个项目**，这里不许知道机器人/IK/舵机/MuJoCo/MeArmPilot"。
> 所以依赖是单向的：`HandState` 不知道 `RobotLink` 存在，`Hand*` 相关代码**未被本次改动触碰**。

```
HandState ─► RobotLink(映射/限速/边沿) ─► IJsonLineClient ─► TCP :9100 ─► MeArmPilot(armpilot-backend)
```

### 生效条件（三条同时满足，缺一不影响视频）

1. Web UI 切到 **`ai-hand`（手势识别）**；`ai-y` / `ai-y-pose` / `ai-off` 下机器人分支不消费任何帧。
2. 启动带 `--robot`（等价 `robot.enable: true`）。**仓库根 `start.bat` 默认就带**，
   所以联合运行时不必手动加；`--no-robot` 可关掉。
3. 目标 `host:port` 上有 MeArmPilot 在监听。**连不上只 WARN，不阻塞视频/AI** —— 视频链路
   照常推流，机器人分支自己退避重连（默认 2s）。

> 🔥 **2026-09-29 踩过的坑：这条分支曾经"完全静默"。** 症状是手在动、臂不动、日志里
> **连一行 `[ROBOT]` 都没有** —— 因为 `robot.enable` 默认 `false` 且启动器不透传 `--robot`，
> `start_robot()` 直接 `return`，整条分支不存在。修法：① 默认值改 `true`；
> ② 两个启动器（`start.bat` / `start-joint.bat`）默认透传 `--robot`；
> ③ `start_robot()` 在未启用时**打一条 INFO**（`[ROBOT] disabled ...`），
> 让"没开"和"开了但坏了"在日志里可区分。

### 启动器参数

```bat
start.bat                          :: 默认 --ai --robot（UI 切手势即开始控臂）
start.bat --no-robot               :: 只要视频 + AI，不要机器人分支
start.bat --robot-host 10.0.0.5 --robot-port 9200
start.bat --robot-map absolute     :: 换绝对定位路线（"指着走"）
start.bat --no-ai                  :: robot 自动一并关闭并打印提示（手势依赖 AI 分支）
```

### 行为契约

| 场景 | 行为 |
|---|---|
| 手离开画面 | 回归 **HOME**（`lost_frames` 帧确认离开） |
| 手**初次**进入画面 | **不**再发一次 HOME（`open()` 已发过），只记录 presence |
| 手**重新**进入画面 | 回归 HOME 并重锁参考点（不继承旧原点） |
| 参考点未锁存 | **不发任何命令**（`reference_initialized == false` 时 delta 是到画幅原点的巨大位移） |
| 握拳 / 张开 | `gripper close` / `gripper open`，**边沿触发**，保持不重复发 |
| 传送指令 | 客户端自限速（`rate_hz`，默认 15Hz）；服务端**不**节流 |
| 手推向已到的工作空间边界 | **软边界**把该轴增益逐步衰减（`soft_boundary`），臂减速贴边而非满速撞墙 |

映射走 **`delta` 增量式（跟手）**：手帧间位移 → 臂位移。增益不是拍脑袋的数：
`gesture-control.md §2.1` 推导为 `workspace 跨度 / 手在画面里的有效行程`，
`gain_x|y|z` 是叠在上面的乘数。坐标轴按**重命名**（非置换）对齐：
手 `dy`(前后)→臂 `X`(front)、手 `dx`(左右)→臂 `-Y`(left)、手 `dz`(上下)→臂 `Z`(up)。

### 增益降幅：手大动、臂小动（`gain_x/y/z`，**默认已降**）

**解决什么**：等比例（`gain = 1.0`）时，手的**自然抖动**也被 1:1 放大成臂的步长，臂一直在
微动、"站不稳"，看起来就是"波动导致机械臂无法正确工作"。

**做法**：只调 **`gain_x/y/z` 乘数**（不动 `motion_deadzone`，不改代码），让手的大幅移动
对应臂的小幅移动。步长公式：

```
step_mm = hand_delta(归一化) × (reach_span_axis / 0.5) × gain_axis × soft_boundary
```

其中 `0.5` 是 `kHandActiveSpan`（手在画面里的有效行程基准）。**降幅后实测步长**：

| 轴 | `reach_span` | `gain` | 手抖 0.02 | vs 0.5mm 硬门槛 | 故意动 0.10 |
|---|---|---|---|---|---|
| X (front) | 39.0 mm | **0.25** | **0.39 mm** | **被滤除** ✅ | 1.95 mm |
| Y (left) | 176.0 mm | **0.50** | 1.76 mm | 通过 | 8.80 mm |
| Z (up) | 41.5 mm | **0.25** | **0.42 mm** | **被滤除** ✅ | 2.08 mm |

**为什么 X/Z 降 0.25、Y 只降 0.5**：Y 的实测 span（176mm）是 X 的 4.5 倍，等比例降 0.25
会让 Y 轴过钝（手大动臂几乎不动）。三轴各自按 span 归一化后，0.25/0.5/0.25 得到的是
**相近的手感灵敏度**，而不是相同的数字。

**取舍（必须知道）**：
- ✅ **收益**：X/Z 的手部抖动（0.02 → 0.39/0.42mm）落在 `deadzone_mm = 0.5` 的硬门槛
  之下，被**自然滤除** —— 这正是"解决波动"的机制，不需要额外加滤波。
- ⚠️ **代价**：X/Z 的**慢速小动作**（手位移 < ~0.025 归一化）也不再产生命令。若觉得
  "迟钝"而非"稳"，**上调对应轴的 `gain`**（`--robot-gain-x 0.4` 等）即可。

> 这两个默认值被 `robot_shipped_gains_are_the_documented_feel` 测试**钉住**（含步长表算术
> 断言），并有一条 `config_robot_gains_load_from_yaml` 保证 yaml / 内置默认一致 —— 改默认
> 值会同时打红两处，防止"文档写一套、代码跑一套"。

### 软边界增益衰减（`soft_boundary`，**delta 路线专用**）

**解决什么**：增量跟手下，手可以继续朝一个方向动而臂已经抵到工作空间边缘，于是之后
每一步都被 `JOINT_LIMIT` 拒绝（实测一轮 20s `rejected=83 / sent=12`）。链路健康、臂不乱跳，
但「手在动、臂不动」的体感很差，日志也被拒绝刷满。

**做法**：每轴独立按"臂离该轴边界还有多远"衰减增益，贴边时衰减到 `soft_min_gain`（下限，
不为 0）。姿态用 **cosine ease（smoothstep）**，两端导数为 0 —— 线性衰减会在进入边界的
那一刻产生导数跳变，手感上是"突然变沉"。

| 配置 | 默认 | 含义 |
|---|---|---|
| `soft_boundary` | `true` | 总开关。**关掉即回到旧行为**（有回归测试钉住） |
| `soft_margin` | `0.15` | 从每端内缩的**比例**（占该轴 span），即"慢速区"宽度 |
| `soft_min_gain` | `0.10` | 衰减下限。**不能为 0**：为 0 时臂无法被命令走出慢速区（任何方向的步长都被缩进死区） |
| **`reach_span_x/y/z`** | **39 / 176 / 41.5** | **实测可达 span（mm）—— 增益与边界的基准**。0 = 回退 `caps` 包围盒 |
| `soft_probe_on_reject` | `true` | 被拒后是否对半重探 |
| `soft_probe_tries` | `2` | 重探次数（含原始尝试）。2 = 原步长 + 一次折半 |

**换臂 / 改机械结构后必须重新实测**（输出了可直接粘进 yaml 的片段）：

```bash
python scripts/measure_reach.py            # 默认 127.0.0.1:9100，--tol 0.25 更精确
```

**三个实现要点**：

1. **逐轴独立**，不做全局降权 —— 臂常常是贴着一面墙而在其它轴仍有空间，全局降权会为了
   修一个问题把另外两个自由轴也拖慢。
2. **衰减需要臂的绝对位置**，所以 delta 路线**重新开始维护 `last_good_mm_`**（从应答
   `tcp` 读）。
3. **⚠️ 局限（必须知道）**：真机**无位置反馈**（坑 7），这里用的是**命令值** `tcp` 作代理 ——
   sim 上准确，真机上"命令值 vs 实际位置"若有偏差，衰减点会随之偏移。

#### 为什么不能直接用 `caps` 的边界（实测推翻的第一版）

`caps.workspace` 是**包围盒**，而且**严重虚大** —— 它由**几何可达**算出来，**不含 IK 是否解得开**。
2026-09-29 用绝对定位（`moveto` 二分，`scripts/measure_reach.py`）在 HOME 高度实测：

| 轴 | `caps` 包围盒 | **实测可达** | 实测 span | 虚大倍数 |
|---|---|---|---|---|
| X (front) | `[40.46, 175.88]` | **`[105.78, 144.78]`** | **39.0 mm** | **3.5×** |
| Y (left) | `[-153.01, 153.01]` | **`[-88.00, 88.00]`** | **176.0 mm** | 1.7× |
| Z (up) | `[49.08, 114.68]` | **`[72.97, 114.47]`** | **41.5 mm** | 1.6× |

**而且 HOME 在三轴上都不居中** —— 这是比虚大更致命的一点：

| 轴 | HOME | **负向余量** | **正向余量** |
|---|---|---|---|
| X | 115.03 | **9.25 mm** | 29.75 mm |
| Y | 0 | 88 mm | 88 mm（唯一居中） |
| Z | 109.22 | 36.25 mm | **5.25 mm** |

所以"拿包围盒算离边界多远"在数学上就是错的：臂真正只剩 **9mm** 行程时，包围盒还说"还剩 60mm"，
权重是 1，照发满速步长，继续被拒。（第一版就栽在这：真机 `rejected 21 / 25 帧`。）

> **绝对定位 vs 步进**：`movexyz` 逐步走只测出"沿**这一条路径**能走多远"，会被关节耦合干扰；
> `moveto` 直接问"这个位姿解得开吗"，答案才属于**点本身**。两者实测一致（X 步进 107~143 / 绝对 105.78~144.78），
> 但绝对法可二分，能拿到 0.25mm 精度。

#### 🔴 增益基准也必须是实测 span（这一步不做，臂就是不动）

第一版修了"衰减边界"，但**增益仍按包围盒算** —— 这是本轮暴露的真正根因：

```
X 轴: axis_scale = (175.88 - 40.46) / 0.5 = 270.85
手部 delta.y = 0.05  →  臂步长 0.05 × 270.85 = 13.5 mm
但 HOME 处 -X 真实余量只有 9.25 mm  ←  一步就跨过头
```

| 轴 | 修复前步长（手动 0.05） | **修复后步长** | HOME 真实余量 |
|---|---|---|---|
| X | 13.5 mm ❌ 超过负向余量 | **1.95 mm** ✅ | -9.25 / +29.75 |
| Y | 30.6 mm | **17.6 mm** ✅ | ±88 |
| Z | 6.56 mm ❌ 超过正向余量 | **2.08 mm** ✅ | -36.25 / **+5.25** |

修法：`axis_scale` 的输入从 `caps` 的 span 换成**实测 span**（`reach_span_x/y/z`）。

- 值为 **0** 时回退到 `caps` 包围盒 —— 对"包围盒确实紧"的设备仍是正确行为，也是未实测设备的唯一合理默认。
- 存的是 **span**（宽度）而非边界：带宽以 `caps.home` 为中心，换臂只需改宽度。

#### 被拒 ≠ 此路不通：拒绝后「细探」

即便步长按实测 span 算，`Z` 正向余量只有 **5.25mm** 这种极端情况仍可能被拒。而原来的学习逻辑是
**一次拒绝就把该轴边界收到当前位置** —— 等于宣告"这个方向彻底不通"。实测 X 负向明明还有 9mm 可走。

修法：**拒绝后按 `soft_probe_tries` 对半重探**（默认 2 = 原步长 + 一次折半）。半长仍低于死区才放弃。

- **必须同时受 `soft_boundary` 约束**：那个开关是"关掉即回到功能出现前的行为"的逃生门，细探属于该功能。
- 细探成功时，学习器只会看到"最终成功"，不会把边界锁在一个**刚被证明可达**的点上（接受分支会重新放宽）。
- 实测效果：`big move: sent 1, rejected 1` —— 一次大步被拒后，半长步长被救回来了。

#### 从拒绝里「学」边界（`eff_min_` / `eff_max_`）

**唯一可信的信息源就是拒绝本身** —— 命令被拒 ⇒ 那个方向的目标不可达 ⇒ 有效边界至多
在臂**当前所在**的位置。

- **收紧**：拒绝时把该轴有效边界拉回到 `min(from+step, from)`。用 `from`（而非
  `from+step`）很关键：被拒的命令**臂根本没动**，用目标值当边界会松一整步，
  **每撞一次墙就多浪费一次拒绝**。
- **放宽**：接受时按 `kRelaxFraction` 缓慢朝实际到达点外扩，且**每 8 次接受才放宽一次** ——
  一次成功只证明**这一个点**可达，不代表整个面。
- **复位**：`open()` 里清掉（`home` 已把臂移到已知位姿，旧的学习结果不再可信）。

#### 慢速区宽度也必须按已学边界算

`margin = soft_margin × span`。若用 `caps` 的 span（X 是 135.43），`0.15 × 135.43 = 20.3mm` ——
**比整条真实 X 轴（39mm）的一半还宽**，臂一离开 HOME 就处在慢速区里，手感是**常年迟钝**而不是贴边减速。
所以 `effective_bounds()` 统一返回"边界 + 该边界的 margin"，且学到的 span 缩小时**取与实测 span 的较大者**，
避免慢速区塌缩成硬停。

#### 顶墙时是「停止推进」，不是「减速推进」

光有衰减还不够：边界收到臂卡死的位置后 `d=0`、权重=下限 `0.10`，
但 `0.10 × 13.54mm ≈ 1.35mm` **仍大于死区 0.5mm** ⇒ 还是每帧发、每帧被拒。

所以还要**方向敏感 + 顶墙抑制**：

- **方向敏感**：朝边界方向的步长才衰减；**往回退的步长权重恒为 1**。
  这保证了「顶住墙推不动，但永远能退回来」。
- **顶墙抑制**：权重已到底且步长仍朝外 ⇒ **直接跳过（不发）**，与死区同语义。

实测效果（`robot_live_backend_drives_sim_arm` 的 (i) 段，真 sim 后端，连推 25 帧）：

| 版本 | rejected |
|---|---|
| 改动前 | **23** |
| 只有衰减（第一版） | 21 |
| + 学习边界 + 方向敏感 + 顶墙抑制 | **1** |

**为什么不直接改成绝对模式**：`--robot-map absolute` 也能避开累积拒绝，但语义变成"指着走"，
丢掉了跟手的手感。软边界保留跟手，只是让臂在边界处安静下来。

### 🔴 `delta` 必须帧间差分（累积量 vs 增量的语义错位）

**症状**：手持续移动时，臂**不是一直生效** —— 中间像卡住（抖动），然后**突然蹿一大步**。

**根因**：`HandState::delta` 的语义是**累积量**：

```
delta = position - reference        // reference 只在首次稳定时锁存一次，之后永不更新
```

而 `RobotLink::step()` 把它当**帧间增量**直接用（`raw_x = hs.delta.y * gain_x`）。
于是**手停住不动时，同一份位移每帧都被重新发一遍**：

```
帧 1  手 moveref帧1  手移动 1cm  delta=0.02  →  发 0.02
帧 2  手不动          delta=0.02  →  又发 0.02   ❌
帧 3  手不动          delta=0.02  →  再发 0.02   ❌
```

每一帧的 0.02 换算成 X 轴步长约 0.39mm，**低于 `deadzone_mm = 0.5`** ⇒ 被跳过 → 看起来"卡住"。
一旦某帧手稍微动一下把步长顶过 0.5mm，臂就动一步 —— 但基准没变，**下一帧继续重发**，
累积起来就是"突然蹿一大步"。

这解释了为什么**测试全绿但真机不对**：参考实现 `gesture_bridge.py::nudge(delta)` 接收的是
**调用方算好的帧间增量**，两端语义从一开始就不一致。

**修法**：在 `RobotLink` 内做**帧间差分** `increment = delta_now − delta_prev`
（等价于 `position_now − position_prev`，手不动时恰好为 0）。架构边界干净：
`HandState` 保持只读、纯逐帧上报，metadata / Web UI 的 delta 消费者完全不受影响。

三条不变式：

| 情形 | 行为 | 理由 |
|---|---|---|
| **第一帧** | 只锁基准、**不发命令** | 没有前驱可减；从 0 起算会把整段累积偏移当成一步发出 —— 正是要修的跳变 |
| **被限流丢掉的帧** | 基准**照常推进**、只是不发 | 否则这一帧的运动被"攒"着，下一帧一次性补发（也是"突然运动"） |
| **参考点未锁存** | 基准**不推进** | 锁存前的 delta 属于被丢弃的窗口，不能成为第一步的原点 |

> 实现落在 `have_prev_delta_` / `prev_delta_[3]`（worker 线程私有），限流判定被**移到差分之后**
> —— 限流器只决定「这帧发不发」，从不决定「手有没有动」。手离开再回来时一并复位
> （回来的手是**新原点**，拿旧基准会算出两段无关位置的差）。

### 手势要的是「开关」，不是「运动」（`gesture_mute_ms`）

**症状**：握拳 → 夹爪闭合的同时臂也在平动；张开同理。操作者只想让它**开关**。

**根因**：**握拳不是静止姿态** —— 手指收拢的过程中 landmark 包围盒缩小、中心漂移，
所以 `position` 在真实地移动；再加上分类器要 `gesture_confirm_frames = 5` 帧
（约 **625 ms** @ 8Hz）才确认手势，这期间有**好几帧"真实可跟随"的手部运动**被机械臂跟了。

**修法**：`gesture_mute_ms` **静默窗口**（默认 **500 ms**，即"手势确认 + 0.5 秒"）。

- 手势**上升沿**触发时置 `muted_until_ms_ = now + mute_ms`；
- 窗口内连续运动被抑制；
- 窗口内**每帧刷新差分基准** ⇒ 等于**丢弃**窗口内位移，避免窗口结束时一次性补发成大跳。

```
握拳确认 ─┬─ gripper close  (1 条)
          └─ 静默 500ms：位移全丢，基准每帧重锁
              └─ 窗口结束：从当前手位继续跟手，不补发
```

`gesture_mute_ms = 0` 即回到旧行为（有对应测试 `robot_gesture_mute_off_is_the_old_behaviour`）。

### 七条协议坑（逐条有对应处理与测试）

| # | 坑 | 处理 |
|---|---|---|
| 1 | 零/极小 delta 是**参数错误**（`zero displacement`） | 低于 `deadzone_mm` 直接跳过并计 `skipped`，**不是**故障 |
| 2 | `state.tcp` 是**命令值**不是过程值 | 只当参考，绝不当作"已经到位"。delta 路线用它维护 `last_good_mm_` 作为**软边界的位置基准**（真机无反馈，命令值是唯一模型）；绝对路线的 `do_moveto` 用它做**目标比对**。两条路线共用同一个闩锁，但用途不同、不互相比对 |
| 3 | 服务端不节流 | 客户端 `rate_hz` 自限 |
| 4 | 包围盒内仍可能不可达（`OUT_OF_WORKSPACE`/`JOINT_LIMIT`） | 保留 `last_good`，**不回零**、不报错。**实测这是常态且远超预期**：包围盒虚大 1.6–3.5×（X 实测 39mm vs 盒 135mm），**HOME 处 -X 只剩 9.25mm、+Z 只剩 5.25mm**。故：① 增益基准改用实测 span；② 被拒后对半细探；③ 拒绝日志留 DEBUG 防刷屏，但**首次连接失败与细探成功是 INFO/WARN** |
| 5 | 目标==当前位置产生零状态帧 | 下发前比对**最后下发目标**（不是应答值）。`last_good_mm_` 按上条只被绝对路线消费 |
| 6 | 边界抄进客户端会漂 | **运行期边界仍一律从 `caps` 取**（实测 `ws=[40.4582,-153.012,49.0849]..[175.883,153.012,114.68]`）。但 `reach_span_*`（增益基准）是**实测值**而非从 `caps` 推导 —— 因为 `caps` 给不出真实可达域。它随机械结构变化，改结构必须重跑 `measure_reach.py` |
| 7 | 真机无位置反馈 | 绿的链路**不等于**物理动了；界面必须区分"已下发"与"已到位"。成功下发打 `CA_LOG_INFO`，肉眼可核 |

### 验收（一键，输出 PASS/FAIL 计数）

```bash
# 全部四级（Level 1 需要在 :9100 跑着 armpilot-backend.exe）
python scripts/verify_gesture_arm.py

# 没有后端也能跑 Level 2
python scripts/verify_gesture_arm.py --no-live

# 三轴可达域实测（换臂 / 改机械结构后必跑，用来刷新 reach_span_*）
python scripts/measure_reach.py            # 需要 :9100 跑着 armpilot-backend.exe
python scripts/measure_reach.py --tol 0.25 # 二分精度（mm），默认 0.25
```

四级阶梯（L2 下分 a/b/c 三组）与对应测试：

| 级 | 验什么 | 测试 |
|---|---|---|
| L1 | VisionFlow 自己的指令直驱真 sim 臂（真 socket / 真 caps / 真 IK） | `robot_live_backend_drives_sim_arm`（`ARM_PILOT_LIVE=1` 才跑，`ctest` 保持无依赖） |
| L2a | 合成像素 → HandState → 指令，对**已知几何真值**断言 | `robot_synthetic_hand_sequence_drives_arm`、`robot_synthetic_sequence_over_worker_thread` |
| L2b | **真实采集帧**走同一条链路（无真值，断鲁棒性） | `robot_realframes_drive_arm_without_ground_truth`、`robot_realframes_over_worker_thread` |
| **L2c** | **软边界 + 后端自愈**（实测 span 基准、被拒后细探、边界关闭时回到旧行为） | `robot_soft_boundary_uses_measured_span`、`robot_soft_probe_recovers_a_halved_step`、`robot_soft_probe_off_sends_only_the_original_step`、`robot_soft_boundary_tapers_gain_monotonically`、`robot_soft_boundary_off_is_the_old_behaviour`、`robot_soft_boundary_survives_rejections` |
| **L2d** | **交付手感**（`gain` 默认值 + 降幅步长表被钉住，防文档/代码漂移） | `robot_shipped_gains_are_the_documented_feel`、`config_robot_gains_load_from_yaml` |
| L3 | 实时摄像头手势控 sim 臂（**人工**） | 见下 |

> L2c 是 2026-09-29 那次"手在动、臂不动"真机故障的回归防线。关键一条是
> `robot_live_backend_drives_sim_arm` 里后来加的**位移断言**：
> `ASSERT(travelled > 10.0f)` —— 只要链路是绿的但臂没真动，它就 FAIL。
>
> L2d 是"降 gain 抑制抖动"那次调优的防线。它存在的理由很实际：
> `gain_x/y/z` 属于**手感**参数，最容易被随手改，而改错了不会让任何行为测试变红
> （行为测试里 gain 被显式 pin 成 1.0）。所以专门用一条测试把 shipped 值 + 步长表
> 写成算术断言，另一条保证 yaml 与内置默认一致。

L3 人工步骤（**必须用 GStreamer 后端**，SIM 后端没有真摄像头）：

```bat
:: 先确认后端枚举到真设备（SIM 后端会报一台假的 "SIM Camera"，不可信）
build-msvc-gst\src\camera-agent.exe --version      :: 期望 Backend : gstreamer
build-msvc-gst\src\camera-agent.exe --list         :: 期望能看到 LRCP USB2.0 等真设备

:: 再启动（MediaMTX 必须在跑，否则视频推流失败）
build-msvc-gst\src\camera-agent.exe --ai --ai-mode hand --robot ^
    --robot-host 127.0.0.1 --robot-port 9100 --config config/camera-agent.yaml ^
    --source mfvideosrc --camera 0 --width 1280 --height 720 --fps 30
```

期望日志：`[ROBOT] connected to ...` → `caps protocol=2 ...` → 手移动时
`cmd=movexyz delta=[..] ok` → 手离开再回来 `hand (re)entered the frame - returning HOME`。

> **`--source` 要用 `mfvideosrc`，不是 `dshowvideosrc`。** 本机这台 LRCP USB2.0
> 走的是 MediaFoundation 栈（`device.api = mediafoundation`）。

### L3 实测记录（2026-09-29，LRCP USB2.0 1280×720@30）

| 项 | 实测 |
|---|---|
| 设备枚举（GStreamer 后端） | `Camera 0: LRCP USB2.0`，最高 1280×720@30 |
| 采集 + 编码 + 推流 | `Negotiated capture format: 1280x720 @ 30fps`，`Status: STREAMING -> rtsp://127.0.0.1:8554/camera01` |
| 手部级联 | `[HAND] ready: palm=... landmark=...`，`conf=0.92~0.98 hand=right` |
| 推理开销 | `infer=~20ms`，`[AI] fps=8.0`（子采样后） |
| 握拳 → 夹爪 | **`[ROBOT] cmd=gripper action=close ok`** —— 真实手势真的驱动了夹爪 |
| 增量跟手 | `cmd=movexyz delta=[-3.65,-7.95,1.81] ok` 等，量级 ±3~30mm |

**两个实测暴露的可用性问题（已处理 / 待观察）**：

1. **检测抖动会误判"手离开"**（已修）。手在画面里时掌心/landmark 级联会偶发漏检，
   实测漏检以 **1–2 帧（≈0.25s）** 的短促形式出现，而 `lost_frames` 原默认 `3`
   恰好压在这个区间上 ⇒ 一次闪烁就被判成"手离开了"，把臂拉回 HOME 并打断跟随
   （20s 内触发 4 次）。已把 `lost_frames` 提到 **6（≈0.75s）**，改后同一场景
   **重入触发降到 0–1 次**。这个值是对着 AI 推理率（~8Hz）算的，不是相机帧率。
2. **撞工作空间边界后持续被拒**（**设计使然，非 bug**）。增量式跟手下，手可以继续
   朝一个方向移动而臂已经到边界，于是之后每一步都被 `JOINT_LIMIT` 拒绝，直到手回退。
   实测一轮 20s 出现 `rejected=83 / sent=12`。这是坑 4 的物理必然，**不是链路故障**
   （`update()` 仍返回 true、链路不断、臂不乱跳）。必要时可用 `--robot-map absolute`
   换成绝对定位路线绕开累积。

> ⚠️ 上面第 2 条记录于**修复软边界之前**。当时的 `rejected=83` 里有相当一部分来自
> "步长基于虚大的包围盒 ⇒ 首帧就越界" 这个真 bug（见坑 4）。修复后同一场景的
> 拒绝大幅下降，且被拒时会有细探救回一个子步。**但"手顶到边界而臂已到位"这条物理
> 必然性依旧存在**，只是不再被增益错误放大。

---

### 两个"必须说清楚"的验证局限

1. **合成序列的拳头是注入的，不是画出来的**。画出来的拳头 MediaPipe 掌心模型**检出 0 个**；
   裁夹具自身掌心区虽然过掌心模型，但被 landmark 模型以 `conf < 0.5` 拒绝 —— 两条路都得不到
   `HandState`。所以拳头通过**注入真实采集的 landmark 表**覆盖（与
   `hand_analyzer_tests.cpp` 冻结的那份**逐位相同**），并在脚本与测试里显式标 `KNOWN LIMITATION`。
   连带约束：`gesture_confirm_frames = 5` 需要**连续 5 帧**，注入必须至少 5 帧才能过手势去抖。
2. **真实帧没有几何真值**，所以 L2b 断言的是**鲁棒性**而不是精度：状态有限且在范围内、
   参考点未锁存时 delta 恒为 0（这条不变式是防"首帧幽灵位移"的）、传输零故障零拒绝、
   确实产生了指令。本机这套 40 帧实测检出率 **20/40 = 50%**（手贴脸被遮挡的另一半检不到，
   这是数据的性质，不是失败）。

---

## 验证流程

### 1. 单元测试（无头）

```bash
ctest --test-dir build-msvc-gst --output-on-failure   # gstreamer 后端构建（默认）
ctest --test-dir build-msvc --output-on-failure       # SIM 后端构建（build_oneclick.bat 产物）
# 或 build 目录（cmake -B build -DCAMERA_AGENT_BACKEND=sim）
ctest --test-dir build --output-on-failure
```

> 需要 `PATH` 里有 GStreamer 的 `bin`（DLL）与 ONNX Runtime 的 `lib`；缺 ORT / 真帧夹具时
> 对应 AI 用例会打印 `[skip]` 并按通过计。

覆盖：摄像头枚举、管线创建、H264 编码、RTSP 连接、RTSP 断开、自动重连（退避 `1/2/5/10s` 封顶验证）、参数错误、正常退出、AI 端到端（detector 真图检出 + ByteTrack 稳定 ID + AIPipeline 生命周期）、YOLO11 解码（通道数判型、双布局合成张量、pose 17 关键点逆变换/conf 不二次 sigmoid）、pose 真模型回归（yolo11n-pose bus.jpg 4 人 17 关键点全落帧内）、**运行时 AI 模式切换（Detect↔Pose、Detect↔Hand 不重启）**、手部（anchor 全 2016 值核对、黄金图 21 点亚毫像素比对、坐标映射/EMA/reference/deadzone/手势去抖、hand 报文逐字节固定）、Metadata 端到端（JSON 字段与 bbox 裁剪、空结果、心跳、有界队列、断服不阻塞、恢复后重连计数、配置解析）、**手势真机数据回归（实测开掌/握拳 landmark 逐位冻结，钉住 `kCurledRatio`）**、**机器人控制（`RobotLink` 状态机 + 七条协议坑、合成序列对已知几何真值、真实帧鲁棒性、真 socket 直驱 sim 臂并断言**臂真的位移**、软边界（曲线/贴边减速/关闭即旧行为/**实测 span 基准**/**拒绝后细探**/**细探开关**）、后端不可达重试与自愈、**交付手感（`gain` 默认值 + 降幅步长表被算术钉住、yaml 与内置默认一致）**、**`delta` 帧间差分（累积量不可逐帧重发、限流帧仍推进基准、未锁参考点不推进基准）**、**手势静默窗口（握拳/张开只开关不平动、窗口内丢弃位移、关闭即旧行为）**）**。**88/88 通过**（sim / gstreamer / LIVE 三模式各跑一遍）。

### 2. 端到端验收（`scripts/e2e-test.ps1`，真实 GStreamer 后端）

```powershell
.\scripts\e2e-test.ps1                 # 默认 --auto（摄像头原生格式）, stream=camera01
.\scripts\e2e-test.ps1 -ForceCaps -Width 1280 -Height 720 -Fps 30   # 需要时才强制 caps
```

> 早期版本默认写死 `240x240@8fps`，那是为某一台 UVC 设备设的，换设备即 caps 协商失败。
> 现在默认 `--auto`，`-ForceCaps` 才是旧的强制路径。

四阶段自动验证：
1. 启动 MediaMTX → agent 推流 → 用 `ffmpeg` 拉 3 帧真实画面（验证采集/编码/推流链路通）。
2. 拉帧计数确认（`capture-HHmmss\frame_*.jpg`）。
3. `taskkill` 掉 MediaMTX → agent **不退出**，状态变 `DISCONNECTED` 并按 `1/2/5/10s` 退避重连（统计 backoff 次数）。
4. 重启 MediaMTX → agent **自动恢复 STREAMING**（从 `mediamtx.log` 实时刷盘的 `stream is available and online` 时间戳判定 auto-resume，避免读 agent 缓冲 stdout 的陈旧数据）。

产物汇总写入 `tests\finished\e2e.log`。

### 3. 内部延迟探针（`--latency-probe`）

仅测 camera-agent 内部（采集→编码→推送），不含 MediaMTX/ffplay/网络：

```bash
camera-agent --auto --source mfvideosrc --latency-probe --duration 8 --log-level info
```

实现：在 `cam.src` / `enc.src` / `parse.src` 挂 buffer probe，按 FIFO 顺序队列配对三段墙钟时刻（**不用 PTS**，编码器会重打 PTS；**用 `h264parse.src` 而非 rtspclientsink**，后者是 request pad 无静态 sink；**videotestsrc 需 `is-live=true`** 防止灌帧破坏 FIFO）。每秒打印：

```
[latency] capture->encode=Xms  encode->push=Yms  total(capture->push)=Zms (n=8)
```

实测稳态 **total ≈ 20ms**（首窗口 ~397ms 瞬态）；>1s 的总延迟不在 camera-agent 内部，由 MediaMTX/ffplay/网络 + 8fps 固有帧间隔（125ms）主导。

### 4. 自动协商验证（`--auto`）

```bash
camera-agent --auto --source mfvideosrc --duration 8 --log-level info
```

预期日志（本机 UVC）：

```
[info] Auto-resolution: not forcing caps; camera negotiates native format
[info] Negotiated capture format: 240x240 @ 8fps
[info] Status: STREAMING -> rtsp://127.0.0.1:8554/camera01
frames=47 dropped=0 ... status=STREAMING   # 8s 内 ~8fps 稳定，0 dropped
```

### 5. Metadata 分支验收（Phase 2）

假服务端（仅标准库，可随时 kill 模拟断服）：

```bash
python scripts/metadata-mock-server.py --port 8000          # 每行一条摘要
python scripts/metadata-mock-server.py --port 8000 --dump   # 打印完整 JSON
python scripts/metadata-mock-server.py --port 8000 --fail-after 20   # 第 20 条后回 500
python scripts/metadata-mock-server.py --port 8000 --die-after 20    # 第 20 条后进程退出
curl http://127.0.0.1:8000/            # 查看累计条数 / uptime
```

| # | 场景 | 做法 | 通过判据 |
|---|------|------|----------|
| 1 | 正常上报 | 起 mock + `camera-agent --ai --metadata --duration 15` | mock 收到 ~5 msg/s；`sent` 单调增，`failed=0 dropped=0`，`latency≈1ms` |
| 2 | 服务端停止 | 运行中 kill 掉 mock | 视频仍 `STREAMING` 且 `dropped=0`；AI 仍 `fps≈5.0`；日志只出 `[METADATA] send failed` + `reconnecting in Nms`，agent 不退出 |
| 3 | 网络/端口不可达 | `--metadata-url http://127.0.0.1:1/...` 启动 | 同上；队列恒 ≤ `queue_size`，`dropped` 增长（丢最老），不无限膨胀 |
| 4 | 服务端恢复 | 断服 20s 后重启 mock | `sent` 自动恢复增长，`failed/dropped` 冻结；`reconnect` +1，日志出 `[METADATA] connection restored` |
| 5 | AI 异常 | 模型路径故意填错后 `--ai --metadata` | 视频正常；AI 不产出结果 → 只剩心跳；agent 不退出 |
| 6 | AI 关闭 | `--no-ai --metadata` | 视频正常；metadata 只发心跳（或 `--metadata-heartbeat 0` 时静默） |

已实测记录见文末「Metadata 分支」章节的验收表。

---

## 调试流程

### A. bat 脚本常见坑（已修复并固化在 `start-camera-agent.bat` / `build_oneclick.bat`）

| 现象 | 根因 | 修复 |
|------|------|------|
| `此时不应有 .` (RC=255) | `if` 块内 `echo "(...)"` 的 `)` 提前闭合 if 块 | echo 串内去括号 |
| cmake 整行被吞作 `-S` 参数 | `%~dp0` 尾反斜杠使 `\"` 转义了引号 | `if "%ROOT:~-1%"=="\" set ROOT=%ROOT:~0,-1%` 去尾斜杠 |
| 错误目录 `carmera-agentbuild-msvc` | `BUILD=%ROOT%build-msvc` 丢失 `\` | `set BUILD=%ROOT%\build-msvc` |
| 日志 0 字节 | `start "t" cmd /c prog > log` 重定向落在 start 的 stdout（新窗口不捕获） | 重定向移入 `cmd /c "..."` 内 |
| `timeout: invalid time interval '/t'` | GNU `timeout` 抢在 Windows 版之前 | 钉 `C:\Windows\System32\timeout.exe` |
| `for /l` 内 `goto` 丢变量 | 块内变量未延迟展开 | 改用 `EnableDelayedExpansion` 标志位，无 goto |

### B. 构建坑（见构建章节约束）

- **MSBuild 不可用** → 一律 Ninja + MSVC。
- **MSYS2 bash 调用 cmake** → rc.exe/mt.exe 找不到 → 用原生 PowerShell/cmd。
- **C4819**（中文源码 UTF-8 警告）→ MSVC 加 `/utf-8`，目标零警告。

### C. 延迟优化（实测 >1s 时的内部卡点定位与修复）

1. **编码未开低延迟**（最大头）：`nvh264enc` 补 `tune=ultra-low-latency zerolatency=true bframes=0 rc-lookahead=0 repeat-sequence-header=true`；`x264enc` 补 `tune=zerolatency bframes=0`；其余 hw 编 `bframes=0 repeat-sequence-header=true`。
2. **整链无 queue**：每段间插 `queue max-size-buffers=2 max-size-bytes=0 max-size-time=0`，避免缓冲堆积。
3. **sink 默认缓冲**：`rtspclientsink latency=0 rtx-time=0`。
4. **观看端**：ffplay 低延迟旗标组（见一键启动章节）。

> 修复后内部延迟从首窗口瞬态降到稳态 ~20ms；总延迟大头在 MediaMTX/ffplay/网络 + 8fps 固有帧间隔，后续若仍 >1s 可进一步调 fps / keyframe_interval（当前 GOP≈3.75s @8fps）。

### D. 真实设备约束（本机 UVC）

- **设备能力随时间变化，不要写死分辨率**：本机先后出现过 "UVC Control"（原生
  240×240 @ 8fps）和 "Integrated Camera"（到 1280×720 @ 10/30）。强制 caps 会在
  不匹配的设备上协商失败 → 进重连死循环。**一律用 `--auto`**。
- 采集源优先 `mfvideosrc`（Media Foundation）；`dshowvideosrc`/`ksvideosrc` 对该设备只出 1 帧。
- 硬编 `nvh264enc`（NVENC）不支持 I420 → caps 不 pin format，交 `videoconvert` 协商。
- `rtspclientsink` 自带 RTP payloader，管线里不要串 `rtph264pay`。
- `videoconvert` 仅转像素格式，**不做缩放**；统一输出尺寸需另加 `videoscale`。

### E. 历史调试探针脚本（`scripts/probe-*.ps1`）

开发延迟/链路问题时创建的独立排查脚本，验证各假设，可参考复用：

| 脚本 | 验证的假设 |
|------|-----------|
| `probe-source.ps1` | dshow/ks/mf 三种源谁能稳定出帧 |
| `probe-mf.ps1` | mfvideosrc + nvh264enc + rtspclientsink 整链 |
| `probe-framerate.ps1` | 不同源在 240×240@8fps 下的产出 |
| `probe-encoder.ps1` | nvh264enc / x264enc 是否接受该小分辨率 |
| `probe-encoder-flow.ps1` | 硬件编码器是否会 stall |
| `probe-format.ps1` | 编码器实际支持的像素格式（NV12 for nvh264enc / I420 for x264enc） |
| `probe-link.ps1` | 哪种描述 rtspclientsink 能 link（是否需 rtph264pay） |
| `probe-push.ps1` | rtspclientsink 自带 payloading 的正确喂法 |
| `probe-pipeline.ps1` | 完整 dshow 管线能否推到 MediaMTX |
| `probe-happy-path.ps1` | 完整 happy path：agent 推流 + ffprobe/ffplay 拉流 |

> 这些脚本多硬编码本机工具路径（如 `D:\data\agent-tools\...`），是一次性排查用途，不保证可移植；正式流程以 `e2e-test.ps1` / `--latency-probe` / `--auto` 为准。

### F. Metadata 分支坑（Phase 2）

| 现象 | 根因 | 修复 |
|------|------|------|
| 服务端已挂，`reconnect` 仍为 0、无 `connection restored` 日志 | **WinHTTP 惰性连接**：`WinHttpConnect` 对不存在的端口也返回成功，`connected()` 一直是 true；断连只在 `send()` 往返失败时暴露 | 重连状态机改由**往返结果**驱动：`send()` 失败 → `register_failure()` 置 `offline_`、起退避窗；首次成功后 `offline_` 由真变假才 `++reconnect_` 并打 `connection restored`。`stats().connected` 取 `transport->connected() && !offline_` |
| 首次连上就打 `connection restored` | 恢复判定只看 `offline_` 会误伤首次连接 | 加 `ever_connected_`：首次成功打 `connected to <url>`，之后恢复才打 `connection restored` |
| 断服期间队列涨到几百条 | 早期实现在离线时仍入队 | 退避窗内直接 `++dropped_` 丢弃（过时 metadata 无价值），队列恒 ≤ `queue_size` |
| `std::clamp` 在 bbox 裁剪上出现 UB | `lo > hi` 时 `std::clamp` 行为未定义（如 `w<=0`） | 自写 `clampi()` 先规整 `hi<lo`，并保证 `x2 ∈ [x1+1, w]` |
| `RecordingTransport` 编译报 C2365 | 测试替身的成员名 `connected` 与成员函数 `connected()` 冲突 | 成员改名 `up` |
| mock server 在 agent 退出时打 `ConnectionResetError` 栈 | agent 直接关闭 keep-alive 套接字，异常发生在 `socketserver.finish_request` 阶段（`BaseHTTPRequestHandler.__init__` 读下一请求行时），早于 `do_POST`，`do_POST` 内的 try/except 包不到 | 在 **`Server` 类（不是 `Handler`）** 覆盖 `handle_error()`：异常是 `ConnectionResetError/BrokenPipeError/OSError` 子类时静默 `return`；已真机验证（agent 退出后 mock 日志 traceback=0） |
| Git Bash 里 `taskkill //PID N //F` 无效（路径转换吃参数），导致"断服窗口"根本没建起来 | MSYS 路径转换 | 改用 `TaskStop` 关后台任务，或在 bash 里 `kill $PID`（进程由 bash 启动） |

---

## 状态与日志

运行期输出摄像头信息、视频参数、流信息、RTSP 地址、状态（`STREAMING`/`DISCONNECTED` 等）以及统计（frames / dropped / bitrate）。关键路径均有 INFO/WARN/ERROR 日志，支持 `--log-level debug`。

自动协商时打印 `Negotiated capture format: WxH @ Ffps`；延迟探针时每秒打印分段延迟。

---

## 异常处理

程序对以下情况做优雅处理，**不会因普通网络错误崩溃退出**：摄像头不存在/被占用、
分辨率/FPS 不支持、GStreamer 缺失、编码器缺失（给出明确 `Required GStreamer ... is not installed.`）、
RTSP Server 不存在、网络断开、服务器断开。断服后进入 `1s → 2s → 5s → 10s`（上限 10s）退避重连，服务器恢复后自动 resume。Ctrl+C 执行：停止采集 → 停止编码 → 停止 RTSP → 停止 AI → 停止 Metadata 发送线程 → 释放 GStreamer → 释放摄像头 → 退出。

**Metadata 服务端异常**（停止 / 网络不通 / 超时 / JSON 编码失败）走独立通道：只记 `[METADATA] ...` WARN 并按指数退避重试，
消息按有界队列丢弃，**不影响 AI 线程、不影响视频链路、agent 不退出**。AI 分支异常同理（模型加载失败 / 推理抛异常都只丢帧不退出）。

---

## 已知问题 / 后续

- **总端到端延迟 >1s**：camera-agent 内部已优化到 ~20ms，大头在 MediaMTX/ffplay/网络 + 8fps 固有帧间隔（125ms/帧）。下一步可调高 fps、缩短 keyframe_interval（当前 GOP≈3.75s），或进一步压 ffplay/网络缓冲。
- **固定输出尺寸**：当前自动协商为原生直通（不缩放）。若需统一输出尺寸（如 1280×720），需加 `videoscale` 元素。
- **枚举择优模式**：当前 `--auto` 为原生直通（取设备默认格式）。如需"自动挑最高分辨率"，可扩展为启动枚举设备 caps 后动态选优（代码中 `--list` 已用 `GstDeviceMonitor`，可复用）。

---

## AI 分支（Phase 1：人检测 + 跟踪，可选）

> **设计铁律**：AI 分支**永远**不破坏视频链路——模型加载失败、推理异常、超时、线程退出都不能让 RTSP 掉帧或 agent 退出。

### 链路

```
Camera (mfvideosrc 1280x720@30)
    |
    +--> videoconvert -> caps -> tee name=aisplit
                                |                          --> enc -> parse -> rtspclientsink  (视频，30fps)
                                +--> queue leaky=upstream
                                     -> videoconvert -> video/x-raw,format=RGB
                                     -> appsink max-buffers=1 drop=true sync=false
                                                |
                                          AI 独立线程
                                          (AIPipeline::thread_loop)
                                                |
                                          YOLOv8n (ORT, 640x640)
                                                |
                                          ByteTrack (Kalman 8 状态)
                                                |
                                          AIFrameResult
```

- **AI 拿原始分辨率 RGB**（1280x720），不做缩放；letterbox 在 C++ 里做以保证 bbox 反变换精度。
- **appsink** 配 `drop=true` + `sync=false` + `max-buffers=1`：流线程永不阻塞；新帧来时旧帧直接丢。
- **AI 队列** `queue leaky=upstream max-size-buffers=2`：再上层缓冲 2 帧，AI 慢时丢最老的、保最新的。
- **AI 关闭时管线字符串逐字节不变**（实测对比 `ai-on` vs `ai-off` 两条 `Pipeline:` 行，前缀完全相同），所以 AI 出问题不可能让视频走别的路径。

### 独立线程模型

- `AIPipeline` 自有 `std::thread`：`push_frame()` 上 GStreamer 流线程**只做短锁 + move + notify_one**，从不阻塞，从不抛异常。
- 子线程在 `process()` 里把 `detector.detect` + `tracker.update` 都包在 try/catch；任何异常 → 记 ERROR、丢一帧、线程继续。
- 连续失败 ≥10 次才再升 ERROR 一次，避免日志爆炸；线程永不退出。
- `consecutive_failures_` / `processed_` / `dropped_` / `skipped_` 全部受锁保护，`stats()` 拷贝出快照。

### 采样策略

- 源 fps ≥ `ai.full_rate_below_fps`（默认 10）→ 按 `1000/ai.fps` 等间隔采（默认 8fps AI = 间隔 125ms；如 30fps 源 + 5fps AI = 间隔 200ms → frame_id 间隔约 6，与规范"每 6 帧一次"一致）。`ai.fps` 可配范围 5-12。
- 源 fps < 10 → **每帧都跑**（慢速摄像头不被进一步饿死）。
- 间隔由 `interval_ms_ = 0` 标记；子线程在 `wait_until(due, pred=!running)` 里醒来再取队首。

### 抽象层（方便换 RKNN）

- `IDetector` / `ITracker` / `AIPipeline`（头文件 `include/camera_agent/ai/`），工厂 `create_detector()` / `create_tracker()` 返回实例。
- 当前 `OnnxYoloDetector` 用 ONNX Runtime C++ API；未来加 `RknnYoloDetector` 只需新加 .cpp + 编译宏切换，**头文件与 StreamController 都不用动**。
- `ByteTrackTracker` 纯 C++，RK3568 同样可跑（ARM 编译即可）。

### 零外部依赖（除 ONNX Runtime）

为不引入 OpenCV / Eigen / 第三方 linalg，自写：

| 自写模块 | 行数级别 | 说明 |
|---------|---------|------|
| 640x640 letterbox + 双线性插值 | ~30 | 保持长宽比 + pad 114 |
| HWC→CHW + /255 归一化 | ~10 | ONNX 输入 tensor 准备 |
| 两 layout 自动探测 + sigmoid + cxcywh→xyxy | ~80 | `transposed = d1<d2` |
| 自实现 IoU + NMS（per-class） | ~50 | 无 OpenCV |
| 8 状态 Kalman（位置/速度） | ~120 | 2 处非参考实现的偏离：状态用 `[cx,cy,w,h,v...]` 而非 `aspect/height`；匹配用贪心而非 `lapjv` |
| ByteTrack 二阶段关联 | ~100 | high-score first (fuse_score 1-iou*score) + low-score (IoU 0.5) + 未确认轨迹 (IoU 0.7) |

### 单元测试（无头端到端）

`tests/ai_detector_tests.cpp` 三个用例（已合入 ctest；含本节在内当时总计 76 个用例，
后续加入机器人/软边界/`delta` 差分等用例后当前总数为 **88/88**）：

- `ai_detector_bus_image`：在 Ultralytics 官方 `bus.jpg` 上跑真模型，断言 ≥1 个 person、bbox 全在 `[0,w]×[0,h]` 内。**实测 4 个 person**。
- `ai_tracker_stable_ids`：3 帧直线漂移的合成检测 → 1 个稳定 track id。
- `ai_pipeline_lifecycle`：init → start → 推 5 帧 → 回调收到结果 → stop 全流程不崩。

测试在 ORT 缺失时自动 skip（输出 `[skip] ONNX Runtime not built in`），仍能 pass。

### 真机验证记录

| 场景 | 摄像头 | 结果 |
|------|--------|------|
| 1280x720@30，**关 AI** | LRCP USB2.0 | 视频 STREAMING，~4 Mbps，**管线串不含 aisplit/aisink/aiconv/aiq** |
| 1280x720@30，**开 AI** | LRCP USB2.0 | 视频 STREAMING 17s/511 帧（≈30fps，**0 dropped**），AI 5.0 fps / infer ≈90 ms / track ≈0.01 ms |
| 同上，画面有人 | LRCP USB2.0 | 20s 内 `id=1 person` 稳定（conf 0.74~0.91），frame_id 间隔 ≈6（30/5），PTS timestamp 单调 |
| `bus.jpg` 端到端 | 静态图 | 4 个 person + bus，bbox 全部在原图坐标内 |

### 已知边界

- **CPU 推理**：i7-8700 @ 640x640 单次约 88 ms（yolo11n 检测）/ 112~150 ms（yolo11n-pose）；1080p 输入（letterbox 后仍为 640x640）下推理时间一致，但 RGB 拷贝到 AI 线程的开销会随分辨率线性增加（720p 一帧 2.7 MB，30 fps 拷贝 ≈80 MB/s，可忽略）。
- **不接 GPU**：`onnxruntime` Windows CPU 即可。RK3568 阶段切 `rknn` 后端。
- **tracker 只支持 class_id 0 (person)**：spec 11 要求不引入 ReID/face recog，因此多类别时多类各自独立跟踪，bbox 共享同一个 track 空间。

---

## Metadata 分支（Phase 2：AI 结果上报，可选）

> **设计铁律**：Metadata 分支**永远**不破坏视频链路，也**永远**不阻塞 AI 线程。
> Server 挂了、网络断了、发送超时、JSON 编码失败，都只记 WARN 并按退避重试；
> Camera → GStreamer → H264 → RTSP 必须照常跑，agent 绝不因 metadata 退出。
>
> 本阶段**只改 Camera Agent**，不碰 Video Server / MediaMTX / Web / 客户端 / 告警引擎。

### 链路与线程模型

```
AI 线程                        MetadataManager（独立发送线程）
  |  push_result()  只做「编码 + 入队」                 |
  +----------------> [有界队列 size=8，满则丢最老] ------+--> connect/POST
                             (非阻塞、不联网、不抛异常)        |
                                                              v
                                                    IMetadataTransport
                                                    （当前：WinHTTP POST JSON）
```

- `push_result()` 跑在 **AI 线程**：只 JSON 编码 + 短锁入队 + `notify_one`，**绝不发起网络请求**。实测对端关闭时 30 次 push 总耗时 <100ms。
- 发送线程独占全部阻塞工作（连接 / POST / 退避 / 心跳）。
- 队列有界（`queue_size`，规范 5~10，默认 8），**满则丢最老保最新**；服务端不可达期间退避窗内的消息直接丢弃，**队列不会无限膨胀**。
- 每一层都 try/catch 隔离；`init()`/`start()` 失败只记 WARN 并继续（视频不受影响）。
- `StreamController` 里 `meta_` 声明在 `ai_` **之前**，保证析构时消费者比生产者活得久。

### 协议（标准 JSON）

帧消息（`AIFrameResult` 直出）：

```json
{
  "version": 1,
  "type": "frame",
  "camera_id": "camera01",
  "frame_id": 15230,
  "timestamp": 1756773210123,
  "video_width": 1280,
  "video_height": 720,
  "objects": [
    { "class": "person", "confidence": 0.93, "track_id": 17, "bbox": [812, 210, 1040, 850] },
    { "class": "person", "confidence": 0.88, "track_id": 18, "bbox": [100, 300, 420, 850],
      "keypoints": [[512.3, 214.6, 0.97], [505.1, 208.2, 0.93], ["…共 17 项…"]] }
  ]
}
```

- `keypoints` 为**加法扩展、可选字段**：仅 pose 模型（如 `yolo11n-pose.onnx`）输出，检测模型的消息与旧版逐字节一致。
- 语义：COCO 17 关键点顺序（0 鼻 … 16 右踝），`[x, y, conf]`，x/y 为**原始视频像素**（已裁剪入帧，2 位小数），conf ∈ [0,1]（模型内已 sigmoid，接收方**不得**再做 sigmoid）。
- 心跳 `ai` 段带 `"keypoints": 17` 表示当前为姿态模式（检测模型无此字段）；`21` 表示手部模式。

**手部模式**（`--ai-mode hand`）在 `objects` 之后**追加**一个 `hand` 块，帧消息的其余部分与上表完全相同：

```json
{
  "version": 1, "type": "frame", "camera_id": "camera01",
  "frame_id": 15230, "timestamp": 1756773210123,
  "video_width": 1280, "video_height": 720, "ai_mode": "ai-hand",
  "objects": [
    { "class": "hand", "confidence": 0.99, "track_id": -1, "bbox": [100, 200, 400, 600],
      "keypoints": [[338, 677, 0.99], ["…共 21 项…", 0.98]] }
  ],
  "hand": {
    "detected": true, "confidence": 0.99, "handedness": "right",
    "reference_initialized": true,
    "position": [0.5120, 0.7340, 0.4800],
    "reference": [0.5000, 0.7000, 0.5000],
    "delta": [0.0120, 0.0340, -0.0200],
    "motion": { "x": "RIGHT", "y": "FORWARD", "z": "STATIONARY" },
    "gesture": "OPEN_PALM", "gesture_candidate": "FIST", "gesture_confidence": 1.00,
    "landmarks": [[338.13, 676.54, -0.00], ["…共 21 项…"]]
  }
}
```

- 手部对象沿用**现有 object 结构**（`objects[]` 里一条 `class:"hand"`），所以前端"画框 + 连关键点"的通用路径不用改；`track_id` 恒为 `-1`（Phase 1 单只手，不跟踪，不编造 ID）。
- `position / reference / delta` 是 **logical 坐标系下的归一化值**（`X+` 右 / `Y+` 前 / `Z+` 上），4 位小数；不是像素，也不是厘米。
- `reference_initialized` 为 `false` 时 **`delta` 无意义、消费方不得使用** —— 这是防止手刚入画时产生一次巨大的幽灵位移。
- `motion` 三轴取值：`STATIONARY` / `LEFT` / `RIGHT` / `FORWARD` / `BACKWARD` / `UP` / `DOWN`；`gesture` 取值：`UNKNOWN` / `OPEN_PALM` / `FIST`。`gesture_candidate` 是**去抖之前**的本帧判定，调试面板用它解释"手势在两种姿态间抖动"。
- `landmarks` 是 21 × `[x, y, z]`，**原始视频像素**；`z` 与 x/y 同尺度，是**相对手腕**的深度（描述手指朝向，**不是离相机多远**，更不是厘米）—— 依据见 `models/README.md`。
- 注意 `objects[].keypoints` 的 x/y 走通用编码会被 `lround` 取整（它是给 canvas 画的），要亚像素/带 z 的数就取 `hand.landmarks`。
- **检测/姿态帧不含任何 hand 字段**（`AIFrameResult::hand` 是 optional，只有 hand 模式才 engage），单测 `hand_absent_on_detect_frame` / `hand_absent_on_pose_frame` 用**逐字节固定字符串**钉住这一点。
- `hand.emit_landmarks: false` 会同时去掉对象的关键点和 `hand.landmarks`，报文显著变小（只留框与派生状态）。

心跳/状态消息（`heartbeat_interval_sec` 周期，默认 10s）：

```json
{
  "version": 1,
  "type": "status",
  "camera_id": "camera01",
  "wall_clock": 1756773215000,
  "ai": {
    "enable": true, "running": true, "fps": 5.00,
    "model": "models/yolo11n.onnx", "tracker": "bytetrack",
    "last_frame_id": 15230, "last_timestamp": 1756773210123, "processed": 99
  }
}
```

关键约束：

| 约束 | 落地方式 |
|------|----------|
| `bbox` 用**原始视频像素** `[x1,y1,x2,y2]`，须满足 `0 <= x1 < x2 <= width` | `bbox_json()` 自写 `clampi()` 裁剪；越界框被夹到画面内；退化框强制 `x2 >= x1+1` |
| **`frame_id` / `timestamp` 必须来自 `AIFrameResult`，发送方不得重新生成** | 编码器原样拷贝（`frame_id` = 摄像头帧计数，`timestamp` = `GST_BUFFER_PTS/GST_MSECOND`）；代码注释已标注该规范条款 |
| 协议须带 `version` | 每条消息都带，`MetadataConfig.version` 可配（默认 1） |
| **每条消息都必须带 `type` 判别字段** | `encode_frame_metadata()` / `encode_status_metadata()` 各自写死 `"type":"frame"` / `"type":"status"`；单测 `metadata_encode_frame`、`metadata_encode_empty_frame` 各有一条断言锁死。2026-09-02 前 `frame` 漏发该字段，零目标帧只带空的 `objects: []`，服务端无法与畸形心跳区分 → 全量 400 |
| 上报频率 ≈ AI 频率（5/s），**不是 30fps** | 一帧一消息，AI 采样率即上报率 |
| **空结果也要发**（服务端据此判断 AI 存活） | `objects: []` 照常编码发送 |
| 服务端地址必须可配，**禁止硬编码** | `metadata.server_url` / `--metadata-url`，无内置兜底地址 |
| 完整 JSON 只在 DEBUG 打印 | `--metadata-log-payload` 控制，默认关；INFO 只打统计行 |

### Transport 抽象

```cpp
class IMetadataTransport {
public:
    virtual const char* name() const = 0;
    virtual bool connect()   = 0;
    virtual void close()     = 0;
    virtual bool connected() const = 0;
    virtual bool send(const std::string& payload, int timeout_ms, double* latency_ms) = 0;
};
std::unique_ptr<IMetadataTransport> create_metadata_transport(const MetadataConfig&);
```

- 当前实现：`HttpMetadataTransport`（**WinHTTP**，Windows 系统组件，**零额外依赖**），`POST` + `Content-Type: application/json`，校验 2xx，读完响应体以复用连接，`steady_clock` 测延迟。
- 非 Windows 目标（未来 RK3568）编译期落到 `NullMetadataTransport`，只新增一个 `http_transport_curl.cpp` 即可切换，**上层与头文件完全不动**。
- `set_transport()` 允许单测注入替身，无需真服务端即可验证投递路径。

### 重连与退避

- 失败后指数退避：`retry_interval_ms`（默认 1000）起，每次 ×2，封顶 `retry_max_interval_ms`（默认 30000），**永不放弃、永不退出**。
- 断连判定由**往返结果**驱动（见调试章节 F：WinHTTP 惰性连接，`connect()` 对死端口也返回成功）。
- 恢复后 `++reconnect_` 并打 `[METADATA] connection restored`；首次连上打 `[METADATA] connected to <url>`，两者用 `ever_connected_` 区分。
- 恢复成功后立即清零 `failure_streak_` 与 `backoff_ms_`，下次故障从最短退避重新开始。

### 日志与统计

```
[info]    [METADATA] transport=http(winhttp) url=... queue=8 heartbeat=10s
[info]    [METADATA] sender started
[info]    [METADATA] connected to http://127.0.0.1:8000/api/metadata
[warning] [METADATA] send failed
[warning] [METADATA] reconnecting in 2000ms (3 consecutive send failures)
[info]    [METADATA] connection restored
[info]    [METADATA] fps=5.0 latency=1ms queue=0 sent=286 failed=19 dropped=171 reconnect=1
```

统计口径（每 1s 滚动窗口算 fps/平均延迟，每 5s 打一行）：

| 字段 | 含义 |
|------|------|
| `fps` | 实际成功上报速率（正常 ≈5.0） |
| `latency` | 单次 HTTP 往返平均耗时（本机 ≈1ms） |
| `queue` | 当前队列深度（应恒 ≤ `queue_size`） |
| `sent` / `failed` | 发送成功数 / 尝试过但失败数 |
| `dropped` | 未尝试即丢弃数（队列满 或 离线退避窗内） |
| `reconnect` | 断连后成功恢复的次数 |

### 验收记录

| # | 场景 | 结果 |
|---|------|------|
| 1 | **正常**：720p@30 + AI 5fps + metadata，15s | 视频 STREAMING（269→390 帧，`dropped=0`，~4 Mbps）；AI `fps=5.0 infer≈80ms`；METADATA `fps=5.0 latency=1ms sent=71 failed=0 dropped=0`；mock 收到 72 条 |
| 2 | **服务端停止**（kill mock，95s 长跑） | 整个断服期间视频 `STREAMING` 且 `dropped=0`，AI 稳定 `fps≈5.0`；`send failed` + `reconnecting in Nms(1s→2s→4s→…)` 按退避出现；`queue` 恒 ≤8，`dropped` 单调增；agent 未退出 |
| 3 | **端口不可达**（`http://127.0.0.1:1`） | 同 2；单测断言 30 次 push <100ms（证明 AI 线程不做网络工作）、`queue_size ≤ 3` |
| 4 | **服务端恢复** | `sent` 自动恢复增长（110→286）、`failed/dropped` 冻结；`reconnect` 计数 +1 并出 `connection restored`（此前因 WinHTTP 惰性连接漏计，已修并由单测 `metadata_reconnect_counted_on_recovery` 锁定） |
| 5 | **AI 异常** | 模型路径故意填错 `models/does-not-exist.onnx` + `--ai --metadata`（`--source videotestsrc`） | 日志 `[AI] model file not found` → `[AI] model init failed (...), video stream continues without AI` → `[warning] [AI] not started; video stream is unaffected`；视频全程 `STREAMING`（frames=424 `dropped=0` bitrate≈4070kbps），AI 不产出结果 → metadata 只剩心跳（`sent=3`，无 `frame` 消息）；mock 收到 cam_t5 共 3 条；agent 正常退出（exit=0） |
| 6 | **AI 关闭** | `--no-ai --metadata`（`--source videotestsrc`） | 视频正常 `STREAMING`（frames=426 `dropped=0` bitrate≈3897kbps）；管线不含 tee/aisplit（AI 分支完全不挂载）；metadata 仅发心跳（每 5s 一条，`sent=3`，mock 收到 cam_t6 共 3 条）；agent 正常退出（exit=0） |

单元测试全部通过（撰写本节时为 **76/76**，当前为 **88/88**）；下表为 Metadata 分支直接相关的 9 个（手部用例见 `tests/hand_wiring_tests.cpp`）：

| 用例 | 断言要点 |
|------|----------|
| `metadata_encode_frame` | 字段名/顺序全对；`confidence:0.93`；越界框夹成 `[0,0,1920,1080]`；**含 `"type":"frame"`** |
| `metadata_encode_empty_frame` | 空结果仍产出 `"objects":[]` 且 `frame_id` 正确；**含 `"type":"frame"`** |
| `metadata_encode_status` | 心跳含 `enable/running/fps/model/tracker/last_frame_id/processed` |
| `metadata_queue_bounded_when_server_down` | 死端口下 30 次 push <100ms、`queue_size ≤ 3`、`dropped+failed > 0` |
| `metadata_not_started_is_noop` | 未 start 时 push 静默忽略，计数全 0 |
| `metadata_config_from_yaml` | 10 个配置项全部来自配置，无硬编码 |
| `metadata_delivers_objects` | 队列→发送线程→Transport 全链路，`track_id`/`bbox`/`frame_id` 原样送达 |
| `metadata_reconnect_counted_on_recovery` | 惰性 Transport（`connect()` 永不失败）下注入 3 次失败，`reconnect==1` 且 `failed==3` |
| `metadata_heartbeat_delivered` | 不发任何帧时心跳仍能送达，含 `"type":"status"` |

### 服务端对接说明

> **已上线**：同仓 `../video-server` 已实现接收端（`POST /api/metadata`，落 SQLite 三表）。
> 见 `video-server/README.md` 第 10 节「AI Metadata 接入」。

对接契约（服务端已按此实现，换其他服务端同样适用）：

1. 提供 `POST <server_url>`，`Content-Type: application/json`，返回 **2xx** 即视为成功（非 2xx 会被记为一次失败并触发退避重连）。
2. 按 `camera_id` 区分设备；`version` 用于协议演进，服务端应容忍未知新增字段。
3. `type` 字段区分两类消息：`frame`（检测结果）/ `status`（AI 心跳）。心跳中 `ai.running=false` 或长时间无 `frame` 消息 = AI 异常。
4. `frame_id` 是**摄像头帧计数**（可能因采样不连续），`timestamp` 是**采集端 PTS（ms）**，`wall_clock` 是**发送端墙钟**——服务端做时序判断请用前两者，判断新鲜度用后者。
5. 上报速率约 5 msg/s/摄像头，服务端按此估算容量；断连期间消息会**主动丢弃**而非补发，服务端不应假设帧序号连续。
6. 服务端可以再夹紧一次 `bbox`/`confidence`，**但不得改写 `frame_id`/`timestamp`**。

### 真机联合验证记录（2026-09-02，video-server 8081）

```
camera-agent --camera 0 --auto --source mfvideosrc --stream camtest \
  --server 127.0.0.1 --port 8554 --ai --metadata \
  --metadata-url http://127.0.0.1:8081/api/metadata \
  --metadata-camera-id camtest --duration 25
```

| 项 | 结果 |
|----|------|
| 视频 | `frames=693 dropped=0 bitrate≈3900kbps status=STREAMING` |
| AI | `fps=5.0 infer≈44ms processed=118`（`objects=0`：画面内无人） |
| Metadata | **`sent=120 failed=0 dropped=0 latency≈8ms reconnect=0`** |
| 服务端落库 | `GET /api/cameras/camtest/metadata` 同时返回 `frame`（`frame_id=719 timestamp=25269 1280x720 object_count=0`）与 `status`（`model=models/yolov8n.onnx tracker=bytetrack fps=5.01`） |
| 摄像头注册 | `GET /api/cameras` → `camtest online 1280x720` |
| 服务端验收 | `python video-server/scripts/verify_metadata.py` → **PASS=35 FAIL=0** |

**修复前**（同一次会话的上一轮）是 `sent=0 failed=5 dropped=90`，全部 frame 被 400 拒收 —— 根因即 `encode_frame_metadata()` 漏发 `type`。抓包手段是 `--metadata-log-payload`。

### 已知边界 / 后续

- 测试 5（AI 异常）与测试 6（AI 关闭）**已补真机脚本化验证**：`video-server/scripts/verify_ai_resilience.py`（真实 camera-agent + 真实 video-server）复跑 **PASS=16 FAIL=0** —— 坏模型下视频仍 `STREAMING`、H264 解码正常、心跳 `ai.enable=true/running=false`、fps=30；`--no-ai` 下视频正常、心跳 `ai.enable=false/running=false`。
- 上述联合验证中 `objects=0`（画面无人），故**真机侧的 `bbox` 落库路径只由服务端验收脚本覆盖**（合成 6 种夹紧场景），未走真实检测结果。下次需对准一个人再跑一轮。
- HTTPS 已支持（`WINHTTP_FLAG_SECURE` 按 scheme 自动开，**默认校验服务器证书链**）。对使用自签证书的 dev 服务端，可通过 `metadata.insecure: true`（YAML）或 `--metadata-insecure`（CLI）放宽校验（忽略 unknown CA / CN / 过期）——**仅限开发环境，生产切勿开启**。
- 无本地落盘/补发：断网期间的检测结果按规范丢弃（过时 metadata 无价值）。若后续需要离线缓存，应加在 Transport 层，不动队列语义。
- 单条消息未做批量合并；若服务端希望降频，可在 Encoder 层做 N 帧聚合（当前按规范保持一帧一消息）。
