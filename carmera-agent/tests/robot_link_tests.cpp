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

// Parse the three components of the first delta array in a movexyz request.
// Returns false when the array is absent or malformed, so a caller can walk a
// request log without tripping over the caps/home payloads in it.
bool parse_movexyz_delta(const std::string& req, float out[3]) {
    const size_t lb = req.find('[');
    if (lb == std::string::npos) return false;
    size_t i = lb + 1;
    for (int k = 0; k < 3; ++k) {
        const size_t comma = req.find_first_of(",]", i);
        if (comma == std::string::npos) return false;
        try {
            out[k] = std::stof(req.substr(i, comma - i));
        } catch (...) {
            return false;
        }
        i = comma + 1;
    }
    return true;
}

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
    // A short window so a test can cross it in a few frames. The tests below
    // drive `update()` with explicit millisecond timestamps, so this is exact:
    // 300 ms means "the third 100 ms step without a detection is the departure".
    c.hand_lost_ms = 300;
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

// The `k`-th component of the delta array of a movexyz request; 0 on a parse
// miss. Free function (rather than reusing FakeClient's private helper) so the
// assertions on emitted step SIZE read the same way as the ones on counts.
// Hand-rolled for the same C4996 reason as the FakeClient copy.
float delta_component(const std::string& req, int k) {
    const size_t lb = req.find('[');
    if (lb == std::string::npos) return 0.0f;
    size_t i = lb + 1;
    for (int c = 0; c <= k; ++c) {
        const size_t comma = req.find_first_of(",]", i);
        if (comma == std::string::npos) return 0.0f;
        if (c == k) {
            try {
                return std::stof(req.substr(i, comma - i));
            } catch (...) {
                return 0.0f;
            }
        }
        i = comma + 1;
    }
    return 0.0f;
}

// The delta of the LAST movexyz request in `reqs`.
float last_movexyz_delta(const std::vector<std::string>& reqs, int k) {
    for (auto it = reqs.rbegin(); it != reqs.rend(); ++it) {
        if (it->find("\"movexyz\"") != std::string::npos) {
            return delta_component(*it, k);
        }
    }
    return 0.0f;
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

    // The first continuous frame only LATCHES the differencing basis (it has no
    // predecessor to subtract), so it sends nothing - that is the fix for the
    // "cumulative delta replayed every frame" bug, not a swallowed frame.
    HandState warm = make_hand();
    warm.delta = Vec3{0.0f, 0.0f, 0.0f};
    f.link.update(warm, t - 100);
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 0);

    // Now a real frame with a held (still) hand: delta unchanged, so the
    // increment is zero.
    HandState hs = make_hand();
    hs.delta = Vec3{0.0f, 0.0f, 0.0f};
    const bool alive = f.link.update(hs, t);
    ASSERT(alive);         // the link is fine - "nothing to do" is not an error
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 0);   // nothing was written
    // One sub-deadzone frame => one skip, and crucially no rejection: an
    // all-zero vector must never reach the server as a parameter error.
    ASSERT_EQ(f.link.commands_skipped(), static_cast<int64_t>(1));
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
    // The hand keeps travelling: delta is CUMULATIVE (relative to the latched
    // reference), so every frame below advances it by another 0.10. A constant
    // delta would now mean "the hand is holding still" and, correctly, produce
    // no further commands.
    float d = 0.10f;

    // Five frames 10 ms apart inside one 66 ms period: exactly one may go out.
    for (int i = 0; i < 5; ++i) {
        hs.delta = Vec3{d, 0.0f, 0.0f};
        d += 0.10f;
        f.link.update(hs, 1000 + i * 10);
    }
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 1);

    // Once the period has elapsed, the next one is allowed through.
    hs.delta = Vec3{d, 0.0f, 0.0f};
    d += 0.10f;
    f.link.update(hs, 1000 + 100);
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 2);

    // And a burst right after is throttled again.
    for (int i = 1; i < 5; ++i) {
        hs.delta = Vec3{d, 0.0f, 0.0f};
        d += 0.10f;
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

    // Two unlatched frames with a deliberately huge delta. Nothing may go out:
    // while the reference is unlatched the delta is meaningless, and the
    // differencing basis is not advanced either (the pre-latch window is
    // discarded wholesale).
    HandState hs = make_hand_unlatched();
    hs.delta = Vec3{0.5f, 0.5f, 0.5f};
    f.link.update(hs, 1000);
    hs.delta = Vec3{0.6f, 0.5f, 0.5f};
    f.link.update(hs, 1100);
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 0);

    // The first LATCHED frame now latches the differencing basis (there is no
    // predecessor to subtract), so it must not carry a step yet.
    hs.reference_initialized = true;
    hs.delta = Vec3{0.7f, 0.5f, 0.5f};
    f.link.update(hs, 1200);
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 0);

    // The next latched frame carries the real increment (0.8 - 0.7). Timestamps
    // are spaced past the 15 Hz rate period (66 ms) so the limiter cannot swallow
    // the frame instead of the reference gate being tested.
    hs.delta = Vec3{0.8f, 0.5f, 0.5f};
    f.link.update(hs, 1300);
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

    // It leaves: `hand_lost_ms` of CONTINUOUS absence. The last detection was at
    // t=1000, so t=1100 and t=1200 are only 100/200 ms of silence - below the
    // 300 ms window - and must NOT count yet. Only t=1300 crosses it. That
    // asymmetry is the point of the time-based rule: a short gap is a flicker,
    // not an exit.
    f.link.update(make_no_hand(), 1100);
    ASSERT(f.link.hand_present());
    f.link.update(make_no_hand(), 1200);
    ASSERT(f.link.hand_present());
    f.link.update(make_no_hand(), 1300);
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
// ㉔ The bursty-detector regression: the 2026-09-29 operator report, written
//    down as a test. On a live camera the palm/landmark cascade does NOT return
//    a steady stream - it returns CLUSTERS of 4-10 detected frames separated by
//    gaps of 0.9-19 s (measured). Under the old frame-count rule a gap crossing
//    `lost_frames` = 6 (~0.75 s at the hand branch's ~8 Hz) made every cluster
//    boundary look like a real exit, so the arm was yanked HOME between clusters
//    and barely followed the hand at all.
//
//    The fixture's window is 300 ms, so the 200 ms silence below stands in for
//    the measured sub-second inter-cluster gaps against the shipped 2000 ms.
//    The assertions are on the `home` COUNT and on presence - "the link stayed
//    up" is not the claim being made here.
// ---------------------------------------------------------------------------
TEST(robot_short_detection_gap_does_not_home) {
    Fixture f(test_cfg());
    ASSERT(f.link.connect_if_needed());
    const int homes_at_open = count_cmds(f.fp->requests, "\"home\"");

    // Cluster 1: the hand is found and steered.
    HandState hs = make_hand();
    hs.delta = Vec3{0.02f, 0.0f, 0.0f};
    f.link.update(hs, 1000);
    ASSERT(f.link.hand_present());

    // The gap. 200 ms is INSIDE the 300 ms window, so the hand must still count
    // as present at every one of these frames.
    f.link.update(make_no_hand(), 1100);
    ASSERT(f.link.hand_present());
    f.link.update(make_no_hand(), 1200);
    ASSERT(f.link.hand_present());
    ASSERT(!f.link.hand_was_away());

    // Cluster 2: the hand was never gone, so this is a CONTINUATION - no HOME,
    // and the follow is unbroken.
    HandState again = make_hand();
    again.delta = Vec3{0.04f, 0.0f, 0.0f};
    f.link.update(again, 1250);
    ASSERT(f.link.hand_present());
    ASSERT_EQ(count_cmds(f.fp->requests, "\"home\""), homes_at_open);

    // The converse still holds: a silence that DOES exceed the window is an exit.
    f.link.update(make_no_hand(), 1600);   // 350 ms since the t=1250 detection
    ASSERT(!f.link.hand_present());
    ASSERT(f.link.hand_was_away());
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
    // Same 300 ms continuous-absence window as test ⑪ (t=1300 is the crossing).
    for (int i = 0; i < 3; ++i) f.link.update(make_no_hand(), 1100 + i * 100);
    ASSERT(!f.link.hand_present());

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
    // Cumulative delta, advanced per frame: a held delta means "the hand is
    // still" and correctly produces no command to drop.
    hs.delta = Vec3{0.10f, 0.0f, 0.0f};       // above the deadzone

    // Establish presence first: that first frame issues no command, so arming
    // the failure before it would leave the fault waiting for the frame after.
    ASSERT(f.link.update(hs, 900));
    ASSERT(f.link.connected());

    // The next exchange dies at the transport level. This is the ONE case where
    // update() reports false: the socket is gone, so the caller may reconnect.
    // The frame is recorded as an attempt first, then the socket drops.
    hs.delta = Vec3{0.20f, 0.0f, 0.0f};       // the hand keeps moving
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
    // NOTE this frame re-latches the differencing basis (open() cleared it on
    // reconnect, so a stale prev_delta_ cannot emit the whole session's motion
    // as one step). It therefore reports "alive" without sending.
    ASSERT(f.link.update(hs, 5000));
    ASSERT(f.link.connected());
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
// ⑰b A pinned axis must not freeze the other two.
//
//     The suppression used to be all-or-nothing: the first axis found pressed
//     against its bound ended the whole frame, so once the arm parked on one
//     wall it stopped responding on ALL of them. On this arm that is not a
//     corner case - at HOME X has 9 mm of travel left and Z has 5 mm - so the
//     operator's "the hand moves and the arm does not" showed up almost
//     immediately. The zeroing must therefore be confined to the pinned axis.
//
//     The frame here pushes +x (into the wall) AND +z (mid-range, free) at the
//     same time, with the arm parked on the +x bound: the x component must
//     vanish, the z component must survive, and the frame must still be SENT.
// ---------------------------------------------------------------------------
TEST(robot_soft_boundary_suppresses_only_the_pinned_axis) {
    RobotConfig cfg = test_cfg();
    cfg.soft_boundary = true;
    cfg.soft_margin = 0.20f;
    Fixture f(cfg);
    ASSERT(f.link.connect_if_needed());

    const RobotCaps& c = f.link.caps();
    // The arm is reported at the +x wall, but z at mid-range (plenty of room).
    f.fp->report_movexyz_tcp_ = true;
    f.fp->movexyz_tcp_[0] = c.ws_max[0];
    f.fp->movexyz_tcp_[1] = 0.0f;
    f.fp->movexyz_tcp_[2] = (c.ws_min[2] + c.ws_max[2]) * 0.5f;

    // Presence frame (consumed by the presence rule, emits nothing).
    HandState warm = make_hand();
    warm.delta = Vec3{0.0f, 0.0f, 0.0f};
    f.link.update(warm, 900);

    // Frame 1: +x only, so the reported `tcp` latches as the wall position.
    // (Hand +y -> arm +x; hand +z -> arm +z. Before this frame there is no
    // commanded position at all, so the taper reads HOME and does not bite.)
    HandState p1 = make_hand();
    p1.delta = Vec3{0.0f, 0.10f, 0.0f};
    f.link.update(p1, 1000);

    // Frames 2-4: keep pushing +x INTO the wall while ALSO moving +z. Each frame
    // carries a fresh increment (HandState::delta is cumulative, so repeating a
    // value means "the hand is still", not "keep moving").
    for (int i = 1; i <= 3; ++i) {
        HandState p = make_hand();
        p.delta = Vec3{0.0f, 0.10f * (i + 1), 0.10f * i};
        f.link.update(p, 1000 + i * 100);
    }

    // Nothing may have been refused: the pinned axis is dropped locally instead
    // of being ground against the server.
    ASSERT_EQ(f.link.commands_rejected(), 0);

    // Walk the request log: the x component must have been live BEFORE the wall
    // was reached and must be gone after, while z keeps being sent.
    int    movexyz_count = 0;
    bool   saw_live_x    = false;
    float  last[3]       = {0.0f, 0.0f, 0.0f};
    for (const std::string& cmd : f.fp->requests) {
        if (cmd.find("movexyz") == std::string::npos) continue;
        float d[3];
        if (!parse_movexyz_delta(cmd, d)) continue;
        ++movexyz_count;
        if (std::fabs(d[0]) > 10.0f) saw_live_x = true;
        last[0] = d[0];
        last[1] = d[1];
        last[2] = d[2];
    }

    // The frame was never frozen: the arm kept being commanded after the park.
    ASSERT(movexyz_count >= 3);
    // ...and the x axis really was working before it hit the wall, so the zero
    // below is the boundary talking and not a dead axis.
    ASSERT(saw_live_x);

    // The pinned axis is dropped, the free axis is untouched.
    ASSERT(std::fabs(last[0]) < 0.01f);
    ASSERT(std::fabs(last[2]) > 1.0f);
    return true;
}

// ---------------------------------------------------------------------------
// ⑰c A refusal must not pull the learned bound onto the arm's own pose.
//
//     learn_from_rejection() used to write `min(from + step, from)` - the
//     arm's CURRENT position - on the theory that "the step was refused, so the
//     arm is still at `from` and the extreme is at most `from`". That leaves the
//     arm standing exactly ON its wall, where the taper reads d == 0 and returns
//     the 0.10 floor for every outward step, AND relax_learned_bound()'s
//     `v > eff_max_` test can never hold (the arm cannot get past a bound that is
//     drawn through its own position). So one refusal closes that side of that
//     axis for the rest of the session, silently.
//
//     This is the mechanism behind the 2026-09-29 live report ("moving the hand
//     barely moves the arm"). In that 35-minute run the Y and Z taper weights
//     were pinned at the floor in 300/1370 and 362/1370 of the taper frames,
//     every one of them with the arm parked at HOME - +Y and +Z were shut while
//     the hand was still being tracked. The backend did not even need to blame
//     the right axis: it refuses the WHOLE command when ANY one axis leaves the
//     solvable set, so a +Z limit also tightened Y.
//
//     A refusal of `from + step` proves that TARGET is unreachable. It proves
//     nothing about the interval between `from` and the target. The bound
//     belongs at the target - one headroom step out - so the arm keeps its full
//     range minus one step, and each further refusal re-tightens by exactly the
//     step it was asked for.
// ---------------------------------------------------------------------------
TEST(robot_rejection_leaves_headroom_at_the_current_pose) {
    RobotConfig cfg = test_cfg();
    cfg.soft_boundary = true;
    cfg.soft_probe_on_reject = false;    // the refusal must reach the learner raw
    cfg.reach_span_x = 60.0f;            // a 60 mm X axis: 120 mm of hand travel
    cfg.reach_span_y = 176.0f;
    cfg.reach_span_z = 42.0f;
    Fixture f(cfg);
    ASSERT(f.link.connect_if_needed());

    // The reported `tcp` is pinned, so `from` is a known constant and the only
    // thing that moves across the next two frames is the learned bound.
    f.fp->report_movexyz_tcp_ = true;
    f.fp->movexyz_tcp_[0] = 115.033f;
    f.fp->movexyz_tcp_[1] = 0.0f;
    f.fp->movexyz_tcp_[2] = 109.224f;
    // A magnitude wall, which is the shape the real JOINT_LIMIT has near an edge:
    // 0.30 of hand travel -> 0.30 * 60/0.5 = 36 mm, refused; the same motion
    // short enough would have gone through.
    f.fp->reject_movexyz_above_mm_ = 15.0f;

    HandState warm = make_hand();
    warm.delta = Vec3{0.0f, 0.0f, 0.0f};
    f.link.update(warm, 900);            // first frame: latches the delta basis only

    HandState push = make_hand();
    push.delta = Vec3{0.0f, 0.13f, 0.0f};   // 0.13 * 120 = 15.6 mm, over the wall
    f.link.update(push, 1000);              // refused -> the learner runs

    ASSERT(f.link.commands_rejected() >= 1);

    const float at[3] = {115.033f, 0.0f, 109.224f};
    // THE INVARIANT: at the pose the refusal came from, outward travel is still
    // available. Pinning the bound to this pose would leave exactly zero.
    ASSERT(!f.link.push_out_of_bound(at, 0, +5.0f));
    ASSERT(f.link.boundary_weight(at, 0, +1.0f) > 0.999f);

    // Control: a step that really does overshoot the attempted target is still
    // caught, so this is headroom and not "the learner stopped learning".
    // 115.033 + 15.6 = 130.633 was the refused target; 25 mm clears it.
    ASSERT(f.link.push_out_of_bound(at, 0, +25.0f));
    return true;
}

// ---------------------------------------------------------------------------
// ⑰d ...and the bound a refusal did pull in has to EXPIRE.
//
//     ⑰c puts the bound one step out, which is right, but a single refusal is
//     still only evidence about one point on a face of the reachable shell - and
//     on this backend it may not even be about the axis it moved. So the bound
//     drifts back out toward its prior band (the measured span, never the caps
//     box) a little on every accepted command.
//
//     Without the drift the bound is a one-way ratchet: each refusal walks the
//     arm's usable range in by a step and nothing ever gives it back, which is
//     the same permanent squeeze the `min(from+step, from)` write caused - just
//     arriving one step per refusal instead of all at once.
// ---------------------------------------------------------------------------
TEST(robot_learned_bound_decays_back_toward_the_prior_band) {
    RobotConfig cfg = test_cfg();
    cfg.soft_boundary = true;
    cfg.soft_probe_on_reject = false;
    cfg.reach_span_x = 60.0f;            // prior band = HOME +/- 30 mm
    cfg.reach_span_y = 176.0f;
    cfg.reach_span_z = 42.0f;
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

    // One refused +x step puts the bound at 115.033 + 15.6 = 130.633, which is
    // INSIDE the 145.033 prior edge, so there is room for the drift to open.
    HandState push = make_hand();
    push.delta = Vec3{0.0f, 0.13f, 0.0f};
    f.link.update(push, 1000);
    ASSERT(f.link.commands_rejected() >= 1);

    const float at[3] = {115.033f, 0.0f, 109.224f};
    // +20 mm lands at 135.033: past the fresh bound, inside the prior edge.
    ASSERT(f.link.push_out_of_bound(at, 0, +20.0f));

    // Now drive the arm INWARD, which is always untapered, so every frame is an
    // accepted command and therefore a relax tick credit. -0.05 of hand travel
    // is 6 mm, comfortably under the fake's 15 mm wall, and the hand offset only
    // ever needs 48 * 0.05 = 2.4 of travel to bank the 6 ticks below.
    const int kFrames = 48;
    for (int i = 0; i < kFrames; ++i) {
        HandState back = make_hand();
        back.delta = Vec3{0.0f, 0.13f - 0.05f * static_cast<float>(i + 1), 0.0f};
        f.link.update(back, 1100 + static_cast<int64_t>(i) * 100);
    }

    // 48 acceptances = 6 relax ticks at kRelaxEveryNAccepts = 8, each moving the
    // bound 10% of the remaining gap: 130.633 -> ~137.4, so the same +20 mm step
    // is inside again. The direction is what matters, not the exact millimetre.
    ASSERT(!f.link.push_out_of_bound(at, 0, +20.0f));
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
    f.link.update(push, 1000);      // first increment; latches the reported position

    // The taper must be a no-op: the weight is exactly 1 on every axis, so the
    // emitted step is the full un-tapered one and the fake accepts it.
    const float pos[3] = {f.link.caps().ws_max[0], 0.0f, 100.0f};
    ASSERT(std::fabs(f.link.boundary_weight(pos, 0) - 1.0f) < 1e-6f);

    const int64_t sent_before   = f.link.commands_sent();
    const int64_t skipped_before = f.link.commands_skipped();
    const size_t  reqs_before    = f.fp->requests.size();

    // Keep the hand travelling one 0.10 increment per period, so each frame has
    // a real step to emit (a held delta is now correctly "no motion").
    float dy = 0.10f;
    for (int i = 1; i <= 5; ++i) {
        dy += 0.10f;
        push.delta = Vec3{0.0f, dy, 0.0f};
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

    // The following frames send nothing: `push.delta` is unchanged, and
    // HandState::delta is CUMULATIVE, so the increment against the previous
    // frame is zero and the frame is gobbled by the deadzone. (Not a claim about
    // the learnt bound - the probe accepted, so the learner never even saw this
    // refusal; its own tests are ⑰c/⑰d below.)
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
//    finding the near-1:1 response too jumpy, then asked for a LONGER reach
//    after living with 0.25 on X/Z (2026-09-29, two passes).
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
//      X       39.0    0.40       0.62        JUST above -> reaches the arm
//      Y      176.0    0.50       3.52        passes
//      Z       41.5    0.40       0.66        JUST above -> reaches the arm
//
//    The X/Z lines are the documented cost of the 0.25 -> 0.4 raise: at 0.25
//    their tremor was 0.39-0.42 mm and the floor filtered it; at 0.4 it is
//    0.62-0.66 mm and it gets through. The test asserts the new truth rather
//    than the old one, so a future "why does it buzz" question has an answer
//    in the diff instead of an archaeology session.
// ---------------------------------------------------------------------------
TEST(robot_shipped_gains_are_the_documented_feel) {
    const RobotConfig def;

    ASSERT(std::fabs(def.gain_x - 0.40f) < 1e-6f);
    ASSERT(std::fabs(def.gain_y - 0.50f) < 1e-6f);
    ASSERT(std::fabs(def.gain_z - 0.40f) < 1e-6f);

    // The spans those gains are read against. Kept here so the mm-per-unit
    // arithmetic above is checkable rather than folklore.
    ASSERT(std::fabs(def.reach_span_x - 39.0f) < 1e-6f);
    ASSERT(std::fabs(def.reach_span_y - 176.0f) < 1e-6f);
    ASSERT(std::fabs(def.reach_span_z - 41.5f) < 1e-6f);

    // The jitter claim, stated as arithmetic against the effective floor
    // (kMinDeltaMm, mirrored here at 0.5). At 0.4 X and Z sit just ABOVE the
    // floor - that is the deliberate trade for reach, and the margin is
    // asserted so a later tweak that silently re-filters them (or blows them
    // far past it) shows up here.
    const float jitter = 0.02f;             // HandConfig::motion_deadzone
    const float span_over_active = 1.0f / 0.5f;  // axis_scale divisor
    const float step_x = span_over_active * def.reach_span_x * def.gain_x * jitter;
    const float step_y = span_over_active * def.reach_span_y * def.gain_y * jitter;
    const float step_z = span_over_active * def.reach_span_z * def.gain_z * jitter;

    ASSERT(step_x > 0.5f && step_x < 1.0f);
    ASSERT(step_z > 0.5f && step_z < 1.0f);
    ASSERT(step_y > 3.0f);

    // And the user-facing statement: a typical deliberate move (0.10) must still
    // produce a visible step on every axis, or the arm feels broken.
    const float move = 0.10f;
    ASSERT(span_over_active * def.reach_span_x * def.gain_x * move > 1.0f);
    ASSERT(span_over_active * def.reach_span_y * def.gain_y * move > 1.0f);
    ASSERT(span_over_active * def.reach_span_z * def.gain_z * move > 1.0f);
    return true;
}

// ---------------------------------------------------------------------------
// ㉒ THE CUMULATIVE-DELTA BUG.
//
// HandState::delta is `position - reference` against a reference that is latched
// once and never moves (hand_analyzer.cpp step 3), so it is a CUMULATIVE offset:
// it does NOT return to zero when the hand stops. The link used to feed it
// straight into the gain, which meant "hand moved 0.10 and is now holding still"
// was commanded as 0.10-worth of millimetres on EVERY frame - the arm crept,
// stalled at the deadzone and then jumped. Measured on hardware as "the motion
// is not continuous, it feels like stutter then a sudden move".
//
// The link must therefore take the per-frame DIFFERENCE, which is what this
// test pins. The failure mode it guards against is invisible to every other
// test in this file, because they all feed a delta that happens to change.
// ---------------------------------------------------------------------------
TEST(robot_cumulative_delta_is_not_replayed_every_frame) {
    Fixture f(test_cfg());
    ASSERT(f.link.connect_if_needed());

    // Frame 0: the hand is at the reference (delta 0). This frame only LATCHES
    // the differencing basis - it has no predecessor to subtract - so it sends
    // nothing. That is deliberate: starting from an implicit zero would emit the
    // whole accumulated offset as one step, which is the very jump being fixed.
    HandState hs = make_hand();
    hs.delta = Vec3{0.0f, 0.0f, 0.0f};
    f.link.update(hs, 900);
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), 0);

    // Frame 1: the hand has now moved to +0.10, so delta reports 0.10. ONE
    // command goes out, worth 0.10 (not 0.10 plus the locked-in basis).
    hs.delta = Vec3{0.10f, 0.0f, 0.0f};
    f.link.update(hs, 1000);
    const int64_t after_first = count_cmds(f.fp->requests, "movexyz");
    ASSERT_EQ(after_first, static_cast<int64_t>(1));

    // Frames 2..6: THE HAND IS NOW HOLDING STILL, so delta stays at 0.10 (it is
    // the same cumulative offset). Nothing more may be sent.
    for (int i = 2; i <= 6; ++i) {
        f.link.update(hs, 1000 + (i - 1) * 100);
    }
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), after_first);

    // The step that DID go out must be one 0.10 increment, not a multiple of it.
    // Axis mapping (see step()): hand X -> arm -Y. So a hand X increment of 0.10
    // appears as the SECOND component of the arm's delta, scaled by the arm's Y
    // span (gain pinned to 1.0 in test_cfg).
    const float box_y = f.link.caps().ws_max[1] - f.link.caps().ws_min[1];
    const float expect = -0.10f * (box_y / 0.5f);
    const float seen = last_movexyz_delta(f.fp->requests, 1);
    ASSERT(std::fabs(seen - expect) < 0.05f);

    // And a further real move emits exactly the INCREMENT, not the new total.
    hs.delta = Vec3{0.15f, 0.0f, 0.0f};        // +0.05 more
    f.link.update(hs, 2000);
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), after_first + 1);
    const float expect_inc = -0.05f * (box_y / 0.5f);
    const float last = last_movexyz_delta(f.fp->requests, 1);
    ASSERT(std::fabs(last - expect_inc) < 0.05f);
    return true;
}

// ---------------------------------------------------------------------------
// ㉓ Gesture mute window: a discrete gesture must be a SWITCH, not a move.
//
// Forming a fist is not a still pose - the fingers curl, the landmark box
// shrinks and its centre drifts - so the hand's reported position genuinely
// moves for the several frames it takes the classifier to confirm (5 frames,
// ~625 ms at the hand branch's ~8 Hz). The operator's intent is "close the
// gripper", and the arm used to follow all of that motion instead.
// ---------------------------------------------------------------------------
TEST(robot_gesture_mutes_continuous_motion) {
    RobotConfig cfg = test_cfg();
    cfg.gesture_mute_ms = 500;
    Fixture f(cfg);
    ASSERT(f.link.connect_if_needed());

    // A moving hand establishes the basis first.
    HandState hs = make_hand();
    hs.delta = Vec3{0.0f, 0.0f, 0.0f};
    f.link.update(hs, 1000);

    // The fist confirms: one gripper command, and NO move.
    hs.gesture = Gesture::Fist;
    const int64_t moves_before = count_cmds(f.fp->requests, "movexyz");
    f.link.update(hs, 1100);
    ASSERT_EQ(count_cmds(f.fp->requests, "gripper"), static_cast<int64_t>(1));
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), moves_before);

    // Now the operator's hand travels WHILE the fist is being held. Every one of
    // these frames must be muted: the motion is gesture-forming (or post-gesture
    // drift), not steering.
    float d = 0.10f;
    for (int i = 1; i <= 4; ++i) {
        d += 0.10f;
        hs.delta = Vec3{d, 0.0f, 0.0f};
        f.link.update(hs, 1100 + i * 100);     // all inside the 500 ms window
    }
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), moves_before);

    // Once the window expires the arm follows again - and, crucially, it does so
    // from the CURRENT hand position: the motion during the window is DISCARDED,
    // not replayed as one accumulated jump.
    d += 0.10f;
    hs.delta = Vec3{d, 0.0f, 0.0f};
    f.link.update(hs, 1700);                   // 600 ms after the mute started
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), moves_before + 1);
    return true;
}

// A muted window with a gesture-mute of 0 must behave as if the feature were
// off: the documented escape hatch, same philosophy as soft_boundary=false.
TEST(robot_gesture_mute_off_is_the_old_behaviour) {
    RobotConfig cfg = test_cfg();
    cfg.gesture_mute_ms = 0;
    Fixture f(cfg);
    ASSERT(f.link.connect_if_needed());

    HandState hs = make_hand();
    hs.delta = Vec3{0.0f, 0.0f, 0.0f};
    f.link.update(hs, 1000);

    hs.gesture = Gesture::Fist;
    f.link.update(hs, 1100);
    ASSERT_EQ(count_cmds(f.fp->requests, "gripper"), static_cast<int64_t>(1));

    // No window, so the very next moving frame goes straight out.
    const int64_t moves_before = count_cmds(f.fp->requests, "movexyz");
    hs.delta = Vec3{0.10f, 0.0f, 0.0f};
    f.link.update(hs, 1200);
    ASSERT_EQ(count_cmds(f.fp->requests, "movexyz"), moves_before + 1);
    return true;
}
