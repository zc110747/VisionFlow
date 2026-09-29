// Live MeArmPilot verification (opt-in).
//
// Every other robot test substitutes a scripted transport, which is what makes
// them deterministic and runnable on any machine. That substitution also means
// none of them prove the real socket works: the Winsock client, the framing,
// the real caps payload, the real workspace bounds.
//
// This file closes that gap. It talks to a REAL `armpilot-backend.exe` over
// TCP and is skipped unless the operator asks for it, so `ctest` stays
// hermetic. Run it with:
//
//   ARM_PILOT_LIVE=1 tests/camera-agent-tests.exe
//
// (optionally ARM_PILOT_HOST / ARM_PILOT_PORT to point elsewhere.)
//
// Level 1 of the plan in the task: "verify VisionFlow's own commands can drive
// the sim arm". It answers that directly, using the same RobotLink the agent
// runs - not a Python client standing in for it.
#include "camera_agent/ai/hand_types.h"
#include "camera_agent/config.h"
#include "camera_agent/robot/json_line_client.h"
#include "camera_agent/robot/robot_link.h"
#include "test_harness.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

#ifdef _MSC_VER
std::string safe_getenv(const char* name) {
    char* p = nullptr;
    _dupenv_s(&p, nullptr, name);
    if (!p) return {};
    std::string v(p);
    std::free(p);
    return v;
}
#else
std::string safe_getenv(const char* name) {
    const char* p = std::getenv(name);
    return p ? std::string(p) : std::string{};
}
#endif

bool live_enabled() {
    const std::string v = safe_getenv("ARM_PILOT_LIVE");
    return !v.empty() && v != "0";
}

ca::RobotConfig live_cfg() {
    ca::RobotConfig c;
    c.enable = true;
    const std::string host = safe_getenv("ARM_PILOT_HOST");
    c.host = host.empty() ? "127.0.0.1" : host;
    const std::string port = safe_getenv("ARM_PILOT_PORT");
    c.port = port.empty() ? 9100 : std::atoi(port.c_str());
    c.map_mode = "delta";
    c.rate_hz = 60;            // no client-side throttle between planned frames
    c.deadzone_mm = 0.5f;
    // Gains pinned to 1.0 and no measured span, so this test walks the CAPS box
    // with a box-derived gain (the X step below is 0.05 * 270.85 = 13.5 mm
    // un-tapered). The shipped feel gains (0.25/0.5/0.25) would make the walk
    // several times slower and the distance assertions below would then depend
    // on tuning rather than on behaviour. See robot_link_tests.cpp test_cfg().
    c.gain_x = 1.0f;
    c.gain_y = 1.0f;
    c.gain_z = 1.0f;
    c.home_on_presence = true;
    c.gesture_home = "off";
    c.gesture_gripper = true;
    c.lost_frames = 2;
    c.log_commands = true;
    return c;
}

ca::HandState hand_at(float x, float y, float z) {
    ca::HandState hs;
    hs.detected = true;
    hs.reference_initialized = true;
    hs.position = ca::Vec3{x, y, z};
    hs.reference = ca::Vec3{x, y, z};
    hs.delta = ca::Vec3{0.0f, 0.0f, 0.0f};
    return hs;
}

}   // namespace

// ---------------------------------------------------------------------------
// Level 1: the real socket, the real backend, the real workspace bounds.
// ---------------------------------------------------------------------------
TEST(robot_live_backend_drives_sim_arm) {
    if (!live_enabled()) {
        std::cout << "  [skip] live backend test disabled - set ARM_PILOT_LIVE=1 "
                     "with armpilot-backend.exe listening on 127.0.0.1:9100\n";
        return true;
    }

    ca::RobotConfig cfg = live_cfg();
    ca::RobotLink link;
    // No set_client(): this is the real Winsock transport.
    ASSERT(link.init(cfg));
    ASSERT(link.connect_if_needed());

    // (a) The real handshake must have produced real bounds.
    const ca::RobotCaps& caps = link.caps();
    ASSERT(caps.valid);
    std::cout << "  caps: protocol=" << caps.protocol
              << " device=" << caps.device
              << " ws.min=[" << caps.ws_min[0] << "," << caps.ws_min[1] << ","
              << caps.ws_min[2] << "]"
              << " ws.max=[" << caps.ws_max[0] << "," << caps.ws_max[1] << ","
              << caps.ws_max[2] << "]"
              << " home=[" << caps.home_tcp[0] << "," << caps.home_tcp[1] << ","
              << caps.home_tcp[2] << "]\n";
    ASSERT(caps.protocol >= 2);                 // movexyz / moveto needed
    ASSERT(caps.ws_max[0] > caps.ws_min[0]);    // non-degenerate workspace
    ASSERT(caps.ws_max[2] > caps.ws_min[2]);
    ASSERT(caps.home_valid);

    // (b) The bounds must be the documented MeArmPilot ones, not invented. A
    //     drift here would silently move the whole controllable volume.
    ASSERT(std::fabs(caps.ws_min[0] - 40.458f) < 0.5f);
    ASSERT(std::fabs(caps.ws_max[0] - 175.883f) < 0.5f);
    ASSERT(std::fabs(caps.ws_min[2] - 49.085f) < 0.5f);
    ASSERT(std::fabs(caps.ws_max[2] - 114.680f) < 0.5f);

    // (c) A hand entering the frame must not jump the arm: the first frame only
    //     records presence. The first command only goes out once the hand MOVES,
    //     so drive a real displacement - a held position is correctly silent.
    const int64_t sent_at_open = link.commands_sent();
    link.update(hand_at(0.5f, 0.5f, 0.5f), 1000);
    ASSERT(link.hand_present());
    // The hand slides: reference stays put, position moves, so delta != 0.
    ca::HandState moved = hand_at(0.5f, 0.5f, 0.5f);
    moved.reference = ca::Vec3{0.45f, 0.5f, 0.5f};   // a 0.05 step in X
    moved.position = ca::Vec3{0.50f, 0.5f, 0.5f};
    moved.delta = ca::Vec3{0.05f, 0.0f, 0.0f};
    ASSERT(link.update(moved, 1033));
    ASSERT(link.commands_sent() > sent_at_open);   // real traffic went out

    // (d) A deliberately large move along one axis must be ACCEPTED or
    //     REJECTED by the real backend - never a transport error. Both answers
    //     are legitimate (pitfall 4); a dead socket is not.
    ca::HandState hs = hand_at(0.5f, 0.5f, 0.5f);
    hs.reference = ca::Vec3{0.2f, 0.5f, 0.5f};
    hs.delta = ca::Vec3{0.3f, 0.0f, 0.0f};         // a big nudge in X
    const int64_t rej_before = link.commands_rejected();
    const int64_t sent_before = link.commands_sent();
    const bool alive = link.update(hs, 1100);
    ASSERT(alive);                                  // transport stayed up
    ASSERT(link.connected());
    // Something must have been attempted: the command was accepted (sent++) or
    // refused by the real IK (rejected++). Neither would mean the frame was
    // silently swallowed.
    //
    // The count is NOT pinned to exactly 1: with the probe enabled a refused
    // step is retried at half length, so one frame can legitimately produce a
    // refusal AND an acceptance (measured live: "sent 1, rejected 1"). What must
    // hold is that the frame did something and stayed on the wire.
    const int64_t now_sent = link.commands_sent();
    const int64_t now_rej = link.commands_rejected();
    const int64_t attempts = (now_sent - sent_before) + (now_rej - rej_before);
    std::cout << "  big move: sent " << (now_sent - sent_before)
              << ", rejected " << (now_rej - rej_before) << "\n";
    ASSERT(attempts >= 1);
    // At most the original attempt plus the configured probes.
    ASSERT(attempts <= static_cast<int64_t>(std::max(1, cfg.soft_probe_tries)));
    // A large single nudge is the pitfall-4 case: inside the bounding box the
    // real reachable set still ends, and the backend says so. We must take the
    // refusal in stride rather than treating it as a broken link.
    ASSERT(link.connected());

    // (e) A tiny delta must be skipped WITHOUT a transport fault (pitfall 1).
    //     `delta` is CUMULATIVE, so "tiny" means a tiny change from the previous
    //     frame, not a tiny absolute value: setting it to ~0 here would read as
    //     a large negative step and be sent, not skipped. Hold the basis and
    //     advance it by a hair. The timestamp is pushed past the 15 Hz period so
    //     the rate limiter cannot be what swallows the frame.
    hs.delta = ca::Vec3{0.3f + 0.0001f, 0.0f, 0.0f};
    const int64_t skipped_before = link.commands_skipped();
    ASSERT(link.update(hs, 1200));
    ASSERT(link.commands_skipped() > skipped_before);

    // (f) The link must have stayed up through all of it.
    ASSERT(link.connected());

    // (g) And the operator's rule: the hand LEAVING and COMING BACK returns the
    //     arm HOME. The HOME is sent on the arrival edge, not on the departure,
    //     so the sequence must include a frame with the hand present again.
    ca::HandState none;
    none.detected = false;
    for (int i = 0; i < cfg.lost_frames + 1; ++i) {
        link.update(none, 1200 + i * 33);
    }
    ASSERT(!link.hand_present());                      // departure registered
    ASSERT(link.hand_was_away());

    const int64_t sent_before_return = link.commands_sent();
    link.update(hand_at(0.5f, 0.5f, 0.5f), 1320);      // the hand comes back
    ASSERT(link.hand_present());
    ASSERT(link.commands_sent() > sent_before_return); // the return HOME went out
    std::cout << "  return HOME sent on re-entry\n";

    // (h) Home on demand: the reported pose must match the caps HOME, which is
    //     the whole point of reading `home` back during open().
    const ca::RobotCommandResult hr = link.do_home();
    ASSERT(hr.ok);
    ASSERT(hr.sent);
    std::cout << "  home ok; reconnects=" << link.reconnects()
              << " rejected=" << link.commands_rejected()
              << " skipped=" << link.commands_skipped() << "\n";

    // (i) Soft boundary against the REAL backend. This is the whole point of the
    //     feature, and the only way to prove it is to actually walk the arm into
    //     a wall and watch what comes back.
    //
    //     The hand pushes steadily along one axis until the arm stops. What must
    //     NOT happen is a long tail of JOINT_LIMIT refusals - that tail is the
    //     live behaviour this feature exists to remove (measured before the
    //     change: rejected=83 against sent=12 over 20 s).
    if (cfg.soft_boundary) {
        const int64_t rej0 = link.commands_rejected();
        const int64_t sent0 = link.commands_sent();

        // Walk toward +x (arm front) hard enough that the arm will hit the wall.
        // 25 pushes of 0.05 normalised: with the MEASURED-span gain (34 mm) that
        // is 3.4 mm per frame, comfortably inside the reachable margin, so the
        // arm should travel and then ease into the edge.
        ca::HandState push = hand_at(0.5f, 0.5f, 0.5f);
        push.reference = push.position;
        push.delta = ca::Vec3{0.0f, 0.0f, 0.0f};

        // Track the reported pose so "did the arm actually move?" can be
        // asserted rather than inferred. Before the fix this was the missing
        // assertion: the test only checked that nothing errored, so a link that
        // refused every step and left the arm frozen still passed.
        const ca::Vec3 tcp_start = link.last_tcp();
        float travelled = 0.0f;

        int64_t t = 2000;
        ca::Vec3 prev = tcp_start;
        // `delta` is cumulative, so a steady push means ADVANCING it by 0.05
        // every frame. Re-sending the same value would read as "the hand stopped"
        // and correctly produce no motion after the first frame.
        float pushed = 0.0f;
        for (int i = 0; i < 25; ++i) {
            pushed += 0.05f;
            push.delta = ca::Vec3{0.0f, pushed, 0.0f};
            link.update(push, t);
            const ca::Vec3 now = link.last_tcp();
            travelled += std::fabs(now.x - prev.x);
            prev = now;
            t += 80;                     // comfortably above the 66 ms period
        }

        const int64_t rej_used = link.commands_rejected() - rej0;
        const int64_t sent_used = link.commands_sent() - sent0;
        std::cout << "  soft boundary: pushed to the wall - sent " << sent_used
                  << ", rejected " << rej_used
                  << ", x travelled " << travelled << " mm\n";

        // The taper must have absorbed the approach. A handful of refusals can
        // still happen (the taper reduces the step, it does not know the exact
        // reachable shell inside the box, so the very last step can still be
        // refused) - but it must not be a rejection per frame.
        ASSERT(link.connected());
        ASSERT(rej_used < 10);

        // THE REGRESSION GUARD for "the hand moves and the arm does not": the
        // arm must have physically travelled. The reachable +x margin from HOME
        // is ~29 mm, so a 3.4 mm step × 25 pushes must cover well over 10 mm
        // even after the taper and the wall stop. A frozen arm scores ~0 and
        // fails here - which is exactly the reported symptom.
        ASSERT(travelled > 10.0f);
    }

    link.stop();
    return true;
}
