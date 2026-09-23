#pragma once

// Internal AI data structures.
//
// These are the contract for Phase 2 (AI metadata upload). They are deliberately
// free of any GStreamer / ONNX Runtime / OpenCV types so the metadata layer can
// serialize them without pulling in the AI implementation.
//
// Coordinate convention: every bbox is expressed in ORIGINAL VIDEO pixels
// (whatever the camera negotiated - 1280x720, 1920x1080, 240x240, ...), never in
// the network input space (640x640).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "camera_agent/ai/hand_types.h"
#include "camera_agent/ai/keypoint.h"

namespace ca {

// Runtime AI mode requested by the web UI (via the video-server aimode
// endpoint). Detect = person detection only; Pose = person detection + 17
// COCO keypoints; Hand = single-hand 3D landmarks + motion + gesture (a
// two-model cascade, see hand_detector.h). The agent keeps its own model
// loaded and swaps it on demand.
//
// Hand is deliberately a fourth *mode*, not a fourth pipeline: it runs on the
// same AIPipeline thread, off the same frame tap, into the same
// MetadataManager. Selecting it replaces the loaded detector; nothing else in
// the agent changes shape.
enum class AIMode : int { Detect = 0, Pose = 1, Hand = 2 };

inline const char* to_string(AIMode m) {
    switch (m) {
        case AIMode::Pose: return "pose";
        case AIMode::Hand: return "hand";
        default:           return "detect";
    }
}

// Wire vocabulary shared with video-server (model.go AIModeDetect / AIModePose)
// and the web UI (client.ts AIMode). Each metadata frame is stamped with the
// mode the agent ACTUALLY used to produce it, so the web can drop transition
// frames whose mode no longer matches the selected mode. "ai-off" is never
// stamped here - it is web-only (the overlay is hidden but the model stays).
inline const char* aimode_web_string(AIMode m) {
    switch (m) {
        case AIMode::Pose: return "ai-y-pose";
        case AIMode::Hand: return "ai-hand";
        default:           return "ai-y";
    }
}

// Translate a mode token into an AIMode. Both vocabularies are accepted: the
// short form used by the CLI / YAML ("detect" / "pose" / "hand") and the wire
// form the video-server speaks ("ai-y" / "ai-y-pose" / "ai-hand").
//
// Returns false for "ai-off" and for anything unrecognised, and that is
// deliberate: "ai-off" means "the web stops drawing, the agent keeps its
// model", so turning it into a model switch would be wrong - and an unknown
// token must leave the current mode alone rather than guess.
inline bool aimode_from_string(const std::string& s, AIMode& out) {
    if (s == "detect" || s == "ai-y")      { out = AIMode::Detect; return true; }
    if (s == "pose"   || s == "ai-y-pose") { out = AIMode::Pose;   return true; }
    if (s == "hand"   || s == "ai-hand")   { out = AIMode::Hand;   return true; }
    return false;
}

// One detected + tracked object.
struct AIObject {
    std::string class_name = "person";
    int         class_id   = 0;    // COCO id; person = 0
    float       confidence = 0.0f;
    int         track_id   = -1;   // -1 = not (yet) tracked

    // bbox in original video pixel coordinates, (x1,y1) = top-left.
    float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;

    // Body keypoints in original video pixels (pose model only; empty with a
    // detection model). Serialized as the additive frame field "keypoints".
    std::vector<Keypoint> keypoints;
};

// Standardized per-frame AI result. This is what Phase 2 will upload.
struct AIFrameResult {
    uint64_t frame_id  = 0;   // monotonic CAMERA frame counter (see spec 13)
    uint64_t timestamp = 0;   // ms, taken from the GStreamer PTS (see spec 14)
    int      video_width  = 0;
    int      video_height = 0;
    std::vector<AIObject> objects;
    // Mode the agent ACTUALLY ran to produce this frame ("ai-y" / "ai-y-pose" /
    // "ai-hand"). Stamped per frame so the web can drop transition frames whose
    // mode no longer matches the selected mode (see aimode_web_string).
    std::string ai_mode;

    // Hand payload. Engaged ONLY when the frame was produced by AIMode::Hand -
    // an engaged value means "this frame carries the hand fields", including
    // the honest case of a hand frame where no hand was found
    // (hand->detected == false). Detect / pose frames leave it disengaged, so
    // the encoder keeps emitting byte-identical JSON for those modes.
    std::optional<HandState> hand;
};

// Raw frame handed from the video pipeline to the AI pipeline.
// `rgb` is tightly packed 24-bit RGB (no stride padding).
struct AIFrame {
    uint64_t             frame_id  = 0;
    uint64_t             timestamp = 0;
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgb;
};

// Everything the AI branch needs from the configuration layer (spec 18).
// Every value is configurable - nothing here is hard-coded at the call site.
// Kept in this header (pure data, no thread/mutex dependencies) so config.h can
// embed it without pulling in the whole AI implementation.

// AI inference-rate bounds (configurable range, never hard-coded at call sites).
constexpr int kAiFpsMin     = 5;    // lower bound, fps
constexpr int kAiFpsMax     = 12;   // upper bound, fps
constexpr int kAiFpsDefault = 8;    // default, fps

// Clamp a requested inference rate into [kAiFpsMin, kAiFpsMax]; tolerates a
// reversed range so it can never be UB (mirrors metadata_encoder.cpp clampi).
inline int clamp_ai_fps(int v) {
    if (v < kAiFpsMin) return kAiFpsMin;
    if (v > kAiFpsMax) return kAiFpsMax;
    return v;
}

struct AIConfig {
    bool        enable      = false;
    int         fps         = kAiFpsDefault;  // target inference rate (range 5-12)
    float       confidence  = 0.5f;   // detector + tracker gate
    std::string model       = "models/yolo11n.onnx";
    int         input_width  = 640;
    int         input_height = 640;
    int         queue_size   = 2;     // bounded; oldest frame is dropped on overflow
    float       nms_threshold   = 0.45f;
    float       match_threshold = 0.8f;
    int         track_buffer    = 30;
    float       low_confidence  = 0.1f;   // detector floor (ByteTrack's 0.1)
    // Source framerates below this are processed frame-by-frame instead of being
    // sub-sampled, so a slow camera is not starved even further.
    int         full_rate_below_fps = 10;
    bool        log_objects = true;
    int         num_threads = 2;

    // --- Runtime AI mode switching (web UI -> video-server -> agent) -------
    // Pose model path. Detect uses `model` above. Auto-detected so either ONNX
    // can be dropped in without code changes.
    std::string model_pose = "models/yolo11n-pose.onnx";
    // Poll the video-server for the desired AI mode and swap the loaded model
    // on change. Disabled when false (model stays fixed at startup).
    bool        aimode_poll     = true;
    int         aimode_poll_ms  = 2000;   // poll period
    // video-server HTTP base used to build GET /api/cameras/{id}/aimode.
    // Left empty by default: StreamController derives it from
    // metadata.server_url (everything up to "/api/metadata") so the operator
    // only configures ONE endpoint. An explicit value here always wins.
    std::string aimode_base_url = "";

    // Mode the branch starts in, before any web request arrives. Exist so the
    // hand mode (and pose) can be exercised on hardware without a running
    // server - the aimode endpoint is the only other way in.
    //
    // Detect keeps the historical behaviour: the configured `model` is loaded
    // and, if it turns out to be a pose ONNX, the mode self-reports as Pose.
    AIMode      initial_mode = AIMode::Detect;

    // --- Hand mode (Phase 1) ------------------------------------------------
    // Owned here (not in a global) so every knob of the hand branch is reached
    // through the same configuration chain as the rest of AI: defaults -> YAML
    // -> CLI. See hand_types.h for what each field means.
    HandConfig  hand;
};

// Runtime statistics of the AI pipeline (spec 16).
struct AIStats {
    double   ai_fps        = 0.0;   // measured processed frames per second
    double   inference_ms  = 0.0;   // rolling mean detector time
    double   tracker_ms    = 0.0;   // rolling mean tracker time
    int      queue_size    = 0;     // frames currently buffered
    uint64_t dropped       = 0;     // frames discarded (queue full / superseded)
    uint64_t processed     = 0;     // frames actually inferred
    uint64_t objects       = 0;     // objects in the most recent result
};

} // namespace ca
