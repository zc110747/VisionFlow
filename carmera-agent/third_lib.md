# third_lib.md — 第三方依赖 / 模型台账

> 约定：项目引入任何需要下载的第三方库 / 模型，在此登记来源、版本、校验值、安装路径与接入方式。

---

## 1. OpenSSL 3.5.7 LTS（便携版）✅ 已安装

| 项 | 内容 |
|---|---|
| 用途 | WSS/TLS 控制面传输（`ws_transport_openssl.cpp`，替代 Schannel） |
| 下载地址 | https://github.com/FireDaemon/openSSL//releases （FireDaemon OpenSSL 3.5.7 ZIP 便携版） |
| 安装路径 | `D:\data\agent-tools\openssl`（x64 子树） |
| SHA-256 | `2591459A06A6DF2D2E2B23B02A28D7C180B95C02FB4965099A708B7365A74014` |
| 接入方式 | CMake 候选路径探测（判据 `include/openssl/ssl.h`）→ `find_package(OpenSSL REQUIRED)`；POST_BUILD 拷 `libssl-3-x64.dll` / `libcrypto-3-x64.dll` 到 exe 同目录 |
| 注意 | IP 字面量主机校验必须 `X509_VERIFY_PARAM_set1_ip_asc`；`SSL_MODE_AUTO_RETRY` 必开 |

## 2. ONNX Runtime（已在用）✅

- Windows x64 Release ZIP，探测路径见 `src/CMakeLists.txt`（缓存变量 / 环境变量 / 常见目录）。

## 3. AI 模型（Ultralytics 官方，`models/` 目录）✅ 已下载并校验一致

来源仓库：https://github.com/ultralytics/assets （release tag `v8.3.0`，官方直接提供 ONNX 成品，无需自行导出）。

### 3.1 yolo11n.onnx —— 人形检测（COCO 80 类，只用 class 0 = person）✅

| 项 | 内容 |
|---|---|
| 下载地址 | https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n.onnx |
| 大小 | 10.4 MB |
| SHA-256 | `634279b40c07c6391472c51ad45b81ebc48706a9a1fe72dd3396322acd0c053b`（本机实测一致） |
| 输出 | `{1, 84, 8400}`（4 box + 80 cls，与 YOLOv8 同布局，现有解码直接兼容） |

### 3.2 yolo11n-pose.onnx —— 人体姿态（17 关键点 COCO，单类 person）✅

| 项 | 内容 |
|---|---|
| 下载地址 | https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n-pose.onnx |
| 大小 | 11.3 MB |
| SHA-256 | `93e2866b0ce678f99b4dd88af0c12e9ea2edf079a361dc2ecbc6226b77ff6408`（本机实测一致） |
| 输出 | `{1, 56, 8400}`（4 box + 1 cls + 17×3 kpt；模型内嵌 metadata `kpt_shape=[17,3]`） |

### 放置位置

```
carmera-agent/
  models/
    yolo11n.onnx        # ✅ 默认检测模型
    yolo11n-pose.onnx   # ✅ 姿态模型（--ai-model models/yolo11n-pose.onnx）
    yolov8n.onnx        # 旧模型，保留作回归对照
```

### 校验命令

```powershell
Get-FileHash models\*.onnx -Algorithm SHA256
```
