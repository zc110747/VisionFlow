using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Diagnostics;

namespace VisionFlow.Desktop;

/// <summary>
/// GUI 取证工具：对指定进程的主窗口做 PrintWindow 截图。
///
/// <para><b>为什么用 PrintWindow 而不是 CopyFromScreen</b></para>
/// <para>
/// 自动化/沙箱会话里窗口经常不处于前台，<c>SetForegroundWindow</c>
/// 会失败（有前台锁定），屏幕抓取拿到的会是桌面或其他窗口。
/// <c>PrintWindow</c> 直接让目标窗口把自身渲染到我们提供的 DC，
/// 不依赖窗口可见性与 Z 序，是无人值守场景下唯一可靠的方式。
/// </para>
/// <para>
/// 注意 <c>PrintWindow</c> 要求目标进程与调用方处于同一会话，
/// 且被测窗口必须已经创建完毕（有非零 HWND）。
/// </para>
///
/// <para><b>为什么不用 <c>Process.MainWindowHandle</c></b></para>
/// <para>
/// 它是基于「该进程最近活跃的顶层窗口」的启发式，且要求窗口是
/// <c>WS_VISIBLE</c> 且没有 owner 的顶层窗口。实测在下拉列表弹出时，
/// 它返回的是 ComboBox 的弹出列表（704x189），而不是主窗口。
/// 因此这里改为 <c>EnumWindows</c> 枚举该进程全部顶层窗口，
/// 取**面积最大**的那个——主窗口一定比任何弹出控件大。
/// 经验值可靠，不依赖 WinForms 内部启发式。
/// </para>
/// </summary>
internal static class GuiCapture
{
    private const uint PW_RENDERFULLCONTENT = 0x00000002;

    [DllImport("user32.dll", ExactSpelling = true)]
    private static extern bool GetWindowRect(IntPtr hWnd, out Rect lpRect);

    [DllImport("user32.dll", ExactSpelling = true)]
    private static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);

    [DllImport("user32.dll", ExactSpelling = true)]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll", ExactSpelling = true)]
    private static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll", ExactSpelling = true)]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll", ExactSpelling = true)]
    private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    /// <summary>
    /// 抓取指定进程的主窗口。成功返回保存路径。
    /// </summary>
    /// <param name="processName">进程名（不含 .exe）。</param>
    /// <param name="outputPath">PNG 输出路径。</param>
    /// <param name="waitSeconds">等待窗口出现的最长秒数。</param>
    internal static int Run(string processName, string outputPath, int waitSeconds)
    {
        ConsoleSetup.UseUtf8();

        Console.WriteLine($"=== GUI 取证: {processName} ===");

        Process? target = null;
        var deadline = DateTime.UtcNow.AddSeconds(waitSeconds);

        // CAUTION: --guicapture runs as VisionFlow.Desktop.exe too, so a plain
        // GetProcessesByName("VisionFlow.Desktop") includes THIS process. Taking
        // list[0] then races against our own PID: when we win, the "target" has
        // no top-level window and the capture fails with "进程没有可见的顶层窗口".
        // Skip ourselves explicitly; also prefer a process that actually owns a
        // top-level window over one that merely exists.
        int selfId = Environment.ProcessId;

        while (DateTime.UtcNow < deadline)
        {
            var candidates = Process.GetProcessesByName(processName)
                                    .Where(p => p.Id != selfId)
                                    .ToArray();

            // Prefer a candidate that already has a visible top-level window.
            foreach (var p in candidates)
            {
                if (FindLargestTopLevelWindow((uint)p.Id, out _) != IntPtr.Zero)
                {
                    target = p;
                    break;
                }
            }

            // Fall back to any candidate (its window may still be coming up);
            // the wait loop below keeps polling for the window.
            if (target is null && candidates.Length > 0)
            {
                target = candidates[0];
            }

            if (target is not null)
            {
                break;
            }

            Thread.Sleep(250);
        }

        if (target is null)
        {
            Console.WriteLine($"RESULT found=False");
            Console.WriteLine("未找到进程");
            return 1;
        }

        Console.WriteLine($"RESULT pid={target.Id}");

        IntPtr hwnd = IntPtr.Zero;
        Rect rect = default;

        while (DateTime.UtcNow < deadline)
        {
            hwnd = FindLargestTopLevelWindow((uint)target.Id, out rect);
            if (hwnd != IntPtr.Zero && IsWindow(hwnd) &&
                rect.Right - rect.Left > 0 && rect.Bottom - rect.Top > 0)
            {
                break;
            }

            Thread.Sleep(250);
        }

        if (hwnd == IntPtr.Zero)
        {
            Console.WriteLine("RESULT hwnd=0");
            Console.WriteLine("进程没有可见的顶层窗口");
            return 1;
        }

        Console.WriteLine($"RESULT hwnd={hwnd}");
        Console.WriteLine($"RESULT visible={IsWindowVisible(hwnd)}");

        int w = rect.Right - rect.Left;
        int h = rect.Bottom - rect.Top;

        Console.WriteLine($"RESULT size={w}x{h}");

        using var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        using var g = Graphics.FromImage(bmp);

        // 先铺一层深色，便于看出哪部分是窗口未绘制的区域
        g.Clear(Color.FromArgb(30, 30, 30));

        IntPtr dc = g.GetHdc();
        bool ok;
        try
        {
            ok = PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT);
        }
        finally
        {
            g.ReleaseHdc(dc);
        }

        Console.WriteLine($"RESULT printwindow={ok}");

        if (!ok)
        {
            Console.WriteLine("PrintWindow 失败（窗口可能尚未完成首次绘制）");
        }

        string? dir = Path.GetDirectoryName(outputPath);
        if (!string.IsNullOrEmpty(dir))
        {
            Directory.CreateDirectory(dir);
        }

        bmp.Save(outputPath, ImageFormat.Png);
        Console.WriteLine($"RESULT saved={outputPath}");
        Console.WriteLine("=== 取证结束 ===");

        return ok ? 0 : 1;
    }

    /// <summary>
    /// 枚举某进程的全部顶层窗口，返回**面积最大**那个的句柄与矩形。
    /// 只统计可见窗口（隐藏的辅助窗口/消息窗口没有内容可截）。
    /// </summary>
    private static IntPtr FindLargestTopLevelWindow(uint processId, out Rect best)
    {
        IntPtr bestHandle = IntPtr.Zero;
        Rect bestRect = default;
        long bestArea = 0;

        EnumWindows((hWnd, _) =>
        {
            GetWindowThreadProcessId(hWnd, out uint pid);
            if (pid != processId || !IsWindowVisible(hWnd))
            {
                return true;    // 继续枚举
            }

            if (!GetWindowRect(hWnd, out Rect r))
            {
                return true;
            }

            long area = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (area > bestArea)
            {
                bestArea = area;
                bestHandle = hWnd;
                bestRect = r;
            }

            return true;    // 继续枚举
        }, IntPtr.Zero);

        best = bestRect;
        return bestHandle;
    }
}
