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

### 7.7 P3 Web 端骨架 / HUD / 面板（2026-09-23 完成）

**文件**：`web/src/ai/{skeleton,hand,draw}.ts`（新）、`web/src/components/{HandHud,HandPanel}.vue`（新）、
`VideoPlayer.vue`（改）、`CameraView.vue`（改）、`client.ts`（改）。
**验收**：`python scripts/verify_hand_overlay.py` → **PASS=41 FAIL=0**；`cd web && npm run check` → clean。

**① 骨架画不出来的根因是"按点数硬编码 17"。** `o.keypoints.length === 17` 让 21 点的手部
永远落不进分支 ⇒ ai-hand 在页面上是"**有方框、没有骨架**"（那个框来自 `objects[]` 里的
`class="hand"` 记录，它一直都在）。拓扑现在是数据：`skeletonFor(count)`，**未知点数返回 null
什么都不画**（硬套短数组会拉出孤立线，看着像跟踪错误，把排查引到错误的层）。

**② hand 帧的骨架只画一次。** `ai_pipeline.cpp` 里 `o.keypoints = hs.landmarks`，所以
`objects[0].keypoints` 与 `hand.landmarks` **同源**。overlay 只用 `hand.landmarks` 画，
objects 循环对 hand 帧跳过骨架 —— 两处都画会叠成双层，像素级重合时看不出，
一旦哪天只改了一处就会像"检测到两只手"。探针逐点比对（1e-6）钉住这条契约。

**③ `delta` 在 `reference_initialized=false` 时必须不显示。** 此时它相对零原点 ⇒ 数值上等于
绝对位置，显示出来就是"手一进画面飞了一大段"。HUD 显示 `—` + `REF LATCHING` → `REF LOCKED`。

**④ `detected=false` 也要画 HUD**（显示 `NO HAND`），否则"手不在画面里"和"前端坏了"长得一样。

**⑤ 前端 `AI_MODES` 是白名单**：`loadAIMode()` 用它过滤服务端下发的模式，
漏登记 `ai-hand` ⇒ 按钮永不点亮、帧永不匹配（**静默**，不报错）。

**⑥ 本轮新增的两道防线**（都可复用）：
- `web/tools/check-sfc.mjs`（`npm run check`）：`tsc` 不读 `.vue`、esbuild 只转译 ⇒
  `<script setup>` 是类型盲区，删 import 留引用会构建全绿、运行时白屏（本轮真实发生）。
  它抽 script 块 + 补编译器宏声明 + 交 tsc + 行号映射回 `.vue`。
- `scripts/verify_hand_overlay.py` + `scripts/hand_overlay_probe.mjs`：
  esbuild 打包**前端自己的模块**、用**记录型 2D context** 断言绘制调用（条数 + 坐标是否被 clamp）、
  Vite `ssrLoadModule` + `@vue/server-renderer` 渲染组件并扫 `undefined`/`NaN`。
  **正样本锚点取自活 payload，且特意挑非 `UNKNOWN` 的手势帧**（`UNKNOWN` 会蒙混过关）。

**⑦ 验前端用的"真机数据"优先从录制库取**：`ai_hand_log` 是 append-only + 原字节保真，
`GET /hand/sessions/{i}` 就能拿到真机帧 —— 本次 1078 帧全部带 21 点，
**不必为了验前端让人再举一次手**（1078 帧里有 1271 个坐标在画面外，也一并验到了）。

**⑧ 部署的隐性前提**：`web/dist` 是**编译期** embed 进 exe 的 ⇒ 只 `npm run build` 不重编 exe，
浏览器里还是旧 UI。`start.bat` 的重建判断曾漏掉这一点（只看 `web/src` vs `web/dist`），已补
"dist 比 exe 新即 STALE"。**改 `.bat` 提醒**：括号块内的 `)` 会提前闭合分支，REM 里也不能有括号。

### §7.8 P3 真机像素验收 + 五个环境陷阱（2026-09-23）

**验收四层**：录制库数据 → 绘制逻辑（记录型 2D context）→ 组件字符串（SSR）
→ **真实浏览器像素**。前三层都能无人值守，第四层必须真浏览器。

| 现象 | 根因 | 修法 |
|---|---|---|
| 长命令**无输出 + SIGTERM** | 命令被 `\| tail` 包住，管道缓冲 ⇒ 上层看到"长时间零输出"，按看门狗杀掉 | 长命令**不走管道**；解释器加 `-u` |
| 脚本在 `agent-browser open` 之后**静默结束、退出码 0** | 批处理里裸调 `.cmd`（agent-browser 是 `.cmd` 垫片）⇒ 子批处理**替换**父批处理 | 每处调用都加 `call`（含 shim / npm 的 `.cmd`） |
| 从 Python 子进程引导浏览器守护进程被整条杀掉 | 未查明（同一命令从 shell 发就正常） | **shell 驱动浏览器、Python 只做断言**（`VF_FROM_DOM` / `VF_FRAME_JSON`） |
| 浏览器 CLI 空转很久后报 `Chrome not found` | 守护进程被回收 | 顺序改成**先等手、再开浏览器**，浏览器会话快进快出 |
| `set` 进变量的长字符串**少字符** | `EnableDelayedExpansion` 下 `!` 启动延迟展开，吃掉到下一个 `!` 之间的字符（实测 1313→1270 字节） | 值里不用 `!`；或局部 `DisableDelayedExpansion` |

**"钉帧"是为了让结论不依赖操作者**：断言应比对**绘制那一刻的帧**，不是"断言那一刻的手"——
操作者一觉得跑完就放下手臂，读实时会把结论变成 SKIP、掩盖真实结果。

**union 陷阱**：画布像素是 **bbox 矩形 + 类别标签 + 骨架**的并集。本项目 agent 给的手部
bbox 是**满画面高**（`[350,0,749,720]`，而 landmark 只占 y 186..697），矩形自己就画出两条
贯穿全高的竖线 ⇒ 质心/重叠检查是 union 检查，**"骨架只画一次"必须另找证据**并在脚本里写明。

### §7.9 第六个陷阱：宿主任务一结束，联合服务整棵进程树被回收（2026-09-23 实测）

**现象**：用 Bash 后台任务起 `start.bat --ai --ai-mode hand --no-pause` 并在同一条命令尾部
`sleep 7200` 持有进程树。任务跑满 **2h 0m 14s** 后自然结束，`netstat` 立刻显示
**8081 / 8889 / 8554 全部无监听**。日志里 `LAUNCHER_EXIT=0`，
且 `start.bat` 自己打印过
`[DONE] Services are running in separate windows; this launcher has finished.`
——**这行不代表服务能活过宿主任务**。

**根因**：Bash 工具执行的长命令被放在一个受管理的作业/进程树里；任务结束（**包括 `sleep`
自然到期**）时整棵树被回收。`start.bat` 用 `start` 拉起的"独立窗口"仍在这棵树内，一并被收。

**判据**：**退出码 0 ≠ 服务还活着**；`launcher finished` 只描述启动器的生命周期，不描述
服务的生命周期。判断服务是否在跑只认 `netstat` 的 LISTENING。

**修法**：
- 真机验收期间，持有任务的存活时间必须 **≥ 你还要验收多久**；验收完主动
  `video-server\scripts\stop-joint.bat`，不要靠任务自然结束回收。
- 需要长时间常驻时，别用"后台任务 + sleep"持有，而是把持有时间设得足够长（或分次续期）。
- 脚本化验收（`verify_*.py`）自身要能**先探活**再断言，探不到就明确报"服务未运行"，
  不要把它当成 overlay 的失败。

### §7.10 P4 端到端验收的两个可复用手法（2026-09-24）

**手法一：受控夹具用「探针 camera」。** `POST /api/cameras` **只写 DB 行、不碰 MediaMTX**，
所以可以建一个专用 camera id、精确摆放合成帧、读回来断言、再删掉，**全程不影响真实
链路和在线 agent**。用它做了两件不需要真手的事：

- **落库保真**：POST 一个带**未知字段**的 hand 块（`vf_probe_sentinel`），导出后断言
  该字段原样存活 ⇒ 机械证明"存原字节而非重新序列化"（`HandState.raw` + `MarshalJSON`）。
  这是**唯一**能证明该设计的检查；比对字段名集合是证明不了的。
- **会话切分**：帧摆在 t=0/5000 ⇒ gap=1000 切 2 段、gap=10000 合 1 段；
  t=0/100/200 中间夹一帧 `detected=false` ⇒ gap=100000 仍是 2 段。

⚠️ **不要拿跨天长日志去断言"大 gap 会合并"** —— 会话边界有**两条独立判据**（时间间隔
与"手离帧"），**任何 gap 都跨不过后者**。库里的会话几乎都是被丢帧切开的。这条
架构文档 §13.4 已经写过一次，2026-09-24 又踩了一次。

**手法二：人机协作实验前，先用合成真值自证分析工具。** `verify_hand_direction.py`
带 `--self-test`：造一段"四个已知动作 + 抖动"的序列，断言分段算法切出的**主轴与符号**
全对。不这样做的话，实验时**切错段会被读成人做错动作**，结论正好反过来。

锚点型分段的三个失败变体（都实测过，在真实数据上**不可见**）：

| 变体 | 实测症状 |
|---|---|
| 锚点每帧跟随 | 追掉 0.05 动作的 72% ⇒ **0 段**（会被读成"操作者没动"） |
| 用最近 N 帧均值重新居中 | 那些帧横跨回位斜坡，锚点偏中位 0.016 > `return_band` ⇒ **下一段永远合不上，吞掉后面全部动作** |
| 吸附但忘了清零 settle 计数 | 此后每帧都吸附 ⇒ 锚点钉在手上，峰值只有 0.0101 |

正解：锚点只在**确认静止 `settle_ms`** 后吸附，**吸附后必须清零计数**。另外要有
**入画过渡跳过** —— 录制从"手刚入画"开始，那一瞬的位移本身就是一次"永不回位"的运动，
会把第 1 个动作吃掉。

**顺便：端口探活不要用 `netstat`。** `grep -E ":(8081|8554)\b"` 在此环境给出**假阴性**
（服务 HTTP 200 却匹配不到）；换个写法又能匹配。判据直接打 HTTP 接口。

---

## §8 手势控臂 Phase 2（2026-09-29，四级验收全通）

### 8.1 架构与边界（**这条是硬约束，不是风格**）

```
HandState -> RobotLink(映射/限速/边沿) -> IJsonLineClient -> TCP :9100 -> armpilot-backend
```

`hand_types.h` 原文："Phase 2 adds HandState -> RobotCommand in a **SEPARATE** project.
Nothing here may know about a robot, IK, servos, MuJoCo or MeArmPilot."
→ 依赖**单向**。新增代码全在 `carmera-agent/{include,src}/camera_agent/robot/`，
`hand_types.h` / `IDetector` / `HandAnalyzer` / video / AI 路径**一行未改**。

生效三条件（缺一不影响视频）：① Web UI 切 `ai-hand`；② 启动带 `--robot`；
③ `:9100` 有后端。连不上只 **WARN**，`RobotLink` 自己退避重连（默认 2s），视频照推。

### 8.2 七个非直观实现点（踩过才知道）

1. **`update()` 返回值只有一个语义**："传输还能用吗"。被服务端拒绝（`JOINT_LIMIT`）
   和低于死区跳过都是**正常协议结果**，仍返回 true。判"有东西发生"要写成
   `sent增 + rejected增 == 1`，**不能只断 `sent > before`** —— `sent_` 只计被接受的。
2. **`do_moveto` 记录的是"下发目标"，不是应答 `tcp`**。真机无位置反馈，应答 `tcp` 是
   命令值，而 `home` 的应答是 home 位姿 → 拿应答当基准比对永远不成立，死区守卫失效会重复发。
3. **`open()` 握手后必须清零会话计数**：`sent_=rejected_=skipped_=0`。
   caps+home 是连接维护不是跟手流量，留在计数里会给测试一个恒定偏移。
4. **首帧不算"重入"**：`presence_established_` 保证会话第一帧只记录不触发 HOME，
   否则每次开会话都白跑一次 home（`open()` 刚发过）。
5. **参考点未锁存必须直接 return**：`!hs.reference_initialized` 时 delta 是到画幅原点的
   巨大位移，消费它会把手甩出去。这是防首帧幽灵位移的**不变式**，测试要单独钉。
6. **增益不是拍脑袋**：`gesture-control.md §2.1` 推 `gain = workspace跨度 / 手有效行程`；
   `axis_scale(lo,hi)=(hi-lo)/kHandActiveSpan`（0.5），`gain_x|y|z` 是乘数。
   轴对应：手 `dy`→臂 `X`，手 `dx`→臂 `-Y`（**镜像**，左侧是 +y），手 `dz`→臂 `Z`。
7. **`spdlog` 占位符是 `{}`，不是 `%.2f`/`%s`** —— 写 printf 风格会静默乱码。

### 8.3 四级验收阶梯（`carmera-agent/scripts/verify_gesture_arm.py` 一键跑）

| 级 | 验什么 | 测试 / 门槛 |
|---|---|---|
| L0 | 构建 | `scripts/build-msvc.ps1 -Backend sim`，0 warning / 0 error |
| L1 | VisionFlow 指令直驱真 sim 臂 | `robot_live_backend_drives_sim_arm`，**`ARM_PILOT_LIVE=1` 才跑** |
| L2a | 合成像素对**已知几何真值** | `robot_synthetic_*`（2 个） |
| L2b | **真实帧**走同链路（无真值，断鲁棒性） | `robot_realframes_*`（2 个） |
| L3 | 实时摄像头（**人工**） | 脚本会打印命令 |

**L1 必须默认 skip**（靠 env 开启），否则 `ctest` 就不再是无依赖的。

实测数据（可直接引用）：caps `protocol=2 ws=[40.4582,-153.012,49.0849]..[175.883,153.012,114.68]
home tcp=[115.033,0,109.224]`；真实后端把大位移**拒绝**（坑 4 真发生）、
极小位移**跳过**（坑 1 真发生）；真实帧检出率 **20/40**（手贴脸遮挡，数据性质）。

### 8.4 合成序列的两个硬限制（别试图绕过）

1. **拳头只能注入，不能画**。画出来的拳头掌心模型检出 **0 palms**；裁夹具自身掌心区过得了
   掌心模型，但被 landmark 模型以 `conf < 0.5` 拒绝。两条路都得不到 `HandState`。
   → `fist_inject_*` 帧标 `"injected": true`，C++ 侧喂**冻结的真实 landmark 表**。
2. **去抖要求连续帧数**：`gesture_confirm_frames = 5` ⇒ 注入**至少 5 帧**，
   1 帧会被当成候选吸收掉，`gripper` 计数永远是 0。

### 8.5 EMA 与"settled"—— 判据不能偷懒

`HandAnalyzer` 用 EMA（`filter_alpha=0.4`）平滑位置 ⇒ **运动帧报的位置还在追**。
拿它和原始几何直接比，误差 0.0992 恰好卡在 0.10 容差线上（看起来"过了"，其实是在测滤波器滞后）。
正解：生成器标 **`settled`**（本帧及前两帧 placement 完全相同 = 手静止了三帧、EMA 已收敛），
测试对 settled 帧用严格容差、运动帧用"滞后量级"的宽松容差。放松单一容差会把映射 bug 藏进滞后里。

### 8.6 生成器的坐标系陷阱

`measure_box_ratio()` **必须在夹具坐标系标定**。拿模型框（夹具坐标）去比裁剪后 patch 的
墨迹（patch 坐标）会凭空造出 ~220px 偏移。正解：`cut_hand()` 返回 `cut_rect`，
在同一坐标系内标定（实测 offset ≈ (−0.2,−0.1) px，预测误差降到 0.029）。
真值取"合成帧非黑像素范围"**直接量测**，不要做易漂移的代数推算。

### 8.7 本机环境坑（本轮新增）

- **`-Clean` 不可用**：宿主 fail-closed 安全包装拦 `Remove-Item -Recurse`，报
  `[safe-delete][SAFE_DELETE_FAIL_CLOSED] trash-failed`，并把 `build-msvc` 删到只剩日志
  （进程 exit 0，看起来像成功）。要干净重建就**先手工挪走目录**再做普通 build。
- **托管 Python 3.13.12 没 cv2**；有 cv2 5.0.0 的是
  `C:\Users\lx176\.workbuddy\binaries\python\envs\default\Scripts\python.exe`。
- **PowerShell 工具吞构建输出**：`Tee-Object` 落 **UTF-16** 日志，读的时候按
  `utf-16 → utf-8 → gbk` 依次试解码；`cmd /c "..."` 被安全策略拦（老坑）。
- **🔥 MSVC/Ninja 偶发 `C1083 Permission denied` 但目标 obj 不存在 → 杀软瞬时锁，直接重试**。
  症状：`xxx.cpp : fatal error C1083: 无法打开编译器生成的文件: "...\xxx.cpp.obj": Permission denied`，
  构建停在中途，同批其余 obj 全部成功。**判据两步**：
  ① `tasklist` 确认没有同名 exe 残留（排除真进程占用）；
  ② `ls` 构建目录 —— **失败的那个 obj 根本不存在**（不是"有文件被锁"），即写入瞬间被拒。
  **修法：原样重试一次**（通常只补编那 1 个目标 + 链接即过）。**不要一上来 `-Clean` 或删
  build 目录** —— 本机 `-Clean` 本身还会因 fail-closed 安全包装把目录删残（见上一条）。

### 8.8 L3 真机联调（2026-09-29 续，四级验收最后一格）

**真机链路实测**（LRCP USB2.0 / 1280×720@30 / GStreamer 后端）：
`--list` 枚举到 `Camera 0: LRCP USB2.0` → `Negotiated capture format: 1280x720 @ 30fps`
→ `Status: STREAMING -> rtsp://127.0.0.1:8554/camera01` → `[HAND] ready` →
`conf=0.92~0.98 hand=right` / `infer=~20ms` / `[AI] fps=8.0` →
**真握拳 → `cmd=gripper action=close ok`**；增量跟手 `cmd=movexyz delta=[..] ok`。

**三条硬结论：**

1. **`lost_frames` 必须对着 AI 推理率算，不是相机帧率。** 手部支路 ~8Hz（一帧 ≈125ms），
   实机检测抖动的形态是 **1–2 帧（≈0.25s）** 短促漏检。默认 `3`（≈0.375s）与它**临界**
   ⇒ 20s 内误判 4 次「手离开」并强拉 HOME、打断跟随。提到 **6（≈0.75s）** 后同一场景
   重入降到 0–1 次。**相机 30fps 会让人把阈值算成 1/4**，这是这个坑最阴的地方。
2. **「持续被拒绝」是增量式的物理必然，不是 bug。** 手继续朝一个方向走而臂已抵边界，
   之后每步吃 `JOINT_LIMIT`，直到手回退。实测一轮 20s `rejected=83 / sent=12`；
   但 `update()` 仍返回 true、链路不断、臂不乱跳 ⇒ **保持 DEBUG 日志防刷屏**，
   README 七坑表第 4 条已改写为"常态"。要绕开就换 `--robot-map absolute`。
3. **`do_movexyz` 不消费 `last_good_mm_`。** 那个锁存只服务 `do_moveto`（绝对路线）——
   delta 下发的是位移不是位置，没有可比对的绝对量。早期在成功分支写它是一次**死状态**
   （已删）。另外 `movexyz` 成功日志原先在 `CA_LOG_DEBUG`，默认 `info` 起法**完全看不见
   跟手在动** ⇒ 必须提到 `CA_LOG_INFO`，否则真机调试像"没反应"。

**GStreamer 抓帧/探针两条坑：**
- `gst-launch -e` + `multifilesink` **会挂住**：`-e` 等 EOS，而 `mfvideosrc` **不产 EOS**。
  → 去掉 `-e`，用 `num-buffers=N` 自然结束。
- `--source` 必须是 **`mfvideosrc`**，不是 `dshowvideosrc`（这台走 MediaFoundation）。
  探针命令 `videoconvert` 到 RGB 抓帧会发青/发品红（色彩矩阵元数据丢失），
  但 **agent 自身管线走 NV12→H264，不受影响**，不要拿探针的偏色去怀疑管线。

**统计口径提醒**：`sent_` 只计被接受的命令，被 IK 拒绝的走 `rejected_`。
断言"有东西发生"写成 `sent增 + rejected增 == 1`，**不能只断 `sent > before`**。

### 8.9 软边界增益衰减（同日续，80/80）

**为什么**：§8.8 记的「边界持续拒绝」虽是物理必然，但体感差。用户选择做**衰减**而非换绝对模式
（绝对模式丢跟手手感）。

**怎么做**：`boundary_weight(pos_mm, axis)` 逐轴算权重，`gain * w` 后再发 `movexyz`。

| 配置 | 默认 | 作用 |
|---|---|---|
| `soft_boundary` | `true` | 总开关，**关掉即旧行为**（有回归测试钉死） |
| `soft_margin` | `0.15` | 从每端内缩比例 = 慢速区宽度 |
| `soft_min_gain` | `0.10` | 衰减下限 |

**四个非直观点（都是踩出来的）**：

1. **必须 smoothstep，不能线性** —— 线性衰减在进入边界那一刻**导数跳变**，手感是"突然变沉"。
   `smoothstep01` 两端导数为 0，进出都平滑。
2. **下限不能为 0** —— 为 0 时臂**无法被命令走出慢速区**（任何方向的步长都被缩进死区），
   只能靠手离开画面才复位。这是"防卡死"而非"留点力气"。
3. **必须逐轴独立**，不能全局降权 —— 臂常常贴着一面墙而另外两轴仍有空间，全局降权会
   为了修一个问题把自由轴也拖慢。
4. **衰减需要绝对位置** ⇒ delta 路线**重新开始维护 `last_good_mm_`**（从应答 `tcp` 读）。
   ⚠️ 注意这跟 §8.8 第 3 条说的"删掉死写入"**不矛盾**：那时写进去没人读，现在
   `boundary_weight()` 每帧都读。改这类"曾经的死代码"前先 grep 确认现在有没有消费者。
   ⚠️ **局限**：真机无位置反馈（坑 7），用的是**命令值**代理，sim 准确、真机近似。

**测试判据的坑**：我原以为 taper 会把步长压进死区从而 `skipped` 增长。
实际 `soft_min_gain(0.10) × 27.09mm ≈ 2.7mm > deadzone(0.5mm)` ⇒ **仍然发送**。
**正确判据是"下发步长被缩小"（13.54 → 1.35），不是 skipped 增长。**

### 8.10 「后端不可达」原本完全静默（🔥 已修，值得单独记）

真机跑 20s 后 `[ROBOT]` **只有 1 行** `control branch started`，没有任何失败提示，
让我误以为在用、实际一次没连上。根因：`connect_if_needed()` 的连接失败日志在
**`CA_LOG_DEBUG`**，默认 `info` 看不见；而 `caps/home/movexyz` 全部依赖连接成功，
所以**整条分支零输出** —— 与上一轮修的「`movexyz` 成功日志藏在 DEBUG」同一类问题。

修法：**首次失败 WARN 一次**（`connect_failed_warned_` 锁存防每 2s 刷屏），
**恢复时 INFO 并重新武装锁存**。测试 `robot_unreachable_backend_retries_and_recovers` 钉住
"重试受限 + 后端出现后自愈 + 自愈后真能发指令"。

**教训（可推广）**：**"分支启动了"和"分支在干活"是两件事**。任何"可选/降级"的旁路分支，
都必须有一条 info 级别的"我现在是死的"的明示，否则操作者无法区分"没动作"和"坏了"。

### 8.11 僵尸监听端口（🔥 新坑，通用性高）

`netstat -ano` 报 `:9100 LISTENING pid=24236`，但该进程**早已不存在**（`tasklist` 查无），
手工 `connect` 被**积极拒绝**（WinError 10061）。

→ **判断一个服务是否活着，不能只看 `netstat`**，必须**实际发一条协议命令看有没有应答**。
宿主回收进程树（§7.9 的机制）会留下这种"端口在、连不上"的残留。
本机还叠加了 **Python 文本读取的编码陷阱**：日志是 UTF-16，用 `splitlines()` 直接读
会把行切碎（表现为"6083 行里 0 条 `[ROBOT]`"）—— 二进制读 + 显式 `decode('utf-16')` 才对。


### 8.12 🔴 包围盒 ≠ 可达域（真机「手在动、臂不动」的根因，83/83）

**症状**：真机跑起来链路全绿（`cmd=movexyz ... ok` 一直刷、`ctest` 全过），
但**臂物理上纹丝不动**。

**根因（两层，缺一不可）**：增益与软边界**都以 `caps.workspace` 为基准**，
而 `caps.workspace` 是**几何可达包围盒，不是 IK 可解域**，虚大 1.6–3.5×。

三轴实测（**绝对 `moveto` 二分**，HOME 高度，tol 0.25mm）：

| 轴 | caps 包围盒 | 实测可达域 | span | HOME 负向余量 | HOME 正向余量 | 盒虚大 |
|---|---|---|---|---|---|---|
| X (front) | `[40.46, 175.88]` | **`[105.78, 144.78]`** | **39.0** | **9.25** | 29.75 | **3.5×** |
| Y (left) | `[-153.01, 153.01]` | `[-88.00, 88.00]` | 176.0 | 88 | 88（居中） | 1.7× |
| Z (up) | `[49.08, 114.68]` | **`[72.97, 114.47]`** | **41.5** | 36.25 | **5.25** | 1.6× |

**为什么必须用绝对法**：`moveto` 测的是"该点**本身**是否 IK 可解"（点的属性）；
`movexyz` 逐步只能测"沿**这条路径**能走多远"（受关节耦合干扰）。绝对法可二分到 0.25mm。

**故障链**：
1. 增益 `axis_scale(span) = span/0.5` ⇒ X 轴用盒宽 `135.43 → 270.85`。
   手 `delta.y=0.10` → 步长 **27.09mm**；而 HOME 处 -X 真实余量仅 **9.25mm**
   ⇒ **第一步必然越界**。
2. 软边界也用盒 ⇒ HOME"距边界 60mm" > margin，权重恒 `1.0`，**完全不衰减**。
   首帧被拒后 `learn_from_rejection` 把 `eff_min_[0]` 收到**当前位置** ⇒ 权重掉到下限
   `0.10` ⇒ 顶墙抑制把整条轴**锁死** ⇒ "臂一动都不动"。

> **教训（可推广）**：**衰减/校验机制本身没坏，是它的坐标系错了。**
> 用"API 声明的范围"当物理真值，是这类静默失效的通用形态 —— 尺子错了，曲线再精细也没用。

**修复三件套**：
1. **增益基准换实测 span**：`reach_span_x/y/z = 39.0/176.0/41.5`（**实测值，非 caps 推导**）。
   `axis_scale(float span)` 签名由 `(lo,hi)` 改成只收 `span`。
2. **被拒后对半细探** `soft_probe_on_reject` + `soft_probe_tries=2`：首步被拒 → 步长 ×0.5
   重试，救回一个子步。**条件必须同时含 `soft_boundary`**（否则关掉软边界时细探还在跑，
   破坏"回到旧行为"的逃生门 —— 这个 bug 被测试抓到了）。
3. **`effective_bounds()` 统一算边界 + margin**：学到的边界优先；否则以 `caps.home_tcp`
   为中心、`measured_span` 为宽的带；`reach_span_*=0` 时回退 caps 盒。
   `boundary_weight()` 与 `push_out_of_bound()` 共用它 ⇒ **慢速区宽度也按真实可达域算**。

**修前 vs 修后（LIVE 实测）**：

| | 修前 | 修后 |
|---|---|---|
| 首帧步长 (X) | 27.09mm | **7.80mm** |
| 顶墙结果 | `sent 2, rejected 1`，臂几乎不动 | **`sent 8, rejected 1, x travelled 29.25 mm`** |
| 真实余量 | 9.25mm（-X） | 29.75mm（+X），用满 **98%** |

**回归防线**：`robot_live_backend_drives_sim_arm` 加**位移断言**
`ASSERT(travelled > 10.0f)` —— **链路绿 ≠ 臂动了**。只数 sent/rejected 是测不出这个 bug 的。

**测试侧的连带坑**：
- `test_cfg()` 必须把 `reach_span_*` **显式设 0**（回退 caps 盒），否则测试继承生产 span，
  与假 caps 不匹配 → `robot_soft_boundary_tapers_gain_monotonically` FAIL。
- **`soft_margin` 默认是 0.15，不是 0.20**（记错就把测试取点算歪）。
- **细探生效后，第 1 帧 `sent` 就会增加**；第 2 帧起 `learn_from_rejection` 已把 `eff_max_`
  收到当前位置 → 顶墙抑制直接 skip（**正确行为**）。断言别写 `sent_after > sent_mid`。
- 假臂拒墙阈值要卡在**原步长与折半步长之间**（27.09 → 15.0 → 13.54）。
- `sscanf` 触发 MSVC **C4996** ⇒ 改 `std::stof` + `find_first_of`/`substr`。

**工具**：`carmera-agent/scripts/measure_reach.py`（粗扫 + 二分，`--tol` 默认 0.25mm，
直接打印可粘进 yaml 的 `reach_span_*` 片段）。
⚠️ **换臂 / 改机械结构后必须重跑它** —— 这三个数是这台臂的物理属性，不是协议属性。


### 8.13 「没有命令下发」= robot 分支从未启动（2026-09-29，静默型故障）

**症状**：Web UI 里 Hand 识别完全正常（`OPEN_PALM` conf 0.99、`REF LOCKED`、
`d=[-8.896,-96.158,2.857]` —— 数据都在），但 armppilot-backend 日志里
**一行 `[ROBOT]` 都没有**，臂当然不动。

**根因（双保险都失效）**：
1. `config/camera-agent.yaml` 的 **`robot.enable: false`**（生产默认，我上一轮做测试没改回来）
2. 仓库根 `start.bat` 的参数解析表**根本没有 `--robot*` 这一族**，
   `AGENT_ARGS` **从不拼接 `--robot`**（`start-joint.bat` 同样）

两条任一都能让 `start_robot()` 在第一行 `if (!cfg_.robot.enable) return;` 静默返回。
**连 WARN 都没有** —— 因为那条 WARN（`robot.enable is true but ai.enable is false`）
在**下一行**的分支里，前置条件恰好相反。这是最阴的一层。

> 🔑 **三类症状的判据（务必背下来）**
> | 日志表现 | 含义 |
> |---|---|
> | **连 `[ROBOT]` banner 都没有** | **分支从未启动**（配置关了 / 参数没传） |
> | 有 `control branch started`，之后没别的 | 启动了但连不上后端（看 WARN） |
> | 有 `connected` + `cmd=... ok` 但臂不动 | 命令发了但物理没动（→ §8.12 尺子问题） |
>
> "能看到分支输出" ≠ "分支在干活"，但**"看不到分支输出"首先怀疑它没启动**。

**修复**：
1. `camera-agent.yaml`：`robot.enable: true`（仍需 AI hand 模式；UI 切手势即开关）。
2. `start.bat` + `start-joint.bat`：加 `--robot`/`--no-robot`/`--robot-host`/`--robot-port`/
   `--robot-map` 解析，**默认 `ENABLE_ROBOT=1`**（与 `--ai` 同构），
   `--no-ai` 时自动一并关闭并打印 `[i] robot control OFF (--no-ai)`。
3. `StreamController::start_robot()`：未启用时打 **INFO**
   `[ROBOT] disabled (robot.enable=false, no --robot) - gesture control is off`。
   **"关掉了"必须有一条 INFO**，否则操作者无法区分"没开"和"坏了"（§8.10 同族）。

**验证手法（本例真正有效的）**：
- **用真实代码块干跑**：把 `start.bat` 的解析区（1..177 行）+ robot 块（真实行）拼成一个
  临时 .bat，`exit /b 0` 后 `echo` 出 `AGENT_ARGS`，4 组参数逐一核对拼装结果。
  这比读代码可靠，也能抓到延迟展开（`!VAR!`）的顺序错误。
  ⚠️ 临时 .bat **必须落在工作区内**（`/tmp` 在 Git Bash 与 Python 里不是同一个目录，见 §2）。
- **端到端必须一次调用内跑完**：把 backend + agent 的启停和日志抓取写进**一个**
  bash 脚本里执行。分两次调用的话，宿主会在工具调用结束时回收整棵进程树（§7.9），
  第二次调用看到的 backend 已经死了 —— 会误判成"连不上"。

**顺手修掉的历史债**：`start-joint.bat` 原来是**纯 LF**（391 行零 CRLF），
而它用了 `call :pick_exe` —— 按铁律 6 这是潜在的 `call :label` 假 FAIL。已整体转 CRLF。


### 8.14 降增益抑制手抖（2026-09-29，85/85）—— 手感参数怎么调 & 怎么钉住

**诉求**：能控了，但手/臂位移比例想大一些（非等比例），用降幅吸收手部波动。

**唯一正确的旋钮是 `gain_x/y/z`**（CLI `--robot-gain-*` > yaml > 默认，三级覆盖），
**不动 `motion_deadzone`** —— 那是抖动的硬下限，改了会把"故意的小动作"一起吃掉。

**公式**：`step_mm = hand_delta(归一化) × (reach_span_axis / 0.5) × gain_axis × soft_boundary`

**为什么三轴不能给同一个数**：`reach_span` 差 4.5 倍（Y=176mm vs X=39mm）。
统一 0.25 会让 Y 轴过钝。**三轴各自按 span 归一化后**，0.25/0.5/0.25 才是**相近的手感**。

| 轴 | span | gain | 手抖 0.02 | vs 0.5mm 门槛 | 故意动 0.10 |
|---|---|---|---|---|---|
| X | 39.0 | 0.25 | **0.39mm** | **被滤除** ✅ | 1.95mm |
| Y | 176.0 | 0.50 | 1.76mm | 通过 | 8.80mm |
| Z | 41.5 | 0.25 | **0.42mm** | **被滤除** ✅ | 2.08mm |

> 🔑 **"解决波动"的机制**：降 gain 后 X/Z 的抖动步长落到 `deadzone_mm=0.5` 之下，
> **被自然滤除**，不需要加滤波。代价是 X/Z 慢速小动作也不动。用户抱怨"迟钝"就上调该轴 gain。
> 这条算术本身值得记住：**降 gain 和滤波是两个可互相替代的手段，前者更便宜**。

**手感参数怎么钉住（这条是本轮的真正经验）**：
`gain_*` 属于**手感**，行为测试里必须 **pin 成 1.0**（否则降 gain 会让硬编码的步长断言
`ASSERT(seen > 26.0f)` 连带变红，测试从"验行为"退化成"验手感"）。但 pin 之后，
**改坏了手感值不会让任何测试变红** —— 所以需要一个**专门的 feel pin 测试**
（`robot_shipped_gains_are_the_documented_feel`）把 shipped 值 + 步长表写成算术断言，
外加一条 `config_robot_gains_load_from_yaml` 保证 yaml 与内置默认一致。
**"文档写一套、代码跑一套"只能靠这种 pin 防住。**

**验证结果**：sim 85/85 · gst 85/85 · LIVE `x travelled 29.25mm` · 验收 17/17 ·
gst 0 warning/0 error；端到端（真 backend + 真 agent 完整进程 + 生产 yaml +
`--ai-mode hand --robot`）拿到 `control branch started` → `connected` →
`caps protocol=2` → `cmd=home ok`。

**本轮新坑（三条都通用，不止这个项目）**：

1. 🔴 **`Start-Process -WindowStyle Hidden` 会让 armpilot-backend 活着但永不监听**。
   进程 `HasExited=False`、stdout/stderr 全空、15 秒 `Get-NetTCPConnection -LocalPort 9100`
   恒为 0；**去掉 `-WindowStyle Hidden`（默认新控制台窗口）后 t=1 就 `conn=1`**。
   以后起 backend 一律不加 Hidden。排查手法：`Get-NetTCPConnection` 轮询 + 写结果文件。
2. **Bash 工具不能跑 `.sh`** —— 会走 `wsl.exe` 被安全策略拦（本机 PATH 遮蔽同族）。
   端到端脚本只能走 PowerShell。
3. **PowerShell 工具吞 `Start-Process` 的子进程输出**（老坑，§2/§3.1 同族），
   但 **`& exe ... 2>&1 | Out-String` 是能拿到输出的** —— 要抓 agent 日志就用这个，
   别用 `Start-Process -RedirectStandardOutput`（会得到 0 字节文件）。

**另两条约束（顺带确认）**：
- `--source` 只吃**单个元素名**（直接拼进 pipeline），传整条 `filesrc ! decodebin ...` 会报
  `element not installed`；**文件源不是受支持路径**，无摄像头验证用 `videotestsrc`。
- 写 C++ 源码**不要用 bash heredoc**：`f << "robot:\n";` 里的 `\n` 会被 shell 处理掉变成
  `robot://n`（**第一行中招，后续行幸免** —— 这种"只坏一行"最难查）。用 Write/Edit 工具写。

