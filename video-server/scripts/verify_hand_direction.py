#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
The spec-16 axis direction experiment: what does each logical axis MEAN on the
real camera, and what must invert_x / invert_y / invert_z be set to?

Why this cannot be answered by reading the code: hand_map_coordinates() maps
image pixels to a logical frame with hard-coded assumptions (X+ is image-right,
Z+ is image-up, Y+ is closer). Whether those coincide with the SEMANTICS the
gesture vocabulary promises - X+ towards the operator's own right, Y+ towards
the camera, Z+ up - depends on how the camera is physically mounted and how the
operator is standing. That is a hardware fact, so it has to be measured.

Method
------
Run this against a recorded session in which an operator performs a known
sequence of motions, each separated by a return to a neutral pose:

    python scripts/verify_hand_direction.py --session 42 --expect X+,Z+,Y+,Y-

A motion is found by ANCHOR AND PEAK, not by an instantaneous speed threshold:
holding a hand up produces continuous sub-deadzone jitter, so a speed test
merges minutes of standing still into one bogus burst. Instead the recording is
walked with a slowly-sliding neutral anchor; when the hand moves further than
`--min-excursion` from the anchor a motion OPENS, its furthest point becomes its
result (not its end, which is back at neutral), and it CLOSES once the hand has
returned near the anchor and stayed there for `--settle-ms`. The anchor then
re-centres, so the next motion is measured against the new resting pose.

Two independent things are then checked:

  1. the DOMINANT axis of motion k is the one the k-th motion was supposed to
     move. If not, the experiment measured the wrong axis - the failure mode
     spec 16's own note warns about ("the operator thinks they tested the other
     axis"), which is why the expected sequence is declared up front rather
     than inferred afterwards.
  2. the SIGN matches the promised semantics, which yields the invert_* values.

The recommendation it prints IS the configuration, but this script never edits
the agent's config - it only reports. It also never touches the model or the
detector, because correcting a mount is a configuration job (spec 17).

Reading the result
------------------
  Y is special. depth_source=size measures how LARGE the hand looks, which is
  monotonic with distance - so Y answers "did the hand move closer?". The other
  source, landmark_z, is the model's wrist-relative depth: it describes whether
  the FINGERS point at the camera, not where the hand is, and the wrist always
  has z == 0 (measured, see carmera-agent). The two are different physical
  quantities that share an axis, so the Y motion must be performed differently
  for each: move the whole hand closer / further away for "size", but point the
  fingers at / away from the camera for "landmark_z".

  Never read a value here as centimetres. These are normalised coordinates.

Usage
-----
  python scripts/verify_hand_direction.py --list
  python scripts/verify_hand_direction.py --session 42 --expect X+,Z+,Y+,Y-
  python scripts/verify_hand_direction.py --file hand_session_camera01_42.json \
         --expect X+,Z+,Y+,Y-
  python scripts/verify_hand_direction.py --session 42 --expect X+,Z+,Y+,Y- --json-out r.json
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.request

# The three logical axes, in the order they are reported.
AXES = ("x", "y", "z")

# What each axis is supposed to mean, in the operator's own frame. This is the
# contract the invert_* flags exist to satisfy - not a description of what the
# mapper currently does.
PROMISED = {
    "x": "X+ towards the operator's own RIGHT hand side",
    "y": "Y+ towards the camera (FORWARD)",
    "z": "Z+ up",
}

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def get_json(url, timeout=30):
    with OPENER.open(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def note(msg):
    print(f"  {msg}")


def maxabs(v):
    return max(abs(x) for x in v)


def sub(a, b):
    return [a[k] - b[k] for k in range(3)]


# ---------------------------------------------------------------------------
# segmentation
# ---------------------------------------------------------------------------
def load_frames(doc):
    """Recorded rows that carry a usable position, in append order."""
    rows = doc.get("frames") or []
    out = []
    for r in rows:
        h = r.get("hand")
        if not h or not h.get("detected"):
            continue
        pos = h.get("position")
        if not (isinstance(pos, list) and len(pos) == 3):
            continue
        out.append({
            "id": r.get("id"),
            "frame_id": r.get("frame_id"),
            "ts": r.get("timestamp"),
            "pos": [float(v) for v in pos],
            # The reference only latches once the hand has been stable, so a
            # recording that never latched still has usable POSITION (which is
            # what this experiment reads) but no usable delta.
            "latched": bool(h.get("reference_initialized")),
            "gesture": h.get("gesture"),
            "candidate": h.get("gesture_candidate"),
        })
    return out


def frame_interval_ms(frames):
    """Median gap between consecutive recorded timestamps. Used to turn a
    settle time into a frame count without assuming the AI fps."""
    gaps = sorted((b["ts"] or 0) - (a["ts"] or 0)
                  for a, b in zip(frames, frames[1:]))
    gaps = [g for g in gaps if g > 0]
    if not gaps:
        return 125.0
    mid = len(gaps) // 2
    return float(gaps[mid]) if len(gaps) % 2 else (gaps[mid - 1] + gaps[mid]) / 2.0


def find_rest_start(frames, settle_n, return_band):
    """Index of the first window of `settle_n` frames that are all within
    `return_band` of their own mean - i.e. the first place the hand is provably
    resting.

    The recording starts when a hand first appears, and a hand appearing IS an
    excursion: it lands somewhere, then settles at the operator's neutral pose.
    Treating that entry transient as a motion would consume one of the declared
    motions and shift every later comparison by one.
    """
    n = len(frames)
    for i in range(0, max(0, n - settle_n) + 1):
        w = [f["pos"] for f in frames[i:i + settle_n]]
        mean = [sum(p[k] for p in w) / settle_n for k in range(3)]
        if all(maxabs(sub(p, mean)) <= return_band for p in w):
            return i
    return 0


def segment(frames, min_excursion, return_band, settle_ms):
    """Anchor-and-peak segmentation. Returns (motions, frames_skipped).

    Each motion carries the neutral pose it started from and the furthest pose
    it reached.

    The anchor is the RESTING pose, and it re-centres ONLY when the hand has
    been within `return_band` of it for `settle_ms`, by snapping onto that pose.
    Three variants were measured while writing this, and all three failed in
    ways that would have been invisible on live data:

      * an anchor that followed the hand every frame tracked 72% of a genuine
        0.05 excursion, so the detector found ZERO motions - which reads as
        "the operator did not move";
      * re-centring on the mean of the last N samples landed the anchor ~0.016
        off neutral, because those samples straddle the return ramp. That is
        further than `return_band`, so the NEXT motion could never be satisfied
        as "returned" and swallowed every motion after it;
      * snapping without resetting the settle counter snapped on EVERY
        subsequent frame, pinning the anchor to the moving hand (peak excursion
        over a 0.05 motion: 0.0101).

    The settle counter is therefore the gate: a motion leaves `return_band`
    within a few frames, so the counter cannot reach settle_ms while the hand is
    moving. The cost is that a motion slower than roughly return_band/settle_ms
    is absorbed into the anchor instead of reported.
    """
    if not frames:
        return [], 0
    interval = frame_interval_ms(frames)
    settle_n = max(1, int(round(settle_ms / interval)))

    start = find_rest_start(frames, settle_n, return_band)
    frames = frames[start:]
    if not frames:
        return [], start
    anchor = list(frames[0]["pos"])

    motions = []
    cur = None
    settled = 0

    for f in frames:
        pos = f["pos"]
        if cur is None:
            exc = maxabs(sub(pos, anchor))
            if exc >= min_excursion:
                cur = {"start": f, "anchor": list(anchor), "peak": f,
                       "peak_exc": exc}
                settled = 0
            elif exc <= return_band:
                settled += 1
                if settled >= settle_n:
                    anchor = list(pos)      # confirmed at rest
                    settled = 0             # and start the count again, or this
                                            # would snap on every later frame
            else:
                settled = 0
        else:
            exc = maxabs(sub(pos, cur["anchor"]))
            if exc > cur["peak_exc"]:
                cur["peak_exc"] = exc
                cur["peak"] = f
            if exc <= return_band:
                settled += 1
                if settled >= settle_n:
                    motions.append(cur)
                    anchor = list(pos)
                    cur = None
                    settled = 0
            else:
                settled = 0

    if cur is not None:
        # The recording ended mid-motion (or the operator never came back).
        # Keep it and say so - dropping it would silently lose the last motion,
        # which is exactly the one a hurried operator performs last.
        cur["unreturned"] = True
        motions.append(cur)
    return motions, start


def motion_delta(m):
    """Signed excursion per axis from the neutral pose to the furthest point."""
    return sub(m["peak"]["pos"], m["anchor"])


def span_ms(m):
    return (m["peak"]["ts"] or 0) - (m["start"]["ts"] or 0)


# ---------------------------------------------------------------------------
# reporting
# ---------------------------------------------------------------------------
def analyse(frames, motions, expect, skipped=0):
    print(f"\n  frames with a usable position : {len(frames)}")
    if skipped:
        print(f"  skipped as entry transient    : {skipped} "
              "(frames before the hand was first seen at rest)")
    latched = [f for f in frames if f["latched"]]
    print(f"  of those, reference latched   : {len(latched)}")
    if frames and not latched:
        note("NOTE: the reference never latched here. Harmless for this "
             "experiment (it reads position) but it means delta/gate checks "
             "cannot be made from this recording.")
    print(f"  motions found                 : {len(motions)}")

    if not motions:
        print("\n  Nothing moved further than the min-excursion band. Either the "
              "operator held still, or --min-excursion is too large.")
        return 1, []

    print("\n  #   frames   to-peak   dX        dY        dZ        dominant")
    print("  " + "-" * 70)
    observed = []
    for i, m in enumerate(motions, 1):
        d = motion_delta(m)
        order = sorted(range(3), key=lambda k: -abs(d[k]))
        dom = order[0]
        # A second axis above 50% of the dominant one means the motion was
        # diagonal, so it is weak evidence about a single axis. Reported, not
        # hidden - a diagonal motion is the most likely way to get a plausible
        # but wrong sign.
        weak = abs(d[dom]) == 0 or abs(d[order[1]]) > 0.5 * abs(d[dom])
        tag = f"{AXES[dom].upper()}{'+' if d[dom] >= 0 else '-'}"
        if weak:
            tag += " (weak)"
        if m.get("unreturned"):
            tag += " (never returned)"
        observed.append((AXES[dom], d[dom], weak))
        print(f"  {i:<3} {m['start']['frame_id']:>7} {span_ms(m):>8}ms  "
              f"{d[0]:+.4f}   {d[1]:+.4f}   {d[2]:+.4f}   {tag}")

    print()
    fails = 0
    n = min(len(motions), len(expect))
    if len(expect) != len(motions):
        note(f"NOTE: {len(expect)} motions declared, {len(motions)} found. "
             f"Comparing the first {n} in order.")
    for i in range(n):
        want_axis, want_sign = expect[i]
        got_axis, got_val, weak = observed[i]
        ok_axis = got_axis == want_axis
        ok_sign = ok_axis and (got_val >= 0) == (want_sign == "+")
        status = "PASS" if (ok_axis and ok_sign and not weak) else "FAIL"
        if status == "FAIL":
            fails += 1
        why = ""
        if not ok_axis:
            why = f" - moved {got_axis.upper()}, not {want_axis.upper()}"
        elif not ok_sign:
            why = f" - {want_axis.upper()} moved the wrong way (wanted {want_sign})"
        elif weak:
            why = " - ambiguous, a second axis moved nearly as far"
        print(f"  [{status}] motion {i + 1} = {want_axis.upper()}{want_sign}"
              f"  ->  {got_axis.upper()}{'+' if got_val >= 0 else '-'} "
              f"({got_val:+.4f}){why}")
    if len(motions) > n:
        print(f"  [SKIP] {len(motions) - n} unexplained motion(s) past the declared list")

    # ---- the configuration this experiment exists to produce ----
    print("\n  " + "=" * 68)
    print("  invert_* derived from the signs measured above")
    print("  " + "=" * 68)
    # One line per AXIS, not per expected motion: --expect names an axis twice
    # when the experiment tests both directions of it (Y+ and Y-).
    wanted = {a for a, _ in expect}
    rec = {}
    for axis in AXES:
        if axis not in wanted:
            continue
        vals = [v for a, v, w in observed if a == axis and not w]
        if len(vals) != 1:
            rec[axis] = None
            print(f"  invert_{axis} = ?      # need exactly one clean "
                  f"{axis.upper()} motion, have {len(vals)}")
            continue
        # A positive measurement means the mapper already agrees with the
        # promise for this axis, so no inversion is needed.
        agrees = vals[0] >= 0
        rec[axis] = not agrees
        print(f"  invert_{axis} = {str(not agrees).lower():<5}  # {axis.upper()} "
              f"measured {'+' if agrees else '-'} for a promised + "
              f"({PROMISED[axis]})")
    unresolved = sorted(a for a in wanted if rec.get(a) is None)
    if unresolved:
        print(f"  unresolved: {', '.join(a.upper() for a in unresolved)} "
              "- need another run with one clean motion on that axis")
    return fails, observed


# ---------------------------------------------------------------------------
# self test
# ---------------------------------------------------------------------------
def synthetic_frames():
    """A recording of four known motions, with the jitter real hands have.

    Exists so the segmentation can be proven correct WITHOUT a hand: if this
    came out wrong during a live experiment, a mis-split would be read as a
    mis-performed motion, and the experiment would conclude the opposite of the
    truth. 125 ms spacing matches the agent's 8 fps inference rate.
    """
    import random
    random.seed(7)
    frames = []
    fid = 1000
    ts = 0
    base = [0.5, 0.5, 0.5]
    spec = [(0, 1), (2, 1), (1, 1), (1, -1)]   # X+, Z+, Y+, Y-
    amp = 0.05

    def push(pos, ts_):
        frames.append({"id": fid, "frame_id": fid, "ts": ts_, "pos": pos,
                       "latched": True, "gesture": "OPEN_PALM",
                       "candidate": "OPEN_PALM"})

    def jitter():
        return [base[k] + random.uniform(-0.004, 0.004) for k in range(3)]

    for _ in range(24):
        fid += 1
        ts += 125
        push(jitter(), ts)
    for axis, sign in spec:
        for step in range(1, 13):          # move out
            fid += 1
            ts += 125
            p = list(base)
            p[axis] = base[axis] + sign * amp * step / 12.0
            push([p[k] + random.uniform(-0.004, 0.004) for k in range(3)], ts)
        for _ in range(10):                # hold at the extreme
            fid += 1
            ts += 125
            p = list(base)
            p[axis] = base[axis] + sign * amp
            push([p[k] + random.uniform(-0.004, 0.004) for k in range(3)], ts)
        for step in range(12, -1, -1):     # come back
            fid += 1
            ts += 125
            p = list(base)
            p[axis] = base[axis] + sign * amp * step / 12.0
            push([p[k] + random.uniform(-0.004, 0.004) for k in range(3)], ts)
        for _ in range(14):                # settle at neutral
            fid += 1
            ts += 125
            push(jitter(), ts)
    return frames, spec


def run_self_test(args):
    print("=" * 70)
    print(" self test: segmentation over a synthetic four-motion recording")
    print("=" * 70)
    frames, spec = synthetic_frames()
    motions, skipped = segment(frames, args.min_excursion, args.return_band,
                               args.settle_ms)
    print(f"\n  frames={len(frames)}  skipped={skipped}  "
          f"motions found={len(motions)}  (expected {len(spec)})")
    ok = len(motions) == len(spec)
    if not ok:
        print("  [FAIL] the segmentation did not find exactly one motion per "
              "synthetic motion")
    for i, m in enumerate(motions, 1):
        d = motion_delta(m)
        order = sorted(range(3), key=lambda k: -abs(d[k]))
        dom = order[0]
        weak = abs(d[dom]) == 0 or abs(d[order[1]]) > 0.5 * abs(d[dom])
        axis, sign = AXES[dom], ("+" if d[dom] >= 0 else "-")
        if i <= len(spec):
            want_axis, want_sign = AXES[spec[i - 1][0]], \
                ("+" if spec[i - 1][1] > 0 else "-")
            good = axis == want_axis and sign == want_sign and not weak
            ok = ok and good
            print(f"  [{'PASS' if good else 'FAIL'}] motion {i}: expected "
                  f"{want_axis.upper()}{want_sign}, got {axis.upper()}{sign} "
                  f"({d[order[0]]:+.4f})")
        else:
            print(f"  [FAIL] motion {i}: unexpected extra "
                  f"({axis.upper()}{sign} {d[dom]:+.4f})")
            ok = False
    print("\n" + "=" * 70)
    print(f" SELF TEST {'PASS' if ok else 'FAIL'}")
    print("=" * 70)
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default="http://127.0.0.1:8081")
    ap.add_argument("--camera", default="camera01")
    ap.add_argument("--gap-ms", type=int, default=1000,
                    help="must match the gap the session list was split with")
    ap.add_argument("--session", type=int, default=0,
                    help="session index; 0 = the most recent one")
    ap.add_argument("--file", default="",
                    help="read an exported hand_session_*.json instead of the API")
    ap.add_argument("--list", action="store_true", help="list sessions and stop")
    ap.add_argument("--self-test", action="store_true",
                    help="check the segmentation on a synthetic sequence and stop")
    ap.add_argument("--expect", default="X+,Z+,Y+,Y-",
                    help="the motions the operator was asked to perform, in order "
                         "(axis + sign), e.g. X+,Z+,Y+,Y-")
    ap.add_argument("--min-excursion", type=float, default=0.03,
                    help="how far from the resting pose counts as a real motion")
    ap.add_argument("--return-band", type=float, default=0.012,
                    help="how close to the resting pose counts as returned")
    ap.add_argument("--settle-ms", type=int, default=600,
                    help="how long the hand must stay back before a motion closes")
    ap.add_argument("--json-out", default="", help="also write the raw table here")
    args = ap.parse_args()

    base = args.base_url.rstrip("/")
    cam = args.camera

    if args.self_test:
        return run_self_test(args)

    # ---- where the data comes from ----
    if args.file:
        with open(args.file, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
        print(f"source: {args.file}")
    else:
        if args.list:
            lst = get_json(f"{base}/api/cameras/{cam}/hand/sessions?gap_ms={args.gap_ms}")
            print(f"total_rows={lst.get('total_rows')} gap_ms={lst.get('gap_ms')} "
                  f"sessions={len(lst.get('sessions') or [])}")
            for s in (lst.get("sessions") or []):
                print(f"  idx {s['index']:<5} frames {s['frame_count']:<6} "
                      f"detected {s['detected_count']:<6} gestures={s['gestures']}")
            return 0
        if args.session > 0:
            idx = args.session
        else:
            lst = get_json(f"{base}/api/cameras/{cam}/hand/sessions?gap_ms={args.gap_ms}")
            sess = lst.get("sessions") or []
            if not sess:
                print("no recorded sessions - record one with a hand in view")
                return 1
            idx = max(s["index"] for s in sess)
            print(f"no --session given, using the most recent one ({idx})")
        doc = get_json(f"{base}/api/cameras/{cam}/hand/sessions/{idx}"
                       f"?gap_ms={args.gap_ms}")
        print(f"source: live API, session {idx}")

    # ---- the expected sequence ----
    expect = []
    for tok in args.expect.split(","):
        tok = tok.strip().upper()
        if len(tok) != 2 or tok[0] not in "XYZ" or tok[1] not in "+-":
            print(f"bad --expect token {tok!r}; want e.g. X+,Z+,Y+,Y-")
            return 2
        expect.append((tok[0].lower(), tok[1]))

    print("=" * 70)
    print(" spec-16 axis direction experiment")
    print(f" camera          : {cam}")
    print(" expected motions: " + ", ".join(f"{a.upper()}{s}" for a, s in expect))
    print(f" min-excursion   : {args.min_excursion}   "
          f"return-band: {args.return_band}   settle: {args.settle_ms}ms")
    print(" promised meaning: " + "; ".join(
        f"{k.upper()} {v}" for k, v in PROMISED.items()))
    print("=" * 70)

    frames = load_frames(doc)
    motions, skipped = segment(frames, args.min_excursion, args.return_band,
                               args.settle_ms)
    fails, observed = analyse(frames, motions, expect, skipped)

    if args.json_out:
        rec = {}
        wanted = {a for a, _ in expect}
        for axis in AXES:
            if axis not in wanted:
                continue
            vals = [v for a, v, w in observed if a == axis and not w]
            rec[axis] = None if len(vals) != 1 else (vals[0] < 0)
        with open(args.json_out, "w", encoding="utf-8") as fh:
            json.dump({
                "expected": args.expect,
                "params": {"min_excursion": args.min_excursion,
                           "return_band": args.return_band,
                           "settle_ms": args.settle_ms},
                "motions": [{"index": i, "axis": a, "delta": v, "ambiguous": w}
                            for i, (a, v, w) in enumerate(observed, 1)],
                "invert": rec,
            }, fh, indent=2)
        print(f"\n raw table written to {args.json_out}")

    print("\n" + "=" * 70)
    if fails:
        print(f" RESULT: {fails} motion(s) did not match the declared sequence.")
        print(" A mismatch on the AXIS usually means a different motion was")
        print(" performed than the one being checked - confirm the order before")
        print(" reading the SIGNS. A mismatch on the SIGN is the result.")
        print("=" * 70)
        return 1
    print(" RESULT: every declared motion matched on axis AND sign.")
    print(" The invert_* values above are the experiment's output.")
    print("=" * 70)
    return 0


if __name__ == "__main__":
    sys.exit(main())
