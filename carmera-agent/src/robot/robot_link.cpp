// RobotLink implementation.
//
// The command semantics are a C++ port of MeArmPilot's reference client
// `core/tools/gesture_bridge.py` (documented in docs/protocol/gesture-control.md).
// Where this file makes a judgement call it is called out inline with the
// pitfall number from that document's §3 table.

#include "camera_agent/robot/robot_link.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "camera_agent/logger.h"

namespace ca {
namespace {

// --- minimal JSON helpers -------------------------------------------------
// Same policy as the rest of this codebase (see extract_json_string in
// stream_controller.cpp): the replies are flat objects of scalars and we only
// need a handful of fields, so no JSON library is pulled in.

// Find the value of `key` starting at `from`, returning the index just past the
// colon. npos when absent.
size_t find_value(const std::string& s, const std::string& key, size_t from = 0) {
    const std::string needle = "\"" + key + "\"";
    size_t p = s.find(needle, from);
    if (p == std::string::npos) return std::string::npos;
    p = s.find(':', p + needle.size());
    if (p == std::string::npos) return std::string::npos;
    return p + 1;
}

bool json_number(const std::string& s, const std::string& key, float& out,
                 size_t from = 0) {
    size_t p = find_value(s, key, from);
    if (p == std::string::npos) return false;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    char* end = nullptr;
    const double v = std::strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p) return false;
    out = static_cast<float>(v);
    return true;
}

// Parse a 3-element numeric array following `key`.
bool json_vec3(const std::string& s, const std::string& key, float out[3],
               size_t from = 0) {
    size_t p = find_value(s, key, from);
    if (p == std::string::npos) return false;
    p = s.find('[', p);
    if (p == std::string::npos) return false;
    ++p;
    for (int i = 0; i < 3; ++i) {
        while (p < s.size() && (s[p] == ' ' || s[p] == ',' || s[p] == '\t')) ++p;
        char* end = nullptr;
        const double v = std::strtod(s.c_str() + p, &end);
        if (end == s.c_str() + p) return false;
        out[i] = static_cast<float>(v);
        p = static_cast<size_t>(end - s.c_str());
    }
    return true;
}

// Read "commands":["a","b",...] so capability is probed from the server's own
// list rather than guessed from the protocol number.
std::vector<std::string> json_string_array(const std::string& s,
                                           const std::string& key) {
    std::vector<std::string> out;
    size_t p = find_value(s, key);
    if (p == std::string::npos) return out;
    const size_t open = s.find('[', p);
    if (open == std::string::npos) return out;
    const size_t close = s.find(']', open);
    if (close == std::string::npos) return out;
    size_t i = open + 1;
    while (i < close) {
        const size_t q1 = s.find('"', i);
        if (q1 == std::string::npos || q1 > close) break;
        const size_t q2 = s.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > close) break;
        out.push_back(s.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return out;
}

std::string format_vec3(const float v[3]) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "[%.2f, %.2f, %.2f]", v[0], v[1], v[2]);
    return buf;
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// MeArmPilot's own threshold (MIN_DELTA_MM in gesture_bridge.py). Kept as the
// hard floor: below it the server would reject the vector as zero displacement.
constexpr float kMinDeltaMm = 0.5f;

// How much of the frame a hand realistically sweeps while being tracked, as a
// fraction of the full [0,1] normalised range. This is the "手势在画面里的活动
// 跨度" of gesture-control.md §2.1: the divisor that turns a workspace span into
// a per-normalised-unit gain.
//
// 0.5 (a quarter of the frame on either side of centre) rather than 1.0 because
// the HandState position is the hand's bounding-box centre, and a real hand
// cannot usefully reach the frame edges without leaving the picture. Erring
// small makes the arm slightly more responsive, which is the safe direction:
// over-shooting is corrected by the next frame, whereas a too-slow arm feels
// broken and invites the operator to wave harder.
constexpr float kHandActiveSpan = 0.5f;

// The gain is measured against the frame, so the normalised motion floor the
// analyzer uses (HandConfig::motion_deadzone, 0.02) has to be expressed in the
// same terms. Below this the arm would be commanded a sub-millimetre step.
inline float axis_scale(float span) {
    if (span <= 0.0f) return 0.0f;
    return span / kHandActiveSpan;
}

// Cosine ease, the same shape as the usual smoothstep: w(0)=0, w(1)=1, and both
// ends have zero derivative. That last property is the whole point here - a
// linear taper would put a kink in the gain exactly where the arm enters the
// margin, which the operator feels as the arm suddenly going sluggish.
inline float smoothstep01(float t) {
    t = clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Learning the effective bound from refusals.
//
// A refused step only teaches us the bound on an axis it was actually pushing
// along. Below this the step is noise (and could walk the bound in for free on
// an axis that had nothing to do with the refusal), so it is ignored.
constexpr float kBoundLearnMinStepMm = 0.5f;

// An acceptance proves one point is reachable, not that the whole face moved
// out, so the learned bound only relaxes after this many consecutive
// acceptances - and then only part of the way (kRelaxFraction).
constexpr int   kRelaxEveryNAccepts = 8;

// Snap-back distance when the arm has just been PROVED to be somewhere beyond
// the bound (the arm reached `v > eff_max_`, so eff_max_ was too tight).
constexpr float kRelaxFraction = 0.25f;

// Drift-back distance, applied on the same tick, toward the axis' prior band
// even when nothing has been proved. This is what gives a learned bound a
// half-life. Without it a bound pulled in by a single refusal is permanent: a
// bound pinned to the arm's own pose leaves the arm sitting exactly ON the wall,
// where `v > eff_max_` can never hold, so kRelaxFraction never fires at all.
//
// Deliberately gentler than kRelaxFraction: a real wall re-learns itself within
// a few frames (the taper decelerates into it, and a refusal plus the halving
// probe is cheap), whereas a wrong wall costs entire axes for the whole session.
// 0.10 per 8 acceptances is a time constant of ~80 accepted frames.
constexpr float kRelaxToPriorFraction = 0.10f;

// Tolerance for "this step would leave the learned bound". A step that lands
// exactly on the bound is fine (that is the extreme of the reachable set); only
// one that overshoots it is suppressed.
constexpr float kBoundEpsilonMm = 0.05f;

} // namespace

// ---------------------------------------------------------------------------
RobotLink::RobotLink() = default;
RobotLink::~RobotLink() { stop(); }

bool RobotLink::init(const RobotConfig& cfg) {
    cfg_ = cfg;

    if (cfg_.port <= 0 || cfg_.port > 65535) {
        CA_LOG_WARN("[ROBOT] invalid port {} - robot control stays disabled",
                    cfg_.port);
        return false;
    }
    if (cfg_.map_mode != "delta" && cfg_.map_mode != "absolute") {
        CA_LOG_WARN("[ROBOT] invalid map_mode '{}' (expected delta | absolute)",
                    cfg_.map_mode);
        return false;
    }
    if (cfg_.rate_hz < 1 || cfg_.rate_hz > 60) {
        CA_LOG_WARN("[ROBOT] invalid rate_hz {} (expected 1..60)", cfg_.rate_hz);
        return false;
    }

    if (!client_) {
        client_.reset(create_json_line_client(cfg_.host, cfg_.port, cfg_.timeout_ms));
    }
    return true;
}

void RobotLink::set_client(std::unique_ptr<IJsonLineClient> c) {
    client_ = std::move(c);
}

void RobotLink::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { thread_loop(); });
    CA_LOG_INFO("[ROBOT] control branch started ({}:{} mode={} rate={}Hz)",
                cfg_.host, cfg_.port, cfg_.map_mode, cfg_.rate_hz);
}

void RobotLink::stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    q_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (client_) client_->close();
    CA_LOG_INFO("[ROBOT] control branch stopped (sent={} rejected={} skipped={} reconnects={})",
                sent_.load(), rejected_.load(), skipped_.load(), reconnects_.load());
}

void RobotLink::push_hand(const HandState& hs) {
    if (!running_.load()) return;
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        // Bounded: three frames is a tenth of a second at the AI rate. Beyond
        // that the oldest hand position is worthless - same policy the metadata
        // queue uses (drop oldest, keep newest).
        constexpr size_t kMaxQueue = 3;
        while (queue_.size() >= kMaxQueue) queue_.pop_front();
        queue_.push_back(hs);
    }
    q_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------
void RobotLink::thread_loop() {
    // Connect lazily: a backend that is not running must not delay video startup
    // nor spin this thread. The first successful caps+home happens in open().
    while (running_.load()) {
        HandState hs;
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(q_mtx_);
            q_cv_.wait_for(lk, std::chrono::milliseconds(50),
                           [this] { return !queue_.empty() || !running_.load(); });
            if (!running_.load()) break;
            if (!queue_.empty()) {
                hs = std::move(queue_.front());
                queue_.pop_front();
                have = true;
            }
        }

        const int64_t now_ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        if (!connect_if_needed()) continue;
        if (have) step(hs, now_ms);
    }
}

bool RobotLink::connect_if_needed() {
    if (!client_) return false;
    if (client_->connected()) return true;

    const int64_t now_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now_ms < next_connect_attempt_ms_) return false;
    next_connect_attempt_ms_ = now_ms + std::max(200, cfg_.reconnect_ms);

    if (!client_->connect()) {
        // WARN the FIRST failure of a down-run and stay at DEBUG afterwards.
        //
        // This used to be DEBUG-only, and that was a real usability bug, not a
        // cosmetic one: the startup line says "control branch started", so an
        // operator whose backend is not actually listening sees a healthy log
        // and concludes the arm is being driven. Nothing else in the branch
        // fires until a connection succeeds (caps/home/movexyz all need it), so
        // there was literally no other clue. One WARN per outage costs nothing;
        // a line every reconnect_ms would be spam, hence the latch.
        if (!connect_failed_warned_) {
            connect_failed_warned_ = true;
            CA_LOG_WARN("[ROBOT] cannot reach {}:{} - gesture control is INACTIVE "
                        "until a backend answers (retrying every {} ms)",
                        cfg_.host, cfg_.port, std::max(200, cfg_.reconnect_ms));
        } else {
            CA_LOG_DEBUG("[ROBOT] connect to {}:{} failed - will retry", cfg_.host, cfg_.port);
        }
        return false;
    }

    // Reconnected: say so, and re-arm the warn latch for the next outage.
    if (connect_failed_warned_) {
        CA_LOG_INFO("[ROBOT] backend is reachable again - gesture control ACTIVE");
        connect_failed_warned_ = false;
    }
    ++reconnects_;
    CA_LOG_INFO("[ROBOT] connected to {}:{}", cfg_.host, cfg_.port);
    if (!open()) {
        client_->close();
        return false;
    }
    return true;
}

bool RobotLink::open() {
    // gesture-control.md §1: "别跳过 ① ②". Without caps the mapping has no
    // bounds (and copying them in would be a second source of truth), and
    // without home the first relative command is a jump from an unknown pose.
    JsonReply reply;
    if (!client_->exchange("{\"cmd\":\"caps\"}", reply)) {
        CA_LOG_WARN("[ROBOT] caps failed at transport level");
        return false;
    }
    if (!reply.ok) {
        // A backend older than protocol v2 answers 'unknown cmd "caps"'. Say so
        // explicitly: the fix is to update the backend, not to retry.
        CA_LOG_WARN("[ROBOT] caps rejected: {} (backend older than protocol v2?)",
                    reply.error);
        return false;
    }

    caps_ = RobotCaps{};
    float proto = 0.0f;
    if (json_number(reply.raw, "protocol", proto)) caps_.protocol = static_cast<int>(proto);
    if (json_number(reply.raw, "workspace", proto)) { /* presence check only */ }
    // "workspace":{"min":[...],"max":[...],...}
    const size_t ws = reply.raw.find("\"workspace\"");
    if (ws != std::string::npos) {
        json_vec3(reply.raw, "min", caps_.ws_min, ws);
        json_vec3(reply.raw, "max", caps_.ws_max, ws);
    }
    caps_.commands = json_string_array(reply.raw, "commands");
    {
        float dev_present = 0.0f;
        (void)dev_present;
    }
    caps_.valid = true;

    // Capability probe from the server's own list (tcp-xyz-v2.md §4: "能力探测
    // 用这个，不要靠版本号猜").
    const bool has_movexyz = std::find(caps_.commands.begin(), caps_.commands.end(),
                                       "movexyz") != caps_.commands.end();
    if (!has_movexyz) {
        CA_LOG_WARN("[ROBOT] backend reports no 'movexyz' - gesture control needs protocol v2");
        caps_.valid = false;
        return false;
    }

    CA_LOG_INFO("[ROBOT] caps protocol={} device={} workspace={}..{}",
                caps_.protocol,
                (json_string_array(reply.raw, "units").empty() ? "?" : "ok"),
                format_vec3(caps_.ws_min).c_str(), format_vec3(caps_.ws_max).c_str());

    // Establish the known basis (②). A relative command is defined against the
    // last accepted target, so this is what makes the first delta sane.
    const RobotCommandResult hr = do_home();
    if (!hr.ok) {
        CA_LOG_WARN("[ROBOT] home failed during open: {}", hr.error);
        return false;
    }

    // The session counters are reset AFTER the handshake: `caps` and `home` are
    // connection upkeep, not hand-following traffic. Left in, they would put a
    // constant offset on every count and make "one gesture, one command"
    // untestable - and a reconnecting link would inflate the numbers an operator
    // reads as "how much am I actually commanding".
    sent_ = 0;
    rejected_ = 0;
    skipped_ = 0;
    // The learned bounds are per-arm-pose knowledge, and home() has just moved
    // the arm to a known pose - so whatever was learned about the reachable
    // shell from the old pose is no longer trustworthy. Start from the caps box
    // again and let the refusals teach it afresh.
    eff_init_ = false;
    eff_relax_count_ = 0;
    // The differencing basis belongs to the OLD session: after a reconnect the
    // hand may be somewhere else entirely, and keeping prev_delta_ would make
    // the first frame after reconnect emit that whole change as one step.
    have_prev_delta_ = false;
    muted_until_ms_ = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Protocol primitives
// ---------------------------------------------------------------------------
RobotCommandResult RobotLink::do_home() {
    RobotCommandResult r;
    if (!client_ || !client_->connected()) return r;

    JsonReply reply;
    if (!client_->exchange("{\"cmd\":\"home\"}", reply)) {
        r.error = "transport failure";
        return r;
    }
    r.sent = true;
    r.ok = reply.ok;
    if (r.ok) {
        ++sent_;
        // The reply carries the state, so the basis is refreshed without an
        // extra round trip (pitfall 2: this is the COMMANDED pose, which is
        // exactly the right thing to use as a basis).
        float tcp[3];
        if (json_vec3(reply.raw, "tcp", tcp)) {
            last_good_mm_[0] = tcp[0];
            last_good_mm_[1] = tcp[1];
            last_good_mm_[2] = tcp[2];
            have_last_good_ = true;
            caps_.home_tcp[0] = tcp[0];
            caps_.home_tcp[1] = tcp[1];
            caps_.home_tcp[2] = tcp[2];
            caps_.home_valid = true;
        }
        // `home` also resets the gripper to its home opening; keep our latch in
        // step so the next fist does not think it is already closed.
        gripper_closed_ = false;
        if (cfg_.log_commands) {
            CA_LOG_INFO("[ROBOT] cmd=home ok tcp={}",  // spdlog style: {} not %s
                        have_last_good_ ? format_vec3(last_good_mm_).c_str() : "?");
        }
    } else {
        ++rejected_;
        r.error = reply.error;
        CA_LOG_WARN("[ROBOT] cmd=home rejected: {}", reply.error);
    }
    return r;
}

RobotCommandResult RobotLink::do_gripper(bool close_it) {
    RobotCommandResult r;
    if (!client_ || !client_->connected()) return r;

    const std::string body = close_it
        ? "{\"cmd\":\"gripper\",\"action\":\"close\"}"
        : "{\"cmd\":\"gripper\",\"action\":\"open\"}";
    JsonReply reply;
    if (!client_->exchange(body, reply)) {
        r.error = "transport failure";
        return r;
    }
    r.sent = true;
    r.ok = reply.ok;
    if (r.ok) {
        ++sent_;
        gripper_closed_ = close_it;
        if (cfg_.log_commands) {
            CA_LOG_INFO("[ROBOT] cmd=gripper action={} ok", close_it ? "close" : "open");
        }
    } else {
        ++rejected_;
        r.error = reply.error;
        CA_LOG_WARN("[ROBOT] cmd=gripper rejected: {}", reply.error);
    }
    return r;
}

RobotCommandResult RobotLink::do_movexyz(float dx, float dy, float dz) {
    RobotCommandResult r;
    if (!client_ || !client_->connected()) return r;

    // Pitfall 1: an all-zero vector is a PARAMETER ERROR on the server. Never
    // send it; treat "too small to matter" as "nothing to do".
    const float magnitude = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (magnitude < std::max(kMinDeltaMm, cfg_.deadzone_mm)) {
        r.ok = true;       // deliberately not a failure
        r.sent = false;
        ++skipped_;
        return r;
    }

    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "{\"cmd\":\"movexyz\",\"delta\":[%.3f,%.3f,%.3f]}", dx, dy, dz);
    JsonReply reply;
    if (!client_->exchange(buf, reply)) {
        r.error = "transport failure";
        return r;
    }
    r.sent = true;
    r.ok = reply.ok;
    if (r.ok) {
        ++sent_;
        // The basis the soft boundary steers by. `state.tcp` is the COMMANDED
        // pose (pitfall 2) - which is exactly the right thing to use here, since
        // the real arm has no feedback and the commanded pose is the only model
        // of "where the arm is" we will ever get.
        //
        // This is NOT the dead write that used to live here. Back then nothing
        // read the latch on this route; now boundary_weight() does, on every
        // following frame. We still do NOT compare a delta against it - the
        // deadzone guard for this route is "is this step big enough to bother",
        // which needs no position - so the distinction from do_moveto (which
        // compares targets) is intact.
        float tcp[3];
        if (json_vec3(reply.raw, "tcp", tcp)) {
            last_good_mm_[0] = tcp[0];
            last_good_mm_[1] = tcp[1];
            last_good_mm_[2] = tcp[2];
            have_last_good_ = true;
            // This point is reachable, so the learned bound may ease back toward
            // it (slowly - see relax_learned_bound).
            relax_learned_bound(tcp);
        }
        if (cfg_.log_commands) {
            CA_LOG_INFO("[ROBOT] cmd=movexyz delta=[{:.2f},{:.2f},{:.2f}] ok", dx, dy, dz);
        }
    } else {
        // Pitfall 4: OUT_OF_WORKSPACE / JOINT_LIMIT are expected near the edges.
        // Keep the basis where it was and let the next frame try again - do NOT
        // slam the target, and do NOT treat this as a link failure.
        //
        // This refusal is also the ONLY trustworthy information about where the
        // real reachable set ends: the caps box overstates it badly (measured:
        // x is already unsolvable at 150 mm where the box says 175.88). So feed
        // it to the soft-boundary learner, which pulls the effective bound in to
        // the point the refused step would have reached.
        ++rejected_;
        r.error = reply.error;
        {
            const float from[3] = {have_last_good_ ? last_good_mm_[0] : 0.0f,
                                   have_last_good_ ? last_good_mm_[1] : 0.0f,
                                   have_last_good_ ? last_good_mm_[2] : 0.0f};
            const float stepv[3] = {dx, dy, dz};
            if (have_last_good_) learn_from_rejection(from, stepv);
        }
        CA_LOG_DEBUG("[ROBOT] cmd=movexyz delta=[{:.2f},{:.2f},{:.2f}] rejected: {}",
                     dx, dy, dz, reply.error.c_str());
    }
    return r;
}

RobotCommandResult RobotLink::do_moveto(float x, float y, float z) {
    RobotCommandResult r;
    if (!client_ || !client_->connected()) return r;

    // Coarse clamp to the outer bounding box (pitfall 4/6). This is NOT a
    // reachability test - the box contains unreachable volume - it only stops
    // the obvious "outside everything" values before they reach the server.
    if (caps_.valid) {
        x = clampf(x, caps_.ws_min[0], caps_.ws_max[0]);
        y = clampf(y, caps_.ws_min[1], caps_.ws_max[1]);
        z = clampf(z, caps_.ws_min[2], caps_.ws_max[2]);
    }

    // Pitfall 5: a re-commanded target needs no round trip. This compares against
    // the last target we SENT, never against a reply (see last_good_mm_ in the
    // header): the arm has no position feedback, so replies carry the commanded
    // value and a `home` reply carries the home pose.
    if (have_last_good_) {
        const float d = std::sqrt(
            (x - last_good_mm_[0]) * (x - last_good_mm_[0]) +
            (y - last_good_mm_[1]) * (y - last_good_mm_[1]) +
            (z - last_good_mm_[2]) * (z - last_good_mm_[2]));
        if (d < std::max(kMinDeltaMm, cfg_.deadzone_mm)) {
            r.ok = true;
            r.sent = false;
            ++skipped_;
            return r;
        }
    }

    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "{\"cmd\":\"moveto\",\"xyz\":[%.3f,%.3f,%.3f]}", x, y, z);
    JsonReply reply;
    if (!client_->exchange(buf, reply)) {
        r.error = "transport failure";
        return r;
    }
    r.sent = true;
    r.ok = reply.ok;
    if (r.ok) {
        ++sent_;
        last_good_mm_[0] = x;
        last_good_mm_[1] = y;
        last_good_mm_[2] = z;
        have_last_good_ = true;
        if (cfg_.log_commands) {
            CA_LOG_INFO("[ROBOT] cmd=moveto xyz=[{:.2f},{:.2f},{:.2f}] ok", x, y, z);
        }
    } else {
        // Pitfall 4: a rejected target leaves the basis alone, so the next frame
        // retries from where the arm actually is.
        ++rejected_;
        r.error = reply.error;
        CA_LOG_DEBUG("[ROBOT] cmd=moveto xyz=[{:.2f},{:.2f},{:.2f}] rejected: {}",
                     x, y, z, reply.error.c_str());
    }
    return r;
}

// ---------------------------------------------------------------------------
// Mapping
// ---------------------------------------------------------------------------
void RobotLink::norm_to_mm(const Vec3& n, float out_mm[3]) const {
    // Bounds come from caps and nowhere else (pitfall 6). When caps is missing
    // we cannot invent a range, so the caller must not reach here; guard anyway.
    if (!caps_.valid) {
        out_mm[0] = out_mm[1] = out_mm[2] = 0.0f;
        return;
    }
    const float nx = clampf(n.x, 0.0f, 1.0f);
    const float ny = clampf(n.y, 0.0f, 1.0f);
    const float nz = clampf(n.z, 0.0f, 1.0f);

    // Relating the two frames. The hand's logical frame (hand_types.h, spec 17)
    //     X: 0 = left edge of frame   -> 1 = right edge
    //     Y: closeness, larger = nearer the camera ("forward")
    //     Z: 0 = bottom of frame      -> 1 = top
    // and the arm's frame (caps.frame) is
    //     x = front,  y = left,  z = up
    //
    // The mapping is therefore a RENAME, not a shuffle of positions:
    //     hand X (right)     -> arm y, MIRRORED (arm +y is left)
    //     hand Y (close/fwd) -> arm x, directly (arm +x is front)
    //     hand Z (up)        -> arm z, directly (arm +z is up)
    const float arm_x = caps_.ws_min[0] + (caps_.ws_max[0] - caps_.ws_min[0]) * ny;
    const float arm_y = caps_.ws_min[1] + (caps_.ws_max[1] - caps_.ws_min[1]) * (1.0f - nx);
    const float arm_z = caps_.ws_min[2] + (caps_.ws_max[2] - caps_.ws_min[2]) * nz;
    out_mm[0] = arm_x;
    out_mm[1] = arm_y;
    out_mm[2] = arm_z;
}

// True when `step_mm` on `axis` would take the arm further OUT of its learned
// bounds from `pos_mm`. Used to decide whether a fully-tapered step should be
// suppressed outright rather than sent at the floor gain.
//
// Asks about the learned bounds, not caps: a step can be "inside caps" while
// already being past anything the arm can reach.
bool RobotLink::push_out_of_bound(const float pos_mm[3], int axis, float step_mm) const {
    if (!caps_.valid || axis < 0 || axis > 2 || step_mm == 0.0f) return false;
    float lo = 0.0f, hi = 0.0f, margin = 0.0f;
    effective_bounds(axis, lo, hi, margin);
    const float target = pos_mm[axis] + step_mm;
    return target > hi + kBoundEpsilonMm || target < lo - kBoundEpsilonMm;
}

// ---------------------------------------------------------------------------
// Soft boundary
// ---------------------------------------------------------------------------
// The bound pair the taper runs against, and the width of the slow zone.
//
// Two sources, in order of trust:
//
//   1. LEARNED bounds (eff_min_/eff_max_), once the link has seen a refusal.
//      These describe the arm that is actually in front of us - the only way
//      to know where the solvable set really ends, since caps is a box.
//   2. The MEASURED band, before any refusal: reach_span_* centred on the caps
//      home pose, or the caps box itself when no span was configured.
//
// The caps box is deliberately never used raw. It is a bounding box over the
// geometric reach and includes poses the IK cannot solve; on this backend it
// overstates X by 4x (box 135 mm, reachable 34 mm) and the arm sits only 5 mm
// from the -X edge at HOME. A taper built on the box therefore reads "60 mm of
// clearance" while the arm is in fact one step from the wall, never dampens,
// and every frame is refused.
//
// The margin is the same span's fraction. Deriving it from the caps span
// instead would put a 20.3 mm slow zone on a 34 mm axis, so the arm would be
// at half gain the moment it leaves HOME - a permanent sluggishness rather
// than a boundary effect.
void RobotLink::effective_bounds(int axis, float& lo_out, float& hi_out,
                                 float& margin_out) const {
    const float m = clampf(cfg_.soft_margin, 0.0f, 0.49f);

    if (eff_init_) {
        lo_out = eff_min_[axis];
        hi_out = eff_max_[axis];
        const float span = hi_out - lo_out;
        // A learned span can be small (a refusal pins it to the arm's current
        // position). Keep the slow zone a usable width regardless, or the taper
        // collapses into the hard stop it exists to prevent.
        const float measured = measured_span(axis);
        margin_out = std::max(span, measured) * m;
        return;
    }

    const float measured = measured_span(axis);
    if (measured > 0.0f && caps_.home_valid) {
        // Centre the band on HOME: reach_span_ carries widths only, so a
        // re-fitted arm needs no new bounds - just a new width.
        const float h = caps_.home_tcp[axis];
        lo_out = h - measured * 0.5f;
        hi_out = h + measured * 0.5f;
        margin_out = measured * m;
        return;
    }

    // No measurement configured: the caps box is all we have. Keep the old
    // behaviour for such a device rather than inventing a band.
    lo_out = caps_.ws_min[axis];
    hi_out = caps_.ws_max[axis];
    margin_out = (caps_.ws_max[axis] - caps_.ws_min[axis]) * m;
}

// The configured measured span for one axis; 0 when unset.
float RobotLink::measured_span(int axis) const {
    switch (axis) {
        case 0: return cfg_.reach_span_x;
        case 1: return cfg_.reach_span_y;
        case 2: return cfg_.reach_span_z;
        default: return 0.0f;
    }
}

// The band the taper runs against BEFORE anything has been learned, exposed so
// the learned bounds can relax back to exactly this and no further.
//
// Kept identical to effective_bounds()' non-learned branch on purpose: "relaxed
// all the way back" and "nothing learned yet" must mean the same thing, or the
// learner would have a second, invisible set of bounds to disagree with.
void RobotLink::prior_bounds(int axis, float& lo_out, float& hi_out) const {
    const float measured = measured_span(axis);
    if (measured > 0.0f && caps_.home_valid) {
        const float h = caps_.home_tcp[axis];
        lo_out = h - measured * 0.5f;
        hi_out = h + measured * 0.5f;
        return;
    }
    lo_out = caps_.ws_min[axis];
    hi_out = caps_.ws_max[axis];
}

float RobotLink::boundary_weight(const float pos_mm[3], int axis, float step_mm) const {
    // Off, unknown geometry, or a degenerate span: no taper at all. Returning 1
    // (rather than 0) keeps the caller's arithmetic identical to the original
    // behaviour when the feature is disabled - which is what makes the
    // "disabled == old behaviour" assertion in the tests meaningful.
    if (!cfg_.soft_boundary || !caps_.valid || axis < 0 || axis > 2) return 1.0f;

    // Clamp the fraction so a position outside the band reads as "on the bound"
    // rather than producing a weight below the floor or above 1.
    const float m = clampf(cfg_.soft_margin, 0.0f, 0.49f);
    if (m <= 0.0f) return 1.0f;

    float lo = 0.0f, hi = 0.0f, margin = 0.0f;
    effective_bounds(axis, lo, hi, margin);
    const float span = hi - lo;
    if (span <= 0.0f || margin <= 0.0f) return 1.0f;

    // Distance to the nearest bound, in mm. 0 = sitting on it.
    const float p = clampf(pos_mm[axis], lo, hi);
    const float d = std::min(p - lo, hi - p);

    // 1 well inside, easing to `soft_min_gain` at the bound. The floor is not
    // cosmetic: at exactly 0 the arm could never be commanded back out of the
    // margin, because every step - in EITHER direction - would be scaled to
    // nothing and then swallowed by the deadzone.
    //
    // This is the weight for a step that does not have a direction of its own
    // (a query from a test, or a step that is not moving toward the bound).
    const float s = clampf((1.0f - d / margin), 0.0f, 1.0f);
    const float eased = smoothstep01(s);
    const float floor_gain = clampf(cfg_.soft_min_gain, 0.0f, 1.0f);
    float w = 1.0f - eased * (1.0f - floor_gain);

    // Direction matters. The whole point of the feature is "the arm cannot be
    // pushed FURTHER into the wall", but it must always be able to come back
    // out - and at the bound itself the two are the same step sign as far as the
    // magnitude goes. So when the step is heading back toward the interior,
    // there is nothing to taper and the weight is 1.
    //
    // This is what actually makes the arm go quiet at the wall: with the bound
    // learned right down to where the arm is stuck, d == 0, the inward weight is
    // the floor, and the floor times a typical step still exceeds the deadzone -
    // so a symmetric taper keeps emitting steps that keep getting refused. Only
    // suppressing the OUTWARD step stops that, and it costs nothing: a step away
    // from the bound is always allowed.
    if (step_mm != 0.0f) {
        const bool toward_max = step_mm > 0.0f;
        // If the step heads toward the wall it is on, keep the taper; if it
        // heads away (or the arm is not near that side at all) there is nothing
        // to damp.
        const bool near_max = (hi - p) <= d + 1e-6f;
        if (toward_max != near_max) return 1.0f;
    }
    return w;
}

// Pull the learned bound in for every axis a refused step was pushing toward.
//
// `attempted` is the target the refused command WOULD have reached (last known
// position + the step that was rejected). The arm demonstrably cannot be at or
// beyond that point, so the effective bound on that side is at most `attempted`.
void RobotLink::learn_from_rejection(const float from_mm[3], const float step_mm[3]) {
    if (!caps_.valid) return;
    // Seed from caps on first use so a learned side and a caps side can coexist
    // without one of them being the default 0.
    for (int a = 0; a < 3; ++a) {
        if (!eff_init_) {
            eff_min_[a] = caps_.ws_min[a];
            eff_max_[a] = caps_.ws_max[a];
        }
    }
    // Only tighten when the step actually went somewhere meaningful on that axis.
    // A refusal on a step that barely moved an axis says nothing about that
    // axis' bound, and tightening it there would walk the bound in for free.
    for (int a = 0; a < 3; ++a) {
        if (step_mm[a] > kBoundLearnMinStepMm) {
            // A refusal of `from + step` proves the arm cannot REACH that
            // target, so the bound is at most there. It does NOT prove the arm
            // has run out of travel: every point strictly between `from` and
            // `from + step` is still untested.
            //
            // This used to write `min(from + step, from)` - pinning the bound to
            // the arm's CURRENT pose - on the theory that the arm "is still at
            // from, so the extreme is at most from". That is the deadlock
            // measured on 2026-09-29. Pinning to the current pose puts the arm
            // exactly ON the wall, where d == 0 and boundary_weight() returns the
            // 0.10 floor for every outward step; and with the arm sitting on the
            // bound, the `v > eff_max_` test in relax_learned_bound() can never
            // hold either, so nothing ever walked it back out. In the recorded
            // run that closed +Y and +Z permanently: the taper weight was pinned
            // at 0.10 in 300/1370 and 362/1370 of the taper frames, all of them
            // with the arm parked at HOME.
            //
            // Writing the attempted target instead costs one step of headroom,
            // so the arm can always creep up to the wall, the taper never sits on
            // its floor at the current pose, and each further refusal just
            // re-tightens by the step it was asked for.
            const float bound = from_mm[a] + step_mm[a];
            if (bound < eff_max_[a]) eff_max_[a] = bound;
        } else if (step_mm[a] < -kBoundLearnMinStepMm) {
            const float bound = from_mm[a] + step_mm[a];
            if (bound > eff_min_[a]) eff_min_[a] = bound;
        }
    }
    eff_init_ = true;
    eff_relax_count_ = 0;
}

// An accepted step proves the point it reached is reachable, so the learned
// bound can relax back out.
//
// Relaxation must be able to fire WITHOUT first reaching past the bound,
// because a bound pulled in by a single refusal is a guess about a whole face of
// the reachable set - and that guess can be wrong for reasons the refusal never
// reveals. On this backend the server refuses the WHOLE command when ANY ONE axis
// falls outside its workspace, so a +Z limit refusal also tightens Y, which was
// never implicated. A guess like that has to expire.
//
// So every relax tick does two things:
//   (a) snap outward to `v` if the arm was just PROVED to be beyond the bound
//       (kRelaxFraction of the gap - keeps a freshly contradicted bound honest);
//   (b) drift every bound kRelaxToPriorFraction of the way back toward its PRIOR
//       band even if nothing was proved.
//
// (b) is what turns the learned bound from a permanent wall into evidence with a
// half-life. It relaxes toward the measured band, never the caps box: the box
// overstates the reachable set by up to 4x, so drifting toward it would just
// re-open the refusal tail the learner exists to stop. Because the relaxation
// only ticks on ACCEPTED commands, bounds re-open only while the arm is actually
// being driven - a parked arm keeps whatever it learned.
void RobotLink::relax_learned_bound(const float reached_mm[3]) {
    if (!eff_init_ || !caps_.valid) return;
    // Only relax every N acceptances: a single success at one (y,z) says little
    // about the whole face, and letting the bound snap back out would undo the
    // pull-in that stopped the refusal tail in the first place.
    if (++eff_relax_count_ < kRelaxEveryNAccepts) return;
    eff_relax_count_ = 0;
    for (int a = 0; a < 3; ++a) {
        float prior_lo = 0.0f, prior_hi = 0.0f;
        prior_bounds(a, prior_lo, prior_hi);
        const float v = clampf(reached_mm[a], prior_lo, prior_hi);

        // (a) proved: ease out to the point the arm just reached, at the faster
        //     rate, because this bound was directly contradicted.
        // (b) unproved: drift out toward the prior band, at the gentler rate.
        const bool proved_hi = v > eff_max_[a];
        const bool proved_lo = v < eff_min_[a];
        const float hi_target = proved_hi ? v : prior_hi;
        const float lo_target = proved_lo ? v : prior_lo;
        eff_max_[a] += (hi_target - eff_max_[a]) * (proved_hi ? kRelaxFraction : kRelaxToPriorFraction);
        eff_min_[a] += (lo_target - eff_min_[a]) * (proved_lo ? kRelaxFraction : kRelaxToPriorFraction);
    }
}

// ---------------------------------------------------------------------------
// The per-frame policy
// ---------------------------------------------------------------------------
bool RobotLink::step_exchange(const RobotCommandResult& r) {
    // One place that answers "did the socket survive this command?". A command
    // that was never attempted (not connected) or deliberately skipped (below
    // the deadzone) is not a failure; a command the SERVER refused is also not a
    // transport failure - only `sent && !ok && error == transport failure` is.
    if (r.error == "transport failure") {
        if (client_) client_->close();
        return false;
    }
    return client_ != nullptr && client_->connected();
}

bool RobotLink::update(const HandState& hs, int64_t now_ms) {
    step(hs, now_ms);
    // The return value answers one question only: "is the transport still
    // usable?". A rejected command (JOINT_LIMIT, OUT_OF_WORKSPACE) is a normal
    // protocol result, the link is fine, and the caller must NOT tear anything
    // down because of it. Only an actual socket failure returns false.
    return client_ != nullptr && client_->connected();
}

bool RobotLink::step(const HandState& hs, int64_t now_ms) {
    // Returns false only when the transport itself failed during this frame.
    if (!client_ || !client_->connected()) return false;

    // ---- presence tracking -------------------------------------------------
    // The rule (operator requirement): when the hand leaves the frame and later
    // comes back, the arm returns HOME and the basis is re-established rather
    // than continuing from a stale origin.
    //
    // The FIRST hand of a session is deliberately not treated as a "return":
    // open() has just sent home and latched the basis, so homing again would be
    // a redundant round trip on every session start. `presence_established_`
    // records that we have seen the state of the world at least once.
    const bool detected = hs.detected;
    bool just_returned = false;

    if (!presence_established_) {
        presence_established_ = true;
        hand_seen_ = detected;
        hand_present_.store(detected);
        last_seen_ms_ = detected ? now_ms : 0;
        if (!detected) ++miss_streak_;
    } else if (detected) {
        miss_streak_ = 0;
        last_seen_ms_ = now_ms;
        if (!hand_seen_) {
            just_returned = true;
            hand_seen_ = true;
            hand_present_.store(true);
            hand_left_seen_.store(false);
        }
    } else {
        ++miss_streak_;
        // Departure == a CONTINUOUS absence of `hand_lost_ms`, measured from the
        // last DETECTION rather than by counting misses. That distinction is the
        // whole point: the detector is bursty (clusters of 4-10 frames split by
        // 0.9-19 s of nothing), so a frame count sits inside the natural gap
        // between clusters and turns every cluster boundary into a false exit.
        // See RobotConfig::hand_lost_ms for the measurement behind 2000 ms.
        //
        // `last_seen_ms_ > 0` guards a session that starts with no hand at all:
        // nothing has ever been latched, so there is no clock to measure from.
        const bool timed_out =
            last_seen_ms_ > 0 && (now_ms - last_seen_ms_) >= cfg_.hand_lost_ms;
        if (hand_seen_ && timed_out) {
            hand_seen_ = false;
            hand_present_.store(false);
            hand_left_seen_.store(true);
            CA_LOG_DEBUG("[ROBOT] hand absent for {} ms (threshold {} ms) - "
                         "treated as left the frame",
                         static_cast<long long>(now_ms - last_seen_ms_),
                         static_cast<long long>(cfg_.hand_lost_ms));
            // Drop the latches so the next entry starts clean.
            last_gesture_ = Gesture::Unknown;
            have_last_good_ = false;
            // ...including the differencing basis. A hand that comes back is a
            // NEW origin (its reference is re-latched too), so holding an old
            // prev_delta_ would emit the difference between two unrelated
            // positions as one step.
            have_prev_delta_ = false;
        }
    }

    if (just_returned && cfg_.home_on_presence) {
        CA_LOG_INFO("[ROBOT] hand (re)entered the frame - returning HOME");
        return step_exchange(do_home());
    }

    if (!detected) return true;   // nothing to do while the hand is away

    // ---- rate limit (pitfall 3) -------------------------------------------
    // The server does not throttle, so this is the only thing keeping the arm's
    // command rate inside what it can follow. Discrete gestures are allowed
    // through immediately: a home press should not be delayed by the move rate.
    const int period_ms = 1000 / std::max(1, cfg_.rate_hz);

    // ---- discrete gestures (edge triggered) -------------------------------
    const Gesture g = hs.gesture;
    const bool rising = (g != last_gesture_);

    const bool home_gesture =
        (cfg_.gesture_home == "open_palm" && g == Gesture::OpenPalm) ||
        (cfg_.gesture_home == "fist"      && g == Gesture::Fist);

    // A discrete gesture means "switch, do not steer". Mute continuous motion
    // for `gesture_mute_ms` afterwards - see the field comment for why forming
    // a fist produces real, followable hand motion for several frames before the
    // classifier even confirms it.
    const int64_t mute_ms = std::max(0, cfg_.gesture_mute_ms);
    auto start_mute = [&]() { muted_until_ms_ = now_ms + mute_ms; };

    if (rising && home_gesture) {
        last_gesture_ = g;
        last_send_ms_ = now_ms;
        start_mute();
        return step_exchange(do_home());
    }

    if (rising && cfg_.gesture_gripper && g == Gesture::Fist) {
        last_gesture_ = g;
        last_send_ms_ = now_ms;
        start_mute();
        return step_exchange(do_gripper(true));
    }
    if (rising && cfg_.gesture_gripper && g == Gesture::OpenPalm &&
        !home_gesture && gripper_closed_) {
        last_gesture_ = g;
        last_send_ms_ = now_ms;
        start_mute();
        return step_exchange(do_gripper(false));
    }
    last_gesture_ = g;
    // ---- continuous motion -------------------------------------------------

    // Muted: a gesture just fired. Re-latch the differencing basis every frame
    // so the motion that happened while the hand was forming (or holding) the
    // gesture is DROPPED, not accumulated and then replayed as one big step when
    // the window ends.
    if (now_ms < muted_until_ms_) {
        prev_delta_[0] = hs.delta.x;
        prev_delta_[1] = hs.delta.y;
        prev_delta_[2] = hs.delta.z;
        have_prev_delta_ = true;
        last_send_ms_ = now_ms;
        return client_->connected();
    }

    // hand_types.h is explicit: while the reference is not latched, `delta` is
    // meaningless and MUST NOT be consumed - that is what prevents the large
    // jump when a hand first enters the frame. Sending nothing here is the
    // correct behaviour, not a dropped frame. The differencing basis is left
    // alone as well: the deltas from before the latch belong to the discarded
    // pre-latch window and must not become the origin of the first real step.
    if (!hs.reference_initialized) return true;

    if (cfg_.map_mode == "absolute") {
        float target[3];
        norm_to_mm(hs.position, target);
        last_send_ms_ = now_ms;
        step_exchange(do_moveto(target[0], target[1], target[2]));
        return client_->connected();
    }

    // delta route: the hand's frame-to-frame displacement becomes the arm's
    // displacement. This cannot be trapped by "inside the box but unreachable"
    // because every step is tiny and a rejection simply leaves the basis alone.
    //
    // Axis assignment mirrors norm_to_mm() so the absolute and delta routes
    // steer the arm the same way:
    //   hand dy (forward)  -> arm X (front)
    //   hand dx (right)    -> arm -Y (left is +y)
    //   hand dz (up)       -> arm Z (up)
    //
    // The gain is not a magic number: gesture-control.md §2.1 derives it as
    //   gain = workspace span / how far the hand travels across the frame
    // i.e. "a hand that crosses the whole active range should move the arm
    // across the whole reachable range". A hand's usable travel is well under
    // the full [0,1] frame (the arm cannot reach the frame edges anyway), and
    // kHandActiveSpan is that fraction. The per-axis `gain_*` config values are
    // multipliers on top, so an operator can speed up or slow down one axis
    // without touching the derivation.
    //
    // The span in that formula must be the REACHABLE span, not the caps box.
    // The box overstates it by 1.6-4.0x on this backend, and at HOME the arm
    // has only ~5 mm of travel left in -X and +Z - so a box-derived gain
    // commands 13-27 mm on the first frame of hand motion and is refused
    // outright. `reach_span_*` carries the measured span; 0 falls back to the
    // box (see RobotConfig for the measurement table).
    auto span_for = [&](float measured, int axis) -> float {
        if (measured > 0.0f) return measured;
        return caps_.ws_max[axis] - caps_.ws_min[axis];
    };
    const float gain_x = cfg_.gain_x * axis_scale(span_for(cfg_.reach_span_x, 0));
    const float gain_y = cfg_.gain_y * axis_scale(span_for(cfg_.reach_span_y, 1));
    const float gain_z = cfg_.gain_z * axis_scale(span_for(cfg_.reach_span_z, 2));

    // Soft boundary: taper each axis by how close the arm is to THAT axis' bound.
    // Per-axis rather than global, because the arm routinely sits near one wall
    // while still having room on the others; a global weight would make the free
    // axes sluggish to fix a problem that is only on one of them.
    //
    // The arm's position comes from last_good_mm_, which the delta route now
    // maintains from the reply's `tcp` (see do_movexyz). Before the first
    // accepted command there is no position at all - in that case we have just
    // homed, so `home_tcp` is the arm's position, and home() always latches it.
    // With neither, take the weight as 1: no basis, no taper, no invented clamp.
    float pos_mm[3];
    bool have_pos = have_last_good_;
    if (have_pos) {
        pos_mm[0] = last_good_mm_[0];
        pos_mm[1] = last_good_mm_[1];
        pos_mm[2] = last_good_mm_[2];
    } else if (caps_.home_valid) {
        pos_mm[0] = caps_.home_tcp[0];
        pos_mm[1] = caps_.home_tcp[1];
        pos_mm[2] = caps_.home_tcp[2];
        have_pos = true;
    }

    // The raw (pre-taper) steps, so the weight can tell "heading into the wall"
    // from "coming back out". Order matches the axis mapping below.
    //
    // ---- frame-to-frame differencing -------------------------------------
    // `hs.delta` is the hand's CUMULATIVE offset from its latched reference
    // (position - reference), NOT this frame's motion - see have_prev_delta_ in
    // the header. The arm wants the increment, so take the difference against
    // the previous frame's delta, which collapses to (pos_now - pos_prev).
    //
    // The basis advances on EVERY frame, including frames the rate limiter is
    // about to drop (that is why the limiter is checked below, not above). If a
    // throttled frame left the basis behind, its motion would be banked and then
    // replayed as one bigger step on the next allowed frame - which is exactly
    // the "stutters, then jerks" feel this is fixing. A dropped frame drops its
    // motion; nothing accumulates.
    //
    // The FIRST frame has no predecessor: starting from zero would send the
    // whole accumulated offset as one step (the exact jump this fixes), so the
    // first frame only latches the basis and sends nothing.
    float inc_x = 0.0f, inc_y = 0.0f, inc_z = 0.0f;
    if (have_prev_delta_) {
        inc_x = hs.delta.x - prev_delta_[0];
        inc_y = hs.delta.y - prev_delta_[1];
        inc_z = hs.delta.z - prev_delta_[2];
    }
    prev_delta_[0] = hs.delta.x;
    prev_delta_[1] = hs.delta.y;
    prev_delta_[2] = hs.delta.z;
    const bool first_frame = !have_prev_delta_;
    have_prev_delta_ = true;
    if (first_frame) {
        last_send_ms_ = now_ms;
        return client_->connected();
    }

    // ---- rate limit --------------------------------------------------------
    // Checked after the basis has advanced: the limiter decides whether this
    // frame is SENT, never whether the hand actually moved.
    if (now_ms - last_send_ms_ < period_ms) return true;

    const float raw_x = inc_y * gain_x;
    const float raw_y = -inc_x * gain_y;
    const float raw_z = inc_z * gain_z;

    const float w_x = have_pos ? boundary_weight(pos_mm, 0, raw_x) : 1.0f;
    const float w_y = have_pos ? boundary_weight(pos_mm, 1, raw_y) : 1.0f;
    const float w_z = have_pos ? boundary_weight(pos_mm, 2, raw_z) : 1.0f;

    float dx_mm = raw_x * w_x;
    float dy_mm = raw_y * w_y;
    float dz_mm = raw_z * w_z;

    // Suppress, PER AXIS, a step that is pressed against a learned bound and
    // still pushing outward. This is the difference between "slow down at the
    // wall" and "stop pushing at the wall": once the bound has been learned
    // right down to where the arm is stuck, the floor gain still scales a normal
    // step to ~1.3 mm - above the deadzone - so a taper alone keeps emitting
    // steps that keep being refused (measured: 21 refusals over 25 pushed frames
    // even with the taper fully engaged).
    //
    // The zeroing is confined to the axis that is actually pinned. This used to
    // be an all-or-nothing early return: the first axis found pinned ended the
    // WHOLE frame, so a hand that had stopped travelling along one direction
    // froze the arm on all three. That is wrong twice over - a Cartesian
    // movexyz is exactly what slide-along-a-wall means, and this arm parks near
    // its edges at HOME (-X has 9 mm of travel left, +Z has 5 mm), so "my hand
    // moved but the arm did not" appears the moment one axis bottoms out while
    // the other two still have room. Suppressing only the pinned axis is the
    // same physics with the dead axis removed instead of the whole frame.
    //
    // Skipping is still the right answer for the pinned axis itself (the arm
    // cannot go there), and it costs nothing: the direction test in
    // boundary_weight() already guarantees the opposite direction is untampered,
    // so the arm can always come back out.
    //
    // When every axis ends up zeroed, do_movexyz() below sees a below-deadzone
    // magnitude and counts it as a skip - the same bookkeeping the old early
    // return did, minus the frame-wide blast radius.
    if (cfg_.soft_boundary && have_pos) {
        const float raw[3] = {raw_x, raw_y, raw_z};
        const float w[3] = {w_x, w_y, w_z};
        float* const step[3] = {&dx_mm, &dy_mm, &dz_mm};
        static const char* const kAxisName[3] = {"x", "y", "z"};
        for (int a = 0; a < 3; ++a) {
            if (w[a] <= cfg_.soft_min_gain + 1e-4f && push_out_of_bound(pos_mm, a, raw[a])) {
                *step[a] = 0.0f;
                CA_LOG_DEBUG("[ROBOT] soft boundary: {} step suppressed at the bound",
                             kAxisName[a]);
            }
        }
    }

    // A tapered step can fall below the deadzone. do_movexyz counts that as a
    // skip, which is the desired outcome: near the edge the arm goes quiet
    // instead of producing a stream of JOINT_LIMIT refusals.
    //
    // Logged at INFO when it bites, not DEBUG: the whole value of this feature
    // is that it is observable in a live run, and a taper that is happening is
    // BY DEFINITION not a per-frame event (the arm is parked at the edge, so the
    // steps below the deadzone stop being sent at all). DEBUG would put it in the
    // same bucket as the refusals it exists to prevent.
    if (cfg_.soft_boundary && have_pos && (w_x < 0.999f || w_y < 0.999f || w_z < 0.999f)) {
        CA_LOG_INFO("[ROBOT] soft boundary taper w=[{:.2f},{:.2f},{:.2f}] pos=[{:.1f},{:.1f},{:.1f}]",
                    w_x, w_y, w_z, pos_mm[0], pos_mm[1], pos_mm[2]);
    }

    last_send_ms_ = now_ms;
    RobotCommandResult send_res = do_movexyz(dx_mm, dy_mm, dz_mm);

    // A refusal means "that step was too big", not "that direction is closed".
    // Before the refusal is handed to the bound learner - which reads it as
    // "the arm cannot be at the target, pull the bound in" - give the same
    // motion one shorter attempt. Halving normally recovers the last few mm
    // that a box-sized gain overshot (measured at HOME: -X has 5 mm of travel
    // left, but the pre-fix gain asked for 13 mm and surrendered on the first
    // refusal, so the hand moved and the arm did not).
    //
    // Gated on soft_boundary as well: that flag is the documented escape hatch
    // back to the pre-feature behaviour ("turn it off and the link behaves
    // exactly as it did before"), and probing is part of the feature.
    //
    // The learner only ever sees the FINAL result of a frame: if the halved
    // step succeeds we do not want it keeping the bound at a point we just
    // proved reachable - the accepted branch re-relaxes it.
    if (cfg_.soft_boundary && cfg_.soft_probe_on_reject &&
        send_res.sent && !send_res.ok) {
        float px = dx_mm, py = dy_mm, pz = dz_mm;
        for (int attempt = 1; attempt < std::max(1, cfg_.soft_probe_tries); ++attempt) {
            px *= 0.5f;
            py *= 0.5f;
            pz *= 0.5f;
            // Below the deadzone there is nothing left to trade: stop probing.
            if (std::sqrt(px * px + py * py + pz * pz) <
                std::max(kMinDeltaMm, cfg_.deadzone_mm)) {
                break;
            }
            RobotCommandResult probe = do_movexyz(px, py, pz);
            if (!probe.sent) break;          // transport gone / gobbled as zero
            if (probe.ok) {
                CA_LOG_INFO("[ROBOT] soft probe recovered a sub-step "
                            "[{:.2f},{:.2f},{:.2f}] after a refusal",
                            px, py, pz);
                send_res = probe;
                break;
            }
            send_res = probe;
        }
    }

    step_exchange(send_res);
    return client_->connected();
}

} // namespace ca
