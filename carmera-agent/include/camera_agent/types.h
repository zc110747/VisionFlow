#pragma once

#include <cstdint>
#include <string>

namespace ca {

// ---- Stream / camera status -------------------------------------------------

enum class StreamStatus {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    STREAMING,
    ERROR
};

enum class CameraStatus {
    CLOSED,
    OPENING,
    OPEN,
    ERROR
};

inline const char* to_string(StreamStatus s) {
    switch (s) {
        case StreamStatus::DISCONNECTED: return "DISCONNECTED";
        case StreamStatus::CONNECTING:   return "CONNECTING";
        case StreamStatus::CONNECTED:    return "CONNECTED";
        case StreamStatus::STREAMING:    return "STREAMING";
        case StreamStatus::ERROR:        return "ERROR";
    }
    return "UNKNOWN";
}

inline const char* to_string(CameraStatus s) {
    switch (s) {
        case CameraStatus::CLOSED:  return "CLOSED";
        case CameraStatus::OPENING: return "OPENING";
        case CameraStatus::OPEN:    return "OPEN";
        case CameraStatus::ERROR:   return "ERROR";
    }
    return "UNKNOWN";
}

// ---- Statistics -------------------------------------------------------------

struct Statistics {
    uint64_t frames = 0;        // frames produced / encoded
    uint64_t dropped = 0;       // frames dropped (e.g. underflow / send fail)
    double   bitrate_kbps = 0.0; // measured encoder output bitrate
};

// ---- Device simulation info (future RK3568 uses the same concept) ----------

struct DeviceInfo {
    std::string     device_id = "camera01";
    std::string     device_name = "Camera Agent";
    std::string     firmware_version = "0.1.0";
    CameraStatus    camera_status = CameraStatus::CLOSED;
    StreamStatus    stream_status = StreamStatus::DISCONNECTED;
    int             width = 0;
    int             height = 0;
    int             fps = 0;
    int             bitrate_kbps = 0;
};

} // namespace ca
