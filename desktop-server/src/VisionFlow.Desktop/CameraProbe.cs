using System.Drawing;
using System.Drawing.Imaging;
using VisionFlow.Desktop.Capture;

namespace VisionFlow.Desktop;

/// <summary>
/// 无头摄像头验收：枚举设备 → 打开第 N 个 → 连续抓若干帧 → 存盘 + 一致性统计。
///
/// <para>
/// 输出机器可读的 <c>RESULT key=value</c> 行，供 run_tests.bat 断言。
/// 无摄像头环境下 <c>RESULT verdict=NO_DEVICE</c>（不算失败，属于环境限制）。
/// </para>
/// </summary>
internal static class CameraProbe
{
    internal static int Run(int deviceIndex, int frames, string? outDir)
    {
        ConsoleSetup.UseUtf8();

        Console.WriteLine("=== 摄像头采集探针 ===");
        Console.WriteLine($"设备索引: {deviceIndex}");
        Console.WriteLine($"目标帧数: {frames}");

        // ── 枚举 ──
        var devices = CameraEnumerator.Enumerate(out var diag);
        Console.WriteLine($"--- 枚举诊断（{diag.Count} 行）---");
        foreach (var line in diag)
        {
            Console.WriteLine("  " + line);
        }

        Console.WriteLine($"RESULT devices={devices.Count}");

        if (devices.Count == 0)
        {
            Console.WriteLine("本机未发现视频采集设备（或隐私设置未授权）。");
            Console.WriteLine("RESULT verdict=NO_DEVICE");
            Console.WriteLine("=== 探针结束 ===");
            return 0;
        }

        Console.WriteLine("--- 枚举结果 ---");
        foreach (var d in devices)
        {
            Console.WriteLine($"  [{d.Index}] {d.FriendlyName}");
            Console.WriteLine($"      link={d.SymbolicLink}");
        }

        if (deviceIndex < 0 || deviceIndex >= devices.Count)
        {
            Console.WriteLine($"设备索引 {deviceIndex} 越界");
            Console.WriteLine("RESULT verdict=BAD_INDEX");
            return 2;
        }

        Console.WriteLine($"选中: [{deviceIndex}] {devices[deviceIndex].FriendlyName}");

        // ── 打开 ──
        var capture = CameraEnumerator.Open(deviceIndex, Console.WriteLine);
        if (capture is null)
        {
            Console.WriteLine("打开设备失败（可能被其他程序占用）。");
            Console.WriteLine("RESULT opened=False");
            Console.WriteLine("RESULT verdict=OPEN_FAILED");
            return 1;
        }

        Console.WriteLine("RESULT opened=True");
        Console.WriteLine($"RESULT width={capture.Width}");
        Console.WriteLine($"RESULT height={capture.Height}");
        Console.WriteLine($"RESULT pixfmt={capture.PixelFormat}");
        Console.WriteLine($"RESULT fps={capture.FrameRate:F2}");

        if (outDir is not null)
        {
            Directory.CreateDirectory(outDir);
        }

        // ── 抓帧 ──
        int got = 0;
        int nullReads = 0;
        int consecutiveNull = 0;
        int maxConsecutiveNull = 0;
        long totalBytes = 0;
        var firstHash = 0L;
        int distinctHashes = 0;
        long lastHash = 0;
        var sw = System.Diagnostics.Stopwatch.StartNew();

        while (got < frames && !capture.EndOfStream)
        {
            var bmp = capture.ReadFrame();
            if (bmp is null)
            {
                nullReads++;
                consecutiveNull++;
                maxConsecutiveNull = Math.Max(maxConsecutiveNull, consecutiveNull);

                if (consecutiveNull > 120)
                {
                    Console.WriteLine("连续 120 次读不到帧，判定设备无输出，中止。");
                    break;
                }

                Thread.Sleep(2);
                continue;
            }

            consecutiveNull = 0;
            totalBytes += (long)bmp.Width * bmp.Height * 4;
            got++;

            // 用像素抽样算一个简单指纹，判断画面是否在变化（防止「读到全黑帧」假通过）
            long hash = QuickHash(bmp);
            if (got == 1)
            {
                firstHash = hash;
                lastHash = hash;
                distinctHashes = 1;
            }
            else if (hash != lastHash)
            {
                distinctHashes++;
                lastHash = hash;
            }

            if (outDir is not null && (got == 1 || got == frames))
            {
                string path = Path.Combine(outDir, $"camera_frame_{got:D3}.bmp");
                bmp.Save(path, ImageFormat.Bmp);
                Console.WriteLine($"  已保存 {path}  ({bmp.Width}x{bmp.Height})");
            }

            bmp.Dispose();
        }

        sw.Stop();

        double elapsedSec = sw.Elapsed.TotalSeconds;
        double effectiveFps = elapsedSec > 0 ? got / elapsedSec : 0;

        Console.WriteLine($"--- 抓帧统计 ---");
        Console.WriteLine($"  成功帧数      : {got}");
        Console.WriteLine($"  空读次数      : {nullReads}（连续最多 {maxConsecutiveNull}）");
        Console.WriteLine($"  耗时          : {elapsedSec:F3} s");
        Console.WriteLine($"  有效帧率      : {effectiveFps:F2} fps");
        Console.WriteLine($"  画面变化次数  : {distinctHashes}");
        Console.WriteLine($"  EndOfStream   : {capture.EndOfStream}");

        Console.WriteLine($"RESULT frames={got}");
        Console.WriteLine($"RESULT nullreads={nullReads}");
        Console.WriteLine($"RESULT distinct={distinctHashes}");
        Console.WriteLine($"RESULT eos={capture.EndOfStream}");

        // 判定：抓满目标帧数即通过；画面是否变化单独报告，
        // 不参与 PASS/FAIL（对着白墙不动也会得到单一画面，属正常）
        bool pass = got >= frames && !capture.EndOfStream;
        Console.WriteLine($"RESULT verdict={(pass ? "PASS" : "FAIL")}");

        capture.Dispose();

        Console.WriteLine("=== 探针结束 ===");
        return pass ? 0 : 1;
    }

    /// <summary>对位图做稀疏抽样哈希，用于判断画面是否变化。</summary>
    private static unsafe long QuickHash(Bitmap bmp)
    {
        var rect = new Rectangle(0, 0, bmp.Width, bmp.Height);
        var data = bmp.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try
        {
            long h = 17;
            int stride = data.Stride;
            byte* basePtr = (byte*)data.Scan0;

            // 每 16 行取一行，每 16 列取一列，够敏感又不慢
            for (int y = 0; y < bmp.Height; y += 16)
            {
                byte* row = basePtr + y * stride;
                for (int x = 0; x < bmp.Width; x += 16)
                {
                    byte* px = row + x * 4;
                    h = h * 31 + px[0];
                    h = h * 31 + px[1];
                    h = h * 31 + px[2];
                }
            }

            return h;
        }
        finally
        {
            bmp.UnlockBits(data);
        }
    }
}
