#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Acceptance test for the P3 hand overlay (web side).

Covers the parts of the browser UI that CAN be proven without a browser, using
the frontend's own compiled modules and payloads taken from the live server:

  [A] live + recorded wire shape   - the hand block the API actually serves,
                                     including the "objects[0] mirrors the hand
                                     block" contract the overlay relies on
  [B] overlay geometry             - src/ai/draw.ts driven with a recording 2D
                                     context: bone/joint counts, and the
                                     no-clamping rule for out-of-frame points
  [C] HUD derivation               - src/ai/hand.ts over every recorded frame
  [D] component markup             - HandHud.vue / HandPanel.vue rendered
                                     through @vue/server-renderer

WHAT THIS DOES NOT COVER (and cannot): the pixels a browser actually paints.
Canvas rasterisation, WebRTC video, the 200 ms polling loop and the mode-switch
round trip all live above this layer. That is what the on-camera check is for -
see the "not covered" note printed at the end.

Methodology mirrors scripts/verify_metadata.py: print a PASS/FAIL tally and exit
non-zero if any check fails. The heavy lifting is done by
scripts/hand_overlay_probe.mjs, which loads the real modules via esbuild / Vite.

Usage:
  python scripts/verify_hand_overlay.py
  python scripts/verify_hand_overlay.py --base-url http://127.0.0.1:8081
  python scripts/verify_hand_overlay.py --camera camera01 --sessions 2
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

PASS = 0
FAIL = 0
SKIP = 0

HERE = os.path.dirname(os.path.abspath(__file__))
WEB_ROOT = os.path.normpath(os.path.join(HERE, "..", "web"))

# Every path the UI reads off a hand block. Verified against
# internal/metadata/model.go (HandState) - not guessed from the field names.
HAND_PATHS = [
    "detected",
    "confidence",
    "handedness",
    "reference_initialized",
    "position",
    "reference",
    "delta",
    "motion.x",
    "motion.y",
    "motion.z",
    "gesture",
    "gesture_candidate",
    "gesture_confidence",
]


def check(name, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  [PASS] {name}" + (f" :: {detail}" if detail else ""))
    else:
        FAIL += 1
        print(f"  [FAIL] {name}" + (f" :: {detail}" if detail else ""))
    return ok


def note(name, detail=""):
    global SKIP
    SKIP += 1
    print(f"  [SKIP] {name}" + (f" :: {detail}" if detail else ""))


def http_get_json(url, timeout=20):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def get(obj, dotted):
    """Dotted-path lookup; returns a (found, value) pair so a legitimately
    false/0 value is not confused with a missing field."""
    cur = obj
    for key in dotted.split("."):
        if not isinstance(cur, dict) or key not in cur:
            return False, None
        cur = cur[key]
    return True, cur


def find_node():
    for cand in (
        shutil.which("node"),
        r"C:\Users\lx176\.workbuddy\binaries\node\versions\22.22.2-3\node.exe",
    ):
        if cand and os.path.exists(cand if os.path.isabs(cand) else shutil.which(cand)):
            return cand
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default="http://127.0.0.1:8081")
    ap.add_argument("--camera", default="camera01")
    ap.add_argument("--sessions", type=int, default=2,
                    help="how many recorded sessions to pull landmarks from")
    args = ap.parse_args()

    base = args.base_url.rstrip("/")
    cam = args.camera

    print("=" * 70)
    print(" P3 hand overlay acceptance (headless)")
    print(f" server : {base}")
    print(f" camera : {cam}")
    print(f" web    : {WEB_ROOT}")
    print("=" * 70)

    # ---- [A] live wire shape -------------------------------------------------
    print("\n[A] live metadata payload")
    try:
        snap = http_get_json(f"{base}/api/cameras/{cam}/metadata")
    except urllib.error.URLError as e:
        check("GET /api/cameras/{id}/metadata", False, str(e))
        return finish()

    frame = snap.get("frame")
    if not check("a frame is being served", bool(frame), "no frame in the snapshot"):
        return finish()

    live_hand = frame.get("hand")
    print(f"  mode={frame.get('ai_mode')} objects={len(frame.get('objects', []))} "
          f"hand={'present' if live_hand else 'absent'}")

    live_ok = True
    if live_hand is None:
        note("live frame carries a hand block",
             f"ai_mode={frame.get('ai_mode')} - not in ai-hand mode, or the agent "
             "has not pushed since the mode was set")
    else:
        live_ok = check("live frame ai_mode is ai-hand",
                        frame.get("ai_mode") == "ai-hand", str(frame.get("ai_mode")))
        missing = [p for p in HAND_PATHS if not get(live_hand, p)[0]]
        check("every field the UI reads exists on the live hand block",
              not missing, ", ".join(missing) if missing else f"{len(HAND_PATHS)} paths")

        # The overlay draws the skeleton ONCE, from the hand block, on the
        # premise that objects[] carries the same landmarks. If that ever
        # drifts, the mode would render either twice or not at all.
        objs = frame.get("objects") or []
        hand_objs = [o for o in objs if o.get("class") == "hand"]
        if hand_objs:
            kps = hand_objs[0].get("keypoints")
            lm = live_hand.get("landmarks")
            same = (
                isinstance(kps, list)
                and isinstance(lm, list)
                and len(kps) == len(lm)
                and all(abs(a[0] - b[0]) < 1e-6 and abs(a[1] - b[1]) < 1e-6
                        for a, b in zip(kps, lm))
            )
            check("objects[0].keypoints mirrors hand.landmarks (skeleton drawn once)",
                  same, f"objects={len(kps) if isinstance(kps, list) else None} "
                        f"hand={len(lm) if isinstance(lm, list) else None}")
            check("a detected hand object carries all 21 keypoints",
                  isinstance(kps, list) and len(kps) in (0, 21),
                  f"len={len(kps) if isinstance(kps, list) else None}")

    # ---- [B] recorded real hand data ----------------------------------------
    print("\n[B] recorded hand frames (real landmarks)")
    archive_hands = []
    try:
        lst = http_get_json(f"{base}/api/cameras/{cam}/hand/sessions?gap_ms=1000")
        sessions = sorted(lst.get("sessions", []), key=lambda s: -s["detected_count"])
        picked = [s for s in sessions[: args.sessions] if s["detected_count"] > 0]
        check("the recorder has sessions with a detected hand", bool(picked),
              f"{len(picked)} session(s) chosen from {len(sessions)} total")
        for s in picked:
            doc = http_get_json(f"{base}/api/cameras/{cam}/hand/sessions/{s['index']}")
            rows = doc.get("frames", [])
            for row in rows:
                hand = row.get("hand")
                if hand and hand.get("detected") and hand.get("landmarks"):
                    archive_hands.append(hand)
            print(f"  session {s['index']}: {len(rows)} rows, "
                  f"gestures={s.get('gestures')}")
    except urllib.error.URLError as e:
        check("GET /hand/sessions", False, str(e))

    if archive_hands:
        with_lm = [h for h in archive_hands if len(h["landmarks"]) == 21]
        check("recorded detections carry 21 landmarks each",
              len(with_lm) == len(archive_hands),
              f"{len(with_lm)}/{len(archive_hands)} frames")
        # Out-of-frame landmarks are legitimate (the agent clamps the bbox, not
        # the points) - report whether this batch happens to contain any, so a
        # "no clamping" PASS is not read as stronger evidence than it is.
        oob = 0
        for h in archive_hands:
            for k in h["landmarks"]:
                if k[0] < 0 or k[0] > 1280 or k[1] < 0 or k[1] > 720:
                    oob += 1
        print(f"  out-of-frame landmark coordinates in this sample: {oob}")
    else:
        check("recorded detections carry 21 landmarks each", False,
              "no recorded frame with landmarks - record one with a hand in view")

    # ---- [C][D] run the frontend's own modules ------------------------------
    print("\n[C/D] frontend modules (esbuild bundle + Vite SSR)")
    node = find_node()
    if not node:
        note("frontend module probe", "node not found on PATH")
        return finish()

    probe = os.path.join(HERE, "hand_overlay_probe.mjs")
    tmpdir = tempfile.mkdtemp(prefix="vf-hand-probe-")
    probe_in = os.path.join(tmpdir, "input.json")
    probe_out = os.path.join(tmpdir, "result.json")
    payload = {
        "webRoot": WEB_ROOT,
        "liveHand": live_hand,
        "archiveHands": archive_hands[:8],
    }
    with open(probe_in, "w", encoding="utf-8") as f:
        json.dump(payload, f)

    try:
        proc = subprocess.run(
            [node, probe, probe_in, probe_out],
            capture_output=True, text=True, timeout=300, cwd=WEB_ROOT,
        )
        for line in (proc.stdout or "").splitlines():
            print(f"  node| {line}")
        for line in (proc.stderr or "").splitlines():
            print(f"  node! {line}")

        if not os.path.exists(probe_out):
            check("frontend module probe produced results", False,
                  f"no output file (exit {proc.returncode})")
            return finish()

        with open(probe_out, encoding="utf-8") as f:
            result = json.load(f)
        if result.get("fatal"):
            check("frontend module probe ran", False, result["fatal"].splitlines()[0])
            return finish()
        for c in result.get("checks", []):
            check(c["name"], c["ok"], c["detail"])
        for n in result.get("notes", []):
            note(n["name"], n["detail"])
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    return finish()


def finish():
    print("\n" + "=" * 70)
    print(f" PASS={PASS}  FAIL={FAIL}  SKIP={SKIP}")
    print("=" * 70)
    print(" NOT COVERED by this script (needs a real browser + a real hand):")
    print("   * canvas rasterisation - the probe asserts the coordinates passed")
    print("     to the 2D context, not the pixels that reach the screen")
    print("   * the WebRTC <video> layer and the 200 ms metadata polling loop")
    print("   * the ai-hand mode-switch round trip (agent polls, ~1s swap)")
    print("   * the HUD's liveliness - a stale block still renders identically")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
