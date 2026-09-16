#include "camera_agent/video_pipeline.h"
#include "test_harness.h"

#include <chrono>
#include <thread>

TEST(pipeline_build_and_run) {
    auto p = ca::VideoPipeline::create();
    ASSERT(p->check_plugins(nullptr));

    ca::PipelineParams pp;
    pp.width = 1280; pp.height = 720; pp.fps = 30; pp.bitrate = 4000;
    ASSERT(p->build(pp, "rtsp://127.0.0.1:8554/camera01"));
    ASSERT(p->start());
    ASSERT(p->is_running());
    ASSERT(p->get_status() == ca::StreamStatus::STREAMING);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT(p->get_stats().frames > 0);

    p->stop();
    ASSERT(p->get_status() == ca::StreamStatus::DISCONNECTED);
    return true;
}

TEST(pipeline_invalid_params) {
    auto p = ca::VideoPipeline::create();
    ca::PipelineParams bad;
    bad.width = 0; bad.height = 720; bad.fps = 30; // invalid -> must fail
    ASSERT(!p->build(bad, "rtsp://127.0.0.1:8554/x"));
    return true;
}
