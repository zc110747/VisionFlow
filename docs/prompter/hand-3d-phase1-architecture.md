# VisionFlow Phase 1 — Hand 3D Gesture Recognition
## Current Architecture Analysis（提示词 §34 要求，改代码前必须先确认）

> 结论先行：**提示词要求的「Video Stream 与 Metadata 双链路独立」在当前代码里已经成立**。
> 手部识别应作为 **现有 AI Pipeline 内部的一个新 detector + 一个新后处理阶段**接入，
> 不新增服务、不新增第二套视频流、不新增第二套 metadata 协议。
> 唯一无法"复用"的硬依赖是 **手部关键点模型本身**（见 §7 决策点）。

---

## 1. Video Pipeline

| 项 | 实现 |
|---|---|
| 采集 → 编码 → 推流 | `carmera-agent/src/pipeline/video_pipeline_gst.cpp`（GStreamer） |
| 链路 | 摄像头 → `tee` → H264 编码 → `rtsp_publisher_gst.cpp` → MediaMTX `:8554` |
| 分叉方式 | `tee` 分出一条 **leaky queue → RGB appsink**，只在注册了 AI sink 时才插入；`ai.enable=false` 时 pipeline 描述与"AI 不存在"完全一致 |
| 帧交接 | `on_ai_sample()` 在 **GStreamer streaming thread** 上执行，只做 `memcpy` + `AIFrame` 入队；契约是「不阻塞、不抛异常」（`video_pipeline_gst.cpp:582`） |
| 反向依赖 | **无**。AI 线程慢/挂/崩都不会回压到采集与推流 |

**前端取流**：`web/src/webrtc/player.ts` → MediaMTX WHEP（`:8889`，由 server 代理）。
**服务端取流**：`internal/mediamtx/manager.go` 通过 MediaMTX control API（`:9997`）发现 `paths`。

---

## 2. Metadata Pipeline

| 项 | 实现 |
|---|---|
| 生产者 | `carmera-agent`：`AIPipeline` → `result_cb_` → `MetadataManager` → 有界队列 → 独立发送线程 |
| 编码 | `src/metadata/metadata_encoder.cpp` — **手写 JSON**（无第三方 JSON 库） |
| 传输 | `src/metadata/http_transport_winhttp.cpp` — WinHTTP `POST {server_url}`，指数退避重试 |
| 消费 | `video-server` `POST /api/metadata`（`internal/api/metadata.go:54`） |
| 判别 | 按 `type` 字段区分 `"frame"`（一次推理结果）/ `"status"`（AI 心跳）；老固件无 `type` 时由 `InferType()` 从字段形状兜底 |
| 持久化 | `internal/metadata/repo.go` — **只保存"每摄像头最新一帧"**：`ai_frame` + `ai_object`（每帧 delete+insert 整组替换）+ `ai_status` |
| 时间基准 | `frame_id` = 摄像头帧计数（`video_pipeline_gst.cpp` 的 `++ai_frame_id_`）；`timestamp` = GStreamer PTS（ns→ms）。**两者一律逐字复制，服务端绝不重编号**（`model.go:12` 硬规则） |
| 读取 | `GET /api/cameras/{id}/metadata` → `Snapshot{camera_id, frame?, status?}` |

**现有 wire 格式（`type:"frame"`）**：

```json
{"version":1,"type":"frame","camera_id":"camera01",
 "frame_id":1234,"timestamp":45678,
 "video_width":1280,"video_height":720,
 "ai_mode":"ai-y-pose",
 "objects":[{"class":"person","confidence":0.91,"track_id":3,
             "bbox":[100,200,400,700],"keypoints":[[x,y,conf],...]}]}
```
**现有 status 格式**：`{"ai":{"enable","running","fps","model","tracker","last_frame_id","last_timestamp","processed","keypoints"}}`

**当前没有的**：没有历史/录制（latest-only），没有 hand 相关字段，没有 AI 之外的时间戳。

---

## 3. AI Pipeline

| 项 | 实现 |
|---|---|
| 位置 | `carmera-agent/src/ai/ai_pipeline.cpp`，跑在**自己的线程**上 |
| 抽象 | `IDetector`（`include/camera_agent/ai/detector.h`）：`init` / `detect(rgb,w,h,out)` / `backend_name()` / `keypoint_count()` |
| 实现 | 唯一实现 `OnnxYoloDetector`（`src/ai/yolo_detector.cpp`），**刻意不用 OpenCV**：letterbox + NMS + YOLOv8/v11 head 解码全部自己写，便于后续搬 RK3568 |
| 关键能力 | `keypoint_count_from_channels(C)` → `(C-5)/3`，即**已经支持任意 K 点单类 pose head**（当前 K=17） |
| 追踪 | `ITracker` / `bytectrack`（`src/ai/byte_tracker.cpp`） |
| 采样 | 源 fps < `full_rate_below_fps`(10) 则逐帧；否则按 `ai.fps`(5–12) 抽样 |
| 结果 | `AIFrameResult{frame_id, timestamp, video_width, video_height, ai_mode, objects[]}` |
| 运行时切模型 | `AIMode{Detect, Pose}` → `request_mode()` → AI 线程 `apply_mode()` 重建 detector（**在锁外加载**，避免阻塞 `push_frame`） |
| 模式来源 | web UI `POST /api/cameras/{id}/aimode` → server 存 DB → agent `aimode_poll_loop()` 每 2s 轮询 → 切模型。**单向下行，无推送通道** |
| 模式回传 | 每帧戳 `ai_mode`（agent **实际**跑的模式），前端丢弃与所选模式不符的过渡帧，避免切模型瞬间画出旧框 |

**模型资产**：`models/yolo11n.onnx`（人检测，COCO class 0）、`models/yolo11n-pose.onnx`（人 + 17 COCO 关键点），**均已入库**。ONNX Runtime 位于 `D:/Software/onnxruntime`，CMake 自动探测（`HAVE_ORT`）。**无任何手部模型。**

---

## 4. Frontend Video

- `web/src/webrtc/player.ts` → `<video>`，**WebRTC only**（无 HLS 兜底，HLS 只留给未来回放）。
- 视频与 overlay 完全解耦：overlay 是叠在 `<video>` 上的一张 **独立 `<canvas>`**（`pointer-events:none`）。

## 5. Frontend Metadata

- `VideoPlayer.vue`：`setInterval(pollMetadata, 200)` → `GET /api/cameras/{id}/metadata` → 画框/骨架到 canvas。
- **overlay 只消费 metadata，绝不重跑 AI**（提示词 §11/§26 的要求当前已满足）。
- 缩放：metadata 的 bbox 是**原始视频像素**，前端按 `video.clientWidth / video.videoWidth` 映射到显示尺寸。
- 模式门控：`if (!frameMode || frameMode !== aiMode) { clearOverlay(); return }` —— `ai-off` 也走同一规则（没有帧会戳 `ai-off`）。
- 现有 mode 按钮：`AI 关 / 人检测 / 姿态`。

---

## 6. Recommended Hand AI Integration Point

```
camera ──tee──┬──► video pipeline ──────────────► RTSP ──► MediaMTX ──► 浏览器 <video>   （完全不动）
              │
              └──► AIPipeline（同一个线程、同一个队列、同一个 MetadataManager）
                        │
                        ├─ AIMode::Detect → OnnxYoloDetector(yolo11n)
                        ├─ AIMode::Pose   → OnnxYoloDetector(yolo11n-pose)
                        └─ AIMode::Hand   → HandDetector(新增) ──► HandAnalyzer(新增) ──► HandState
                                                                                              │
                                                          AIFrameResult.hand ──┐              │
                                                          MetadataEncoder（additive "hand" 字段）
                                                                              ▼
                                                        POST /api/metadata → ai_frame.hand
                                                                              ▼
                                                   GET .../metadata → {frame{...,hand}} → HandOverlay + HandPanel
```

**逐条对应提示词约束：**

| 提示词 | 落点 |
|---|---|
| §4 Video Stream 保持独立 | video pipeline 一行不改；不往视频帧里塞任何手势数据 |
| §5/§26 手势属于 AI/Metadata 链路、复用而非新建 | 新增 `HandDetector : IDetector` + `HandAnalyzer`，插在**现有** `AIPipeline` 内 |
| §7 扩展现有 schema，不建新协议 | 同一个 `POST /api/metadata`，`type:"frame"` 上**增量加** `"hand":{...}` |
| §8 时间同步复用 | 直接复用 `frame_id` + `timestamp`，不建第二套时间基准 |
| §9 metadata 不带视频帧 | 只传结构化数值 |
| §10 沿用现有 object 结构 | `objects[]` 里多一条 `class:"hand"`（含 bbox/confidence/21 关键点），派生状态放 `hand` |
| §13 优先复用现有 YOLO 能力 | 复用 `IDetector` 抽象、letterbox/bilinear 原语、有界队列、切模式机制 |
| §27 扩展现有 Recorder | 现有"录制"就是 metadata 存储（latest-only）→ 在其上加 **append-only `ai_hand_log`** + JSON 导出 |
| §30/§31 不含机器人 | 到 `Frontend` 结束；`hand` 结构里不出现任何 robot/IK/servo 概念 |

---

## 7. 决策点（必须用户确认后才能动手）

### 7.1 硬依赖：仓库内**没有** 21 点手部关键点模型

现有两个模型都做不了手：
- `yolo11n.onnx` → 人体框，无关键点
- `yolo11n-pose.onnx` → **人体 17 点 COCO**骨架，不是手

而提示词 §10/§14 明确要求 **21 点手部 landmarks**，§15/§16/§19/§21 还要求可验证的 Z（深度）。

### 7.2 已实测验证的候选方案（本轮已下载并探测，非推测）

**方案 A（推荐）—— MediaPipe Hand Landmark 两级级联**，来源 `opencv/opencv_zoo`，**Apache-2.0**：

| 模型 | 输入（实测） | 输出（实测） |
|---|---|---|
| `palm_detection_mediapipe_2023feb.onnx` (3.9 MB) | NHWC `(1,192,192,3)` f32 RGB /255，**黑色 (0,0,0) 居中 letterbox** | `(1,2016,18)` 回归 + `(1,2016,1)` 分类 logits（需 sigmoid）<br>2016 anchor = 24×24 网格×2 + 12×12 网格×6（与官方 `mp_palmdet.py` 硬编码表一致） |
| `handpose_estimation_mediapipe_2023feb.onnx` (4.1 MB) | NHWC `(1,224,224,3)` f32 RGB /255 | `Identity(1,63)` = 21×(x,y,z) + `Identity_1(1,1)` conf + `Identity_2(1,1)` handedness + `Identity_3(1,63)` world landmarks |

- **真 3D**：`z` 相对手腕（WRIST，index 0），与 x/y 同尺度（官方代码 `landmarks[:,2] *= max(scale_factor)`），不是厘米。
- 含 **left/right handedness**，对手势判定有用。
- 代价：需要在 C++ 里实现「SSD anchor 解码 + NMS + 按掌心关键点算旋转角 + 仿射裁切 224 + 逆变换回原图」。**这些原语（双线性重采样、NMS、letterbox）在 `yolo_detector.cpp` 已有可复用实现**，且不引入 OpenCV。

**方案 B —— 单级 YOLO-pose 手部模型（21 点）**
- 优点：`decode_yolo_output` 的 `(C-5)/3` 逻辑**天然支持 K=21**，改动最小。
- 缺点：① 需要另找一个可信的单级 21 点手模型（第三方来源不可控）；② YOLO-pose head **不输出 z**，"前后"只能靠 bbox 尺寸反推。

### 7.3 Z 的语义（§15/§16 要求实测确定，不能假设）

即使采用方案 A，也必须区分两个不同的"z"，否则 §21 的 `FORWARD/BACKWARD` 会语义错误：

| 量 | 含义 | 用途 |
|---|---|---|
| MediaPipe `landmark.z` | **手套内相对手腕**的深度 → 描述"手指朝向相机/背向相机" | 手势几何判定辅助 |
| 手的**表观尺寸**（掌心跨度 / bbox 对角线，像素） | **手整体离相机的远近** → 单调、稳健 | logical 轴 Y = FORWARD / BACKWARD |

即：`position.z` 的取值来源设计为 **配置项** `hand_depth_source = size | landmark_z`（默认 `size`），
两者都按 §16 用真机做方向实验后写进文档，**不写"z = 厘米"这类假设**。

### 7.4 需要用户拍板的两件事（**已拍板**）

1. **模型路线** → **方案 A（MediaPipe 两级级联）**。
2. **模型是否入库** → **入库**，两个 ONNX + Apache-2.0 LICENSE 一并提交到 `carmera-agent/models/`，
   SHA-256 与输入输出形状记入 `carmera-agent/models/README.md`。

---

## 8. 拟定的阶段划分与提交点

| 阶段 | 内容 | 验收 | 状态 |
|---|---|---|---|
| **P1a** | 校准 anchor 表 + 2 个 ONNX 入库 + 黄金夹具 | 全 2016 值比对 + 模型 SHA-256 入档 | ✅ 完成并提交 |
| **P1b** | `HandDetector`：palm → 旋转裁切 → landmark，含自实现重采样/仿射/解码 | 黄金比对 **dx 0.00034 px** | ✅ 完成并提交 |
| **P1c** | `HandAnalyzer`：坐标映射 / EMA / reference / deadzone / motion / gesture 去抖 | 13 个无模型单测全绿 | ✅ 完成并提交 |
| **P1d** | 接线：`AIMode::Hand` + `hand` metadata 编码 + config/CLI | 双构零警告 | ✅ 完成并提交 |
| **P1e** | P1 验收：真机 720p@30 出 `ai-hand` 帧 + 定 `kCurledRatio` | 全链路真机跑通（需举手配合） | ✅ 完成（见 §11） |
| **P2** | server：schema 增量列 + `hand` 解码/校验 + append-only `ai_hand_log` 录制器 + `hand_session_*.json` 导出 + `ai-hand` 放行 | `go test ./...` 全绿 + 真机 metadata 落库 | ✅ 实现完成（见 §13） |
| **P3** | 前端：21 点手部骨架 overlay + HUD + `HandPanel` 调试面板 + `ai-hand` 按钮（只消费 metadata） | 构建零错误 + 无头渲染断言 | 待开始 |
| **P4** | 端到端：`verify_hand.py`（PASS/FAIL 计数）+ §16 坐标方向实验记录 + README | 全链路真机跑通 | 待开始 |

---

## 9. 实证记录：核心解码链路的黄金基准比对（P1a–P1c 完成的判据）

提示词 §34 要求"确认现有架构后再改代码"，§15/§16 要求"实测不假设"。本节记录**已经跑出来的原始数据**，
是所有"z 不是厘米""阈值 1.15 有出处"这类结论的唯一依据。

### 9.1 基准的构造

参考实现（官方 `mp_palmdet.py` / `mp_handpose.py`）把 `cv.dnn` 换成 **ONNX Runtime**（与 C++ 同一个
推理后端），在 `tests/golden/woman_hands.png` 上跑一遍，逐值记录。这样 C++ 与基准之间的差异**只可能**
来自我自己的移植，不掺杂 `cv.dnn` 与 ORT 的差异。

夹具必须是**无损格式**：实测 stb_image 与 libjpeg-turbo 解码同一张 JPEG，584177/614400 像素一致、
最大差 3 LSB、16919 个像素差 >1。输入像素都不是同一份，残差会到 ~0.43 px，足以掩盖真实的亚像素级
变换 bug。因此夹具用 PNG（两侧解码逐字节一致，实测 max diff = 0）。

### 9.2 实测结果

| 量 | 与官方参考的最大差 |
|---|---|
| 掌心检测选择 | anchor **idx=456**，score **0.845936**（与参考逐位相同） |
| 21 点 x / y / z | **0.00034 / 0.00079 / 0.00173 px** |
| 手部 bbox | **0.00065 px** |
| landmark 置信度 | 0.990193 vs 0.990192 |

即核心解码链路**亚毫像素级一致**，不是"看起来差不多"。测试容差设为 0.02 px（比实测宽 2 个数量级，
以免换一个 ORT 构建就 flaky，同时仍远低于任何真实变换 bug 的量级）。

### 9.3 两个被实测钉死的语义（§15/§16）

| 结论 | 数据 | 影响的设计 |
|---|---|---|
| `landmark.z` 是**手腕相对值**，手腕 z 恒为 0，与手离相机多远无关 | 手腕 z = `+1.83e-06`（手占画面 1/3 大小时） | `HandConfig::depth_source` 默认 `"size"` 而非 `"landmark_z"` |
| 真实张开手的四指 tip/MCP 比值为 **1.59–1.84** | 右手 1.5927/1.7069/1.6751/1.5901；左手 1.7354/1.8355/1.7877/1.6845 | `kExtendedRatio = 1.15` 距真实数据很远，安全 |

**唯一还缺实测的数字**：`kCurledRatio = 0.90`（握拳侧阈值）。夹具里没有握拳样本，只能靠真机补一张
fist 图 —— 这是本阶段显式标注的未验证项，不是遗漏。

### 9.4 踩到的坑（供后续复用）

1. **官方 `_preprocess` 的 `pad_bias` 在 `if` 分支内赋值**。基准脚本最初漏抄这两行，导致掌框整体 x 偏移
   +160px —— C++ 是对的，基准是错的。定位花了整轮时间，代价是两次错误的"网络选错了手"推断。
2. **`cv.dnn.NMSBoxes` 把 `(x1,y1,x2,y2)` 当 `(x,y,w,h)` 用**。C++ 侧刻意改用真 IoU（掌心几乎不重叠，
   本夹具上无差异），差异由合成单测 `hand_palm_nms_suppresses_overlaps` 覆盖。
3. **`palm_anchor_count` 的表结构不是"4 层 × 2"**，而是 `stride 8 × 2 锚/格` + `stride 16 × 6 锚/格`。
   逐值核对到 1e-6（残差 3e-8 = float32 字面量舍入）才敢定稿。
4. **JPEG 解码器不是算法的一部分，但不一致**。实测 stb_image 与 libjpeg-turbo 解码同一张 JPEG：
   584177/614400 像素一致、最大差 3 LSB、16919 像素差 >1。用它做夹具时残差会到 **0.43 px**，
   比真实移植误差大 1000 倍，恰好掩盖本测试要抓的亚像素 bug。**夹具必须无损**：改用 PNG 后
   两侧逐字节一致（max diff = 0），残差立刻降到 0.0017 px 量级。
5. **`rc.exe` / `mt.exe` 不在手工 PATH 上**。Debug 目录首次配置会因 `CMAKE_RC_COMPILER` 找不到而
   判定 "compiler broken"，实际与代码无关 —— 需要把
   `C:\Program Files (x86)\Windows Kits\10\bin\<ver>\x64` 加进 PATH。

---

## 10. P1d 接线决策记录（后续 P2/P3 必须按此实现）

### 10.1 `hand` 字段为什么是 optional，而不是"总在的结构体"

`AIFrameResult::hand` 是 `std::optional<HandState>`，只有 Hand 模式才 engage。
编码器因此能在**不判断模式**的前提下保证：detect / pose 帧的 JSON 与手部功能出现之前**逐字节相同**。

这不是洁癖 —— `metadata_encoder.cpp` 是两条链路共用的唯一序列化点，一旦它按 `ai_mode` 分支，
就多了一处"模式字符串写错导致报文变形"的可能。单测用**固定整串**钉住了两个方向的输出：

| 用例 | 断言 |
|---|---|
| `hand_absent_on_detect_frame` | 整帧 JSON 等于字面量，且不含 `hand` |
| `hand_absent_on_pose_frame` | 同上 + `keypoints` 仍是 `[x,y,conf]` 且仍是对象最后一个字段 |
| `hand_block_is_appended_after_objects` | hand 帧整串等于字面量（含 `hand` 块的完整字段顺序） |

### 10.2 关键点为什么放两处

| 位置 | 内容 | 精度 | 用途 |
|---|---|---|---|
| `objects[0].keypoints` | `[x,y,conf]` | x/y 被 `lround` 取整 | 复用通用绘制路径（canvas 按像素画） |
| `hand.landmarks` | `[x,y,z]` | 2 位小数，像素尺度 | 唯一带 z 的地方；也是 `emit_landmarks=false` 时一起消失的那份 |

共享的 `keypoints` 三元组是 **pose 的契约**，给它加第 4 位会让每个消费方都变成"变长数组"解析；
而第三维只对手有意义，所以放在 hand 块里。代价是 x/y 冗余了一份（12fps × ~500B 可忽略）。

### 10.3 `install_mode` / `apply_mode` 拆开，以及"启动自动识别"为何只留在 `init()`

- `install_mode(m)`：只做"加载 + 原子换指针"，失败时**上一个模型原封不动**（因为换指针严格发生在加载成功之后）。
- `apply_mode(m)`：`install_mode` + 打一条 "model switched" 日志。启动时不该出现"switched"（那时还没切过任何东西）。
- 旧的"`--ai-model` 指向 pose ONNX 就自动报 Pose"逻辑**只保留在 `init()`**：如果把它搬进 `apply_mode`，
  运行期 Web 点"人检测"而 `ai.model` 恰好是 pose 文件时，agent 会回一个 `ai-y-pose`，
  前端按模式门控会**丢弃所有帧**——表现成"AI 坏了"。运行期的显式请求必须被逐字执行。
- 两个模式**互斥持有模型**：切到 Hand 就释放 YOLO session（反之亦然）。失败路径不释放，所以"切换失败后没有模型"不可能发生。

### 10.4 模式 token 只有一个解析点

`aimode_from_string()`（`ai_types.h`）同时服务 YAML、CLI、aimode 轮询器。它**故意拒绝** `ai-off`
与任何未知 token：

- `ai-off` 的语义是"Web 停止绘制"，不是"换一个模型"；把它翻译成模型切换会加载没人要的模型。
- 未知 token（例如 video-server 加了新模式而 agent 还没升级）必须**保持现状**，不能猜一个最接近的。

CLI 与 YAML 的区别只在错误处理：CLI 打错是 `exit 2`（操作者刚敲的），YAML 打错是 WARN + 保持默认
（配置文件里的一个手误不该让整次运行失败）。`depth_source` 同样按闭集校验，因为它一旦落到
mapper 的默认分支，"方向实验"就会在操作者以为测了另一个轴的情况下给出结论。

---

## 11. P1e 实证记录：真机验收与 `kCurledRatio` 标定（2026-09-23）

### 11.1 真机链路通过（P1e 第一半）

`camera-agent --camera 0 --width 1280 --height 720 --fps 30 --ai --ai-mode hand`
（真实 LRCP USB2.0，`nvh264enc` 硬编）：

```
[HAND] ready: palm=...2023feb.onnx (2016 anchors) landmark=...2023feb.onnx (21 keypoints)
[AI]   ready: backend=onnxruntime(mediapipe-hand) mode=hand(21kpt)
Negotiated capture format: 1280x720 @ 30fps
[AI] mode=hand fps=5.0 infer=11.4~28.3ms     [METADATA] sent=72 failed=0 reconnect=0
```

⇒ 视频流与 metadata 双链路在真机上独立跑通，`ai_mode:"ai-hand"` 报文零失败送达。

### 11.2 踩到的两个坑（**都不是代码问题**，但都伪装成代码问题）

**① 摄像头枚举骗人。** `--list` 列出**两条** `LRCP USB2.0`，其实是**同一台物理设备被枚举两次**
（DirectShow 只认到一个：`VID_0C45&PID_6366`）。`--camera 1` 起流即
`gst_base_src_start / Failed to start` → `camera busy / unavailable?`。
**本机只有 index 0 可用**，双摄并行这条路不通。判据：**`--list` 能列出 ≠ 能打开，必须实跑一次。**

**② 镜头被遮挡，且症状是"又暗又糊"而不是"全黑"。** 遮挡期间连跑 5 分钟 **1498 帧、0 次检出**。
我没有把它归因于"操作者没摆姿势"，而是去量化画面：

| 指标 | 遮挡时 | 解除后对照（黄金夹具） | 比值 |
|---|---|---|---|
| 平均亮度 YAVG | 62.4 / 255 | 85.7 | 0.73× |
| 边缘能量（`edgedetect` 后 YAVG） | **0.769** | 4.287 | **0.18×** |

**取证手法（可复用）**：agent 推流时**直接从 RTSP 拉帧**
（`ffmpeg -rtsp_transport tcp -i rtsp://127.0.0.1:8554/camera01 -frames:v 1 x.png`）。
好处是①**零摄像头争用**（本机只有一个物理设备，另开一次采集会把 agent 顶掉）；
②拿到的正是 tee 分支上游、**AI 分支同一路**的画面，验证力强于另开一次采集。
`edgedetect` 的边缘能量比"暗不暗"可靠得多 —— 遮挡的真实症状是**没有锐利边缘**。

### 11.3 `kCurledRatio`：0.90 → 1.10（**本轮唯一的行为改动**）

**为什么旧值必错。** 0.90 是 P1c 时**没有握拳样本**情况下的占位值（代码注释当时就写明
"still needs the live experiment"）。实测两段独立稳态：

| 手指 | 稳态**开掌**（55 帧 / 10.8 s） | 稳态**握拳**（14 帧） |
|---|---|---|
| index | 1.587 – 1.782 | **1.222 – 1.398** |
| middle | 1.621 – 1.875 | **0.850 – 0.983** ← 冲到 0.983 |
| ring | 1.516 – 1.734 | 0.731 – 0.871 |
| pinky | 1.429 – 1.534 | 0.722 – 0.963 |

旧阈值下，真握拳的四指投票是 `extended=1 / curled≤2`，而 `kGestureVote=0.75` 需要 3 指 —— 
**14 帧真握拳里 11 帧输出 `UNKNOWN`**（`gesture_candidate` 已经是 `FIST`，但票数不够）。

**新值怎么定。** 最差手指上的簇间距是 **`[0.983, 1.429]`**，阈值落在其间即可分开两种姿势。
1.10 比握拳簇高 **0.117**、比开掌簇低 **0.329**；且**必须**仍在 `kExtendedRatio = 1.15` 之下
—— `hand_classify_gesture()` 是 `if / else if`，**死区就是 `[kCurledRatio, kExtendedRatio]`**，
两者一旦交叉，死区会**静默失效**而不是报错。

**几何限制（不是标定问题，Phase 1 不修）**：**index 指是四指里最不可靠的一个，
也是唯一跨越两个阈值的指头**。`MCP5` 是四指里离腕点最近的，所以同样的卷曲程度下
index 比值天生偏高。**同一只手的两次握拳实测**：

| 手指 | 松拳（14 帧） | 紧拳（10 帧） |
|---|---|---|
| **index** | **1.222 – 1.398**（> 1.15 ⇒ 投 extended） | **0.614 – 1.971**（中位 0.829 ⇒ 多数投 curled） |
| middle | 0.850 – 0.983 | 0.464 – 3.720（中位 0.679） |
| ring | 0.731 – 0.871 | 0.339 – 3.048（中位 0.551） |
| pinky | 0.722 – 0.963 | 0.376 – 2.154（中位 0.444） |

⇒ **不能断言 index"永远不投 curled"** —— 它按握拳的松紧两头都投得出来。
但 middle/ring/pinky 在**两次**握拳里都 ≤ **0.983**，是可靠的三指。
**FIST 实际只靠这三指，且恰好卡在 `kGestureVote=0.75` 的等号上**；这三指若有一指被遮挡，
手势就会丢。根治要按手指归一化（或用 PIP/DIP 关节），属分类器改动，刻意留到后续阶段。

### 11.3b 新阈值的真机在线复验（**通过**）

用改了阈值的二进制重跑 `--ai-mode hand`，同一只手再握一次：

```
33 帧检出，其中 27 帧 gesture = FIST     ← 旧阈值下这些全部是 UNKNOWN
gesture_candidate: FIST 23 / OPEN_PALM 8
连续 FIST 段（#660–678）中位比值 0.464 – 0.833，confirmed 稳定锁存 FIST
```

⇒ 阈值改动在**真实在线链路**（含 5 帧去抖）上生效，不只是离线反算。
另一种紧握拳也把四种手指都压到 **0.339 – 0.983**，距 1.10 仍有 ≥ 0.117 余量。

### 11.4 为什么用 landmark 表而不是图片夹具

真机帧里**有操作者的脸和办公环境**，把摄像头截图提交进仓库是隐私问题。而
`hand.landmarks` 是**图像像素坐标、2 位小数**，精度远高于要标定的 ratio，
**报文本身就够做标定**。所以证据以 landmark 表的形式冻结在
`tests/hand_analyzer_tests.cpp :: hand_gesture_real_capture_open_and_fist`，
逐位复现本轮实测输入。离线复算脚本（**已提升为仓库内工具**）：
`carmera-agent/scripts/analyze_hand_landmarks.py`
（按 `kTips={8,12,16,20}` / `kMcps={5,9,13,17}` 逐字复刻 C++ 公式，输出四指分位分布 +
直方图 + Otsu 阈值建议）。

**⚠️ 脚本自身的坑（已修）**：Otsu 拿"每帧四指中位数"做二分类时，**过渡帧**（手出入画、
运动模糊）在构造上就落在谷底且数量占优，会把阈值**往上拉**——第一版因此给出 1.3546，
甚至**越过了 `kExtendedRatio`**。现在脚本会把它当上界并强制钳到 `kExtendedRatio - 0.05`。
标定姿势必须**保持不动**，否则测的是过渡而不是姿势。

### 11.5 仍未验证

- `depth_source` 的方向（哪个符号是"靠近"）与真实 UVC 的 `invert_*` 取值
  —— 注意：这只是**符号选取**问题，不是 12.5 那种"轴对调"级别的语义错误。
- ~~画面疑似整体旋转 90°~~ → **已解决**：确认为滚转 90°，用户**物理转回本体**后实测正立，
  零代码改动。取证、两个方案的取舍与排除项见 §12。

---

## 12. 画面朝向实证：摄像头滚转 90°（2026-09-23 定案）

### 12.1 结论

**采集画面被滚转 90°，修正方向 = 顺时针 90°**
（GStreamer：`videoflip video-direction=90r`；ffmpeg：`transpose=1`）。

原始帧里**真实世界的"上"指向画面左侧**。顺时针旋转后画面正立：
天花板灯板与格栅在上、人发际在上且眼镜水平、笔记本边框在下（已现场验证，见 §12.4）。

### 12.2 为什么不能靠"看着像"定案

第一遍肉眼判读给出了**互相矛盾**的两个线索：天花板灯板/格栅在画面**左侧**（⇒ 上是左），
但人脸的朝向又像"上是右"。原因是 raw 帧里人脸只有约 150 px 高、而且整体侧躺，
那个尺度上肉眼判"上下"不可靠 —— 我第一遍就读错了。

于是改用两条**客观**线索：

1. **放大裁切人脸**（`crop=700:460:350:120` 后再 2× 最近邻）→ 发际在**左**、下巴在**右**
   ⇒ 上是左，与灯板线索一致；
2. **两种旋转都做出来复查**：顺时针 90° 得正立人脸；逆时针 90° 人头朝下 ⇒ 排除。

> 教训：涉及坐标系的结论，**不要用"看着对"作判据**，要把候选解都渲染出来做排除。

### 12.3 关键排除项：DirectShow ≠ Media Foundation

`ffmpeg -f dshow` 与 agent 实际使用的 `mfvideosrc` 是**两条不同的采集路径**，
MF 会应用 UVC 的 roll 属性而 DirectShow 不一定。如果只抓 dshow 就下结论，
有可能测的是"另一条路径的行为"。所以取证必须用 **agent 同款源**：

```bash
gst-launch-1.0 -q mfvideosrc device-index=0 num-buffers=40 \
  ! queue ! videoconvert ! video/x-raw,width=1280,height=720,framerate=30/1 \
  ! videoconvert ! pngenc ! multifilesink location=frame_%02d.png
```

两条路径结论一致（都侧躺）⇒ 不是 DirectShow 独有现象，是**传感器/安装**的朝向问题。

### 12.4 修法已现场验证

`videoflip` 存在于本机 GStreamer（`videofilter` 插件），且**首选属性是 `video-direction`**，
`method` 已标记 deprecated：

| 属性 | 值 | 含义 |
|---|---|---|
| `video-direction` | `90r` | 顺时针旋转 90°（**本项目要用这个**） |
| `video-direction` | `90l` | 逆时针 90°（实测人头朝下，排除） |
| `method`（deprecated） | `clockwise` | 等价于 `90r` |

实测管线与输出：

```
mfvideosrc device-index=0 ! queue ! videoconvert
  ! video/x-raw,width=1280,height=720,framerate=30/1
  ! videoflip name=orient video-direction=90r
  ! videoconvert ! pngenc ! multifilesink      ⇒ 输出 720x1280，画面正立 ✅
```

> ⚠️ **位置约束**：`videoflip` 必须放在 **caps 之后、`tee` 之前**。
> 放在 `tee` 之后只有 AI 分支被转、视频流不转，前端 overlay 会与视频错位；
> 放在 caps 之前会让 caps 过滤器（1280×720）与翻转输出（720×1280）冲突。

### 12.5 影响面（**选方案 B 后已全部消解**）

| 项 | 若走方案 A（软件旋转）会怎样 | 实际（方案 B 物理转回） |
|---|---|---|
| §17 logical 坐标系 | `hand_map_coordinates()` **硬假设画面正立**（`x=cx/fw` → X+ 右；`z=(fh-cy)/fh` → Z+ 上），只有 `invert_x/y/z` 三个**符号翻转**。90° 滚转是**轴对调**，符号翻转折不出来 ⇒ 侧躺时 hand 的 logical X 实际测的是**竖直**方向 | **前提恢复成立**：画面已正立，X/Z 的语义回到设计意图。`invert_*` 的**取值**仍需按 §16 实验定，但不再是"轴对调"级别的错误 |
| P3 overlay | `landmarks` 是图像像素坐标，叠在侧躺视频上仍能对齐，但骨架朝向与用户直觉相反 | **无需任何旋转补偿**，像素坐标直接可用 |
| 视频流本身 | 输出变 720×1280 竖幅，`video_width/height` 报文口径要跟着改，`videoflip` 须卡在 caps 之后 / `tee` 之前 | **编码分辨率仍是 1280×720 横幅**，报文口径、`--probe` 读摄像头源 pad 的口径、MediaMTX 协商全部不变 |
| 代码改动 | 首次给视频管线引入 flip 元素 + 新配置项 + 探针口径修正 | **零代码改动** |

### 12.6 修法定案：方案 B 物理转回（**已实测通过，2026-09-23**）

用户把摄像头本体转 90° 装回后，用**与 agent 同款源**（`mfvideosrc device-index=0`，
1280×720@30）重抓判读：

- 画面**完全正立**：天花板/灯板在上、人脸直立（发际在上、眼镜水平、下巴在下）、
  置物架在左，1280×720 横幅。
- §12.6 表里那条风险（**该模组是否传感器在 PCB 上就已转 90°**，若是则转本体后依旧侧躺）
  **没有发生**：转本体确实修正了画面。

| 方案 | 决定 | 备注 |
|---|---|---|
| **A 软件旋转** | **不采用**（保留为已现场验证的备选） | 元素/属性已实测：`videoflip video-direction=90r`（顺时针，输出 720×1280）。若以后换机位又遇到同类问题（RK3568 侧同理），这是现成的修法，届时再看要不要做成配置项 |
| **B 物理转回** | ✅ **采用** | 已验证：正立、零代码、无坐标口径变更 |

> 之所以先验证再定案：方案 B 的风险是"看着像机械问题、实际是传感器朝向问题"，
> 一旦如此，转本体等于白干。实测排除了这一条。

### 12.7 取证纪律（本轮修正的一条方法论）

**"抓一帧看看"必须交代清楚是哪条采集路径抓的。** 本轮先用了 DirectShow，
差点把一条路径的行为当成设备的行为。凡涉及"设备到底输出什么"，
判据必须来自**与产品同款的那条链路**（这里是 `mfvideosrc`），
或至少两条路径互相印证。

第二条：**朝向这类整体几何结论，不要靠单一线索的肉眼判读。** 本轮我先按
"灯板在左 ⇒ 上是左"判了一次，又按"脸的朝向"判了一次，两次**互相矛盾**；
最后是把人像区域裁出来放大、以**文字/眼镜/发际线**这类客观线索才定死的。
判读方向时，先找一个"上下不对称且无歧义"的目标（文字、人脸、书本），
不要用"亮斑在哪"这种只能说明光源位置的线索。

---

## 13. P2 实证记录：服务端 hand 落库与导出（2026-09-23）

### 13.1 验收数字（原始）

```
gofmt -l internal/ cmd/      → 无输出（干净）
go vet ./...                 → 无输出（干净）
go test ./...                → 61 PASS / 0 FAIL   （P2 新增 19 个用例）
```

真机（`start.bat --ai --ai-mode hand`，重编后 `video-server.exe` 13:18:22）：

| 检查 | 实测 |
|---|---|
| `POST /api/cameras/camera01/aimode {"mode":"ai-hand"}` | **HTTP 200**（改动前 **400**） |
| agent 侧 | `[AI] model switched -> mode=hand`；此后稳定 `mode=hand fps=8.0 infer=12.2~14.4ms`，`sent=1420 failed=0 dropped=0 reconnect=0` |
| `ai_hand_log` | **387 行**（快照），`frame_id` 4922..6369，`detected=0` ×387（镜头前当时无人举手） |
| `ai_frame.hand` | `ai_mode='ai-hand'`，hand 块 327 B |
| `GET /hand/sessions` | `total_rows` 与行数一致、`gap_ms=1000`、**0 会话** —— 与"全部 detected=false"自洽 |

schema（在老库上确认，`addColumnIfMissing` 生效）：

```
ai_frame     + hand TEXT                       （增量列）
ai_hand_log  id, camera_id, frame_id, timestamp,
             detected, gesture, payload, received_at
```

### 13.2 "放行 `ai-hand`"是硬前提，不是锦上添花

实测过的因果链：agent 带 `--ai-mode hand` 启动 → `[HAND] ready: ... mode=hand(21kpt)` →
**1 秒后** `[AI] model switched -> mode=detect (models/yolo11n.onnx)`。

原因是 agent 的 aimode 轮询器读服务端存的模式，而 DB 里是 `ai-y`，且
`ValidAIMode()` 当时拒收 `ai-hand` —— 于是**服务端拒绝存储 → 轮询器把 agent 拽回 ai-y**，
联合运行下 hand 模式**根本不可能**稳定下来。这就是 P2 把"放行"排在最前面的原因。

### 13.3 保真落库：为什么存原字节而不是重新序列化

`HandState.MarshalJSON` 返回 **agent 自己的字节**（解码时原样留存），
录制器再把它整段写进 `payload`。这条性质**可被数据自证**：库里取出的 payload 是

```json
"confidence":0.00, ... "position":[0.0000,0.0000,0.0000], ...
```

—— 定点两位小数是 **C++ 编码器的格式**；Go 若重新编码会写成 `0` / `[0,0,0]`。
所以"服务端不改写生产者数据"不是设计声明，是**能验的**。

代价：`ai_frame.hand` 与 `ai_hand_log.payload` 两份字节都存。前者供 Web 读最新状态，
后者供离线重放（标定、轴向实验），两者用途不同，且都不大（327 B/帧）。

### 13.4 会话切分的两条判据是独立的（**本轮踩到的一个测试预期错误**）

`splitHandSessions` 的规则是：

1. `detected == false` 的行 **无条件**结束当前会话；
2. 相邻两行的 **agent 时钟**（PTS ms，非服务端 `received_at`）间隔 > `gap_ms` 才切分。

我最初的 API 用例把预期写成"`gap_ms=10000` 应把两段 hold 合成 1 个会话"，
**是错的**：那两段之间夹着一帧 `detected=false`，gap 根本没机会生效。
`gap` 管的是"**全是 detected 的行**之间隔了多久"（录制器漏采 / `detected_only` 模式），
它**跨不过**"手离开画面"这条事实；否则分析会把用户明明放下手的两次动作合并成一次。

测试已改成在 gap 真正生效的场景下测它（两段 hold 之间**没有任何行**），
并把这条不变式钉进纯函数测试表（`no gap merges across a loss`）。

### 13.5 错误分支真机核对

| 请求 | 期望 | 实测 |
|---|---|---|
| `?gap_ms=abc` | 400 | ✅ 400 |
| `?gap_ms=0` | 400 | ✅ 400 |
| 未知相机 | 404 | ✅ 404 |
| 不存在的会话号 | 404 | ✅ 404 |
| `/hand/sessions/abc` | 400 | ✅ 400 |

未知相机给 **404 而不是"0 个会话"**：后者会让"相机 id 打错"看起来像"录制器正常但空"。

### 13.6 前端在本阶段**故意不动**

`web/src` 的模式列表（`AI_MODES`）不含 `ai-hand`，而使用点是
`if (AI_MODES.includes(r.mode))` —— 未知模式被**忽略**而非抛错。所以服务端先放行
`ai-hand` 不会让现有页面坏掉；"手势"按钮与骨架 overlay 留到 P3。

### 13.7 受控链路验证：合成帧走通"部署中的那个 build"

单测走的是进程内的 `httptest`；这一轮改成对**正在跑的 :8081** 发真实 HTTP，
把 HTTP → 解码 → 校验 → 落库 → 切分 → 导出的整条链在部署形态上跑一遍。
先把 agent 切到 `ai-off`（否则它会往同一 camera_id 插帧，见 §13.8）：

| 步骤 | 结果 |
|---|---|
| `POST /api/metadata` × 5（3 帧 FIST → 1 帧 `detected=false` → 1 帧 OPEN_PALM） | 全部 `204` |
| `GET /hand/sessions` | `total_rows=5`、2 个会话：**#1 = 10001..10003（3 帧 FIST）**、**#2 = 10005（1 帧 OPEN_PALM）** |
| 同上 `?gap_ms=10000` | 仍 **2** 个会话 —— 印证 §13.4：gap 跨不过 `detected=false` 那一刀 |
| `GET /hand/sessions/1` | `version=1`、`frames=3`、`gesture=[FIST,FIST,FIST]` |
| 21 点 landmark 往返 | `landmarks=21`，**逐位一致 = True**（`[100.0,200.0,0.0]` … `[120.0,220.0,0.2]`） |
| `DELETE /hand/log` | `{"cleared":5}` |

### 13.8 一个测试方法学的坑：灌合成帧前先关 agent

**往 `POST /api/metadata` 灌合成帧时，必须先把 agent 切到 `ai-off`**（或换一个
`camera_id`）。理由：日志按**插入顺序**（`ORDER BY id`）排列，而真机帧的 PTS 与
合成帧不是同一时间基 —— 两者会互相插队，触发 §13.4 里"时间戳倒退 = agent 重启"那条规则。

我第一轮就撞上了：真机帧（`ts=494697`）夹在合成帧（`ts=500125` / `500250`）之间，
`d = -5428ms`，`|d| > gap` ⇒ flush。**本该 1 个会话被切成 3 个**。

这不是 bug（规则按设计生效，且它正是为了防"agent 重启被当成同一段动作"），
但不设防的验证脚本会因此得出**错误结论**并去改没坏的东西。
P4 的 `verify_hand.py` 必须注意：要么先关 agent，要么用独立的 camera_id。

### 13.9 仍未验证 / 待补

- **真机 `detected=true` 帧的会话与导出实证**（需举手配合）：要验的是 **agent 真实产出**的
  21 点 `landmarks`（含 C++ 的定点格式）过库返回来还是不是原样。
  §13.7 已用合成帧验过服务端这一侧，单测也覆盖，但**真机那一环还没连上**。
- `ai_hand_log` 的容量上界只在单测里验过（cap=5 剪到 5 行、最旧先剪）；
  真机默认 cap=100000 到不了，暂无法触发。



