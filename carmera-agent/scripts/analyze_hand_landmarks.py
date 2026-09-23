#!/usr/bin/env python3
"""Offline calibration helper for hand_classify_gesture().

Reads a `metadata-mock-server.py --dump` log, recomputes the exact quantities
the C++ classifier uses, and prints their distribution.

Why offline: the wire payload already carries the 21 landmarks of every detected
hand (`hand.landmarks`, image pixels, 2 decimals).  That is >= 100x finer than
the ratios we are calibrating, so no rebuild / re-run is needed to move the
thresholds -- this script must mirror `hand_analyzer.cpp` exactly:

    constexpr int kWristIdx = 0;
    constexpr int kTips[4]  = {8, 12, 16, 20};   // index / middle / ring / pinky
    constexpr int kMcps[4]  = {5,  9, 13, 17};
    ratio[f] = dist3(lm[kTips[f]], lm[kWristIdx]) / dist3(lm[kMcps[f]], lm[kWristIdx])
    extended if ratio > kExtendedRatio (1.15)
    curled   if ratio < kCurledRatio   (1.10; was 0.90 before the live calibration)

Usage:
    python scripts/analyze_hand_landmarks.py mock.jsonl
    python scripts/analyze_hand_landmarks.py mock.jsonl --hist
    python scripts/analyze_hand_landmarks.py mock.jsonl --tail 30      # newest pose only
    python scripts/analyze_hand_landmarks.py mock.jsonl --gesture FIST
    python scripts/analyze_hand_landmarks.py mock.jsonl --msg-from 660 --msg-to 690
    python scripts/analyze_hand_landmarks.py mock.jsonl --timeline 60  # split a capture by eye

Where the log comes from:
    python scripts/metadata-mock-server.py --port 8099 --dump > mock.jsonl

Promoted from a one-off scratch script because threshold calibration keeps coming
back: it is the only place that mirrors hand_classify_gesture() in another language,
so a future threshold change should be re-checked against it.
"""

import argparse
import json
import math
import re
import sys
from collections import defaultdict

# Mirror of the constants in src/ai/hand_analyzer.cpp. KEEP IN SYNC - if these
# drift from the C++, every distribution below is measured against the wrong
# reference lines and the "below it / dead band" counts stop meaning anything.
K_EXTENDED = 1.15
K_CURLED   = 1.10      # was 0.90 until the 2026-09-23 live calibration

WRIST = 0
TIPS = (8, 12, 16, 20)
MCPS = (5, 9, 13, 17)
FINGERS = ("index", "middle", "ring", "pinky")

LINE_RE = re.compile(r"^\[mock\] #(\d+) (\{.*\})\s*$")


def dist3(a, b):
    return math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2)


def ratios(lm):
    """The four d_tip/d_mcp ratios, or None if any landmark is missing."""
    if len(lm) < 21:
        return None
    w = lm[WRIST]
    out = []
    for tip, mcp in zip(TIPS, MCPS):
        d_tip = dist3(lm[tip], w)
        d_mcp = dist3(lm[mcp], w)
        if d_mcp < 0.5:          # kMinSpanPx
            out.append(None)
        else:
            out.append(d_tip / d_mcp)
    return out


def load(path):
    """-> list of (seq, camera_id, frame_id, gesture, conf, ratios)"""
    rows = []
    bad = 0
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = LINE_RE.match(line.rstrip("\n"))
            if not m:
                continue
            try:
                msg = json.loads(m.group(2))
            except json.JSONDecodeError:
                bad += 1
                continue
            if msg.get("type") != "frame":
                continue
            hand = msg.get("hand")
            if not hand or not hand.get("detected"):
                continue
            lm = hand.get("landmarks")
            r = ratios(lm) if lm else None
            rows.append((
                int(m.group(1)),
                msg.get("camera_id"),
                msg.get("frame_id"),
                hand.get("gesture"),
                hand.get("confidence") or 0.0,
                r,
            ))
    if bad:
        print("  (%d unparsable lines skipped)" % bad, file=sys.stderr)
    return rows


def quantiles(vals, qs=(0.0, 0.05, 0.25, 0.5, 0.75, 0.95, 1.0)):
    if not vals:
        return None
    s = sorted(vals)
    out = []
    for q in qs:
        idx = min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))
        out.append(s[idx])
    return out


def otsu(values):
    """Classic Otsu threshold over a 1-D sample. Picks the split that maximises
    the between-class variance - i.e. the valley between the fist and the
    open-palm modes, without assuming where either mode sits."""
    lo, hi = min(values), max(values)
    if hi - lo < 1e-6:
        return hi
    bins = 256
    hist = [0] * bins
    scale = (bins - 1) / (hi - lo)
    for v in values:
        hist[int((v - lo) * scale)] += 1
    total = len(values)
    sum_all = sum(i * h for i, h in enumerate(hist))
    sum_b = 0.0
    w_b = 0
    best_t, best_var = lo, -1.0
    for i in range(bins):
        w_b += hist[i]
        if w_b == 0:
            continue
        w_f = total - w_b
        if w_f == 0:
            break
        sum_b += i * hist[i]
        m_b = sum_b / w_b
        m_f = (sum_all - sum_b) / w_f
        var = w_b * w_f * (m_b - m_f) ** 2
        if var > best_var:
            best_var, best_t = var, lo + i / scale
    return best_t


def compute_otsu_quality(values, t):
    """eta = between-class variance / total variance. 1.0 == perfect split."""
    lo = sum(values) / len(values)
    total_var = sum((v - lo) ** 2 for v in values) / len(values)
    if total_var < 1e-12:
        return 0.0
    a = [v for v in values if v < t]
    b = [v for v in values if v >= t]
    if not a or not b:
        return 0.0
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    between = (len(a) * (ma - lo) ** 2 + len(b) * (mb - lo) ** 2) / len(values)
    return between / total_var


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--hist", action="store_true", help="print a 0.05-wide histogram")
    ap.add_argument("--tail", type=int, default=0,
                    help="only analyse the LAST N detected-hand frames "
                         "(use this to isolate the newest pose from the log)")
    ap.add_argument("--gesture", default="",
                    help="only analyse frames the agent labelled with this gesture")
    ap.add_argument("--timeline", type=int, default=0, metavar="N",
                    help="print the last N detected frames as id/gesture/median, "
                         "so a capture can be split into pose segments by eye")
    ap.add_argument("--msg-from", type=int, default=0, metavar="N",
                    help="only analyse messages with arrival # >= N")
    ap.add_argument("--msg-to", type=int, default=0, metavar="N",
                    help="only analyse messages with arrival # <= N")
    args = ap.parse_args()

    rows = load(args.log)
    if args.msg_from:
        rows = [r for r in rows if r[0] >= args.msg_from]
    if args.msg_to:
        rows = [r for r in rows if r[0] <= args.msg_to]
    if args.timeline:
        print("last %d detected frames (msg # is arrival order = time proxy)"
              % args.timeline)
        print("   msg#   frame_id   gesture        median")
        for seq, cam, fid, g, conf, r in rows[-args.timeline:]:
            vals = [v for v in (r or []) if v is not None]
            m = sorted(vals)[len(vals) // 2] if vals else float("nan")
            print("  %6d  %8d   %-13s  %.3f" % (seq, fid, g, m))
    if args.gesture:
        rows = [r for r in rows if r[3] == args.gesture]
    if args.tail > 0:
        rows = rows[-args.tail:]
    if not rows:
        print("no detected-hand frames found in %s" % args.log)
        print("=> the capture caught no hand; re-run it with a hand in front of the camera")
        return 1

    print("detected-hand frames: %d" % len(rows))

    # ---- gesture tally (what the shipped classifier decided) -----------------
    tally = defaultdict(int)
    for _, _, _, g, _, _ in rows:
        tally[g] += 1
    print("\ngesture verdicts emitted by the agent:")
    for g, n in sorted(tally.items(), key=lambda kv: -kv[1]):
        print("  %-12s %5d  (%.1f%%)" % (g, n, 100.0 * n / len(rows)))

    # ---- per-finger ratio distribution --------------------------------------
    print("\nper-finger dist3(tip,wrist)/dist3(mcp,wrist)  [px, incl. z]")
    print("  %-7s %8s %8s %8s %8s %8s %8s   %5s" %
          ("finger", "min", "p05", "p25", "p50", "p75", "max", "n"))
    for f in range(4):
        vals = [r[f] for *_, r in rows if r and r[f] is not None]
        q = quantiles(vals)
        if q is None:
            print("  %-7s  (no samples)" % FINGERS[f])
            continue
        print("  %-7s %8.4f %8.4f %8.4f %8.4f %8.4f %8.4f   %5d" %
              (FINGERS[f], q[0], q[1], q[2], q[3], q[4], q[5], len(vals)))

    # ---- the two thresholds this data has to separate ------------------------
    allv = [v for *_, r in rows if r for v in r if v is not None]
    if allv:
        s = sorted(allv)
        print("\nall-finger ratios pooled: n=%d  min=%.4f  max=%.4f" % (len(s), s[0], s[-1]))
        print("  kExtendedRatio = %.2f  -> above it: %d samples (%.1f%%)"
              % (K_EXTENDED, sum(1 for v in s if v > K_EXTENDED),
                 100.0 * sum(1 for v in s if v > K_EXTENDED) / len(s)))
        print("  kCurledRatio   = %.2f  -> below it: %d samples (%.1f%%)"
              % (K_CURLED, sum(1 for v in s if v < K_CURLED),
                 100.0 * sum(1 for v in s if v < K_CURLED) / len(s)))

        # The dead band between the two thresholds is where a gesture vote is
        # lost.  Report it, because a fist that lands inside the dead band is
        # exactly the failure mode a wrong kCurledRatio produces.
        band = [v for v in s if K_CURLED <= v <= K_EXTENDED]
        print("  dead band [%.2f, %.2f]: %d samples (%.1f%%)"
              % (K_CURLED, K_EXTENDED, len(band), 100.0 * len(band) / len(s)))

    if args.hist and allv:
        print("\nhistogram (bin=0.05)")
        bins = defaultdict(int)
        for v in allv:
            bins[math.floor(v / 0.05) * 0.05] += 1
        for b in sorted(bins):
            print("  %5.2f-%-5.2f %5d  %s" % (b, b + 0.05, bins[b], "#" * min(60, bins[b])))

    # ---- recommended kCurledRatio -------------------------------------------
    # Per-finger samples are noisy (a single blurry finger can read 0.85 on an
    # open palm), so thresholding them directly overfits.  Collapse each FRAME
    # to the median of its four ratios first: that is closer to the quantity the
    # vote actually acts on.
    #
    # CAVEAT, and it matters: transitional frames (hand entering/leaving, motion
    # blur) sit in the valley by construction and are the majority of a casual
    # capture.  They drag Otsu *up*.  Treat t as an upper bound, and only trust
    # it after `--steady` has removed the transitions.
    med = []
    for *_, r in rows:
        if not r:
            continue
        vals = [v for v in r if v is not None]
        if vals:
            med.append(sorted(vals)[len(vals) // 2])
    if len(med) >= 20:
        t = otsu(med)
        below = sum(1 for v in med if v < t)
        ratio = compute_otsu_quality(med, t)
        print("\nper-FRAME median of the four ratios -> Otsu threshold")
        print("  frames=%d  Otsu t=%.4f   eta(between/total var)=%.4f"
              % (len(med), t, ratio))
        print("  frames below t: %d (%.1f%%)   above: %d (%.1f%%)"
              % (below, 100.0 * below / len(med), len(med) - below,
                 100.0 * (len(med) - below) / len(med)))

        # The suggestion must respect the design invariant kCurledRatio <
        # kExtendedRatio, otherwise the two bands invert and no ratio can ever
        # be both, which silently kills the dead band the classifier relies on.
        KE = K_EXTENDED
        sugg = min(KE - 0.05, max(0.80, min(t - 0.10, KE - 0.05)))
        print("  => kCurledRatio must satisfy kCurledRatio < kExtendedRatio (=%.2f)." % KE)
        print("     suggested kCurledRatio <= %.2f   (shipped: %.2f, from the C++ constants)"
              % (sugg, K_CURLED))
        if abs(K_CURLED - 1.10) > 1e-9:
            print("     NOTE: K_CURLED here (%.2f) no longer matches the 2026-09-23 "
                  "calibration (1.10) - update both it and the C++ together." % K_CURLED)
        if t >= KE:
            print("     WARNING: Otsu landed at/above kExtendedRatio -> the capture "
                  "is dominated by transitional frames. Re-capture holding each "
                  "pose STEADY (~20s, then switch) and re-run.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
