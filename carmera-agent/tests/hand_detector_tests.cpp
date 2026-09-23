// Hand-detector tests.
//
// Three layers, deliberately separated so that a machine WITHOUT ONNX Runtime
// still validates the parts that silently produce garbage when wrong:
//
//   1. the 2016-entry palm anchor table   (pure; no model)
//   2. the per-row palm decode            (pure; no model)
//   3. the full palm -> landmark cascade  (ORT + the golden fixture)
//
// The cascade test compares against values produced by the official opencv_zoo
// algorithm running through ONNX Runtime itself (see tests/golden/README.md), so
// a failure isolates *this* port rather than any cv.dnn vs ORT difference.
#include "camera_agent/ai/hand_analyzer.h"
#include "camera_agent/ai/hand_decode.h"
#include "camera_agent/ai/hand_detector.h"
#include "camera_agent/ai/hand_types.h"
#include "test_harness.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "stb_image.h"

namespace {

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

std::string find_hand_fixture() {
    const std::string env = safe_getenv("AI_HAND_FIXTURE");
    if (!env.empty() && std::ifstream(env.c_str()).good()) return env;
    const char* candidates[] = {
        "tests/golden/woman_hands.png",
        "carmera-agent/tests/golden/woman_hands.png",
        "../tests/golden/woman_hands.png",
    };
    for (const char* c : candidates) {
        if (std::ifstream(c).good()) return c;
    }
    return {};
}

bool hand_models_present() {
    return std::ifstream("models/palm_detection_mediapipe_2023feb.onnx").good() &&
           std::ifstream("models/handpose_estimation_mediapipe_2023feb.onnx").good();
}

// ---------------------------------------------------------------------------
// Golden output of the reference implementation on tests/golden/woman_hands.jpg
// (640x960). Provenance + regeneration recipe: tests/golden/README.md.
// ---------------------------------------------------------------------------
constexpr int   kGoldenW = 640;
constexpr int   kGoldenH = 960;
constexpr float kGoldenConf       = 0.9901927709579468f;
// Raw handedness output of the landmark model; the >0.5 rule turns it into
// "right" below.
constexpr float kGoldenHandedness = 0.8830371499061584f;

// [x1, y1, x2, y2] in ORIGINAL image pixels.
constexpr float kGoldenBox[4] = {237.3376650181921f, 283.2167763928976f,
                                 525.2307964228695f, 462.627224916215f};

// 21 x (x, y, z) in ORIGINAL image pixels. z is wrist-relative (see below).
constexpr float kGoldenLm[21][3] = {
    {294.0438878706285f,  380.50522756034485f,   1.8321100136680407e-06f},
    {327.34817453686236f, 416.3201058545175f,   -8.227321888719286f},
    {366.1242699223126f,  438.1142885266247f,   -9.273765070097786f},
    {404.380724365266f,   438.1621637539444f,   -9.853621670178004f},
    {428.50744689600754f, 421.00829132568674f, -10.114352677549634f},
    {397.7803490355188f,  412.2879642219478f,    6.001199867044176f},
    {430.6375369752749f,  412.4502889177531f,    5.208628943988255f},
    {450.3629063134026f,  409.1127984023387f,    3.122297840459006f},
    {465.4654654318609f,  404.22732301478396f,   1.4201419268335613f},
    {395.84612766622354f, 387.49709861925174f,   7.408834627696446f},
    {430.20242823675846f, 384.5034279151322f,    7.153137803077698f},
    {451.50995169827013f, 378.46513444695927f,   2.3296793081930702f},
    {468.524573570433f,   373.39958119235746f,  -1.3760199780975069f},
    {386.23484572294694f, 364.3189482198938f,    6.908502033778599f},
    {416.6483774353141f,  358.2863981492658f,    3.91940176486969f},
    {436.1572802062084f,  352.75923797992044f,  -2.333226421049663f},
    {448.338493965223f,   350.9614021681809f,   -6.2397096157073975f},
    {372.4089035736884f,  341.9439049702979f,    5.366094214575631f},
    {396.65691263415835f, 332.35879419783674f,   2.258617347904614f},
    {412.4647933623651f,  329.4285585882975f,   -1.7222510810409273f},
    {423.6675167784979f,  330.12075967924983f,  -4.390829673835209f},
};

// With the fixture in a LOSSLESS format both sides decode byte-identical pixels
// (a JPEG would not: stb_image and libjpeg-turbo disagree by up to 3 LSB on ~5%
// of pixels, which alone moves landmarks by ~0.4 px and hides real bugs). What
// remains is only this port's own resamplers vs OpenCV's:
//
//   measured on this fixture:  dx 0.00034 px  dy 0.00079 px  dz 0.00173 px
//   measured hand box:         max 0.00065 px
//
// The tolerance is two orders of magnitude looser than that so a different ONNX
// Runtime build or optimisation level cannot make the test flaky, while still
// being far below anything a wrong transform could hide behind (a wrong
// transform is off by whole pixels, not by 0.00x).
constexpr float kLmTolerancePx  = 0.02f;
constexpr float kBoxTolerancePx = 0.03f;

// The model geometry is a compile-time constant, so it is asserted at compile
// time: a wrong anchor count would otherwise only show up as garbage boxes at
// runtime, and asserting constants at runtime trips MSVC's C4127 under /W4.
static_assert(ca::kPalmInputSize == 192, "palm model input is 192x192");
static_assert(ca::kPalmAnchorCount == 2016, "palm model emits 2016 rows");
static_assert(ca::kPalmRegChannels == 18, "4 box + 2*7 palm keypoint regressors");
static_assert(ca::kPalmKeypointCount == 7, "MediaPipe palm has 7 keypoints");
// The anchor table is two blocks: 24x24 cells x 2 anchors, then 12x12 x 6.
static_assert(24 * 24 * 2 == 1152, "first anchor block size");
static_assert(12 * 12 * 6 == 864, "second anchor block size");
static_assert(1152 + 864 == ca::kPalmAnchorCount, "block sizes must sum to 2016");

// The fixture contains TWO hands; `max_hands = 1` keeps the highest-scoring
// palm, which the reference ranks first as well (anchor 456, score 0.845936,
// palm box [297.25, 343.87, 395.01, 441.61]). Everything below therefore
// compares against that palm's hand - a mismatch here would show up as a wildly
// different confidence long before it showed up in the coordinates.
static_assert(kGoldenHandedness > 0.5f, "the golden hand is a right hand");

}  // namespace

// ---------------------------------------------------------------------------
// 1. Anchor table
// ---------------------------------------------------------------------------
TEST(hand_palm_anchor_table) {
    ASSERT_EQ(ca::palm_anchor_count(), 2016);

    std::vector<float> a;
    ca::palm_build_anchors(a);
    ASSERT_EQ(a.size(), 2016u * 2u);

    // Spot values read out of the official opencv_zoo `_load_anchors()` literal
    // (checked element by element at 1e-6; the 3e-8 residual is float32 literal
    // rounding). A shifted or transposed table moves EVERY detected box while
    // still looking plausible, which is why these are pinned here.
    const struct { int i; float cx, cy; } expect[] = {
        {0,    0.0208333302f, 0.0208333302f},   // stride 8,  cell (0,0),  anchor 0
        {575,  0.979166687f,  0.479166657f},    // stride 8,  cell (23,11), anchor 0
        {1151, 0.979166687f,  0.979166687f},    // last stride 8 anchor
        {1152, 0.0416666716f, 0.0416666716f},   // first stride 16 anchor
        {1158, 0.125f,        0.0416666716f},   // stride 16, cell (1,0)
        {2015, 0.958333313f,  0.958333313f},    // last stride 16 anchor
    };
    for (const auto& e : expect) {
        ASSERT(std::fabs(a[static_cast<size_t>(e.i) * 2u] - e.cx) < 1e-6f);
        ASSERT(std::fabs(a[static_cast<size_t>(e.i) * 2u + 1u] - e.cy) < 1e-6f);
    }

    // Block boundary: both blocks start at the centre of their first cell and
    // the layer change sits exactly at index 1152 (see the static_asserts above).

    // Every centre must sit strictly inside the unit square.
    for (size_t i = 0; i < a.size(); i += 2) {
        ASSERT(a[i] > 0.0f && a[i] < 1.0f);
        ASSERT(a[i + 1] > 0.0f && a[i + 1] < 1.0f);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 1b. Anchor table, ALL 2016 rows, against the verbatim official transcription
// ---------------------------------------------------------------------------
TEST(hand_palm_anchor_table_matches_official) {
    // tests/golden/palm_anchors_reference.txt is transcribed from the official
    // opencv_zoo `_load_anchors()` literal - it is NOT generated by the formula
    // under test, which is what makes this comparison meaningful.
    const char* candidates[] = {
        "tests/golden/palm_anchors_reference.txt",
        "carmera-agent/tests/golden/palm_anchors_reference.txt",
        "../tests/golden/palm_anchors_reference.txt",
    };
    std::string path;
    for (const char* c : candidates) {
        if (std::ifstream(c).good()) { path = c; break; }
    }
    if (path.empty()) {
        std::cout << "  [skip] palm_anchors_reference.txt not found\n";
        return true;
    }

    std::vector<float> a;
    ca::palm_build_anchors(a);
    ASSERT_EQ(a.size(), 2016u * 2u);

    std::ifstream in(path.c_str());
    ASSERT(in.good());
    std::string line;
    int i = 0;
    float worst = 0.0f;
    int worst_i = -1;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        float cx = 0.0f, cy = 0.0f;
        if (!(is >> cx >> cy)) continue;
        if (i >= ca::kPalmAnchorCount) break;
        const float dx = std::fabs(a[static_cast<size_t>(i) * 2u] - cx);
        const float dy = std::fabs(a[static_cast<size_t>(i) * 2u + 1u] - cy);
        const float d = std::max(dx, dy);
        if (d > worst) { worst = d; worst_i = i; }
        ++i;
    }
    ASSERT(i == ca::kPalmAnchorCount);
    std::cout << "  anchors compared: " << i << ", worst diff " << worst
              << " at index " << worst_i << "\n";
    // The residual is float32 literal rounding (~3e-8 observed); 1e-6 is the
    // documented tolerance and is orders of magnitude smaller than a grid or
    // ordering mistake would produce.
    ASSERT(worst < 1e-6f);
    return true;
}

// ---------------------------------------------------------------------------
// 2. Per-row decode
// ---------------------------------------------------------------------------
TEST(hand_palm_decode_row) {
    std::vector<float> anchors;
    ca::palm_build_anchors(anchors);

    // Anchor 1152 is the centre of the first stride-16 cell: (1/24, 1/24).
    const int idx = 1152;
    const float expect_x = (1.0f / 24.0f) * 192.0f;   // 8 px at scale 192
    ASSERT(std::fabs(expect_x - 8.0f) < 1e-4f);

    std::vector<float> reg(18, 0.0f);
    ca::PalmBox p = ca::palm_decode_row(reg.data(), anchors, idx, 192.0f, 0.0f, 0.0f);
    // Zero deltas -> a degenerate box exactly on the anchor centre.
    ASSERT(std::fabs(p.x1 - 8.0f) < 1e-4f);
    ASSERT(std::fabs(p.x2 - 8.0f) < 1e-4f);
    ASSERT(std::fabs(p.y1 - 8.0f) < 1e-4f);
    ASSERT(std::fabs(p.y2 - 8.0f) < 1e-4f);
    for (int j = 0; j < 7; ++j) {
        ASSERT(std::fabs(p.lm[j][0] - 8.0f) < 1e-4f);
        ASSERT(std::fabs(p.lm[j][1] - 8.0f) < 1e-4f);
    }

    // w = h = 96/192 = 0.5 -> +/-0.25 in anchor space -> +/-48 px at scale 192.
    reg[2] = 96.0f;
    reg[3] = 96.0f;
    reg[4] = 0.0f;    // palm keypoint 0 x delta
    reg[5] = 0.0f;    // palm keypoint 0 y delta
    reg[6] = 48.0f;   // palm keypoint 1 x delta -> +0.25 anchor units -> 56 px
    reg[7] = 0.0f;    // palm keypoint 1 y delta
    p = ca::palm_decode_row(reg.data(), anchors, idx, 192.0f, 0.0f, 0.0f);
    ASSERT(std::fabs(p.x1 - (8.0f - 48.0f)) < 1e-3f);
    ASSERT(std::fabs(p.x2 - (8.0f + 48.0f)) < 1e-3f);
    ASSERT(std::fabs(p.y1 - (8.0f - 48.0f)) < 1e-3f);
    ASSERT(std::fabs(p.y2 - (8.0f + 48.0f)) < 1e-3f);
    ASSERT(std::fabs(p.lm[0][0] - 8.0f) < 1e-3f);
    ASSERT(std::fabs(p.lm[1][0] - 56.0f) < 1e-3f);
    ASSERT(std::fabs(p.lm[1][1] - 8.0f) < 1e-3f);

    // The letterbox padding is subtracted in ORIGINAL pixels, after the scale.
    p = ca::palm_decode_row(reg.data(), anchors, idx, 192.0f, 10.0f, 20.0f);
    ASSERT(std::fabs(p.x1 - (8.0f - 48.0f - 10.0f)) < 1e-3f);
    ASSERT(std::fabs(p.y1 - (8.0f - 48.0f - 20.0f)) < 1e-3f);
    ASSERT(std::fabs(p.x2 - (8.0f + 48.0f - 10.0f)) < 1e-3f);
    ASSERT(std::fabs(p.y2 - (8.0f + 48.0f - 20.0f)) < 1e-3f);

    // Out-of-range inputs must return the default box instead of reading OOB.
    const ca::PalmBox bad = ca::palm_decode_row(reg.data(), anchors, 99999, 192.0f, 0.0f, 0.0f);
    ASSERT(bad.x1 == 0.0f && bad.x2 == 0.0f);
    const ca::PalmBox null_reg = ca::palm_decode_row(nullptr, anchors, idx, 192.0f, 0.0f, 0.0f);
    ASSERT(null_reg.x1 == 0.0f && null_reg.x2 == 0.0f);
    return true;
}

TEST(hand_palm_nms_suppresses_overlaps) {
    // Two nearly identical boxes plus one far away; greedy NMS keeps the
    // higher-scoring duplicate and the distant box.
    std::vector<ca::PalmBox> boxes(3);
    boxes[0].x1 = 0.0f;   boxes[0].y1 = 0.0f;   boxes[0].x2 = 100.0f; boxes[0].y2 = 100.0f;
    boxes[1].x1 = 5.0f;   boxes[1].y1 = 5.0f;   boxes[1].x2 = 105.0f; boxes[1].y2 = 105.0f;
    boxes[2].x1 = 300.0f; boxes[2].y1 = 300.0f; boxes[2].x2 = 400.0f; boxes[2].y2 = 400.0f;
    const std::vector<float> scores = {0.9f, 0.8f, 0.7f};

    std::vector<int> keep;
    ca::palm_nms(keep, boxes, scores, 0.5f, 0.3f, 5000);
    ASSERT_EQ(keep.size(), 2u);
    ASSERT_EQ(keep[0], 0);   // best first
    ASSERT_EQ(keep[1], 2);

    // The score gate runs before NMS.
    std::vector<int> few;
    ca::palm_nms(few, boxes, scores, 0.85f, 0.3f, 5000);
    ASSERT_EQ(few.size(), 1u);
    ASSERT_EQ(few[0], 0);
    return true;
}

// ---------------------------------------------------------------------------
// 3. Full cascade against the golden fixture
// ---------------------------------------------------------------------------
TEST(hand_cascade_golden_woman_hands) {
    auto det = ca::create_hand_detector();
    if (std::string(det->backend_name()) == "none") {
        std::cout << "  [skip] ONNX Runtime not built in\n";
        return true;
    }
    if (!hand_models_present()) {
        std::cout << "  [skip] hand models missing from models/ "
                     "(see carmera-agent/models/README.md)\n";
        return true;
    }
    const std::string path = find_hand_fixture();
    if (path.empty()) {
        std::cout << "  [skip] woman_hands.png not found "
                     "(set AI_HAND_FIXTURE=<absolute path>)\n";
        return true;
    }

    int w = 0, h = 0, n = 0;
    uint8_t* rgb = stbi_load(path.c_str(), &w, &h, &n, 3);
    ASSERT(rgb != nullptr);
    ASSERT_EQ(w, kGoldenW);
    ASSERT_EQ(h, kGoldenH);

    ca::HandConfig cfg;   // defaults == the constants the golden was built with
    ASSERT(det->init(cfg));
    ASSERT_EQ(det->keypoint_count(), 21);

    ca::HandDetection out;
    const bool ok = det->detect_hand(rgb, w, h, out);
    stbi_image_free(rgb);
    ASSERT(ok);
    ASSERT(out.detected);
    ASSERT_EQ(out.landmarks.size(), 21u);

    // --- confidence + handedness -------------------------------------------
    std::cout << "  woman_hands.png conf=" << out.confidence
              << " handedness=" << ca::to_string(out.handedness) << "\n";
    ASSERT(std::fabs(out.confidence - kGoldenConf) < 0.02f);
    ASSERT_EQ(out.handedness, ca::Handedness::Right);

    // --- 21 landmarks, sub-pixel -------------------------------------------
    float max_dx = 0.0f, max_dy = 0.0f, max_dz = 0.0f;
    for (size_t i = 0; i < 21; ++i) {
        max_dx = std::max(max_dx, std::fabs(out.landmarks[i].x - kGoldenLm[i][0]));
        max_dy = std::max(max_dy, std::fabs(out.landmarks[i].y - kGoldenLm[i][1]));
        max_dz = std::max(max_dz, std::fabs(out.landmarks[i].z - kGoldenLm[i][2]));
    }
    std::cout << "  landmark diff vs reference: dx=" << max_dx
              << " dy=" << max_dy << " dz=" << max_dz << " px (tol "
              << kLmTolerancePx << ")\n";
    ASSERT(max_dx < kLmTolerancePx);
    ASSERT(max_dy < kLmTolerancePx);
    ASSERT(max_dz < kLmTolerancePx);

    // Landmarks must stay inside the frame (this is the letterbox inverse
    // transform working, the most fragile step in the whole cascade).
    for (const auto& k : out.landmarks) {
        ASSERT(k.x >= 0.0f && k.x <= static_cast<float>(w));
        ASSERT(k.y >= 0.0f && k.y <= static_cast<float>(h));
        ASSERT(k.conf > 0.0f && k.conf <= 1.0f);
    }

    // --- the "z is wrist-relative" fact, measured not assumed (spec 15/16) ---
    // The wrist defines the model's depth origin, so its z is ~0 no matter how
    // far the hand is from the camera. This is exactly why depth_source defaults
    // to "size" rather than "landmark_z".
    ASSERT(std::fabs(out.landmarks[0].z) < 0.5f);

    // --- derived hand box ---------------------------------------------------
    const float box_diff[4] = {
        std::fabs(out.x1 - kGoldenBox[0]), std::fabs(out.y1 - kGoldenBox[1]),
        std::fabs(out.x2 - kGoldenBox[2]), std::fabs(out.y2 - kGoldenBox[3])};
    std::cout << "  box diff vs reference: " << box_diff[0] << ", "
              << box_diff[1] << ", " << box_diff[2] << ", " << box_diff[3] << " px\n";
    for (float d : box_diff) ASSERT(d < kBoxTolerancePx);
    ASSERT(out.x2 > out.x1);
    ASSERT(out.y2 > out.y1);

    // --- the gesture classifier on a REAL open hand (spec 22) ---------------
    // Reports the four finger ratios so the threshold band in hand_analyzer.cpp
    // stays traceable to measured data.
    const ca::GestureResult g = ca::hand_classify_gesture(out.landmarks);
    std::cout << "  gesture on the reference hand: " << ca::to_string(g.g)
              << " (conf " << g.confidence << ")\n";
    ASSERT_EQ(g.g, ca::Gesture::OpenPalm);
    return true;
}
