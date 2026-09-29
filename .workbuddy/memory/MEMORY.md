# VisionFlow 记忆索引

> **只放骨架 + 指针**；明细在 **`playbook.md`**（按 §号 Read 那一节）。
> 时间线原始记录在 `YYYY-MM-DD.md`，按需读最近的。
> ⚠️ 本文件超 **~4k 字符**会被静默截断（改完必查 `len()`，别拿 `wc -c` 字节数比）。
> 新增内容优先写 `playbook.md`，这里只加"每次会话都必须遵守"的铁律指针。

## 项目是什么

`VisionFlow` = **video-server**（Go 1.27 单二进制：MediaMTX 托管 + REST API + 嵌入式 Vue3
Web UI + SQLite + AI metadata 落库 + AI 模式下发） **+ carmera-agent**（C++/MSVC +
GStreamer：UVC 采集 → H264 → RTSP 推流 + 端侧 YOLO11 检测/姿态 + ByteTrack + 结果上报）。
链路：`摄像头 → agent → MediaMTX → video-server → 浏览器(WebRTC/HLS)`。

## 铁律

1. **联合运行一律用 `config/config.joint.yaml`（:8081）** —— 本机 `8080` 被 `ApplicationWebServer`
   占用、`9997` 被 `vivoSyncService` 占用，默认配置会 bind 失败。→ §1
2. **配置路径必须带 `config/` 前缀** —— `Load()` 找不到文件会**静默回退默认 8080**，不报错。→ §2
3. **联合运行只有一个 MediaMTX** —— 不要执行 `carmera-agent\start-camera-agent.bat`。→ §2
4. **agent 后端看 `--version`/CMakeCache，不看目录名** —— 本 checkout 是 `gstreamer` 后端。→ §3 §6
5. **判据看 MediaMTX `paths/list` 的 `online=true`**，不要看 `/api/cameras`（DB 会复用）。→ §3
6. **仓库根 `start.bat` 是联合运行入口**；文件必须 **CRLF**（LF 会 `call :label` 假 FAIL）。
   验证全链路可 `--source videotestsrc`；`--source` **只吃单个元素名**（不能传整条 pipeline）。
   真机 UVC 走 **`mfvideosrc`**（非 dshow）+ GStreamer 后端。→ §6 §8.14
7. **手势控臂（Phase 2）验收走 `carmera-agent\scripts\verify_gesture_arm.py`** —— 四级阶梯，
   输出 PASS/FAIL 计数。Level 1 要 :9100 有 `armpilot-backend.exe`；没有就 `--no-live`。
   **依赖是单向的**：`RobotLink` 消费 `HandState`，`Hand*` 代码不许反向知道机器人。→ §8
8. **不要用 `-Clean`** —— 宿主安全包装会拦 `Remove-Item -Recurse` 并把 `build-msvc` 删空。
   要干净重建先手工挪走目录。→ §8
9. **`lost_frames` 是对着 AI 推理率（~8Hz）算的，不是相机 30fps** —— 实机抖动 1–2 帧
   （≈0.25s），设小会误判"手离开"并强拉 HOME。当前默认 **6**。→ §8.8
10. **构建偶发 `C1083 Permission denied`（obj 不存在）= 杀软瞬时锁，原样重试即过** ——
   与 `LNK1104`（exe 被占用）区分：先 `tasklist` 排除进程，再 `ls` 看该 obj 是否存在。→ §8.7
11. 🔴 **`caps.workspace` 是几何包围盒，不是可达域 —— 虚大 1.6–3.5×**。**凡"手位移 → 臂位移"
   的换算与"距边界多远"的判定，基准一律用实测 `reach_span_x/y/z`（39/176/41.5）**。
   换臂/改结构后必跑 `measure_reach.py`。→ §8.12
12. **链路绿 ≠ 臂动了** —— 真机无位置反馈。回归断言必须落在**真实位移**上
   （`ASSERT(travelled > 10.0f)`），不能只看 sent/rejected 计数。→ §8.12
13. 🔴 **「日志里连 `[ROBOT]` banner 都没有」= 分支从未启动**（查 `--robot` / `robot.enable` /
   `control branch started`）；**有 banner 没后续** = 启了连不上；**有 `cmd ok` 但臂不动**
   = §8.12 尺子问题。两个启动器已**默认带 `--robot`**。→ §8.13
14. **`gain_x/y/z` 是手感参数，行为测试须 pin 成 1.0**，另用 feel pin 测试钉 shipped 值
   （默认 **0.25/0.5/0.25**，三轴不同值因 span 差 4.5×）。降 gain 后 X/Z 手抖落到 0.5mm
   死区之下**被自然滤除** —— 这就是"解决波动"的机制。→ §8.14
15. **起 `armpilot-backend.exe` 不能加 `-WindowStyle Hidden`**（永不监听 9100）。
   抓 agent 日志用 `& exe ... 2>&1 | Out-String`。→ §8.14

## 常用命令

```bash
video-server\build_onelick.bat          # 全仓库构建；scripts\build.bat 只 server
carmera-agent\build_oneclick.bat        # 只 agent（默认 gstreamer）
start.bat                               # 仓库根：联合运行入口
start.bat --source videotestsrc --no-browser --no-pause --probe   # 无摄像头自检
video-server\scripts\stop-joint.bat
python video-server\scripts\verify_joint.py / verify_e2e.py / verify_metadata.py
python carmera-agent\scripts\verify_gesture_arm.py    # 手势控臂四级阶梯
python carmera-agent\scripts\measure_reach.py         # 三轴可达域实测
cd video-server && go test ./...                      # 40 用例
cd carmera-agent && ctest --test-dir build-msvc --output-on-failure   # 85 用例
```

## 按需查阅（读 playbook.md）

| 需要什么 | 去哪 |
|---|---|
| 端口表 / 工具路径 / 真机摄像头规格 | §1 |
| 配置陷阱（静默回退、require_known_camera） | §2 |
| 验收脚本与实测坑（TCP 推流、后台进程挂起、DB 复用） | §3 |
| **"视频正常但 AI 不显示"排查顺序** / 二进制过旧判定 | §4 |
| 9/4 merge 冲突残留事件与教训 | §5 |
| **根 `start.bat` + 多摄像头结论 + AI 能力选择缺口（方案 A/B/C）** | §6 |
| **手部 3D 识别分支**（模型/anchor 表/两个 z 的语义/黄金基准/实测成绩/构建 env） | §7 |
| **手势控臂 Phase 2**（robot 模块/七条协议坑/四级验收/合成与真实帧验证局限/**L3 真机三条硬结论**） | §8 |
| **包围盒 ≠ 可达域**（三轴实测表 / 修前修后步长对比 / 细探 / `effective_bounds`） | §8.12 |
| **「没有命令下发」= 分支从未启动**（静默型故障三类判据 / 启动器默认带 `--robot`） | §8.13 |
| **降增益抑制手抖**（gain 0.25/0.5/0.25 / 三轴为何不能同值 / 手感参数怎么 pin / backend 必须带控制台启动） | §8.14 |

## 交付约定

- 每完成一个模块**立即增量汇报**，报**原始日志**不报"成功了"。
- 构建结果用数字说话（RAM/FLASH 占比、警告数）；关键改动落 README。
- `git push` 由用户自己处理，我只做本地 commit / diff。
- 不记录 token / 凭据 / 密钥。
