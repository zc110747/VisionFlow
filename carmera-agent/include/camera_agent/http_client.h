#pragma once

#include <string>

namespace ca {

// Synchronous HTTP GET used by the AI-mode poller to read the desired mode
// from the video-server (GET /api/cameras/{id}/aimode).
//
// Returns true on a 2xx response, with the body read into `body_out`.
// Any transport / non-2xx result returns false and clears `body_out`. The
// poller treats both "server down" and "unknown mode" as "keep current model",
// so this function never throws and never blocks the video/AI branch.
bool http_get(const std::string& url, std::string& body_out, int timeout_ms = 2000);

} // namespace ca
