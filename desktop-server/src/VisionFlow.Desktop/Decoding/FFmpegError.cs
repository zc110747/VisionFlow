using System.Runtime.InteropServices;
using FFmpeg.AutoGen;

namespace VisionFlow.Desktop.Decoding;

/// <summary>ffmpeg 错误码格式化。</summary>
internal static unsafe class FFmpegError
{
    /// <summary>把 ffmpeg 负错误码转成可读文本（含符号名）。</summary>
    public static string Text(int err)
    {
        const int bufSize = 256;
        byte* buf = stackalloc byte[bufSize];

        string message;
        if (ffmpeg.av_strerror(err, buf, bufSize) < 0)
        {
            message = "(无法解析)";
        }
        else
        {
            message = Marshal.PtrToStringAnsi((nint)buf) ?? "(空)";
        }

        return $"{err}({message})";
    }
}
