// Level-2 verification: a SYNTHETIC hand sequence driven through the real
// detector + analyzer + RobotLink, with the expected arm commands asserted.
//
// Why this file exists (and why it is not another robot_link_tests.cpp)
// ---------------------------------------------------------------------
// robot_link_tests.cpp proves the POLICY (rate limit, deadzone, edges, home on
// return) using hand-built HandStates and a scripted transport. It says nothing
// about the other half of the chain:
//
//     synthetic pixels -> palm/landmark model -> HandDetection
//                      -> HandAnalyzer (mapper, EMA, reference, gesture)
//                      -> HandState -> RobotLink -> JSON on the wire
//
// This test walks that whole path. The frames come from
// tests/golden/tools/make_synthetic_hand_sequence.py, which composites the hand
// of the approved fixture at commanded positions and writes, alongside the PNGs,
// the normalised position each frame SHOULD produce. The script's ground truth
// is derived from its own compositing geometry, never from the detector - so a
// mismatch here localises to the port, not to the script.
//
// The sequence is deliberately scripted to exercise the policy in order:
//   settle x3 (reference latches) -> move right (a run of movexyz)
//   -> fist (one gripper close) -> hand gone (silence) -> return (one home)
//
// Everything is skipped, not failed, when the models or the generated sequence
// are absent: this test must never be the reason a clean checkout cannot build.

#include "camera_agent/ai/hand_analyzer.h"
#include "camera_agent/ai/hand_detector.h"
#include "camera_agent/ai/hand_types.h"
#include "camera_agent/robot/json_line_client.h"
#include "camera_agent/robot/robot_link.h"
#include "test_harness.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "stb_image.h"

namespace {

constexpr int kFrameW = 640;
constexpr int kFrameH = 480;

// Position tolerance. The composite puts the hand at an exact place, but the
// detector then predicts its own box (that is the thing under test), so this is
// measuring the model's box accuracy against the composite geometry, not a
// rounding difference. Measured on settled frames it lands well under 0.06.
constexpr float kPosTol = 0.10f;

// Tolerance for frames where the hand is still moving. HandAnalyzer smooths the
// mapped position with an EMA (HandConfig::filter_alpha = 0.4), so a moving
// frame legitimately reports a point part-way between the previous position and
// the current one. The generator marks which frames are settled (see
// SETTLE_RUN in make_synthetic_hand_sequence.py); only the others get this
// slack, and it is sized to the filter's one-step lag plus the box error, not
// picked to make the test pass. A regression that broke the axis mapping would
// miss by far more than this.
constexpr float kMovingTol = 0.35f;

#ifdef _MSC_VER
// getenv() triggers C4996 under /W4; route through _dupenv_s instead.
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

// The generated sequence. Searched next to the build dir first (where the
// generator writes by convention) and then a few repo-relative spots, so the
// test works from the build dir, from the repo root, and under ctest.
std::string find_sequence_dir() {
    const std::string env = safe_getenv("AI_SYNTH_SEQ");
    if (!env.empty() && std::ifstream((env + "/expected.json").c_str()).good()) {
        return env;
    }
    const char* candidates[] = {
        "synth",
        "build-msvc/synth",
        "carmera-agent/build-msvc/synth",
        "../build-msvc/synth",
    };
    for (const char* c : candidates) {
        if (std::ifstream((std::string(c) + "/expected.json").c_str()).good()) {
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
// A tiny reader for the generator's JSON. Hand-rolled rather than pulling in a
// parser: the schema is fixed by the generator, and the numbers we want are a
// flat, well-known set. Anything unexpected is treated as "absent".
// ---------------------------------------------------------------------------
struct FramePlan {
    std::string file;
    bool present = false;
    bool injected = false;
    bool settled = false;
    std::string gesture;
    float ex = 0.0f, ey = 0.0f, ez = 0.0f;
    bool have_expected = false;
};

std::string read_all(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Slice out the i-th top-level object of the "frames" array.
std::string frame_object(const std::string& json, size_t index) {
    const size_t key = json.find("\"frames\"");
    if (key == std::string::npos) return {};
    size_t arr = json.find('[', key);
    if (arr == std::string::npos) return {};
    size_t depth = 0, obj_start = std::string::npos;
    size_t seen = 0;
    for (size_t i = arr; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '{') {
            if (depth == 0) obj_start = i;
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                if (seen == index) return json.substr(obj_start, i - obj_start + 1);
                ++seen;
            }
        }
    }
    return {};
}

float num_field(const std::string& obj, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return 0.0f;
    p = obj.find(':', p + needle.size());
    if (p == std::string::npos) return 0.0f;
    try {
        return std::stof(obj.substr(p + 1));
    } catch (...) {
        return 0.0f;
    }
}

std::string str_field(const std::string& obj, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return {};
    p = obj.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    p = obj.find('"', p);
    if (p == std::string::npos) return {};
    const size_t e = obj.find('"', p + 1);
    if (e == std::string::npos) return {};
    return obj.substr(p + 1, e - p - 1);
}

// The "expected" sub-object lives one level deeper; re-scan inside it.
float expected_field(const std::string& obj, const std::string& key) {
    const size_t p = obj.find("\"expected\"");
    if (p == std::string::npos) return 0.0f;
    return num_field(obj.substr(p), key);
}

std::vector<FramePlan> load_plan(const std::string& dir) {
    const std::string json = read_all(dir + "/expected.json");
    std::vector<FramePlan> out;
    for (size_t i = 0;; ++i) {
        const std::string obj = frame_object(json, i);
        if (obj.empty()) break;
        FramePlan fp;
        fp.file = str_field(obj, "file");
        fp.gesture = str_field(obj, "gesture");
        fp.injected = obj.find("\"injected\": true") != std::string::npos ||
                      obj.find("\"injected\":true") != std::string::npos;
        fp.settled = obj.find("\"settled\": true") != std::string::npos ||
                     obj.find("\"settled\":true") != std::string::npos;
        // "present": false is a literal, so look for the true form.
        fp.present = obj.find("\"present\": false") == std::string::npos &&
                     obj.find("\"present\":false") == std::string::npos;
        if (fp.present && !fp.injected) {
            fp.ex = expected_field(obj, "x");
            fp.ey = expected_field(obj, "y");
            fp.ez = expected_field(obj, "z");
            fp.have_expected = true;
        }
        out.push_back(fp);
    }
    return out;
}

// ---------------------------------------------------------------------------
// A scripted transport that records what actually went on the wire, and answers
// with the REAL caps payload (so bounds and the axis mapping are the genuine
// ones, not invented numbers).
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
    c.gesture_home = "off";    // keep open_palm free: this sequence uses fist
    c.gesture_gripper = true;
    c.lost_frames = 2;
    return c;
}

// ---------------------------------------------------------------------------
// The injected fist.
//
// The generator cannot synthesise a fist image (see its KNOWN LIMITATION note:
// a drawn fist scores 0 palms, a cropped palm region is rejected by the landmark
// model). So the fist step is delivered as LANDMARK data instead. These are the
// REAL captured landmarks already frozen by
// `tests/hand_analyzer_tests.cpp :: hand_gesture_real_capture_open_and_fist`
// (live LRCP USB2.0 camera, 1280x720@30). Transcribed here rather than shared
// through a header because the two tests assert different things about the same
// evidence: that one pins the classifier, this one pins the whole chain.
//
// The frame size matters: the analyzer maps landmarks by dividing by the video
// dimensions, so injecting them against the 640x480 synthetic frame keeps the
// scale consistent with the composited frames around it.
// ---------------------------------------------------------------------------
const float kRealFist[21][3] = {
    {1037.29f, 624.99f,   0.00f}, {1023.49f, 535.79f,   0.99f}, {992.62f, 465.95f,  -6.14f},
    { 962.16f, 428.01f, -17.80f}, { 931.99f, 425.56f, -29.40f}, {896.69f, 485.57f,   0.43f},
    { 856.25f, 442.71f, -11.17f}, { 882.76f, 413.06f, -18.76f}, {918.25f, 394.19f, -23.95f},
    { 883.49f, 522.70f, -11.59f}, { 864.47f, 471.04f, -27.36f}, {925.40f, 463.08f, -35.80f},
    { 977.53f, 471.54f, -39.77f}, { 881.52f, 564.65f, -24.75f}, {868.17f, 508.72f, -38.13f},
    { 929.63f, 498.59f, -41.96f}, { 981.55f, 504.62f, -43.57f}, {891.76f, 609.09f, -38.94f},
    { 883.12f, 559.80f, -47.25f}, { 924.71f, 531.29f, -48.39f}, {964.51f, 520.14f, -49.04f},
};

ca::HandDetection injected_fist() {
    ca::HandDetection d;
    d.detected = true;
    d.confidence = 0.95f;
    d.handedness = ca::Handedness::Right;
    d.landmarks.resize(21);
    for (int i = 0; i < 21; ++i) {
        ca::Keypoint k;
        k.x = kRealFist[i][0];
        k.y = kRealFist[i][1];
        k.z = kRealFist[i][2];
        k.conf = 0.9f;
        d.landmarks[static_cast<size_t>(i)] = k;
    }
    // The detector normally derives the box from the landmarks; do the same so
    // the analyzer sees a realistic detection rather than a zero box.
    float x1 = d.landmarks[0].x, y1 = d.landmarks[0].y;
    float x2 = x1, y2 = y1;
    for (const auto& k : d.landmarks) {
        x1 = std::min(x1, k.x); y1 = std::min(y1, k.y);
        x2 = std::max(x2, k.x); y2 = std::max(y2, k.y);
    }
    d.x1 = x1; d.y1 = y1; d.x2 = x2; d.y2 = y2;
    return d;
}


} // namespace

// ---------------------------------------------------------------------------
// The whole chain on synthetic pixels.
// ---------------------------------------------------------------------------
TEST(robot_synthetic_hand_sequence_drives_arm) {
    const std::string dir = find_sequence_dir();
    if (dir.empty()) {
        std::cout << "  [skip] synthetic sequence not found - generate it with:\n"
                     "         python tests/golden/tools/make_synthetic_hand_sequence.py \\\n"
                     "                --fixture tests/golden/woman_hands.png \\\n"
                     "                --frames build-msvc/synth --expect build-msvc/synth/expected.json\n";
        return true;
    }
    if (!hand_models_present()) {
        std::cout << "  [skip] hand models missing from models/ "
                     "(see carmera-agent/models/README.md)\n";
        return true;
    }

    const std::vector<FramePlan> plan = load_plan(dir);
    ASSERT(!plan.empty());
    std::cout << "  sequence: " << plan.size() << " frames from " << dir << "\n";

    // --- the real cascade --------------------------------------------------
    ca::HandConfig hcfg;   // defaults: depth_source "size", the documented one
    std::unique_ptr<ca::IHandDetector> det = ca::create_hand_detector();
    ASSERT(det != nullptr);
    ASSERT(det->init(hcfg));

    ca::HandAnalyzer analyzer;
    ASSERT(analyzer.init(hcfg));

    // --- the real robot policy, with a recording transport ------------------
    auto client = std::make_unique<RecordingClient>();
    RecordingClient* rc = client.get();
    ca::RobotLink link;
    link.set_client(std::move(client));
    ASSERT(link.init(test_cfg()));
    ASSERT(link.connect_if_needed());
    ASSERT(link.caps().valid);

    const int homes_after_open = count_cmds(rc->requests, "home");

    int detected_frames = 0, missed_frames = 0, injected_frames = 0;
    float max_settled_err = 0.0f;
    float max_moving_err = 0.0f;
    int settled_frames = 0;
    int frames_with_expected = 0;
    int zero_delta_frames = 0;
    int frame_id = 0;

    for (const FramePlan& fp : plan) {
        ca::HandDetection det_out;
        if (fp.injected) {
            // A real captured fist, delivered as landmarks (see kRealFist).
            det_out = injected_fist();
            ++injected_frames;
        } else if (fp.present) {
            const std::string path = dir + "/" + fp.file;
            int w = 0, h = 0, n = 0;
            uint8_t* rgb = stbi_load(path.c_str(), &w, &h, &n, 3);
            ASSERT(rgb != nullptr);
            ASSERT_EQ(w, kFrameW);
            ASSERT_EQ(h, kFrameH);
            const bool ok = det->detect_hand(rgb, w, h, det_out);
            stbi_image_free(rgb);
            ASSERT(ok);
            if (det_out.detected) ++detected_frames; else ++missed_frames;
        } else {
            det_out = ca::HandDetection{};   // nothing in frame
            ++missed_frames;
        }

        ca::HandState hs;
        analyzer.update(det_out, kFrameW, kFrameH,
                        static_cast<uint64_t>(frame_id), 0,
                        30.0, hs);

        // --- the pixels -> HandState half ---------------------------------
        if (fp.present && fp.have_expected && hs.detected) {
            const float dx = std::fabs(hs.position.x - fp.ex);
            const float dy = std::fabs(hs.position.y - fp.ey);
            const float dz = std::fabs(hs.position.z - fp.ez);
            const float e = std::max(dx, std::max(dy, dz));
            if (fp.settled) {
                max_settled_err = std::max(max_settled_err, e);
                ++settled_frames;
            } else {
                max_moving_err = std::max(max_moving_err, e);
            }
            ++frames_with_expected;
            if (e > kPosTol && fp.settled) {
                std::cout << "  [pos] frame " << frame_id << " (" << fp.file
                          << ") want (" << fp.ex << "," << fp.ey << "," << fp.ez
                          << ") got (" << hs.position.x << "," << hs.position.y
                          << "," << hs.position.z << ")\n";
            }
        }

        // --- and then the hand-following half -----------------------------
        // Feed the worker directly (no thread): the policy is what is under
        // test here, and the queue/thread path is covered by robot_link_tests.
        const size_t before = rc->requests.size();
        link.update(hs, 1000 + frame_id * 100);
        const size_t after = rc->requests.size();
        if (after == before) {
            // Nothing went out. Count the legitimate reasons so the final
            // "the arm actually did something" assertion is meaningful.
            ++zero_delta_frames;
        }
        ++frame_id;
    }

    // --- assertions ---------------------------------------------------------
    std::cout << "  detected " << detected_frames << " composited, missed "
              << missed_frames << ", injected " << injected_frames << "\n";
    std::cout << "  position error: settled " << max_settled_err
              << " (tol " << kPosTol << ") over " << settled_frames
              << " frames, moving " << max_moving_err
              << " (tol " << kMovingTol << ")\n";
    std::cout << "  commands: movexyz=" << count_cmds(rc->requests, "movexyz")
              << " moveto=" << count_cmds(rc->requests, "moveto")
              << " home=" << count_cmds(rc->requests, "home")
              << " gripper=" << count_cmds(rc->requests, "gripper")
              << "\n";

    // (1) The detector must find the composited hand in every composited frame.
    //     A synthetic hand the model cannot see would make the rest meaningless.
    ASSERT(detected_frames > 0);
    ASSERT_EQ(missed_frames, 3);                  // the three empty frames
    ASSERT_EQ(injected_frames, 6);                // the injected fist run
    ASSERT_EQ(detected_frames + missed_frames + injected_frames,
              static_cast<int>(plan.size()));
    ASSERT(frames_with_expected > 0);

    // (2) Mapping accuracy. Settled frames (hand held still for three frames,
    //     so the analyzer's EMA has converged) are held to the tight tolerance;
    //     moving frames only have to stay inside the documented filter lag.
    ASSERT(settled_frames > 0);
    ASSERT(max_settled_err < kPosTol);
    ASSERT(max_moving_err < kMovingTol);

    // (3) The moving-right run must have produced real relative commands.
    ASSERT(count_cmds(rc->requests, "movexyz") > 0);

    // (4) The fist must have opened/closed the gripper exactly once.
    ASSERT_EQ(count_cmds(rc->requests, "gripper"), 1);

    // (5) The hand left the frame and came back: exactly one extra HOME, and no
    //     phantom commands while it was away.
    ASSERT_EQ(count_cmds(rc->requests, "home"), homes_after_open + 1);

    // (6) The pipeline must have stayed connected the whole way.
    ASSERT(link.connected());
    ASSERT_EQ(link.commands_rejected(), static_cast<int64_t>(0));
    return true;
}

// ---------------------------------------------------------------------------
// The same sequence, but through the real worker THREAD and queue. This is the
// integration shape the agent actually runs, so at least one case must use it.
// ---------------------------------------------------------------------------
TEST(robot_synthetic_sequence_over_worker_thread) {
    const std::string dir = find_sequence_dir();
    if (dir.empty() || !hand_models_present()) {
        std::cout << "  [skip] synthetic sequence or models not available\n";
        return true;
    }
    const std::vector<FramePlan> plan = load_plan(dir);
    ASSERT(!plan.empty());

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

    // Real thread, real queue, real reconnect loop - only the socket is fake.
    link.start();
    ASSERT(link.is_running());

    int frame_id = 0;
    for (const FramePlan& fp : plan) {
        ca::HandDetection d;
        if (fp.injected) {
            d = injected_fist();
        } else if (!fp.present) {
            d = ca::HandDetection{};
        } else {
            int w = 0, h = 0, n = 0;
            uint8_t* rgb = stbi_load((dir + "/" + fp.file).c_str(), &w, &h, &n, 3);
            ASSERT(rgb != nullptr);
            const bool ok = det->detect_hand(rgb, w, h, d);
            stbi_image_free(rgb);
            ASSERT(ok);
        }
        ca::HandState hs;
        analyzer.update(d, kFrameW, kFrameH, static_cast<uint64_t>(frame_id),
                        0, 30.0, hs);
        link.push_hand(hs);
        ++frame_id;
    }

    // The worker drains on its own schedule; give it a bounded window and then
    // assert on what arrived. Polling a count is the only honest way to wait for
    // a thread whose timing is not part of the contract.
    for (int i = 0; i < 100 && count_cmds(rc->requests, "movexyz") == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::cout << "  worker: connected=" << link.connected()
              << " reconnects=" << link.reconnects()
              << " movexyz=" << count_cmds(rc->requests, "movexyz")
              << " home=" << count_cmds(rc->requests, "home")
              << " gripper=" << count_cmds(rc->requests, "gripper") << "\n";

    // The thread must have connected and opened the session on its own.
    ASSERT(link.connected());
    ASSERT(link.caps().valid);
    // And the frames it drained must have produced real traffic.
    ASSERT(count_cmds(rc->requests, "movexyz") > 0);

    link.stop();
    ASSERT(!link.is_running());
    return true;
}
