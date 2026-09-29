#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Synthesise a hand-image SEQUENCE with a known ground truth, for the
hand -> robot-arm branch (`tests/golden/README.md` is the sibling doc).

Why this exists
---------------
The robot branch can be unit-tested with hand-built `HandState` values
(`tests/robot_link_tests.cpp`), but that proves nothing about the *image ->
landmark -> HandState* half. To close that gap without a camera we need frames
whose hand is at a **known place in the frame, at a known size**, so the
expected normalised position can be computed independently and compared against
what the real detector + analyzer produce.

The synthetic frames are built by compositing a hand cut out of the approved
fixture (`woman_hands.png`, Apache-2.0, see its README) at a commanded scale and
offset. That keeps the pixels **real hand pixels** - the palm detector and the
landmark model both find it - while letting us dial the position exactly.

Ground truth is expressed in the SAME normalised logical frame as `HandState`
(spec 17), so the comparison is apples to apples:

    x  = hand box centre x / frame width        (0 = left edge)
    z  = (frame height - hand box centre y) / frame height   (0 = bottom edge)
    y  = sqrt(box width * box height) / frame height         (`depth_source=size`)

Because the *expected* value is derived from the composite's own bookkeeping and
the *actual* value comes from the detector, a mismatch localises to the detector
or the mapper - not to this script.

The generated sequence also carries a scripted gesture and a presence plan, so
one run exercises the whole `RobotLink` policy in order:

    frame 0..2   open palm, centre          -> reference latches
    frame 3..N   open palm, moving +x       -> a stream of movexyz
    frame N+1    fist                       -> one gripper close
    frame N+2..  hand ABSENT                -> nothing is commanded
    last         open palm, reappears       -> one home ("hand returned")

Two output modes
----------------
* `--frames <dir>`   write the PNGs, for feeding a real pipeline / a human
* `--expect <json>`  write the ground-truth JSON, for the assertion side

Both are produced in one run so they cannot drift apart.

Usage
-----
    python tests/golden/tools/make_synthetic_hand_sequence.py \\
        --fixture tests/golden/woman_hands.png \\
        --frames  /tmp/synth --expect /tmp/synth/expected.json

The fixture's hand region is located with the SAME reference implementation the
golden uses (`make_golden_hand.py`), so "which pixels are the hand" is not a
second, hand-rolled guess.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

import cv2 as cv
import numpy as np

# The reference implementation lives next to this file; import it rather than
# re-deriving the palm/landmark decode (that would be a second source of truth).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_golden_hand as ref  # noqa: E402


# The sequence is deliberately scripted to exercise the whole RobotLink policy
# in order. `gesture` drives the discrete edges; `present=False` is an empty
# frame and drives the presence/home rule.
#
# KNOWN LIMITATION - why there is no fist frame
# ---------------------------------------------
# The fist step is NOT generated as an image. A fist that the MediaPipe palm
# detector accepts could not be produced synthetically here: a drawn fist scores
# 0 palms (the detector wants real hand texture), and a crop of the fixture's own
# palm region is found by the palm model but then REJECTED by the landmark model
# (confidence < 0.5), so no HandState is produced either way.
#
# The fist -> gripper edge is therefore covered where it can be covered
# honestly: `tests/hand_analyzer_tests.cpp :: hand_gesture_real_capture_*` pins
# the classifier on a REAL captured fist (frozen as a landmark table), and
# `tests/robot_link_tests.cpp :: robot_fist_controls_gripper` pins the policy
# that turns Fist into one `gripper close`. This script's `fist_injected` step
# is marked with `"injected": true` so the C++ side knows to feed it a landmark
# table instead of loading a PNG.
STEPS = [
    # name            gesture      present  dx     dy     scale
    ("settle_0",      "open_palm", True,  0.35,  0.55, 0.42),
    ("settle_1",      "open_palm", True,  0.35,  0.55, 0.42),
    ("settle_2",      "open_palm", True,  0.35,  0.55, 0.42),
    ("right_1",       "open_palm", True,  0.42,  0.55, 0.42),
    ("right_2",       "open_palm", True,  0.49,  0.55, 0.42),
    ("right_3",       "open_palm", True,  0.56,  0.55, 0.42),
    ("closer_1",      "open_palm", True,  0.56,  0.55, 0.52),
    # The gesture debounce (HandConfig::gesture_confirm_frames = 5) needs FIVE
    # consecutive frames before a gesture is confirmed, so one injected fist
    # frame would be absorbed as a candidate and never reach RobotLink. Repeat it
    # past the gate.
    ("fist_inject_0", "fist",      False, 0.0,   0.0,  0.0),   # landmark table
    ("fist_inject_1", "fist",      False, 0.0,   0.0,  0.0),
    ("fist_inject_2", "fist",      False, 0.0,   0.0,  0.0),
    ("fist_inject_3", "fist",      False, 0.0,   0.0,  0.0),
    ("fist_inject_4", "fist",      False, 0.0,   0.0,  0.0),
    ("fist_inject_5", "fist",      False, 0.0,   0.0,  0.0),
    ("gone_0",        None,        False, 0.0,   0.0,  0.0),
    ("gone_1",        None,        False, 0.0,   0.0,  0.0),
    ("gone_2",        None,        False, 0.0,   0.0,  0.0),
    ("back_home",     "open_palm", True,  0.50,  0.50, 0.42),
]

# Steps whose "present" is realised by injecting landmarks, not by compositing.
INJECTED = {"fist_inject_0", "fist_inject_1", "fist_inject_2",
            "fist_inject_3", "fist_inject_4", "fist_inject_5"}

# HandAnalyzer smooths the mapped position with an EMA (filter_alpha = 0.4), so a
# frame that MOVES reports a position still catching up with where the hand
# actually is. Comparing those frames against the raw composite geometry would
# be measuring the filter's lag, not the mapping's accuracy.
#
# "Settled" is therefore defined geometrically, from the sequence itself: a
# frame counts as settled when it and the two frames before it share the same
# placement, i.e. the hand has been held still for three frames - long enough
# for the EMA to converge (residual 0.6^3 = 0.216 of the last step, and that
# step is zero). Only settled frames are asserted against the strict tolerance;
# moving frames are allowed the documented lag.
SETTLE_RUN = 3

# Default output size. The fixture is 640x960 (portrait); a 640x480 landscape
# frame is closer to what the agent's capture path produces and leaves room to
# move the hand horizontally, which this sequence needs.
FRAME_W, FRAME_H = 640, 480


def locate_fixture_hand(fixture_path: str, model_dir: str):
    """Return (image, hand_box, landmark_xy) for the fixture's top-scoring hand.

    Reuses the reference palm + landmark decode, so "where is the hand" comes
    from the same code the golden numbers do.
    """
    image = cv.imread(fixture_path)
    if image is None:
        raise SystemExit(f"cannot read fixture {fixture_path}")
    ref.MODEL_DIR = model_dir
    palms, _, _ = ref.palm_infer(image)
    if len(palms) == 0:
        raise SystemExit("the fixture yielded no palm - is it the right image?")
    hand = ref.landmark_infer(image, palms[0])
    if hand is None:
        raise SystemExit("the fixture's palm was rejected by the landmark model")
    box = np.array(hand["bbox"], dtype=np.float64)          # x1,y1,x2,y2
    lm = np.array(hand["landmarks"], dtype=np.float64)      # 21 x [x,y,z]
    return image, box, lm


def cut_hand(image: np.ndarray, box: np.ndarray):
    """Cut the hand out with a small margin. Returns (patch, margin_rect).

    `margin_rect` is where the cut sits in the ORIGINAL image, so the patch's
    ink box can be mapped back into fixture coordinates - needed to calibrate the
    model/ink relationship without mixing two coordinate systems.
    """
    h, w = image.shape[:2]
    x1, y1, x2, y2 = box
    mx, my = (x2 - x1) * 0.06, (y2 - y1) * 0.06
    x1 = int(max(0, math.floor(x1 - mx)))
    y1 = int(max(0, math.floor(y1 - my)))
    x2 = int(min(w, math.ceil(x2 + mx)))
    y2 = int(min(h, math.ceil(y2 + my)))
    return image[y1:y2, x1:x2].copy(), (x1, y1, x2, y2)


def draw_fist(w: int, h: int) -> np.ndarray:
    """A drawn fist. KEPT FOR REFERENCE ONLY - see the KNOWN LIMITATION note.

    This does not work as a test asset: the MediaPipe palm detector returns
    0 palms for it, because it wants real hand texture rather than a flat
    cartoon. It is left here so the next person does not spend the same hour
    rediscovering that, and so the negative result is reproducible:

        python tests/golden/tools/make_synthetic_hand_sequence.py --draw-fist-check
    """
    img = np.full((h, w, 3), 40, dtype=np.uint8)
    skin = (140, 165, 205)      # BGR, a plausible skin tone
    dark = (95, 115, 150)
    cv.rectangle(img, (int(w * 0.12), int(h * 0.34)),
                 (int(w * 0.88), int(h * 0.94)), skin, -1)
    for i in range(4):
        cx = int(w * (0.24 + 0.175 * i))
        cv.ellipse(img, (cx, int(h * 0.36)), (int(w * 0.075), int(h * 0.085)),
                   0, 0, 360, skin, -1)
    for i in range(1, 4):
        cx = int(w * (0.24 + 0.175 * i - 0.0875))
        cv.line(img, (cx, int(h * 0.34)), (cx, int(h * 0.62)), dark, 2)
    cv.ellipse(img, (int(w * 0.22), int(h * 0.62)),
               (int(w * 0.09), int(h * 0.16)), 20, 0, 360, skin, -1)
    return img


def place(patch: np.ndarray, fw: int, fh: int, dx: float, dy: float, scale: float):
    """Scale `patch` so the hand inside it is `scale` of the frame height, then
    translate it so the hand's centre lands on (dx, dy) in frame fractions.

    Returns (frame, actual_box_px). The box is measured on the COMPOSED frame
    (the non-black extent), not recomputed from the resize algebra: the algebra
    has to track the crop margin, the scale factor and the frame clipping at
    once, and getting any one of them subtly wrong silently shifts the ground
    truth the detector is then judged against. Measuring what was actually drawn
    cannot drift.
    """
    ph, pw = patch.shape[:2]
    ys, xs = np.where(patch.any(axis=2))
    if len(xs) == 0:
        raise SystemExit("empty hand patch")
    hx1, hx2, hy1, hy2 = int(xs.min()), int(xs.max()), int(ys.min()), int(ys.max())

    # Scale so the hand's own height becomes `scale` of the frame.
    want_h = max(8, int(fh * scale))
    k = want_h / (hy2 - hy1 + 1)
    nw, nh = max(1, int(round(pw * k))), max(1, int(round(ph * k)))
    interp = cv.INTER_AREA if k < 1 else cv.INTER_LINEAR
    resized = cv.resize(patch, (nw, nh), interpolation=interp)

    # Where the hand sits inside the resized patch, and the offset that puts its
    # centre on target.
    cx0 = (hx1 + hx2) * 0.5 * (nw / pw)
    cy0 = (hy1 + hy2) * 0.5 * (nh / ph)
    tcx, tcy = dx * fw, dy * fh
    off_x, off_y = int(round(tcx - cx0)), int(round(tcy - cy0))

    out = np.zeros((fh, fw, 3), dtype=np.uint8)
    sx1, sy1 = max(0, -off_x), max(0, -off_y)
    dx1, dy1 = max(0, off_x), max(0, off_y)
    cw, ch = min(nw - sx1, fw - dx1), min(nh - sy1, fh - dy1)
    if cw <= 0 or ch <= 0:
        raise SystemExit(f"hand placed entirely off-frame at ({dx},{dy})")
    out[dy1:dy1 + ch, dx1:dx1 + cw] = resized[sy1:sy1 + ch, sx1:sx1 + cw]

    # Ground truth = the non-black extent of what was just drawn.
    nz = np.where(out.any(axis=2))
    v_t, v_b, v_l, v_r = int(nz[0].min()), int(nz[0].max()), int(nz[1].min()), int(nz[1].max())
    return out, (float(v_l), float(v_t), float(v_r + 1), float(v_b + 1))


def expected_logical(box, fw: int, fh: int, invert=(False, False, False)):
    """Box (pixels) -> the normalised logical position `HandState` should carry.

    Mirrors `hand_map_coordinates()` in src/ai/hand_analyzer.cpp exactly; if the
    two ever disagree, this script is what tells us.
    """
    x1, y1, x2, y2 = box
    cx, cy = (x1 + x2) * 0.5, (y1 + y2) * 0.5
    bw, bh = x2 - x1, y2 - y1
    x = cx / fw
    z = (fh - cy) / fh
    y = math.sqrt(bw * bh) / fh if bw > 0 and bh > 0 else 0.0
    if invert[0]:
        x = -x
    if invert[1]:
        y = -y
    if invert[2]:
        z = -z
    return {"x": x, "y": y, "z": z}


# ---------------------------------------------------------------------------
# Predicting the DETECTED box, not the drawn ink
# ---------------------------------------------------------------------------
# The composite's ink rectangle and the model's reported hand box are two
# different shapes: the ink includes the cut's margin, while the model's box is
# a hull over the 21 predicted landmarks (see hand_detector.cpp). Comparing them
# directly bakes a constant offset into the tolerance and hides real errors.
#
# So the prediction is calibrated once, on the fixture itself: run the model on
# the raw fixture, and record how its box relates to the ink box of the same
# hand. It measures 0.92x in width and 0.98x in height, offset slightly - a
# stable per-model property, not a per-frame accident. Each synthesised frame's
# predicted box is then the fixture's model box put through the SAME scale and
# translation that the compositing applied.
def measure_box_ratio(fixture_model_box, fixture_ink_box):
    """Return (kw, kh, dcx, dcy): model box = ink box scaled/offset by these."""
    mx1, my1, mx2, my2 = fixture_model_box
    ix1, iy1, ix2, iy2 = fixture_ink_box
    mw, mh = mx2 - mx1, my2 - my1
    iw, ih = ix2 - ix1, iy2 - iy1
    kw = mw / iw if iw > 0 else 1.0
    kh = mh / ih if ih > 0 else 1.0
    dcx = ((mx1 + mx2) * 0.5) - ((ix1 + ix2) * 0.5)
    dcy = ((my1 + my2) * 0.5) - ((iy1 + iy2) * 0.5)
    return kw, kh, dcx, dcy


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fixture", default="tests/golden/woman_hands.png")
    ap.add_argument("--models", default="models")
    ap.add_argument("--frames", help="directory to write the PNG sequence into")
    ap.add_argument("--expect", help="JSON file to write the ground truth into")
    ap.add_argument("--width", type=int, default=FRAME_W)
    ap.add_argument("--height", type=int, default=FRAME_H)
    ap.add_argument("--draw-fist-check", action="store_true",
                    help="write the drawn-fist probe to build-msvc/_fist_probe.png "
                         "and exit (documents why no fist is composited)")
    args = ap.parse_args()

    if args.draw_fist_check:
        probe = draw_fist(args.width, args.height)
        out = "build-msvc/_fist_probe.png"
        os.makedirs(os.path.dirname(out), exist_ok=True)
        cv.imwrite(out, probe)
        print(f"wrote {out} - run make_golden_hand.py on it; expect 'palms after "
              f"NMS : 0', which is why the sequence injects a fist instead")
        return 0

    image, box, lm = locate_fixture_hand(args.fixture, args.models)
    print(f"fixture hand box = {np.round(box, 2).tolist()}")

    patch_src, cut_rect = cut_hand(image, box)

    # Calibrate in FIXTURE coordinates: the model's box for the fixture hand, and
    # the ink box of the same hand in the un-cropped fixture. Using the patch here
    # would compare the model's box (fixture coords) with the patch's ink (patch
    # coords) and invent a ~220 px offset out of nothing.
    ink_fx1, ink_fy1, ink_fx2, ink_fy2 = cut_rect
    ink0 = (float(ink_fx1), float(ink_fy1), float(ink_fx2), float(ink_fy2))
    kw, kh, dcx, dcy = measure_box_ratio(box, ink0)
    print(f"fixture ink box = {[round(v,1) for v in ink0]}")
    print(f"model/ink box ratio = {kw:.4f} w, {kh:.4f} h, "
          f"offset ({dcx:+.1f},{dcy:+.1f}) px")
    # Patch-local ink extent, used to relate each composite to the fixture ink.
    _ys, _xs = np.where(patch_src.any(axis=2))
    patch_ink = (float(_xs.min()), float(_ys.min()),
                 float(_xs.max() + 1), float(_ys.max() + 1))
    patch_kw = (ink0[2] - ink0[0]) / (patch_ink[2] - patch_ink[0])
    patch_kh = (ink0[3] - ink0[1]) / (patch_ink[3] - patch_ink[1])

    if args.frames:
        os.makedirs(args.frames, exist_ok=True)

    plan = {"frame_width": args.width, "frame_height": args.height,
            "fixture": args.fixture, "frames": []}

    def placement(step):
        """The static placement of a composited step, for the settle test."""
        _, _, present, dx, dy, scale = step
        return (present, round(dx, 6), round(dy, 6), round(scale, 6))

    for i, (name, gesture, present, dx, dy, scale) in enumerate(STEPS):
        # Settled = this frame and the two before it share a placement, so the
        # EMA has had three identical inputs to converge on.
        settled = (i >= SETTLE_RUN - 1 and
                   all(placement(STEPS[j]) == placement(STEPS[i])
                       for j in range(i - SETTLE_RUN + 1, i + 1)))
        if name in INJECTED:
            # Delivered as a landmark table by the C++ side, not as pixels.
            entry = {"index": i, "name": name, "present": True,
                     "injected": True, "gesture": gesture, "file": None,
                     "box": None, "expected": None, "settled": settled}
        elif not present:
            frame = np.zeros((args.height, args.width, 3), dtype=np.uint8)
            entry = {"index": i, "name": name, "present": False,
                     "gesture": "none", "file": None, "expected": None,
                     "settled": settled}
        else:
            frame, ink = place(patch_src, args.width, args.height, dx, dy, scale)
            # Predicted DETECTED box. The fixture's own hand is the reference
            # (its model box and its ink box were measured above); this composite
            # is that same hand scaled and translated, so the model box
            # transforms with it.
            s_w = (ink[2] - ink[0]) / (patch_ink[2] - patch_ink[0])
            s_h = (ink[3] - ink[1]) / (patch_ink[3] - patch_ink[1])
            ink_cx = (ink[0] + ink[2]) * 0.5
            ink_cy = (ink[1] + ink[3]) * 0.5
            # The fixture-relative model offset, carried through the composite's
            # own scale (patch_k* maps patch coords back to fixture coords).
            m_cx = ink_cx + (dcx * patch_kw) * s_w
            m_cy = ink_cy + (dcy * patch_kh) * s_h
            mw = (ink[2] - ink[0]) * kw
            mh = (ink[3] - ink[1]) * kh
            model_box = (m_cx - mw / 2, m_cy - mh / 2,
                         m_cx + mw / 2, m_cy + mh / 2)
            exp = expected_logical(model_box, args.width, args.height)
            entry = {"index": i, "name": name, "present": True,
                     "injected": False, "gesture": gesture,
                     "file": f"f_{i:02d}.png",
                     "ink_box": [round(v, 3) for v in ink],
                     "box": [round(v, 3) for v in model_box],
                     "expected": {k: round(v, 6) for k, v in exp.items()},
                     "settled": settled}
        if args.frames and entry.get("file"):
            cv.imwrite(os.path.join(args.frames, entry["file"]), frame)
        plan["frames"].append(entry)

    if args.expect:
        os.makedirs(os.path.dirname(os.path.abspath(args.expect)), exist_ok=True)
        with open(args.expect, "w", encoding="utf-8") as f:
            json.dump(plan, f, indent=1)

    print(f"wrote {len(STEPS)} frames"
          + (f" -> {args.frames}" if args.frames else " (no --frames: not written)")
          + (f"; ground truth -> {args.expect}" if args.expect else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
