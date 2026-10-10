using VisionFlow.Desktop.Network;

namespace VisionFlow.Desktop.Ai;

/// <summary>
/// 一个相机当前的 AI 叠加状态快照。
///
/// <para>
/// 这是「渲染层唯一的数据来源」：绘制代码只读这个对象，
/// 不直接接触 DTO，也不自己做网络请求。
/// </para>
/// </summary>
internal sealed class AiOverlayState
{
    /// <summary>该帧对应的相机。</summary>
    public string CameraId { get; init; } = string.Empty;

    /// <summary>帧序号（agent 的计数器，服务端原样透传）。</summary>
    public long FrameId { get; init; }

    /// <summary>agent 侧时间戳（PTS 单位）。</summary>
    public long Timestamp { get; init; }

    /// <summary>该帧对应的视频原始宽（bbox 坐标的参考系）。</summary>
    public int VideoWidth { get; init; }

    /// <summary>该帧对应的视频原始高。</summary>
    public int VideoHeight { get; init; }

    /// <summary>产生该帧时 agent 实际运行的模式。</summary>
    public string? AiMode { get; init; }

    /// <summary>检测目标列表。</summary>
    public IReadOnlyList<ObjectViewDto> Objects { get; init; } = [];

    /// <summary>AI 是否在运行（来自心跳）。</summary>
    public bool Running { get; init; }

    /// <summary>agent 报告的处理帧率。</summary>
    public double Fps { get; init; }

    /// <summary>模型名。</summary>
    public string? Model { get; init; }

    /// <summary>跟踪器名。</summary>
    public string? Tracker { get; init; }

    /// <summary>已处理帧总数。</summary>
    public long Processed { get; init; }

    /// <summary>最近一次成功拉取的时间（本端时钟）。</summary>
    public DateTimeOffset FetchedAt { get; init; }

    /// <summary>数据是否足够新鲜（超过该阈值视为 agent 已停）。</summary>
    public bool IsStale(TimeSpan threshold)
        => DateTimeOffset.Now - FetchedAt > threshold;

    /// <summary>
    /// 从服务端快照构造。
    /// </summary>
    public static AiOverlayState FromSnapshot(string cameraId, SnapshotDto snap)
    {
        var frame = snap.Frame;
        var status = snap.Status;

        return new AiOverlayState
        {
            CameraId = cameraId,
            FrameId = frame?.FrameId ?? 0,
            Timestamp = frame?.Timestamp ?? 0,
            VideoWidth = frame?.VideoWidth ?? 0,
            VideoHeight = frame?.VideoHeight ?? 0,
            AiMode = frame?.AiMode,
            Objects = frame?.Objects ?? [],
            Running = status?.Running ?? false,
            Fps = status?.Fps ?? 0,
            Model = status?.Model,
            Tracker = status?.Tracker,
            Processed = status?.Processed ?? 0,
            FetchedAt = DateTimeOffset.Now,
        };
    }
}
