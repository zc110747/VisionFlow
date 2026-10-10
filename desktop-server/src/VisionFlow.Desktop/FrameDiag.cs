using FFmpeg.AutoGen;
using VisionFlow.Desktop.Decoding;
using VisionFlow.Desktop.Rendering;

namespace VisionFlow.Desktop;

/// <summary>
/// 硬解/软解输出一致性诊断：<c>--diagframe &lt;输入&gt;</c>。
/// 打印首帧的像素格式、linesize、以及采样像素值，
/// 用于定位「硬解与软解画面不一致」类问题。
/// </summary>
internal static unsafe class FrameDiag
{
    public static int Run(string input)
    {
        try
        {
            Console.OutputEncoding = System.Text.Encoding.UTF8;
        }
        catch (IOException)
        {
        }

        FFmpegLoader.Initialize();

        Console.WriteLine("=========== 首帧解码诊断 ===========");

        Dump(input, hardware: false);
        Console.WriteLine();
        Dump(input, hardware: true);

        return 0;

        static void Dump(string input, bool hardware)
        {
            Console.WriteLine($"--- {(hardware ? "硬解" : "软解")} ---");

            AVFormatContext* fmt = null;
            if (ffmpeg.avformat_open_input(&fmt, input, null, null) < 0)
            {
                Console.WriteLine("打开失败");
                return;
            }

            ffmpeg.avformat_find_stream_info(fmt, null);
            int si = ffmpeg.av_find_best_stream(fmt, AVMediaType.AVMEDIA_TYPE_VIDEO, -1, -1, null, 0);
            var stream = fmt->streams[si];
            var par = stream->codecpar;

            var codec = ffmpeg.avcodec_find_decoder(par->codec_id);
            var ctx = ffmpeg.avcodec_alloc_context3(codec);
            ffmpeg.avcodec_parameters_to_context(ctx, par);
            ctx->pkt_timebase = stream->time_base;

            AVBufferRef* hwDev = null;
            if (hardware)
            {
                if (ffmpeg.av_hwdevice_ctx_create(
                        &hwDev, AVHWDeviceType.AV_HWDEVICE_TYPE_D3D11VA, null, null, 0) >= 0)
                {
                    ctx->hw_device_ctx = ffmpeg.av_buffer_ref(hwDev);
                }
            }

            if (ffmpeg.avcodec_open2(ctx, null, null) < 0)
            {
                Console.WriteLine("avcodec_open2 失败");
                return;
            }

            var pkt = ffmpeg.av_packet_alloc();
            var frame = ffmpeg.av_frame_alloc();
            var swFrame = ffmpeg.av_frame_alloc();
            SwsContext* sws = null;

            while (true)
            {
                if (ffmpeg.av_read_frame(fmt, pkt) < 0)
                {
                    break;
                }

                if (pkt->stream_index != si)
                {
                    ffmpeg.av_packet_unref(pkt);
                    continue;
                }

                ffmpeg.avcodec_send_packet(ctx, pkt);
                ffmpeg.av_packet_unref(pkt);

                if (ffmpeg.avcodec_receive_frame(ctx, frame) < 0)
                {
                    continue;
                }

                Console.WriteLine($"  原始帧: w={frame->width} h={frame->height} " +
                                  $"format={(AVPixelFormat)frame->format} " +
                                  $"linesize=[{frame->linesize[(uint)0]}, {frame->linesize[(uint)1]}]");

                AVFrame* src = frame;
                if (frame->format == (int)AVPixelFormat.AV_PIX_FMT_D3D11)
                {
                    ffmpeg.av_hwframe_transfer_data(swFrame, frame, 0);
                    ffmpeg.av_frame_copy_props(swFrame, frame);
                    src = swFrame;

                    Console.WriteLine($"  搬运后: w={src->width} h={src->height} " +
                                      $"format={(AVPixelFormat)src->format} " +
                                      $"linesize=[{src->linesize[(uint)0]}, {src->linesize[(uint)1]}]");
                }

                int w = src->width;
                int h = src->height;

                sws = ffmpeg.sws_getCachedContext(
                    sws, w, h, (AVPixelFormat)src->format, w, h, AVPixelFormat.AV_PIX_FMT_BGRA,
                    (int)SwsFlags.SWS_FAST_BILINEAR, null, null, null);

                // 用独立缓冲，打印与 Bitmap 相同的布局
                int stride = w * 4;
                byte* dst = (byte*)ffmpeg.av_malloc((ulong)(stride * h));
                byte*[] dstData = [dst];
                int[] dstLinesize = [stride];

                ffmpeg.sws_scale(sws, src->data, src->linesize, 0, h, dstData, dstLinesize);

                Console.WriteLine($"  转换后: stride={stride}");

                // 采样第一行前 16 个像素与第 100 行前 8 个像素
                Console.Write("  第 0 行前 16 像素 (BGRA): ");
                for (int x = 0; x < 16; x++)
                {
                    byte* p = dst + x * 4;
                    Console.Write($"[{p[0]},{p[1]},{p[2]}]");
                }

                Console.WriteLine();

                Console.Write("  第 100 行前 8 像素 (BGRA): ");
                for (int x = 0; x < 8; x++)
                {
                    byte* p = dst + (long)100 * stride + x * 4;
                    Console.Write($"[{p[0]},{p[1]},{p[2]}]");
                }

                Console.WriteLine();

                ffmpeg.av_freep(&dst);
                ffmpeg.av_frame_unref(frame);
                if (src == swFrame)
                {
                    ffmpeg.av_frame_unref(swFrame);
                }

                break;
            }

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
        }
    }
}
