#include "camera_agent/camera_manager.h"
#include "camera_agent/logger.h"

#include <memory>

namespace ca {

// SIM backend: no real hardware. Provides a single synthetic camera so the
// control flow (enumeration, selection, CLI) can be exercised headlessly.
class SimCameraManager : public CameraManager {
public:
    std::vector<CameraInfo> enumerate() override {
        CameraInfo cam;
        cam.id = 0;
        cam.name = "SIM Camera";
        cam.resolutions = {{640, 480}, {1280, 720}, {1920, 1080}};
        cam.fps = {15, 30};
        return {cam};
    }

    bool is_available(int id) const override {
        return id == 0;
    }

    std::string backend_name() const override {
        return "sim";
    }
};

std::unique_ptr<CameraManager> CameraManager::create() {
    return std::make_unique<SimCameraManager>();
}

} // namespace ca
