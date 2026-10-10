using System.Diagnostics;
using System.Drawing;
using VisionFlow.Desktop.Capture;

namespace VisionFlow.Desktop.Playback;

/// <summary>
/// 本地摄像头预览控制。
///
/// <para><b>与 <see cref="PlayerController"/> 的关系</b></para>
/// <para>
/// 两者对 UI 的契约完全一致：后台线程产出 BGRA <see cref="Bitmap"/>，
/// 通过 <c>FrameReady</c> 交给 <see cref="Rendering.VideoCanvas"/>。
/// 区别只在数据来源与节流方式：
/// </para>
/// <list type="bullet">
///   <item>播放器：FFmpeg 解码文件/流，按 pts 节流，有 <c>Seek</c>/<c>Duration</c></item>
///   <item>采集器：MF 读设备，**设备本身就是节拍源**（ReadSample 会阻塞到有帧），
///         因此不需要也不应该再做时间戳节流，否则会和设备节拍打架</item>
/// </list>
/// <para>
/// 之所以单独成类而不是塞进 <see cref="PlayerController"/>：播放器有 seek/时长/
/// 结束语义，采集器没有（直播无限长、不可 seek）。混在一个类里会让
/// <c>DurationSeconds</c> 之类的属性出现「摄像头下无意义」的分支，
/// 后续 AI 叠加接入时更容易出错。
/// </para>
/// </summary>
internal sealed class CameraPreviewController : IDisposable
{
    private readonly object _sync = new();
    private CancellationTokenSource? _cts;
    private Thread? _worker;
    private bool _disposed;

    /// <summary>新帧就绪。参数为 BGRA 位图，接收方接管所有权。</summary>
    public event Action<Bitmap>? FrameReady;

    /// <summary>日志。</summary>
    public event Action<string>? Log;

    /// <summary>预览状态变化（true=正在预览）。</summary>
    public event Action<bool>? RunningChanged;

    /// <summary>预览意外结束（设备拔出 / 读帧持续失败）。</summary>
    public event Action<string>? Ended;

    /// <summary>当前采集器（预览中非 null）。</summary>
    public CameraCapture? Capture { get; private set; }

    /// <summary>是否正在预览。</summary>
    public bool IsRunning { get; private set; }

    /// <summary>设备显示名。</summary>
    public string DeviceName { get; private set; } = string.Empty;

    /// <summary>已呈现帧数。</summary>
    public long PresentedFrames { get; private set; }

    /// <summary>
    /// 开始预览指定设备。
    /// </summary>
    public bool Start(int deviceIndex)
    {
        Stop();

        var capture = CameraEnumerator.Open(deviceIndex, msg => Log?.Invoke(msg));
        if (capture is null)
        {
            return false;
        }

        lock (_sync)
        {
            Capture = capture;
            DeviceName = capture.DeviceName;
            PresentedFrames = 0;
            _cts = new CancellationTokenSource();
            var token = _cts.Token;

            IsRunning = true;
            RunningChanged?.Invoke(true);

            _worker = new Thread(() => CaptureLoop(capture, token))
            {
                IsBackground = true,
                Name = "VisionFlow.Camera",
            };
            _worker.Start();
        }

        return true;
    }

    private void CaptureLoop(CameraCapture capture, CancellationToken token)
    {
        Log?.Invoke($"[采集] 开始预览 {capture.DeviceName} " +
                    $"({capture.Width}x{capture.Height} {capture.PixelFormat} " +
                    $"{(capture.FrameRate > 0 ? capture.FrameRate.ToString("F2") + "fps" : "帧率未知")})");

        var fpsClock = Stopwatch.StartNew();
        long fpsBase = 0;
        string? endReason = null;

        try
        {
            while (!token.IsCancellationRequested)
            {
                var frame = capture.ReadFrame();

                if (frame is null)
                {
                    if (capture.EndOfStream)
                    {
                        endReason = "设备流已结束（可能被拔出或被其他程序抢占）";
                        break;
                    }

                    // ReadSample 无新帧是常态（设备按自己的节拍出帧）。
                    // 这里短暂让出 CPU，避免空转烧核。
                    if (token.WaitHandle.WaitOne(2))
                    {
                        break;
                    }

                    continue;
                }

                PresentedFrames++;
                RaiseFrameReady(frame);

                // 每秒报一次实测帧率，便于判断设备是否掉帧
                double elapsed = fpsClock.Elapsed.TotalSeconds;
                if (elapsed >= 1.0)
                {
                    long delta = PresentedFrames - fpsBase;
                    Log?.Invoke($"[采集] 实测帧率 {delta / elapsed:F1} fps（累计 {PresentedFrames} 帧）");
                    fpsBase = PresentedFrames;
                    fpsClock.Restart();
                }
            }
        }
        catch (Exception ex)
        {
            endReason = $"采集线程异常: {ex.GetType().Name}: {ex.Message}";
            Log?.Invoke("[采集] " + endReason);
        }
        finally
        {
            lock (_sync)
            {
                IsRunning = false;
            }

            RunningChanged?.Invoke(false);

            if (!token.IsCancellationRequested && endReason is not null)
            {
                Ended?.Invoke(endReason);
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

    /// <summary>停止预览并释放采集器。</summary>
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

            // ReadSample 是阻塞调用，给它一点时间返回
            worker?.Join(TimeSpan.FromSeconds(2));
            cts.Dispose();
        }

        lock (_sync)
        {
            Capture?.Dispose();
            Capture = null;
            IsRunning = false;
        }

        RunningChanged?.Invoke(false);
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
