#pragma once

#include <string>

#include "camera_agent/ai/ai_types.h"
#include "camera_agent/metadata/metadata_types.h"

namespace ca {

struct CameraConfig {
    int         id = 0;
    int         width = 1280;
    int         height = 720;
    int         fps = 30;
    bool        auto_res = false;   // true: don't force caps, let camera negotiate native format
};

struct EncoderConfig {
    std::string codec = "h264";   // h264
    int         bitrate = 4000;   // kbps
    int         keyframe_interval = 30;
};

struct StreamConfig {
    std::string id = "camera01";
};

struct RtspConfig {
    std::string server = "127.0.0.1";
    int         port = 8554;
};

struct Config {
    CameraConfig  camera;
    EncoderConfig encoder;
    StreamConfig  stream;
    RtspConfig    rtsp;
    AIConfig      ai;       // independent AI branch (spec 18)
    MetadataConfig metadata; // async AI result upload (Phase 2)
    std::string   device_id = "camera01";
    std::string   source = "auto";     // camera source element; "auto" or a GStreamer src name (e.g. videotestsrc for testing)
    std::string   log_level = "info";
    bool        measure_latency = false; // --latency-probe: instrument per-stage latency
};

// Build the default configuration (matches camera-agent.yaml defaults).
Config make_default_config();

// Load configuration from a YAML file. Missing keys fall back to defaults.
// Returns true on success (file optional: if absent, defaults are used).
bool load_config(Config& cfg, const std::string& path);

} // namespace ca
