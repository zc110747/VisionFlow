# carmera-agent 项目长期记忆

## 项目定位
Windows PC 摄像头 → GStreamer → H.264 → RTSP 推流，模拟未来 RK3568 嵌入式摄像头设备。
后端可切换：`sim`（仿真，无摄像头依赖）/`gstreamer`（真实采集）。由 `CAMERA_AGENT_BACKEND` 控制。

## 构建约定（Windows / 本机）
- **不能用 MSBuild 生成器**（沙箱下直接崩溃，安全策略拦截 + MSYSTEM 污染）。一律用
  **Ninja + MSVC**，封装在 `scripts/build-msvc.ps1`（PowerShell，非沙箱模式跑）。
- 工具链必须指向 **VS 自带原生** CMake/Ninja（`D:\Software\vs\Common7\IDE\CommonExtensions\Microsoft\CMake\...`），
  绝不用 MSYS2 的 cmake（冒号分隔 PATH 让 rc.exe 找不到）。
- cl.exe 显式指定：`D:/Software/vs/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe`；
  PATH 补 `Windows Kits\10\bin\10.0.26100.0\x64`（rc/mt）。
- 源码 UTF-8 → MSVC 加 `/utf-8` 消 C4819。目标：零警告。
- spdlog 由 FetchContent 拉取（需 GitHub 可达）。
- **后端是编译期宏 `CAMERA_AGENT_BACKEND`（sim/gstreamer/auto），不是运行时旗标**：`auto` 仅当构建期探测到
  GStreamer 才解析为 gstreamer，否则回退 sim。构建 GStreamer 后端：`scripts/build-msvc.ps1 -Backend gstreamer`
  （默认落到 `build-msvc-gst`，与 SIM 的 `build-msvc` 分离），脚本会写 `<BuildDir>/backend.txt` 标记。
- **SIM 后端是 headless 模拟器，绝不向 MediaMTX 推流**（rtsp_publisher_sim.connect 只置 connected_=true 不建连；
  video_pipeline_sim.run 只数帧）。所以 SIM 包跑 `start-joint*.bat` 必现 `no stream is available on path 'camera01'`
  / 视频全 offline。**要看实时画面+AI 框必须用 GStreamer 后端 + 本机真实摄像头**。

## 真实设备约束（本机）
- 唯一摄像头 "UVC Control"，原生 **240×240 @ 8fps**；默认分辨率协商失败。
- 采集源优先 **mfvideosrc**（Media Foundation）；dshowvideosrc/ksvideosrc 对该设备只出 1 帧。
- 硬编 **nvh264enc**（NVENC），不支持 I420 → caps 不 pin format，交 videoconvert 协商。
- `rtspclientsink` 自带 RTP payloader，管线里不要串 `rtph264pay`。

## 验收
- 单测 **29/29**（10 基础 + AI/YOLO11 解码/pose 真模型回归/运行时 AI 模式切换 + Metadata；+2 metadata 编码器盖戳 ai_mode 测试）；
  端到端 `scripts/e2e-test.ps1`（MediaMTX 收流 + ffmpeg 拉帧 + 断服重连 + auto-resume）。
- auto-resume 判定读 mediamtx.log（实时刷盘），不读 agent.log（stdout 缓冲陈旧）。
- Metadata 验收用 `scripts/metadata-mock-server.py`（仅标准库；`--dump` / `--fail-after N` / `--die-after N`）。

## AI 分支（Phase 1）与 Metadata 分支（Phase 2）铁律
- **两条铁律**：AI 异常不能断视频；Metadata 异常既不能断视频也不能阻塞 AI 线程。
- AI 线程里 `push_result()` 只做「编码 + 短锁入队 + notify」，**绝不联网**；阻塞工作全在发送线程。
- 队列一律**有界、丢最老保最新**；Metadata 在**离线退避窗内直接丢弃**（不过时数据无价值），
  所以断服时队列不膨胀。
- `StreamController` 里 `meta_` 声明在 `ai_` **之前**，保证析构时消费者比生产者活得久。
- 协议：`{version,type,camera_id,frame_id,timestamp,video_width,video_height,objects[{class,confidence,track_id,bbox[x1,y1,x2,y2]}]}`；
  `type` 区分 `frame` / `status`（心跳）。`frame_id`/`timestamp` **必须原样拷贝自 AIFrameResult，禁止重新生成**。
- 服务端地址必须可配（`metadata.server_url` / `--metadata-url`），**禁止硬编码**。

## WinHTTP 惰性连接（踩过的大坑）
- `WinHttpConnect` 对不存在的端口也返回成功，所以 `transport->connected()` 在服务端已挂时仍是 true。
  **断连只能在 `send()` 往返失败时发现**。
- 因此重连状态机必须**由往返结果驱动**：`send()` 失败 → 置 `offline_` + 起退避窗；
  首次成功后 `offline_` 由真变假才 `++reconnect_` 并打 `connection restored`；
  用 `ever_connected_` 区分"首次连上"与"断后恢复"。
- 对外暴露的 `stats().connected` 要取 `transport->connected() && !offline_`。
- 该回归由单测 `metadata_reconnect_counted_on_recovery` 锁定（用 `connect()` 永不失败的
  `FlakyTransport` 注入 3 次 send 失败，断言 `reconnect==1 && failed==3`）。

## Web 驱动 AI 模式切换（agent 轮询，2026-09-04 完成）
- 三态：`ai-off`（仅 Web 隐藏 overlay，agent 不换模型）/ `ai-y`（yolo11n 人检测）/
  `ai-y-pose`（yolo11n-pose 17 关键点姿态）。**默认 `ai-y`**。
- 控制链路：Web `POST /api/cameras/{id}/aimode` → video-server 落 `ai_aimode` 表 →
  agent 独立线程 `aimode_poll_loop` 轮询 `GET /api/cameras/{id}/aimode` 拿 `mode` 字段，
  调 `AIPipeline::request_mode()` → AI 线程 `apply_mode()` 锁外重建 detector、锁内换 `detector_` 指针。
- agent→server 是单向 WinHTTP POST（metadata），**无反向通道** → 故用 agent 主动轮询（不是 server 推送）。
- `ai-off` 语义：agent 忽略该请求，**保持启动模型**；仅 Web 隐藏 overlay（符合需求：不通知停模型）。
- `ai-y`/`ai-y-pose` 才触发运行时模型重建并重绘。`apply_mode` 慢的 ONNX 加载在锁外，指针交换才加锁 → 不阻塞视频/采集线程。
- 协议/字段：`frame.objects[].keypoints` 为 `[x,y,conf]`，仅 pose 模型有；video-server `Object.Keypoints` 存 `ai_object.keypoints`（TEXT JSON）。
- 测试：agent `ai_pipeline_switch_models`（Detect↔Pose 不重启）；video-server `TestAIMode*` + `TestSaveFrameRoundTripsKeypoints`；web `client.ts` 加 `AIMode`/`getAIMode`/`setAIMode`，`VideoPlayer.vue` 加三模式选择器 + COCO 骨架绘制。
- **全链路 live e2e 已验证（2026-09-04 续做）**：真实 `video-server.exe` + 自带 MediaMTX + agent sim 后端
  （`--source videotestsrc`，x264enc 软件编码兜底，无 GPU 亦可跑）跑通：curl `POST /api/cameras/camera01/aimode`
  `{"mode":"ai-y-pose"}` → agent `aimode_poll_loop` 轮询命中 → 日志 `model switched -> mode=pose`（加载 yolo11n-pose.onnx），
  `POST ai-y` → `model switched -> mode=detect`，**全程不重启**。ai-off 仅 Web 隐藏 overlay，agent 保持启动模型。
  video-server API live 冒烟 8/8 通过（默认 ai-y / 往返 / 非法 400 / ai-off / 新相机默认）。
- **切换期丢帧统一规则（本续做修正第二版）**：AI Meta 在 payload 内**自带 agent 实际运行模式** `ai_mode`
 （`ai-y`/`ai-y-pose`），由 agent `current_mode_` 逐帧盖；前端只合成 `嵌入模式 == 选中模式` 的帧，否则 `clearOverlay`
  丢弃——**只动 `<canvas>` overlay，绝不碰 WebRTC `<video>`**。从而 `ai-off` 也被统一（无帧被盖 `ai-y-off`，旧帧天然 mismatch）。
  早前 live `GET /metadata` 看似缺 `ai_mode` 实为读到了切换前的旧 NULL 帧（`omitempty` 省略），`ai_frame.ai_mode` 列经
  `addColumnIfMissing` 已自愈；SIM 后端无人脸不跑推理，故 agent 盖戳 live 演示需真机/带人帧。

## video-server schema 前向兼容（2026-09-04 修）
- 现象：joint 运行 agent 推 metadata 时 server 回 `HTTP 500` —— `table ai_object has no column named keypoints`。
  根因：`ai_object.keypoints` 列是 pose 模型需求新增的，但 `data/video.joint.db` 由旧 schema 创建；
  `Migrate` 用 `CREATE TABLE IF NOT EXISTS` 对**已存在**的表不会加列，旧表缺列 → INSERT 失败。
- 修复：`Migrate` 末尾加 `addColumnIfMissing(db,"ai_object","keypoints","TEXT")`（PRAGMA table_info 探测，
  缺列才 `ALTER TABLE ... ADD COLUMN`）。旧库启动即自愈，无需手动清库。回归测试
  `TestMigrateAddsKeypointsColumnToExistingDB`（造旧 schema → Migrate → SaveFrame 带 keypoints 成功落库）。

## 一键启动 / 目录约定（2026-08-31 新增）
- 根目录 `start-camera-agent.bat`：一键启动脚本（纯英文 .bat）。
  流程：(1) taskkill mediamtx/camera-agent/ffplay；(2) 顺序 start mediamtx → camera-agent → ffplay；
  (3) 打印 RTSP URL 等访问信息。日志重定向到 `tests/finished/`。
- **外部工具走 PATH，不写绝对路径**（mediamtx/ffplay/ffmpeg/gstreamer 均已入系统 PATH）；
  仅本项目 `build-msvc\src\camera-agent.exe` 用相对路径，便于移植。
- 生成的调试产物（*.log / *.err / *.h264 / capture-* / auto.crt / auto.key）统一归 `tests/finished/` 或忽略；
  `.ps1` 脚本留在 `scripts/` 原目录。
- 根目录 `mediamtx.yml`：MediaMTX demo 配置，关键是 `paths: all_others:`（允许任意路径发布，否则报
  "path 'camera01' is not configured"）。bat 先 `cd /d %ROOT%` 保证 mediamtx 从此 yml 启动。
- **坑**：`start "... " cmd /c "prog > log 2>&1"` 内嵌引号会让 cmd 重定向解析失败（日志文件不生成）。
  项目路径无空格 → cmd /c 内不加内层引号即可。
- `.gitignore` 已补：*.err *.log *.h264 capture-* auto.crt auto.key。

## Skills 沉淀位置（跨会话约定）
- 端侧 AI 零依赖配方（Phase 1 YOLO+ByteTrack + Phase 2 Metadata 上报）沉淀在 **`soc-edge-ai-zerodep`**，**双份存储**：
  用户级 `~/.workbuddy/skills/soc-edge-ai-zerodep/SKILL.md` 与
  项目级 `D:\user_project\git\SOC-Agent\.workbuddy\skills\soc-edge-ai-zerodep/SKILL.md`。
  **改一处必须两边同步**（2026-09-03 同步过一次）。
- 项目级 skills 目录另有 `soc-camera-rtsp-agent`、`soc-windows-gstreamer-build`（主架构 / MSVC 构建）。

## video-server SQLite 并发铁律（2026-09-04 修）
- `database.Open` 必须开 **WAL**（`journal_mode=WAL` + `synchronous=NORMAL` + `busy_timeout` + `SetMaxOpenConns`），
  否则回滚日志下 agent 写事务 / web 读 / monitor UPSERT 互相阻塞 → 偶发 `SQLITE_BUSY`。
- handler 严禁把瞬时锁误判为永久错误：`getCamera`/`cameraStream` 的 `repo.Get` 错误=404、`cameraMetadata` 的
  `meta.Latest` 错误=500 曾因此误报。
- 所有读写路径包 `dbutil.RetryOnBusy`（仅在 "database is locked"/SQLITE_BUSY/(5) 时重试）；
  真正缺失的相机仍返回 404（语义正确）。回归测试 `internal/database/concurrent_test.go`。
