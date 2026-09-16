# video-server 项目长期记忆

## 项目定位
中心视频服务器（Go + MediaMTX + Vue3）。Camera Agent 通过 RTSP 推流进来，
Video Server 提供 RTSP 转发 / WebRTC / HTTP REST / 内嵌 Web UI。
**只有一个 MediaMTX** —— 由 video-server 自己拉起（带 :9997 控制 API 供 monitor 轮询），
camera-agent 侧绝不能再起第二个（会抢 :8554）。

## 关键事实
- 端口单一来源在 `config/*.yaml`，**源码里不硬编码**。
  - `config.yaml` → HTTP 8080（本机被 ApplicationWebServer 占用，故联合运行不用它）
  - `config.joint.yaml` → **HTTP 8081** / RTSP 8554 / MediaMTX API 9997 / WebRTC 8889 / HLS 8888
    / ICE 8189 / API 仅绑 127.0.0.1
- **Web UI 是 `go:embed` 编译进二进制的**（不是磁盘直出！旧笔记误记为磁盘直出，曾导致"改了前端
  网页不生效"）。`web/embed.go` 有 `//go:embed all:dist`，`server.go` 用 `fs.Sub(webstatic.Dist,"dist")`
  提供 SPA。因此**任何前端改动都必须两步走**：先在 `web/` 跑 `npm run build` 再生 `web/dist`，
  **再** `go build -trimpath -o video-server.exe ./cmd/video-server` 把新 dist 嵌进二进制，
  最后重启服务。只改 Go 也要 `go build`（无需 npm）。验证：`grep Bs_sVbh6 video-server.exe`
  看新 bundle hash 是否在二进制里。
- `internal/mediamtx/manager.go:94 findBinary()` 三级回退解析 mediamtx：
  配置值 → `mediamtx/mediamtx.exe` → `exec.LookPath("mediamtx")`。
  仓库里**没有** bundled mediamtx，实际靠系统 PATH（`D:\data\agent-tools\mediamtx_v1.20.1_windows_amd64`）。
- MediaMTX 运行时配置每次启动生成到 `data/mediamtx-*.yml`（临时名，不覆盖入库文件）。
- `internal/netiface` 负责枚举 IPv4 并排序：物理私网网卡 > 虚拟网卡 >
  公网 > link-local > loopback。API `GET /api/net/addresses` 的 `public_host` 就是权威 LAN 地址。

## 一键启动（scripts/start-joint.bat）
顺序：**(1) 清残留 → (2) mtime 比对过期则自动 go build → (3) 预检 → (4) 读端口 →
(5) 起 server → (6) 自动挑摄像头索引 → (7) 起 agent → (8) 等自动注册 → (9) 开浏览器 → 摘要**。
- 清残留必须**前置**：Windows 对运行中的 exe 持独占句柄，否则重建 `Access is denied`。
- `:pick_exe` 子程序取最新 `video-server*.exe`（`dir /b /o-d`），被调用两次（起始 + 重建后）。
- agent 必须带 `--auto`（UVC 摄像头原生 240×240@8fps，强制 1280×720 会 caps 协商失败进重连死循环）。
- 停止：`scripts\stop-joint.bat`。

## 验收
- `python scripts/verify_joint.py --stream camera01` —— 真实推流端端到端，PASS/FAIL 计数
  （当前 **PASS=14 FAIL=0 INFO=2**）。
- stage 7 的 WHEP 502 是**合成 SDP 缺 ice-ufrag** 导致的 400 转 502，**非缺陷**，属 INFO。
  真实浏览器 WebRTC 已验证可用（日志见 `peer connection established` + `is reading ... 1 track (H264)`）。

## ⚠️ HLS 黑屏根因（WebRTC 通、HLS 两端黑屏）—— 已修复并验证
- 根因：`manager.go:GenerateConfig()` 只写 `hls: true`、**没钉 `hlsVariant`** → MediaMTX v1.20.1
  默认 `lowLatency`（LL-HLS/fmp4）。LL-HLS 的 GAP 占位段 `gap.mp4` 被 MediaMTX 以 `text/html`
  返回，hls.js（`lowLatencyMode:true`）拿到非视频 MIME → 致命错误 → 黑屏。
- 代理层正常（index/媒体/init 都 200 `video/mp4`），故障在浏览器端解析 LL-HLS。
- **已修复（源码已改，2026-09-02）**：`manager.go:GenerateConfig()` 的 hls 块加 `hlsVariant: mpegts`
  （经典 TS，最稳兜底）。`go build -trimpath -o video-server.exe ./cmd/video-server` 重建通过（`go vet` 干净）。
- **验证（同次会话端到端）**：ffmpeg 推 H.264 测试流 → RTSP :8554/camera01 → 代理 :8081/hls/
  主播放列表 200 `application/vnd.apple.mpegurl` → `main_stream.m3u8` **无 `EXT-X-GAP`** →
  首片 `8c2eee06a3d5_main_seg16.ts` HTTP 200 `Content-Type: video/mp2t`（42488B，魔数 0x47 TS 同步）→
  根因已从源头消除，hls.js 不再触发致命错误。验证进程与临时文件已清理。

## ⚠️ HLS 网页仍黑屏的二次根因（2026-09-02 二轮）
- 后端改 `hlsVariant: mpegts` 后网页**仍黑**，真因不是后端、也不是 lowLatencyMode 对经典流无害——
  而是 **Web UI 是 `go:embed` 进二进制的**（见上"关键事实"）。上一轮只改了 Go 没 `npm run build`+
  `go build`，运行中的二进制从没包含任何修复，所以网页纹丝不动。
- **前端对齐修复**（Web UI 是经典 mpegts，**不是** LL-HLS）：`web/src/webrtc/player.ts`
  `startHLS()` 的 `new Hls(...)` 把 `lowLatencyMode: true` 改为 `false`，并加 `fragLoadingMaxRetry/
  manifestLoadingMaxRetry/levelLoadingMaxRetry=6` + 对 `NETWORK_ERROR` 调 `hls.startLoad()`、
  `MEDIA_ERROR` 调 `hls.recoverMediaError()` 自愈（不再一错就整段重载）。
- **正确交付流程（二轮已执行）**：`cd web && npm run build`（产 `dist/assets/index-Bs_sVbh6.js`）
  → `go build -trimpath -o video-server.exe ./cmd/video-server`（把新 dist 嵌进二进制）。
  `grep Bs_sVbh6 video-server.exe` 确认新 bundle 已嵌入。重启服务后网页 HLS 即生效。
- **回归门 hardening**（见 `scripts/verify_e2e.py`）：`--host` 默认 `127.0.0.1`（MediaMTX RTSP
  监听 IPv4-only，`localhost` 会解析到 `::1` 导致推流静默失败）；自动选连续空闲端口段；ffmpeg
  stderr 落盘；注册失败自动查 mediamtx `paths/list`。实测 `--streams 1`→PASS=8、`--streams 2`→PASS=11，
  均 FAIL=0（含 `hls playback` mpegts/video/mp2t）。

## ⚠️ HLS 网页仍黑屏的三次根因（2026-09-02 三轮，用户"重建后仍黑"的真因）
- 上一轮"embed 重建"是**必要非充分**：用户重启新二进制 + 硬刷新后 HLS **仍黑**，说明还有独立 bug。
- **真因：`web/src/webrtc/player.ts` 在 WebRTC↔HLS 切换时没清掉 `<video>` 上的 `srcObject`**。
  - 播放器先走 WebRTC，`ontrack` 把 WebRTC 的 `MediaStream` 赋给 `video.srcObject`。
  - 点 HLS（或 WebRTC 超时/失败回退）时 `teardownPeer()` 只 `pc.close()`，**没清 `srcObject`**；
    随后 `hls.attachMedia(video)` 想用 `video.src` 驱动元素，但 `srcObject` 优先级高于 `src` →
    hls.js 拉到的 `.ts` 切片**永远渲染不出来** → 黑屏（元素停在 2×2 占位，`currentTime` 卡 0）。
  - **curl 测不出、浏览器必踩**：代理层 `.ts` 返回 200 `video/mp2t` 完全正常，所以 HTTP 级验证全绿，
    但 `<video>` 因 srcObject 冲突不出画。这是纯前端切换逻辑缺陷，与 embed/mpegts 无关。
- **如何定位**：用 Playwright 把"服务+ffmpeg 推流+Chromium"装进**同一个 node 进程**（避免跨调用回收），
  1) 同源页注入 hls.js 直连代理 → 出画（320×240、ct 推进）→ 证明代理/后端正常；
  2) 走真实 App 路由 `/`→点卡片→点 HLS 按钮 → 只拉播放列表、不拉 `.ts`、元素 2×2 → 复现黑屏；
  3) 同一流程在 `page.evaluate` 里 `video.srcObject=null` 后再点 HLS → 立刻出画 → 锁定 srcObject 冲突；
  4) 翻转 `enableWorker` 验证不是 Worker 问题（隔离测试 `enableWorker:true` 照样出画）。
- **已修复（`player.ts`，2026-09-02 三轮）**：
  - `teardownPeer()`：close 后 `if (this.video.srcObject) this.video.srcObject = null`
  - `startHLS()` 开头：`this.video.srcObject = null`（防御性，hls 接管前清场）
  - `teardownHLS()` / `connectWebRTC()` 开头：`removeAttribute('src')`+`load()`+`srcObject=null`，
    保证双向切换都从干净 `<video>` 开始。
  - 重建：`npm run build`（产 `index-BsYIT4ll.js`）→ `go build -trimpath -o video-server.exe`（13:57:51）。
- **验证（真实 App 流程，无手动补丁）**：点 HLS 后 `.ts` 切片 200 `video/mp2t`、元素 320×240、
  `currentTime` 0.04→14.04s 推进、`paused=false` → **出画，黑屏消除**。`bufferStalledError` 仅为
  headless 下合成源偏慢的非致命告警，不影响播放。
- **铁律**：任何"WebRTC 与 HLS 共用同一 `<video>` 元素"的播放器，切换传输时**必须清 `srcObject` 与 `src`**，
  否则另一端渲染被压制。这是视频元素 srcObject 优先于 src 的固有语义。

## 能力沉淀
- 服务端经验已抽成 skill `soc-video-streaming`（系统级 + 项目级 `.workbuddy/skills/` 各一份），
  与 `soc-camera-rtsp-agent` / `soc-windows-gstreamer-build` / `soc-debug-verification` 分工互补。

## HLS 时延要点（2026-09-02 实测）
- HLS 端到端 ≈ 发布端滞后（~0.75s，≤1 段边界）+ hls.js frontier gap（段数×段长）。
  **player.ts 已用低延迟 live 参数**：`liveSyncDurationCount:1, liveMaxLatencyDurationCount:6,
  maxLiveSyncPlaybackRate:2, maxBufferLength:8`（bundle index-Ct3uBpII.js）。
- 服务端分段时长被上游关键帧间隔钳制（MediaMTX mpegts 无 IDR 不起新段）：GOP 1s⇒1s 段、
  GOP 4s⇒4s 段。**降 HLS 时延的前提是推流端 GOP≈1s**（camera-agent --auto 已自动 keyint=fps）。
- 测量工具：`scripts/hls_latency_probe.js A|B|C`（需 NODE_PATH 到 managed playwright workspace）。
- Git Bash 杀进程必须 `MSYS2_ARG_CONV_EXCL="*" taskkill /F /IM x`，否则 `//F` 被路径转换报错、静默不杀。

## 目录约定
`cmd/` `internal/` `config/` `data/`（运行时态，不提交）`logs/` `mediamtx/`（仅 LICENSE+yml，无二进制）
`web/` `scripts/`。
