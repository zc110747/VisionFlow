using System.Runtime.InteropServices;
using FFmpeg.AutoGen;

namespace VisionFlow.Desktop.Rendering;

/// <summary>
/// FFmpeg 运行库加载与版本对齐校验。
///
/// <para><b>为什么需要版本校验</b></para>
/// <para>
/// FFmpeg.AutoGen 是「按编译期头文件版本号拼 DLL 文件名」去加载本机运行库的：
/// 它内部持有 <c>LIBAVUTIL_VERSION_MAJOR</c> 等常量，运行时找的是
/// <c>avutil-60.dll</c> 这种带主版本号的固定名字。所以 NuGet 包版本必须与
/// 本机 ffmpeg 运行库**主版本严格一致**，差 1 就直接加载失败。
/// </para>
/// <para>
/// 实测症状极具误导性：调用任何 av_* 函数（例如 <c>av_version_info</c>）时抛
/// <c>NotSupportedException: Specified method is not supported.</c>，
/// 看不出跟版本有关。曾在 AutoGen 8.0.0（期望 avutil-60 / avcodec-62）
/// 配本机 master 快照（avutil-61 / avcodec-63）时踩到，全部差 1 个主版本。
/// </para>
/// <para><b>版本对应表</b>（用于选型，改依赖前先查这里）</para>
/// <list type="table">
/// <item><description>AutoGen 8.0.0 → FFmpeg 8.0 / 8.1：avutil-60, avcodec-62, avformat-62, swscale-9, avfilter-11, swresample-6</description></item>
/// <item><description>FFmpeg 9.0 / master → avutil-61, avcodec-63, avformat-63, swscale-10, avfilter-12, swresample-7</description></item>
/// <item><description>FFmpeg 7.1 → avutil-59, avcodec-61, avformat-61, swscale-8, avfilter-10, swresample-5</description></item>
/// </list>
/// <para>
/// 本项目锁定：<b>AutoGen 8.0.0 + ffmpeg-n8.1 gpl-shared</b>（两者严格对齐）。
/// </para>
/// </summary>
internal static class FFmpegLoader
{
    /// <summary>探测到的、确实含有 avcodec 的目录。未初始化时为 null。</summary>
    public static string? ResolvedDirectory { get; private set; }

    /// <summary>ffmpeg 版本字符串（由 av_version_info 返回），未初始化时为 null。</summary>
    public static string? VersionInfo { get; private set; }

    /// <summary>已加载的各运行库主版本号，如 <c>{ "avutil": 60, "avcodec": 62 }</c>。</summary>
    public static IReadOnlyDictionary<string, int> LoadedLibraryMajors { get; private set; }
        = new Dictionary<string, int>();

    private static readonly string[] LibraryNames =
        ["avutil", "avcodec", "avformat", "avdevice", "avfilter", "swscale", "swresample"];

    /// <summary>候选运行库目录。优先取 exe 同目录（构建时已拷贝），其次环境变量。</summary>
    private static IEnumerable<string> CandidateDirectories()
    {
        // 1) exe 同目录 —— 构建目标 CopyFFmpegRuntime 会把 DLL 拷到这里
        yield return AppContext.BaseDirectory;

        // 2) 环境变量显式指定
        var env = Environment.GetEnvironmentVariable("FFMPEG_ROOT");
        if (!string.IsNullOrWhiteSpace(env))
        {
            yield return Path.Combine(env, "bin");
            yield return env;
        }

        // 3) 本机开发路径：8.1 稳定分支（与 AutoGen 8.0.0 对齐）
        yield return @"D:\data\agent-tools\ffmpeg-n8.1-latest-win64-gpl-shared\bin";
    }

    /// <summary>初始化。失败时抛 <see cref="InvalidOperationException"/>，消息含修复指引。</summary>
    public static void Initialize() => InitializeVerbose(null);

    /// <summary>
    /// 带诊断轨迹的初始化。
    /// </summary>
    /// <param name="trace">诊断输出回调；null 表示静默。</param>
    public static void InitializeVerbose(Action<string>? trace)
    {
        void T(string s) => trace?.Invoke(s);

        foreach (var dir in CandidateDirectories())
        {
            T($"探测目录: {dir} (存在={Directory.Exists(dir)})");

            if (!Directory.Exists(dir))
            {
                continue;
            }

            var majors = ScanLibraryMajors(dir);
            T($"  运行库主版本: [{FormatMajors(majors)}]");

            if (majors.Count == 0)
            {
                continue;
            }

            if (!NativeDllPath.RegisterSearchDirectory(dir, out int win32Error))
            {
                T($"  注册搜索路径失败 (Win32={win32Error})，跳过");
                continue;
            }

            ffmpeg.RootPath = dir;

            // 版本对齐校验：AutoGen 期望的主版本 vs 目录里实际存在的主版本。
            // 不对齐时给出明确报错，而不是让调用方看到 NotSupportedException。
            var expected = GetAutoGenExpectedMajors(T);
            var mismatch = DescribeMismatch(expected, majors);
            if (mismatch is not null)
            {
                throw new InvalidOperationException(
                    $"ffmpeg 运行库与 FFmpeg.AutoGen 版本不对齐：\n" +
                    $"  目录      : {dir}\n" +
                    $"  {mismatch}\n" +
                    "请改用与本机运行库主版本一致的 FFmpeg.AutoGen 包，" +
                    "或换成与包版本匹配的 ffmpeg 运行库。");
            }

            try
            {
                VersionInfo = ffmpeg.av_version_info();
            }
            catch (Exception ex)
            {
                throw new InvalidOperationException(
                    $"已找到 ffmpeg 运行库（{dir}）但符号解析失败：{ex.GetType().Name}: {ex.Message}\n" +
                    $"  目录内运行库: [{FormatMajors(majors)}]\n" +
                    $"  AutoGen 期望 : [{FormatMajors(expected)}]", ex);
            }

            ResolvedDirectory = dir;
            LoadedLibraryMajors = majors;
            T($"  已加载: {VersionInfo}");
            return;
        }

        throw new InvalidOperationException(
            "未找到 ffmpeg 运行库（avcodec-*.dll）。\n" +
            "请设置环境变量 FFMPEG_ROOT 指向 ffmpeg shared build 根目录，\n" +
            "或确认构建已把 DLL 拷贝到程序输出目录。");
    }

    /// <summary>扫描目录下各库文件名里的主版本号。</summary>
    private static Dictionary<string, int> ScanLibraryMajors(string dir)
    {
        var result = new Dictionary<string, int>();

        foreach (var name in LibraryNames)
        {
            // 形如 avutil-60.dll
            var file = Directory.EnumerateFiles(dir, $"{name}-*.dll")
                .Select(Path.GetFileNameWithoutExtension)
                .FirstOrDefault(f => f is not null);

            if (file is null)
            {
                continue;
            }

            int dash = file.LastIndexOf('-');
            if (dash >= 0 && int.TryParse(file.AsSpan(dash + 1), out int major))
            {
                result[name] = major;
            }
        }

        return result;
    }

    /// <summary>
    /// 从 FFmpeg.AutoGen 程序集里读它编译期固化的 <c>LIBAV*_VERSION_MAJOR</c> 常量。
    /// 这些常量决定了它运行时去加载哪些 DLL 名。
    /// </summary>
    private static Dictionary<string, int> GetAutoGenExpectedMajors(Action<string>? trace)
    {
        var result = new Dictionary<string, int>();
        var type = typeof(ffmpeg);
        const System.Reflection.BindingFlags flags =
            System.Reflection.BindingFlags.Public |
            System.Reflection.BindingFlags.NonPublic |
            System.Reflection.BindingFlags.Static;

        foreach (var name in LibraryNames)
        {
            var field = type.GetField($"LIB{name.ToUpperInvariant()}_VERSION_MAJOR", flags);
            if (field?.GetValue(null) is int v)
            {
                result[name] = v;
            }
        }

        trace?.Invoke($"  AutoGen 期望主版本: [{FormatMajors(result)}]");
        return result;
    }

    private static string FormatMajors(IReadOnlyDictionary<string, int> majors) =>
        majors.Count == 0
            ? "(空)"
            : string.Join(", ", majors.OrderBy(kv => kv.Key).Select(kv => $"{kv.Key}-{kv.Value}"));

    private static string? DescribeMismatch(
        IReadOnlyDictionary<string, int> expected,
        IReadOnlyDictionary<string, int> actual)
    {
        var diffs = new List<string>();

        foreach (var (name, exp) in expected.OrderBy(kv => kv.Key))
        {
            if (!actual.TryGetValue(name, out int act))
            {
                diffs.Add($"{name}: 期望 {exp}，目录中缺失");
            }
            else if (act != exp)
            {
                diffs.Add($"{name}: 期望 {exp}，实际 {act}");
            }
        }

        if (diffs.Count == 0)
        {
            return null;
        }

        return "  不一致项:\n" + string.Join("\n", diffs.Select(d => $"    - {d}"));
    }

    /// <summary>硬解可用的 hwaccel 列表（只创建 hw_device，不涉及解码器实例）。</summary>
    /// <remarks>
    /// 只探测本项目回退链真正会用到的类型。D3D11VA 是首选，
    /// DXVA2 作为老驱动兜底，QSV/CUDA 作为厂商加速的附加项。
    /// </remarks>
    public static unsafe string DescribeAvailableHwaccels()
    {
        AVHWDeviceType[] probe =
        [
            AVHWDeviceType.AV_HWDEVICE_TYPE_D3D11VA,
            AVHWDeviceType.AV_HWDEVICE_TYPE_DXVA2,
            AVHWDeviceType.AV_HWDEVICE_TYPE_QSV,
            AVHWDeviceType.AV_HWDEVICE_TYPE_CUDA,
        ];

        var ok = new List<string>();
        var fail = new List<string>();

        foreach (var type in probe)
        {
            AVBufferRef* ctx = null;
            int rc = ffmpeg.av_hwdevice_ctx_create(&ctx, type, null, null, 0);
            if (rc >= 0)
            {
                ok.Add(Short(type));
                ffmpeg.av_buffer_unref(&ctx);
            }
            else
            {
                fail.Add($"{Short(type)}:{ErrorText(rc)}");
            }
        }

        return $"可用=[{string.Join(", ", ok)}] 不可用=[{string.Join(", ", fail)}]";

        static string Short(AVHWDeviceType t) =>
            t.ToString().Replace("AV_HWDEVICE_TYPE_", string.Empty);
    }

    /// <summary>把 ffmpeg 负错误码转成可读文本。</summary>
    public static unsafe string ErrorText(int err)
    {
        const int bufSize = 256;
        byte* buf = stackalloc byte[bufSize];

        if (ffmpeg.av_strerror(err, buf, bufSize) < 0)
        {
            return $"error 0x{err:X8}";
        }

        return Marshal.PtrToStringAnsi((nint)buf) ?? $"error 0x{err:X8}";
    }
}
