#!/usr/bin/env python3
"""Measure the REAL reachable span of each axis on a running MeArmPilot backend.

Why this exists
---------------
`caps.workspace` is a BOUNDING BOX over the geometric reach. It includes poses
the IK cannot solve, so it is much larger than the set of positions the arm can
actually be commanded to - measured on this project's sim backend it overstates
X by ~4x (box 135 mm, reachable 34 mm).

The agent derives its per-normalised-unit gain from a span, and derives the
soft-boundary band from the same span. Feeding it the box makes one frame of
hand motion command several times the arm's remaining travel, so the first step
is refused and the arm appears frozen while the hand moves. The fix is to
measure the spans and put them in `robot.reach_span_*`.

This script produces exactly those numbers.

Method
------
Absolute `moveto` probes, not `movexyz` walks. A delta walk measures "how far
can I get along this one path", which is confounded by the joints coupling -
the arm can reach a point by one route and not another. `moveto` asks the direct
question, "is this pose solvable", so the answer is a property of the point.

For each axis it sweeps outward from HOME in both directions until the backend
refuses, then bisects the gap between the last good value and the first refused
one to land within `--tol` mm of the true edge.

Usage
-----
    python scripts/measure_reach.py                      # 127.0.0.1:9100
    python scripts/measure_reach.py --host X --port N
    python scripts/measure_reach.py --tol 0.25           # finer edge

Output is a table plus the YAML snippet to paste into camera-agent.yaml.

Note: this OVERWRITES the arm's pose (it homes first and moves around).
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time

AXES = [("X front", 0), ("Y left", 1), ("Z up", 2)]


class Backend:
    """One connection, JSON-lines, tolerant of a backend that answers slowly."""

    def __init__(self, host: str, port: int, timeout: float = 2.0) -> None:
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def send(self, obj: dict, wait: float = 0.30) -> dict | None:
        self.sock.sendall((json.dumps(obj) + "\n").encode())
        deadline = time.time() + wait
        out: list[dict] = []
        while time.time() < deadline:
            self.sock.settimeout(0.15)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                break
            self.buf += chunk
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                line = line.strip()
                if line:
                    try:
                        out.append(json.loads(line.decode()))
                    except json.JSONDecodeError:
                        pass
        return out[0] if out else None

    def close(self) -> None:
        self.sock.close()


def home_tcp(b: Backend) -> list[float]:
    r = b.send({"cmd": "home"}, 0.5)
    if not r or not r.get("ok"):
        raise RuntimeError(f"home failed: {r}")
    return [float(v) for v in r["state"]["tcp"]]


def reachable(b: Backend, xyz: list[float]) -> bool:
    r = b.send({"cmd": "moveto", "xyz": [float(v) for v in xyz]})
    return bool(r and r.get("ok"))


def edge(b: Backend, axis: int, base: list[float], direction: int,
         step: float, limit: int, tol: float) -> float | None:
    """Walk outward until refused, then bisect. Returns the last reachable value."""
    last_ok = base[axis]
    probe = list(base)

    # Coarse: grow the step while it keeps working so a wide axis does not take
    # a hundred probes.
    cur = step
    for _ in range(limit):
        probe[axis] = last_ok + direction * cur
        if reachable(b, probe):
            last_ok = probe[axis]
        else:
            break
    else:
        return last_ok

    # Fine: bisect between last_ok and the first refusal.
    lo, hi = last_ok, last_ok + direction * cur      # lo=good, hi=bad
    while abs(hi - lo) > tol:
        mid = (lo + hi) / 2.0
        probe[axis] = mid
        if reachable(b, probe):
            lo = mid
        else:
            hi = mid
    probe[axis] = lo
    if reachable(b, probe):
        return lo
    return last_ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9100)
    ap.add_argument("--step", type=float, default=4.0,
                    help="initial outward step (mm), doubles as it succeeds")
    ap.add_argument("--limit", type=int, default=40,
                    help="max coarse probes per direction")
    ap.add_argument("--tol", type=float, default=0.5,
                    help="edge resolution (mm)")
    args = ap.parse_args()

    try:
        b = Backend(args.host, args.port)
    except OSError as e:
        print(f"cannot reach {args.host}:{args.port}: {e}", file=sys.stderr)
        return 2

    try:
        home = home_tcp(b)
        caps = b.send({"cmd": "caps"})
        box = None
        if caps and caps.get("ok"):
            ws = caps["caps"].get("workspace", {})
            box = (list(ws.get("min", [])), list(ws.get("max", [])))

        print(f"backend {args.host}:{args.port}")
        print(f"HOME tcp = [{home[0]:.3f}, {home[1]:.3f}, {home[2]:.3f}]")
        if box:
            print(f"caps box = [{box[0][0]:.2f}, {box[1][0]:.2f}] x "
                  f"[{box[0][1]:.2f}, {box[1][1]:.2f}] x "
                  f"[{box[0][2]:.2f}, {box[1][2]:.2f}]")
        print()

        spans: dict[int, float] = {}
        for name, axis in AXES:
            base = list(home)
            lo = edge(b, axis, base, -1, args.step, args.limit, args.tol)
            home_tcp(b)                      # re-home between directions
            hi = edge(b, axis, base, +1, args.step, args.limit, args.tol)
            home_tcp(b)
            if lo is None or hi is None:
                print(f"{name:10}  FAILED to measure")
                continue
            spans[axis] = hi - lo
            infl = ""
            if box:
                boxspan = box[1][axis] - box[0][axis]
                if boxspan > 0:
                    infl = f"   box={boxspan:7.2f}  inflation={boxspan / spans[axis]:.1f}x"
            print(f"{name:10}  reachable=[{lo:8.2f}, {hi:8.2f}]  span={spans[axis]:6.2f}"
                  f"  HOME margin: -{home[axis] - lo:6.2f} / +{hi - home[axis]:6.2f}{infl}")

        if len(spans) == 3:
            print()
            print("paste into carmera-agent.yaml under `robot:`")
            print(f"  reach_span_x: {spans[0]:.1f}")
            print(f"  reach_span_y: {spans[1]:.1f}")
            print(f"  reach_span_z: {spans[2]:.1f}")
    finally:
        b.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
