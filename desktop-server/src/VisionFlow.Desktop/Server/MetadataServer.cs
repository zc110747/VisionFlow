using System.Net;
using System.Text;
using System.Text.Json;

namespace VisionFlow.Desktop.Server;

/// <summary>
/// 摄像头在服务器侧的聚合状态。
///
/// <para>
/// 由两路信息合并而成：MediaMTX 的路径列表（谁在推**视频**）与
/// metadata 上报（谁的 **AI** 在跑）。两者可以独立缺失——
/// 视频在推但 AI 关了，或者 AI 开着但视频断了——所以不互相覆盖。
/// </para>
/// </summary>
internal sealed class CameraState
{
    /// <summary>摄像头标识（MediaMTX 路径名 == metadata.camera_id）。</summary>
    public required string CameraId { get; init; }

    /// <summary>视频是否在线（MediaMTX 路径 ready）。</summary>
    public bool VideoOnline { get; set; }

    /// <summary>该路径的轨道编码，如 H264。</summary>
    public IReadOnlyList<string> Tracks { get; set; } = [];

    /// <summary>最近一次收到的帧消息（可能为 null：只推视频、AI 关着）。</summary>
    public MetadataMessage? LastFrame { get; set; }

    /// <summary>最近一次心跳。</summary>
    public MetadataStatus? LastStatus { get; set; }

    /// <summary>最近一次收到任何 metadata 的时间。</summary>
    public DateTimeOffset LastMetadataAt { get; set; }

    /// <summary>服务端期望的 AI 模式（由本端 aimode 端点维护）。</summary>
    public string DesiredAiMode { get; set; } = DefaultAiMode;

    /// <summary>默认 AI 模式。与 video-server 的 DefaultAIMode 保持一致。</summary>
    public const string DefaultAiMode = "ai-y";

    /// <summary>已接受的 AI 模式集合。</summary>
    public static readonly string[] ValidAiModes = ["ai-off", "ai-y", "ai-y-pose", "ai-hand"];

    /// <summary>模式是否合法。</summary>
    public static bool IsValidAiMode(string? mode) =>
        mode is not null && Array.IndexOf(ValidAiModes, mode) >= 0;
}

/// <summary>
/// 内嵌的 metadata 与 aimode HTTP 服务端。
///
/// <para><b>实现的两个端点</b></para>
/// <list type="bullet">
///   <item><c>POST /api/metadata</c> —— 接收 agent 的 AI 结果与心跳。成功返
///     <b>204</b>（无正文），与 video-server 的契约一致。</item>
///   <item><c>GET /api/cameras/{id}/aimode</c> —— agent 轮询期望模式，返回
///     <c>{"mode":"..."}</c>。</item>
///   <item><c>POST /api/cameras/{id}/aimode</c> —— 本端 UI 下发模式。非法模式返
///     <b>400</b>，与 video-server 行为一致。</item>
/// </list>
///
/// <para><b>为什么 agent 零配置就能对接</b></para>
/// <para>
/// agent 侧 <c>ai.aimode_base_url</c> 留空时，会从 <c>metadata.server_url</c>
/// 推导出 aimode 的基地址（去掉 <c>/api/metadata</c> 后缀，见
/// <c>stream_controller.cpp::derive_aimode_base</c>）。因此只要把 agent 的
/// <c>--metadata-url</c> 指向本端点，aimode 轮询会自动跟过来——
/// 不需要第二个地址。
/// </para>
///
/// <para><b>线程模型</b></para>
/// <para>
/// <see cref="HttpListener"/> 在自己的线程上回调，UI 线程同时在读状态，
/// 所以所有对 <see cref="Cameras"/> 的访问都在 <see cref="_gate"/> 下。
/// 回调里只做"存下来 + 置脏位"，不直接碰 UI——UI 更新由窗体自己的定时器拉取。
/// </para>
///
/// <para><b>为什么绑定前缀要写成本地回环 + 通配两种</b></para>
/// <para>
/// <see cref="HttpListener"/> 的前缀必须与请求的 Host 匹配。<c>localhost</c>
/// 前缀不接受 <c>127.0.0.1</c> 的请求（反之亦然），因此两个都注册，
/// 否则会出现"curl 能通、agent 报连接被拒"这类只跟 Host 头有关的怪问题。
/// </para>
/// </summary>
internal sealed class MetadataServer : IDisposable
{
    private readonly HttpListener _listener = new();
    private readonly Dictionary<string, CameraState> _cameras = new(StringComparer.OrdinalIgnoreCase);
    private readonly object _gate = new();
    private readonly int _port;

    private Thread? _acceptThread;
    private volatile bool _running;
    private bool _disposed;

    /// <summary>累计收到的 metadata 消息数（诊断用）。</summary>
    private long _received;

    /// <summary>最近一次失败原因（成功为 null）。</summary>
    public string? LastError { get; private set; }

    /// <summary>是否在监听。</summary>
    public bool IsRunning => _running;

    /// <summary>监听端口。</summary>
    public int Port => _port;

    /// <summary>已收到的 metadata 消息总数。</summary>
    public long ReceivedCount => Interlocked.Read(ref _received);

    /// <summary>
    /// 状态变更通知。在 HTTP 线程上触发，订阅方**不要**直接碰 UI，
    /// 需要用 <c>BeginInvoke</c> 转投。
    /// </summary>
    public event Action? StateChanged;

    public MetadataServer(int port)
    {
        _port = port;
    }

    /// <summary>本端 metadata 端点的完整 URL（给 agent 的 --metadata-url 用）。</summary>
    public string MetadataUrl => $"http://127.0.0.1:{_port}/api/metadata";

    /// <summary>启动监听。</summary>
    public bool Start(Action<string>? log = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (_running)
        {
            return true;
        }

        LastError = null;

        try
        {
            // HttpListener 的前缀区分 Host，localhost 与 127.0.0.1 必须都注册。
            _listener.Prefixes.Clear();
            _listener.Prefixes.Add($"http://127.0.0.1:{_port}/");
            _listener.Prefixes.Add($"http://localhost:{_port}/");
            _listener.Start();
        }
        catch (HttpListenerException ex)
        {
            LastError = $"监听 {_port} 端口失败: {ex.Message}";
            log?.Invoke($"[服务器] {LastError}");
            log?.Invoke($"[服务器] 提示：端口是否被占用？netstat -ano | findstr :{_port}");
            return false;
        }
        catch (Exception ex)
        {
            LastError = $"启动 HTTP 监听失败: {ex.Message}";
            log?.Invoke($"[服务器] {LastError}");
            return false;
        }

        _running = true;
        _acceptThread = new Thread(AcceptLoop)
        {
            IsBackground = true,
            Name = "VisionFlow.MetadataServer",
        };
        _acceptThread.Start();

        log?.Invoke($"[服务器] metadata 端点已监听 http://127.0.0.1:{_port}");
        log?.Invoke($"[服务器]   POST /api/metadata");
        log?.Invoke($"[服务器]   GET|POST /api/cameras/{{id}}/aimode");
        return true;
    }

    private void AcceptLoop()
    {
        while (_running)
        {
            HttpListenerContext context;
            try
            {
                context = _listener.GetContext();
            }
            catch (HttpListenerException)
            {
                // Stop() 关闭 listener 后 GetContext 抛此异常，属正常退出路径
                break;
            }
            catch (Exception)
            {
                // ObjectDisposedException / InvalidOperationException 等
                // 都只在关闭竞态下出现，与上面的 HttpListenerException 同属
                // 退出路径。注意：HttpListenerException 派生自 SystemException，
                // 已覆盖 InvalidOperationException/ObjectDisposedException，
                // 单独列它们会触发 CS0160，故合并为此分支。
                break;
            }

            // 串行处理：agent 是单发送线程，并发度极低；
            // 串行化省掉了一整套并发写 _cameras 的推理负担。
            try
            {
                Handle(context);
            }
            catch (Exception ex)
            {
                LastError = $"处理请求时异常: {ex.Message}";
                TryClose(context, HttpStatusCode.InternalServerError);
            }
        }
    }

    private void Handle(HttpListenerContext context)
    {
        string path = context.Request.Url?.AbsolutePath ?? "/";
        string method = context.Request.HttpMethod;

        // POST /api/metadata
        if (method == "POST" && path.Equals("/api/metadata", StringComparison.OrdinalIgnoreCase))
        {
            HandleMetadata(context);
            return;
        }

        // GET|POST /api/cameras/{id}/aimode
        if (path.StartsWith("/api/cameras/", StringComparison.OrdinalIgnoreCase) &&
            path.EndsWith("/aimode", StringComparison.OrdinalIgnoreCase))
        {
            string middle = path["/api/cameras/".Length..];
            string cameraId = middle[..^"/aimode".Length];

            if (string.IsNullOrWhiteSpace(cameraId))
            {
                TryClose(context, HttpStatusCode.NotFound);
                return;
            }

            if (method == "GET")
            {
                HandleGetAiMode(context, cameraId);
                return;
            }

            if (method == "POST")
            {
                HandleSetAiMode(context, cameraId);
                return;
            }
        }

        // GET /api/health —— 便于脚本与人工确认服务在跑
        if (method == "GET" && path.Equals("/api/health", StringComparison.OrdinalIgnoreCase))
        {
            var payload = new
            {
                ok = true,
                service = "visionflow-desktop-server",
                port = _port,
                cameras = Snapshot().Count,
                received = ReceivedCount,
            };
            WriteJson(context, HttpStatusCode.OK, payload);
            return;
        }

        TryClose(context, HttpStatusCode.NotFound);
    }

    /// <summary>POST /api/metadata —— 收下 agent 的帧或心跳。</summary>
    private void HandleMetadata(HttpListenerContext context)
    {
        string body;
        using (var reader = new StreamReader(context.Request.InputStream,
                   context.Request.ContentEncoding ?? Encoding.UTF8))
        {
            body = reader.ReadToEnd();
        }

        MetadataMessage? message;
        try
        {
            message = JsonSerializer.Deserialize<MetadataMessage>(body, JsonOptions);
        }
        catch (JsonException ex)
        {
            // 报文不可解析：400。agent 会按退避重试，但这是它的 bug 而非
            // 我们的负载问题，所以明确报 400 而不是吞掉。
            WriteJson(context, HttpStatusCode.BadRequest,
                new { ok = false, error = $"malformed JSON: {ex.Message}" });
            return;
        }

        if (message is null || string.IsNullOrWhiteSpace(message.CameraId))
        {
            WriteJson(context, HttpStatusCode.BadRequest,
                new { ok = false, error = "missing camera_id" });
            return;
        }

        if (!message.IsFrame && !message.IsStatus)
        {
            WriteJson(context, HttpStatusCode.BadRequest,
                new { ok = false, error = $"unknown type '{message.Type}' (expected frame|status)" });
            return;
        }

        message.ReceivedAt = DateTimeOffset.Now;

        lock (_gate)
        {
            if (!_cameras.TryGetValue(message.CameraId, out var state))
            {
                state = new CameraState { CameraId = message.CameraId };
                _cameras[message.CameraId] = state;
            }

            if (message.IsFrame)
            {
                state.LastFrame = message;
            }
            else
            {
                state.LastStatus = message.Ai;
            }

            state.LastMetadataAt = message.ReceivedAt;
        }

        Interlocked.Increment(ref _received);
        StateChanged?.Invoke();

        // 204：与 video-server 的契约一致。agent 只认 2xx，不给正文更省。
        TryClose(context, HttpStatusCode.NoContent);
    }

    /// <summary>GET /api/cameras/{id}/aimode。</summary>
    private void HandleGetAiMode(HttpListenerContext context, string cameraId)
    {
        string mode;
        lock (_gate)
        {
            mode = _cameras.TryGetValue(cameraId, out var state)
                ? state.DesiredAiMode
                : CameraState.DefaultAiMode;
        }

        WriteJson(context, HttpStatusCode.OK, new AiModeResponse { Mode = mode });
    }

    /// <summary>POST /api/cameras/{id}/aimode。</summary>
    private void HandleSetAiMode(HttpListenerContext context, string cameraId)
    {
        string body;
        using (var reader = new StreamReader(context.Request.InputStream,
                   context.Request.ContentEncoding ?? Encoding.UTF8))
        {
            body = reader.ReadToEnd();
        }

        AiModeRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<AiModeRequest>(body, JsonOptions);
        }
        catch (JsonException ex)
        {
            WriteJson(context, HttpStatusCode.BadRequest,
                new { ok = false, error = $"malformed JSON: {ex.Message}" });
            return;
        }

        string? mode = request?.Mode;

        // 非法模式必须 400：如果这里放行，agent 的 aimode_from_string 会
        // 静默拒绝（保持当前模型），而 UI 却显示已切换 —— 那种"看起来成功
        // 但什么都没发生"的错位是排查成本最高的故障。
        if (!CameraState.IsValidAiMode(mode))
        {
            WriteJson(context, HttpStatusCode.BadRequest, new
            {
                ok = false,
                error = $"invalid mode '{mode}'",
                valid = CameraState.ValidAiModes,
            });
            return;
        }

        lock (_gate)
        {
            if (!_cameras.TryGetValue(cameraId, out var state))
            {
                state = new CameraState { CameraId = cameraId };
                _cameras[cameraId] = state;
            }

            state.DesiredAiMode = mode!;
        }

        StateChanged?.Invoke();

        WriteJson(context, HttpStatusCode.OK, new AiModeResponse { Mode = mode! });
    }

    /// <summary>取一份当前相机状态的快照（调用方可安全遍历，不再持锁）。</summary>
    public List<CameraState> Snapshot()
    {
        lock (_gate)
        {
            return [.. _cameras.Values];
        }
    }

    /// <summary>按 id 取单个相机状态；不存在返回 null。</summary>
    public CameraState? Get(string cameraId)
    {
        lock (_gate)
        {
            return _cameras.TryGetValue(cameraId, out var state) ? state : null;
        }
    }

    /// <summary>确保某相机存在（用于把 MediaMTX 上发现但还没上报 metadata 的路径也纳入列表）。</summary>
    public CameraState EnsureCamera(string cameraId)
    {
        lock (_gate)
        {
            if (!_cameras.TryGetValue(cameraId, out var state))
            {
                state = new CameraState { CameraId = cameraId };
                _cameras[cameraId] = state;
            }

            return state;
        }
    }

    /// <summary>
    /// 用 MediaMTX 的路径列表刷新视频在线状态。
    /// 返回是否有任何变化（用于决定要不要通知 UI 重画）。
    /// </summary>
    public bool ApplyVideoPaths(IReadOnlyDictionary<string, (bool Ready, IReadOnlyList<string> Tracks)> paths)
    {
        bool changed = false;

        lock (_gate)
        {
            // 先全部置离线，再按传入的路径点亮——这样"停止推流"能被检测到。
            foreach (var state in _cameras.Values)
            {
                if (state.VideoOnline)
                {
                    state.VideoOnline = false;
                    changed = true;
                }
            }

            foreach (var (id, info) in paths)
            {
                if (!_cameras.TryGetValue(id, out var state))
                {
                    state = new CameraState { CameraId = id };
                    _cameras[id] = state;
                    changed = true;
                }

                if (state.VideoOnline != info.Ready)
                {
                    state.VideoOnline = info.Ready;
                    changed = true;
                }

                state.Tracks = info.Tracks;
            }
        }

        return changed;
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
    };

    private static void WriteJson(HttpListenerContext context, HttpStatusCode status, object payload)
    {
        byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(payload);
        context.Response.StatusCode = (int)status;
        context.Response.ContentType = "application/json; charset=utf-8";
        context.Response.ContentLength64 = bytes.Length;
        context.Response.OutputStream.Write(bytes, 0, bytes.Length);
        context.Response.Close();
    }

    private static void TryClose(HttpListenerContext context, HttpStatusCode status)
    {
        try
        {
            context.Response.StatusCode = (int)status;
            context.Response.ContentLength64 = 0;
            context.Response.Close();
        }
        catch
        {
            // 客户端已断开（agent 关闭 keep-alive 套接字）——预期内，忽略
        }
    }

    public void Stop()
    {
        if (!_running)
        {
            return;
        }

        _running = false;

        try
        {
            _listener.Stop();
            _listener.Close();
        }
        catch
        {
            // 已关闭
        }

        _acceptThread?.Join(TimeSpan.FromSeconds(2));
        _acceptThread = null;
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
