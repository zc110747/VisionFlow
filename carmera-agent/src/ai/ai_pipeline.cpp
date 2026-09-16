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

    DetectorConfig dcfg;
    dcfg.model_path     = cfg.model;
    dcfg.input_width    = cfg.input_width;
    dcfg.input_height   = cfg.input_height;
    dcfg.confidence     = cfg.confidence;
    dcfg.low_confidence = cfg.low_confidence;
    dcfg.nms_threshold  = cfg.nms_threshold;
    dcfg.num_threads    = cfg.num_threads;
    dcfg.class_id       = 0;  // person

    try {
        detector_ = create_detector();
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[AI] detector factory threw: {}", e.what());
        return false;
    }
    if (!detector_) {
        CA_LOG_ERROR("[AI] detector factory returned null");
        return false;
    }
    if (!detector_->init(dcfg)) {
        // spec 19: model load failure must not stop the agent.
        CA_LOG_ERROR("[AI] model init failed ({}), video stream continues without AI",
                     cfg.model);
        detector_.reset();
        return false;
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
        detector_.reset();
        return false;
    }
    if (!tracker_) {
        CA_LOG_ERROR("[AI] tracker factory returned null");
        detector_.reset();
        return false;
    }
    tracker_->configure(tcfg);

    const std::string mode =
        detector_->keypoint_count() > 0
            ? "pose(" + std::to_string(detector_->keypoint_count()) + "kpt)"
            : "detect";
    CA_LOG_INFO("[AI] ready: backend={} model={} input={}x{} conf={:.2f} mode={}",
                detector_->backend_name(), cfg.model, cfg.input_width,
                cfg.input_height, cfg.confidence, mode);
    CA_LOG_INFO("[AI] source {}x{}@{}fps -> {} (queue={}, interval={}ms)",
                video_width, video_height, video_fps,
                full_rate ? "full-rate" : "sub-sampled", cfg.queue_size,
                interval_ms_);
    current_mode_.store(detector_->keypoint_count() > 0 ? AIMode::Pose
                                                         : AIMode::Detect);
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
    // Build a fresh detector for the requested model. The ONNX load is slow
    // (hundreds of ms); it must NOT run under mtx_ so push_frame() stays
    // non-blocking. The swap back into detector_ is the only locked step.
    DetectorConfig dcfg;
    dcfg.model_path     = (m == AIMode::Pose) ? cfg_.model_pose : cfg_.model;
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
        current_mode_.store(m);
    }
    CA_LOG_INFO("[AI] model switched -> mode={} ({})", to_string(m),
                (m == AIMode::Pose) ? cfg_.model_pose : cfg_.model);
    return true;
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
