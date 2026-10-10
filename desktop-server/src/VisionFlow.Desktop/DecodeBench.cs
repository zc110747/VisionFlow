using System.Diagnostics;
using System.Runtime.InteropServices;
using FFmpeg.AutoGen;
using VisionFlow.Desktop.Decoding;
using VisionFlow.Desktop.Rendering;

namespace VisionFlow.Desktop;

/// <summary>
/// 解码性能剖析探针：<c>--decodebench &lt;输入&gt; [--soft] [--frames N]</c>。
/// 把「解码」「hwframe 搬运」「色彩转换」三段分别计时，
/// 用于判断硬解相对软解的真实收益与瓶颈位置。
/// </summary>
internal static unsafe class DecodeBench
{
    public static int Run(string input, bool forceSoftware, int frames)
    {
        try
        {
            Console.OutputEncoding = System.Text.Encoding.UTF8;
        }
        catch (IOException)
        {
        }

        FFmpegLoader.Initialize();

        Console.WriteLine("=========== 解码性能剖析 ===========");
        Console.WriteLine($"输入     : {input}");
        Console.WriteLine($"硬解     : {(forceSoftware ? "否" : "是")}");
        Console.WriteLine($"帧数     : {frames}");
        Console.WriteLine();

        AVFormatContext* fmt = null;
        int rc = ffmpeg.avformat_open_input(&fmt, input, null, null);
        if (rc < 0)
        {
            Console.WriteLine($"打开失败: {FFmpegError.Text(rc)}");
            return 1;
        }

        ffmpeg.avformat_find_stream_info(fmt, null);
        int si = ffmpeg.av_find_best_stream(fmt, AVMediaType.AVMEDIA_TYPE_VIDEO, -1, -1, null, 0);
        var stream = fmt->streams[si];
        var par = stream->codecpar;

        var codec = ffmpeg.avcodec_find_decoder(par->codec_id);
        var ctx = ffmpeg.avcodec_alloc_context3(codec);
        ffmpeg.avcodec_parameters_to_context(ctx, par);
        ctx->pkt_timebase = stream->time_base;

        // 硬解设备
        AVBufferRef* hwDev = null;
        if (!forceSoftware)
        {
            int hrc = ffmpeg.av_hwdevice_ctx_create(
                &hwDev, AVHWDeviceType.AV_HWDEVICE_TYPE_D3D11VA, null, null, 0);
            if (hrc >= 0)
            {
                ctx->hw_device_ctx = ffmpeg.av_buffer_ref(hwDev);
            }
            else
            {
                Console.WriteLine($"硬解设备创建失败，退化为软解: {FFmpegError.Text(hrc)}");
            }
        }

        rc = ffmpeg.avcodec_open2(ctx, null, null);
        if (rc < 0)
        {
            Console.WriteLine($"avcodec_open2 失败: {FFmpegError.Text(rc)}");
            return 1;
        }

        var pkt = ffmpeg.av_packet_alloc();
        var frame = ffmpeg.av_frame_alloc();
        var swFrame = ffmpeg.av_frame_alloc();
        SwsContext* sws = null;

        // 计时要点：FFmpeg 的解码是**异步**的 —— 真正的解码工作发生在
        // avcodec_send_packet 里（内部可能一次产生多帧），而
        // avcodec_receive_frame 通常只是把已解好的帧取出来（近乎零成本）。
        // 只计时 receive 会得到「解码 0.01ms/帧」这种荒谬结果。
        // 正确做法：把 send 与 receive 一起计入「解码」。
        double tDecode = 0;      // send_packet + receive_frame
        double tTransfer = 0;    // av_hwframe_transfer_data
        double tConvert = 0;     // sws_scale
        int decoded = 0;
        bool sawHwFrame = false;

        var sw = new Stopwatch();

        while (decoded < frames)
        {
            rc = ffmpeg.av_read_frame(fmt, pkt);
            if (rc < 0)
            {
                sw.Restart();
                ffmpeg.avcodec_send_packet(ctx, null);
                sw.Stop();
                tDecode += sw.Elapsed.TotalMilliseconds;
            }
            else
            {
                if (pkt->stream_index != si)
                {
                    ffmpeg.av_packet_unref(pkt);
                    continue;
                }

                sw.Restart();
                ffmpeg.avcodec_send_packet(ctx, pkt);
                sw.Stop();
                tDecode += sw.Elapsed.TotalMilliseconds;

                ffmpeg.av_packet_unref(pkt);
            }

            while (true)
            {
                int rrc = ffmpeg.avcodec_receive_frame(ctx, frame);
                if (rrc < 0)
                {
                    break;
                }

                AVFrame* src = frame;

                int pixFmt = frame->format;
                if (pixFmt == (int)AVPixelFormat.AV_PIX_FMT_D3D11 ||
                    pixFmt == (int)AVPixelFormat.AV_PIX_FMT_DXVA2_VLD)
                {
                    sawHwFrame = true;

                    sw.Restart();
                    ffmpeg.av_hwframe_transfer_data(swFrame, frame, 0);
                    sw.Stop();
                    tTransfer += sw.Elapsed.TotalMilliseconds;

                    ffmpeg.av_frame_copy_props(swFrame, frame);
                    src = swFrame;
                }

                int w = src->width;
                int h = src->height;
                var srcFmt = (AVPixelFormat)src->format;

                sws = ffmpeg.sws_getCachedContext(
                    sws, w, h, srcFmt, w, h, AVPixelFormat.AV_PIX_FMT_BGRA,
                    (int)SwsFlags.SWS_BILINEAR, null, null, null);

                byte* buf = (byte*)ffmpeg.av_malloc((ulong)(w * h * 4));
                byte*[] dst = [buf];
                int[] linesize = [w * 4];

                sw.Restart();
                ffmpeg.sws_scale(sws, src->data, src->linesize, 0, h, dst, linesize);
                sw.Stop();
                tConvert += sw.Elapsed.TotalMilliseconds;

                ffmpeg.av_freep(&buf);

                ffmpeg.av_frame_unref(frame);
                if (src == swFrame)
                {
                    ffmpeg.av_frame_unref(swFrame);
                }

                decoded++;

                if (decoded >= frames)
                {
                    break;
                }
            }

            if (rc < 0 && decoded < frames)
            {
                break;
            }
        }

        Console.WriteLine($"实际后端      : {(sawHwFrame ? "Hardware（观察到硬解帧）" : "Software")}");
        Console.WriteLine($"已解码帧数    : {decoded}");
        Console.WriteLine();
        Console.WriteLine($"解码耗时      : {tDecode,8:F1} ms  ({tDecode / Math.Max(decoded, 1),6:F2} ms/帧)");
        Console.WriteLine($"hwframe 搬运  : {tTransfer,8:F1} ms  ({tTransfer / Math.Max(decoded, 1),6:F2} ms/帧)");
        Console.WriteLine($"色彩转换(BGRA): {tConvert,8:F1} ms  ({tConvert / Math.Max(decoded, 1),6:F2} ms/帧)");
        Console.WriteLine($"合计          : {tDecode + tTransfer + tConvert,8:F1} ms");

        double total = tDecode + tTransfer + tConvert;
        if (total > 0)
        {
            Console.WriteLine();
            Console.WriteLine($"占比: 解码 {tDecode / total * 100:F1}% | " +
                              $"搬运 {tTransfer / total * 100:F1}% | " +
                              $"转换 {tConvert / total * 100:F1}%");
        }

        // 清理
        if (sws is not null)
        {
            ffmpeg.sws_freeContext(sws);
        }

        ffmpeg.av_frame_free(&frame);
        ffmpeg.av_frame_free(&swFrame);
        ffmpeg.av_packet_free(&pkt);
        ffmpeg.avcodec_free_context(&ctx);
        if (hwDev is not null)
        {
            ffmpeg.av_buffer_unref(&hwDev);
        }

        ffmpeg.avformat_close_input(&fmt);
        return 0;
    }
}
