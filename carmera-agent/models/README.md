# models/ — 模型资产与溯源

所有模型均为**已入库的二进制**（克隆即可运行，无需联网）。每个模型的来源、
许可与 SHA-256 记录在此，改动或替换模型时必须同步更新本文件。

## 1. 现有模型

| 文件 | 用途 | 输入（实测） | 输出（实测） | 大小 |
|---|---|---|---|---|
| `yolo11n.onnx` | 人检测（COCO class 0），`ai-y` 模式 | NCHW `(1,3,640,640)` f32 /255 | `(1,84,8400)` channel-major | 10.4 MB |
| `yolo11n-pose.onnx` | 人 + 17 COCO 关键点，`ai-y-pose` 模式 | NCHW `(1,3,640,640)` f32 /255 | `(1,56,8400)` = 5 + 3×17 | 11.3 MB |
| `palm_detection_mediapipe_2023feb.onnx` | 手部两级级联 **第 1 级**：掌心检测，`ai-hand` 模式 | **NHWC** `(1,192,192,3)` f32 RGB /255，**黑色 (0,0,0) 居中 letterbox** | `Identity(1,2016,18)` = 4 框偏移 + 14（7 关键点 ×2）<br>`Identity_1(1,2016,1)` 分类 **logits（需 sigmoid）** | 3.9 MB |
| `handpose_estimation_mediapipe_2023feb.onnx` | 手部两级级联 **第 2 级**：21 点手部关键点 | **NHWC** `(1,224,224,3)` f32 RGB /255 | `Identity(1,63)` = 21×(x,y,z) 屏幕坐标<br>`Identity_1(1,1)` conf<br>`Identity_2(1,1)` handedness（≤0.5 = 左手）<br>`Identity_3(1,63)` = 21×(x,y,z) world（米，不用） | 4.1 MB |

## 2. 手部模型溯源

来源：**[opencv/opencv_zoo](https://github.com/opencv/opencv_zoo)**（MediaPipe Hands 的
TFLite→ONNX 转换版）

```
https://github.com/opencv/opencv_zoo/raw/main/models/palm_detection_mediapipe/palm_detection_mediapipe_2023feb.onnx
https://github.com/opencv/opencv_zoo/raw/main/models/handpose_estimation_mediapipe/handpose_estimation_mediapipe_2023feb.onnx
```

| 文件 | SHA-256 |
|---|---|
| `palm_detection_mediapipe_2023feb.onnx` | `78ff51c38496b7fc8b8ebdb6cc8c1abb02fa6c38427c6848254cdaba57fcce7c` |
| `handpose_estimation_mediapipe_2023feb.onnx` | `db0898ae717b76b075d9bf563af315b29562e11f8df5027a1ef07b02bef6d81c` |

许可：**Apache License 2.0**（原文见 `LICENSE-MediaPipe-Apache-2.0.txt`）。
上游算法与论文：MediaPipe Hands: On-device Real-time Hand Tracking。

> ⚠️ `*_int8.onnx` / `*_int8bq.onnx` 变体**不要用**：官方 README 明确标注 int8 量化版
> 精度显著下降、可能产生无效结果。

## 3. 掌心检测的 anchor 表（**不可凭记忆改写**）

palm 模型**不在图内输出 anchor**，必须由运行时生成并与网络的 2016 行输出**严格同序**。
顺序错了不会报错，只会静默输出垃圾框。已逐值核对（容差 1e-6，实际残差 3e-8 = float32 字面量舍入）：

```
层 0: stride 8  → 网格 24×24，每格 2 个 anchor，索引      0 .. 1151
层 1: stride 16 → 网格 12×12，每格 6 个 anchor，索引   1152 .. 2015
两层循环序: for y -> for x -> for k
anchor 中心 = ((x + 0.5) / grid, (y + 0.5) / grid)
```

核对脚本与结论见 `docs/prompter/hand-3d-phase1-architecture.md` §7.2 / §9.4；
C++ 侧实现为 `src/ai/hand_decode.cpp` 的 `palm_build_anchors()`，并由
`tests/hand_detector_tests.cpp` 的 `hand_palm_anchor_table` 断言首/中/末项与总数。

## 4. 端到端数值验证（黄金基准）

`tests/golden/` 存有无损夹具 `woman_hands.png` 与一份由官方算法（`cv.dnn` 换成 ONNX Runtime）
逐值产出的参考输出。`tests/hand_detector_tests.cpp` 的 `hand_cascade_golden_woman_hands`
把 C++ 的两级级联结果与之比对，实测残差：

```
21 点 x / y / z  :  0.00034 / 0.00079 / 0.00173 px
手部 bbox        :  0.00065 px
掌心 anchor/score:  idx=456 / 0.845936   （与参考逐位相同）
```

溯源、夹具为什么必须无损（JPEG 解码器之间有最大 3 LSB 差异）、以及如何重新生成，
见 `tests/golden/README.md`。

## 5. 复现下载（可选）

```bash
cd carmera-agent/models
curl -sLO https://github.com/opencv/opencv_zoo/raw/main/models/palm_detection_mediapipe/palm_detection_mediapipe_2023feb.onnx
curl -sLO https://github.com/opencv/opencv_zoo/raw/main/models/handpose_estimation_mediapipe/handpose_estimation_mediapipe_2023feb.onnx
sha256sum -c <<'EOF'
78ff51c38496b7fc8b8ebdb6cc8c1abb02fa6c38427c6848254cdaba57fcce7c  palm_detection_mediapipe_2023feb.onnx
db0898ae717b76b075d9bf563af315b29562e11f8df5027a1ef07b02bef6d81c  handpose_estimation_mediapipe_2023feb.onnx
EOF
```
