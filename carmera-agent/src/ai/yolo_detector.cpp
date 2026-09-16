// YOLO detector, zero external dependencies besides ONNX Runtime.
//
// Deliberately NO OpenCV: letterbox resize, NMS and the YOLOv8/YOLO11 output
// decode (detection and pose heads) are implemented here. That keeps the binary small, avoids an
// OpenCV version lock-in, and - more importantly - lets the same code move to
// RK3568 unchanged (cross-compiling OpenCV there is a much bigger job).
//
// When the project is built without ONNX Runtime, create_detector() returns a
// null detector whose init() fails loudly; the AI pipeline then disables itself
// and the video stream keeps running (spec 19).

#include "camera_agent/ai/detector.h"
#include "camera_agent/ai/yolo_decode.h"
#include "camera_agent/logger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
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
#endif

namespace ca {
namespace {

// YOLO letterbox convention: pad with mid-grey 114.
constexpr uint8_t kPadValue = 114;

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Map a source coordinate (in pixels) to the two neighbouring taps + weight.
inline void bilinear_axis(float f, int n, int& i0, int& i1, float& w) {
    if (n < 2) { i0 = 0; i1 = 0; w = 0.0f; return; }
    const float fmax = static_cast<float>(n - 1);
    if (f < 0.0f) f = 0.0f;
    if (f > fmax) f = fmax;
    int i = static_cast<int>(f);
    if (i > n - 2) i = n - 2;
    if (i < 0) i = 0;
    i0 = i;
    i1 = i + 1;
    w = f - static_cast<float>(i0);
}

// Aspect-preserving resize: scale so the whole frame fits the canvas, then
// center it and fill the remaining bands with kPadValue. Returns the scale and
// padding so detection boxes can be mapped back to source pixels exactly.
void letterbox(const uint8_t* src, int sw, int sh,
               uint8_t* dst, int dw, int dh,
               float& scale_out, int& pad_x_out, int& pad_y_out) {
    const float r = std::min(static_cast<float>(dw) / static_cast<float>(sw),
                             static_cast<float>(dh) / static_cast<float>(sh));
    int nw = static_cast<int>(static_cast<float>(sw) * r + 0.5f);
    int nh = static_cast<int>(static_cast<float>(sh) * r + 0.5f);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw > dw) nw = dw;
    if (nh > dh) nh = dh;

    const int pad_x = (dw - nw) / 2;
    const int pad_y = (dh - nh) / 2;
    scale_out = r;
    pad_x_out = pad_x;
    pad_y_out = pad_y;

    std::fill(dst, dst + static_cast<size_t>(dw) * static_cast<size_t>(dh) * 3u,
              kPadValue);

    for (int y = 0; y < nh; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) / r - 0.5f;
        int y0 = 0, y1 = 0;
        float wy = 0.0f;
        bilinear_axis(fy, sh, y0, y1, wy);
        const size_t row0 = static_cast<size_t>(y0) * static_cast<size_t>(sw);
        const size_t row1 = static_cast<size_t>(y1) * static_cast<size_t>(sw);
        const size_t orow = static_cast<size_t>(y + pad_y) * static_cast<size_t>(dw);

        for (int x = 0; x < nw; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) / r - 0.5f;
            int x0 = 0, x1 = 0;
            float wx = 0.0f;
            bilinear_axis(fx, sw, x0, x1, wx);

            const uint8_t* p00 = src + (row0 + static_cast<size_t>(x0)) * 3u;
            const uint8_t* p01 = src + (row0 + static_cast<size_t>(x1)) * 3u;
            const uint8_t* p10 = src + (row1 + static_cast<size_t>(x0)) * 3u;
            const uint8_t* p11 = src + (row1 + static_cast<size_t>(x1)) * 3u;

            uint8_t* q = dst + (orow + static_cast<size_t>(x + pad_x)) * 3u;
            for (int c = 0; c < 3; ++c) {
                const float top = static_cast<float>(p00[c]) * (1.0f - wx) +
                                  static_cast<float>(p01[c]) * wx;
                const float bot = static_cast<float>(p10[c]) * (1.0f - wx) +
                                  static_cast<float>(p11[c]) * wx;
                const float v = top * (1.0f - wy) + bot * wy;
                q[c] = static_cast<uint8_t>(v + 0.5f);
            }
        }
    }
}

inline float iou_xyxy(const Detection& a, const Detection& b) {
    const float ix1 = std::max(a.x1, b.x1);
    const float iy1 = std::max(a.y1, b.y1);
    const float ix2 = std::min(a.x2, b.x2);
    const float iy2 = std::min(a.y2, b.y2);
    const float iw = ix2 - ix1;
    const float ih = iy2 - iy1;
    if (iw <= 0.0f || ih <= 0.0f) return 0.0f;
    const float inter = iw * ih;
    const float ua = (a.x2 - a.x1) * (a.y2 - a.y1) +
                     (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
    if (ua <= 0.0f) return 0.0f;
    return inter / ua;
}

// Greedy non-maximum suppression, applied per class.
void nms(std::vector<Detection>& dets, float thr) {
    if (dets.empty()) return;
    std::sort(dets.begin(), dets.end(),
              [](const Detection& a, const Detection& b) {
                  return a.confidence > b.confidence;
              });
    std::vector<char> keep(dets.size(), 1);
    for (size_t i = 0; i < dets.size(); ++i) {
        if (!keep[i]) continue;
        for (size_t j = i + 1; j < dets.size(); ++j) {
            if (!keep[j]) continue;
            if (dets[i].class_id != dets[j].class_id) continue;
            if (iou_xyxy(dets[i], dets[j]) > thr) keep[j] = 0;
        }
    }
    std::vector<Detection> out;
    out.reserve(dets.size());
    for (size_t i = 0; i < dets.size(); ++i)
        if (keep[i]) out.push_back(dets[i]);
    dets.swap(out);
}

} // namespace

// ---- shared, testable decode (declared in yolo_decode.h) -------------------

int keypoint_count_from_channels(int64_t channels) {
    const int64_t min_pose_channels = 5 + 3 * 4;   // 4 box/cls + >=4 kpts
    if (channels < min_pose_channels) return 0;
    const int64_t rest = channels - 5;
    if (rest % 3 != 0) return 0;
    return static_cast<int>(rest / 3);
}

void decode_yolo_output(const float* data, int64_t rows, int64_t cols,
                        bool channel_major, int want_class, float floor_score,
                        int ow, int oh, float scale, int pad_x, int pad_y,
                        std::vector<Detection>& out) {
    out.clear();

    const int64_t channels = channel_major ? rows : cols;
    const int64_t anchors  = channel_major ? cols : rows;
    if (channels <= 4 || anchors <= 0) {
        CA_LOG_ERROR("[AI] unexpected model output shape {}x{}", rows, cols);
        return;
    }

    const int kpts = keypoint_count_from_channels(channels);
    const int64_t num_classes =
        channels - 4 - static_cast<int64_t>(kpts) * 3;
    if (want_class < 0 || want_class >= num_classes) {
        CA_LOG_ERROR("[AI] model has {} classes; class id {} is out of range",
                     num_classes, want_class);
        return;
    }

    auto at = [&](int64_t a, int64_t c) -> float {
        return channel_major ? data[c * anchors + a] : data[a * channels + c];
    };

    const float max_x = static_cast<float>(ow - 1);
    const float max_y = static_cast<float>(oh - 1);

    for (int64_t a = 0; a < anchors; ++a) {
        const float score = at(a, 4 + want_class);
        if (score < floor_score) continue;

        const float cx = at(a, 0);
        const float cy = at(a, 1);
        const float bw = at(a, 2);
        const float bh = at(a, 3);

        Detection d;
        // Undo the letterbox: subtract padding, then divide by the scale.
        d.x1 = clampf((cx - bw * 0.5f - static_cast<float>(pad_x)) / scale, 0.0f, max_x);
        d.y1 = clampf((cy - bh * 0.5f - static_cast<float>(pad_y)) / scale, 0.0f, max_y);
        d.x2 = clampf((cx + bw * 0.5f - static_cast<float>(pad_x)) / scale, 0.0f, max_x);
        d.y2 = clampf((cy + bh * 0.5f - static_cast<float>(pad_y)) / scale, 0.0f, max_y);
        if (d.x2 <= d.x1 || d.y2 <= d.y1) continue;

        d.confidence = score;
        d.class_id   = want_class;
        d.class_name = "person";

        if (kpts > 0) {
            d.keypoints.resize(static_cast<size_t>(kpts));
            for (int j = 0; j < kpts; ++j) {
                const int64_t base = 5 + static_cast<int64_t>(j) * 3;
                Keypoint& k = d.keypoints[static_cast<size_t>(j)];
                k.x = clampf((at(a, base + 0) - static_cast<float>(pad_x)) / scale,
                             0.0f, max_x);
                k.y = clampf((at(a, base + 1) - static_cast<float>(pad_y)) / scale,
                             0.0f, max_y);
                // The pose head already applies the sigmoid inside the exported
                // graph. Clamp numeric drift only - NEVER sigmoid again here.
                k.conf = clampf(at(a, base + 2), 0.0f, 1.0f);
            }
        }
        out.push_back(std::move(d));
    }
}

#ifdef CAMERA_AGENT_HAVE_ORT

class OnnxYoloDetector : public IDetector {
public:
    bool init(const DetectorConfig& cfg) override;
    bool detect(const uint8_t* rgb, int w, int h,
                std::vector<Detection>& out) override;
    const char* backend_name() const override { return "onnxruntime"; }
    int keypoint_count() const override { return kpt_count_; }

private:
    DetectorConfig        cfg_{};
    std::vector<uint8_t>  model_bytes_;   // must outlive the session
    std::unique_ptr<Ort::Env>     env_;
    std::unique_ptr<Ort::Session> session_;
    std::string           in_name_, out_name_;
    std::vector<float>    input_;    // NCHW normalized
    std::vector<uint8_t>  canvas_;   // letterboxed RGB
    bool                  have_io_names_ = false;
    // 0 = detection model; >0 = pose head with this many keypoints (COCO: 17).
    int                   kpt_count_ = 0;

    // Query the model's own opinion about its mode. Preference: the embedded
    // `kpt_shape` metadata (Ultralytics exports "[17, 3]" on pose models),
    // then the output tensor shape (C = 5 + 3*K).
    void probe_mode();
};

void OnnxYoloDetector::probe_mode() {
    int kpt = 0;
    try {
        Ort::AllocatorWithDefaultOptions alloc;
        auto kpt_shape = session_->GetModelMetadata()
                             .LookupCustomMetadataMapAllocated("kpt_shape", alloc);
        if (kpt_shape) {
            const char* s = kpt_shape.get();
            while (*s && (*s < '0' || *s > '9')) ++s;
            if (*s) kpt = std::atoi(s);   // "[17, 3]" -> 17
        }
    } catch (const std::exception&) {
        kpt = 0;
    }
    if (kpt <= 0) {
        try {
            const std::vector<int64_t> shape =
                session_->GetOutputTypeInfo(0)
                    .GetTensorTypeAndShapeInfo()
                    .GetShape();
            if (shape.size() == 3) {
                const int64_t c = shape[1] < shape[2] ? shape[1] : shape[2];
                kpt = keypoint_count_from_channels(c);
            }
        } catch (const std::exception&) {
            kpt = 0;
        }
    }
    kpt_count_ = kpt > 0 ? kpt : 0;
    if (kpt_count_ > 0) {
        CA_LOG_INFO("[AI] pose model detected: {} keypoints per object",
                    kpt_count_);
    }
}

bool OnnxYoloDetector::init(const DetectorConfig& cfg) {
    cfg_ = cfg;

    // Load the model into memory first. Besides being a single code path, this
    // sidesteps ORTCHAR_T (wchar_t on Windows) path handling entirely.
    std::ifstream f(cfg.model_path, std::ios::binary);
    if (!f) {
        CA_LOG_ERROR("[AI] model file not found: {}", cfg.model_path);
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff sz = f.tellg();
    if (sz <= 0) {
        CA_LOG_ERROR("[AI] model file is empty: {}", cfg.model_path);
        return false;
    }
    f.seekg(0, std::ios::beg);
    model_bytes_.resize(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(model_bytes_.data()), sz);
    if (!f) {
        CA_LOG_ERROR("[AI] failed to read model: {}", cfg.model_path);
        return false;
    }

    try {
        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "camera-agent");
        Ort::SessionOptions so;
        if (cfg.num_threads > 0) so.SetIntraOpNumThreads(cfg.num_threads);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        so.SetLogSeverityLevel(3);
        session_ = std::make_unique<Ort::Session>(
            *env_, model_bytes_.data(), model_bytes_.size(), so);
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[AI] ONNX Runtime session creation failed: {}", e.what());
        session_.reset();
        env_.reset();
        return false;
    }

    try {
        Ort::AllocatorWithDefaultOptions alloc;
        in_name_  = std::string(session_->GetInputNameAllocated(0, alloc).get());
        out_name_ = std::string(session_->GetOutputNameAllocated(0, alloc).get());
        have_io_names_ = true;
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[AI] could not query model I/O names: {}", e.what());
        return false;
    }

    canvas_.resize(static_cast<size_t>(cfg.input_width) *
                   static_cast<size_t>(cfg.input_height) * 3u);
    input_.resize(static_cast<size_t>(3) *
                  static_cast<size_t>(cfg.input_width) *
                  static_cast<size_t>(cfg.input_height));

    probe_mode();
    return true;
}

bool OnnxYoloDetector::detect(const uint8_t* rgb, int w, int h,
                              std::vector<Detection>& out) {
    out.clear();
    if (!session_ || !have_io_names_ || !rgb || w <= 0 || h <= 0) return false;

    const int iw = cfg_.input_width;
    const int ih = cfg_.input_height;
    if (canvas_.size() < static_cast<size_t>(iw) * ih * 3u) return false;

    float scale = 1.0f;
    int   pad_x = 0, pad_y = 0;
    letterbox(rgb, w, h, canvas_.data(), iw, ih, scale, pad_x, pad_y);

    // HWC -> CHW, /255 (YOLOv8 expects float32 RGB in [0,1]).
    const size_t plane = static_cast<size_t>(iw) * static_cast<size_t>(ih);
    for (size_t i = 0; i < plane; ++i) {
        input_[i]             = static_cast<float>(canvas_[i * 3u + 0u]) / 255.0f;
        input_[plane + i]     = static_cast<float>(canvas_[i * 3u + 1u]) / 255.0f;
        input_[2 * plane + i] = static_cast<float>(canvas_[i * 3u + 2u]) / 255.0f;
    }

    try {
        const std::array<int64_t, 4> shape{1, 3, static_cast<int64_t>(ih),
                                           static_cast<int64_t>(iw)};
        Ort::MemoryInfo mem =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> inputs;
        inputs.emplace_back(Ort::Value::CreateTensor<float>(
            mem, input_.data(), input_.size(), shape.data(), shape.size()));

        const char* in_names[]  = {in_name_.c_str()};
        const char* out_names[] = {out_name_.c_str()};

        std::vector<Ort::Value> outs = session_->Run(
            Ort::RunOptions{nullptr}, in_names, inputs.data(), 1, out_names, 1);
        if (outs.empty()) return false;

        Ort::TensorTypeAndShapeInfo tinfo = outs[0].GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> oshape = tinfo.GetShape();
        if (oshape.size() != 3 || oshape[0] != 1) {
            CA_LOG_ERROR("[AI] unexpected model output rank ({})", oshape.size());
            out.clear();
            return false;
        }
        // {1, C, N} with C < N is channel-major (YOLOv8/11 default export);
        // otherwise assume anchor-major. Both are handled by the shared decode.
        const bool channel_major = (oshape[1] < oshape[2]);
        decode_yolo_output(outs[0].GetTensorMutableData<float>(),
                           oshape[1], oshape[2], channel_major,
                           cfg_.class_id, cfg_.low_confidence,
                           w, h, scale, pad_x, pad_y, out);
        nms(out, cfg_.nms_threshold);
        return true;
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[AI] inference failed: {}", e.what());
        out.clear();
        return false;
    }
}

#else // !CAMERA_AGENT_HAVE_ORT

class NullDetector : public IDetector {
public:
    bool init(const DetectorConfig& cfg) override {
        CA_LOG_ERROR("[AI] this build has no ONNX Runtime; cannot load {}",
                     cfg.model_path);
        return false;
    }
    bool detect(const uint8_t*, int, int, std::vector<Detection>&) override {
        return false;
    }
    const char* backend_name() const override { return "none"; }
};

#endif

std::unique_ptr<IDetector> create_detector() {
#ifdef CAMERA_AGENT_HAVE_ORT
    return std::make_unique<OnnxYoloDetector>();
#else
    return std::make_unique<NullDetector>();
#endif
}

} // namespace ca
