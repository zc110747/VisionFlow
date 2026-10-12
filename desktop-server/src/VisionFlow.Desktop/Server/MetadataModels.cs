using System.Text.Json.Serialization;

namespace VisionFlow.Desktop.Server;

/// <summary>
/// camera-agent 上报的 AI metadata wire 协议。
///
/// <para><b>真值来源</b></para>
/// <para>
/// 字段名与类型逐一对照
/// <c>carmera-agent/src/metadata/metadata_encoder.cpp</c> 的
/// <c>encode_frame_metadata()</c> / <c>encode_status_metadata()</c>。
/// 该文件是协议的权威定义——agent 侧有单元测试把它逐字节钉住
/// （<c>metadata_tests.cpp</c> 固定了 detect/pose 报文的字节形态），
/// 因此这里的解析必须严格匹配，不能"猜个大概"。
/// </para>
///
/// <para><b>为什么顶层用一个类而不是两个</b></para>
/// <para>
/// 两种报文共用一个 <c>type</c> 判别字段（spec 14 明确要求每条消息都带），
/// 其余字段按 type 取值。用一个可空字段过多的类，比用多态反序列化更简单，
/// 也更容易在缺字段时给出精确的错误信息。
/// </para>
///
/// <para><b>未知字段一律忽略</b></para>
/// <para>
/// 协议是**向后兼容**演进的：hand 模式加 <c>hand</c> 块、status 加
/// <c>keypoints</c> 计数，都是"新增不破坏"。因此不设
/// <c>JsonUnmappedMemberHandling.Disallow</c>——那会让老客户端在新 agent
/// 面前直接失败。
/// </para>
/// </summary>
internal sealed class MetadataMessage
{
    /// <summary>协议版本。当前为 1。</summary>
    [JsonPropertyName("version")]
    public int Version { get; init; }

    /// <summary>判别字段：<c>frame</c> 或 <c>status</c>。</summary>
    [JsonPropertyName("type")]
    public string Type { get; init; } = string.Empty;

    /// <summary>摄像头标识。agent 侧来自 <c>metadata.camera_id</c>。</summary>
    [JsonPropertyName("camera_id")]
    public string CameraId { get; init; } = string.Empty;

    /// <summary>帧号。agent 原样透传（spec 15：绝不重新生成）。</summary>
    [JsonPropertyName("frame_id")]
    public long FrameId { get; init; }

    /// <summary>时间戳（毫秒 epoch）。</summary>
    [JsonPropertyName("timestamp")]
    public long Timestamp { get; init; }

    /// <summary>视频原始宽度。bbox/keypoints 的坐标参考系。</summary>
    [JsonPropertyName("video_width")]
    public int VideoWidth { get; init; }

    /// <summary>视频原始高度。bbox/keypoints 的坐标参考系。</summary>
    [JsonPropertyName("video_height")]
    public int VideoHeight { get; init; }

    /// <summary>
    /// 产生该帧时 agent 实际运行的 AI 模式（<c>ai-y</c> / <c>ai-y-pose</c> /
    /// <c>ai-hand</c>）。切换过渡期可能仍是旧模式——消费端据此丢弃陈旧帧。
    /// </summary>
    [JsonPropertyName("ai_mode")]
    public string? AiMode { get; init; }

    /// <summary>检测到的目标。仅 <c>type=frame</c> 时存在。</summary>
    [JsonPropertyName("objects")]
    public List<MetadataObject>? Objects { get; init; }

    /// <summary>AI 状态心跳。仅 <c>type=status</c> 时存在。</summary>
    [JsonPropertyName("ai")]
    public MetadataStatus? Ai { get; init; }

    /// <summary>服务端收到该消息的本地时间（非 wire 字段，本端补记）。</summary>
    [JsonIgnore]
    public DateTimeOffset ReceivedAt { get; set; } = DateTimeOffset.Now;

    /// <summary>是否为帧消息。</summary>
    [JsonIgnore]
    public bool IsFrame => string.Equals(Type, "frame", StringComparison.Ordinal);

    /// <summary>是否为状态心跳。</summary>
    [JsonIgnore]
    public bool IsStatus => string.Equals(Type, "status", StringComparison.Ordinal);
}

/// <summary>一个检测目标。</summary>
internal sealed class MetadataObject
{
    [JsonPropertyName("class")]
    public string Class { get; init; } = string.Empty;

    [JsonPropertyName("confidence")]
    public double Confidence { get; init; }

    [JsonPropertyName("track_id")]
    public int TrackId { get; init; }

    /// <summary>
    /// 包围盒 <c>[x1,y1,x2,y2]</c>，单位是**原始视频像素的整数**。
    /// encoder 侧已 clamp 到帧内，并保证 x1&lt;x2、y1&lt;y2。
    /// </summary>
    [JsonPropertyName("bbox")]
    public int[] Bbox { get; init; } = [];

    /// <summary>
    /// 每个关键点 <c>[x, y, conf]</c>。坐标是原始视频像素（2 位小数），
    /// conf 在 [0,1]。仅姿态模型存在，检测模型的报文里**没有这个键**。
    /// </summary>
    [JsonPropertyName("keypoints")]
    public double[][]? Keypoints { get; init; }
}

/// <summary>AI 状态心跳的 <c>ai</c> 块。</summary>
internal sealed class MetadataStatus
{
    /// <summary>配置里 AI 分支是否开启。</summary>
    [JsonPropertyName("enable")]
    public bool Enable { get; init; }

    /// <summary>AI 流水线当前是否真在跑。</summary>
    [JsonPropertyName("running")]
    public bool Running { get; init; }

    [JsonPropertyName("fps")]
    public double Fps { get; init; }

    /// <summary>已加载的模型文件名。</summary>
    [JsonPropertyName("model")]
    public string? Model { get; init; }

    /// <summary>跟踪器名（如 bytetrack）。</summary>
    [JsonPropertyName("tracker")]
    public string? Tracker { get; init; }

    [JsonPropertyName("last_frame_id")]
    public long LastFrameId { get; init; }

    [JsonPropertyName("last_timestamp")]
    public long LastTimestamp { get; init; }

    /// <summary>累计处理帧数。</summary>
    [JsonPropertyName("processed")]
    public long Processed { get; init; }

    /// <summary>关键点数量。大于 0 时才在 wire 上出现。</summary>
    [JsonPropertyName("keypoints")]
    public int KeypointCount { get; init; }
}

/// <summary>aimode 端点的响应体：<c>{"mode":"ai-y"}</c>。</summary>
internal sealed class AiModeResponse
{
    [JsonPropertyName("mode")]
    public string Mode { get; init; } = string.Empty;
}

/// <summary>aimode 端点的请求体。</summary>
internal sealed class AiModeRequest
{
    [JsonPropertyName("mode")]
    public string? Mode { get; init; }
}
