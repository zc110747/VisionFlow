#pragma once

// One body keypoint (YOLO11n-pose, COCO 17-joint convention).
//
// Coordinates are in ORIGINAL VIDEO pixels, same convention as Detection /
// AIObject bboxes (never in the 640x640 network input space).
//
// COCO joint order (index meaning, emitted verbatim on the wire):
//   0 nose            1 left_eye        2 right_eye      3 left_ear
//   4 right_ear       5 left_shoulder   6 right_shoulder 7 left_elbow
//   8 right_elbow     9 left_wrist     10 right_wrist   11 left_hip
//  12 right_hip      13 left_knee      14 right_knee    15 left_ankle
//  16 right_ankle
//
// `conf` is the keypoint visibility/confidence in [0,1]. The pose head
// already applies the sigmoid inside the exported graph, so consumers must
// NOT sigmoid it again (same rule as the YOLO class scores).

namespace ca {

struct Keypoint {
    float x    = 0.0f;
    float y    = 0.0f;
    float conf = 0.0f;
};

} // namespace ca
