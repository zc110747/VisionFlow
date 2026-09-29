#!/usr/bin/env python3
"""Gesture->arm transfer diagnosis: "my hand moves, the arm barely does".

Answers ONE question with numbers, per axis:

    the hand sweeps s of the normalised frame  ->  how far does the ARM go?

It reads two already-recorded streams, so it never touches the live session:

  * the hand log            video-server's SQLite `ai_hand_log` (every AI hand frame
                            the agent produced, landmarks included)
  * the joint stream        optional JSONL captured from the ArmPilot joint
                            WebSocket (ws://127.0.0.1:8090/ws/joint) - the arm's
                            OWN commanded pose, as the firmware reports it

and mirrors `robot_link.cpp::step()` exactly:

    inc      = delta_now - delta_prev                   # cumulative -> increment
    raw_armX =  inc_hand_y * gain_x * (span_x / 0.5)     armX = front/back
    raw_armY = -inc_hand_x * gain_y * (span_y / 0.5)     armY = left/right
    raw_armZ =  inc_hand_z * gain_z * (span_z / 0.5)     armZ = up/down
    |raw| < deadzone_mm  -> never sent

The gain that makes an axis cross F of its reachable span for a given hand travel
s is `gain = 0.5 * F / s` - the span cancels, so it depends only on how much the
hand really moves on THAT axis. That is the whole point: one constant
(kHandActiveSpan = 0.5) is applied to three axes whose real travel differs ~4x.

Usage
-----
  python scripts/diagnose_hand_arm_transfer.py
  python scripts/diagnose_hand_arm_transfer.py --joints joint_stream.jsonl
  python scripts/diagnose_hand_arm_transfer.py --gains 1.5,0.6,1.8
  python scripts/diagnose_hand_arm_transfer.py --db /path/to/video.joint.db

Capture the joint stream with (read-only subscriber, never sends a command):

  node scripts/ws_joint_record.mjs 60000 joint_stream.jsonl
"""
from __future__ import annotations

import argparse
import json
import math
import sqlite3
import sys
from pathlib import Path

# ---- defaults: the meARM-v1 truth (robot.yaml) + the shipped agent config -----
SPAN = {"x": 39.0, "y": 176.0, "z": 41.5}       # measured reachable span, mm
GAINS = {"x": 0.25, "y": 0.5, "z": 0.25}        # camera-agent.yaml gain_*
DEADZONE_MM = 0.5
K_HAND_ACTIVE_SPAN = 0.5
FEED = {"x": "y", "y": "x", "z": "z"}           # arm axis <- hand axis
NAME = {"x": "armX front", "y": "armY left", "z": "armZ up"}
HANDAX = {"x": "hand x left/right", "y": "hand y depth", "z": "hand z up/down"}
# 1 servo degree -> TCP mm, from robot.yaml calibration + link lengths
SERVO_QUANTUM_MM = {"base": 2.01, "shoulder": 0.97, "elbow": 0.58}
# FK constants (robot.yaml: base link 60, L1 80, L2 80, wrist->TCP 40 kept level)
BASE_H, L1, L2, WRIST = 60.0, 80.0, 80.0, 40.0
HOME = {"base": 0.0, "shoulder": 0.8498937633, "elbow": 112.6185771989}
LIMITS = {"base": (-60.0, 60.0), "shoulder": (-6.0936827341, 49.454929245),
          "elbow": (108.4414852068, 141.8582211436)}


def pct(v, q):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))]


def fk(j):
    """joints -> TCP (x front, y left, z up) in mm."""
    th1, th2, b = (math.radians(j["shoulder"]), math.radians(j["elbow"]),
                   math.radians(j["base"]))
    r = L1 * math.sin(th1) + L2 * math.sin(th2) + WRIST
    z = BASE_H + L1 * math.cos(th1) + L2 * math.cos(th2)
    return r * math.cos(b), r * math.sin(b), z


# ---------------------------------------------------------------------------
# hand log -> sessions -> the command stream the agent would emit
# ---------------------------------------------------------------------------
def load_hand_rows(db: Path):
    # read-only: the server is live and holds the WAL
    c = sqlite3.connect(f"file:{db.as_posix()}?mode=ro", uri=True)
    try:
        return c.execute(
            "select timestamp, detected, payload from ai_hand_log order by id").fetchall()
    finally:
        c.close()


def to_sessions(rows, gap_ms):
    """Runs of consecutive DETECTIONS - the server's own session rule
    (metadata.splitHandSessions) - plus a hard split on a time gap or a
    timestamp going backwards (the capture stream restarted)."""
    out, cur, prev = [], [], None
    for ts, det, payload in rows:
        if prev is not None and (ts - prev > gap_ms or ts < prev):
            if cur:
                out.append(cur)
            cur = []
        if det:
            cur.append((ts, payload))
        elif cur:
            out.append(cur)
            cur = []
        prev = ts
    if cur:
        out.append(cur)
    return out


def usable(pts, min_frames):
    """Keep frames that carry a CONSUMABLE delta (reference latched)."""
    ok = []
    for ts, payload in pts:
        try:
            h = json.loads(payload)
        except Exception:
            continue
        if not isinstance(h, dict) or not h.get("detected"):
            continue
        p, d = h.get("position"), h.get("delta")
        if p and d and h.get("reference_initialized"):
            ok.append((ts, p, d))
    return ok if len(ok) >= min_frames else None


def replay(pts, gains):
    """Mirror robot_link.cpp::step() over one session."""
    g = {a: gains[a] * (SPAN[a] / K_HAND_ACTIVE_SPAN) for a in SPAN}
    travel = {a: 0.0 for a in "xyz"}
    steps = {a: [] for a in "xyz"}
    sent = skipped = 0
    prev = None
    for _, _, d in pts:
        if prev is None:            # first frame only latches the basis
            prev = list(d)
            continue
        inc = [d[i] - prev[i] for i in range(3)]
        prev = list(d)
        raw = [inc[1] * g["x"], -inc[0] * g["y"], inc[2] * g["z"]]
        if math.sqrt(sum(v * v for v in raw)) < max(0.5, DEADZONE_MM):
            skipped += 1
            continue
        sent += 1
        for i, a in enumerate("xyz"):
            travel[a] += raw[i]
            steps[a].append(abs(raw[i]))
    return travel, steps, sent, skipped


# ---------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    here = Path(__file__).resolve().parent
    ap.add_argument("--db", type=Path,
                    default=here.parent.parent / "video-server/data/video.joint.db")
    ap.add_argument("--joints", type=Path, default=None,
                    help="JSONL captured from the joint WebSocket")
    ap.add_argument("--gains", default=None,
                    help="what-if: x,y,z (default = the shipped 0.25,0.5,0.25)")
    ap.add_argument("--gap-ms", type=int, default=1000)
    ap.add_argument("--min-frames", type=int, default=20)
    ap.add_argument("--suggest-f", type=float, default=0.5,
                    help="target: a session of hand travel should cross F x the span")
    a = ap.parse_args()

    gains = GAINS
    if a.gains:
        v = [float(x) for x in a.gains.split(",")]
        if len(v) != 3:
            print("--gains needs exactly x,y,z", file=sys.stderr)
            return 2
        gains = dict(zip("xyz", v))

    print("=" * 84)
    print("手势 → 机械臂 位移映射诊断")
    print(f"  hand log : {a.db}")
    print(f"  gains    : x={gains['x']} y={gains['y']} z={gains['z']}"
          f"   (span x/y/z = {SPAN['x']}/{SPAN['y']}/{SPAN['z']} mm,"
          f" deadzone {DEADZONE_MM} mm)")
    print("=" * 84)

    # ---------------- hand side -------------------------------------------
    rows = load_hand_rows(a.db)
    sess = to_sessions(rows, a.gap_ms)
    good = [s for s in (usable(p, a.min_frames) for p in sess) if s]
    if not good:
        print("no usable hand sessions in the log", file=sys.stderr)
        return 1
    frames = sum(len(p) for p in good)
    dur = sum((p[-1][0] - p[0][0]) / 1000.0 for p in good)
    print(f"样本: {len(rows)} 行 -> {len(sess)} 段检测段 -> {len(good)} 段可用 "
          f"({frames} 帧, {dur:.0f} s)")

    hspan = {ax: [] for ax in "xyz"}
    for pts in good:
        for ax in "xyz":
            i = "xyz".index(ax)
            v = [p[i] for _, p, _ in pts]
            hspan[ax].append(max(v) - min(v))

    print("-" * 84)
    print("1. 手部归一化坐标的单段活动范围 —— 这才是增益真正的输入量")
    print(f"   {'手部轴':<22}{'p10':>9}{'中位':>9}{'p90':>9}{'最大':>9}")
    for ax in "xyz":
        v = hspan[ax]
        print(f"   {HANDAX[ax]:<22}{pct(v,.1):>9.4f}{pct(v,.5):>9.4f}"
              f"{pct(v,.9):>9.4f}{max(v):>9.4f}")

    print("-" * 84)
    print("2. 换算系数 mm / 归一化单位（= gain x span / 0.5）")
    for ax in "xyz":
        g = gains[ax] * (SPAN[ax] / K_HAND_ACTIVE_SPAN)
        print(f"   {NAME[ax]:<12} feed={HANDAX[FEED[ax]]:<18}"
              f"gain={gains[ax]:<6} => {g:8.1f} mm/单位")

    res = [replay(p, gains) for p in good]
    print("-" * 84)
    print("3. 同一条命令里各轴分量的量级（|raw| mm）")
    print(f"   {'轴':<12}{'p50':>9}{'p90':>9}{'p99':>9}{'最大':>9}"
          f"{'< 死区占比':>12}")
    for ax in "xyz":
        v = [x for _, st, _, _ in res for x in st[ax]]
        lo = sum(1 for x in v if x < DEADZONE_MM) / max(1, len(v)) * 100
        print(f"   {NAME[ax]:<12}{pct(v,.5):>9.3f}{pct(v,.9):>9.3f}"
              f"{pct(v,.99):>9.3f}{(max(v) if v else float('nan')):>9.3f}{lo:>11.1f}%")

    print("-" * 84)
    print("4. 每轴权限 = 该段净位移 / 该轴可达域（中位）")
    for ax in "xyz":
        v = [abs(tr[ax]) / SPAN[ax] * 100 for tr, _, _, _ in res]
        print(f"   {NAME[ax]:<12}{pct(v,.5):>7.1f}%   手部单段范围(中位)="
              f"{pct(hspan[FEED[ax]],.5):.4f}")

    print("-" * 84)
    print(f"5. 建议 gains：让每轴在一次手部行程内走完 F={a.suggest_f} 倍可达域")
    print("   推导  s*gain*(span/0.5) = F*span  =>  span 抵消  =>  gain = 0.5*F/s")
    print(f"   {'arm 轴':<12}{'feed':<18}{'s(中位)':>10}{'建议 gain':>11}"
          f"{'当前':>8}{'倍数':>7}")
    for ax in "xyz":
        s = pct(hspan[FEED[ax]], .5)
        need = 0.5 * a.suggest_f / s
        print(f"   {NAME[ax]:<12}{HANDAX[FEED[ax]]:<18}{s:>10.4f}{need:>11.2f}"
              f"{gains[ax]:>8}{need/gains[ax]:>7.1f}x")

    if not a.gains:
        print("-" * 84)
        print("6. what-if：换成建议 gains 后，同一批真实数据的每段净位移")
        prop = {ax: 0.5 * a.suggest_f / pct(hspan[FEED[ax]], .5) for ax in "xyz"}
        res2 = [replay(p, prop) for p in good]
        print(f"   {'':<14}{'armX front':>13}{'armY left':>13}{'armZ up':>13}")
        for label, rr in (("当前", res), ("建议", res2)):
            row = [pct([abs(tr[ax]) / SPAN[ax] * 100 for tr, _, _, _ in rr], .5)
                   for ax in "xyz"]
            print(f"   {label:<14}{row[0]:>12.1f}%{row[1]:>12.1f}%{row[2]:>12.1f}%")
        print(f"   建议值 x={prop['x']:.2f} y={prop['y']:.2f} z={prop['z']:.2f}"
              "  （不含软边界衰减与真机量化，故为上界）")

    # ---------------- arm side -------------------------------------------
    if a.joints:
        print("-" * 84)
        print("7. 真机控制面实测（关节 WebSocket 录制 -> FK -> TCP）")
        x, y, z = fk(HOME)
        print(f"   FK 自检: HOME -> x={x:.3f} z={z:.3f}  "
              f"(配置实测余量反推 x=115.030 z=109.220, 误差 "
              f"{x-115.03:+.4f}/{z-109.22:+.4f} mm)")
        jr = []
        for line in a.joints.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            m = json.loads(line)
            if m.get("type") == "joint_state" and m.get("joints"):
                jr.append(m)
        if jr:
            tcp = [fk(m["joints"]) for m in jr]
            print(f"   {len(jr)} 帧 / {(jr[-1]['t']-jr[0]['t'])/1000:.1f} s")
            print(f"   {'轴':<12}{'起':>9}{'止':>9}{'行程(min..max)':>20}"
                  f"{'Σ|Δ| mm':>12}")
            for i, ax in enumerate("xyz"):
                v = [p[i] for p in tcp]
                tot = sum(abs(v[k] - v[k - 1]) for k in range(1, len(v)))
                print(f"   {NAME[ax]:<12}{v[0]:>9.2f}{v[-1]:>9.2f}"
                      f"{'%8.2f..%8.2f' % (min(v), max(v)):>20}{tot:>12.2f}")
            print(f"   关节行程占限位行程: " + "  ".join(
                f"{j} {(max(r['joints'][j] for r in jr)-min(r['joints'][j] for r in jr))/(hi-lo)*100:.0f}%"
                for j, (lo, hi) in LIMITS.items()))

    print("-" * 84)
    print("8. 真机可分辨下限（1 个舵机度对应的 TCP 位移）")
    for j, mm in SERVO_QUANTUM_MM.items():
        print(f"   {j:<9}{mm:5.2f} mm   <- 任何小于它的单帧命令都不会产生可见运动")
    return 0


if __name__ == "__main__":
    sys.exit(main())
