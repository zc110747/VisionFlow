using System.Diagnostics;
using VisionFlow.Desktop.Network;

namespace VisionFlow.Desktop.Ai;

/// <summary>
/// AI 数据轮询器。按固定间隔拉取某相机的 metadata 快照并推送事件。
///
/// <para><b>为什么是轮询而不是长连接/WebSocket</b></para>
/// <para>
/// video-server 的 metadata 是「最新值」语义（<c>GET /api/cameras/{id}/metadata</c>
/// 返回最新一帧 + 最新心跳），不是事件流。服务端也没有为 AI 提供推送通道
/// ——agent 侧同样靠轮询拿 AI 模式。因此轮询是该架构下的自然选择，
/// 与现有服务端能力一致，不需要改动 video-server。
/// </para>
///
/// <para><b>轮询频率与去重</b></para>
/// <para>
/// 默认 150ms（约 6.7Hz）。摄像头 AI 通常跑 15~30fps，
/// 更高的轮询频率只会重复拿到同一个 <c>frame_id</c>，
/// 徒增请求量。因此按 <c>frame_id</c> 去重：只有帧真正变化才推送
/// <see cref="FrameUpdated"/>；心跳变化单独推送 <see cref="StatusUpdated"/>。
/// </para>
/// </summary>
internal sealed class AiMetadataPoller : IDisposable
{
    /// <summary>
    /// 轮询间隔。150ms 是在「及时性」与「请求量」之间的折中：
    /// 30fps 下每帧 33ms，150ms 约等于每 4~5 帧取一次，
    /// 足以让叠加框看起来跟手，又把请求压到 ≈6.7 req/s。
    /// </summary>
    public static readonly TimeSpan DefaultInterval = TimeSpan.FromMilliseconds(150);

    /// <summary>数据超过该时长未更新则视为失效（agent 可能已停）。</summary>
    public static readonly TimeSpan StaleThreshold = TimeSpan.FromSeconds(3);

    private readonly NetworkCameraClient _client;
    private readonly object _sync = new();
    private CancellationTokenSource? _cts;
    private Thread? _worker;
    private bool _disposed;

    private long _lastFrameId = -1;
    private long _consecutiveFailures;

    /// <summary>该 poller 对应的相机 ID。</summary>
    private string _cameraId = string.Empty;

    public AiMetadataPoller(NetworkCameraClient client)
    {
        _client = client;
    }

    /// <summary>新的 AI 帧到达（frame_id 变化时触发）。</summary>
    public event Action<AiOverlayState>? FrameUpdated;

    /// <summary>心跳信息更新。</summary>
    public event Action<AiOverlayState>? StatusUpdated;

    /// <summary>日志。</summary>
    public event Action<string>? Log;

    /// <summary>连续失败达到阈值（服务不可达 / 相机不存在）。</summary>
    public event Action<string>? Failed;

    /// <summary>是否正在轮询。</summary>
    public bool IsRunning { get; private set; }

    /// <summary>最近一次成功获取的状态。</summary>
    public AiOverlayState? Latest { get; private set; }

    /// <summary>轮询间隔（可调，便于测试）</summary>
    public TimeSpan Interval { get; set; } = DefaultInterval;

    /// <summary>开始轮询指定相机。</summary>
    public void Start(string cameraId)
    {
        Stop();

        lock (_sync)
        {
            _cameraId = cameraId;
            _lastFrameId = -1;
            _consecutiveFailures = 0;
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
        Log?.Invoke($"[AI] 开始轮询 {cameraId}，间隔 {Interval.TotalMilliseconds:F0}ms");

        var sw = Stopwatch.StartNew();
        long requests = 0;

        while (!token.IsCancellationRequested)
        {
            var snap = _client.GetMetadata(cameraId);
            requests++;

            if (snap is null)
            {
                _consecutiveFailures++;

                // 连续失败 10 次（约 1.5s）才上报，避免服务抖动时刷屏
                if (_consecutiveFailures == 10)
                {
                    Failed?.Invoke($"AI 数据获取连续失败：{_client.LastError}");
                }

                if (token.WaitHandle.WaitOne(Interval))
                {
                    break;
                }

                continue;
            }

            if (_consecutiveFailures > 0)
            {
                Log?.Invoke($"[AI] 轮询恢复正常（此前连续失败 {_consecutiveFailures} 次）");
                _consecutiveFailures = 0;
            }

            var state = AiOverlayState.FromSnapshot(cameraId, snap);
            Latest = state;

            // 帧去重：只有 frame_id 变化才算新帧
            if (snap.Frame is not null && state.FrameId != _lastFrameId)
            {
                _lastFrameId = state.FrameId;
                FrameUpdated?.Invoke(state);
            }

            // 心跳每次都推（fps 等是连续值，需要刷新显示）
            if (snap.Status is not null)
            {
                StatusUpdated?.Invoke(state);
            }

            // 每 5 秒报一次轮询统计，便于判断轮询是否健康
            if (sw.Elapsed.TotalSeconds >= 5)
            {
                double rate = requests / sw.Elapsed.TotalSeconds;
                Log?.Invoke($"[AI] 轮询 {rate:F1} req/s，最新 frame_id={_lastFrameId}");
                requests = 0;
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

            // 单次请求最多等 2s（HttpClient 超时）
            worker?.Join(TimeSpan.FromSeconds(3));
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
