#pragma once

// Object detector abstraction (spec 20).
//
//   IDetector
//     +-- YoloDetector      (ONNX Runtime, PC phase)
//     +-- RKNNYoloDetector  (future RK3568 phase)
//
// Server / Web never learn which one is in use.

#include <memory>
#include <string>
#include <vector>

#include "camera_agent/ai/keypoint.h"

namespace ca {

// One raw detection, bbox already mapped back to original video coordinates.
// With a pose model the keypoints vector carries the body joints (same pixel
// space); with a detection model it stays empty.
struct Detection {
    float       x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
    float       confidence = 0.0f;
    int         class_id   = 0;
    std::string class_name = "person";
    std::vector<Keypoint> keypoints;   // empty = detection model
};

struct DetectorConfig {
    std::string model_path    = "models/yolo11n.onnx";
    int         input_width   = 640;
    int         input_height  = 640;
    // Detections below this score are handed to the tracker only as "low score"
    // candidates: they can refresh an existing track but never spawn a new one.
    float       confidence    = 0.5f;
    // Hard floor for what the detector emits at all (ByteTrack's 0.1).
    float       low_confidence = 0.1f;
    float       nms_threshold = 0.45f;
    int         num_threads   = 2;
    // COCO class id to keep. 0 == person; phase 1 detects people only.
    int         class_id      = 0;
};

class IDetector {
public:
    virtual ~IDetector() = default;

    // Load the model. Returns false on any failure; the caller must then keep
    // running with AI disabled (spec 19). Never throws.
    virtual bool init(const DetectorConfig& cfg) = 0;

    // Run inference on a packed RGB frame of size w x h.
    // Returns false if inference failed (the frame is simply skipped).
    virtual bool detect(const uint8_t* rgb, int w, int h,
                        std::vector<Detection>& out) = 0;

    // Human readable backend, used in the "[AI] model loaded" log line.
    virtual const char* backend_name() const = 0;

    // 0 = detection model; >0 = pose model with this many keypoints per
    // object (17 for the COCO YOLO11n-pose). Known right after init().
    virtual int keypoint_count() const { return 0; }
};

// Factory: returns the ONNX Runtime implementation when the project was built
// with it, otherwise a null detector whose init() always fails.
std::unique_ptr<IDetector> create_detector();

} // namespace ca
