#pragma once

// Multi-object tracker abstraction (spec 20).
//
//   ITracker
//     +-- ByteTrackTracker  (PC + RK3568; ByteTrack is pure arithmetic)
//
// Only ByteTrack is used in phase 1: DeepSORT / ReID / BoT-SORT are explicitly
// out of scope (spec 11).

#include <memory>
#include <vector>

#include "camera_agent/ai/detector.h"
#include "camera_agent/ai/keypoint.h"

namespace ca {

struct TrackedObject {
    float       x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
    int         track_id   = -1;
    float       confidence = 0.0f;
    int         class_id   = 0;
    std::string class_name = "person";
    // Pass-through of the matched detection's keypoints (pose model only).
    std::vector<Keypoint> keypoints;
};

struct TrackerConfig {
    // Detections at/above this score may create a new track and are associated
    // first. Lower-scored ones can only refresh an existing track.
    float high_threshold  = 0.5f;
    float match_threshold = 0.8f;  // IoU gate, first association
    int   track_buffer    = 30;    // lost track lifetime, in "30fps frames"
    int   frame_rate      = 5;     // AI frame rate; converts track_buffer -> frames
};

class ITracker {
public:
    virtual ~ITracker() = default;
    virtual void configure(const TrackerConfig& cfg) = 0;
    virtual std::vector<TrackedObject> update(const std::vector<Detection>& dets) = 0;
};

std::unique_ptr<ITracker> create_tracker();

} // namespace ca
