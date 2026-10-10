using System.Runtime.InteropServices;

namespace VisionFlow.Desktop.Capture;

/// <summary>COM 对象释放辅助。设备热插拔常态下，Release 也可能抛异常。</summary>
internal static class ComReleaseExtensions
{
    /// <summary>
    /// 释放 COM 对象，吞掉释放过程中的异常。
    /// <para>
    /// 摄像头被拔出的瞬间，RCW 上的 Release 有可能因底层连接已断而抛
    /// <c>COMException</c>。这类异常在清理路径上没有任何补救价值，
    /// 但若冒泡出去会打断整个 Dispose 链，造成其余资源泄漏。
    /// </para>
    /// </summary>
    internal static void ReleaseComObjectSafely(this object? comObject)
    {
        if (comObject is null || !Marshal.IsComObject(comObject))
        {
            return;
        }

        try
        {
            Marshal.ReleaseComObject(comObject);
        }
        catch (Exception ex) when (ex is COMException or InvalidOperationException)
        {
            // 对象已断开或已被释放，无需处理
        }
    }
}
