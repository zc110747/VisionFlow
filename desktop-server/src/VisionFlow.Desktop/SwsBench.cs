using System.Diagnostics;
using FFmpeg.AutoGen;
using VisionFlow.Desktop.Decoding;
using VisionFlow.Desktop.Rendering;

namespace VisionFlow.Desktop;

/// <summary>
/// 色彩转换基准：<c>--swsbench</c>。
/// 构造固定的 NV12 4K 帧，分别用不同 sws 算法与线程配置测纯转换成本，
/// 用于判断「NV12→BGRA 该用什么参数」，不掺入解码开销。
/// </summary>
internal static unsafe class SwsBench
{
    private const int Width = 3840;
    private const int Height = 2160;
    private const int Iterations = 60;

    public static int Run()
    {
        try
        {
            Console.OutputEncoding = System.Text.Encoding.UTF8;
        }
        catch (IOException)
        {
        }

        FFmpegLoader.Initialize();

        Console.WriteLine("=========== 色彩转换基准 (NV12 → BGRA) ===========");
        Console.WriteLine($"分辨率: {Width}x{Height}  迭代: {Iterations}");
        Console.WriteLine();

        // 构造一个 NV12 源帧（内容不重要，只测转换吞吐）
        AVFrame* src = BuildSourceFrame();
        if (src is null)
        {
            Console.WriteLine("构造源帧失败");
            return 1;
        }

        Console.WriteLine($"源帧: NV12, linesize=[{src->linesize[0]}, {src->linesize[1]}]");
        Console.WriteLine();

        var results = new List<(string Label, double MsPerFrame)>();

        Measure(src, ref results, "BILINEAR (当前启用)", (int)SwsFlags.SWS_BILINEAR, 0);
        Measure(src, ref results, "FAST_BILINEAR", (int)SwsFlags.SWS_FAST_BILINEAR, 0);
        Measure(src, ref results, "POINT (无缩放时最快)", (int)SwsFlags.SWS_POINT, 0);
        Measure(src, ref results, "BICUBIC", (int)SwsFlags.SWS_BICUBIC, 0);

        Console.WriteLine();

        int cores = Environment.ProcessorCount;
        foreach (int t in new[] { 2, 4, cores })
        {
            Measure(src, ref results, $"FAST_BILINEAR + threads={t}", (int)SwsFlags.SWS_FAST_BILINEAR, t);
        }

        Console.WriteLine();
        Measure(src, ref results, $"BILINEAR + threads={cores}", (int)SwsFlags.SWS_BILINEAR, cores);

        Console.WriteLine();
        Console.WriteLine("=========== 结论 ===========");

        if (results.Count > 0)
        {
            var best = results.OrderBy(r => r.MsPerFrame).First();
            var worst = results.OrderByDescending(r => r.MsPerFrame).First();
            Console.WriteLine($"最快: {best.Label} ({best.MsPerFrame:F2} ms/帧)");
            Console.WriteLine($"最慢: {worst.Label} ({worst.MsPerFrame:F2} ms/帧)");
            Console.WriteLine($"最大可优化倍数: {worst.MsPerFrame / best.MsPerFrame:F2}×");
        }

        ffmpeg.av_frame_free(&src);
        return 0;
    }

    /// <summary>构造一个填充了内容的 NV12 帧。</summary>
    private static AVFrame* BuildSourceFrame()
    {
        AVFrame* src = ffmpeg.av_frame_alloc();
        if (src is null)
        {
            return null;
        }

        src->format = (int)AVPixelFormat.AV_PIX_FMT_NV12;
        src->width = Width;
        src->height = Height;

        if (ffmpeg.av_frame_get_buffer(src, 32) < 0)
        {
            ffmpeg.av_frame_free(&src);
            return null;
        }

        // 填内容，避免全零走异常快速路径
        for (int p = 0; p < 2; p++)
        {
            int planeH = p == 0 ? Height : Height / 2;
            // AutoGen 的 AVFrame::data / linesize 是固定数组封装类型，索引器参数为 uint
            int stride = (int)src->linesize[(uint)p];
            byte* basePtr = src->data[(uint)p];

            for (int y = 0; y < planeH; y++)
            {
                byte* row = basePtr + ((long)y * stride);
                for (int x = 0; x < stride; x++)
                {
                    row[x] = (byte)((x + y + p * 37) & 0xFF);
                }
            }
        }

        return src;
    }

    /// <summary>测一种 sws 配置，把耗时计入 <paramref name="results"/>。</summary>
    private static void Measure(
        AVFrame* src,
        ref List<(string Label, double MsPerFrame)> results,
        string label,
        int swsFlags,
        int threads)
    {
        SwsContext* sws = null;
        sws = ffmpeg.sws_getCachedContext(
            sws,
            Width, Height, AVPixelFormat.AV_PIX_FMT_NV12,
            Width, Height, AVPixelFormat.AV_PIX_FMT_BGRA,
            swsFlags, null, null, null);

        if (sws is null)
        {
            Console.WriteLine($"{label,-34} : sws 上下文创建失败");
            return;
        }

        if (threads > 0)
        {
            // 注意：sws 的 threads 选项在部分构建里不可用，失败要如实报出
            int rc = ffmpeg.av_opt_set_int(sws, "threads", threads, 0);
            if (rc < 0)
            {
                Console.WriteLine($"{label,-34} : 设置 threads={threads} 失败 {FFmpegError.Text(rc)}（该选项可能未编译进本构建）");
            }
        }

        byte* dst = (byte*)ffmpeg.av_malloc((ulong)(Width * Height * 4));
        if (dst is null)
        {
            Console.WriteLine($"{label,-34} : 目标缓冲分配失败");
            ffmpeg.sws_freeContext(sws);
            return;
        }

        byte*[] dstData = [dst];
        int[] dstLinesize = [Width * 4];

        // 预热，让 sws 完成内部初始化
        ffmpeg.sws_scale(sws, src->data, src->linesize, 0, Height, dstData, dstLinesize);

        var sw = Stopwatch.StartNew();
        for (int i = 0; i < Iterations; i++)
        {
            ffmpeg.sws_scale(sws, src->data, src->linesize, 0, Height, dstData, dstLinesize);
        }

        sw.Stop();

        double msPerFrame = sw.Elapsed.TotalMilliseconds / Iterations;
        results.Add((label, msPerFrame));

        Console.WriteLine($"{label,-34} : {msPerFrame,7:F2} ms/帧   ({1000 / msPerFrame,6:F1} fps)");

        ffmpeg.av_freep(&dst);
        ffmpeg.sws_freeContext(sws);
    }
}
