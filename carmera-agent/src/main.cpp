#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "camera_agent/types.h"
#include "camera_agent/logger.h"
#include "camera_agent/config.h"
#include "camera_agent/camera_manager.h"
#include "camera_agent/stream_controller.h"

#ifndef CAMERA_AGENT_VERSION
#define CAMERA_AGENT_VERSION "0.1.0"
#endif
#ifndef CAMERA_AGENT_BACKEND_NAME
#define CAMERA_AGENT_BACKEND_NAME "unknown"
#endif

static void print_help() {
    std::cout <<
        "Camera Agent " CAMERA_AGENT_VERSION "\n"
        "Simulates an embedded Linux camera device using a PC camera.\n\n"
        "Usage:\n"
        "  camera-agent [options]\n\n"
        "Options:\n"
        "  --list             List available cameras (id/name/resolution/fps)\n"
        "  --camera <id>      Select camera id (default 0)\n"
        "  --width <px>       Capture width (default 1280)\n"
        "  --height <px>      Capture height (default 720)\n"
        "  --fps <n>          Capture fps (default 30)\n"
        "  --auto             Auto-negotiate resolution/fps: let the camera use its\n"
        "                      native format instead of forcing --width/--height/--fps\n"
        "  --bitrate <kbps>   Encoder bitrate (default 4000)\n"
        "  --stream <id>      Stream id (default camera01)\n"
        "  --server <host>    RTSP server host (default 127.0.0.1)\n"
        "  --port <n>         RTSP server port (default 8554)\n"
        "  --device-id <id>   Device id for status reporting\n"
        "  --source <elem>    Camera source element (auto | mfvideosrc | dshowvideosrc | videotestsrc ...)\n"
        "  --config <path>    YAML config file (default: config/camera-agent.yaml if present)\n"
        "  --duration <sec>   Auto-stop after N seconds (0 = until Ctrl+C)\n"
        "  --log-level <lvl>  trace|debug|info|warn|error (default info)\n"
        "  --latency-probe    Instrument per-stage latency (capture->encode->push)\n"
        "\n"
        "AI options (person detection + tracking; independent of the video stream):\n"
        "  --ai               Enable the AI branch\n"
        "  --no-ai            Disable the AI branch\n"
        "  --ai-fps <n>       Inference rate (default 8, range 5-12). Clamped to\n"
        "                      [5,12]. Ignored when the source framerate is below\n"
        "                      --ai-full-rate-below\n"
        "  --ai-confidence <f> Detection / tracker score gate (default 0.5)\n"
        "  --ai-model <path>  ONNX model (default models/yolo11n.onnx). Detection\n"
        "                      and pose models are auto-detected; e.g.\n"
        "                      models/yolo11n-pose.onnx adds 17 keypoints per person\n"
        "  --ai-model-pose <path>  Pose model (default models/yolo11n-pose.onnx)\n"
        "  --ai-aimode-base <url>  video-server HTTP base for mode polling\n"
        "                          (default http://127.0.0.1:8081)\n"
        "  --ai-aimode-poll-ms <n> mode poll period in ms (default 2000)\n"
        "  --no-ai-aimode    Disable web-driven model switching (fixed at startup)\n"
        "  --ai-input <w> <h> Network input size (default 640 640)\n"
        "  --ai-queue <n>     Bounded frame queue depth (default 2)\n"
        "  --ai-full-rate-below <n>\n"
        "                     Source fps below this -> infer EVERY frame (default 10)\n"
        "  --ai-log-objects   Log every detected object (default: on)\n"
        "\n"
        "Metadata options (async AI result upload; never blocks video or AI):\n"
        "  --metadata             Enable metadata upload\n"
        "  --no-metadata          Disable metadata upload\n"
        "  --metadata-url <url>   Server endpoint (default http://127.0.0.1:8000/api/metadata)\n"
        "  --metadata-camera-id <id>\n"
        "                         Camera id reported to the server (default camera01)\n"
        "  --metadata-queue <n>   Bounded queue depth (default 8)\n"
        "  --metadata-timeout <ms> Per-request timeout (default 2000)\n"
        "  --metadata-heartbeat <sec>\n"
        "                         Status message period, 0 = off (default 10)\n"
        "  --metadata-log-payload Dump full JSON at debug level\n"
        "  --metadata-insecure    Relax TLS cert check (self-signed dev server ONLY)\n"
        "\n"
        "  --version          Show version\n"
        "  --help             Show this help\n";
}

static void print_env() {
    std::cout << "Camera Agent " CAMERA_AGENT_VERSION "\n";
    std::cout << "  Backend : " << CAMERA_AGENT_BACKEND_NAME << "\n";
#ifdef _WIN32
    std::cout << "  OS      : Windows\n";
#else
    std::cout << "  OS      : posix\n";
#endif
    std::cout << "  C++ std : " << __cplusplus << "\n";
}

static int cmd_list() {
    auto mgr = ca::CameraManager::create();
    auto cams = mgr->enumerate();
    if (cams.empty()) {
        std::cout << "No cameras found.\n";
        return 1;
    }
    for (const auto& c : cams) {
        std::cout << "Camera " << c.id << "\n";
        std::cout << "  Name: " << c.name << "\n";
        std::cout << "  Resolution:\n";
        for (const auto& r : c.resolutions)
            std::cout << "    " << r.width << "x" << r.height << "\n";
        std::cout << "  FPS:\n";
        for (int f : c.fps)
            std::cout << "    " << f << "\n";
    }
    return 0;
}

static bool file_exists(const std::string& p) {
    std::ifstream f(p);
    return f.good();
}

int main(int argc, char** argv) {
    // ---- 1. Parse raw CLI into an overrides structure ----
    struct Cli {
        bool list=false, version=false, help=false, latency=false, autoneg=false;
        bool camera=false, width=false, height=false, fps=false, bitrate=false;
        bool stream=false, server=false, port=false, device=false, log=false, dur=false, source=false;
        int  camera_v=0, width_v=0, height_v=0, fps_v=0, bitrate_v=0, port_v=0;
        double duration_v=0;
        std::string stream_v, server_v, device_v, log_v, config, source_v;
        // AI overrides (all optional; YAML < CLI as everywhere else)
        bool ai=false, ai_no=false, ai_fps=false, ai_conf=false, ai_model=false;
        bool ai_w=false, ai_h=false, ai_queue=false, ai_frb=false, ai_log_objects=false;
        int  ai_fps_v=0, ai_w_v=0, ai_h_v=0, ai_queue_v=0, ai_frb_v=0;
        float ai_conf_v=0;
        std::string ai_model_v;
        // Runtime AI mode switching
        bool ai_model_pose=false, ai_aimode_base=false, ai_aimode_poll_ms=false, ai_aimode_off=false;
        std::string ai_model_pose_v, ai_aimode_base_v;
        int  ai_aimode_poll_ms_v=0;
        // Metadata overrides
        bool md=false, md_no=false, md_url=false, md_cam=false, md_queue=false;
        bool md_timeout=false, md_hb=false, md_log_payload=false, md_insecure=false;
        int  md_queue_v=0, md_timeout_v=0, md_hb_v=0;
        std::string md_url_v, md_cam_v;
    } cli;

    auto get_val = [&](int i, const char* opt) -> std::string {
        if (i + 1 >= argc) { CA_LOG_ERROR("Missing value for {}", opt); return ""; }
        return argv[i + 1];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--list")                    cli.list = true;
        else if (a == "--version")                 cli.version = true;
        else if (a == "--help" || a == "-h")       cli.help = true;
        else if (a == "--log-level")   { cli.log = true;    cli.log_v = get_val(i++, "--log-level"); }
        else if (a == "--camera")     { cli.camera = true;  cli.camera_v = std::atoi(get_val(i++, "--camera").c_str()); }
        else if (a == "--width")      { cli.width = true;   cli.width_v = std::atoi(get_val(i++, "--width").c_str()); }
        else if (a == "--height")     { cli.height = true;  cli.height_v = std::atoi(get_val(i++, "--height").c_str()); }
        else if (a == "--fps")        { cli.fps = true;     cli.fps_v = std::atoi(get_val(i++, "--fps").c_str()); }
        else if (a == "--bitrate")    { cli.bitrate = true; cli.bitrate_v = std::atoi(get_val(i++, "--bitrate").c_str()); }
        else if (a == "--stream")     { cli.stream = true;  cli.stream_v = get_val(i++, "--stream"); }
        else if (a == "--server")     { cli.server = true;  cli.server_v = get_val(i++, "--server"); }
        else if (a == "--port")       { cli.port = true;    cli.port_v = std::atoi(get_val(i++, "--port").c_str()); }
        else if (a == "--device-id")  { cli.device = true;  cli.device_v = get_val(i++, "--device-id"); }
        else if (a == "--latency-probe") cli.latency = true;
        else if (a == "--auto")         cli.autoneg = true;
        else if (a == "--source")     { cli.source = true;   cli.source_v = get_val(i++, "--source"); }
        else if (a == "--config")     { cli.config = get_val(i++, "--config"); }
        else if (a == "--duration")   { cli.dur = true;     cli.duration_v = std::atof(get_val(i++, "--duration").c_str()); }
        else if (a == "--ai")                  cli.ai = true;
        else if (a == "--no-ai")               cli.ai_no = true;
        else if (a == "--ai-log-objects")      cli.ai_log_objects = true;
        else if (a == "--ai-fps")    { cli.ai_fps = true;   cli.ai_fps_v = std::atoi(get_val(i++, "--ai-fps").c_str()); }
        else if (a == "--ai-confidence") { cli.ai_conf = true;
                                          cli.ai_conf_v = (float)std::atof(get_val(i++, "--ai-confidence").c_str()); }
        else if (a == "--ai-model")  { cli.ai_model = true; cli.ai_model_v = get_val(i++, "--ai-model"); }
        else if (a == "--ai-model-pose") { cli.ai_model_pose = true;
                                           cli.ai_model_pose_v = get_val(i++, "--ai-model-pose"); }
        else if (a == "--ai-aimode-base") { cli.ai_aimode_base = true;
                                            cli.ai_aimode_base_v = get_val(i++, "--ai-aimode-base"); }
        else if (a == "--ai-aimode-poll-ms") { cli.ai_aimode_poll_ms = true;
                                               cli.ai_aimode_poll_ms_v = std::atoi(get_val(i++, "--ai-aimode-poll-ms").c_str()); }
        else if (a == "--no-ai-aimode") cli.ai_aimode_off = true;
        else if (a == "--ai-queue")  { cli.ai_queue = true; cli.ai_queue_v = std::atoi(get_val(i++, "--ai-queue").c_str()); }
        else if (a == "--ai-full-rate-below") { cli.ai_frb = true;
                                               cli.ai_frb_v = std::atoi(get_val(i++, "--ai-full-rate-below").c_str()); }
        else if (a == "--metadata")            cli.md = true;
        else if (a == "--no-metadata")         cli.md_no = true;
        else if (a == "--metadata-log-payload") cli.md_log_payload = true;
        else if (a == "--metadata-insecure")    cli.md_insecure = true;
        else if (a == "--metadata-url") { cli.md_url = true;
                                          cli.md_url_v = get_val(i++, "--metadata-url"); }
        else if (a == "--metadata-camera-id") { cli.md_cam = true;
                                                cli.md_cam_v = get_val(i++, "--metadata-camera-id"); }
        else if (a == "--metadata-queue") { cli.md_queue = true;
                                            cli.md_queue_v = std::atoi(get_val(i++, "--metadata-queue").c_str()); }
        else if (a == "--metadata-timeout") { cli.md_timeout = true;
                                              cli.md_timeout_v = std::atoi(get_val(i++, "--metadata-timeout").c_str()); }
        else if (a == "--metadata-heartbeat") { cli.md_hb = true;
                                                cli.md_hb_v = std::atoi(get_val(i++, "--metadata-heartbeat").c_str()); }
        else if (a == "--ai-input") {
            if (i + 2 >= argc) { CA_LOG_ERROR("Missing value for --ai-input <w> <h>"); }
            else { cli.ai_w = cli.ai_h = true;
                   cli.ai_w_v = std::atoi(argv[i + 1]);
                   cli.ai_h_v = std::atoi(argv[i + 2]);
                   i += 2; }
        }
    }

    // ---- 2. Precedence: defaults -> YAML -> CLI ----
    ca::Config cfg = ca::make_default_config();

    std::string cfgpath = cli.config;
    if (cfgpath.empty() && file_exists("config/camera-agent.yaml"))
        cfgpath = "config/camera-agent.yaml";
    if (!cfgpath.empty())
        ca::load_config(cfg, cfgpath);

    if (cli.camera) cfg.camera.id       = cli.camera_v;
    if (cli.width)  cfg.camera.width    = cli.width_v;
    if (cli.height) cfg.camera.height   = cli.height_v;
    if (cli.fps)    cfg.camera.fps      = cli.fps_v;
    if (cli.autoneg)   cfg.camera.auto_res = true;
    if (cli.bitrate) cfg.encoder.bitrate = cli.bitrate_v;
    if (cli.stream) cfg.stream.id       = cli.stream_v;
    if (cli.server) cfg.rtsp.server     = cli.server_v;
    if (cli.port)   cfg.rtsp.port       = cli.port_v;
    if (cli.device) cfg.device_id       = cli.device_v;
    if (cli.log)    cfg.log_level       = cli.log_v;
    if (cli.latency) cfg.measure_latency = true;
    if (cli.source)  cfg.source          = cli.source_v;
    if (cli.ai)      cfg.ai.enable       = true;
    if (cli.ai_no)   cfg.ai.enable       = false;
    if (cli.ai_fps)  cfg.ai.fps          = ca::clamp_ai_fps(cli.ai_fps_v);
    if (cli.ai_conf) cfg.ai.confidence   = cli.ai_conf_v;
    if (cli.ai_model)  cfg.ai.model         = cli.ai_model_v;
    if (cli.ai_model_pose)  cfg.ai.model_pose      = cli.ai_model_pose_v;
    if (cli.ai_aimode_base) cfg.ai.aimode_base_url = cli.ai_aimode_base_v;
    if (cli.ai_aimode_poll_ms) cfg.ai.aimode_poll_ms = cli.ai_aimode_poll_ms_v;
    if (cli.ai_aimode_off)      cfg.ai.aimode_poll    = false;
    if (cli.ai_w)      cfg.ai.input_width   = cli.ai_w_v;
    if (cli.ai_h)      cfg.ai.input_height  = cli.ai_h_v;
    if (cli.ai_queue)  cfg.ai.queue_size    = cli.ai_queue_v;
    if (cli.ai_frb)    cfg.ai.full_rate_below_fps = cli.ai_frb_v;
    if (cli.ai_log_objects) cfg.ai.log_objects = true;
    if (cli.md)              cfg.metadata.enable       = true;
    if (cli.md_no)           cfg.metadata.enable       = false;
    if (cli.md_url)          cfg.metadata.server_url   = cli.md_url_v;
    if (cli.md_cam)          cfg.metadata.camera_id    = cli.md_cam_v;
    if (cli.md_queue)        cfg.metadata.queue_size   = cli.md_queue_v;
    if (cli.md_timeout)      cfg.metadata.timeout_ms   = cli.md_timeout_v;
    if (cli.md_hb)           cfg.metadata.heartbeat_interval_sec = cli.md_hb_v;
    if (cli.md_log_payload)  cfg.metadata.log_payload  = true;
    if (cli.md_insecure)     cfg.metadata.skip_tls_verify = true;
    const double duration = cli.dur ? cli.duration_v : 0.0;

    ca::log::set_level(cfg.log_level);

    if (cli.version) { print_env(); return 0; }
    if (cli.help)    { print_help(); return 0; }

    CA_LOG_INFO("Camera Agent starting (backend={})", CAMERA_AGENT_BACKEND_NAME);

    if (cli.list) return cmd_list();

    // ---- 3. Run ----
    std::signal(SIGINT, [](int) { ca::request_app_stop(); });

    ca::StreamController controller(cfg);
    if (!controller.start()) {
        CA_LOG_ERROR("Failed to start stream. Exiting.");
        return 1;
    }

    controller.run_blocking(duration);

    controller.stop();
    CA_LOG_INFO("Stopped. Bye.");
    return 0;
}
