# Camera Agent AI 阶段 —— 外部依赖安装清单

> 生成时间：2026-09-02
> 适用：Camera Agent 增加 5fps YOLO 人员检测 + ByteTrack 跟踪（第一阶段，不改 RTSP/Video Server/Web）
>
> **所有下载地址均已实测可达（HTTP 206 / 实际 Content-Length 已核对），可放心下载。**

---

## 0. 结论速览

| 类别 | 组件 | 体积 | 是否必装 | 安装方式 |
|---|---|---|---|---|
| ① | **ONNX Runtime** (C++, win-x64) 1.29.0 | ~75 MB | ✅ **必装** | 下载 zip，**纯解压**（无安装程序） |
| ② | **YOLO ONNX 模型** `yolov8n.onnx` | 12.8 MB | ✅ **必装** | 下载单个文件，放进项目 `models\` |
| ③ | OpenCV 4.14.0 Windows 预编译 | 203 MB（解压 ~800 MB） | ⭕ **建议不装** | 自解压 exe |
| ④ | CUDA 12.x + cuDNN 9.x | ~3 GB | ❌ **不装** | — |
| ⑤ | Eigen 3.4 | 1.3 MB | ❌ **不装** | 我用定长矩阵自己实现卡尔曼 |
| ⑥ | Python `ultralytics` / `onnx` | — | ❌ **不装** | 用官方预导出 ONNX，无需再导出 |

**你只需要做 2 件事**：下载解压 ①，下载放进 ②。其余我全部用零依赖代码实现。

### 为什么 ③⑤⑥ 不装

| 组件 | 不装的理由 |
|---|---|
| OpenCV | 本项目 AI 侧只需要「letterbox 缩放 + NMS + RGB 取帧」，合计约 120 行。**自实现可无痛迁移 RK3568**（嵌入式上 OpenCV 往往要交叉编译，反而是负担）。另外本机 Python 已有 `cv2`，我会用它做**交叉校验**，保证自实现结果与 OpenCV 一致。 |
| Eigen | ByteTrack 的卡尔曼滤波状态只有 8 维、观测 4 维，定长矩阵 + 高斯-约当求逆约 120 行即可，无需求助外部库。 |
| ultralytics | Ultralytics 官方已提供预导出 ONNX，直接下载即可，不需要 Python 再导出一遍。 |
| CUDA | YOLOv8n 在 i7-8700 上 CPU 推理约 25–40 ms/帧，而目标只有 5fps（200 ms 预算），CPU 占用约 15%，**完全够用**。装 CUDA 反而引入 3GB 依赖与版本匹配风险。 |

---

## 1. 必装 ①：ONNX Runtime（C++ / Windows x64）

### 用途
AI 推理引擎。C++ 侧通过 `Ort::Session` 加载 YOLO ONNX 模型做前向推理。
后续迁移 RK3568 时，`IDetector` 接口不变，只把 `OnnxYoloDetector` 换成 `RKNNYoloDetector`。

### 下载

| 项 | 值 |
|---|---|
| **推荐版本** | **1.29.0**（最新） |
| 下载地址 | https://github.com/microsoft/onnxruntime/releases/download/v1.29.0/onnxruntime-win-x64-1.29.0.zip |
| 备用版本（若 1.29.0 有异常） | 1.22.0 —— https://github.com/microsoft/onnxruntime/releases/download/v1.22.0/onnxruntime-win-x64-1.22.0.zip |
| 发布页（可看全部版本） | https://github.com/microsoft/onnxruntime/releases |

> ⚠️ **不要下载带 `-gpu_cuda` 后缀的包**（那需要配 CUDA，见第 4 节，本项目不用）。
> ⚠️ **不要下载 `win-arm64`**（本机是 x64）。

### 安装步骤（解压即可，无安装程序）

1. 下载 zip。
2. **解压**。zip 内有一层顶层目录 `onnxruntime-win-x64-1.29.0\`，请把它里面的 `include\` 和 `lib\` 两个文件夹取出。
3. 放到目标路径，最终结构**必须**是：

```
D:\Software\onnxruntime\
  ├── include\
  │     ├── onnxruntime_c_api.h
  │     ├── onnxruntime_cxx_api.h        ← 关键校验文件
  │     ├── onnxruntime_cxx_inline.h
  │     ├── onnxruntime_session_options_config_keys.h
  │     └── onnxruntime_run_options_config_keys.h
  └── lib\
        ├── onnxruntime.lib              ← 关键校验文件（链接用）
        ├── onnxruntime.dll              ← 关键校验文件（运行时）
        └── onnxruntime_providers_shared.dll
```

### ⚠️ 注意项

| # | 注意项 | 说明 |
|---|---|---|
| 1 | **放 D 盘，不要放 C 盘 `Program Files`** | 不需要管理员权限；且后续我要把 `onnxruntime.dll` 拷贝到 exe 目录，`Program Files` 下会触发 UAC 弹窗。 |
| 2 | **路径禁止空格与中文** | 本项目是 **Ninja + MSVC**，空格路径在 `.rsp` 响应文件和 `-I` 参数里极易出错。这也是本项目 `build-msvc.ps1` 一直踩的坑。 |
| 3 | **建议就用 `D:\Software\onnxruntime`** | `D:\Software` 已存在（你的常用软件目录），无需新建盘符结构。 |
| 4 | **不需要设置 ONNXRUNTIME 环境变量** | 我会在 CMake 里用 `ONNXRUNTIME_ROOT` 缓存变量指向它，你自己不动环境变量。 |
| 5 | **不要删 `onnxruntime_providers_shared.dll`** | 即使只用 CPU EP，`Ort::Session` 初始化时也会尝试加载它。 |

### 自检（安装后跑这一条）

```bash
ls -la /d/Software/onnxruntime/include/onnxruntime_cxx_api.h \
       /d/Software/onnxruntime/lib/onnxruntime.lib \
       /d/Software/onnxruntime/lib/onnxruntime.dll
```
三条都不报 `No such file` 即 OK。

---

## 2. 必装 ②：YOLO ONNX 模型

### 用途
人员检测器（只取 COCO 的 `person` 类，class_id = 0）。

### 下载（二选一）

| 模型 | 大小 | 下载地址 | 说明 |
|---|---|---|---|
| **yolov8n.onnx**（推荐，与提示词一致） | 12.8 MB | https://github.com/ultralytics/assets/releases/download/v8.4.0/yolov8n.onnx | 提示词明确写 YOLO；v8n 是 nano 版，输入 640×640，输出 `[1, 84, 8400]` |
| yolo11n.onnx（备选，更快） | 10.9 MB | https://github.com/ultralytics/assets/releases/download/v8.4.0/yolo11n.onnx | 输出布局同为 `[1, 84, 8400]`，精度/速度略优 |

> 两者输出张量布局一致（`1 × 84 × 8400`，84 = 4 框坐标 + 80 类），我的解析代码会**自动识别**并适配，你下哪个都能跑。

### 安装步骤

1. 在**项目根目录**新建文件夹 `models\`：

```
D:\user_project\git\SOC-Agent\001-carmera_rtmp_net\carmera-agent\models\
```

2. 把下载的文件放进去，命名为 `yolov8n.onnx`：

```
D:\user_project\git\SOC-Agent\001-carmera_rtmp_net\carmera-agent\models\yolov8n.onnx
```

### ⚠️ 注意项

| # | 注意项 |
|---|---|
| 1 | 该模型是 Ultralytics **官方预导出**的 ONNX（opset 较新），**不需要**再用 Python 导出或做任何转换。 |
| 2 | 若你下载的是 `yolo11n.onnx`，请**保持原文件名**并告诉我，我把配置里的 `ai.model` 改成对应路径即可。 |
| 3 | 模型文件 12.8MB，会被 git 跟踪。若不想入仓，告诉我，我加进 `.gitignore`（当前 `.gitignore` 没有 models 规则）。 |

### 自检

```bash
ls -la /d/user_project/git/SOC-Agent/001-carmera_rtmp_net/carmera-agent/models/yolov8n.onnx
# 期望大小：yolov8n = 12851049 字节；yolo11n = 10930182 字节
```

---

## 3. 可选 ①：OpenCV 4.14.0（**默认不装**）

仅在你想让我用标准 `cv::resize` / `cv::dnn::NMSBoxes` / 调试存图时才需要。
**默认方案是我自实现 letterbox + NMS，并用本机 Python `cv2` 做逐像素交叉校验**，所以这一项可以跳过。

若你决定装：

| 项 | 值 |
|---|---|
| 版本 | 4.14.0（4.x 最新；**不要装 5.0.0**，5.x 有 API 破坏性变更且 RK3568 BSP 普遍还是 4.x） |
| 地址 | https://github.com/opencv/opencv/releases/download/4.14.0/opencv-4.14.0-windows.exe |
| 大小 | 203 MB（解压后约 800 MB，D 盘现有 1116 GB 可用，无压力） |
| 发布页 | https://opencv.org/releases/ |

**注意项**：
- 是**自解压 exe**，双击会弹框让你填解压路径，填 `D:\Software\opencv` 即可（不是安装，只是解压）。
- 解压后目录为 `D:\Software\opencv\build\x64\vc16\`（OpenCV 4.14 预编译用 VS2019 工具链 `vc16`）。
- **vc16 与 VS2022 完全兼容**：MSVC v142 / v143 工具集二进制兼容，链接不会有问题。若包里是 `vc17` 更好，两者我都支持。
- 运行时需要 `D:\Software\opencv\build\x64\vc16\bin` 在 PATH 上，或由我拷贝 DLL 到 exe 目录（我会选后者，避免污染你的 PATH）。
- 路径同样**禁止空格和中文**。

---

## 4. 可选 ②：CUDA / cuDNN（**建议不装**）

本机是 **NVIDIA GTX 1070**（Pascal, sm_61）+ i7-8700。

**不装的理由**：YOLOv8n @ 640×640 在 i7-8700 上 CPU 推理约 25–40 ms/帧，而 AI 目标帧率只有 5fps（每帧预算 200 ms），CPU 占用约 15%，余量充足。装 CUDA 要额外 3 GB 并处理 cuDNN 版本匹配，收益为负。

若后续（比如换成 YOLOv8s/m 或要跑 30fps AI）确实需要 GPU，我再单独给你清单：
- CUDA Toolkit 12.x：https://developer.nvidia.com/cuda-downloads
- cuDNN 9.x：https://developer.nvidia.com/cudnn
- ONNX Runtime 换用 `onnxruntime-win-x64-gpu_cuda12-1.29.0.zip`
- 注意：GTX 1070 是 sm_61，CUDA 12 仍支持（CUDA 12 最低 sm_50），OK。

---

## 5. 明确不需要装的东西

| 组件 | 原因 |
|---|---|
| vcpkg | 本机无 vcpkg，且我们只需解压式依赖，不需要包管理器 |
| Eigen | 自实现定长矩阵卡尔曼（见第 0 节） |
| 任何 Python 包 | 用官方预导出 ONNX；验证用你已有的 Python `cv2` / `numpy` |
| protobuf / ncnn / TensorRT | 用不上 |
| OpenCV 5.0.0 | API 破坏性变更，RK3568 生态普遍还是 4.x |

---

## 6. 路径约定汇总（安装后请按此对表）

| 用途 | 约定路径 | 备注 |
|---|---|---|
| ONNX Runtime 根目录 | `D:\Software\onnxruntime` | 其下有 `include\` 和 `lib\` |
| ONNX Runtime 头文件 | `D:\Software\onnxruntime\include` | CMake `target_include_directories` |
| ONNX Runtime 导入库 | `D:\Software\onnxruntime\lib\onnxruntime.lib` | CMake `target_link_libraries` |
| ONNX Runtime 运行 DLL | `D:\Software\onnxruntime\lib\*.dll` | 构建后自动拷到 exe 同目录 |
| YOLO 模型 | `<项目根>\models\yolov8n.onnx` | YAML 里配 `ai.model` |
| OpenCV（可选） | `D:\Software\opencv` | 若装，其下有 `build\x64\vc16\` |

**项目根**：`D:\user_project\git\SOC-Agent\001-carmera_rtmp_net\carmera-agent`

---

## 7. 安装完成后请回传给我

请回复下面 3 项（若与约定一致，直接说「按约定装好了」即可）：

1. ONNX Runtime 的实际解压路径（若不是 `D:\Software\onnxruntime`，请给实际路径）
2. 模型文件的实际路径与文件名（`yolov8n.onnx` 还是 `yolo11n.onnx`）
3. 是否额外装了 OpenCV（是 → 给路径；否 → 我按零依赖方案实现）

---

## 8. 两个需要你确认的设计冲突

> 这两点不影响你现在安装，但会影响我实现时的取舍，先提出来。

### 8.1 提示词写 1080P30，但本机摄像头实际是 240×240@8fps

| 提示词假设 | 本机实测 |
|---|---|
| 1920×1080 @ 30fps | **240×240 @ 8fps**（`Negotiated capture format: 240x240 @ 8fps`） |

**我的处理**：不写死任何分辨率。
- bbox 坐标一律按**实际协商分辨率**归一化/还原，1080P 和 240×240 都能正确输出；
- `AIFrameResult.video_width/height` 填的就是协商值；
- 等你在 RK3568 上接 1080P 摄像头时，代码无需改动。

### 8.2 「每 6 帧取 1 帧」在 8fps 源上不成立

提示词的 `30 / 6 = 5fps` 是基于 30fps 的。8fps 源上「每 6 帧」= 1.33fps，太慢。

**我的处理**：改成**基于 PTS 的时间间隔采样**（默认 `ai.fps=5` → 每 200 ms 取一帧），30fps 源上等价于每 6 帧、8fps 源上约每 1.6 帧，两种源都能达到目标 AI 帧率。配置项 `AI_FPS` 仍然保留，语义不变。

### 8.3 帧共享方式（已定，供你确认）

```
Camera → videoconvert → [caps] → tee ──┬→ queue → 编码器 → RTSP        （原链路，AI 关闭时完全一致）
                                       └→ queue(leaky) → videoconvert → appsink   （AI 分支，新增）
```

- AI 关闭时（`ai.enable=false`）**不插入 tee/appsink**，管线字符串与今天逐字相同 → 零风险。
- AI 分支的 `queue` 用 `max-size-buffers=2 leaky=upstream`，appsink 用 `max-buffers=1 drop=true` —— 队列满时**丢旧帧留最新**，绝不会反向阻塞视频分支。
- appsink 取的是**原始分辨率 RGB**，letterbox 缩放（保持宽高比 + padding）在 C++ 里做，**不用 GStreamer 拉伸到 640**（拉伸会破坏宽高比，导致 bbox 变形）。

---

## 9. 环境自检命令（可选，装完自己跑一下）

在 Git Bash 里执行，一次核对所有依赖：

```bash
echo "=== ONNX Runtime ==="
for f in include/onnxruntime_cxx_api.h lib/onnxruntime.lib lib/onnxruntime.dll; do
  [ -f "/d/Software/onnxruntime/$f" ] && echo "  [OK] $f" || echo "  [!!] MISSING $f"
done

echo "=== Model ==="
ls -la /d/user_project/git/SOC-Agent/001-carmera_rtmp_net/carmera-agent/models/*.onnx 2>/dev/null \
  || echo "  [!!] no .onnx in models/"

echo "=== Optional OpenCV ==="
[ -d "/d/Software/opencv/build/x64" ] && ls /d/Software/opencv/build/x64/ || echo "  [--] not installed (default, OK)"
```
