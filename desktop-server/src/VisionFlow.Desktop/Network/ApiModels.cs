using System.Text.Json.Serialization;

namespace VisionFlow.Desktop.Network;

/// <summary>
/// video-server REST API 的响应模型。
///
/// <para>
/// 所有 JSON 字段名与 Go 侧的 struct tag 一一对应（见
/// <c>video-server/internal/camera/camera.go</c> 与
/// <c>video-server/internal/metadata/model.go</c>）。
/// 特意保持「只读视图」形态：本端只消费，不构造，因此属性全为 init-only。
/// </para>
/// </summary>
internal sealed class CameraDto
{
    [JsonPropertyName("id")]
    public string Id { get; init; } = string.Empty;

    [JsonPropertyName("name")]
    public string Name { get; init; } = string.Empty;

    [JsonPropertyName("stream_path")]
    public string StreamPath { get; init; } = string.Empty;

    [JsonPropertyName("device_ip")]
    public string? DeviceIp { get; init; }

    [JsonPropertyName("status")]
    public string Status { get; init; } = string.Empty;

    [JsonPropertyName("resolution")]
    public string? Resolution { get; init; }

    [JsonPropertyName("fps")]
    public int Fps { get; init; }

    [JsonPropertyName("bitrate")]
    public int Bitrate { get; init; }

    /// <summary>
    /// 由 API 层根据配置补全的 RTSP 地址。缺失时本端无法直拉，
    /// 需要按 <c>rtsp://host:port/{stream_path}</c> 自行拼接。
    /// </summary>
    [JsonPropertyName("rtsp_url")]
    public string? RtspUrl { get; init; }

    /// <summary>
    /// 相机是否在线。服务端用 <c>status</c> 字符串表达
    /// （实测取值为 <c>online</c> / <c>offline</c>）。
    /// 这里统一成一个布尔，避免各处重复做字符串比较。
    /// </summary>
    [JsonIgnore]
    public bool IsOnline =>
        string.Equals(Status, "online", StringComparison.OrdinalIgnoreCase);
}

/// <summary>GET /api/cameras/{id}/aimode 与 POST 的响应：<c>{"mode":"ai-y"}</c>。</summary>
internal sealed class AiModeDto
{
    [JsonPropertyName("mode")]
    public string Mode { get; init; } = string.Empty;
}

/// <summary>一个检测目标。</summary>
internal sealed class ObjectViewDto
{
    [JsonPropertyName("class")]
    public string Class { get; init; } = string.Empty;

    [JsonPropertyName("confidence")]
    public double Confidence { get; init; }

    [JsonPropertyName("track_id")]
    public int TrackId { get; init; }

    /// <summary>[x1,y1,x2,y2]，单位为**原始视频像素**（从不归一化）。</summary>
    [JsonPropertyName("bbox")]
    public int[] Bbox { get; init; } = [];

    /// <summary>每个关键点的 [x, y, conf]；仅姿态模型存在。</summary>
    [JsonPropertyName("keypoints")]
    public double[][]? Keypoints { get; init; }
}

/// <summary>最新一帧推理结果。</summary>
internal sealed class FrameViewDto
{
    [JsonPropertyName("frame_id")]
    public long FrameId { get; init; }

    [JsonPropertyName("timestamp")]
    public long Timestamp { get; init; }

    [JsonPropertyName("video_width")]
    public int VideoWidth { get; init; }

    [JsonPropertyName("video_height")]
    public int VideoHeight { get; init; }

    /// <summary>产生该帧时 agent 实际运行的 AI 模式。</summary>
    [JsonPropertyName("ai_mode")]
    public string? AiMode { get; init; }

    [JsonPropertyName("object_count")]
    public int ObjectCount { get; init; }

    [JsonPropertyName("received_at")]
    public DateTimeOffset ReceivedAt { get; init; }

    [JsonPropertyName("objects")]
    public List<ObjectViewDto> Objects { get; init; } = [];
}

/// <summary>AI 心跳。</summary>
internal sealed class StatusViewDto
{
    [JsonPropertyName("enable")]
    public bool Enable { get; init; }

    [JsonPropertyName("running")]
    public bool Running { get; init; }

    [JsonPropertyName("fps")]
    public double Fps { get; init; }

    [JsonPropertyName("model")]
    public string? Model { get; init; }

    [JsonPropertyName("tracker")]
    public string? Tracker { get; init; }

    [JsonPropertyName("last_frame_id")]
    public long LastFrameId { get; init; }

    [JsonPropertyName("processed")]
    public long Processed { get; init; }

    [JsonPropertyName("wall_clock")]
    public long WallClock { get; init; }

    [JsonPropertyName("received_at")]
    public DateTimeOffset ReceivedAt { get; init; }
}

/// <summary>
/// 一个相机的完整 AI 快照。
/// <para>
/// <see cref="Frame"/> / <see cref="Status"/> 都可能为 null：
/// agent 可以在 AI 关闭时只发心跳，所以两者都不能假定存在。
/// </para>
/// </summary>
internal sealed class SnapshotDto
{
    [JsonPropertyName("camera_id")]
    public string CameraId { get; init; } = string.Empty;

    [JsonPropertyName("frame")]
    public FrameViewDto? Frame { get; init; }

    [JsonPropertyName("status")]
    public StatusViewDto? Status { get; init; }
}
