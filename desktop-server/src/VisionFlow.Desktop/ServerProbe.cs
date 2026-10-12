using System.Diagnostics;
using System.Text;
using VisionFlow.Desktop.Rendering;
using VisionFlow.Desktop.Server;

namespace VisionFlow.Desktop;

/// <summary>
/// 无头服务器探针：端到端验证「本端作为视频服务器」这条链路。
///
/// <para><b>为什么要端到端而不只是单元测试</b></para>
/// <para>
/// 这条链路上的每一环都可能单独通过而整体不通：
/// MediaMTX 起来了但端口写错、metadata 能 POST 但 aimode 读不回来、
/// 路径列表返回了但 <c>ready</c> 恒为 false。单元测试只会告诉你
/// "每个函数都对"，而真实故障恰恰出在它们之间的契约上。
/// 因此本探针自己拉起整套服务器，再用真实 HTTP 请求和真实
/// ffmpeg 推流/拉流去打它。
/// </para>
///
/// <para><b>agent 用什么替代</b></para>
/// <para>
/// 真实 camera-agent 需要摄像头与 GStreamer，在 CI/无人值守环境下
/// 不一定可用。这里用 <b>ffmpeg</b> 代替 agent 承担两个角色：
/// </para>
/// <list type="number">
///   <item><c>ffmpeg -re -f lavfi -i testsrc ... -f rtsp rtsp://...</c>
///     —— 扮演"推流方"，验证 RTSP ANNOUNCE 接收路径</item>
///   <item>用 <c>HttpClient</c> 直接 POST metadata / GET aimode
///     —— 扮演"AI 上报方"，验证 HTTP 契约（报文格式与
///     <c>metadata_encoder.cpp</c> 对齐）</item>
/// </list>
/// <para>
/// 这样即使没有摄像头、没有 agent 可执行文件，也能把服务端这条
/// 链路完整验一遍。真实 agent 接入是这套协议的**同一份**契约，
/// 因此这里通过就意味着 agent 能接上。
/// </para>
/// </summary>
internal static class ServerProbe
{
    private const int MetadataPort = ServerHost.DefaultMetadataPort;

    internal static int Run(bool keepMediaMtx, int streamSeconds)
    {
        ConsoleSetup.UseUtf8();

        int pass = 0;
        int fail = 0;
        int skip = 0;

        // key 是显式传入的 ASCII 标识，供 run_tests.bat 用 findstr 断言。
        // 不自动从 name 派生：name 含中文时派生出空串，多个检查项会撞成
        // 同一个 "CHK =ok"，验收会变成"看着全绿其实什么都没验"。
        void Check(string key, string name, bool ok, string? detail = null)
        {
            if (ok)
            {
                pass++;
                Console.WriteLine($"  [PASS] {name}{(detail is null ? "" : $" — {detail}")}");
            }
            else
            {
                fail++;
                Console.WriteLine($"  [FAIL] {name}{(detail is null ? "" : $" — {detail}")}");
            }

            // 机器可读行：只含 ASCII。上面那行含中文与箭头，在 GBK 控制台下
            // 按字节匹配会失败，因此验收必须依赖这一行。
            Console.WriteLine($"CHK {key}={(ok ? "ok" : "fail")}");
        }

        void Skip(string key, string name, string reason)
        {
            skip++;
            Console.WriteLine($"  [SKIP] {name} — {reason}");
            Console.WriteLine($"CHK {key}=skip");
        }

        Console.WriteLine("=== 内嵌服务器探针 ===");

        // ── 1. 二进制解析 ──
        Console.WriteLine("\n--- 1. MediaMTX 二进制 ---");
        string? binary = MediaMtxHost.ResolveBinary();
        Check("mediamtx_exe_found", "找到 mediamtx.exe", binary is not null, binary ?? "未找到");
        if (binary is null)
        {
            Console.WriteLine("无法继续：缺少 MediaMTX 二进制。");
            Console.WriteLine($"PASS={pass} FAIL={fail} SKIP={skip}");
            Console.WriteLine("RESULT verdict=NO_BINARY");
            Console.WriteLine("=== 探针结束 ===");
            return 1;
        }

        // ── 2. 启动服务器 ──
        Console.WriteLine("\n--- 2. 启动内嵌服务器 ---");
        using var server = new ServerHost();

        var logs = new List<string>();
        bool started = server.Start(msg =>
        {
            logs.Add(msg);
            Console.WriteLine("  " + msg);
        });

        Check("server_started", "服务器启动", started, server.LastError);
        if (!started)
        {
            Console.WriteLine($"PASS={pass} FAIL={fail} SKIP={skip}");
            Console.WriteLine("RESULT verdict=START_FAILED");
            Console.WriteLine("=== 探针结束 ===");
            return 1;
        }

        Check("mtx_process_running", "MediaMTX 进程在运行", server.MediaMtx?.IsRunning == true);
        Check("metadata_listening", "metadata 服务在监听", server.Metadata?.IsRunning == true);
        Check("server_ready", "服务器整体就绪", server.IsRunning);

        // ── 3. 控制 API 可达 ──
        Console.WriteLine("\n--- 3. MediaMTX 控制 API ---");
        Check("api_paths_list_reachable", "控制 API /v3/paths/list 可达", server.MediaMtx!.ProbeApi());
        var pathsBefore = server.MediaMtx.ListPaths();
        Console.WriteLine($"  当前路径数: {pathsBefore.Count}");

        // ── 4. metadata HTTP 契约 ──
        Console.WriteLine("\n--- 4. metadata 端点契约 ---");
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(5) };
        string metaUrl = $"http://127.0.0.1:{MetadataPort}/api/metadata";
        string healthUrl = $"http://127.0.0.1:{MetadataPort}/api/health";

        // 4.1 health
        try
        {
            string health = http.GetStringAsync(healthUrl).GetAwaiter().GetResult();
            Check("health_200", "GET /api/health 返回 200", health.Contains("\"ok\":true"), Truncate(health, 80));
        }
        catch (Exception ex)
        {
            Check("health_200", "GET /api/health 返回 200", false, ex.Message);
        }

        // 4.2 合法 frame 报文 → 204
        const string cameraId = "probe-cam-01";
        string frameJson = $$"""
            {
              "version": 1,
              "type": "frame",
              "camera_id": "{{cameraId}}",
              "frame_id": 1001,
              "timestamp": 1730000000000,
              "video_width": 1920,
              "video_height": 1080,
              "ai_mode": "ai-y",
              "objects": [
                {
                  "class": "person",
                  "confidence": 0.87,
                  "track_id": 3,
                  "bbox": [100, 200, 300, 500]
                }
              ]
            }
            """;

        var postResult = PostJson(http, metaUrl, frameJson);
        Check("post_frame_204", "POST /api/metadata (frame) -> 204",
            postResult.Status == 204, $"HTTP {postResult.Status}");

        // 4.3 合法 status 报文 → 204
        string statusJson = $$"""
            {
              "version": 1,
              "type": "status",
              "camera_id": "{{cameraId}}",
              "timestamp": 1730000000000,
              "ai": {
                "enable": true,
                "running": true,
                "fps": 25.5,
                "model": "yolo11n.onnx",
                "tracker": "bytetrack",
                "last_frame_id": 1001,
                "last_timestamp": 1730000000000,
                "processed": 42
              }
            }
            """;

        postResult = PostJson(http, metaUrl, statusJson);
        Check("post_status_204", "POST /api/metadata (status) -> 204",
            postResult.Status == 204, $"HTTP {postResult.Status}");

        // 4.4 畸形 JSON → 400（不是 500，也不是静默吞掉）
        postResult = PostJson(http, metaUrl, "{ this is not json ");
        Check("post_malformed_400", "POST 畸形 JSON -> 400", postResult.Status == 400, $"HTTP {postResult.Status}");

        // 4.5 缺 camera_id → 400
        postResult = PostJson(http, metaUrl, """{"version":1,"type":"frame","frame_id":1}""");
        Check("post_no_camera_id_400", "POST 缺 camera_id -> 400", postResult.Status == 400, $"HTTP {postResult.Status}");

        // 4.6 未知 type → 400
        postResult = PostJson(http, metaUrl,
            $$"""{"version":1,"type":"bogus","camera_id":"{{cameraId}}"}""");
        Check("post_unknown_type_400", "POST 未知 type -> 400", postResult.Status == 400, $"HTTP {postResult.Status}");

        // ── 5. 相机状态合并 ──
        Console.WriteLine("\n--- 5. 相机状态合并 ---");
        var list = server.ListCameras(out var diag);
        foreach (var d in diag)
        {
            Console.WriteLine("  " + d);
        }

        var cam = list.FirstOrDefault(c => c.CameraId == cameraId);
        Check("camera_from_metadata_listed", "metadata 上报的相机会出现在列表中", cam is not null);
        Check("camera_video_offline_before_push", "条目标记为视频离线（尚未推流）", cam?.IsOnline == false,
            $"IsOnline={cam?.IsOnline}");

        // ── 6. aimode 读写闭环 ──
        Console.WriteLine("\n--- 6. aimode 端点 ---");
        string aimodeUrl = $"http://127.0.0.1:{MetadataPort}/api/cameras/{cameraId}/aimode";

        try
        {
            string body = http.GetStringAsync(aimodeUrl).GetAwaiter().GetResult();
            Check("get_aimode_200", "GET aimode 返回 200 且有 mode", body.Contains("\"mode\""), Truncate(body, 60));

            // 默认值应为 ai-y（与 video-server 的 DefaultAIMode 一致）
            Check("default_mode_ai_y", "默认模式为 ai-y", body.Contains("ai-y"), Truncate(body, 60));
        }
        catch (Exception ex)
        {
            Check("get_aimode_200", "GET aimode 返回 200", false, ex.Message);
        }

        // 切换到 ai-y-pose
        postResult = PostJson(http, aimodeUrl, """{"mode":"ai-y-pose"}""");
        Check("post_aimode_pose_200", "POST aimode (ai-y-pose) -> 200", postResult.Status == 200, $"HTTP {postResult.Status}");

        try
        {
            string body = http.GetStringAsync(aimodeUrl).GetAwaiter().GetResult();
            Check("readback_ai_y_pose", "回读模式为 ai-y-pose", body.Contains("ai-y-pose"), Truncate(body, 60));
        }
        catch (Exception ex)
        {
            Check("readback_ai_y_pose", "回读模式", false, ex.Message);
        }

        // 非法模式 → 400
        postResult = PostJson(http, aimodeUrl, """{"mode":"ai-bogus"}""");
        Check("post_invalid_mode_400", "POST 非法模式 -> 400", postResult.Status == 400, $"HTTP {postResult.Status}");

        // ai-off 是合法模式（agent 侧会拒绝它换模型，但端点接受）
        postResult = PostJson(http, aimodeUrl, """{"mode":"ai-off"}""");
        Check("post_ai_off_200", "POST ai-off -> 200（合法）", postResult.Status == 200, $"HTTP {postResult.Status}");

        // ── 7. 真实 RTSP 推流接收 + 拉流解码 ──
        Console.WriteLine("\n--- 7. RTSP 推流接收与拉流 ---");
        string? ffmpeg = FindFfmpeg();
        if (ffmpeg is null)
        {
            Skip("rtsp_push_pull", "RTSP 推流/拉流闭环", "未找到 ffmpeg.exe");
        }
        else
        {
            Console.WriteLine($"  ffmpeg: {ffmpeg}");
            string pushUrl = $"{server.RtspBase}/{cameraId}";
            Console.WriteLine($"  推流地址: {pushUrl}");

            // 推流必须比"等就绪 + 拉流解码"活得久，否则拉流拉到一半源就断了。
            // 解码 15 帧在硬解下实测约 4s，加上最长 10s 的就绪等待，
            // 取 streamSeconds + 20s 作为安全余量（推不完由 TryKill 收掉）。
            var ff = StartFfmpegPush(ffmpeg, pushUrl, streamSeconds + 20);
            if (ff is null)
            {
                Check("ffmpeg_push_started", "启动 ffmpeg 推流", false);
            }
            else
            {
                try
                {
                    // 等 MediaMTX 把这个路径标记为 ready
                    bool ready = WaitForPathReady(server, cameraId, TimeSpan.FromSeconds(10));
                    Check("mtx_path_ready", "MediaMTX 报告路径 ready", ready,
                        ready ? null : "10s 内未就绪");

                    if (ready)
                    {
                        var paths = server.MediaMtx!.ListPaths();
                        var p = paths.FirstOrDefault(x => x.Name == cameraId);
                        Console.WriteLine($"  路径轨迹: {string.Join(",", p?.Tracks ?? [])}");
                        Check("path_has_tracks", "路径有视频轨道", (p?.Tracks.Count ?? 0) > 0);
                    }

                    // 用本端 ffmpeg 解码器真实拉流
                    if (ready)
                    {
                        try
                        {
                            FFmpegLoader.Initialize();
                            int rc = DecodeProbe.Run(pushUrl, forceSoftware: false,
                                frames: 15, outDir: null, simulateHwFailure: false);
                            Check("rtsp_pull_decode", "本端直拉 RTSP 解码", rc == 0, $"DecodeProbe rc={rc}");
                        }
                        catch (Exception ex)
                        {
                            Check("rtsp_pull_decode", "本端直拉 RTSP 解码", false, ex.Message);
                        }
                    }

                    // 视频就绪后，相机应被标为在线
                    var list2 = server.ListCameras(out _);
                    var cam2 = list2.FirstOrDefault(c => c.CameraId == cameraId);
                    if (ready)
                    {
                        Check("camera_online_after_push", "推流后相机标记为在线", cam2?.IsOnline == true,
                            $"IsOnline={cam2?.IsOnline}");
                    }
                }
                finally
                {
                    TryKill(ff);
                }
            }
        }

        // ── 8. 停止与端口释放 ──
        Console.WriteLine("\n--- 8. 停止服务器 ---");
        server.Stop();

        Check("mtx_stopped", "MediaMTX 已停止", server.MediaMtx is null || !server.MediaMtx.IsRunning);
        Check("metadata_stopped", "metadata 已停止", server.Metadata is null || !server.Metadata.IsRunning);

        // 端口必须真的释放：否则下次启动会因"端口被占用"失败
        Thread.Sleep(800);
        bool portFree = IsPortFree(MetadataPort);
        Check("metadata_port_released", $"metadata 端口 {MetadataPort} 已释放", portFree);

        Console.WriteLine($"\nPASS={pass} FAIL={fail} SKIP={skip}");
        Console.WriteLine($"RESULT pass={pass}");
        Console.WriteLine($"RESULT fail={fail}");
        Console.WriteLine($"RESULT skip={skip}");
        Console.WriteLine(fail == 0 ? "STATUS: ALL PASSED" : "STATUS: FAILURES");
        Console.WriteLine($"RESULT verdict={(fail == 0 ? "PASS" : "FAIL")}");
        Console.WriteLine("=== 探针结束 ===");

        return fail == 0 ? 0 : 1;
    }

    // ── 辅助 ────────────────────────────────────────────────────────

    private static (int Status, string Body) PostJson(HttpClient http, string url, string json)
    {
        try
        {
            using var content = new StringContent(json, Encoding.UTF8, "application/json");
            using var resp = http.PostAsync(url, content).GetAwaiter().GetResult();
            string body = resp.Content.ReadAsStringAsync().GetAwaiter().GetResult();
            return ((int)resp.StatusCode, body);
        }
        catch (Exception ex)
        {
            return (-1, ex.Message);
        }
    }

    private static string Truncate(string s, int n)
        => s.Length <= n ? s : s[..n] + "…";

    /// <summary>找到可用于推流的 ffmpeg.exe。</summary>
    private static string? FindFfmpeg()
    {
        // 1. 环境变量（build_oneclick.bat 已按此约定设置）
        string? root = Environment.GetEnvironmentVariable("FFMPEG_ROOT");
        if (!string.IsNullOrWhiteSpace(root))
        {
            string candidate = Path.Combine(root, "bin", "ffmpeg.exe");
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        // 2. PATH
        string? pathVar = Environment.GetEnvironmentVariable("PATH");
        if (!string.IsNullOrEmpty(pathVar))
        {
            foreach (string seg in pathVar.Split(Path.PathSeparator))
            {
                if (string.IsNullOrWhiteSpace(seg))
                {
                    continue;
                }

                try
                {
                    string full = Path.Combine(seg.Trim(), "ffmpeg.exe");
                    if (File.Exists(full))
                    {
                        return full;
                    }
                }
                catch (ArgumentException)
                {
                }
            }
        }

        return null;
    }

    /// <summary>起一个 ffmpeg 合成测试图案推到 RTSP。</summary>
    private static Process? StartFfmpegPush(string ffmpeg, string url, int seconds)
    {
        // -re: 按真实时间推，让 MediaMTX 看到连续流而不是瞬间灌完
        // testsrc: 无外部素材也能出画面
        var psi = new ProcessStartInfo
        {
            FileName = ffmpeg,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };

        psi.ArgumentList.Add("-hide_banner");
        psi.ArgumentList.Add("-re");
        psi.ArgumentList.Add("-f");
        psi.ArgumentList.Add("lavfi");
        psi.ArgumentList.Add("-i");
        psi.ArgumentList.Add($"testsrc=size=640x480:rate=25");
        psi.ArgumentList.Add("-t");
        psi.ArgumentList.Add(seconds.ToString());
        psi.ArgumentList.Add("-c:v");
        psi.ArgumentList.Add("libx264");
        psi.ArgumentList.Add("-preset");
        psi.ArgumentList.Add("ultrafast");
        psi.ArgumentList.Add("-tune");
        psi.ArgumentList.Add("zerolatency");
        psi.ArgumentList.Add("-pix_fmt");
        psi.ArgumentList.Add("yuv420p");
        psi.ArgumentList.Add("-f");
        psi.ArgumentList.Add("rtsp");
        psi.ArgumentList.Add("-rtsp_transport");
        psi.ArgumentList.Add("tcp");
        psi.ArgumentList.Add(url);

        try
        {
            var proc = new Process { StartInfo = psi };
            proc.Start();
            return proc;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  启动 ffmpeg 失败: {ex.Message}");
            return null;
        }
    }

    /// <summary>轮询直到该路径 ready。</summary>
    private static bool WaitForPathReady(ServerHost server, string cameraId, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            var paths = server.MediaMtx?.ListPaths() ?? [];
            if (paths.Any(p => p.Name == cameraId && p.Ready))
            {
                return true;
            }

            Thread.Sleep(300);
        }

        return false;
    }

    private static void TryKill(Process proc)
    {
        try
        {
            if (!proc.HasExited)
            {
                proc.Kill(entireProcessTree: true);
                proc.WaitForExit(3000);
            }
        }
        catch
        {
            // 进程可能已自行退出
        }
        finally
        {
            proc.Dispose();
        }
    }

    /// <summary>端口是否空闲（能绑定上就说明前的监听已释放）。</summary>
    private static bool IsPortFree(int port)
    {
        try
        {
            var listener = new System.Net.Sockets.TcpListener(
                System.Net.IPAddress.Loopback, port);
            listener.Start();
            listener.Stop();
            return true;
        }
        catch
        {
            return false;
        }
    }
}
