#pragma once

// Transport abstraction (spec 16).
//
//   IMetadataTransport
//       └── HttpMetadataTransport   (WinHTTP on Windows; zero extra dependency)
//       └── (future) WebSocketMetadataTransport / MqttMetadataTransport
//
// The transport owns ONLY the network. Encoding, queueing, retries and stats
// live in MetadataManager, so swapping the wire protocol never touches the AI
// branch.

#include <memory>
#include <string>

#include "camera_agent/metadata/metadata_types.h"

namespace ca {

class IMetadataTransport {
public:
    virtual ~IMetadataTransport() = default;

    // Backend name for logs, e.g. "http(winhttp)" / "none".
    virtual const char* name() const = 0;

    // Establish the long-lived session/connection. Idempotent: returns true
    // immediately when already connected.
    virtual bool connect() = 0;
    virtual void close()   = 0;
    virtual bool connected() const = 0;

    // Send one already-encoded payload. Blocks up to `timeout_ms`.
    // On success sets *latency_ms to the round trip time.
    virtual bool send(const std::string& payload, int timeout_ms,
                      double* latency_ms) = 0;
};

// Creates the default transport for this platform. Never returns nullptr.
std::unique_ptr<IMetadataTransport> create_metadata_transport(const MetadataConfig& cfg);

} // namespace ca
