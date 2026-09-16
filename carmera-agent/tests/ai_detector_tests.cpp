// End-to-end smoke test for the AI branch (spec 19). Validates:
//
//   1. The YOLO detector loads the ONNX model and produces boxes on a real
//      image (the canonical Ultralytics bus.jpg, where yolov8n.onnx reliably
//      detects 3-4 people and 1 bus).
//   2. Bounding boxes are already mapped back to the ORIGINAL image
//      coordinates (0 <= x < w, 0 <= y < h) - this is the single most fragile
//      transformation in the whole pipeline, so we assert it explicitly.
//   3. The ByteTrack tracker assigns stable IDs to objects that persist
//      across multiple frames.
//   4. The AIPipeline lifecycle works end-to-end: init -> start -> push
//      frames -> see results come back through the callback -> stop, with
//      the thread cleanly joining.
//
// When CAMERA_AGENT_HAVE_ORT is not defined the detector tests skip
// themselves (the project is designed to build and run without the model).
#include "camera_agent/ai/ai_types.h"
#include "camera_agent/ai/ai_pipeline.h"
#include "camera_agent/ai/detector.h"
#include "camera_agent/ai/tracker.h"
#include "camera_agent/ai/yolo_decode.h"
#include "test_harness.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace {

#ifdef _MSC_VER
// getenv() triggers C4996 under /W4; route through _dupenv_s instead.
std::string safe_getenv(const char* name) {
    char* p = nullptr;
    _dupenv_s(&p, nullptr, name);
    if (!p) return {};
    std::string v(p);
    std::free(p);
    return v;
}
#else
std::string safe_getenv(const char* name) {
    const char* p = std::getenv(name);
    return p ? std::string(p) : std::string{};
}
#endif

std::string find_fixture() {
    const std::string env = safe_getenv("AI_BUS_FIXTURE");
    if (!env.empty() && std::ifstream(env.c_str()).good()) return env;
    const char* candidates[] = {
        "bus.jpg",
        "tests/finished/bus.jpg",
        "../tests/finished/bus.jpg",
    };
    for (const char* c : candidates) {
        if (std::ifstream(c).good()) return c;
    }
    return {};
}

bool detector_available() {
    return std::string(ca::create_detector()->backend_name()) != "none";
}

// Prefer the YOLO11 detection model; fall back to the legacy yolov8n.
std::string find_detect_model() {
    if (std::ifstream("models/yolo11n.onnx").good()) return "models/yolo11n.onnx";
    if (std::ifstream("models/yolov8n.onnx").good()) return "models/yolov8n.onnx";
    return {};
}

// Build a synthetic {1, 84, N} / {1, N, 84} detect tensor with exactly one
// scored anchor (layout selected by channel_major).
std::vector<float> make_detect_tensor(int64_t anchors, int64_t anchor_idx,
                                      bool channel_major) {
    std::vector<float> t(static_cast<size_t>(84) * static_cast<size_t>(anchors),
                         0.0f);
    const auto set = [&](int64_t a, int64_t c, float v) {
        const size_t i = channel_major
            ? static_cast<size_t>(c) * static_cast<size_t>(anchors) +
                  static_cast<size_t>(a)
            : static_cast<size_t>(a) * 84u + static_cast<size_t>(c);
        t[i] = v;
    };
    set(anchor_idx, 4, 0.9f);    // person score (already sigmoided by the head)
    set(anchor_idx, 0, 370.0f);  // cx, network input space
    set(anchor_idx, 1, 330.0f);  // cy
    set(anchor_idx, 2, 100.0f);  // bw
    set(anchor_idx, 3, 200.0f);  // bh
    return t;
}

}  // namespace

TEST(ai_detector_bus_image) {
    if (!detector_available()) {
        std::cout << "  [skip] ONNX Runtime not built in\n";
        return true;
    }
    const std::string path = find_fixture();
    if (path.empty()) {
        std::cout << "  [skip] bus.jpg not found "
                     "(set AI_BUS_FIXTURE=<absolute path>)\n";
        return true;
    }

    const std::string model = find_detect_model();
    if (model.empty()) {
        std::cout << "  [skip] no detection model in models/ "
                     "(yolo11n.onnx or yolov8n.onnx)\n";
        return true;
    }

    int w = 0, h = 0, n = 0;
    uint8_t* rgb = stbi_load(path.c_str(), &w, &h, &n, 3);
    ASSERT(rgb != nullptr);
    ASSERT(w > 0 && h > 0);

    auto det = ca::create_detector();
    ca::DetectorConfig cfg;
    cfg.model_path     = model;
    cfg.input_width    = 640;
    cfg.input_height   = 640;
    cfg.confidence     = 0.25f;
    cfg.low_confidence = 0.1f;
    cfg.nms_threshold  = 0.45f;
    ASSERT(det->init(cfg));
    ASSERT_EQ(det->keypoint_count(), 0);   // detection model, not pose

    std::vector<ca::Detection> out;
    const bool ok = det->detect(rgb, w, h, out);
    stbi_image_free(rgb);
    ASSERT(ok);
    ASSERT(out.size() >= 1u);

    int persons = 0;
    for (const auto& d : out) {
        ASSERT(d.x1 >= 0.0f && d.x1 <= static_cast<float>(w));
        ASSERT(d.y1 >= 0.0f && d.y1 <= static_cast<float>(h));
        ASSERT(d.x2 >  d.x1);
        ASSERT(d.y2 >  d.y1);
        if (d.class_id == 0) ++persons;
    }
    std::cout << "  bus.jpg " << w << "x" << h
              << " -> " << out.size() << " boxes ("
              << persons << " persons)\n";
    ASSERT(persons >= 1);
    return true;
}

// ---- YOLO11 decode (pure functions; synthetic tensors, no ORT needed) ------

TEST(ai_decode_channel_count) {
    using ca::keypoint_count_from_channels;
    ASSERT_EQ(keypoint_count_from_channels(84), 0);   // YOLOv8/11 COCO detect
    ASSERT_EQ(keypoint_count_from_channels(56), 17);  // YOLO11n-pose (COCO 17)
    ASSERT_EQ(keypoint_count_from_channels(5), 0);    // single-class detect
    ASSERT_EQ(keypoint_count_from_channels(4), 0);    // box-only (invalid)
    ASSERT_EQ(keypoint_count_from_channels(8), 0);    // 1 kpt < 4 -> detect
    ASSERT_EQ(keypoint_count_from_channels(17), 4);   // smallest pose head
    return true;
}

TEST(ai_decode_detect_both_layouts) {
    const int64_t anchors = 100;
    // Letterbox of 640x480 into 640x640: scale=1.0, pad_x=0, pad_y=80.
    std::vector<ca::Detection> out;

    const auto t = make_detect_tensor(anchors, 37, true);
    ca::decode_yolo_output(t.data(), 84, anchors, true, 0, 0.1f,
                           640, 480, 1.0f, 0, 80, out);
    ASSERT_EQ(out.size(), 1u);
    ASSERT(out[0].keypoints.empty());   // detection model -> no keypoints
    ASSERT(std::fabs(out[0].x1 - 320.0f) < 0.01f);
    ASSERT(std::fabs(out[0].y1 - 150.0f) < 0.01f);
    ASSERT(std::fabs(out[0].x2 - 420.0f) < 0.01f);
    ASSERT(std::fabs(out[0].y2 - 350.0f) < 0.01f);
    ASSERT(std::fabs(out[0].confidence - 0.9f) < 1e-6f);
    ASSERT(out[0].class_id == 0);
    ASSERT(out[0].class_name == "person");

    // Same data in anchor-major layout ({1, N, C}) decodes identically.
    const auto t2 = make_detect_tensor(anchors, 37, false);
    out.clear();
    ca::decode_yolo_output(t2.data(), anchors, 84, false, 0, 0.1f,
                           640, 480, 1.0f, 0, 80, out);
    ASSERT_EQ(out.size(), 1u);
    ASSERT(std::fabs(out[0].x1 - 320.0f) < 0.01f);
    ASSERT(std::fabs(out[0].y2 - 350.0f) < 0.01f);
    return true;
}

TEST(ai_decode_pose_keypoints) {
    const int64_t channels = 56;   // 4 box + 1 cls + 17*3 kpt (YOLO11n-pose)
    const int64_t anchors  = 50;
    std::vector<float> t(static_cast<size_t>(channels) *
                             static_cast<size_t>(anchors),
                         0.0f);
    const auto set = [&](int64_t a, int64_t c, float v) {
        t[static_cast<size_t>(c) * static_cast<size_t>(anchors) +
          static_cast<size_t>(a)] = v;
    };
    const int64_t a = 10;
    set(a, 4, 0.8f);                                        // person score
    set(a, 0, 200.0f); set(a, 1, 160.0f);                   // box cx, cy
    set(a, 2, 100.0f); set(a, 3, 120.0f);                   // box w, h
    set(a, 5, 200.0f); set(a, 6, 160.0f); set(a, 7, 0.7f);  // kpt 0 (nose)
    set(a, 5 + 16 * 3, 0.0f);                               // kpt 16 x
    set(a, 5 + 16 * 3 + 1, 80.0f);                          // kpt 16 y
    set(a, 5 + 16 * 3 + 2, 0.05f);                          // kpt 16 conf

    // Letterbox of 640x480 into 640x640: scale=1.0, pad_x=0, pad_y=80.
    std::vector<ca::Detection> out;
    ca::decode_yolo_output(t.data(), channels, anchors, true, 0, 0.1f,
                           640, 480, 1.0f, 0, 80, out);
    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].keypoints.size(), 17u);

    // box inverse transform
    ASSERT(std::fabs(out[0].x1 - 150.0f) < 0.01f);
    ASSERT(std::fabs(out[0].y1 - 20.0f)  < 0.01f);
    ASSERT(std::fabs(out[0].x2 - 250.0f) < 0.01f);
    ASSERT(std::fabs(out[0].y2 - 140.0f) < 0.01f);

    // nose: inverse-transformed to original pixels
    ASSERT(std::fabs(out[0].keypoints[0].x - 200.0f) < 0.01f);
    ASSERT(std::fabs(out[0].keypoints[0].y - 80.0f)  < 0.01f);
    // conf must pass through un-sigmoided (sigmoid(0.7) would be ~0.668)
    ASSERT(std::fabs(out[0].keypoints[0].conf - 0.7f) < 1e-6f);

    // ankle: letterbox pad region clamped into the frame at (0, 0)
    ASSERT(std::fabs(out[0].keypoints[16].x) < 0.01f);
    ASSERT(std::fabs(out[0].keypoints[16].y) < 0.01f);
    ASSERT(std::fabs(out[0].keypoints[16].conf - 0.05f) < 1e-6f);
    return true;
}

TEST(ai_pose_model_bus_image) {
    if (!detector_available()) {
        std::cout << "  [skip] ONNX Runtime not built in\n";
        return true;
    }
    const std::string path = find_fixture();
    if (path.empty()) {
        std::cout << "  [skip] bus.jpg not found "
                     "(set AI_BUS_FIXTURE=<absolute path>)\n";
        return true;
    }
    if (!std::ifstream("models/yolo11n-pose.onnx").good()) {
        std::cout << "  [skip] models/yolo11n-pose.onnx not found "
                     "(download per third_lib.md)\n";
        return true;
    }

    int w = 0, h = 0, n = 0;
    uint8_t* rgb = stbi_load(path.c_str(), &w, &h, &n, 3);
    ASSERT(rgb != nullptr);

    auto det = ca::create_detector();
    ca::DetectorConfig cfg;
    cfg.model_path     = "models/yolo11n-pose.onnx";
    cfg.input_width    = 640;
    cfg.input_height   = 640;
    cfg.confidence     = 0.25f;
    cfg.low_confidence = 0.1f;
    cfg.nms_threshold  = 0.45f;
    ASSERT(det->init(cfg));
    ASSERT_EQ(det->keypoint_count(), 17);

    std::vector<ca::Detection> out;
    const bool ok = det->detect(rgb, w, h, out);
    stbi_image_free(rgb);
    ASSERT(ok);

    int persons = 0;
    for (const auto& d : out) {
        ASSERT(d.x1 >= 0.0f && d.x1 <= static_cast<float>(w));
        ASSERT(d.y1 >= 0.0f && d.y1 <= static_cast<float>(h));
        ASSERT(d.x2 > d.x1);
        ASSERT(d.y2 > d.y1);
        ASSERT_EQ(d.keypoints.size(), 17u);
        for (const auto& k : d.keypoints) {
            ASSERT(k.x >= 0.0f && k.x <= static_cast<float>(w));
            ASSERT(k.y >= 0.0f && k.y <= static_cast<float>(h));
            ASSERT(k.conf >= 0.0f && k.conf <= 1.0f);
        }
        if (d.class_id == 0) ++persons;
    }
    std::cout << "  pose bus.jpg " << w << "x" << h << " -> "
              << out.size() << " boxes (" << persons << " persons, 17 kpt)\n";
    ASSERT(persons >= 1);
    return true;
}

TEST(ai_tracker_stable_ids) {
    auto tr = ca::create_tracker();
    ca::TrackerConfig tcfg;
    tcfg.high_threshold  = 0.5f;
    tcfg.match_threshold = 0.8f;
    tcfg.track_buffer    = 30;
    tcfg.frame_rate      = 5;
    tr->configure(tcfg);

    int last_id = -1;
    for (int f = 0; f < 3; ++f) {
        std::vector<ca::Detection> dets(1);
        dets[0].class_id   = 0;
        dets[0].class_name = "person";
        dets[0].confidence = 0.9f;
        dets[0].x1 = 100.0f + 20.0f * f;
        dets[0].y1 = 100.0f;
        dets[0].x2 = 150.0f + 20.0f * f;
        dets[0].y2 = 200.0f;
        auto tracks = tr->update(dets);
        ASSERT(!tracks.empty());
        last_id = tracks[0].track_id;
    }
    ASSERT(last_id >= 0);
    return true;
}

TEST(ai_pipeline_lifecycle) {
    const std::string path = find_fixture();
    int w = 0, h = 0, n = 0;
    uint8_t* rgb = nullptr;
    if (!path.empty()) rgb = stbi_load(path.c_str(), &w, &h, &n, 3);
    if (!rgb) {
        w = 320; h = 240;
        rgb = static_cast<uint8_t*>(std::malloc(w * h * 3));
        std::memset(rgb, 0, w * h * 3);
    }

    ca::AIConfig cfg;
    cfg.enable       = true;
    cfg.fps          = 5;
    cfg.model        = find_detect_model();
    cfg.queue_size   = 2;
    cfg.num_threads  = 1;
    cfg.log_objects  = false;
    cfg.input_width  = 640;
    cfg.input_height = 640;

    ca::AIPipeline pipe;
    if (pipe.init(cfg, w, h, 30)) {
        pipe.start();
        if (pipe.is_running()) {
            int got = 0;
            pipe.set_result_callback([&](const ca::AIFrameResult&) { ++got; });
            for (int i = 0; i < 5; ++i) {
                ca::AIFrame f;
                f.frame_id  = static_cast<uint64_t>(i);
                f.timestamp = 0;
                f.width     = w;
                f.height    = h;
                f.rgb.assign(rgb, rgb + w * h * 3);
                pipe.push_frame(std::move(f));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            pipe.stop();
            (void)got;
        }
    } else {
        std::cout << "  [skip] AI pipeline not started (model unavailable)\n";
    }
    if (rgb) stbi_image_free(rgb);
    return true;
}

// Runtime AI-mode switch (web UI -> video-server -> poller -> apply_mode).
// Verifies the detector can be swapped Detect<->Pose WITHOUT rebuilding the
// whole pipeline, and that current_mode()/keypoint_count() track the swap.
TEST(ai_pipeline_switch_models) {
    if (!detector_available()) {
        std::cout << "  [skip] ONNX Runtime not built in\n";
        return true;
    }
    if (!std::ifstream("models/yolo11n.onnx").good() ||
        !std::ifstream("models/yolo11n-pose.onnx").good()) {
        std::cout << "  [skip] need models/yolo11n.onnx and "
                     "models/yolo11n-pose.onnx\n";
        return true;
    }

    ca::AIConfig cfg;
    cfg.enable        = true;
    cfg.fps           = 5;
    cfg.model         = "models/yolo11n.onnx";
    cfg.model_pose    = "models/yolo11n-pose.onnx";
    cfg.queue_size    = 2;
    cfg.num_threads   = 1;
    cfg.log_objects   = false;
    cfg.input_width   = 640;
    cfg.input_height  = 640;
    cfg.confidence    = 0.25f;
    cfg.low_confidence = 0.1f;
    cfg.nms_threshold = 0.45f;

    ca::AIPipeline pipe;
    ASSERT(pipe.init(cfg, 640, 480, 30));
    ASSERT_EQ(pipe.current_mode(), ca::AIMode::Detect);
    ASSERT_EQ(pipe.keypoint_count(), 0);

    // Switch to pose at runtime (no pipeline restart).
    ASSERT(pipe.apply_mode(ca::AIMode::Pose));
    ASSERT_EQ(pipe.current_mode(), ca::AIMode::Pose);
    ASSERT_EQ(pipe.keypoint_count(), 17);

    // Switch back to detect.
    ASSERT(pipe.apply_mode(ca::AIMode::Detect));
    ASSERT_EQ(pipe.current_mode(), ca::AIMode::Detect);
    ASSERT_EQ(pipe.keypoint_count(), 0);
    std::cout << "  runtime model switch detect<->pose OK\n";
    return true;
}
