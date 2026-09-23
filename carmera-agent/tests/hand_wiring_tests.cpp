// Wiring tests for the hand mode: the mode vocabulary and the additive
// "hand" metadata block (Phase 1, spec 7 / 8 / 10 / 17).
//
// Two things are being protected here and neither needs a model:
//
//   1. The mode tokens have ONE mapping, shared by the YAML loader, the CLI and
//      the aimode poller. A drifting spelling would silently stop a switch.
//   2. Detect and pose frames keep the EXACT byte layout they had before the
//      hand block existed. The pinned strings below are the contract: if a
//      future edit to the shared encoder shifts a byte, these fail. That is why
//      AIFrameResult::hand is an optional engaged only by the hand mode rather
//      than a struct that always prints.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "camera_agent/ai/ai_pipeline.h"
#include "camera_agent/ai/ai_types.h"
#include "camera_agent/config.h"
#include "camera_agent/metadata/metadata_encoder.h"
#include "test_harness.h"

namespace {

bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

// Only the model-gated cases need this, so it is unused in a build without ONNX
// Runtime - and an unused static function is a /W4 warning (C4505).
[[maybe_unused]] bool file_exists(const char* p) {
    std::ifstream f(p);
    return f.good();
}

ca::AIFrameResult make_result(uint64_t frame_id, uint64_t ts, int w, int h) {
    ca::AIFrameResult r;
    r.frame_id     = frame_id;
    r.timestamp    = ts;
    r.video_width  = w;
    r.video_height = h;
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Mode vocabulary
// ---------------------------------------------------------------------------

// One parser for three call sites (YAML, CLI, aimode poller). "ai-off" is NOT a
// mode and must not parse: it means "the web stops drawing, the agent keeps its
// model", so turning it into a switch would load a model nobody asked for.
TEST(aimode_token_mapping) {
    ca::AIMode m = ca::AIMode::Detect;

    ASSERT(ca::aimode_from_string("detect", m) && m == ca::AIMode::Detect);
    ASSERT(ca::aimode_from_string("ai-y", m) && m == ca::AIMode::Detect);
    ASSERT(ca::aimode_from_string("pose", m) && m == ca::AIMode::Pose);
    ASSERT(ca::aimode_from_string("ai-y-pose", m) && m == ca::AIMode::Pose);
    ASSERT(ca::aimode_from_string("hand", m) && m == ca::AIMode::Hand);
    ASSERT(ca::aimode_from_string("ai-hand", m) && m == ca::AIMode::Hand);

    // Everything else leaves the current mode alone.
    ASSERT(!ca::aimode_from_string("ai-off", m));
    ASSERT(!ca::aimode_from_string("off", m));
    ASSERT(!ca::aimode_from_string("", m));
    ASSERT(!ca::aimode_from_string("HAND", m));      // case sensitive on purpose
    ASSERT(!ca::aimode_from_string("ai-hand-2", m));
    return true;
}

// The wire vocabulary is what the web compares a frame's ai_mode against, so
// each mode needs its own token - two modes sharing one would make the overlay
// draw hand keypoints as a body skeleton.
TEST(aimode_web_strings_are_distinct) {
    const std::string d = ca::aimode_web_string(ca::AIMode::Detect);
    const std::string p = ca::aimode_web_string(ca::AIMode::Pose);
    const std::string h = ca::aimode_web_string(ca::AIMode::Hand);
    ASSERT_EQ(d, std::string("ai-y"));
    ASSERT_EQ(p, std::string("ai-y-pose"));
    ASSERT_EQ(h, std::string("ai-hand"));
    // Round trip: what the agent stamps is what the poller can ask for.
    ca::AIMode back = ca::AIMode::Detect;
    ASSERT(ca::aimode_from_string(h, back) && back == ca::AIMode::Hand);
    return true;
}

// ---------------------------------------------------------------------------
// 2. Byte-level regression on the shared encoder
// ---------------------------------------------------------------------------

// A detect frame must serialize EXACTLY as it did before the hand block existed.
TEST(hand_absent_on_detect_frame) {
    ca::MetadataConfig cfg;
    cfg.camera_id = "c1";

    ca::AIFrameResult r = make_result(9, 1000, 640, 480);
    r.ai_mode = "ai-y";
    ca::AIObject o;
    o.class_name = "person";
    o.confidence = 0.5f;
    o.track_id   = 3;
    o.x1 = 10; o.y1 = 20; o.x2 = 110; o.y2 = 220;
    r.objects.push_back(o);

    const std::string want =
        "{\"version\":1,\"type\":\"frame\",\"camera_id\":\"c1\",\"frame_id\":9,"
        "\"timestamp\":1000,\"video_width\":640,\"video_height\":480,"
        "\"ai_mode\":\"ai-y\",\"objects\":[{\"class\":\"person\","
        "\"confidence\":0.50,\"track_id\":3,\"bbox\":[10,20,110,220]}]}";
    const std::string got = ca::encode_frame_metadata(r, cfg);
    if (got != want) {
        std::cerr << "  got : " << got << "\n  want: " << want << "\n";
    }
    ASSERT_EQ(got, want);
    ASSERT(!contains(got, "hand"));
    return true;
}

// Same for a pose frame, keypoints included: the per-object keypoint triple is
// still [x,y,conf] and still the LAST field of the object.
TEST(hand_absent_on_pose_frame) {
    ca::MetadataConfig cfg;
    cfg.camera_id = "c1";

    ca::AIFrameResult r = make_result(9, 1000, 640, 480);
    r.ai_mode = "ai-y-pose";
    ca::AIObject o;
    o.class_name = "person";
    o.confidence = 0.5f;
    o.track_id   = 3;
    o.x1 = 10; o.y1 = 20; o.x2 = 110; o.y2 = 220;
    ca::Keypoint k;
    k.x = 110.0f; k.y = 120.0f; k.z = 0.0f; k.conf = 0.9f;
    o.keypoints.push_back(k);
    r.objects.push_back(o);

    const std::string want =
        "{\"version\":1,\"type\":\"frame\",\"camera_id\":\"c1\",\"frame_id\":9,"
        "\"timestamp\":1000,\"video_width\":640,\"video_height\":480,"
        "\"ai_mode\":\"ai-y-pose\",\"objects\":[{\"class\":\"person\","
        "\"confidence\":0.50,\"track_id\":3,\"bbox\":[10,20,110,220],"
        "\"keypoints\":[[110.00,120.00,0.90]]}]}";
    const std::string got = ca::encode_frame_metadata(r, cfg);
    if (got != want) {
        std::cerr << "  got : " << got << "\n  want: " << want << "\n";
    }
    ASSERT_EQ(got, want);
    // A body pose carries no z on the wire, and no hand block.
    ASSERT(!contains(got, "hand"));
    ASSERT(!contains(got, "landmarks"));
    return true;
}

// ---------------------------------------------------------------------------
// 3. The hand block itself
// ---------------------------------------------------------------------------

TEST(hand_block_is_appended_after_objects) {
    ca::MetadataConfig cfg;
    cfg.camera_id = "c1";

    ca::AIFrameResult r = make_result(9, 1000, 640, 480);
    r.ai_mode = "ai-hand";

    ca::AIObject o;
    o.class_name = "hand";
    o.class_id   = -1;
    o.confidence = 0.99f;
    o.track_id   = -1;          // Phase 1 is single-hand: never tracked
    o.x1 = 100; o.y1 = 200; o.x2 = 300; o.y2 = 400;
    ca::Keypoint k0;
    k0.x = 1.5f; k0.y = 2.25f; k0.z = 3.14f; k0.conf = 0.9f;
    ca::Keypoint k1;
    k1.x = 10.0f; k1.y = 20.0f; k1.z = -1.25f; k1.conf = 0.5f;
    o.keypoints.push_back(k0);
    o.keypoints.push_back(k1);
    r.objects.push_back(o);

    ca::HandState h;
    h.detected              = true;
    h.confidence            = 0.88f;
    h.handedness            = ca::Handedness::Right;
    h.reference_initialized = true;
    h.position = ca::Vec3{0.5f, 0.75f, 0.25f};
    h.reference = ca::Vec3{0.45f, 0.7f, 0.2f};
    h.delta     = ca::Vec3{0.05f, 0.05f, 0.05f};
    h.motion.x  = ca::MotionX::Right;
    h.motion.y  = ca::MotionY::Forward;
    h.motion.z  = ca::MotionZ::Up;
    h.gesture            = ca::Gesture::OpenPalm;
    h.gesture_candidate  = ca::Gesture::Fist;
    h.gesture_confidence = 1.0f;
    h.landmarks.push_back(k0);
    r.hand = h;

    // Two things to note about the object half:
    //   * the shared keypoint field rounds x/y to INTEGER pixels (lround) - it
    //     is a drawing payload for a canvas, never a measurement;
    //   * the full-precision 3D numbers live in the hand block below, which is
    //     why the two are not redundant.
    const std::string want =
        "{\"version\":1,\"type\":\"frame\",\"camera_id\":\"c1\",\"frame_id\":9,"
        "\"timestamp\":1000,\"video_width\":640,\"video_height\":480,"
        "\"ai_mode\":\"ai-hand\",\"objects\":[{\"class\":\"hand\","
        "\"confidence\":0.99,\"track_id\":-1,\"bbox\":[100,200,300,400],"
        "\"keypoints\":[[2.00,2.00,0.90],[10.00,20.00,0.50]]}],"
        "\"hand\":{\"detected\":true,\"confidence\":0.88,"
        "\"handedness\":\"right\",\"reference_initialized\":true,"
        "\"position\":[0.5000,0.7500,0.2500],"
        "\"reference\":[0.4500,0.7000,0.2000],"
        "\"delta\":[0.0500,0.0500,0.0500],"
        "\"motion\":{\"x\":\"RIGHT\",\"y\":\"FORWARD\",\"z\":\"UP\"},"
        "\"gesture\":\"OPEN_PALM\",\"gesture_candidate\":\"FIST\","
        "\"gesture_confidence\":1.00,"
        "\"landmarks\":[[1.50,2.25,3.14]]}}";
    const std::string got = ca::encode_frame_metadata(r, cfg);
    if (got != want) {
        std::cerr << "  got : " << got << "\n  want: " << want << "\n";
    }
    ASSERT_EQ(got, want);
    return true;
}

// A hand frame with nothing in it is still a hand frame: the reference state and
// the gesture latch are exactly what the debug panel needs to explain an empty
// frame, so they must be present with detected:false.
TEST(hand_block_present_when_no_hand_found) {
    ca::MetadataConfig cfg;
    ca::AIFrameResult r = make_result(9, 1000, 640, 480);
    r.ai_mode = "ai-hand";
    ca::HandState h;          // default: no hand
    r.hand = h;

    const std::string s = ca::encode_frame_metadata(r, cfg);
    ASSERT(contains(s, "\"objects\":[]"));
    ASSERT(contains(s, "\"hand\":{\"detected\":false,\"confidence\":0.00,"
                       "\"handedness\":\"unknown\",\"reference_initialized\":false"));
    // Logical zero position, all three motion axes STATIONARY, gesture UNKNOWN.
    ASSERT(contains(s, "\"position\":[0.0000,0.0000,0.0000]"));
    ASSERT(contains(s, "\"motion\":{\"x\":\"STATIONARY\",\"y\":\"STATIONARY\","
                       "\"z\":\"STATIONARY\"}"));
    ASSERT(contains(s, "\"gesture\":\"UNKNOWN\""));
    // No landmarks -> no key at all (not an empty array).
    ASSERT(!contains(s, "landmarks"));
    return true;
}

// hand.emit_landmarks=false must shrink the payload without changing its shape:
// the analyzer is the single gate, so the object loses its keypoints and the
// hand block loses its landmarks, and the pipeline just forwards what it gets.
TEST(hand_landmarks_omitted_when_disabled) {
    ca::MetadataConfig cfg;
    ca::AIFrameResult r = make_result(9, 1000, 640, 480);
    r.ai_mode = "ai-hand";

    ca::AIObject o;
    o.class_name = "hand";
    o.confidence = 0.9f;
    o.track_id   = -1;
    o.x1 = 1; o.y1 = 2; o.x2 = 3; o.y2 = 4;
    r.objects.push_back(o);   // no keypoints: analyzer had emit_landmarks off

    ca::HandState h;
    h.detected = true;
    h.confidence = 0.9f;
    r.hand = h;               // no landmarks: same reason

    const std::string s = ca::encode_frame_metadata(r, cfg);
    ASSERT(contains(s, "\"class\":\"hand\""));
    ASSERT(!contains(s, "keypoints"));
    ASSERT(!contains(s, "landmarks"));
    ASSERT(contains(s, "\"hand\":{\"detected\":true"));
    return true;
}

// ---------------------------------------------------------------------------
// 4. Configuration chain (YAML -> AIConfig)
// ---------------------------------------------------------------------------

// Every hand knob must come from the file, not from a default buried in the
// code: the spec 16 direction experiment is run by editing these values, so a
// field that silently ignores its YAML entry would invalidate the experiment.
TEST(hand_config_from_yaml) {
    const char* path = "_hand_cfg_test.yaml";
    {
        std::ofstream f(path);
        f << "ai:\n"
             "  initial_mode: hand\n"
             "  hand:\n"
             "    model_palm: models/my_palm.onnx\n"
             "    model_landmark: models/my_landmark.onnx\n"
             "    palm_score_threshold: 0.7\n"
             "    palm_nms_threshold: 0.4\n"
             "    landmark_confidence: 0.6\n"
             "    max_hands: 2\n"
             "    filter_alpha: 0.25\n"
             "    motion_deadzone: 0.05\n"
             "    reference_stable_frames: 4\n"
             "    lost_frames_to_reset: 7\n"
             "    gesture_confirm_frames: 9\n"
             "    depth_source: landmark_z\n"
             "    invert_x: true\n"
             "    invert_z: true\n"
             "    emit_landmarks: false\n";
    }
    ca::Config cfg = ca::make_default_config();
    const bool ok = ca::load_config(cfg, path);
    std::remove(path);

    ASSERT(ok);
    ASSERT_EQ(cfg.ai.initial_mode, ca::AIMode::Hand);
    ASSERT_EQ(cfg.ai.hand.model_palm, std::string("models/my_palm.onnx"));
    ASSERT_EQ(cfg.ai.hand.model_landmark, std::string("models/my_landmark.onnx"));
    ASSERT_EQ(cfg.ai.hand.palm_score_threshold, 0.7f);
    ASSERT_EQ(cfg.ai.hand.palm_nms_threshold, 0.4f);
    ASSERT_EQ(cfg.ai.hand.landmark_confidence, 0.6f);
    ASSERT_EQ(cfg.ai.hand.max_hands, 2);
    ASSERT_EQ(cfg.ai.hand.filter_alpha, 0.25f);
    ASSERT_EQ(cfg.ai.hand.motion_deadzone, 0.05f);
    ASSERT_EQ(cfg.ai.hand.reference_stable_frames, 4);
    ASSERT_EQ(cfg.ai.hand.lost_frames_to_reset, 7);
    ASSERT_EQ(cfg.ai.hand.gesture_confirm_frames, 9);
    ASSERT_EQ(cfg.ai.hand.depth_source, std::string("landmark_z"));
    ASSERT(cfg.ai.hand.invert_x);
    ASSERT(!cfg.ai.hand.invert_y);   // absent -> stays at the default
    ASSERT(cfg.ai.hand.invert_z);
    ASSERT(!cfg.ai.hand.emit_landmarks);
    return true;
}

// Two closed vocabularies, two ways of getting them wrong. Both must fall back
// to a working default AND say so, rather than accept the typo: a bad
// depth_source would make the mapper take its default branch while the operator
// believes the experiment is measuring the other axis.
TEST(hand_config_rejects_unknown_tokens) {
    const char* path = "_hand_cfg_bad_test.yaml";
    {
        std::ofstream f(path);
        f << "ai:\n"
             "  initial_mode: wibble\n"
             "  hand:\n"
             "    depth_source: gyroscope\n";
    }
    ca::Config cfg = ca::make_default_config();
    const bool ok = ca::load_config(cfg, path);
    std::remove(path);

    ASSERT(ok);
    ASSERT_EQ(cfg.ai.initial_mode, ca::AIMode::Detect);          // default kept
    ASSERT_EQ(cfg.ai.hand.depth_source, std::string("size"));    // default kept
    return true;
}

// ---------------------------------------------------------------------------
// 5. The pipeline actually selects the mode (needs the two ONNX files)
// ---------------------------------------------------------------------------

TEST(ai_pipeline_hand_mode_round_trip) {
#ifdef CAMERA_AGENT_HAVE_ORT
    ca::AIConfig cfg;
    if (!file_exists(cfg.hand.model_palm.c_str()) ||
        !file_exists(cfg.hand.model_landmark.c_str()) ||
        !file_exists("models/yolo11n.onnx")) {
        std::cout << "  [skip] hand / detect ONNX models not present\n";
        return true;
    }

    cfg.enable       = true;
    cfg.fps          = 5;
    cfg.num_threads  = 1;
    cfg.log_objects  = false;
    cfg.initial_mode = ca::AIMode::Hand;

    ca::AIPipeline pipe;
    // Starting straight in hand mode is the path P1e uses on real hardware
    // (no server needed), so it must not merely "work after a switch".
    ASSERT(pipe.init(cfg, 1280, 720, 30));
    ASSERT_EQ(pipe.current_mode(), ca::AIMode::Hand);
    ASSERT_EQ(pipe.keypoint_count(), 21);
    ASSERT_EQ(pipe.current_model_path(), cfg.hand.model_palm);
    ASSERT_EQ(std::string(pipe.active_backend_name()),
              std::string("onnxruntime(mediapipe-hand)"));

    // Hand -> detect must unload the cascade (a stale 21 would make the status
    // heartbeat advertise hand keypoints for a person detector).
    ASSERT(pipe.apply_mode(ca::AIMode::Detect));
    ASSERT_EQ(pipe.current_mode(), ca::AIMode::Detect);
    ASSERT_EQ(pipe.keypoint_count(), 0);
    ASSERT_EQ(pipe.current_model_path(), cfg.model);

    // ... and back, without a pipeline restart.
    ASSERT(pipe.apply_mode(ca::AIMode::Hand));
    ASSERT_EQ(pipe.current_mode(), ca::AIMode::Hand);
    ASSERT_EQ(pipe.keypoint_count(), 21);
    std::cout << "  hand mode round trip detect<->hand OK\n";
    return true;
#else
    std::cout << "  [skip] ONNX Runtime not built in\n";
    return true;
#endif
}

// A hand mode requested when the models are missing must degrade to detection
// rather than kill the AI branch (spec 19) - the video stream is never a
// casualty of a missing model.
TEST(ai_pipeline_hand_mode_falls_back_when_models_missing) {
#ifdef CAMERA_AGENT_HAVE_ORT
    if (!file_exists("models/yolo11n.onnx")) {
        std::cout << "  [skip] models/yolo11n.onnx not present\n";
        return true;
    }

    ca::AIConfig cfg;
    cfg.enable       = true;
    cfg.fps          = 5;
    cfg.num_threads  = 1;
    cfg.log_objects  = false;
    cfg.initial_mode = ca::AIMode::Hand;
    cfg.hand.model_palm     = "models/does-not-exist-palm.onnx";
    cfg.hand.model_landmark = "models/does-not-exist-landmark.onnx";

    ca::AIPipeline pipe;
    ASSERT(pipe.init(cfg, 1280, 720, 30));           // still usable
    ASSERT_EQ(pipe.current_mode(), ca::AIMode::Detect);
    ASSERT_EQ(pipe.keypoint_count(), 0);

    // An explicit request for the broken mode is refused AND the working model
    // stays loaded - a failed switch must never leave the branch modelless.
    ASSERT(!pipe.apply_mode(ca::AIMode::Hand));
    ASSERT_EQ(pipe.current_mode(), ca::AIMode::Detect);
    ASSERT_EQ(pipe.keypoint_count(), 0);
    return true;
#else
    std::cout << "  [skip] ONNX Runtime not built in\n";
    return true;
#endif
}
