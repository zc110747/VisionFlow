using System.Drawing;
using System.Windows.Forms;
using VisionFlow.Desktop.Ai;
using VisionFlow.Desktop.Capture;
using VisionFlow.Desktop.Playback;
using VisionFlow.Desktop.Rendering;
using VisionFlow.Desktop.Server;

namespace VisionFlow.Desktop.UI;

/// <summary>
/// 主窗体。
///
/// <para><b>已实现</b></para>
/// <list type="number">
///   <item>本地视频文件：FFmpeg 硬解优先，失败自动回退软解</item>
///   <item>本地摄像头：MF 枚举 + 选中预览，AI 功能显式禁用</item>
///   <item>网络相机：<b>本端内嵌服务器</b>接收 agent 推流（不再连 video-server）</item>
///   <item>AI metadata 解析、叠加绘制与模式切换（走本端 8099 端点）</item>
/// </list>
///
/// <para><b>架构变更说明（重要）</b></para>
/// <para>
/// 本窗体曾经通过 <c>NetworkCameraClient</c> 去连 video-server 的 REST API。
/// 按最新设计，<b>desktop-server 自己就是视频服务器</b>：
/// <see cref="ServerHost"/> 内嵌托管 MediaMTX（RTSP :8554）并自己提供
/// metadata / aimode 的 HTTP 端点（:8099），agent 直接推过来。
/// 因此网络相机这一组数据全部来自本进程，不再依赖任何外部服务。
/// </para>
///
/// <para><b>呈现路径统一</b></para>
/// <para>
/// 文件/网络相机走 <see cref="PlayerController"/>，本地摄像头走
/// <see cref="CameraPreviewController"/>，两者都通过 <c>FrameReady</c>
/// 把 BGRA 位图交给同一个 <see cref="VideoCanvas"/>。
/// 源切换时只换控制器，渲染层不动。
/// </para>
/// </summary>
internal sealed class MainForm : Form
{
    private readonly VideoCanvas _canvas = new();
    private readonly ComboBox _sourceCombo = new();
    private readonly ComboBox _aiModeCombo = new();
    private readonly Button _openFileButton = new();
    private readonly Button _refreshButton = new();
    private readonly Button _playPauseButton = new();
    private readonly Button _stopButton = new();
    private readonly TextBox _logBox = new();
    private readonly StatusStrip _statusStrip = new();
    private readonly ToolStripStatusLabel _statusLabel = new();
    private readonly ToolStripStatusLabel _backendLabel = new();
    private readonly ToolStripStatusLabel _sourceKindLabel = new();
    private readonly ToolStripStatusLabel _serverLabel = new();

    private readonly PlayerController _player = new();
    private readonly CameraPreviewController _camera = new();

    /// <summary>内嵌视频服务器（MediaMTX + metadata 端点）。</summary>
    private readonly ServerHost _server = new();

    private readonly AiMetadataPoller _aiPoller;

    /// <summary>当前 AI 叠加数据（null = 无数据 / 已停止）。</summary>
    private AiOverlayState? _aiState;

    /// <summary>叠加是否可见（AI 模式非 ai-off 且有数据）。</summary>
    private bool _aiOverlayVisible;

    /// <summary>用户选中的 AI 模式（null = 未选择）。</summary>
    private string? _selectedAiMode;

    /// <summary>抑制 AI 模式下拉的 SelectedIndexChanged（程序化赋值时）。</summary>
    private bool _suppressAiModeEvent;

    /// <summary>防止下拉框在程序填充列表时触发播放。</summary>
    private bool _populating;

    /// <summary>当前选中的源。</summary>
    private SourceItem? _current;

    /// <summary>
    /// GUI 取证用：首次填充下拉框后自动选中的源关键字（null = 不自动选）。
    /// </summary>
    private readonly string? _autoSelectKeyword;

    public MainForm(string? autoSelectKeyword = null)
    {
        _autoSelectKeyword = autoSelectKeyword;

        Text = "VisionFlow Desktop — 视频显示与 AI 处理";
        MinimumSize = new Size(960, 640);
        Size = new Size(1280, 800);
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Microsoft YaHei UI", 9f);

        _aiPoller = new AiMetadataPoller(_server);

        BuildLayout();
        WirePlayers();
        WireAi();
        WireServer();

        _refreshButton.Click += OnRefreshSources;
        _playPauseButton.Click += OnPlayPause;
        _stopButton.Click += OnStop;
        _sourceCombo.SelectedIndexChanged += OnSourceChanged;
        _canvas.OverlayDraw += OnOverlayDraw;

        RunEnvironmentSelfCheck();

        // 首次加载在窗体显示后做，避免设计期/构造期就打开设备
        Shown += OnFirstShown;
    }

    private void OnFirstShown(object? sender, EventArgs e)
    {
        Shown -= OnFirstShown;
        RefreshSources(autoPlay: false);

        if (_autoSelectKeyword is { Length: > 0 } keyword)
        {
            // 取证场景下，目标源（如 agent 推流的网络相机）往往在窗口显示之后
            // 才被 MediaMTX 认出来。只尝试一次会静默落空，截图就抓不到该路径。
            // 因此按固定间隔重试若干次，直到选中或超时。
            // 注意：这是纯粹的取证辅助，正常交互路径不依赖它。
            const int attempts = 20;
            const int intervalMs = 1000;

            var retry = new System.Windows.Forms.Timer { Interval = intervalMs };
            int left = attempts;
            retry.Tick += (_, _) =>
            {
                // 先刷新再匹配：目标源可能在这 1 秒内才被 MediaMTX 认出来并入列。
                // SelectPreservingCurrent 会在重建列表时尽量保住当前选中项，
                // 因此不会把已经开始的播放打断。
                RefreshSources(autoPlay: false);

                if (TryAutoSelect(keyword) || --left <= 0)
                {
                    retry.Stop();
                    retry.Dispose();
                }
            };
            retry.Start();
        }
    }

    /// <summary>
    /// 按关键字自动选中一个可选源（仅用于 GUI 取证，便于截到非默认路径的状态）。
    /// 匹配 Display 或 Input，取第一个命中的。
    /// </summary>
    /// <returns>找到并已选中返回 true；未找到返回 false（供调用方重试）。</returns>
    private bool TryAutoSelect(string keyword)
    {
        for (int i = 0; i < _sourceCombo.Items.Count; i++)
        {
            if (_sourceCombo.Items[i] is SourceItem s &&
                (s.Display.Contains(keyword, StringComparison.OrdinalIgnoreCase) ||
                 s.Input.Contains(keyword, StringComparison.OrdinalIgnoreCase)))
            {
                Log($"[取证] 自动选中 #{i}: {s.Display}");

                if (_sourceCombo.SelectedIndex == i)
                {
                    // 目标已是当前选中项：SelectedIndexChanged 不会触发，直接启动
                    StartSource(s);
                    return true;
                }

                _sourceCombo.SelectedIndex = i;
                return true;
            }
        }

        return false;
    }

    private void BuildLayout()
    {
        // ── 顶部工具条 ──────────────────────────────────────────────
        var topPanel = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            Height = 88,
            ColumnCount = 8,
            RowCount = 2,
            Padding = new Padding(8, 6, 8, 6),
        };
        topPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 70));
        topPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        topPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 90));
        topPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 70));
        topPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 16));
        topPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 70));
        topPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 160));
        topPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 60));
        topPanel.RowStyles.Add(new RowStyle(SizeType.Absolute, 38));
        topPanel.RowStyles.Add(new RowStyle(SizeType.Absolute, 38));

        // 第一行：视频源
        topPanel.Controls.Add(MakeLabel("视频源", ContentAlignment.MiddleRight), 0, 0);

        _sourceCombo.Dock = DockStyle.Fill;
        _sourceCombo.DropDownStyle = ComboBoxStyle.DropDownList;
        _sourceCombo.Margin = new Padding(0, 0, 8, 0);
        topPanel.Controls.Add(_sourceCombo, 1, 0);

        _openFileButton.Text = "打开文件…";
        _openFileButton.Dock = DockStyle.Fill;
        _openFileButton.Margin = new Padding(0, 0, 8, 0);
        _openFileButton.Click += OnOpenFile;
        topPanel.Controls.Add(_openFileButton, 2, 0);

        _refreshButton.Text = "刷新";
        _refreshButton.Dock = DockStyle.Fill;
        _refreshButton.Margin = new Padding(0, 0, 8, 0);
        topPanel.Controls.Add(_refreshButton, 3, 0);

        topPanel.Controls.Add(new Panel { Dock = DockStyle.Fill }, 4, 0);
        topPanel.Controls.Add(MakeLabel("AI 模式", ContentAlignment.MiddleRight), 5, 0);

        _aiModeCombo.Dock = DockStyle.Fill;
        _aiModeCombo.DropDownStyle = ComboBoxStyle.DropDownList;
        _aiModeCombo.Margin = new Padding(0, 0, 8, 0);
        _aiModeCombo.Enabled = false;   // 由选中源决定是否启用
        _aiModeCombo.SelectedIndexChanged += OnAiModeChanged;
        topPanel.Controls.Add(_aiModeCombo, 6, 0);

        topPanel.Controls.Add(new Panel { Dock = DockStyle.Fill }, 7, 0);

        // 第二行：播放控制
        var controlRow = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            Margin = new Padding(0),
        };

        _playPauseButton.Text = "暂停";
        _playPauseButton.Width = 80;
        _playPauseButton.Enabled = false;
        controlRow.Controls.Add(_playPauseButton);

        _stopButton.Text = "停止";
        _stopButton.Width = 80;
        _stopButton.Enabled = false;
        controlRow.Controls.Add(_stopButton);

        topPanel.Controls.Add(controlRow, 1, 1);
        topPanel.SetColumnSpan(controlRow, 7);

        // ── 画面 + 日志 ─────────────────────────────────────────────
        var split = new SplitContainer
        {
            Dock = DockStyle.Fill,
            Orientation = Orientation.Horizontal,
            SplitterDistance = 420,
            Panel1MinSize = 200,
            Panel2MinSize = 80,
        };

        _canvas.Dock = DockStyle.Fill;
        split.Panel1.Controls.Add(_canvas);

        _logBox.Dock = DockStyle.Fill;
        _logBox.Multiline = true;
        _logBox.ReadOnly = true;
        _logBox.ScrollBars = ScrollBars.Vertical;
        _logBox.BackColor = Color.FromArgb(24, 24, 24);
        _logBox.ForeColor = Color.FromArgb(210, 210, 210);
        _logBox.Font = new Font("Consolas", 9f);
        _logBox.WordWrap = false;
        split.Panel2.Controls.Add(_logBox);

        // ── 状态栏 ─────────────────────────────────────────────────
        _statusLabel.Text = "就绪";
        _statusLabel.Spring = true;
        _statusLabel.TextAlign = ContentAlignment.MiddleLeft;
        _backendLabel.Text = "后端: -";
        _sourceKindLabel.Text = "源: -";
        _serverLabel.Text = "服务器: -";
        _statusStrip.Items.Add(_statusLabel);
        _statusStrip.Items.Add(_sourceKindLabel);
        _statusStrip.Items.Add(_backendLabel);
        _statusStrip.Items.Add(_serverLabel);
        _statusStrip.Dock = DockStyle.Bottom;

        Controls.Add(split);
        Controls.Add(_statusStrip);
        Controls.Add(topPanel);
    }

    private static Label MakeLabel(string text, ContentAlignment align) => new()
    {
        Text = text,
        Dock = DockStyle.Fill,
        TextAlign = align,
        Margin = new Padding(0, 0, 6, 0),
    };

    // ── 控制器接线 ──────────────────────────────────────────────────

    private void WirePlayers()
    {
        _player.Log += Log;
        _player.FrameReady += SubmitFrame;
        _player.PlayingChanged += OnPlayerPlayingChanged;
        _player.Ended += () => SetStatus("播放结束");

        _camera.Log += Log;
        _camera.FrameReady += SubmitFrame;
        // 注意：RunningChanged / Ended 由采集工作线程触发，
        // 其处理器内部会访问 UI 控件 / MessageBox，必须回到 UI 线程执行，
        // 否则抛 InvalidOperationException（跨线程操作无效）——即“显示线程异常”。
        _camera.RunningChanged += OnCameraRunningChanged;
        _camera.Ended += OnCameraEnded;
    }

    private void SubmitFrame(Bitmap frame)
    {
        if (IsDisposed)
        {
            frame.Dispose();
            return;
        }

        _canvas.Submit(frame);
    }

    private void OnPlayerPlayingChanged(bool playing)
    {
        // PlayingChanged 由播放工作线程触发，回 UI 线程后再碰控件。
        if (!IsDisposed && InvokeRequired)
        {
            BeginInvoke(() => OnPlayerPlayingChanged(playing));
            return;
        }

        if (IsDisposed)
        {
            return;
        }

        _playPauseButton.Text = playing ? "暂停" : "播放";
        _playPauseButton.Enabled = _player.Decoder is not null;
        _stopButton.Enabled = _player.Decoder is not null;
        SetStatus(playing ? "播放中" : "已暂停");
    }

    private void OnCameraRunningChanged(bool running)
    {
        // RunningChanged 由采集工作线程触发，回 UI 线程后再碰控件。
        if (!IsDisposed && InvokeRequired)
        {
            BeginInvoke(() => OnCameraRunningChanged(running));
            return;
        }

        if (IsDisposed)
        {
            return;
        }

        // 摄像头是直播：没有暂停/停止之外的控制，暂停无意义 → 只留停止
        _playPauseButton.Enabled = false;
        _playPauseButton.Text = "暂停";
        _stopButton.Enabled = running;
        SetStatus(running ? "摄像头预览中" : "摄像头已停止");

        // 设备打开成功（工作线程内完成，此时 Capture 已就绪）→ 回填协商出的格式
        if (running)
        {
            var cap = _camera.Capture;
            if (cap is not null)
            {
                _backendLabel.Text = $"后端: 摄像头采集 ({cap.Width}x{cap.Height} {cap.PixelFormat})";
            }
        }
    }

    private void OnCameraEnded(string reason)
    {
        // Ended 由采集工作线程触发，MessageBox.Show(this, ...) 必须在 UI 线程执行。
        if (!IsDisposed && InvokeRequired)
        {
            BeginInvoke(() => OnCameraEnded(reason));
            return;
        }

        if (IsDisposed)
        {
            return;
        }

        SetStatus($"采集结束：{reason}");
        MessageBox.Show(this, reason, "摄像头预览结束",
            MessageBoxButtons.OK, MessageBoxIcon.Warning);
    }

    // ── 源管理 ──────────────────────────────────────────────────────

    private void OnOpenFile(object? sender, EventArgs e)
    {
        using var dialog = new OpenFileDialog
        {
            Title = "选择视频文件",
            Filter = "视频文件|*.mp4;*.mkv;*.avi;*.mov;*.flv;*.wmv;*.ts;*.m4v;*.webm;*.mpg;*.mpeg;*.rmvb|所有文件|*.*",
            CheckFileExists = true,
        };

        if (dialog.ShowDialog(this) != DialogResult.OK)
        {
            return;
        }

        var item = new SourceItem(
            SourceKind.LocalFile,
            $"[文件] {Path.GetFileName(dialog.FileName)}",
            dialog.FileName);

        InsertSourceAndSelect(item);
    }

    private void OnRefreshSources(object? sender, EventArgs e) => RefreshSources(autoPlay: false);

    /// <summary>
    /// 重建下拉框：本地文件（保留已打开的） + 本地设备 + 网络相机。
    /// </summary>
    private void RefreshSources(bool autoPlay)
    {
        _populating = true;
        try
        {
            var previous = _current;

            _sourceCombo.Items.Clear();
            _sourceCombo.Items.Add(new SourceItemHeader("文件"));

            // 保留用户通过「打开文件」加进来的条目
            foreach (var item in _fileItems)
            {
                _sourceCombo.Items.Add(item);
            }

            // ── 本地设备 ──
            _sourceCombo.Items.Add(new SourceItemHeader("本地设备"));
            var devices = CameraEnumerator.Enumerate(out var diag);
            foreach (var line in diag)
            {
                Log($"[枚举] {line}");
            }

            if (devices.Count == 0)
            {
                _sourceCombo.Items.Add(new SourceItemPlaceholder("（未发现摄像头）"));
            }
            else
            {
                foreach (var d in devices)
                {
                    _sourceCombo.Items.Add(new SourceItem(
                        SourceKind.LocalCamera,
                        $"[本地] {d.FriendlyName}",
                        $"mf://device/{d.Index}",
                        d.Index));
                }
            }

            // ── 网络相机（来自本端内嵌服务器）──
            _sourceCombo.Items.Add(new SourceItemHeader("网络相机"));
            var cameras = _server.ListCameras(out var netDiag);
            foreach (var line in netDiag)
            {
                Log($"[服务器] {line}");
            }

            if (cameras.Count == 0)
            {
                _sourceCombo.Items.Add(new SourceItemPlaceholder(
                    _server.LastError is { Length: > 0 } err
                        ? $"（{err}）"
                        : "（暂无 agent 推流）"));
            }
            else
            {
                foreach (var item in cameras)
                {
                    _sourceCombo.Items.Add(item);
                }
            }

            // 恢复之前的选择
            if (previous is not null)
            {
                int idx = FindSelectableIndex(previous);
                if (idx >= 0)
                {
                    _sourceCombo.SelectedIndex = idx;
                    _populating = false;
                    if (autoPlay)
                    {
                        StartSource(previous);
                    }

                    return;
                }
            }

            SelectFirstSelectable();
        }
        finally
        {
            _populating = false;
        }
    }

    private readonly List<SourceItem> _fileItems = [];

    private void InsertSourceAndSelect(SourceItem item)
    {
        // 已存在同样输入的文件则不重复添加
        var existing = _fileItems.FirstOrDefault(f =>
            string.Equals(f.Input, item.Input, StringComparison.OrdinalIgnoreCase));

        if (existing is not null)
        {
            int idx = _sourceCombo.Items.IndexOf(existing);
            if (idx >= 0)
            {
                _sourceCombo.SelectedIndex = idx;
                return;
            }
        }

        _fileItems.Add(item);
        RefreshSources(autoPlay: false);

        int target = _sourceCombo.Items.IndexOf(item);
        if (target >= 0)
        {
            _sourceCombo.SelectedIndex = target;
        }
    }

    private int FindSelectableIndex(SourceItem item)
    {
        for (int i = 0; i < _sourceCombo.Items.Count; i++)
        {
            if (_sourceCombo.Items[i] is SourceItem s &&
                s.Kind == item.Kind &&
                string.Equals(s.Input, item.Input, StringComparison.OrdinalIgnoreCase))
            {
                return i;
            }
        }

        return -1;
    }

    private void SelectFirstSelectable()
    {
        for (int i = 0; i < _sourceCombo.Items.Count; i++)
        {
            if (_sourceCombo.Items[i] is SourceItem)
            {
                _sourceCombo.SelectedIndex = i;
                return;
            }
        }

        _sourceCombo.SelectedIndex = -1;
    }

    private void OnSourceChanged(object? sender, EventArgs e)
    {
        if (_populating)
        {
            return;
        }

        if (_sourceCombo.SelectedItem is not SourceItem item)
        {
            return;
        }

        StartSource(item);
    }

    /// <summary>启动指定源的播放/预览。</summary>
    private void StartSource(SourceItem item)
    {
        StopAll();

        _canvas.Clear();
        _logBox.Clear();
        _current = item;

        Log($"=== 打开: {item.Display}（{item.Kind}）===");
        SetStatus($"正在打开 {item.Display} …");

        switch (item.Kind)
        {
            case SourceKind.LocalCamera:
                StartCamera(item);
                break;

            case SourceKind.LocalFile:
            case SourceKind.NetworkCamera:
                StartPlayer(item);
                break;
        }

        UpdateAiModeAvailability(item);

        // 网络相机：启动 AI 数据轮询
        if (item.Kind == SourceKind.NetworkCamera)
        {
            StartAiPolling(item);
        }

        Text = $"VisionFlow Desktop — {item.Display}";
    }

    private void StartCamera(SourceItem item)
    {
        _sourceKindLabel.Text = "源: 本地摄像头";

        // 设备打开是异步的（在采集工作线程内完成，MF COM 对象严禁跨 apartment 使用）：
        // Start 只负责拉起工作线程；打开结果经 RunningChanged/Ended 事件回 UI 线程。
        if (!_camera.Start(item.DeviceIndex))
        {
            SetStatus("摄像头打开失败");
            _backendLabel.Text = "后端: -";
            MessageBox.Show(this,
                $"无法打开摄像头：{item.Display}\n详情见下方日志。",
                "打开失败", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        _stopButton.Enabled = true;
        _backendLabel.Text = "后端: 摄像头打开中…";
    }

    private void StartPlayer(SourceItem item)
    {
        _sourceKindLabel.Text = item.Kind == SourceKind.NetworkCamera ? "源: 网络相机" : "源: 本地文件";

        // 直播源（网络相机 / RTSP）没有固定时长，必须按“到达即呈现”处理。
        //
        // 若仍按时间戳节流：DecodeLoop 会计算 expectedMs = presented * frameInterval，
        // 而网络源的首帧等待时间远大于一帧间隔（握手 + 关键帧），于是
        // actualMs 一路领先 → waitMs < -200 恒成立 → 每一帧都被判定“落后太多”
        // 而 Dispose + continue，RaiseFrameReady 永不触发，画布永远停在占位符，
        // 但 IsPlaying 仍为 true（状态栏显示“播放中”）。
        // 症状极具误导性：日志有“[播放] 开始”，却没有任何帧、也没有错误。
        //
        // 本地文件仍保留节流（避免全速解码后瞬间丢帧）。
        _player.ThrottleByTimestamp = item.Kind != SourceKind.NetworkCamera;

        if (!_player.Play(item.Input))
        {
            SetStatus("打开失败");
            _backendLabel.Text = "后端: -";
            MessageBox.Show(this, $"无法打开：{item.Input}\n详情见下方日志。",
                "打开失败", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        var decoder = _player.Decoder!;
        _playPauseButton.Enabled = true;
        _stopButton.Enabled = true;

        string backendText = decoder.Backend switch
        {
            Decoding.DecodeBackend.Hardware => "硬件解码",
            Decoding.DecodeBackend.Software => "软件解码",
            _ => decoder.Backend.ToString(),
        };

        _backendLabel.Text = decoder.FallbackReason == Decoding.FallbackReason.None
            ? $"后端: {backendText}"
            : $"后端: {backendText}（由 {decoder.FallbackReason} 回退）";
    }

    private void StopAll()
    {
        _player.Stop();
        _camera.Stop();
        _aiPoller.Stop();
        _aiState = null;
        _aiOverlayVisible = false;
    }

    private void StartAiPolling(SourceItem item)
    {
        if (!item.SupportsAi || string.IsNullOrEmpty(item.CameraId))
        {
            return;
        }

        // 先读取本端维护的当前模式，使下拉框与真实状态一致
        // （这个值就是 agent 下次轮询 aimode 会拿到的那个，两侧天然一致）
        string? mode = _server.GetAiMode(item.CameraId);
        if (mode is not null)
        {
            Log($"[AI] 服务器当前模式: {mode}");
            SelectAiModeInCombo(mode);
            _selectedAiMode = mode;
            _aiOverlayVisible = !string.Equals(mode, "ai-off", StringComparison.Ordinal);
        }

        _aiPoller.Start(item.CameraId);
    }

    private void SelectAiModeInCombo(string mode)
    {
        for (int i = 0; i < _aiModeCombo.Items.Count; i++)
        {
            if (_aiModeCombo.Items[i] is AiModeItem m &&
                string.Equals(m.Id, mode, StringComparison.Ordinal))
            {
                // 直接赋值会触发 OnAiModeChanged 再下发一次；此处用标志抑制
                SetAiModeComboIndex(i);
                return;
            }
        }
    }

    // ── 播放控制 ────────────────────────────────────────────────────

    private void OnPlayPause(object? sender, EventArgs e)
    {
        var decoder = _player.Decoder;
        if (decoder is null)
        {
            return;
        }

        if (_player.IsPlaying)
        {
            _player.Stop();
            _playPauseButton.Text = "播放";
            SetStatus("已停止");
        }
        else
        {
            _player.Play(decoder.Input);
        }
    }

    private void OnStop(object? sender, EventArgs e)
    {
        StopAll();
        _canvas.Clear();
        _playPauseButton.Text = "暂停";
        _playPauseButton.Enabled = false;
        _stopButton.Enabled = false;
        _backendLabel.Text = "后端: -";
        _sourceKindLabel.Text = "源: -";
        UpdateAiModeAvailability(null);
        SetStatus("已停止");
        _current = null;
    }

    // ── AI 叠加（阶段 5）────────────────────────────────────────────

    private void WireAi()
    {
        _aiPoller.Log += Log;

        _aiPoller.FrameUpdated += state =>
        {
            if (IsDisposed)
            {
                return;
            }

            // 陈旧帧抑制：agent 在模式切换后仍可能吐出上一种模式的帧
            // （服务端也是有意的——它把 ai_mode 原样透传，由消费端判断）。
            // 若该帧的 ai_mode 与当前选中模式不符，丢弃不画，
            // 否则切换瞬间会看到「旧模式的框」闪一下。
            if (_selectedAiMode is { Length: > 0 } want &&
                state.AiMode is { Length: > 0 } got &&
                !string.Equals(want, got, StringComparison.Ordinal))
            {
                return;
            }

            _aiState = state;
            _aiOverlayVisible = true;
            _canvas.Invalidate();
        };

        _aiPoller.StatusUpdated += state =>
        {
            if (IsDisposed)
            {
                return;
            }

            if (InvokeRequired)
            {
                BeginInvoke(() => UpdateAiStatus(state));
                return;
            }

            UpdateAiStatus(state);
        };

        _aiPoller.Failed += reason =>
        {
            if (IsDisposed)
            {
                return;
            }

            if (InvokeRequired)
            {
                BeginInvoke(() => SetStatus($"AI: {reason}"));
                return;
            }

            SetStatus($"AI: {reason}");
        };
    }

    private void UpdateAiStatus(AiOverlayState state)
    {
        string mode = state.AiMode is { Length: > 0 } m ? m : "?";
        string model = state.Model is { Length: > 0 } mm ? mm : "-";

        _sourceKindLabel.Text =
            $"AI: {mode} {(state.Running ? "运行中" : "已停")} {state.Fps:F1}fps {model}";

        _aiState = state;
    }

    /// <summary>
    /// 叠加层绘制回调。由 <see cref="VideoCanvas"/> 在画面之后调用。
    /// </summary>
    private void OnOverlayDraw(Graphics g, VideoTransform transform)
    {
        var state = _aiState;
        if (state is null)
        {
            return;
        }

        AiOverlayRenderer.Draw(g, transform, state, _aiOverlayVisible);
    }

    private void OnAiModeChanged(object? sender, EventArgs e)
    {
        if (_suppressAiModeEvent)
        {
            return;
        }

        if (_aiModeCombo.SelectedItem is not AiModeItem mode || _current is null)
        {
            return;
        }

        _selectedAiMode = mode.Id;

        // ai-off：本地隐藏叠加。注意语义——服务端文档明确说明
        // ai-off 只是「UI 不再绘制」，agent 仍持有已加载的模型。
        if (string.Equals(mode.Id, "ai-off", StringComparison.Ordinal))
        {
            _aiOverlayVisible = false;
            _canvas.Invalidate();
            Log("[AI] 已关闭叠加显示（ai-off）");
        }
        else
        {
            _aiOverlayVisible = true;
        }

        // 只有网络相机有可下发的相机 ID；本地文件/摄像头不进入此分支
        if (_current.Kind != SourceKind.NetworkCamera)
        {
            Log($"[AI] 当前源不支持 AI 模式下发（{_current.Kind}）");
            return;
        }

        string cameraId = ResolveCameraId(_current);
        if (string.IsNullOrEmpty(cameraId))
        {
            Log("[AI] 无法确定相机 ID，模式未下发");
            return;
        }

        Log($"[AI] 下发模式 → {mode.Id}（相机 {cameraId}）");

        if (!_server.SetAiMode(cameraId, mode.Id))
        {
            Log($"[AI] 模式下发失败（相机 {cameraId} 非法模式或服务器未启动）");
            SetStatus($"AI 模式下发失败：{mode.Id}");
            return;
        }

        Log($"[AI] 模式已下发: {mode.Id}（agent 将在 ≤2s 内轮询到）");
    }

    /// <summary>
    /// 从下拉框条目反查相机 ID。
    /// <para>
    /// 播放地址是 RTSP URL，而 aimode 接口需要相机 ID，
    /// 两者不是同一个值，因此在下拉框条目的 Input 里带上原始相机 ID。
    /// </para>
    /// </summary>
    private static string ResolveCameraId(SourceItem item)
    {
        // Input 格式: rtsp://host:port/{streamPath}
        // camera id 与 stream_path 在 video-server 里可能不同，
        // 因此优先用显式记录的值。
        return item.CameraId;
    }

    /// <summary>
    /// 根据当前源更新 AI 模式下拉的可用性。
    /// <para>本地摄像头不支持 AI（需求 4 明确要求），必须置灰。</para>
    /// <para>
    /// <b>这里的选择动作必须抑制事件。</b>本方法是"把下拉框调整到与当前源相称的
    /// 状态"，属于 UI 回填，不是用户点选；一旦让 SelectedIndex 变化冒泡到
    /// <see cref="OnAiModeChanged"/>，就会把默认项当成用户命令真下发给 agent。
    /// </para>
    /// <para>
    /// 实测过的故障链：切换源 → 本方法把 SelectedIndex 从 -1 置为 0（ai-off）
    /// → OnAiModeChanged 走 SetAiMode 直写 <c>DesiredAiMode</c>（走的是
    /// 进程内 ServerHost.SetAiMode，**不经过 HTTP，所以任何服务端日志都看不到**）
    /// → agent 下一次 2s 轮询读到 ai-off。表现是"用户什么都没点，AI 却自己关了"。
    /// </para>
    /// </summary>
    private void UpdateAiModeAvailability(SourceItem? item)
    {
        bool supported = item?.SupportsAi == true;

        _aiModeCombo.Enabled = supported;

        if (!supported)
        {
            // 置灰的同时清空选择，避免看起来「选了但没生效」
            SetAiModeComboIndex(-1);
            return;
        }

        if (_aiModeCombo.Items.Count == 0)
        {
            PopulateAiModes();
        }

        if (_aiModeCombo.SelectedIndex < 0 && _aiModeCombo.Items.Count > 0)
        {
            SetAiModeComboIndex(0);
        }
    }

    /// <summary>
    /// 程序化设置 AI 模式下拉的选中项，并抑制随之而来的
    /// <see cref="OnAiModeChanged"/>——调用方若真想下发模式，必须显式调用
    /// <see cref="ServerHost.SetAiMode"/>，不能依赖事件副作用。
    /// </summary>
    private void SetAiModeComboIndex(int index)
    {
        _suppressAiModeEvent = true;
        try
        {
            _aiModeCombo.SelectedIndex = index;
        }
        finally
        {
            _suppressAiModeEvent = false;
        }
    }

    private void PopulateAiModes()
    {
        _aiModeCombo.Items.Add(new AiModeItem("ai-off", "关闭 (ai-off)"));
        _aiModeCombo.Items.Add(new AiModeItem("ai-y", "人检测 (ai-y)"));
        _aiModeCombo.Items.Add(new AiModeItem("ai-y-pose", "人体姿态 (ai-y-pose)"));
        _aiModeCombo.Items.Add(new AiModeItem("ai-hand", "手部关键点 (ai-hand)"));
    }

    // ── 日志与状态 ──────────────────────────────────────────────────

    public void Log(string message)
    {
        if (IsDisposed)
        {
            return;
        }

        if (InvokeRequired)
        {
            BeginInvoke(() => Log(message));
            return;
        }

        _logBox.AppendText($"{DateTime.Now:HH:mm:ss.fff}  {message}{Environment.NewLine}");
    }

    public void SetStatus(string text)
    {
        if (IsDisposed)
        {
            return;
        }

        if (InvokeRequired)
        {
            BeginInvoke(() => SetStatus(text));
            return;
        }

        _statusLabel.Text = text;
    }

    private void RunEnvironmentSelfCheck()
    {
        Log("=== VisionFlow Desktop 启动自检 ===");
        Log($"程序目录        : {AppContext.BaseDirectory}");
        Log($"ffmpeg 运行库   : {FFmpegLoader.ResolvedDirectory}");
        Log($"ffmpeg 版本     : {FFmpegLoader.VersionInfo}");
        Log($"硬解探测        : {FFmpegLoader.DescribeAvailableHwaccels()}");
        Log($"内嵌服务器      : {_server.RtspBase}（RTSP 接收 agent 推流）");
        Log($"metadata 端点   : {_server.MetadataUrl}");
        Log($"系统            : {Environment.OSVersion} / {(Environment.Is64BitProcess ? "x64" : "x86")}");
        Log("=== 自检结束 ===");
    }

    /// <summary>
    /// 接线内嵌服务器：启动它，并把"相机列表变化"事件搬回 UI 线程刷新下拉框。
    /// </summary>
    private void WireServer()
    {
        // CamerasChanged 由 MediaMTX 路径轮询线程 / HttpListener 线程触发，
        // 必须 BeginInvoke 回 UI 线程后才能碰控件。
        _server.CamerasChanged += () =>
        {
            if (IsDisposed)
            {
                return;
            }

            if (InvokeRequired)
            {
                BeginInvoke(RefreshServerStatus);
                return;
            }

            RefreshServerStatus();
        };

        if (_server.Start(Log))
        {
            SetServerLabel(ok: true);
            Log($"[服务器] agent 推流地址: {_server.RtspBase}/<camera_id>");
            Log($"[服务器] agent 的 --metadata-url: {_server.MetadataUrl}");
        }
        else
        {
            SetServerLabel(ok: false);
            Log($"[服务器] 启动失败: {_server.LastError}");
            Log("[服务器] 本地文件播放与本地摄像头不受影响，但网络相机不可用。");
        }
    }

    private void SetServerLabel(bool ok)
    {
        _serverLabel.Text = ok ? "服务器: 运行中" : "服务器: 未启动";
        _serverLabel.ForeColor = ok ? Color.FromArgb(120, 200, 120) : Color.FromArgb(220, 140, 120);
    }

    /// <summary>刷新服务器状态标签上的相机计数。</summary>
    private void RefreshServerStatus()
    {
        if (IsDisposed)
        {
            return;
        }

        var cameras = _server.ListCameras(out _);
        int online = cameras.Count(c => c.IsOnline);
        _serverLabel.Text = _server.IsRunning
            ? $"服务器: 运行中 ({online}/{cameras.Count} 在线)"
            : "服务器: 未启动";
        _serverLabel.ForeColor = _server.IsRunning
            ? Color.FromArgb(120, 200, 120)
            : Color.FromArgb(220, 140, 120);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _aiPoller.Dispose();
            _player.Dispose();
            _camera.Dispose();
            _server.Dispose();
            _canvas.Dispose();
        }

        base.Dispose(disposing);
    }
}

/// <summary>下拉框分组标题（不可选）。</summary>
internal sealed class SourceItemHeader(string text)
{
    public string Text { get; } = text;

    public override string ToString() => $"── {Text} ──";
}

/// <summary>下拉框占位提示（不可选）。</summary>
internal sealed class SourceItemPlaceholder(string text)
{
    public string Text { get; } = text;

    public override string ToString() => Text;
}

/// <summary>AI 模式条目。</summary>
internal sealed record AiModeItem(string Id, string Display)
{
    public override string ToString() => Display;
}
