"""Regenerate the hand-branch golden values (tests/golden/README.md).

Implements the official opencv_zoo algorithm (mp_palmdet.py + mp_handpose.py)
EXACTLY, but runs inference through ONNX Runtime - the same runtime the C++ uses -
so that a numeric comparison against the C++ output isolates porting mistakes
from any cv.dnn vs ORT difference.

Prerequisites:
    pip install opencv-python onnxruntime numpy
    the two ONNX files from carmera-agent/models/ next to this invocation

Usage:
    make_golden_hand.py <fixture.png> <out.json> [model_dir]

Then transcribe the printed landmarks into tests/hand_detector_tests.cpp.
The fixture must be LOSSLESS (.png): stb_image and OpenCV's JPEG decoder differ
by up to 3 LSB on ~5% of pixels, which alone moves landmarks by ~0.4 px and
would hide the sub-pixel bugs this golden exists to catch.

Known trap: the reference's `_preprocess` assigns `pad_bias[0] = left` and
`pad_bias[1] = top` INSIDE the resize branch. Dropping those two lines shifts
every decoded palm box by +pad_x and cost a full debugging round - the C++ was
correct and the golden was wrong. Keep them.
"""
import json
import os
import sys

import cv2 as cv
import numpy as np
import onnxruntime as ort

MODELS = {
    "palm": "palm_detection_mediapipe_2023feb.onnx",
    "landmark": "handpose_estimation_mediapipe_2023feb.onnx",
}
MODEL_DIR = "."

# --- constants copied verbatim from the reference implementation ------------
PALM_INPUT = 192
LANDMARK_INPUT = 224
PALM_KEYPOINTS = 7
PALM_BOX_PRE_SHIFT_VECTOR = [0, 0]
PALM_BOX_PRE_ENLARGE_FACTOR = 4
PALM_BOX_SHIFT_VECTOR = [0, -0.4]
PALM_BOX_ENLARGE_FACTOR = 3
HAND_BOX_SHIFT_VECTOR = [0, -0.1]
HAND_BOX_ENLARGE_FACTOR = 1.65
PALM_LANDMARKS_INDEX_OF_PALM_BASE = 0
PALM_LANDMARKS_INDEX_OF_MIDDLE_FINGER_BASE = 2
SCORE_THRESHOLD = 0.5
NMS_THRESHOLD = 0.3
TOPK = 5000
LANDMARK_CONF_THRESHOLD = 0.5


def build_anchors():
    """Same recipe as the C++ (verified against the official literal)."""
    out = []
    for stride, per_cell in ((8, 2), (16, 6)):
        grid = PALM_INPUT // stride
        for y in range(grid):
            for x in range(grid):
                cx = (x + 0.5) / grid
                cy = (y + 0.5) / grid
                for _ in range(per_cell):
                    out.append((cx, cy))
    return np.array(out, dtype=np.float64)


def sess(name):
    # Single thread so the C++ (num_threads config) and this agree bit-for-bit
    # more often than not; a few ULPs of difference here are irrelevant anyway.
    so = ort.SessionOptions()
    so.intra_op_num_threads = 1
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    return ort.InferenceSession(os.path.join(MODEL_DIR, MODELS[name]), so,
                               providers=["CPUExecutionProvider"])


def run(s, blob):
    """Feed NHWC float32 and return outputs keyed by position."""
    inp = s.get_inputs()[0]
    outs = s.run(None, {inp.name: blob.astype(np.float32)})
    return outs, [o.name for o in s.get_outputs()], inp


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))


def finger_ratios(lm):
    """|tip - wrist| / |mcp - wrist| for index/middle/ring/pinky (same as the
    C++ hand_classify_gesture), so the threshold band stays traceable."""
    tips, mcps = (8, 12, 16, 20), (5, 9, 13, 17)
    out = []
    for t, m in zip(tips, mcps):
        d = lambda a, b: float(np.linalg.norm(np.array(lm[a]) - np.array(lm[b])))
        out.append(d(t, 0) / d(m, 0))
    return out


# ---------------------------------------------------------------------------
def palm_infer(image):
    h, w, _ = image.shape
    pad_bias = np.array([0.0, 0.0])
    ratio = min(PALM_INPUT / h, PALM_INPUT / w)
    if h != PALM_INPUT or w != PALM_INPUT:
        rw, rh = int(w * ratio), int(h * ratio)
        resized = cv.resize(image, (rw, rh))
        pad_w, pad_h = PALM_INPUT - rw, PALM_INPUT - rh
        left, top = pad_w // 2, pad_h // 2
        resized = cv.copyMakeBorder(resized, top, pad_h - top, left, pad_w - left,
                                    cv.BORDER_CONSTANT, None, (0, 0, 0))
        # The reference records the padding here; forgetting these two lines
        # leaves the whole decode offset by the letterbox width (this script
        # originally had that bug - keep them).
        pad_bias[0] = left
        pad_bias[1] = top
    else:
        resized = image
    rgb = cv.cvtColor(resized, cv.COLOR_BGR2RGB).astype(np.float32) / 255.0
    pad_bias = (pad_bias / ratio).astype(np.int32)
    blob = rgb[np.newaxis, :, :, :]

    s = sess("palm")
    outs, names, inp = run(s, blob)
    assert inp.shape == [1, 192, 192, 3], f"unexpected palm input {inp.shape}"

    # Identify reg (18 channels) vs score (1 channel) by shape, not by order.
    reg = score = None
    for o in outs:
        if o.shape[-1] == 18:
            reg = o[0]
        elif o.shape[-1] == 1:
            score = o[0, :, 0]
    assert reg is not None and score is not None, f"palm outputs {[o.shape for o in outs]}"

    anchors = build_anchors()
    assert anchors.shape[0] == reg.shape[0], f"anchor mismatch {anchors.shape} vs {reg.shape}"

    scale = max(w, h)
    score = sigmoid(score.astype(np.float64))
    cxy = reg[:, :2] / PALM_INPUT
    wh = reg[:, 2:4] / PALM_INPUT
    xy1 = (cxy - wh / 2 + anchors) * scale
    xy2 = (cxy + wh / 2 + anchors) * scale
    boxes = np.concatenate([xy1, xy2], axis=1)
    boxes -= [pad_bias[0], pad_bias[1], pad_bias[0], pad_bias[1]]

    lms = reg[:, 4:].reshape(-1, PALM_KEYPOINTS, 2) / PALM_INPUT
    lms = lms + anchors[:, np.newaxis, :]
    lms = lms * scale
    lms -= pad_bias

    keep = cv.dnn.NMSBoxes(boxes, score.astype(np.float32),
                           SCORE_THRESHOLD, NMS_THRESHOLD, top_k=TOPK)
    keep = np.array(keep).flatten() if len(keep) else np.array([], dtype=int)
    if len(keep) == 0:
        return np.empty((0, 19)), pad_bias, np.array([w, h])

    order = keep[np.argsort(-score[keep])]
    rows = np.c_[boxes[order], lms[order].reshape(len(order), -1), score[order]]
    return rows, pad_bias, np.array([w, h])


def crop_and_pad_from_palm(image, palm_bbox, for_rotation=False):
    wh_palm_bbox = palm_bbox[1] - palm_bbox[0]
    shift_vector = (PALM_BOX_PRE_SHIFT_VECTOR if for_rotation
                    else PALM_BOX_SHIFT_VECTOR)
    palm_bbox = palm_bbox + np.array(shift_vector) * wh_palm_bbox
    center = np.sum(palm_bbox, axis=0) / 2
    wh_palm_bbox = palm_bbox[1] - palm_bbox[0]
    scale = PALM_BOX_PRE_ENLARGE_FACTOR if for_rotation else PALM_BOX_ENLARGE_FACTOR
    new_half = wh_palm_bbox * scale / 2
    palm_bbox = np.array([center - new_half, center + new_half]).astype(np.int32)
    palm_bbox[:, 0] = np.clip(palm_bbox[:, 0], 0, image.shape[1])
    palm_bbox[:, 1] = np.clip(palm_bbox[:, 1], 0, image.shape[0])
    crop = image[palm_bbox[0][1]:palm_bbox[1][1], palm_bbox[0][0]:palm_bbox[1][0], :]
    side_len = int(np.linalg.norm(crop.shape[:2])) if for_rotation else int(max(crop.shape[:2]))
    pad_h, pad_w = side_len - crop.shape[0], side_len - crop.shape[1]
    left, top = pad_w // 2, pad_h // 2
    crop = cv.copyMakeBorder(crop, top, pad_h - top, left, pad_w - left,
                             cv.BORDER_CONSTANT, None, (0, 0, 0))
    bias = palm_bbox[0] - [left, top]
    return crop, palm_bbox, bias


def landmark_infer(image, palm):
    pad_bias = np.array([0, 0], dtype=np.int32)
    palm_bbox = palm[0:4].reshape(2, 2)
    crop, palm_bbox, bias = crop_and_pad_from_palm(image, palm_bbox, True)
    crop = cv.cvtColor(crop, cv.COLOR_BGR2RGB)
    pad_bias = pad_bias + bias

    palm_bbox = palm_bbox - pad_bias
    palm_landmarks = palm[4:18].reshape(PALM_KEYPOINTS, 2) - pad_bias
    p1 = palm_landmarks[PALM_LANDMARKS_INDEX_OF_PALM_BASE]
    p2 = palm_landmarks[PALM_LANDMARKS_INDEX_OF_MIDDLE_FINGER_BASE]
    radians = np.pi / 2 - np.arctan2(-(p2[1] - p1[1]), p2[0] - p1[0])
    radians = radians - 2 * np.pi * np.floor((radians + np.pi) / (2 * np.pi))
    angle = np.rad2deg(radians)

    center = np.sum(palm_bbox, axis=0) / 2
    rotation_matrix = cv.getRotationMatrix2D(tuple(center), angle, 1.0)
    rotated_image = cv.warpAffine(crop, rotation_matrix, (crop.shape[1], crop.shape[0]))

    homogeneous = np.c_[palm_landmarks, np.ones(palm_landmarks.shape[0])]
    rotated_palm_landmarks = np.array([np.dot(homogeneous, rotation_matrix[0]),
                                       np.dot(homogeneous, rotation_matrix[1])])
    rotated_palm_bbox = np.array([np.amin(rotated_palm_landmarks, axis=1),
                                  np.amax(rotated_palm_landmarks, axis=1)])
    crop2, rotated_palm_bbox, _ = crop_and_pad_from_palm(rotated_image,
                                                         rotated_palm_bbox, False)
    blob = cv.resize(crop2, dsize=(LANDMARK_INPUT, LANDMARK_INPUT),
                     interpolation=cv.INTER_AREA).astype(np.float32) / 255.0
    blob = blob[np.newaxis, :, :, :]

    s = sess("landmark")
    outs, names, inp = run(s, blob)
    assert inp.shape == [1, 224, 224, 3], f"unexpected landmark input {inp.shape}"
    lm = conf = hand = word = None
    for o in outs:
        if o.size == 63 and lm is None:
            lm = o[0]
        elif o.size == 63:
            word = o[0]
        elif o.size == 1 and conf is None:
            conf = o[0][0]
        elif o.size == 1:
            hand = o[0][0]
    assert not any(v is None for v in (lm, conf, hand, word)), \
        f"landmark outputs {[o.shape for o in outs]}"

    if conf < LANDMARK_CONF_THRESHOLD:
        return None

    landmarks = lm.reshape(-1, 3).astype(np.float64)
    wh_rot = rotated_palm_bbox[1] - rotated_palm_bbox[0]
    scale_factor = wh_rot / LANDMARK_INPUT
    landmarks[:, :2] = (landmarks[:, :2] - LANDMARK_INPUT / 2) * max(scale_factor)
    landmarks[:, 2] = landmarks[:, 2] * max(scale_factor)

    coords_rotation = cv.getRotationMatrix2D((0, 0), angle, 1.0)
    rotated = np.dot(landmarks[:, :2], coords_rotation[:, :2])
    rotated = np.c_[rotated, landmarks[:, 2]]

    rotation_component = np.array([[rotation_matrix[0][0], rotation_matrix[1][0]],
                                   [rotation_matrix[0][1], rotation_matrix[1][1]]])
    translation = np.array([rotation_matrix[0][2], rotation_matrix[1][2]])
    inv_t = np.array([-np.dot(rotation_component[0], translation),
                      -np.dot(rotation_component[1], translation)])
    inverse_rotation_matrix = np.c_[rotation_component, inv_t]
    center_h = np.append(np.sum(rotated_palm_bbox, axis=0) / 2, 1)
    original_center = np.array([np.dot(center_h, inverse_rotation_matrix[0]),
                               np.dot(center_h, inverse_rotation_matrix[1])])
    landmarks[:, :2] = rotated[:, :2] + original_center + pad_bias

    bbox = np.array([np.amin(landmarks[:, :2], axis=0), np.amax(landmarks[:, :2], axis=0)])
    wh_bbox = bbox[1] - bbox[0]
    bbox = bbox + np.array(HAND_BOX_SHIFT_VECTOR) * wh_bbox
    center_b = np.sum(bbox, axis=0) / 2
    wh_bbox = bbox[1] - bbox[0]
    new_half = wh_bbox * HAND_BOX_ENLARGE_FACTOR / 2
    bbox = np.array([center_b - new_half, center_b + new_half])

    return {"bbox": bbox.reshape(-1).tolist(),
            "landmarks": landmarks.tolist(),
            "handedness_raw": float(hand),
            "confidence": float(conf)}


def main():
    image_path, out_path = sys.argv[1], sys.argv[2]
    global MODEL_DIR
    if len(sys.argv) > 3:
        MODEL_DIR = sys.argv[3]
    image = cv.imread(image_path)
    assert image is not None, f"cannot read {image_path}"
    h, w = image.shape[:2]
    print(f"image {w}x{h}")

    palms, pad_bias, _ = palm_infer(image)
    print(f"palms after NMS : {len(palms)}  (sorted by score desc = the C++ selection rule)")
    result = {"image": {"w": w, "h": h}, "image_path": image_path,
              "palm_pad_bias": pad_bias.tolist(),
              "anchors": len(build_anchors()), "hands": [],
              "palm_rows": [p.tolist() for p in palms]}
    for i, p in enumerate(palms):
        print(f"  palm[{i}] box={np.round(p[:4], 2).tolist()} score={p[-1]:.4f}")
        hand = landmark_infer(image, p)
        if hand is None:
            print(f"  hand[{i}] rejected by landmark confidence")
            continue
        ratios = finger_ratios(hand["landmarks"])
        print(f"  hand[{i}] conf={hand['confidence']:.4f} "
              f"handedness_raw={hand['handedness_raw']:.3f} "
              f"wrist={np.round(hand['landmarks'][0][:2], 2).tolist()} "
              f"z0={hand['landmarks'][0][2]:.2f} "
              f"tip/mcp ratios={[round(r, 4) for r in ratios]}")
        hand["palm_index"] = i
        result["hands"].append(hand)

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1)
    print(f"wrote {out_path}: {len(result['hands'])} hand(s)")


if __name__ == "__main__":
    main()
