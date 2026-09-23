#pragma once

// Hand analyzer: raw per-frame detections -> one stable HandState.
//
// This is where all the temporal and geometric judgement lives, deliberately
// separated from the detector so it can be unit tested with synthetic input and
// no model, and so the pipeline's "what is a hand doing" logic has exactly one
// home:
//
//   HandDetection -> Coordinate Mapper  (spec 17)  -> logical XYZ
//                 -> EMA filter         (spec 19)
//                 -> Reference / delta  (spec 18)
//                 -> Motion analyzer    (spec 20/21)
//                 -> Gesture analyzer   (spec 22) + debounce (spec 23)
//                 -> HandState          (spec 24)
//
// It holds state across frames and therefore must be used from ONE thread
// (the AI thread). Every method is noexcept in spirit: no exceptions escape.

#include <string>
#include <vector>

#include "camera_agent/ai/hand_types.h"

namespace ca {

// Position of a hand inside the frame, in the logical frame of spec 17, plus the
// apparent closeness that drives the FORWARD/BACKWARD axis.
//
// Pure functions, exposed for the unit tests: this is the "Coordinate Mapper"
// of spec 17, and getting a sign wrong here is exactly the kind of bug that a
// robot would act on in Phase 2.
struct LogicalPos {
    Vec3 pos;      // x right [0,1], y closeness (see depth_source), z up [0,1]
};

// Map one detection to the logical frame. `w`/`h` are the video dimensions the
// model ran on. `depth_source` is HandConfig::depth_source.
LogicalPos hand_map_coordinates(const HandDetection& det, int w, int h,
                                const std::string& depth_source,
                                bool invert_x, bool invert_y, bool invert_z);

// Classify one axis of motion from its delta. `deadzone` is in the same
// normalised units as the delta (spec 20).
MotionX classify_motion_x(float dx, float deadzone);
MotionY classify_motion_y(float dy, float deadzone);
MotionZ classify_motion_z(float dz, float deadzone);

// Geometric gesture result for one frame (spec 22) - no classifier is trained,
// this is purely the landmark geometry.
struct GestureResult {
    Gesture g = Gesture::Unknown;
    float   confidence = 0.0f;
};

// Open palm vs fist from the 21 landmarks. Uses the ratio of each finger's
// tip distance from the wrist to its knuckle (MCP) distance from the wrist:
// an extended finger has its tip further out than its knuckle, a curled one has
// the tip pulled back towards the palm. The thumb is deliberately not part of
// the vote - it is the noisiest joint and both gestures contain it.
GestureResult hand_classify_gesture(const std::vector<Keypoint>& landmarks);

// ---------------------------------------------------------------------------
class HandAnalyzer {
public:
    // Returns false when the configuration is unusable (out-of-range alpha or
    // deadzone); the caller then keeps the previous mode.
    bool init(const HandConfig& cfg);

    // Forget every temporal state (reference, filter, gesture latch). Called when
    // the AI mode changes so a switch never carries stale history across.
    void reset();

    // Fold one frame in. `video_w`/`video_h` are the dimensions the detector ran
    // on, `fps` is the AI branch's measured rate (already known to the pipeline).
    // Always produces a fully populated HandState.
    void update(const HandDetection& det, int video_w, int video_h,
                uint64_t frame_id, uint64_t timestamp, double fps,
                HandState& out);

private:
    HandConfig cfg_{};
    bool       inited_ = false;

    // Reference point (spec 18).
    bool  ref_initialized_ = false;
    Vec3  reference_{};
    int   stable_count_ = 0;   // consecutive detections before the latch
    int   lost_count_   = 0;   // consecutive misses since the last detection

    // EMA filter (spec 19).
    bool have_filtered_ = false;
    Vec3 filtered_{};

    // Gesture debounce (spec 23).
    Gesture gesture_candidate_ = Gesture::Unknown;
    int     candidate_count_   = 0;
    Gesture gesture_confirmed_ = Gesture::Unknown;
    float   gesture_conf_      = 0.0f;
};

} // namespace ca
