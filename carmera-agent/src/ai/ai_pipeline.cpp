// AI pipeline implementation (spec 7 / 8 / 16 / 19).
//
// Design rules that MUST NOT be broken:
//   1. push_frame() is called from the GStreamer streaming thread. It takes a
//      short lock, moves one buffer and returns. It never waits on inference,
//      never waits on the network and never throws.
//   2. All model / tracker work happens on this class' own thread. Any failure
//      there is logged and swallowed: the video pipeline keeps running.
//   3. The queue is bounded. On overflow the OLDEST frame is discarded so the
//      detector always works on the newest picture (a stale detection is worse
//      than no detection for a live stream).

#include "camera_agent/ai/ai_pipeline.h"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <utility>

#include "camera_agent/logger.h"

namespace ca {
namespace {

// Statistics are averaged over this window and flushed to the log every
// kLogIntervalMs milliseconds.
constexpr int64_t kStatsWindowMs = 1000;
constexpr int64_t kLogIntervalMs = 5000;

// After this many consecutive inference failures the error is escalated to
// ERROR level so it is visible without --log-level debug.
constexpr int kFailureEscalate = 10;

double to_ms(std::chrono::steady_clock::time_point a,
             std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// The YOLO model a Detect/Pose request loads. One function so the load path,
// the switch log and the status heartbeat can never disagree about which file
// belongs to which mode. (Hand runs two different models - see install_mode.)
const std::string& yolo_model_for(const AIConfig& cfg, AIMode m) {
    return (m == AIMode::Pose) ? cfg.model_pose : cfg.model;
}

} // namespace

AIPipeline::~AIPipeline() {
    stop();
}

bool AIPipeline::init(const AIConfig& cfg, int video_width, int video_height,
                      int video_fps) {
    cfg_       = cfg;
    video_w_   = video_width;
    video_h_   = video_height;
    video_fps_ = video_fps;

    if (!cfg.enable) {
        CA_LOG_INFO("[AI] disabled by configuration (ai.enable=false)");
        return false;
    }

    // Sampling policy:
    //   source fps < full_rate_below_fps -> every frame is inferred
    //   otherwise                        -> at most cfg.fps frames per second
    const int ai_fps = clamp_ai_fps(cfg.fps);  // enforce [5,12] at the use site
    const bool full_rate = (video_fps > 0 && video_fps < cfg.full_rate_below_fps);
    if (full_rate) {
        interval_ms_ = 0;
    } else {
        interval_ms_ = (ai_fps > 0) ? (1000 / ai_fps) : 0;
    }

    // The hand analyzer is pure logic (no model, no ORT), so it is validated up
    // front: a later switch to Hand then only has to load the two ONNX files.
    hand_ready_ = hand_analyzer_.init(cfg.hand);
    if (!hand_ready_) {
        CA_LOG_WARN("[AI] hand configuration rejected (filter alpha / deadzone "
                    "out of range); the hand mode stays unavailable");
    }

    TrackerConfig tcfg;
    tcfg.high_threshold  = cfg.confidence;
    tcfg.match_threshold = cfg.match_threshold;
    tcfg.track_buffer    = cfg.track_buffer;
    // track_buffer is expressed in "30 fps frames"; tell the tracker the rate at
    // which we actually feed it so the lost-track lifetime stays wall-clock sane.
    tcfg.frame_rate = full_rate ? (video_fps > 0 ? video_fps : ai_fps) : ai_fps;
    if (tcfg.frame_rate <= 0) tcfg.frame_rate = 1;

    try {
        tracker_ = create_tracker();
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[AI] tracker factory threw: {}", e.what());
        return false;
    }
    if (!tracker_) {
        CA_LOG_ERROR("[AI] tracker factory returned null");
        return false;
    }
    tracker_->configure(tcfg);

    // ---- initial model ----------------------------------------------------
    // install_mode() (not apply_mode) because nothing is being "switched" yet;
    // the ready log below is the event.
    AIMode start = cfg.initial_mode;
    if (start == AIMode::Hand && !hand_ready_) start = AIMode::Detect;
    if (!install_mode(start)) {
        // A hand model that cannot be loaded must not cost the operator an
        // otherwise working AI branch (spec 19) - fall back to detection.
        if (start != AIMode::Detect && install_mode(AIMode::Detect)) {
            CA_LOG_WARN("[AI] {} models unavailable; started in detect mode instead",
                        to_string(start));
        } else {
            // spec 19: model load failure must not stop the agent.
            CA_LOG_ERROR("[AI] model init failed ({}), video stream continues without AI",
                         yolo_model_for(cfg, AIMode::Detect));
            detector_.reset();
            return false;
        }
    }
    // Historical auto-detection, preserved: pointing `model` at a pose ONNX used
    // to move the reported mode to Pose by itself. It stays here - and only here
    // - because at runtime an explicit request is honoured verbatim (a web click
    // on "人检测" must not silently answer "pose" and make the overlay drop every
    // frame as a mode mismatch).
    if (current_mode_.load() == AIMode::Detect && detector_ &&
        detector_->keypoint_count() > 0) {
        current_mode_.store(AIMode::Pose);
        CA_LOG_INFO("[AI] {} contains a pose head -> mode auto-detected as pose",
                    cfg.model);
    }

    const AIMode mode = current_mode_.load();
    CA_LOG_INFO("[AI] ready: backend={} model={} input={}x{} conf={:.2f} mode={}",
                active_backend_name(), current_model_path(), cfg.input_width,
                cfg.input_height, cfg.confidence,
                (mode == AIMode::Pose || mode == AIMode::Hand)
                    ? std::string(to_string(mode)) + "(" +
                          std::to_string(keypoint_count()) + "kpt)"
                    : std::string(to_string(mode)));
    CA_LOG_INFO("[AI] source {}x{}@{}fps -> {} (queue={}, interval={}ms)",
                video_width, video_height, video_fps,
                full_rate ? "full-rate" : "sub-sampled", cfg.queue_size,
                interval_ms_);
    return true;
}

void AIPipeline::start() {
    if (running_.load()) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.clear();
        last_due_ = std::chrono::steady_clock::now();
        stat_window_start_ = last_due_;
        last_log_ = last_due_;
    }
    running_.store(true);
    try {
        thread_ = std::thread(&AIPipeline::thread_loop, this);
    } catch (const std::exception& e) {
        running_.store(false);
        CA_LOG_ERROR("[AI] failed to spawn thread: {}", e.what());
    }
}

void AIPipeline::stop() {
    if (!running_.load() && !thread_.joinable()) return;
    running_.store(false);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.clear();
    }
    CA_LOG_DEBUG("[AI] pipeline stopped");
}

void AIPipeline::request_mode(AIMode m) {
    std::lock_guard<std::mutex> lk(mtx_);
    pending_mode_ = m;
}

bool AIPipeline::apply_mode(AIMode m) {
    const AIMode before = current_mode_.load();
    if (!install_mode(m)) return false;
    const AIMode now = current_mode_.load();
    if (now == before) {
        CA_LOG_INFO("[AI] mode={} re-selected; model reloaded ({})", to_string(m),
                    current_model_path());
    } else {
        CA_LOG_INFO("[AI] model switched -> mode={} ({})", to_string(now),
                    current_model_path());
    }
    return true;
}

bool AIPipeline::install_mode(AIMode m) {
    // The ONNX load is slow (hundreds of ms); it must NOT run under mtx_ so
    // push_frame() stays non-blocking. The swap back into the members is the
    // only locked step, and it happens strictly after a successful load - so a
    // failure here always leaves the previously working model untouched.
    if (m == AIMode::Hand) {
        if (!hand_ready_) {
            CA_LOG_ERROR("[AI] hand mode unavailable: its configuration was "
                         "rejected at startup");
            return false;
        }
        std::unique_ptr<IHandDetector> hd;
        try {
            hd = create_hand_detector();
        } catch (const std::exception& e) {
            CA_LOG_ERROR("[AI] hand detector factory threw: {}", e.what());
            return false;
        }
        if (!hd) {
            CA_LOG_ERROR("[AI] hand detector factory returned null");
            return false;
        }
        if (!hd->init(cfg_.hand)) {
            CA_LOG_ERROR("[AI] hand model init failed ({} + {}) - keeping {}",
                         cfg_.hand.model_palm, cfg_.hand.model_landmark,
                         to_string(current_mode_.load()));
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            hand_detector_ = std::move(hd);
            // The modes are mutually exclusive, so holding both would only cost
            // memory (the YOLO session is ~20 MB) for a model that cannot run.
            detector_.reset();
            current_mode_.store(AIMode::Hand);
            // A fresh hand must not inherit the previous one's origin, filter or
            // gesture latch - the reference point in particular would make the
            // first delta a large phantom jump.
            hand_analyzer_.reset();
        }
        return true;
    }

    // ---- Detect / Pose: the existing YOLO path -----------------------------
    DetectorConfig dcfg;
    dcfg.model_path     = yolo_model_for(cfg_, m);
    dcfg.input_width    = cfg_.input_width;
    dcfg.input_height   = cfg_.input_height;
    dcfg.confidence     = cfg_.confidence;
    dcfg.low_confidence = cfg_.low_confidence;
    dcfg.nms_threshold  = cfg_.nms_threshold;
    dcfg.num_threads    = cfg_.num_threads;
    dcfg.class_id       = 0;  // person

    std::unique_ptr<IDetector> det;
    try {
        det = create_detector();
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[AI] model switch to {} failed: factory threw: {}",
                     to_string(m), e.what());
        return false;
    }
    if (!det) {
        CA_LOG_ERROR("[AI] model switch to {} failed: factory returned null",
                     to_string(m));
        return false;
    }
    if (!det->init(dcfg)) {
        CA_LOG_ERROR("[AI] model switch to {} failed: init {} (video continues)",
                     to_string(m), dcfg.model_path);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        detector_ = std::move(det);
        hand_detector_.reset();
        current_mode_.store(m);
    }
    return true;
}

std::string AIPipeline::current_model_path() const {
    std::lock_guard<std::mutex> lk(mtx_);
    switch (current_mode_.load()) {
        // First stage of the cascade - the one that names the mode. The
        // landmark model path is in cfg_.hand.model_landmark and is logged by
        // the detector itself at init.
        case AIMode::Hand: return cfg_.hand.model_palm;
        case AIMode::Pose: return cfg_.model_pose;
        default:           return cfg_.model;
    }
}

const char* AIPipeline::active_backend_name() const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (hand_detector_) return hand_detector_->backend_name();
    if (detector_)      return detector_->backend_name();
    return "none";
}

void AIPipeline::push_frame(AIFrame&& f) {
    if (!running_.load()) return;
    try {
        std::lock_guard<std::mutex> lk(mtx_);
        const size_t limit = cfg_.queue_size > 0
                                 ? static_cast<size_t>(cfg_.queue_size)
                                 : 1u;
        // Bounded queue, drop-oldest-keep-newest (spec 8): a live stream only
        // cares about the current scene.
        while (queue_.size() >= limit) {
            queue_.pop_front();
            ++dropped_;
        }
        queue_.push_back(std::move(f));
    } catch (const std::exception& e) {
        CA_LOG_WARN("[AI] push_frame failed: {}", e.what());
        return;
    }
    cv_.notify_one();
}

void AIPipeline::thread_loop() {
    CA_LOG_DEBUG("[AI] thread started");
    while (running_.load()) {
        AIFrame frame;
        {
            std::unique_lock<std::mutex> lk(mtx_);

            if (interval_ms_ > 0) {
                // Sub-sampled mode: wake up at the next due time, or when the
                // pipeline is stopped. Frames that arrive early simply sit in
                // the bounded queue and are superseded.
                const auto due = last_due_;
                cv_.wait_until(lk, due, [this] { return !running_.load(); });
            } else {
                cv_.wait(lk, [this] {
                    return !queue_.empty() || !running_.load();
                });
            }

            if (!running_.load() && queue_.empty()) break;
            if (queue_.empty()) continue;

            frame = std::move(queue_.back());
            if (queue_.size() > 1) {
                skipped_ += static_cast<uint64_t>(queue_.size() - 1);
            }
            queue_.clear();

            if (interval_ms_ > 0) {
                const auto now = std::chrono::steady_clock::now();
                const auto step = std::chrono::milliseconds(interval_ms_);
                if (last_due_ + step <= now) {
                    // Fell behind (slow inference): resync instead of bursting.
                    last_due_ = now + step;
                } else {
                    last_due_ += step;
                }
            }
        }

        // Consume a pending AI mode request (web UI driven). This runs AFTER
        // the queue lock above is released so that apply_mode()'s own (internal)
        // swap lock never nests under it - a nested lock on the same
        // non-recursive mutex would deadlock the thread. Snapshot the intent,
        // reset it (so a failed switch is not retried in a busy loop), then
        // rebuild the detector outside any lock.
        {
            std::optional<AIMode> want;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (pending_mode_) {
                    want = *pending_mode_;
                    pending_mode_.reset();
                }
            }
            if (want && *want != current_mode_.load()) {
                apply_mode(*want);  // on failure the current model is kept
            }
        }

        process(frame);
    }
    CA_LOG_DEBUG("[AI] thread exited");
}

void AIPipeline::process(AIFrame& f) {
    // Hand produces a different shape of result (one cascade, one HandState, no
    // tracker), so it gets its own frame handler rather than a branch wedged
    // into the detect/pose path below. current_mode_ is written only by this
    // thread (install_mode), so this read needs no lock.
    if (current_mode_.load() == AIMode::Hand) {
        process_hand(f);
        return;
    }

    // detector_ may be swapped by apply_mode() (AI thread) between calls; read
    // the pointer once under lock so a whole frame uses one consistent model.
    IDetector* det = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        det = detector_.get();
    }
    if (!det || !tracker_) return;

    const auto t0 = std::chrono::steady_clock::now();

    std::vector<Detection> dets;
    bool ok = false;
    try {
        ok = det->detect(f.rgb.empty() ? nullptr : f.rgb.data(),
                         f.width, f.height, dets);
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[AI] detect() threw: {}", e.what());
        ok = false;
    } catch (...) {
        CA_LOG_ERROR("[AI] detect() threw an unknown exception");
        ok = false;
    }

    const auto t1 = std::chrono::steady_clock::now();

    if (!ok) {
        // spec 3.1 / 19: a broken detector is not allowed to take the process
        // down nor to stall the video branch. Log and keep the thread alive.
        ++consecutive_failures_;
        if (consecutive_failures_ == 1 || consecutive_failures_ == kFailureEscalate) {
            CA_LOG_ERROR("[AI] inference failed ({} consecutive), frame skipped",
                         consecutive_failures_.load());
        }
        return;
    }
    consecutive_failures_ = 0;

    std::vector<TrackedObject> tracks;
    try {
        tracks = tracker_->update(dets);
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[AI] tracker update() threw: {}", e.what());
        return;
    } catch (...) {
        CA_LOG_ERROR("[AI] tracker update() threw an unknown exception");
        return;
    }

    const auto t2 = std::chrono::steady_clock::now();

    AIFrameResult res;
    res.frame_id     = f.frame_id;
    res.timestamp    = f.timestamp;
    res.video_width  = video_w_;
    res.video_height = video_h_;
    // Stamp the mode this frame was actually produced with. Read atomically so
    // a mid-switch swap is reflected exactly once the new detector is live;
    // transition frames keep the OLD mode and are dropped by the web.
    res.ai_mode      = aimode_web_string(current_mode_.load());
    res.objects.reserve(tracks.size());
    for (const auto& t : tracks) {
        AIObject o;
        o.class_name = t.class_name;
        o.class_id   = t.class_id;
        o.confidence = t.confidence;
        o.track_id   = t.track_id;
        o.x1         = t.x1;
        o.y1         = t.y1;
        o.x2         = t.x2;
        o.y2         = t.y2;
        o.keypoints  = t.keypoints;
        res.objects.push_back(std::move(o));
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        ++processed_;
        objects_       = res.objects.size();
        sum_infer_ms_ += to_ms(t0, t1);
        sum_track_ms_ += to_ms(t1, t2);
        ++stat_frames_;
        log_periodic_locked();
    }

    if (cfg_.log_objects && !res.objects.empty()) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        for (size_t i = 0; i < res.objects.size(); ++i) {
            const auto& o = res.objects[i];
            if (i) ss << " | ";
            ss << "id=" << o.track_id << " " << o.class_name << " "
               << std::setprecision(2) << o.confidence << " ["
               << std::setprecision(0) << o.x1 << "," << o.y1 << "->"
               << o.x2 << "," << o.y2 << "]" << std::setprecision(2);
        }
        CA_LOG_INFO("[AI] frame={} ts={} objects={} {}", f.frame_id, f.timestamp,
                    res.objects.size(), ss.str());
    }

    if (result_cb_) {
        try {
            result_cb_(res);
        } catch (const std::exception& e) {
            CA_LOG_ERROR("[AI] result callback threw: {}", e.what());
        } catch (...) {
            CA_LOG_ERROR("[AI] result callback threw an unknown exception");
        }
    }
}

void AIPipeline::process_hand(AIFrame& f) {
    IHandDetector* det = nullptr;
    double ai_fps = 0.0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        det    = hand_detector_.get();
        ai_fps = last_fps_;
    }
    if (!det) return;

    const auto t0 = std::chrono::steady_clock::now();

    HandDetection hd;
    bool ok = false;
    try {
        ok = det->detect_hand(f.rgb.empty() ? nullptr : f.rgb.data(), f.width,
                              f.height, hd);
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[AI] detect_hand() threw: {}", e.what());
        ok = false;
    } catch (...) {
        CA_LOG_ERROR("[AI] detect_hand() threw an unknown exception");
        ok = false;
    }

    const auto t1 = std::chrono::steady_clock::now();

    if (!ok) {
        // Same contract as the detect/pose path: a broken detector is logged
        // and the frame is dropped, but the thread and the video branch live on.
        ++consecutive_failures_;
        if (consecutive_failures_ == 1 || consecutive_failures_ == kFailureEscalate) {
            CA_LOG_ERROR("[AI] hand inference failed ({} consecutive), frame skipped",
                         consecutive_failures_.load());
        }
        return;
    }
    consecutive_failures_ = 0;

    // The analyzer is fed even when nothing was found: that is precisely how a
    // hand that left the frame ages out (lost_frames_to_reset) instead of
    // freezing its last state forever. It also applies hand.emit_landmarks
    // itself, so both the object and the hand block see the same decision.
    HandState hs;
    hand_analyzer_.update(hd, f.width, f.height, f.frame_id, f.timestamp,
                          ai_fps, hs);

    const auto t2 = std::chrono::steady_clock::now();

    AIFrameResult res;
    res.frame_id     = f.frame_id;
    res.timestamp    = f.timestamp;
    res.video_width  = video_w_;
    res.video_height = video_h_;
    res.ai_mode      = aimode_web_string(AIMode::Hand);
    if (hs.detected) {
        // Reuse the existing object shape so the overlay's generic "draw boxes
        // and keypoints" path works unchanged (spec 10). track_id stays -1:
        // Phase 1 is single-hand and nothing downstream consumes a hand id.
        AIObject o;
        o.class_name = "hand";
        o.class_id   = -1;   // not a COCO class
        o.confidence = hs.confidence;
        o.track_id   = -1;
        o.x1 = hs.x1;
        o.y1 = hs.y1;
        o.x2 = hs.x2;
        o.y2 = hs.y2;
        o.keypoints = hs.landmarks;   // already empty when landmarks are off
        res.objects.push_back(std::move(o));
    }
    // Engaged even with no hand: the frame is a hand-mode frame, and the extra
    // fields (reference state, gesture latch) are exactly what the debug panel
    // needs in order to explain an empty frame.
    res.hand = std::move(hs);

    {
        std::lock_guard<std::mutex> lk(mtx_);
        ++processed_;
        objects_       = res.objects.size();
        sum_infer_ms_ += to_ms(t0, t1);
        sum_track_ms_ += to_ms(t1, t2);   // hand: analyzer time, not a tracker
        ++stat_frames_;
        log_periodic_locked();
    }

    const HandState& h = *res.hand;
    // Same gate as the detect/pose path: log the frame only when the model
    // actually found something, otherwise a 12 fps empty scene floods the log.
    if (cfg_.log_objects && h.detected) {
        CA_LOG_INFO("[AI] frame={} ts={} mode=hand conf={:.2f} "
                    "hand={} gesture={} motion=({},{},{})",
                    f.frame_id, f.timestamp, h.confidence,
                    to_string(h.handedness), to_string(h.gesture),
                    to_string(h.motion.x), to_string(h.motion.y),
                    to_string(h.motion.z));
    }

    if (result_cb_) {
        try {
            result_cb_(res);
        } catch (const std::exception& e) {
            CA_LOG_ERROR("[AI] result callback threw: {}", e.what());
        } catch (...) {
            CA_LOG_ERROR("[AI] result callback threw an unknown exception");
        }
    }
}

void AIPipeline::log_periodic_locked() {
    const auto now = std::chrono::steady_clock::now();

    // Rolling 1 second window -> instantaneous AI fps.
    const double window_ms = to_ms(stat_window_start_, now);
    if (window_ms >= static_cast<double>(kStatsWindowMs)) {
        last_fps_ = (stat_frames_ * 1000.0) / window_ms;
        if (stat_frames_ > 0) {
            last_infer_ms_ = sum_infer_ms_ / static_cast<double>(stat_frames_);
            last_track_ms_ = sum_track_ms_ / static_cast<double>(stat_frames_);
        }
        stat_window_start_ = now;
        stat_frames_       = 0;
        sum_infer_ms_      = 0.0;
        sum_track_ms_      = 0.0;
    }

    const double since_log = to_ms(last_log_, now);
    if (since_log < static_cast<double>(kLogIntervalMs)) return;
    last_log_ = now;

    CA_LOG_INFO("[AI] fps={:.1f} infer={:.1f}ms track={:.2f}ms queue={} "
                "processed={} dropped={} skipped={} objects={}",
                last_fps_, last_infer_ms_, last_track_ms_,
                static_cast<int>(queue_.size()), processed_,
                dropped_ + skipped_, skipped_, objects_);
}

AIStats AIPipeline::stats() const {
    AIStats s;
    std::lock_guard<std::mutex> lk(mtx_);
    s.ai_fps       = last_fps_;
    s.inference_ms = last_infer_ms_;
    s.tracker_ms   = last_track_ms_;
    s.queue_size   = static_cast<int>(queue_.size());
    s.dropped      = dropped_ + skipped_;
    s.processed    = processed_;
    s.objects      = objects_;
    return s;
}

} // namespace ca
