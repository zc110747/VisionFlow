#pragma once

#include <memory>
#include <string>
#include <vector>

namespace ca {

struct Resolution {
    int width = 0;
    int height = 0;
};

struct CameraInfo {
    int                  id = -1;
    std::string          name;
    std::vector<Resolution> resolutions;
    std::vector<int>     fps;
};

// Abstract camera manager. Concrete backends (GStreamer / SIM) implement it.
// The capture layer is isolated here so that, on the real RK3568 device, the
// GStreamer/Windows source can be replaced by a V4L2 source without touching
// the rest of the pipeline.
class CameraManager {
public:
    // Factory selects the backend compiled via CAMERA_AGENT_BACKEND.
    static std::unique_ptr<CameraManager> create();

    virtual ~CameraManager() = default;

    // Enumerate all available cameras with supported resolutions and FPS.
    virtual std::vector<CameraInfo> enumerate() = 0;

    // Whether a camera with the given id is present.
    virtual bool is_available(int id) const = 0;

    // Human readable backend name (e.g. "gstreamer", "sim").
    virtual std::string backend_name() const = 0;
};

} // namespace ca
