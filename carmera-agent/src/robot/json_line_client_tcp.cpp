// TCP implementation of IJsonLineClient (Windows Winsock).
//
// Wire format (MeArmPilot docs/protocol/README.md):
//   * request  : one compact JSON object, LF terminated
//   * reply    : exactly ONE line, LF terminated, in request order
//
// Blocking send/recv with SO_RCVTIMEO. The whole class is used from a single
// thread (RobotLink's own), which is what makes the "one reply per request"
// assumption safe: no other consumer can steal a line off the socket.

#include "camera_agent/robot/json_line_client.h"

#include "camera_agent/logger.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstring>
#include <mutex>
#include <string>

namespace ca {
namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

void close_socket(socket_t s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

// WSAStartup must happen exactly once per process, before any other Winsock
// call. The HTTP client uses WinHTTP, which manages its own internals, so this
// is the only place that needs it.
bool ensure_winsock() {
#ifdef _WIN32
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA d;
        ok = (::WSAStartup(MAKEWORD(2, 2), &d) == 0);
        if (!ok) {
            CA_LOG_ERROR("[ROBOT] WSAStartup failed");
        }
    });
    return ok;
#else
    return true;
#endif
}

// Extract a top-level string field without a JSON library. The replies we must
// read are flat objects of scalars, and the only two fields inspected are "ok"
// and "error" - exactly the same minimal-parsing policy the rest of this
// codebase already uses for the AI-mode endpoint (extract_json_string).
//
// A full JSON parser would be the wrong trade here: it would be the only reason
// to pull in a dependency, and the protocol's error strings are the part we
// must quote verbatim, so we keep the raw line anyway.
bool extract_bool(const std::string& json, const std::string& key, bool& out) {
    const std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return false;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (json.compare(p, 4, "true") == 0)  { out = true;  return true; }
    if (json.compare(p, 5, "false") == 0) { out = false; return true; }
    return false;
}

std::string extract_string(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    p = json.find('"', p);
    if (p == std::string::npos) return {};
    const size_t start = p + 1;
    const size_t end = json.find('"', start);
    if (end == std::string::npos) return {};
    return json.substr(start, end - start);
}

// ---------------------------------------------------------------- real client
class TcpJsonLineClient : public IJsonLineClient {
public:
    TcpJsonLineClient(std::string host, int port, int timeout_ms)
        : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {}

    ~TcpJsonLineClient() override { close(); }

    bool connect() override {
        if (sock_ != kInvalidSocket) return true;
        if (!ensure_winsock()) return false;

        // getaddrinfo so a hostname works as well as a literal IP.
        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* res = nullptr;
        const std::string port_str = std::to_string(port_);
        if (::getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
            return false;
        }

        socket_t s = kInvalidSocket;
        for (addrinfo* it = res; it; it = it->ai_next) {
            s = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
            if (s == kInvalidSocket) continue;
            if (::connect(s, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) break;
            close_socket(s);
            s = kInvalidSocket;
        }
        ::freeaddrinfo(res);

        if (s == kInvalidSocket) return false;

        // A control link must never let a stalled peer block the caller
        // indefinitely: every recv gets a deadline.
        set_timeout(s);
        // Small commands; do not wait for Nagle to coalesce them.
        BOOL nodelay = TRUE;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        sock_ = s;
        buf_.clear();
        return true;
    }

    void close() override {
        if (sock_ != kInvalidSocket) {
            close_socket(sock_);
            sock_ = kInvalidSocket;
        }
        buf_.clear();
    }

    bool connected() const override { return sock_ != kInvalidSocket; }

    bool exchange(const std::string& request, JsonReply& reply) override {
        reply = JsonReply{};
        if (sock_ == kInvalidSocket) return false;

        const std::string line = request + "\n";
        if (!send_all(line)) {
            close();
            return false;
        }
        if (!recv_line(reply.raw)) {
            close();
            return false;
        }

        bool ok = false;
        if (extract_bool(reply.raw, "ok", ok)) reply.ok = ok;
        if (!reply.ok) reply.error = extract_string(reply.raw, "error");
        return true;
    }

private:
    void set_timeout(socket_t s) {
#ifdef _WIN32
        // On Windows SO_RCVTIMEO/SO_SNDTIMEO take a DWORD in milliseconds.
        DWORD tv = static_cast<DWORD>(timeout_ms_ > 0 ? timeout_ms_ : 2000);
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&tv), sizeof(tv));
        ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv{};
        tv.tv_sec  = (timeout_ms_ > 0 ? timeout_ms_ : 2000) / 1000;
        tv.tv_usec = ((timeout_ms_ > 0 ? timeout_ms_ : 2000) % 1000) * 1000;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    }

    bool send_all(const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            const int n = ::send(sock_, data.data() + sent,
                                 static_cast<int>(data.size() - sent), 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    // Read until one LF is in the buffer. Anything after it stays buffered for
    // the next call - replies are consumed strictly in request order.
    bool recv_line(std::string& out) {
        for (;;) {
            const size_t nl = buf_.find('\n');
            if (nl != std::string::npos) {
                out.assign(buf_, 0, nl);
                buf_.erase(0, nl + 1);
                // Tolerate CRLF in case a proxy or a future server version adds it.
                if (!out.empty() && out.back() == '\r') out.pop_back();
                return true;
            }
            char chunk[4096];
            const int n = ::recv(sock_, chunk, sizeof(chunk), 0);
            if (n <= 0) return false;
            buf_.append(chunk, static_cast<size_t>(n));
            // A runaway peer must not grow this buffer without bound.
            if (buf_.size() > (1u << 20)) return false;
        }
    }

    std::string host_;
    int         port_ = 9100;
    int         timeout_ms_ = 2000;
    socket_t    sock_ = kInvalidSocket;
    std::string buf_;
};

} // namespace

IJsonLineClient* create_json_line_client(const std::string& host, int port,
                                         int timeout_ms) {
    return new TcpJsonLineClient(host, port, timeout_ms);
}

} // namespace ca
