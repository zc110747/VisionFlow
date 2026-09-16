#pragma once

// AI pipeline: the *independent* second branch of camera-agent.
//
//   Camera -> tee -+-> video pipeline (unchanged) -> H264 -> RTSP
//                  +-> AI pipeline  (this class)  -> AIFrameResult
//
// Threading contract (spec 7): push_frame() is called from the GStreamer
// streaming thread and must never block or throw; all the work happens in this
// class' own thread. A slow / stuck / crashed detector therefore cannot stall or
// kill the video pipeline (spec 3.1 / spec 19).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "camera_agent/ai/ai_types.h"
#include "camera_agent/ai/detector.h"
#include "camera_agent/ai/tracker.h"

namespace ca {

// NOTE: AIConfig lives in ai_types.h (pure data) so that config.h can embed it
// without dragging in <thread>/<mutex>/<condition_variable>.

class AIPipeline {
public:
    using ResultCallback = std::function<void(const AIFrameResult&)>;

    ~AIPipeline();

    // Full-rate or sub-sampled mode is decided here from the negotiated source
    // framerate. Returns false on any init failure -> caller keeps video only.
    bool init(const AIConfig& cfg, int video_width, int video_height,
              int video_fps);
    void start();
    void stop();
    bool is_running() const { return running_.load(); }

    // Called from the video pipeline thread. Non-blocking, never throws.
    void push_frame(AIFrame&& f);

    void set_result_callback(ResultCallback cb) { result_cb_ = std::move(cb); }

    AIStats stats() const;

    // 0 = detection model; >0 = pose model with this many keypoints per
    // object. Valid after a successful init(), 0 otherwise.
    int keypoint_count() const {
        // detector_ is swapped under mtx_ by the AI thread (apply_mode), while
        // this is read from the status-provider thread - guard the read.
        std::lock_guard<std::mutex> lk(mtx_);
        return detector_ ? detector_->keypoint_count() : 0;
    }

    // --- Runtime AI mode switching (web UI -> video-server -> poller) ------
    // Current loaded mode (Detect / Pose). Read by the status provider on a
    // different thread, so it is atomic.
    AIMode current_mode() const { return current_mode_.load(); }
    // Request a mode change from the poller thread. Intent only - the AI thread
    // consumes it in thread_loop() and rebuilds the detector (apply_mode).
    void request_mode(AIMode m);
    // Synchronously rebuild the detector for `m`. Returns false (and keeps the
    // current model) on any load/init failure. Used by the AI thread and by
    // unit tests; never throws, never stops the video branch.
    bool apply_mode(AIMode m);

private:
    void thread_loop();
    void process(AIFrame& f);
    void log_periodic_locked();

    AIConfig  cfg_{};
    int       video_w_ = 0, video_h_ = 0, video_fps_ = 0;
    // Sub-sampling interval in ms; 0 means "process every frame".
    int64_t   interval_ms_ = 0;

    std::unique_ptr<IDetector> detector_;
    std::unique_ptr<ITracker>  tracker_;

    // Current loaded mode. Written only by the AI thread; read atomically.
    std::atomic<AIMode> current_mode_{AIMode::Detect};
    // Pending mode request, guarded by mtx_ (set by request_mode, consumed by
    // the AI thread in thread_loop).
    std::optional<AIMode> pending_mode_;

    mutable std::mutex       mtx_;
    std::condition_variable  cv_;
    std::deque<AIFrame>      queue_;
    std::atomic<bool>        running_{false};
    std::thread              thread_;
    // Consecutive inference failures; used to escalate the log level without
    // spamming. The thread is never terminated because of it (spec 3.1).
    std::atomic<int>         consecutive_failures_{0};

    // Pacing (sub-sampled mode only)
    std::chrono::steady_clock::time_point last_due_{};

    // Statistics (guarded by mtx_)
    uint64_t dropped_     = 0;   // discarded on push because the queue was full
    uint64_t skipped_     = 0;   // superseded by a newer frame while sub-sampling
    uint64_t processed_   = 0;
    uint64_t objects_     = 0;
    double   sum_infer_ms_ = 0.0;
    double   sum_track_ms_ = 0.0;
    uint64_t stat_frames_ = 0;
    std::chrono::steady_clock::time_point stat_window_start_{};
    double   last_fps_     = 0.0;
    double   last_infer_ms_ = 0.0;
    double   last_track_ms_ = 0.0;
    std::chrono::steady_clock::time_point last_log_{};

    ResultCallback result_cb_;
};

} // namespace ca
