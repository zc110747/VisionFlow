#include "camera_agent/stream_controller.h"
#include "test_harness.h"

#include <chrono>
#include <thread>

// Reconnect must auto-resume after a link drop (immediate backoff for the test).
TEST(reconnect_auto_resume) {
    ca::Config cfg = ca::make_default_config();
    ca::StreamController ctrl(cfg);
    ctrl.set_reconnect_schedule({0, 0, 0, 0});
    ASSERT(ctrl.start());
    ASSERT(ctrl.get_status() == ca::StreamStatus::STREAMING);

    ctrl.simulate_link_lost();
    bool resumed = false;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (ctrl.get_status() == ca::StreamStatus::STREAMING) { resumed = true; break; }
    }
    ASSERT(resumed); // auto-recovered without exiting

    ctrl.stop();
    ASSERT(ctrl.get_status() == ca::StreamStatus::DISCONNECTED);
    return true;
}

// The agent must NOT exit when the server goes down; it keeps retrying.
TEST(reconnect_no_exit_on_down) {
    ca::Config cfg = ca::make_default_config();
    ca::StreamController ctrl(cfg);
    ctrl.set_reconnect_schedule({0, 0, 0, 0});
    ASSERT(ctrl.start());
    ctrl.simulate_link_lost();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // Still alive (reconnecting or already resumed), not crashed.
    ASSERT(ctrl.get_status() == ca::StreamStatus::STREAMING ||
           ctrl.get_status() == ca::StreamStatus::DISCONNECTED);

    ctrl.stop();
    return true;
}
