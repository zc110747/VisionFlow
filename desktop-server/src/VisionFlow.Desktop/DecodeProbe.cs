using System.Drawing;
using System.Drawing.Imaging;
using VisionFlow.Desktop.Decoding;

namespace VisionFlow.Desktop;

/// <summary>
/// 解码验收探针：用 <c>--decodetest &lt;输入&gt; [--soft]</c> 调用。
/// 无头解码前 N 帧并落盘 PNG，用于自动化验证硬解/软解两条路径。
/// </summary>
internal static class DecodeProbe
{
    /// <summary>
    /// 解码验收探针：--decodetest &lt;输入&gt; [--soft|--failsim] [--frames N] [--out DIR]
    /// </summary>
    /// <param name="forceSoftware">强制禁用硬解（验证软解路径）。</param>
    /// <param name="simulateHwFailure">模拟硬解不可用（验证自动回退链）。</param>
    public static int Run(string input, bool forceSoftware, int frames, string? outDir,
        bool simulateHwFailure = false)
    {
        try
        {
            Console.OutputEncoding = System.Text.Encoding.UTF8;
        }
        catch (IOException)
        {
            // 忽略
        }

        outDir ??= Path.Combine(Path.GetTempPath(), "visionflow-decprobe");
        Directory.CreateDirectory(outDir);

        Console.WriteLine("=========== 解码验收探针 ===========");
        Console.WriteLine($"输入      : {input}");
        Console.WriteLine($"硬解尝试  : {(!forceSoftware ? "是" : "否（强制软解）")}");
        if (simulateHwFailure)
        {
            Console.WriteLine($"回退模拟  : 是（把 D3D11VA/DXVA2 从候选链中剔除，检查是否自动落软解）");
        }

        Console.WriteLine($"目标帧数  : {frames}");
        Console.WriteLine($"输出目录  : {outDir}");
        Console.WriteLine();

        using var decoder = new VideoDecoder
        {
            HardwareEnabled = !forceSoftware,
            SimulateHardwareUnavailable = simulateHwFailure,
        };
        decoder.Log = Console.WriteLine;

        if (!decoder.Open(input, Console.WriteLine))
        {
            Console.WriteLine("[ FAIL ] 打开输入失败");
            return 1;
        }

        Console.WriteLine();
        Console.WriteLine($"实测后端  : {decoder.Backend}");
        Console.WriteLine($"回退原因  : {decoder.FallbackReason}");
        Console.WriteLine();

        int saved = 0;
        var sw = System.Diagnostics.Stopwatch.StartNew();

        while (saved < frames)
        {
            var bitmap = decoder.DecodeNextFrame();
            if (bitmap is null)
            {
                break;
            }

            if (saved < 5 || saved == frames - 1)
            {
                string path = Path.Combine(outDir, $"frame_{saved:D5}.png");
                bitmap.Save(path, ImageFormat.Png);
                Console.WriteLine($"[ 保存 ] {path} ({bitmap.Width}x{bitmap.Height})");
            }

            bitmap.Dispose();
            saved++;
        }

        sw.Stop();

        Console.WriteLine();
        Console.WriteLine($"已解码帧数: {saved}");
        Console.WriteLine($"耗时      : {sw.ElapsedMilliseconds} ms");
        if (saved > 0)
        {
            Console.WriteLine($"解码帧率  : {saved / Math.Max(sw.Elapsed.TotalSeconds, 0.001):F1} fps");
        }

        Console.WriteLine($"最终后端  : {decoder.Backend}");

        // 机器可读输出（供脚本 grep，全 ASCII，不受控制台代码页影响）
        Console.WriteLine();
        Console.WriteLine($"RESULT backend={decoder.Backend}");
        Console.WriteLine($"RESULT fallback={decoder.FallbackReason}");
        Console.WriteLine($"RESULT frames={saved}");
        Console.WriteLine($"RESULT expected={frames}");

        if (saved == 0)
        {
            Console.WriteLine("解码结论  : 失败（0 帧）");
            Console.WriteLine("RESULT verdict=FAIL");
            return 1;
        }

        Console.WriteLine("解码结论  : 通过");
        Console.WriteLine("RESULT verdict=PASS");
        return 0;
    }
}
