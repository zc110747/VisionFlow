#!/usr/bin/env python3
"""Distribution of soft-boundary taper weights in an agent log.

The self-lock bug shows up as a weight stuck at the floor (0.10) while the arm
sits at HOME: the learned bound was pinned to the current pose, so the axis is
permanently tapered and its per-frame step falls under the deadzone.
"""
from __future__ import annotations

import re
import sys
from collections import Counter

RE_TAPER = re.compile(
    r"soft boundary taper w=\[(?P<wx>[-\d.]+),(?P<wy>[-\d.]+),(?P<wz>[-\d.]+)\]"
    r" pos=\[(?P<px>[-\d.]+),(?P<py>[-\d.]+),(?P<pz>[-\d.]+)\]"
)

path = sys.argv[1]
rows = []
with open(path, "r", encoding="utf-8", errors="replace") as fh:
    for line in fh:
        m = RE_TAPER.search(line)
        if m:
            rows.append([float(m.group(k)) for k in ("wx", "wy", "wz", "px", "py", "pz")])

print(f"taper frames: {len(rows)}")
if not rows:
    raise SystemExit(0)

names = ("X", "Y", "Z")
for i, name in enumerate(names):
    c = Counter(round(r[i], 2) for r in rows)
    top = ", ".join(f"{w:.2f}x{n}" for w, n in c.most_common(6))
    pinned = sum(n for w, n in c.items() if w <= 0.101)
    zero = sum(n for w, n in c.items() if w <= 0.0001)
    print(f"  w{name}: {top}")
    print(f"       at floor(<=0.10): {pinned} ({100 * pinned / len(rows):.1f}%)   at zero: {zero}")

# Frames where an axis is tapered but the pose is exactly HOME: nothing can move.
home = sum(1 for r in rows if abs(r[3] - 115.03) < 0.5 and abs(r[4]) < 0.5 and abs(r[5] - 109.22) < 0.5)
print(f"  taper frames while pinned at HOME: {home} ({100 * home / len(rows):.1f}%)")
