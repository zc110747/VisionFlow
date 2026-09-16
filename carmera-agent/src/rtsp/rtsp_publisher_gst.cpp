// Real RTSP publisher for the GStreamer backend.
//
// In the GStreamer backend the RTSP session is owned by `rtspclientsink`, which
// sits at the end of the video pipeline (see video_pipeline_gst.cpp). The RTSP
// handshake (ANNOUNCE / SETUP / RECORD) happens when that pipeline goes to
// PLAYING, so there is no separate socket for this class to own.
//
// This class is the session-state layer around it:
//   - builds the push URL,
//   - verifies the target server is reachable, so a wrong --server/--port is
//     reported as a clear warning instead of a silently black stream,
//   - tracks connect/disconnect state for status reporting.
//
// Reachability is deliberately NOT fatal: if the server is down we still let
// the pipeline start. rtspclientsink will fail, the pipeline bus reports an
// error, StreamController flips to DISCONNECTED and the backoff reconnect loop
// (1s -> 2s -> 5s -> 10s) takes over. That is the required behaviour - the
// agent must not exit because the video server is temporarily unavailable.

#include "camera_agent/rtsp_publisher.h"
#include "camera_agent/logger.h"

#include <gio/gio.h>
#include <gst/gst.h>

#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>

namespace ca {
namespace {

constexpr int kReachabilityTimeoutSec = 2;

bool element_available(const char* element) {
    GstElementFactory* f = gst_element_factory_find(element);
    if (!f) return false;
    gst_object_unref(f);
    return true;
}

// Split "rtsp://host:port/stream" into host + port.
bool parse_host_port(const std::string& url, std::string& host, int& port) {
    const std::string scheme = "rtsp://";
    if (url.size() <= scheme.size() || url.compare(0, scheme.size(), scheme) != 0)
        return false;

    const size_t path = url.find('/', scheme.size());
    const std::string authority = (path == std::string::npos)
                                      ? url.substr(scheme.size())
                                      : url.substr(scheme.size(), path - scheme.size());
    if (authority.empty()) return false;

    const size_t colon = authority.rfind(':');
    if (colon == std::string::npos) return false;

    host = authority.substr(0, colon);
    const std::string port_str = authority.substr(colon + 1);
    if (host.empty() || port_str.empty()) return false;
    for (char c : port_str) {
        if (c < '0' || c > '9') return false;
    }

    const int p = std::atoi(port_str.c_str());
    if (p <= 0 || p > 65535) return false;
    port = p;
    return true;
}

// Cheap probe: DNS resolution + TCP connect with a short timeout.
// Uses GIO (already linked via gio-2.0) so this stays cross-platform and keeps
// Win32 sockets out of application code.
//
// The proxy resolver is disabled on purpose: we are opening a plain TCP socket
// to a specific host:port, a proxy makes no sense here. It also works around
// broken GIO proxy modules (libgiolibproxy.dll fails to load on some Windows
// installs, which otherwise makes every probe report "unreachable").
bool server_reachable(const std::string& host, int port) {
    GSocketClient* client = g_socket_client_new();
    if (!client) return false;
    g_socket_client_set_timeout(client, kReachabilityTimeoutSec);
    g_socket_client_set_enable_proxy(client, FALSE);

    GError* err = nullptr;
    GSocketConnection* conn = g_socket_client_connect_to_host(
        client, host.c_str(), static_cast<guint16>(port), nullptr, &err);
    if (err) {
        CA_LOG_DEBUG("Reachability probe to {}:{} failed: {}", host, port,
                     err->message ? err->message : "unknown");
        g_error_free(err);
    }
    if (conn) g_object_unref(conn);
    g_object_unref(client);
    return conn != nullptr;
}

class GstRtspPublisher : public RtspPublisher {
public:
    GstRtspPublisher() {
        if (!gst_is_initialized())
            gst_init(nullptr, nullptr);
    }

    std::string build_url(const RtspLocation& loc) const override {
        std::ostringstream os;
        os << "rtsp://" << loc.server << ":" << loc.port << "/" << loc.stream;
        return os.str();
    }

    bool connect(const std::string& url) override {
        if (!element_available("rtspclientsink")) {
            CA_LOG_ERROR("Required GStreamer element 'rtspclientsink' is not installed.");
            return false;
        }

        std::string host;
        int port = 0;
        if (!parse_host_port(url, host, port)) {
            CA_LOG_ERROR("Invalid RTSP URL: {}", url);
            return false;
        }

        url_ = url;
        // The socket itself belongs to rtspclientsink inside the pipeline;
        // from here on we only track session state.
        connected_ = true;

        if (server_reachable(host, port)) {
            CA_LOG_INFO("RTSP server reachable at {}:{} - pushing to {}", host, port, url);
        } else {
            CA_LOG_WARN("RTSP server {}:{} is not reachable yet. The agent will keep "
                        "running and retry with backoff (1s/2s/5s/10s).", host, port);
        }
        return true;
    }

    void disconnect() override {
        if (connected_)
            CA_LOG_INFO("RTSP session released: {}", url_);
        connected_ = false;
    }

    bool is_connected() const override { return connected_; }

private:
    std::string url_;
    bool        connected_ = false;
};

} // namespace

std::unique_ptr<RtspPublisher> RtspPublisher::create() {
    return std::make_unique<GstRtspPublisher>();
}

} // namespace ca
