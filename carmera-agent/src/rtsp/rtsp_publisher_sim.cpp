#include "camera_agent/rtsp_publisher.h"
#include "camera_agent/logger.h"

#include <memory>
#include <sstream>

namespace ca {

// SIM backend: builds the RTSP URL correctly (so URL construction is testable)
// but does not open a real network connection.
class SimRtspPublisher : public RtspPublisher {
public:
    std::string build_url(const RtspLocation& loc) const override {
        std::ostringstream os;
        os << "rtsp://" << loc.server << ":" << loc.port << "/" << loc.stream;
        return os.str();
    }

    bool connect(const std::string& url) override {
        CA_LOG_INFO("[sim] rtsp connect -> {}", url);
        url_ = url;
        connected_ = true;
        return true;
    }

    void disconnect() override {
        connected_ = false;
        CA_LOG_INFO("[sim] rtsp disconnected");
    }

    bool is_connected() const override { return connected_; }

private:
    std::string url_;
    bool        connected_ = false;
};

std::unique_ptr<RtspPublisher> RtspPublisher::create() {
    return std::make_unique<SimRtspPublisher>();
}

} // namespace ca
