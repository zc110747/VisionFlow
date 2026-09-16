// ByteTrack, dependency-free.
//
// Faithful to the original algorithm's structure:
//   * 8-state / 4-measurement Kalman filter (constant velocity model)
//   * two-stage association: high-score detections first, then low-score ones
//     against the tracks that survived the first pass
//   * a third pass that confirms "unconfirmed" (single-frame) tracks
//   * IoU cost with detection-score fusion on the first pass
//
// Differences from the reference implementation, both deliberate:
//   1. The Kalman state is [cx, cy, w, h, vcx, vcy, vw, vh] instead of
//      [cx, cy, aspect, h, ...]. Same model, simpler to verify, and avoids the
//      aspect/height coupling that makes the noise terms hard to reason about.
//   2. Matching is greedy over cost-sorted candidate pairs instead of the
//      Jonker-Volgenant (lapjv) solver. lapjv is ~250 lines of index juggling;
//      greedy is deterministic, trivially auditable and - for the few tracks a
//      fixed camera produces - equivalent in practice.
//
// No Eigen: the linear algebra below is a ~40 line fixed-size template.

#include "camera_agent/ai/tracker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ca {
namespace {

// ---- tiny fixed-size matrix ------------------------------------------------

template <int R, int C>
struct Mat {
    float v[R * C];
    Mat() { set_zero(); }
    void set_zero() {
        for (int i = 0; i < R * C; ++i) v[i] = 0.0f;
    }
    float& at(int r, int c) { return v[r * C + c]; }
    float  at(int r, int c) const { return v[r * C + c]; }
};

// (A x K) * (K x B) -> (A x B)
template <int A, int B, int K>
Mat<A, B> mul(const Mat<A, K>& a, const Mat<K, B>& b) {
    Mat<A, B> r;
    for (int i = 0; i < A; ++i) {
        for (int j = 0; j < B; ++j) {
            float s = 0.0f;
            for (int k = 0; k < K; ++k) s += a.at(i, k) * b.at(k, j);
            r.at(i, j) = s;
        }
    }
    return r;
}

template <int R, int C>
Mat<C, R> trans(const Mat<R, C>& m) {
    Mat<C, R> t;
    for (int i = 0; i < R; ++i)
        for (int j = 0; j < C; ++j) t.at(j, i) = m.at(i, j);
    return t;
}

// Gauss-Jordan with partial pivoting. False => singular (caller keeps the prior).
template <int N>
bool inv(const Mat<N, N>& in, Mat<N, N>& out) {
    float a[N * N];
    for (int i = 0; i < N * N; ++i) a[i] = in.v[i];
    out.set_zero();
    for (int i = 0; i < N; ++i) out.at(i, i) = 1.0f;

    for (int col = 0; col < N; ++col) {
        int   piv  = col;
        float best = std::fabs(a[col * N + col]);
        for (int r = col + 1; r < N; ++r) {
            const float t = std::fabs(a[r * N + col]);
            if (t > best) { best = t; piv = r; }
        }
        if (best < 1e-12f) return false;
        if (piv != col) {
            for (int k = 0; k < N; ++k) {
                std::swap(a[col * N + k], a[piv * N + k]);
                std::swap(out.v[col * N + k], out.v[piv * N + k]);
            }
        }
        const float d = a[col * N + col];
        for (int k = 0; k < N; ++k) {
            a[col * N + k]     /= d;
            out.v[col * N + k] /= d;
        }
        for (int r = 0; r < N; ++r) {
            if (r == col) continue;
            const float f = a[r * N + col];
            if (f == 0.0f) continue;
            for (int k = 0; k < N; ++k) {
                a[r * N + k]     -= f * a[col * N + k];
                out.v[r * N + k] -= f * out.v[col * N + k];
            }
        }
    }
    return true;
}

using Vec8  = std::array<float, 8>;
using Mat88 = Mat<8, 8>;
using Mat84 = Mat<8, 4>;
using Mat48 = Mat<4, 8>;
using Mat44 = Mat<4, 4>;

// ByteTrack noise weights.
constexpr float kStdWeightPosition = 1.0f / 20.0f;
constexpr float kStdWeightVelocity = 1.0f / 160.0f;

const Mat88& motion_matrix() {
    static const Mat88 F = [] {
        Mat88 m;
        for (int i = 0; i < 8; ++i) m.at(i, i) = 1.0f;
        m.at(0, 4) = 1.0f;
        m.at(1, 5) = 1.0f;
        m.at(2, 6) = 1.0f;
        m.at(3, 7) = 1.0f;
        return m;
    }();
    return F;
}

const Mat48& update_matrix() {
    static const Mat48 H = [] {
        Mat48 m;
        for (int i = 0; i < 4; ++i) m.at(i, i) = 1.0f;
        return m;
    }();
    return H;
}

void kf_initiate(Vec8& mean, Mat88& cov, const float z[4]) {
    for (int i = 0; i < 8; ++i) mean[i] = 0.0f;
    mean[0] = z[0];
    mean[1] = z[1];
    mean[2] = z[2];
    mean[3] = z[3];

    const float w = z[2] > 0.0f ? z[2] : 1.0f;
    const float h = z[3] > 0.0f ? z[3] : 1.0f;
    const float sp[4] = {2.0f * kStdWeightPosition * w,
                         2.0f * kStdWeightPosition * h,
                         2.0f * kStdWeightPosition * w,
                         2.0f * kStdWeightPosition * h};
    const float sv[4] = {10.0f * kStdWeightVelocity * w,
                         10.0f * kStdWeightVelocity * h,
                         10.0f * kStdWeightVelocity * w,
                         10.0f * kStdWeightVelocity * h};
    cov.set_zero();
    for (int i = 0; i < 4; ++i) {
        cov.at(i, i)         = sp[i] * sp[i];
        cov.at(i + 4, i + 4) = sv[i] * sv[i];
    }
}

void kf_predict(Vec8& mean, Mat88& cov) {
    const float w = mean[2] > 0.0f ? mean[2] : 1.0f;
    const float h = mean[3] > 0.0f ? mean[3] : 1.0f;
    const float sp[4] = {kStdWeightPosition * w, kStdWeightPosition * h,
                         kStdWeightPosition * w, kStdWeightPosition * h};
    const float sv[4] = {kStdWeightVelocity * w, kStdWeightVelocity * h,
                         kStdWeightVelocity * w, kStdWeightVelocity * h};
    Mat88 q;
    for (int i = 0; i < 4; ++i) {
        q.at(i, i)         = sp[i] * sp[i];
        q.at(i + 4, i + 4) = sv[i] * sv[i];
    }

    const Mat88& f = motion_matrix();
    Vec8 nm{};
    nm.fill(0.0f);
    for (int i = 0; i < 8; ++i) {
        float s = 0.0f;
        for (int j = 0; j < 8; ++j) s += f.at(i, j) * mean[j];
        nm[i] = s;
    }
    mean = nm;

    const Mat88 ft = trans(f);
    Mat88 n = mul<8, 8, 8>(mul<8, 8, 8>(f, cov), ft);
    for (int i = 0; i < 64; ++i) n.v[i] += q.v[i];
    cov = n;
}

void kf_update(Vec8& mean, Mat88& cov, const float z[4]) {
    const float w = mean[2] > 0.0f ? mean[2] : 1.0f;
    const float h = mean[3] > 0.0f ? mean[3] : 1.0f;
    const float sp[4] = {kStdWeightPosition * w, kStdWeightPosition * h,
                         kStdWeightPosition * w, kStdWeightPosition * h};
    Mat44 r;
    for (int i = 0; i < 4; ++i) r.at(i, i) = sp[i] * sp[i];

    const Mat48& hm = update_matrix();
    const Mat84  ht = trans(hm);
    const Mat84  pht = mul<8, 4, 8>(cov, ht);
    Mat44        s   = mul<4, 4, 8>(hm, pht);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) s.at(i, j) += r.at(i, j);

    Mat44 sinv;
    if (!inv<4>(s, sinv)) return;   // singular -> keep the prediction

    const Mat84 k = mul<8, 4, 4>(pht, sinv);
    float y[4];
    for (int i = 0; i < 4; ++i) y[i] = z[i] - mean[i];
    for (int i = 0; i < 8; ++i) {
        float acc = 0.0f;
        for (int j = 0; j < 4; ++j) acc += k.at(i, j) * y[j];
        mean[i] += acc;
    }

    const Mat88 kh = mul<8, 8, 4>(k, hm);
    Mat88 ikh;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j)
            ikh.at(i, j) = (i == j ? 1.0f : 0.0f) - kh.at(i, j);
    cov = mul<8, 8, 8>(ikh, cov);
}

// ---- geometry helpers ------------------------------------------------------

using Box4 = std::array<float, 4>;   // x1, y1, x2, y2

inline Box4 box_from_mean(const Vec8& m) {
    Box4 b{};
    b[0] = m[0] - m[2] * 0.5f;
    b[1] = m[1] - m[3] * 0.5f;
    b[2] = m[0] + m[2] * 0.5f;
    b[3] = m[1] + m[3] * 0.5f;
    return b;
}

inline float iou_box(const Box4& a, const Box4& b) {
    const float ix1 = std::max(a[0], b[0]);
    const float iy1 = std::max(a[1], b[1]);
    const float ix2 = std::min(a[2], b[2]);
    const float iy2 = std::min(a[3], b[3]);
    const float iw = ix2 - ix1;
    const float ih = iy2 - iy1;
    if (iw <= 0.0f || ih <= 0.0f) return 0.0f;
    const float inter = iw * ih;
    const float ua = (a[2] - a[0]) * (a[3] - a[1]) +
                     (b[2] - b[0]) * (b[3] - b[1]) - inter;
    return ua > 0.0f ? inter / ua : 0.0f;
}

// ---- track + detection records --------------------------------------------

struct STrack {
    Vec8        mean{};
    Mat88       cov{};
    Box4        tlwh{};        // last measurement as x, y, w, h
    float       score = 0.0f;
    int         track_id = 0;
    int         frame_id = 0;
    int         start_frame = 0;
    int         end_frame = 0;
    bool        activated = false;   // survived at least 2 frames
    bool        lost = false;
    int         class_id = 0;
    std::string class_name = "person";
    // Latest matched detection's keypoints (pose model only; the tracker
    // itself is bbox-only, these are carried through untouched).
    std::vector<Keypoint> kpts;
};

struct DetBox {
    Box4        xyxy{};
    float       score = 0.0f;
    int         class_id = 0;
    std::string class_name = "person";
    std::vector<Keypoint> kpts;
};

struct MatchResult {
    std::vector<std::pair<int, int>> matches;
    std::vector<char>                track_used;
    std::vector<char>                det_used;
};

// Greedy IoU association. `fuse_score` reproduces ByteTrack's fuse_score():
//   cost = 1 - iou * det_score
MatchResult greedy_match(const std::vector<const STrack*>& tracks,
                         const std::vector<const DetBox*>& dets,
                         float thresh, bool fuse_score) {
    MatchResult res;
    res.track_used.assign(tracks.size(), 0);
    res.det_used.assign(dets.size(), 0);

    struct Pair { float cost; int t; int d; };
    std::vector<Pair> cand;
    const int nt = static_cast<int>(tracks.size());
    const int nd = static_cast<int>(dets.size());
    for (int t = 0; t < nt; ++t) {
        const Box4 tb = box_from_mean(tracks[t]->mean);
        for (int d = 0; d < nd; ++d) {
            const float iou = iou_box(tb, dets[d]->xyxy);
            const float cost = fuse_score
                                   ? 1.0f - iou * dets[d]->score
                                   : 1.0f - iou;
            if (cost > thresh) continue;
            cand.push_back(Pair{cost, t, d});
        }
    }

    std::sort(cand.begin(), cand.end(),
              [](const Pair& a, const Pair& b) { return a.cost < b.cost; });
    for (const Pair& p : cand) {
        if (res.track_used[p.t] || res.det_used[p.d]) continue;
        res.track_used[p.t] = 1;
        res.det_used[p.d] = 1;
        res.matches.emplace_back(p.t, p.d);
    }
    return res;
}

} // namespace

class ByteTrackTracker : public ITracker {
public:
    void configure(const TrackerConfig& cfg) override;
    std::vector<TrackedObject> update(const std::vector<Detection>& dets) override;

private:
    void activate(STrack& t, int frame_id);
    void re_activate(STrack& t, const DetBox& d, int frame_id);
    void refresh(STrack& t, const DetBox& d, int frame_id);

    TrackerConfig        cfg_{};
    std::vector<STrack>  tracked_;
    std::vector<STrack>  lost_;
    int                  frame_id_ = 0;
    int                  next_id_ = 1;
    int                  max_time_lost_ = 30;
};

void ByteTrackTracker::configure(const TrackerConfig& cfg) {
    cfg_ = cfg;
    const double fr = cfg.frame_rate > 0 ? static_cast<double>(cfg.frame_rate) : 30.0;
    max_time_lost_ = static_cast<int>(fr / 30.0 * static_cast<double>(cfg.track_buffer));
    if (max_time_lost_ < 1) max_time_lost_ = 1;
}

void ByteTrackTracker::activate(STrack& t, int frame_id) {
    const float z[4] = {t.tlwh[0] + t.tlwh[2] * 0.5f,
                        t.tlwh[1] + t.tlwh[3] * 0.5f,
                        t.tlwh[2], t.tlwh[3]};
    kf_initiate(t.mean, t.cov, z);
    t.track_id    = next_id_++;
    t.frame_id    = frame_id;
    t.start_frame = frame_id;
    t.end_frame   = frame_id;
    // The reference implementation only auto-confirms tracks born on frame 1;
    // every later track has to be seen twice before it is reported.
    t.activated   = (frame_id == 1);
    t.lost        = false;
}

void ByteTrackTracker::re_activate(STrack& t, const DetBox& d, int frame_id) {
    const float z[4] = {(d.xyxy[0] + d.xyxy[2]) * 0.5f,
                        (d.xyxy[1] + d.xyxy[3]) * 0.5f,
                        d.xyxy[2] - d.xyxy[0],
                        d.xyxy[3] - d.xyxy[1]};
    kf_update(t.mean, t.cov, z);
    t.tlwh = Box4{d.xyxy[0], d.xyxy[1],
                  d.xyxy[2] - d.xyxy[0], d.xyxy[3] - d.xyxy[1]};
    t.score       = d.score;
    t.class_id    = d.class_id;
    t.class_name  = d.class_name;
    t.kpts        = d.kpts;
    t.frame_id    = frame_id;
    t.end_frame   = frame_id;
    t.activated   = true;
    t.lost        = false;
}

void ByteTrackTracker::refresh(STrack& t, const DetBox& d, int frame_id) {
    const float z[4] = {(d.xyxy[0] + d.xyxy[2]) * 0.5f,
                        (d.xyxy[1] + d.xyxy[3]) * 0.5f,
                        d.xyxy[2] - d.xyxy[0],
                        d.xyxy[3] - d.xyxy[1]};
    kf_update(t.mean, t.cov, z);
    t.tlwh = Box4{d.xyxy[0], d.xyxy[1],
                  d.xyxy[2] - d.xyxy[0], d.xyxy[3] - d.xyxy[1]};
    t.score      = d.score;
    t.class_id   = d.class_id;
    t.class_name = d.class_name;
    t.kpts       = d.kpts;
    t.frame_id   = frame_id;
    t.end_frame  = frame_id;
    t.activated  = true;
    t.lost       = false;
}

std::vector<TrackedObject> ByteTrackTracker::update(
    const std::vector<Detection>& dets) {
    ++frame_id_;

    // ---- split detections into high / low score --------------------------
    std::vector<DetBox> boxes;
    boxes.reserve(dets.size());
    for (const Detection& d : dets) {
        DetBox b;
        b.xyxy       = Box4{d.x1, d.y1, d.x2, d.y2};
        b.score      = d.confidence;
        b.class_id   = d.class_id;
        b.class_name = d.class_name;
        b.kpts       = d.keypoints;
        boxes.push_back(std::move(b));
    }
    // `boxes` is pre-reserved, so these pointers stay valid.
    std::vector<const DetBox*> hi, lo;
    for (const DetBox& b : boxes) {
        if (b.score >= cfg_.high_threshold) hi.push_back(&b);
        else                                lo.push_back(&b);
    }

    // ---- split last frame's tracks into confirmed / unconfirmed ----------
    std::vector<STrack> unconfirmed;
    std::vector<STrack> confirmed;
    for (const STrack& t : tracked_) {
        if (t.activated) confirmed.push_back(t);
        else             unconfirmed.push_back(t);
    }

    // ---- pool: confirmed + lost; predict all of them ---------------------
    std::vector<STrack> pool;
    pool.reserve(confirmed.size() + lost_.size());
    for (STrack& t : confirmed) pool.push_back(t);
    for (STrack& t : lost_)     pool.push_back(t);
    for (STrack& t : pool)      kf_predict(t.mean, t.cov);

    std::vector<const STrack*> tptr;
    tptr.reserve(pool.size());
    for (const STrack& t : pool) tptr.push_back(&t);

    std::vector<STrack> activated_out;
    std::vector<STrack> refind_out;
    std::vector<STrack> new_lost;
    std::vector<char>   pool_matched(pool.size(), 0);

    // ---- pass 1: pool vs high-score detections ---------------------------
    const MatchResult m1 = greedy_match(tptr, hi, cfg_.match_threshold, true);
    std::vector<char> hi_used(hi.size(), 0);
    for (const std::pair<int, int>& pr : m1.matches) {
        pool_matched[static_cast<size_t>(pr.first)] = 1;
        hi_used[static_cast<size_t>(pr.second)] = 1;
        STrack& t = pool[static_cast<size_t>(pr.first)];
        const DetBox& d = *hi[static_cast<size_t>(pr.second)];
        if (t.lost) {
            re_activate(t, d, frame_id_);
            refind_out.push_back(t);
        } else {
            refresh(t, d, frame_id_);
            activated_out.push_back(t);
        }
    }

    // ---- pass 2: unmatched *tracked* tracks vs low-score detections ------
    std::vector<const STrack*> r_tracks;
    std::vector<int>           r_index;
    for (size_t i = 0; i < pool.size(); ++i) {
        if (pool_matched[i] || pool[i].lost) continue;
        r_tracks.push_back(&pool[i]);
        r_index.push_back(static_cast<int>(i));
    }
    const MatchResult m2 = greedy_match(r_tracks, lo, 0.5f, false);
    for (const std::pair<int, int>& pr : m2.matches) {
        STrack& t = pool[static_cast<size_t>(r_index[static_cast<size_t>(pr.first)])];
        refresh(t, *lo[static_cast<size_t>(pr.second)], frame_id_);
        activated_out.push_back(t);
    }
    for (size_t i = 0; i < r_tracks.size(); ++i) {
        if (m2.track_used[i]) continue;
        STrack& t = pool[static_cast<size_t>(r_index[i])];
        t.lost      = true;
        t.end_frame = frame_id_;
        new_lost.push_back(t);
    }

    // ---- pass 3: unconfirmed tracks vs leftover high-score detections ----
    std::vector<const DetBox*> rem_hi;
    for (size_t i = 0; i < hi.size(); ++i)
        if (!hi_used[i]) rem_hi.push_back(hi[i]);

    std::vector<const STrack*> uptr;
    uptr.reserve(unconfirmed.size());
    for (const STrack& t : unconfirmed) uptr.push_back(&t);
    const MatchResult m3 = greedy_match(uptr, rem_hi, 0.7f, false);
    for (const std::pair<int, int>& pr : m3.matches) {
        refresh(unconfirmed[static_cast<size_t>(pr.first)],
                *rem_hi[static_cast<size_t>(pr.second)], frame_id_);
        activated_out.push_back(unconfirmed[static_cast<size_t>(pr.first)]);
    }
    // Unmatched unconfirmed tracks are dropped: they never got a second look.

    // ---- init new tracks from the still-unused high-score detections ------
    for (size_t i = 0; i < rem_hi.size(); ++i) {
        if (m3.det_used[i]) continue;
        const DetBox& d = *rem_hi[i];
        STrack t;
        t.tlwh = Box4{d.xyxy[0], d.xyxy[1],
                      d.xyxy[2] - d.xyxy[0], d.xyxy[3] - d.xyxy[1]};
        t.score      = d.score;
        t.class_id   = d.class_id;
        t.class_name = d.class_name;
        t.kpts       = d.kpts;
        activate(t, frame_id_);
        activated_out.push_back(t);
    }

    // ---- expire lost tracks that were not re-found -----------------------
    for (size_t i = 0; i < pool.size(); ++i) {
        if (pool_matched[i] || !pool[i].lost) continue;
        if (frame_id_ - pool[i].end_frame > max_time_lost_) continue;   // dropped
        new_lost.push_back(pool[i]);
    }

    tracked_ = std::move(activated_out);
    for (STrack& t : refind_out) tracked_.push_back(std::move(t));
    lost_ = std::move(new_lost);

    // ---- report confirmed tracks only ------------------------------------
    std::vector<TrackedObject> out;
    out.reserve(tracked_.size());
    for (const STrack& t : tracked_) {
        if (!t.activated) continue;
        const Box4 b = box_from_mean(t.mean);
        TrackedObject o;
        o.x1 = b[0];
        o.y1 = b[1];
        o.x2 = b[2];
        o.y2 = b[3];
        o.track_id   = t.track_id;
        o.confidence = t.score;
        o.class_id   = t.class_id;
        o.class_name = t.class_name;
        o.keypoints  = t.kpts;
        out.push_back(std::move(o));
    }
    return out;
}

std::unique_ptr<ITracker> create_tracker() {
    return std::make_unique<ByteTrackTracker>();
}

} // namespace ca
