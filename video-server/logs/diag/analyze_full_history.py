# Full-history hand->arm transfer analysis, straight from the recorded hand log.
#
# Reads video.joint.db (ai_hand_log) read-only, splits rows into sessions the way
# the server itself does (runs of consecutive detections), and asks one question
# per axis:
#
#     "the hand sweeps s of the normalised frame  ->  how far does the ARM go?"
#
# The arm step the agent emits (robot_link.cpp::step) is
#     inc = delta_now - delta_prev            # cumulative -> increment
#     raw_armX =  inc_hand_y * gain_x * (span_x/0.5)      armX = front/back
#     raw_armY = -inc_hand_x * gain_y * (span_y/0.5)      armY = left/right
#     raw_armZ =  inc_hand_z * gain_z * (span_z/0.5)      armZ = up/down
# and a command whose 3-vector magnitude is under deadzone_mm is never sent.
#
# Run: python analyze_full_history.py
import json
import math
import sqlite3
import sys

DB = r"E:/cnb/git/VisionFlow/video-server/data/video.joint.db"

SPAN = {"x": 39.0, "y": 176.0, "z": 41.5}        # measured reachable span, mm
SHIPPED = {"x": 0.25, "y": 0.5, "z": 0.25}
CANDIDATES = [
    ("shipped 0.25/0.50/0.25", SHIPPED),
    ("proposed 1.50/0.60/1.80", {"x": 1.50, "y": 0.60, "z": 1.80}),
    ("half-sweep 1.48/0.63/1.82", {"x": 1.48, "y": 0.63, "z": 1.82}),
]
DEADZONE_MM = 0.5
K_HAND_ACTIVE_SPAN = 0.5
GAP_MS = 1000
MIN_SESSION_FRAMES = 20
FEED = {"x": "y", "y": "x", "z": "z"}            # arm axis <- hand axis
NAME = {"x": "armX front", "y": "armY left", "z": "armZ up"}
HANDAX = {"x": "hand x left/right", "y": "hand y depth", "z": "hand z up/down"}


def pct(v, q):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))]


def sessions(rows):
    """Runs of consecutive detections (the server's own rule), plus splits on a
    time gap > GAP_MS and on the timestamp going backwards (capture restart)."""
    out, cur, prev = [], [], None
    for ts, det, payload in rows:
        if prev is not None and (ts - prev > GAP_MS or ts < prev):
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


def parse(frames):
    pts = []
    for ts, payload in frames:
        try:
            h = json.loads(payload)
        except Exception:
            continue
        if not isinstance(h, dict) or not h.get("detected"):
            continue
        p, d = h.get("position"), h.get("delta")
        if not p or not d or not h.get("reference_initialized"):
            continue
        pts.append((ts, p, d))
    return pts


def run(pts, gains):
    """Replay one session through the shipped policy. Returns travel per arm axis."""
    g = {a: gains[a] * (SPAN[a] / K_HAND_ACTIVE_SPAN) for a in SPAN}
    travel = {a: 0.0 for a in "xyz"}
    steps = {a: [] for a in "xyz"}
    sent = skipped = 0
    prev = None
    for _, _, d in pts:
        if prev is None:
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


def main():
    c = sqlite3.connect(f"file:{DB}?mode=ro", uri=True)
    rows = c.execute("select timestamp, detected, payload from ai_hand_log order by id").fetchall()
    c.close()

    sess = sessions(rows)
    good = []
    for s in sess:
        pts = parse(s)
        if len(pts) >= MIN_SESSION_FRAMES:
            good.append(pts)

    print("=" * 84)
    print(f"数据源 ai_hand_log {len(rows)} 行 -> {len(sess)} 段检测段 -> "
          f"可用(>= {MIN_SESSION_FRAMES} 帧) {len(good)} 段")
    print(f"可用样本: {sum(len(p) for p in good)} 帧, 共 "
          f"{sum((p[-1][0]-p[0][0])/1000.0 for p in good):.0f} s")

    # ---- 1. hand input range ------------------------------------------------
    print("=" * 84)
    print("1. 手部归一化坐标的【单段活动范围】—— 这才是增益的输入量")
    print(f"{'手部轴':<22}{'p10':>9}{'中位':>9}{'p90':>9}{'最大':>9}")
    hspan = {a: [] for a in "xyz"}
    for pts in good:
        for a in "xyz":
            i = "xyz".index(a)
            v = [p[i] for _, p, _ in pts]
            hspan[a].append(max(v) - min(v))
    for a in "xyz":
        v = hspan[a]
        print(f"{HANDAX[a]:<22}{pct(v,.1):>9.4f}{pct(v,.5):>9.4f}{pct(v,.9):>9.4f}{max(v):>9.4f}")

    # ---- 2. gain ------------------------------------------------------------
    print("=" * 84)
    print("2. 换算系数 mm / 归一化单位（= gain x span / 0.5）")
    for a in "xyz":
        g = SHIPPED[a] * (SPAN[a] / K_HAND_ACTIVE_SPAN)
        print(f"   {NAME[a]:<12} feed={HANDAX[FEED[a]]:<18} gain={SHIPPED[a]:<5} "
              f"span={SPAN[a]:<7} => {g:7.1f} mm/单位")

    # ---- 3. shipped behaviour ----------------------------------------------
    res = [run(pts, SHIPPED) for pts in good]
    print("=" * 84)
    print("3. shipped 配置下，同一条命令里各轴分量的量级（|raw| mm）")
    print(f"{'轴':<12}{'p50':>9}{'p90':>9}{'p99':>9}{'最大':>9}")
    for a in "xyz":
        v = [x for _, st, _, _ in res for x in st[a]]
        print(f"{NAME[a]:<12}{pct(v,.5):>9.3f}{pct(v,.9):>9.3f}{pct(v,.99):>9.3f}"
              f"{(max(v) if v else float('nan')):>9.3f}")
    for a in "xyz":
        v = [x for _, st, _, _ in res for x in st[a]]
        n = len(v)
        print(f"   {NAME[a]:<12} 单轴步长 < 0.5mm 的占比 = "
              f"{sum(1 for x in v if x < 0.5)/max(1,n)*100:5.1f}%")

    # ---- 4. authority ------------------------------------------------------
    print("=" * 84)
    print("4. 每轴【权限】= 该段净位移 / 该轴可达域（89 段的中位）")
    print(f"{'轴':<12}{'shipped':>12}{'手部单段范围(中位)':>22}")
    for a in "xyz":
        v = [abs(tr[a]) / SPAN[a] * 100 for tr, _, _, _ in res]
        print(f"{NAME[a]:<12}{pct(v,.5):>11.1f}%{pct(hspan[FEED[a]],.5):>22.4f}")

    # ---- 5. what gains would equalise --------------------------------------
    print("=" * 84)
    print("5. 要让三轴手感一致，gain 该是多少")
    print("   推导: 手走 s(归一化) 想走完 F 倍可达域")
    print("         s*gain*(span/0.5) = F*span  =>  span 抵消  =>  gain = 0.5*F/s")
    print(f"{'arm 轴':<12}{'feed':<18}{'手部中位 s':>12}{'F=.5':>8}{'F=1.0':>8}{'shipped':>9}")
    for a in "xyz":
        s = pct(hspan[FEED[a]], .5)
        print(f"{NAME[a]:<12}{HANDAX[FEED[a]]:<18}{s:>12.4f}"
              f"{0.5*0.5/s:>8.2f}{0.5*1.0/s:>8.2f}{SHIPPED[a]:>9}")

    # ---- 6. what-if sweep --------------------------------------------------
    print("=" * 84)
    print("6. 换 gains 后每段净位移（占该轴可达域的中位 %，同一批真实数据重算）")
    print(f"{'候选 gains':<26}{'armX front':>14}{'armY left':>14}{'armZ up':>14}")
    for label, G in CANDIDATES:
        r2 = [run(pts, G) for pts in good]
        row = []
        for a in "xyz":
            v = [abs(tr[a]) / SPAN[a] * 100 for tr, _, _, _ in r2]
            row.append(pct(v, .5))
        print(f"{label:<26}{row[0]:>13.1f}%{row[1]:>13.1f}%{row[2]:>13.1f}%")
    print("   注：不含软边界衰减与真机 1° 舵机量化，故为【上界】")


if __name__ == "__main__":
    sys.exit(main())
