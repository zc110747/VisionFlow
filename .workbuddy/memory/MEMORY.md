# VisionFlow 记忆索引

> **只放骨架 + 指针**；明细在 **`playbook.md`**（按 §号 Read 那一节）。
> 时间线原始记录在 `YYYY-MM-DD.md`，按需读最近的。
> ⚠️ 本文件超 **~4.1k 字符**会被静默截断（改完必查 `len()`，别拿 `wc -c` 字节数比）。

## 项目是什么

`VisionFlow` = **video-server**（Go 单二进制：MediaMTX 托管 + REST API + 嵌入式 Vue3
Web UI + SQLite + AI metadata 落库 + AI 模式下发） **+ carmera-agent**（C++/MSVC +
GStreamer：UVC 采集 → H264 → RTSP 推流 + 端侧 YOLO11 检测/姿态 + ByteTrack + 结果上报）。
链路：`摄像头 → agent → MediaMTX → video-server → 浏览器(WebRTC/HLS)`。

## 铁律

1. **联合运行一律用 `config/config.joint.yaml`（:8081）** —— 本机 `8080` 被 `ApplicationWebServer`
   占用、`9997` 被 `vivoSyncService` 占用，默认配置会 bind 失败。→ §1
2. **配置路径必须带 `config/` 前缀** —— `Load()` 找不到文件会**静默回退默认 8080**，不报错。→ §2
3. **联合运行只有一个 MediaMTX** —— 不要执行 `carmera-agent\start-camera-agent.bat`。→ §2
4. **agent 后端看 `--version`/CMakeCache，不看目录名**；gstreamer 构建产物在
   **`build-msvc-gst`**（`build_oneclick.bat` 才用 `build-msvc`）。→ §3 §6
5. **判据看 MediaMTX `paths/list` 的 `online=true`**，不要看 `/api/cameras`（DB 会复用）。→ §3
6. **仓库根 `start.bat` 是联合运行入口**；文件必须 **CRLF**（LF 会 `call :label` 假 FAIL）。
   验证全链路可 `--source videotestsrc`；`--source` **只吃单个元素名**（不能传整条 pipeline）。
   真机 UVC 走 **`mfvideosrc`**（非 dshow）+ GStreamer 后端。→ §6 §8.14
7. **手势控臂验收走 `verify_gesture_arm.py`**（四级阶梯 + PASS/FAIL）。Level 1 要 :9100 有
   `armpilot-backend.exe`，没有就 `--no-live`。**依赖单向**：`RobotLink` 消费 `HandState`。→ §8
8. **不要用 `-Clean`** —— 宿主安全包装会拦 `Remove-Item -Recurse` 并把 `build-msvc` 删空。
9. **`lost_frames` 对 AI 推理率（~8Hz）算，不是相机 30fps** —— 实机抖动 1–2 帧，
   设小会误判"手离开"并强拉 HOME。当前默认 **6**。→ §8.8
10. **构建偶发 `C1083 Permission denied`（obj 不存在）= 杀软瞬时锁，原样重试即过** ——
   与 `LNK1104`（exe 被占用）区分；`go:embed` 瞬时失败同族。→ §8.7
11. 🔴 **`caps.workspace` 是几何包围盒，不是可达域 —— 虚大 1.6–3.5×**。**凡"手位移 → 臂位移"
   的换算与"距边界多远"的判定，基准一律用实测 `reach_span_x/y/z`（39/176/41.5）**；
   换臂/改结构后必跑 `measure_reach.py`。→ §8.12
12. **链路绿 ≠ 臂动了** —— 真机无位置反馈。回归断言必须落在**真实位移**上
   （`ASSERT(travelled > 10.0f)`），不能只看 sent/rejected 计数。→ §8.12
13. 🔴 **「日志里连 `[ROBOT]` banner 都没有」= 分支从未启动**；**有 banner 没后续** = 启了
   连不上；**有 `cmd ok` 但臂不动** = §8.12 尺子问题。→ §8.13
14. **`gain_x/y/z` 是手感参数，行为测试须 pin 成 1.0**，另用 feel pin 测试钉 shipped 值
   （默认 **0.25/0.5/0.25**，三轴不同值因 span 差 4.5×）。→ §8.14
15. **起 `armpilot-backend.exe` 不能加 `-WindowStyle Hidden`**（永不监听 9100）；跑测试要把
   "起→测→关"写进**同一脚本**用 Bash `run_in_background`（PowerShell 工具会杀进程树）。→ §8.14
16. 🔴 **`HandState::delta` 是累积量（`position − reference`）—— 转帧间增量必须显式差分**
   `inc = delta_now − delta_prev`。三条不变式：**第一帧只锁基准不发**、**限流帧仍推进基准**、
   **基准无效不推进**。症状「小步被死区吞 + 偶尔突然运动」≈ 必然是这条。→ §8.15
17. 🔴 **离散手势（握拳/张开）必须冻结连续控制通道**（`gesture_mute_ms` 默认 500ms），
   冻结期**丢弃**位移而非缓冲（缓冲 = 窗口结束一次性大跳）。**手势姿态本身不是静止的**。→ §8.15
18. 🔴 **源文件"凭空消失"/换机 clone 后编译全挂，先查 `.gitignore` 通配** —— `*.mod` 会连带
   吞掉 `go.mod`，而 `git add` 对被忽略文件**静默跳过**（`go.mod` 曾因此从未被提交，
   新 clone 里 Go 全挂）。探针 `git check-ignore -v <path>`；本仓已加 `!go.mod` 例外。→ §9

## 常用命令

```bash
video-server\build_onelick.bat          # 全仓库构建；scripts\build.bat 只 server
carmera-agent\scripts\build-msvc.ps1 -Backend gstreamer   # 只 agent（-> build-msvc-gst）
start.bat [--source videotestsrc --no-browser --no-pause --probe]   # 联合运行 / 无摄像头自检
video-server\scripts\stop-joint.bat
python video-server\scripts\verify_joint.py / verify_e2e.py / verify_metadata.py
python carmera-agent\scripts\verify_gesture_arm.py [--no-live] / measure_reach.py
cd video-server && go test ./...                    # 91 断言
cd carmera-agent && ctest --test-dir build-msvc-gst --output-on-failure   # 88 用例
```

> `*.bat` 入口在 Agent 沙箱里跑不了（cmd.exe 被拦）；等价命令手抄或用 `.ps1`。

## 按需查阅（读 playbook.md）

| 需要什么 | 去哪 |
|---|---|
| 端口表 / 工具路径 / 真机摄像头规格 | §1 |
| 配置陷阱 / 验收脚本实测坑 / 「视频正常但 AI 不显示」排查顺序 / 旧二进制判定 / 9-4 merge 残留 | §2–§5 |
| **根 `start.bat` + 多摄像头结论 + AI 能力选择缺口（方案 A/B/C）** | §6 |
| **手部 3D 识别分支**（模型/anchor/两个 z 的语义/黄金基准/实测成绩/构建 env） | §7 |
| **手势控臂 Phase 2**（robot 模块/七条协议坑/四级验收/合成与真帧验证局限） | §8 |
| 包围盒≠可达域 / 「无命令下发」静默故障 / 降增益 / delta 差分 + 静默窗口 | §8.12–§8.15 |
| 源文件"消失"、`.gitignore` 静默吞 `go.mod`、换机 clone 编不过 | §9 |

## 交付约定

- 每完成一个模块**立即增量汇报**，报**原始日志**不报"成功了"。
- 构建结果用数字说话（RAM/FLASH 占比、警告数）；关键改动落 README。
- `git push` 由用户自己处理，我只做本地 commit / diff。
- 不记录 token / 凭据 / 密钥。
