#include "camera_agent/metadata/metadata_encoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace ca {
namespace {

// --- tiny JSON helpers (no external JSON library) ---------------------------

void append_escaped(std::string& out, const std::string& s) {
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
}

std::string jstr(const std::string& s) {
    std::string out;
    out += '"';
    append_escaped(out, s);
    out += '"';
    return out;
}

std::string fixed2(double v) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << v;
    return os.str();
}

// std::min/std::max based clamp that tolerates lo > hi (std::clamp would be UB).
int clampi(int v, int lo, int hi) {
    if (hi < lo) hi = lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Clamp a bbox into the video frame (spec 5) and emit "[x1,y1,x2,y2]" as ints.
// Guarantees 0 <= x1 < x2 <= video_width and 0 <= y1 < y2 <= video_height.
std::string bbox_json(const AIObject& o, int w, int h) {
    if (w <= 0 || h <= 0) return "[0,0,0,0]";

    const int ix1 = clampi(static_cast<int>(std::floor(o.x1)), 0, w - 1);
    const int iy1 = clampi(static_cast<int>(std::floor(o.y1)), 0, h - 1);
    const int ix2 = clampi(static_cast<int>(std::ceil(o.x2)),  ix1 + 1, w);
    const int iy2 = clampi(static_cast<int>(std::ceil(o.y2)),  iy1 + 1, h);

    std::ostringstream os;
    os << '[' << ix1 << ',' << iy1 << ',' << ix2 << ',' << iy2 << ']';
    return os.str();
}

// Additive, backward-compatible pose extension: "keypoints":[[x,y,conf],...].
// Coordinates are original-video pixels (floats, 2 decimals), clamped into the
// frame; conf is emitted as-is in [0,1] (already sigmoided by the model).
// Present only when the object carries keypoints (pose model), so detection
// model output is byte-identical to previous versions.
std::string keypoints_json(const AIObject& o, int w, int h) {
    if (o.keypoints.empty()) return {};
    if (w <= 0 || h <= 0) w = h = 0;

    std::ostringstream os;
    os << ",\"keypoints\":[";
    for (size_t j = 0; j < o.keypoints.size(); ++j) {
        const Keypoint& k = o.keypoints[j];
        if (j) os << ',';
        const int x = clampi(static_cast<int>(std::lround(k.x)), 0,
                             w > 0 ? w : 0);
        const int y = clampi(static_cast<int>(std::lround(k.y)), 0,
                             h > 0 ? h : 0);
        os << '[' << fixed2(x) << ',' << fixed2(y) << ','
           << fixed2(k.conf < 0.0f ? 0.0f : (k.conf > 1.0f ? 1.0f : k.conf))
           << ']';
    }
    os << ']';
    return os.str();
}

uint64_t epoch_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

std::string encode_frame_metadata(const AIFrameResult& r, const MetadataConfig& cfg) {
    std::string s;
    s.reserve(256 + r.objects.size() * 96);
    s += "{\"version\":" + std::to_string(cfg.version);
    // spec 14: every message carries a discriminator. It MUST be emitted here -
    // without it a receiver can only guess from the payload shape, and a frame
    // with zero objects is indistinguishable from a malformed heartbeat.
    s += ",\"type\":\"frame\"";
    s += ",\"camera_id\":" + jstr(cfg.camera_id);
    // spec 15: copied verbatim, never regenerated.
    s += ",\"frame_id\":" + std::to_string(r.frame_id);
    s += ",\"timestamp\":" + std::to_string(r.timestamp);
    s += ",\"video_width\":" + std::to_string(r.video_width);
    s += ",\"video_height\":" + std::to_string(r.video_height);
    // Mode the agent actually ran for this frame ("ai-y" / "ai-y-pose"). Older
    // servers ignore the field; the web uses it to drop transition frames.
    if (!r.ai_mode.empty()) {
        s += ",\"ai_mode\":" + jstr(r.ai_mode);
    }
    s += ",\"objects\":[";
    for (size_t i = 0; i < r.objects.size(); ++i) {
        const AIObject& o = r.objects[i];
        if (i) s += ',';
        s += "{\"class\":" + jstr(o.class_name);
        s += ",\"confidence\":" + fixed2(o.confidence);
        s += ",\"track_id\":" + std::to_string(o.track_id);
        s += ",\"bbox\":" + bbox_json(o, r.video_width, r.video_height);
        s += keypoints_json(o, r.video_width, r.video_height);
        s += '}';
    }
    s += "]}";
    return s;
}

std::string encode_status_metadata(const AIStatusInfo& st, const MetadataConfig& cfg) {
    std::string s;
    s.reserve(320);
    s += "{\"version\":" + std::to_string(cfg.version);
    s += ",\"type\":\"status\"";
    s += ",\"camera_id\":" + jstr(cfg.camera_id);
    s += ",\"timestamp\":" + std::to_string(st.last_timestamp);
    s += ",\"ai\":{\"enable\":" + std::string(st.enable ? "true" : "false");
    s += ",\"running\":" + std::string(st.running ? "true" : "false");
    s += ",\"fps\":" + fixed2(st.fps);
    s += ",\"model\":" + jstr(st.model);
    s += ",\"tracker\":" + jstr(st.tracker);
    s += ",\"last_frame_id\":" + std::to_string(st.last_frame_id);
    s += ",\"last_timestamp\":" + std::to_string(st.last_timestamp);
    s += ",\"processed\":" + std::to_string(st.processed);
    if (st.keypoint_count > 0) {
        s += ",\"keypoints\":" + std::to_string(st.keypoint_count);
    }
    s += "}";
    s += ",\"wall_clock\":" + std::to_string(epoch_ms());
    s += "}";
    return s;
}

} // namespace ca
