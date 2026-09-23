# Hand-branch golden fixtures

## Contents

| file | what |
|---|---|
| `woman_hands.png` | the fixture both sides run on |
| `palm_anchors_reference.txt` | all 2016 anchor rows, transcribed verbatim from the official literal |
| `tools/make_golden_hand.py` | regenerates the landmark/box/confidence golden |
| `tools/make_palm_anchor_reference.py` | regenerates the anchor table file from `mp_palmdet.py` |

## `woman_hands.png`

The official MediaPipe / opencv_zoo hand-pose sample portrait, re-encoded
losslessly so that every decoder produces byte-identical pixels.

| | |
|---|---|
| size | 640 x 960 |
| SHA-256 | `e2665d70f4825765a521d12bcfb6d1f5053755086feaabee694de4992feae661` |
| source | `woman_hands.jpg`, SHA-256 `70cbeb38e198c9862202e0979c21a99b40ca980d3e7b250176c85b1636a40f12` (matches the published upstream value, so the source is the genuine asset and not a re-encode) |
| derivation | `cv.imwrite("woman_hands.png", cv.imread("woman_hands.jpg"))` |

### Why not just ship the JPEG

Because the JPEG decoder is *not* part of the algorithm under test, and two
decoders do not agree. Measured on this very image:

| | stb_image (what the agent uses) vs libjpeg-turbo (what the reference uses) |
|---|---|
| identical pixels | 584 177 / 614 400 |
| max abs difference | 3 / 255 |
| pixels differing by > 1 | 16 919 |

Feeding the reference and the port *different pixels* moves the 21 landmarks by
up to ~0.43 px, which is 1000x larger than the residual this fixture is meant to
detect - it would hide exactly the class of sub-pixel transform bug the test
exists to catch.

With the PNG both decoders are lossless and byte-identical (verified: max pixel
difference 0), so any remaining difference comes only from this port's own
resamplers.

## How the golden numbers in `tests/hand_detector_tests.cpp` were produced

The expected values are hard-coded in that file (rather than parsed from JSON) so
the assertion is readable in review and needs no parser in the test binary. They
come from a faithful transcription of the upstream algorithm - a copy of
`mp_palmdet.py` / `mp_handpose.py` with `cv.dnn` swapped for ONNX Runtime, the
same runtime the C++ uses - run once on this image:

1. palm model, aspect-preserving resize + **black** pad to 192x192, NHWC float32 / 255
2. `pad_bias = (left, top) / ratio` = `(160, 0)` for this image
3. decode all 2016 anchors against the 2-layer anchor table
4. score gate 0.5, NMS 0.3 -> 2 palms survive
5. sort by score descending (the same rule `max_hands = 1` implements)
6. rotate/crop the winner to 224x224 (`_cropAndPadFromPalm`, INTER_AREA)
7. landmark model -> 21 x (x, y, z) + handedness + confidence
8. inverse transforms back to **original image pixels**

> **Trap that cost real debugging time:** the reference's `_preprocess` assigns
> `pad_bias[0] = left` and `pad_bias[1] = top` *inside* the resize branch. An
> earlier version of this golden script kept `pad_bias` at `[0, 0]`, which
> shifted every decoded palm box by +160 px in x. The C++ was correct all along;
> the golden was wrong. Keep those two lines.

Recorded output - the fixture contains two hands, and `max_hands = 1` keeps the
first:

| quantity | palm[0] (selected) | palm[1] (not used) |
|---|---|---|
| palm anchor index | 456 | 785 |
| palm score | 0.845936 | 0.795090 |
| palm box | `297.25, 343.87, 395.01, 441.61` | `118.42, 608.72, 262.57, 752.71` |
| landmark confidence | 0.990193 | 0.994586 |
| handedness raw | 0.883037 (`right`) | 0.079 (would be `left`) |
| wrist landmark z | `+1.83e-06` | `~0` |
| hand box | `237.34, 283.22, 525.23, 462.63` | - |
| tip/mcp ratios | 1.5927 / 1.7069 / 1.6751 / 1.5901 | 1.7354 / 1.8355 / 1.7877 / 1.6845 |

Two facts in that table are load-bearing for the wider design, which is why the
fixture earns its place in the repo:

* **The wrist's z is ~0 regardless of the hand's distance** (`+1.8e-06` here, on a
  hand that fills a third of the frame). `landmark.z` is *wrist-relative* and
  therefore describes hand **orientation**, not how far away the hand is. This is
  the measured basis for `HandConfig::depth_source` defaulting to `"size"`
  instead of `"landmark_z"` - see spec 15/16.
* **All four finger tip/knuckle ratios are 1.59-1.84** on genuinely open hands,
  which is why the extended threshold in `src/ai/hand_analyzer.cpp` sits at 1.15
  (far below real open-hand data) and the cascade test asserts that this fixture
  classifies as `OPEN_PALM`.

## Agreement actually achieved

Measured by `camera-agent-tests` against this fixture:

| | max difference |
|---|---|
| 21 landmark x / y / z | 0.00034 / 0.00079 / 0.00173 px |
| hand box | 0.00065 px |
| palm score (C++ `palm_nms` vs reference `cv.dnn.NMSBoxes`) | bit-identical |

Note the reference hands `cv.dnn.NMSBoxes` a `(x1,y1,x2,y2)` quad even though
that function expects `(x,y,w,h)`; `palm_nms` deliberately implements true IoU
instead. It makes no difference here (the two palms do not overlap), so this
fixture does not cover that divergence - `hand_palm_nms_suppresses_overlaps`
covers it synthetically.

The anchor table is checked the same way but in full:
`hand_palm_anchor_table_matches_official` compares **all 2016 rows** (not a
sample) against `palm_anchors_reference.txt` at a 1e-6 tolerance, because a
shifted or reordered table is the one failure mode that produces plausible-looking
boxes rather than an error.

## Licence

`woman_hands.*` is part of the MediaPipe hand-pose sample data used by
opencv_zoo, distributed under Apache-2.0. See
`carmera-agent/models/LICENSE-MediaPipe-Apache-2.0.txt`.

## Regenerating / extending

Both generators ship in `tools/` and need only `opencv-python`, `onnxruntime` and
`numpy`.

```bash
# golden landmarks / box / confidence (writes JSON, then transcribe the
# landmarks it prints into tests/hand_detector_tests.cpp)
python tests/golden/tools/make_golden_hand.py \
       tests/golden/woman_hands.png golden.json carmera-agent/models

# the 2016-row anchor table (needs the official mp_palmdet.py, see its docstring)
python tests/golden/tools/make_palm_anchor_reference.py \
       mp_palmdet.py carmera-agent/tests/golden/palm_anchors_reference.txt
```

`make_palm_anchor_reference.py` deliberately transcribes the official literal
rather than calling the formula under test - regenerating it from
`palm_build_anchors()` would make the test unable to detect a wrong formula.

When adding a second fixture, prefer a case the current one does **not** cover -
a **fist** (the curled threshold `kCurledRatio` is the one still-unvalidated
number), a hand at a different distance, or a rotated hand. A fist fixture is the
highest-value addition, because it is the only way to close the last
unmeasured constant in the gesture classifier.
