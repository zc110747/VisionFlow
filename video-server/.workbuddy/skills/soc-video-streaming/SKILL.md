---
name: soc-video-streaming
description: SOC/嵌入式摄像头视频流服务端开发：video-server(Go) 管理 MediaMTX 子进程 + WebRTC 信令代理 + HLS 反向代理 + 摄像头注册发现，前端 Vue3+hls.js（WebRTC 优先 / HLS 兜底）。覆盖 MediaMTX 集成（findBinary 三级回退、GenerateConfig 动态配置、WHEP 代理、HLS 代理）、WebRTC vs HLS 决策与手机兜底设计、【真实 Bug】WebRTC 通但 HLS 两端黑屏的根因与修复（运行时配置未钉 hlsVariant→默认 lowLatency→gap.mp4 以 text/html 返回→hls.js 致命）、一键构建/启动脚本、沙箱诊断方法论。适用于"video-server 开发""MediaMTX 集成""WebRTC/HLS 播放""摄像头流媒体服务端""HLS 黑屏排查""推流到浏览器"。触发词：video-server、MediaMTX、WebRTC、HLS、WHEP、hls.js、流媒体服务端、摄像头播放、推流到浏览器、HLS 黑屏、lowLatency、fmp4。
agent_created: true
---

# SOC 视频流服务端（video-server + MediaMTX + WebRTC/HLS）

把"一台 SOC/嵌入式设备（或它的 Windows 仿真器）的摄像头画面，低延迟推到浏览器（主机 + 手机）"做成可复用的服务端模板。
本 skill 是**服务端（RTSP Server + Web 服务 + 信令/代理）**规范；推流端见 `soc-camera-rtsp-agent`，GStreamer 构建见 `soc-windows-gstreamer-build`，Windows 脚本调试见 `soc-debug-verification`。

## 一、架构全景（先把链路画清楚）

```
 camera / camera-agent            MediaMTX                  video-server (Go)              Browser
 (GStreamer RTSP publisher)  ──RTSP──▶  (sub-process)   ──control API──▶  HTTP :8081
        │                                  │  WebRTC :8889                  │  /api/cameras/{id}/webrtc  (WHEP 代理)
        │                                  │  HLS    :8888                  │  /hls/{stream}/{file}        (反向代理)
        │                                  └──────────────────────────────────┘
        └── publishes rtsp://host:8554/camera01 ───────────────────────────────────────▶  WebRTC(亚秒) / HLS(兜底)
```

| 组件 | 职责 | 关键文件 |
|------|------|----------|
| `video-server` (Go) | HTTP API、管理 MediaMTX 生命周期、WebRTC 信令代理、HLS 反向代理、摄像头注册/发现、健康检查 | `cmd/video-server/main.go`、`internal/server`、`internal/mediamtx/manager.go`、`internal/api/{handlers,hls,router}.go` |
| `MediaMTX` (子进程) | 媒体服务器：RTSP 收流 + WebRTC/HLS 出流。**由 video-server 拉起并写动态配置** | `internal/mediamtx/manager.go` 的 `GenerateConfig()` |
| `camera-agent` (C++/GStreamer) | RTSP 推流端（Publisher）。未来换 RK3568 真机只换采集源 | 见 `soc-camera-rtsp-agent` |
| 前端 (Vue3+Vite+hls.js) | WebRTC 优先，超时 7s 自动切 HLS 兜底 | `web/src/webrtc/player.ts`、`web/src/components/VideoPlayer.vue` |

> 数据流：camera `--RTSP-->` MediaMTX `--WebRTC/HLS-->` browser。video-server 自己**不解码不转码**，只做信令中转与 HTTP 代理。

## 二、MediaMTX 集成要点（最容易踩的坑都在这）

### 2.1 findBinary 三级回退（让 exe 在任意机器都能启动）
`internal/mediamtx/manager.go` 的 `findBinary()`：
1. 配置里的显式路径（Windows 自动补 `.exe`）
2. 工作区内置副本 `./mediamtx/mediamtx(.exe)`
3. `exec.LookPath("mediamtx")`（系统 PATH）

> 旧版只对配置值做 `os.Stat` 会报 `mediamtx binary not found`；新版三级回退后 mediamtx 在 PATH 里即可。**改了源码必须重新 `go build`**，否则跑的是旧 exe（本项目真实踩过的坑，见 §六）。

### 2.2 GenerateConfig 每次启动动态生成 mediamtx 配置
- 端口（RTSP/WebRTC/HLS/API/ICE）以 `config.yaml` 为**单一事实来源**，写入 `data/mediamtx-*.yml`（临时名，避免覆盖签入文件）。
- **关键认知：运行时用的是动态生成的 `data/mediamtx.yml`，不是 `mediamtx/mediamtx.yml` 静态文件！** 静态文件里的 `hlsVariant: lowLatency` 等项在运行时**完全不生效**。要改 mediamtx 运行时行为，必须改 `GenerateConfig()` 里的格式化字符串（见 §四）。
- 关掉 `rtmp/srt/moq`（只 RTSP/WebRTC/HLS），少开 3 个监听口，否则第二个实例起不来。
- WebRTC 必须开 `webrtcLocalTCPAddress`（ICE over TCP）：手机 Wi-Fi 常挡 UDP，这是"桌面能播、手机黑屏"的经典根因之一（在 WebRTC 侧）。

### 2.3 WebRTC 信令代理（WHEP）
`cameraWebRTC()` POST `/api/cameras/{id}/webrtc` → 转发 SDP offer 到 MediaMTX `http://127.0.0.1:8889/{path}/whep`，返回 answer。浏览器只跟 video-server 的 8081 端口打交道，WebRTC 端口不对外暴露、无 CORS 问题。

### 2.4 HLS 反向代理
`hlsProxy()` GET `/hls/{stream}/{file}` → `http://127.0.0.1:8888/{stream}/{file}`。同域、单端口、纯 TCP，手机只需开一个防火墙口。透传 `?session=` 查询（MediaMTX 的 LL-HLS 会话亲和）。

## 三、WebRTC vs HLS 决策（手机兜底设计）

| | WebRTC | HLS |
|---|---|---|
| 延迟 | 亚秒 | 数秒（LL-HLS 更低但仍 >1s） |
| 传输 | UDP(+TCP ICE) | 纯 HTTP/TCP |
| 手机痛点 | Wi-Fi 挡 UDP/ICE；iOS Safari 在 http 源拒绝 WebRTC | 无（纯 TCP + 原生支持） |
| 角色 | 默认优先 | **兜底** |

前端 `player.ts`：`connectWebRTC()` 起手，7s 无帧 → `startHLS()` 兜底；iOS Safari 走原生 HLS（`canPlayType('application/vnd.apple.mpegurl')`）。
> 设计意图：**HLS 必须是最稳的那条路**，因为它专门用来救 WebRTC 失败的手机。所以 HLS 绝不能选最脆的格式。

## 四、【真实 Bug】WebRTC 通，但 HLS 主机+手机都黑屏

### 现象
WebRTC 两端（桌面 + 手机）正常出画；点 HLS 按钮（或 WebRTC 超时兜底到 HLS）后两端都黑屏，前端 `player-detail` 显示 `HLS error: ...` 或一直 `loading HLS`。

### 根因（已实测确认）
`manager.go` 的 `GenerateConfig()` 写了 `hls: true` 但**没有 `hlsVariant`**，MediaMTX v1.20.1 默认 `hlsVariant: lowLatency`（LL-HLS / fmp4）。后果链：
1. 媒体清单变成 LL-HLS：`#EXT-X-SERVER-CONTROL` / `#EXT-X-PART-INF` / `#EXT-X-MAP` + 大量 `#EXT-X-GAP` 占位段 `gap.mp4`。
2. GAP 占位段被 MediaMTX 以 **`text/html`（200，约 6.8KB 错误页）** 返回，而非视频。
3. hls.js 配置 `lowLatencyMode: true` 期望 LL-HLS parts；拿到 `text/html` 非视频 MIME → 抛**致命错误** → `scheduleHLSRetry` 死循环 → 黑屏。
4. 违背了 §三的设计意图：兜底路反而最脆。

> 服务端代理层本身是通的：`index.m3u8`(200)、媒体清单(200)、`*_init.mp4`(200 `video/mp4` 682B 合法 fmp4) 都正常。故障在**浏览器 hls.js 解析 LL-HLS 的 GAP/HTML 响应**。

### 修复（一行，**已于 2026-09-02 应用并端到端验证**）
在 `manager.go` 的 `GenerateConfig()` 格式化串里，紧跟 `hls: true` / `hlsAddress: %s` 加：
```yaml
hlsVariant: mpegts
```
经典 MPEG-TS HLS：hls.js 默认支持、iOS Safari 原生支持，无 LL-HLS 会话脆弱性，正好契合"手机兜底"定位。
（静态 `mediamtx/mediamtx.yml` 里的 `hlsVariant: lowLatency` 是错的，要么删掉要么改 mpegts，因为它会误导读代码的人。）
> **现状**：源码已改（`internal/mediamtx/manager.go` 第 17 行 `hlsVariant: mpegts`），`go build -trimpath -o video-server.exe ./cmd/video-server` 重建通过。下次起服务直接生效，无需再动。

### 验证方法（沙箱特性见 §六）
同一次 Bash 调用内：启动 video-server + camera-agent → 等 ~14s → 抓 `/hls/camera01/index.m3u8`。
也可不依赖 camera-agent，用 ffmpeg 模拟推流：
```bash
# 模拟一路 H.264 推流（本沙箱实测可用；ffmpeg 在 PATH）
ffmpeg -re -f lavfi -i testsrc=size=640x480:rate=15 -f lavfi -i sine=frequency=440 \
  -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 15 \
  -c:a aac -f rtsp rtsp://127.0.0.1:8554/camera01
```
- **修前**：媒体清单含 `#EXT-X-GAP` / `gap.mp4`，片段经代理返回 `text/html`。
- **修后（2026-09-02 实测）**：
  - 主播放列表 200 `application/vnd.apple.mpegurl` → 指向 `main_stream.m3u8?session=...`
  - 媒体清单 `main_stream.m3u8` **无 `EXT-X-GAP` / 无 `EXT-X-PART-INF`**，标准 `#EXTINF` + `*.ts` 段
  - 首片 `8c2eee06a3d5_main_seg16.ts` 200 **`Content-Type: video/mp2t`** 42488B，魔数 `47 40 00 10`（TS 同步字节 0x47）
  - 结论：hls.js 不再拿到 `text/html` 致命 MIME，HLS 黑屏闭环修复。

```bash
# 快速验证 HLS 链路（需在同一进程内起服务）
python - <<'PY'
import urllib.request, re
b = urllib.request.urlopen("http://localhost:8081/hls/camera01/index.m3u8", timeout=10).read().decode()
m = re.search(r'\n([^\n]+\.m3u8\?[^\n]+)', b)
mb = urllib.request.urlopen("http://localhost:8081/hls/camera01/"+m.group(1).strip()).read().decode()
print("GAP in playlist:", "EXT-X-GAP" in mb, "| PART-INF:", "EXT-X-PART-INF" in mb)
PY
```

### 二次坑（2026-09-02 三轮，真因）：WebRTC↔HLS 切换不净场 → srcObject 压制 HLS 渲染
**即使 §四 已修好 mpegts，点 HLS 仍黑屏** —— 这是"后端修了、网页硬刷新仍黑"的真正原因（与 embed / mpegts 无关）。

**根因**：`web/src/webrtc/player.ts` 里 WebRTC 与 HLS 共用同一个 `<video>` 元素。`connectWebRTC()` 的
`ontrack` 把 WebRTC 的 `MediaStream` 赋给 `video.srcObject`；切到 HLS 时 `teardownPeer()` 只 `pc.close()`，
**没清 `srcObject`**。HTML 语义里 `srcObject` 优先级高于 `src`，于是 `hls.attachMedia(video)` 后用 `src`
驱动的切片**永远渲染不出来** → 元素停在 2×2 占位、`currentTime` 卡 0、黑屏。代理层一切正常（`.ts` 200
`video/mp2t`），所以 curl / HTTP 级验证全绿，但 `<video>` 不出画——**纯前端切换逻辑缺陷，curl 测不出、浏览器必踩**。

**定位方法（关键：必须用真实浏览器，不能只 curl）**：用 Playwright 把"服务 + ffmpeg 推流 + Chromium"放进
**同一个 node 进程**（避免跨工具调用回收 server 父进程），
1. 同源页注入 hls.js 直连 `/hls/...` → 出画（320×240、ct 推进）→ 证明代理/后端正常；
2. 走真实 App 路由 `/` → 点摄像头卡片 → 点 HLS 按钮 → 只拉播放列表、不拉 `.ts`、元素 2×2 → 复现黑屏；
3. 同一流程在 `page.evaluate` 里 `video.srcObject = null` 后再点 HLS → 立刻出画 → **锁定 srcObject 冲突**；
4. 翻转 `enableWorker` 验证不是 Worker 问题（隔离测试 `enableWorker:true` 照样出画）。
> 另外：`about:blank` 注入 hls.js 会触发 Chromium **Private Network Access / 更私有地址空间** CORS 拦截
>（`origin 'null'` 抓 `127.0.0.1`）。必须 `page.goto('http://127.0.0.1:<port>/')` 同源页内测，否则是测试假象。

**修复（已应用并真实浏览器验证）**：切换传输时彻底净场 `<video>`——
- `teardownPeer()`：`if (this.video.srcObject) this.video.srcObject = null`
- `startHLS()` 开头：`this.video.srcObject = null`（hls 接管前防御性清场）
- `teardownHLS()` / `connectWebRTC()` 开头：`removeAttribute('src')` + `load()` + `srcObject = null`
- 重建：`cd web && npm run build` → `go build -trimpath -o video-server.exe`
**验证（真实 App 流程，无手动补丁）**：点 HLS 后 `.ts` 200 `video/mp2t`、元素 320×240、`currentTime`
0.04→14.04s 推进、`paused=false` → 出画。

> **铁律**：任何 WebRTC 与 HLS 共用同一 `<video>` 的播放器，切换传输**必须清 `srcObject` 与 `src`**，否则
> 另一端渲染被压制。这是 `<video>` 的固有语义，不是 hls.js bug。

## 四·A HLS 时延优化（2026-09-02 实测，用户报 5-6s、WebRTC 无感）

**分解**：HLS 端到端 ≈ 发布端滞后（服务端分段/排队，实测中位 ~0.75s，含 ≤1 段边界等待）+ **hls.js
frontier gap** = 播放头刻意落后播放列表边缘的段数 × 段长（hls.js 默认 `liveSyncDurationCount:3`）。

**关键物理约束**：MediaMTX mpegts 只能在**关键帧**处起新段 → 段长被上游 GOP 钳制（GOP 1s⇒1s 段、
GOP 4s⇒4s 段）。**降 HLS 时延第一步是推流端 GOP≈1s**（`-g fps -keyint_min fps`，camera-agent `--auto`
已内置 keyint=fps 修正）。

**实测数据**（`scripts/hls_latency_probe.js A|B|C`：chromium + 同源注入 hls.js，读 level details 的
last fragment edge − currentTime；A=默认 sync3，B=sync2+1.5x，C=sync1+2x）：

| frontier gap | @1s GOP | @4s GOP |
|---|---|---|
| A 默认（sync3） | ~2.4s | **~10.4s** |
| B（sync2） | ~1.4s | – |
| C（sync1+2x，零 stall） | ~0.5-1.0s | ~2.4s |

**落地**（player.ts startHLS，bundle index-Ct3uBpII.js）：`liveSyncDurationCount:1,
liveMaxLatencyDurationCount:6, maxLiveSyncPlaybackRate:2, maxBufferLength:8` + 保留容错重试。
真实 App 验证：出画 + 发布端滞后中位 0.75s（媒体播放列表 `#EXT-X-PROGRAM-DATE-TIME` 对时）；
用户端预期 5-6s → ~2.5-3.5s。iOS 原生播放器无这些旋钮，缓冲更大属预期。

**测量要点**：headless 播放器必须 `muted+playsInline`；逐样本用 `page.evaluate` 快读 + `Promise.race`
超时，勿让单个长 evaluate 悬挂；同源页注入 hls.js（`about:blank` 会被 PNA CORS 拦，见上）。

## 四·B 合成 e2e 回归门（scripts/verify_e2e.py，不依赖真摄像头）

用 ffmpeg 模拟推流 + **同进程内**拉起 video-server（subprocess.Popen，server 全程存活），覆盖
health/database/mediamtx / 自动注册 / RTSP 播放 / 流元数据 / HLS mpegts 回归（§四 LL-HLS 黑屏修复）/
WebRTC 信令可达。用于没有真摄像头 / camera-agent 的沙箱快速回归。

### 铁律：RTSP 推流验证用 127.0.0.1，不要用 localhost
MediaMTX 的 RTSP 监听器是 **IPv4-only**（`0.0.0.0:<port>`；HTTP 服务才是双栈）。本机 `localhost`
优先解析到 `::1`(IPv6)，ffmpeg 连 `::1:<port>` 失败且不回退 127.0.0.1 → mediamtx `paths/list`
始终 0 path → 监控器无 camera 可登记 → `/api/cameras` 返回 `null`（**假阴性**，极易误判成注册 bug）。
**一律 `rtsp://127.0.0.1:<port>/<stream>`**；HTTP 检查用 localhost 没问题（双栈）。

### 门自身已硬化（根因：上面这条 + 残留端口碰撞都会静默出 null）
- 自动选一段连续空闲端口（`_free_block(18080,6)`：http/rtsp/webrtc/hls/api/ice），生成配置全部
  patch 到该段，bind==check 且绝不与残留 mediamtx 孤儿占住的 8554/8888/8889/9997/8189 冲突。
- ffmpeg stderr 落盘（`data/verify_e2e_ffmpeg_<name>_<run_id>.log`）；注册失败时自动 dump ffmpeg
  日志 + 直接查 mediamtx `paths/list` + 打印 ffmpeg 存活态，立刻给出真因。
- 生成配置 `level: debug`，失败时 tail 服务器日志可见监控器 `monitor: <path> -> online` 行。

### 用法与实测
```bash
python scripts/verify_e2e.py --streams 2      # 前台单进程；PASS/FAIL 计数；exit 非 0 = 失败
```
实测：`--streams 1` → **PASS=8 FAIL=0**；`--streams 2` → **PASS=11 FAIL=0**（camera01/02 均 online、
RTSP/元数据/HLS 全过）。WebRTC 502 是合成 SDP 缺 ice-ufrag 的 INFO（非 FAIL）。真机联合验收仍由
`verify_joint.py`（PASS=14）负责——本门是"改了 HLS/注册/监控逻辑后 30 秒冒烟"用的。

## 五、一键构建 / 启动脚本（本项目的交付物）

| 脚本 | 位置 | 作用 |
|------|------|------|
| `build_onelick.bat` | 项目根 | 全量构建：preflight(go/node/vs) → Web UI(npm install 自愈+ vite build) → `go build` video-server → 调 `carmera-agent\build_oneclick.bat` → summary。失败路径 `pause`+打印 `reason`/`hint`，`--no-pause` 供 CI |
| `start_oneclick.bat` | 项目根 | 挑最新 `video-server*.exe` → 解析/校验 config → 清残留 → 分离启动(日志 `logs\video-server.log`) → 轮询 `/api/health` 40s。LAN 地址取自服务端 `GET /api/net/addresses` |
| `scripts/start-joint.bat` | scripts | 联合拉起 video-server + MediaMTX + camera-agent；含"源码比 exe 新则自动 `go build`"防复发 |

> 防复发铁律：**改 Go 源码后必须 `go build`**，否则跑旧 exe（本项目第一轮故障就是 mediamtx 在 PATH 却报"找不到"——因为 exe 是改源码前的旧版）。`start-joint.bat` 已自动比对 mtime 重建。

> ⚠️ **Web UI 是 `go:embed` 进二进制的（不是磁盘直出）**：`web/embed.go` 的 `//go:embed all:dist` + `server.go:50` 的 `fs.Sub(webstatic.Dist,"dist")`。所以**任何前端改动都必须两步走**：先在 `web/` 跑 `npm run build` 再生 `web/dist`，**再** `go build`（把新 dist 嵌进 exe），最后重启服务。只改 Go 也要 `go build`（无需 npm）。**曾经误记为"磁盘直出、改前端不用 npm"，导致改了前端网页纹丝不动——此坑已踩实，以后一律 npm+build+重启**。验证嵌入：`grep <新bundlehash> video-server.exe`（如 `grep Bs_sVbh6`）。

## 六、沙箱诊断方法论（Windows 环境必须知道）

| 铁律 | 说明 |
|------|------|
| **跨调用进程会被回收** | PowerShell/Bash 工具调用里 `start` 拉起的进程，调用结束后几分钟被清掉。**要在调用间隔做 HTTP 验证，必须"同一次 Bash 调用内"起进程 + 发请求** |
| **代理 vs 直连对比** | 怀疑代理 bug 时，同时抓 `http://localhost:8081/hls/...`（代理）和 `http://127.0.0.1:8888/...`（直连），一致=mediamtx 问题，不一致=代理问题 |
| **PowerShell `>` 落盘是 UTF-16** | 读诊断文件用 `python -c "open(f,'rb').read().decode('utf-16')"`；或脚本内用 `cmd /c "... > file 2>&1"` 让 cmd 内部重定向 |
| **npm/reg.exe 被沙箱拦** | `npm install`、`vcvarsall.bat`（vswhere 调 reg.exe）在本沙箱会被黑名单挡；验证 web 构建可手动解包 tarball 到 `node_modules`，或关沙箱跑 |
| **真实退出码** | PowerShell 把原生 stderr 当错误 → 报 exit 1（假象）。判脚本真实退出码要在同调用里读 `$LASTEXITCODE` |

## 七、配套 skill（交叉引用，别重复造轮子）
- `soc-camera-rtsp-agent` — RTSP 推流端（camera-agent）架构与实现
- `soc-windows-gstreamer-build` — GStreamer MSVC 构建踩坑
- `soc-debug-verification` — .bat 写法十二坑、进程/日志诊断、故障定位决策树

## 八、端到端验收清单
1. `build_onelick.bat` 全绿（web dist + video-server.exe + camera-agent.exe 都在）
2. `start_oneclick.bat --no-pause` → `GET /api/health` 返回 `{"status":"ok"}`
3. `GET /api/cameras` 能看到 `camera01` 且 `status: online`
4. 浏览器 WebRTC 出画（桌面 + 手机）
5. **HLS 兜底**：WebRTC 超时或点 HLS 按钮，两端均出画（修 §四 后）
6. `scripts/verify_joint.py --stream camera01` → `PASS>=14 FAIL=0`（WHEP 探测的 502 是合成 SDP 缺 ice-ufrag 的脚本假象，真浏览器 WebRTC 已验证可用，可忽略）
