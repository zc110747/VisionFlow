#pragma once

// Shared, dependency-free YOLO output decoding (YOLOv8 / YOLO11, detect and
// pose heads). Lives in a header so the unit tests can exercise the exact code
// path the detector uses, with synthetic tensors, without ONNX Runtime.
//
// Layouts handled (auto-selected by the caller from the tensor shape):
//   {1, C, N}  channel-major (YOLOv8/11 default export)  -> data[c * N + a]
//   {1, N, C}  anchor-major (some exports)               -> data[a * C + c]
// with C = 4 box coords + class score(s) [+ 3 * K keypoints for pose heads]
// and N = number of anchors.

#include <cstdint>
#include <vector>

#include "camera_agent/ai/detector.h"

namespace ca {

// Number of body keypoints implied by an output channel count.
// Returns 0 for detection heads (and for anything that is not plausibly a
// pose head). Pose head: C = 5 + 3*K with K >= 4 keypoints
// (COCO pose models export K = 17 -> C = 56).
// NOTE: a detection model with exactly 4/7/10/13/... classes would be
// misdetected by this shape heuristic; the runtime detector therefore
// prefers the model's embedded `kpt_shape` metadata when present.
int keypoint_count_from_channels(int64_t channels);

// Decode one raw YOLO output tensor into Detection(s) mapped back to the
// ORIGINAL video pixel space (letterbox inverse transform, pad-114 convention).
//
//   data          : raw float tensor payload, {1, rows, cols}
//   channel_major : true for {1, C, N}, false for {1, N, C}
//   want_class    : class id to keep (0 = person)
//   floor_score   : emit nothing below this score
//   ow / oh       : original video size (inverse-transform target space)
//   scale / pad_x / pad_y : letterbox parameters of the forward transform
//   out           : receives the kept detections (cleared first on error)
//
// The class score and the keypoint confidence are already sigmoided inside
// the exported graph; neither is sigmoided again here.
void decode_yolo_output(const float* data, int64_t rows, int64_t cols,
                        bool channel_major, int want_class, float floor_score,
                        int ow, int oh, float scale, int pad_x, int pad_y,
                        std::vector<Detection>& out);

} // namespace ca
