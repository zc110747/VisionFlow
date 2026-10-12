using System.Globalization;
using System.Text;

namespace VisionFlow.Desktop.Server;

/// <summary>
/// 内嵌 MediaMTX 的端口与运行参数。
///
/// <para><b>为什么端口集中在这里</b></para>
/// <para>
/// 生成的 YAML、健康检查的 URL、给 UI 拼的 RTSP 地址都必须一致。
/// 把端口值放在一个地方，就不可能出现"配置里是 8554、探测却在轮询 8555"
/// 这种只在运行时才暴露的错位。
/// </para>
/// </summary>
internal sealed record MediaMtxOptions
{
    /// <summary>RTSP 服务端口。camera-agent 默认推到这里（config: rtsp.port）。</summary>
    public int RtspPort { get; init; } = 8554;

    /// <summary>控制 API 端口。仅本机使用，供路径枚举与健康检查。</summary>
    public int ApiPort { get; init; } = 9997;

    /// <summary>控制 API 绑定地址。只给本进程用，故绑回环、不暴露到局域网。</summary>
    public string ApiAddress { get; init; } = "127.0.0.1";

    /// <summary>媒体面绑定地址。0.0.0.0 让局域网内的 agent 也能推流过来。</summary>
    public string MediaAddress { get; init; } = "0.0.0.0";

    /// <summary>WebRTC 端口（本端暂不使用，但保持与 video-server 一致的开端口集合）。</summary>
    public int WebrtcPort { get; init; } = 8889;

    /// <summary>HLS 端口（同上）。</summary>
    public int HlsPort { get; init; } = 8888;
}

/// <summary>
/// 生成 MediaMTX 的 YAML 配置。
///
/// <para><b>为什么要自己生成而不是用仓库里那份 yml</b></para>
/// <para>
/// agent 自带的 <c>mediamtx.yml</c> 是给"手工启动演示"用的最小配置。
/// 作为内嵌服务器，端口必须跟着本进程的 <see cref="MediaMtxOptions"/> 走，
/// 否则用户在 UI 上改了端口、配置文件却还是旧的。
/// 每次启动重新生成到一个唯一临时文件，好处是既不会覆盖任何已入库的
/// 配置文件，重复启动也不会去覆写已存在的文件（某些受限环境禁止覆写）。
/// </para>
///
/// <para><b>为什么只开 RTSP</b></para>
/// <para>
/// 本服务器的消费者是 desktop-server 自己（走 RTSP 直拉）与可选的浏览器
/// （走 WebRTC/HLS）。RTMP / SRT / MoQ 全部关掉：每开一个就多一个监听
/// 套接字，而它们在本项目里没有任何使用场景。
/// </para>
/// </summary>
internal static class MediaMtxConfig
{
    /// <summary>渲染 YAML 正文。</summary>
    internal static string Render(MediaMtxOptions options) =>
        string.Create(CultureInfo.InvariantCulture, $"""
            # 由 VisionFlow Desktop 自动生成，请勿手工编辑。
            # 每次启动按当前端口重新生成，端口唯一真值源在 MediaMtxOptions。
            logLevel: info
            api: true
            apiAddress: {options.ApiAddress}:{options.ApiPort}
            rtsp: true
            rtspAddress: {options.MediaAddress}:{options.RtspPort}
            rtmp: false
            srt: false
            moq: false
            webrtc: true
            webrtcAddress: {options.MediaAddress}:{options.WebrtcPort}
            webrtcLocalUDPAddress: :8189
            webrtcLocalTCPAddress: :8189
            webrtcIPsFromInterfaces: true
            hls: true
            hlsVariant: mpegts
            hlsAddress: {options.MediaAddress}:{options.HlsPort}
            playback: false
            record: false
            paths:
              all_others:
                # source: publisher —— 允许 agent 向任意路径推流，
                # 不需要为每个 camera_id 预建路径。
                source: publisher

            """);

    /// <summary>把 YAML 写到 <paramref name="directory"/> 下的唯一临时文件，返回路径。</summary>
    internal static string WriteTo(string directory, MediaMtxOptions options)
    {
        Directory.CreateDirectory(directory);

        string path = Path.Combine(
            directory,
            $"mediamtx-{Environment.ProcessId}-{DateTime.UtcNow.Ticks}.yml");

        // UTF-8 无 BOM：MediaMTX 的 YAML 解析器不认 BOM。
        File.WriteAllText(path, Render(options), new UTF8Encoding(false));
        return path;
    }
}
