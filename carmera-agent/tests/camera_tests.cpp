#include "camera_agent/camera_manager.h"
#include "test_harness.h"

#include <string>

TEST(camera_enumeration) {
    auto mgr = ca::CameraManager::create();
    auto cams = mgr->enumerate();
    ASSERT(cams.size() >= 1);

    bool found0 = false;
    for (const auto& c : cams) {
        if (c.id != 0) continue;
        found0 = true;
        ASSERT(c.name.find("SIM") != std::string::npos);

        bool has720 = false, has30 = false;
        for (const auto& r : c.resolutions)
            if (r.width == 1280 && r.height == 720) has720 = true;
        for (int f : c.fps)
            if (f == 30) has30 = true;
        ASSERT(has720);
        ASSERT(has30);
    }
    ASSERT(found0);
    return true;
}
