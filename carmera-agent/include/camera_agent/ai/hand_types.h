#pragma once

// Hand AI data structures (Phase 1: single-hand 3D motion + gesture).
//
// Pure data only - no GStreamer / ONNX Runtime / thread types - so the metadata
// layer can serialize them without pulling in the AI implementation, and unit
// tests can build them by hand.
//
// ARCHITECTURAL BOUNDARY (must not drift):
//   This is the END of Phase 1. A HandState is turned into metadata and sent to
//   the frontend; nothing here may know about a robot, IK, servos, MuJoCo or
//   MeArmPilot. Phase 2 adds "HandState -> RobotCommand" in a SEPARATE project.

#include <cstdint>
#include <string>
#include <vector>

#include "camera_agent/ai/keypoint.h"

namespace ca {

// ---------------------------------------------------------------------------
// Logical coordinate system (spec 17). Every consumer past this point speaks
// ONLY this frame - the raw model/camera axes never leak out.
//
//   X+ RIGHT      X- LEFT
//   Y+ FORWARD    Y- BACKWARD      (forward = towards the camera)
//   Z+ UP         Z- DOWN
//
// position/delta/reference are NORMALISED, so they are resolution independent:
//   x  in [0,1]  across the frame width,  0 = left edge
//   z  in [0,1]  up the frame height,     0 = bottom edge
//   y  depth proxy, ~1.0 = hand as large as the frame height (see `depth_source`)
// ---------------------------------------------------------------------------

// Per-axis motion state (spec 21). One enum per axis so an invalid combination
// (e.g. "UP" on the X axis) is unrepresentable rather than merely unlikely.
enum class MotionX : int { Stationary = 0, Left = 1, Right = 2 };
enum class MotionY : int { Stationary = 0, Backward = 1, Forward = 2 };
enum class MotionZ : int { Stationary = 0, Down = 1, Up = 2 };

// Phase 1 gesture vocabulary (spec 22). Deliberately tiny: the classifier is
// geometric, so anything that does not clearly match stays Unknown instead of
// guessing.
enum class Gesture : int { Unknown = 0, OpenPalm = 1, Fist = 2 };

// Handedness as reported by the landmark model.
enum class Handedness : int { Unknown = 0, Left = 1, Right = 2 };

const char* to_string(MotionX m);
const char* to_string(MotionY m);
const char* to_string(MotionZ m);
const char* to_string(Gesture g);
const char* to_string(Handedness h);

// A logical 3-vector, used for position / reference / delta.
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Motion3 {
    MotionX x = MotionX::Stationary;
    MotionY y = MotionY::Stationary;
    MotionZ z = MotionZ::Stationary;
};

// ---------------------------------------------------------------------------
// Raw per-frame detector output for ONE hand.
// ---------------------------------------------------------------------------
struct HandDetection {
    bool  detected   = false;
    float confidence = 0.0f;
    // bbox in ORIGINAL VIDEO pixels, (x1,y1) top-left. Derived by the detector
    // from the 21 landmarks, then enlarged (see hand_detector.cpp).
    float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
    Handedness handedness = Handedness::Unknown;
    // 21 landmarks in ORIGINAL VIDEO pixels; empty when !detected.
    std::vector<Keypoint> landmarks;
};

// ---------------------------------------------------------------------------
// The unified internal hand result (spec 24). This is what the AI branch
// produces; the metadata encoder turns it into the wire format.
// ---------------------------------------------------------------------------
struct HandState {
    bool  detected   = false;
    float confidence = 0.0f;
    Handedness handedness = Handedness::Unknown;

    // Reference point state machine (spec 18): false until the hand has been
    // stably visible for `reference_stable_frames` frames. While false, delta is
    // meaningless and MUST NOT be consumed - that is what prevents the huge
    // absolute jump when a hand first enters the frame.
    bool reference_initialized = false;

    // Logical, normalised (see the header comment). Zero while !detected.
    Vec3 position;
    Vec3 reference;
    Vec3 delta;
    Motion3 motion;

    Gesture gesture = Gesture::Unknown;
    float   gesture_confidence = 0.0f;
    // Raw (pre-debounce) gesture of this frame - kept so the UI/debug view can
    // show the candidate vs the confirmed value.
    Gesture gesture_candidate = Gesture::Unknown;

    // Raw evidence, kept for the overlay and for spec 10's "full landmarks in
    // Phase 1" rule. Empty when the detector produced nothing.
    float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
    std::vector<Keypoint> landmarks;

    // Time base - copied verbatim from the capture side (spec 8): never
    // regenerated anywhere downstream.
    uint64_t frame_id  = 0;
    uint64_t timestamp = 0;
    double   fps       = 0.0;
};

// ---------------------------------------------------------------------------
// Configuration (spec 18/19/20/23: every knob configurable, nothing hard-coded
// at the call site).
// ---------------------------------------------------------------------------
struct HandConfig {
    // --- models -------------------------------------------------------------
    std::string model_palm     = "models/palm_detection_mediapipe_2023feb.onnx";
    std::string model_landmark = "models/handpose_estimation_mediapipe_2023feb.onnx";

    // --- detector thresholds (upstream MediaPipe defaults, spec 13 reuse) ----
    float palm_score_threshold = 0.5f;
    float palm_nms_threshold   = 0.3f;
    float landmark_confidence  = 0.5f;
    // Phase 1 is single-hand (spec 2 / spec 32): extra palms are detected by the
    // model but only the highest-scoring one is promoted to a HandState.
    int   max_hands = 1;

    // --- analyzer / filter (spec 19) ----------------------------------------
    // EMA: filtered = alpha * current + (1 - alpha) * previous.
    float filter_alpha = 0.4f;

    // --- deadzone (spec 20), in the SAME normalised units as delta ----------
    // |delta_axis| below this reads as STATIONARY, which is what stops the
    // LEFT/RIGHT/LEFT/RIGHT chatter caused by hand tremor.
    float motion_deadzone = 0.02f;

    // --- reference point (spec 18) ------------------------------------------
    // Consecutive detections required before the reference is latched.
    int reference_stable_frames = 3;
    // Consecutive misses after which the reference is dropped and the state
    // machine restarts at NO_HAND (a new hand gets a fresh origin).
    int lost_frames_to_reset = 10;

    // --- gesture debounce (spec 23) -----------------------------------------
    int gesture_confirm_frames = 5;

    // --- depth (spec 15/16) -------------------------------------------------
    // Where the logical Y (FORWARD/BACKWARD) axis comes from:
    //   "size"       apparent hand size in the frame. Monotonic with distance to
    //                the camera, so it answers "did the hand move closer?".
    //   "landmark_z" the landmark model's wrist-relative z. This describes hand
    //                ORIENTATION (fingers toward/away from the camera), not the
    //                hand's distance - kept selectable so the spec 16 experiment
    //                can be run and its outcome recorded in the docs.
    std::string depth_source = "size";

    // --- coordinate mapper (spec 17) ----------------------------------------
    // Flip an axis when the mounted camera's geometry turns out to be mirrored.
    // The spec 16 experiment decides these; they are configuration, not magic
    // numbers buried in the code.
    bool invert_x = false;
    bool invert_y = false;
    bool invert_z = false;

    // --- payload (spec 10) --------------------------------------------------
    // Phase 1 keeps full landmarks so the overlay and the debug panel have
    // everything; a production run can turn them off to shrink the metadata.
    bool emit_landmarks = true;
};

} // namespace ca
