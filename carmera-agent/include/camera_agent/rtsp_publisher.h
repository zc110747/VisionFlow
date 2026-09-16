#pragma once

#include <memory>
#include <string>

namespace ca {

struct RtspLocation {
    std::string server = "127.0.0.1";
    int         port = 8554;
    std::string stream = "camera01";
};

// Abstract RTSP publisher. The Camera Agent is an RTSP *publisher* only;
// it does NOT implement an RTSP server. It pushes the encoded stream to a
// remote RTSP server (e.g. rtspclientsink on GStreamer).
class RtspPublisher {
public:
    static std::unique_ptr<RtspPublisher> create();

    virtual ~RtspPublisher() = default;

    // Build the full RTSP URL from location parts.
    virtual std::string build_url(const RtspLocation& loc) const = 0;

    // Connect (push) to the given RTSP URL. Returns false if it cannot connect.
    virtual bool connect(const std::string& url) = 0;

    // Disconnect from the server.
    virtual void disconnect() = 0;

    // Whether currently connected to the server.
    virtual bool is_connected() const = 0;
};

} // namespace ca
