# Analyze recorded live hand sessions -> what the arm would actually be commanded.
#
# Mirrors robot_link.cpp::step() exactly:
#   inc      = delta_now - delta_prev           (cumulative -> increment, first frame latches only)
#   raw_x    = inc_y * gain_x * (reach_span_x / 0.5)     arm X = front/back
#   raw_y    = -inc_x * gain_y * (reach_span_y / 0.5)    arm Y = left/right
#   raw_z    = inc_z * gain_z * (reach_span_z / 0.5)     arm Z = up/down
#   deadzone: |v| < max(0.5, deadzone_mm) -> SKIPPED (never sent)
#
# Usage: python analyze_hand_to_arm.py sess_234.json sess_235.json ...
import json
import math
import sys
from pathlib import Path

# --- shipped config (carmera-agent/config/camera-agent.yaml) -----------------
GAIN = {"x": 0.25, "y": 0.5, "z": 0.25}
SPAN = {"x": 39.0, "y": 176.0, "z": 41.5}   # measured reachable span, mm
DEADZONE_MM = 0.5
K_HAND_ACTIVE_SPAN = 0.5
RATE_HZ = 15

AXIS_NAME = {"x": "armX(front)", "y": "armY(left)", "z": "armZ(up)"}


def totals():
    return {a: GAIN[a] * (SPAN[a] / K_HAND_ACTIVE_SPAN) for a in SPAN}


def analyze(path):
    d = json.loads(Path(path).read_text(encoding="utf-8"))
    frames = d["frames"]
    g = totals()

    # hand position range
    pos = {a: [] for a in "xyz"}
    for f in frames:
        h = f.get("hand") or {}
        p = h.get("position")
        if not p:
            continue
        for i, a in enumerate("xyz"):
            pos[a].append(p[i])

    print("=" * 78)
    print(f"{Path(path).name}  frames={len(frames)}  "
          f"t={frames[0]['timestamp']}..{frames[-1]['timestamp']} ms  "
          f"({(frames[-1]['timestamp']-frames[0]['timestamp'])/1000.0:.1f} s)")
    print("-" * 78)
    print("A. 手部归一化坐标的实际活动范围（这才是增益的输入量）")
    for a in "xyz":
        v = pos[a]
        if not v:
            continue
        print(f"   hand {a}: min={min(v):.4f} max={max(v):.4f} span={max(v)-min(v):.4f}")

    # increments -> commands
    have_prev = False
    prev = [0.0, 0.0, 0.0]
    sent = skipped = 0
    travelled = {"x": 0.0, "y": 0.0, "z": 0.0}
    below = {"x": 0, "y": 0, "z": 0}          # axis-wise sub-deadzone contribution
    step_abs = {a: [] for a in "xyz"}         # |step| per SENT command
    raw_abs = {a: [] for a in "xyz"}          # |raw| per frame that had motion
    last_send_ms = None
    period = 1000 // RATE_HZ
    limited = 0

    for f in frames:
        h = f.get("hand") or {}
        dl = h.get("delta")
        if not dl or not h.get("reference_initialized"):
            continue
        if not have_prev:
            prev = list(dl)
            have_prev = True
            last_send_ms = f["timestamp"]
            continue
        inc = [dl[i] - prev[i] for i in range(3)]
        prev = list(dl)
        raw = [inc[1] * g["x"], -inc[0] * g["y"], inc[2] * g["z"]]

        ts = f["timestamp"]
        if last_send_ms is not None and ts - last_send_ms < period:
            limited += 1
            continue
        last_send_ms = ts

        for i, a in enumerate("xyz"):
            if abs(raw[i]) > 1e-6:
                raw_abs[a].append(abs(raw[i]))

        mag = math.sqrt(sum(v * v for v in raw))
        if mag < max(0.5, DEADZONE_MM):
            skipped += 1
            for i, a in enumerate("xyz"):
                below[a] += 1
            continue
        sent += 1
        for i, a in enumerate("xyz"):
            travelled[a] += raw[i]
            step_abs[a].append(abs(raw[i]))

    print("-" * 78)
    print(f"B. 换算结果（shipped gain, 每单位归一化增量 -> mm）")
    for a in "xyz":
        print(f"   {AXIS_NAME[a]:<12} gain={GAIN[a]:<5} span={SPAN[a]:<6} "
              f"=> {g[a]:.1f} mm / 归一化单位")

    print("-" * 78)
    print(f"C. 命令统计（限流阈值 {period} ms，AI 帧间隔 ~{frames[1]['timestamp']-frames[0]['timestamp']} ms）")
    print(f"   帧被限流丢弃 = {limited}")
    print(f"   发送 sent = {sent}    跳过 skipped(死区) = {skipped}")
    print(f"   跳过率 = {skipped/max(1,sent+skipped)*100:.1f}%")

    print("-" * 78)
    print("D. 各轴实际位移（累计净位移 / 该轴可达域）")
    for a in "xyz":
        net = travelled[a]
        print(f"   {AXIS_NAME[a]:<12} 净位移 = {net:+8.2f} mm   占可达域(span {SPAN[a]}) "
              f"= {abs(net)/SPAN[a]*100:5.1f}%")
        if step_abs[a]:
            print(f"        {'':<12} 单步 |raw|: 中位 {sorted(step_abs[a])[len(step_abs[a])//2]:.2f} mm, "
                  f"最大 {max(step_abs[a]):.2f} mm")
        else:
            print(f"        {'':<12} 单步: 一条命令都没有下发（全部被死区吞掉）")

    print("-" * 78)
    print("E. 死区吞掉的帧里，各轴 raw 步长的量级")
    for a in "xyz":
        if raw_abs[a]:
            med = sorted(raw_abs[a])[len(raw_abs[a])//2]
            mx = max(raw_abs[a])
            print(f"   {AXIS_NAME[a]:<12} 有过运动的帧: {len(raw_abs[a]):3d}   "
                  f"|raw| 中位 {med:.3f} mm  最大 {mx:.2f} mm   "
                  f"(死区 {DEADZONE_MM} mm)")
        else:
            print(f"   {AXIS_NAME[a]:<12} 没有任何运动帧")
    return {"sent": sent, "skipped": skipped, "travelled": travelled, "pos": pos}


if __name__ == "__main__":
    agg = {"sent": 0, "skipped": 0}
    tot = {"x": 0.0, "y": 0.0, "z": 0.0}
    allpos = {a: [] for a in "xyz"}
    for p in sys.argv[1:]:
        r = analyze(p)
        agg["sent"] += r["sent"]
        agg["skipped"] += r["skipped"]
        for a in "xyz":
            tot[a] += r["travelled"][a]
            allpos[a] += r["pos"][a]
    print("=" * 78)
    print(f"合计: sent={agg['sent']} skipped={agg['skipped']} "
          f"跳过率={agg['skipped']/max(1,agg['sent']+agg['skipped'])*100:.1f}%")
    print(f"合计净位移: X(front)={tot['x']:+.1f}mm  Y(left)={tot['y']:+.1f}mm  Z(up)={tot['z']:+.1f}mm")
    print("全样本手部范围:")
    for a in "xyz":
        v = allpos[a]
        print(f"   hand {a}: span={max(v)-min(v):.4f}  [{min(v):.4f}, {max(v):.4f}]")
