#include "camera_agent/rtsp_publisher.h"
#include "test_harness.h"

TEST(rtsp_url_construction) {
    auto p = ca::RtspPublisher::create();
    ASSERT_EQ(p->build_url({"127.0.0.1", 8554, "camera01"}),
              "rtsp://127.0.0.1:8554/camera01");
    ASSERT_EQ(p->build_url({"192.168.1.100", 8554, "cam1"}),
              "rtsp://192.168.1.100:8554/cam1");
    return true;
}

TEST(rtsp_connect_disconnect) {
    auto p = ca::RtspPublisher::create();
    ASSERT(p->connect("rtsp://127.0.0.1:8554/camera01"));
    ASSERT(p->is_connected());
    p->disconnect();
    ASSERT(!p->is_connected());
    return true;
}
