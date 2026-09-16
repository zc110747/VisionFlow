#include <chrono>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "camera_agent/config.h"
#include "camera_agent/metadata/metadata_encoder.h"
#include "camera_agent/metadata/metadata_manager.h"
#include "test_harness.h"

namespace {

ca::AIFrameResult make_result(uint64_t frame_id, uint64_t ts, int w, int h) {
    ca::AIFrameResult r;
    r.frame_id     = frame_id;
    r.timestamp    = ts;
    r.video_width  = w;
    r.video_height = h;
    return r;
}

bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

// Test double: records every payload instead of putting it on the wire, so the
// delivery path (queue -> sender thread -> transport) can be checked with real
// object data and without a server.
class RecordingTransport : public ca::IMetadataTransport {
public:
    const char* name() const override { return "test-recorder"; }
    bool connect() override { up = true; return true; }
    void close()   override { up = false; }
    bool connected() const override { return up; }
    bool send(const std::string& payload, int, double* latency) override {
        std::lock_guard<std::mutex> lk(m);
        sent.push_back(payload);
        if (latency) *latency = 1.5;
        return true;
    }
    std::vector<std::string> snapshot() const {
        std::lock_guard<std::mutex> lk(m);
        return sent;
    }

    bool up = false;
    mutable std::mutex m;
    std::vector<std::string> sent;
};

// Test double that mimics WinHTTP's LAZY connect: connect() always succeeds
// (even against a dead server) and only the round trip reveals the outage.
// This is the exact shape that used to hide the reconnect counter behind
// transport->connected(), so the recovery path must be driven by send().
class FlakyTransport : public ca::IMetadataTransport {
public:
    const char* name() const override { return "test-flaky"; }
    bool connect() override { up = true; return true; }   // never refuses
    void close()   override { up = false; }
    bool connected() const override { return up; }
    bool send(const std::string&, int, double* latency) override {
        std::lock_guard<std::mutex> lk(m);
        ++attempts;
        if (latency) *latency = 1.0;
        if (fails_left > 0) { --fails_left; up = false; return false; }
        ++ok_count;
        return true;
    }

    int  fails_left = 3;
    int  attempts   = 0;
    int  ok_count   = 0;
    bool up         = false;
    mutable std::mutex m;
};

} // namespace

// spec 4 / spec 5: exact field names, bbox clamped into the video frame.
TEST(metadata_encode_frame) {
    ca::MetadataConfig cfg;
    cfg.camera_id = "camera_001";

    ca::AIFrameResult r = make_result(15230, 1756773210123ULL, 1920, 1080);

    ca::AIObject a;
    a.class_name = "person";
    a.confidence = 0.934f;
    a.track_id   = 17;
    a.x1 = 812; a.y1 = 210; a.x2 = 1040; a.y2 = 850;
    r.objects.push_back(a);

    // Out-of-range box: must be clamped to the frame (spec 5).
    ca::AIObject b;
    b.confidence = 0.5f;
    b.track_id   = 18;
    b.x1 = -50; b.y1 = -30; b.x2 = 99999; b.y2 = 99999;
    r.objects.push_back(b);

    const std::string s = ca::encode_frame_metadata(r, cfg);
    ASSERT(contains(s, "\"version\":1"));
    // spec 14: the frame discriminator. It was missing once, which made every
    // frame message a protocol violation for a spec-following receiver.
    ASSERT(contains(s, "\"type\":\"frame\""));
    ASSERT(contains(s, "\"camera_id\":\"camera_001\""));
    ASSERT(contains(s, "\"frame_id\":15230"));
    ASSERT(contains(s, "\"timestamp\":1756773210123"));
    ASSERT(contains(s, "\"video_width\":1920"));
    ASSERT(contains(s, "\"video_height\":1080"));
    ASSERT(contains(s, "\"class\":\"person\""));
    ASSERT(contains(s, "\"confidence\":0.93"));
    ASSERT(contains(s, "\"track_id\":17"));
    ASSERT(contains(s, "\"bbox\":[812,210,1040,850]"));
    ASSERT(contains(s, "\"bbox\":[0,0,1920,1080]"));   // clamped
    return true;
}

// spec 12: an empty result is still a valid message (server-side AI liveness).
TEST(metadata_encode_empty_frame) {
    ca::MetadataConfig cfg;
    const std::string s = ca::encode_frame_metadata(make_result(15236, 1756773210323ULL, 1280, 720), cfg);
    ASSERT(contains(s, "\"type\":\"frame\""));
    ASSERT(contains(s, "\"objects\":[]"));
    ASSERT(contains(s, "\"frame_id\":15236"));
    ASSERT(contains(s, "\"video_width\":1280"));
    return true;
}

// The per-frame AI mode must be stamped into the wire JSON so the web can drop
// transition frames during a runtime detector swap. Older agents omit it; the
// server then treats the (empty) mode as "does not match" and the web drops it.
TEST(metadata_encode_frame_includes_ai_mode) {
    ca::MetadataConfig cfg;
    ca::AIFrameResult r = make_result(15237, 1756773210423ULL, 1280, 720);
    r.ai_mode = "ai-y-pose";   // stamped by AIPipeline::process() per frame
    ca::AIObject a;
    a.class_name = "person";
    a.confidence = 0.9f;
    a.track_id   = 5;
    a.x1 = 10; a.y1 = 20; a.x2 = 30; a.y2 = 40;
    r.objects.push_back(a);

    const std::string s = ca::encode_frame_metadata(r, cfg);
    ASSERT(contains(s, "\"ai_mode\":\"ai-y-pose\""));
    ASSERT(contains(s, "\"type\":\"frame\""));
    return true;
}

// An empty ai_mode must NOT emit the field (backward compatible: older agents
// that do not stamp it leave the field absent rather than sending "").
TEST(metadata_encode_frame_omits_empty_ai_mode) {
    ca::MetadataConfig cfg;
    ca::AIFrameResult r = make_result(15238, 1756773210523ULL, 1280, 720);
    r.ai_mode = "";   // not stamped
    const std::string s = ca::encode_frame_metadata(r, cfg);
    ASSERT(!contains(s, "ai_mode"));
    return true;
}

// spec 13: status/heartbeat carries the AI health fields.
TEST(metadata_encode_status) {
    ca::MetadataConfig cfg;
    cfg.camera_id = "camera_001";
    ca::AIStatusInfo st;
    st.enable         = true;
    st.running        = true;
    st.fps            = 5.0;
    st.model          = "models/yolov8n.onnx";
    st.last_frame_id  = 15230;
    st.last_timestamp = 1756773210123ULL;
    st.processed      = 99;
    const std::string s = ca::encode_status_metadata(st, cfg);
    ASSERT(contains(s, "\"type\":\"status\""));
    ASSERT(contains(s, "\"enable\":true"));
    ASSERT(contains(s, "\"fps\":5.00"));
    ASSERT(contains(s, "\"model\":\"models/yolov8n.onnx\""));
    ASSERT(contains(s, "\"tracker\":\"bytetrack\""));
    ASSERT(contains(s, "\"last_frame_id\":15230"));
    ASSERT(contains(s, "\"processed\":99"));
    return true;
}

// spec 7 + spec 10: with the server unreachable the queue stays bounded, pushes
// never block, and stop() still joins cleanly. No exception may escape.
TEST(metadata_queue_bounded_when_server_down) {
    ca::MetadataConfig cfg;
    cfg.enable              = true;
    // Port 1 is closed: connect() succeeds at the WinHTTP level but every send
    // fails, which is exactly the "server down" path we want to exercise.
    cfg.server_url          = "http://127.0.0.1:1/api/metadata";
    cfg.queue_size          = 3;
    cfg.timeout_ms          = 100;
    cfg.retry_interval_ms   = 50;
    cfg.retry_max_interval_ms = 100;
    cfg.heartbeat_interval_sec = 1;

    ca::MetadataManager m;
    ASSERT(m.init(cfg));
    m.start();
    ASSERT(m.is_running());

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 30; ++i)
        m.push_result(make_result(1000 + i, 100 + i, 1280, 720));
    const double push_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();

    // push_result() must not perform any network work.
    ASSERT(push_ms < 100.0);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const auto s = m.stats();
    // Bounded: never more than queue_size messages pending.
    ASSERT(s.queue_size <= 3);
    // Something was discarded or failed - proof the sender is not stuck.
    ASSERT(s.dropped + s.failed > 0);

    m.stop();
    ASSERT(!m.is_running());
    return true;
}

// A manager that was never started must ignore results silently (metadata off,
// or init failed): no crash, no counters.
TEST(metadata_not_started_is_noop) {
    ca::MetadataManager m;
    ASSERT(!m.is_running());
    m.push_result(make_result(1, 2, 640, 480));
    m.stop();
    const auto s = m.stats();
    ASSERT_EQ(s.sent, 0u);
    ASSERT_EQ(s.failed, 0u);
    ASSERT_EQ(s.dropped, 0u);
    return true;
}

// spec 9: every server-side knob is configuration, nothing is hard-coded.
TEST(metadata_config_from_yaml) {
    const char* path = "_metadata_cfg_test.yaml";
    {
        std::ofstream f(path);
        f << "metadata:\n"
          << "  enable: true\n"
          << "  server_url: http://192.168.1.9:9000/ingest\n"
          << "  camera_id: cam_007\n"
          << "  version: 2\n"
          << "  queue_size: 12\n"
          << "  timeout_ms: 1500\n"
          << "  retry_interval_ms: 700\n"
          << "  retry_max_interval_ms: 20000\n"
          << "  heartbeat_interval_sec: 25\n"
          << "  log_payload: true\n";
    }
    ca::Config cfg = ca::make_default_config();
    const bool ok = ca::load_config(cfg, path);
    std::remove(path);
    ASSERT(ok);
    ASSERT(cfg.metadata.enable);
    ASSERT_EQ(cfg.metadata.server_url, std::string("http://192.168.1.9:9000/ingest"));
    ASSERT_EQ(cfg.metadata.camera_id, std::string("cam_007"));
    ASSERT_EQ(cfg.metadata.version, 2);
    ASSERT_EQ(cfg.metadata.queue_size, 12);
    ASSERT_EQ(cfg.metadata.timeout_ms, 1500);
    ASSERT_EQ(cfg.metadata.retry_interval_ms, 700);
    ASSERT_EQ(cfg.metadata.retry_max_interval_ms, 20000);
    ASSERT_EQ(cfg.metadata.heartbeat_interval_sec, 25);
    ASSERT(cfg.metadata.log_payload);
    return true;
}

// End-to-end through the manager (queue -> sender thread -> transport) with real
// object data: the server must receive track ids and bboxes verbatim.
TEST(metadata_delivers_objects) {
    ca::MetadataConfig cfg;
    cfg.enable                = true;
    cfg.heartbeat_interval_sec = 0;   // frames only
    cfg.queue_size            = 8;
    cfg.timeout_ms            = 100;

    std::unique_ptr<RecordingTransport> rec(new RecordingTransport());
    RecordingTransport* raw = rec.get();

    ca::MetadataManager m;
    ASSERT(m.init(cfg));
    m.set_transport(std::move(rec));
    m.start();

    for (int i = 0; i < 3; ++i) {
        ca::AIFrameResult r = make_result(500 + i, 9000 + i, 1280, 720);
        ca::AIObject o;
        o.track_id   = 7 + i;
        o.confidence = 0.87f;
        o.x1 = 100; o.y1 = 120; o.x2 = 300; o.y2 = 480;
        r.objects.push_back(o);
        m.push_result(r);
    }

    for (int i = 0; i < 100; ++i) {
        if (m.stats().sent >= 3) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    m.stop();

    const auto s = m.stats();
    ASSERT_EQ(s.sent, 3u);
    ASSERT_EQ(s.failed, 0u);
    ASSERT_EQ(s.dropped, 0u);

    const std::vector<std::string> v = raw->snapshot();
    ASSERT_EQ(v.size(), std::size_t(3));
    ASSERT(contains(v[0], "\"class\":\"person\""));
    ASSERT(contains(v[0], "\"track_id\":7"));
    ASSERT(contains(v[0], "\"bbox\":[100,120,300,480]"));
    // spec 15: frame_id is the camera frame counter, copied unchanged.
    ASSERT(contains(v[0], "\"frame_id\":500"));
    ASSERT(contains(v[2], "\"frame_id\":502"));
    return true;
}

// spec 10 / spec 18: the reconnect counter must be driven by the RESULT of the
// round trip, not by connect(). With a lazy transport connect() never fails, so
// a manager that trusts connected() would silently report reconnect=0 forever.
TEST(metadata_reconnect_counted_on_recovery) {
    ca::MetadataConfig cfg;
    cfg.enable                  = true;
    cfg.heartbeat_interval_sec  = 0;      // frames only
    cfg.queue_size              = 8;
    cfg.timeout_ms              = 100;
    cfg.retry_interval_ms       = 30;
    cfg.retry_max_interval_ms   = 60;

    std::unique_ptr<FlakyTransport> t(new FlakyTransport());
    FlakyTransport* raw = t.get();
    raw->fails_left = 3;                  // 3 failed round trips, then recovery

    ca::MetadataManager m;
    ASSERT(m.init(cfg));
    m.set_transport(std::move(t));
    m.start();

    for (int i = 0; i < 300; ++i) {
        m.push_result(make_result(700 + i, 7000 + i, 1280, 720));
        const auto s = m.stats();
        if (s.reconnect >= 1 && s.sent >= 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    m.stop();

    const auto s = m.stats();
    ASSERT_EQ(s.reconnect, 1u);           // exactly one restore, not one per msg
    ASSERT_EQ(s.failed, 3u);              // the three injected failures
    ASSERT(s.sent >= 1u);                 // delivery resumed
    ASSERT(raw->attempts >= 4);
    // spec 7: the queue never grew without bound during the outage.
    ASSERT(s.queue_size <= 8);
    ASSERT(s.dropped + s.failed > 0);
    return true;
}

// spec 13: the periodic status message rides on the same metadata link, so the
// server sees AI liveness even when no frame result arrives.
TEST(metadata_heartbeat_delivered) {
    ca::MetadataConfig cfg;
    cfg.enable                 = true;
    cfg.heartbeat_interval_sec = 1;
    cfg.queue_size             = 8;
    cfg.timeout_ms             = 100;

    std::unique_ptr<RecordingTransport> rec(new RecordingTransport());
    RecordingTransport* raw = rec.get();

    ca::AIStatusInfo st;
    st.enable    = true;
    st.running   = true;
    st.fps       = 5.0;
    st.model     = "models/yolov8n.onnx";
    st.processed = 42;

    ca::MetadataManager m;
    ASSERT(m.init(cfg));
    m.set_transport(std::move(rec));
    m.set_status_provider([&st]() { return st; });
    m.start();

    // No frames at all: the heartbeat alone must keep the link alive.
    for (int i = 0; i < 150; ++i) {
        if (!raw->snapshot().empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    m.stop();

    const std::vector<std::string> v = raw->snapshot();
    ASSERT(!v.empty());
    ASSERT(contains(v[0], "\"type\":\"status\""));
    ASSERT(contains(v[0], "\"enable\":true"));
    ASSERT(contains(v[0], "\"processed\":42"));
    return true;
}
