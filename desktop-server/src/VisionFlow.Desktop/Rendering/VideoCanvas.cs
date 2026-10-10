using System.Drawing;
using System.Windows.Forms;

namespace VisionFlow.Desktop.Rendering;

/// <summary>
/// 视频显示画布。
/// <para>
/// 硬解与软解共用同一条呈现路径：解码端最终都产出 <see cref="Bitmap"/>（BGRA），
/// 由本控件双缓冲绘制。这样硬解→软解回退不需要切换渲染层，
/// 回退逻辑只存在于解码器内部。
/// </para>
/// <para>
/// 叠加层（AI 框、骨架）通过 <see cref="OverlayDraw"/> 回调在画面之后绘制，
/// 坐标以「视频原始像素」为单位，由本控件负责缩放到显示区域。
/// </para>
/// </summary>
internal sealed class VideoCanvas : Control
{
    private readonly object _sync = new();
    private Bitmap? _current;
    private Bitmap? _backBuffer;
    private Size _videoSize;

    public VideoCanvas()
    {
        SetStyle(
            ControlStyles.AllPaintingInWmPaint |
            ControlStyles.UserPaint |
            ControlStyles.OptimizedDoubleBuffer |
            ControlStyles.ResizeRedraw,
            true);

        BackColor = Color.Black;
    }

    /// <summary>叠加层绘制回调。参数：目标 Graphics、视频→显示变换、视频像素尺寸。</summary>
    public event Action<Graphics, VideoTransform>? OverlayDraw;

    /// <summary>显示区相对于视频应如何处理。当前策略：等比缩放 + 居中留边。</summary>
    [System.ComponentModel.DesignerSerializationVisibility(
        System.ComponentModel.DesignerSerializationVisibility.Hidden)]
    [System.ComponentModel.Browsable(false)]
    public bool Letterbox { get; set; } = true;

    /// <summary>已呈现的帧数（用于诊断）。</summary>
    [System.ComponentModel.DesignerSerializationVisibility(
        System.ComponentModel.DesignerSerializationVisibility.Hidden)]
    [System.ComponentModel.Browsable(false)]
    public long PresentedFrames { get; private set; }

    /// <summary>提交一帧。传入的 Bitmap 由本控件接管所有权，调用方不要再释放。</summary>
    public void Submit(Bitmap frame)
    {
        Bitmap? retire;
        lock (_sync)
        {
            retire = _current;
            _current = frame;
            _videoSize = frame.Size;
            PresentedFrames++;
        }

        retire?.Dispose();
        Invalidate();
    }

    /// <summary>清空画面（切换源 / 停止播放时调用）。</summary>
    public void Clear()
    {
        Bitmap? retire;
        lock (_sync)
        {
            retire = _current;
            _current = null;
            _videoSize = Size.Empty;
            PresentedFrames = 0;
        }

        retire?.Dispose();
        Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(BackColor);

        Bitmap? frame;
        Size vidSize;
        lock (_sync)
        {
            frame = _current;
            vidSize = _videoSize;
        }

        if (frame is null || vidSize.IsEmpty)
        {
            DrawPlaceholder(e.Graphics);
            return;
        }

        var transform = VideoTransform.Fit(vidSize, ClientSize, Letterbox);

        e.Graphics.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.NearestNeighbor;
        e.Graphics.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.Half;
        e.Graphics.DrawImage(frame, transform.Destination);

        OverlayDraw?.Invoke(e.Graphics, transform);
    }

    private void DrawPlaceholder(Graphics g)
    {
        if (ClientSize.Width < 40 || ClientSize.Height < 20)
        {
            return;
        }

        using var brush = new SolidBrush(Color.FromArgb(110, 110, 110));
        using var font = new Font("Consolas", 10f);
        using var fmt = new StringFormat
        {
            Alignment = StringAlignment.Center,
            LineAlignment = StringAlignment.Center,
        };
        g.DrawString("未选择视频源", font, brush, new RectangleF(0, 0, ClientSize.Width, ClientSize.Height), fmt);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            lock (_sync)
            {
                _current?.Dispose();
                _current = null;
                _backBuffer?.Dispose();
                _backBuffer = null;
            }
        }

        base.Dispose(disposing);
    }
}

/// <summary>视频像素坐标 → 控件客户区坐标的仿射变换（只含缩放与平移）。</summary>
internal readonly struct VideoTransform
{
    public Rectangle Destination { get; init; }

    public float ScaleX { get; init; }

    public float ScaleY { get; init; }

    public float OffsetX { get; init; }

    public float OffsetY { get; init; }

    public static VideoTransform Fit(Size video, Size canvas, bool letterbox)
    {
        if (video.Width <= 0 || video.Height <= 0 || canvas.Width <= 0 || canvas.Height <= 0)
        {
            return new VideoTransform
            {
                Destination = Rectangle.Empty,
                ScaleX = 1f,
                ScaleY = 1f,
            };
        }

        float sx = (float)canvas.Width / video.Width;
        float sy = (float)canvas.Height / video.Height;

        if (letterbox)
        {
            float s = Math.Min(sx, sy);
            sx = s;
            sy = s;
        }

        int w = (int)MathF.Round(video.Width * sx);
        int h = (int)MathF.Round(video.Height * sy);
        int ox = (canvas.Width - w) / 2;
        int oy = (canvas.Height - h) / 2;

        return new VideoTransform
        {
            Destination = new Rectangle(ox, oy, w, h),
            ScaleX = sx,
            ScaleY = sy,
            OffsetX = ox,
            OffsetY = oy,
        };
    }

    /// <summary>把视频像素点映射到控件坐标。</summary>
    public PointF Map(float x, float y) => new(OffsetX + x * ScaleX, OffsetY + y * ScaleY);
}
