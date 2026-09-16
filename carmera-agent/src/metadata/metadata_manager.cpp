#include "camera_agent/metadata/metadata_manager.h"
#include "camera_agent/metadata/metadata_encoder.h"
#include "camera_agent/logger.h"

#include <algorithm>
#include <utility>

namespace ca {
namespace {

constexpr int      kLogIntervalMs      = 5000;
constexpr int      kFailureEscalate    = 10;    // log every Nth consecutive failure
constexpr int64_t  kIdlePollMs         = 200;   // wake-up period with no heartbeat

int64_t hb_period_ms(const MetadataConfig& c) {
    return (c.heartbeat_interval_sec > 0)
               ? static_cast<int64_t>(c.heartbeat_interval_sec) * 1000
               : 0;
}

} // namespace

MetadataManager::~MetadataManager() { stop(); }

bool MetadataManager::init(const MetadataConfig& cfg) {
    if (running_.load()) return true;
    cfg_ = cfg;
    if (cfg_.queue_size < 1) cfg_.queue_size = 1;
    if (cfg_.retry_interval_ms < 1) cfg_.retry_interval_ms = 1;

    try {
        transport_ = create_metadata_transport(cfg_);
    } catch (const std::exception& e) {
        CA_LOG_ERROR("[METADATA] transport creation failed: {}", e.what());
        return false;
    } catch (...) {
        CA_LOG_ERROR("[METADATA] transport creation failed");
        return false;
    }
    if (!transport_) {
        CA_LOG_ERROR("[METADATA] no transport available");
        return false;
    }
    CA_LOG_INFO("[METADATA] transport={} url={} queue={} heartbeat={}s",
                transport_->name(), cfg_.server_url, cfg_.queue_size,
                cfg_.heartbeat_interval_sec);
    return true;
}

void MetadataManager::start() {
    if (running_.load() || !transport_) return;
    running_ = true;
    stat_window_start_ = std::chrono::steady_clock::now();
    last_log_          = std::chrono::steady_clock::now();
    next_connect_      = std::chrono::steady_clock::now();
    thread_ = std::thread(&MetadataManager::thread_loop, this);
    CA_LOG_INFO("[METADATA] sender started");
}

void MetadataManager::stop() {
    running_ = false;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (transport_) transport_->close();
}

void MetadataManager::set_status_provider(StatusProvider cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    status_cb_ = std::move(cb);
}

void MetadataManager::set_transport(std::unique_ptr<IMetadataTransport> t) {
    if (running_.load()) return;   // the sender thread owns it once started
    transport_ = std::move(t);
}

// Called from the AI thread: encode + enqueue only. Never blocks, never throws.
void MetadataManager::push_result(const AIFrameResult& r) {
    if (!running_.load() || !transport_) return;

    std::string payload;
    try {
        payload = encode_frame_metadata(r, cfg_);
    } catch (const std::exception& e) {
        CA_LOG_WARN("[METADATA] encode failed: {}", e.what());
        return;
    } catch (...) {
        CA_LOG_WARN("[METADATA] encode failed");
        return;
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        // spec 7: bounded queue, drop the oldest so the newest state survives.
        const size_t limit = static_cast<size_t>(cfg_.queue_size);
        while (queue_.size() >= limit) {
            queue_.pop_front();
            ++dropped_;
        }
        queue_.push_back(std::move(payload));
    }
    cv_.notify_one();
}

void MetadataManager::thread_loop() {
    const int64_t hb = hb_period_ms(cfg_);
    auto next_hb = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(hb > 0 ? hb : kIdlePollMs);

    while (running_.load()) {
        std::string payload;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            if (queue_.empty()) {
                const auto deadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(hb > 0 ? hb : kIdlePollMs);
                cv_.wait_until(lk, deadline, [this] {
                    return !queue_.empty() || !running_.load();
                });
            }
            if (!running_.load()) break;
            if (!queue_.empty()) {
                payload = std::move(queue_.front());
                queue_.pop_front();
            }
        }

        if (!payload.empty()) deliver(payload);

        // ---- periodic status / heartbeat (spec 13) ----
        const auto now = std::chrono::steady_clock::now();
        if (hb > 0 && now >= next_hb) {
            StatusProvider cb;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                cb = status_cb_;
            }
            if (cb) {
                std::string sp;
                try {
                    sp = encode_status_metadata(cb(), cfg_);
                } catch (const std::exception& e) {
                    CA_LOG_WARN("[METADATA] status encode failed: {}", e.what());
                } catch (...) {
                    CA_LOG_WARN("[METADATA] status encode failed");
                }
                if (!sp.empty()) deliver(sp);
            }
            // Resync instead of bursting: a slow send must not queue up
            // catch-up heartbeats.
            next_hb = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(hb);
        }
    }
}

// Sender thread only: record a failed round trip, extend the backoff window and
// count the payload as discarded (spec 10: backoff, never give up, never exit).
// `attempted` separates "we tried and it failed" (failed_) from "we never got
// to try" (dropped_).
void MetadataManager::register_failure(bool attempted) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (attempted) ++failed_;
        else           ++dropped_;
    }
    ++failure_streak_;
    backoff_ms_ = (backoff_ms_ <= 0)
                      ? cfg_.retry_interval_ms
                      : std::min(backoff_ms_ * 2, cfg_.retry_max_interval_ms);
    next_connect_ = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(backoff_ms_);
    offline_ = true;
    if (failure_streak_ == 1 || failure_streak_ % kFailureEscalate == 0) {
        CA_LOG_WARN("[METADATA] reconnecting in {}ms ({} consecutive {})",
                    backoff_ms_, failure_streak_,
                    attempted ? "send failures" : "connect failures");
    }
}

// Sender thread only: backoff gate -> connect -> POST.
void MetadataManager::deliver(const std::string& payload) {
    if (!transport_) return;

    // ---- backoff gate (spec 7 / spec 10) ----
    // Live metadata is worthless once stale, so while the server is unreachable
    // we drop instead of buffering: the queue must never grow without bound.
    // NOTE: WinHTTP connects lazily, so offline_ is only set by a failed round
    // trip (register_failure), never by connect() - see the header comment.
    if (offline_.load() && std::chrono::steady_clock::now() < next_connect_) {
        std::lock_guard<std::mutex> lk(mtx_);
        ++dropped_;
        return;
    }

    // ---- (re)connect ----
    if (!transport_->connected()) {
        bool ok = false;
        try { ok = transport_->connect(); }
        catch (const std::exception& e) {
            CA_LOG_WARN("[METADATA] connect threw: {}", e.what());
        } catch (...) {
            CA_LOG_WARN("[METADATA] connect threw an unknown exception");
        }
        if (!ok) { register_failure(false); return; }
    }

    CA_LOG_DEBUG("[METADATA] sending {} bytes", payload.size());
    if (cfg_.log_payload) CA_LOG_DEBUG("[METADATA] payload: {}", payload);

    double latency = 0.0;
    bool   ok = false;
    try {
        ok = transport_->send(payload, cfg_.timeout_ms, &latency);
    } catch (const std::exception& e) {
        CA_LOG_WARN("[METADATA] send threw: {}", e.what());
    } catch (...) {
        CA_LOG_WARN("[METADATA] send threw an unknown exception");
    }

    if (ok) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ++sent_;
            ++stat_count_;
            sum_latency_ms_ += latency;
            if (offline_) {
                offline_        = false;
                failure_streak_ = 0;
                backoff_ms_     = 0;
                ++reconnect_;
                CA_LOG_INFO("[METADATA] connection restored");
            } else if (!ever_connected_) {
                ever_connected_ = true;
                CA_LOG_INFO("[METADATA] connected to {}", cfg_.server_url);
            }
            log_periodic_locked();
        }
    } else {
        CA_LOG_WARN("[METADATA] send failed");
        register_failure(true);
        std::lock_guard<std::mutex> lk(mtx_);
        log_periodic_locked();
    }
}

// mtx_ must be held.
void MetadataManager::log_periodic_locked() {
    const auto now = std::chrono::steady_clock::now();
    const double win = std::chrono::duration<double, std::milli>(
        now - stat_window_start_).count();
    if (win >= 1000.0) {
        last_fps_ = static_cast<double>(stat_count_) * 1000.0 / win;
        last_latency_ms_ = (stat_count_ > 0)
                               ? sum_latency_ms_ / static_cast<double>(stat_count_)
                               : 0.0;
        stat_count_      = 0;
        sum_latency_ms_  = 0.0;
        stat_window_start_ = now;
    }
    const double since_log = std::chrono::duration<double, std::milli>(
        now - last_log_).count();
    if (since_log >= static_cast<double>(kLogIntervalMs)) {
        last_log_ = now;
        CA_LOG_INFO("[METADATA] fps={:.1f} latency={:.0f}ms queue={} sent={} "
                    "failed={} dropped={} reconnect={}",
                    last_fps_, last_latency_ms_, static_cast<int>(queue_.size()),
                    sent_, failed_, dropped_, reconnect_);
    }
}

MetadataStats MetadataManager::stats() const {
    MetadataStats s;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        s.sent       = sent_;
        s.failed     = failed_;
        s.dropped    = dropped_;
        s.reconnect  = reconnect_;
        s.queue_size = static_cast<int>(queue_.size());
        s.latency_ms = last_latency_ms_;
        s.fps        = last_fps_;
    }
    // connected() alone is not enough: with WinHTTP it stays true after a lazy
    // connect even when the server is down. offline_ is set by the round trip.
    s.connected = transport_ ? (transport_->connected() && !offline_.load()) : false;
    return s;
}

} // namespace ca
