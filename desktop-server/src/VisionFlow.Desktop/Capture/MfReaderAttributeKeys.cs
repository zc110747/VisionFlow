namespace VisionFlow.Desktop.Capture;

/// <summary>
/// SourceReader 创建属性键。
///
/// <para><b>这些 GUID 必须逐一对照 SDK 头文件抄，不能凭印象写</b></para>
/// <para>
/// 本项目第一版把 <c>MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING</c>
/// 写成了自造的 GUID，结果 <c>SetGUID</c> 成功返回 S_OK（属性存储不校验键名），
/// 但 SourceReader 根本不认识这个键，**静默地不插入 Video Processor MFT**，
/// 于是请求 RGB32 时被 <c>MF_E_INVALIDMEDIATYPE</c> 拒绝。
/// 这种错误没有任何报错，只能靠「功能不生效」反推。
/// </para>
/// <para>
/// 权威出处：<c>Windows Kits\10\Include\&lt;ver&gt;\um\mfreadwrite.h</c> 的
/// <c>EXTERN_GUID</c> 行。
/// </para>
/// </summary>
internal static class MfReaderAttributeKeys
{
    /// <summary>
    /// <c>{0F81DA2C-B537-4672-A8B2-A681B17307A3}</c>
    /// MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING（Win8+）。
    /// 开启高级视频处理，将 Video Processor MFT 的全部能力
    /// （格式转换、缩放、色彩空间、帧率适配）纳入 SourceReader。
    /// </summary>
    internal static readonly Guid EnableAdvancedVideoProcessing =
        new("0F81DA2C-B537-4672-A8B2-A681B17307A3");

    /// <summary>
    /// <c>{FB394F3D-CCF1-42EE-BBB3-F9B845D5681D}</c>
    /// MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING。
    /// 旧版基础转换能力（Win7 时代），仅支持有限格式对；
    /// 保留定义用于诊断对比。
    /// </summary>
    internal static readonly Guid EnableVideoProcessing =
        new("FB394F3D-CCF1-42EE-BBB3-F9B845D5681D");

    /// <summary>
    /// <c>{56B67165-219E-456D-A22E-2D3004C7FE56}</c>
    /// MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN。
    /// Shutdown 时主动断开数据源，对热插拔设备更干净。
    /// </summary>
    internal static readonly Guid DisconnectMediaSourceOnShutdown =
        new("56B67165-219E-456D-A22E-2D3004C7FE56");
}
