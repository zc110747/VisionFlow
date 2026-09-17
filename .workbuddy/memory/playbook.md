# VisionFlow Playbook（项目明细库）

> 本仓库 `.workbuddy/memory/` 的**伴生明细文件**，与 `MEMORY.md`（索引）配对。
> 日戳文件 `YYYY-MM-DD.md` 是原始时间线；**这里只沉淀跨会话仍有用的结论**。
> 读取方式：索引里 `→ §N` 就 Read 对应章节，不必整份读。

---

## §1 端口与本机环境

- ⚠️ **本机默认端口被无关程序占用**：`8080` = `ApplicationWebServer`、`9997` = `vivoSyncService`
  ⇒ 用 `config/config.yaml` 的默认值起服务会 **bind 失败**。
  **联合运行一律走 `config/config.joint.yaml`（`http_port: 8081`）**，这是当初改配置的唯一原因。
- 联合运行端口表：

| 端口 | 进程 | 用途 |
|---|---|---|
| `8081` | video-server | REST API + 嵌入式 Web UI |
| `8554` | MediaMTX（video-server 子进程） | RTSP 推入 / 拉出 |
| `9997` | MediaMTX | 控制 API（monitor 每 3s 轮询，仅本机） |
| `8889` | MediaMTX | WebRTC（WHEP 由服务端代理） |
| `8888` | MediaMTX | HLS（服务端在 `/hls/*` 同源代理） |

- 工具路径（脚本内自动探测，**不要硬编码**）：
  `mediamtx` = `D:\data\agent-tools\mediamtx_v1.20.1_windows_amd64`；
  `ffmpeg/ffprobe` = `D:\data\agent-tools\ffmpeg-master-latest-win64-gpl-shared\bin`。
- 真机摄像头：`LRCP USB2.0`（1280×720@30）、内置 `Integrated Camera`（1280×720@10）；
  枚举用 `camera-agent --list`。

---

## §2 配置陷阱

- ⚠️ **配置路径打错会静默回退默认**：`Load()` 找不到文件时不报错、直接回落到默认 `8080`
  ⇒ 传 `config.joint.yaml`（漏了 `config/`）会**无声吞掉**，现象是"说好 8081 却在 8080"。
  **正确写法 `config/config.joint.yaml`。**（建议后续改成 fatal 退出或至少 warn。）
- `metadata.require_known_camera` 联合运行**必须 `false`** —— metadata 常早于 monitor 下一次
  3s 轮询把摄像头登记进 `cameras` 表，置 `true` 会让 agent 白退避一轮。
- ⚠️ **联合运行只有一个 MediaMTX**：`video-server` 自己拉起且只有它开了控制 API。
  不要执行 `carmera-agent\start-camera-agent.bat`（起第二个实例抢 `:8554`，且没有控制 API，
  服务器看不到那路流）。
- `start-demo.bat`（早期放在仓库根）**已被 `video-server\scripts\start-joint.bat` 取代**，
  根目录现已无该文件；旧笔记里提到它不要照做。

---

## §3 验收脚本与实测坑

- ⚠️ **`verify_joint.py` 默认用的 agent 是 `build-msvc`（SIM 后端）**：
  `rtsp_publisher_sim.cpp` 的 `connect()` 是 mock（源码注释明说不真正建连接），只打日志置
  `STREAMING` → MediaMTX 永远收不到 publish → monitor 不注册 →
  **`auto-registered FAIL` 是假故障**。真机联合验收必须显式指定
  `--agent ...build-msvc-gst/src/camera-agent.exe`。
- **联合脚本的 agent 选择顺序**：优先 `build-msvc-gst/`，缺失才回退 `build-msvc/`
  ⇒ **两个目录都必须是当前源码构建的**，否则跑的是旧逻辑。
- e2e 的 ffmpeg **必须带 `-rtsp_transport tcp`**（UDP 推流被 MediaMTX 拒，publish 建不起来，
  且 ffmpeg stderr 被 DEVNULL 吞掉，看不到原因）。
- **别直接复用 `config.joint.yaml` 做探测**：会复用上一轮 joint run 的 DB（`camera_id` 残留），
  `/api/cameras` 里有 `camera01` **不等于**本轮注册成功 ⇒ 判据要看
  **MediaMTX `paths/list` 的 `online=true`**。
- 沙箱下 **managed python 的孙进程 ffmpeg 会挂起**（零输出、alive 但不连 RTSP；同方式曾成功过
  ⇒ 偶发）。绕法 = 在**同一次 Bash 调用内**用 `( exe ... & )` 直接后台起 ffmpeg，再 curl 探测。
- 起服务用 `run_in_background`（工具托管才会持续存活）；`cmd //c start-demo.bat` 会在调用返回时
  **回收 `start ""` 的子进程**。
- 验收脚本产出**统一 `PASS/FAIL` 计数**，结尾形如 `== RESULT: PASS=N FAIL=0 ==`。

---

## §4 诊断经验（高价值）

- **"视频正常但 AI 不显示" 的排查顺序**（2026-09-07 实测，一次差点误判为端口问题）：
  1. 先看 **agent 发出的 metadata payload 是否带 `ai_mode`** ——
     用 `--metadata-log-payload` 抓 wire JSON，或 `GET /api/cameras/<id>/metadata` 看 `frame`。
  2. 再看 **UI 门控**：`web/src/components/VideoPlayer.vue` 的
     `if (!frameMode || frameMode !== aiMode.value) clearOverlay()`
     —— `frame.ai_mode` 缺失时 `frameMode` 为 `undefined` ⇒ **每帧都被判不匹配 ⇒ overlay 永远清空**。
  3. 最后才怀疑端口 / 服务端。
- **判定"代码对但二进制旧"**：比对**二进制 mtime 与源码 mtime**（本次 exe 早约 22 小时），
  再用 `grep -c ai_mode` 查 agent 日志 / 实际 payload。比读代码快得多。
- 端口错配的真实后果（历史）：agent 旧默认 `metadata.server_url=:8000`（与 MediaMTX RTP 端口
  撞车）、`aimode_base_url=:8081` ⇒ AI 帧 POST 进虚空 / aimode 轮询打错端口，而视频不受影响。
  现已在 `stream_controller.cpp` 里用 `derive_aimode_base()` 从 `metadata.server_url` 派生基址。

---

## §5 历史事件（避免重踩）

- **2026-09-04 merge 残留**：merge commit `2966111` 在 3 个文件里留下未解决的冲突标记就提交
  （`89aab99`），导致 `go build` 失败；另有 2 个文件自动合并"成功"但语义错误（同名测试函数、
  同名 `const` / `drawSkeleton` 重复声明）。
  修复基线取 **`d73e5b1`（本地、更新、已验证）**，用 `git checkout d73e5b1 -- <file>` 恢复。
  **教训：自动合并成功 ≠ 语义正确**，必须靠 build + vet + test 兜底；
  判断"哪侧是已验证基线"看提交时间 + 主题对应，比逐块手工取舍可靠。
- 修复后回归：`go build/vet/test` 全绿、`gofmt -l` 空、`npm run build` ✓、
  全目录 grep 无 `<<<<<<<` 残留。
