using System.Runtime.InteropServices;

namespace VisionFlow.Desktop.Rendering;

/// <summary>
/// kernel32 的 DLL 搜索路径控制。
/// <para>
/// 坑记录：<c>ExactSpelling = true</c> 与 <c>CharSet.Unicode</c> 同时使用时，
/// 运行时会去找**不带 W/A 后缀**的导出名（<c>SetDllDirectory</c>），
/// 而 kernel32 实际导出的是 <c>SetDllDirectoryW</c>，于是抛
/// "Unable to find an entry point named 'SetDllDirectory'"。
/// 去掉 <c>ExactSpelling</c>，让 marshaler 自动配 W 后缀即可。
/// </para>
/// </summary>
internal static class NativeDllPath
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AddDllDirectory(string newDirectory);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetDllDirectory(string lpPathName);

    /// <summary>把目录加入进程级 DLL 搜索路径。</summary>
    /// <param name="directory">绝对路径。</param>
    /// <param name="win32Error">任一调用失败时的 Win32 错误码（都成功则为 0）。</param>
    public static bool RegisterSearchDirectory(string directory, out int win32Error)
    {
        // SetDllDirectory 与 AddDllDirectory 是两条独立机制，都设上更稳。
        // 注意 AddDllDirectory 返回的是「cookie」，失败时才返回 null；
        // 这里只用它是否成功作为附加信号。
        bool added = AddDllDirectory(directory);
        int addErr = added ? 0 : Marshal.GetLastWin32Error();

        bool set = SetDllDirectory(directory);
        int setErr = set ? 0 : Marshal.GetLastWin32Error();

        win32Error = added ? 0 : (set ? 0 : (addErr != 0 ? addErr : setErr));
        return added || set;
    }
}
