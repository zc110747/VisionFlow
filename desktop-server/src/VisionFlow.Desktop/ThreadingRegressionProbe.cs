using System.Drawing;
using System.Windows.Forms;
using VisionFlow.Desktop.Capture;
using VisionFlow.Desktop.Playback;

namespace VisionFlow.Desktop;

/// <summary>
/// 跨线程事件回归探针（--threadtest）。
///
/// <para><b>目的</b>：把 "打开本地摄像头 → 显示线程异常（InvalidOperationException: 跨线程操作无效）"
/// 的修复变成可执行回归守卫。原始 bug 的根因是 <see cref="CameraPreviewController"/> 的后台采集线程
/// 在 <c>CaptureLoop</c> 的 <c>finally</c> 与 <c>Stop()</c> 路径上触发
/// <see cref="CameraPreviewController.RunningChanged"/> / <see cref="CameraPreviewController.Ended"/>，
/// 而 <see cref="UI.MainForm"/> 的处理器直接访问了 WinForms 控件（未封送到 UI 线程）。</para>
///
/// <para><b>做法</b>：用真实 <see cref="CameraPreviewController"/> 在后台线程触发上述事件，
/// 处理器采用与 <c>MainForm.OnCameraRunningChanged / OnCameraEnded</c> 完全一致的封送惯用法
/// （<c>if (!IsDisposed &amp;&amp; InvokeRequired) BeginInvoke(...)</c>），再断言：</para>
/// <list type="bullet">
///   <item>全程零异常（修复前会抛 InvalidOperationException 并崩掉采集线程）</item>
///   <item>handler 实际在 UI 线程上执行（BeginInvoke 确实回灌了消息队列）</item>
///   <item>RunningChanged 至少收到 启动(true) 与 停止(false) 两次</item>
/// </list>
///
/// <para>另含 Phase B（机制证明）：用「未封送」handler 记录事件触发线程，断言事件
/// 确实在后台采集线程触发（.NET 下跨线程碰 <c>Control.Text</c> 并不抛异常，
/// 会抛的是 <c>MessageBox.Show(this,…)</c> 等路径），从而确定性证明 Phase A 的
/// 封送是必要的。</para>
///
/// <para>Phase A 还断言采集工作线程<b>实际读到帧</b>——这是
/// 「SourceReader 严禁跨 apartment 使用」的回归守卫：旧实现在 UI 线程（STA）建
/// Reader、工作线程（MTA）读帧，第一次 ReadSample 即抛 InvalidCastException
/// （E_NOINTERFACE）；修复后整个 MF 生命周期收在工作线程内（见
/// CameraPreviewController 类注释）。</para>
/// </summary>
internal static class ThreadingRegressionProbe
{
    public static int Run(string? outDir)
    {
        int pass = 0;
        int fail = 0;
        var failures = new List<string>();

        void Check(bool ok, string label)
        {
            if (ok)
            {
                pass++;
                Console.WriteLine($"  PASS  {label}");
            }
            else
            {
                fail++;
                failures.Add(label);
                Console.WriteLine($"  FAIL  {label}");
            }
        }

        // ── 建立 UI 线程（STA）与真实控件 ──
        Application.EnableVisualStyles();
        int uiThreadId = Environment.CurrentManagedThreadId;
        using var form = new Form();
        _ = form.Handle; // 强制创建句柄，绑定到 UI 线程

        var exceptions = new List<Exception>();
        int runningChanges = 0;
        int uiRunningHits = 0;
        int uiEndedHits = 0;

        // 与 MainForm 修复后的惯用法一致：先 IsDisposed 守卫，再 BeginInvoke 回 UI 线程。
        void SafeHandler(Action body)
        {
            try
            {
                body();
            }
            catch (Exception ex)
            {
                exceptions.Add(ex);
            }
        }

        var controller = new CameraPreviewController();
        controller.Log += m => Console.WriteLine($"[log] {m}");
        int frameCount = 0;
        controller.FrameReady += f =>
        {
            frameCount++;
            f.Dispose();
        };
        controller.RunningChanged += running =>
        {
            runningChanges++;
            if (form.IsDisposed)
            {
                return;
            }

            if (form.InvokeRequired)
            {
                form.BeginInvoke(new Action<bool>(r => SafeHandler(() =>
                {
                    form.Text = r ? "running" : "stopped";
                    if (Environment.CurrentManagedThreadId == uiThreadId)
                    {
                        uiRunningHits++;
                    }
                })), running);
            }
            else
            {
                SafeHandler(() =>
                {
                    form.Text = running ? "running" : "stopped";
                    if (Environment.CurrentManagedThreadId == uiThreadId)
                    {
                        uiRunningHits++;
                    }
                });
            }
        };
        controller.Ended += reason =>
        {
            if (form.IsDisposed)
            {
                return;
            }

            if (form.InvokeRequired)
            {
                form.BeginInvoke(new Action<string>(_ => SafeHandler(() =>
                {
                    if (Environment.CurrentManagedThreadId == uiThreadId)
                    {
                        uiEndedHits++;
                    }
                })), reason);
            }
            else
            {
                SafeHandler(() =>
                {
                    if (Environment.CurrentManagedThreadId == uiThreadId)
                    {
                        uiEndedHits++;
                    }
                });
            }
        };

        // ── Phase A：真实控制器跨线程事件 + 封送 ──
        Console.WriteLine("=== Phase A: 真实 CameraPreviewController 跨线程事件（封送）===");
        bool started;
        try
        {
            started = controller.Start(0);
        }
        catch (Exception ex)
        {
            exceptions.Add(ex);
            started = false;
        }

        Check(started, "摄像头成功打开并开始预览");

        // 泵消息 + 让采集线程出几帧
        for (int i = 0; i < 60 && controller.IsRunning; i++)
        {
            Application.DoEvents();
            Thread.Sleep(20);
        }

        try
        {
            controller.Stop(); // 后台线程 finally 触发 RunningChanged(false)（跨线程）
        }
        catch (Exception ex)
        {
            exceptions.Add(ex);
        }

        // 排空 BeginInvoke 回灌的回调
        for (int i = 0; i < 120; i++)
        {
            Application.DoEvents();
            Thread.Sleep(10);
        }

        Check(runningChanges >= 2, $"RunningChanged 至少收到 启动+停止 两次 (got={runningChanges})");
        Check(uiRunningHits >= 2, $"RunningChanged handler 在 UI 线程执行 (got={uiRunningHits})");
        Check(frameCount > 0, $"采集工作线程实际读到帧 (got={frameCount}) —— SourceReader 同线程生命周期守卫"
            + "（回归 2026-10 InvalidCastException：STA 建 Reader / MTA 读帧 → QI E_NOINTERFACE）");
        Check(exceptions.Count == 0, $"封送后全程零异常 (exceptions={exceptions.Count})");
        if (exceptions.Count > 0)
        {
            foreach (var ex in exceptions)
            {
                Console.WriteLine($"    [exception] {ex.GetType().Name}: {ex.Message}");
            }
        }

        controller.Dispose();
        form.Dispose();

        // ── Phase B：机制证明 —— 事件确实在后台线程触发（跨线程条件成立）──
        // 关键事实：CameraPreviewController 的 RunningChanged/Ended 由采集工作线程触发，
        // 而非 UI 线程。这就是原 bug 的根：handler 若直接碰控件就会跨线程访问。
        // 这里用“未封送”的 handler 记录触发线程，断言它 != UI 线程，
        // 从而确定性地证明 BeginInvoke 封送（Phase A）是必要的。
        // （注：.NET 下 Control.Text/Handle 跨线程访问不会抛异常——WinForms 把 WM_* 经消息循环
        // 静默封送；真正会抛的是 MessageBox.Show(this,…)/Enabled 等路径，故本阶段验证“触发线程”而非“是否抛异常”。）
        Console.WriteLine("=== Phase B: 机制证明（事件在后台线程触发 → 跨线程条件成立）===");
        int uiThreadIdB = Environment.CurrentManagedThreadId;
        int eventThreadId = -1;
        bool sawEvent = false;
        bool sawWorkerEvent = false;
        var ctl2 = new CameraPreviewController();
        ctl2.Log += _ => { };
        ctl2.RunningChanged += _ =>
        {
            // 故意“未封送”：直接记录触发线程（不碰任何控件，避免引入噪声）
            sawEvent = true;
            eventThreadId = Environment.CurrentManagedThreadId;
            if (eventThreadId != uiThreadIdB)
            {
                sawWorkerEvent = true;
            }
        };

        try
        {
            ctl2.Start(0);
        }
        catch (Exception ex)
        {
            exceptions.Add(ex);
        }

        for (int i = 0; i < 60 && ctl2.IsRunning; i++)
        {
            Application.DoEvents();
            Thread.Sleep(20);
        }

        try
        {
            ctl2.Stop();
        }
        catch (Exception ex)
        {
            exceptions.Add(ex);
        }

        for (int i = 0; i < 120; i++)
        {
            Application.DoEvents();
            Thread.Sleep(10);
        }

        Check(sawEvent, "未封送 handler 确实被触发");
        Check(sawWorkerEvent,
            $"存在在后台线程触发的事件（eventThread={eventThreadId}, uiThread={uiThreadIdB}）→ 跨线程条件成立");
        ctl2.Dispose();

        // ── 汇总 ──
        Console.WriteLine();
        Console.WriteLine($"RESULT running_changes={runningChanges}");
        Console.WriteLine($"RESULT handler_on_ui_thread={uiRunningHits}");
        Console.WriteLine($"RESULT phaseA_frames={frameCount}");
        Console.WriteLine($"RESULT ended_on_ui_thread={uiEndedHits}");
        Console.WriteLine($"RESULT phaseA_exceptions={exceptions.Count}");
        Console.WriteLine($"RESULT phaseB_saw_event={sawEvent}");
        Console.WriteLine($"RESULT phaseB_event_thread={eventThreadId}");
        Console.WriteLine($"RESULT phaseB_ui_thread={uiThreadIdB}");
        Console.WriteLine($"RESULT phaseB_worker_thread_event={sawWorkerEvent}");
        Console.WriteLine($"RESULT pass={pass} fail={fail}");
        Console.WriteLine($"RESULT verdict={(fail == 0 ? "PASS" : "FAIL")}");
        Console.WriteLine($"RESULT failures={fail}");

        if (fail > 0)
        {
            Console.WriteLine("RESULT failed_labels=" + string.Join(";", failures));
        }

        return fail == 0 ? 0 : 1;
    }
}
