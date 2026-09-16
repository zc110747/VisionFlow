#pragma once

// Metadata upload contract (Phase 2 / doc/step3.prompter_metadata.md).
//
//   AIFrameResult -> Encoder -> bounded Queue -> Sender thread -> Transport -> Server
//
// Hard rules encoded here:
//  * spec 9  - nothing about the server is hard-coded: url / camera id / queue
//              depth / timeouts / heartbeat period are all configuration.
//  * spec 14 - the wire format carries a `version` so it can evolve.
//  * spec 15 - frame_id / timestamp are COPIED from AIFrameResult. The sender
//              must never regenerate them (see metadata_encoder.h).

#include <cstdint>
#include <string>

namespace ca {

struct MetadataConfig {
    bool        enable      = false;
    // Default points at the video-server's default HTTP port (8080, see
    // video-server/config/config.yaml). It was 8000 before, which collided with
    // MediaMTX's RTP port and silently dropped every AI frame - the agent kept
    // running while the web UI never saw any detection. Keep this in lock-step
    // with the server's server.http_port.
    std::string server_url  = "http://127.0.0.1:8080/api/metadata";
    std::string camera_id   = "camera01";
    int         version     = 1;      // protocol version on the wire (spec 14)

    int  queue_size              = 8;  // bounded; oldest is dropped on overflow (spec 7)
    int  timeout_ms              = 2000;
    int  retry_interval_ms       = 1000;   // first retry delay
    int  retry_max_interval_ms   = 30000;  // exponential backoff ceiling
    int  heartbeat_interval_sec  = 10;     // 0 = disable the status heartbeat

    // Full JSON payloads are only dumped at DEBUG level (spec 17).
    bool log_payload = false;

    // TLS server-certificate verification. Secure by default: when the metadata
    // endpoint uses https, WinHTTP validates the server certificate chain. Set
    // this to true ONLY for a dev server with a self-signed certificate (ignores
    // unknown CA / CN / expiry). Never enable in production.
    bool skip_tls_verify = false;
};

// Runtime statistics of the metadata sender (spec 18).
struct MetadataStats {
    double   fps         = 0.0;  // messages actually sent per second
    double   latency_ms  = 0.0;  // rolling mean of one HTTP round trip
    uint64_t sent        = 0;
    uint64_t failed      = 0;
    uint64_t dropped     = 0;    // discarded: queue full or send failed
    uint64_t reconnect   = 0;
    int      queue_size  = 0;
    bool     connected   = false;
};

// AI branch health, published as the periodic status/heartbeat message (spec 13).
// The project has no separate heartbeat mechanism, so the metadata link doubles
// as the liveness signal: an empty `objects` list is still sent every AI frame.
struct AIStatusInfo {
    bool        enable         = false;
    bool        running        = false;
    double      fps            = 0.0;
    std::string model;
    std::string tracker        = "bytetrack";
    uint64_t    last_frame_id  = 0;
    uint64_t    last_timestamp = 0;   // ms, video time base (from AIFrameResult)
    uint64_t    processed      = 0;
    // 0 = detection model; >0 = pose model with this many keypoints per
    // object (emitted in the heartbeat so receivers can adapt their UI).
    int         keypoint_count = 0;
};

} // namespace ca
