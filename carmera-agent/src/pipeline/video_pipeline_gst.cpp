// Real video pipeline backed by GStreamer 1.0.
// capture -> videoconvert -> (caps) -> H264 encoder -> h264parse ->
// rtspclientsink(location).
//
// Note: rtspclientsink exposes a *request* pad (sink_%u) and selects its own
// RTP payloader internally (pad property "payloader"). Feeding it the output of
// an explicit rtph264pay fails to link ("could not link pay0 to rtspclientsink0"),
// so the encoded stream goes straight from h264parse into rtspclientsink.
// Compiled only when CAMERA_AGENT_BACKEND == gstreamer.
#include "camera_agent/video_pipeline.h"
#include "camera_agent/logger.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ca {

namespace {

bool element_available(const char* element) {
    GstElementFactory* f = gst_element_factory_find(element);
    if (f) { gst_object_unref(f); return true; }
    return false;
}

// Map the configured encoder name to a GStreamer element + options.
//
// Low-latency flags are applied for every encoder. NVENC (nvh264enc) by
// default reorders B-frames and runs lookahead, which alone adds 300 ms ~ 1 s+
// of glass-to-glass latency; `zerolatency=true` + `tune=ultra-low-latency` +
// `bframes=0` removes the reordering delay. Property names verified with
// `gst-inspect-1.0`.
std::string encoder_element(const std::string& enc, int bitrate, int keyint) {
    const std::string bit = " bitrate=" + std::to_string(bitrate);
    // key-int-max is the x264/Intel property name. nvh264enc instead exposes
    // "gop-size" (verified with gst-inspect-1.0) - passing key-int-max there is a
    // silent no-op, leaving NVENC's default (~infinite) GOP and making WebRTC wait
    // many seconds for the first decodable IDR. Use the right name per backend.
    const std::string ki = " key-int-max=" + std::to_string(keyint);
    if (enc == "h264" || enc == "x264" || enc == "x264enc") {
        return "x264enc name=enc" + bit + ki +
               " tune=zerolatency speed-preset=veryfast bframes=0"
               " repeat-sequence-header=true";
    }
    // NVIDIA NVENC: zerolatency disables B-frame reorder; rc-lookahead=0 drops
    // the lookahead window; repeat-sequence-header keeps every IDR self-contained
    // so a reconnect (auto-resume) does not wait for the next keyframe.
    // GOP is set via "gop-size" (NOT key-int-max) - this is the fix for the
    // multi-second WebRTC first-frame delay on NVENC.
    if (enc.rfind("nv", 0) == 0) {
        return enc + " name=enc" + bit +
               " gop-size=" + std::to_string(keyint) +
               " tune=ultra-low-latency zerolatency=true bframes=0"
               " rc-lookahead=0 repeat-sequence-header=true";
    }
    // Intel MFX.
    if (enc.rfind("mfx", 0) == 0) {
        return enc + " name=enc" + bit + ki +
               " tune=low-latency bframes=0 rc-lookahead=0"
               " repeat-sequence-header=true";
    }
    // Unknown hardware encoder: disable B-frames as a safe low-latency baseline.
    return enc + " name=enc" + bit + " bframes=0";
}

// Pick the best Windows camera source. On this machine gst-device-monitor
// reports the UVC device under Media Foundation (mfvideosrc); dshowvideosrc and
// ksvideosrc leave the pipeline stalled after the first frame for such devices,
// so Media Foundation is preferred when available.
std::string pick_source() {
    const char* sw[] = {"mfvideosrc", "dshowvideosrc", "ksvideosrc", nullptr};
    for (const char** s = sw; *s; ++s) {
        if (element_available(*s)) return *s;
    }
    return "";
}

std::string source_element(const std::string& src) {
    std::string name;
    if (src == "ksvideosrc" || src == "ks")            name = "ksvideosrc";
    else if (src == "dshowvideosrc" || src == "dshow") name = "dshowvideosrc";
    else if (src == "mfvideosrc" || src == "mf")       name = "mfvideosrc";
    else if (src == "auto" || src.empty())             name = pick_source();
    else                                                name = src; // pass through any element name (e.g. videotestsrc)
    if (name.empty()) name = "dshowvideosrc"; // last resort; required-check reports missing
    return name;
}

// Prefer a hardware H264 encoder; fall back to x264enc.
std::string pick_encoder() {
    const char* hw[] = {"mfxh264enc", "nvh264enc", "vah264enc", "v4l2h264enc", nullptr};
    for (const char** e = hw; *e; ++e) {
        if (element_available(*e)) {
            CA_LOG_INFO("Using hardware H264 encoder: {}", *e);
            return *e;
        }
    }
    CA_LOG_INFO("No hardware H264 encoder found; falling back to x264enc");
    return "x264enc";
}

} // namespace

// Clamp helper used below.
static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Maximum keyint we will ever apply (frames). Only a safety bound; the real
// value is the negotiated framerate (=> ~1s GOP).
constexpr int kMaxKeyint = 300;

// Auto bitrate for a negotiated native format (auto_res mode). Scales with
// pixel throughput so 1080p@30 gets materially more headroom than 240x240,
// while staying within sane bounds. ~0.07 bit/pixel is a good H264 baseline at
// the veryfast preset.
static int auto_bitrate(int w, int h, int fps) {
    if (w <= 0 || h <= 0 || fps <= 0) return 2000;
    double px = double(w) * double(h) * double(fps);
    int kb = int(px * 0.07 / 1000.0);
    if (kb < 800)   kb = 800;
    if (kb > 12000) kb = 12000;
    return kb;
}

// ---- Latency instrumentation (opt-in via PipelineParams.measure_latency) ----
// Each frame's buffer is stamped with the wall-clock moment it passes three
// pipeline boundaries, keyed by its (monotonic) PTS:
//   stage 0: capture  - source element src pad      (raw frame out of camera)
//   stage 1: encode   - encoder src pad             (H264 out)
//   stage 2: push     - rtspclientsink sink pad      (just before network send)
// Differences between consecutive stages give per-segment latency entirely
// inside camera-agent. mediamtx / ffplay are deliberately NOT measured here.
struct LatencyTracker {
    std::mutex mtx;
    // FIFO queues of wall-clock timestamps at each stage. Buffers flow 1:1 and
    // in order through capture -> encode -> push, so the front of each queue is
    // the same logical frame. We match by ORDER, not by PTS: the H264 encoder
    // (re)stamps PTS, which would otherwise break per-frame keying.
    std::deque<gint64> cap_q, enc_q, push_q;
    gint64   sum_enc = 0, sum_push = 0, sum_total = 0;
    uint64_t n = 0;
    std::chrono::steady_clock::time_point last_log =
        std::chrono::steady_clock::now();

    void reset() {
        std::lock_guard<std::mutex> l(mtx);
        cap_q.clear(); enc_q.clear(); push_q.clear();
        sum_enc = sum_push = sum_total = 0;
        n = 0;
    }

    void push_stage(int stage, gint64 t) {
        std::lock_guard<std::mutex> l(mtx);
        auto& q = (stage == 0) ? cap_q : (stage == 1) ? enc_q : push_q;
        q.push_back(t);
        // Resync if a stage fell behind (e.g. a dropped buffer): drain all queues.
        if (q.size() > 1024) { cap_q.clear(); enc_q.clear(); push_q.clear(); return; }
        if (cap_q.empty() || enc_q.empty() || push_q.empty()) return;
        const gint64 tc = cap_q.front(); cap_q.pop_front();
        const gint64 te = enc_q.front(); enc_q.pop_front();
        const gint64 tp = push_q.front(); push_q.pop_front();
        sum_enc   += (te - tc);
        sum_push  += (tp - te);
        sum_total += (tp - tc);
        ++n;
    }

    void maybe_log() {
        std::lock_guard<std::mutex> l(mtx);
        auto now = std::chrono::steady_clock::now();
        const double dt =
            std::chrono::duration<double>(now - last_log).count();
        if (n > 0 && dt >= 1.0) {
            CA_LOG_INFO("[latency] capture->encode={:.1f}ms  encode->push={:.1f}ms"
                        "  total(capture->push)={:.1f}ms  (n={})",
                        sum_enc  / (double)n / 1e6,
                        sum_push / (double)n / 1e6,
                        sum_total/ (double)n / 1e6,
                        n);
            sum_enc = sum_push = sum_total = 0;
            n = 0;
            last_log = now;
        }
    }
};

struct LatencyCbCtx {
    LatencyTracker* t = nullptr;
    int            stage = 0;
};

static GstPadProbeReturn latency_probe_cb(GstPad*, GstPadProbeInfo* info,
                                          gpointer user) {
    auto* ctx = static_cast<LatencyCbCtx*>(user);
    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;
    ctx->t->push_stage(ctx->stage, (gint64)gst_util_get_timestamp());
    return GST_PAD_PROBE_OK;
}

class GstVideoPipeline : public VideoPipeline {
public:
    GstVideoPipeline() {
        if (!gst_is_initialized())
            gst_init(nullptr, nullptr);
    }

    ~GstVideoPipeline() override { stop(); }

    bool build(const PipelineParams& p, const std::string& rtsp_url) override {
        // Remember the params so a low-latency GOP correction can rebuild with
        // adjusted keyint/bitrate after the real framerate is known.
        pp_ = p;
        auto_res_ = p.auto_res;
        built_keyint_ = p.keyframe_interval;
        rtsp_url_ = rtsp_url;
        keyint_corrected_ = false;

        // ---- Required element checks (clear error, no crash) ----
        const std::string src = source_element(p.source);
        const std::string enc_elem = (p.encoder == "h264" || p.encoder == "x264")
                                     ? pick_encoder() : p.encoder;

        // AI branch requested? Then the tee/appsink elements must exist too.
        // It is decided here (before the description is assembled) and only
        // appends to the end of the string - with AI off the description is
        // byte-identical to the pre-AI pipeline.
        const bool with_ai = static_cast<bool>(ai_sink_);

        std::vector<const char*> required = {
            src.c_str(), "videoconvert", enc_elem.c_str(),
            "h264parse", "rtspclientsink"
        };
        if (with_ai) required.push_back("appsink");
        for (const char* e : required) {
            if (!element_available(e)) {
                CA_LOG_ERROR("Required GStreamer element '{}' is not installed.", e);
                missing_ = e;
                return false;
            }
        }

        // ---- Build the pipeline description ----
        // The caps filter deliberately leaves `format` unconstrained: hardware
        // encoders such as nvh264enc accept NV12/Y444/... but NOT I420, so
        // pinning I420 here makes every HW encoder fail to link. Without it,
        // videoconvert negotiates whatever the chosen encoder supports.
        //
        // `queue` between stages decouples capture / convert / encode / push into
        // separate threads so a slow stage (HW encode) never blocks the camera
        // thread, and caps the buffered depth at 2 frames so frames cannot pile
        // up and inflate glass-to-glass latency. rtspclientsink latency=0 /
        // rtx-time=0 disable its receive/RTX buffering.
        const std::string q =
            "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0";

        // When auto_res is requested, do NOT pin width/height/framerate. The
        // camera (and GStreamer negotiation) then uses its native format -- the
        // robust path for UVC devices whose only supported mode (e.g. 240x240@8fps)
        // would otherwise fail the forced caps filter and spin the reconnect loop.
        const std::string conv_out =
            p.auto_res ? "" :
            (" ! video/x-raw,width=" + std::to_string(p.width) +
             ",height=" + std::to_string(p.height) +
             ",framerate=" + std::to_string(p.fps) + "/1");

        // ---- AI branch (spec 2 / 5) ----------------------------------------
        // Tapped AFTER the (optional) caps filter, so the box coordinates the
        // detector produces are in exactly the same pixel space the video
        // pipeline encodes - no second capture device, no re-open of /dev/video.
        //
        //   queue leaky=upstream      : never back-pressure the video branch;
        //                               a full queue drops the OLDEST frame.
        //   appsink drop=true         : never blocks the streaming thread
        //                               either; the AI pipeline simply sees the
        //                               newest frame when it is ready.
        //   sync=false                : no clock waiting inside the sink.
        //
        // The AI branch runs at the FULL camera rate and is sub-sampled by
        // AIPipeline (5 fps by default). Frames are delivered at the camera's
        // native resolution in RGB; letterboxing to the network input size is
        // done in the detector so the bbox inverse-transform stays exact.
        const std::string ai_branch =
            with_ai ? (" aisplit."
                       " ! queue name=aiq max-size-buffers=2 max-size-bytes=0"
                         " max-size-time=0 leaky=upstream"
                       " ! videoconvert name=aiconv"
                       " ! video/x-raw,format=RGB"
                       " ! appsink name=aisink max-buffers=1 drop=true sync=false")
                    : "";

        std::string desc =
            src + " name=cam" +
            (src == "mfvideosrc" || src == "dshowvideosrc" || src == "ksvideosrc"
                 ? " device-index=" + std::to_string(p.camera_id) : "") +
            // videotestsrc (synthetic test source) is not live by default and
            // would flood the pipeline unbounded; rate-limit it like a real
            // camera so the latency probes measure a realistic, balanced flow.
            (src == "videotestsrc" ? " is-live=true" : "") +
            " ! " + q +
            " ! videoconvert name=conv" + conv_out +
            (with_ai ? " ! tee name=aisplit" : "") +
            " ! " + q +
            " ! " + encoder_element(enc_elem, p.bitrate, p.keyframe_interval) +
            " ! " + q +
            " ! h264parse name=parse" +
            " ! rtspclientsink name=sink location=" + rtsp_url + " latency=0 rtx-time=0"
            + ai_branch;

        if (p.auto_res)
            CA_LOG_INFO("Auto-resolution: not forcing caps; camera negotiates native format");
        CA_LOG_INFO("Pipeline: {}", desc);

        GError* err = nullptr;
        pipeline_ = gst_parse_launch(desc.c_str(), &err);
        if (!pipeline_) {
            CA_LOG_ERROR("Failed to create pipeline: {}", err ? err->message : "unknown");
            if (err) g_error_free(err);
            return false;
        }

        // ---- Stats probe on the encoder output ----
        GstElement* enc = gst_bin_get_by_name(GST_BIN(pipeline_), "enc");
        if (enc) {
            GstPad* srcpad = gst_element_get_static_pad(enc, "src");
            if (srcpad) {
                gst_pad_add_probe(srcpad,
                    static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER),
                    &GstVideoPipeline::probe_cb, this, nullptr);
                gst_object_unref(srcpad);
            }
            gst_object_unref(enc);
        }

        // ---- AI frame supply: hook the appsink ------------------------------
        // The frame counter restarts with every (re)build: it is a per-stream
        // camera frame counter (spec 13).
        ai_frame_id_ = 0;
        if (with_ai) {
            GstElement* as = gst_bin_get_by_name(GST_BIN(pipeline_), "aisink");
            if (!as) {
                CA_LOG_ERROR("AI branch: element 'aisink' not found; AI disabled");
            } else {
                // Callback API instead of signals: no marshal overhead, and the
                // callback simply moves the frame into the AI queue.
                GstAppSinkCallbacks cbs{};
                cbs.new_sample = &GstVideoPipeline::ai_new_sample_cb;
                gst_app_sink_set_callbacks(GST_APP_SINK(as), &cbs, this, nullptr);
                gst_object_unref(as);
                CA_LOG_INFO("AI branch attached: tee -> leaky queue -> RGB appsink");
            }
        }

        // ---- Opt-in latency probes (capture / encode / push) ----
        measure_latency_ = p.measure_latency;
        neg_valid_ = false;   // re-query negotiated caps after (re)build
        if (measure_latency_) {
            lat_.reset();
            CA_LOG_INFO("Latency probe enabled: capture(cam.src) -> encode(enc.src)"
                        " -> push(parse.src)");
            auto attach = [&](const char* name, const char* pad, int stage) {
                GstElement* e = gst_bin_get_by_name(GST_BIN(pipeline_), name);
                if (!e) { CA_LOG_WARN("latency probe: element '{}' not found", name); return; }
                GstPad* p2 = gst_element_get_static_pad(e, pad);
                if (p2) {
                    lat_ctx_[stage].t = &lat_;
                    lat_ctx_[stage].stage = stage;
                    gst_pad_add_probe(p2,
                        static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER),
                        &latency_probe_cb, &lat_ctx_[stage], nullptr);
                    gst_object_unref(p2);
                }
                gst_object_unref(e);
            };
            // rtspclientsink exposes a *request* sink pad (sink_%u), not a static
            // "sink", so its push boundary is sampled at h264parse's static src
            // pad: rtspclientsink (a sink) pulls from h264parse, so the parse.src
            // probe fires exactly as the encoded frame is taken for pushing.
            attach("cam",   "src", 0);
            attach("enc",   "src", 1);
            attach("parse", "src", 2);
        }

        // ---- Bus watch ----
        // stop() raises stop_flag_ to make the previous bus thread return. It has
        // to be cleared here, otherwise a rebuilt pipeline (reconnect path) would
        // spawn a watcher that exits immediately and never reports STREAMING.
        stop_flag_ = false;
        bus_ = gst_element_get_bus(pipeline_);
        bus_thread_ = std::thread(&GstVideoPipeline::bus_loop, this);
        return true;
    }

    bool start() override {
        if (!pipeline_) return false;
        set_status(StreamStatus::CONNECTING);
        const GstStateChangeReturn r =
            gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (r == GST_STATE_CHANGE_FAILURE) {
            CA_LOG_ERROR("Failed to set pipeline to PLAYING (camera busy / unavailable?)");
            set_status(StreamStatus::ERROR);
            return false;
        }
        query_negotiated();

        // Live sources finalize caps a moment after PLAYING; wait (bounded) so
        // we can read the real framerate before deciding the GOP. Avoids a
        // false "unknown fps" that would skip the low-latency correction.
        for (int i = 0; i < 30 && !neg_valid_; ++i) {
            g_usleep(100000); // 100 ms
            query_negotiated();
        }

        // Low-latency GOP correction (auto_res only). The configured keyint is
        // expressed in FRAMES; at the camera's native (often low) framerate 30
        // frames can mean several seconds, forcing the WebRTC player to wait
        // that long for the first IDR. Once we know the negotiated fps we
        // rebuild ONCE with keyint = ~1 second of frames, so the player starts
        // within ~1s regardless of the source framerate. Bitrate is also
        // re-derived from the negotiated native resolution in auto_res mode.
        if (auto_res_ && neg_valid_ && neg_fps_ > 0 && !keyint_corrected_) {
            const int desired = clamp_int(neg_fps_, 1, kMaxKeyint);
            if (desired != built_keyint_) {
                keyint_corrected_ = true;
                CA_LOG_INFO("Negotiated {}fps ({}x{}); rebuilding with keyint={} for "
                            "<=1s GOP (was {})", neg_fps_, neg_w_, neg_h_, desired, built_keyint_);
                rebuild_with_keyint(desired);
            }
        }
        return true;
    }

    void stop() override {
        stop_flag_ = true;
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        }
        if (bus_thread_.joinable()) bus_thread_.join();
        if (bus_) { gst_object_unref(bus_); bus_ = nullptr; }
        if (pipeline_) { gst_object_unref(pipeline_); pipeline_ = nullptr; }
        // Suppress the transient DISCONNECTED while we are mid GOP-correction
        // rebuild, otherwise the StreamController would start a reconnect loop.
        if (!rebuilding_) set_status(StreamStatus::DISCONNECTED);
    }

    Statistics get_stats() const override {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        return stats_;
    }

    bool is_running() const override { return pipeline_ != nullptr && running_; }

    StreamStatus get_status() const override { return status_.load(); }

    bool get_negotiated_resolution(int& width, int& height, int& fps) const override {
        if (!neg_valid_) return false;
        width = neg_w_; height = neg_h_; fps = neg_fps_;
        return true;
    }

    void set_status_callback(StatusCallback cb) override { cb_ = std::move(cb); }

    bool check_plugins(std::vector<std::string>* missing) override {
        // A camera source (any Windows backend) is required.
        if (!element_available("mfvideosrc") && !element_available("dshowvideosrc") &&
            !element_available("ksvideosrc")) {
            if (missing) missing->push_back("mfvideosrc / ksvideosrc / dshowvideosrc (camera source)");
        }
        // rtspclientsink does its own RTP payloading, so no rtppay element here.
        const char* base[] = {"videoconvert", "x264enc",
                              "h264parse", "rtspclientsink"};
        for (const char* e : base) {
            if (!element_available(e)) {
                if (missing) missing->push_back(e);
            }
        }
        return missing ? missing->empty() : true;
    }

private:
    void set_status(StreamStatus s) {
        status_ = s;
        if (cb_) cb_(s);
    }

    // Rebuild the pipeline with a corrected keyint (and, in auto_res mode, a
    // bitrate re-derived from the negotiated native format). Used once at
    // startup to enforce a <=1s GOP. The rebuilding_ flag suppresses the
    // transient DISCONNECTED status so the StreamController's reconnect loop is
    // not wrongly triggered.
    void rebuild_with_keyint(int desired) {
        rebuilding_ = true;
        stop();
        rebuilding_ = false;

        pp_.keyframe_interval = desired;
        if (auto_res_) {
            pp_.bitrate = auto_bitrate(neg_w_, neg_h_, neg_fps_);
            CA_LOG_INFO("auto_res bitrate set to {}kbps for {}x{}@{}fps",
                        pp_.bitrate, neg_w_, neg_h_, neg_fps_);
        }

        if (!build(pp_, rtsp_url_)) {
            CA_LOG_ERROR("GOP correction: rebuild failed");
            set_status(StreamStatus::ERROR);
            return;
        }
        set_status(StreamStatus::CONNECTING);
        const GstStateChangeReturn r =
            gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (r == GST_STATE_CHANGE_FAILURE) {
            CA_LOG_ERROR("GOP correction: failed to set PLAYING");
            set_status(StreamStatus::ERROR);
            return;
        }
        query_negotiated();
    }

    // Read the actually-negotiated capture format and log it. In auto_res mode
    // this is the camera's native resolution/fps; it also corrects DeviceInfo
    // reporting when an explicit caps was used.
    //
    // The camera SOURCE pad ("cam" src) carries the device's native format and
    // always has fixed negotiated caps (width/height/framerate). The encoder
    // sink pad can still report only template caps at PLAYING time, so it is
    // only used as a fallback.
    void query_negotiated() {
        if (!pipeline_) return;
        auto read_pad = [&](const char* elem, const char* pad) -> bool {
            GstElement* e = gst_bin_get_by_name(GST_BIN(pipeline_), elem);
            if (!e) return false;
            GstPad* p = gst_element_get_static_pad(e, pad);
            bool ok = false;
            if (p) {
                GstCaps* caps = gst_pad_get_current_caps(p);
                if (!caps) caps = gst_pad_query_caps(p, nullptr);
                if (caps && gst_caps_get_size(caps) > 0) {
                    const GstStructure* s = gst_caps_get_structure(caps, 0);
                    gint w = 0, h = 0;
                    if (gst_structure_get_int(s, "width", &w) &&
                        gst_structure_get_int(s, "height", &h)) {
                        int fps = 0;
                        const GValue* fr = gst_structure_get_value(s, "framerate");
                        if (fr && GST_VALUE_HOLDS_FRACTION(fr)) {
                            const gint den = gst_value_get_fraction_denominator(fr);
                            const gint num = gst_value_get_fraction_numerator(fr);
                            if (den > 0) fps = num / den;
                        }
                        // Log only when the negotiated format changes (avoids a
                        // duplicate line from start() and the PLAYING state msg).
                        if (!neg_valid_ || w != neg_w_ || h != neg_h_ || fps != neg_fps_) {
                            neg_w_ = w; neg_h_ = h; neg_fps_ = fps; neg_valid_ = true;
                            CA_LOG_INFO("Negotiated capture format: {}x{} @ {}fps", w, h, fps);
                        }
                        ok = true;
                    } else {
                        CA_LOG_INFO("Negotiated caps (no w/h): {}", gst_caps_to_string(caps));
                    }
                    if (caps) gst_caps_unref(caps);
                }
                gst_object_unref(p);
            }
            gst_object_unref(e);
            return ok;
        };
        if (read_pad("cam", "src")) return;
        read_pad("enc", "sink");
    }

    // ---- AI branch ---------------------------------------------------------
    // Runs on the GStreamer streaming thread. Contract: never block, never
    // throw, never touch anything but the frame -> queue hand-off. A slow or
    // broken detector therefore cannot stall capture/encode/push (spec 3.1).
    static GstFlowReturn ai_new_sample_cb(GstAppSink* sink, gpointer user) {
        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (!sample) return GST_FLOW_OK;
        static_cast<GstVideoPipeline*>(user)->on_ai_sample(sample);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    void on_ai_sample(GstSample* sample) {
        if (!ai_sink_) return;

        GstBuffer* buf  = gst_sample_get_buffer(sample);
        GstCaps*   caps = gst_sample_get_caps(sample);
        if (!buf || !caps) return;

        GstVideoInfo info;
        if (!gst_video_info_from_caps(&info, caps)) return;
        const int w = GST_VIDEO_INFO_WIDTH(&info);
        const int h = GST_VIDEO_INFO_HEIGHT(&info);
        if (w <= 0 || h <= 0) return;

        GstMapInfo map;
        if (!gst_buffer_map(buf, &map, GST_MAP_READ)) return;

        AIFrame f;
        // spec 13: monotonic CAMERA frame counter. The tee sees every captured
        // frame, so this counts real frames - not AI results.
        f.frame_id = ++ai_frame_id_;
        // spec 14: timestamp from the GStreamer PTS (ns -> ms), NOT time(NULL).
        if (GST_BUFFER_PTS_IS_VALID(buf))
            f.timestamp = static_cast<uint64_t>(GST_BUFFER_PTS(buf) / GST_MSECOND);
        f.width  = w;
        f.height = h;

        // Copy plane 0 row by row: RGB rows may be 4-byte aligned, so the
        // stride can exceed width*3. The AI pipeline wants tightly packed data.
        const gint   stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
        const size_t row    = static_cast<size_t>(w) * 3u;
        const uint8_t* src  = map.data + GST_VIDEO_INFO_PLANE_OFFSET(&info, 0);
        if (stride > 0 && map.size >= static_cast<gsize>(stride) * h) {
            f.rgb.resize(row * static_cast<size_t>(h));
            for (int y = 0; y < h; ++y) {
                std::memcpy(f.rgb.data() + static_cast<size_t>(y) * row,
                            src + static_cast<size_t>(y) * stride, row);
            }
            gst_buffer_unmap(buf, &map);
            try {
                ai_sink_(std::move(f));
            } catch (const std::exception& e) {
                CA_LOG_WARN("[AI] frame sink threw: {}", e.what());
            } catch (...) {
                CA_LOG_WARN("[AI] frame sink threw an unknown exception");
            }
        } else {
            gst_buffer_unmap(buf, &map);
        }
    }

    static GstPadProbeReturn probe_cb(GstPad*, GstPadProbeInfo* info, gpointer user) {
        auto* self = static_cast<GstVideoPipeline*>(user);
        GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
        if (buf) {
            const gsize sz = gst_buffer_get_size(buf);
            std::lock_guard<std::mutex> lk(self->stats_mtx_);
            self->stats_.frames += 1;
            self->bytes_since_sample_ += sz;
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - self->last_sample_).count();
            if (dt >= 1.0) {
                self->stats_.bitrate_kbps = (self->bytes_since_sample_ * 8.0) / 1000.0 / dt;
                self->bytes_since_sample_ = 0;
                self->last_sample_ = now;
            }
        }
        return GST_PAD_PROBE_OK;
    }

    void bus_loop() {
        while (!stop_flag_) {
            GstMessage* msg = gst_bus_timed_pop(bus_, 100 * 1000000); // 100 ms
            if (!msg) continue;
            handle_message(msg);
            gst_message_unref(msg);
            if (measure_latency_) lat_.maybe_log();
        }
    }

    void handle_message(GstMessage* msg) {
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* e = nullptr; gchar* dbg = nullptr;
                gst_message_parse_error(msg, &e, &dbg);
                CA_LOG_ERROR("GStreamer error: {} ({})", e ? e->message : "?", dbg ? dbg : "");
                if (e) g_error_free(e);
                if (dbg) g_free(dbg);
                if (!rebuilding_) set_status(StreamStatus::DISCONNECTED); // trigger reconnect in Phase 6
                break;
            }
            case GST_MESSAGE_EOS:
                CA_LOG_WARN("End of stream (server closed?)");
                if (!rebuilding_) set_status(StreamStatus::DISCONNECTED);
                break;
            case GST_MESSAGE_STATE_CHANGED: {
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline_)) {
                    GstState old, cur, pending;
                    gst_message_parse_state_changed(msg, &old, &cur, &pending);
                    if (cur == GST_STATE_PLAYING) {
                        running_ = true;
                        query_negotiated();   // caps may finalize only now
                        set_status(StreamStatus::STREAMING);
                    } else if (cur == GST_STATE_NULL) {
                        running_ = false;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    GstElement*   pipeline_ = nullptr;
    GstBus*       bus_ = nullptr;
    std::thread   bus_thread_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> running_{false};
    std::atomic<StreamStatus> status_{StreamStatus::DISCONNECTED};
    StatusCallback cb_;
    std::string   missing_;

    // Build params cached so the GOP-correction rebuild can reuse them with an
    // adjusted keyint/bitrate.
    PipelineParams pp_{};
    std::string   rtsp_url_;
    bool          auto_res_ = false;
    int           built_keyint_ = 0;
    bool          keyint_corrected_ = false;
    // True while the one-time GOP-correction rebuild is in flight; suppresses
    // the transient DISCONNECTED status that would otherwise fire the reconnect
    // loop.
    bool          rebuilding_ = false;

    bool         measure_latency_ = false;
    LatencyTracker lat_;
    LatencyCbCtx lat_ctx_[3];

    // Negotiated capture format (filled by query_negotiated()).
    int  neg_w_ = 0, neg_h_ = 0, neg_fps_ = 0;
    bool neg_valid_ = false;

    // AI branch: camera frame counter (spec 13). The tee feeds every frame, so
    // this is a true captured-frame counter, independent of the AI rate.
    uint64_t ai_frame_id_ = 0;

    mutable std::mutex stats_mtx_;
    Statistics    stats_{};
    guint64       bytes_since_sample_ = 0;
    std::chrono::steady_clock::time_point last_sample_ =
        std::chrono::steady_clock::now();
};

std::unique_ptr<VideoPipeline> VideoPipeline::create() {
    return std::make_unique<GstVideoPipeline>();
}

} // namespace ca
