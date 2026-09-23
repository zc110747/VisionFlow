#pragma once

// Hand detector: the MediaPipe palm-detection -> hand-landmark cascade.
//
//   frame --+--> palm detector  (192x192, 2016 anchors)  -> palm box + 7 palm kpts
//           |
//           +--> rotate/crop that palm to 224x224 --> landmark model
//                                                    -> 21 x (x,y,z) + handedness
//
// Why this shape and not "just reuse IDetector":
//   * The palm -> landmark cascade needs TWO models and an image-space ROI
//     transform, so a FrameResult cannot be produced by one `detect()` call.
//   * `IDetector` stays untouched, so the existing person-detect / pose models
//     and their runtime switching are provably unaffected.
//
// It is NOT a new service and NOT a second pipeline: this class is owned by the
// same AIPipeline, on the same thread, feeding the same MetadataManager, and it
// runs only while AIMode::Hand is selected.

#include <memory>
#include <string>
#include <vector>

#include "camera_agent/ai/hand_types.h"

namespace ca {

class IHandDetector {
public:
    virtual ~IHandDetector() = default;

    // Load both models. Returns false on any failure; the caller must then keep
    // the previously loaded mode and let the video stream run (spec 19).
    // Never throws.
    virtual bool init(const HandConfig& cfg) = 0;

    // Run the cascade on a tightly packed 24-bit RGB frame of size w x h.
    // `out.detected` is false when nothing was found (not an error).
    // Returns false only when inference itself failed (the frame is skipped).
    virtual bool detect_hand(const uint8_t* rgb, int w, int h,
                             HandDetection& out) = 0;

    virtual const char* backend_name() const = 0;

    // 21 once init() succeeded, 0 otherwise - mirrors IDetector::keypoint_count
    // so the AI heartbeat can advertise the same way for every mode.
    virtual int keypoint_count() const = 0;
};

// Factory. Returns the ONNX Runtime implementation when the project was built
// with it, otherwise a null detector whose init() always fails (the AI branch
// then simply cannot select the hand mode; the video stream is unaffected).
std::unique_ptr<IHandDetector> create_hand_detector();

} // namespace ca
