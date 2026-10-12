using System.Drawing;
using System.Drawing.Drawing2D;
using VisionFlow.Desktop.Rendering;
using VisionFlow.Desktop.Server;

namespace VisionFlow.Desktop.Ai;

/// <summary>
/// AI 叠加层绘制。
///
/// <para><b>坐标系：两套尺寸，绝不能混用</b></para>
/// <para>
/// bbox 与 keypoints 的坐标是 **AI 帧的原始视频像素**
/// （<c>frame.video_width x frame.video_height</c>），
/// 而画面显示尺寸是**解码器解出的视频尺寸**
/// （<c>VideoCanvas</c> 里那张 Bitmap 的大小）。
/// </para>
/// <para>
/// 两者通常相同，但**没有任何机制保证相同**：
/// agent 可能对 1920x1080 采集做 640x384 推理（letterbox），
/// 也可能元数据比画面晚到一帧而处于切换分辨率的瞬间。
/// 因此这里先把 AI 坐标按其自身参考系**归一化**，再乘到显示变换上，
/// 而不是直接 <c>Map(x, y)</c>。直接映射会在分辨率不一致时
/// 把框画到完全错误的位置，且看起来「像是 AI 不准」，极难排查。
/// </para>
///
/// <para><b>数据驱动拓扑</b></para>
/// <para>
/// 骨架连线来自 <see cref="SkeletonTopologies"/>。关节点数与任何已知拓扑
/// 都不匹配时**跳过该对象的骨架**（仍画框），
/// 而不是猜一个拓扑连线——错位骨架比没有骨架更有误导性。
/// 详见 <see cref="SkeletonTopology"/>。
/// </para>
/// </summary>
internal static class AiOverlayRenderer
{
    // ── 视觉参数 ──

    /// <summary>检测框颜色（青绿，与深色画面/深色 UI 协调且不刺眼）。</summary>
    private static readonly Color BoxColor = Color.FromArgb(0, 230, 180);

    /// <summary>标签背景色。</summary>
    private static readonly Color LabelBackColor = Color.FromArgb(200, 0, 60, 50);

    /// <summary>骨架连线颜色。</summary>
    private static readonly Color BoneColor = Color.FromArgb(255, 240, 90);

    /// <summary>关键点颜色。</summary>
    private static readonly Color JointColor = Color.FromArgb(255, 80, 80);

    /// <summary>关键点低于该置信度不绘制（模型输出的关节置信度，[0,1]）。</summary>
    private const double KeypointConfidenceFloor = 0.35;

    /// <summary>低于该置信度的目标整框不绘制，避免噪声框糊屏。</summary>
    private const double BoxConfidenceFloor = 0.25;

    /// <summary>
    /// 绘制一帧的 AI 叠加。
    /// </summary>
    /// <param name="g">目标 Graphics（已设置好变换）。</param>
    /// <param name="transform">视频像素 → 控件坐标的变换。</param>
    /// <param name="state">AI 数据。</param>
    /// <param name="visible">叠加是否可见（ai-off 或未选中网络相机时为 false）。</param>
    public static void Draw(Graphics g, VideoTransform transform, AiOverlayState state, bool visible)
    {
        if (!visible || state.Objects.Count == 0)
        {
            return;
        }

        // 该帧自己的参考系。为 0 说明是心跳-only 或旧 agent，无法定位 → 不画
        int refW = state.VideoWidth;
        int refH = state.VideoHeight;
        if (refW <= 0 || refH <= 0)
        {
            return;
        }

        var previousSmoothing = g.SmoothingMode;
        g.SmoothingMode = SmoothingMode.AntiAlias;

        try
        {
            using var boxPen = new Pen(BoxColor, 2f);
            using var bonePen = new Pen(BoneColor, 2f);
            using var jointBrush = new SolidBrush(JointColor);

            foreach (var obj in state.Objects)
            {
                if (obj.Confidence < BoxConfidenceFloor)
                {
                    continue;
                }

                DrawBox(g, transform, obj, refW, refH, boxPen);

                if (obj.Keypoints is { Length: > 0 } kps)
                {
                    DrawSkeleton(g, transform, kps, refW, refH, bonePen, jointBrush);
                }
            }
        }
        finally
        {
            g.SmoothingMode = previousSmoothing;
        }
    }

    private static void DrawBox(
        Graphics g,
        VideoTransform transform,
        MetadataObject obj,
        int refW,
        int refH,
        Pen pen)
    {
        if (obj.Bbox.Length != 4)
        {
            return;
        }

        // bbox = [x1,y1,x2,y2]，原始视频像素，从不归一化
        var tl = MapNormalized(obj.Bbox[0], obj.Bbox[1], refW, refH, transform);
        var br = MapNormalized(obj.Bbox[2], obj.Bbox[3], refW, refH, transform);

        var rect = RectangleF.FromLTRB(tl.X, tl.Y, br.X, br.Y);
        if (rect.Width <= 0 || rect.Height <= 0)
        {
            return;
        }

        g.DrawRectangle(pen, rect.X, rect.Y, rect.Width, rect.Height);

        // 标签：类别 + 置信度 + 跟踪号
        string label = obj.TrackId > 0
            ? $"{obj.Class} #{obj.TrackId} {obj.Confidence:P0}"
            : $"{obj.Class} {obj.Confidence:P0}";

        using var font = new Font("Consolas", 8.5f);
        var textSize = g.MeasureString(label, font);

        // 标签贴在框左上角外侧；太靠上就移到框内侧，避免被裁掉
        float labelY = rect.Y - textSize.Height - 2;
        if (labelY < 0)
        {
            labelY = rect.Y + 2;
        }

        using var back = new SolidBrush(LabelBackColor);
        g.FillRectangle(back, rect.X, labelY, textSize.Width + 6, textSize.Height);

        using var textBrush = new SolidBrush(Color.White);
        g.DrawString(label, font, textBrush, rect.X + 3, labelY);
    }

    private static void DrawSkeleton(
        Graphics g,
        VideoTransform transform,
        double[][] keypoints,
        int refW,
        int refH,
        Pen bonePen,
        Brush jointBrush)
    {
        // 按实际点数选拓扑；无匹配则跳过骨架（不猜）
        var topology = SkeletonTopologies.ForPointCount(keypoints.Length);
        if (topology is null)
        {
            return;
        }

        // 先算所有可见关节的显示坐标（不可见的用 null 占位）
        var points = new PointF?[keypoints.Length];
        for (int i = 0; i < keypoints.Length; i++)
        {
            var kp = keypoints[i];
            if (kp.Length < 3)
            {
                continue;
            }

            double conf = kp[2];

            // conf 可能是 NaN（模型未输出），NaN 与任何比较都为 false，
            // 这里显式判掉，否则 NaN 会滑过阈值比较被画出来。
            if (double.IsNaN(conf) || conf < KeypointConfidenceFloor)
            {
                continue;
            }

            points[i] = MapNormalized(kp[0], kp[1], refW, refH, transform);
        }

        // 画骨：两端关节都可见才画
        foreach (var (from, to) in topology.Bones)
        {
            if (from >= points.Length || to >= points.Length)
            {
                continue;
            }

            var a = points[from];
            var b = points[to];
            if (a is null || b is null)
            {
                continue;
            }

            g.DrawLine(bonePen, a.Value, b.Value);
        }

        // 画关节
        const float jointRadius = 3f;
        for (int i = 0; i < points.Length; i++)
        {
            if (points[i] is not { } p)
            {
                continue;
            }

            g.FillEllipse(
                jointBrush,
                p.X - jointRadius,
                p.Y - jointRadius,
                jointRadius * 2,
                jointRadius * 2);
        }
    }

    /// <summary>
    /// 把「AI 参考系下的像素坐标」映射到控件坐标。
    ///
    /// <para>
    /// 先按 AI 自身的参考尺寸归一化到 [0,1]，再乘显示变换的缩放与偏移。
    /// 这样即使 AI 参考分辨率与显示视频分辨率不同，位置依然正确。
    /// </para>
    /// </summary>
    private static PointF MapNormalized(
        double x,
        double y,
        int refW,
        int refH,
        VideoTransform transform)
    {
        float nx = (float)(x / refW);
        float ny = (float)(y / refH);

        return new PointF(
            transform.OffsetX + nx * transform.Destination.Width,
            transform.OffsetY + ny * transform.Destination.Height);
    }
}
