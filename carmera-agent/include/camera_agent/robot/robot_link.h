#pragma once

// RobotLink: HandState -> MeArmPilot commands.
//
// This is the Phase 2 boundary that hand_types.h deliberately left open:
//
//   hand_types.h : "Phase 2 adds HandState -> RobotCommand in a SEPARATE
//                   project. Nothing here may know about a robot, IK, servos,
//                   MuJoCo or MeArmPilot."
//
// So the dependency is one-way and lives on THIS side: RobotLink consumes a
// HandState, and HandState knows nothing about it. Nothing in the Hand* code
// is modified by this module.
//
// ── What it reproduces, and from where ──────────────────────────────────────
// The behaviour is a faithful port of MeArmPilot's own reference client
// `core/tools/gesture_bridge.py`, documented in
// `docs/protocol/gesture-control.md`. That document is the single source of
// truth for "how a visionflow client is supposed to behave"; this file invents
// nothing. The seven documented pitfalls are each handled explicitly:
//
//  1. zero delta is a parameter error, not a fault  -> skip tiny deltas
//  2. state.tcp is the COMMANDED value               -> usable as a basis, but
//                                                       never as "it arrived"
//  3. the server does not throttle                   -> we rate-limit ourselves
//  4. inside the bounding box may still be unreachable-> keep last_good, no slam
//  5. target == current produces no state frame      -> checked before sending
//  6. copying the bounds into the client drifts      -> bounds come from `caps`
//  7. no position feedback on the real arm           -> a green link is NOT
//                                                       proof of physical motion
//
// ── Threading ───────────────────────────────────────────────────────────────
// Every method runs on RobotLink's own thread (start()/stop() own it). update()
// is called from THAT thread, not from the AI thread - the AI thread only
// enqueues a copy of the HandState, mirroring how the metadata branch is fed.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "camera_agent/ai/hand_types.h"
#include "camera_agent/config.h"
#include "camera_agent/robot/json_line_client.h"

namespace ca {

// Parsed `caps` reply: the only place the client is allowed to learn the
// geometry. Every number here is derived by MeArmPilot from its robot.yaml.
struct RobotCaps {
    bool        valid = false;
    int         protocol = 0;
    std::string device;                       // sim | serial | mujoco
    // `workspace` - the TCP bounding box (mm). Coarse clamp ONLY: the reachable
    // set is a doughnut shell cut by joint limits, so the inside of this box can
    // still be unreachable. The final word is always the command's `ok`.
    float ws_min[3] = {0, 0, 0};
    float ws_max[3] = {0, 0, 0};
    // HOME TCP position (mm), read back from the `home` reply. Used as the
    // reference point that "return home" drives to.
    float home_tcp[3] = {0, 0, 0};
    bool  home_valid = false;
    std::vector<std::string> commands;        // capability probe, never guess
};

// Outcome of one command, kept so the caller can log raw protocol text.
struct RobotCommandResult {
    bool        ok = false;
    bool        sent = false;      // false = skipped deliberately (e.g. zero delta)
    std::string error;             // verbatim server error, when ok == false
};

class RobotLink {
public:
    RobotLink();
    ~RobotLink();

    RobotLink(const RobotLink&) = delete;
    RobotLink& operator=(const RobotLink&) = delete;

    // Creates the transport. Returns false on bad configuration (the caller
    // keeps a fully working video + AI pipeline: robot control is optional).
    bool init(const RobotConfig& cfg);

    // Replace the transport before start() - used by the unit tests to drive the
    // whole mapping/rate state machine against a scripted fake, with no socket.
    void set_client(std::unique_ptr<IJsonLineClient> c);

    // Start / stop the worker thread. start() does NOT connect: the connection
    // is established (and re-established) by the thread, so an absent backend
    // can never delay or fail the video pipeline's startup.
    void start();
    void stop();
    bool is_running() const { return running_.load(); }

    // Producer side. Called from the AI thread; enqueues a COPY of the state and
    // returns immediately. Never blocks on the network, never throws. A full
    // queue drops the OLDEST entry - a stale hand position is worse than none.
    void push_hand(const HandState& hs);

    // ---- Everything below runs on the worker thread -----------------------
    // Exposed for the unit tests, which drive them directly (deterministic, no
    // sleeps, no server).

    // caps -> home, in that order, exactly as gesture-control.md §1 requires.
    // Returns false when caps cannot be obtained (then nothing is ever sent).
    bool open();

    // Connection state machine, driven by the worker loop.
    bool connect_if_needed();

    // One hand state in. Returns false when the TRANSPORT is unusable (socket
    // died, or nothing was connected). A command the server refused - or one we
    // deliberately skipped because it was below the deadzone - is a normal
    // protocol outcome and still returns true; the caller must not act on it.
    // Every individual command's outcome is also accumulated into the counters
    // below, which is what the tests assert on. `now_ms` is passed in so the
    // rate limit is testable without real clocks.
    bool update(const HandState& hs, int64_t now_ms);

    // --- protocol primitives (each sends exactly one command) --------------
    RobotCommandResult do_home();
    RobotCommandResult do_gripper(bool close_it);
    RobotCommandResult do_movexyz(float dx, float dy, float dz);
    RobotCommandResult do_moveto(float x, float y, float z);

    // Normalised [0,1]^3 -> mm, bounds taken from caps. Public so the test can
    // assert the mapping independently of any command.
    void norm_to_mm(const Vec3& n, float out_mm[3]) const;

    // Soft-boundary taper (delta route only). Returns the gain multiplier to
    // apply on `axis` for a step of `step_mm`, given the arm's commanded
    // position `pos_mm[axis]`. In [soft_min_gain, 1]:
    //   1   - the arm is comfortably inside the workspace
    //   ->soft_min_gain - the arm is on (or past) the learned effective bound
    // Monotone and smooth (cosine ease) so the arm decelerates into the edge
    // instead of hitting a discontinuity. Returns 1 when the taper is off or
    // caps is missing. Public so the test can assert the curve on its own.
    //
    // NOTE the bounds used here are NOT caps.ws_min/ws_max directly. Read the
    // long comment on eff_bound_ in the .cpp: the caps box is much larger than
    // the truly reachable set, so the taper runs against bounds the link LEARNS
    // from the refusals the backend actually sends back.
    float boundary_weight(const float pos_mm[3], int axis, float step_mm = 0.0f) const;

    // True when a step of `step_mm` on `axis` would take the arm past its
    // learned bounds. Such a step is suppressed outright rather than sent at the
    // floor gain (see the call site in step()).
    bool push_out_of_bound(const float pos_mm[3], int axis, float step_mm) const;

    // The bound pair the taper actually runs against, in mm: the learned bounds
    // when the link has seen a refusal, else a band of the MEASURED span centred
    // on the caps home pose. The caps box is never used as-is - it overstates
    // the reachable set by up to 4x, and a taper measured against it does not
    // engage until long after the arm has already been refused. See the
    // measurement table on RobotConfig::reach_span_x.
    //
    // `margin_out` (optional) receives the taper width for that axis, which is
    // `soft_margin` of the same span - NOT of the caps span, or the slow zone
    // would cover most of a tight axis (X: 20.3 mm of margin over a 34 mm span).
    void effective_bounds(int axis, float& lo_out, float& hi_out,
                          float& margin_out) const;

    // The configured measured span (mm) for one axis; 0 when unset.
    float measured_span(int axis) const;

    // The band the taper runs against before anything has been learned: the
    // measured span centred on the caps home pose, or the caps box when no span
    // is configured. Identical to effective_bounds()' non-learned branch, so the
    // learned bounds can relax back to exactly this and "relaxed all the way
    // back" stays indistinguishable from "nothing learned yet".
    void prior_bounds(int axis, float& lo_out, float& hi_out) const;

    // --- introspection (for logs and tests) --------------------------------
    const RobotCaps& caps() const { return caps_; }
    bool  connected() const { return client_ && client_->connected(); }
    int64_t commands_sent()     const { return sent_.load(); }
    int64_t commands_rejected() const { return rejected_.load(); }
    int64_t commands_skipped()  const { return skipped_.load(); }
    int64_t reconnects()        const { return reconnects_.load(); }
    // The last COMMANDED pose (mm). This is the model of where the arm is: the
    // real arm has no feedback (pitfall 7), so this is the reply's `tcp` from
    // the last accepted command. Exposed so a test can assert the arm actually
    // TRAVELLED, which is the only way to catch "the link is healthy but the
    // arm never moves".
    ca::Vec3 last_tcp() const {
        return ca::Vec3{last_good_mm_[0], last_good_mm_[1], last_good_mm_[2]};
    }
    bool have_last_tcp() const { return have_last_good_; }
    // Hand presence as the worker sees it (drives the away/arrive home rule).
    bool hand_present() const { return hand_present_.load(); }
    bool hand_was_away() const { return hand_left_seen_.load(); }
    bool home_on_return() const { return cfg_.home_on_presence; }

    // Whether the last logged config was applied (tests assert on this).
    const RobotConfig& config() const { return cfg_; }

private:
    void thread_loop();
    // Fold one queued state into commands. Returns false when the link is down.
    bool step(const HandState& hs, int64_t now_ms);
    bool step_exchange(const RobotCommandResult& r);

    // Soft-boundary bound learning. `from_mm` is the position the refused step
    // started at and `step_mm` the step that was rejected.
    //
    // The learned bound is placed at the ATTEMPTED TARGET (`from + step`), never
    // at `from`. Pinning it to the arm's own pose leaves the arm sitting exactly
    // on the wall, where the taper is at its floor and relaxation can never fire
    // - a permanent loss of that side of the axis. See the long comment in the
    // .cpp for the measured instance of this.
    void learn_from_rejection(const float from_mm[3], const float step_mm[3]);
    // An accepted step tells us where the arm actually got to, AND lets every
    // learned bound drift back toward its prior band. The drift is what stops a
    // single refusal from closing an axis for the rest of the session.
    void relax_learned_bound(const float reached_mm[3]);

    // Returns true when the reply means "this frame needs no command".
    static bool is_benign_skip(const std::string& error);

    RobotConfig cfg_{};
    RobotCaps   caps_{};
    std::unique_ptr<IJsonLineClient> client_;

    // --- queue: AI thread -> worker thread ---------------------------------
    mutable std::mutex      q_mtx_;
    std::condition_variable q_cv_;
    std::deque<HandState>   queue_;

    std::atomic<bool> running_{false};
    std::thread       thread_;

    // --- worker-thread-only state -----------------------------------------
    int64_t last_send_ms_ = 0;
    // The last TCP target we COMMANDED (not the one the arm reports back).
    // Needed because the arm has no position feedback: the `tcp` in a reply is
    // the commanded value, and for `home` it is the home pose - so comparing a
    // new target against the reply would never match and the deadzone guard
    // below would let the same target out again and again (pitfall 5).
    bool  have_last_good_ = false;
    float last_good_mm_[3] = {0, 0, 0};

    // --- frame-to-frame differencing (delta route) -------------------------
    // THE BUG THIS FIXES. HandState::delta is `position - reference`, and the
    // reference is latched ONCE and never moves (hand_analyzer.cpp step 3). So
    // the field is a CUMULATIVE offset from wherever the hand first settled -
    // it does not fall back to zero when the hand stops.
    //
    // Treating it as "how far to move this frame" therefore re-sends the same
    // displacement every frame: hand moves 0.10 and holds, and the arm is
    // commanded 0.10 worth of millimetres 8 times a second. Measured symptom:
    // the arm crept, stalled at the deadzone, then jumped. The reference client
    // (gesture_bridge.py::nudge) takes a per-call increment, so nothing there
    // contradicted the wrong reading.
    //
    // The arm wants the INCREMENT, so this link does the differencing itself:
    //   increment = delta_now - delta_prev
    // which is exactly (position_now - position_prev) and is zero when the hand
    // holds still. Keeping it here - rather than changing the analyzer's
    // reference policy - leaves HandState a pure per-frame report and keeps the
    // metadata/UI consumers of `delta` (which really do want the cumulative
    // value) untouched.
    bool  have_prev_delta_ = false;
    float prev_delta_[3] = {0, 0, 0};

    // --- gesture mute window -----------------------------------------------
    // Until this timestamp, continuous motion is suppressed: a discrete gesture
    // has just fired, and the hand motion that accompanied forming it must not
    // steer the arm. On expiry the differencing basis is re-latched to the
    // current delta, so everything that happened during the window is dropped
    // instead of being replayed as one large step.
    int64_t muted_until_ms_ = 0;

    // --- learned effective reachable bounds (soft boundary) ----------------
    // caps.ws_min/ws_max is a BOUNDING BOX, and a loose one. Measured against
    // the real sim backend at the home height: x stops being solvable at ~150 mm
    // while the box advertises 175.88 mm. Tapering against the box therefore
    // does nothing useful - at the point the arm actually jams, the box still
    // says there are 26 mm to go, so the weight is 1 and the step goes out at
    // full size to be refused again.
    //
    // So the taper runs against bounds the link LEARNS. The only trustworthy
    // signal available is the refusal itself: when a command that would have
    // moved the arm further in +x comes back rejected, the effective +x bound is
    // at most where the arm was. Each refusal pulls that side in; a later
    // acceptance relaxes it back out (the reachable shell is not axis-aligned,
    // so a bound learned at one y/z is conservative - and can be wrong - for
    // another).
    //
    // The pair starts at the caps box and only ever moves inward first, so with
    // no refusals at all the behaviour is exactly the caps-based one.
    //
    // The pull-in is NOT permanent. Every kRelaxEveryNAccepts accepted commands
    // each side drifts kRelaxToPriorFraction of the way back toward the axis'
    // prior band (prior_bounds()), so a bound learned from one refusal - possibly
    // one caused by a DIFFERENT axis, since the backend refuses the whole command
    // - expires instead of closing that direction for the rest of the session.
    float eff_min_[3] = {0, 0, 0};
    float eff_max_[3] = {0, 0, 0};
    bool  eff_init_ = false;
    // Relaxation is deliberately slower than tightening: a bound pulled in by a
    // real refusal is probably real, whereas one acceptance only proves THIS
    // pose is reachable.
    int   eff_relax_count_ = 0;
    // Edge detection for the discrete gestures: an held OpenPalm must emit ONE
    // home, not one per frame (the server does not deduplicate for us).
    Gesture last_gesture_ = Gesture::Unknown;
    bool    gripper_closed_ = false;
    // Presence: true while a hand is being reported. A hand that disappears and
    // comes back re-establishes the basis and returns HOME (configured).
    bool    hand_seen_ = false;
    // False until the first state of the session has been folded in. Without it
    // the very first hand of a session would look like a "return" and trigger a
    // redundant HOME right after open() already sent one.
    bool    presence_established_ = false;
    // Worker-clock timestamp of the last frame that DETECTED a hand. The
    // departure test is `now_ms - last_seen_ms_ >= cfg_.hand_lost_ms`, i.e. a
    // CONTINUOUS absence - any detection in between rewinds the clock, which is
    // what makes a flicker harmless. 0 means "never seen yet", which keeps the
    // test false so an absent-from-the-start session cannot fire a spurious
    // departure (and `hand_seen_` is false then anyway).
    //
    // Deliberately a timestamp, not a frame counter: see RobotConfig::hand_lost_ms
    // for the measured reason (detections come in bursts, and the frame rate is
    // not constant enough for a frame count to mean a fixed amount of time).
    int64_t last_seen_ms_ = 0;
    int     miss_streak_ = 0;
    int64_t backoff_until_ms_ = 0;
    int64_t next_connect_attempt_ms_ = 0;
    // Latches the "backend unreachable" WARN so an outage produces exactly one
    // WARN at info level instead of one line per reconnect attempt. Re-armed
    // once a connection succeeds, so a later outage warns again.
    bool    connect_failed_warned_ = false;

    // --- atomics: worker -> any reader (logs, tests) -----------------------
    std::atomic<bool>    hand_present_{false};
    std::atomic<bool>    hand_left_seen_{false};
    std::atomic<int64_t> sent_{0};
    std::atomic<int64_t> rejected_{0};
    std::atomic<int64_t> skipped_{0};
    std::atomic<int64_t> reconnects_{0};
};

} // namespace ca
