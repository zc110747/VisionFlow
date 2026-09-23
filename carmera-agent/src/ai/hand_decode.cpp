// MediaPipe palm-detection decode primitives (declared in hand_decode.h).
//
// Deliberately free of ONNX Runtime so that the two highest-risk pieces - the
// anchor table and the box/landmark un-letterboxing - can be unit tested on a
// machine that has no model backend installed at all.

#include "camera_agent/ai/hand_decode.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ca {
namespace {

// One SSD anchor block: a square grid of cells, `per_cell` anchors each.
struct AnchorLayer {
    int stride;
    int per_cell;
};

// Verified against the official opencv_zoo `_load_anchors()` literal, element by
// element, at a 1e-6 tolerance (residual 3e-8 == float32 literal rounding).
// Changing this array changes the meaning of every row the model emits.
constexpr AnchorLayer kAnchorLayers[] = {
    {8, 2},    // 24x24 grid -> 1152 anchors, indices    0 .. 1151
    {16, 6},   // 12x12 grid ->  864 anchors, indices 1152 .. 2015
};

} // namespace
int palm_anchor_count() {
    int n = 0;
    for (const auto& l : kAnchorLayers) {
        const int grid = kPalmInputSize / l.stride;
        n += grid * grid * l.per_cell;
    }
    return n;
}

void palm_build_anchors(std::vector<float>& out) {
    out.clear();
    out.reserve(static_cast<size_t>(palm_anchor_count()) * 2u);
    for (const auto& l : kAnchorLayers) {
        const int grid = kPalmInputSize / l.stride;
        for (int y = 0; y < grid; ++y) {
            for (int x = 0; x < grid; ++x) {
                const float cx = (static_cast<float>(x) + 0.5f) /
                                 static_cast<float>(grid);
                const float cy = (static_cast<float>(y) + 0.5f) /
                                 static_cast<float>(grid);
                for (int k = 0; k < l.per_cell; ++k) {
                    out.push_back(cx);
                    out.push_back(cy);
                }
            }
        }
    }
}

PalmBox palm_decode_row(const float* reg, const std::vector<float>& anchors,
                        int anchor_idx, float scale, float pad_x, float pad_y) {
    PalmBox p;
    if (!reg || anchor_idx < 0) return p;
    const size_t a = static_cast<size_t>(anchor_idx) * 2u;
    if (a + 1u >= anchors.size()) return p;

    const float inv_input = 1.0f / static_cast<float>(kPalmInputSize);

    // Box: normalised centre/size deltas -> anchor space -> image pixels.
    const float cx = reg[0] * inv_input + anchors[a];
    const float cy = reg[1] * inv_input + anchors[a + 1u];
    const float w  = reg[2] * inv_input;
    const float h  = reg[3] * inv_input;

    p.x1 = (cx - w * 0.5f) * scale - pad_x;
    p.y1 = (cy - h * 0.5f) * scale - pad_y;
    p.x2 = (cx + w * 0.5f) * scale - pad_x;
    p.y2 = (cy + h * 0.5f) * scale - pad_y;

    // 7 palm keypoints: same normalisation, same anchor centre, same un-padding.
    for (int j = 0; j < kPalmKeypointCount; ++j) {
        const int b = 4 + j * 2;
        const float kx = reg[b] * inv_input + anchors[a];
        const float ky = reg[b + 1] * inv_input + anchors[a + 1u];
        p.lm[j][0] = kx * scale - pad_x;
        p.lm[j][1] = ky * scale - pad_y;
    }
    return p;
}

void palm_nms(std::vector<int>& idx, const std::vector<PalmBox>& boxes,
              const std::vector<float>& scores, float score_threshold,
              float nms_threshold, int top_k) {
    idx.clear();
    const size_t n = std::min(boxes.size(), scores.size());
    if (n == 0) return;

    // Score gate first, then sort descending - same order the reference gets
    // from NMSBoxes, so the surviving set is comparable when boxes do not overlap.
    std::vector<int> cand;
    cand.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (scores[i] < score_threshold) continue;
        cand.push_back(static_cast<int>(i));
    }
    std::sort(cand.begin(), cand.end(), [&](int a, int b) {
        return scores[static_cast<size_t>(a)] > scores[static_cast<size_t>(b)];
    });
    if (top_k > 0 && static_cast<int>(cand.size()) > top_k) cand.resize(static_cast<size_t>(top_k));

    std::vector<char> suppressed(cand.size(), 0);
    for (size_t i = 0; i < cand.size(); ++i) {
        if (suppressed[i]) continue;
        idx.push_back(cand[i]);
        const PalmBox& A = boxes[static_cast<size_t>(cand[i])];
        const float aa = (A.x2 - A.x1) * (A.y2 - A.y1);
        for (size_t j = i + 1; j < cand.size(); ++j) {
            if (suppressed[j]) continue;
            const PalmBox& B = boxes[static_cast<size_t>(cand[j])];
            const float ix1 = std::max(A.x1, B.x1);
            const float iy1 = std::max(A.y1, B.y1);
            const float ix2 = std::min(A.x2, B.x2);
            const float iy2 = std::min(A.y2, B.y2);
            const float iw = ix2 - ix1;
            const float ih = iy2 - iy1;
            if (iw <= 0.0f || ih <= 0.0f) continue;
            const float inter = iw * ih;
            const float bb = (B.x2 - B.x1) * (B.y2 - B.y1);
            const float uni = aa + bb - inter;
            if (uni <= 0.0f) continue;
            if (inter / uni > nms_threshold) suppressed[j] = 1;
        }
    }
}

} // namespace ca
