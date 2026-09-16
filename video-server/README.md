# video-server

基于 **MediaMTX** 的轻量级视频监控/转发服务：一个 Go 二进制同时承载数据库、RTSP/WebRTC 媒体服务、摄像头自动发现与嵌入式 Web UI。

- 后端：Go（`modernc.org/sqlite` 纯 Go SQLite + `gopkg.in/yaml.v3` 配置），单二进制 `video-server(.exe)`
- 前端：Vue 3 + TypeScript + Vite，构建后经 `//go:embed all:dist` 打进同一二进制
- 媒体：MediaMTX 作为子进程拉起，提供 RTSP / WebRTC(HLS) 转发
- 摄像头：monitor 每 3s 轮询 MediaMTX 控制 API，自动注册/上下线

---

## 1. 架构

```
                         ┌──────────────────────────────┐
       浏览器 / 客户端 ──▶│  video-server (Go 单二进制)    │
                         │  ├─ REST API  (/api/*)        │
                         │  ├─ 嵌入式 Web UI (SPA)        │
                         │  ├─ Monitor (3s 轮询发现)      │
                         │  └─ MediaMTX Manager (子进程)  │
                         └───────────┬──────────────────┘
                                     │ 启动 / 控制 / WHEP 代理
                                     ▼
                         ┌──────────────────────────────┐
                         │  MediaMTX 子进程              │
                         │  RTSP :8554 / WebRTC :8889    │
                         │  控制 API :9997 (/v3/paths)   │
                         └───────────┬──────────────────┘
                                     ▲ ffmpeg RTSP publisher
                                     │ (testsrc / 真实摄像头)
           摄像头 Agent / OBS / ffmpeg ─┘

   存储：SQLite (data/video.db)  ── camera 表（按 stream_path upsert）
```

| 模块 | 职责 |
|---|---|
| `cmd/video-server` | 入口：加载配置 → 开库 → 起 MediaMTX → 起 monitor → 起 HTTP 服务 |
| `internal/config` | YAML 配置加载 + 默认值合并，端口单一来源 |
| `internal/mediamtx` | 生成临时 MediaMTX 配置、拉起/停止子进程、健康探测、WHEP 代理 |
| `internal/monitor` | 轮询 MediaMTX 路径，按 `stream_path` 自动注册摄像头、10s 无数据置 offline |
| `internal/camera` | 摄像头 Repository（SQLite CRUD + UpsertByStreamPath） |
| `internal/api` | REST 路由与处理器 |
| `internal/server` | HTTP 服务装配（API 路由 + SPA 静态文件） |
| `internal/database` | SQLite 打开与建表（自动创建 `data/` 目录） |
| `web/` | Vue3 前端源码，`web/dist/` 由构建产出并被 embed |

---

## 2. 目录结构

```
video-server/
├── cmd/video-server/main.go     # 入口
├── internal/                    # Go 后端
│   ├── api/  config/  camera/  database/  logger/
│   ├── mediamtx/  metadata/  monitor/  server/  stream/
├── web/                         # Vue3 前端（需 npm build 产出 dist/）
│   └── dist/                    # 构建产物（被 Go embed）
├── config/config.yaml          # 主配置（端口单一来源）
├── config/config.joint.yaml    # 联合运行配置（HTTP 移到 8081，避开 8080 冲突）
├── scripts/
│   ├── build.bat               # 一键构建（前端 + 后端）
│   ├── test-stream.bat         # 单路测试推流（需本地 ffmpeg）
│   ├── verify_e2e.py           # 端到端验收（ffmpeg 合成流，PASS/FAIL 计数）
│   ├── start-joint.bat         # 联合运行：一键拉起 server + camera-agent
│   ├── stop-joint.bat          # 联合运行：停掉全部进程
│   ├── verify_joint.py         # 联合运行验收（真实推流端，PASS/FAIL 计数）
│   └── verify_metadata.py      # AI metadata 接收验收（含旧版无 type 兼容，PASS/FAIL 计数）
├── data/                       # 运行时态（DB / 临时 MediaMTX 配置 / 日志）—— 不提交
├── go.mod / go.sum
└── README.md
```

---

## 3. 环境要求

| 工具 | 版本/说明 | 用途 |
|---|---|---|
| Go | 1.27+ | 编译后端 |
| Node.js + npm | 任意较新 LTS | 构建前端 `web/dist` |
| MediaMTX | v1.20.1（须本地有二进制） | RTSP/WebRTC 媒体服务子进程 |
| ffmpeg / ffprobe | 含 libx264 | 推流测试与播放验证（验收脚本依赖） |

> MediaMTX 二进制**不会**被项目打包，需在 `config.yaml` 的 `mediamtx.binary` 指明绝对/相对路径。
> Windows 上若填 `xxx/mediamtx`（无 `.exe`），管理器会自动补 `.exe`。
> 本机当前配置：`D:/data/agent-tools/mediamtx_v1.20.1_windows_amd64/mediamtx.exe`。

---

## 4. 开发构建

### 4.1 前端（产出 `web/dist`）
```bash
cd web
npm install --no-audit --no-fund
npm run build          # 产出 web/dist/{index.html,assets/...}
```
> Go 用 `//go:embed all:dist` 读取 `web/dist`，**不构建前端会导致 `go build` 失败**。

### 4.2 后端
```bash
# 直接构建
go build -trimpath -o video-server.exe ./cmd/video-server
```
> ⚠️ 首次构建 `modernc.org/sqlite`（C→Go 转译库）编译较慢，约 **8 分钟**；构建缓存命中后约 **1 分钟**。

### 4.3 一键构建
```bat
scripts\build.bat
```
依次执行：定位 `go`（PATH 或 `.toolchain/go`）→ `npm install` → `npm run build` → `go build -trimpath -o video-server.exe ./cmd/video-server`。

### 4.4 根目录一键脚本（全项目构建 / 单服务启动）

`scripts\build.bat` 只管 video-server 自己；根目录这两个脚本把**整个仓库**串起来：

| 脚本 | 作用 | 覆盖范围 |
|---|---|---|
| `build_onelick.bat` | 一次构建整个项目 | Web UI + video-server + carmera-agent |
| `start_oneclick.bat` | 一键启动 video-server | 只起服务端（它自己拉起 MediaMTX） |

```bat
build_onelick.bat                  # 全部构建
build_onelick.bat --skip-web       # 跳过 Web UI
build_onelick.bat --skip-agent     # 跳过 carmera-agent
build_onelick.bat --clean          # 清空 web\dist 与 carmera-agent\build-msvc 后重建
build_onelick.bat --backend sim    # carmera-agent 后端：gstreamer（默认）| sim | auto
build_onelick.bat --no-pause       # 无人值守（CI），失败也不暂停

start_oneclick.bat                 # 默认 config\config.joint.yaml（8081）
start_oneclick.bat --config config.yaml    # 文件名或路径都接受
start_oneclick.bat --no-browser    # 不开浏览器
start_oneclick.bat --no-kill       # 保留已经在跑的实例
start_oneclick.bat --no-pause      # 无人值守
```

**`build_onelick.bat` 的五步与自检**

| 步骤 | 内容 | 失败时 |
|---|---|---|
| `[1/5]` preflight | 探测 `go` / `node`+`npm` / Visual Studio（vswhere 或 `D:\Software\vs`） | 缺 `go` 或缺 VS 直接 FAIL；缺 `node` 但 `web\dist` 已存在则 WARN 跳过 |
| `[2/5]` Web UI | `npm install`（仅缺 `node_modules` 时）→ `npm run build` → 校验 `web\dist\index.html` | **构建失败会自动 `npm install` 后重试一次** |
| `[3/5]` video-server | `go build -trimpath -o video-server.exe ./cmd/video-server` | 目标被运行实例占用时，自动回退写 `video-server.new.exe` |
| `[4/5]` carmera-agent | 调用 `carmera-agent\build_oneclick.bat --no-pause [clean] <backend>` | 非 0 退出或产物缺失 → FAIL |
| `[5/5]` summary | 打印各产物字节数 / mtime，并给出下一步命令 | — |

> 为什么 `[2/5]` 要"失败后重装依赖再试一次"：`node_modules` 存在**只证明曾经装过**，不等于装全。
> 若 `vite build` 报 `Rollup failed to resolve import "xxx"`，先重装依赖再试。前端实时播放器
> 不依赖 `hls.js`（HLS 仅服务端保留，供后续录播回放）；若只想跳过前端构建：`--skip-web`。

**两个脚本的统一约定**

- 所有失败路径都 `pause` 并打印 `reason` + `hint`，不会静默退出；CI 用 `--no-pause` 关掉。
- 退出码：成功 `0`，任何失败 `1`（`--no-pause` 下同样成立）。
- 脚本内 `.bat` 一律纯英文，避免 GBK 控制台解析问题。

---

## 5. 配置（`config/config.yaml`）

所有端口均在 YAML 配置，**源码中无硬编码端口**。

| 键 | 默认 | 说明 |
|---|---|---|
| `server.host` | `auto` | 对外 URL 使用的主机。`auto`/`0.0.0.0` = 自动取本机局域网 IPv4；也可写死 IP 或域名 |
| `server.bind` | `0.0.0.0` | HTTP 监听地址。`0.0.0.0` = 本机任意 IPv4 的同一端口（局域网可达）；`127.0.0.1` = 仅本机 |
| `server.http_port` | `8080` | REST API 与 Web UI |
| `rtsp.port` | `8554` | MediaMTX RTSP 端口 |
| `webrtc.port` | `8889` | MediaMTX WebRTC 端口 |
| `database.path` | `data/video.db` | SQLite 路径（目录自动创建） |
| `mediamtx.binary` | `./mediamtx/mediamtx` | MediaMTX 可执行文件 |
| `mediamtx.config` | `./data/mediamtx.yml` | 运行时生成的配置落点（源 config/ 不被覆盖） |
| `mediamtx.bind` | `0.0.0.0` | 媒体端口（RTSP/WebRTC/HLS）监听地址 |
| `mediamtx.api_bind` | `127.0.0.1` | MediaMTX **控制 API** 监听地址，仅本机（只有本服务轮询它） |
| `mediamtx.api_port` | `9997` | MediaMTX 控制 API（路径发现用） |
| `mediamtx.hls_port` | `8888` | HLS 端口（可选） |
| `mediamtx.rtp_port` | `8000` | RTP UDP 端口（多实例并存时改它避免冲突） |
| `mediamtx.rtcp_port` | `8001` | RTCP UDP 端口 |
| `log.level` | `info` | `debug/info/warn/error` |
| `log.file` | 空 | 留空则日志打到 stdout |
| `metadata.enabled` | `true` | AI 结果接收开关；`false` → `POST /api/metadata` 返回 503 |
| `metadata.retention_rows` | `2000` | 每路保留的检测明细条数，`0` = 不裁剪 |
| `metadata.max_body_bytes` | `1048576` | 单包上限（字节），超出 → 413 |
| `metadata.require_known_camera` | `false` | `true` = 只接受已登记摄像头；联合运行须为 `false` |

生成给 MediaMTX 的实际配置要点：
```yaml
paths:
  all_others:
    source: publisher     # 任意 RTSP 推流者都能创建路径 → 摄像头自动发现
record: false
```

---

## 6. 运行

```bash
# 默认读取 config/config.yaml
./video-server.exe

# 指定配置
./video-server.exe path/to/config.yaml

# 或用根目录一键脚本（自动清残留 → 启动 → 等 /api/health → 打印本机与局域网地址）
start_oneclick.bat                       # 默认 config\config.joint.yaml（8081）
start_oneclick.bat --config config.yaml  # 换配置
```
启动后服务会：
1. 打开（必要时创建）SQLite 库与 `camera` 表；
2. 生成唯一临时 `data/mediamtx-*.yml` 并拉起 MediaMTX 子进程；
3. 启动 monitor（每 3s 轮询 `GET /v3/paths/list`）；
4. 监听 `http_port` 提供 API 与 Web UI。

Ctrl+C 优雅退出（先停 HTTP，再停 MediaMTX 并清理临时配置）。

### 6.1 局域网访问（任意 IP 同一端口）

默认配置已经是"局域网可达"的：`server.bind: 0.0.0.0` 让**同一个端口同时服务本机所有 IPv4**（以太网/Wi-Fi/虚拟网卡），`server.host: auto` 让对外 URL 里的主机自动解析成局域网 IP，而不是只能本机用的 `localhost`。

启动后会打印全部可访问地址，不需要再去 `ipconfig` 里猜：

```
INFO  http server listening on 0.0.0.0:8081 (all local IPv4+IPv6 addresses)  [socket=[::]:8081]
INFO  ==========================================================================
INFO   video-server ready
INFO  --------------------------------------------------------------------------
INFO   HTTP listen   : 0.0.0.0:8081  (same port on every local IP)
INFO   Web UI (local): http://127.0.0.1:8081/
INFO   Web UI (LAN)  : http://192.168.3.5:8081/   (WLAN)
INFO   Web UI (LAN)  : http://198.18.0.1:8081/   (BeiBei)
INFO  --------------------------------------------------------------------------
INFO   RTSP push/pull: rtsp://192.168.3.5:8554/<stream-path>
INFO   WebRTC play   : open the Web UI above (signaling proxied by the server, port 8889)
INFO   HLS (optional): http://192.168.3.5:8888/<stream-path>/index.m3u8
INFO   MediaMTX API  : http://127.0.0.1:9997  (bind=127.0.0.1, used by the monitor only)
INFO  --------------------------------------------------------------------------
INFO   Media bind    : 0.0.0.0  -> RTSP 8554 / WebRTC 8889 / HLS 8888
INFO  ==========================================================================
```

排序规则：真实私网 LAN 地址（192.168/10/172.16-31）优先，虚拟网卡标注 `[virtual NIC]`，APIPA 标注 `[link-local]`，回环地址排在最后。

**同一份信息也能通过接口拿到**（UI 或脚本用）：

```bash
curl http://<任意本机IP>:8081/api/net/addresses
# {"bind":"0.0.0.0","http_port":8081,"public_host":"192.168.3.5",
#  "rtsp_url":"rtsp://192.168.3.5:8554/<stream-path>",
#  "addresses":[{"ip":"192.168.3.5","interface":"WLAN","private":true,...}],
#  "web_ui_urls":["http://192.168.3.5:8081/","http://127.0.0.1:8081/"]}
```

**命令行覆盖**（临时切换，不改配置）：

```bash
./video-server.exe -config config/config.joint.yaml -bind 0.0.0.0   # 全网卡
./video-server.exe -bind 127.0.0.1                                  # 临时只本机
```

`-bind 127.0.0.1` 时对外 URL 会自动降级成 `127.0.0.1`，不会把别的机器指向一个拨不通的 LAN 地址。

**另一台机器连不上时，按顺序排查：**

| 现象 | 原因 | 处理 |
|---|---|---|
| 浏览器 `ERR_CONNECTION_TIMED_OUT` | Windows 防火墙拦掉入站（最常见） | 管理员运行 `scripts\firewall-add.bat`（或显式给端口：`scripts\firewall-add.bat 8081 8554 8889 8888`） |
| `ERR_CONNECTION_REFUSED` | 服务没起来 / 端口被占 | 看日志里的 `HTTP listen` 行；换 `server.http_port` |
| 打得开页面但播放失败 | 媒体的 UDP 侧被拦 | 放行 `mediamtx.exe` 程序规则（`firewall-add.bat` 已加）；仍不行给服务套 HTTPS（见 6.2） |
| 地址不对（192.168 之外） | 选到了虚拟网卡 | 显式写 `server.host: 192.168.x.x` |
| **网页能开、列表正常，但手机上没有画面** | WebRTC 在手机侧失败（详见 6.2） | 详见 6.2；播放器会**直接显示 WebRTC 失败原因**（不再降级 HLS），多为 http 下 WebRTC 被拒 → 套 HTTPS |

撤销放行规则：`scripts\firewall-remove.bat`。

### 6.2 手机能开页面但播不出画面（WebRTC 在手机上失败）

桌面正常、手机黑屏是 WebRTC 的经典症状，与局域网绑定无关。根因按概率排列：

1. **UDP 不通，而 MediaMTX 默认只开 UDP 做 ICE。**
   MediaMTX 的 `webrtcLocalTCPAddress` 默认为空 —— WebRTC 只能走 UDP。手机走 Wi-Fi 时，
   路由器/AP 常对无线客户端之间的 UDP 做限制，于是一个候选对都建不起来。
   **本项目已默认同时开启 TCP/ICE**（`mediamtx.ice_udp_port` / `ice_tcp_port`，默认均 `8189`），
   启动日志里应看到两行 ICE：
   ```
   [WebRTC] started with listeners on 0.0.0.0:8889 (TCP/HTTP), :8189 (UDP/ICE), :8189 (TCP/ICE)
   ```
2. **移动端 WebKit 在 http（非安全上下文）下拒绝 WebRTC。**
   iOS 上所有浏览器都是 WebKit 内核，所以 iPhone 的 Chrome 与 Safari 表现一致 —— 这正是
   "两个浏览器都不行"的原因。此时 WebRTC 救不回来，正解是给服务套 HTTPS（见下方说明），而不是退回 HLS。
3. **自动播放被拦截**（低电量/省流模式）：播放器会显示 `TAP TO PLAY`，点一下即可。

**实时播放不再使用 HLS 兜底（2026-09-03）**

Web 端实时视频流**只走 WebRTC**：延迟亚秒级，符合实时监控需求。HLS 因分段式固有延迟
（见 6.3，端到端 2-3.5s 起）在实时场景无价值，已从前端播放器移除 —— WebRTC 失败时播放器
**直接显示失败原因**（`WEBRTC` 通道 + 一行错误说明），不再静默降级到 HLS。

> 这意味着：若手机端 WebRTC 因 **http 非安全上下文** 被拒（iOS WebKit 必现），**唯一正解**
> 是给服务套 HTTPS（自签或证书），而不是退回 HLS。HLS 的同源/纯 TCP 优势在实时场景让位于延迟。

**服务端 HLS 保留（用于后续录播回放）**：`/hls/<stream>/index.m3u8` 代理与 MediaMTX HLS 产出
保持不变，未来"缓存录像查看"功能会复用它；前端 `StreamInfo.hls_url` / `hls_direct_url` 字段也
保留，待回放页接入。手工验证服务端仍在产 HLS（不经过浏览器）：

```bash
curl http://<server-ip>:8081/hls/camera01/index.m3u8
```

### 6.3 HLS 时延特性（服务端保留，用于后续录播回放）

HLS 端到端时延 ≈ **发布端滞后** + **hls.js frontier gap**：

1. **发布端滞后（~0.7s）**：服务端把流切成 1 秒的分段再写播放列表，播放器必须等整段生成完才拉，
   天然含 ≤1 段边界等待。这是分段式 HLS 的固有代价，无法消除。
2. **frontier gap（播放器刻意滞后）**：hls.js 默认 `liveSyncDurationCount=3`，播放头被放在播放列表
   边缘之后 3 个分段处，纯属缓冲策略。这一项**之前是时延大头**，现已调低（见下）。

**关键约束 —— 服务端分段时长被上游 GOP 钳制**：MediaMTX 只能在**关键帧（IDR）**处起新段。若推流端
GOP=4s，服务端就出 4 秒的分段，frontier gap 会按 4 秒一级放大。**要让 HLS 时延可控，推流端 GOP 必须
≈1s**（`camera-agent --auto` 已内置 keyint=fps 修正，自动保证 1 秒一个关键帧）。

**前端低延迟参数（已随 web HLS 播放器移除，2026-09-03）**：原 `web/src/webrtc/player.ts` 中的
`liveSyncDurationCount:1` / `liveMaxLatencyDurationCount:6` / `maxLiveSyncPlaybackRate:2` /
`maxBufferLength:8` 等 hls.js 调优参数已不再使用；待录播回放页接入 HLS 时再酌情复用。

实测（`scripts/hls_latency_probe.js A|B|C`，chromium 注入 hls.js 读 level details）：

| frontier gap | @1s GOP | @4s GOP |
|---|---|---|
| 默认（sync3，调优前） | ~2.4s | **~10s** |
| sync2 + 追帧 1.5× | ~1.4s | – |
| **sync1 + 追帧 2×（当前）** | ~0.5-1s | ~2.4s |

真机（合成源 + 真实 App 点 HLS）发布端滞后中位 0.75s。**预期端到端 HLS 时延 ~2-3.5s**（浏览器侧
贡献已从 ~2.4s 降到 ~0.5-1s）。WebRTC 为亚秒级，实时场景走 WebRTC；HLS 不再用于实时播放，仅服务端
保留供后续录播回放。iOS Safari 走原生播放器、无上述旋钮，缓冲更大属预期。

延迟复测：`scripts/hls_latency_probe.js C`（需先起服务并推流，见 §8 的 ffmpeg 命令）。

> 安全提示：MediaMTX 的**控制 API**（默认 `api_bind: 127.0.0.1:9997`）只对内监听 —— 它没有任何鉴权，暴露到局域网等于把每一路流的控制权交出去。媒体端口（RTSP/WebRTC/HLS）同理无鉴权，只在可信网络内开放。

### 6.4 实时播放稳定性加固（2026-09-03）

实时播放链路（WebRTC 播放器 + 服务端信令/HLS 代理）做了两处稳定性加固，均已有单元测试覆盖。

**(a) WebRTC 播放器 `clearWatchdog` 缺失 → 连接 established 时崩溃与重启循环**

`web/src/webrtc/player.ts` 的 `onconnectionstatechange` 在 `connectionState === 'connected'` 时调用
`this.clearWatchdog()`，但类内从未定义该方法，抛 `Uncaught TypeError` → 后续 `emit('connected')` 不执行、
状态机卡在 `connecting`；7s 看门狗定时器未被清除，超时触发 `onWebRTCFailure()` → 重连 → 无限循环（即
"显示过程中会重启"）。已补一个只清 watchdog 的 `clearWatchdog()` 方法，与调用点同名。提交 `05db7de`。

**(b) 重启瞬间一次性 `502`（MediaMTX 端点启动竞态）**

服务端 `Manager.Start()` 只等 MediaMTX **控制 API** 就绪（`waitReady` 12s），但 WebRTC 的 WHEP 端点
（`:8889/<path>/whep`）常在控制 API 就绪后**再晚几百 ms 才监听**。刚重启后浏览器首个 offer 命中未监听端口 →
连接拒绝 → `WebRTCOffer` 回 **502**；客户端 `player.ts` 重连自愈，故"一次 502 后正常"。

修复：`WebRTCOffer`（`internal/mediamtx/manager.go`）与 `hlsProxy`（`internal/api/hls.go`）对 **transport 层
错误（连接拒绝 / 上游未监听）** 做 **4 次 / 250ms 退避重试**，吸收启动竞态；逻辑错误（4xx/5xx）仍立即返回、
不重试。提交 `bd73519` / `e9df885`。回归测试 `manager_test.go` / `hls_test.go`（`api` + `mediamtx` 包，4 用例 PASS）。

> 验证：`go vet ./...` 0 警告；`go test ./...` 全绿；`internal/api` + `internal/mediamtx` 4 用例 PASS。

---

## 7. 联合运行（video-server + carmera-agent）

把 C++ 摄像头 Agent（`../carmera-agent`）与本项目跑成一条完整链路：

```
UVC 摄像头
  └─ camera-agent.exe     GStreamer 采集 → H264 编码 → rtspclientsink
       └─(RTSP push)──▶ MediaMTX :8554        ← 由 video-server 拉起
                          └─ monitor（3s 轮询控制 API）自动注册摄像头
                              └─ REST API / Web UI :8081（WebRTC / HLS 播放）
```

### 7.1 只有一个 MediaMTX —— 联合运行的核心约束

`video-server` 启动时会**自己拉起** MediaMTX 子进程，且**只有这一个实例**开启了控制
API（`:9997`）；monitor 正是靠它每 3s 轮询 `/v3/paths/list` 来发现摄像头。

`carmera-agent` 目录里也有自己的 `mediamtx.yml` 与 `start-camera-agent.bat`，
**联合运行时不要执行它们** —— 那会起第二个 MediaMTX 抢 `:8554`，而且它没有控制 API，
服务器根本看不到那路流。正确做法是直接启动 `camera-agent.exe`，让它推到服务器那个
MediaMTX 上。

### 7.2 一键启动

```bat
scripts\start-joint.bat                  :: 起服务 + 起推流 + 自动开浏览器
scripts\start-joint.bat --no-browser     :: 不开浏览器
scripts\start-joint.bat --camera 0 --stream cam01
scripts\start-joint.bat --ai --metadata   :: 同时开 AI 检测与结果上报
scripts\stop-joint.bat                   :: 停掉全部（含 MediaMTX 子进程）
```

| 环境变量 | 默认 | 说明 |
|---|---|---|
| `VS_CONFIG` | `config.joint.yaml` | 服务器配置文件名 |
| `CAMERA_ID` | 自动（`--list` 枚举后取第一个可用） | 摄像头索引 |
| `STREAM_ID` | `camera01` | RTSP 路径名 |
| `CAMERA_SOURCE` | 自动 | 强制 GStreamer 源（mfvideosrc / dshowvideosrc …） |
| `ENABLE_AI` | `0` | `1` = 给 agent 加 `--ai`（人检测，默认 5 fps） |
| `ENABLE_METADATA` | `0` | `1` = 加 `--metadata`，把 AI 结果 POST 回服务端 |
| `METADATA_URL` | `http://127.0.0.1:<HTTP_PORT>/api/metadata` | 上报端点（仅 `ENABLE_METADATA=1` 时生效） |
| `NO_BROWSER` | `0` | `1` = 不开浏览器 |

AI 与 Metadata 默认关闭：不是每台机器都有 `models/yolov8n.onnx`，默认开着会让
联合启动在缺模型时直接失败。开了之后结果落在服务端 SQLite，用
`curl http://127.0.0.1:8081/api/cameras/<STREAM_ID>/metadata` 查看（详见第 10 节）。

> **必须带 `--auto`**：UVC 摄像头的原生分辨率/帧率各不相同（本机上过 240×240@8fps，
> 也上过 1280×720@30），写死分辨率会导致 caps 协商失败、流水线进不了 PLAYING、
> Agent 陷入重连死循环。两个启动脚本均已默认加上。

**脚本自带的三道自检**（无需手动干预）：

1. **清理残留** —— 先 `taskkill` 掉 video-server / camera-agent / mediamtx。放在最前面，
   否则 Windows 对运行中的 `.exe` 持有独占句柄，后续重建必然 `Access is denied`。
2. **二进制过期自动重建** —— 把 `video-server*.exe` 的 mtime 与 `cmd\`、`internal\` 下
   最新的 `.go` 比较，源码更新就自动 `go build`。这是最有价值的保护：改了 Go 源码却忘了
   重新构建时，所有预检都会通过，但跑起来的仍是旧逻辑（本次故障正是如此——
   `findBinary()` 的 PATH 回退已改，exe 却是旧版，于是 `mediamtx binary not found`）。
   构建失败不致命，会打印 `[WARN]` 并继续使用现有二进制。
3. **LAN 地址从服务端取** —— 查询 `GET /api/net/addresses` 的 `public_host`，
   而不是脚本本地重算。服务端会把物理网卡排在 VMware / WSL / Hyper-V / vEthernet
   等虚拟网卡之前，打印的 IP 与 RTSP、WebRTC URL 里播发的一致。

### 7.3 端到端验收

```bash
python scripts/verify_joint.py
python scripts/verify_joint.py --stream camera01 --keep   # --keep: 结束时不杀进程，便于人工查看
```

与 `verify_e2e.py`（用 ffmpeg 推**合成**流）不同，本脚本驱动的是**真实推流端**
（C++ camera-agent + GStreamer），断言整条链路：

| 检查 | 含义 |
|---|---|
| `camera-agent enumerates at least one camera` | Agent 能枚举摄像头（GStreamer 后端可用） |
| `server /api/health` / `database` / `mediamtx control API` | 服务三态正常 |
| `camera-agent reached STREAMING` | 真实采集链路建立（日志出现 STREAMING） |
| `stream auto-registered` / `reports online` | monitor 从 MediaMTX 自动注册并置在线 |
| `rtsp playback camera01` | ffprobe 在 RTSP 上解出 h264 |
| `negotiated resolution propagated to API` | Agent 协商的分辨率经 MediaMTX track 传到 API |
| `API resolution matches decoded stream` | API 分辨率与 ffprobe 实测一致 |
| `webrtc signaling` | WHEP 端点可达（合成 SDP 返回 400/502 属 INFO，完整协商需真实浏览器） |

当前实测：**PASS=11 FAIL=0 INFO=2**（240×240 @ 8fps）。

> `resolution` 的来源是 MediaMTX 控制 API 的 `tracks2[].codecProps`。RTSP 推流端不会
> 主动上报规格，所以 monitor 在自动注册时会把它回写进 `cameras` 表；未知时保持原值，
> 不会把已有数据清空。

### 7.4 适配不同分辨率 / 帧率的摄像头（720p@30 / 720p@60 / 1080p@30 …）

链路对摄像头规格是**自适应的**，换摄像头无需改代码：

- **`--auto`（默认）是最稳的路径**：Agent 不强制 caps，由摄像头与 GStreamer 协商出
  原生格式。换上 720p@30、720p@60、1080p@30 的摄像头，它直接协商对应规格，
  MediaMTX 把真实宽高回传给服务器（已实测 240×240@8fps 原生摄像头可跑通）。
- **强制指定规格**：带 `--width/--height/--fps` 且**不带** `--auto`，例如
  `camera-agent.exe --width 1280 --height 720 --fps 60 --stream camera01 ...`。
  适用于想锁定某档输出的场景；前提是摄像头支持该规格（否则 caps 协商失败进重连）。

**GOP / 首帧延迟自适应（修复"开流要等约 5s 才出画面"）**

编码器的 GOP 属性单位是**帧**（不是秒）。若按固定 30 帧，在 8fps 摄像头上意味着
约 3.75s 才出一个关键帧，WebRTC 播放端必须等下一个 IDR 才能出首帧 → 体感 ~5s。
Agent 在 `auto_res` 下拿到真实协商帧率后会**一次性重建管线**，把
`keyint` 设为「≈1 秒的帧数」（如 8fps→8、30fps→30、60fps→60），无论源帧率多少，
播放端都在 ~1s 内出首帧。日志可见
`Negotiated Nfps (WxH); rebuilding with keyint=N for <=1s GOP`。

> ⚠️ **不同编码器的 GOP 属性名不同**：`x264enc` / `mfxh264enc` 用 `key-int-max`，
> 但本机默认走 **`nvh264enc`（NVIDIA NVENC）**，其属性是 **`gop-size`**（已用
> `gst-inspect-1.0 nvh264enc` 确认）。`encoder_element()` 必须按后端用对名字，
> 否则 GOP 校正被**静默丢弃**，NVENC 退回默认 GOP（~7-8s）→ WebRTC 首帧 4-5s 且无报错。
> 改 GOP 前先 `gst-inspect-1.0 <enc>` 确认属性名。

**码率自适应**

`auto_res` 下 Agent 按协商出的原生「宽×高×帧率」自动估算码率（约 0.07 bit/像素，
下限 800kbps、上限 12000kbps）：1080p@30 约 6000kbps、720p@30 约 2000kbps、
240×240@8 约 800kbps。显式模式（`--width/--height/--fps` 不带 `--auto`）沿用配置值
（默认 4000kbps）；高分辨率建议显式加 `--bitrate`，例如 1080p@30 给 `--bitrate 6000`。

**帧率 / 码率显示**

MediaMTX 控制 API 只暴露宽高，**不暴露帧率**。因此：

- **fps**：服务器 monitor 另起节流（每 120s 最多一次）的 `ffprobe` 探测 RTSP 流，
  取 `avg_frame_rate` 回填；环境需有 `ffprobe`（已在 PATH）。
- **bitrate**：服务器用 MediaMTX 的 `bytesReceived` 累计值做跨扫描增量算出。
- 两者均为「0 不覆盖」：探测/采样未就绪前保持原值，不会把已有数据清空。
  Web UI 上 fps/bitrate 为空属正常过渡态，几秒后即有值。

### 7.5 端口表（联合运行）

| 端口 | 进程 | 用途 |
|---|---|---|
| `8081` | video-server | REST API + 嵌入式 Web UI |
| `8554` | MediaMTX（video-server 子进程） | RTSP：Agent 推入、播放器拉出 |
| `9997` | MediaMTX | 控制 API，服务器 monitor 轮询 |
| `8889` | MediaMTX | WebRTC（WHEP 信令，由服务器代理） |
| `8888` | MediaMTX | HLS（可选） |

> 默认走 `config/config.joint.yaml` 而**不是** `config.yaml`：本机 `:8080` 会被
> `ApplicationWebServer` 抢占，改用 8081 后演示不会再随机起不来。

---

## 8. 验证（端到端验收）

`scripts/verify_e2e.py` 用 Python 标准库实现（无第三方依赖），方法论对齐 SOC `soc-camera-rtsp-agent`：

- 启动服务（服务自带 MediaMTX 子进程）；
- 用 `ffmpeg` 推 N 路合成 RTSP 流（`camera01..cameraNN`）；
- 断言 `/api/health`、摄像头自动注册、`ffprobe` 播放、`/api/cameras/{id}/stream` 元数据、WebRTC WHEP 端点可达性；
- 打印 **PASS/FAIL 计数**，任一核心检查失败则非零退出。

```bash
# 默认 3 路、指向 ./video-server.exe
python3 scripts/verify_e2e.py

# 自定义
python3 scripts/verify_e2e.py --binary D:/.../video-server.exe --streams 3 --http-port 8080 --rtsp-port 8554
```

### 验收检查项（PASS=11 / FAIL=0 为例）
| 检查 | 含义 |
|---|---|
| `server /api/health reachable` | 服务 HTTP 可达 |
| `database healthy` | SQLite Ping 正常 |
| `mediamtx (media_server) healthy` | MediaMTX 控制 API 就绪 |
| `cameras auto-registered (3 expected)` | monitor 从 publisher 自动建摄像头 |
| `registered cameras report online` | 有在线摄像头 |
| `rtsp playback camera01/02/03` | ffprobe 在 RTSP 上看到 h264 |
| `stream metadata camera01/02/03` | `/api/cameras/{id}/stream` 返回正确 `rtsp_url` |
| `webrtc signaling camera01/02/03` | WHEP 端点可达并代理到 MediaMTX（合成 SDP 返回 502 属 **INFO**，完整协商需真实浏览器） |

> 为避免“复用已存在文件导致只读/访问拒绝”，脚本**每轮**生成独立 config 与独立 DB（`data/video.e2e_<runid>.db`、`data/e2e_<runid>.yaml`、`data/verify_e2e_server_<runid>.log`），运行结束后由调用方清理。

单路手动推流（需本地 ffmpeg）：
```bat
scripts\test-stream.bat camera01
```

---

## 9. REST API 参考

基址 `http://<host>:<http_port>`。

| 方法 & 路径 | 说明 |
|---|---|
| `GET /api/health` | 返回 `{"status","database","media_server"}` |
| `GET /api/net/addresses` | 监听地址 + 全部可访问 IP/URL（`bind`、`public_host`、`web_ui_urls`、`addresses`） |
| `GET /api/cameras` | 摄像头列表（含 `rtsp_url`） |
| `POST /api/cameras` | 创建摄像头（`id` 必填） |
| `GET /api/cameras/{id}` | 单个摄像头详情 |
| `PUT /api/cameras/{id}` | 更新 |
| `DELETE /api/cameras/{id}` | 删除 |
| `GET /api/cameras/{id}/status` | 仅状态 `{"id","status"}` |
| `GET /api/cameras/{id}/stream` | 播放元数据（rtsp_url、分辨率、fps、bitrate、webrtc.signaling、hls_url） |
| `POST /api/cameras/{id}/webrtc` | 转发 SDP offer 到 MediaMTX WHEP，返回 answer |
| `GET /hls/{stream}/index.m3u8` | 同源 HLS 代理（转发到 MediaMTX 的 `hls_port`），手机端兜底播放用 |
| `POST /api/metadata` | 接收 camera-agent 的 AI 结果（`frame` / `status`），成功一律 `204` |
| `GET /api/metadata` | 全局概览：`{"enabled","cameras":[{camera_id, frame, status}]}` |
| `GET /api/cameras/{id}/metadata` | 单路快照：`{"camera_id","frame":{...},"status":{...}}` |

摄像头自动注册由 monitor 驱动：RTSP 推流者连上 MediaMTX 后，`all_others.source: publisher` 会自动创建路径，monitor 检测到 `HasSource` 即按 `stream_path` upsert 为摄像头（`online`/`offline` 由 `Ready` 决定；10s 无数据置 `offline`）。

---

## 10. AI Metadata 接入（camera-agent → video-server）

camera-agent 的 AI 分支把检测结果异步 POST 到服务端，与视频流经不同通道、互不阻塞。本节描述服务端这一侧的落库行为与可调项。

### 10.1 两类消息

一个端点 `POST /api/metadata`，由 `type` 字段区分：

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

**铁律（与 agent 侧一致）**：`frame_id`（摄像头帧计数）与 `timestamp`（采集端 PTS，ms）由**采集侧**产生，服务端**原样存储**，绝不重编号或"修正"。`bbox` 是原始像素 `[x1,y1,x2,y2]`。

### 10.2 三张表

| 表 | 粒度 | 说明 |
|---|---|---|
| `ai_status` | 每路一行 | 最新心跳（`ai_enable`/`ai_running`/`ai_fps`/`model`/`tracker`/`processed`） |
| `ai_frame` | 每路一行 | **最新一帧**的帧号、时间戳、分辨率、目标数 |
| `ai_object` | 每条目标一行 | 检测明细（`class`/`confidence`/`track_id`/`bbox`），带滚动裁剪 |

`ai_frame` 只留最新帧：监控场景关心"现在画面里有什么"，历史帧由上层日志负责。`ai_object` 按 `retention_rows` 滚动裁剪（默认每路 2000 条），不是每写一次就 DELETE —— 而是每 64 次保存触发一次批量裁剪，避免高频写入被 DELETE 拖慢。

### 10.3 配置

```yaml
metadata:
  enabled: true              # false → 端点返回 503 + Retry-After: 30，agent 进入退避
  retention_rows: 2000       # 每路保留的检测明细条数，0 = 不裁剪
  max_body_bytes: 1048576    # 单包上限，超出 → 413（decode 之前就拒，不缓冲）
  require_known_camera: false # true = 只接受已在 cameras 表登记的路
```

`require_known_camera` 联合运行时**必须为 `false`**：agent 一流起来就开始推 metadata，通常早于 monitor 下一次 3s 轮询把摄像头登记进 `cameras` 表。置 `true` 会让 agent 白退避一轮。

### 10.4 服务端硬化（不信任上游）

agent 已经 clamped，服务端仍然再夹一次：

- `bbox` 先修正 `lo > hi`（反向框），再夹进 `[0, width] × [0, height]`，并保证 `x2 >= x1+1`（退化框会被撑开成 1×1，而不是丢弃）
- `confidence` 夹到 `[0,1]`
- `frame` 缺 `camera_id` 或宽高非正 → `400`
- 坏消息**绝不覆盖好数据**（验收脚本用 `frame_id` 前后比对锁定）
- 视频流路径完全不受影响：metadata 写失败不会反映到 `/api/health`

### 10.5 旧版 agent 兼容（无 `type` 字段）

2026-09-02 联合验证时发现：agent 的 `encode_frame_metadata()` 漏发 `type`，零目标的帧只带空的 `objects: []`，服务端无法与畸形心跳区分，全部被 400 拒收（现场症状 `sent=0 failed=5 dropped=90`）。

双向修复：

1. **agent 侧**：`metadata_encoder.cpp` 补上 `"type":"frame"`（单测 `metadata_encode_frame` / `metadata_encode_empty_frame` 各加一条断言锁死）
2. **服务端**：`metadata.InferType()` 按载荷形状兜底 —— 有 `ai` 对象 → `status`；有 `objects` 或 `frame_id` → `frame`；都不匹配 → `400`（不再往下猜）。**显式 `type` 永远优先于推断**

这样已部署的旧 agent 无需升级即可继续上报。

### 10.6 验收

```bash
# 独立模式：自建服务、自开端口、8s 压测
python scripts/verify_metadata.py

# 指向已在跑的服务
python scripts/verify_metadata.py --base-url http://127.0.0.1:8081
```

覆盖 11 组共 35 项：ingest 基础 / 往返保真 / 心跳 / bbox 硬化（越界、退化、反向）/ 空结果 / 畸形输入（400、413、不覆盖好数据）/ **旧版无 `type` 兼容** / camera_id↔stream_path 双向解析 / 全局概览 / 5 msg/s 持续压测 / 媒体路径不受影响。

当前结果：**PASS=35 FAIL=0**。

端到端方案（camera-agent → video-server 完整链路）与 spec §23 交付物映射见 [`doc/metadata_e2e.md`](doc/metadata_e2e.md)。

### 10.7 查询示例

```bash
# 单路快照
curl -s http://127.0.0.1:8081/api/cameras/camera01/metadata | python -m json.tool

# 全局概览
curl -s http://127.0.0.1:8081/api/metadata | python -m json.tool
```

未知摄像头返回**空快照**而非 404（`frame`/`status` 为 `null`），这样前端轮询不必区分"没数据"和"没这路"。

---

## 11. 调试与排错

| 现象 | 原因 / 修复 |
|---|---|
| `go build` 报找不到 `web/dist` | 未构建前端 → `cd web && npm run build` |
| 启动报 `mediamtx binary not found` | `mediamtx.binary` 路径不对；确认文件存在（Windows 可省略 `.exe`，管理器自动补） |
| `/api/health` 中 `media_server: error` | MediaMTX 没起来。查 `:9997` 端口冲突或二进制路径；看服务日志里 `[mediamtx]` 行 |
| `attempt to write a readonly database (8)` / `SQLITE_READONLY` | 旧的 `data/video.db` 不可写。**删除 `data/video.db` 重启**即可；验收脚本已用每轮新 DB 规避 |
| 服务无法写配置/日志（`Access is denied`） | 运行时态落在受保护目录。确保 `data/` 可写；服务器写的是唯一临时 `data/mediamtx-*.yml`，不覆盖源码 |
| 摄像头不自动出现 | 推流名需匹配；ffmpeg 建议 `-rtsp_transport tcp`；monitor 每 3s 扫描、10s 无数据转 offline，等几秒再看 `/api/cameras` |
| WebRTC 返回 502 / 400 | 合成 SDP offer 不被 MediaMTX 接受属正常（INFO）。完整协商请用真实浏览器客户端走 `/api/cameras/{id}/webrtc` |
| 端口被占用（如 `:8080` / `:9997`） | 本机 `:8080` 可能已被 `ApplicationWebServer`、`:9997` 被 `vivoSyncService` 等占用。在 `config.yaml` 改端口即可 |
| **联合运行**：摄像头一直不出现 | 多半起了**两个** MediaMTX。只保留 `video-server` 拉起的那个（带 `:9997` 控制 API）；不要执行 `carmera-agent\start-camera-agent.bat`，它起的实例没有 API，服务器看不见 |
| **联合运行**：Agent 反复重连、进不了 PLAYING | 未加 `--auto`。强制 1280×720@30 而摄像头只有 240×240@8fps → caps 协商失败。见 `soc-camera-rtsp-agent` 第五节 |
| **联合运行**：`resolution` 为空 | MediaMTX 还没解析出 track 属性，等一轮 monitor（3s）；持续为空则查 `/v3/paths/list` 的 `tracks2` |
| **联合运行**：`.bat` 起的进程随终端一起退出 | 脚本用 `start` 开独立窗口；在某些非交互 shell（如被回收的自动化会话）里子窗口会被连带清理。双击运行或在普通终端里执行 |
| 首次构建极慢 | `modernc.org/sqlite` 转译库首次编译约 8 分钟，属正常，缓存命中后约 1 分钟 |
| 构建/拉取模块失败（代理/网络） | 设 `GOPROXY=https://goproxy.cn,direct`；CI 可用 `go build -mod=readonly` 防止改写 `go.mod/go.sum` |
| **Metadata**：`POST /api/metadata` 返回 503 | `metadata.enabled: false`，或服务端未初始化 repository。检查配置段是否存在；agent 侧会按 `Retry-After: 30` 退避 |
| **Metadata**：agent 报 `server replied HTTP 400` | 消息缺 `type` 且形状不可识别，或 `frame` 缺 `camera_id`/宽高。用 `--metadata-log-payload` 抓实际载荷比对；旧版无 `type` 的帧服务端已能兜底（见 10.5） |
| **Metadata**：`GET /api/cameras/{id}/metadata` 返回空 | 落库用的 camera_id 与查询用的不一致。`resolveCamera` 会先把 id 规范化（`Get` → `GetByStreamPath`）再回写，但未知 id 在 permissive 模式下原样存储。查 `GET /api/metadata` 看实际存的 `camera_id` |
| **Metadata**：单包返回 413 | 超出 `metadata.max_body_bytes`（默认 1 MiB）。调大该值，或让 agent 降低单帧目标数 |
| **Metadata**：`media_server: error` 且没有任何摄像头 | MediaMTX 子进程因 UDP 端口冲突退出（默认 RTP/RTCP 8000/8001 是全局共享的）。给 `mediamtx.rtp_port` / `rtcp_port` 显式配值，并 `taskkill` 残留 mediamtx |
| 浏览器控制台 `Uncaught TypeError: this.clearWatchdog is not a function` | `player.ts` 缺方法 → 见 6.4(a)，已修复 `05db7de`；**重启 video-server 加载新二进制**后消失 |
| 重启服务后首帧前偶发一次 `502 (Bad Gateway)` | MediaMTX WHEP 端点启动竞态 → 见 6.4(b)，已加 4 次/250ms 退避重试；若**频繁**出现说明上游持续不可达（查 MediaMTX 是否真起来、`/api/health` 的 `media_server` 字段） |

---

## 12. 典型工作流（开发 → 验收）

```bash
# 1) 改代码后重新构建
scripts\build.bat

# 2) 启动服务（另一个终端）
./video-server.exe

# 3) 推测试流（可多开几个终端/参数）
scripts\test-stream.bat camera01
scripts\test-stream.bat camera02

# 4) 浏览器看列表/播放
#    http://localhost:8080/

# 5) 无人值守端到端验收
python3 scripts/verify_e2e.py --binary video-server.exe
python3 scripts/verify_metadata.py
#    观察结尾  == RESULT: PASS=N FAIL=0 ==

# 6) 单元测试
go test ./...
```

---

## 13. 提交注意（`.gitignore` 建议）

`data/` 为运行时态，建议忽略：
```
data/
video-server
video-server.exe
web/dist/
```
`config/config.yaml` 可提交为模板，本地 `mediamtx.binary` 绝对路径差异用本地覆盖或环境变量处理。
