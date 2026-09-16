#include "camera_agent/video_pipeline.h"
#include "camera_agent/logger.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace ca {

// SIM backend: no real GStreamer pipeline. Drives the same lifecycle/state
// machine as the real backend so the orchestration, statistics and reconnect
// logic can be exercised headlessly.
class SimVideoPipeline : public VideoPipeline {
public:
    bool build(const PipelineParams& p, const std::string& rtsp_url) override {
        if (p.width <= 0 || p.height <= 0 || p.fps <= 0) {
            CA_LOG_ERROR("[sim] invalid params: {}x{} @{}fps", p.width, p.height, p.fps);
            return false;
        }
        params_ = p;
        url_ = rtsp_url;
        CA_LOG_INFO("[sim] build: {}x{} @{}fps enc={} br={}kbps -> {}",
                    p.width, p.height, p.fps, p.encoder, p.bitrate, rtsp_url);
        built_ = true;
        return true;
    }

    bool start() override {
        if (!built_) return false;
        running_ = true;
        set_status(StreamStatus::STREAMING);
        worker_ = std::thread(&SimVideoPipeline::run, this);
        CA_LOG_INFO("[sim] streaming started");
        return true;
    }

    void stop() override {
        running_ = false;
        if (worker_.joinable()) worker_.join();
        set_status(StreamStatus::DISCONNECTED);
        CA_LOG_INFO("[sim] streaming stopped");
    }

    Statistics get_stats() const override { return stats_; }
    bool is_running() const override { return running_; }
    StreamStatus get_status() const override { return status_.load(); }

    void set_status_callback(StatusCallback cb) override { cb_ = std::move(cb); }

    bool check_plugins(std::vector<std::string>*) override { return true; }

    void simulate_link_lost() override {
        CA_LOG_WARN("[sim] RTSP link lost (simulated)");
        set_status(StreamStatus::DISCONNECTED);
    }

private:
    void set_status(StreamStatus s) {
        status_ = s;
        if (cb_) cb_(s);
    }

    void run() {
        auto last = std::chrono::steady_clock::now();
        while (running_) {
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            if (dt >= 1.0 / static_cast<double>(params_.fps > 0 ? params_.fps : 1)) {
                const uint64_t n =
                    static_cast<uint64_t>(dt * params_.fps);
                stats_.frames += n;
                stats_.bitrate_kbps = static_cast<double>(params_.bitrate);
                last = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    PipelineParams          params_{};
    std::string             url_;
    bool                    built_ = false;
    bool                    running_ = false;
    std::atomic<StreamStatus> status_{StreamStatus::DISCONNECTED};
    Statistics              stats_{};
    StatusCallback          cb_;
    std::thread             worker_;
};

std::unique_ptr<VideoPipeline> VideoPipeline::create() {
    return std::make_unique<SimVideoPipeline>();
}

} // namespace ca
