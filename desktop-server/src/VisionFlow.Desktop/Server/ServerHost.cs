using VisionFlow.Desktop.Ai;
using VisionFlow.Desktop.UI;

namespace VisionFlow.Desktop.Server;

/// <summary>
/// 内嵌视频服务器的总编排器。
///
/// <para><b>它替代了什么</b></para>
/// <para>
/// 改造前，本端作为**客户端**去连 video-server 的 REST API，再由后者
/// 给出 MediaMTX 上的 RTSP 地址。现在本端自己就是那台服务器：内嵌
/// MediaMTX 接收 agent 推来的流，自己维护 metadata 与 aimode 端点。
/// 因此这个类同时承担了原先散落在 <c>NetworkCameraClient</c> +
/// <c>video-server/internal/camera</c> + <c>video-server/internal/api</c>
/// 里的职责。
/// </para>
///
/// <para><b>三个子系统的关系</b></para>
/// <code>
///   agent ──RTSP ANNOUNCE──▶ MediaMtxHost(:8554)
///                                 │
///                                 │ GET /v3/paths/list  ← 谁在推视频
///                                 ▼
///                            CameraRegistry  ──▶ 下拉框
///                                 ▲
///   agent ──POST /api/metadata──▶ MetadataServer(:8099)
///   agent ──GET /aimode (2s)────▶ MetadataServer  ← 下发模式
/// </code>
/// <para>
/// 视频面与 AI 面是**两条独立的通道**：视频走 RTP，AI 走 HTTP。
/// 它们可能单独缺失（视频推着但 AI 关着、AI 开着但视频刚断），
/// 所以这里不做"一个不通就整个判离线"的耦合，而是分别记录、
/// 合并成一条 UI 条目。
/// </para>
///
/// <para><b>为什么端口从 8099 开始</b></para>
/// <para>
/// 8080 在本机被 <c>ApplicationWebServer</c> 占用、8081 被 video-server
/// 占用（都是实测结论）。8099 是用户指定的端口，避开这两个。
/// 若仍被占用，<see cref="Start"/> 会把 <see cref="LastError"/> 填上
/// 端口冲突提示，UI 侧降级为"只放本地源"而不是整窗崩溃。
/// </para>
/// </summary>
internal sealed class ServerHost : IDisposable
{
    /// <summary>metadata HTTP 端点端口（用户指定）。</summary>
    internal const int DefaultMetadataPort = 8099;

    /// <summary>MediaMTX 路径轮询间隔。1s 足够及时，又不至于压满控制 API。</summary>
    private static readonly TimeSpan PathPollInterval = TimeSpan.FromSeconds(1);

    private readonly MediaMtxOptions _options;
    private readonly string _workDir;
    private readonly object _sync = new();

    private MediaMtxHost? _mtx;
    private MetadataServer? _metadata;

    // 显式限定 System.Threading.Timer：本项目开了 ImplicitUsings 且引用
    // WinForms，裸写 Timer 会与 System.Windows.Forms.Timer 冲突（CS0104）。
    private System.Threading.Timer? _pathPoller;
    private bool _disposed;

    public ServerHost(MediaMtxOptions? options = null, string? workDir = null)
    {
        _options = options ?? new MediaMtxOptions();
        _workDir = workDir ?? Path.Combine(
            Path.GetTempPath(), "VisionFlow", "server");
    }

    /// <summary>MediaMTX 托管器（未启动时为 null）。</summary>
    public MediaMtxHost? MediaMtx => _mtx;

    /// <summary>metadata 服务端（未启动时为 null）。</summary>
    public MetadataServer? Metadata => _metadata;

    /// <summary>整个服务器是否已就绪（两个子系统都起来了）。</summary>
    public bool IsRunning =>
        _mtx is { IsRunning: true } && _metadata is { IsRunning: true };

    /// <summary>启动失败的原因（成功为 null）。</summary>
    public string? LastError { get; private set; }

    /// <summary>本端 metadata 端点 URL —— 给 agent 的 <c>--metadata-url</c> 用。</summary>
    public string MetadataUrl =>
        $"http://127.0.0.1:{_metadata?.Port ?? DefaultMetadataPort}/api/metadata";

    /// <summary>本机 RTSP 基地址。</summary>
    public string RtspBase => _mtx?.RtspBase ?? $"rtsp://127.0.0.1:{_options.RtspPort}";

    /// <summary>给 agent 的「怎么连过来」提示（一条可直接抄的字符串）。</summary>
    public string AgentHint =>
        $"rtsp://<本机IP>:{_options.RtspPort}/<camera_id>  +  --metadata-url {MetadataUrl}";

    /// <summary>
    /// 相机列表变化（路径增减 / 上线状态翻转 / metadata 到达）。
    /// <b>在后台线程触发</b>，订阅方需 <c>BeginInvoke</c>。
    /// </summary>
    public event Action? CamerasChanged;

    /// <summary>
    /// 启动两个子系统。任一个失败都会把原因写进 <see cref="LastError"/>，
    /// 并返回 false；已启动的部分会由 <see cref="Stop"/> 统一回收。
    /// </summary>
    public bool Start(Action<string>? log = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        LastError = null;

        // ── 1. metadata 端点 ──
        // 先起 HTTP 端点：即使 MediaMTX 起不来，UI 也能显示
        // "metadata 在跑但视频服务器没起来"，比整体静默更有诊断价值。
        var metadata = new MetadataServer(DefaultMetadataPort);
        if (!metadata.Start(log))
        {
            LastError = metadata.LastError ?? "metadata 端点启动失败";
            metadata.Dispose();
            return false;
        }

        // ── 2. MediaMTX ──
        var mtx = new MediaMtxHost(_options, _workDir);
        if (!mtx.Start(log))
        {
            LastError = mtx.LastError ?? "MediaMTX 启动失败";
            mtx.Dispose();
            metadata.Dispose();
            return false;
        }

        lock (_sync)
        {
            _metadata = metadata;
            _mtx = mtx;
        }

        // metadata 一到就通知 UI（agent 上报了新的 camera_id）
        metadata.StateChanged += () => CamerasChanged?.Invoke();

        // ── 3. 路径轮询 ──
        // MediaMTX 不会主动推"谁上线了"的事件，只能轮询。
        // 1s 的间隔对上/下线延迟来说完全够（下拉框不是毫秒级 UI）。
        _pathPoller = new System.Threading.Timer(_ => PollPaths(), null,
            PathPollInterval, PathPollInterval);

        // 立刻跑一次，省掉首次 1s 的空白期
        PollPaths();

        log?.Invoke($"[服务器] 就绪：agent 请推流到 {RtspBase}/<camera_id>");
        log?.Invoke($"[服务器] agent 的 --metadata-url 应指向 {MetadataUrl}");
        return true;
    }

    /// <summary>轮询 MediaMTX 路径列表并合并进 metadata 的相机表。</summary>
    private void PollPaths()
    {
        if (_disposed)
        {
            return;
        }

        var mtx = _mtx;
        var metadata = _metadata;
        if (mtx is null || metadata is null || !mtx.IsRunning)
        {
            return;
        }

        try
        {
            var paths = mtx.ListPaths();

            // 转成 metadata 侧需要的形态：路径名 → (ready, 轨道)
            var map = new Dictionary<string, (bool Ready, IReadOnlyList<string> Tracks)>(
                StringComparer.OrdinalIgnoreCase);

            foreach (var p in paths)
            {
                if (string.IsNullOrWhiteSpace(p.Name))
                {
                    continue;
                }

                map[p.Name] = (p.Ready, p.Tracks);
            }

            if (metadata.ApplyVideoPaths(map))
            {
                CamerasChanged?.Invoke();
            }
        }
        catch (Exception ex)
        {
            // 轮询失败不能杀掉整个服务器——MediaMTX 可能正在重启。
            LastError = $"路径轮询失败: {ex.Message}";
        }
    }

    /// <summary>
    /// 列出当前所有相机，合并成可直接供 UI 使用的条目。
    ///
    /// <para><b>合并规则</b></para>
    /// <list type="bullet">
    ///   <item>视频在线（MediaMTX 有 ready 路径）→ 可播放，<c>IsOnline=true</c></item>
    ///   <item>只有 metadata（agent 的 AI 在跑，但视频未就绪）→ 条目仍在，
    ///     标为视频离线；用户能看到"AI 活着但视频断了"这种状态</item>
    ///   <item>两者都没有 → 不出现在列表里</item>
    /// </list>
    /// </summary>
    public IReadOnlyList<SourceItem> ListCameras(out IReadOnlyList<string> diagnostics)
    {
        var diag = new List<string>();
        var items = new List<SourceItem>();

        var metadata = _metadata;
        if (metadata is null)
        {
            diag.Add("服务器未启动");
            diagnostics = diag;
            return items;
        }

        var states = metadata.Snapshot();
        diag.Add($"相机总数 {states.Count}");

        foreach (var state in states)
        {
            string rtsp = $"{RtspBase}/{state.CameraId}";

            string video = state.VideoOnline ? "视频在线" : "视频离线";
            string ai = state.LastStatus is { Running: true } st
                ? $"AI {st.Fps:F1}fps"
                : state.LastStatus is not null
                    ? "AI 已停"
                    : "无 AI 数据";

            string label = $"[服务器] {state.CameraId} ({video}, {ai})";

            items.Add(new SourceItem(
                SourceKind.NetworkCamera,
                label,
                rtsp,
                CameraId: state.CameraId)
            {
                // 决定默认选中谁：只有视频真的在推才认为"可用"。
                // 否则用户点开就是一个必然失败的黑屏。
                IsOnline = state.VideoOnline,
            });
        }

        // 在线优先，避免默认踩到离线的
        var ordered = items
            .OrderByDescending(i => i.IsOnline)
            .ThenBy(i => i.CameraId, StringComparer.OrdinalIgnoreCase)
            .ToList();

        int onlineCount = ordered.Count(i => i.IsOnline);
        diag.Add($"视频在线 {onlineCount} / 共 {ordered.Count}");

        diagnostics = diag;
        return ordered;
    }

    /// <summary>
    /// 读取某相机期望的 AI 模式。
    /// <para>
    /// 注意这与 agent 读的是**同一个值**（都出自 <see cref="MetadataServer"/>
    /// 的 <c>DesiredAiMode</c>），因此 UI 显示的模式就是 agent 下次轮询会拿到的，
    /// 不会出现两边不一致。
    /// </para>
    /// </summary>
    public string? GetAiMode(string cameraId)
        => _metadata?.Get(cameraId)?.DesiredAiMode;

    /// <summary>
    /// 设置某相机期望的 AI 模式。<see cref="CameraState.IsValidAiMode"/> 通过才写。
    /// </summary>
    public bool SetAiMode(string cameraId, string mode)
    {
        var metadata = _metadata;
        if (metadata is null || !CameraState.IsValidAiMode(mode))
        {
            return false;
        }

        // EnsureCamera 而不是 Get：允许在 agent 还没推上来之前就预置模式，
        // agent 一旦连上轮询就能立刻拿到（这是"提前配置"的正常用法）。
        metadata.EnsureCamera(cameraId).DesiredAiMode = mode;
        CamerasChanged?.Invoke();
        return true;
    }

    /// <summary>取某相机最近一次的 AI 叠加数据（供 UI 直接渲染，无需再走 HTTP）。</summary>
    public AiOverlayState? GetOverlayState(string cameraId)
    {
        var state = _metadata?.Get(cameraId);
        return state is null ? null : AiOverlayState.FromCameraState(state);
    }

    public void Stop(Action<string>? log = null)
    {
        System.Threading.Timer? poller;
        MediaMtxHost? mtx;
        MetadataServer? metadata;

        lock (_sync)
        {
            poller = _pathPoller;
            mtx = _mtx;
            metadata = _metadata;
            _pathPoller = null;
            _mtx = null;
            _metadata = null;
        }

        poller?.Dispose();

        // 先停 metadata 再停 MediaMTX：agent 的 aimode 轮询返回失败比
        // 推流中断更容易被它正确处理（agent 侧对 aimode 失败是容忍的）。
        metadata?.Stop();
        metadata?.Dispose();

        mtx?.Stop(log);
        mtx?.Dispose();
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
