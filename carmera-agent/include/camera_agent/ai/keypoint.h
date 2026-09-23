#pragma once

// One model keypoint.
//
// Coordinates are in ORIGINAL VIDEO pixels, same convention as Detection /
// AIObject bboxes (never in the 640x640 network input space).
//
// x / y / conf are shared by every keypoint-producing model:
//
//   * body pose (YOLO11n-pose) - COCO 17-joint convention, emitted in this
//     order: 0 nose, 1 left_eye, 2 right_eye, 3 left_ear, 4 right_ear,
//     5 left_shoulder, 6 right_shoulder, 7 left_elbow, 8 right_elbow,
//     9 left_wrist, 10 right_wrist, 11 left_hip, 12 right_hip, 13 left_knee,
//     14 right_knee, 15 left_ankle, 16 right_ankle. `z` is UNUSED and stays 0.
//
//   * hand (MediaPipe hand landmark) - 21 points, emitted in this order:
//     0 wrist, then 4 per finger: 1-4 thumb (CMC/MCP/IP/TIP),
//     5-8 index, 9-12 middle, 13-16 ring, 17-20 pinky (each MCP/PIP/DIP/TIP).
//     Here `z` IS meaningful: the model's depth relative to the WRIST, already
//     rescaled into the same units as x/y. It describes the hand's ORIENTATION
//     (fingers toward / away from the camera), NOT how far the hand is from the
//     camera - see hand_types.h `depth_source`. Never treat it as centimetres.
//
// `conf` is the keypoint visibility/confidence in [0,1]. The pose head already
// applies the sigmoid inside the exported graph, so consumers must NOT sigmoid
// it again (same rule as the YOLO class scores). Hand landmarks carry a single
// per-hand confidence instead, so their per-point `conf` mirrors it.

namespace ca {

struct Keypoint {
    float x    = 0.0f;
    float y    = 0.0f;
    float z    = 0.0f;   // 0 with a body-pose model; wrist-relative depth for hands
    float conf = 0.0f;
};

} // namespace ca
