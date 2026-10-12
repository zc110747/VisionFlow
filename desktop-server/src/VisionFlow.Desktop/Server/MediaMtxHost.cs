using System.Diagnostics;
using System.Net.Http;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace VisionFlow.Desktop.Server;

/// <summary>MediaMTX 控制 API 的一个路径条目（GET /v3/paths/list）。</summary>
internal sealed class MediaMtxPath
{
    [JsonPropertyName("name")]
    public string Name { get; init; } = string.Empty;

    /// <summary>路径是否已就绪（有发布者且轨道已协商完成）。</summary>
    [JsonPropertyName("ready")]
    public bool Ready { get; init; }

    /// <summary>是否可读（至少有一个可用的发布者）。</summary>
    [JsonPropertyName("available")]
    public bool Available { get; init; }

    [JsonPropertyName("online")]
    public bool Online { get; init; }

    /// <summary>轨道编码列表，如 <c>["H264"]</c>。ready 前可能为空。</summary>
    [JsonPropertyName("tracks")]
    public List<string> Tracks { get; init; } = [];
}

/// <summary>GET /v3/paths/list 的响应。</summary>
internal sealed class MediaMtxPathList
{
    [JsonPropertyName("itemCount")]
    public int ItemCount { get; init; }

    [JsonPropertyName("items")]
    public List<MediaMtxPath> Items { get; init; } = [];
}

/// <summary>
/// 内嵌 MediaMTX 的托管器。
///
/// <para><b>职责边界</b></para>
/// <para>
/// 只做三件事：找到二进制、按配置启动它、报告它的状态。
/// 摄像头枚举、metadata 接收、UI 全部在别处——这样"服务器没起来"的
/// 故障面被限制在这一个类里，排查时不需要跨模块追。
/// </para>
///
/// <para><b>为什么用子进程而不是进程内 RTSP 栈</b></para>
/// <para>
/// MediaMTX 是单一 Go 二进制、行为已经被 video-server 那一侧验证过
/// （同一份 agent 推流、同一套 RTSP over TCP 语义）。自己写一个 RTSP
/// 服务器要处理 ANNOUNCE/SETUP/PLAY 状态机、RTP 的 FU-A 分片重组、
/// TCP interleaving、关键帧缓存——每一处都是能静默出错的地方。
/// 复用二进制是把不确定性换成已验证的组件。
/// </para>
///
/// <para><b>进程退出必须回收</b></para>
/// <para>
/// <see cref="Stop"/> 必须先 CloseMainWindow 再 Kill；仅 Kill 会留下
/// 占着 8554 的孤儿进程，下次启动就会因端口冲突失败——而那个错误
/// 看起来像"配置写错了"。另外必须挂 <see cref="Process.Exited"/>，
/// 让崩溃能被立刻发现，而不是等到健康检查超时。
/// </para>
/// </summary>
internal sealed class MediaMtxHost : IDisposable
{
    /// <summary>启动后等待控制 API 就绪的最长时间。</summary>
    private static readonly TimeSpan ReadyTimeout = TimeSpan.FromSeconds(12);

    /// <summary>健康检查与路径枚举的 HTTP 超时。两者都只打回环，短超时足够。</summary>
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(3);

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
    };

    private Process? _process;
    private string? _configPath;
    private bool _disposed;

    public MediaMtxHost(MediaMtxOptions options, string workingDirectory)
    {
        Options = options;
        WorkingDirectory = workingDirectory;
    }

    /// <summary>当前生效的端口配置。</summary>
    public MediaMtxOptions Options { get; }

    /// <summary>MediaMTX 的工作目录（生成的 YAML 与日志都放这里）。</summary>
    public string WorkingDirectory { get; }

    /// <summary>解析到的二进制路径（未启动时为 null）。</summary>
    public string? BinaryPath { get; private set; }

    /// <summary>是否正在运行（子进程未退出）。</summary>
    public bool IsRunning => _process is { HasExited: false };

    /// <summary>启动时记录的失败原因（成功为 null）。</summary>
    public string? LastError { get; private set; }

    /// <summary>控制 API 基地址。</summary>
    public string ApiBase => $"http://{Options.ApiAddress}:{Options.ApiPort}";

    /// <summary>本机拉流用的 RTSP 基地址。</summary>
    public string RtspBase => $"rtsp://127.0.0.1:{Options.RtspPort}";

    /// <summary>
    /// 解析 MediaMTX 二进制路径。
    ///
    /// <para>
    /// 查找顺序：显式路径 → 程序目录（构建时拷进去的副本）→ 仓库 mediamtx/ →
    /// PATH。程序目录排最前，是因为发布出去的 exe 旁边就该有它；
    /// 开发时从 bin 目录回退到仓库也能直接跑起来。
    /// </para>
    /// </summary>
    public static string? ResolveBinary(string? explicitPath = null)
    {
        var candidates = new List<string>(5);

        if (!string.IsNullOrWhiteSpace(explicitPath))
        {
            candidates.Add(explicitPath);
        }

        // 1. 程序目录（构建产物 / 发布包）
        candidates.Add(Path.Combine(AppContext.BaseDirectory, "mediamtx.exe"));

        // 2. 仓库根的 mediamtx/ —— 开发时从 bin\...\win-x64 往上找
        //    bin/<Config>/<Tfm>/<Rid> → src/<Proj> → src → desktop-server
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 6 && dir is not null; i++)
        {
            candidates.Add(Path.Combine(dir.FullName, "mediamtx", "mediamtx.exe"));
            dir = dir.Parent;
        }

        // 3. PATH
        string? fromPath = FindOnPath("mediamtx.exe");
        if (fromPath is not null)
        {
            candidates.Add(fromPath);
        }

        foreach (string c in candidates)
        {
            if (!string.IsNullOrWhiteSpace(c) && File.Exists(c))
            {
                return c;
            }
        }

        return null;
    }

    /// <summary>在 PATH 上找可执行文件；找不到返回 null。</summary>
    private static string? FindOnPath(string fileName)
    {
        string? pathVar = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrEmpty(pathVar))
        {
            return null;
        }

        foreach (string segment in pathVar.Split(Path.PathSeparator))
        {
            if (string.IsNullOrWhiteSpace(segment))
            {
                continue;
            }

            try
            {
                string full = Path.Combine(segment.Trim(), fileName);
                if (File.Exists(full))
                {
                    return full;
                }
            }
            catch (ArgumentException)
            {
                // PATH 里可能有非法字符的段落，跳过而不是整体失败
            }
        }

        return null;
    }

    /// <summary>
    /// 生成配置并启动 MediaMTX，然后等控制 API 就绪。
    /// </summary>
    /// <param name="log">日志回调（可为 null）。</param>
    /// <returns>启动且就绪返回 true。</returns>
    public bool Start(Action<string>? log = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (IsRunning)
        {
            log?.Invoke("[服务器] MediaMTX 已在运行，跳过启动");
            return true;
        }

        LastError = null;

        BinaryPath = ResolveBinary();
        if (BinaryPath is null)
        {
            LastError = "未找到 mediamtx.exe（查找顺序：程序目录 → 仓库 mediamtx/ → PATH）";
            log?.Invoke($"[服务器] {LastError}");
            return false;
        }

        log?.Invoke($"[服务器] MediaMTX 二进制: {BinaryPath}");

        try
        {
            _configPath = MediaMtxConfig.WriteTo(WorkingDirectory, Options);
            log?.Invoke($"[服务器] 配置已生成: {_configPath}");
        }
        catch (Exception ex)
        {
            LastError = $"生成配置失败: {ex.Message}";
            log?.Invoke($"[服务器] {LastError}");
            return false;
        }

        var psi = new ProcessStartInfo
        {
            FileName = BinaryPath,
            WorkingDirectory = WorkingDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        psi.ArgumentList.Add(_configPath);

        try
        {
            _process = new Process { StartInfo = psi, EnableRaisingEvents = true };
            _process.OutputDataReceived += (_, e) => Forward(e.Data, log);
            _process.ErrorDataReceived += (_, e) => Forward(e.Data, log);

            if (!_process.Start())
            {
                LastError = "Process.Start 返回 false";
                log?.Invoke($"[服务器] {LastError}");
                return false;
            }

            _process.BeginOutputReadLine();
            _process.BeginErrorReadLine();
        }
        catch (Exception ex)
        {
            LastError = $"启动 MediaMTX 失败: {ex.Message}";
            log?.Invoke($"[服务器] {LastError}");
            log?.Invoke("[服务器] 提示：8554 是否被其它程序占用？netstat -ano | findstr :8554");
            return false;
        }

        if (!WaitReady(log))
        {
            LastError = $"MediaMTX 控制 API {ApiBase} 在 " +
                        $"{ReadyTimeout.TotalSeconds:F0}s 内未就绪" +
                        (_process.HasExited ? $"（进程已退出，code={_process.ExitCode}）" : "（进程仍在运行）");
            log?.Invoke($"[服务器] {LastError}");
            return false;
        }

        log?.Invoke($"[服务器] MediaMTX 就绪: RTSP :{Options.RtspPort} / API :{Options.ApiPort}");
        return true;
    }

    private static void Forward(string? line, Action<string>? log)
    {
        if (!string.IsNullOrWhiteSpace(line))
        {
            log?.Invoke($"[mtx] {line}");
        }
    }

    /// <summary>轮询控制 API 直到就绪或超时。</summary>
    private bool WaitReady(Action<string>? log)
    {
        var deadline = DateTime.UtcNow.Add(ReadyTimeout);

        while (DateTime.UtcNow < deadline)
        {
            // 进程已经死了就不必再等 —— 端口冲突/配置错都会走到这里，
            // 早一秒返回就能早一秒把真实原因报出来。
            if (_process is { HasExited: true })
            {
                return false;
            }

            if (ProbeApi())
            {
                return true;
            }

            Thread.Sleep(250);
        }

        return ProbeApi();
    }

    /// <summary>控制 API 是否可达。</summary>
    public bool ProbeApi()
    {
        try
        {
            using var http = new HttpClient { Timeout = RequestTimeout };
            using var resp = http.GetAsync($"{ApiBase}/v3/paths/list").GetAwaiter().GetResult();
            return resp.IsSuccessStatusCode;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// 枚举当前所有推流路径。
    ///
    /// <para>
    /// 这是"谁在推流"的唯一真值来源：agent 不会向本进程注册，
    /// 它只是把 RTP 推进 8554；MediaMTX 知道哪些路径真的活了。
    /// 失败时返回空列表而不是抛异常——服务器没起来时 UI 仍要能画出下拉框。
    /// </para>
    /// </summary>
    public IReadOnlyList<MediaMtxPath> ListPaths()
    {
        try
        {
            using var http = new HttpClient { Timeout = RequestTimeout };
            string json = http.GetStringAsync($"{ApiBase}/v3/paths/list?itemsPerPage=1000")
                .GetAwaiter().GetResult();

            var parsed = JsonSerializer.Deserialize<MediaMtxPathList>(json, JsonOptions);
            return parsed?.Items ?? [];
        }
        catch
        {
            return [];
        }
    }

    /// <summary>停止子进程并删除本次生成的配置文件。</summary>
    public void Stop(Action<string>? log = null)
    {
        var proc = _process;
        _process = null;

        if (proc is not null)
        {
            try
            {
                if (!proc.HasExited)
                {
                    proc.Kill(entireProcessTree: true);
                    proc.WaitForExit(5000);
                }
            }
            catch (Exception ex)
            {
                log?.Invoke($"[服务器] 结束 MediaMTX 进程时出错: {ex.Message}");
            }
            finally
            {
                proc.Dispose();
            }

            log?.Invoke("[服务器] MediaMTX 已停止");
        }

        // 只删自己生成的那个文件：仓库里的 mediamtx.yml 不属于本进程。
        if (_configPath is not null)
        {
            try
            {
                if (File.Exists(_configPath))
                {
                    File.Delete(_configPath);
                }
            }
            catch
            {
                // 删不掉不是错误，临时文件会被系统清理
            }

            _configPath = null;
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
