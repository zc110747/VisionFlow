using System.Runtime.InteropServices;
using FFmpeg.AutoGen;
using VisionFlow.Desktop.Rendering;

namespace VisionFlow.Desktop;

/// <summary>
/// 无界面自检。用 <c>--selfcheck</c> 调用，把环境探针结果打到 stdout
/// 并以进程退出码表达结论（0=通过）。用于构建后的自动化验收。
/// </summary>
internal static class SelfCheck
{
    public static int Run()
    {
        // 控制台默认代码页在管道重定向下是 GBK，中文会乱码。
        // 自检输出要求可被脚本 grep，所以强制 UTF-8。
        try
        {
            Console.OutputEncoding = System.Text.Encoding.UTF8;
        }
        catch (IOException)
        {
            // 无控制台（GUI 子系统且未附加）时忽略
        }

        int failures = 0;

        void Pass(string name, string detail) => Console.WriteLine($"[ PASS ] {name} :: {detail}");

        void Fail(string name, string detail)
        {
            Console.WriteLine($"[ FAIL ] {name} :: {detail}");
            failures++;
        }

        Console.WriteLine("=========== VisionFlow Desktop 环境自检 ===========");
        Console.WriteLine($"程序目录 : {AppContext.BaseDirectory}");
        Console.WriteLine($"进程架构 : {(Environment.Is64BitProcess ? "x64" : "x86")}");
        Console.WriteLine($"CPU 核心 : {Environment.ProcessorCount}");
        Console.WriteLine();

        // 1) ffmpeg 运行库定位 + 版本对齐校验 + 符号解析
        try
        {
            FFmpegLoader.InitializeVerbose(Console.WriteLine);
            Pass("ffmpeg 运行库加载", FFmpegLoader.ResolvedDirectory!);
        }
        catch (Exception ex)
        {
            Fail("ffmpeg 运行库加载", ex.Message);
            Console.WriteLine($"\n自检结论: 失败（{failures} 项）");
            return 1;
        }

        Console.WriteLine();

        // 2) 版本信息
        var version = FFmpegLoader.VersionInfo;
        if (string.IsNullOrEmpty(version))
        {
            Fail("ffmpeg 版本查询", "av_version_info 返回空");
        }
        else
        {
            Pass("ffmpeg 版本查询", version);
        }

        // 3) 已加载库的主版本（与 AutoGen 期望一致性已在加载期校验过）
        Pass("运行库版本对齐", string.Join(", ",
            FFmpegLoader.LoadedLibraryMajors.OrderBy(kv => kv.Key).Select(kv => $"{kv.Key}-{kv.Value}")));

        // 4) 硬解设备创建（需求 1 的真正前置条件）
        try
        {
            var detail = FFmpegLoader.DescribeAvailableHwaccels();
            Console.WriteLine($"[ INFO ] 硬解设备创建 :: {detail}");

            if (detail.Contains("可用=[D3D11VA"))
            {
                Pass("D3D11VA 硬解设备", "可创建");
            }
            else
            {
                Fail("D3D11VA 硬解设备", "无法创建，需求1 将只能走软解");
            }
        }
        catch (Exception ex)
        {
            Fail("硬解设备探测", ex.Message);
        }

        // 5) 解码器与关键能力存在性
        unsafe
        {
            CheckDecoder(AVCodecID.AV_CODEC_ID_H264, "H264");
            CheckDecoder(AVCodecID.AV_CODEC_ID_HEVC, "HEVC");

            void CheckDecoder(AVCodecID id, string label)
            {
                var codec = ffmpeg.avcodec_find_decoder(id);
                if (codec is null)
                {
                    Fail($"{label} 解码器", "avcodec_find_decoder 返回 null");
                    return;
                }

                string name = codec->name is null ? "(unnamed)" : Marshal.PtrToStringAnsi((nint)codec->name)!;
                Pass($"{label} 解码器", name);
            }
        }

        // 6) RTSP 拉流能力（需求 3 的直拉依赖）
        //    注意：rtsp 是 **demuxer**（libavformat），不是 avio 协议，
        //    所以不能用 avio_enum_protocols 判断 —— 那样会假失败。
        unsafe
        {
            var rtspFormat = ffmpeg.av_find_input_format("rtsp");
            if (rtspFormat is null)
            {
                Fail("RTSP demuxer", "av_find_input_format(\"rtsp\") 返回 null");
            }
            else
            {
                string name = rtspFormat->name is null
                    ? "(unnamed)"
                    : Marshal.PtrToStringAnsi((nint)rtspFormat->name)!;
                string longName = rtspFormat->long_name is null
                    ? string.Empty
                    : Marshal.PtrToStringAnsi((nint)rtspFormat->long_name)!;
                Pass("RTSP demuxer", $"name={name}, long_name={longName}");
            }

            // avio 协议层：rtsp 底层走 rtp/tcp/udp，这里校验这些确实在
            var protos = new List<string>();
            void* opaque = null;
            for (int index = 0; ; index++)
            {
                var p = ffmpeg.avio_enum_protocols(&opaque, index);
                if (p is null)
                {
                    break;
                }

                protos.Add(p);
            }

            Console.WriteLine($"[ INFO ] avio 协议({protos.Count}) :: {string.Join(", ", protos.OrderBy(x => x))}");

            foreach (var required in new[] { "tcp", "udp", "http" })
            {
                if (protos.Contains(required))
                {
                    Pass($"avio 协议 {required}", "可用");
                }
                else
                {
                    Fail($"avio 协议 {required}", "不可用");
                }
            }
        }

        Console.WriteLine();
        if (failures == 0)
        {
            Console.WriteLine("自检结论: 全部通过");
            return 0;
        }

        Console.WriteLine($"自检结论: 失败（{failures} 项）");
        return 1;
    }
}
