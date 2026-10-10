using System.Net.Http;
using System.Text;
using System.Text.Json;
using VisionFlow.Desktop.UI;

namespace VisionFlow.Desktop.Network;

/// <summary>
/// video-server REST API 客户端。
///
/// <para><b>基地址来源</b></para>
/// <para>
/// 默认 <c>http://127.0.0.1:8081</c>。8080 在本机常被其他进程占用，
/// 通过环境变量 <c>VIDEO_SERVER_URL</c> 可覆盖。
/// </para>
///
/// <para><b>为什么用同步 + 短超时</b></para>
/// <para>
/// 刷新下拉框是用户点一下就要出结果的操作，且服务器大概率不在跑。
/// 接口全部走同步等待并设 2 秒超时：超时即视为「服务未运行」，
/// 立即在下拉框里显示提示项，而不是让 UI 卡住等 OS 默认的 100 秒。
/// </para>
///
/// <para><b>不抛异常</b></para>
/// <para>
/// video-server 没启动是完全正常的运行状态（用户可能只用本地文件/摄像头）。
/// 因此所有方法失败时返回空集合并把原因写进 <see cref="LastError"/>，
/// 由调用方决定是否展示。抛异常会让「刷新」按钮在无服务时弹错误框，
/// 体验上等同于把可选依赖当作硬依赖。
/// </para>
/// </summary>
internal sealed class NetworkCameraClient : IDisposable
{
    /// <summary>默认基地址（HTTP REST API）。</summary>
    internal const string DefaultBaseUrl = "http://127.0.0.1:8081";

    /// <summary>请求超时。短超时是为了让「服务没跑」快速失败。</summary>
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(2);

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
    };

    private readonly HttpClient _http;

    public NetworkCameraClient(string? baseUrl = null)
    {
        BaseUrl = (baseUrl
                   ?? Environment.GetEnvironmentVariable("VIDEO_SERVER_URL")
                   ?? DefaultBaseUrl).TrimEnd('/');

        _http = new HttpClient
        {
            BaseAddress = new Uri(BaseUrl + "/"),
            Timeout = RequestTimeout,
        };
    }

    /// <summary>当前生效的基地址。</summary>
    public string BaseUrl { get; }

    /// <summary>最近一次失败的简短原因（成功时为 null）。</summary>
    public string? LastError { get; private set; }

    /// <summary>服务是否可达（最近一次调用结果）。</summary>
    public bool ServerReachable { get; private set; }

    /// <summary>
    /// 列出所有网络相机。
    /// </summary>
    public IReadOnlyList<CameraDto> ListCameras(out IReadOnlyList<string> diagnostics)
    {
        var diag = new List<string>();
        var result = new List<CameraDto>();

        var cameras = GetJson<List<CameraDto>>("api/cameras", diag);
        if (cameras is null)
        {
            diagnostics = diag;
            return result;
        }

        result.AddRange(cameras);
        diag.Add($"GET /api/cameras → {result.Count} 个相机");
        diagnostics = diag;
        return result;
    }

    /// <summary>读取某相机的 AI 快照（最新帧 + 心跳）。</summary>
    public SnapshotDto? GetMetadata(string cameraId)
    {
        var diag = new List<string>();
        return GetJson<SnapshotDto>($"api/cameras/{Uri.EscapeDataString(cameraId)}/metadata", diag);
    }

    /// <summary>读取某相机期望的 AI 模式。</summary>
    public string? GetAiMode(string cameraId)
    {
        var diag = new List<string>();
        var dto = GetJson<AiModeDto>($"api/cameras/{Uri.EscapeDataString(cameraId)}/aimode", diag);
        return dto?.Mode;
    }

    /// <summary>
    /// 设置某相机的 AI 模式。成功返回 true。
    /// </summary>
    public bool SetAiMode(string cameraId, string mode)
    {
        try
        {
            string body = JsonSerializer.Serialize(new { mode });
            using var content = new StringContent(body, Encoding.UTF8, "application/json");

            var resp = _http.PostAsync(
                $"api/cameras/{Uri.EscapeDataString(cameraId)}/aimode", content)
                .GetAwaiter().GetResult();

            if (resp.IsSuccessStatusCode)
            {
                LastError = null;
                ServerReachable = true;
                return true;
            }

            LastError = $"POST aimode → HTTP {(int)resp.StatusCode}";
            return false;
        }
        catch (Exception ex)
        {
            LastError = ShortReason(ex);
            ServerReachable = false;
            return false;
        }
    }

    /// <summary>
    /// GET 一个 JSON 资源。失败返回 null 并把原因追加到 <paramref name="diag"/>。
    /// </summary>
    private T? GetJson<T>(string relativeUrl, List<string> diag) where T : class
    {
        try
        {
            var resp = _http.GetAsync(relativeUrl).GetAwaiter().GetResult();

            if (!resp.IsSuccessStatusCode)
            {
                string reason = $"GET /{relativeUrl} → HTTP {(int)resp.StatusCode}";
                LastError = reason;
                diag.Add(reason);
                ServerReachable = false;
                return null;
            }

            string json = resp.Content.ReadAsStringAsync().GetAwaiter().GetResult();

            var value = JsonSerializer.Deserialize<T>(json, JsonOptions);
            if (value is null)
            {
                const string reason = "响应体为空或无法解析";
                LastError = reason;
                diag.Add(reason);
                return null;
            }

            LastError = null;
            ServerReachable = true;
            return value;
        }
        catch (TaskCanceledException)
        {
            // HttpClient 超时也抛 TaskCanceledException，需与主动取消区分
            string reason = $"连接 {BaseUrl} 超时（{RequestTimeout.TotalSeconds:F0}s）";
            LastError = reason;
            diag.Add(reason);
            ServerReachable = false;
            return null;
        }
        catch (HttpRequestException ex)
        {
            string reason = $"无法连接 {BaseUrl}：{ShortReason(ex)}";
            LastError = reason;
            diag.Add(reason);
            ServerReachable = false;
            return null;
        }
        catch (JsonException ex)
        {
            string reason = $"JSON 解析失败：{ShortReason(ex)}";
            LastError = reason;
            diag.Add(reason);
            return null;
        }
        catch (Exception ex)
        {
            string reason = $"请求异常：{ShortReason(ex)}";
            LastError = reason;
            diag.Add(reason);
            return null;
        }
    }

    private static string ShortReason(Exception ex)
    {
        // SocketException 的 Message 会附带一大段 OS 描述，
        // 这里只取类型名 + 最内层消息，便于在窄下拉框/日志里读。
        var inner = ex;
        while (inner.InnerException is not null)
        {
            inner = inner.InnerException;
        }

        return inner == ex
            ? ex.Message
            : $"{ex.GetType().Name}: {inner.Message}";
    }

    public void Dispose() => _http.Dispose();

    /// <summary>
    /// 把一个相机 DTO 转成下拉框条目。
    /// 无法确定可播放地址时返回 null（由调用方决定如何提示）。
    /// </summary>
    internal SourceItem? ToSourceItem(CameraDto camera)
    {
        string url = ResolvePlayUrl(camera);
        if (string.IsNullOrEmpty(url))
        {
            return null;
        }

        string label = string.IsNullOrEmpty(camera.Name) ? camera.Id : camera.Name;
        bool online = camera.IsOnline;

        // 状态直接标在条目上：下拉框里一眼能看出哪些是活的，
        // 避免用户（以及无人值守的自动选中）优先踩到离线相机。
        string status = online ? "在线" : "离线";

        return new SourceItem(
            SourceKind.NetworkCamera,
            $"[网络] {label} ({camera.Id}, {status})",
            url,
            CameraId: camera.Id)
        {
            IsOnline = online,
        };
    }

    /// <summary>
    /// 解析出可直接喂给 FFmpeg 的播放地址。
    ///
    /// <para><b>为什么要改写服务端给出的主机名</b></para>
    /// <para>
    /// 服务端的 <c>rtsp_url</c> 填的是它自己那台机器的网卡地址
    /// （实测为 <c>192.168.16.52</c>），因为它是按「局域网内其它客户端
    /// 访问本服务」的假设生成的。但桌面端通常与服务端同机运行，
    /// 这个地址一旦遇到网卡 IP 变更、多网卡、DHCP 续租或端口转发，
    /// 就会连不上并报 <c>404 Not Found</c>（MediaMTX 上没有该路径）。
    /// </para>
    /// <para>
    /// 因此：<b>只要 REST API 基地址指向本机</b>（127.0.0.1 / localhost），
    /// 就把播放地址的主机强制换成回环地址——同机场景下这是唯一恒定的选择。
    /// 基地址是远程时保留服务端给出的主机（那才是正确的对外地址）。
    /// </para>
    /// </summary>
    private string ResolvePlayUrl(CameraDto camera)
    {
        string host = HostOf(BaseUrl) is { Length: > 0 } h ? h : "127.0.0.1";

        if (!string.IsNullOrWhiteSpace(camera.RtspUrl))
        {
            return RewriteHost(camera.RtspUrl, host);
        }

        if (string.IsNullOrWhiteSpace(camera.StreamPath))
        {
            return string.Empty;
        }

        // 端口 8554 是 MediaMTX 的默认 RTSP 端口
        return $"rtsp://{host}:8554/{camera.StreamPath.TrimStart('/')}";
    }

    /// <summary>取出 URL 的主机名；解析失败返回 null。</summary>
    private static string? HostOf(string url)
        => Uri.TryCreate(url, UriKind.Absolute, out var uri) ? uri.Host : null;

    /// <summary>
    /// 把 RTSP URL 的主机替换成 <paramref name="host"/>，保留端口与路径。
    /// 非绝对 URL 时原样返回。
    /// </summary>
    private static string RewriteHost(string rtspUrl, string host)
    {
        if (!Uri.TryCreate(rtspUrl, UriKind.Absolute, out var uri))
        {
            return rtspUrl;
        }

        if (string.Equals(uri.Host, host, StringComparison.OrdinalIgnoreCase))
        {
            return rtspUrl;
        }

        var builder = new UriBuilder(uri) { Host = host };

        // UriBuilder 对非默认端口会带上 :port，对 8554 这类自定义端口是正确行为；
        // 若原 URL 未显式写端口则 Uri.Port 会取 scheme 默认值（554），
        // 此处需还原成「不写端口」的形态。
        bool explicitPort = !uri.IsDefaultPort;
        builder.Port = explicitPort ? uri.Port : -1;

        return builder.Uri.ToString();
    }
}
