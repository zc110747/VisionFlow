// RobotLink tests.
//
// These reproduce MeArmPilot's own reference-client self check
// (`core/tools/gesture_bridge.py --demo`, 7 items) on the C++ side, plus the
// behaviour this project adds on top (presence -> HOME, reference gating).
//
// A scripted fake transport is substituted for the socket, so the whole
// mapping / rate / edge-detection state machine is exercised with no backend
// process, no ports and no timing flakiness. The fake answers with the EXACT
// reply shapes captured from a live `armpilot-backend.exe` on 2026-09-29
// (protocol v2), so the parsers are tested against real payloads.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "camera_agent/ai/hand_types.h"
#include "camera_agent/config.h"
#include "camera_agent/robot/json_line_client.h"
#include "camera_agent/robot/robot_link.h"
#include "test_harness.h"

using namespace ca;

namespace {

// ---------------------------------------------------------------------------
// A scripted stand-in for the TCP link.
// ---------------------------------------------------------------------------
class FakeClient : public IJsonLineClient {
public:
    bool connect() override {
        if (fail_connect_) return false;
        is_connected_ = true;
        return true;
    }
    void close() override { is_connected_ = false; }
    bool connected() const override { return is_connected_; }

    bool exchange(const std::string& request, JsonReply& reply) override {
        requests.push_back(request);
        if (!is_connected_) return false;
        if (drop_next_) {          // simulate a transport-level failure
            drop_next_ = false;
            is_connected_ = false;
            return false;
        }

        reply.raw.clear();
        reply.ok = true;
        reply.error.clear();

        // caps
        if (request.find("\"caps\"") != std::string::npos) {
            reply.raw = kCaps;
        } else if (request.find("\"home\"") != std::string::npos) {
            reply.raw = kHome;
            tcp_ = {115.03347742002849f, 0.0f, 109.22362788339144f};
        } else if (request.find("\"movexyz\"") != std::string::npos) {
            // Reproduce the real server rule: an all-zero vector is a parameter
            // error, everything else succeeds.
            if (request.find("[0.000,0.000,0.000]") != std::string::npos) {
                reply.ok = false;
                reply.error = "invalid delta: zero displacement";
            } else if (reject_movexyz_above_mm_ > 0.0f &&
                       first_delta_magnitude(request) > reject_movexyz_above_mm_) {
                // Reproduce the real WALL rule: the backend refuses any step
                // that would land outside the solvable set, which for a
                // follow-the-hand command means "too big a step in that
                // direction". Zero displacement never lands here (handled
                // above), so the probe's halved retry is what this exercises:
                // a step above the threshold is refused, the same motion at
                // half length is accepted.
                reply.ok = false;
                reply.error = "JOINT_LIMIT: 目标几何可达，但两支解都越界";
            } else {
                reply.raw = kState;
                // The soft-boundary tests need the arm's COMMANDED position to
                // be a controlled input, because that is what the taper reads.
                // Live, this comes back in the reply's `tcp` (pitfall 2); here
                // the test sets it directly.
                if (report_movexyz_tcp_) {
                    char buf[192];
                    std::snprintf(buf, sizeof(buf),
                                  R"({"ok": true, "cmd": "movexyz", "state": {"joints": {}, )"
                                  R"("tcp": [%.4f, %.4f, %.4f], "device": "sim"}})",
                                  movexyz_tcp_[0], movexyz_tcp_[1], movexyz_tcp_[2]);
                    reply.raw = buf;
                }
            }
        } else if (request.find("\"moveto\"") != std::string::npos) {
            // Reproduce the real rejection. The interesting case is a target
            // INSIDE the bounding box but outside the reachable shell - the box
            // is only a coarse clamp. The fake rejects the box's extreme -y
            // corner, which is exactly where "still in the box, still
            // unreachable" lives.
            if (request.find("-153.012") != std::string::npos) {
                reply.ok = false;
                reply.error = "JOINT_LIMIT: no valid branch";
            } else {
                reply.raw = kState;
            }
        } else if (request.find("\"gripper\"") != std::string::npos) {
            reply.raw = kState;
        } else {
            reply.ok = false;
            reply.error = "unknown cmd";
        }
        return true;
    }

    std::vector<std::string> requests;
    bool fail_connect_ = false;
    bool drop_next_ = false;
    std::vector<float> tcp_{115.0f, 0.0f, 109.0f};
    // When set, every successful movexyz reports this as the commanded `tcp`.
    // Used by the soft-boundary tests, which need "where the arm is" to be an
    // input they control rather than the fixed captured payload.
    bool report_movexyz_tcp_ = false;
    float movexyz_tcp_[3] = {115.03f, 0.0f, 109.22f};
    // When > 0, a movexyz whose delta magnitude exceeds this is refused with
    // JOINT_LIMIT - i.e. the fake backend has a wall. Lets a test drive the
    // probe-on-refusal path with a step the fake would accept once halved.
    float reject_movexyz_above_mm_ = 0.0f;

private:
    // |[dx,dy,dz]| of the first delta array in a movexyz request; 0 on a parse
    // miss (which makes the rejection rule above a no-op rather than a bogus
    // refusal).
    //
    // Hand-rolled rather than sscanf: sscanf is flagged as unsafe by MSVC's
    // deprecation lint (C4996) and the project builds warning-free.
    static float first_delta_magnitude(const std::string& req) {
        const size_t lb = req.find('[');
        if (lb == std::string::npos) return 0.0f;
        float v[3] = {0.0f, 0.0f, 0.0f};
        size_t i = lb + 1;
        for (int k = 0; k < 3; ++k) {
            const size_t comma = req.find_first_of(",]", i);
            if (comma == std::string::npos) return 0.0f;
            try {
                v[k] = std::stof(req.substr(i, comma - i));
            } catch (...) {
                return 0.0f;
            }
            i = comma + 1;
        }
        return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }
    bool is_connected_ = false;

    // Captured verbatim from a live backend (fields trimmed to what we parse).
    static constexpr const char* kCaps =
        R"({"ok": true, "cmd": "caps", "caps": {"protocol": 2, "device": "sim", )"
        R"("units": {"length": "mm", "angle": "deg"}, )"
        R"("joints": [{"id": "base", "min": -60, "max": 60}], )"
        R"("workspace": {"min": [40.45820357387613, -153.0122829829571, 49.08488994998381], )"
        R"("max": [175.88332957985008, 153.0122829829571, 114.67984540077312], "samples": 12}, )"
        R"("commands": ["move", "gripper", "servo", "state", "movexyz", "moveto", "home", "caps"]}})";

    static constexpr const char* kHome =
        R"({"ok": true, "cmd": "home", "state": {"joints": {"base": 0, "gripper": 50}, )"
        R"("servo": [90, 90.00000000002939, 90.00000000006139, 90], )"
        R"("tcp": [115.03347742002849, 0, 109.22362788339144], "device": "sim"}})";

    static constexpr const char* kState =
        R"({"ok": true, "cmd": "movexyz", "state": {"joints": {"base": -1.43}, )"
        R"("tcp": [120.03347742002852, -3.0000000000000004, 111.22362788339144], "device": "sim"}})";
};

// Build a HandState that looks like a latched, detected hand.
HandState make_hand() {
    HandState hs;
    hs.detected = true;
    hs.confidence = 0.95f;
    hs.reference_initialized = true;
    hs.position = {0.5f, 0.5f, 0.5f};
    hs.reference = {0.5f, 0.5f, 0.5f};
    hs.frame_id = 1;
    return hs;
}

// A detected hand with no reference latched yet - hand_types.h says delta MUST
// NOT be consumed in this state.
HandState make_hand_unlatched() {
    HandState hs = make_hand();
    hs.reference_initialized = false;
    return hs;
}

HandState make_no_hand() {
    HandState hs;
    hs.detected = false;
    hs.frame_id = 99;
    return hs;
}

RobotConfig test_cfg() {
    RobotConfig c;
    c.enable = true;
    c.map_mode = "delta";
    c.rate_hz = 15;
    c.gesture_home = "open_palm";
    c.gesture_gripper = true;
    c.home_on_presence = true;
    c.lost_frames = 3;
    c.deadzone_mm = 0.5f;
    // Pin the gains to 1.0. The shipped defaults are tuned for feel (currently
    // 0.25/0.5/0.25 on this arm - see RobotConfig::gain_x) and a test that
    // silently inherited them would have its hard-coded step arithmetic like
    // `0.10 * 270.85 = 27.09` drift out from under it every time someone
    // retunes. Behaviour under test is the taper / probe / mapping, not the
    // feel, so the basis is held fixed here. Feel changes get their own test.
    c.gain_x = 1.0f;
    c.gain_y = 1.0f;
    c.gain_z = 1.0f;
    // No measured span: the soft-boundary band falls back to the caps box. The
    // tests that drive a FAKE arm therefore taper against the fake box, which is
    // the only geometry they know about. Production ships measured spans
    // (34/176/42 on this sim backend - see RobotConfig::reach_span_x) and the
    // measured-span path has its own test, robot_soft_boundary_uses_measured_span,
    // so a test here never silently inherits a number it cannot reason about.
    c.reach_span_x = 0.0f;
    c.reach_span_y = 0.0f;
    c.reach_span_z = 0.0f;
    return c;
}

// Owns a RobotLink plus the fake it is wired to, so the tests can inspect both.
struct Fixture {
    std::unique_ptr<FakeClient> fake;
    RobotLink link;
    FakeClient* fp = nullptr;

    explicit Fixture(const RobotConfig& cfg) {
        fake = std::make_unique<FakeClient>();
        fp = fake.get();
        link.set_client(std::move(fake));
        link.init(cfg);
    }
};

// Count requests whose text contains `needle`.
int count_cmds(const std::vector<std::string>& reqs, const std::string& needle) {
    int n = 0;
    for (const auto& r : reqs) {
        if (r.find(needle) != std::string::npos) ++n;
    }
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// ① open() establishes the documented session: caps, then home.
// ---------------------------------------------------------------------------
TEST(robot_open_does_caps_then_home) {
    Fixture f(test_cfg());
    ASSERT(f.link.connect_if_needed());
    ASSERT_EQ(f.fp->requests.size(), static_cast<size_t>(2));
    ASSERT(f.fp->requests[0].find("\"caps\"") != std::string::npos);
    ASSERT(f.fp->requests[1].find("\"home\"") != std::string::npos);

    // The bounds must come from caps, not from this project.
    const RobotCaps& c = f.link.caps();
    ASSERT(c.valid);
    ASSERT_EQ(c.protocol, 2);
    // 40.45820357387613 .. 175.88332957985008
    ASSERT(c.ws_min[0] > 40.45f && c.ws_min[0] < 40.46f);
    ASSERT(c.ws_max[0] > 175.88f && c.ws_max[0] < 175.89f);
    // home must have recorded a TCP basis
    ASSERT(c.home_valid);
    return true;
}

// ---------------------------------------------------------------------------
// ② Capability probe: a v1-only backend (no movexyz) must be refused, not
//    guessed at from the protocol number.
// ---------------------------------------------------------------------------
TEST(robot_open_rejects_backend_without_movexyz) {
    class NoV2Client : public FakeClient {
    public:
        bool exchange(const std::string& req, JsonReply& reply) override {
            requests.push_back(req);
            reply.raw = R"({"ok": true, "cmd": "caps", "caps": {"protocol": 1, )"
                        R"("workspace": {"min": [0,0,0], "max": [1,1,1]}, )"
                        R"("commands": ["move", "gripper", "servo", "state"]}})";
            reply.ok = true;
            return true;
        }
    };
    auto fake = std::make_unique<NoV2Client>();
    NoV2Client* fp = fake.get();
    RobotLink link;
    link.set_client(std::move(fake));
    link.init(test_cfg());
    ASSERT(link.connect_if_needed() == false);
    // It read caps, found no movexyz, and stopped - it did NOT send home.
    ASSERT_EQ(fp->requests.size(), static_cast<size_t>(1));
    return true;
}

// ---------------------------------------------------------------------------
// ③ Mapping: bounds come from caps, and the hand's logical frame is related to
//    the arm's frame (hand RIGHT -> arm -Y, hand UP -> arm +Z).
// ---------------------------------------------------------------------------
TEST(robot_norm_to_mm_uses_caps_bounds_and_axis_sense) {
    Fixture f(test_cfg());
    ASSERT(f.link.connect_if_needed());

    // Centre of the box: every axis lands on the midpoint.
    float mid[3];
    f.link.norm_to_mm(Vec3{0.5f, 0.5f, 0.5f}, mid);
    ASSERT(mid[0] > 108.0f && mid[0] < 108.2f);   // (40.458 + 175.883)/2 = 108.17
    ASSERT(mid[1] > -0.01f && mid[1] < 0.01f);    // symmetric
    ASSERT(mid[2] > 81.8f && mid[2] < 81.9f);     // (49.085 + 114.680)/2 = 81.88

    // Arm X (front) follows the hand's forward axis: hand right (x=1) must NOT
    // push the arm forward.
    float a[3], b[3];
    f.link.norm_to_mm(Vec3{1.0f, 0.5f, 0.5f}, a);   // hand far right
    f.link.norm_to_mm(Vec3{0.0f, 0.5f, 0.5f}, b);   // hand far left
    ASSERT(a[1] < b[1]);      // right of frame -> arm y decreases (arm left is +y)

    // Raising the hand raises the arm.
    float lo[3], hi[3];
    f.link.norm_to_mm(Vec3{0.5f, 0.5f, 0.0f}, lo);
    f.link.norm_to_mm(Vec3{0.5f, 0.5f, 1.0f}, hi);
    ASSERT(hi[2] > lo[2]);
    return true;
}

// ---------------------------------------------------------------------------
// ④ Zero/tiny displacement must be SKIPPED, not sent and not counted as an
//    error (gesture-control.md pitfall 1).
// ---------------------------------------------------------------------------
TEST(robot_zero_delta_is_skipped_not_failed) {
    Fixture f(test_cfg());
    ASSERT(f.link.connect_if_needed());
    const int64_t t = 1000;

    // Establish presence first: the first frame of a session is consumed by the
    // presence rule and never reaches the delta logic.
    HandState warm = make_hand();
    warm.delta = Vec3{0.0f, 0.0f, 0.0f};
    f.link.update(warm, t - 100);
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 0);

    HandState hs = make_hand();
    hs.delta = Vec3{0.0f, 0.0f, 0.0f};
    const bool alive = f.link.update(hs, t);
    ASSERT(alive);         // the link is fine - "nothing to do" is not an error
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 0);   // nothing was written
    // Two sub-deadzone frames => two skips, and crucially no rejection: an
    // all-zero vector must never reach the server as a parameter error.
    ASSERT_EQ(f.link.commands_skipped(), static_cast<int64_t>(2));
    ASSERT_EQ(f.link.commands_rejected(), static_cast<int64_t>(0));
    return true;
}

// ---------------------------------------------------------------------------
// ⑤ A three-axis delta becomes ONE movexyz with the axes mapped as documented.
// ---------------------------------------------------------------------------
TEST(robot_delta_maps_to_single_movexyz) {
    Fixture f(test_cfg());
    ASSERT(f.link.connect_if_needed());
    const int64_t t = 1000;

    // Establish presence on a neutral frame (the first frame of a session is
    // consumed by the presence rule).
    HandState warm = make_hand();
    warm.delta = Vec3{0.0f, 0.0f, 0.0f};
    f.link.update(warm, t - 100);

    HandState hs = make_hand();
    // A hand sweep big enough to mean something: 0.1 of the frame per axis.
    hs.delta = Vec3{0.10f, 0.10f, 0.10f};
    ASSERT(f.link.update(hs, t));
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 1);

    // Parse the three components out of the command and check the SIGNS, which
    // are what the mapping decides. Magnitudes belong to the caps-derived gain.
    //
    //   hand right (+x)   -> arm -y   (arm +y is left)
    //   hand forward (+y) -> arm +x   (arm +x is front)
    //   hand up (+z)      -> arm +z
    const std::string& cmd = f.fp->requests.back();
    const size_t lb = cmd.find('[');
    ASSERT(lb != std::string::npos);
    float d[3] = {0, 0, 0};
    {
        const char* p = cmd.c_str() + lb + 1;
        for (int i = 0; i < 3; ++i) {
            char* end = nullptr;
            d[i] = static_cast<float>(std::strtod(p, &end));
            p = end;
            if (*p == ',') ++p;
        }
    }
    ASSERT(d[0] > 0.0f);    // arm x from hand forward
    ASSERT(d[1] < 0.0f);    // arm y from hand right, mirrored
    ASSERT(d[2] > 0.0f);    // arm z from hand up
    // All three are non-trivial, i.e. no axis was silently dropped.
    ASSERT(std::fabs(d[0]) > 1.0f);
    ASSERT(std::fabs(d[1]) > 1.0f);
    ASSERT(std::fabs(d[2]) > 1.0f);
    return true;
}

// ---------------------------------------------------------------------------
// ⑥ Rate limit: the server does not throttle, so we must (pitfall 3).
// ---------------------------------------------------------------------------
TEST(robot_rate_limit_throttles_move_commands) {
    Fixture f(test_cfg());          // 15 Hz -> 66 ms period
    ASSERT(f.link.connect_if_needed());

    // Establish presence with a zero delta, so the warm frame issues no command
    // and the timed run below starts from a known, empty state.
    HandState warm = make_hand();
    warm.delta = Vec3{0.0f, 0.0f, 0.0f};
    f.link.update(warm, 900);
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 0);

    HandState hs = make_hand();
    hs.delta = Vec3{0.10f, 0.0f, 0.0f};   // well above the deadzone

    // Five frames 10 ms apart inside one 66 ms period: exactly one may go out.
    for (int i = 0; i < 5; ++i) {
        f.link.update(hs, 1000 + i * 10);
    }
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 1);

    // Once the period has elapsed, the next one is allowed through.
    f.link.update(hs, 1000 + 100);
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 2);

    // And a burst right after is throttled again.
    for (int i = 1; i < 5; ++i) {
        f.link.update(hs, 1000 + 100 + i * 5);
    }
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 2);
    return true;
}

// ---------------------------------------------------------------------------
// ⑦ An unreachable absolute target must NOT slam: the basis is preserved and
//    the command reports failure (pitfall 4).
// ---------------------------------------------------------------------------
TEST(robot_unreachable_moveto_keeps_last_good) {
    RobotConfig cfg = test_cfg();
    cfg.map_mode = "absolute";
    Fixture f(cfg);
    ASSERT(f.link.connect_if_needed());

    // Put the link into a known state with one good absolute target. `home` has
    // already latched a TCP basis, so the first frame only establishes presence
    // and the second frame is the one that actually reaches the arm.
    HandState hs = make_hand();
    hs.position = Vec3{0.5f, 0.5f, 0.5f};
    ASSERT(f.link.update(hs, 1000));   // first frame: presence, no command
    ASSERT(f.link.update(hs, 1200));   // second frame: a real, accepted moveto
    ASSERT_EQ(f.link.commands_sent(), static_cast<int64_t>(1));
    ASSERT_EQ(f.link.commands_rejected(), static_cast<int64_t>(0));
    // The handshake (caps + home) must not be counted as hand-following traffic.
    ASSERT_EQ(count_cmds(f.fp->requests, "moveto"), 1);

    // A repeated target must be a no-op, not a second round trip. This is
    // checked against the last COMMANDED target: the arm has no position
    // feedback, so the reply's `tcp` is the commanded value and comparing
    // against it would never match (pitfall 5).
    ASSERT(f.link.update(hs, 1300));
    ASSERT_EQ(f.link.commands_sent(), static_cast<int64_t>(1));
    ASSERT_EQ(f.link.commands_rejected(), static_cast<int64_t>(0));

    // Now drive the hand to the frame edge. The hand's x maps (mirrored) onto the
    // arm's y, so hand x = 1.0 puts the arm on the box's -y wall: inside the
    // bounding box, yet outside the reachable shell (the box is a coarse clamp
    // only). The fake rejects exactly that, reproducing JOINT_LIMIT.
    const int64_t sent_before = f.link.commands_sent();
    HandState edge = make_hand();
    edge.position = Vec3{1.0f, 0.5f, 0.5f};
    // The rejection is a NORMAL protocol result: the command went out, the server
    // said no, and the link is untouched. update() must therefore still report a
    // usable transport - returning false here would make the caller reconnect on
    // every unreachable target.
    ASSERT(f.link.update(edge, 1400));

    // The rejection must be absorbed: nothing counted as sent, one rejection,
    // and no exception / no lost connection.
    ASSERT_EQ(f.link.commands_sent(), sent_before);
    ASSERT_EQ(f.link.commands_rejected(), static_cast<int64_t>(1));
    ASSERT(f.link.connected());

    // The basis must be intact - a fresh, reachable target still goes out. (Not
    // the original centre: that one is now the last COMMANDED target, so the
    // deadzone guard would correctly skip it and the assertion would not prove
    // anything about recovery.)
    HandState back = make_hand();
    back.position = Vec3{0.25f, 0.5f, 0.5f};
    ASSERT(f.link.update(back, 1600));
    ASSERT_EQ(f.link.commands_sent(), sent_before + 1);
    ASSERT_EQ(f.link.commands_rejected(), static_cast<int64_t>(1));   // still just the one
    return true;
}

// ---------------------------------------------------------------------------
// ⑧ A latched reference is REQUIRED before any continuous move. Before that the
//    delta is meaningless and must not reach the arm (hand_types.h, spec 18).
// ---------------------------------------------------------------------------
TEST(robot_no_move_before_reference_latched) {
    Fixture f(test_cfg());
    ASSERT(f.link.connect_if_needed());

    HandState hs = make_hand_unlatched();
    hs.delta = Vec3{0.5f, 0.5f, 0.5f};    // deliberately huge
    f.link.update(hs, 1000);
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 0);

    // Once latched, the same delta does go out.
    hs.reference_initialized = true;
    f.link.update(hs, 1100);
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 1);
    return true;
}

// ---------------------------------------------------------------------------
// ⑨ Open palm -> HOME, and it must fire ONCE per gesture, not once per frame.
// ---------------------------------------------------------------------------
TEST(robot_open_palm_homes_once_per_gesture) {
    Fixture f(test_cfg());
    ASSERT(f.link.connect_if_needed());
    const int homes_after_open = count_cmds(f.fp->requests, "\"home\"");

    HandState hs = make_hand();
    // First frame of the session, no gesture: establishes presence without
    // homing again (open() already did that).
    hs.gesture = Gesture::Unknown;
    f.link.update(hs, 1000);
    ASSERT_EQ(count_cmds(f.fp->requests, "\"home\""), homes_after_open);

    // Now the open palm arrives: one home on the rising edge...
    hs.gesture = Gesture::OpenPalm;
    f.link.update(hs, 1100);
    ASSERT_EQ(count_cmds(f.fp->requests, "\"home\""), homes_after_open + 1);

    // ...and nine more frames of the SAME held gesture add nothing.
    for (int i = 0; i < 9; ++i) f.link.update(hs, 1200 + i * 100);
    ASSERT_EQ(count_cmds(f.fp->requests, "\"home\""), homes_after_open + 1);
    return true;
}

// ---------------------------------------------------------------------------
// ⑩ Fist -> gripper close, and a later open palm -> gripper open (only when the
//    open palm is not itself bound to home).
// ---------------------------------------------------------------------------
TEST(robot_fist_controls_gripper) {
    RobotConfig cfg = test_cfg();
    cfg.gesture_home = "off";        // so open palm is free for the gripper
    Fixture f(cfg);
    ASSERT(f.link.connect_if_needed());

    HandState hs = make_hand();
    // Establish presence on a neutral frame first.
    hs.gesture = Gesture::Unknown;
    f.link.update(hs, 900);
    ASSERT_EQ(count_cmds(f.fp->requests, "\"close\""), 0);

    hs.gesture = Gesture::Fist;
    f.link.update(hs, 1000);
    ASSERT_EQ(count_cmds(f.fp->requests, "\"close\""), 1);

    // Holding the fist must not repeat the command.
    f.link.update(hs, 1100);
    ASSERT_EQ(count_cmds(f.fp->requests, "\"close\""), 1);

    // Releasing it opens the gripper.
    hs.gesture = Gesture::OpenPalm;
    f.link.update(hs, 1200);
    ASSERT_EQ(count_cmds(f.fp->requests, "\"open\""), 1);

    // And holding the open palm does not repeat that either.
    f.link.update(hs, 1300);
    ASSERT_EQ(count_cmds(f.fp->requests, "\"open\""), 1);
    return true;
}

// ---------------------------------------------------------------------------
// ⑪ The operator's rule: when the hand leaves the frame and comes back, the arm
//    returns HOME and the basis is re-established.
// ---------------------------------------------------------------------------
TEST(robot_hand_leaves_then_returns_homes) {
    Fixture f(test_cfg());
    ASSERT(f.link.connect_if_needed());
    const int homes_at_open = count_cmds(f.fp->requests, "\"home\"");

    // A hand is present and moving.
    HandState hs = make_hand();
    hs.delta = Vec3{0.02f, 0.0f, 0.0f};
    f.link.update(hs, 1000);
    ASSERT(f.link.hand_present());
    // Establishing the very first hand must not itself count as a "return".
    ASSERT_EQ(count_cmds(f.fp->requests, "\"home\""), homes_at_open);

    // It leaves: `lost_frames` consecutive misses.
    for (int i = 0; i < 3; ++i) f.link.update(make_no_hand(), 1100 + i * 100);
    ASSERT(!f.link.hand_present());
    ASSERT(f.link.hand_was_away());

    // It comes back -> exactly one home, regardless of the gesture it returns in.
    HandState back = make_hand();
    back.gesture = Gesture::Unknown;
    f.link.update(back, 1500);
    ASSERT_EQ(count_cmds(f.fp->requests, "\"home\""), homes_at_open + 1);
    ASSERT(f.link.hand_present());
    ASSERT(!f.link.hand_was_away());
    return true;
}

// ---------------------------------------------------------------------------
// ⑫ With home_on_presence disabled the arm must stay put when the hand returns.
// ---------------------------------------------------------------------------
TEST(robot_home_on_return_can_be_disabled) {
    RobotConfig cfg = test_cfg();
    cfg.home_on_presence = false;
    Fixture f(cfg);
    ASSERT(f.link.connect_if_needed());
    const int homes_at_open = count_cmds(f.fp->requests, "\"home\"");

    HandState hs = make_hand();
    hs.delta = Vec3{0.02f, 0.0f, 0.0f};
    f.link.update(hs, 1000);   // establish presence
    for (int i = 0; i < 3; ++i) f.link.update(make_no_hand(), 1100 + i * 100);

    HandState back = make_hand();
    back.delta = Vec3{0.02f, 0.0f, 0.0f};
    f.link.update(back, 1500);
    ASSERT_EQ(count_cmds(f.fp->requests, "\"home\""), homes_at_open);
    return true;
}

// ---------------------------------------------------------------------------
// ⑬ A dead backend must be invisible to everything else: no crash, no throw,
//    and the link reports itself as disconnected.
// ---------------------------------------------------------------------------
TEST(robot_survives_unreachable_backend) {
    RobotConfig cfg = test_cfg();
    auto fake = std::make_unique<FakeClient>();
    FakeClient* fp = fake.get();
    fp->fail_connect_ = true;
    RobotLink link;
    link.set_client(std::move(fake));
    link.init(cfg);

    ASSERT(!link.connect_if_needed());
    ASSERT(!link.connected());

    // Feeding frames while disconnected must be a no-op, not a crash.
    HandState hs = make_hand();
    hs.delta = Vec3{0.2f, 0.2f, 0.2f};
    const bool alive = link.update(hs, 1000);
    ASSERT(!alive);           // nothing was achieved: the link is not connected
    ASSERT_EQ(fp->requests.size(), static_cast<size_t>(0));
    return true;
}

// ---------------------------------------------------------------------------
// ⑲ An unreachable backend must announce itself ONCE, and must reconnect on its
//    own once the backend appears - not stay dead until the agent restarts.
//
//    This pins the usability fix: the startup line alone ("control branch
//    started") used to be the only thing an operator saw, so a backend that was
//    not listening looked exactly like one that was.
// ---------------------------------------------------------------------------
TEST(robot_unreachable_backend_retries_and_recovers) {
    RobotConfig cfg = test_cfg();
    cfg.reconnect_ms = 200;      // keep the test fast; the backoff is clamped to >=200
    auto fake = std::make_unique<FakeClient>();
    FakeClient* fp = fake.get();
    fp->fail_connect_ = true;

    RobotLink link;
    link.set_client(std::move(fake));
    link.init(cfg);

    // First attempt fails, and the failure is not a crash or a throw.
    ASSERT(!link.connect_if_needed());
    ASSERT(!link.connected());
    ASSERT_EQ(link.reconnects(), static_cast<int64_t>(0));

    // A retry inside the backoff window must be suppressed, not spun on: the
    // whole point of the delay is that a down backend does not cost a connect()
    // syscall per loop iteration.
    ASSERT(!link.connect_if_needed());

    // Once the backend appears, the SAME link must connect without being
    // recreated - this is the recovery path an operator relies on when they
    // start armpilot-backend after the camera.
    fp->fail_connect_ = false;
    // The backoff is 200 ms; the test cannot sleep on real time without making
    // ctest flaky, so wait for the window with a bounded spin instead.
    bool connected = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (link.connect_if_needed()) { connected = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT(connected);
    ASSERT(link.connected());
    ASSERT(link.caps().valid);                       // the handshake ran
    ASSERT_EQ(link.reconnects(), static_cast<int64_t>(1));

    // And it now behaves like any healthy link: a real hand frame produces a
    // command. (Presence is established by the first frame.)
    HandState warm = make_hand();
    warm.delta = Vec3{0.0f, 0.0f, 0.0f};
    link.update(warm, 1000);
    HandState hs = make_hand();
    hs.delta = Vec3{0.10f, 0.0f, 0.0f};
    link.update(hs, 1100);
    ASSERT(count_cmds(fp->requests, "movexyz") >= 1);
    return true;
}

// ---------------------------------------------------------------------------
// ⑭ A transport failure mid-session must be absorbed and retried, not thrown.
// ---------------------------------------------------------------------------
TEST(robot_recovers_after_transport_drop) {
    Fixture f(test_cfg());
    ASSERT(f.link.connect_if_needed());

    HandState hs = make_hand();
    hs.delta = Vec3{0.10f, 0.0f, 0.0f};       // above the deadzone

    // Establish presence first: that first frame issues no command, so arming
    // the failure before it would leave the fault waiting for the frame after.
    ASSERT(f.link.update(hs, 900));
    ASSERT(f.link.connected());

    // The next exchange dies at the transport level. This is the ONE case where
    // update() reports false: the socket is gone, so the caller may reconnect.
    // The frame is recorded as an attempt first, then the socket drops.
    f.fp->drop_next_ = true;
    ASSERT(!f.link.update(hs, 1000));
    ASSERT(!f.link.connected());
    const size_t attempts_after_drop = f.fp->requests.size();
    ASSERT(attempts_after_drop > 0);

    // Reconnecting is deliberately rate limited by reconnect_ms (2000 by
    // default) so a down backend is not hammered. connect_if_needed() uses the
    // real clock, so the test waits out the backoff rather than faking it.
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    ASSERT(f.link.connect_if_needed());
    ASSERT(f.link.connected());
    ASSERT(f.link.caps().valid);

    // ...and the rate limiter must not have been poisoned by the failure.
    ASSERT(f.link.update(hs, 5000));
    return true;
}

// ---------------------------------------------------------------------------
// ⑮ Invalid configuration must be refused up front.
// ---------------------------------------------------------------------------
TEST(robot_rejects_invalid_config) {
    {
        RobotConfig c = test_cfg();
        c.map_mode = "sideways";
        RobotLink l;
        ASSERT(!l.init(c));
    }
    {
        RobotConfig c = test_cfg();
        c.port = 0;
        RobotLink l;
        ASSERT(!l.init(c));
    }
    {
        RobotConfig c = test_cfg();
        c.rate_hz = 0;
        RobotLink l;
        ASSERT(!l.init(c));
    }
    return true;
}

// ---------------------------------------------------------------------------
// ⑯ Soft boundary: the gain tapers as the arm approaches a bound, and never
//    reaches zero (a zero weight would trap the arm at the wall).
//
//    Asserted on the curve itself rather than through a command, so the shape
//    (monotone, smooth, floored) is pinned independently of the plumbing.
// ---------------------------------------------------------------------------
TEST(robot_soft_boundary_tapers_gain_monotonically) {
    RobotConfig cfg = test_cfg();
    cfg.soft_boundary = true;
    cfg.soft_margin = 0.20f;
    cfg.soft_min_gain = 0.10f;
    Fixture f(cfg);
    ASSERT(f.link.connect_if_needed());          // latches caps

    const RobotCaps& c = f.link.caps();
    ASSERT(c.valid);
    // x span from the captured caps: 40.458 .. 175.883 (135.4 mm).
    const float lo = c.ws_min[0];
    const float hi = c.ws_max[0];

    // Well inside: no taper at all. Half-way is far more than one margin in.
    const float mid[3] = {(lo + hi) * 0.5f, 0.0f, 0.0f};
    ASSERT(f.link.boundary_weight(mid, 0) > 0.999f);

    // Walk from the centre to the upper bound and require a monotone, bounded
    // taper that ends AT the floor - not below it, because `soft_min_gain` is
    // the escape route that lets the arm travel back out.
    float prev = 1.01f;
    for (int i = 0; i <= 10; ++i) {
        const float p = mid[0] + (hi - mid[0]) * (static_cast<float>(i) / 10.0f);
        const float pos[3] = {p, 0.0f, 0.0f};
        const float w = f.link.boundary_weight(pos, 0);
        ASSERT(w <= prev + 1e-4f);               // never increases
        ASSERT(w >= cfg.soft_min_gain - 1e-4f);  // never below the floor
        ASSERT(w <= 1.0f + 1e-4f);
        prev = w;
    }
    // At the bound itself the weight is exactly the floor.
    const float on_bound[3] = {hi, 0.0f, 0.0f};
    ASSERT(std::fabs(f.link.boundary_weight(on_bound, 0) - cfg.soft_min_gain) < 1e-3f);

    // Symmetry: the LOW bound is tapered identically (the arm has two walls).
    const float low_bound[3] = {lo, 0.0f, 0.0f};
    ASSERT(std::fabs(f.link.boundary_weight(low_bound, 0) - cfg.soft_min_gain) < 1e-3f);

    // And a point outside the box reads as "on the bound", not as something
    // below the floor: `caps` is sampled, so a stale position can land there.
    const float outside[3] = {hi + 50.0f, 0.0f, 0.0f};
    ASSERT(f.link.boundary_weight(outside, 0) >= cfg.soft_min_gain - 1e-4f);

    // The taper is per-axis: sitting on x's wall says nothing about z.
    const float on_x_wall[3] = {hi, 0.0f, (c.ws_min[2] + c.ws_max[2]) * 0.5f};
    ASSERT(f.link.boundary_weight(on_x_wall, 2) > 0.999f);
    return true;
}

// ---------------------------------------------------------------------------
// ⑰ Soft boundary ON: a hand pushing past the wall gets TAPERED steps, and once
//    the effective bound is learned the outward step is SUPPRESSED outright -
//    so the arm stops grinding out one JOINT_LIMIT rejection per frame.
// ---------------------------------------------------------------------------
TEST(robot_soft_boundary_quiets_the_wall) {
    RobotConfig cfg = test_cfg();
    cfg.soft_boundary = true;
    cfg.soft_margin = 0.20f;
    Fixture f(cfg);
    ASSERT(f.link.connect_if_needed());

    const RobotCaps& c = f.link.caps();
    // Park the arm right on the +x bound (the cap's max). From here every step
    // toward +x is refused by the real server, and with the taper it must be
    // scaled down - then stopped - rather than sent at full size.
    f.fp->report_movexyz_tcp_ = true;
    f.fp->movexyz_tcp_[0] = c.ws_max[0];
    f.fp->movexyz_tcp_[1] = 0.0f;
    f.fp->movexyz_tcp_[2] = (c.ws_min[2] + c.ws_max[2]) * 0.5f;

    // Establish presence first (the first frame of a session is consumed by the
    // presence rule and emits nothing).
    HandState warm = make_hand();
    warm.delta = Vec3{0.0f, 0.0f, 0.0f};
    f.link.update(warm, 900);

    // A fixed-size hand push (+y in hand space -> +x on the arm). The first one
    // latches the reported position; from then on the arm is pinned to the wall.
    HandState push = make_hand();
    push.delta = Vec3{0.0f, 0.10f, 0.0f};
    f.link.update(push, 1000);

    const int64_t rejected_before = f.link.commands_rejected();
    const size_t  reqs_before     = f.fp->requests.size();

    for (int i = 1; i <= 10; ++i) {
        f.link.update(push, 1000 + i * 100);
    }

    // Nothing may have been REFUSED by the server: the taper's whole job is to
    // stop issuing commands the arm cannot take.
    ASSERT_EQ(f.link.commands_rejected(), rejected_before);

    // The outward step must be tapered or suppressed, never sent at full size.
    // (delta.y * gain_x = 0.10 * 270.85 = 27.09 mm un-tapered.) Whichever of the
    // two mechanisms fired, the invariant is the same: no full-size step.
    float max_step = 0.0f;
    for (size_t i = reqs_before; i < f.fp->requests.size(); ++i) {
        const std::string& cmd = f.fp->requests[i];
        if (cmd.find("movexyz") == std::string::npos) continue;
        const size_t lb = cmd.find('[');
        if (lb == std::string::npos) continue;
        max_step = std::max(max_step, static_cast<float>(std::atof(cmd.c_str() + lb + 1)));
    }
    ASSERT(max_step < 27.09f * 0.5f);   // tapered or suppressed - never full size

    // The wall must also be quiet: with the bound learned, the arm stops emitting
    // outward steps entirely, which is what removes the rejection tail.
    ASSERT(f.link.commands_skipped() > 0);

    // Sanity: the taper really is at (or very near) the floor for this position.
    const float pos[3] = {c.ws_max[0], 0.0f, 0.0f};
    ASSERT(f.link.boundary_weight(pos, 0, +1.0f) <= cfg.soft_min_gain + 1e-3f);
    return true;
}

// ---------------------------------------------------------------------------
// ⑱ Soft boundary OFF must reproduce the old behaviour exactly - the same
//    command as before the feature existed. This is the regression guard: it is
//    what makes "just turn it off" a real escape hatch.
// ---------------------------------------------------------------------------
TEST(robot_soft_boundary_off_is_the_old_behaviour) {
    RobotConfig cfg = test_cfg();
    cfg.soft_boundary = false;
    Fixture f(cfg);
    ASSERT(f.link.connect_if_needed());

    // Park the arm ON the +x wall - the position that tapers to the floor when
    // the feature is on.
    f.fp->report_movexyz_tcp_ = true;
    f.fp->movexyz_tcp_[0] = f.link.caps().ws_max[0];
    f.fp->movexyz_tcp_[1] = 0.0f;
    f.fp->movexyz_tcp_[2] = 100.0f;

    HandState warm = make_hand();
    warm.delta = Vec3{0.0f, 0.0f, 0.0f};
    f.link.update(warm, 900);

    HandState push = make_hand();
    push.delta = Vec3{0.0f, 0.10f, 0.0f};
    f.link.update(push, 1000);      // latches the reported position

    // The taper must be a no-op: the weight is exactly 1 on every axis, so the
    // emitted step is the full un-tapered one and the fake accepts it.
    const float pos[3] = {f.link.caps().ws_max[0], 0.0f, 100.0f};
    ASSERT(std::fabs(f.link.boundary_weight(pos, 0) - 1.0f) < 1e-6f);

    const int64_t sent_before   = f.link.commands_sent();
    const int64_t skipped_before = f.link.commands_skipped();
    const size_t  reqs_before    = f.fp->requests.size();

    for (int i = 1; i <= 5; ++i) {
        f.link.update(push, 1000 + i * 100);
    }

    // A full-size step every period: all five go out, none swallowed.
    ASSERT_EQ(f.link.commands_sent() - sent_before, static_cast<int64_t>(5));
    ASSERT_EQ(f.link.commands_skipped(), skipped_before);

    // The un-tapered magnitude, i.e. delta.y * gain_x = 0.10 * 270.85 = 27.09.
    float seen = 0.0f;
    for (size_t i = reqs_before; i < f.fp->requests.size(); ++i) {
        const std::string& cmd = f.fp->requests[i];
        if (cmd.find("movexyz") == std::string::npos) continue;
        const size_t lb = cmd.find('[');
        if (lb == std::string::npos) continue;
        seen = std::max(seen, static_cast<float>(std::atof(cmd.c_str() + lb + 1)));
    }
    ASSERT(seen > 26.0f);        // full size, nothing tapered away
    return true;
}

// ---------------------------------------------------------------------------
// ⑲ The measured reach span is the gain basis, and it is what makes the delta
//    route usable at all on a box-inflated backend.
//
//    Regression guard for the reported failure: "the hand moves and the arm
//    does not". The caps box on this backend is 4x the reachable X span while
//    HOME sits 5 mm from the -X edge, so a box-derived gain emitted a step
//    several times the remaining travel, the first step was refused, and the
//    learner latched the axis shut.
// ---------------------------------------------------------------------------
TEST(robot_soft_boundary_uses_measured_span) {
    // Same fake arm, but with the measured span configured the way production
    // does (a 34 mm X axis, not the 135.4 mm box).
    RobotConfig cfg = test_cfg();
    cfg.reach_span_x = 34.0f;
    cfg.reach_span_y = 176.0f;
    cfg.reach_span_z = 42.0f;
    Fixture f(cfg);
    ASSERT(f.link.connect_if_needed());
    ASSERT(f.link.caps().valid);

    // Home is at X 115.033 on the fake arm. A 34 mm band centred there is
    // [98.03, 132.03] with a default soft_margin of 0.15 -> a 5.1 mm slow zone
    // at each end, so the taper only bites within ~5 mm of a bound. A point
    // 1.97 mm inside the low bound is therefore well into the taper, while the
    // same point against the 135.4 mm box is 69 mm clear and does not taper.
    const float near_low[3] = {100.0f, 0.0f, 0.0f};
    const float w_near = f.link.boundary_weight(near_low, 0);
    ASSERT(w_near < 0.90f);          // the taper is engaged...

    // ...and the same point against the BOX does not taper at all: 100 mm is
    // 60 mm from the box's low bound, far outside a 20.3 mm slow zone. This is
    // the contrast that pins "measured, not box".
    RobotConfig box_cfg = cfg;
    box_cfg.reach_span_x = 0.0f;     // fall back to the caps box
    Fixture fb(box_cfg);
    ASSERT(fb.link.connect_if_needed());
    ASSERT(fb.link.boundary_weight(near_low, 0) > 0.999f);

    // ...and the gain itself is the measured span, not the box: one normalised
    // unit of hand travel commands 34/0.5 = 68 mm, not 135.4/0.5 = 270.9 mm.
    // Drive one frame and read the emitted step.
    f.fp->report_movexyz_tcp_ = true;
    f.fp->movexyz_tcp_[0] = 115.033f;
    f.fp->movexyz_tcp_[1] = 0.0f;
    f.fp->movexyz_tcp_[2] = 109.224f;

    HandState warm = make_hand();
    warm.delta = Vec3{0.0f, 0.0f, 0.0f};
    f.link.update(warm, 900);
    HandState push = make_hand();
    push.delta = Vec3{0.0f, 0.10f, 0.0f};        // 0.10 of the hand's active span
    f.link.update(push, 1000);

    float seen = 0.0f;
    for (const std::string& cmd : f.fp->requests) {
        if (cmd.find("movexyz") == std::string::npos) continue;
        const size_t lb = cmd.find('[');
        if (lb == std::string::npos) continue;
        seen = std::max(seen, static_cast<float>(std::atof(cmd.c_str() + lb + 1)));
    }
    // 34/0.5 * 0.10 = 6.8 mm (plus the taper, so <= that). Against the box it
    // would have been 27.1 mm. Anything under 12 pins "measured, not box".
    ASSERT(seen > 0.0f);
    ASSERT(seen < 12.0f);
    return true;
}

// ---------------------------------------------------------------------------
// ⑳ A refusal is "too big", not "never": one halved retry recovers the step.
//
//    The wall here is a magnitude threshold, which is how the real backend
//    behaves near an edge - a short enough step in the same direction succeeds.
//    Without the probe the refusal would reach the learner undistilled and the
//    bound would be pulled all the way in to the arm's current position.
// ---------------------------------------------------------------------------
TEST(robot_soft_probe_recovers_a_halved_step) {
    RobotConfig cfg = test_cfg();
    cfg.soft_boundary = true;
    cfg.soft_probe_on_reject = true;
    cfg.soft_probe_tries = 2;
    Fixture f(cfg);
    ASSERT(f.link.connect_if_needed());

    // The frame's step here is the box-derived 0.10 * 270.85 = 27.09 mm because
    // test_cfg() leaves the measured span unset - so the wall is placed between
    // where the original lands and where ONE halving lands (27.09 / 2 = 13.54):
    // 15 mm. The original is refused, the half-length retry is accepted. This
    // is exactly the shape of the reported bug, where the wall sat a fraction
    // of the way into the commanded step.
    f.fp->report_movexyz_tcp_ = true;
    f.fp->movexyz_tcp_[0] = 115.033f;
    f.fp->movexyz_tcp_[1] = 0.0f;
    f.fp->movexyz_tcp_[2] = 109.224f;
    f.fp->reject_movexyz_above_mm_ = 15.0f;

    HandState warm = make_hand();
    warm.delta = Vec3{0.0f, 0.0f, 0.0f};
    f.link.update(warm, 900);

    const int64_t sent_before = f.link.commands_sent();
    HandState push = make_hand();
    push.delta = Vec3{0.0f, 0.10f, 0.0f};
    f.link.update(push, 1000);       // latches position (reports HOME)

    // THE POINT OF THE TEST: that frame's full-size step (27.09 mm) tripped the
    // 15 mm wall, and the half-length retry (13.54 mm) got it through. Without
    // the probe this frame would have ended as a bare refusal and the arm would
    // not have moved at all.
    const int64_t sent_mid = f.link.commands_sent();
    ASSERT(sent_mid > sent_before);

    for (int i = 1; i <= 3; ++i) f.link.update(push, 1000 + i * 100);
    const int64_t sent_after = f.link.commands_sent();

    // The following frames go quiet instead of repeating the exchange: the
    // refusal taught the learner that the bound is at the arm's current
    // position, so the outward step is suppressed outright (that suppression is
    // what robot_soft_boundary_quiets_the_wall pins down). Requiring "keeps
    // sending" here would demand the feature NOT work.
    ASSERT(sent_after == sent_mid);

    // And the accepted traffic is SMALLER than the refused first attempt: the
    // probe halves. Anything at or below the 15 mm wall that actually got
    // through is the halved step; the full 27.09 mm was refused.
    float smallest_accepted = 1e9f;
    for (const std::string& cmd : f.fp->requests) {
        if (cmd.find("movexyz") == std::string::npos) continue;
        const size_t lb = cmd.find('[');
        if (lb == std::string::npos) continue;
        const float m = static_cast<float>(std::atof(cmd.c_str() + lb + 1));
        if (m > 0.0f && m <= 15.0f) smallest_accepted = std::min(smallest_accepted, m);
    }
    ASSERT(smallest_accepted < 1e8f);   // a sub-threshold step really was sent
    return true;
}

// ---------------------------------------------------------------------------
// ㉑ With the probe OFF a refusal goes straight to the learner, i.e. the old
//    behaviour is still reachable - the switch is a real switch.
// ---------------------------------------------------------------------------
TEST(robot_soft_probe_off_sends_only_the_original_step) {
    RobotConfig cfg = test_cfg();
    cfg.soft_boundary = true;
    cfg.soft_probe_on_reject = false;
    Fixture f(cfg);
    ASSERT(f.link.connect_if_needed());

    f.fp->report_movexyz_tcp_ = true;
    f.fp->movexyz_tcp_[0] = 115.033f;
    f.fp->movexyz_tcp_[1] = 0.0f;
    f.fp->movexyz_tcp_[2] = 109.224f;
    f.fp->reject_movexyz_above_mm_ = 15.0f;

    HandState warm = make_hand();
    warm.delta = Vec3{0.0f, 0.0f, 0.0f};
    f.link.update(warm, 900);

    HandState push = make_hand();
    push.delta = Vec3{0.0f, 0.10f, 0.0f};
    f.link.update(push, 1000);

    const int64_t rej_before = f.link.commands_rejected();
    for (int i = 1; i <= 3; ++i) f.link.update(push, 1000 + i * 100);

    // No halved retry was issued: the first refusal at a wall teaches the
    // learner, and the axis then tapers/suppresses. Only the ORIGINAL size
    // appears in the traffic, never a sub-threshold step.
    bool saw_sub_threshold = false;
    for (const std::string& cmd : f.fp->requests) {
        if (cmd.find("movexyz") == std::string::npos) continue;
        const size_t lb = cmd.find('[');
        if (lb == std::string::npos) continue;
        const float m = static_cast<float>(std::atof(cmd.c_str() + lb + 1));
        if (m > 0.0f && m <= 15.0f) saw_sub_threshold = true;
    }
    ASSERT(!saw_sub_threshold);
    ASSERT(f.link.commands_rejected() >= rej_before);
    return true;
}

// ---------------------------------------------------------------------------
// ㉑ The SHIPPED feel gains. This is deliberately a pin, not a behaviour test:
//    it exists so that retuning the arm's responsiveness is a conscious edit
//    with a failing test attached, rather than a number someone nudges and
//    forgets. The operator asked for "hand moves big, arm moves small" after
//    finding the near-1:1 response too jumpy (2026-09-29).
//
//    If this fails because you MEANT to retune: update the yaml, config.h and
//    these numbers together, and re-check the jitter arithmetic in the comment
//    on RobotConfig::gain_x. If it failed and you did not mean to change it,
//    something has quietly reverted the feel.
//
//    What the numbers mean, per axis, at the analyzer's own motion_deadzone
//    (0.02 normalised - the smallest hand movement that yields a delta):
//
//      axis  span(mm)  gain   mm per 0.02    vs the 0.5 mm minimum step
//      X       39.0    0.25       0.39        filtered (tremor rejected)
//      Y      176.0    0.50       1.76        passes
//      Z       41.5    0.25       0.42        filtered (tremor rejected)
// ---------------------------------------------------------------------------
TEST(robot_shipped_gains_are_the_documented_feel) {
    const RobotConfig def;

    ASSERT(std::fabs(def.gain_x - 0.25f) < 1e-6f);
    ASSERT(std::fabs(def.gain_y - 0.50f) < 1e-6f);
    ASSERT(std::fabs(def.gain_z - 0.25f) < 1e-6f);

    // The spans those gains are read against. Kept here so the mm-per-unit
    // arithmetic above is checkable rather than folklore.
    ASSERT(std::fabs(def.reach_span_x - 39.0f) < 1e-6f);
    ASSERT(std::fabs(def.reach_span_y - 176.0f) < 1e-6f);
    ASSERT(std::fabs(def.reach_span_z - 41.5f) < 1e-6f);

    // The two "jitter is filtered" claims, stated as arithmetic against the
    // effective floor (kMinDeltaMm, mirrored here at 0.5). X and Z must land
    // UNDER it and Y must stay over it, or the tuned feel is not what shipped.
    const float jitter = 0.02f;             // HandConfig::motion_deadzone
    const float span_over_active = 1.0f / 0.5f;  // axis_scale divisor
    const float step_x = span_over_active * def.reach_span_x * def.gain_x * jitter;
    const float step_y = span_over_active * def.reach_span_y * def.gain_y * jitter;
    const float step_z = span_over_active * def.reach_span_z * def.gain_z * jitter;

    ASSERT(step_x < 0.5f);
    ASSERT(step_z < 0.5f);
    ASSERT(step_y > 0.5f);

    // And the user-facing statement: a typical deliberate move (0.10) must still
    // produce a visible step on every axis, or the arm feels broken.
    const float move = 0.10f;
    ASSERT(span_over_active * def.reach_span_x * def.gain_x * move > 1.0f);
    ASSERT(span_over_active * def.reach_span_y * def.gain_y * move > 1.0f);
    ASSERT(span_over_active * def.reach_span_z * def.gain_z * move > 1.0f);
    return true;
}
