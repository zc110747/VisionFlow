#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "camera_agent/types.h"
#include "camera_agent/config.h"
#include "camera_agent/backoff.h"
#include "camera_agent/video_pipeline.h"
#include "camera_agent/ai/ai_pipeline.h"
#include "camera_agent/metadata/metadata_manager.h"

namespace ca {

class VideoPipeline;
class RtspPublisher;

// Request the active run loop to stop (called from a SIGINT handler).
void request_app_stop();

// Orchestrates one camera stream: builds the pipeline + publisher, manages the
// run loop, and exposes status/statistics/device-info for display and tests.
// Reconnection (Phase 6) is layered on top of start()/stop() and status watching.
class StreamController {
public:
    explicit StreamController(Config cfg);
    ~StreamController();

    // Start capture + encode + RTSP push. Returns false on a fatal init error.
    bool start();

    // Stop everything and release resources.
    void stop();

    // Block until `duration_sec` elapse, or until stop() is called externally
    // (e.g. by a Ctrl+C handler). duration_sec <= 0 means run until stopped.
    void run_blocking(double duration_sec = 0.0);

    StreamStatus get_status() const;
    Statistics   get_stats() const;
    DeviceInfo   get_device_info() const;

    const std::string& rtsp_url() const { return rtsp_url_; }

    // Enable/disable auto-reconnect (default on). Useful to turn off in tests.
    void set_reconnect_enabled(bool on) { reconnect_enabled_ = on; }
    // Override the backoff schedule (seconds) - used by tests to avoid long waits.
    void set_reconnect_schedule(const std::vector<int>& s) { backoff_ = BackoffScheduler(s); }

    // (Test/debug) Force the underlying link to drop, exercising reconnect.
    void simulate_link_lost();

    // ---- AI branch --------------------------------------------------------
    // Starts the independent AI pipeline after the video pipeline is up and the
    // capture format is known. Any failure here is logged and swallowed: the
    // caller keeps a fully functional video stream (spec 3.1 / 19).
    void start_ai();

    // Runtime AI-mode poller: reads the desired mode from the video-server and
    // requests a model swap on change. Started only when cfg_.ai.aimode_poll.
    void start_aimode_poller();
    void aimode_poll_loop();

    // Access to the AI branch (never null; enable=false -> idle object).
    AIPipeline&       ai()       { return ai_; }
    const AIPipeline& ai() const { return ai_; }

    // ---- Metadata branch --------------------------------------------------
    // Starts the asynchronous metadata sender (Phase 2). Independent of both the
    // video and the AI branch: failures only produce WARN logs.
    void start_metadata();

    MetadataManager&       metadata()       { return meta_; }
    const MetadataManager& metadata() const { return meta_; }

private:
    void on_status(StreamStatus s);
    void reconnect_loop();
    void internal_restart();   // stop + rebuild + start, without flipping running_

    Config                        cfg_;
    std::unique_ptr<VideoPipeline> pipeline_;
    std::unique_ptr<RtspPublisher> publisher_;
    std::atomic<StreamStatus>     status_{StreamStatus::DISCONNECTED};
    std::string                   rtsp_url_;
    PipelineParams                pp_{};
    bool                          running_ = false;

    bool                          reconnect_enabled_ = true;
    BackoffScheduler              backoff_;
    std::thread                   reconnect_thread_;
    std::atomic<bool>             reconnecting_{false};

    // AI-mode poller thread (web UI -> video-server -> model swap).
    std::thread                   aimode_thread_;
    int                           attempt_ = 0;

    // A STREAMING report right after PLAYING is not proof the RTSP server is
    // actually reachable (rtspclientsink connects asynchronously and the
    // pipeline flips to PLAYING before the handshake completes). We only reset
    // the backoff/attempt counter once STREAMING has been stable for this many
    // seconds, so a brief false-STREAMING during a server outage does not wipe
    // the escalating 1/2/5/10s backoff.
    static constexpr double kStableGraceSec = 3.0;
    std::chrono::steady_clock::time_point last_disconnect_ =
        std::chrono::steady_clock::now();

    // Metadata branch: owns its own thread and bounded queue. Optional like the
    // AI branch; declared BEFORE ai_ so that it outlives it (the AI thread is
    // the producer that pushes into it).
    MetadataManager meta_;

    // AI branch: owns its own thread and bounded queue. It is completely
    // optional - if it never starts, nothing else changes.
    AIPipeline ai_;

    // Last AI frame seen - mirrored here so the metadata heartbeat can report
    // LAST_FRAME_ID / LAST_TIMESTAMP (spec 13) without touching AI internals.
    std::atomic<uint64_t> last_ai_frame_id_{0};
    std::atomic<uint64_t> last_ai_ts_{0};
};

} // namespace ca
