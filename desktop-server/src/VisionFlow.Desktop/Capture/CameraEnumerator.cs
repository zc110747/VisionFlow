using System.Runtime.InteropServices;

namespace VisionFlow.Desktop.Capture;

/// <summary>一个本地摄像头设备。</summary>
internal sealed record CameraDevice(
    string FriendlyName,
    string SymbolicLink,
    int Index)
{
    public override string ToString() => FriendlyName;
}

/// <summary>
/// 本地摄像头枚举器（MediaFoundation 设备源）。
///
/// <para><b>生命周期</b></para>
/// <para>
/// <c>MFStartup</c>/<c>MFShutdown</c> 是进程级引用计数。本项目只有一个
/// 长时间运行的 UI 进程，因此用 <see cref="MfRuntime"/> 做一次性初始化，
/// 由 <c>Program.Main</c> 在退出时统一反初始化。
/// </para>
///
/// <para><b>枚举为什么不用 WMI/PnP</b></para>
/// <para>
/// Windows PnP（<c>Get-PnpDevice -Class Camera</c>）列出的是**硬件节点**，
/// 包含不少不是视频采集源的设备（例如某些复合设备会多出一个 "UVC Control" 节点），
/// 且顺序与 DirectShow/MF 的设备索引**不一致**。MF 的
/// <c>MFEnumDeviceSources</c> 返回的才是可直接 <c>ActivateObject</c> 的
/// 视频采集源，索引与后续打开操作一一对应。所以枚举必须走 MF。
/// </para>
/// </summary>
internal static class CameraEnumerator
{
    private static readonly object Sync = new();
    private static bool _started;

    /// <summary>
    /// 确保 MediaFoundation 已初始化。多次调用安全（内部幂等）。
    /// </summary>
    internal static bool EnsureStarted(out string error)
    {
        lock (Sync)
        {
            if (_started)
            {
                error = string.Empty;
                return true;
            }

            int hr = MediaFoundation.MFStartup(MediaFoundation.MF_VERSION, MediaFoundation.MFSTARTUP_FULL);
            if (hr < 0)
            {
                error = $"MFStartup 失败: {MediaFoundation.HResultText(hr)}";
                return false;
            }

            _started = true;
            error = string.Empty;
            return true;
        }
    }

    /// <summary>反初始化。进程退出时调用。</summary>
    internal static void Shutdown()
    {
        lock (Sync)
        {
            if (!_started)
            {
                return;
            }

            MediaFoundation.MFShutdown();
            _started = false;
        }
    }

    /// <summary>
    /// 枚举本机视频采集设备。返回空列表表示无设备（或调用方环境不允许），
    /// 具体原因通过 <paramref name="diagnostics"/> 回传。
    /// </summary>
    internal static IReadOnlyList<CameraDevice> Enumerate(out IReadOnlyList<string> diagnostics)
    {
        var deviceList = new List<CameraDevice>();
        var diag = new List<string>();

        if (!EnsureStarted(out string startError))
        {
            diag.Add(startError);
            diagnostics = diag;
            return deviceList;
        }

        // 构造「只找视频采集源」的属性过滤器
        int hr = MediaFoundation.MFCreateAttributes(out IMFAttributes attrs, 1);
        if (hr < 0)
        {
            diag.Add($"MFCreateAttributes 失败: {MediaFoundation.HResultText(hr)}");
            diagnostics = diag;
            return deviceList;
        }

        var devType = MediaFoundation.MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE;
        var vidcap = MediaFoundation.MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID;
        hr = attrs.SetGUID(ref devType, ref vidcap);
        if (hr < 0)
        {
            diag.Add($"设置设备类型过滤失败: {MediaFoundation.HResultText(hr)}");
            ReleaseRef(attrs);
            diagnostics = diag;
            return deviceList;
        }

        hr = MediaFoundation.MFEnumDeviceSources(attrs, out IntPtr activateArrayPtr, out uint count);
        ReleaseRef(attrs);

        if (hr < 0)
        {
            diag.Add($"MFEnumDeviceSources 失败: {MediaFoundation.HResultText(hr)}");
            diagnostics = diag;
            return deviceList;
        }

        diag.Add($"MFEnumDeviceSources 返回 {count} 个激活对象");

        try
        {
            for (uint i = 0; i < count; i++)
            {
                // 该数组是 IMFActivate* 的连续数组，逐项解引用
                IntPtr itemPtr = Marshal.ReadIntPtr(activateArrayPtr, (int)(i * IntPtr.Size));
                if (itemPtr == IntPtr.Zero)
                {
                    diag.Add($"  [{i}] 空激活对象，跳过");
                    continue;
                }

                IMFActivate? activate = null;
                try
                {
                    activate = (IMFActivate)Marshal.GetObjectForIUnknown(itemPtr);

                    string friendlyName = ReadString(activate, MediaFoundation.MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME)
                                          ?? $"(未命名设备 #{i})";
                    string link = ReadString(activate, MediaFoundation.MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK)
                                  ?? string.Empty;

                    deviceList.Add(new CameraDevice(friendlyName, link, deviceList.Count));
                    diag.Add($"  [{deviceList.Count - 1}] {friendlyName}");
                }
                catch (Exception ex)
                {
                    diag.Add($"  [{i}] 读取设备属性异常: {ex.GetType().Name}: {ex.Message}");
                }
                finally
                {
                    // 释放我们的引用；数组里原始指针由 CoTaskMemFree 统一处理
                    if (activate is not null)
                    {
                        Marshal.ReleaseComObject(activate);
                    }
                }
            }
        }
        finally
        {
            Marshal.FreeCoTaskMem(activateArrayPtr);
        }

        diagnostics = diag;
        return deviceList;
    }

    /// <summary>
    /// 打开指定设备，返回可读帧的 <see cref="CameraCapture"/>。
    /// </summary>
    internal static CameraCapture? Open(int deviceIndex, Action<string>? log = null)
    {
        if (!EnsureStarted(out string startError))
        {
            log?.Invoke(startError);
            return null;
        }

        int hr = MediaFoundation.MFCreateAttributes(out IMFAttributes attrs, 1);
        if (hr < 0)
        {
            log?.Invoke($"MFCreateAttributes 失败: {MediaFoundation.HResultText(hr)}");
            return null;
        }

        var devType = MediaFoundation.MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE;
        var vidcap = MediaFoundation.MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID;
        attrs.SetGUID(ref devType, ref vidcap);

        hr = MediaFoundation.MFEnumDeviceSources(attrs, out IntPtr activateArrayPtr, out uint count);
        ReleaseRef(attrs);

        if (hr < 0)
        {
            log?.Invoke($"MFEnumDeviceSources 失败: {MediaFoundation.HResultText(hr)}");
            return null;
        }

        IMFActivate? activate = null;
        try
        {
            if (deviceIndex < 0 || deviceIndex >= count)
            {
                log?.Invoke($"设备索引 {deviceIndex} 越界（共 {count} 个）");
                return null;
            }

            IntPtr itemPtr = Marshal.ReadIntPtr(activateArrayPtr, deviceIndex * IntPtr.Size);
            if (itemPtr == IntPtr.Zero)
            {
                log?.Invoke($"设备索引 {deviceIndex} 的激活对象为空");
                return null;
            }

            activate = (IMFActivate)Marshal.GetObjectForIUnknown(itemPtr);

            var capture = new CameraCapture();
            if (!capture.Open(activate, log))
            {
                capture.Dispose();
                return null;
            }

            return capture;
        }
        catch (Exception ex)
        {
            log?.Invoke($"打开设备异常: {ex.GetType().Name}: {ex.Message}");
            return null;
        }
        finally
        {
            if (activate is not null)
            {
                Marshal.ReleaseComObject(activate);
            }

            Marshal.FreeCoTaskMem(activateArrayPtr);
        }
    }

    /// <summary>读取设备的友好名称（失败返回 null）。</summary>
    internal static string? ReadFriendlyName(IMFActivate activate)
        => ReadString(activate, MediaFoundation.MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);

    /// <summary>
    /// 从属性存储读取字符串。PROPVARIANT 必须分配在**非托管内存**上再传给
    /// GetItem（它按 void* 写入），不能用 ref struct 直接传。
    /// </summary>
    private static unsafe string? ReadString(IMFActivate attrs, Guid key)
    {
        int size = sizeof(PropVariant);
        IntPtr buffer = Marshal.AllocHGlobal(size);
        try
        {
            // 清零，避免 vt 为随机值导致 PropVariantClear 误判类型
            for (int i = 0; i < size; i++)
            {
                Marshal.WriteByte(buffer, i, 0);
            }

            int hr = attrs.GetItem(ref key, buffer);
            if (hr < 0)
            {
                return null;
            }

            var pv = *(PropVariant*)buffer;
            try
            {
                return pv.GetString();
            }
            finally
            {
                // MF 分配的宽字符串需要释放
                PropVariant.ClearNative(buffer);
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    /// <summary>释放非空 COM 引用（不置空调用方变量，仅减引用计数）。</summary>
    private static void ReleaseRef(object? comObject)
    {
        if (comObject is null || !Marshal.IsComObject(comObject))
        {
            return;
        }

        Marshal.ReleaseComObject(comObject);
    }
}
