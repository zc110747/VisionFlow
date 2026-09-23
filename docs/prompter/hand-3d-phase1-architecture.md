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
| **P1e** | P1 验收：真机 720p@30 出 `ai-hand` 帧 + 补 fist 夹具定 `kCurledRatio` | 全链路真机跑通（需举手配合） | 待开始 |
| **P2** | server：schema 增量列 + `hand` 解码/校验 + append-only `ai_hand_log` 录制器 + `hand_session_*.json` 导出 + `ai-hand` 放行 | `go test ./...` 全绿 + 真机 metadata 落库 | 待开始 |
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

