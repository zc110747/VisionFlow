#include "camera_agent/metadata/metadata_transport.h"
#include "camera_agent/logger.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#include <chrono>
#include <string>

namespace ca {
namespace {

#ifdef _WIN32

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        &out[0], n);
    return out;
}

// HTTP POST transport on WinHTTP (OS component - no extra dependency to install).
class HttpMetadataTransport : public IMetadataTransport {
public:
    explicit HttpMetadataTransport(MetadataConfig cfg) : cfg_(std::move(cfg)) {}
    ~HttpMetadataTransport() override { close(); }

    const char* name() const override { return "http(winhttp)"; }

    bool connect() override {
        if (connected_) return true;
        if (!parsed_ && !parse_url()) {
            CA_LOG_ERROR("[METADATA] invalid server_url: {}", cfg_.server_url);
            return false;
        }
        session_ = WinHttpOpen(L"camera-agent/1.0",
                               WINHTTP_ACCESS_TYPE_NO_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) return false;
        WinHttpSetTimeouts(session_, cfg_.timeout_ms, cfg_.timeout_ms,
                           cfg_.timeout_ms, cfg_.timeout_ms);
        conn_ = WinHttpConnect(session_, host_.c_str(),
                               static_cast<INTERNET_PORT>(port_), 0);
        if (!conn_) { close(); return false; }
        connected_ = true;
        return true;
    }

    void close() override {
        if (conn_)    { WinHttpCloseHandle(conn_);    conn_ = nullptr; }
        if (session_) { WinHttpCloseHandle(session_); session_ = nullptr; }
        connected_ = false;
    }

    bool connected() const override { return connected_; }

    bool send(const std::string& payload, int timeout_ms, double* latency_ms) override {
        const auto t0 = std::chrono::steady_clock::now();
        if (latency_ms) *latency_ms = 0.0;
        if (!connect()) return false;

        const DWORD flags = secure_ ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET req = WinHttpOpenRequest(conn_, L"POST", path_.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!req) { connected_ = false; return false; }
        if (timeout_ms > 0)
            WinHttpSetTimeouts(req, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

        // Dev-only escape hatch: a metadata server fronted by a self-signed
        // certificate cannot pass WinHTTP's default chain validation. When the
        // operator explicitly opts in (skip_tls_verify), relax the checks so the
        // POST can succeed. This is a no-op for plain http and for https with a
        // valid certificate, so the secure default is never weakened silently.
        if (secure_ && cfg_.skip_tls_verify) {
            DWORD sec = SECURITY_FLAG_IGNORE_CERT_CN_INVALID
                      | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
                      | SECURITY_FLAG_IGNORE_UNKNOWN_CA;
            WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
        }

        const std::wstring hdr = L"Content-Type: application/json";
        bool ok = false;
        if (WinHttpSendRequest(req, hdr.c_str(), static_cast<DWORD>(hdr.size()),
                               const_cast<char*>(payload.data()),
                               static_cast<DWORD>(payload.size()),
                               static_cast<DWORD>(payload.size()), 0)) {
            if (WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0;
                DWORD sz = sizeof(status);
                if (WinHttpQueryHeaders(req,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                        WINHTTP_NO_HEADER_INDEX)) {
                    ok = (status >= 200 && status < 300);
                    if (!ok)
                        CA_LOG_WARN("[METADATA] server replied HTTP {}", status);
                }
                // Drain the response body so the connection stays reusable.
                char  buf[512];
                DWORD rd = 0;
                while (WinHttpReadData(req, buf, static_cast<DWORD>(sizeof(buf)),
                                       &rd) && rd > 0) { /* discard */ }
            }
        }
        WinHttpCloseHandle(req);

        const double dt = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        if (latency_ms) *latency_ms = dt;
        // A failed round trip invalidates the session: the manager will retry
        // with backoff. Nothing here throws, so the AI/video branch is safe.
        if (!ok) connected_ = false;
        return ok;
    }

private:
    bool parse_url() {
        parsed_ = true;
        const std::wstring url = utf8_to_wide(cfg_.server_url);
        URL_COMPONENTSW comp{};
        comp.dwStructSize       = sizeof(comp);
        comp.dwSchemeLength     = static_cast<DWORD>(-1);
        comp.dwHostNameLength   = static_cast<DWORD>(-1);
        comp.dwUrlPathLength    = static_cast<DWORD>(-1);
        comp.dwExtraInfoLength  = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &comp))
            return false;
        if (!comp.lpszHostName || comp.dwHostNameLength == 0) return false;
        host_.assign(comp.lpszHostName, comp.dwHostNameLength);
        if (comp.lpszUrlPath && comp.dwUrlPathLength > 0)
            path_.assign(comp.lpszUrlPath, comp.dwUrlPathLength);
        if (path_.empty()) path_ = L"/";
        if (comp.lpszExtraInfo && comp.dwExtraInfoLength > 0)
            CA_LOG_WARN("[METADATA] query strings are not supported and will be dropped");
        port_   = static_cast<int>(comp.nPort);
        secure_ = (comp.nScheme == INTERNET_SCHEME_HTTPS);
        return true;
    }

    MetadataConfig cfg_;
    bool        parsed_ = false;
    bool        secure_ = false;
    int         port_   = 80;
    std::wstring host_, path_;
    HINTERNET   session_ = nullptr, conn_ = nullptr;
    bool        connected_ = false;
};

#else // !_WIN32

// Placeholder for non-Windows targets (future RK3568 build). Keeping the symbol
// here means the rest of the metadata layer compiles unchanged.
class NullMetadataTransport : public IMetadataTransport {
public:
    const char* name() const override { return "none"; }
    bool connect() override { return false; }
    void close()   override {}
    bool connected() const override { return false; }
    bool send(const std::string&, int, double*) override { return false; }
};

#endif

} // namespace

std::unique_ptr<IMetadataTransport> create_metadata_transport(const MetadataConfig& cfg) {
#ifdef _WIN32
    return std::unique_ptr<IMetadataTransport>(new HttpMetadataTransport(cfg));
#else
    (void)cfg;
    return std::unique_ptr<IMetadataTransport>(new NullMetadataTransport());
#endif
}

} // namespace ca
