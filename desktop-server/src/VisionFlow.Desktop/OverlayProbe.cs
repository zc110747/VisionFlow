using System.Drawing;
using System.Drawing.Imaging;
using VisionFlow.Desktop.Ai;
using VisionFlow.Desktop.Network;
using VisionFlow.Desktop.Rendering;

namespace VisionFlow.Desktop;

/// <summary>
/// AI 叠加渲染无头验收。
///
/// <para><b>为什么要断言像素而不是「看一眼」</b></para>
/// <para>
/// 叠加层最容易出的错是**坐标错位**（框画到别处）与**该画却没画**，
/// 这两种在截图里都「看着像 AI 不准」，肉眼无法区分是模型问题还是渲染问题。
/// 这里把画面渲染到内存位图，然后按预期坐标采样像素：
/// 框边位置必须是检测框颜色，框外必须是背景色。
/// </para>
/// </summary>
internal static class OverlayProbe
{
    internal static int Run(string? outDir)
    {
        ConsoleSetup.UseUtf8();

        Console.WriteLine("=== AI 叠加渲染验收 ===");

        int failures = 0;
        void Check(string name, bool ok, string detail)
        {
            Console.WriteLine($"[ {(ok ? "PASS" : "FAIL")} ] {name} :: {detail}");
            if (!ok)
            {
                failures++;
            }
        }

        // ── 拓扑表自检（不依赖网络）──
        var coco = SkeletonTopologies.ForPointCount(17);
        Check("拓扑 COCO-17 匹配", coco is not null && coco.Name == "COCO-17",
            coco?.Name ?? "(null)");

        var hand = SkeletonTopologies.ForPointCount(21);
        Check("拓扑 HAND-21 匹配", hand is not null && hand.Name == "HAND-21",
            hand?.Name ?? "(null)");

        // 关键：未知点数必须返回 null（跳过绘制），不能猜一个拓扑
        var unknown = SkeletonTopologies.ForPointCount(5);
        Check("未知点数(5)不匹配任何拓扑", unknown is null, unknown?.Name ?? "(null，正确)");

        var unknown0 = SkeletonTopologies.ForPointCount(0);
        Check("点数 0 不匹配任何拓扑", unknown0 is null, unknown0?.Name ?? "(null，正确)");

        // 连线索引必须在范围内
        bool cocoIdxOk = coco is not null && coco.Bones.All(b => b.From < 17 && b.To < 17);
        Check("COCO-17 连线索引合法", cocoIdxOk, cocoIdxOk ? "全部 < 17" : "存在越界索引");

        bool handIdxOk = hand is not null && hand.Bones.All(b => b.From < 21 && b.To < 21);
        Check("HAND-21 连线索引合法", handIdxOk, handIdxOk ? "全部 < 21" : "存在越界索引");

        // ── 像素级渲染验证 ──
        if (outDir is not null)
        {
            Directory.CreateDirectory(outDir);
        }

        // 视频 1280x720，画布也用同尺寸 → 变换为 1:1，便于精确断言
        const int vw = 1280;
        const int vh = 720;
        var canvasSize = new Size(vw, vh);
        var transform = VideoTransform.Fit(new Size(vw, vh), canvasSize, letterbox: true);

        Check("1:1 变换缩放为 1", Math.Abs(transform.ScaleX - 1f) < 0.001f,
            $"ScaleX={transform.ScaleX:F4} ScaleY={transform.ScaleY:F4}");

        // 构造一帧：一个目标，bbox 与 COCO17 关键点
        var state = BuildTestFrame(vw, vh, keypointCount: 17);

        using (var bmp = RenderOverlay(state, canvasSize, transform, width: vw, height: vh))
        {
            if (outDir is not null)
            {
                string path = Path.Combine(outDir, "overlay_coco17.png");
                bmp.Save(path, ImageFormat.Png);
                Console.WriteLine($"  已保存 {path}");
            }

            // bbox 是 [100,120,340,600] → 1:1 下应画在这些像素上
            var expectedBox = new Rectangle(100, 120, 240, 480);

            Check("框左边线处有像素",
                HasNonBackgroundNear(bmp, expectedBox.Left, expectedBox.Top + 100, radius: 2),
                $"采样 x={expectedBox.Left}, y={expectedBox.Top + 100}");

            Check("框上边线处有像素",
                HasNonBackgroundNear(bmp, expectedBox.Left + 100, expectedBox.Top, radius: 2),
                $"采样 x={expectedBox.Left + 100}, y={expectedBox.Top}");

            Check("框右边线处有像素",
                HasNonBackgroundNear(bmp, expectedBox.Right, expectedBox.Top + 100, radius: 2),
                $"采样 x={expectedBox.Right}, y={expectedBox.Top + 100}");

            Check("框下边线处有像素",
                HasNonBackgroundNear(bmp, expectedBox.Left + 100, expectedBox.Bottom, radius: 2),
                $"采样 x={expectedBox.Left + 100}, y={expectedBox.Bottom}");

            // 框外（远离框）必须还是背景 —— 证明没有「画满全屏」
            Check("框外右侧保持背景",
                IsBackground(bmp, 900, 360),
                "采样 (900,360) 应为背景");

            Check("框外左下保持背景",
                IsBackground(bmp, 50, 700),
                "采样 (50,700) 应为背景");

            // 骨架关节：关键点 (200,180) 处应有红色关节像素
            Check("关键点 (200,180) 处有关节像素",
                HasRedJointNear(bmp, 200, 180, radius: 5),
                "采样半径 5px");
        }

        // ── 关节点数与拓扑不匹配时必须跳过骨架 ──
        var mismatched = BuildTestFrame(vw, vh, keypointCount: 7);
        using (var bmp = RenderOverlay(mismatched, canvasSize, transform, width: vw, height: vh))
        {
            if (outDir is not null)
            {
                string path = Path.Combine(outDir, "overlay_mismatch7.png");
                bmp.Save(path, ImageFormat.Png);
                Console.WriteLine($"  已保存 {path}");
            }

            // 框应照画
            Check("点数不匹配时框仍然绘制",
                HasNonBackgroundNear(bmp, 100, 220, radius: 2),
                "框左边线仍存在");

            // 但关键点处不应有关节色（骨架被跳过）
            bool jointDrawn = HasRedJointNear(bmp, 200, 180, radius: 5);
            Check("点数不匹配时骨架被跳过", !jointDrawn,
                jointDrawn ? "错误：仍然画了关节" : "未画关节（正确）");
        }

        // ── 参考分辨率与显示分辨率不一致时的坐标归一化 ──
        // AI 参考 640x360，显示 1280x720（2 倍）。bbox [50,60,170,300]
        // 归一化后应落在显示坐标 [100,120,340,600]
        var scaledState = new AiOverlayState
        {
            CameraId = "scale-test",
            FrameId = 1,
            VideoWidth = 640,
            VideoHeight = 360,
            AiMode = "ai-y",
            Objects =
            [
                new ObjectViewDto
                {
                    Class = "person",
                    Confidence = 0.9,
                    TrackId = 0,
                    Bbox = [50, 60, 170, 300],
                },
            ],
            FetchedAt = DateTimeOffset.Now,
        };

        using (var bmp = RenderOverlay(scaledState, canvasSize, transform, width: vw, height: vh))
        {
            if (outDir is not null)
            {
                string path = Path.Combine(outDir, "overlay_scaled.png");
                bmp.Save(path, ImageFormat.Png);
                Console.WriteLine($"  已保存 {path}");
            }

            Check("参考分辨率 640x360 的框正确缩放到显示坐标",
                HasNonBackgroundNear(bmp, 100, 220, radius: 2),
                "期望左边线在 x=100");

            Check("缩放后的框右边线在 x=340",
                HasNonBackgroundNear(bmp, 340, 220, radius: 2),
                "期望右边线在 x=340");
        }

        // ── ai-off 时不应绘制任何东西 ──
        using (var bmp = RenderOverlay(state, canvasSize, transform, width: vw, height: vh, visible: false))
        {
            bool anyDrawn =
                HasNonBackgroundNear(bmp, 100, 220, radius: 2) ||
                HasRedJointNear(bmp, 200, 180, radius: 5);

            Check("叠加不可见时完全不绘制", !anyDrawn,
                anyDrawn ? "错误：仍然绘制了内容" : "无绘制（正确）");
        }

        Console.WriteLine($"RESULT failures={failures}");
        Console.WriteLine($"RESULT verdict={(failures == 0 ? "PASS" : "FAIL")}");
        Console.WriteLine("=== 验收结束 ===");

        return failures == 0 ? 0 : 1;
    }

    private static AiOverlayState BuildTestFrame(int w, int h, int keypointCount)
    {
        // COCO 布局下给几个有意义的关节；其余给低置信度（应被过滤）
        var kps = new List<double[]>();
        for (int i = 0; i < keypointCount; i++)
        {
            kps.Add([0, 0, 0.0]);   // 默认低置信度
        }

        // 只点亮 0(=200,180) 与 1(=190,170)，两者之间有连线
        if (keypointCount >= 2)
        {
            kps[0] = [200, 180, 0.95];
            kps[1] = [190, 170, 0.93];
        }

        return new AiOverlayState
        {
            CameraId = "overlay-test",
            FrameId = 42,
            Timestamp = 12345,
            VideoWidth = w,
            VideoHeight = h,
            AiMode = "ai-y-pose",
            Objects =
            [
                new ObjectViewDto
                {
                    Class = "person",
                    Confidence = 0.91,
                    TrackId = 1,
                    Bbox = [100, 120, 340, 600],
                    Keypoints = kps.ToArray(),
                },
            ],
            Running = true,
            Fps = 29.7,
            Model = "yolo11n-pose",
            FetchedAt = DateTimeOffset.Now,
        };
    }

    private static Bitmap RenderOverlay(
        AiOverlayState state,
        Size canvasSize,
        VideoTransform transform,
        int width,
        int height,
        bool visible = true)
    {
        var bmp = new Bitmap(width, height, PixelFormat.Format32bppArgb);
        using var g = Graphics.FromImage(bmp);

        // 背景用纯黑，与 VideoCanvas 的 BackColor 一致
        g.Clear(Color.Black);

        AiOverlayRenderer.Draw(g, transform, state, visible);
        return bmp;
    }

    /// <summary>在给定点附近是否存在非背景像素。</summary>
    private static bool HasNonBackgroundNear(Bitmap bmp, int cx, int cy, int radius)
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
                if (!IsBackgroundColor(c))
                {
                    return true;
                }
            }
        }

        return false;
    }

    private static bool IsBackground(Bitmap bmp, int x, int y)
    {
        if (x < 0 || y < 0 || x >= bmp.Width || y >= bmp.Height)
        {
            return true;
        }

        return IsBackgroundColor(bmp.GetPixel(x, y));
    }

    /// <summary>背景定义为黑色（各通道 < 24）。</summary>
    private static bool IsBackgroundColor(Color c) => c.R < 24 && c.G < 24 && c.B < 24;

    /// <summary>在给定点附近是否存在「偏红」的关节像素。</summary>
    private static bool HasRedJointNear(Bitmap bmp, int cx, int cy, int radius)
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
                // 关节色 (255,80,80)：红显著高于绿蓝
                if (c.R > 150 && c.G < 150 && c.B < 150)
                {
                    return true;
                }
            }
        }

        return false;
    }
}
