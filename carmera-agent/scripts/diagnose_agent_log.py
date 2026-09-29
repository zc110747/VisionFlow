#!/usr/bin/env python3
"""Triage a camera-agent log for the "the arm barely moves" complaint.

The robot branch is only observable through its own log lines, because the real
arm has no position feedback: a green link is not proof of motion. So this
script turns the log into the three numbers that decide whether the arm was
actually being driven, or was busy being yanked back to HOME:

  1. COMMAND MIX   - how many `home` vs `movexyz` went out, and why each home
                     fired (hand re-entered the frame / open-palm gesture).
                     A home count comparable to the movexyz count means the arm
                     spent its time travelling back, not following the hand.
  2. DETECTION SHAPE - the hand branch does not report a steady stream. Cluster
                     the detected frames by inter-gap and print the gap
                     distribution: if the natural inter-cluster gap is longer
                     than the configured loss window, every cluster boundary
                     reads as a real exit and triggers a home.
  3. COMMANDED TRAVEL - sum of |delta| per axis. This is how far the arm was
                     TOLD to go. It is an upper bound on real motion, never a
                     measurement of it.

Usage:
    python scripts/diagnose_agent_log.py <agent.log> [--gap-ms 2000]

Everything read is a log line; nothing here starts, stops or contacts anything.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from dataclasses import dataclass, field

# [2026-09-29 20:45:13.355] [info] [AI] frame=2578 ts=88261 mode=hand conf=0.82 hand=right gesture=UNKNOWN motion=(STATIONARY,STATIONARY,STATIONARY)
RE_DETECT = re.compile(
    r"^\[(?P<t>[\d-]+ [\d:.]+)\]\s+\[\w+\]\s+\[AI\] frame=\d+ ts=\d+ mode=hand "
    r"conf=(?P<conf>[\d.]+) hand=(?P<hand>\w+) gesture=(?P<gesture>\w+)"
)
# [2026-09-29 20:45:13.986] [info] [AI] mode=hand fps=7.9 ... processed=705 dropped=1891 objects=1
RE_STATS = re.compile(r"\[AI\] mode=hand fps=.* processed=(\d+) dropped=(\d+)")
# [2026-09-29 20:45:14.483] [info] [ROBOT] cmd=movexyz delta=[-0.18,2.33,-0.53] ok
RE_MOVEXYZ = re.compile(
    r"^\[(?P<t>[\d-]+ [\d:.]+)\]\s+\[\w+\]\s+\[ROBOT\] cmd=movexyz "
    r"delta=\[(?P<x>-?[\d.]+),(?P<y>-?[\d.]+),(?P<z>-?[\d.]+)\]\s*(?P<ok>ok)?"
)
RE_HOME = re.compile(r"^\[(?P<t>[\d-]+ [\d:.]+)\]\s+\[\w+\]\s+\[ROBOT\] cmd=home")
RE_REENTER = re.compile(r"\[ROBOT\] hand \(re\)entered the frame")
RE_TAPER = re.compile(r"\[ROBOT\] soft boundary taper")
RE_SUPPRESS = re.compile(r"\[ROBOT\] soft boundary: .* suppressed")
RE_REJECT = re.compile(r"\[ROBOT\].*(rejected|refused|JOINT_LIMIT|OUT_OF_WORKSPACE)", re.I)
RE_ABSENT = re.compile(r"\[ROBOT\] hand absent for (\d+) ms")


def ms(t: str) -> int:
    """'2026-09-29 20:45:13.355' -> ms since midnight (enough for gaps)."""
    hh, mm, ss = t.split(" ")[1].split(":")
    return int(hh) * 3_600_000 + int(mm) * 60_000 + int(float(ss) * 1000)


@dataclass
class Report:
    first_ts: str = ""
    last_ts: str = ""
    span_ms: int = 0
    processed: int = 0
    detected: int = 0
    detections: list = field(default_factory=list)   # (t_ms, hand, gesture)
    homes: list = field(default_factory=list)        # (t_ms, reason)
    moves: list = field(default_factory=list)        # (t_ms, x, y, z)
    taper: int = 0
    suppressed: int = 0
    rejected: int = 0
    absent: list = field(default_factory=list)       # ms values from the new DEBUG line


def parse(path: str) -> Report:
    r = Report()
    reenter_at: list = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line.startswith("["):
                continue
            t = line[1 : line.index("]")]
            if not r.first_ts:
                r.first_ts = t
            r.last_ts = t

            m = RE_DETECT.match(line)
            if m:
                r.detections.append((ms(m["t"]), m["hand"], m["gesture"]))
                r.detected += 1
                continue

            m = RE_STATS.search(line)
            if m:
                # processed is cumulative; the last one wins.
                r.processed = int(m.group(1))
                continue

            if RE_REENTER.search(line):
                reenter_at.append(ms(t))

            m = RE_MOVEXYZ.match(line)
            if m:
                r.moves.append((ms(t), float(m["x"]), float(m["y"]), float(m["z"])))
                continue

            if RE_HOME.search(line):
                # Attribute the home to whichever cause is closest in time.
                reason = "gesture(open_palm)"
                if reenter_at and abs(ms(t) - reenter_at[-1]) <= 5:
                    reason = "hand re-entered"
                r.homes.append((ms(t), reason))
                continue

            if RE_TAPER.search(line):
                r.taper += 1
                continue
            if RE_SUPPRESS.search(line):
                r.suppressed += 1
                continue
            if RE_REJECT.search(line):
                r.rejected += 1
                continue
            m = RE_ABSENT.search(line)
            if m:
                r.absent.append(int(m.group(1)))

    if r.first_ts and r.last_ts:
        r.span_ms = ms(r.last_ts) - ms(r.first_ts)
    return r


def clusters(detections: list, gap_ms: int) -> list:
    """Split detection timestamps into runs separated by more than gap_ms."""
    out: list = []
    cur: list = []
    for t, _h, _g in detections:
        if cur and t - cur[-1] > gap_ms:
            out.append(cur)
            cur = []
        cur.append(t)
    if cur:
        out.append(cur)
    return out


def pct(vals: list, q: float) -> float:
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(q * (len(s) - 1)))]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--gap-ms", type=int, default=2000,
                    help="the configured loss window (RobotConfig::hand_lost_ms)")
    ap.add_argument("--cluster-gap-ms", type=int, default=400,
                    help="silence that ENDS a burst (~3 frame intervals at 8 Hz). "
                         "Kept well below --gap-ms, or the measurement is circular")
    ap.add_argument("--old-gap-ms", type=int, default=750,
                    help="the window this change replaced (6 frames at ~8 Hz), so "
                         "the report can say how many spurious homes it removes")
    args = ap.parse_args()

    r = parse(args.log)
    if not r.first_ts:
        print("no parsable log lines", file=sys.stderr)
        return 2

    print("=" * 72)
    print("camera-agent log triage")
    print("=" * 72)
    print(f"span          {r.first_ts}  ->  {r.last_ts}   ({r.span_ms / 1000:.0f} s)")
    print(f"AI processed  {r.processed} frames")
    if r.span_ms:
        print(f"detected      {r.detected} frames "
              f"({100.0 * r.detected / max(1, r.processed):.2f}% of processed)")
    print()

    # ---- 1. command mix ----------------------------------------------------
    print("-" * 72)
    print("1. COMMAND MIX   (what the arm was actually told to do)")
    print("-" * 72)
    reasons = Counter(x[1] for x in r.homes)
    print(f"  home      {len(r.homes):>5}   " +
          (" ".join(f"[{k}: {v}]" for k, v in reasons.most_common()) or ""))
    print(f"  movexyz   {len(r.moves):>5}")
    print(f"  rejected  {r.rejected:>5}")
    print(f"  suppressed at a bound {r.suppressed:>5}")
    print(f"  taper frames          {r.taper:>5}")
    if r.absent:
        print(f"  departures logged     {len(r.absent):>5}  "
              f"(absence ms: {sorted(r.absent)})")
    if r.homes:
        ratio = len(r.homes) / max(1, len(r.moves))
        verdict = ("ARM SPENT ITS TIME GOING HOME" if ratio >= 0.3
                   else "follow dominates")
        print(f"  home/movexyz = {ratio:.2f}  ->  {verdict}")
    print()

    # ---- 2. detection shape ------------------------------------------------
    print("-" * 72)
    print("2. DETECTION SHAPE   (is the loss window wider than the natural gap?)")
    print("-" * 72)
    # Cluster on a SHORT gap so the natural burst structure stays visible. Using
    # the loss window as the split threshold here would be circular - every gap
    # reported would trivially exceed the very threshold it was split on.
    cs = clusters(r.detections, args.cluster_gap_ms)
    lens = [len(c) for c in cs]
    print(f"  bursts (split on > {args.cluster_gap_ms} ms of silence): {len(cs)}")
    if lens:
        print(f"  burst sizes: min={min(lens)} median={pct(lens, .5):.0f} "
              f"p90={pct(lens, .9):.0f} max={max(lens)}")
    gaps = [b[0] - a[-1] for a, b in zip(cs, cs[1:])]
    if gaps:
        print(f"  gaps between bursts (ms): min={min(gaps)} "
              f"median={pct(gaps, .5):.0f} p90={pct(gaps, .9):.0f} max={max(gaps)}")
        print()
        n_old = sum(1 for g in gaps if g >= args.old_gap_ms)
        n_new = sum(1 for g in gaps if g >= args.gap_ms)
        absorbed = sum(1 for g in gaps if args.old_gap_ms <= g < args.gap_ms)
        print(f"  gaps crossing the OLD window ({args.old_gap_ms} ms): {n_old} / {len(gaps)}"
              f"   <- each one declared the hand 'left' and sent HOME")
        print(f"  gaps crossing the NEW window ({args.gap_ms} ms): {n_new} / {len(gaps)}")
        print(f"  -> spurious departures the wider window absorbs: {absorbed}")
        if absorbed == 0:
            print("     NOTE: nothing absorbed - every gap here is longer than the new")
            print("     window too, so the arm was NOT merely reacting to flicker.")
    print()

    # ---- 3. commanded travel ----------------------------------------------
    print("-" * 72)
    print("3. COMMANDED TRAVEL   (upper bound on motion - there is no feedback)")
    print("-" * 72)
    if r.moves:
        sx = sum(abs(m[1]) for m in r.moves)
        sy = sum(abs(m[2]) for m in r.moves)
        sz = sum(abs(m[3]) for m in r.moves)
        for name, s in (("X front/back", sx), ("Y left/right", sy), ("Z up/down", sz)):
            print(f"  sum |delta {name:<13}| = {s:8.2f} mm")
        print(f"  net |sum delta| = "
              f"[{sum(m[1] for m in r.moves):7.2f}, "
              f"{sum(m[2] for m in r.moves):7.2f}, "
              f"{sum(m[3] for m in r.moves):7.2f}] mm")
        steps = sorted(max(abs(m[1]), abs(m[2]), abs(m[3])) for m in r.moves)
        print(f"  per-command largest-axis step: median={pct(steps, .5):.2f} mm "
              f"p90={pct(steps, .9):.2f} mm max={steps[-1]:.2f} mm")
    else:
        print("  no movexyz commands at all")
    print()
    print("NOTE  net vs sum: |sum| << sum means the arm oscillated rather than")
    print("      travelled. Both are COMMANDED values; the real arm is open-loop.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
