# VisionFlow 记忆索引

> **只放骨架 + 指针**；明细在 **`playbook.md`**（按 §号 Read 那一节）。
> 时间线原始记录在 `YYYY-MM-DD.md` 日戳文件里，按需读最近的，不必全读。
> ⚠️ 本文件超 **~3k 字符**会被静默截断，新增内容优先写 `playbook.md`。

## 项目是什么

`VisionFlow` = **video-server**（Go 1.27 单二进制：MediaMTX 托管 + REST API + 嵌入式 Vue3 Web UI
+ SQLite + AI metadata 落库 + AI 模式下发） **+ carmera-agent**（C++/MSVC + GStreamer：
UVC 采集 → H264 → RTSP 推流 + 端侧 YOLO11 检测/姿态 + ByteTrack + 结果异步上报）。
链路：`摄像头 → agent → MediaMTX → video-server → 浏览器(WebRTC/HLS)`。

## 铁律

1. **联合运行一律用 `config/config.joint.yaml`（:8081）** —— 本机 `8080` 被 `ApplicationWebServer`
   占用、`9997` 被 `vivoSyncService` 占用，默认配置会 bind 失败。→ §1
2. **配置路径必须带 `config/` 前缀** —— `Load()` 找不到文件会**静默回退默认 8080**，不报错。→ §2
3. **联合运行只有一个 MediaMTX** —— 不要执行 `carmera-agent\start-camera-agent.bat`。→ §2
4. **agent 后端看 `--version`/CMakeCache，不看目录名** —— 2026-09-17 更正：本 checkout 的
   `build-msvc\CMakeCache.txt` 里 `CAMERA_AGENT_BACKEND=gstreamer`，`--version` 打印
   `Backend : gstreamer`，实测能真推 H264；"`build-msvc`=SIM" 是**旧 checkout 的结论**，
   动手前先 `camera-agent.exe --version` 核对。→ §3 §6
5. **判据看 MediaMTX `paths/list` 的 `online=true`**，不要看 `/api/cameras` 有没有该路
   （DB 会复用上一轮）。→ §3
6. **仓库根 `start.bat` 是联合运行入口**；文件必须 **CRLF**（LF 会 `call :label` 假 FAIL）。
   本机**无可用 UVC 摄像头**，验证全链路要 `--source videotestsrc`。→ §6

## 常用命令

```bash
# 构建（各自目录）
video-server\build_onelick.bat                     # 全仓库：Web + server + agent
video-server\scripts\build.bat                     # 只 server
carmera-agent\build_oneclick.bat                   # 只 agent（默认 gstreamer 后端）

# 运行
start.bat                                              # 仓库根：联合运行入口（= start-joint-ai 全功能）
start.bat --source videotestsrc --no-browser --no-pause --probe   # 无摄像头时的全链路自检
video-server\scripts\start-joint.bat --ai --metadata    # 联合运行 + 开浏览器
video-server\scripts\stop-joint.bat
video-server\start_oneclick.bat                         # 只起服务端

# 验收（输出 PASS/FAIL 计数）
python video-server\scripts\verify_joint.py        # 真实推流端整链路
python video-server\scripts\verify_e2e.py          # ffmpeg 合成流
python video-server\scripts\verify_metadata.py     # AI 结果接收
cd video-server && go test ./...                   # 40 个用例
cd carmera-agent && ctest --test-dir build-msvc --output-on-failure   # 29 个用例
```

## 按需查阅（读 playbook.md）

| 需要什么 | 去哪 |
|---|---|
| 端口表 / 工具路径 / 真机摄像头规格 | §1 |
| 配置陷阱（静默回退、require_known_camera） | §2 |
| 验收脚本与实测坑（TCP 推流、后台进程挂起、DB 复用） | §3 |
| **"视频正常但 AI 不显示"排查顺序** / 二进制过旧判定 | §4 |
| 9/4 merge 冲突残留事件与教训 | §5 |
| **根 `start.bat` 用法 + 多摄像头结论 + AI 能力选择缺口（方案 A/B/C）** | §6 |
| **手部 3D 识别分支**（模型/anchor 表/两个 z 的语义/黄金基准/实测成绩/构建 env） | §7 |

## 交付约定

- 每完成一个模块**立即增量汇报**，报**原始日志**不报"成功了"。
- 构建结果用数字说话（RAM/FLASH 占比、警告数）；关键改动落 README。
- `git push` 由用户自己处理，我只做本地 commit / diff。
- 不记录 token / 凭据 / 密钥。
