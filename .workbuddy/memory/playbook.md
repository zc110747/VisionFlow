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
- 真机摄像头：`LRCP USB2.0`（`VID_0C45&PID_6366`，1280×720@30）。
  ⚠️ `--list` 会把它列成 **两条**（同一台设备枚举两次），**只有 `--camera 0` 能真正上流**，
  `--camera 1` 报 `Failed to start`。枚举用 `camera-agent --list`，可用性必须实跑确认。
  ⚠️ **该模组画面被滚转 90°**（真实世界的"上"指向画面**左侧**）。修正 =
  **顺时针 90°**（`videoflip video-direction=90r`）。取证与影响面见架构文档 §12。

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
- **仓库根 `start.bat`（2026-09-17 新增）= `video-server\scripts\start-joint-ai.bat` 的全功能副本**，
  只是把路径基准从 `<repo>\video-server\scripts` 改成 `<repo>`（`REPO=%~dp0` →
  `ROOT=%REPO%\video-server` → `AGENT_DIR=%REPO%\carmera-agent`）。**新功能：`--probe` /
  `PROBE=1` 自检；`--no-pause` / `NO_PAUSE=1` 关掉收尾暂停**。详见 §6。
  早期根目录的 `start-demo.bat` 与本文件**无关**，不要混淆。

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

---

## §6 仓库根 `start.bat` 与 AI 能力选择（2026-09-17）

### 6.1 启动器

```bash
start.bat                                   # 与 scripts\start-joint-ai.bat 等价（AI+metadata 默认开）
start.bat --source videotestsrc --no-browser --no-pause --probe
```
- 参数与 `start-joint-ai.bat` 完全一致（`--camera/--stream/--source/--ai-fps/--no-browser/
  --ai/--no-ai/--metadata/--no-metadata`），**新增** `--probe`（跑 API 自检）、
  `--no-pause`（收尾不等按键）。环境变量同名可用（`PROBE=1`、`NO_PAUSE=1`）。
- 文件必须 **CRLF 换行**（LF 会让 `call :label` 间歇失败，见用户级 playbook §4.2）。
- ⚠️ **摄像头以「实际能否上流」为准，别看 `--list`**（2026-09-23 更正）：
  `--list` 里的两条 `LRCP USB2.0` 是**同一台物理设备被枚举两次**，
  `--camera 1` 起流即 `gst_base_src_start / Failed to start`
  （`camera busy / unavailable?`）⇒ **本机只有 `--camera 0` 可用**。
  当天插着摄像头时 `--list` 能列出且 index 0 正常出 1280×720@30。
  没插摄像头时 `--list` → `No cameras found.`，此时验证全链路必须 `--source videotestsrc`。
- ⚠️ 沙箱会在工具调用返回时**回收 `start ""` 起的子进程**（与 §3 一致）⇒
  启动器自己打印的 `[OK] ... healthy` / `registered` 与 `--probe` 就是**唯一可信的运行态证据**；
  不要在调用返回后再去 curl。`--probe` 正是为此加的（13 项 PASS）。

### 6.2 多摄像头结论：**架构上支持，链路已通**

| 环节 | 证据 |
|---|---|
| MediaMTX 动态收流 | `manager.go` 生成 `paths: all_others: source: publisher`，无静态 path 表 |
| 自动注册 | `monitor.go` 轮询 `paths/list?itemsPerPage=1000`，凡 `HasSource` 的 path 各建一行 |
| 表结构 | `cameras.stream_path UNIQUE`；`ai_frame/ai_object/ai_status/ai_aimode` 全部以 `camera_id` 为主键 |
| 前端 | `HomeView` 逐张 `CameraCard`；`CameraView/VideoPlayer` 按 `{id}` 实例化，各自持有 aiMode 与 metadata 轮询 |
| 身份映射 | `resolveCamera()` 先按 id、再按 `stream_path` 解析 agent 的 `metadata.camera_id` |

⇒ **N 个 agent 只要各自用不同的 `--stream` + 对应 `--metadata-camera-id` 就能并存。**
注意事项：① 两个 agent 都默认 `camera01`，会互抢同一 path；② `--auto` 的相机索引自动挑选
**取第一个** ⇒ 同机双 agent 必须显式 `--camera 0/1`；③ 三者共用同一个 MediaMTX `:8554`；
④ `verify_joint.py --stream` 默认 `camera01`，多路要逐路跑；⑤ 启动器只起 **1 个** agent
（多路要手工再起 N-1 个，或后续加 `--agents` 循环）。

### 6.3 AI 能力选择：**当前不支持，UI 是个"哑控件"**

服务端**没有任何能力/协商概念**，UI 恒显示 3 个按钮且从不置灰：

1. `Camera` 结构无 ai/capabilities 字段；只有 `ai_status` 心跳里的
   `ai_enable/ai_running/model/keypoint_count`（且只存最新一行，agent 掉线不清）。
2. `VideoPlayer.vue` 的 `aiModeOptions` 是硬编码三条，模板里**没有 `:disabled`**——
   而 CSS 里 `.mode-btn:disabled` 样式早就写好了 ⇒ 禁用态当初预留但没接上。
3. `GET /api/cameras/{id}/aimode` 无记录时**无条件返回 `ai-y`**（`DefaultAIMode`），
   即使该 agent 根本没跑 AI、甚至没连上 ⇒ UI 显示"人检测"为激活态，误导。
4. **静默死控件**：agent 的 aimode 轮询线程只在 `cfg_.ai.enable` 为真时启动
   （`stream_controller.cpp` `start_ai()` 早退）⇒ 后端不带 AI 时 UI 切模式 POST 得 200，
   但没有任何一方会响应。
5. `metadata.enabled:false` 时也不灰：`main.go` 总是构造非 nil 的 meta repo，
   `aimode` 的 `h.meta == nil` 分支永不触发，只有 `ingest` 会 503。
6. 盲区：agent 用 `--no-metadata` 时服务端**零信号**，无法区分"没 AI"与"没上报"。

**方案 A（纯服务端推断，改动最小，推荐先做）**：由心跳三态判能力 ——
`enable && running` → 支持（`model` 名含 `pose` 才放开 ai-y-pose，即
`keypoint_count>0`）；`enable && !running` → "想要但起不来"（无 AI 编译/模型缺失）→ 置灰；
无心跳或心跳超时（如 >30s）→ **unknown，按 fail-closed 全灰**。
覆盖"后端不带 AI"与"metadata 关掉"两种，唯独 `--no-metadata` 仍盲。
**方案 B（显式能力上报，最干净）**：status 心跳加 `capabilities{ai,modes,keypoint_count}`，
服务端持久化新列并在 `/api/cameras/{id}` 暴露；UI 按 `modes` 渲染。
需要同时改 agent（`metadata_encoder` / `AIStatusInfo`）+ server（repo/handlers）+ web。
**方案 C（握手注册）**：agent 启动 POST `/capabilities`，`POST aimode` 对不支持的模式回 409。
必备共同项：`POST /aimode` 对不支持的模式返回 4xx（而不是静默 200）+ UI 默认置灰。

---

## §7 手部 3D 识别分支（2026-09-23，P1a–P1e + P2 真机验收完成；P2 见 §7.6）

### 7.1 模型与几何（**改这些数字前先读 `carmera-agent/models/README.md`**）

两级级联：`palm_detection_mediapipe_2023feb.onnx`（NHWC 192²，2016 anchor）
→ 按掌心 7 点算旋转 → 裁 224² → `handpose_estimation_mediapipe_2023feb.onnx`（21 点 + handedness）。
两个模型已入库，Apache-2.0，SHA-256 在 `models/README.md`。

**anchor 表是最高风险项**（模型不内嵌，运行时必须同序生成，错了静默出垃圾框）：
```
idx    0..1151  stride 8  24×24 网格 × 2 锚/格
idx 1152..2015  stride 16 12×12 网格 × 6 锚/格
循环序 for y -> for x -> for k；中心 ((x+0.5)/grid, (y+0.5)/grid)
```
全 2016 值由 `tests/golden/palm_anchors_reference.txt`（**独立誊抄**官方字面量）钉死。

### 7.2 两个"z"的语义（实测，不是假设）

| 量 | 含义 | 用在哪 |
|---|---|---|
| `landmark.z` | **手腕相对**深度，手腕恒为 0（实测 1.8e-06），描述手指**朝向** | 手势几何辅助 |
| 手部 bbox 几何均值 `/frame_h` | **离相机远近**，单调 | logical Y = FORWARD/BACKWARD |

→ `HandConfig::depth_source` 默认 `"size"`，`"landmark_z"` 保留做 §16 实验对比。

### 7.3 验收与判据

```bash
# 单测（56 个，其中 11 个无模型也能跑）
./build-msvc/tests/camera-agent-tests.exe        # 需 PATH 含 gstreamer bin + onnxruntime/lib
ctest --test-dir build-msvc --output-on-failure
```
实测成绩（与官方参考同 ORT 后端）：21 点 **dx 0.00034 / dy 0.00079 / dz 0.00173 px**、
bbox 0.0005 px、anchor 全 2016 值 worst diff **7.45058e-09**。详见 `tests/golden/README.md`。

**手势阈值已全部实测（P1e）**，见架构文档 §11：

| 常量 | 值 | 出处 |
|---|---|---|
| `kExtendedRatio` | 1.15 | 黄金夹具开掌 1.59–1.84（留足余量） |
| `kCurledRatio` | **1.10** | 真机握拳簇上沿 0.983 / 开掌簇下沿 1.429 ⇒ 1.10 取中间 |
| `kGestureVote` | 0.75 | 四指里要 3 指一致 |

⚠️ **`kCurledRatio` 必须 < `kExtendedRatio`**：`hand_classify_gesture()` 是 `if / else if`，
**死区就是 `[kCurledRatio, kExtendedRatio]`**，两者交叉会让死区**静默失效**而不报错。
`hand_gesture_open_fist_ambiguous` 有 1.09/1.16 两个边界断言专门防这个。

⚠️ **index 指握拳时永不投 curled**（MCP5 离腕最近，比值仍 1.2–1.4）⇒ FIST 只靠
middle/ring/pinky 三指，**恰好卡在 `kGestureVote=0.75` 的等号上**，三指缺一即丢。

**标定方法（可复用）**：`hand.landmarks` 是**像素坐标 2 位小数**，精度远高于要标定的
ratio ⇒ **报文本身就够标定，不用改代码不用重编**。脚本
`carmera-agent/scripts/analyze_hand_landmarks.py`（`--tail` / `--gesture` / `--msg-from/-to`
/ `--timeline` / `--hist`，含 Otsu；**已从 scratch 提升为仓库内正式工具**，因为它逐字复刻了
`hand_classify_gesture()` 的公式，是复核阈值的唯一入口）。⚠️ **Otsu 会被过渡帧拉偏**
（过渡帧在构造上落在谷底且数量占优，实测因此给出 1.3546 —— 越过了 `kExtendedRatio`）
⇒ 脚本已强制钳到 `kExtendedRatio - 0.05`；**标定姿势必须保持不动**。

### 7.4 坑（本项目专属，方法论已升级到全局 playbook §11）

1. **夹具必须 PNG。** JPEG 下 stb_image 与 libjpeg-turbo 差 3 LSB/5% 像素，
   残差被抬到 0.43 px，比真实误差大 250 倍。
2. **官方 `_preprocess` 的 `pad_bias` 在 `if` 分支内赋值**，誊抄漏了 → 基准脚本自己错、
   每个框偏移 +160 px，看起来像"网络选错了手"。**差分对不上先怀疑基准。**
3. **构建环境**：`cmd.exe` 中转被沙箱拒（全局 §3.2），跑 .bat 只能
   `./build_oneclick.bat --no-pause < /dev/null`；要精确控制 target 或 `grep` 零警告，
   用手工 MSVC env（全局 §7.4），注意 **Debug 首配需要 SDK 的 `rc.exe`/`mt.exe`**。
   `build_oneclick.bat` 默认后端 = gstreamer，与现有 `build-msvc` 一致。
4. **链接报 `LNK1104: 无法打开文件 src\camera-agent.exe` = exe 正被占用**，
   多半是后台还在跑采集窗口 ⇒ **先停采集再构建**。
5. **构建日志别写 `/tmp`**（`tee /tmp/x.log` → Permission denied，本环境 `/tmp` 不落文件），
   写项目目录。`source scripts/msvc-env.sh` 前若报「could not detect MSVC toolset」，
   是 `sort -V` 被 System32 遮蔽（脚本已修，优先 `/usr/bin/sort`）。
6. `tasklist //FI "IMAGENAME eq x.exe"` **静默返回空**（System32 垫片接管）——
   要判"还有没有进程"只能用 `tasklist | grep -i x`。
7. **抓帧取证必须用「与产品同款」的采集路径。** 用 `ffmpeg -f dshow` 抓的帧，
   不等于 agent（`mfvideosrc` / Media Foundation）看到的帧 —— MF 会应用 UVC 的 roll 属性。
   凡涉及"设备到底输出什么"，判据要么来自同款链路，要么**两条路径互相印证**。
8. **涉及坐标系的结论不能靠"看着对"。** 第一遍肉眼判画面朝向得出了自相矛盾的两条线索。
   正确做法：把**所有候选解都渲染出来做排除**（±90° 两种都做，看哪种人脸正立）。
   这次的定案依据是「放大裁脸看发际朝哪」+「逆时针版人头朝下」。

### 7.5 画面朝向：摄像头滚转 90°（2026-09-23 **已解决：物理转回，零代码**）

**结论**：真实世界的"上"指向画面**左侧** ⇒ 侧躺 = 顺时针 90° 装反。用户把本体转回后
实测正立（同款源 `mfvideosrc` 抓帧判读：天花板在上、眼镜水平、置物架在左，仍 1280×720）。
**方案 A（软件旋转）不采用，但已现场验证可用，留作备选**：

| 项 | 值 |
|---|---|
| GStreamer | `videoflip video-direction=90r`（`method=clockwise` 已 deprecated） |
| ffmpeg | `transpose=1` |
| 实测输出 | 1280×720 **→ 720×1280**（竖幅），这才是当初不想走 A 的代价之一 |
| 插入位置 | **caps 之后、`tee` 之前**（放 tee 之后只有 AI 分支被转 → overlay 错位） |

**为什么当初必须修**（现在已由物理转回解除）：`hand_map_coordinates()` 硬假设画面正立
（`x=cx/fw`、`z=(fh-cy)/fh`），只有 `invert_x/y/z` 三个**符号翻转** —— 90° 滚转是
**轴对调**，符号翻转折不出来；侧躺时 hand 的 logical X 实际测的是**竖直**方向。
⚠️ `invert_*` 的**取值**仍未实验确定（§16），但现在只是"符号选哪个"，不是"轴对调"。

**⚠️ 排查朝向时的坑（本轮踩过）**

1. **别用 `ffmpeg -f dshow` 抓帧去推设备行为** —— agent 走的是 `mfvideosrc`
   （Media Foundation），MF 会应用 UVC 的 roll 属性，两条路径可能不一致。
   判据必须取自**与产品同款的那条链路**。
2. **别靠单一线索肉眼判"上在哪"** —— 我先按"灯板在左"判一次、又按"脸的朝向"判一次，
   两次**互相矛盾**。可靠线索是**上下不对称且无歧义**的目标（文字、眼镜、发际线），
   必要时裁近放大再看。"亮斑在哪"只能说明光源位置。

### 7.6 P2 服务端 hand 落库（2026-09-23 完成）

**接口**（`internal/api/hand.go`）：`GET /hand/sessions[?gap_ms=N]`、
`GET /hand/sessions/{index}`（下载 `hand_session_<cam>_<n>.json`）、`DELETE /hand/log`。
未知相机 → **404**（不是"0 个会话"，否则打错 id 看起来像"录制器正常但空"）。

**两条设计决定（都有实测依据）**

1. **payload 存 agent 原字节**，不重新序列化 —— 库里 `position` 是 `[0.0000,...]`
   这种 C++ 定点格式，Go 重编码会写 `[0,...]`。这就是"服务端不改写生产者数据"的**可验证据**。
2. **会话切分是两条独立判据**：`detected=false` **无条件**结束会话；
   `gap_ms` 只管"全是 detected 的行之间隔了多久"（录制器漏采 / `detected_only`）。
   **任何 gap 都跨不过"手离开画面"** —— 否则"握住→松开→再握"会被合并成一次动作。
   切分用 agent 时钟（PTS）而非 `received_at`：网络抖动会让重发的 POST 看起来像"手走开又回来"。

**落库纪律**：`ai_frame.hand` 与 `ai_hand_log` 追加**同事务**（避免"网页看到了但日志没有"）；
`hand_log_max_rows <= 0` 关录制器但**最新帧照样写**（前端不依赖历史）；
裁剪在事务**之外**做（剪失败只多占空间，绝不导致 ingest 失败）。

**⚠️ 联合运行下跑 hand 的硬前提**：`ValidAIMode` 必须收 `ai-hand`。
否则 agent 的 aimode 轮询器读回 `ai-y`，**1 秒内把 agent 从 hand 拽回 detect** ——
症状是"agent 日志明明打印了 `mode=hand` 却立刻变成 detect"，很容易误判成 agent 的 bug。

**⚠️ 灌合成帧前先 `POST {"mode":"ai-off"}`**：日志按 `ORDER BY id`（插入顺序）排列，
真机帧的 PTS 与合成帧不是同一时间基，两者互相插队会触发"时间戳倒退 = agent 重启"
那条切分规则 —— 本该 1 个会话会被切成 3 个。不是 bug，但不设防的验证脚本会
据此去改没坏的东西（实测数字见架构文档 §13.8）。

**真机验收实测（2026-09-23，见架构文档 §13.9）**

| 项 | 数字 |
|---|---|
| `ai_hand_log` | 16460 行，`detected=1` **2125 行**（OPEN_PALM 808 / FIST 683 / UNKNOWN 634）|
| 每行 landmark 点数 | 2125 行**全 = 21**，异常 0 |
| 会话数（`gap_ms=1000`）| **194**，最大会话 559 帧 |
| 逐字节保真 | 559/559 条 payload 原文在导出响应中原样出现 |
| 数值保真 | 35217 个坐标，不一致 **0** |

**① `id` 是这份日志里唯一的单调键。** agent 每次启动 `frame_id` / `timestamp`
**双双归零**（实测 `(fid=77702,ts=2591322)` → `(fid=1,ts=700)`，Δ=−2590622ms）。
切分按插入序 `id` 所以不受影响，但**导出文件必须带 `id`**，否则下游离线分析按
`frame_id` 排序会把两次运行交错。`HandLogRow.ID` 已补（`repo.go` 的导出 SELECT 也要带上）。

**② `gap_ms` 只在往小调时生效。** 真机相邻行 `timestamp` 间隔 p50 = **128ms**（≈8fps），
所以 `gap_ms >= 200` 都会桥接整段 hold。实测：`30→2193`、`128→697`、`200→195`、`1000→194`。
我先把 gap 往 **600000** 试，看到"完全没作用"一度以为参数坏了 —— **那是探测方向错**。

**③ landmark 可以落在画面外，且这是设计。** `hand_detector.cpp:782-783` 的 landmark
映射**刻意不 clamp**（bbox 在同函数 `:810-813` 里是 clamp 的）。实测 y 最大 840.96 > 720，
原因不是映射错：这一轮手**贴画面底边举**，腕点本身 `y=742~756`，第二级裁切方块伸出边界。
**判据不需要看画面**：x 和 y 会不会**一起**越界 —— 只 y 越界且贴着某条边 ⇒ 手本来就出框。
**P3 影响**：canvas overlay 不能假设 `landmark ∈ [0,w]×[0,h]`。

**④ 真机 `ai_frame` 零回归的直接证据**：`camera01` 是 `ai_mode=ai-hand` 且 `hand` 非空（327 B）；
历史相机 `testcam` 的 `ai_mode` 为 NULL、`hand` 列 **IS NULL** ⇒ detect/pose 帧从不写该列。

