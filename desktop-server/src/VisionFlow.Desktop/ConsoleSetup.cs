namespace VisionFlow.Desktop;

/// <summary>
/// 控制台输出初始化。
///
/// <para>
/// 管道重定向时控制台默认代码页是 GBK（936），中文会乱码；
/// 所有无头诊断入口都要求输出可被脚本 grep，所以统一强制 UTF-8。
/// </para>
/// </summary>
internal static class ConsoleSetup
{
    internal static void UseUtf8()
    {
        try
        {
            Console.OutputEncoding = System.Text.Encoding.UTF8;
        }
        catch (IOException)
        {
            // 无控制台（GUI 子系统且未附加）时忽略
        }
    }
}
