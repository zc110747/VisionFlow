#pragma once

#include <string>

#include "camera_agent/ai/ai_types.h"
#include "camera_agent/metadata/metadata_types.h"

namespace ca {

struct CameraConfig {
    int         id = 0;
    int         width = 1280;
    int         height = 720;
    int         fps = 30;
    bool        auto_res = false;   // true: don't force caps, let camera negotiate native format
};

struct EncoderConfig {
    std::string codec = "h264";   // h264
    int         bitrate = 4000;   // kbps
    int         keyframe_interval = 30;
};

struct StreamConfig {
    std::string id = "camera01";
};

struct RtspConfig {
    std::string server = "127.0.0.1";
    int         port = 8554;
};

// ---- Robot control branch (MeArmPilot, TCP JSON Lines) ---------------------
// Optional and default-OFF. It only ever becomes active while the AI branch is
// in Hand mode (the web UI's "ai-hand"), which is what makes the whole feature
// opt-in from the operator's point of view.
//
// Every failure mode here degrades to a WARN log: an unreachable, slow or
// refusing backend must not be able to affect the video, AI or metadata
// branches (same rule those two already follow).
struct RobotConfig {
    bool        enable = false;      // master switch; still requires AI hand mode
    std::string host = "127.0.0.1";
    int         port = 9100;         // MeArmPilot TCP JSON Lines
    int         timeout_ms = 2000;   // per-command socket timeout
    int         reconnect_ms = 2000; // delay between reconnect attempts

    // How a hand movement becomes arm movement.
    //   delta    : the hand's frame-to-frame displacement -> movexyz (relative).
    //              "hand left, arm left". Follows the hand naturally and cannot
    //              be trapped by the "inside the box but unreachable" case.
    //   absolute : the hand's position in the frame -> moveto. "point and go",
    //              but an unreachable target must keep the last good point.
    std::string map_mode = "delta";

    // Client-side throttle. The server does NOT throttle (min_send_interval_ms
    // is 0), and sending faster than the arm can follow just makes commands
    // overwrite each other, so the documented 10-20 Hz is enforced here.
    int rate_hz = 15;

    // Normalised-unit -> mm gain per axis, applied to the DELTA route only.
    // The absolute route derives its scale from caps.workspace instead, so that
    // no bound is ever copied into this project (pitfall 6).
    //
    // The effective gain is `gain_axis * reach_span_axis / 0.5`. Values below
    // 1.0 make the arm move LESS than the hand, which is how hand-tracking
    // jitter is kept out of the arm. Tuned live on 2026-09-29: the near-1:1
    // response was too jumpy, and dropping X/Z to 0.25 puts their per-frame
    // jitter (a 0.02-normalised hand tremor, the analyzer's motion_deadzone)
    // under the 0.5 mm minimum step, so it is filtered rather than acted on.
    // Y stays at 0.5 because its span is 4.5x X's - the raw numbers are not
    // comparable across axes, only their mm-per-normalised-unit products are.
    float gain_x = 0.25f;
    float gain_y = 0.5f;
    float gain_z = 0.25f;

    // Below this the command is skipped rather than sent. MeArmPilot rejects an
    // all-zero vector as a parameter error, and hand tremor would otherwise
    // produce a meaningless micro-move every frame (pitfall 1).
    float deadzone_mm = 0.5f;

    // ---- soft boundary (delta route only) -----------------------------------
    // The delta route follows the hand, so the hand can keep travelling after
    // the arm has reached the edge of its workspace; every further step in that
    // direction is refused with JOINT_LIMIT until the hand comes back. Measured
    // live: a 20 s run produced rejected=83 against sent=12 - the link stays
    // healthy, but "the hand moves and the arm does not" is a poor experience.
    //
    // With this on, the per-axis gain is tapered as the arm approaches its
    // bound, so the arm eases into the edge instead of grinding against it. The
    // taper is driven by the arm's COMMANDED position (the only position we
    // have: the real arm has no feedback, pitfall 7), i.e. it is a best-effort
    // model of where the arm is - exact on sim, approximate on real hardware.
    bool  soft_boundary = true;

    // Fraction of each axis' workspace span, measured from each end, over which
    // the gain is tapered. 0.15 means the last 15% before a bound is a "slow
    // zone". Too large and the arm feels weak everywhere; too small and it is
    // still a hard stop.
    float soft_margin = 0.15f;

    // The tapered gain never reaches zero: a weight of exactly 0 would make the
    // arm unable to travel back OUT of the margin (the hand would have to leave
    // the frame to recover), and it would also turn every frame into a skip.
    // This floor keeps a slow escape route while still damping the approach.
    float soft_min_gain = 0.10f;

    // ---- measured reach (delta-route gain basis) ----------------------------
    // `caps.workspace` is a BOUNDING BOX, and a badly inflated one: it is built
    // from the geometric reach, not from the set of poses the IK can actually
    // solve. Measured against this project's sim backend (2026-09-29, absolute
    // `moveto` bisection at the HOME height via scripts/measure_reach.py):
    //
    //   axis        caps box            measured reachable    span     infl
    //   X front     [40.46, 175.88]     [105.78, 144.78]      39.0 mm  3.5x
    //   Y left      [-153.0, 153.0]     [-88.0, 88.0]        176.0 mm  1.7x
    //   Z up        [49.08, 114.68]     [72.97, 114.47]       41.5 mm  1.6x
    //
    // Worse, HOME is not centred in the true range: at HOME the arm has only
    // ~9 mm of travel left in -X and ~5 mm in +Z. Deriving the gain from the
    // box (X gain 135.43/0.5 = 270.9) therefore makes a single frame of hand
    // motion command 13-27 mm - several times the entire remaining margin - so
    // the very first step is refused, the learner reads that as "this whole
    // direction is unreachable", and the axis latches shut. That is exactly the
    // reported symptom: the hand moves, the arm does not.
    //
    // These are the spans the gain is derived from instead. Empty (0) means
    // "fall back to the caps box" - correct for a backend whose box really is
    // tight, and the only sane default for a device we have not measured.
    // Values are SPANS in mm, not bounds: the centre comes from the caps home
    // pose, so a re-fitted arm only needs its span updated.
    float reach_span_x = 39.0f;
    float reach_span_y = 176.0f;
    float reach_span_z = 41.5f;

    // A refused step is a "too big" answer, not a "never" answer: before the
    // step is written off, retry once at half length. Without this the first
    // refusal at a wall teaches the learner that the axis is closed, and the
    // arm stops short of ground it could still cover (measured: -X has 5 mm of
    // usable travel left at HOME, but the old code surrendered on the first
    // refusal because the commanded step was 13 mm).
    bool  soft_probe_on_reject = true;

    // Attempts (including the original) when probing after a refusal. 2 = one
    // retry at half length.
    int   soft_probe_tries = 2;

    // Discrete gesture bindings. Behaviour mirrors gesture-control.md §5:
    //   open_palm -> home        (also the "reset" of the mapping basis)
    //   fist      -> gripper close / open
    //   off       -> do not bind that gesture
    std::string gesture_home = "open_palm";   // open_palm | fist | off
    bool        gesture_gripper = true;

    // Hand left the frame and came back -> return HOME and re-establish the
    // basis instead of continuing from a stale origin. This is the "回归 home"
    // rule; setting it false keeps the arm wherever it was.
    bool home_on_presence = true;

    // Frames a hand must be absent before it counts as "left". The HandAnalyzer
    // already resets its own reference after `lost_frames_to_reset`, and this
    // gives the robot side an independent, configurable reaction point.
    //
    // Measured against the AI inference rate (the hand branch runs ~8 Hz, so a
    // frame is ~125 ms), NOT the camera rate. The cascade drops the occasional
    // frame on a real hand: on a live LRCP USB2.0 capture the misses came in
    // bursts of 1-2 frames (~0.25 s) while the hand never left the view. The
    // old value of 3 sat on top of that, so a flicker read as a real exit,
    // returned the arm HOME and broke the follow. 6 rides out the burst.
    int lost_frames = 6;

    // --- gesture mute window -------------------------------------------------
    // After a discrete gesture fires (gripper open/close, or home), continuous
    // motion is MUTED for this long. Why it is needed at all:
    //
    // Making a fist is not a still pose. The fingers curl, the landmark bbox
    // shrinks and its centre drifts, so the HandState's position genuinely moves
    // while the operator is forming the gesture - and the classifier only
    // confirms after `gesture_confirm_frames` (5) frames. So a fist produces
    // half a second of honest-looking hand motion BEFORE it is recognised as a
    // gesture at all, and that motion was being followed by the arm.
    //
    // The operator's intent is "make a switch, do not move" (measured: the arm
    // visibly walked away while the fist formed). Mutting is the only way to say
    // "this stretch of hand motion was gesture-forming, not steering".
    //
    // 500 ms covers the 5-frame confirm window (~625 ms at the hand branch's
    // ~8 Hz) plus the curl itself. The basis is re-latched when the window
    // ends, so motion during the window is DISCARDED rather than replayed -
    // replaying it would turn a held fist back into a jump.
    int gesture_mute_ms = 500;

    bool log_commands = true;   // log every command with the raw reply
};

struct Config {
    CameraConfig  camera;
    EncoderConfig encoder;
    StreamConfig  stream;
    RtspConfig    rtsp;
    AIConfig      ai;       // independent AI branch (spec 18)
    MetadataConfig metadata; // async AI result upload (Phase 2)
    RobotConfig   robot;    // gesture -> MeArmPilot control (Phase 2, opt-in)
    std::string   device_id = "camera01";
    std::string   source = "auto";     // camera source element; "auto" or a GStreamer src name (e.g. videotestsrc for testing)
    std::string   log_level = "info";
    bool        measure_latency = false; // --latency-probe: instrument per-stage latency
};

// Build the default configuration (matches camera-agent.yaml defaults).
Config make_default_config();

// Load configuration from a YAML file. Missing keys fall back to defaults.
// Returns true on success (file optional: if absent, defaults are used).
bool load_config(Config& cfg, const std::string& path);

} // namespace ca
