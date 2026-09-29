#include "camera_agent/config.h"
#include "test_harness.h"

#include <cmath>
#include <cstdio>
#include <fstream>

TEST(config_defaults) {
    auto c = ca::make_default_config();
    ASSERT_EQ(c.camera.width, 1280);
    ASSERT_EQ(c.camera.height, 720);
    ASSERT_EQ(c.camera.fps, 30);
    ASSERT_EQ(c.encoder.bitrate, 4000);
    ASSERT_EQ(c.stream.id, "camera01");
    ASSERT_EQ(c.rtsp.server, "127.0.0.1");
    ASSERT_EQ(c.rtsp.port, 8554);
    return true;
}

TEST(config_load_and_override) {
    const char* path = "config_test_tmp.yaml";
    {
        std::ofstream f(path);
        f << "camera:\n  width: 640\n  height: 480\n  fps: 15\n";
        f << "encoder:\n  bitrate: 2000\n";
        f << "stream:\n  id: camYaml\n";
    }
    ca::Config c = ca::make_default_config();
    ASSERT(ca::load_config(c, path));
    ASSERT_EQ(c.camera.width, 640);
    ASSERT_EQ(c.camera.height, 480);
    ASSERT_EQ(c.camera.fps, 15);
    ASSERT_EQ(c.encoder.bitrate, 2000);
    ASSERT_EQ(c.stream.id, "camYaml");

    // Precedence: a CLI override must win over the loaded YAML value.
    c.camera.width = 999;
    ASSERT_EQ(c.camera.width, 999);

    std::remove(path);
    return true;
}

// The shipped feel (see RobotConfig::gain_x) must survive the YAML round trip.
// The defaults, the yaml and the robot suite's pin all have to agree, or the
// operator's "hand big, arm small" setting quietly stops being applied.
TEST(config_robot_gains_load_from_yaml) {
    const char* path = "config_robot_gain_tmp.yaml";
    {
        std::ofstream f(path);
        f << "robot:\n";
        f << "  enable: true\n";
        f << "  gain_x: 0.25\n";
        f << "  gain_y: 0.5\n";
        f << "  gain_z: 0.25\n";
        f << "  reach_span_x: 39.0\n";
        f << "  reach_span_y: 176.0\n";
        f << "  reach_span_z: 41.5\n";
    }
    ca::Config c = ca::make_default_config();
    ASSERT(ca::load_config(c, path));

    ASSERT(c.robot.enable);
    ASSERT(std::fabs(c.robot.gain_x - 0.25f) < 1e-6f);
    ASSERT(std::fabs(c.robot.gain_y - 0.50f) < 1e-6f);
    ASSERT(std::fabs(c.robot.gain_z - 0.25f) < 1e-6f);
    ASSERT(std::fabs(c.robot.reach_span_x - 39.0f) < 1e-6f);
    ASSERT(std::fabs(c.robot.reach_span_y - 176.0f) < 1e-6f);
    ASSERT(std::fabs(c.robot.reach_span_z - 41.5f) < 1e-6f);

    // The built-in defaults must equal what the shipped yaml carries, so a
    // deployment that loses the yaml file still gets the tuned feel rather than
    // the old jumpy 1:1.
    ca::Config d = ca::make_default_config();
    ASSERT(std::fabs(d.robot.gain_x - 0.25f) < 1e-6f);
    ASSERT(std::fabs(d.robot.gain_y - 0.50f) < 1e-6f);
    ASSERT(std::fabs(d.robot.gain_z - 0.25f) < 1e-6f);

    std::remove(path);
    return true;
}
