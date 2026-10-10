namespace VisionFlow.Desktop.UI;

/// <summary>视频源种类。</summary>
internal enum SourceKind
{
    /// <summary>本地视频文件（FFmpeg 解码，可 seek，可硬解）。</summary>
    LocalFile,

    /// <summary>本地摄像头（MediaFoundation 采集）。AI 功能不可用。</summary>
    LocalCamera,

    /// <summary>网络相机（video-server 管理的 RTSP 流，FFmpeg 解码）。AI 可用。</summary>
    NetworkCamera,
}

/// <summary>
/// 下拉框里的一项视频源。
///
/// <para><b>为什么需要 <see cref="Kind"/> 而不只看字符串</b></para>
/// <para>
/// AI 模式下拉的启用状态、播放控制按钮的可用性、以及后续 AI 叠加是否渲染，
/// 全都取决于源种类，而不是输入字符串长什么样。
/// 从字符串反推种类（例如判断开头是不是 rtsp://）会在
/// 「本地文件路径恰好以 rtsp 开头」这类边界上出错，
/// 而且无法区分「本地摄像头」与「网络相机」——两者外观都是视频流。
/// </para>
/// </summary>
internal sealed record SourceItem(
    SourceKind Kind,
    string Display,
    string Input,
    int DeviceIndex = -1,
    string CameraId = "")
{
    /// <summary>AI 功能对该源是否可用。本地摄像头恒为 false。</summary>
    public bool SupportsAi => Kind != SourceKind.LocalCamera;

    /// <summary>是否支持播放控制（暂停/停止/seek）。摄像头是直播，无此语义。</summary>
    public bool SupportsPlaybackControl => Kind != SourceKind.LocalCamera;

    /// <summary>
    /// 该源当前是否在线。仅网络相机有真实含义
    /// （本地文件/摄像头恒为在线——能枚举出来就说明可用）。
    /// </summary>
    public bool IsOnline { get; init; } = true;

    public override string ToString() => Display;
}
