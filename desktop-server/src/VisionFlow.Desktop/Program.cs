using VisionFlow.Desktop.Rendering;
using VisionFlow.Desktop.UI;

namespace VisionFlow.Desktop;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        // 无界面自检模式：供构建后 / CI 验证 ffmpeg 加载与硬解可用性，
        // 不弹窗、不依赖桌面会话。
        if (args.Any(a => string.Equals(a, "--selfcheck", StringComparison.OrdinalIgnoreCase)))
        {
            return SelfCheck.Run();
        }

        // 无头解码验收：--decodetest <输入> [--soft] [--frames N] [--out DIR]
        int decodeIdx = Array.FindIndex(args,
            a => string.Equals(a, "--decodetest", StringComparison.OrdinalIgnoreCase));

        if (decodeIdx >= 0)
        {
            if (decodeIdx + 1 >= args.Length)
            {
                Console.Error.WriteLine("用法: --decodetest <输入路径或URL> [--soft] [--frames N] [--out DIR]");
                return 2;
            }

            string input = args[decodeIdx + 1];
            bool soft = args.Any(a => string.Equals(a, "--soft", StringComparison.OrdinalIgnoreCase));
            bool failSim = args.Any(a => string.Equals(a, "--failsim", StringComparison.OrdinalIgnoreCase));

            int frames = 60;
            int fIdx = Array.FindIndex(args, a => string.Equals(a, "--frames", StringComparison.OrdinalIgnoreCase));
            if (fIdx >= 0 && fIdx + 1 < args.Length && int.TryParse(args[fIdx + 1], out int f))
            {
                frames = f;
            }

            string? outDir = null;
            int oIdx = Array.FindIndex(args, a => string.Equals(a, "--out", StringComparison.OrdinalIgnoreCase));
            if (oIdx >= 0 && oIdx + 1 < args.Length)
            {
                outDir = args[oIdx + 1];
            }

            // 解码探针同样需要 ffmpeg 就绪
            try
            {
                FFmpegLoader.Initialize();
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"ffmpeg 初始化失败: {ex.Message}");
                return 1;
            }

            return DecodeProbe.Run(input, soft, frames, outDir, failSim);
        }

        // 解码性能剖析：--decodebench <输入> [--soft] [--frames N]
        int benchIdx = Array.FindIndex(args,
            a => string.Equals(a, "--decodebench", StringComparison.OrdinalIgnoreCase));

        if (benchIdx >= 0 && benchIdx + 1 < args.Length)
        {
            string bInput = args[benchIdx + 1];
            bool bSoft = args.Any(a => string.Equals(a, "--soft", StringComparison.OrdinalIgnoreCase));
            int bFrames = 90;
            int bfIdx = Array.FindIndex(args, a => string.Equals(a, "--frames", StringComparison.OrdinalIgnoreCase));
            if (bfIdx >= 0 && bfIdx + 1 < args.Length && int.TryParse(args[bfIdx + 1], out int bf))
            {
                bFrames = bf;
            }

            return DecodeBench.Run(bInput, bSoft, bFrames);
        }

        // 色彩转换基准：--swsbench
        if (args.Any(a => string.Equals(a, "--swsbench", StringComparison.OrdinalIgnoreCase)))
        {
            return SwsBench.Run();
        }

        // 首帧一致性诊断：--diagframe <输入>
        int diagIdx = Array.FindIndex(args,
            a => string.Equals(a, "--diagframe", StringComparison.OrdinalIgnoreCase));
        if (diagIdx >= 0 && diagIdx + 1 < args.Length)
        {
            return FrameDiag.Run(args[diagIdx + 1]);
        }

        // 摄像头采集验收：--camtest [--device N] [--frames N] [--out DIR] [--list]
        int camIdx = Array.FindIndex(args,
            a => string.Equals(a, "--camtest", StringComparison.OrdinalIgnoreCase));
        if (camIdx >= 0)
        {
            // --list 只枚举不采帧
            if (args.Any(a => string.Equals(a, "--list", StringComparison.OrdinalIgnoreCase)))
            {
                return CameraList.Run();
            }

            int camDevice = 0;
            int cdIdx = Array.FindIndex(args, a => string.Equals(a, "--device", StringComparison.OrdinalIgnoreCase));
            if (cdIdx >= 0 && cdIdx + 1 < args.Length && int.TryParse(args[cdIdx + 1], out int cd))
            {
                camDevice = cd;
            }

            int camFrames = 30;
            int cfIdx = Array.FindIndex(args, a => string.Equals(a, "--frames", StringComparison.OrdinalIgnoreCase));
            if (cfIdx >= 0 && cfIdx + 1 < args.Length && int.TryParse(args[cfIdx + 1], out int cf))
            {
                camFrames = cf;
            }

            string? camOut = null;
            int coIdx = Array.FindIndex(args, a => string.Equals(a, "--out", StringComparison.OrdinalIgnoreCase));
            if (coIdx >= 0 && coIdx + 1 < args.Length)
            {
                camOut = args[coIdx + 1];
            }

            try
            {
                return CameraProbe.Run(camDevice, camFrames, camOut);
            }
            finally
            {
                Capture.CameraEnumerator.Shutdown();
            }
        }

        // 跨线程事件回归：--threadtest
        // 用真实 CameraPreviewController 在后台线程触发 RunningChanged/Ended，
        // 验证 MainForm 引入的 BeginInvoke 封送不会导致跨线程 InvalidOperationException。
        if (args.Any(a => string.Equals(a, "--threadtest", StringComparison.OrdinalIgnoreCase)))
        {
            try
            {
                return ThreadingRegressionProbe.Run(null);
            }
            finally
            {
                Capture.CameraEnumerator.Shutdown();
            }
        }

        // 内嵌服务器端到端验收：--servertest [--keep] [--seconds N]
        // 拉起 MediaMTX + metadata 端点，用 ffmpeg 模拟 agent 推流，
        // 再走一遍 HTTP 契约与真实解码拉流。
        if (args.Any(a => string.Equals(a, "--servertest", StringComparison.OrdinalIgnoreCase)))
        {
            bool keep = args.Any(a => string.Equals(a, "--keep", StringComparison.OrdinalIgnoreCase));

            int seconds = 20;
            int sIdx = Array.FindIndex(args, a => string.Equals(a, "--seconds", StringComparison.OrdinalIgnoreCase));
            if (sIdx >= 0 && sIdx + 1 < args.Length && int.TryParse(args[sIdx + 1], out int s))
            {
                seconds = s;
            }

            try
            {
                FFmpegLoader.Initialize();
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"ffmpeg 初始化失败: {ex.Message}");
                return 1;
            }

            return ServerProbe.Run(keep, seconds);
        }

        // AI 叠加渲染验收：--overlaytest [--out DIR]
        if (args.Any(a => string.Equals(a, "--overlaytest", StringComparison.OrdinalIgnoreCase)))
        {
            string? ovOut = null;
            int ovIdx = Array.FindIndex(args, a => string.Equals(a, "--out", StringComparison.OrdinalIgnoreCase));
            if (ovIdx >= 0 && ovIdx + 1 < args.Length)
            {
                ovOut = args[ovIdx + 1];
            }

            return OverlayProbe.Run(ovOut);
        }

        // AI 叠加落在实际解码帧验收：--realframe <输入> [--frames N] [--out DIR]
        int rfIdx = Array.FindIndex(args,
            a => string.Equals(a, "--realframe", StringComparison.OrdinalIgnoreCase));
        if (rfIdx >= 0)
        {
            string? rfInput = null;
            if (rfIdx + 1 < args.Length &&
                !args[rfIdx + 1].StartsWith("--", StringComparison.OrdinalIgnoreCase))
            {
                rfInput = args[rfIdx + 1];
            }

            int rfFrames = 5;
            int rfFIdx = Array.FindIndex(args, a => string.Equals(a, "--frames", StringComparison.OrdinalIgnoreCase));
            if (rfFIdx >= 0 && rfFIdx + 1 < args.Length && int.TryParse(args[rfFIdx + 1], out int rf))
            {
                rfFrames = rf;
            }

            string? rfOut = null;
            int rfOIdx = Array.FindIndex(args, a => string.Equals(a, "--out", StringComparison.OrdinalIgnoreCase));
            if (rfOIdx >= 0 && rfOIdx + 1 < args.Length)
            {
                rfOut = args[rfOIdx + 1];
            }

            try
            {
                FFmpegLoader.Initialize();
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"ffmpeg 初始化失败: {ex.Message}");
                return 1;
            }

            return RealFrameOverlayProbe.Run(rfInput, rfFrames, rfOut);
        }

        // GUI 取证：--guicapture [--process NAME] [--out FILE] [--wait SEC]
        if (args.Any(a => string.Equals(a, "--guicapture", StringComparison.OrdinalIgnoreCase)))
        {
            string gcProcess = "VisionFlow.Desktop";
            int gpIdx = Array.FindIndex(args, a => string.Equals(a, "--process", StringComparison.OrdinalIgnoreCase));
            if (gpIdx >= 0 && gpIdx + 1 < args.Length)
            {
                gcProcess = args[gpIdx + 1];
            }

            string gcOut = Path.Combine(Path.GetTempPath(), "vf_gui.png");
            int goIdx = Array.FindIndex(args, a => string.Equals(a, "--out", StringComparison.OrdinalIgnoreCase));
            if (goIdx >= 0 && goIdx + 1 < args.Length)
            {
                gcOut = args[goIdx + 1];
            }

            int gcWait = 15;
            int gwIdx = Array.FindIndex(args, a => string.Equals(a, "--wait", StringComparison.OrdinalIgnoreCase));
            if (gwIdx >= 0 && gwIdx + 1 < args.Length && int.TryParse(args[gwIdx + 1], out int gw))
            {
                gcWait = gw;
            }

            return GuiCapture.Run(gcProcess, gcOut, gcWait);
        }

        ApplicationConfiguration.Initialize();

        // 先把 ffmpeg 运行库目录注册进 DLL 搜索路径，
        // 否则 FFmpeg.AutoGen 在第一次 P/Invoke 时会抛 DllNotFoundException。
        FFmpegLoader.Initialize();

        // GUI 取证辅助：--select-source <关键字>
        // 首帧填充下拉框后自动选中 Display/Input 中首个含该关键字的源，
        // 使无人值守截图能覆盖「网络相机」等非默认路径。
        string? autoSelect = null;
        int asIdx = Array.FindIndex(args,
            a => string.Equals(a, "--select-source", StringComparison.OrdinalIgnoreCase));
        if (asIdx >= 0 && asIdx + 1 < args.Length)
        {
            autoSelect = args[asIdx + 1];
        }

        try
        {
            Application.Run(new MainForm(autoSelect));
        }
        finally
        {
            Capture.CameraEnumerator.Shutdown();
        }

        return 0;
    }
}
