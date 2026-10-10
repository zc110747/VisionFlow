using VisionFlow.Desktop.Network;

namespace VisionFlow.Desktop;

/// <summary>
/// 无头网络相机探针：验证 video-server REST 对接。
/// <para>
/// 服务未运行时按「不可达」报告，不算失败——它本来就是可选依赖。
/// 只有在服务可达却解析失败时才判 FAIL。
/// </para>
/// </summary>
internal static class NetworkProbe
{
    internal static int Run(string? baseUrl, string? cameraId, string? setMode)
    {
        ConsoleSetup.UseUtf8();

        Console.WriteLine("=== 网络相机探针 ===");

        using var client = new NetworkCameraClient(baseUrl);
        Console.WriteLine($"基地址: {client.BaseUrl}");

        // ── 1. 枚举相机 ──
        var cameras = client.ListCameras(out var diag);
        Console.WriteLine("--- 诊断 ---");
        foreach (var line in diag)
        {
            Console.WriteLine("  " + line);
        }

        Console.WriteLine($"RESULT reachable={client.ServerReachable}");
        Console.WriteLine($"RESULT cameras={cameras.Count}");

        if (!client.ServerReachable)
        {
            Console.WriteLine($"服务不可达：{client.LastError}");
            Console.WriteLine("RESULT verdict=UNREACHABLE");
            Console.WriteLine("=== 探针结束 ===");
            return 0;   // 不可达不算失败
        }

        Console.WriteLine("--- 相机列表 ---");
        string? firstOnlineId = null;
        string? firstOnlineUrl = null;

        foreach (var c in cameras)
        {
            Console.WriteLine($"  {c.Id} | {c.Name} | status={c.Status} | " +
                              $"stream={c.StreamPath} | rtsp={c.RtspUrl ?? "(无)"} | " +
                              $"{c.Resolution ?? "?"} @ {c.Fps}fps");

            var item = client.ToSourceItem(c);
            Console.WriteLine($"      下拉框条目: {item?.Display ?? "(不可播放)"}");
            Console.WriteLine($"      播放地址  : {item?.Input ?? "-"}");

            if (c.IsOnline && item is not null && firstOnlineId is null)
            {
                firstOnlineId = c.Id;
                firstOnlineUrl = item.Input;
            }
        }

        int onlineCount = cameras.Count(c => c.IsOnline);
        Console.WriteLine($"RESULT online={onlineCount}");
        Console.WriteLine($"RESULT first_online={firstOnlineId ?? "null"}");

        if (cameras.Count == 0)
        {
            Console.WriteLine("RESULT verdict=NO_CAMERA");
            Console.WriteLine("=== 探针结束 ===");
            return 0;
        }

        // ── 2. 选定相机：读 AI 模式 + 快照 ──
        string target = cameraId ?? cameras[0].Id;
        Console.WriteLine($"--- 目标相机: {target} ---");

        string? mode = client.GetAiMode(target);
        Console.WriteLine($"当前 AI 模式: {mode ?? "(读取失败)"}");
        Console.WriteLine($"RESULT aimode={mode ?? "null"}");

        var snap = client.GetMetadata(target);
        if (snap is null)
        {
            Console.WriteLine($"读取 metadata 失败: {client.LastError}");
            Console.WriteLine("RESULT metadata=None");
        }
        else
        {
            Console.WriteLine($"metadata.camera_id = {snap.CameraId}");

            if (snap.Status is { } st)
            {
                Console.WriteLine($"  心跳: running={st.Running} enable={st.Enable} " +
                                  $"fps={st.Fps:F2} model={st.Model ?? "-"} tracker={st.Tracker ?? "-"} " +
                                  $"processed={st.Processed}");
                Console.WriteLine($"RESULT status_running={st.Running}");
                Console.WriteLine($"RESULT status_fps={st.Fps:F2}");
                Console.WriteLine($"RESULT status_model={st.Model ?? "null"}");
            }
            else
            {
                Console.WriteLine("  心跳: (无)");
                Console.WriteLine("RESULT status_running=null");
            }

            if (snap.Frame is { } fr)
            {
                Console.WriteLine($"  最新帧: frame_id={fr.FrameId} {fr.VideoWidth}x{fr.VideoHeight} " +
                                  $"ai_mode={fr.AiMode ?? "-"} objects={fr.Objects.Count}");
                Console.WriteLine($"RESULT frame_objects={fr.Objects.Count}");
                Console.WriteLine($"RESULT frame_size={fr.VideoWidth}x{fr.VideoHeight}");
                Console.WriteLine($"RESULT frame_aimode={fr.AiMode ?? "null"}");

                foreach (var o in fr.Objects)
                {
                    int kp = o.Keypoints?.Length ?? 0;
                    string bbox = o.Bbox.Length == 4
                        ? $"[{o.Bbox[0]},{o.Bbox[1]},{o.Bbox[2]},{o.Bbox[3]}]"
                        : "(bbox 长度异常)";
                    Console.WriteLine($"      {o.Class} conf={o.Confidence:F3} track={o.TrackId} " +
                                      $"bbox={bbox} keypoints={kp}");
                }
            }
            else
            {
                Console.WriteLine("  最新帧: (无)");
                Console.WriteLine("RESULT frame_objects=null");
            }
        }

        // ── 3. 可选：切换 AI 模式 ──
        if (!string.IsNullOrEmpty(setMode))
        {
            Console.WriteLine($"--- 设置 AI 模式 → {setMode} ---");
            bool ok = client.SetAiMode(target, setMode);
            Console.WriteLine($"POST 结果: {(ok ? "成功" : "失败: " + client.LastError)}");
            Console.WriteLine($"RESULT setmode_ok={ok}");

            if (ok)
            {
                string? readback = client.GetAiMode(target);
                Console.WriteLine($"回读: {readback}");
                Console.WriteLine($"RESULT setmode_readback={readback ?? "null"}");
                Console.WriteLine($"RESULT verdict={(readback == setMode ? "PASS" : "FAIL")}");
            }
            else
            {
                Console.WriteLine("RESULT verdict=SET_FAILED");
            }
        }
        else
        {
            Console.WriteLine("RESULT verdict=PASS");
        }

        // ── 4. 在线相机直拉验证 ──
        // 这是「需求 3」的真正闭环判据：REST 枚举出的地址必须能被
        // FFmpeg 打开。只断言「列表返回了 N 个相机」是不够的——
        // 服务端的 rtsp_url 主机名可能是错的（见 ResolvePlayUrl 注释），
        // 那种情况下列表正常但拉流必 404。
        if (firstOnlineUrl is not null)
        {
            Console.WriteLine($"--- 在线相机直拉: {firstOnlineUrl} ---");

            try
            {
                Rendering.FFmpegLoader.Initialize();
                int rc = DecodeProbe.Run(firstOnlineUrl, forceSoftware: false, frames: 12,
                    outDir: null, simulateHwFailure: false);
                Console.WriteLine($"RESULT open_online_rc={rc}");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"直拉异常: {ex.Message}");
                Console.WriteLine("RESULT open_online_rc=-1");
            }
        }
        else
        {
            Console.WriteLine("--- 在线相机直拉: 跳过（当前无在线相机）---");
            Console.WriteLine("RESULT open_online_rc=skip");
        }

        Console.WriteLine("=== 探针结束 ===");
        return 0;
    }
}
