#pragma once

// A blocking, one-request-one-reply client for the "JSON Lines" wire format
// used by MeArmPilot: one compact JSON object per line, LF terminated, strictly
// one reply per request (see docs/protocol/README.md in the MeArmPilot repo).
//
// This is deliberately NOT an HTTP client. The existing http_client.h is
// WinHTTP and cannot be reused: MeArmPilot's control interface is a raw TCP
// socket carrying JSON Lines, and there is no request method, path or header
// anywhere in the protocol.
//
// The interface exists so the mapping/rate logic (RobotLink) can be unit tested
// with a scripted fake - no socket, no backend process, no timing flakiness.
// That mirrors how IMetadataTransport lets the metadata branch be tested.

#include <string>

namespace ca {

// A parsed reply. `ok` mirrors the protocol's own "ok" field; `raw` is kept
// verbatim so a failure can be logged with the server's exact wording (the
// error TEXT is part of the protocol contract - "zero displacement" is a
// documented, non-fatal case that the caller must recognise).
struct JsonReply {
    bool        ok = false;
    std::string raw;    // full reply line, as received (for logging)
    std::string error;  // value of "error" when ok == false
};

class IJsonLineClient {
public:
    virtual ~IJsonLineClient() = default;

    // Connect / reconnect. Safe to call repeatedly; returns false when the
    // peer is unreachable (the caller keeps its own state and retries later).
    virtual bool connect() = 0;

    // Drop the connection (idempotent).
    virtual void close() = 0;

    virtual bool connected() const = 0;

    // Send one request line and read exactly one reply line.
    //
    // Returns false when the exchange could not be completed at all (not
    // connected, timeout, peer closed). A reply that is well-formed JSON but
    // carries ok:false returns TRUE and fills `error` - a rejected command is a
    // normal outcome of this protocol, not a transport failure.
    virtual bool exchange(const std::string& request, JsonReply& reply) = 0;
};

// Create the real TCP implementation. Never returns null; a construction
// failure surfaces on the first connect(). The caller owns the pointer.
IJsonLineClient* create_json_line_client(const std::string& host, int port,
                                         int timeout_ms);

} // namespace ca
