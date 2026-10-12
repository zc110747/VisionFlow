using System.Diagnostics;
using VisionFlow.Desktop.Server;

namespace VisionFlow.Desktop.Ai;

/// <summary>
/// AI 数据轮询器。
///
/// <para><b>为什么是进程内轮询而不是 HTTP</b></para>
/// <para>
/// 改造前本端是 video-server 的**客户端**，只能通过
/// <c>GET /api/cameras/{id}/metadata</c> 拉数据，因此必须走 HTTP 轮询。
/// 现在 metadata 就落在本进程的 <see cref="MetadataServer"/> 里，
/// 再自己发 HTTP 请求给自己只是绕一圈 loopback——多一次序列化、
/// 多一个线程、还引入"HTTP 失败但数据其实就在内存里"的假故障。
/// 因此改成直接读 <see cref="ServerHost.GetOverlayState"/>。
/// </para>
///
/// <para><b>保留轮询而不是改事件推送</b></para>
/// <para>
/// <see cref="MetadataServer.StateChanged"/> 只说明"内存变了"，
/// 但它每次 metadata 都触发（含去重前的重复帧），而 UI 需要的语义是
/// "有新帧时刷新画面"。保持 150ms 定时拉取，配合 <c>frame_id</c> 去重，
/// 逻辑与改造前完全一致——差异只在数据来源，不在时序行为，
/// 这样已有的验收脚本判据不需要重写。
/// </para>
/// </summary>
internal sealed class AiMetadataPoller : IDisposable
{
    /// <summary>轮询间隔，与改造前的 HTTP 轮询保持一致。</summary>
    public static readonly TimeSpan DefaultInterval = TimeSpan.FromMilliseconds(150);

    /// <summary>数据超过该时长未更新则视为失效（agent 可能已停）。</summary>
    public static readonly TimeSpan StaleThreshold = TimeSpan.FromSeconds(3);

    private readonly ServerHost _server;
    private readonly object _sync = new();
    private CancellationTokenSource? _cts;
    private Thread? _worker;
    private bool _disposed;

    private long _lastFrameId = -1;

    public AiMetadataPoller(ServerHost server)
    {
        _server = server;
    }

    /// <summary>新的 AI 帧到达（frame_id 变化时触发）。</summary>
    public event Action<AiOverlayState>? FrameUpdated;

    /// <summary>心跳信息更新。</summary>
    public event Action<AiOverlayState>? StatusUpdated;

    /// <summary>日志。</summary>
    public event Action<string>? Log;

    /// <summary>连续失败达到阈值（相机不存在 / 服务器未启动）。</summary>
    public event Action<string>? Failed;

    /// <summary>是否正在轮询。</summary>
    public bool IsRunning { get; private set; }

    /// <summary>最近一次成功获取的状态。</summary>
    public AiOverlayState? Latest { get; private set; }

    /// <summary>轮询间隔（可调，便于测试）。</summary>
    public TimeSpan Interval { get; set; } = DefaultInterval;

    /// <summary>开始轮询指定相机。</summary>
    public void Start(string cameraId)
    {
        Stop();

        lock (_sync)
        {
            _lastFrameId = -1;
            _cts = new CancellationTokenSource();
            var token = _cts.Token;

            IsRunning = true;

            _worker = new Thread(() => PollLoop(cameraId, token))
            {
                IsBackground = true,
                Name = "VisionFlow.AiPoll",
            };
            _worker.Start();
        }
    }

    private void PollLoop(string cameraId, CancellationToken token)
    {
        Log?.Invoke($"[AI] 开始轮询 {cameraId}，间隔 {Interval.TotalMilliseconds:F0}ms（进程内）");

        var sw = Stopwatch.StartNew();
        long polls = 0;
        int consecutiveMisses = 0;

        while (!token.IsCancellationRequested)
        {
            var state = _server.GetOverlayState(cameraId);
            polls++;

            if (state is null)
            {
                consecutiveMisses++;

                // 连续 10 次（约 1.5s）才上报，避免启动瞬间刷屏
                if (consecutiveMisses == 10)
                {
                    Failed?.Invoke($"相机 {cameraId} 无 AI 数据（服务器未启动或 agent 未上报）");
                }

                if (token.WaitHandle.WaitOne(Interval))
                {
                    break;
                }

                continue;
            }

            if (consecutiveMisses > 0)
            {
                Log?.Invoke($"[AI] 轮询恢复正常（此前连续缺失 {consecutiveMisses} 次）");
                consecutiveMisses = 0;
            }

            Latest = state;

            // 帧去重：只有 frame_id 变化才算新帧
            if (state.FrameId != _lastFrameId)
            {
                _lastFrameId = state.FrameId;
                FrameUpdated?.Invoke(state);
            }

            // 心跳每次都推（fps 等是连续值，需要刷新显示）
            if (state.Running)
            {
                StatusUpdated?.Invoke(state);
            }

            if (sw.Elapsed.TotalSeconds >= 5)
            {
                double rate = polls / sw.Elapsed.TotalSeconds;
                Log?.Invoke($"[AI] 轮询 {rate:F1} 次/s，最新 frame_id={_lastFrameId}");
                polls = 0;
                sw.Restart();
            }

            if (token.WaitHandle.WaitOne(Interval))
            {
                break;
            }
        }

        Log?.Invoke("[AI] 轮询已停止");
    }

    /// <summary>停止轮询。</summary>
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
            IsRunning = false;
        }

        if (cts is not null)
        {
            cts.Cancel();

            // 进程内读取不阻塞，线程应立即退出
            worker?.Join(TimeSpan.FromSeconds(1));
            cts.Dispose();
        }
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
