#!/usr/bin/env python
"""spec-16 evidence: are the three logical hand axes independent?

WHY THIS EXISTS
---------------
`hand_map_coordinates()` derives all three logical axes from ONE detection box:

    x = (x1 + x2) / 2 / fw          box centre, horizontal
    z = (fh - (y1 + y2) / 2) / fh   box centre, vertical
    y = sqrt(bw * bh) / fh          box SIZE  (geometric mean of the sides)

A rectangle has four degrees of freedom (centre 2 + sides 2) and is squeezed
into three outputs, so the axes cannot be independent by construction. Whether
they are *usefully* independent is an empirical question, and on this camera
mounting the answer is no: the measured corr(position.Y, position.Z) is +0.84,
and 94.7% of the observed motion variance lies in a 2-D subspace.

That matters because `depth_source = size` is supposed to make Y mean
"closer / farther" (spec 21). Under a camera whose optical axis is not
perpendicular to the hand's plane, moving the hand UP also brings it CLOSER, so
"up" and "forward" are the same physical motion. No setting of invert_x/y/z or
depth_source can undo that - it is geometry, not a sign convention.

MEASUREMENT DISCIPLINE (learned the hard way while writing this)
---------------------------------------------------------------
Compare LIKE FOR LIKE. An earlier version of this analysis compared
corr(position.Y, position.Z) = +0.837 against corr(hand size, hand cent.y) =
-0.492 and concluded "a landmark-derived scale would halve the coupling". Those
are different PAIRS, so the comparison was meaningless. The correct same-pair
numbers are corr(lmspan, Z) = +0.782 and corr(lmsize, Z) = +0.734, i.e. R^2
drops only 0.70 -> 0.54..0.61, about 15%. Hence: the coupling is dominated by
perspective, and swapping the size formula does not fix it.

The script therefore always prints the whole correlation matrix on ONE frame
set, so no subset mismatch can manufacture a difference.

USAGE
-----
    python scripts/verify_hand_axis_coupling.py               # most recent session
    python scripts/verify_hand_axis_coupling.py --all         # everything recorded
    python scripts/verify_hand_axis_coupling.py --since 2026-09-24T01:23:17
    python scripts/verify_hand_axis_coupling.py --file dump.json
    python scripts/verify_hand_axis_coupling.py --gate        # exit 1 if the
                                                              # mounting changed

`--gate` is for regression: the documented conclusion is "2-DOF, Y coupled".
If the camera is re-aimed so its axis is perpendicular to the hand plane, the
coupling drops and this returns 1, which means the §15 document is now stale.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import urllib.request

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


# --- the recorded conclusion, so `--gate` has a baseline ---------------------
# 2026-09-24, camera01, 1160 frames, camera mounted obliquely to the hand plane.
# See docs/prompter/hand-3d-phase1-architecture.md §15.
RECORDED_CORR_YZ = 0.84
COUPLED_MIN = 0.60       # above this, Y cannot be called an independent axis
RANK2_MIN_SHARE = 0.90   # PC1+PC2 share that makes the motion 2-DOF
MIN_FRAMES = 60          # below this the correlations are not worth quoting


def http_json(url, timeout=60):
    op = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with op.open(url, timeout=timeout) as fh:
        return json.loads(fh.read().decode("utf-8"))


def corr(a, b):
    n = len(a)
    if n < 2:
        return 0.0
    ma, mb = sum(a) / n, sum(b) / n
    va = sum((v - ma) ** 2 for v in a)
    vb = sum((v - mb) ** 2 for v in b)
    if va <= 0 or vb <= 0:
        return 0.0
    return sum((a[i] - ma) * (b[i] - mb) for i in range(n)) / math.sqrt(va * vb)


def d2(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


# ---------------------------------------------------------------------------
# data
# ---------------------------------------------------------------------------
def load_rows(args):
    """Return (rows, provenance). Accepts either an export or the live API."""
    if args.file:
        with open(args.file, encoding="utf-8") as fh:
            doc = json.load(fh)
        rows = doc.get("rows") or doc.get("frames") or []
        return rows, f"file {args.file}"

    base, cam = args.base_url.rstrip("/"), args.camera
    if args.all:
        listing = http_json(f"{base}/api/cameras/{cam}/hand/sessions"
                            f"?gap_ms={args.gap_ms}")
        sess = listing.get("sessions") or []
        if not sess:
            return [], "live API (no sessions recorded)"
        rows = []
        for s in sess:
            d = http_json(f"{base}/api/cameras/{cam}/hand/sessions/{s['index']}"
                          f"?gap_ms={args.gap_ms}")
            rows.extend(d.get("frames") or [])
        return rows, f"live API, ALL {len(sess)} session(s)"

    idx = args.session
    if idx <= 0:
        listing = http_json(f"{base}/api/cameras/{cam}/hand/sessions"
                            f"?gap_ms={args.gap_ms}")
        sess = listing.get("sessions") or []
        if not sess:
            return [], "live API (no sessions recorded)"
        idx = max(s["index"] for s in sess)
    d = http_json(f"{base}/api/cameras/{cam}/hand/sessions/{idx}"
                  f"?gap_ms={args.gap_ms}")
    return d.get("frames") or [], f"live API, session {idx}"


def usable(rows, since):
    """One pass, one frame set - every statistic below shares it."""
    out = []
    for r in rows:
        if since and (r.get("received_at") or "") < since:
            continue
        h = r.get("hand") or {}
        pos = h.get("position")
        if not pos or len(pos) != 3:
            continue
        if not h.get("detected"):
            # position is [0,0,0] when nothing is detected; keeping those would
            # manufacture one enormous excursion per drop-out.
            continue
        lm = h.get("landmarks")
        rec = {"fid": r.get("frame_id"), "ts": r.get("received_at"),
               "gesture": h.get("gesture"),
               "X": float(pos[0]), "Y": float(pos[1]), "Z": float(pos[2]),
               "lmx": None, "lmy": None, "lmsize": None, "lmspan": None}
        if isinstance(lm, list) and len(lm) == 21:
            try:
                P = [(float(q[0]), float(q[1])) for q in lm]
                xs = [q[0] for q in P]
                ys = [q[1] for q in P]
                bw, bh = max(xs) - min(xs), max(ys) - min(ys)
                if bw > 0 and bh > 0:
                    rec["lmx"] = sum(xs) / 21
                    rec["lmy"] = sum(ys) / 21
                    rec["lmsize"] = math.sqrt(bw * bh)
                    rec["lmspan"] = sum([d2(P[0], P[9]), d2(P[0], P[5]),
                                         d2(P[5], P[17]), d2(P[9], P[12])]) / 4
            except (TypeError, ValueError, IndexError):
                pass
        out.append(rec)
    return out


# ---------------------------------------------------------------------------
# statistics
# ---------------------------------------------------------------------------
def principal_shares(vs):
    """Variance shares of the 3-vector motion, by power iteration with deflation."""
    n = len(vs)
    c = [[0.0] * 3 for _ in range(3)]
    mean = [sum(v[k] for v in vs) / n for k in range(3)]
    for v in vs:
        d = [v[k] - mean[k] for k in range(3)]
        for a in range(3):
            for b in range(3):
                c[a][b] += d[a] * d[b]
    for a in range(3):
        for b in range(3):
            c[a][b] /= n

    out, m = [], [row[:] for row in c]
    for _ in range(3):
        v = [1.0, 1.0, 1.0]
        for _ in range(200):
            w = [sum(m[a][b] * v[b] for b in range(3)) for a in range(3)]
            nrm = math.sqrt(sum(t * t for t in w))
            if nrm < 1e-15:
                break
            v = [t / nrm for t in w]
        lam = sum(v[a] * sum(m[a][b] * v[b] for b in range(3))
                  for a in range(3))
        out.append((lam, v))
        for a in range(3):
            for b in range(3):
                m[a][b] -= lam * v[a] * v[b]
    tot = sum(e for e, _ in out)
    if tot <= 0:
        return [], 0.0
    return [(e / tot, v) for e, v in out], tot


def per_motion(rows):
    """Count motions whose dY and dZ share a sign. Deliberately simple: this is
    a qualitative statement ('every motion was up-and-closer'), not a repeat of
    the segmentation that verify_hand_direction.py owns."""
    runs, cur = [], []
    for r in rows:
        cur.append(r)
        if len(cur) >= 2:
            if abs(cur[-1]["Y"] - cur[-2]["Y"]) < 1e-9:
                continue
        if len(cur) >= 20:
            runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    same = tot = 0
    for run in runs:
        dy = run[-1]["Y"] - run[0]["Y"]
        dz = run[-1]["Z"] - run[0]["Z"]
        if abs(dy) < 0.02 or abs(dz) < 0.02:
            continue
        tot += 1
        if dy * dz > 0:
            same += 1
    return same, tot


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default="http://127.0.0.1:8081")
    ap.add_argument("--camera", default="camera01")
    ap.add_argument("--gap-ms", type=int, default=1000)
    ap.add_argument("--session", type=int, default=0,
                    help="session index; 0 = the most recent one")
    ap.add_argument("--all", action="store_true",
                    help="concatenate every recorded session")
    ap.add_argument("--file", default="",
                    help="read an exported hand_session_*.json or a raw dump")
    ap.add_argument("--since", default="",
                    help="only frames received at/after this UTC timestamp")
    ap.add_argument("--json-out", default="", help="also write the raw table")
    ap.add_argument("--gate", action="store_true",
                    help="exit 1 if the coupling has DROPPED below the "
                         "recorded level (i.e. the mounting changed and the "
                         "documented conclusion is now stale)")
    args = ap.parse_args()

    raw, provenance = load_rows(args)
    if not raw:
        print(f"no recorded rows ({provenance})")
        print("record some hand frames first: ai-hand mode + a hand in view")
        return 2
    rows = usable(raw, args.since)
    print("=" * 74)
    print(" spec-16 axis coupling - are X / Y / Z independent measurements?")
    print("=" * 74)
    print(f"  source     : {provenance}")
    print(f"  rows       : {len(raw)} read, {len(rows)} with a detected position")
    if args.since:
        print(f"  since      : {args.since}")
    if len(rows) < MIN_FRAMES:
        print(f"\n  only {len(rows)} usable frames (< {MIN_FRAMES}); "
              "correlations would not be worth quoting")
        return 2

    gestures = {}
    for r in rows:
        gestures[r.get("gesture") or "?"] = gestures.get(r.get("gesture") or "?", 0) + 1

    # ---- correlation matrix, one frame set ---------------------------------
    cols = {"X": [], "Y": [], "Z": []}
    for r in rows:
        for k in cols:
            cols[k].append(r[k])
    lmk = [r for r in rows if r["lmsize"] is not None]
    labels = ["X", "Y", "Z"]
    if lmk:
        for k in ("lmx", "lmy", "lmsize", "lmspan"):
            cols[k] = [r[k] for r in lmk]
        labels += ["lmx", "lmy", "lmsize", "lmspan"]

    print()
    print("  correlation matrix (one frame set, so no subset mismatch)")
    print("    " + "%-9s" % "" + "".join("%9s" % k for k in labels))
    for a in labels:
        print("    %-9s" % a
              + "".join("%+9.3f" % corr(cols[a], cols[b]) for b in labels))

    # ---- span and effective rank -------------------------------------------
    print()
    print("  span of each reported axis")
    for k in "XYZ":
        v = cols[k]
        print(f"    {k}: {min(v):+.4f} .. {max(v):+.4f}   span {max(v) - min(v):.4f}")

    vecs = [{"X": r["X"], "Y": r["Y"], "Z": r["Z"]} for r in rows]
    shares, _ = principal_shares([[v["X"], v["Y"], v["Z"]] for v in vecs])
    print()
    print("  principal directions of the 3-vector motion")
    for i, (s, v) in enumerate(shares, 1):
        print(f"    #{i}  {100.0 * s:5.1f}% of variance   "
              f"axis [{v[0]:+.2f} {v[1]:+.2f} {v[2]:+.2f}]")
    rank2 = (shares[0][0] + shares[1][0]) if len(shares) >= 2 else 0.0

    # ---- where the coupling comes from -------------------------------------
    r_yz = corr(cols["Y"], cols["Z"])
    print()
    print("  where the Y-Z coupling comes from  (like-for-like pairs only)")
    print(f"    corr(position.Y, position.Z)   = {r_yz:+.3f}   <- as reported")
    if lmk:
        print(f"    corr(lmsize, position.Z)       = {corr(cols['lmsize'], cols['Z']):+.3f}"
              "   <- landmark-only size, same pair")
        print(f"    corr(lmspan, position.Z)       = {corr(cols['lmspan'], cols['Z']):+.3f}"
              "   <- pose-robust rigid spans, same pair")
        print("    => if these barely differ, the coupling is PERSPECTIVE, not the "
              "detector box,")
        print("       and changing the size formula would not help.")

    same, tot = per_motion(rows)
    print()
    print(f"  monotonic runs where dY and dZ share a sign : {same} / {tot}")
    print("    (this script's own crude run split, as an independent check on the")
    print("     segmentation that verify_hand_direction.py owns. The two methods")
    print("     produce different denominators by design; do not mix the ratios.)")

    # ---- verdict ------------------------------------------------------------
    coupled = abs(r_yz) >= COUPLED_MIN
    flat = rank2 >= RANK2_MIN_SHARE
    print()
    print("=" * 74)
    if coupled and flat:
        print(" VERDICT: 3 logical axes, 2 effective degrees of freedom.")
        print(f"   corr(Y, Z) = {r_yz:+.3f} >= {COUPLED_MIN}  -> Y is not an "
              "independent axis")
        print(f"   PC1+PC2    = {100.0 * rank2:.1f}% >= "
              f"{100.0 * RANK2_MIN_SHARE:.0f}%  -> the motion is 2-D")
        print("   Y (depth_source=size) tracks 'how big the hand looks', and on a "
              "tilted")
        print("   mounting 'up' also means 'closer', so up and forward are one "
              "motion.")
        print("   invert_x/y/z and depth_source cannot repair this; re-aim the "
              "camera.")
    elif coupled:
        print(f" VERDICT: Y is coupled to Z (corr {r_yz:+.3f}) but the motion was "
              f"not flat ({100.0 * rank2:.1f}%).")
        print("   The operator probably did move along the optical axis as well.")
    else:
        print(f" VERDICT: the axes look separable (corr(Y, Z) = {r_yz:+.3f}).")
        print("   Either the camera is now perpendicular to the hand's plane, or "
              "the")
        print("   recording never exercised a vertical sweep. If the mounting was "
              "changed,")
        print("   update §15 - it currently documents the coupled case.")
    print("=" * 74)

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as fh:
            json.dump({"provenance": provenance, "frames": len(rows),
                       "gestures": gestures, "corr_Y_Z": r_yz,
                       "principal_shares": [s for s, _ in shares],
                       "same_sign_runs": [same, tot],
                       "recorded_corr_Y_Z": RECORDED_CORR_YZ}, fh, indent=1)
        print(f"  table -> {args.json_out}")

    if args.gate:
        # The gate is inverted on purpose: this script defends a NEGATIVE result.
        # A drop means the world changed, not that the code regressed.
        if abs(r_yz) < COUPLED_MIN:
            print(f"\n  GATE: coupling fell to {abs(r_yz):.3f} (< {COUPLED_MIN}) "
                  f"from the recorded {RECORDED_CORR_YZ:.2f}.")
            print("  The mounting changed. Re-derive §15 instead of trusting it.")
            return 1
        print(f"\n  GATE: still coupled ({abs(r_yz):.3f}); matches the recorded "
              f"{RECORDED_CORR_YZ:.2f}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
