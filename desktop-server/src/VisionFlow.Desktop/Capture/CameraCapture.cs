using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

namespace VisionFlow.Desktop.Capture;

/// <summary>采集输出的像素格式（由 MF 转换后交付）。</summary>
internal enum CapturePixelFormat
{
    Unknown,
    Rgb32,
}

/// <summary>
/// 本地摄像头采集器（MediaFoundation SourceReader）。
///
/// <para><b>为什么请求 RGB32 而不是拿摄像头原生格式</b></para>
/// <para>
/// 摄像头原生输出多为 MJPEG / YUY2 / NV12，每种都要自己写转换器。
/// 打开 <c>MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING</c> 并
/// <c>SetCurrentMediaType</c> 请求 <c>MFVideoFormat_RGB32</c> 后，
/// MF 会插入 Video Processor MFT 替我们完成：
/// 解码 MJPEG → 色彩空间转换 → （必要时）缩放/帧率适配。
/// 输出内存布局与 <c>VideoDecoder</c> 的 BGRA 输出一致，
/// 于是 <c>VideoCanvas</c> 与后续 AI 叠加层两条路径可以完全复用。
/// </para>
///
/// <para><b>为什么不走 FFmpeg 的 dshow 输入</b></para>
/// <para>
/// 用 <c>avformat_open_input("video=设备名")</c> 也能采，但设备名里的
/// 中文/特殊字符要过 dshow 的伪 URL 编码，且设备索引与下拉框顺序难以对齐；
/// 更重要的是那会把摄像头采集塞进解码器抽象里，丧失「本地设备不是解码」的语义。
/// 这里走 MF 原生路径，采集与解码彻底分开。
/// </para>
/// </summary>
internal sealed unsafe class CameraCapture : IDisposable
{
    private readonly object _sync = new();
    private IMFSourceReader? _reader;
    private IMFMediaSource? _source;
    private bool _disposed;

    /// <summary>视频像素宽。</summary>
    public int Width { get; private set; }

    /// <summary>视频像素高。</summary>
    public int Height { get; private set; }

    /// <summary>协商到的帧率（未知为 0）。</summary>
    public double FrameRate { get; private set; }

    /// <summary>实际交付的像素格式。</summary>
    public CapturePixelFormat PixelFormat { get; private set; } = CapturePixelFormat.Unknown;

    /// <summary>已读到的帧数。</summary>
    public long CapturedFrames { get; private set; }

    /// <summary>ReadSample 返回的流结束标记（设备被拔出时会出现）。</summary>
    public bool EndOfStream { get; private set; }

    /// <summary>设备显示名（用于日志）。</summary>
    public string DeviceName { get; private set; } = string.Empty;

    /// <summary>
    /// 用激活对象打开设备并协商输出格式为 RGB32。
    /// </summary>
    internal bool Open(IMFActivate activate, Action<string>? log)
    {
        lock (_sync)
        {
            DeviceName = CameraEnumerator.ReadFriendlyName(activate) ?? "(未命名设备)";

            // ① 激活出 IMFMediaSource
            var iidSource = typeof(IMFMediaSource).GUID;
            int hr = activate.ActivateObject(ref iidSource, out object sourceObj);
            if (hr < 0 || sourceObj is not IMFMediaSource source)
            {
                log?.Invoke($"[采集] ActivateObject 失败: {MediaFoundation.HResultText(hr)}");
                return false;
            }

            _source = source;

            // ② 建 SourceReader 并协商 RGB32 输出。
            //    两级降级：高级视频处理 → 旧版视频处理。
            //    两级都不成时退到设备原生类型（画面可能无法显示，但至少打开成功，
            //    便于区分「设备打不开」与「格式不支持」）。
            if (TryOpenReader(source, advanced: true, log))
            {
                if (NegotiateRgb32(log))
                {
                    log?.Invoke($"[采集] 设备已打开（高级视频处理路径）: {DeviceName}");
                    return true;
                }

                log?.Invoke("[采集] 高级视频处理路径下 RGB32 协商失败，降级到旧版视频处理");
                DisposeReader();
            }
            else
            {
                DisposeReader();
            }

            if (!TryOpenReader(source, advanced: false, log))
            {
                return false;
            }

            if (!NegotiateRgb32(log))
            {
                log?.Invoke("[采集] 两条转换路径均失败，尝试设备原生媒体类型");
                if (!DescribeCurrentType(log))
                {
                    return false;
                }
            }
            else
            {
                log?.Invoke($"[采集] 设备已打开（旧版视频处理路径）: {DeviceName}");
                return true;
            }

            log?.Invoke($"[采集] 设备已打开（原生格式，未转换）: {DeviceName}");
            return true;
        }
    }

    /// <summary>
    /// 创建 SourceReader。<paramref name="advanced"/> 决定用哪一个视频处理标志。
    /// </summary>
    private bool TryOpenReader(IMFMediaSource source, bool advanced, Action<string>? log)
    {
        int hr = MediaFoundation.MFCreateAttributes(out IMFAttributes readerAttrs, 2);
        if (hr < 0)
        {
            log?.Invoke($"[采集] 创建读取器属性失败: {MediaFoundation.HResultText(hr)}");
            return false;
        }

        try
        {
            uint on = 1;

            var keyVp = advanced
                ? MfReaderAttributeKeys.EnableAdvancedVideoProcessing
                : MfReaderAttributeKeys.EnableVideoProcessing;
            readerAttrs.SetUINT32(ref keyVp, on);

            // 关停时主动断开数据源，对热插拔设备更干净
            var keyDisconnect = MfReaderAttributeKeys.DisconnectMediaSourceOnShutdown;
            readerAttrs.SetUINT32(ref keyDisconnect, on);

            hr = MediaFoundation.MFCreateSourceReaderFromMediaSource(source, readerAttrs, out IMFSourceReader reader);
            if (hr < 0 || reader is null)
            {
                log?.Invoke($"[采集] MFCreateSourceReaderFromMediaSource 失败: {MediaFoundation.HResultText(hr)}");
                return false;
            }

            _reader = reader;
        }
        finally
        {
            Marshal.ReleaseComObject(readerAttrs);
        }

        // 只选视频流，其余全关
        _reader.SetStreamSelection(uint.MaxValue, 0);  // MF_SOURCE_READER_ALL_STREAMS, false
        _reader.SetStreamSelection(MediaFoundation.MF_SOURCE_READER_FIRST_VIDEO_STREAM, 1);

        return true;
    }

    /// <summary>丢弃当前 SourceReader（用于降级重试）。</summary>
    private void DisposeReader()
    {
        if (_reader is null)
        {
            return;
        }

        try
        {
            _reader.Flush(uint.MaxValue);
        }
        catch
        {
            // 忽略
        }

        Marshal.ReleaseComObject(_reader);
        _reader = null;
    }

    /// <summary>
    /// 把输出媒体类型设成 RGB32。
    /// 返回 false 表示设备/转换器拒绝该格式（调用方据此决定降级路径）。
    /// </summary>
    private bool NegotiateRgb32(Action<string>? log)
    {
        if (_reader is null)
        {
            return false;
        }

        int hr = MediaFoundation.MFCreateMediaType(out IMFMediaType target);
        if (hr < 0)
        {
            log?.Invoke($"[采集] MFCreateMediaType 失败: {MediaFoundation.HResultText(hr)}");
            return false;
        }

        try
        {
            var majorType = MediaFoundation.MF_MT_MAJOR_TYPE;
            var video = MediaFoundation.MFMediaType_Video;
            target.SetGUID(ref majorType, ref video);

            var subType = MediaFoundation.MF_MT_SUBTYPE;
            var rgb32 = MediaFoundation.MFVideoFormat_RGB32;
            target.SetGUID(ref subType, ref rgb32);

            hr = _reader.SetCurrentMediaType(
                MediaFoundation.MF_SOURCE_READER_FIRST_VIDEO_STREAM, IntPtr.Zero, target);

            if (hr < 0)
            {
                log?.Invoke($"[采集] 请求 RGB32 输出被拒绝: {MediaFoundation.HResultText(hr)}");
                return false;
            }

            bool described = DescribeCurrentType(log);

            // 请求成功但实际格式不是 RGB32（转换器静默改档），仍算协商失败
            if (described && PixelFormat != CapturePixelFormat.Rgb32)
            {
                log?.Invoke("[采集] SetCurrentMediaType 返回成功，但实际格式并非 RGB32");
                return false;
            }

            return described;
        }
        finally
        {
            Marshal.ReleaseComObject(target);
        }
    }

    /// <summary>读取当前媒体类型并提取尺寸/帧率。</summary>
    private bool DescribeCurrentType(Action<string>? log)
    {
        if (_reader is null)
        {
            return false;
        }

        int hr = _reader.GetCurrentMediaType(
            MediaFoundation.MF_SOURCE_READER_FIRST_VIDEO_STREAM, out IMFMediaType current);

        if (hr < 0 || current is null)
        {
            log?.Invoke($"[采集] GetCurrentMediaType 失败: {MediaFoundation.HResultText(hr)}");
            return false;
        }

        try
        {
            var frameSize = MediaFoundation.MF_MT_FRAME_SIZE;
            hr = MediaFoundation.GetAttributeSize(current, ref frameSize, out uint w, out uint h);
            if (hr < 0)
            {
                log?.Invoke($"[采集] 读取帧尺寸失败: {MediaFoundation.HResultText(hr)}");
                return false;
            }

            Width = (int)w;
            Height = (int)h;

            var subTypeKey = MediaFoundation.MF_MT_SUBTYPE;
            Guid sub = Guid.Empty;
            if (current.GetGUID(ref subTypeKey, out Guid got) >= 0)
            {
                sub = got;
            }

            PixelFormat = sub == MediaFoundation.MFVideoFormat_RGB32
                ? CapturePixelFormat.Rgb32
                : CapturePixelFormat.Unknown;

            // 帧率是打包的 32:32（高 32 位分子 / 低 32 位分母）
            var frameRateKey = MediaFoundation.MF_MT_FRAME_RATE;
            if (MediaFoundation.GetAttributeRatio(current, ref frameRateKey, out uint num, out uint den) >= 0
                && num > 0 && den > 0)
            {
                FrameRate = num / (double)den;
            }

            log?.Invoke($"[采集] 格式协商结果: {Width}x{Height}, {SubtypeName(sub)}, 帧率 {(FrameRate > 0 ? FrameRate.ToString("F2") : "未知")}");
            return true;
        }
        finally
        {
            Marshal.ReleaseComObject(current);
        }
    }

    /// <summary>
    /// 同步读取一帧并转成 BGRA <see cref="Bitmap"/>。
    /// 返回 null 表示本轮流没有新帧（正常情况，直播源常见）或已结束。
    /// </summary>
    public Bitmap? ReadFrame()
    {
        lock (_sync)
        {
            if (_disposed || _reader is null)
            {
                return null;
            }

            int hr = _reader.ReadSample(
                MediaFoundation.MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0,
                out uint actualStreamIndex,
                out uint streamFlags,
                out long timestamp,
                out IMFSample? sample);

            if (hr < 0)
            {
                // MF_E_INVALIDREQUEST 在设备被拔出时常见，按流结束处理
                EndOfStream = true;
                return null;
            }

            // MF_SOURCE_READERF_ENDOFSTREAM = 0x2
            if ((streamFlags & 0x2) != 0)
            {
                EndOfStream = true;
                sample?.ReleaseComObjectSafely();
                return null;
            }

            // MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED = 0x1：分辨率被驱动改了，重新读参数
            if ((streamFlags & 0x1) != 0)
            {
                sample?.ReleaseComObjectSafely();
                Width = 0;
                Height = 0;
                return null;
            }

            if (sample is null)
            {
                return null;   // 无新帧
            }

            try
            {
                var bitmap = ToBitmap(sample, out uint sampleLen);
                if (bitmap is not null)
                {
                    CapturedFrames++;

                    // 首帧反推真实尺寸（协商值偶尔与实际不符）
                    if (Width == 0 || Height == 0)
                    {
                        Width = bitmap.Width;
                        Height = bitmap.Height;
                    }
                }

                return bitmap;
            }
            finally
            {
                sample.ReleaseComObjectSafely();
            }
        }
    }

    /// <summary>把采样的连续缓冲拷成 BGRA Bitmap。</summary>
    private Bitmap? ToBitmap(IMFSample sample, out uint byteLength)
    {
        byteLength = 0;

        int hr = sample.ConvertToContiguousBuffer(out IMFMediaBuffer buffer);
        if (hr < 0 || buffer is null)
        {
            return null;
        }

        try
        {
            hr = buffer.Lock(out IntPtr data, out uint maxLen, out uint curLen);
            if (hr < 0)
            {
                return null;
            }

            try
            {
                if (data == IntPtr.Zero || curLen == 0)
                {
                    return null;
                }

                byteLength = curLen;

                // 用协商尺寸；若为 0（首帧或格式变更后）则按缓冲长度反推 RGBA 尺寸
                int w = Width;
                int h = Height;
                if (w <= 0 || h <= 0)
                {
                    return null;
                }

                if (PixelFormat != CapturePixelFormat.Rgb32)
                {
                    // 非 RGB32 时缓冲不是可直接显示的 BGRA；此处不猜测排列，
                    // 返回 null 让上层报错（保持「不静默画错」的原则）。
                    return null;
                }

                long needed = (long)w * h * 4;
                if (curLen < needed)
                {
                    return null;
                }

                // ① 从 MF 缓冲拷到托管数组
                byte[] managed = new byte[needed];
                Marshal.Copy(data, managed, 0, (int)needed);

                // ② 建位图并整块搬运
                //    注意：本类有一个同名成员 PixelFormat（采集格式枚举），
                //    会遮蔽 System.Drawing.Imaging.PixelFormat，必须完全限定。
                var bitmap = new Bitmap(w, h, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
                var rect = new Rectangle(0, 0, w, h);
                var locked = bitmap.LockBits(rect, ImageLockMode.WriteOnly, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
                try
                {
                    // 注意 stride：Bitmap 的行跨距可能 > w*4（对齐），逐行拷
                    if (locked.Stride == w * 4)
                    {
                        Marshal.Copy(managed, 0, locked.Scan0, (int)needed);
                    }
                    else
                    {
                        for (int y = 0; y < h; y++)
                        {
                            IntPtr dst = IntPtr.Add(locked.Scan0, y * locked.Stride);
                            Marshal.Copy(managed, y * w * 4, dst, w * 4);
                        }
                    }
                }
                finally
                {
                    bitmap.UnlockBits(locked);
                }

                // MF 的 RGB32 是 XRGB（高位保留），alpha 通道为 0。
                // GDI+ 按 ARGB 解释会得到全透明位图；但我们绘制时不使用 alpha 混合
                // （DrawImage 到不透明背景），所以不需要修补。这一点在
                // docs/camera-capture.md 里记录，避免后续误判为「画面全黑」。
                return bitmap;
            }
            finally
            {
                buffer.Unlock();
            }
        }
        finally
        {
            Marshal.ReleaseComObject(buffer);
        }
    }

    private static string SubtypeName(Guid sub)
    {
        if (sub == MediaFoundation.MFVideoFormat_RGB32)
        {
            return "RGB32";
        }

        if (sub == MediaFoundation.MFVideoFormat_NV12)
        {
            return "NV12";
        }

        return sub.ToString("B").ToUpperInvariant();
    }

    public void Dispose()
    {
        lock (_sync)
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;

            if (_reader is not null)
            {
                try
                {
                    _reader.Flush(uint.MaxValue);
                }
                catch
                {
                    // 设备已消失时 Flush 可能抛，忽略
                }

                Marshal.ReleaseComObject(_reader);
                _reader = null;
            }

            if (_source is not null)
            {
                try
                {
                    _source.Shutdown();
                }
                catch
                {
                    // 同上
                }

                Marshal.ReleaseComObject(_source);
                _source = null;
            }
        }
    }
}
