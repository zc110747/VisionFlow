// Hand analyzer implementation (spec 17-24).
//
// Everything here is deterministic arithmetic over the detections it is fed, so
// the whole "what is the hand doing" contract is unit-testable without a model,
// a camera or ONNX Runtime - see tests/hand_analyzer_tests.cpp.

#include "camera_agent/ai/hand_analyzer.h"

#include <cmath>
#include <cstddef>

namespace ca {
namespace {

// --- gesture geometry thresholds (spec 22) ---------------------------------
//
// A finger is measured by the ratio of two 3D distances to the wrist:
//     ratio = |tip - wrist| / |knuckle(MCP) - wrist|
// An extended finger reaches further out than its knuckle; a curled finger is
// folded back so its tip ends up closer to the wrist than the knuckle.
//
// The EXTENDED side of this band is not a guess: on the official MediaPipe
// sample portrait (tests/golden/woman_hands.png, which contains two open hands)
// the four measured ratios are 1.5927 / 1.7069 / 1.6751 / 1.5901 for one hand
// and 1.7354 / 1.8355 / 1.7877 / 1.6845 for the other - see
// tests/golden/README.md. 1.15 therefore sits far below real open-hand data.
// The CURLED side cannot be calibrated from these samples (neither hand is a
// fist) and is deliberately placed below 1.0 with a margin band; it is the one
// number in this file that still needs the live, spec 16 style experiment.
constexpr float kExtendedRatio = 1.15f;
constexpr float kCurledRatio   = 0.90f;

// Share of the four fingers that must agree before a gesture is reported.
// Below this the hand is between poses and the honest answer is UNKNOWN.
constexpr float kGestureVote = 0.75f;

// Landmark indices (MediaPipe hand convention, see keypoint.h).
constexpr int kWristIdx = 0;
constexpr int kTips[4]  = {8, 12, 16, 20};    // index / middle / ring / pinky
constexpr int kMcps[4]  = {5,  9, 13, 17};

// Below this the numerator/denominator is numerically meaningless (degenerate
// landmarks from a partly occluded hand).
constexpr float kMinSpanPx = 0.5f;

inline float dist3(const Keypoint& a, const Keypoint& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline Vec3 lerp3(const Vec3& prev, const Vec3& cur, float alpha) {
    Vec3 v;
    v.x = alpha * cur.x + (1.0f - alpha) * prev.x;
    v.y = alpha * cur.y + (1.0f - alpha) * prev.y;
    v.z = alpha * cur.z + (1.0f - alpha) * prev.z;
    return v;
}

} // namespace

// ---------------------------------------------------------------------------
// Enum -> wire strings. Single definition point so the agent, the server-side
// validator and the web UI can never drift apart on spelling.
// ---------------------------------------------------------------------------
const char* to_string(MotionX m) {
    switch (m) {
        case MotionX::Left:  return "LEFT";
        case MotionX::Right: return "RIGHT";
        default:             return "STATIONARY";
    }
}

const char* to_string(MotionY m) {
    switch (m) {
        case MotionY::Backward: return "BACKWARD";
        case MotionY::Forward:  return "FORWARD";
        default:                return "STATIONARY";
    }
}

const char* to_string(MotionZ m) {
    switch (m) {
        case MotionZ::Down: return "DOWN";
        case MotionZ::Up:   return "UP";
        default:            return "STATIONARY";
    }
}

const char* to_string(Gesture g) {
    switch (g) {
        case Gesture::OpenPalm: return "OPEN_PALM";
        case Gesture::Fist:     return "FIST";
        default:                return "UNKNOWN";
    }
}

const char* to_string(Handedness h) {
    switch (h) {
        case Handedness::Left:  return "left";
        case Handedness::Right: return "right";
        default:                return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Coordinate mapper (spec 17)
// ---------------------------------------------------------------------------
LogicalPos hand_map_coordinates(const HandDetection& det, int w, int h,
                                const std::string& depth_source,
                                bool invert_x, bool invert_y, bool invert_z) {
    LogicalPos out;
    if (w <= 0 || h <= 0) return out;

    // Image -> logical. The camera frame already tells us left from right, so
    // these are the only two conversions needed for the in-plane axes:
    //   x  : pixels grow to the RIGHT  -> X+ is already RIGHT
    //   z  : pixels grow DOWNWARD      -> Z+ (UP) needs the axis flipped
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    const float cx = (det.x1 + det.x2) * 0.5f;
    const float cy = (det.y1 + det.y2) * 0.5f;

    float x = cx / fw;
    float z = (fh - cy) / fh;

    // Depth (logical Y, "closer = +"). Two very different quantities can be
    // used here and they must not be confused:
    //
    //   "size"        how LARGE the hand looks. Monotonic with the distance to
    //                 the camera, so it answers "did the hand move closer?".
    //                 This is the default because it is what the FORWARD /
    //                 BACKWARD axis is supposed to mean (spec 21).
    //   "landmark_z"  the model's wrist-relative depth. It describes the hand's
    //                 ORIENTATION (fingers pointing towards / away from the
    //                 camera), NOT its position - the wrist itself always has
    //                 z == 0, which was measured, not assumed.
    float y = 0.0f;
    if (depth_source == "landmark_z" && det.landmarks.size() > 1) {
        // Mean over every landmark except the wrist, which is the origin of the
        // model's relative depth and therefore carries no information.
        float sum = 0.0f;
        for (size_t i = 1; i < det.landmarks.size(); ++i) {
            sum += det.landmarks[i].z;
        }
        const float mean_z = sum / static_cast<float>(det.landmarks.size() - 1);
        // `/ fh` puts it on the same scale as x/z. The sign is a deliberate
        // mapper decision (spec 17) established by the spec 16 experiment.
        y = -mean_z / fh;
    } else {
        const float bw = det.x2 - det.x1;
        const float bh = det.y2 - det.y1;
        if (bw > 0.0f && bh > 0.0f) {
            // Geometric mean of the box sides over the frame height: a stable,
            // resolution-independent "how big is the hand on screen" measure.
            y = std::sqrt(bw * bh) / fh;
        }
    }

    // Spec 17: a mismatched camera mounting is corrected HERE, by configuration,
    // never by editing the model or the detector.
    if (invert_x) x = -x;
    if (invert_y) y = -y;
    if (invert_z) z = -z;

    out.pos.x = x;
    out.pos.y = y;
    out.pos.z = z;
    return out;
}

MotionX classify_motion_x(float dx, float deadzone) {
    if (std::fabs(dx) < deadzone) return MotionX::Stationary;
    return dx > 0.0f ? MotionX::Right : MotionX::Left;
}

MotionY classify_motion_y(float dy, float deadzone) {
    if (std::fabs(dy) < deadzone) return MotionY::Stationary;
    return dy > 0.0f ? MotionY::Forward : MotionY::Backward;
}

MotionZ classify_motion_z(float dz, float deadzone) {
    if (std::fabs(dz) < deadzone) return MotionZ::Stationary;
    return dz > 0.0f ? MotionZ::Up : MotionZ::Down;
}

// ---------------------------------------------------------------------------
// Gesture (spec 22)
// ---------------------------------------------------------------------------
GestureResult hand_classify_gesture(const std::vector<Keypoint>& lm) {
    GestureResult r;
    const size_t need = static_cast<size_t>(kTips[3]) + 1u;   // 21
    if (lm.size() < need) return r;

    const Keypoint& wrist = lm[kWristIdx];
    int extended = 0;
    int curled   = 0;

    for (int f = 0; f < 4; ++f) {
        const float d_tip = dist3(lm[static_cast<size_t>(kTips[f])], wrist);
        const float d_mcp = dist3(lm[static_cast<size_t>(kMcps[f])], wrist);
        if (d_mcp < kMinSpanPx) continue;   // degenerate: skip this finger
        const float ratio = d_tip / d_mcp;
        if (ratio > kExtendedRatio) {
            ++extended;
        } else if (ratio < kCurledRatio) {
            ++curled;
        }
    }

    const float open = static_cast<float>(extended) * 0.25f;
    const float fist = static_cast<float>(curled) * 0.25f;
    if (open >= kGestureVote) {
        r.g = Gesture::OpenPalm;
        r.confidence = open;
    } else if (fist >= kGestureVote) {
        r.g = Gesture::Fist;
        r.confidence = fist;
    } else {
        // Between poses (fingers spread apart, partly occluded, mid-transition).
        // Reporting a confident answer here is how a gesture UI starts lying.
        r.g = Gesture::Unknown;
        r.confidence = 0.0f;
    }
    return r;
}

// ---------------------------------------------------------------------------
// HandAnalyzer
// ---------------------------------------------------------------------------
bool HandAnalyzer::init(const HandConfig& cfg) {
    if (!(cfg.filter_alpha > 0.0f && cfg.filter_alpha <= 1.0f)) return false;
    if (!(cfg.motion_deadzone >= 0.0f)) return false;
    if (cfg.reference_stable_frames < 1) return false;
    if (cfg.lost_frames_to_reset < 1) return false;
    if (cfg.gesture_confirm_frames < 1) return false;
    cfg_ = cfg;
    inited_ = true;
    reset();
    return true;
}

void HandAnalyzer::reset() {
    ref_initialized_ = false;
    reference_ = Vec3{};
    stable_count_ = 0;
    lost_count_ = 0;
    have_filtered_ = false;
    filtered_ = Vec3{};
    gesture_candidate_ = Gesture::Unknown;
    candidate_count_ = 0;
    gesture_confirmed_ = Gesture::Unknown;
    gesture_conf_ = 0.0f;
}

void HandAnalyzer::update(const HandDetection& det, int video_w, int video_h,
                          uint64_t frame_id, uint64_t timestamp, double fps,
                          HandState& out) {
    out = HandState{};
    out.frame_id  = frame_id;
    out.timestamp = timestamp;
    out.fps       = fps;

    // ---- no hand in this frame -------------------------------------------
    if (!det.detected) {
        ++lost_count_;
        // Drop the filter so a re-entering hand is not smeared from wherever it
        // left, and restart the latched gesture only once the hand is really gone
        // (a one-frame dropout must not flicker the UI).
        have_filtered_ = false;
        candidate_count_ = 0;
        if (lost_count_ >= cfg_.lost_frames_to_reset) {
            reset();
        }
        out.detected = false;
        out.reference_initialized = ref_initialized_;
        out.gesture = gesture_confirmed_;
        return;
    }

    lost_count_ = 0;
    out.detected = true;
    out.confidence = det.confidence;
    out.handedness = det.handedness;
    out.x1 = det.x1;
    out.y1 = det.y1;
    out.x2 = det.x2;
    out.y2 = det.y2;
    if (cfg_.emit_landmarks) out.landmarks = det.landmarks;

    // ---- 1. Coordinate Mapper (spec 17) -----------------------------------
    const LogicalPos raw = hand_map_coordinates(det, video_w, video_h,
                                               cfg_.depth_source, cfg_.invert_x,
                                               cfg_.invert_y, cfg_.invert_z);

    // ---- 2. EMA filter (spec 19): filtered = a*cur + (1-a)*prev -----------
    if (!have_filtered_) {
        filtered_ = raw.pos;
        have_filtered_ = true;
    } else {
        filtered_ = lerp3(filtered_, raw.pos, cfg_.filter_alpha);
    }
    out.position = filtered_;

    // ---- 3. Reference point (spec 18) ------------------------------------
    // NO_HAND -> HAND_DETECTED -> REFERENCE_INITIALIZED -> TRACKING
    if (!ref_initialized_) {
        ++stable_count_;
        if (stable_count_ >= cfg_.reference_stable_frames) {
            // Latch on the FILTERED position: latching the raw first sample would
            // bake that frame's jitter straight into every later delta.
            reference_ = filtered_;
            ref_initialized_ = true;
        }
    }
    out.reference_initialized = ref_initialized_;

    if (ref_initialized_) {
        out.reference = reference_;
        out.delta.x = filtered_.x - reference_.x;
        out.delta.y = filtered_.y - reference_.y;
        out.delta.z = filtered_.z - reference_.z;
        // ---- 4. Motion analyzer (spec 20/21) -----------------------------
        // The logical axes already point where the names say (X+ right,
        // Y+ forward, Z+ up), so the sign of the delta IS the direction.
        out.motion.x = classify_motion_x(out.delta.x, cfg_.motion_deadzone);
        out.motion.y = classify_motion_y(out.delta.y, cfg_.motion_deadzone);
        out.motion.z = classify_motion_z(out.delta.z, cfg_.motion_deadzone);
    } else {
        // Before the reference exists the delta is the distance from the frame
        // origin, which is huge and meaningless - report nothing instead of a
        // direction that would be pure noise.
        out.reference = Vec3{};
        out.delta = Vec3{};
        out.motion = Motion3{};
    }

    // ---- 5. Gesture + debounce (spec 22/23) ------------------------------
    const GestureResult g = hand_classify_gesture(det.landmarks);
    out.gesture_candidate = g.g;
    if (g.g == gesture_candidate_) {
        ++candidate_count_;
    } else {
        gesture_candidate_ = g.g;
        candidate_count_ = 1;
    }
    if (candidate_count_ >= cfg_.gesture_confirm_frames) {
        gesture_confirmed_ = gesture_candidate_;
        gesture_conf_ = g.confidence;
    }
    out.gesture = gesture_confirmed_;
    out.gesture_confidence = gesture_conf_;
}

} // namespace ca
