#pragma once

// Shared, testable decode primitives for the MediaPipe hand pipeline.
//
// Split out of hand_detector.cpp for the same reason yolo_decode.h exists: the
// anchor table and the box/landmark decode are the parts that silently produce
// garbage when they are wrong, so they must be assertable WITHOUT loading a
// model (which needs ONNX Runtime and two 4 MB files).
//
// Everything here is pure arithmetic on caller-provided buffers.

#include <vector>

namespace ca {

// ---------------------------------------------------------------------------
// Palm detection - anchor table
// ---------------------------------------------------------------------------

// The palm model returns 2016 rows and does NOT embed the anchor boxes; the
// runtime has to generate them in exactly the same order. Verified value by
// value against the official opencv_zoo literal - see carmera-agent/models/README.md
// section "掌心检测的 anchor 表". Layout:
//
//   indices    0 .. 1151   grid 24x24 (stride 8),  2 anchors per cell
//   indices 1152 .. 2015   grid 12x12 (stride 16), 6 anchors per cell
//   both blocks loop      for y -> for x -> for k
//   centre                ((x + 0.5) / grid, (y + 0.5) / grid)
constexpr int kPalmInputSize = 192;
constexpr int kPalmAnchorCount = 2016;
// Regressions per anchor: 4 box + 2 * 7 palm keypoints.
constexpr int kPalmRegChannels = 18;
constexpr int kPalmKeypointCount = 7;

// Number of anchors (== the model's row count).
int palm_anchor_count();

// Fill `out` with 2 * palm_anchor_count() floats, cx/cy interleaved.
void palm_build_anchors(std::vector<float>& out);

// ---------------------------------------------------------------------------
// Palm detection - per-row decode
// ---------------------------------------------------------------------------

// One decoded palm, in ORIGINAL VIDEO pixels.
struct PalmBox {
    float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
    // 7 palm keypoints (0 and 2 are the palm base / middle-finger base used to
    // derive the hand's rotation), also in original video pixels.
    float lm[7][2] = {};
};

// Decode one anchor row.
//
//   reg        kPalmRegChannels floats: [cx_delta, cy_delta, w_delta, h_delta,
//              then 7 x (x_delta, y_delta)]
//   anchors    2 * kPalmAnchorCount floats, cx/cy interleaved (palm_build_anchors)
//   anchor_idx which anchor this row belongs to
//   scale      max(original_w, original_h) - mirrors the reference implementation's
//              `scale = max(original_shape)`, i.e. the inverse of the letterbox ratio
//   pad_x/pad_y  letterbox padding expressed in ORIGINAL image pixels
//
// The reference decoder divides every delta by the 192 px network input, adds the
// anchor centre, multiplies by `scale`, then subtracts the padding.
PalmBox palm_decode_row(const float* reg, const std::vector<float>& anchors,
                        int anchor_idx, float scale, float pad_x, float pad_y);

// ---------------------------------------------------------------------------
// Greedy NMS on (x1,y1,x2,y2) boxes.
// ---------------------------------------------------------------------------
// The reference implementation hands OpenCV `NMSBoxes` a (x1,y1,x2,y2) quad even
// though that function expects (x,y,w,h). We deliberately do NOT replicate that
// quirk: we suppress on the TRUE box IoU, which is the intended behaviour and is
// a no-op for palms (they are essentially never overlapping).
void palm_nms(std::vector<int>& idx, const std::vector<PalmBox>& boxes,
              const std::vector<float>& scores, float score_threshold,
              float nms_threshold, int top_k);

} // namespace ca
