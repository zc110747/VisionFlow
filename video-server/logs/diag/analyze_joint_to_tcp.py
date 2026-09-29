# Turn the recorded joint_state stream into "how far was the ARM commanded to go".
#
# FK from robot.yaml's own kinematics (the single truth source):
#   base link 60mm vertical, upper arm L1=80, forearm L2=80, wrist->TCP 40mm kept
#   HORIZONTAL (the gripper is locked level by the parallelogram).
#   `elbow` is stored as an ABSOLUTE angle from the zenith (joint coupling), so the
#   forearm direction uses elbow directly, not (elbow - shoulder).
#
# Self-check: at homePose the FK must land on the HOME point implied by the
# measured margins in camera-agent.yaml:  x = 105.78 + 9.25 = 115.03
#                                         z =  72.97 + 36.25... wait, +Z margin 5.25
#                                             => 114.47 - 5.25 = 109.22
import json
import math
import sys

BASE_H, L1, L2, WRIST = 60.0, 80.0, 80.0, 40.0
HOME = {"base": 0.0, "shoulder": 0.8498937633, "elbow": 112.6185771989, "gripper": 50.0}
# Limit box straight from robot.yaml actuators (servo limits mapped through calibration)
LIMITS = {"base": (-60.0, 60.0), "shoulder": (-6.0936827341, 49.454929245),
          "elbow": (108.4414852068, 141.8582211436)}


def fk(j):
    th1 = math.radians(j["shoulder"])
    th2 = math.radians(j["elbow"])
    b = math.radians(j["base"])
    r = L1 * math.sin(th1) + L2 * math.sin(th2) + WRIST
    z = BASE_H + L1 * math.cos(th1) + L2 * math.cos(th2)
    return r * math.cos(b), r * math.sin(b), z


x, y, z = fk(HOME)
print("=" * 78)
print("0. FK 自检（HOME 位姿 vs 配置里实测余量反推的 HOME 点）")
print(f"    FK 给出      x={x:8.3f}  y={y:8.3f}  z={z:8.3f}")
print(f"    实测反推      x={105.78+9.25:8.3f}  y={'--':>8}  z={114.47-5.25:8.3f}")
print(f"    误差          dx={x-(105.78+9.25):+.4f} mm   dz={z-(114.47-5.25):+.4f} mm")

# --- reachable band over the whole joint limit box (validates the FK's shape) ---
xs, ys, zs = [], [], []
N = 60
for i in range(N + 1):
    for k in range(N + 1):
        for m in range(N + 1):
            j = {"base": LIMITS["base"][0] + (LIMITS["base"][1] - LIMITS["base"][0]) * m / N,
                 "shoulder": LIMITS["shoulder"][0] + (LIMITS["shoulder"][1] - LIMITS["shoulder"][0]) * i / N,
                 "elbow": LIMITS["elbow"][0] + (LIMITS["elbow"][1] - LIMITS["elbow"][0]) * k / N}
            a, b, c = fk(j)
            xs.append(a); ys.append(b); zs.append(c)
print("=" * 78)
print("1. 关节限位盒整体映射出的 TCP 可达范围（粗采样，用于和实测表对照）")
print(f"    X 前后  [{min(xs):8.2f}, {max(xs):8.2f}]  span={max(xs)-min(xs):6.2f}   实测 [105.78, 144.78] span=39.0")
print(f"    Y 左右  [{min(ys):8.2f}, {max(ys):8.2f}]  span={max(ys)-min(ys):6.2f}   实测 [-88.00,  88.00] span=176.0")
print(f"    Z 上下  [{min(zs):8.2f}, {max(zs):8.2f}]  span={max(zs)-min(zs):6.2f}   实测 [ 72.97, 114.47] span=41.5")

# --- the recorded stream -------------------------------------------------------
path = sys.argv[1] if len(sys.argv) > 1 else "joint_stream.jsonl"
rows = []
for line in open(path, encoding="utf-8"):
    line = line.strip()
    if not line:
        continue
    m = json.loads(line)
    if m.get("type") == "joint_state" and m.get("joints"):
        rows.append(m)
print("=" * 78)
print(f"2. 实测关节流：{len(rows)} 帧，"
      f"{(rows[-1]['t']-rows[0]['t'])/1000:.1f} s")

print(f"{'轴':<14}{'起点mm':>10}{'终点mm':>10}{'起止差':>10}{'行程(min..max)':>18}{'Σ|Δ| mm':>12}")
tcp = [fk(r["joints"]) for r in rows]
for i, (name, unit) in enumerate([("X 前后", "mm"), ("Y 左右", "mm"), ("Z 上下", "mm")]):
    v = [p[i] for p in tcp]
    tot = sum(abs(v[k] - v[k - 1]) for k in range(1, len(v)))
    print(f"{name:<14}{v[0]:>10.2f}{v[-1]:>10.2f}{v[-1]-v[0]:>+10.2f}"
          f"{min(v):>18.2f}..{max(v):<0.0f}" if False else
          f"{name:<14}{v[0]:>10.2f}{v[-1]:>10.2f}{v[-1]-v[0]:>+10.2f}"
          f"{'%8.2f..%8.2f' % (min(v), max(v)):>18}{tot:>12.2f}")

print()
print("3. 每个关节被命令到的行程占其限位行程的比例")
for j, (lo, hi) in LIMITS.items():
    v = [r["joints"][j] for r in rows]
    print(f"    {j:<9} [{min(v):8.3f}, {max(v):8.3f}]  行程 {max(v)-min(v):6.3f}° / "
          f"可用 {hi-lo:6.2f}°  => 用了 {(max(v)-min(v))/(hi-lo)*100:5.1f}%")
