// Hand-analyzer tests (spec 17-24). Pure arithmetic: no model, no camera, no
// ONNX Runtime - so these run on every machine and pin the contract that the
// robot-facing Phase 2 will consume.
#include "camera_agent/ai/hand_analyzer.h"
#include "camera_agent/ai/hand_types.h"
#include "test_harness.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {

// MediaPipe hand landmark indices (see include/camera_agent/ai/keypoint.h).
constexpr int kWrist   = 0;
constexpr int kTips[4] = {8, 12, 16, 20};   // index / middle / ring / pinky
constexpr int kMcps[4] = {5,  9, 13, 17};

// A 21-landmark hand where each finger points in its own direction (so no two
// distances coincide) and the tip sits `tip_ratio` x the knuckle distance from
// the wrist. tip_ratio > 1.15 reads as extended, < 0.90 as curled.
std::vector<ca::Keypoint> make_hand(float tip_ratio) {
    std::vector<ca::Keypoint> lm(21);
    const float dirs[4][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}, {0.0f, -1.0f}};
    const float wrist_x = 100.0f, wrist_y = 100.0f;
    const float mcp_d = 20.0f;
    lm[kWrist].x = wrist_x;
    lm[kWrist].y = wrist_y;
    for (int f = 0; f < 4; ++f) {
        lm[static_cast<size_t>(kMcps[f])].x = wrist_x + dirs[f][0] * mcp_d;
        lm[static_cast<size_t>(kMcps[f])].y = wrist_y + dirs[f][1] * mcp_d;
        lm[static_cast<size_t>(kTips[f])].x = wrist_x + dirs[f][0] * mcp_d * tip_ratio;
        lm[static_cast<size_t>(kTips[f])].y = wrist_y + dirs[f][1] * mcp_d * tip_ratio;
    }
    for (auto& k : lm) k.conf = 0.9f;
    return lm;
}

ca::HandDetection make_det(float x1, float y1, float x2, float y2,
                           float tip_ratio = 1.70f) {
    ca::HandDetection d;
    d.detected   = true;
    d.confidence = 0.8f;
    d.handedness = ca::Handedness::Left;
    d.x1 = x1;
    d.y1 = y1;
    d.x2 = x2;
    d.y2 = y2;
    d.landmarks = make_hand(tip_ratio);
    return d;
}

ca::HandConfig base_cfg() {
    ca::HandConfig cfg;
    cfg.filter_alpha           = 1.0f;   // no smoothing: position == mapper output
    cfg.motion_deadzone        = 0.02f;
    cfg.reference_stable_frames = 3;
    cfg.lost_frames_to_reset   = 10;
    cfg.gesture_confirm_frames = 5;
    return cfg;
}

}  // namespace

// ---------------------------------------------------------------------------
// spec 17 - coordinate mapper
// ---------------------------------------------------------------------------
TEST(hand_mapper_in_plane_axes) {
    ca::HandDetection d = make_det(100.0f, 100.0f, 300.0f, 200.0f);
    const ca::LogicalPos p =
        ca::hand_map_coordinates(d, 640, 480, "size", false, false, false);

    ASSERT(std::fabs(p.pos.x - 0.3125f) < 1e-6f);      // cx 200 / 640
    ASSERT(std::fabs(p.pos.z - 0.6875f) < 1e-6f);      // (480 - cy 150) / 480
    ASSERT(std::fabs(p.pos.y - 0.29462783f) < 1e-5f);  // sqrt(200*100) / 480

    // Z+ is UP: the same box moved down the image must report a LOWER z.
    const ca::HandDetection low = make_det(100.0f, 200.0f, 300.0f, 300.0f);
    const ca::LogicalPos pl =
        ca::hand_map_coordinates(low, 640, 480, "size", false, false, false);
    ASSERT(pl.pos.z < p.pos.z);

    // X+ is RIGHT: pixels already grow to the right, so x must grow with cx.
    const ca::HandDetection right = make_det(300.0f, 100.0f, 500.0f, 200.0f);
    const ca::LogicalPos pr =
        ca::hand_map_coordinates(right, 640, 480, "size", false, false, false);
    ASSERT(pr.pos.x > p.pos.x);

    // invert_* is the spec 17 escape hatch for a mirrored camera mount: each
    // flag must flip exactly its own axis and nothing else.
    const ca::LogicalPos inv =
        ca::hand_map_coordinates(d, 640, 480, "size", true, false, true);
    ASSERT(std::fabs(inv.pos.x + p.pos.x) < 1e-6f);
    ASSERT(std::fabs(inv.pos.z + p.pos.z) < 1e-6f);
    ASSERT(std::fabs(inv.pos.y - p.pos.y) < 1e-6f);
    return true;
}

TEST(hand_mapper_depth_source_switch) {
    ca::HandDetection d = make_det(100.0f, 100.0f, 300.0f, 200.0f);
    // The landmark model's depth is WRIST-RELATIVE, so the wrist itself keeps
    // z == 0 - measured on the official sample, not assumed.
    for (size_t i = 1; i < d.landmarks.size(); ++i) d.landmarks[i].z = 10.0f;

    const ca::LogicalPos lz =
        ca::hand_map_coordinates(d, 640, 480, "landmark_z", false, false, false);
    ASSERT(std::fabs(lz.pos.y - (-10.0f / 480.0f)) < 1e-6f);

    // "size" must ignore that z entirely and use the apparent hand size.
    const ca::LogicalPos sz =
        ca::hand_map_coordinates(d, 640, 480, "size", false, false, false);
    ASSERT(sz.pos.y > 0.0f);
    ASSERT(std::fabs(sz.pos.y - 0.29462783f) < 1e-5f);

    // Degenerate boxes must not produce a garbage depth.
    ca::HandDetection flat = make_det(100.0f, 100.0f, 100.0f, 100.0f);
    const ca::LogicalPos f =
        ca::hand_map_coordinates(flat, 640, 480, "size", false, false, false);
    ASSERT(f.pos.y == 0.0f);
    return true;
}

// ---------------------------------------------------------------------------
// spec 20/21 - motion + deadzone
// ---------------------------------------------------------------------------
TEST(hand_motion_deadzone) {
    const float dz = 0.02f;
    ASSERT_EQ(ca::classify_motion_x(0.0f, dz), ca::MotionX::Stationary);
    ASSERT_EQ(ca::classify_motion_x(dz * 0.5f, dz), ca::MotionX::Stationary);
    ASSERT_EQ(ca::classify_motion_x(-dz * 0.5f, dz), ca::MotionX::Stationary);
    ASSERT_EQ(ca::classify_motion_x(dz * 2.0f, dz), ca::MotionX::Right);
    ASSERT_EQ(ca::classify_motion_x(-dz * 2.0f, dz), ca::MotionX::Left);

    ASSERT_EQ(ca::classify_motion_y(dz * 2.0f, dz), ca::MotionY::Forward);
    ASSERT_EQ(ca::classify_motion_y(-dz * 2.0f, dz), ca::MotionY::Backward);
    ASSERT_EQ(ca::classify_motion_y(0.0f, dz), ca::MotionY::Stationary);

    ASSERT_EQ(ca::classify_motion_z(dz * 2.0f, dz), ca::MotionZ::Up);
    ASSERT_EQ(ca::classify_motion_z(-dz * 2.0f, dz), ca::MotionZ::Down);
    ASSERT_EQ(ca::classify_motion_z(0.0f, dz), ca::MotionZ::Stationary);
    return true;
}

// ---------------------------------------------------------------------------
// spec 22 - gesture
// ---------------------------------------------------------------------------
TEST(hand_gesture_open_fist_ambiguous) {
    ASSERT_EQ(ca::hand_classify_gesture(make_hand(1.70f)).g, ca::Gesture::OpenPalm);
    ASSERT_EQ(ca::hand_classify_gesture(make_hand(1.00f)).g, ca::Gesture::Unknown);
    ASSERT_EQ(ca::hand_classify_gesture(make_hand(0.80f)).g, ca::Gesture::Fist);
    ASSERT_EQ(ca::hand_classify_gesture(make_hand(0.10f)).g, ca::Gesture::Fist);

    // Confidence is the share of the four fingers that voted.
    ASSERT(std::fabs(ca::hand_classify_gesture(make_hand(1.70f)).confidence - 1.0f) < 1e-6f);

    // Partial or degenerate landmark lists must never be classified.
    std::vector<ca::Keypoint> few(5);
    ASSERT_EQ(ca::hand_classify_gesture(few).g, ca::Gesture::Unknown);
    ASSERT_EQ(ca::hand_classify_gesture(std::vector<ca::Keypoint>{}).g, ca::Gesture::Unknown);
    return true;
}

TEST(hand_to_string_wire_contract) {
    // These strings go on the wire and are matched by the server and the web UI,
    // so they are asserted here rather than left to drift.
    ASSERT(std::string(ca::to_string(ca::Gesture::OpenPalm)) == "OPEN_PALM");
    ASSERT(std::string(ca::to_string(ca::Gesture::Fist)) == "FIST");
    ASSERT(std::string(ca::to_string(ca::Gesture::Unknown)) == "UNKNOWN");
    ASSERT(std::string(ca::to_string(ca::MotionX::Right)) == "RIGHT");
    ASSERT(std::string(ca::to_string(ca::MotionX::Left)) == "LEFT");
    ASSERT(std::string(ca::to_string(ca::MotionY::Forward)) == "FORWARD");
    ASSERT(std::string(ca::to_string(ca::MotionY::Backward)) == "BACKWARD");
    ASSERT(std::string(ca::to_string(ca::MotionZ::Up)) == "UP");
    ASSERT(std::string(ca::to_string(ca::MotionZ::Down)) == "DOWN");
    ASSERT(std::string(ca::to_string(ca::MotionZ::Stationary)) == "STATIONARY");
    ASSERT(std::string(ca::to_string(ca::Handedness::Left)) == "left");
    ASSERT(std::string(ca::to_string(ca::Handedness::Right)) == "right");
    ASSERT(std::string(ca::to_string(ca::Handedness::Unknown)) == "unknown");
    return true;
}

// ---------------------------------------------------------------------------
// spec 18/19 - reference point, EMA, motion integration
// ---------------------------------------------------------------------------
TEST(hand_analyzer_reference_latch_and_motion) {
    ca::HandAnalyzer an;
    ASSERT(an.init(base_cfg()));

    ca::HandState st;
    // Frames 1-2: detected, but the reference is NOT latched yet, and the delta
    // must read zero rather than "distance from the frame origin" (spec 18).
    for (uint64_t f = 1; f <= 2; ++f) {
        an.update(make_det(100.0f, 100.0f, 300.0f, 200.0f), 640, 480, f, f * 33, 30.0, st);
        ASSERT(st.detected);
        ASSERT(!st.reference_initialized);
        ASSERT(std::fabs(st.delta.x) < 1e-6f);
        ASSERT(std::fabs(st.delta.y) < 1e-6f);
        ASSERT(std::fabs(st.delta.z) < 1e-6f);
        ASSERT_EQ(st.motion.x, ca::MotionX::Stationary);
    }

    // Frame 3 (stable_frames = 3) latches it on the filtered position.
    an.update(make_det(100.0f, 100.0f, 300.0f, 200.0f), 640, 480, 3, 99, 30.0, st);
    ASSERT(st.reference_initialized);
    ASSERT(std::fabs(st.reference.x - 0.3125f) < 1e-6f);
    ASSERT(std::fabs(st.delta.x) < 1e-6f);

    // 5 px right = delta 0.0078 < deadzone 0.02 -> STATIONARY. This is what
    // stops hand tremor from chattering RIGHT/LEFT/RIGHT.
    an.update(make_det(105.0f, 100.0f, 305.0f, 200.0f), 640, 480, 4, 132, 30.0, st);
    ASSERT(std::fabs(st.delta.x - 0.0078125f) < 1e-6f);
    ASSERT_EQ(st.motion.x, ca::MotionX::Stationary);

    // 64 px right = +0.1 -> RIGHT.
    an.update(make_det(164.0f, 100.0f, 364.0f, 200.0f), 640, 480, 5, 165, 30.0, st);
    ASSERT(std::fabs(st.delta.x - 0.1f) < 1e-5f);
    ASSERT_EQ(st.motion.x, ca::MotionX::Right);

    // Back left of the reference -> LEFT.
    an.update(make_det(80.0f, 100.0f, 280.0f, 200.0f), 640, 480, 6, 198, 30.0, st);
    ASSERT_EQ(st.motion.x, ca::MotionX::Left);

    // Down the image (z shrinks) and larger on screen (y grows) -> DOWN+FORWARD.
    an.update(make_det(80.0f, 140.0f, 360.0f, 300.0f), 640, 480, 7, 231, 30.0, st);
    ASSERT_EQ(st.motion.z, ca::MotionZ::Down);
    ASSERT_EQ(st.motion.y, ca::MotionY::Forward);
    ASSERT(st.delta.z < 0.0f);
    ASSERT(st.delta.y > 0.0f);
    return true;
}

TEST(hand_analyzer_lost_frames_reset_reference) {
    ca::HandConfig cfg = base_cfg();
    cfg.reference_stable_frames = 2;
    cfg.lost_frames_to_reset    = 3;

    ca::HandAnalyzer an;
    ASSERT(an.init(cfg));

    ca::HandState st;
    an.update(make_det(100.0f, 100.0f, 300.0f, 200.0f), 640, 480, 1, 1, 30.0, st);
    ASSERT(!st.reference_initialized);
    an.update(make_det(100.0f, 100.0f, 300.0f, 200.0f), 640, 480, 2, 2, 30.0, st);
    ASSERT(st.reference_initialized);

    const ca::HandDetection none;   // detected == false
    an.update(none, 640, 480, 3, 3, 30.0, st);
    ASSERT(!st.detected);
    // One dropped frame must NOT discard the reference: that would flicker the
    // UI on a single missed inference.
    ASSERT(st.reference_initialized);
    an.update(none, 640, 480, 4, 4, 30.0, st);
    ASSERT(st.reference_initialized);
    // Third consecutive miss crosses lost_frames_to_reset -> full reset.
    an.update(none, 640, 480, 5, 5, 30.0, st);
    ASSERT(!st.reference_initialized);

    // A hand re-entering afterwards starts the latch from scratch.
    an.update(make_det(100.0f, 100.0f, 300.0f, 200.0f), 640, 480, 6, 6, 30.0, st);
    ASSERT(!st.reference_initialized);
    ASSERT(std::fabs(st.delta.x) < 1e-6f);
    return true;
}

TEST(hand_analyzer_ema_smoothing) {
    ca::HandConfig cfg = base_cfg();
    cfg.filter_alpha            = 0.4f;
    cfg.reference_stable_frames = 100;   // keep the reference out of the way

    ca::HandAnalyzer an;
    ASSERT(an.init(cfg));

    ca::HandState st;
    // First sample passes straight through - no previous value to blend with.
    an.update(make_det(100.0f, 100.0f, 300.0f, 200.0f), 640, 480, 1, 1, 30.0, st);
    ASSERT(std::fabs(st.position.x - 0.3125f) < 1e-6f);

    // Next raw x = 320/640 = 0.5; EMA gives 0.4*0.5 + 0.6*0.3125 = 0.3875.
    an.update(make_det(220.0f, 100.0f, 420.0f, 200.0f), 640, 480, 2, 2, 30.0, st);
    ASSERT(std::fabs(st.position.x - 0.3875f) < 1e-5f);
    return true;
}

TEST(hand_analyzer_gesture_debounce) {
    ca::HandConfig cfg = base_cfg();
    cfg.reference_stable_frames = 1;
    cfg.gesture_confirm_frames  = 5;

    ca::HandAnalyzer an;
    ASSERT(an.init(cfg));

    ca::HandState st;
    const ca::HandDetection open = make_det(100.0f, 100.0f, 300.0f, 200.0f, 1.70f);

    // The candidate is visible immediately; the CONFIRMED value only after
    // gesture_confirm_frames frames (spec 23), which is what keeps a one-frame
    // misclassification from reaching the UI.
    for (uint64_t f = 1; f <= 5; ++f) {
        an.update(open, 640, 480, f, f, 30.0, st);
        ASSERT_EQ(st.gesture_candidate, ca::Gesture::OpenPalm);
        if (f < 5) ASSERT_EQ(st.gesture, ca::Gesture::Unknown);
    }
    ASSERT_EQ(st.gesture, ca::Gesture::OpenPalm);
    ASSERT(std::fabs(st.gesture_confidence - 1.0f) < 1e-5f);

    // Now close the hand: the confirmed value must survive the first 4 frames.
    const ca::HandDetection fist = make_det(100.0f, 100.0f, 300.0f, 200.0f, 0.70f);
    for (uint64_t f = 0; f < 4; ++f) {
        an.update(fist, 640, 480, 100 + f, 100 + f, 30.0, st);
        ASSERT_EQ(st.gesture_candidate, ca::Gesture::Fist);
        ASSERT_EQ(st.gesture, ca::Gesture::OpenPalm);
    }
    an.update(fist, 640, 480, 200, 200, 30.0, st);
    ASSERT_EQ(st.gesture, ca::Gesture::Fist);
    return true;
}

TEST(hand_analyzer_rejects_bad_config) {
    ca::HandConfig cfg;
    ca::HandAnalyzer an;

    cfg.filter_alpha = 0.0f;
    ASSERT(!an.init(cfg));
    cfg.filter_alpha = 1.5f;
    ASSERT(!an.init(cfg));
    cfg.filter_alpha = 0.4f;
    ASSERT(an.init(cfg));

    cfg.motion_deadzone = -0.1f;
    ASSERT(!an.init(cfg));
    cfg.motion_deadzone = 0.02f;

    cfg.gesture_confirm_frames = 0;
    ASSERT(!an.init(cfg));
    cfg.gesture_confirm_frames = 5;

    cfg.reference_stable_frames = 0;
    ASSERT(!an.init(cfg));
    cfg.reference_stable_frames = 3;

    cfg.lost_frames_to_reset = 0;
    ASSERT(!an.init(cfg));
    return true;
}
