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
///
/// <para><b>⚠️ MF 对象严禁跨 apartment 使用（2026-10 修复 InvalidCastException）</b></para>
/// <para>
/// 旧实现把 <c>CameraEnumerator.Open</c>（激活设备 + 创建 SourceReader + 格式协商）
/// 放在调用方线程（UI 线程，STA），再把 <c>CameraCapture</c> 交给 MTA 采集线程读帧。
/// SourceReader 这条 COM 链（含高级视频处理插入的 Video Processor MFT）不是完全 agile 的：
/// STA 创建、MTA 调用时第一次 <c>ReadSample</c> 的 QueryInterface 即失败，抛
/// <c>InvalidCastException … IMFSourceReader … E_NOINTERFACE</c>。
/// 旁证：<c>--camtest</c>（同线程打开 + 同线程读帧）一直通过，
/// GUI 与 <c>--threadtest</c>（跨线程）必挂。
/// 因此现在的设计是：<see cref="Start"/> 只负责拉起工作线程，
/// <b>MF 的整个生命周期（打开、读帧、释放）全部收在工作线程（MTA）内</b>——
/// 这也是 MediaFoundation 官方推荐的 apartment。
/// </para>
/// <para>
/// 该设计同时消除了另一处偶发崩溃：旧 <see cref="Stop"/> 在 Join 后于 UI 线程
/// <c>Dispose</c> 采集器，工作线程可能仍卡在 ReadSample 里——跨线程
/// <c>Marshal.ReleaseComObject</c> 会把 RCW 断连，下一拍 ReadSample 同样抛
/// InvalidCastException。现在释放也发生在创建它的同一线程上。
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

    /// <summary>预览状态变化（true=正在预览，即设备已成功打开并开始出帧）。</summary>
    public event Action<bool>? RunningChanged;

    /// <summary>预览意外结束（设备拔出 / 读帧持续失败 / 打开失败）。</summary>
    public event Action<string>? Ended;

    /// <summary>当前采集器（打开成功后非 null，由工作线程赋值）。</summary>
    public CameraCapture? Capture { get; private set; }

    /// <summary>工作线程是否存活（含「正在打开」阶段）。</summary>
    public bool IsRunning { get; private set; }

    /// <summary>设备显示名（打开成功后可用）。</summary>
    public string DeviceName { get; private set; } = string.Empty;

    /// <summary>已呈现帧数。</summary>
    public long PresentedFrames { get; private set; }

    /// <summary>
    /// 开始预览指定设备。
    ///
    /// <para>设备打开是异步的（约几百毫秒）：本方法只负责拉起采集工作线程并立即返回。
    /// 打开结果通过事件交付——成功触发 <see cref="RunningChanged"/>(true)（此时
    /// <see cref="Capture"/> 已就绪），失败触发 <see cref="Ended"/>（原因内含）。</para>
    /// </summary>
    public bool Start(int deviceIndex)
    {
        Stop();

        lock (_sync)
        {
            if (_disposed)
            {
                return false;
            }

            _cts = new CancellationTokenSource();
            var token = _cts.Token;
            PresentedFrames = 0;

            IsRunning = true;

            _worker = new Thread(() => OpenAndCapture(deviceIndex, token))
            {
                IsBackground = true,
                Name = "VisionFlow.Camera",
            };
            _worker.Start();
        }

        return true;
    }

    /// <summary>
    /// 工作线程主体：打开设备 → 读帧循环。
    /// 整个 MF 生命周期（激活、建 Reader、协商、读帧、释放）都收在本线程内，
    /// 避免 COM 对象跨 apartment 使用（见类注释）。
    /// </summary>
    private void OpenAndCapture(int deviceIndex, CancellationToken token)
    {
        var capture = CameraEnumerator.Open(deviceIndex, msg => Log?.Invoke(msg));

        if (capture is null)
        {
            const string reason = "设备打开失败（被占用 / 无权限 / 索引无效，详见日志）";
            Log?.Invoke("[采集] " + reason);

            lock (_sync)
            {
                IsRunning = false;
            }

            RunningChanged?.Invoke(false);
            if (!token.IsCancellationRequested)
            {
                Ended?.Invoke(reason);
            }

            return;
        }

        lock (_sync)
        {
            if (token.IsCancellationRequested)
            {
                // Start→Stop 快速连击：打开完成前就被叫停
                capture.Dispose();
                IsRunning = false;
                RunningChanged?.Invoke(false);
                return;
            }

            Capture = capture;
            DeviceName = capture.DeviceName;
        }

        // 打开成功：此时 Capture 已就绪，UI 可安全读取分辨率/格式
        RunningChanged?.Invoke(true);

        CaptureLoop(capture, token);
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

            // 堆栈单独一行，便于定位 COM/interop 层问题
            Log?.Invoke(ex.StackTrace ?? "(无堆栈)");
        }
        finally
        {
            // 释放也发生在创建它的同一线程（见类注释：严禁跨 apartment 释放）
            capture.Dispose();

            lock (_sync)
            {
                if (ReferenceEquals(Capture, capture))
                {
                    Capture = null;
                }

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

    /// <summary>停止预览。采集器的释放由工作线程自行完成（同线程原则）。</summary>
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

            // ReadSample 是阻塞调用，给它一点时间返回（设备 30fps 时单拍 ≤ 35ms，
            // 打开阶段约几百毫秒，2 秒足够；超时则工作线程随 IsBackground 退出）
            worker?.Join(TimeSpan.FromSeconds(2));
            cts.Dispose();
        }

        lock (_sync)
        {
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
