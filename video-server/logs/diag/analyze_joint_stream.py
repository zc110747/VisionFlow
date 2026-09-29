# Analyse the recorded joint_state stream: what did the ARM actually get commanded?
#
# Each line is a joint_state broadcast (Origin=command) => the arm's commanded pose.
# The question this answers: while the operator waves a hand, how many degrees do
# J1 (base), J2 (shoulder) and J3 (elbow) really travel?
import json
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "joint_stream.jsonl"
rows = []
for line in open(path, encoding="utf-8"):
    line = line.strip()
    if not line:
        continue
    try:
        m = json.loads(line)
    except Exception:
        continue
    if m.get("type") == "joint_state" and m.get("joints"):
        rows.append(m)

if not rows:
    print("no joint_state rows")
    sys.exit(1)

J = ["base", "shoulder", "elbow", "gripper"]
t0 = rows[0]["t"]
t1 = rows[-1]["t"]
print(f"joint_state frames = {len(rows)}   span = {(t1-t0)/1000.0:.1f} s   "
      f"平均 {(len(rows)/max(0.001,(t1-t0)/1000.0)):.1f} Hz")
print()
print("各关节被命令到的角度范围（度）")
print(f"{'关节':<10}{'min':>10}{'max':>10}{'行程':>10}{'单帧最大变化':>14}")
stats = {}
for j in J:
    v = [r["joints"].get(j) for r in rows if r["joints"].get(j) is not None]
    if not v:
        continue
    d = [abs(v[i] - v[i - 1]) for i in range(1, len(v))]
    stats[j] = (min(v), max(v), max(v) - min(v), max(d) if d else 0.0)
    print(f"{j:<10}{min(v):>10.3f}{max(v):>10.3f}{max(v)-min(v):>10.3f}"
          f"{(max(d) if d else 0.0):>14.3f}")

# total path length per joint (sum of |delta|) = "how much did it actually move"
print()
print("累计运动量（Σ|Δ角|，度）—— 反映真实动过多少，不是起止差")
for j in J:
    v = [r["joints"].get(j) for r in rows if r["joints"].get(j) is not None]
    tot = sum(abs(v[i] - v[i - 1]) for i in range(1, len(v)))
    print(f"   {j:<10} Σ|Δ| = {tot:8.2f}°")

print()
print("前 12 帧明细")
for r in rows[:12]:
    print("  t=%7.0fms  " % (r["t"] - t0) +
          "  ".join(f"{j}={r['joints'].get(j):.3f}" for j in J if r["joints"].get(j) is not None) +
          f"   origin={r.get('origin')}")
