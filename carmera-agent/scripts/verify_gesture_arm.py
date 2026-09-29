#!/usr/bin/env python3
"""Gesture -> MeArmPilot end-to-end verification.

Runs the verification ladder from the task plan in one shot and prints a
PASS/FAIL tally:

  Level 0  build                the test binary exists and is current
  Level 1  live backend         RobotLink talks to a real armpilot-backend.exe
                                over TCP: caps, home, accept/reject, re-entry
  Level 2a synthetic            pixels -> HandState -> commands against a KNOWN
                                geometric ground truth (the composited sequence)
  Level 2b real frames          the same chain on real captures, asserting
                                robustness rather than accuracy (no ground truth)
  Level 3  live camera          MANUAL - prints the command for the operator

Level 1 self-skips when no backend is listening, so this script is safe to run
on a machine that only has the build.

Usage:
    python scripts/verify_gesture_arm.py
    python scripts/verify_gesture_arm.py --host 127.0.0.1 --port 9100
    python scripts/verify_gesture_arm.py --no-live      # skip Level 1
"""
from __future__ import annotations

import argparse
import os
import re
import socket
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS_EXE = ROOT / "build-msvc" / "tests" / "camera-agent-tests.exe"
SYNTH = ROOT / "build-msvc" / "synth" / "expected.json"
GENERATOR = ROOT / "tests" / "golden" / "tools" / "make_synthetic_hand_sequence.py"

# The four tests that make up Levels 1 and 2, in the order we report them.
LEVEL1 = "robot_live_backend_drives_sim_arm"
LEVEL2_SYNTH = ["robot_synthetic_hand_sequence_drives_arm",
                "robot_synthetic_sequence_over_worker_thread"]
LEVEL2_REAL = ["robot_realframes_drive_arm_without_ground_truth",
               "robot_realframes_over_worker_thread"]
# Soft-boundary behaviour lives in the same test binary as the rest of the
# RobotLink suite, so it rides along with Level 2's run - listed separately so a
# regression names itself instead of hiding inside the aggregate count.
SOFT_BOUNDARY = ["robot_soft_boundary_tapers_gain_monotonically",
                 "robot_soft_boundary_quiets_the_wall",
                 "robot_soft_boundary_off_is_the_old_behaviour",
                 "robot_soft_boundary_uses_measured_span",
                 "robot_soft_probe_recovers_a_halved_step",
                 "robot_soft_probe_off_sends_only_the_original_step",
                 # Feel: a pin on the shipped gain/span values, so retuning the
                 # arm's responsiveness (hand big -> arm small) cannot happen
                 # silently. See robot_link_tests.cpp ㉑.
                 "robot_shipped_gains_are_the_documented_feel"]
BACKEND_RETRY = ["robot_unreachable_backend_retries_and_recovers"]

results: list[tuple[str, bool, str]] = []


def record(name: str, ok: bool, detail: str = "") -> None:
    results.append((name, ok, detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" +
          (f"  - {detail}" if detail else ""))


def port_open(host: str, port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(1.0)
        return s.connect_ex((host, port)) == 0


def run_tests(env_extra: dict[str, str]) -> tuple[int, str]:
    env = dict(os.environ)
    env.update(env_extra)
    p = subprocess.run([str(TESTS_EXE)], cwd=str(ROOT), env=env,
                       capture_output=True, text=True, errors="replace")
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def per_test_status(output: str) -> dict[str, bool]:
    """Map each test name to pass/fail from the harness's own output."""
    out: dict[str, bool] = {}
    for m in re.finditer(r"\[ RUN \] (\S+)", output):
        out[m.group(1)] = False
    for m in re.finditer(r"\[ OK  \] (\S+)", output):
        out[m.group(1)] = True
    return out


def ensure_synthetic(force: bool) -> bool:
    """The synthetic sequence is a build artifact; regenerate when missing."""
    if SYNTH.exists() and not force:
        return True
    if not GENERATOR.exists():
        return False
    py = sys.executable
    cmd = [py, str(GENERATOR),
           "--fixture", str(ROOT / "tests" / "golden" / "woman_hands.png"),
           "--models", "models",
           "--frames", str(ROOT / "build-msvc" / "synth"),
           "--expect", str(SYNTH)]
    p = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True,
                       errors="replace")
    return p.returncode == 0 and SYNTH.exists()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9100)
    ap.add_argument("--no-live", action="store_true",
                    help="skip Level 1 (live backend) entirely")
    ap.add_argument("--regen", action="store_true",
                    help="regenerate the synthetic sequence first")
    args = ap.parse_args()

    print("=" * 68)
    print("Gesture -> MeArmPilot verification")
    print("=" * 68)

    # ---- Level 0: build ------------------------------------------------
    print("\n[Level 0] build")
    if not TESTS_EXE.exists():
        record("test binary present", False,
               f"missing {TESTS_EXE} - run scripts/build-msvc.ps1 -Backend sim")
        return finish()
    record("test binary present", True, TESTS_EXE.name)

    if not ensure_synthetic(args.regen):
        record("synthetic sequence present", False,
               "regeneration failed - see make_synthetic_hand_sequence.py")
        return finish()
    record("synthetic sequence present", True, f"{SYNTH.parent}")

    # ---- Level 1: live backend -----------------------------------------
    print(f"\n[Level 1] live backend {args.host}:{args.port}")
    live = not args.no_live and port_open(args.host, args.port)
    if args.no_live:
        record("backend reachable", True, "skipped by --no-live")
    elif not live:
        record("backend reachable", False,
               "nothing listening - start armpilot-backend.exe "
               "(Level 1 skipped, not a code failure)")
    else:
        record("backend reachable", True)
        rc, out = run_tests({"ARM_PILOT_LIVE": "1",
                             "ARM_PILOT_HOST": args.host,
                             "ARM_PILOT_PORT": str(args.port)})
        st = per_test_status(out)
        record("RobotLink drives real sim arm", st.get(LEVEL1, False))
        for line in out.splitlines():
            if line.strip().startswith(("caps:", "big move:", "return HOME",
                                        "home ok")):
                print(f"        {line.strip()}")

    # ---- Level 2: synthetic + real frames ------------------------------
    print("\n[Level 2a] synthetic sequence (known ground truth)")
    rc, out = run_tests({})
    st = per_test_status(out)
    for name in LEVEL2_SYNTH:
        record(name, st.get(name, False))
    for line in out.splitlines():
        if "sequence:" in line or "detected " in line or "position error:" in line \
                or "commands: movexyz" in line:
            print(f"        {line.strip()}")

    print("\n[Level 2b] real captured frames (robustness, no ground truth)")
    for name in LEVEL2_REAL:
        record(name, st.get(name, False))
    for line in out.splitlines():
        if "real frames:" in line:
            print(f"        {line.strip()}")

    print("\n[Level 2c] soft boundary + backend self-healing (no hardware)")
    for name in SOFT_BOUNDARY + BACKEND_RETRY:
        record(name, st.get(name, False))

    # ---- Level 3: manual ----------------------------------------------
    print("\n[Level 3] live camera - MANUAL")
    print("  Run this with a real camera and the backend up, then perform an")
    print("  open palm in front of the lens and move it left/right and nearer:")
    print()
    print("    build-msvc\\src\\camera-agent.exe ^")
    print("        --ai --ai-mode hand --robot ^")
    print(f"        --robot-host {args.host} --robot-port {args.port} ^")
    print("        --source mfvideosrc")
    print()
    print("  Expect in the log:")
    print("    [ROBOT] connected to <host>:<port>")
    print("    [ROBOT] caps protocol=2 ...")
    print("    [ROBOT] cmd=movexyz delta=[..] ok   <- while the hand moves")
    print("    [ROBOT] hand (re)entered the frame - returning HOME")
    record("live camera instructions printed", True, "manual step")

    return finish()


def finish() -> int:
    print("\n" + "=" * 68)
    passed = sum(1 for _, ok, _ in results if ok)
    for name, ok, detail in results:
        mark = "PASS" if ok else "FAIL"
        print(f"  [{mark}] {name}" + (f"  - {detail}" if detail else ""))
    print("-" * 68)
    print(f"  {passed}/{len(results)} checks passed")
    print("=" * 68)
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
