# VisionFlow 记忆索引

> **只放骨架 + 指针**；明细在 **`playbook.md`**（按 §号 Read 那一节）。时间线在 `YYYY-MM-DD.md`。
> ⚠️ 超 **~4.1k 字符**会被静默截断（改完必查 `len()`，别拿字节数比）。

## 项目是什么

`VisionFlow` = **video-server**（Go 单二进制：MediaMTX 托管 + REST API + 嵌入式 Vue3 Web UI +
SQLite + AI metadata 落库 + AI 模式下发） **+ carmera-agent**（C++/MSVC + GStreamer：UVC 采集
→ H264 → RTSP 推流 + 端侧 YOLO11 检测/姿态 + ByteTrack + 上报）；链路
`摄像头 → agent → MediaMTX → video-server → 浏览器(WebRTC/HLS)`。

## 铁律

1. **联合运行一律用 `config/config.joint.yaml`（:8081）** —— 本机 `8080`/`9997` 分别被
   `ApplicationWebServer`/`vivoSyncService` 占用，默认配置 bind 失败。→ §1
2. **配置路径必须带 `config/` 前缀** —— `Load()` 找不到文件会**静默回退默认 8080**。→ §2
3. **联合运行只有一个 MediaMTX** —— 别执行 `carmera-agent\start-camera-agent.bat`。→ §2
4. **agent 后端看 `--version`/CMakeCache，不看目录名**；gstreamer 产物在
   **`build-msvc-gst`**（`build_oneclick.bat` 用 `build-msvc`）。→ §3 §6
5. **判据看 MediaMTX `paths/list` 的 `online=true`**，别看 `/api/cameras`（DB 会复用）。→ §3
6. **仓库根 `start.bat` 是联合运行入口**；文件必须 **CRLF**（LF 假 FAIL）。
   `--source` **只吃单个元素名**；真机 UVC 走 **`mfvideosrc`** + GStreamer 后端。→ §6 §8.14
7. **手势控臂验收走 `verify_gesture_arm.py`**（四级阶梯/PASS-FAIL）。Level 1 要 :9100 有
   `armpilot-backend.exe`，没有就 `--no-live`。→ §8
8. **不要用 `-Clean`**（宿主安全包装会删空 `build-msvc`）；构建偶发 `C1083`/`go:embed` 瞬时
   失败 = 杀软锁，原样重试即过；`LNK1104` = exe 被占用。→ §8.7
9. 🔴 **`hand_lost_ms`(默认 2000ms) 是"连续无检出"时间窗，不是帧数** —— 手部检出**成簇**
   （簇 2–10 帧，簇间静默 0.9–19s）。旧 `lost_frames=6`(0.75s) 落在簇间隙内 ⇒ 每簇边界都判
   "离开"强拉 HOME。→ §8.17
10. 🔴 **`caps.workspace` 是包围盒不是可达域（虚大 1.6–3.5×）** —— 凡"手位移→臂位移"换算与
   边界判定，基准一律用实测 `reach_span_x/y/z`（39/176/41.5），换臂后重跑 `measure_reach.py`。→ §8.12
11. **链路绿 ≠ 臂动了** —— 真机无位置反馈。断言必须落在**真实位移**上（`travelled > 10.0f`），
   别只数 sent/rejected。→ §8.12
12. 🔴 **无 `[ROBOT]` banner = 分支**未启动；有 banner 无后续 = 连不上；有 `cmd ok` 臂不动
   = §8.12 尺子 / §8.17 自锁。→ §8.13
13. **`gain_x/y/z` 是手感参数，行为测试须 pin 成 1.0**，另用 feel pin 测试钉 shipped 值
   （现为 **0.4/0.5/0.4**，三轴不同值因 span 差 4.5×）。→ §8.14
14. **起 `armpilot-backend.exe` 不能加 `-WindowStyle Hidden`**（永不监听 9100）；跑测试要把
   "起→测→关"写进**同一脚本**用 Bash `run_in_background`（PowerShell 工具会杀进程树）。→ §8.14
15. 🔴 **`HandState::delta` 是累积量（`position − reference`）—— 转帧间增量必须显式差分**
   `inc = delta_now − delta_prev`。不变式：第一帧只锁基准不发 / 限流帧仍推进基准 / 基准无效
   不推进。「小步被吞 + 偶尔猛跳」≈ 必然是这条。→ §8.15
16. 🔴 **离散手势必须冻结连续控制通道**（`gesture_mute_ms` 默认 500ms），冻结期**丢弃**位移
   而非缓冲（缓冲 = 结束时一次性大跳）。**手势姿态不是静止的**。→ §8.15
17. 🔴 **源文件"消失"/换机编不过，先查 `.gitignore` 通配** —— `*.mod` 吞掉 `go.mod`，
   `git add` 对忽略文件**静默跳过**。探针 `git check-ignore -v`；已加 `!go.mod`。→ §9
18. 🔴 **臂"被饿死"先查三轴权限配比，别先怀疑增益** —— 旧 `gain 0.25/0.5/0.25` 方向反了；
   正解 **`gain = 0.5·F/s`**（span 抵消）。→ §8.16
19. ✅ **软边界 HOME 自锁已修复**（2026-09-29 真机验证）：旧 `learn_from_rejection` 把边界钉死在
   当时位姿⇒放松需"先越过边界"⇒永久锁死（523 taper 帧里 197 帧静默、全 HOME）。**修法**：
   拒绝时写 `bound = from + step`（留一步余量，而非钉死当前位）；`relax_learned_bound` 每 8 次
   接受朝实测带漂移 `kRelaxToPriorFraction=0.10`（带半衰期）；逐轴独立置零而非整帧丢弃。
   **验证**：联合运行 35min，834 条 movexyz / Σ|Δ|=3143mm / **0 次拒绝**；taper 仅到可达带边缘
   时落 0.10，臂可原路返回，无自锁。→ §8.17

## 常用命令

```bash
video-server\build_onelick.bat          # 全仓库构建；scripts\build.bat 只 server
carmera-agent\scripts\build-msvc.ps1 -Backend gstreamer   # 只 agent（-> build-msvc-gst）
start.bat [--source videotestsrc --no-browser --no-pause --probe]   # 联合运行 / 无摄像头自检
python carmera-agent\scripts\verify_gesture_arm.py [--no-live] / measure_reach.py
python carmera-agent\scripts\diagnose_hand_arm_transfer.py / diagnose_agent_log.py <agent.log>
python video-server\scripts\verify_joint.py / verify_e2e.py / verify_metadata.py
cd video-server && go test ./...                    # 91 断言
cd carmera-agent && ctest --test-dir build-msvc-gst --output-on-failure   # 90 用例
```

> `.bat` 入口在沙箱里跑不了（cmd.exe 被拦）；等价命令手抄或用 `.ps1`。

## 按需查阅（读 playbook.md）

| 需要什么 | 去哪 |
|---|---|
| 端口表 / 工具路径 / 真机摄像头规格 | §1 |
| 配置陷阱 / 验收脚本实测坑 / 视频正常但 AI 不显示 / 旧二进制判定 / 9-4 merge 残留 | §2–§5 |
| **根 `start.bat` + 多摄像头结论 + AI 能力选择缺口（方案 A/B/C）** | §6 |
| **手部 3D 识别分支**（模型/anchor/两个 z 的语义/黄金基准/实测成绩/构建 env） | §7 |
| **手势控臂 Phase 2**（robot 模块/七条协议坑/四级验收/合成与真帧验证局限） | §8 |
| §8 细分：包围盒≠可达域 / 无命令 / delta 差分+静默窗 / 三轴配比 / **"受限"三源诊断** | §8.12–§8.17 |
| 源文件"消失"、`.gitignore` 静默吞 `go.mod`、换机 clone 编不过 | §9 |

## 交付约定

- 每完成一个模块**立即增量汇报**，报**原始日志**不报"成功了"。
- 构建结果用数字说话（RAM/FLASH 占比、警告数）；关键改动落 README。
- `git push` 由用户自己处理，我只做本地 commit / diff。
- 不记录 token / 凭据 / 密钥。
