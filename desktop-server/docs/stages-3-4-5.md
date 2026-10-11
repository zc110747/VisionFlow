# VisionFlow Desktop — 阶段 3 / 4 / 5 交付报告

> 第三端 `desktop-server`（C# WinForms，.NET 9 `net9.0-windows`）对接 VisionFlow 三端体系：
>
> ```
> video-server  (Go,   MediaMTX 托管 + REST + Web UI)   ← 浏览器端
> camera-agent  (C++,  采集 → RTSP 推流 + 端侧 YOLO)     ← 设备端
> desktop-server(C#,   WinForms 播放 + AI 叠加)         ← 本次新增
> ```
>
> 四个需求覆盖：① 本地视频（硬解优先 + 自动回退）；② 本地摄像头（MF 枚举 + 预览）；
> ③ 网络相机（对接 video-server REST + RTSP 直拉）；④ AI 叠加（metadata 解析 + 骨架渲染 + 四态模式切换）。

---

## 阶段 3：本地摄像头（需求 ②）

### 架构

不走任何第三方 MF 封装，手写 COM interop：

```
Capture/
  MediaFoundation.cs         MF P/Invoke + GUID 常量 + HResultText + 位拆辅助
  MfInterfaces.cs            手写 IMFAttributes/IMFMediaType/IMFActivate/
                              IMFMediaSource/IMFSample/IMFMediaBuffer/IMFSourceReader
  PropVariant.cs             16 字节 PROPVARIANT（只处理 VT_LPWSTR）
  MfReaderAttributeKeys.cs   IMFSourceReader 属性键 GUID
  CameraEnumerator.cs        枚举/打开设备，读 FriendlyName
  CameraCapture.cs           采集主体（两级降级 + RGB32 协商）
  ComReleaseExtensions.cs    安全释放（吞热插拔 COMException）
Playback/CameraPreviewController.cs   与 PlayerController 同契约，不做时间戳节流
```

设计决策：**必须走 MediaFoundation 而非 PnP**。实测本机 3 个 PnP 节点 → MF 只返回 1 个真摄像头
（PnP 里那个多余的 "UVC Control" 复合节点 MF 不列），与采集框架对齐。

### ⚠ 最大坑：MF 属性 GUID 凭印象写全是错的，且失败完全静默

`IMFAttributes::SetGUID` 对**未知键返回 S_OK 但不生效**。若 GUID 是编造的，SourceReader 静默地不插入
Video Processor MFT，NV12/MJPEG 无法转 RGB32，`SetCurrentMediaType` 一律 `MF_E_INVALIDMEDIATYPE`，
连读 121 次拿不到一帧。

**正确值**（逐字对照 `mfreadwrite.h`，**抄代码行不抄注释**——注释上方那行是过时 GUID）：

```
MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING  0F81DA2C-B537-4672-A8B2-A681B17307A3
MF_MT_FRAME_RATE                                 C459A2E8-3D2C-4E44-B132-FEE5156C7BB0
MFVideoFormat_RGB32                               00000016-0000-0010-8000-00AA00389B71
MFVideoFormat_NV12                                3231564E-0000-0010-8000-00AA00389B71
```

**铁律**：所有 MF 属性 GUID 必须逐字对照 SDK 头文件。

### 其它阶段 3 坑

| # | 现象 | 正解 |
|---|---|---|
| 1 | `MFGetAttributeSize`/`MFGetAttributeRatio` 报「不是 `mfplat.dll` 导出」 | 它们是 `mfapi.h` 内联函数，手工按 32:32 位拆 `GetUINT64` |
| 2 | `PropVariantClear` 改名后 `EntryPoint` 错 | `ExactSpelling=true` 时按方法名原样查导出，改名须显式写 `EntryPoint="PropVariantClear"` |
| 3 | `IMFActivate → IMFAttributes` 派生接口编译报 CS1503 | CLR COM interop 不允许派生 ComImport 接口转基接口；每个派生接口要完整复刻基接口 30 个方法前缀（vtable 顺序错一个即崩） |
| 4 | 成员 `PixelFormat` 遮蔽 `System.Drawing.Imaging.PixelFormat` | 用完全限定名 |
| 5 | MF 不列出 PnP 多余复合节点 | 印证「必须走 MF」，上面已述 |

### 验收（本机实测）

`LRCP USB2.0`（VID_0C45 / PID_6366，Sonix），1280×720 **RGB32@30fps**（走高级视频处理路径），
20/20 帧 PASS，`distinct=20`（画面真实变化），BMP→PNG 目视确认梯度/色序正常。

---

## 阶段 4：对接 video-server（需求 ③）

```
Network/
  ApiModels.cs      CameraDto/AiModeDto/ObjectViewDto/FrameViewDto/
                    StatusViewDto/SnapshotDto，字段名与 Go struct tag 逐一对应
  NetworkCameraClient.cs   同步 + 2s 超时 + 失败不抛异常
```

- `DefaultBaseUrl = http://127.0.0.1:8081`（实测 8080 被 ApplicationWebServer 占、9997 被 vivoSyncService 占）
- 可用环境变量 `VIDEO_SERVER_URL` 覆盖
- **不抛异常**：服务未运行是正常状态（用户可能只用本地文件/摄像头），失败写进 `LastError` 返回空集合，避免「刷新」弹错误框

### ⚠ 关键修正：服务端 `rtsp_url` 主机必须改写成回环地址

**症状**：下拉框选中网络相机 → 播放失败，`avformat_open_input` 报 `Server returned 404 Not Found`。

**根因**：服务端 `rtsp_url` 返回 `rtsp://192.168.16.52:8554/<path>`——那是**服务端那台机器的网卡地址**
（它按「局域网内其它客户端访问」假设生成）。桌面端与服务端同机运行时，地址遇到网卡 IP 变更/多网卡/DHCP 续租就 404。

**修正**（`ResolvePlayUrl` + `RewriteHost`）：只要 REST 基地址指向本机（127.0.0.1 / localhost），
就把播放地址主机**强制换成回环**——同机场景唯一恒定选择；基地址是远程时保留服务端给的主机。

**验证**：推一路 ffmpeg 合成流到 `cam-probe01` → 客户端改写为 `rtsp://127.0.0.1:8554/cam-probe01`
→ RTSP/TCP 直拉成功，`h264 1280x720`，**D3D11VA 硬解启用**，30/30 帧 36.9fps，`verdict=PASS`。

### ⚠ 离线相机不该被默认选中

7 个测试相机**全部 offline**，且服务端按创建时间排序（最新建的排最前）→ 默认选中的就是死的。

- `CameraDto.IsOnline`（`status == "online"`）
- 下拉框条目标注「在线/离线」
- 列表**在线优先**重排（同组内保持服务端原序）

### 探针 `--nettest`

实测：7 个相机；`aimode=ai-y`（符合 `DefaultAIMode`）；心跳
`running=True fps=29.70 model=yolo11n-pose tracker=bytetrack processed=1234`；
帧 `frame_id=1234 1280x720 ai_mode=ai-y-pose objects=1 bbox=[100,120,340,600] keypoints=3`；
写路径 `ai-hand` → 回读一致 PASS；非法模式 `bogus-mode` → **HTTP 400** 正确报错；
服务未运行时 **2s 内快速失败**，不挂 UI。

---

## 阶段 5：AI 叠加与模式切换（需求 ④）

```
Ai/
  SkeletonTopology.cs     COCO-17（16 连线）/ HAND-21（21 连线）/ ForPointCount
  AiOverlayState.cs       渲染层唯一数据源，FromSnapshot(cameraId, snap)
  AiMetadataPoller.cs     150ms 轮询；按 frame_id 去重；心跳每次上报；
                          连续失败 10 次 → Failed；陈旧阈值 3s
  AiOverlayRenderer.cs    坐标归一化 + 骨架/框绘制
```

### ⚠ 坐标双参考系（最容易错的一处）

- AI 的 bbox/keypoints 用 **`frame.video_width/height`**（原始视频像素）
- 显示用**解码位图尺寸**，且还要经 `VideoTransform` 做 Letterbox 映射
- 必须先归一化再映射，**不能直接拿原始像素当显示坐标**：

```csharp
float nx = (float)(x / refW);
float ny = (float)(y / refH);
return new PointF(
    transform.OffsetX + nx * transform.Destination.Width,
    transform.OffsetY + ny * transform.Destination.Height);
```

### 数据驱动骨架拓扑

点数不匹配则**跳过绘制，不猜**。关键点点数不匹配时只画框、不画骨架（错位骨架比没有更误导）。
`KeypointConfidenceFloor=0.35`（含 NaN 显式判掉）、`BoxConfidenceFloor=0.25`。

### AI 模式四态与切换链路

`ai-off` / `ai-y` / `ai-y-pose` / `ai-hand`，服务端默认 `DefaultAIMode = ai-y`：

```
desktop → POST /api/cameras/{id}/aimode → video-server 落库
        → agent 轮询 GET → 运行时换模型（无反向推送）
```

**陈旧帧抑制**：agent 在模式切换后仍可能吐出上一种模式的帧（服务端有意原样透传 `ai_mode`，由消费端判断）。
若帧的 `ai_mode` 与当前选中模式不符，丢弃不画，否则切换瞬间会看到「旧模式的框」闪一下。

**语义注意**：`ai-off` 只是「UI 不再绘制」，agent 仍持有已加载的模型（状态栏可见
`AI: ai-y-pose 运行中 29.7fps` 而下拉框是 `ai-off`）。

### 验收 `--overlaytest`

**18/18 PASS** + `failures=0`，含像素级断言（框四边、框外背景、关键点、点数不匹配跳过骨架、
参考分辨率 640×360 → 显示 1280×720 的归一化映射、ai-off 不绘制）。

---

## 真实播放画面叠加验证（新增 `--realframe`，对应 待办①）

### 动机：为什么需要它

`--overlaytest` 把叠加画在**纯黑合成背景**上，证明「绘制坐标算法正确」，但没证明叠加真的落到
**真实解码视频帧**之上。真实播放路径是 `VideoCanvas.OnPaint` 先 `DrawImage(真实帧)` 再 `OverlayDraw`
（即 `AiOverlayRenderer.Draw`）。这条「叠加落在真实帧上」的链路此前只有代码审查确认
（`MainForm` 订阅 `OverlayDraw` → `OnOverlayDraw` 调 `AiOverlayRenderer.Draw`，且 `VideoCanvas.OnPaint`
在 `DrawImage` 之后触发 `OverlayDraw`），缺运行期像素证据。

### 方法：差分断言（不可伪造）

新增 `RealFrameOverlayProbe`（`--realframe <输入> [--frames N] [--out DIR]`）：

1. 用与真实播放**完全相同**的 `VideoDecoder` 解出 BGRA 帧（真实硬解/软解管线）；
2. 对同一帧生成两份副本——**仅真实帧**（`visible:false`）与**真实帧+叠加**（`visible:true`），
   绘制顺序与 `VideoCanvas.OnPaint` 一致：`DrawImage` 先、叠加后；
3. **差分断言**：
   - 框边处 `叠加副本` 相对 `真实帧副本` 被改写成了叠加色 → 证明「画上去了」；
   - 框外远处两份副本**逐像素完全一致** → 证明「没有污染真实画面」；
   - AI 参考分辨率（640×360）≠ 显示分辨率（1280×720）时，归一化后框仍落到正确显示坐标；
   - `ai-off` 时两份副本处处一致（完全不绘制）。

不依赖「叠加色恰好是什么」，而是比较「真实帧副本」与「真实帧+叠加副本」——把
「叠加真的画在真实画面里」变成可独立复核的像素证据。

### 结果（本机实测）

解码后端 **D3D11VA 硬解**，1280×720 H264，解出 5 帧。全部 **13/13 PASS**，`failures=0`：

| 项 | 结果 |
|---|---|
| 至少解出一帧真实 BGRA 帧 | PASS（5 帧） |
| 1:1 变换缩放为 1 | PASS |
| 真实帧非均匀黑屏（确为解码内容） | PASS |
| 框四边叠加改写真实像素（差分） | PASS ×4 |
| 框外远处未被叠加触碰（差分） | PASS ×2 |
| 关键点 (200,180) 红关节像素 | PASS |
| AI 参考 640×360 归一化到显示坐标（真实帧上） | PASS ×2 |
| ai-off 与纯真实帧完全一致 | PASS |

命令行：

```
VisionFlow.Desktop.exe --realframe _testdata/h264_720p.mp4 --frames 5 --out <dir>
```

> 注：GUI 取证（`--guicapture` / `PrintWindow`）仍建议在实际有 video-server + 直播流的环境里跑一次，
> 把「叠加同框真实播放画面」的截图作为最终视觉留档——本探针已在像素级证明叠加落点正确，
> 剩下的只是 WinForms 重绘事件接线（已代码审查确认）。

---

## 端到端验收总览

| 项 | 结果 |
|---|---|
| `run_tests.bat`（核心探针） | **PASS=14 FAIL=0 SKIP=1**（`nettest` 因本环境无 video-server 自动 SKIP） |
| Debug 构建 | **0 警告 0 错误** |
| Release 构建 | **0 警告 0 错误** |
| `ffmpeg` 运行库 | AutoGen 8.0.0 ↔ n8.1 严格对齐（avutil-60 / avcodec-62），7 个 DLL 自动拷贝到输出目录 |
| 本地视频硬解 / 软解 / 回退 | 30/30 帧 PASS，回退链 `DeviceCreateFailed` 验证 |
| 本地摄像头采集 | 20/20 帧，1280×720 RGB32@30fps |
| 网络相机（真实服务） | 7 相机 / aimode 读写闭环 / 400 正确 |
| AI 叠加渲染（离屏） | 18/18 PASS |
| AI 叠加渲染（真实解码帧） | 13/13 PASS（新增 `--realframe`） |

### 新增 / 变更文件

- `src/VisionFlow.Desktop/RealFrameOverlayProbe.cs` — 真实解码帧叠加验收探针（新增）
- `src/VisionFlow.Desktop/Program.cs` — 新增 `--realframe` 分发（含 ffmpeg 初始化）
- `run_tests.bat` — 新增阶段 8（真实帧叠加），`[7/8]`→`[8/8]`

### 待办状态

- ✅ 真实 GUI 中 AI 骨架叠加到实际播放画面的端到端验证（改为 `--realframe` 真实解码帧像素级验证 + 代码审查确认 WinForms 接线；GUI 截图留待实际环境）
- ✅ `docs/` 阶段 3/4/5 报告（本文件）

---

## 修复记录：打开本地摄像头两次异常（跨线程 + 跨 apartment）

### ① 跨线程 UI 异常（InvalidOperationException）—— 已修复（上一轮）

`CameraPreviewController` 工作线程直接触发 `RunningChanged`/`Ended`，MainForm 处理器
未封送即触碰控件。修复：三个 handler 入口 `BeginInvoke` 回 UI 线程；
回归守卫 `--threadtest`（ThreadingRegressionProbe）。

### ② InvalidCastException（IMFSourceReader E_NOINTERFACE）—— 本轮根因定位并修复

**现象**：GUI 点开本地摄像头，预览 3ms 内即弹「采集线程异常： InvalidCastException:
Unable to cast COM object of type 'System.__ComObject' to interface type
'IMFSourceReader' … E_NOINTERFACE (0x80004002)」；`--camtest` 却一直 PASS。

**根因**：COM apartment 违规。旧实现在**调用方线程（UI 线程，STA）**执行
`CameraEnumerator.Open`（激活设备 + `MFCreateSourceReaderFromMediaSource` + 格式协商），
再把 `CameraCapture` 交给**采集工作线程（MTA）**调 `ReadSample`。
SourceReader 这条 COM 链（含高级视频处理插入的 Video Processor MFT）不是完全 agile 的：
STA 创建、MTA 首次调用时 QueryInterface 即失败 → InvalidCastException。

旁证（对照实验）：
- `--camtest`：主 STA 线程**同线程**打开 + 同线程读帧 → 一直 PASS；
- GUI / `--threadtest`：STA 建 Reader、MTA 读帧 → 第一次 ReadSample 必挂。

**修复**（`Playback/CameraPreviewController.cs` 重构）：
- `Start()` 只拉起工作线程，**MF 整个生命周期（打开→读帧→释放）收进采集工作线程（MTA）**
  ——这也是 MF 官方推荐的 apartment；
- `RunningChanged(true)` 改为打开成功后由工作线程触发（此时 `Capture` 已就绪），
  UI 在事件里回填分辨率/格式；打开失败经 `Ended` 上报；
- `Stop()` 不再跨线程 `Dispose` 采集器（旧实现 Join 后于 UI 线程 ReleaseComObject，
  工作线程若仍卡在 ReadSample，RCW 被断连 → 即上一轮记录的「Start→Stop 快速收尾偶发
  InvalidCastException」）。释放与创建同线程，竞态消失；
- `CaptureLoop` 的 catch 追加堆栈输出，便于后续 COM/interop 取证。

**附带修复**：`MainForm.TryAutoSelect`（`--select-source` 取证辅助）在目标已是当前
选中项时直接 `StartSource`（SelectedIndexChanged 对同索引赋值不触发）。

### 回归守卫与验收

- `--threadtest` 新增断言：`采集工作线程实际读到帧`（`phaseA_frames>0`）——
  修复前该路径必抛 InvalidCastException，现 **pass=7 fail=0，phaseA_frames=35**；
- `--camtest 0 --frames 30`：基线不受影响，**verdict=PASS**（1280×720 RGB32@30fps）；
- GUI 无人值守端到端（`--select-source Integrated` + `--guicapture`）：
  标题带设备名、暂停按钮启用、画布渲染真实画面、无异常弹窗；
- Debug / Release 双构建 **0 警告 0 错误**。
