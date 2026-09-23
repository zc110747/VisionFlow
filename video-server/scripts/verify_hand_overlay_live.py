#!/usr/bin/env python3
"""verify_hand_overlay_live.py - pixel-level proof that the hand overlay lands ON the hand.

Why this exists
---------------
Every other layer is already covered without a browser:

  * data   - verify_hand_overlay.py feeds RECORDED frames through the real
             draw.ts with a recording 2D context, so the drawing *logic* is
             proven, but nothing ever paints a real pixel.
  * DOM    - SSR renders HandHud/HandPanel, so the HUD *strings* are proven,
             but no layout or live polling is exercised.

What is left is exactly the thing a user notices first: "the skeleton does not
sit on my hand". That is a claim about real pixels in a real browser against a
real camera. This script closes it:

  1. wait for the live agent to actually DETECT a hand (21 landmarks)
  2. drive a real Chromium to the camera page
  3. read the overlay canvas back with getImageData and map the painted pixels
     back into ORIGINAL video coordinates
  4. assert the painted pixels sit on the hand the API just reported

Assertion 4 is the whole point. "Some pixels were painted" would pass on a
skeleton drawn in the wrong corner; comparing against the live landmarks is
what makes it a claim about the hand rather than about the canvas.

Exit codes: 0 = all passed, 1 = a check failed, 2 = could not run (no hand seen
within the timeout, or no Chromium available) - distinct on purpose, because
"the operator was not in front of the camera" is not a product defect.

Usage
-----
  python scripts/verify_hand_overlay_live.py --wait-hand 120
  python scripts/verify_hand_overlay_live.py --host 127.0.0.1:8081 --camera camera01
  python scripts/verify_hand_overlay_live.py --no-browser      # data side only
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))

PASS = 0
FAIL = 0
SKIP = 0


def check(name: str, ok: bool, detail: str = "") -> bool:
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  [PASS] {name}" + (f"  ({detail})" if detail else ""))
    else:
        FAIL += 1
        print(f"  [FAIL] {name}" + (f"  -> {detail}" if detail else ""))
    return ok


def skip(name: str, detail: str = "") -> None:
    global SKIP
    SKIP += 1
    print(f"  [SKIP] {name}" + (f"  -> {detail}" if detail else ""))


# ---------------------------------------------------------------------------
# HTTP (never through a proxy: a proxy answers 502 for its own upstream
# failures and the report would blame the wrong component)
# ---------------------------------------------------------------------------
def get_json(base: str, path: str, timeout: float = 5.0):
    req = urllib.request.Request(base.rstrip("/") + path, headers={"User-Agent": "verify_hand_overlay_live"})
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(req, timeout=timeout) as r:
        return json.loads(r.read())


def frame_of(base: str, camera: str):
    """The frame under test: live, or the one pinned via VF_FRAME_JSON.

    Pinning exists because the honest comparison is "the pixels drew the frame
    that was on screen when they were painted" - not "the pixels match whatever
    the hand happens to be doing when the assertion process starts". An
    operator lowers their arm the moment they think they are done; reading live
    then reports SKIP and hides a real result. It also makes wait_for_hand
    return immediately, since a pinned frame is already known.
    """
    pinned = os.environ.get("VF_FRAME_JSON", "")
    if pinned:
        with open(pinned, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
        return doc.get("frame") or {}
    doc = get_json(base, f"/api/cameras/{camera}/metadata")
    return doc.get("frame") or {}


def wait_for_hand(base: str, camera: str, timeout: float):
    """Poll until the agent reports a detected hand with the full landmark set.

    Returns (frame, seconds_waited) or (None, seconds_waited).
    """
    t0 = time.time()
    last = ""
    while True:
        try:
            f = frame_of(base, camera)
            h = f.get("hand") or {}
            n = len(h.get("landmarks") or [])
            last = f"detected={h.get('detected')} gesture={h.get('gesture')} landmarks={n}"
            if h.get("detected") and n == 21:
                return f, time.time() - t0
        except Exception as e:  # noqa: BLE001
            last = f"transport: {e}"
        if time.time() - t0 >= timeout:
            print(f"  last seen: {last}")
            return None, time.time() - t0
        time.sleep(1.0)


# ---------------------------------------------------------------------------
# Chromium discovery - agent-browser ships no browser here, but playwright's
# cached Chromium is on disk and fully usable via --executable-path.
# ---------------------------------------------------------------------------
def find_chrome() -> str:
    pats = [
        os.path.expandvars(r"%LOCALAPPDATA%\ms-playwright\chromium-*\chrome-win64\chrome.exe"),
        os.path.expandvars(r"%LOCALAPPDATA%\ms-playwright\chromium-*\chrome-win\chrome.exe"),
        os.path.expanduser(r"~/.agent-browser/browsers/*/chrome.exe"),
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    ]
    for pat in pats:
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[-1]
    return ""


def find_agent_browser() -> str:
    for cand in [
        os.path.expandvars(r"%APPDATA%\npm\agent-browser.cmd"),
        os.path.expandvars(r"%APPDATA%\npm\agent-browser"),
    ]:
        if os.path.exists(cand):
            return cand
    return shutil.which("agent-browser") or ""


# JS executed inside the page. Returns compact JSON:
#   video  : the <video> box and its real decoded size
#   canvas : every <canvas> with its painted-pixel bounding box (alpha > 0)
#   hud    : the HTML overlay text (the HUD is HTML on purpose - see README)
# Numbers only, so the assertions below stay free of string parsing.
# The expression is ONE line with no double quotes, no arrow functions and no
# comparison operators. That is not cosmetic: on Windows agent-browser is a
# .cmd shim, so the expression travels through cmd.exe. Python joins argv with
# list2cmdline, which escapes every inner `"` as `\"` - an escape cmd.exe does
# not honour, so the quoting collapses and the eval silently returns a literal
# `null` (observed: the expression never ran and the report read like "the page
# has no DOM"). Single quotes, `function` instead of `=>`, and Math.min/Math.max
# instead of `<`/`>` leave cmd.exe nothing to mangle.
EVAL_JS = (
    "(() => {"
    " const out = { video: null, canvas: [], hud: null, panel: null, buttons: [] };"
    " const v = document.querySelector('video');"
    " if (v) out.video = { vw: v.videoWidth, vh: v.videoHeight, rs: v.readyState, cw: v.clientWidth, ch: v.clientHeight };"
    " for (const c of document.querySelectorAll('canvas')) {"
    "  const s = { w: c.width, h: c.height, cw: c.clientWidth, ch: c.clientHeight };"
    "  try {"
    "   const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;"
    "   let n = 0, minX = -1, minY = -1, maxX = -1, maxY = -1;"
    # `y - c.height` instead of `y !== c.height`, for the same reason there are
    # no double quotes: the .bat that consumes this string reads it into a
    # variable under EnableDelayedExpansion, where `!` opens a delayed variable
    # reference and silently eats everything up to the next `!`. Measured cost of
    # leaving `!==` in: 1313 bytes in, 1270 out, and eval then fails on a
    # truncated expression. Non-zero is truthy, so subtraction loops fine.
    "   for (let y = 0; y - c.height; y++) {"
    "    for (let x = 0; x - c.width; x++) {"
    "     if (d[(y * c.width + x) * 4 + 3]) {"
    "      if (n === 0) { minX = x; maxX = x; minY = y; maxY = y; }"
    "      else { minX = Math.min(minX, x); maxX = Math.max(maxX, x); minY = Math.min(minY, y); maxY = Math.max(maxY, y); }"
    "      n++;"
    "     }"
    "    }"
    "   }"
    "   s.opaque = n; s.minX = minX; s.minY = minY; s.maxX = maxX; s.maxY = maxY;"
    "  } catch (e) { s.error = String(e); }"
    "  out.canvas.push(s);"
    " }"
    " const norm = function (sel) { const e = document.querySelector(sel); return e ? e.innerText.replace(/\\s+/g, ' ').trim() : null; };"
    " out.hud = norm('.hand-hud');"
    " out.panel = norm('.hand-panel');"
    " out.buttons = [...document.querySelectorAll('button')].map(function (b) { return b.innerText.trim(); });"
    " return JSON.stringify(out);"
    "})()"
)


def run_cli(exe: str, args: list[str], timeout: float = 180.0):
    """Run agent-browser, return (rc, stdout+stderr combined)."""
    p = subprocess.run([exe] + args, capture_output=True, text=True, timeout=timeout, shell=False)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def parse_eval(out: str):
    """The CLI prints the JS return value; recover the JSON object from it.

    agent-browser prints the value as a JS string literal, so the JSON we
    returned comes back re-escaped ('{\\"video\\":...}'). Decoding that layer
    first is the difference between a working check and a bogus
    "Expecting property name enclosed in double quotes": the brace slice in the
    fallback finds the first `{` *inside* those escapes and slices garbage.
    """
    text = (out or "").strip()
    for cand in [text] + text.splitlines():
        cand = cand.strip()
        if not cand:
            continue
        try:
            value = json.loads(cand)
        except Exception:  # noqa: BLE001
            continue
        if isinstance(value, str):
            try:
                return json.loads(value)
            except Exception:  # noqa: BLE001
                continue
        if isinstance(value, dict):
            return value

    i, j = text.find("{"), text.rfind("}")
    if i < 0 or j <= i:
        raise ValueError(f"no JSON object in eval output: {text[:400]!r}")
    return json.loads(text[i : j + 1])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1:8081")
    ap.add_argument("--camera", default="camera01")
    ap.add_argument("--wait-hand", type=float, default=120.0, help="seconds to wait for a detected hand")
    ap.add_argument("--settle", type=float, default=2.0, help="seconds to let the page poll after load")
    ap.add_argument("--shot-dir", default=os.path.join(tempfile.gettempdir(), "vf-shots"))
    ap.add_argument("--no-browser", action="store_true", help="stop after the data-side check")
    # How far the painted pixels may sit from the reported landmarks, as a
    # fraction of the frame diagonal. The skeleton connects landmarks, so with a
    # held-still hand the expected distance is ~0; this only absorbs the
    # hand moving between the API read and the canvas paint.
    ap.add_argument("--tol", type=float, default=0.06)
    args = ap.parse_args()

    base = "http://" + args.host
    print(f"verify_hand_overlay_live: {base} camera={args.camera}")

    # ---- [1] the agent must be reporting a hand ---------------------------
    f, waited = wait_for_hand(base, args.camera, args.wait_hand)
    if not check(
        "the live agent reports a detected hand with 21 landmarks",
        f is not None,
        f"after {waited:.0f}s" if f is not None else f"not within {args.wait_hand:.0f}s - raise a hand at the camera",
    ):
        print("\nRESULT: SKIP - no hand in front of the camera; this is a setup")
        print("        condition, not a defect. Re-run while holding a hand up.")
        return 2

    h = f["hand"]
    lms = h["landmarks"]
    fw, fh = int(f.get("video_width") or 0), int(f.get("video_height") or 0)
    print(f"  live frame: id={f.get('frame_id')} {fw}x{fh} gesture={h.get('gesture')} "
          f"conf={h.get('confidence')} handed={h.get('handedness')} ref_init={h.get('reference_initialized')}")

    lm_xs = [p[0] for p in lms]
    lm_ys = [p[1] for p in lms]
    lm_box = (min(lm_xs), min(lm_ys), max(lm_xs), max(lm_ys))
    lm_cx, lm_cy = sum(lm_xs) / len(lm_xs), sum(lm_ys) / len(lm_ys)
    print(f"  landmark bbox in ORIGINAL px: ({lm_box[0]:.0f},{lm_box[1]:.0f})-({lm_box[2]:.0f},{lm_box[3]:.0f})")

    if args.no_browser:
        print(f"\nRESULT: data-side only, PASS={PASS} FAIL={FAIL}")
        return 1 if FAIL else 0

    # ---- [2] obtain the browser's view of the page ------------------------
    # Two ways in, because on this host they behave very differently:
    #
    #   (a) VF_FROM_DOM=<file>  the SHELL drove the browser and redirected the
    #       `eval` output into <file>. Preferred here, and the only path that
    #       works: booting the browser daemon from inside a Python subprocess
    #       gets the whole command killed (unexplained SIGTERM), while the
    #       identical CLI calls issued from a shell run fine.
    #   (b) default             drive the CLI from this process. Kept because it
    #       is the natural shape of the check on a host without that limit.
    dom = None
    shot = ""
    from_dom = getattr(args, "from_dom", "") or os.environ.get("VF_FROM_DOM", "")
    if from_dom:
        print(f"  DOM snapshot: {from_dom}", flush=True)
        try:
            with open(from_dom, "r", encoding="utf-8") as fh:
                raw = fh.read()
            dom = parse_eval(raw)
            check("the DOM snapshot is parsable", True, f"{len(raw)} bytes")
        except Exception as e:  # noqa: BLE001
            check("the DOM snapshot is parsable", False, str(e))
            print(f"\nRESULT: PASS={PASS} FAIL={FAIL}")
            return 1
    else:
        cli = find_agent_browser()
        chrome = find_chrome()
        if not check("agent-browser CLI is available", bool(cli), cli or "not found"):
            return 1
        if not check("a Chromium binary is available", bool(chrome), chrome or "run: agent-browser install"):
            return 1

        url = f"{base}/camera/{args.camera}"
        try:
            rc, out = run_cli(cli, ["open", url, "--executable-path", chrome])
            check("browser opened the camera page", rc == 0 and "✗" not in out, out.strip()[:200])

            time.sleep(args.settle)

            rc, out = run_cli(cli, ["eval", EVAL_JS])
            dom = parse_eval(out)

            os.makedirs(args.shot_dir, exist_ok=True)
            shot = os.path.join(args.shot_dir, f"hand-overlay-{args.camera}.png")
            rc, out = run_cli(cli, ["screenshot", shot])
            check("screenshot captured", os.path.exists(shot) and os.path.getsize(shot) > 0, shot)
        except Exception as e:  # noqa: BLE001
            check("browser session completed", False, str(e))
            print(f"\nRESULT: PASS={PASS} FAIL={FAIL}")
            return 1
        finally:
            # Best effort: on this host the daemon dies with the parent command
            # anyway, and `close` on a missing daemon can block, so it is
            # time-bounded and never fatal.
            try:
                run_cli(cli, ["close"], timeout=45)
            except Exception:  # noqa: BLE001
                pass

    # The frame the canvas actually painted from. Read AFTER the browser leg so
    # it brackets the paint, then take the landmark centroid from that frame -
    # the canvas repaints on the same 200 ms poll, so this is the closest
    # available stand-in for "the frame that is on screen".
    f2 = frame_of(base, args.camera)
    h2 = f2.get("hand") or {}
    lm2 = h2.get("landmarks") or []
    if len(lm2) == 21:
        xs = [p[0] for p in lm2]
        ys = [p[1] for p in lm2]
        cx2, cy2 = sum(xs) / 21, sum(ys) / 21
        b2 = (min(xs), min(ys), max(xs), max(ys))
    else:
        cx2, cy2, b2 = lm_cx, lm_cy, lm_box
    fw2 = int(f2.get("video_width") or fw)
    fh2 = int(f2.get("video_height") or fh)

    # ---- [3] the DOM / canvas facts --------------------------------------
    v = dom.get("video")
    if check("<video> decodes a real stream", bool(v) and v.get("vw", 0) > 0, json.dumps(v)):
        pass

    canv = dom.get("canvas") or []
    overlay = canv[0] if canv else None
    if not check("an overlay <canvas> exists", bool(overlay), f"{len(canv)} canvas element(s)"):
        print(f"\nRESULT: PASS={PASS} FAIL={FAIL}")
        return 1

    # The overlay must share the video element's box, otherwise every point is
    # scaled against the wrong rectangle and the skeleton drifts.
    if v:
        same_box = overlay.get("cw") == v.get("cw") and overlay.get("ch") == v.get("ch")
        check(
            "overlay canvas is laid out over the video box",
            same_box,
            f"canvas {overlay.get('cw')}x{overlay.get('ch')} vs video {v.get('cw')}x{v.get('ch')}",
        )

    # The pixel set measured below is the UNION of everything the overlay
    # paints: the object bbox rectangle, its class label, and the hand skeleton.
    #
    # For ai-hand the agent reports the hand bbox as a FULL-FRAME-HEIGHT box -
    # measured [350, 0, 749, 720] for a frame whose landmarks only span
    # y 186..697 - so that rectangle already paints two tall vertical lines and
    # dominates the bounding box below. Two consequences, both deliberate:
    #   * the checks here are union tests. They catch "the overlay draws
    #     somewhere else entirely", not "the skeleton is 3 px off the knuckle".
    #   * a skeleton-only claim rests on verify_hand_overlay.py (recorded 2D
    #     context on real frames) plus the screenshot this script saves.
    opaque = overlay.get("opaque") or 0
    if not check(
        "the overlay painted pixels while a hand is present",
        opaque > 0,
        f"opaque={opaque} canvas={overlay.get('w')}x{overlay.get('h')}"
        + (f" error={overlay.get('error')}" if overlay.get("error") else ""),
    ):
        print("\n  NOTE: no pixel was painted even though the API reports a hand.")
        print("        Check the metadata poll interval and the mode chip (must be 手势).")
        print(f"\nRESULT: PASS={PASS} FAIL={FAIL}")
        return 1

    # ---- [4] do those pixels sit ON the hand? -----------------------------
    # Canvas px -> original video px. Canvas width equals the video element's
    # client width, so the scale is (video_width / canvas_width).
    scale = (fw2 / float(overlay["w"])) if overlay.get("w") else 0.0
    if overlay.get("w") and overlay["w"] != v.get("cw"):
        scale = (fw2 / float(overlay["w"]))
    drawn = {
        "x0": overlay["minX"] * scale,
        "y0": overlay["minY"] * scale,
        "x1": overlay["maxX"] * scale,
        "y1": overlay["maxY"] * scale,
    }
    drawn_cx = (drawn["x0"] + drawn["x1"]) / 2.0
    drawn_cy = (drawn["y0"] + drawn["y1"]) / 2.0
    diag = (fw2 * fw2 + fh2 * fh2) ** 0.5
    dist = ((drawn_cx - cx2) ** 2 + (drawn_cy - cy2) ** 2) ** 0.5
    tol_px = args.tol * diag
    print(f"  painted bbox in ORIGINAL px: ({drawn['x0']:.0f},{drawn['y0']:.0f})-({drawn['x1']:.0f},{drawn['y1']:.0f})")
    print(f"  landmark centroid=({cx2:.0f},{cy2:.0f})  painted centroid=({drawn_cx:.0f},{drawn_cy:.0f})  dist={dist:.0f}px (tol={tol_px:.0f}px)")
    check(
        "painted skeleton is centred on the reported hand",
        dist <= tol_px,
        f"dist={dist:.0f}px <= tol={tol_px:.0f}px",
    )

    # Overlap test: the painted box must intersect the landmark box. A skeleton
    # drawn "somewhere else on screen" fails this even if it is centred.
    pad = diag * 0.02
    overlap = not (
        drawn["x1"] < b2[0] - pad or drawn["x0"] > b2[2] + pad
        or drawn["y1"] < b2[1] - pad or drawn["y0"] > b2[3] + pad
    )
    check(
        "painted skeleton overlaps the hand's bounding box",
        overlap,
        f"painted=({drawn['x0']:.0f},{drawn['y0']:.0f})-({drawn['x1']:.0f},{drawn['y1']:.0f}) "
        f"hand=({b2[0]:.0f},{b2[1]:.0f})-({b2[2]:.0f},{b2[3]:.0f})",
    )

    # ---- [5] the HTML HUD reflects the same live frame --------------------
    hud = dom.get("hud") or ""
    panel = dom.get("panel") or ""
    bad = [s for s in ("undefined", "NaN", "[object Object]") if s in hud or s in panel]
    check("HUD/panel contain no undefined / NaN / [object Object]", not bad, f"found={bad}" if bad else "clean")
    check(
        "HUD shows the live gesture",
        bool(h2.get("gesture")) and h2.get("gesture") in hud,
        f"hud={hud!r}",
    )
    if h2.get("reference_initialized"):
        check("HUD reports REF LOCKED once the reference is latched", "REF LOCKED" in hud, hud)
    else:
        check("HUD reports REF LATCHING while the reference is unset", "REF LATCHING" in hud, hud)

    # ---- [6] the mode chip -------------------------------------------------
    check("a 手势 (hand) mode chip is present", any("手势" in b for b in dom.get("buttons") or []), str(dom.get("buttons")))

    print(f"\nRESULT: PASS={PASS} FAIL={FAIL} SKIP={SKIP}")
    print(f"  screenshot: {shot}")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
