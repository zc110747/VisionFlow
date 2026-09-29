#include "camera_agent/config.h"
#include "camera_agent/logger.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace ca {

namespace {

// --- Minimal YAML reader (supports nested maps + scalar values) -------------
struct YNode {
    enum Type { MAP, SCALAR } type = MAP;
    std::string scalar;
    std::map<std::string, YNode> children;
};

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        return s.substr(1, s.size() - 2);
    return s;
}

int indent_of(const std::string& line) {
    int n = 0;
    while (n < static_cast<int>(line.size()) &&
           (line[n] == ' ' || line[n] == '\t')) ++n;
    return n;
}

// Parse a very small YAML subset used by camera-agent.yaml.
YNode parse_yaml(std::istream& in) {
    YNode root;
    root.type = YNode::MAP;
    // stack of (indent, node*) — top is the current parent map.
    std::vector<std::pair<int, YNode*>> stack;
    stack.push_back({-1, &root});

    std::string raw;
    while (std::getline(in, raw)) {
        // strip trailing CR and inline comments (outside quotes - good enough here)
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;

        // cut comment (naive: first '#' not inside quotes)
        size_t hash = line.find('#');
        if (hash != std::string::npos) {
            // only treat as comment if preceded by whitespace
            if (hash == 0 || std::isspace(static_cast<unsigned char>(line[hash - 1])))
                line = trim(line.substr(0, hash));
            if (line.empty()) continue;
        }

        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        const std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));
        const int indent = indent_of(raw);

        // pop stack until we find a parent with smaller indent
        while (stack.size() > 1 && stack.back().first >= indent)
            stack.pop_back();
        YNode* parent = stack.back().second;

        YNode node;
        if (val.empty()) {
            node.type = YNode::MAP;
            parent->children[key] = node;
            stack.push_back({indent, &parent->children[key]});
        } else {
            node.type = YNode::SCALAR;
            node.scalar = strip_quotes(val);
            parent->children[key] = node;
        }
    }
    return root;
}

const YNode* find(const YNode& root, const std::string& key) {
    auto it = root.children.find(key);
    return (it == root.children.end()) ? nullptr : &it->second;
}

int as_int(const YNode* n, int def) {
    if (!n || n->type != YNode::SCALAR) return def;
    return std::atoi(n->scalar.c_str());
}
bool as_bool(const YNode* n, bool def) {
    if (!n || n->type != YNode::SCALAR) return def;
    const std::string s = n->scalar;
    return (s == "true" || s == "True" || s == "1" || s == "yes" || s == "YES");
}
std::string as_str(const YNode* n, const std::string& def) {
    if (!n || n->type != YNode::SCALAR) return def;
    return n->scalar;
}
float as_float(const YNode* n, float def) {
    if (!n || n->type != YNode::SCALAR) return def;
    return static_cast<float>(std::atof(n->scalar.c_str()));
}

} // namespace

Config make_default_config() {
    return Config{};
}

bool load_config(Config& cfg, const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        CA_LOG_WARN("Config file not found: {} (using defaults / CLI overrides)", path);
        return false;
    }

    YNode root = parse_yaml(f);

    if (auto* cam = find(root, "camera")) {
        cfg.camera.id     = as_int(find(*cam, "id"), cfg.camera.id);
        cfg.camera.width  = as_int(find(*cam, "width"), cfg.camera.width);
        cfg.camera.height = as_int(find(*cam, "height"), cfg.camera.height);
        cfg.camera.fps    = as_int(find(*cam, "fps"), cfg.camera.fps);
        cfg.camera.auto_res = as_bool(find(*cam, "auto"), cfg.camera.auto_res);
    }
    if (auto* enc = find(root, "encoder")) {
        cfg.encoder.codec            = as_str(find(*enc, "codec"), cfg.encoder.codec);
        cfg.encoder.bitrate          = as_int(find(*enc, "bitrate"), cfg.encoder.bitrate);
        cfg.encoder.keyframe_interval = as_int(find(*enc, "keyframe_interval"), cfg.encoder.keyframe_interval);
    }
    if (auto* st = find(root, "stream"))
        cfg.stream.id = as_str(find(*st, "id"), cfg.stream.id);
    if (auto* rtsp = find(root, "rtsp")) {
        cfg.rtsp.server = as_str(find(*rtsp, "server"), cfg.rtsp.server);
        cfg.rtsp.port   = as_int(find(*rtsp, "port"), cfg.rtsp.port);
    }
    // ---- AI branch (spec 18): all parameters configurable, none hard-coded ----
    if (auto* n = find(root, "ai")) {
        cfg.ai.enable       = as_bool(find(*n, "enable"), cfg.ai.enable);
        cfg.ai.fps          = clamp_ai_fps(as_int(find(*n, "fps"), cfg.ai.fps));
        cfg.ai.confidence   = as_float(find(*n, "confidence"), cfg.ai.confidence);
        cfg.ai.model        = as_str(find(*n, "model"), cfg.ai.model);
        cfg.ai.input_width  = as_int(find(*n, "input_width"), cfg.ai.input_width);
        cfg.ai.input_height = as_int(find(*n, "input_height"), cfg.ai.input_height);
        cfg.ai.queue_size   = as_int(find(*n, "queue_size"), cfg.ai.queue_size);
        cfg.ai.nms_threshold   = as_float(find(*n, "nms_threshold"), cfg.ai.nms_threshold);
        cfg.ai.match_threshold = as_float(find(*n, "match_threshold"), cfg.ai.match_threshold);
        cfg.ai.track_buffer    = as_int(find(*n, "track_buffer"), cfg.ai.track_buffer);
        cfg.ai.low_confidence  = as_float(find(*n, "low_confidence"), cfg.ai.low_confidence);
        cfg.ai.full_rate_below_fps =
            as_int(find(*n, "full_rate_below_fps"), cfg.ai.full_rate_below_fps);
        cfg.ai.log_objects = as_bool(find(*n, "log_objects"), cfg.ai.log_objects);
        cfg.ai.num_threads = as_int(find(*n, "num_threads"), cfg.ai.num_threads);
        cfg.ai.model_pose      = as_str(find(*n, "model_pose"), cfg.ai.model_pose);
        cfg.ai.aimode_poll     = as_bool(find(*n, "aimode_poll"), cfg.ai.aimode_poll);
        cfg.ai.aimode_poll_ms  = as_int(find(*n, "aimode_poll_ms"), cfg.ai.aimode_poll_ms);
        cfg.ai.aimode_base_url = as_str(find(*n, "aimode_base_url"), cfg.ai.aimode_base_url);

        // Mode the branch starts in. An unrecognised token keeps the default
        // instead of aborting the load - a typo in a config file must not cost
        // the operator the whole run.
        const std::string im = as_str(find(*n, "initial_mode"), "");
        if (!im.empty()) {
            AIMode parsed;
            if (aimode_from_string(im, parsed)) {
                cfg.ai.initial_mode = parsed;
            } else {
                CA_LOG_WARN("[config] ai.initial_mode '{}' is not a known mode "
                            "(detect|pose|hand); keeping '{}'",
                            im, to_string(cfg.ai.initial_mode));
            }
        }

        // ---- hand mode (Phase 1) ------------------------------------------
        if (auto* h = find(*n, "hand")) {
            cfg.ai.hand.model_palm    = as_str(find(*h, "model_palm"), cfg.ai.hand.model_palm);
            cfg.ai.hand.model_landmark =
                as_str(find(*h, "model_landmark"), cfg.ai.hand.model_landmark);
            cfg.ai.hand.palm_score_threshold =
                as_float(find(*h, "palm_score_threshold"), cfg.ai.hand.palm_score_threshold);
            cfg.ai.hand.palm_nms_threshold =
                as_float(find(*h, "palm_nms_threshold"), cfg.ai.hand.palm_nms_threshold);
            cfg.ai.hand.landmark_confidence =
                as_float(find(*h, "landmark_confidence"), cfg.ai.hand.landmark_confidence);
            cfg.ai.hand.max_hands  = as_int(find(*h, "max_hands"), cfg.ai.hand.max_hands);
            cfg.ai.hand.filter_alpha =
                as_float(find(*h, "filter_alpha"), cfg.ai.hand.filter_alpha);
            cfg.ai.hand.motion_deadzone =
                as_float(find(*h, "motion_deadzone"), cfg.ai.hand.motion_deadzone);
            cfg.ai.hand.reference_stable_frames =
                as_int(find(*h, "reference_stable_frames"),
                       cfg.ai.hand.reference_stable_frames);
            cfg.ai.hand.lost_frames_to_reset =
                as_int(find(*h, "lost_frames_to_reset"), cfg.ai.hand.lost_frames_to_reset);
            cfg.ai.hand.gesture_confirm_frames =
                as_int(find(*h, "gesture_confirm_frames"),
                       cfg.ai.hand.gesture_confirm_frames);
            cfg.ai.hand.depth_source =
                as_str(find(*h, "depth_source"), cfg.ai.hand.depth_source);
            cfg.ai.hand.invert_x = as_bool(find(*h, "invert_x"), cfg.ai.hand.invert_x);
            cfg.ai.hand.invert_y = as_bool(find(*h, "invert_y"), cfg.ai.hand.invert_y);
            cfg.ai.hand.invert_z = as_bool(find(*h, "invert_z"), cfg.ai.hand.invert_z);
            cfg.ai.hand.emit_landmarks =
                as_bool(find(*h, "emit_landmarks"), cfg.ai.hand.emit_landmarks);

            // depth_source is a closed vocabulary, not free text: an unknown
            // value would silently fall through the mapper's default branch and
            // make the §16 experiment report the wrong axis.
            if (cfg.ai.hand.depth_source != "size" &&
                cfg.ai.hand.depth_source != "landmark_z") {
                CA_LOG_WARN("[config] ai.hand.depth_source '{}' is not a known "
                            "source (size|landmark_z); using 'size'",
                            cfg.ai.hand.depth_source);
                cfg.ai.hand.depth_source = "size";
            }
        }
    }
    // ---- Metadata upload (Phase 2 / spec 9): server address is never hard-coded
    if (auto* n = find(root, "metadata")) {
        cfg.metadata.enable        = as_bool(find(*n, "enable"), cfg.metadata.enable);
        cfg.metadata.server_url    = as_str(find(*n, "server_url"), cfg.metadata.server_url);
        cfg.metadata.camera_id     = as_str(find(*n, "camera_id"), cfg.metadata.camera_id);
        cfg.metadata.version       = as_int(find(*n, "version"), cfg.metadata.version);
        cfg.metadata.queue_size    = as_int(find(*n, "queue_size"), cfg.metadata.queue_size);
        cfg.metadata.timeout_ms    = as_int(find(*n, "timeout_ms"), cfg.metadata.timeout_ms);
        cfg.metadata.retry_interval_ms =
            as_int(find(*n, "retry_interval_ms"), cfg.metadata.retry_interval_ms);
        cfg.metadata.retry_max_interval_ms =
            as_int(find(*n, "retry_max_interval_ms"), cfg.metadata.retry_max_interval_ms);
        cfg.metadata.heartbeat_interval_sec =
            as_int(find(*n, "heartbeat_interval_sec"), cfg.metadata.heartbeat_interval_sec);
        cfg.metadata.log_payload = as_bool(find(*n, "log_payload"), cfg.metadata.log_payload);
        cfg.metadata.skip_tls_verify =
            as_bool(find(*n, "insecure"), cfg.metadata.skip_tls_verify);
    }
    // ---- robot branch (gesture -> MeArmPilot) ------------------------------
    // Optional and off by default. Like the metadata branch, nothing here is
    // validated into a hard failure: a bad value is logged and the branch stays
    // inert, so an operator can never break the video stream with a typo.
    if (auto* n = find(root, "robot")) {
        cfg.robot.enable       = as_bool(find(*n, "enable"), cfg.robot.enable);
        cfg.robot.host         = as_str(find(*n, "host"), cfg.robot.host);
        cfg.robot.port         = as_int(find(*n, "port"), cfg.robot.port);
        cfg.robot.timeout_ms   = as_int(find(*n, "timeout_ms"), cfg.robot.timeout_ms);
        cfg.robot.reconnect_ms = as_int(find(*n, "reconnect_ms"), cfg.robot.reconnect_ms);
        cfg.robot.map_mode     = as_str(find(*n, "map_mode"), cfg.robot.map_mode);
        cfg.robot.rate_hz      = as_int(find(*n, "rate_hz"), cfg.robot.rate_hz);
        cfg.robot.gain_x       = as_float(find(*n, "gain_x"), cfg.robot.gain_x);
        cfg.robot.gain_y       = as_float(find(*n, "gain_y"), cfg.robot.gain_y);
        cfg.robot.gain_z       = as_float(find(*n, "gain_z"), cfg.robot.gain_z);
        cfg.robot.deadzone_mm  = as_float(find(*n, "deadzone_mm"), cfg.robot.deadzone_mm);
        cfg.robot.soft_boundary =
            as_bool(find(*n, "soft_boundary"), cfg.robot.soft_boundary);
        cfg.robot.soft_margin  = as_float(find(*n, "soft_margin"), cfg.robot.soft_margin);
        cfg.robot.soft_min_gain =
            as_float(find(*n, "soft_min_gain"), cfg.robot.soft_min_gain);
        cfg.robot.reach_span_x =
            as_float(find(*n, "reach_span_x"), cfg.robot.reach_span_x);
        cfg.robot.reach_span_y =
            as_float(find(*n, "reach_span_y"), cfg.robot.reach_span_y);
        cfg.robot.reach_span_z =
            as_float(find(*n, "reach_span_z"), cfg.robot.reach_span_z);
        cfg.robot.soft_probe_on_reject =
            as_bool(find(*n, "soft_probe_on_reject"), cfg.robot.soft_probe_on_reject);
        cfg.robot.soft_probe_tries =
            as_int(find(*n, "soft_probe_tries"), cfg.robot.soft_probe_tries);
        cfg.robot.gesture_home = as_str(find(*n, "gesture_home"), cfg.robot.gesture_home);
        cfg.robot.gesture_gripper =
            as_bool(find(*n, "gesture_gripper"), cfg.robot.gesture_gripper);
        cfg.robot.home_on_presence =
            as_bool(find(*n, "home_on_presence"), cfg.robot.home_on_presence);
        cfg.robot.lost_frames  = as_int(find(*n, "lost_frames"), cfg.robot.lost_frames);
        cfg.robot.gesture_mute_ms =
            as_int(find(*n, "gesture_mute_ms"), cfg.robot.gesture_mute_ms);
        cfg.robot.log_commands = as_bool(find(*n, "log_commands"), cfg.robot.log_commands);

        // Closed vocabularies, same policy the hand depth source uses: an
        // unknown value would silently pick a default branch and behave in a way
        // nobody asked for, so keep the previous value and say so.
        if (cfg.robot.map_mode != "delta" && cfg.robot.map_mode != "absolute") {
            CA_LOG_WARN("[config] robot.map_mode '{}' unknown (delta | absolute) - keeping default",
                        cfg.robot.map_mode);
            cfg.robot.map_mode = RobotConfig{}.map_mode;
        }
        if (cfg.robot.gesture_home != "open_palm" &&
            cfg.robot.gesture_home != "fist" &&
            cfg.robot.gesture_home != "off") {
            CA_LOG_WARN("[config] robot.gesture_home '{}' unknown (open_palm | fist | off) - keeping default",
                        cfg.robot.gesture_home);
            cfg.robot.gesture_home = RobotConfig{}.gesture_home;
        }
    }
    cfg.device_id = as_str(find(root, "device_id"), cfg.device_id);
    cfg.log_level = as_str(find(root, "log_level"), cfg.log_level);
    return true;
}

} // namespace ca
