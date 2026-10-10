using System.Runtime.InteropServices;

namespace VisionFlow.Desktop.Capture;

/// <summary>
/// PROPVARIANT 的最小实现：只覆盖 MF 设备属性会用到的 VT_LPWSTR / VT_EMPTY。
///
/// <para>
/// MF 的设备属性（如 <c>MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME</c>）通过
/// <c>IMFAttributes::GetItem</c> 以 PROPVARIANT 返回。真实 PROPVARIANT 是
/// 16 字节的 union，首 2 字节是 vt 类型标签，后面按类型解释。
/// </para>
/// <para>
/// 这里只按 8 字节对齐留出足够空间（<c>vt(2) + reserved(6) + union(8) = 16</c>），
/// 并实现 <see cref="GetString"/> 读 LPWSTR 与 <see cref="Clear"/>
/// （调用 <c>PropVariantClear</c> 释放 MF 分配的字符串）。
/// </para>
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PropVariant
{
    internal ushort vt;
    internal ushort wReserved1;
    internal ushort wReserved2;
    internal ushort wReserved3;
    internal IntPtr p;          // 指针/整数载荷（LPWSTR 时是宽字符串地址）

    internal const ushort VT_EMPTY = 0;
    internal const ushort VT_LPWSTR = 31;

    /// <summary>当前值是否为宽字符串。</summary>
    internal readonly bool IsString => vt == VT_LPWSTR && p != IntPtr.Zero;

    /// <summary>读出字符串；非字符串时返回 null。</summary>
    internal readonly string? GetString()
    {
        if (!IsString)
        {
            return null;
        }

        var s = Marshal.PtrToStringUni(p);
        return string.IsNullOrEmpty(s) ? null : s;
    }

    /// <summary>
    /// 对**非托管内存**上的 PROPVARIANT 做清理（GetItem 写入的那种）。
    /// 这是唯一需要清理的路径：托管结构体上的字符串指针是 MF 分配的，
    /// 必须调用 <c>PropVariantClear</c> 释放。
    /// </summary>
    internal static void ClearNative(IntPtr pvar)
    {
        try
        {
            PropVariantClear(pvar);
        }
        catch (DllNotFoundException)
        {
            // ole32 理论上一定存在；真缺了就放弃清理，不让诊断路径崩掉
        }
        catch (EntryPointNotFoundException)
        {
            // 同上：清理失败不影响主流程
        }
    }

    /// <remarks>
    /// <c>ExactSpelling = true</c> 时，运行时按**方法名原样**查找导出。
    /// 因此方法名必须与真实导出名一致；要改名就必须显式写 <c>EntryPoint</c>，
    /// 否则会抛 <c>EntryPointNotFoundException</c>。
    /// </remarks>
    [DllImport("ole32.dll", EntryPoint = "PropVariantClear", ExactSpelling = true)]
    private static extern int PropVariantClear(IntPtr pvar);
}
