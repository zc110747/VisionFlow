// MediaPipe hand detector: palm detection -> hand landmark cascade.
//
// Ported from the official opencv_zoo implementation (Apache-2.0):
//   models/palm_detection_mediapipe/mp_palmdet.py
//   models/handpose_estimation_mediapipe/mp_handpose.py
//
// Deliberately NO OpenCV, for the same reason yolo_detector.cpp has none: the
// binary stays small, there is no OpenCV version lock-in, and the same code can
// move to RK3568 later. The handful of image primitives needed (bilinear / area
// resize, affine warp, centred padding) are implemented here.
//
// When the project is built without ONNX Runtime, create_hand_detector() returns
// a null detector whose init() fails loudly. The AI pipeline then simply refuses
// the hand mode; the person modes and the video stream are unaffected.

#include "camera_agent/ai/hand_detector.h"

#include "camera_agent/ai/hand_decode.h"
#include "camera_agent/logger.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <vector>

#ifdef CAMERA_AGENT_HAVE_ORT

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4127 4244 4245 4267 4996)
#endif
#include <onnxruntime_cxx_api.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <array>
#include <memory>
#include <string>

namespace ca {
namespace {

// ---------------------------------------------------------------------------
// Model geometry (mirrors the reference implementation - do not "tidy" these)
// ---------------------------------------------------------------------------
constexpr int kLandmarkInputSize = 224;
constexpr int kHandKeypointCount = 21;

constexpr float kPi = 3.14159265358979323846f;

// Palm ROI coefficients.
constexpr float kPalmBoxPreShift[2] = {0.0f, 0.0f};
constexpr float kPalmBoxPreEnlarge  = 4.0f;
constexpr float kPalmBoxShift[2]    = {0.0f, -0.4f};
constexpr float kPalmBoxEnlarge     = 3.0f;
// Hand bbox coefficients, applied to the 21 landmarks at the very end.
constexpr float kHandBoxShift[2]    = {0.0f, -0.1f};
constexpr float kHandBoxEnlarge     = 1.65f;

// Which palm keypoints define the hand's rotation axis.
constexpr int kPalmBaseIdx      = 0;   // wrist side
constexpr int kMidFingerBaseIdx = 2;   // middle-finger side

constexpr int kPalmNmsTopK = 5000;     // matches the reference's NMSBoxes top_k

// Spelled out so an `unsigned int` literal never narrows into a uint8_t buffer
// implicitly (MSVC /W4 flags that at the std::fill call site).
constexpr uint8_t kZeroByte = 0;

// ---------------------------------------------------------------------------
// Small image primitives
// ---------------------------------------------------------------------------

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline int clampi(int v, int lo, int hi) {
    if (hi < lo) hi = lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

// Bilinear resample of a tightly packed RGB image, matching cv::resize's
// INTER_LINEAR sample placement (src = (dst + 0.5) * scale - 0.5) with edge
// replication - which is exactly what OpenCV does at the borders.
void resize_rgb_linear(const uint8_t* src, int sw, int sh,
                       uint8_t* dst, int dw, int dh) {
    const float sx = static_cast<float>(sw) / static_cast<float>(dw);
    const float sy = static_cast<float>(sh) / static_cast<float>(dh);
    const float fx_max = static_cast<float>(sw - 1);
    const float fy_max = static_cast<float>(sh - 1);

    for (int y = 0; y < dh; ++y) {
        float fy = (static_cast<float>(y) + 0.5f) * sy - 0.5f;
        if (fy < 0.0f) fy = 0.0f;
        if (fy > fy_max) fy = fy_max;
        int y0 = static_cast<int>(fy);
        if (y0 > sh - 2) y0 = (sh >= 2) ? sh - 2 : 0;
        const int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
        const float wy = fy - static_cast<float>(y0);

        for (int x = 0; x < dw; ++x) {
            float fx = (static_cast<float>(x) + 0.5f) * sx - 0.5f;
            if (fx < 0.0f) fx = 0.0f;
            if (fx > fx_max) fx = fx_max;
            int x0 = static_cast<int>(fx);
            if (x0 > sw - 2) x0 = (sw >= 2) ? sw - 2 : 0;
            const int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            const float wx = fx - static_cast<float>(x0);

            const uint8_t* p00 = src + (static_cast<size_t>(y0) * sw + x0) * 3u;
            const uint8_t* p01 = src + (static_cast<size_t>(y0) * sw + x1) * 3u;
            const uint8_t* p10 = src + (static_cast<size_t>(y1) * sw + x0) * 3u;
            const uint8_t* p11 = src + (static_cast<size_t>(y1) * sw + x1) * 3u;
            uint8_t* q = dst + (static_cast<size_t>(y) * dw + x) * 3u;
            for (int c = 0; c < 3; ++c) {
                const float top = p00[c] * (1.0f - wx) + p01[c] * wx;
                const float bot = p10[c] * (1.0f - wx) + p11[c] * wx;
                q[c] = static_cast<uint8_t>(top * (1.0f - wy) + bot * wy + 0.5f);
            }
        }
    }
}

// Area-average resample: each output pixel is the weighted mean of the source
// interval it covers. This is cv::resize INTER_AREA, which the reference uses
// for the 224x224 landmark crop.
void resize_rgb_area(const uint8_t* src, int sw, int sh,
                     uint8_t* dst, int dw, int dh) {
    const float sx = static_cast<float>(sw) / static_cast<float>(dw);
    const float sy = static_cast<float>(sh) / static_cast<float>(dh);
    for (int y = 0; y < dh; ++y) {
        const float y0f = static_cast<float>(y) * sy;
        const float y1f = y0f + sy;
        const int iy0 = clampi(static_cast<int>(y0f), 0, sh - 1);
        const int iy1 = clampi(static_cast<int>(std::ceil(y1f)) - 1, 0, sh - 1);
        for (int x = 0; x < dw; ++x) {
            const float x0f = static_cast<float>(x) * sx;
            const float x1f = x0f + sx;
            const int ix0 = clampi(static_cast<int>(x0f), 0, sw - 1);
            const int ix1 = clampi(static_cast<int>(std::ceil(x1f)) - 1, 0, sw - 1);

            float acc[3] = {0.0f, 0.0f, 0.0f};
            float wsum = 0.0f;
            for (int syi = iy0; syi <= iy1; ++syi) {
                const float wy = std::min(y1f, static_cast<float>(syi) + 1.0f) -
                                 std::max(y0f, static_cast<float>(syi));
                if (wy <= 0.0f) continue;
                for (int sxi = ix0; sxi <= ix1; ++sxi) {
                    const float wx = std::min(x1f, static_cast<float>(sxi) + 1.0f) -
                                     std::max(x0f, static_cast<float>(sxi));
                    if (wx <= 0.0f) continue;
                    const float wgt = wx * wy;
                    const uint8_t* p =
                        src + (static_cast<size_t>(syi) * sw + sxi) * 3u;
                    acc[0] += static_cast<float>(p[0]) * wgt;
                    acc[1] += static_cast<float>(p[1]) * wgt;
                    acc[2] += static_cast<float>(p[2]) * wgt;
                    wsum += wgt;
                }
            }
            uint8_t* q = dst + (static_cast<size_t>(y) * dw + x) * 3u;
            if (wsum <= 0.0f) {
                q[0] = q[1] = q[2] = 0;
            } else {
                for (int c = 0; c < 3; ++c) {
                    q[c] = static_cast<uint8_t>(acc[c] / wsum + 0.5f);
                }
            }
        }
    }
}

// cv::resize's own rule: INTER_AREA only applies when BOTH axes shrink;
// otherwise the linear path is used.
void resize_rgb(const uint8_t* src, int sw, int sh,
                uint8_t* dst, int dw, int dh) {
    if (dw < sw && dh < sh) {
        resize_rgb_area(src, sw, sh, dst, dw, dh);
    } else {
        resize_rgb_linear(src, sw, sh, dst, dw, dh);
    }
}

// 2x3 affine transform, laid out exactly like cv::getRotationMatrix2D returns
// it: [ a b c ; d e f ] with a = cos, b = sin, d = -sin, e = cos.
struct Affine2x3 {
    float a = 1.0f, b = 0.0f, c = 0.0f;
    float d = 0.0f, e = 1.0f, f = 0.0f;
};

Affine2x3 rotation_matrix_2d(float cx, float cy, float angle_deg) {
    const float rad = angle_deg * (kPi / 180.0f);
    const float alpha = std::cos(rad);
    const float beta  = std::sin(rad);
    Affine2x3 m;
    m.a = alpha;
    m.b = beta;
    m.c = (1.0f - alpha) * cx - beta * cy;
    m.d = -beta;
    m.e = alpha;
    m.f = beta * cx + (1.0f - alpha) * cy;
    return m;
}

Affine2x3 invert_affine(const Affine2x3& m) {
    Affine2x3 inv;
    const float det = m.a * m.e - m.b * m.d;
    if (std::fabs(det) < 1e-12f) return inv;
    inv.a =  m.e / det;
    inv.b = -m.b / det;
    inv.c = (m.b * m.f - m.e * m.c) / det;
    inv.d = -m.d / det;
    inv.e =  m.a / det;
    inv.f = (m.d * m.c - m.a * m.f) / det;
    return inv;
}

// cv::warpAffine(..., INTER_LINEAR, BORDER_CONSTANT, 0): inverse mapping plus
// bilinear taps, with anything outside the source reading as black.
void warp_affine_rgb(const uint8_t* src, int sw, int sh,
                     uint8_t* dst, int dw, int dh, const Affine2x3& m) {
    const Affine2x3 inv = invert_affine(m);
    const float max_x = static_cast<float>(sw - 1);
    const float max_y = static_cast<float>(sh - 1);

    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            const float fx = inv.a * static_cast<float>(x) +
                             inv.b * static_cast<float>(y) + inv.c;
            const float fy = inv.d * static_cast<float>(x) +
                             inv.e * static_cast<float>(y) + inv.f;
            uint8_t* q = dst + (static_cast<size_t>(y) * dw + x) * 3u;
            if (fx < 0.0f || fy < 0.0f || fx > max_x || fy > max_y) {
                q[0] = q[1] = q[2] = 0;
                continue;
            }
            const int x0 = static_cast<int>(fx);
            const int y0 = static_cast<int>(fy);
            const int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            const int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
            const float wx = fx - static_cast<float>(x0);
            const float wy = fy - static_cast<float>(y0);
            const uint8_t* p00 = src + (static_cast<size_t>(y0) * sw + x0) * 3u;
            const uint8_t* p01 = src + (static_cast<size_t>(y0) * sw + x1) * 3u;
            const uint8_t* p10 = src + (static_cast<size_t>(y1) * sw + x0) * 3u;
            const uint8_t* p11 = src + (static_cast<size_t>(y1) * sw + x1) * 3u;
            for (int c = 0; c < 3; ++c) {
                const float top = p00[c] * (1.0f - wx) + p01[c] * wx;
                const float bot = p10[c] * (1.0f - wx) + p11[c] * wx;
                q[c] = static_cast<uint8_t>(top * (1.0f - wy) + bot * wy + 0.5f);
            }
        }
    }
}

// Result of one `_cropAndPadFromPalm` call.
struct CropResult {
    bool  ok     = false;
    int   side   = 0;        // padded square edge, in pixels
    float bias_x = 0.0f;     // palm_bbox[0] - (left, top)
    float bias_y = 0.0f;
    float box[2][2] = {};    // the clipped integer palm box, as floats
};

// Port of mp_handpose.py `_cropAndPadFromPalm`. The padded square lands in
// `scratch`; `box` is modified in place, matching the reference reassigning
// `palm_bbox`.
CropResult crop_and_pad(const uint8_t* src, int sw, int sh,
                        float box[2][2], bool for_rotation,
                        std::vector<uint8_t>& scratch) {
    CropResult r;

    float whx = box[1][0] - box[0][0];
    float why = box[1][1] - box[0][1];
    const float* shift = for_rotation ? kPalmBoxPreShift : kPalmBoxShift;
    box[0][0] += shift[0] * whx;
    box[0][1] += shift[1] * why;
    box[1][0] += shift[0] * whx;
    box[1][1] += shift[1] * why;

    const float cx = (box[0][0] + box[1][0]) * 0.5f;
    const float cy = (box[0][1] + box[1][1]) * 0.5f;
    whx = box[1][0] - box[0][0];
    why = box[1][1] - box[0][1];
    const float enlarge = for_rotation ? kPalmBoxPreEnlarge : kPalmBoxEnlarge;
    const float half_x = whx * enlarge * 0.5f;
    const float half_y = why * enlarge * 0.5f;

    // numpy `.astype(np.int32)` truncates toward zero, as does static_cast.
    int x0 = clampi(static_cast<int>(cx - half_x), 0, sw);
    int y0 = clampi(static_cast<int>(cy - half_y), 0, sh);
    int x1 = clampi(static_cast<int>(cx + half_x), 0, sw);
    int y1 = clampi(static_cast<int>(cy + half_y), 0, sh);
    if (x1 <= x0 || y1 <= y0) return r;   // empty crop: this palm is unusable

    box[0][0] = static_cast<float>(x0);
    box[0][1] = static_cast<float>(y0);
    box[1][0] = static_cast<float>(x1);
    box[1][1] = static_cast<float>(y1);

    const int cw = x1 - x0;
    const int ch = y1 - y0;
    const int side = for_rotation
                         ? static_cast<int>(std::sqrt(
                               static_cast<double>(cw) * cw +
                               static_cast<double>(ch) * ch))
                         : std::max(cw, ch);
    if (side <= 0) return r;

    const int left = (side - cw) / 2;
    const int top  = (side - ch) / 2;

    r.side   = side;
    r.bias_x = box[0][0] - static_cast<float>(left);
    r.bias_y = box[0][1] - static_cast<float>(top);
    r.box[0][0] = box[0][0];
    r.box[0][1] = box[0][1];
    r.box[1][0] = box[1][0];
    r.box[1][1] = box[1][1];

    scratch.assign(static_cast<size_t>(side) * side * 3u, kZeroByte);
    for (int y = 0; y < ch; ++y) {
        const size_t src_off = (static_cast<size_t>(y0 + y) * sw + x0) * 3u;
        const size_t dst_off = (static_cast<size_t>(top + y) * side + left) * 3u;
        std::copy(src + src_off, src + src_off + static_cast<size_t>(cw) * 3u,
                  scratch.begin() + static_cast<ptrdiff_t>(dst_off));
    }
    r.ok = true;
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// ONNX Runtime implementation
// ---------------------------------------------------------------------------

namespace {

class OnnxHandDetector : public IHandDetector {
public:
    bool init(const HandConfig& cfg) override;
    bool detect_hand(const uint8_t* rgb, int w, int h,
                     HandDetection& out) override;
    const char* backend_name() const override {
        return "onnxruntime(mediapipe-hand)";
    }
    int keypoint_count() const override {
        return ready_ ? kHandKeypointCount : 0;
    }

private:
    // One loaded model: weights (must outlive the session), session, I/O names.
    struct Model {
        std::vector<uint8_t>          bytes;
        std::unique_ptr<Ort::Session> session;
        std::vector<std::string>      in_names;
        std::vector<std::string>      out_names;

        void reset() {
            session.reset();
            bytes.clear();
            in_names.clear();
            out_names.clear();
        }
    };

    bool load_model(Model& m, const std::string& path);
    bool run_palm(const uint8_t* rgb, int w, int h,
                  std::vector<PalmBox>& palms, std::vector<float>& scores);
    bool run_landmark(const uint8_t* rgb, int w, int h, const PalmBox& palm,
                      HandDetection& out);

    HandConfig cfg_{};
    bool       ready_ = false;
    std::unique_ptr<Ort::Env> env_;
    Model      palm_;
    Model      lm_;

    // Anchor centres, built once from the recipe in hand_decode.cpp.
    std::vector<float> anchors_;

    // Scratch buffers, reused across frames and named by the stage they serve.
    std::vector<uint8_t> palm_resized_;   // frame scaled to fit 192x192
    std::vector<uint8_t> palm_canvas_;    // 192x192x3 letterboxed RGB
    std::vector<float>   palm_input_;     // 192x192x3 NHWC float, /255
    std::vector<uint8_t> lm_input_u8_;    // 224x224x3 resized crop
    std::vector<float>   lm_input_;       // 224x224x3 NHWC float, /255
    std::vector<uint8_t> crop_a_;         // stage-2 first crop (padded square)
    std::vector<uint8_t> rot_a_;          // stage-2 rotated square
    std::vector<uint8_t> crop_b_;         // stage-2 second crop (padded square)
    std::vector<PalmBox> palm_boxes_;
    std::vector<float>   palm_scores_;
    std::vector<int>     palm_keep_;
};

bool OnnxHandDetector::load_model(Model& m, const std::string& path) {
    m.reset();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        CA_LOG_ERROR("[HAND] model file not found: {}", path);
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff sz = f.tellg();
    if (sz <= 0) {
        CA_LOG_ERROR("[HAND] model file is empty: {}", path);
        return false;
    }
    f.seekg(0, std::ios::beg);
    m.bytes.resize(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(m.bytes.data()), sz);
    if (!f) {
        CA_LOG_ERROR("[HAND] failed to read model: {}", path);
        return false;
    }
    try {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(1);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        so.SetLogSeverityLevel(3);
        m.session = std::make_unique<Ort::Session>(*env_, m.bytes.data(),
                                                   m.bytes.size(), so);
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[HAND] session creation failed for {}: {}", path, e.what());
        m.reset();
        return false;
    }
    try {
        Ort::AllocatorWithDefaultOptions alloc;
        const size_t ni = m.session->GetInputCount();
        for (size_t i = 0; i < ni; ++i) {
            m.in_names.emplace_back(m.session->GetInputNameAllocated(i, alloc).get());
        }
        const size_t no = m.session->GetOutputCount();
        for (size_t i = 0; i < no; ++i) {
            m.out_names.emplace_back(m.session->GetOutputNameAllocated(i, alloc).get());
        }
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[HAND] could not query model I/O names: {}", e.what());
        m.reset();
        return false;
    }
    if (m.in_names.empty() || m.out_names.empty()) {
        CA_LOG_ERROR("[HAND] model {} exposes no I/O", path);
        m.reset();
        return false;
    }
    return true;
}

bool OnnxHandDetector::init(const HandConfig& cfg) {
    cfg_ = cfg;
    ready_ = false;
    if (cfg.max_hands < 1) {
        CA_LOG_ERROR("[HAND] hand.max_hands must be >= 1 (got {})", cfg.max_hands);
        return false;
    }
    if (cfg.palm_score_threshold <= 0.0f || cfg.palm_score_threshold >= 1.0f) {
        CA_LOG_ERROR("[HAND] hand.palm_score_threshold must be in (0,1) (got {})",
                     cfg.palm_score_threshold);
        return false;
    }

    try {
        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR,
                                          "camera-agent-hand");
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[HAND] ORT env creation failed: {}", e.what());
        return false;
    }
    if (!load_model(palm_, cfg.model_palm) ||
        !load_model(lm_, cfg.model_landmark)) {
        palm_.reset();
        lm_.reset();
        return false;
    }

    palm_build_anchors(anchors_);
    if (static_cast<int>(anchors_.size()) != kPalmAnchorCount * 2) {
        CA_LOG_ERROR("[HAND] anchor table has {} entries, expected {}",
                     anchors_.size() / 2, kPalmAnchorCount);
        palm_.reset();
        lm_.reset();
        return false;
    }

    palm_resized_.assign(static_cast<size_t>(kPalmInputSize) * kPalmInputSize * 3u, kZeroByte);
    palm_canvas_.assign(static_cast<size_t>(kPalmInputSize) * kPalmInputSize * 3u, kZeroByte);
    palm_input_.assign(static_cast<size_t>(kPalmInputSize) * kPalmInputSize * 3u, 0.0f);
    lm_input_u8_.assign(static_cast<size_t>(kLandmarkInputSize) *
                            kLandmarkInputSize * 3u, kZeroByte);
    lm_input_.assign(static_cast<size_t>(kLandmarkInputSize) *
                         kLandmarkInputSize * 3u, 0.0f);
    palm_boxes_.resize(static_cast<size_t>(kPalmAnchorCount));
    palm_scores_.resize(static_cast<size_t>(kPalmAnchorCount));
    palm_keep_.reserve(static_cast<size_t>(kPalmAnchorCount));

    ready_ = true;
    CA_LOG_INFO("[HAND] ready: palm={} ({} anchors) landmark={} ({} keypoints) "
                "max_hands={}",
                cfg.model_palm, palm_anchor_count(), cfg.model_landmark,
                kHandKeypointCount, cfg.max_hands);
    return true;
}

bool OnnxHandDetector::run_palm(const uint8_t* rgb, int w, int h,
                                std::vector<PalmBox>& palms,
                                std::vector<float>& scores) {
    palms.clear();
    scores.clear();

    // --- preprocess: aspect-preserving resize + centred BLACK padding --------
    // Note this letterbox pads with black (0,0,0), NOT the mid-grey 114 the YOLO
    // detector uses. That is what the model was trained on - feeding 114 here
    // measurably degrades palm scores.
    const double ratio = std::min(
        static_cast<double>(kPalmInputSize) / static_cast<double>(h),
        static_cast<double>(kPalmInputSize) / static_cast<double>(w));
    const int rw = static_cast<int>(static_cast<double>(w) * ratio);
    const int rh = static_cast<int>(static_cast<double>(h) * ratio);
    if (rw <= 0 || rh <= 0) return false;

    const int pad_w = kPalmInputSize - rw;
    const int pad_h = kPalmInputSize - rh;
    const int left = pad_w / 2;
    const int top  = pad_h / 2;

    if (w == kPalmInputSize && h == kPalmInputSize) {
        std::copy(rgb, rgb + static_cast<size_t>(w) * h * 3u, palm_canvas_.begin());
    } else {
        if (palm_resized_.size() < static_cast<size_t>(rw) * rh * 3u) {
            palm_resized_.resize(static_cast<size_t>(rw) * rh * 3u);
        }
        resize_rgb_linear(rgb, w, h, palm_resized_.data(), rw, rh);
        std::fill(palm_canvas_.begin(), palm_canvas_.end(), kZeroByte);
        for (int y = 0; y < rh; ++y) {
            const size_t src_off = static_cast<size_t>(y) * rw * 3u;
            const size_t dst_off =
                (static_cast<size_t>(top + y) * kPalmInputSize + left) * 3u;
            std::copy(palm_resized_.begin() + static_cast<ptrdiff_t>(src_off),
                      palm_resized_.begin() +
                          static_cast<ptrdiff_t>(src_off + static_cast<size_t>(rw) * 3u),
                      palm_canvas_.begin() + static_cast<ptrdiff_t>(dst_off));
        }
    }

    // The canvas is already interleaved RGB, so "to NHWC" is only the /255.
    for (size_t i = 0; i < palm_input_.size(); ++i) {
        palm_input_[i] = static_cast<float>(palm_canvas_[i]) / 255.0f;
    }
    // The reference re-expresses the padding in ORIGINAL image pixels by
    // dividing by the same ratio it scaled with.
    const float pad_x = static_cast<float>(
        static_cast<int>(static_cast<double>(left) / ratio));
    const float pad_y = static_cast<float>(
        static_cast<int>(static_cast<double>(top) / ratio));

    // --- forward ------------------------------------------------------------
    std::vector<Ort::Value> outs;
    try {
        const std::array<int64_t, 4> shape{1, kPalmInputSize, kPalmInputSize, 3};
        Ort::MemoryInfo mem =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> inputs;
        inputs.emplace_back(Ort::Value::CreateTensor<float>(
            mem, palm_input_.data(), palm_input_.size(), shape.data(), shape.size()));

        std::vector<const char*> in_names;
        in_names.reserve(palm_.in_names.size());
        for (const auto& n : palm_.in_names) in_names.push_back(n.c_str());
        std::vector<const char*> out_names;
        out_names.reserve(palm_.out_names.size());
        for (const auto& n : palm_.out_names) out_names.push_back(n.c_str());

        outs = palm_.session->Run(Ort::RunOptions{nullptr}, in_names.data(),
                                  inputs.data(), inputs.size(),
                                  out_names.data(), out_names.size());
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[HAND] palm inference failed: {}", e.what());
        return false;
    }
    if (outs.size() < 2) {
        CA_LOG_ERROR("[HAND] palm model returned {} outputs, expected 2",
                     outs.size());
        return false;
    }

    // Identify the branches by their channel count instead of trusting the
    // export order: the regressors carry 18 values per anchor, the classifier 1.
    const float* reg = nullptr;
    const float* cls = nullptr;
    int64_t rows = 0;
    for (auto& o : outs) {
        const std::vector<int64_t> s = o.GetTensorTypeAndShapeInfo().GetShape();
        if (s.size() != 3 || s[0] != 1) continue;
        if (s[1] == kPalmAnchorCount && s[2] == kPalmRegChannels) {
            reg  = o.GetTensorMutableData<float>();
            rows = s[1];
        } else if (s[1] == kPalmAnchorCount && s[2] == 1) {
            cls = o.GetTensorMutableData<float>();
        }
    }
    if (!reg || !cls || rows != kPalmAnchorCount) {
        CA_LOG_ERROR("[HAND] unexpected palm output shape (rows={}, reg={}, cls={})",
                     rows, reg != nullptr, cls != nullptr);
        return false;
    }

    // --- decode every anchor -------------------------------------------------
    const float scale = static_cast<float>(std::max(w, h));
    for (int64_t i = 0; i < rows; ++i) {
        // The exported graph applies no sigmoid on the classification branch.
        const float logit = cls[i];
        palm_scores_[static_cast<size_t>(i)] = 1.0f / (1.0f + std::exp(-logit));
        palm_boxes_[static_cast<size_t>(i)] =
            palm_decode_row(reg + i * kPalmRegChannels, anchors_,
                            static_cast<int>(i), scale, pad_x, pad_y);
    }

    palm_nms(palm_keep_, palm_boxes_, palm_scores_, cfg_.palm_score_threshold,
             cfg_.palm_nms_threshold, kPalmNmsTopK);


    const size_t take = std::min<size_t>(palm_keep_.size(),
                                         static_cast<size_t>(cfg_.max_hands));
    palms.reserve(take);
    scores.reserve(take);
    for (size_t i = 0; i < take; ++i) {
        const size_t idx = static_cast<size_t>(palm_keep_[i]);
        palms.push_back(palm_boxes_[idx]);
        scores.push_back(palm_scores_[idx]);
    }
    return true;
}

bool OnnxHandDetector::run_landmark(const uint8_t* rgb, int w, int h,
                                    const PalmBox& palm, HandDetection& out) {
    // ---- crop 1: the palm box, enlarged, padded to a square ----------------
    float box[2][2] = {{palm.x1, palm.y1}, {palm.x2, palm.y2}};
    const CropResult c1 = crop_and_pad(rgb, w, h, box, /*for_rotation=*/true,
                                       crop_a_);
    if (!c1.ok) return false;
    const float pad_bias_x = c1.bias_x;
    const float pad_bias_y = c1.bias_y;

    // Shift the palm box and its 7 keypoints into the padded-crop space.
    float pb[2][2] = {{c1.box[0][0] - pad_bias_x, c1.box[0][1] - pad_bias_y},
                      {c1.box[1][0] - pad_bias_x, c1.box[1][1] - pad_bias_y}};
    float lms[kPalmKeypointCount][2];
    for (int j = 0; j < kPalmKeypointCount; ++j) {
        lms[j][0] = palm.lm[j][0] - pad_bias_x;
        lms[j][1] = palm.lm[j][1] - pad_bias_y;
    }

    // ---- orientation from the palm-base -> middle-finger-base axis ---------
    const float dx = lms[kMidFingerBaseIdx][0] - lms[kPalmBaseIdx][0];
    const float dy = lms[kMidFingerBaseIdx][1] - lms[kPalmBaseIdx][1];
    float radians = kPi * 0.5f - std::atan2(-dy, dx);
    radians -= 2.0f * kPi * std::floor((radians + kPi) / (2.0f * kPi));
    const float angle_deg = radians * (180.0f / kPi);

    const float ccx = (pb[0][0] + pb[1][0]) * 0.5f;
    const float ccy = (pb[0][1] + pb[1][1]) * 0.5f;
    const Affine2x3 rot = rotation_matrix_2d(ccx, ccy, angle_deg);

    rot_a_.assign(crop_a_.size(), kZeroByte);
    warp_affine_rgb(crop_a_.data(), c1.side, c1.side, rot_a_.data(),
                    c1.side, c1.side, rot);

    // Bounding box of the rotated palm keypoints (rotation only - no padding).
    float rx0 = lms[0][0] * rot.a + lms[0][1] * rot.b + rot.c;
    float ry0 = lms[0][0] * rot.d + lms[0][1] * rot.e + rot.f;
    float rx1 = rx0, ry1 = ry0;
    for (int j = 1; j < kPalmKeypointCount; ++j) {
        const float rx = lms[j][0] * rot.a + lms[j][1] * rot.b + rot.c;
        const float ry = lms[j][0] * rot.d + lms[j][1] * rot.e + rot.f;
        rx0 = std::min(rx0, rx);
        rx1 = std::max(rx1, rx);
        ry0 = std::min(ry0, ry);
        ry1 = std::max(ry1, ry);
    }
    float rbox[2][2] = {{rx0, ry0}, {rx1, ry1}};
    // `rbox` is reassigned by the call, exactly like the reference's
    // `rotated_palm_bbox` - the postprocess below relies on the clipped value.
    const CropResult c2 = crop_and_pad(rot_a_.data(), c1.side, c1.side, rbox,
                                       /*for_rotation=*/false, crop_b_);
    if (!c2.ok) return false;

    resize_rgb(crop_b_.data(), c2.side, c2.side, lm_input_u8_.data(),
               kLandmarkInputSize, kLandmarkInputSize);
    for (size_t i = 0; i < lm_input_.size(); ++i) {
        lm_input_[i] = static_cast<float>(lm_input_u8_[i]) / 255.0f;
    }

    // ---- forward -----------------------------------------------------------
    std::vector<Ort::Value> outs;
    try {
        const std::array<int64_t, 4> shape{1, kLandmarkInputSize,
                                           kLandmarkInputSize, 3};
        Ort::MemoryInfo mem =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> inputs;
        inputs.emplace_back(Ort::Value::CreateTensor<float>(
            mem, lm_input_.data(), lm_input_.size(), shape.data(), shape.size()));

        std::vector<const char*> in_names;
        in_names.reserve(lm_.in_names.size());
        for (const auto& n : lm_.in_names) in_names.push_back(n.c_str());
        std::vector<const char*> out_names;
        out_names.reserve(lm_.out_names.size());
        for (const auto& n : lm_.out_names) out_names.push_back(n.c_str());

        outs = lm_.session->Run(Ort::RunOptions{nullptr}, in_names.data(),
                                inputs.data(), inputs.size(),
                                out_names.data(), out_names.size());
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[HAND] landmark inference failed: {}", e.what());
        return false;
    }
    if (outs.size() < 4) {
        CA_LOG_ERROR("[HAND] landmark model returned {} outputs, expected 4",
                     outs.size());
        return false;
    }

    // The export order is (screen landmarks, confidence, handedness, world
    // landmarks); the reference unpacks it positionally, and the shape assertions
    // below make sure that assumption still holds for this model file.
    const std::vector<int64_t> s0 = outs[0].GetTensorTypeAndShapeInfo().GetShape();
    const std::vector<int64_t> s1 = outs[1].GetTensorTypeAndShapeInfo().GetShape();
    const std::vector<int64_t> s2 = outs[2].GetTensorTypeAndShapeInfo().GetShape();
    if (s0.size() != 2 || s0[1] != 3 * kHandKeypointCount ||
        s1.size() != 2 || s1[1] != 1 ||
        s2.size() != 2 || s2[1] != 1) {
        CA_LOG_ERROR("[HAND] unexpected landmark output shapes");
        return false;
    }
    const float* lm_data = outs[0].GetTensorMutableData<float>();
    const float conf = outs[1].GetTensorMutableData<float>()[0];
    const float handed = outs[2].GetTensorMutableData<float>()[0];
    if (conf < cfg_.landmark_confidence) return false;

    // ---- map the 21 landmarks back to ORIGINAL video pixels ----------------
    const float wh_rot_x = rbox[1][0] - rbox[0][0];
    const float wh_rot_y = rbox[1][1] - rbox[0][1];
    const float sf = std::max(wh_rot_x / static_cast<float>(kLandmarkInputSize),
                              wh_rot_y / static_cast<float>(kLandmarkInputSize));
    const float half = static_cast<float>(kLandmarkInputSize) * 0.5f;

    // Rotation about the origin, used to bring the crop-space landmarks back
    // into the un-rotated frame. The reference computes
    // `np.dot(pts, getRotationMatrix2D((0,0), angle)[:, :2])`, i.e. a row-vector
    // product, which is the TRANSPOSE of `rot`'s rotation part.
    const Affine2x3 rz = rotation_matrix_2d(0.0f, 0.0f, angle_deg);

    // Inverse of the centred rotation, to recover the crop centre in the
    // pre-rotation frame.
    const float rc00 = rot.a, rc01 = rot.d;
    const float rc10 = rot.b, rc11 = rot.e;
    const float tx = rot.c, ty = rot.f;
    const float itx = -(rc00 * tx + rc01 * ty);
    const float ity = -(rc10 * tx + rc11 * ty);
    const float ccx2 = (c2.box[0][0] + c2.box[1][0]) * 0.5f;
    const float ccy2 = (c2.box[0][1] + c2.box[1][1]) * 0.5f;
    const float ocx = rc00 * ccx2 + rc01 * ccy2 + itx;
    const float ocy = rc10 * ccx2 + rc11 * ccy2 + ity;

    out = HandDetection{};
    out.landmarks.resize(static_cast<size_t>(kHandKeypointCount));
    for (int j = 0; j < kHandKeypointCount; ++j) {
        const float nx = (lm_data[j * 3 + 0] - half) * sf;
        const float ny = (lm_data[j * 3 + 1] - half) * sf;
        const float nz = lm_data[j * 3 + 2] * sf;
        const float ux = nx * rz.a + ny * rz.d;
        const float uy = nx * rz.b + ny * rz.e;
        Keypoint& k = out.landmarks[static_cast<size_t>(j)];
        k.x = ux + ocx + pad_bias_x;
        k.y = uy + ocy + pad_bias_y;
        k.z = nz;
        k.conf = conf;
    }

    // ---- hand bbox derived from the landmarks (shift + enlarge) ------------
    float bx0 = out.landmarks[0].x, bx1 = bx0;
    float by0 = out.landmarks[0].y, by1 = by0;
    for (const auto& k : out.landmarks) {
        bx0 = std::min(bx0, k.x);
        bx1 = std::max(bx1, k.x);
        by0 = std::min(by0, k.y);
        by1 = std::max(by1, k.y);
    }
    const float wb_x = bx1 - bx0;
    const float wb_y = by1 - by0;
    bx0 += kHandBoxShift[0] * wb_x;
    bx1 += kHandBoxShift[0] * wb_x;
    by0 += kHandBoxShift[1] * wb_y;
    by1 += kHandBoxShift[1] * wb_y;
    const float ccx3 = (bx0 + bx1) * 0.5f;
    const float ccy3 = (by0 + by1) * 0.5f;
    const float hx = (bx1 - bx0) * kHandBoxEnlarge * 0.5f;
    const float hy = (by1 - by0) * kHandBoxEnlarge * 0.5f;

    out.detected   = true;
    out.confidence = conf;
    out.x1 = clampf(ccx3 - hx, 0.0f, static_cast<float>(w > 0 ? w - 1 : 0));
    out.y1 = clampf(ccy3 - hy, 0.0f, static_cast<float>(h > 0 ? h - 1 : 0));
    out.x2 = clampf(ccx3 + hx, 0.0f, static_cast<float>(w));
    out.y2 = clampf(ccy3 + hy, 0.0f, static_cast<float>(h));
    out.handedness = (handed <= 0.5f) ? Handedness::Left : Handedness::Right;
    return true;
}

bool OnnxHandDetector::detect_hand(const uint8_t* rgb, int w, int h,
                                   HandDetection& out) {
    out = HandDetection{};
    if (!ready_ || !rgb || w <= 0 || h <= 0) return false;

    std::vector<PalmBox> palms;
    std::vector<float> scores;
    bool ok = false;
    try {
        ok = run_palm(rgb, w, h, palms, scores);
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[HAND] palm stage threw: {}", e.what());
        return false;
    }
    if (!ok) return false;
    if (palms.empty()) return true;   // no hand present is not an error

    // `palm_nms` already returns the survivors sorted best-first, so palms[0] is
    // the highest-scoring hand - Phase 1 tracks a single hand (spec 2 / spec 32).
    try {
        if (!run_landmark(rgb, w, h, palms[0], out)) {
            out = HandDetection{};
        }
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[HAND] landmark stage threw: {}", e.what());
        out = HandDetection{};
    }
    return true;
}

} // namespace
} // namespace ca

#endif // CAMERA_AGENT_HAVE_ORT

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
namespace ca {

#ifndef CAMERA_AGENT_HAVE_ORT
namespace {

// Built without ONNX Runtime: the hand mode is simply unavailable, and init()
// says so loudly instead of letting the pipeline run a detector that cannot work.
class NullHandDetector : public IHandDetector {
public:
    bool init(const HandConfig& cfg) override {
        CA_LOG_ERROR("[HAND] this build has no ONNX Runtime; cannot load {} / {}",
                     cfg.model_palm, cfg.model_landmark);
        return false;
    }
    bool detect_hand(const uint8_t*, int, int, HandDetection&) override {
        return false;
    }
    const char* backend_name() const override { return "none"; }
    int keypoint_count() const override { return 0; }
};

} // namespace
#endif

std::unique_ptr<IHandDetector> create_hand_detector() {
#ifdef CAMERA_AGENT_HAVE_ORT
    return std::unique_ptr<IHandDetector>(new OnnxHandDetector());
#else
    return std::unique_ptr<IHandDetector>(new NullHandDetector());
#endif
}

} // namespace ca
