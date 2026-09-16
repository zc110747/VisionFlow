// HTTP GET client (Windows / WinHTTP). OS component - no extra dependency.
// Used by the AI-mode poller to fetch the desired mode from the video-server.

#include "camera_agent/http_client.h"
#include "camera_agent/logger.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif

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

bool winhttp_get(const std::string& url, std::string& body_out, int timeout_ms) {
    body_out.clear();

    const std::wstring wur = utf8_to_wide(url);
    URL_COMPONENTSW comp{};
    comp.dwStructSize     = sizeof(comp);
    comp.dwSchemeLength   = static_cast<DWORD>(-1);
    comp.dwHostNameLength = static_cast<DWORD>(-1);
    comp.dwUrlPathLength  = static_cast<DWORD>(-1);
    comp.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wur.c_str(), static_cast<DWORD>(wur.size()), 0, &comp))
        return false;
    if (!comp.lpszHostName || comp.dwHostNameLength == 0) return false;

    std::wstring host(comp.lpszHostName, comp.dwHostNameLength);
    std::wstring path = (comp.lpszUrlPath && comp.dwUrlPathLength > 0)
                            ? std::wstring(comp.lpszUrlPath, comp.dwUrlPathLength)
                            : std::wstring(L"/");
    const bool secure = (comp.nScheme == INTERNET_SCHEME_HTTPS);
    const int  port   = static_cast<int>(comp.nPort ? comp.nPort
                                                     : (secure ? 443 : 80));

    HINTERNET session = WinHttpOpen(L"camera-agent/1.0",
                                    WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;

    bool ok = false;
    HINTERNET conn = WinHttpConnect(session, host.c_str(),
                                    static_cast<INTERNET_PORT>(port), 0);
    if (conn) {
        const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (req) {
            if (timeout_ms > 0)
                WinHttpSetTimeouts(req, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, sz = sizeof(status);
                if (WinHttpQueryHeaders(req,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                        WINHTTP_NO_HEADER_INDEX)) {
                    if (status >= 200 && status < 300) {
                        // Read the body (capped to a sane size).
                        char  buf[2048];
                        DWORD rd = 0;
                        std::string body;
                        while (WinHttpReadData(req, buf, sizeof(buf), &rd) && rd > 0)
                            body.append(buf, rd);
                        body_out = std::move(body);
                        ok = true;
                    }
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return ok;
}

#else // !_WIN32

bool winhttp_get(const std::string&, std::string&, int) {
    // Placeholder for non-Windows targets (future RK3568 build). The poller is
    // a dev convenience; without an HTTP client the agent simply keeps its
    // startup model.
    return false;
}

#endif

} // namespace

bool http_get(const std::string& url, std::string& body_out, int timeout_ms) {
    return winhttp_get(url, body_out, timeout_ms);
}

} // namespace ca
