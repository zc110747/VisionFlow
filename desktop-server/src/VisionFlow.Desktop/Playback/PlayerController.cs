using System.Diagnostics;
using System.Drawing;
using VisionFlow.Desktop.Decoding;

namespace VisionFlow.Desktop.Playback;

/// <summary>
/// 播放控制：在后台线程解码，按时间戳节流后把帧投递到 UI。
///
/// <para>
/// 播放循环与 UI 完全解耦：解码线程只负责产出 BGRA 帧并调用
/// <see cref="FrameReady"/>，由 UI 侧决定如何呈现。这样后续接入
/// 本地摄像头（同样产出帧）与网络相机（复用同一解码器）时，
/// 呈现路径不需要改动。
/// </para>
/// </summary>
internal sealed class PlayerController : IDisposable
{
    private readonly object _sync = new();
    private CancellationTokenSource? _cts;
    private Thread? _worker;
    private bool _disposed;

    /// <summary>新帧就绪。参数为 BGRA 位图，接收方接管所有权。</summary>
    public event Action<Bitmap>? FrameReady;

    /// <summary>日志。</summary>
    public event Action<string>? Log;

    /// <summary>播放状态变化（true=正在播放）。</summary>
    public event Action<bool>? PlayingChanged;

    /// <summary>播放自然结束（本地文件读完）。</summary>
    public event Action? Ended;

    /// <summary>当前解码器（播放中非 null）。</summary>
    public VideoDecoder? Decoder { get; private set; }

    /// <summary>是否正在播放。</summary>
    public bool IsPlaying { get; private set; }

    /// <summary>是否启用硬解尝试。</summary>
    public bool HardwareEnabled { get; set; } = true;

    /// <summary>是否按时间戳节流（本地文件为 true；直播源为 false，全速解码）。</summary>
    public bool ThrottleByTimestamp { get; set; } = true;

    /// <summary>当前播放位置（秒）。</summary>
    public double PositionSeconds { get; private set; }

    /// <summary>
    /// 打开输入并开始播放。会先停掉当前播放。
    /// </summary>
    public bool Play(string input)
    {
        Stop();

        var decoder = new VideoDecoder { HardwareEnabled = HardwareEnabled };
        decoder.Log = msg => Log?.Invoke(msg);

        if (!decoder.Open(input, msg => Log?.Invoke(msg)))
        {
            decoder.Dispose();
            return false;
        }

        lock (_sync)
        {
            Decoder = decoder;
            _cts = new CancellationTokenSource();
            var token = _cts.Token;

            IsPlaying = true;
            PlayingChanged?.Invoke(true);

            _worker = new Thread(() => DecodeLoop(decoder, token))
            {
                IsBackground = true,
                Name = "VisionFlow.Decode",
            };
            _worker.Start();
        }

        return true;
    }

    private void DecodeLoop(VideoDecoder decoder, CancellationToken token)
    {
        Log?.Invoke($"[播放] 开始，后端={decoder.Backend} 硬解尝试={HardwareEnabled}");

        var clock = Stopwatch.StartNew();
        long presented = 0;

        try
        {
            while (!token.IsCancellationRequested)
            {
                var frame = decoder.DecodeNextFrame();
                if (frame is null)
                {
                    Log?.Invoke($"[播放] 解码结束，共 {presented} 帧");
                    break;
                }

                // 按时间戳节流：本地文件的 pts 决定何时呈现，
                // 避免全速解码后瞬间丢帧。
                if (ThrottleByTimestamp && decoder.FrameRate > 1)
                {
                    double frameIntervalMs = 1000.0 / decoder.FrameRate;
                    double expectedMs = presented * frameIntervalMs;
                    double actualMs = clock.Elapsed.TotalMilliseconds;
                    double waitMs = expectedMs - actualMs;

                    if (waitMs > 1)
                    {
                        // 单次最多睡 100ms，便于及时响应取消
                        int sleep = (int)Math.Min(waitMs, 100);
                        if (token.WaitHandle.WaitOne(sleep))
                        {
                            frame.Dispose();
                            break;
                        }

                        if (waitMs > 100 && !token.IsCancellationRequested)
                        {
                            continue;   // 还没到时间，继续等
                        }
                    }
                    else if (waitMs < -200)
                    {
                        // 落后太多，直接跳帧追上（保持音画/实时语义）
                        frame.Dispose();
                        presented++;
                        continue;
                    }
                }

                presented++;
                PositionSeconds = decoder.FrameRate > 0
                    ? presented / decoder.FrameRate
                    : 0;

                RaiseFrameReady(frame);
            }
        }
        catch (Exception ex)
        {
            Log?.Invoke($"[播放] 解码线程异常: {ex.GetType().Name}: {ex.Message}");
        }
        finally
        {
            lock (_sync)
            {
                IsPlaying = false;
            }

            PlayingChanged?.Invoke(false);

            if (!token.IsCancellationRequested)
            {
                Ended?.Invoke();
            }
        }
    }

    private void RaiseFrameReady(Bitmap frame)
    {
        var handler = FrameReady;
        if (handler is null)
        {
            frame.Dispose();
            return;
        }

        handler.Invoke(frame);
    }

    /// <summary>停止播放并释放解码器。</summary>
    public void Stop()
    {
        Thread? worker;
        CancellationTokenSource? cts;

        lock (_sync)
        {
            cts = _cts;
            worker = _worker;
            _cts = null;
            _worker = null;
        }

        if (cts is not null)
        {
            cts.Cancel();

            // 给解码线程一点时间退出（它可能在 av_read_frame 阻塞）
            worker?.Join(TimeSpan.FromSeconds(2));
            cts.Dispose();
        }

        lock (_sync)
        {
            Decoder?.Dispose();
            Decoder = null;
            IsPlaying = false;
            PositionSeconds = 0;
        }

        PlayingChanged?.Invoke(false);
    }

    /// <summary>跳转（仅本地文件有意义）。</summary>
    public void Seek(double seconds)
    {
        var decoder = Decoder;
        if (decoder is null || seconds < 0)
        {
            return;
        }

        if (seconds > decoder.DurationSeconds && decoder.DurationSeconds > 0)
        {
            return;
        }

        decoder.SeekTo(seconds);
        PositionSeconds = seconds;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        Stop();
    }
}
