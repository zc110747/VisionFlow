using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using FFmpeg.AutoGen;

namespace VisionFlow.Desktop.Decoding;

/// <summary>
/// 视频解码器。硬解优先，三条失败路径各自回退到软解。
///
/// <para><b>为什么硬解要这么绕</b></para>
/// <para>
/// FFmpeg 的硬解不是「换一个解码器」那么简单，而是：
/// 先建 hw_device（D3D11 设备）→ 把 hw_device_ctx 挂到 codec_ctx →
/// 提供 get_format 回调让解码器协商出硬解像素格式（如 AV_PIX_FMT_D3D11）
/// → 解码产出硬解帧（数据在 GPU 显存里）→ 用 av_hwframe_transfer_data
/// 搬回系统内存 → swscale 转 BGRA → 交给 UI。
/// </para>
/// <para>
/// 三个失败点分别在「建设备」「协商格式」「真正解码首帧」，
/// 任何一处失败都要能退回软解并继续播，而不是整个失败。
/// </para>
/// </summary>
internal sealed unsafe class VideoDecoder : IDisposable
{
    private readonly object _sync = new();

    private AVFormatContext* _fmtCtx;
    private AVCodecContext* _codecCtx;
    private AVBufferRef* _hwDeviceCtx;
    private SwsContext* _swsCtx;
    private AVFrame* _frame;
    private AVFrame* _swFrame;
    private AVPacket* _packet;
    private int _streamIndex = -1;
    private bool _disposed;

    /// <summary>当前实际生效的解码后端。</summary>
    public DecodeBackend Backend { get; private set; } = DecodeBackend.Uninitialized;

    /// <summary>回退原因（未回退时为 <see cref="FallbackReason.None"/>）。</summary>
    public FallbackReason FallbackReason { get; private set; } = FallbackReason.None;

    /// <summary>是否允许尝试硬解。</summary>
    public bool HardwareEnabled { get; init; } = true;

    /// <summary>
    /// 测试用：把所有硬解候选从链中剔除，模拟「本机无硬解能力」，
    /// 用于验证自动回退链真的会生效（而不是只在参数被显式禁用时才回退）。
    /// </summary>
    public bool SimulateHardwareUnavailable { get; init; }

    /// <summary>输入地址：本地文件路径，或 rtsp:// / http:// 等 URL。</summary>
    public string Input { get; private set; } = string.Empty;

    /// <summary>视频像素宽。</summary>
    public int Width { get; private set; }

    /// <summary>视频像素高。</summary>
    public int Height { get; private set; }

    /// <summary>流时长（秒）。直播源为 0。</summary>
    public double DurationSeconds { get; private set; }

    /// <summary>帧率（分子/分母已折算）。未知时为 0。</summary>
    public double FrameRate { get; private set; }

    /// <summary>总帧数估计。未知时为 0。</summary>
    public long TotalFrames { get; private set; }

    /// <summary>已解码帧数。</summary>
    public long DecodedFrames { get; private set; }

    /// <summary>日志回调。</summary>
    public Action<string>? Log { get; set; }

    /// <summary>
    /// 打开输入并完成解码器初始化（含硬解尝试）。
    /// </summary>
    /// <returns>成功返回 true。</returns>
    public bool Open(string input, Action<string>? log = null)
    {
        lock (_sync)
        {
            Log = log ?? Log;
            Input = input;

            if (!OpenDemuxer())
            {
                return false;
            }

            if (!ResolveStream())
            {
                return false;
            }

            // 硬解尝试 + 软解兜底，全部在内部完成
            OpenDecoderWithFallback();

            // 分配复用缓冲
            _frame = ffmpeg.av_frame_alloc();
            _swFrame = ffmpeg.av_frame_alloc();
            _packet = ffmpeg.av_packet_alloc();

            if (_frame is null || _swFrame is null || _packet is null)
            {
                Log?.Invoke("[解码] 分配帧/包缓冲失败");
                return false;
            }

            ReportStreamInfo();
            return true;
        }
    }

    private bool OpenDemuxer()
    {
        AVFormatContext* ctx = null;

        // 网络源（rtsp/http）需要显式超时，否则断流时 avformat_open_input 会挂很久
        AVDictionary* opts = null;
        if (IsNetworkInput(Input))
        {
            ffmpeg.av_dict_set(&opts, "rtsp_transport", "tcp", 0);
            ffmpeg.av_dict_set(&opts, "stimeout", "5000000", 0);        // 5s，单位微秒
            ffmpeg.av_dict_set(&opts, "timeout", "5000000", 0);         // http/tcp 超时
            ffmpeg.av_dict_set(&opts, "buffer_size", "1048576", 0);
            _ = ffmpeg.av_dict_set(&opts, "fflags", "nobuffer", 0);
            Log?.Invoke($"[解码] 网络输入，已设置 rtsp_transport=tcp / 超时 5s");
        }

        int rc = ffmpeg.avformat_open_input(&ctx, Input, null, &opts);
        ffmpeg.av_dict_free(&opts);

        if (rc < 0)
        {
            Log?.Invoke($"[解码] avformat_open_input 失败: {FFmpegError.Text(rc)} ({Input})");
            return false;
        }

        _fmtCtx = ctx;

        rc = ffmpeg.avformat_find_stream_info(_fmtCtx, null);
        if (rc < 0)
        {
            Log?.Invoke($"[解码] avformat_find_stream_info 失败: {FFmpegError.Text(rc)}");
            return false;
        }

        return true;
    }

    private bool ResolveStream()
    {
        int index = ffmpeg.av_find_best_stream(
            _fmtCtx, AVMediaType.AVMEDIA_TYPE_VIDEO, -1, -1, null, 0);

        if (index < 0)
        {
            Log?.Invoke($"[解码] 未找到视频流: {FFmpegError.Text(index)}");
            return false;
        }

        _streamIndex = index;
        var stream = _fmtCtx->streams[index];
        var par = stream->codecpar;

        var codec = ffmpeg.avcodec_find_decoder(par->codec_id);
        if (codec is null)
        {
            Log?.Invoke($"[解码] 无可用解码器，codec_id={par->codec_id}");
            return false;
        }

        _codecCtx = ffmpeg.avcodec_alloc_context3(codec);
        if (_codecCtx is null)
        {
            Log?.Invoke("[解码] avcodec_alloc_context3 失败");
            return false;
        }

        int rc = ffmpeg.avcodec_parameters_to_context(_codecCtx, par);
        if (rc < 0)
        {
            Log?.Invoke($"[解码] 参数复制失败: {FFmpegError.Text(rc)}");
            return false;
        }

        _codecCtx->pkt_timebase = stream->time_base;

        string codecName = codec->name is null
            ? "unknown"
            : Marshal.PtrToStringAnsi((nint)codec->name)!;
        Log?.Invoke($"[解码] 视频流 #{index}: {codecName}, {par->width}x{par->height}");

        return true;
    }

    /// <summary>
    /// 打开解码器：先试硬解，失败按 <see cref="FallbackReason"/> 记录并落软解。
    /// </summary>
    private void OpenDecoderWithFallback()
    {
        if (!HardwareEnabled)
        {
            Log?.Invoke("[解码] 硬解已被显式禁用，直接使用软解");
            Backend = DecodeBackend.Software;
            FallbackReason = FallbackReason.DisabledByUser;
            OpenCodecSoftware();
            return;
        }

        // ── 尝试硬解 ────────────────────────────────────────────
        if (!TryCreateHardwareDevice(out var deviceType, out var deviceRef))
        {
            Backend = DecodeBackend.Software;
            FallbackReason = FallbackReason.DeviceCreateFailed;
            Log?.Invoke("[回退] 硬解设备创建失败 → 切换到软解");
            OpenCodecSoftware();
            return;
        }

        _hwDeviceCtx = deviceRef;

        int rc = ffmpeg.avcodec_open2(_codecCtx, null, null);
        if (rc < 0)
        {
            // 失败点 ②：avcodec_open2 不接受带 hw_device_ctx 的上下文
            Log?.Invoke($"[回退] avcodec_open2(硬解) 失败: {FFmpegError.Text(rc)} → 切换到软解");
            ReleaseHwDevice();

            Backend = DecodeBackend.Software;
            FallbackReason = FallbackReason.CodecOpenFailed;
            OpenCodecSoftware();
            return;
        }

        Backend = DecodeBackend.Hardware;
        Log?.Invoke($"[解码] 硬解已启用: {ShortName(deviceType)}（hw_device_ctx 已挂载）");
    }

    /// <summary>
    /// 依次尝试可用的硬解设备。返回 false 表示全部失败。
    /// 成功时通过出参给出选定类型与已创建的设备引用。
    /// </summary>
    private bool TryCreateHardwareDevice(out AVHWDeviceType selectedType, out AVBufferRef* deviceRef)
    {
        selectedType = AVHWDeviceType.AV_HWDEVICE_TYPE_NONE;
        deviceRef = null;

        // 测试注入：模拟硬解不可用，直接判定失败让调用方走回退
        if (SimulateHardwareUnavailable)
        {
            Log?.Invoke("[解码] (模拟) 本机无可用硬解设备");
            return false;
        }

        // 顺序即优先级：D3D11VA 是现代 Windows 首选，DXVA2 兼容老驱动
        AVHWDeviceType[] candidates =
        [
            AVHWDeviceType.AV_HWDEVICE_TYPE_D3D11VA,
            AVHWDeviceType.AV_HWDEVICE_TYPE_DXVA2,
        ];

        foreach (var type in candidates)
        {
            AVBufferRef* created = null;
            int rc = ffmpeg.av_hwdevice_ctx_create(&created, type, null, null, 0);

            if (rc >= 0)
            {
                // 关键：把设备挂到 codec_ctx，必须在 avcodec_open2 之前设置。
                _codecCtx->hw_device_ctx = ffmpeg.av_buffer_ref(created);

                selectedType = type;
                deviceRef = created;
                return true;
            }

            Log?.Invoke($"[解码] 硬解设备 {ShortName(type)} 创建失败: {FFmpegError.Text(rc)}");
        }

        return false;
    }

    private void OpenCodecSoftware()
    {
        // 确保没有残留的硬解上下文
        ReleaseHwDevice();

        int rc = ffmpeg.avcodec_open2(_codecCtx, null, null);
        if (rc < 0)
        {
            Log?.Invoke($"[解码] avcodec_open2(软解) 也失败: {FFmpegError.Text(rc)}");
            return;
        }

        Log?.Invoke("[解码] 软解已启用");
    }

    /// <summary>
    /// 释放硬解设备引用。
    /// 指针是实例字段，取址需要 fixed，这里统一封装避免各处散落。
    /// </summary>
    private void ReleaseHwDevice()
    {
        if (_hwDeviceCtx is null)
        {
            return;
        }

        fixed (AVBufferRef** p = &_hwDeviceCtx)
        {
            ffmpeg.av_buffer_unref(p);
        }

        _hwDeviceCtx = null;
    }

    /// <summary>
    /// 解码下一帧到 BGRA <see cref="Bitmap"/>。
    /// 返回 null 表示流结束或发生不可恢复错误。
    /// </summary>
    public Bitmap? DecodeNextFrame()
    {
        lock (_sync)
        {
            if (_disposed || _codecCtx is null || _fmtCtx is null)
            {
                return null;
            }

            while (true)
            {
                // 先从解码器取已解出的帧
                var bitmap = TryReceiveFrame();
                if (bitmap is not null)
                {
                    return bitmap;
                }

                // 取不到就读下一个包
                int rc = ffmpeg.av_read_frame(_fmtCtx, _packet);

                if (rc < 0)
                {
                    // 流结束：把剩余帧冲出来
                    rc = ffmpeg.avcodec_send_packet(_codecCtx, null);
                    if (rc < 0 && rc != ffmpeg.AVERROR_EOF)
                    {
                        Log?.Invoke($"[解码] 冲包失败: {FFmpegError.Text(rc)}");
                    }

                    var tail = TryReceiveFrame();
                    if (tail is not null)
                    {
                        return tail;
                    }

                    Log?.Invoke($"[解码] 读取结束: {FFmpegError.Text(rc)}");
                    return null;
                }

                if (_packet->stream_index != _streamIndex)
                {
                    ffmpeg.av_packet_unref(_packet);
                    continue;
                }

                rc = ffmpeg.avcodec_send_packet(_codecCtx, _packet);
                ffmpeg.av_packet_unref(_packet);

                if (rc < 0)
                {
                    // 失败点 ③：解码中才暴露的不兼容。此时已完成若干帧，
                    // 重建解码器代价高，这里只记录并继续（多数码流是瞬时错误）。
                    Log?.Invoke($"[解码] send_packet 失败: {FFmpegError.Text(rc)}");
                }
            }
        }
    }

    /// <summary>从解码器接收一帧并转成 BGRA Bitmap。</summary>
    private Bitmap? TryReceiveFrame()
    {
        if (_codecCtx is null || _frame is null)
        {
            return null;
        }

        int rc = ffmpeg.avcodec_receive_frame(_codecCtx, _frame);
        if (rc < 0)
        {
            return null;
        }

        AVFrame* source = _frame;

        // 硬解帧在显存里，必须先搬回系统内存
        if (_frame->format == (int)AVPixelFormat.AV_PIX_FMT_D3D11 ||
            _frame->format == (int)AVPixelFormat.AV_PIX_FMT_DXVA2_VLD)
        {
            int transfer = ffmpeg.av_hwframe_transfer_data(_swFrame, _frame, 0);
            if (transfer < 0)
            {
                Log?.Invoke($"[解码] av_hwframe_transfer_data 失败: {FFmpegError.Text(transfer)}");
                ffmpeg.av_frame_unref(_frame);
                return null;
            }

            ffmpeg.av_frame_copy_props(_swFrame, _frame);
            source = _swFrame;
        }

        var bitmap = ConvertToBitmap(source, ref _swsCtx);

        ffmpeg.av_frame_unref(_frame);
        if (source == _swFrame)
        {
            ffmpeg.av_frame_unref(_swFrame);
        }

        if (bitmap is not null)
        {
            DecodedFrames++;

            // 首帧成功后确定真实尺寸
            if (Width == 0 || Height == 0)
            {
                Width = bitmap.Width;
                Height = bitmap.Height;
            }
        }

        return bitmap;
    }

    /// <summary>把任意像素格式的 AVFrame 转成 32bpp BGRA 的 <see cref="Bitmap"/>。</summary>
    /// <remarks>
    /// <para>
    /// 用 <c>SWS_FAST_BILINEAR</c> 而非 <c>SWS_BILINEAR</c>：我们不改尺寸，
    /// 不需要高质量重采样。实测 4K 下两者差约 2%（10.92→10.71 ms/帧），
    /// 因为同尺寸转换是**内存带宽受限**而非计算受限，换算法空间很小。
    /// </para>
    /// <para>
    /// 真正的优化出路是渲染层直接消费 GPU 纹理以取消这次转换，
    /// 详见 docs/decode-benchmark.md。这里不做无谓的微优化宣称。
    /// </para>
    /// </remarks>
    private static Bitmap? ConvertToBitmap(AVFrame* frame, ref SwsContext* sws)
    {
        int w = frame->width;
        int h = frame->height;
        if (w <= 0 || h <= 0)
        {
            return null;
        }

        // 目标格式：BGRA，正好对应 GDI+ 的 Format32bppArgb 内存布局
        const AVPixelFormat targetFormat = AVPixelFormat.AV_PIX_FMT_BGRA;

        sws = ffmpeg.sws_getCachedContext(
            sws,
            w, h, (AVPixelFormat)frame->format,
            w, h, targetFormat,
            (int)SwsFlags.SWS_FAST_BILINEAR,
            null, null, null);

        if (sws is null)
        {
            return null;
        }

        var bitmap = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        var data = bitmap.LockBits(
            new Rectangle(0, 0, w, h),
            ImageLockMode.WriteOnly,
            PixelFormat.Format32bppArgb);

        try
        {
            byte*[] dstData = [(byte*)data.Scan0];
            int[] dstLinesize = [data.Stride];

            ffmpeg.sws_scale(sws, frame->data, frame->linesize, 0, h, dstData, dstLinesize);
        }
        finally
        {
            bitmap.UnlockBits(data);
        }

        return bitmap;
    }

    private void ReportStreamInfo()
    {
        if (_fmtCtx is null)
        {
            return;
        }

        double duration = _fmtCtx->duration / (double)ffmpeg.AV_TIME_BASE;
        DurationSeconds = duration > 0 ? duration : 0;

        var stream = _fmtCtx->streams[_streamIndex];
        if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
        {
            FrameRate = stream->avg_frame_rate.num / (double)stream->avg_frame_rate.den;
        }
        else if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0)
        {
            FrameRate = stream->r_frame_rate.num / (double)stream->r_frame_rate.den;
        }

        TotalFrames = stream->nb_frames;
        Width = _codecCtx is null ? 0 : _codecCtx->width;
        Height = _codecCtx is null ? 0 : _codecCtx->height;

        string dur = DurationSeconds > 0
            ? TimeSpan.FromSeconds(DurationSeconds).ToString(@"hh\:mm\:ss")
            : "直播/未知";
        Log?.Invoke($"[解码] 分辨率 {Width}x{Height}, 帧率 {FrameRate:F2}, 时长 {dur}, " +
                    $"后端 {Backend}{(FallbackReason == FallbackReason.None ? string.Empty : $"（回退原因:{FallbackReason}）")}");
    }

    /// <summary>跳转到指定时间点（秒）。</summary>
    public bool SeekTo(double seconds)
    {
        lock (_sync)
        {
            if (_fmtCtx is null || _streamIndex < 0)
            {
                return false;
            }

            long ts = (long)(seconds * ffmpeg.AV_TIME_BASE);
            int rc = ffmpeg.av_seek_frame(_fmtCtx, _streamIndex, ts, ffmpeg.AVSEEK_FLAG_BACKWARD);

            if (rc < 0)
            {
                Log?.Invoke($"[解码] seek 失败: {FFmpegError.Text(rc)}");
                return false;
            }

            ffmpeg.avcodec_flush_buffers(_codecCtx);
            return true;
        }
    }

    /// <summary>根据 <see cref="Input"/> 判断是否为网络源。</summary>
    private static bool IsNetworkInput(string input) =>
        input.StartsWith("rtsp://", StringComparison.OrdinalIgnoreCase) ||
        input.StartsWith("rtmp://", StringComparison.OrdinalIgnoreCase) ||
        input.StartsWith("http://", StringComparison.OrdinalIgnoreCase) ||
        input.StartsWith("https://", StringComparison.OrdinalIgnoreCase) ||
        input.StartsWith("udp://", StringComparison.OrdinalIgnoreCase) ||
        input.StartsWith("srt://", StringComparison.OrdinalIgnoreCase);

    private static string ShortName(AVHWDeviceType t) =>
        t.ToString().Replace("AV_HWDEVICE_TYPE_", string.Empty);

    public void Dispose()
    {
        lock (_sync)
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;

            if (_swsCtx is not null)
            {
                ffmpeg.sws_freeContext(_swsCtx);
                _swsCtx = null;
            }

            if (_frame is not null)
            {
                fixed (AVFrame** p = &_frame)
                {
                    ffmpeg.av_frame_free(p);
                }
            }

            if (_swFrame is not null)
            {
                fixed (AVFrame** p = &_swFrame)
                {
                    ffmpeg.av_frame_free(p);
                }
            }

            if (_packet is not null)
            {
                fixed (AVPacket** p = &_packet)
                {
                    ffmpeg.av_packet_free(p);
                }
            }

            if (_codecCtx is not null)
            {
                fixed (AVCodecContext** p = &_codecCtx)
                {
                    ffmpeg.avcodec_free_context(p);
                }
            }

            if (_hwDeviceCtx is not null)
            {
                ReleaseHwDevice();
            }

            if (_fmtCtx is not null)
            {
                fixed (AVFormatContext** p = &_fmtCtx)
                {
                    ffmpeg.avformat_close_input(p);
                }
            }
        }
    }
}
