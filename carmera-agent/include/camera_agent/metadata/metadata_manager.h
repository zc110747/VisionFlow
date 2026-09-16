#pragma once

// MetadataManager: the asynchronous tail of the AI branch.
//
//   AI thread --push_result()--> [bounded queue] --sender thread--> Transport --> Server
//
// Threading contract (spec 6 / spec 11):
//   * push_result() runs on the AI thread. It only ENCODES and ENQUEUES, and it
//     never blocks, never touches the network and never throws. A dead server
//     can therefore not stall or stop the AI branch, let alone the video branch.
//   * The sender thread owns all blocking work (connect / POST / backoff).
//   * Any exception is caught here; the video and AI branches are untouched.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "camera_agent/ai/ai_types.h"
#include "camera_agent/metadata/metadata_types.h"
#include "camera_agent/metadata/metadata_transport.h"

namespace ca {

class MetadataManager {
public:
    using StatusProvider = std::function<AIStatusInfo()>;

    ~MetadataManager();

    // Creates the transport. Returns false on any problem -> caller keeps video.
    bool init(const MetadataConfig& cfg);
    void start();
    void stop();
    bool is_running() const { return running_.load(); }

    // Called from the AI thread: encode + enqueue, drop the OLDEST on overflow
    // (spec 7). Non-blocking, never throws.
    void push_result(const AIFrameResult& r);

    // Source of the periodic status/heartbeat message (spec 13). Set before start().
    void set_status_provider(StatusProvider cb);

    // Replace the transport created by init() - used by unit tests to observe
    // the encoded payloads without a real server. Must be called before start().
    void set_transport(std::unique_ptr<IMetadataTransport> t);

    MetadataStats stats() const;

private:
    void thread_loop();
    void deliver(const std::string& payload);   // sender thread only
    // Record a failed round trip, start/extend the backoff window and count the
    // message as discarded. `attempted` distinguishes "we tried and it failed"
    // (failed_) from "we never got to try" (dropped_).
    void register_failure(bool attempted);
    void log_periodic_locked();

    MetadataConfig          cfg_{};
    std::unique_ptr<IMetadataTransport> transport_;

    mutable std::mutex      mtx_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::atomic<bool>       running_{false};
    std::thread             thread_;
    StatusProvider          status_cb_;

    // Backoff state (sender thread only, offline_ is also read by stats())
    // WinHTTP connects lazily: connect() succeeds even when the server is down,
    // so "offline" is only known after a failed send. offline_ therefore drives
    // the backoff window and the reconnect counter.
    std::atomic<bool>    offline_{false};
    bool ever_connected_ = false;
    int  failure_streak_ = 0;
    int  backoff_ms_     = 0;
    std::chrono::steady_clock::time_point next_connect_{};

    // Statistics (guarded by mtx_)
    uint64_t sent_     = 0;
    uint64_t failed_   = 0;   // transmission attempted and failed -> discarded
    uint64_t dropped_  = 0;   // discarded without attempting (queue full / offline)
    uint64_t reconnect_ = 0;
    double   sum_latency_ms_ = 0.0;
    uint64_t stat_count_ = 0;
    double   last_latency_ms_ = 0.0;
    double   last_fps_        = 0.0;
    std::chrono::steady_clock::time_point stat_window_start_{};
    std::chrono::steady_clock::time_point last_log_{};
};

} // namespace ca
