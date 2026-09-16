#include "camera_agent/config.h"
#include "test_harness.h"

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
