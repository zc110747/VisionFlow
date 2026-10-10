using System.Runtime.InteropServices;
using System.Runtime.InteropServices.Marshalling;

namespace VisionFlow.Desktop.Capture;

/// <summary>
/// MediaFoundation 的最小 P/Invoke + COM 接口声明。
///
/// <para><b>为什么直接手写 COM 而不用 NuGet 包</b></para>
/// <para>
/// 本地摄像头采集只需要 <c>IMFActivate</c>（枚举设备）+ <c>IMFSourceReader</c>
/// （读帧）两个接口。引入第三方封装包（如 MediaFoundation.Net）会带来版本与
/// 许可负担，而手写声明总量不到 200 行，且能精确控制
/// <c>MF_SOURCE_READER_*</c> 标志位与「原始/已转换」输出的区分。
/// </para>
///
/// <para><b>关键设计：让 MF 做像素格式转换</b></para>
/// <para>
/// <c>MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING</c> 打开后，
/// SourceReader 会在内部插入 Video Processor MFT，把摄像头输出的
/// NV12/YUY2/MJPEG 统一转成我们请求的 <c>RGB32</c>。
/// 这样上层拿到的就是 GDI+ 可直接消费的 BGRA 内存布局，
/// 与 <c>VideoDecoder</c> 的输出格式完全一致，渲染层无需分支。
/// </para>
/// </summary>
internal static class MediaFoundation
{
    // ── 初始化 ──────────────────────────────────────────────────────

    [DllImport("mfplat.dll", ExactSpelling = true)]
    internal static extern int MFStartup(uint version, uint flags);

    [DllImport("mfplat.dll", ExactSpelling = true)]
    internal static extern int MFShutdown();

    // ── 属性读取辅助 ────────────────────────────────────────────────

    /// <remarks>
    /// <c>MFGetAttributeSize</c> / <c>MFGetAttributeRatio</c> 都是 Windows SDK
    /// 头文件里的 <b>inline</b> 函数，**不是 mfplat.dll 的导出**，
    /// 不能 P/Invoke。它们内部就是对 <c>GetUINT64</c> 的位拆分：
    /// <code>
    ///   UINT32 w = (UINT32)(value &gt;&gt; 32);   // 高 32 位 = 宽/分子
    ///   UINT32 h = (UINT32)(value &amp; 0xFFFFFFFF); // 低 32 位 = 高/分母
    /// </code>
    /// 这里按同样的位序手工拆分。
    /// </remarks>
    internal static int GetAttributeSize(IMFMediaType type, ref Guid key, out uint width, out uint height)
    {
        width = 0;
        height = 0;

        int hr = type.GetUINT64(ref key, out ulong packed);
        if (hr < 0)
        {
            return hr;
        }

        width = (uint)(packed >> 32);
        height = (uint)(packed & 0xFFFFFFFF);
        return 0;
    }

    /// <summary>读取打包成 32:32 的比率属性（帧率等）。</summary>
    internal static int GetAttributeRatio(IMFMediaType type, ref Guid key, out uint numerator, out uint denominator)
    {
        numerator = 0;
        denominator = 0;

        int hr = type.GetUINT64(ref key, out ulong packed);
        if (hr < 0)
        {
            return hr;
        }

        numerator = (uint)(packed >> 32);
        denominator = (uint)(packed & 0xFFFFFFFF);
        return 0;
    }

    [DllImport("mfplat.dll", ExactSpelling = true)]
    internal static extern int MFCreateMediaType(out IMFMediaType ppMFType);

    [DllImport("mfplat.dll", ExactSpelling = true)]
    internal static extern int MFCreateAttributes(out IMFAttributes ppMFAttributes, uint cInitialSize);

    [DllImport("mfreadwrite.dll", ExactSpelling = true, CharSet = CharSet.Unicode)]
    internal static extern int MFCreateSourceReaderFromMediaSource(
        IMFMediaSource pMediaSource,
        IMFAttributes? pAttributes,
        out IMFSourceReader ppSourceReader);

    [DllImport("mf.dll", ExactSpelling = true, CharSet = CharSet.Unicode)]
    internal static extern int MFEnumDeviceSources(
        IMFAttributes pAttributes,
        out IntPtr pppSourceActivate,
        out uint pcSourceActivate);

    // ── 常量 ────────────────────────────────────────────────────────

    internal const uint MF_VERSION = 0x00020070;   // MF_SDK_VERSION=2, MF_API_VERSION=0x0070
    internal const uint MFSTARTUP_FULL = 0;        // MFSTARTUP_NOSOCKET 不设，需要网络设备发现

    internal const int MF_E_NO_MORE_TYPES = unchecked((int)0xC00D36B9);
    internal const int MF_E_ATTRIBUTENOTFOUND = unchecked((int)0xC00D36E6);
    internal const int MF_E_INVALIDMEDIATYPE = unchecked((int)0xC00D36B4);
    internal const int MF_E_NOTACCEPTING = unchecked((int)0xC00D36B3);
    internal const int MF_E_HW_MFT_FAILED_START_STREAMING = unchecked((int)0xC00D3704);
    internal const int MF_E_INVALIDREQUEST = unchecked((int)0xC00D36B2);

    /// <summary>MF_SOURCE_READER_FIRST_VIDEO_STREAM</summary>
    internal const uint MF_SOURCE_READER_FIRST_VIDEO_STREAM = 0xFFFFFFFC;

    /// <summary>MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING（旧，仅基础转换）</summary>
    internal const uint MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING = 0x00010000;

    /// <summary>MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN</summary>
    internal const uint MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN = 0x00000002;

    /// <summary>
    /// MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING。
    /// 这是能吃到 Video Processor MFT 全功能（含缩放/色彩空间/帧率转换）的那个标志，
    /// 也是让它把 MJPEG/NV12 转 RGB32 的关键。
    /// </summary>
    internal const uint MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING = 0x00000001;

    // 关键媒体类型 GUID
    // ⚠ 全部对照 Windows Kits\10\Include\<ver>\um\mfapi.h 的 DEFINE_GUID 抄写。
    //   头文件里 GUID 上方常有**过时注释**（例如 MF_MT_FRAME_RATE 上一行注释
    //   写的是另一个旧 GUID），只信 DEFINE_GUID 的实际取值。
    internal static readonly Guid MF_MT_MAJOR_TYPE = new("48eba18e-f8c9-4687-bf11-0a74c9f96a8f");
    internal static readonly Guid MF_MT_SUBTYPE = new("f7e34c9a-42e8-4714-b74b-cb29d72c35e5");
    internal static readonly Guid MF_MT_FRAME_SIZE = new("1652c33d-d6b2-4012-b834-72030849a37d");
    internal static readonly Guid MF_MT_FRAME_RATE = new("c459a2e8-3d2c-4e44-b132-fee5156c7bb0");
    internal static readonly Guid MF_MT_INTERLACE_MODE = new("e2724bb8-e676-4806-b4b2-a8d6efb44ccd");

    internal static readonly Guid MFMediaType_Video = new("73646976-0000-0010-8000-00AA00389B71");

    /// <summary>
    /// MFVideoFormat_RGB32 = <c>D3DFMT_X8R8G8B8</c>(22) 展开为 GUID
    /// → <c>00000016-0000-0010-8000-00AA00389B71</c>。
    /// 内存布局为 X8R8G8B8（BGRA 字节序），与 GDI+ 的 Format32bppArgb 一致。
    /// </summary>
    internal static readonly Guid MFVideoFormat_RGB32 = new("00000016-0000-0010-8000-00AA00389B71");

    /// <summary>MFVideoFormat_NV12 = <c>FCC('NV12')</c> → <c>3231564E-0000-0010-8000-00AA00389B71</c>。</summary>
    internal static readonly Guid MFVideoFormat_NV12 = new("3231564E-0000-0010-8000-00AA00389B71");

    // 设备源属性
    internal static readonly Guid MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE = new("c60ac5fe-252a-478f-a0ef-bc8fa5f7cad3");
    internal static readonly Guid MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID = new("8ac3587a-4ae7-42d8-99e0-0a6013eef90f");
    internal static readonly Guid MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME = new("60d0e559-52f8-4fa2-bbce-acdb34a8ec01");
    internal static readonly Guid MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK = new("58f0aad8-22bf-4f8a-bb3d-d2c4978c6e2f");

    /// <summary>把 HRESULT 错误码转成可读文本。</summary>
    internal static string HResultText(int hr) => hr switch
    {
        MF_E_NO_MORE_TYPES => "MF_E_NO_MORE_TYPES（没有更多可协商的媒体类型）",
        MF_E_ATTRIBUTENOTFOUND => "MF_E_ATTRIBUTENOTFOUND（请求的属性不存在）",
        MF_E_INVALIDMEDIATYPE => "MF_E_INVALIDMEDIATYPE（媒体类型不被接受）",
        MF_E_NOTACCEPTING => "MF_E_NOTACCEPTING（当前状态下不接受该调用）",
        MF_E_HW_MFT_FAILED_START_STREAMING => "MF_E_HW_MFT_FAILED_START_STREAMING（设备启动流失败，通常为被占用）",
        MF_E_INVALIDREQUEST => "MF_E_INVALIDREQUEST（无效请求）",
        unchecked((int)0x80070005) => "E_ACCESSDENIED（访问被拒绝：系统隐私设置未允许摄像头）",
        unchecked((int)0x8007001F) => "E_GENERIC（设备通用错误）",
        unchecked((int)0x80070490) => "E_ELEMENT_NOT_FOUND（找不到设备）",
        unchecked((int)0x80040154) => "REGDB_E_CLASSNOTREG（类未注册）",
        _ => $"0x{hr:X8}",
    };
}
