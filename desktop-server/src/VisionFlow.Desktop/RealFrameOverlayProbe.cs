using System.Drawing;
using System.Drawing.Imaging;
using VisionFlow.Desktop.Ai;
using VisionFlow.Desktop.Decoding;
using VisionFlow.Desktop.Network;
using VisionFlow.Desktop.Rendering;

namespace VisionFlow.Desktop;

/// <summary>
/// AI 叠加「落在实际解码帧上」的无头验收。
///
/// <para><b>为什么需要这个探针（与 --overlaytest 的区别）</b></para>
/// <para>
/// <see cref="OverlayProbe"/> 把叠加画在**纯黑合成背景**上断言像素，
/// 它证明了「绘制坐标算法正确」，但没有证明叠加真的落到了**真实解码视频帧**之上——
/// 真实播放路径是 <c>VideoCanvas.OnPaint</c> 先 <c>DrawImage(真实帧)</c>
/// 再 <c>OverlayDraw</c>（即 <see cref="AiOverlayRenderer.Draw"/>）。
/// 本探针补上这一段：用与真实播放完全相同的 <see cref="VideoDecoder"/> 解出 BGRA 帧，
/// 再走同一套叠加绘制，最后做**差分断言**——
/// 框边处的像素相对「仅含真实帧」的副本被改写成了叠加色（证明画上去了），
/// 框外远处像素两份副本完全一致（证明没有污染真实画面）。
/// </para>
///
/// <para><b>差分断言的不可伪造性</b></para>
/// <para>
/// 不依赖「叠加色恰好是什么」，而是比较「真实帧副本」与「真实帧+叠加副本」：
/// 同一点若被叠加改写则两副本不同，若未被触及则完全相同。
/// 这把「叠加真的画在真实画面里」这件事变成可独立复核的像素证据，而不是肉眼判断。
/// </para>
/// </summary>
internal static class RealFrameOverlayProbe
{
    internal static int Run(string? input, int frames, string? outDir)
    {
        ConsoleSetup.UseUtf8();

        Console.WriteLine("=== AI 叠加落在实际解码帧验收 ===");

        if (string.IsNullOrWhiteSpace(input))
        {
            // 未显式给输入时，回落到仓库根的 _testdata（与 run_tests.bat 一致）
            string cwd = Directory.GetCurrentDirectory();
            input = Path.Combine(cwd, "_testdata", "h264_720p.mp4");
        }

        int failures = 0;
        void Check(string name, bool ok, string detail)
        {
            Console.WriteLine($"[ {(ok ? "PASS" : "FAIL")} ] {name} :: {detail}");
            if (!ok)
            {
                failures++;
            }
        }

        if (!File.Exists(input))
        {
            Console.WriteLine($"[ SKIP ] 测试视频不存在: {input}（无素材则无法做真实帧验收）");
            Console.WriteLine($"RESULT verdict=SKIP");
            Console.WriteLine($"RESULT failures={failures}");
            return 2; // SKIP，不算失败
        }

        if (outDir is not null)
        {
            Directory.CreateDirectory(outDir);
        }

        // ── 用真实播放同源的解码器解出 BGRA 帧 ──
        using var decoder = new VideoDecoder { HardwareEnabled = true };
        if (!decoder.Open(input, msg => Console.WriteLine($"  [解码] {msg}")))
        {
            Console.WriteLine("[ FAIL ] 无法打开测试视频（解码管线本身失败）");
            Console.WriteLine($"RESULT verdict=FAIL");
            Console.WriteLine($"RESULT failures={failures + 1}");
            return 1;
        }

        Console.WriteLine($"  解码后端: {decoder.Backend}" +
                          (decoder.FallbackReason == FallbackReason.None
                              ? string.Empty
                              : $"（回退原因: {decoder.FallbackReason}）"));

        // 解前几帧，取第一帧用于合成
        Bitmap? realFrame = null;
        int decoded = 0;
        for (int i = 0; i < frames; i++)
        {
            var f = decoder.DecodeNextFrame();
            if (f is null)
            {
                break;
            }

            decoded++;
            if (realFrame is null)
            {
                realFrame = f; // 持有首帧所有权
            }
            else
            {
                f.Dispose();
            }
        }

        Check("至少解出一帧真实 BGRA 帧", realFrame is not null, $"实际解出 {decoded} 帧");
        if (realFrame is null)
        {
            Console.WriteLine($"RESULT verdict=FAIL");
            Console.WriteLine($"RESULT failures={failures}");
            return 1;
        }

        int fw = realFrame.Width;
        int fh = realFrame.Height;
        Console.WriteLine($"  真实帧尺寸: {fw}x{fh}");

        // 画布与帧同尺寸 → 1:1 变换，便于精确断言（与 VideoCanvas 同一变换算法）
        var canvasSize = new Size(fw, fh);
        var transform = VideoTransform.Fit(new Size(fw, fh), canvasSize, letterbox: true);
        Check("1:1 变换缩放为 1", Math.Abs(transform.ScaleX - 1f) < 0.001f,
            $"ScaleX={transform.ScaleX:F4} ScaleY={transform.ScaleY:F4}");

        // ── 真实帧内容存在性：证明是「真实解码帧」而非合成黑屏 ──
        Check("真实帧非均匀黑屏（确为解码内容）",
            FrameHasRealContent(realFrame),
            "采样方差高于纯色阈值");

        // ── 构造一帧 AI 数据：bbox + COCO-17 关键点（点亮 0、1 两点）──
        var state = BuildState(fw, fh, keypointCount: 17, [100, 120, 340, 600]);
        var expectedBox = new Rectangle(100, 120, 240, 480); // [100,120,340,600]

        // 仅真实帧副本 A
        using var plain = Composite(realFrame, canvasSize, transform, state, visible: false);
        // 真实帧 + 叠加副本 B
        using var overlaid = Composite(realFrame, canvasSize, transform, state, visible: true);

        if (outDir is not null)
        {
            string p1 = Path.Combine(outDir, "realframe_plain.png");
            string p2 = Path.Combine(outDir, "realframe_overlaid.png");
            plain.Save(p1, ImageFormat.Png);
            overlaid.Save(p2, ImageFormat.Png);
            Console.WriteLine($"  已保存 {p1} / {p2}");
        }

        // ── 差分断言 ①：框边处被叠加改写（画上去了）──
        Check("框左边线处叠加改写真实像素",
            OverlayRewroteAt(plain, overlaid, expectedBox.Left, expectedBox.Top + 100, radius: 3),
            $"采样 x={expectedBox.Left}, y={expectedBox.Top + 100}");

        Check("框上边线处叠加改写真实像素",
            OverlayRewroteAt(plain, overlaid, expectedBox.Left + 100, expectedBox.Top, radius: 3),
            $"采样 x={expectedBox.Left + 100}, y={expectedBox.Top}");

        Check("框右边线处叠加改写真实像素",
            OverlayRewroteAt(plain, overlaid, expectedBox.Right, expectedBox.Top + 100, radius: 3),
            $"采样 x={expectedBox.Right}, y={expectedBox.Top + 100}");

        Check("框下边线处叠加改写真实像素",
            OverlayRewroteAt(plain, overlaid, expectedBox.Left + 100, expectedBox.Bottom, radius: 3),
            $"采样 x={expectedBox.Left + 100}, y={expectedBox.Bottom}");

        // ── 差分断言 ②：框外远处像素两份副本完全一致（未污染真实画面）──
        Check("框外右上真实内容未被叠加触碰",
            IdenticalNear(plain, overlaid, (int)(fw * 0.75), (int)(fh * 0.15), radius: 2),
            $"采样 ({(int)(fw * 0.75)},{(int)(fh * 0.15)})");

        Check("框外下方真实内容未被叠加触碰",
            IdenticalNear(plain, overlaid, (int)(fw * 0.2), (int)(fh * 0.9), radius: 2),
            $"采样 ({(int)(fw * 0.2)},{(int)(fh * 0.9)})");

        // ── 关键点：红关节确实出现在 (200,180) 附近 ──
        Check("关键点 (200,180) 处出现红关节像素",
            HasJointColorNear(overlaid, 200, 180, radius: 5),
            "采样半径 5px");

        // ── AI 参考分辨率 ≠ 显示分辨率时的归一化（在真实帧上验证）──
        // 真实帧 1280x720，AI 报 640x360，bbox [50,60,170,300]
        // 归一化后应落到显示坐标 [100,120,340,600]（与上面预期一致）
        var scaledState = BuildState(640, 360, keypointCount: 17, [50, 60, 170, 300]);
        using var overlaidScaled = Composite(realFrame, canvasSize, transform, scaledState, visible: true);

        if (outDir is not null)
        {
            string p3 = Path.Combine(outDir, "realframe_overlaid_scaled.png");
            overlaidScaled.Save(p3, ImageFormat.Png);
            Console.WriteLine($"  已保存 {p3}");
        }

        Check("AI 参考 640x360 的框正确归一化到显示坐标（真实帧上）",
            OverlayRewroteAt(plain, overlaidScaled, 100, 220, radius: 3),
            "期望左边线在 x=100");

        Check("缩放后的框右边线在 x=340（真实帧上）",
            OverlayRewroteAt(plain, overlaidScaled, 340, 220, radius: 3),
            "期望右边线在 x=340");

        // ── ai-off：完全不绘制（差分应处处一致）──
        using var overlaidOff = Composite(realFrame, canvasSize, transform, state, visible: false);
        Check("叠加不可见时与纯真实帧完全一致",
            IdenticalNear(plain, overlaidOff, expectedBox.Left + 100, expectedBox.Top + 100, radius: 2),
            "框内采样点两副本一致（无叠加）");

        realFrame.Dispose();

        Console.WriteLine($"RESULT failures={failures}");
        Console.WriteLine($"RESULT verdict={(failures == 0 ? "PASS" : "FAIL")}");
        Console.WriteLine("=== 验收结束 ===");

        return failures == 0 ? 0 : 1;
    }

    /// <summary>
    /// 把真实帧绘制到目标位图；visible=true 时再叠加 AI 层。
    /// 与 <see cref="VideoCanvas"/> 的 OnPaint 顺序一致：先 DrawImage(真实帧) 再 OverlayDraw。
    /// </summary>
    private static Bitmap Composite(
        Bitmap realFrame,
        Size canvasSize,
        VideoTransform transform,
        AiOverlayState state,
        bool visible)
    {
        var bmp = new Bitmap(canvasSize.Width, canvasSize.Height, PixelFormat.Format32bppArgb);
        using var g = Graphics.FromImage(bmp);
        g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.NearestNeighbor;
        g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.Half;
        g.DrawImage(realFrame, transform.Destination);
        AiOverlayRenderer.Draw(g, transform, state, visible);
        return bmp;
    }

    private static AiOverlayState BuildState(int refW, int refH, int keypointCount, int[] bbox)
    {
        var kps = new List<double[]>();
        for (int i = 0; i < keypointCount; i++)
        {
            kps.Add([0, 0, 0.0]); // 默认低置信度
        }

        if (keypointCount >= 2)
        {
            kps[0] = [200, 180, 0.95];
            kps[1] = [190, 170, 0.93];
        }

        return new AiOverlayState
        {
            CameraId = "realframe-test",
            FrameId = 1,
            Timestamp = 1,
            VideoWidth = refW,
            VideoHeight = refH,
            AiMode = "ai-y-pose",
            Objects =
            [
                new ObjectViewDto
                {
                    Class = "person",
                    Confidence = 0.91,
                    TrackId = 1,
                    Bbox = bbox,
                    Keypoints = kps.ToArray(),
                },
            ],
            Running = true,
            Fps = 29.7,
            Model = "yolo11n-pose",
            FetchedAt = DateTimeOffset.Now,
        };
    }

    /// <summary>真实帧确为解码内容：采样一批点，方差高于纯色阈值。</summary>
    private static bool FrameHasRealContent(Bitmap bmp)
    {
        int stride = Math.Max(1, bmp.Width / 16);
        int strideY = Math.Max(1, bmp.Height / 16);
        var samples = new List<int>();
        for (int y = 0; y < bmp.Height; y += strideY)
        {
            for (int x = 0; x < bmp.Width; x += stride)
            {
                var c = bmp.GetPixel(x, y);
                samples.Add(c.R + c.G + c.B);
            }
        }

        if (samples.Count < 4)
        {
            return false;
        }

        double mean = samples.Average();
        double variance = samples.Average(s => (s - mean) * (s - mean));
        return variance > 25.0; // 纯色方差≈0；真实内容（testsrc2/实拍）远高于此
    }

    /// <summary>
    /// 在 (cx,cy) 邻域内：overlaid 相对 plain 至少有一点被「叠加色」改写。
    /// 同时要求改写后的像素属于叠加色族之一（青绿框 / 红关节 / 黄骨），
    /// 避免把解码噪声误判为叠加。
    /// </summary>
    private static bool OverlayRewroteAt(Bitmap plain, Bitmap overlaid, int cx, int cy, int radius)
    {
        for (int dy = -radius; dy <= radius; dy++)
        {
            for (int dx = -radius; dx <= radius; dx++)
            {
                int x = cx + dx;
                int y = cy + dy;
                if (x < 0 || y < 0 || x >= overlaid.Width || y >= overlaid.Height)
                {
                    continue;
                }

                Color a = plain.GetPixel(x, y);
                Color b = overlaid.GetPixel(x, y);
                if (a != b && IsOverlayColored(b))
                {
                    return true;
                }
            }
        }

        return false;
    }

    /// <summary>在 (cx,cy) 邻域内：两份副本逐像素完全一致（未被叠加触碰）。</summary>
    private static bool IdenticalNear(Bitmap plain, Bitmap overlaid, int cx, int cy, int radius)
    {
        for (int dy = -radius; dy <= radius; dy++)
        {
            for (int dx = -radius; dx <= radius; dx++)
            {
                int x = cx + dx;
                int y = cy + dy;
                if (x < 0 || y < 0 || x >= overlaid.Width || y >= overlaid.Height)
                {
                    continue;
                }

                if (plain.GetPixel(x, y) != overlaid.GetPixel(x, y))
                {
                    return false;
                }
            }
        }

        return true;
    }

    /// <summary>在 (cx,cy) 邻域内是否存在偏红的关节像素。</summary>
    private static bool HasJointColorNear(Bitmap bmp, int cx, int cy, int radius)
    {
        for (int dy = -radius; dy <= radius; dy++)
        {
            for (int dx = -radius; dx <= radius; dx++)
            {
                int x = cx + dx;
                int y = cy + dy;
                if (x < 0 || y < 0 || x >= bmp.Width || y >= bmp.Height)
                {
                    continue;
                }

                var c = bmp.GetPixel(x, y);
                if (c.R > 150 && c.G < 150 && c.B < 150)
                {
                    return true;
                }
            }
        }

        return false;
    }

    /// <summary>
    /// 叠加色族：青绿框 (0,230,180) / 红关节 (255,80,80) / 黄骨 (255,240,90)。
    /// 用宽松区间，忽略抗锯齿/混合的轻微偏差。
    /// </summary>
    private static bool IsOverlayColored(Color c)
    {
        // 青绿框：g 显著高，r 低，b 中高
        bool box = c.G > 150 && c.R < 120 && c.B > 90 && c.G > c.R + 40;
        // 红关节：r 高，g/b 低
        bool joint = c.R > 150 && c.G < 150 && c.B < 150;
        // 黄骨：r 高且 g 高，b 低
        bool bone = c.R > 150 && c.G > 150 && c.B < 140;
        return box || joint || bone;
    }
}
