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

        // 网络相机对接验收：--nettest [--url URL] [--camera ID] [--setmode MODE]
        int netIdx = Array.FindIndex(args,
            a => string.Equals(a, "--nettest", StringComparison.OrdinalIgnoreCase));
        if (netIdx >= 0)
        {
            string? netUrl = null;
            int nuIdx = Array.FindIndex(args, a => string.Equals(a, "--url", StringComparison.OrdinalIgnoreCase));
            if (nuIdx >= 0 && nuIdx + 1 < args.Length)
            {
                netUrl = args[nuIdx + 1];
            }

            string? netCamera = null;
            int ncIdx = Array.FindIndex(args, a => string.Equals(a, "--camera", StringComparison.OrdinalIgnoreCase));
            if (ncIdx >= 0 && ncIdx + 1 < args.Length)
            {
                netCamera = args[ncIdx + 1];
            }

            string? netSetMode = null;
            int nsIdx = Array.FindIndex(args, a => string.Equals(a, "--setmode", StringComparison.OrdinalIgnoreCase));
            if (nsIdx >= 0 && nsIdx + 1 < args.Length)
            {
                netSetMode = args[nsIdx + 1];
            }

            return NetworkProbe.Run(netUrl, netCamera, netSetMode);
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
