// Robot control on REAL captured frames.
//
// The synthetic sequence (robot_synthetic_tests.cpp) proves the chain against a
// known geometric ground truth: the generator composited the hand, so it knows
// where the hand is and can assert the mapped position to within a tolerance.
//
// These frames are different in kind. They are real captures of a hand held
// near the face - no ground truth exists, the hand is partly occluded, and the
// pose drifts. Asserting a mapped position here would be inventing a number.
//
// So this file asserts the properties that ARE knowable without ground truth:
//
//   1. the detector actually finds the hand (a real hit rate, not zero);
//   2. every HandState it produces is well formed (finite, in range, and its
//      delta is only non-zero once the reference is locked - the invariant that
//      stops a first-frame origin jump from throwing the arm);
//   3. the whole chain survives real pixels: no transport error, no rejection;
//   4. the arm received real commands.
//
// In other words: the synthetic test pins ACCURACY, this one pins ROBUSTNESS on
// real data. Neither is a substitute for the other.
#include "camera_agent/ai/hand_analyzer.h"
#include "camera_agent/ai/hand_detector.h"
#include "camera_agent/ai/hand_types.h"
#include "camera_agent/robot/json_line_client.h"
#include "camera_agent/robot/robot_link.h"
#include "test_harness.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "stb_image.h"

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

// The real capture set. 40 frames, 1280x720, captured live.
std::string find_realframe_dir() {
    const std::string env = safe_getenv("AI_REALFRAME_DIR");
    if (!env.empty() && std::ifstream((env + "/f_00.png").c_str()).good()) {
        return env;
    }
    const char* candidates[] = {
        "tests/finished/orient2",
        "carmera-agent/tests/finished/orient2",
        "../tests/finished/orient2",
        "../../tests/finished/orient2",
    };
    for (const char* c : candidates) {
        if (std::ifstream((std::string(c) + "/f_00.png").c_str()).good()) {
            return c;
        }
    }
    return {};
}

bool hand_models_present() {
    return std::ifstream("models/palm_detection_mediapipe_2023feb.onnx").good() &&
           std::ifstream("models/handpose_estimation_mediapipe_2023feb.onnx").good();
}

// ---------------------------------------------------------------------------
// Same scripted transport as the synthetic test: answers with the REAL caps
// payload so the bounds and the axis mapping are the genuine ones.
// ---------------------------------------------------------------------------
const char* kCaps =
    R"({"ok": true, "cmd": "caps", "caps": {"protocol": 2, "device": "sim", )"
    R"("units": {"length": "mm", "angle": "deg"}, )"
    R"("frame": {"x": "front", "y": "left", "z": "up", "handed": "right"}, )"
    R"("geom": {"pivotZ": 60, "l1": 80, "l2": 80, "toolR": 40, )"
    R"("reachMin": 0, "reachMax": 160}, )"
    R"("joints": [{"id": 0, "role": "base", "min": -60, "max": 60}], )"
    R"("servos": [{"n": 1, "id": "servo_9", "joint": "base", "pwmMin": 30, "pwmMax": 150}], )"
    R"("home": {"base": 0, "shoulder": 0.8498937633, "elbow": 112.6185771989, "gripper": 50}, )"
    R"("workspace": {"min": [40.458, -153.012, 49.085], )"
    R"("max": [175.883, 153.012, 114.680], "samples": 12}, )"
    R"("commands": ["move", "gripper", "servo", "state", "movexyz", "moveto", "home", "caps"]}})";

const char* kHomeReply =
    R"({"ok": true, "cmd": "home", "state": {"joints": {"base": 0}, )"
    R"("tcp": [115.03347742002849, 0, 109.22362788339144], "device": "sim"}})";

const char* kStateReply =
    R"({"ok": true, "cmd": "movexyz", "state": {"joints": {"base": 0}, )"
    R"("tcp": [115.03347742002849, 0, 109.22362788339144], "device": "sim"}})";

class RecordingClient : public ca::IJsonLineClient {
public:
    bool connect() override { connected_ = true; return true; }
    void close() override { connected_ = false; }
    bool connected() const override { return connected_; }

    bool exchange(const std::string& request, ca::JsonReply& reply) override {
        requests.push_back(request);
        reply.raw.clear();
        reply.ok = true;
        reply.error.clear();
        if (request.find("\"caps\"") != std::string::npos) {
            reply.raw = kCaps;
        } else if (request.find("\"home\"") != std::string::npos) {
            reply.raw = kHomeReply;
        } else if (request.find("\"movexyz\"") != std::string::npos ||
                   request.find("\"moveto\"") != std::string::npos ||
                   request.find("\"gripper\"") != std::string::npos) {
            reply.raw = kStateReply;
        } else {
            reply.ok = false;
            reply.error = "unknown cmd";
        }
        return true;
    }

    bool connected_ = false;
    std::vector<std::string> requests;
};

int count_cmds(const std::vector<std::string>& reqs, const std::string& cmd) {
    int n = 0;
    for (const auto& r : reqs) {
        if (r.find("\"" + cmd + "\"") != std::string::npos) ++n;
    }
    return n;
}

ca::RobotConfig test_cfg() {
    ca::RobotConfig c;
    c.enable = true;
    c.host = "127.0.0.1";
    c.port = 9100;
    c.map_mode = "delta";
    c.rate_hz = 60;            // high, so each planned frame is not throttled
    c.deadzone_mm = 0.5f;
    c.home_on_presence = true;
    c.gesture_home = "off";
    c.gesture_gripper = true;
    c.lost_frames = 2;
    return c;
}

bool finite3(const ca::Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

}   // namespace

// ---------------------------------------------------------------------------
// The chain on real pixels.
// ---------------------------------------------------------------------------
TEST(robot_realframes_drive_arm_without_ground_truth) {
    const std::string dir = find_realframe_dir();
    if (dir.empty()) {
        std::cout << "  [skip] real-frame set not found (expected "
                     "tests/finished/orient2/f_00..f_39.png)\n";
        return true;
    }
    if (!hand_models_present()) {
        std::cout << "  [skip] hand models missing from models/\n";
        return true;
    }

    ca::HandConfig hcfg;   // defaults: depth_source "size"
    std::unique_ptr<ca::IHandDetector> det = ca::create_hand_detector();
    ASSERT(det != nullptr);
    ASSERT(det->init(hcfg));

    ca::HandAnalyzer analyzer;
    ASSERT(analyzer.init(hcfg));

    auto client = std::make_unique<RecordingClient>();
    RecordingClient* rc = client.get();
    ca::RobotLink link;
    link.set_client(std::move(client));
    ASSERT(link.init(test_cfg()));
    ASSERT(link.connect_if_needed());
    ASSERT(link.caps().valid);

    int frames = 0, hits = 0, misses = 0;
    int bad_state = 0;          // not finite / out of range
    int delta_before_ref = 0;   // delta non-zero while the reference was locked=no
    int cmds = 0;

    for (int i = 0; i < 40; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "/f_%02d.png", i);
        const std::string path = dir + name;
        int w = 0, h = 0, n = 0;
        uint8_t* rgb = stbi_load(path.c_str(), &w, &h, &n, 3);
        if (!rgb) continue;                 // a gap in the set is not a failure
        ++frames;

        ca::HandDetection d;
        const bool ok = det->detect_hand(rgb, w, h, d);
        stbi_image_free(rgb);
        ASSERT(ok);
        if (d.detected) ++hits; else ++misses;

        ca::HandState hs;
        analyzer.update(d, w, h, static_cast<uint64_t>(i), 0, 30.0, hs);

        // (2) Every state must be well formed.
        if (hs.detected) {
            const ca::Vec3 p = hs.position;
            if (!finite3(p) || p.x < -0.01f || p.x > 1.01f ||
                p.y < -0.01f || p.y > 2.5f || p.z < -0.01f || p.z > 1.01f) {
                ++bad_state;
                std::cout << "  [state] frame " << i << " position out of range ("
                          << p.x << "," << p.y << "," << p.z << ")\n";
            }
            if (!finite3(hs.delta)) ++bad_state;
            // The invariant that matters: before the reference is locked the
            // delta is the distance from the frame origin, which is huge and
            // meaningless. Consuming it would throw the arm across the table.
            if (!hs.reference_initialized &&
                (hs.delta.x != 0.0f || hs.delta.y != 0.0f || hs.delta.z != 0.0f)) {
                ++delta_before_ref;
            }
        }

        const size_t before = rc->requests.size();
        link.update(hs, 1000 + i * 33);
        cmds += static_cast<int>(rc->requests.size() - before);
    }

    std::cout << "  real frames: " << frames << " detected " << hits
              << " missed " << misses
              << " (" << (frames ? (100.0 * hits / frames) : 0.0) << "%)"
              << ", commands " << cmds
              << ", movexyz=" << count_cmds(rc->requests, "movexyz")
              << ", home=" << count_cmds(rc->requests, "home")
              << ", rejected=" << link.commands_rejected() << "\n";

    // (1) The detector must actually see this hand. A zero here means the real
    //     capture is nothing like the model's world and the rest proves nothing.
    ASSERT(frames == 40);
    ASSERT(hits > 0);

    // (2) No malformed state, and the pre-reference invariant must hold exactly.
    ASSERT_EQ(bad_state, 0);
    ASSERT_EQ(delta_before_ref, 0);

    // (3) Real pixels must not break the transport.
    ASSERT(link.connected());
    ASSERT_EQ(link.commands_rejected(), static_cast<int64_t>(0));

    // (4) The hand moved, so the chain must have produced traffic.
    ASSERT(cmds > 0);
    ASSERT(count_cmds(rc->requests, "movexyz") > 0);
    return true;
}

// ---------------------------------------------------------------------------
// The same frames through the real worker thread, to make sure the queueing
// path is exercised on real data too (the synthetic test does this for the
// composited sequence; this does it for the live capture).
// ---------------------------------------------------------------------------
TEST(robot_realframes_over_worker_thread) {
    const std::string dir = find_realframe_dir();
    if (dir.empty() || !hand_models_present()) {
        std::cout << "  [skip] real-frame set or models not available\n";
        return true;
    }

    ca::HandConfig hcfg;
    std::unique_ptr<ca::IHandDetector> det = ca::create_hand_detector();
    ASSERT(det != nullptr);
    ASSERT(det->init(hcfg));
    ca::HandAnalyzer analyzer;
    ASSERT(analyzer.init(hcfg));

    auto client = std::make_unique<RecordingClient>();
    RecordingClient* rc = client.get();
    ca::RobotLink link;
    link.set_client(std::move(client));
    ASSERT(link.init(test_cfg()));

    link.start();
    ASSERT(link.is_running());

    for (int i = 0; i < 40; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "/f_%02d.png", i);
        int w = 0, h = 0, n = 0;
        uint8_t* rgb = stbi_load((dir + name).c_str(), &w, &h, &n, 3);
        if (!rgb) continue;
        ca::HandDetection d;
        const bool ok = det->detect_hand(rgb, w, h, d);
        stbi_image_free(rgb);
        ASSERT(ok);
        ca::HandState hs;
        analyzer.update(d, w, h, static_cast<uint64_t>(i), 0, 30.0, hs);
        link.push_hand(hs);
    }

    for (int i = 0; i < 100 && count_cmds(rc->requests, "movexyz") == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::cout << "  worker: connected=" << link.connected()
              << " movexyz=" << count_cmds(rc->requests, "movexyz")
              << " home=" << count_cmds(rc->requests, "home") << "\n";

    ASSERT(link.connected());
    ASSERT(link.caps().valid);
    ASSERT(count_cmds(rc->requests, "movexyz") > 0);

    link.stop();
    ASSERT(!link.is_running());
    return true;
}
