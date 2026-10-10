using VisionFlow.Desktop.Capture;

namespace VisionFlow.Desktop;

/// <summary>
/// 只枚举不采集：<c>--camtest --list</c>。
/// 用于快速确认下拉框实际会显示什么，以及区分「无设备」与「设备打不开」。
/// </summary>
internal static class CameraList
{
    internal static int Run()
    {
        ConsoleSetup.UseUtf8();

        Console.WriteLine("=== 本地摄像头枚举 ===");

        var devices = CameraEnumerator.Enumerate(out var diag);

        Console.WriteLine($"--- 诊断轨迹（{diag.Count} 行）---");
        foreach (var line in diag)
        {
            Console.WriteLine("  " + line);
        }

        Console.WriteLine("--- 设备列表 ---");
        if (devices.Count == 0)
        {
            Console.WriteLine("  (空)");
        }
        else
        {
            foreach (var d in devices)
            {
                Console.WriteLine($"  [{d.Index}] {d.FriendlyName}");
                if (!string.IsNullOrEmpty(d.SymbolicLink))
                {
                    Console.WriteLine($"      symbolicLink={d.SymbolicLink}");
                }
            }
        }

        Console.WriteLine($"RESULT devices={devices.Count}");
        Console.WriteLine($"RESULT verdict={(devices.Count > 0 ? "HAS_DEVICE" : "NO_DEVICE")}");
        Console.WriteLine("=== 枚举结束 ===");

        CameraEnumerator.Shutdown();
        return 0;
    }
}
