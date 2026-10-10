using FFmpeg.AutoGen;

namespace VisionFlow.Desktop.Decoding;

/// <summary>
/// 解码后端。硬解与软解共用同一个解码器封装，本枚举只表达「当前实际生效」的路径，
/// 由解码器在三条失败路径上各自翻转。
/// </summary>
internal enum DecodeBackend
{
    /// <summary>尚未开始。</summary>
    Uninitialized,

    /// <summary>硬解（hw_device + 硬解像素格式，解码在 GPU 上完成）。</summary>
    Hardware,

    /// <summary>软解（CPU 解码）。所有硬解失败路径的最终归宿。</summary>
    Software,
}

/// <summary>硬解回退发生的原因，用于日志与 UI 提示。</summary>
internal enum FallbackReason
{
    None,

    /// <summary>硬解设备创建失败（无 GPU / 驱动不支持 / 该 hwaccel 不可用）。</summary>
    DeviceCreateFailed,

    /// <summary>get_format 回调里硬解像素格式不被解码器接受。</summary>
    FormatNotAccepted,

    /// <summary>解码器打开失败（avcodec_open2 返回负值）。</summary>
    CodecOpenFailed,

    /// <summary>首帧解码报错，码流与硬解器不兼容。</summary>
    FirstFrameFailed,

    /// <summary>调用方显式要求禁用硬解。</summary>
    DisabledByUser,
}
