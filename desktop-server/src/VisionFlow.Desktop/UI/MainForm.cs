using System.Drawing;
using System.Windows.Forms;
using VisionFlow.Desktop.Ai;
using VisionFlow.Desktop.Capture;
using VisionFlow.Desktop.Network;
using VisionFlow.Desktop.Playback;
using VisionFlow.Desktop.Rendering;

namespace VisionFlow.Desktop.UI;

/// <summary>
/// 主窗体。
///
/// <para><b>已实现</b></para>
/// <list type="number">
///   <item>本地视频文件：FFmpeg 硬解优先，失败自动回退软解（阶段 2）</item>
///   <item>本地摄像头：MF 枚举 + 选中预览，AI 功能显式禁用（阶段 3）</item>
///   <item>网络相机：对接 video-server REST API（阶段 4）</item>
///   <item>AI metadata 叠加与模式切换（阶段 5）</item>
/// </list>
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

    private readonly PlayerController _player = new();
    private readonly CameraPreviewController _camera = new();
    private readonly NetworkCameraClient _network = new();
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

        _aiPoller = new AiMetadataPoller(_network);

        BuildLayout();
        WirePlayers();
        WireAi();

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
            TryAutoSelect(keyword);
        }
    }

    /// <summary>
    /// 按关键字自动选中一个可选源（仅用于 GUI 取证，便于截到非默认路径的状态）。
    /// 匹配 Display 或 Input，取第一个命中的。
    /// </summary>
    private void TryAutoSelect(string keyword)
    {
        for (int i = 0; i < _sourceCombo.Items.Count; i++)
        {
            if (_sourceCombo.Items[i] is SourceItem s &&
                (s.Display.Contains(keyword, StringComparison.OrdinalIgnoreCase) ||
                 s.Input.Contains(keyword, StringComparison.OrdinalIgnoreCase)))
            {
                Log($"[取证] 自动选中 #{i}: {s.Display}");
                _sourceCombo.SelectedIndex = i;
                return;
            }
        }

        Log($"[取证] 未找到匹配「{keyword}」的源，保持默认选择");
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
        _statusStrip.Items.Add(_statusLabel);
        _statusStrip.Items.Add(_sourceKindLabel);
        _statusStrip.Items.Add(_backendLabel);
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
        _camera.RunningChanged += OnCameraRunningChanged;
        _camera.Ended += reason =>
        {
            SetStatus($"采集结束：{reason}");
            MessageBox.Show(this, reason, "摄像头预览结束",
                MessageBoxButtons.OK, MessageBoxIcon.Warning);
        };
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
        if (IsDisposed)
        {
            return;
        }

        // 摄像头是直播：没有暂停/停止之外的控制，暂停无意义 → 只留停止
        _playPauseButton.Enabled = false;
        _playPauseButton.Text = "暂停";
        _stopButton.Enabled = running;
        SetStatus(running ? "摄像头预览中" : "摄像头已停止");
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

            // ── 网络相机 ──
            _sourceCombo.Items.Add(new SourceItemHeader("网络相机"));
            var cameras = _network.ListCameras(out var netDiag);
            foreach (var line in netDiag)
            {
                Log($"[网络] {line}");
            }

            if (cameras.Count == 0)
            {
                _sourceCombo.Items.Add(new SourceItemPlaceholder(
                    _network.LastError is { Length: > 0 } err
                        ? $"（{err}）"
                        : "（无网络相机）"));
            }
            else
            {
                int skipped = 0;

                // 在线优先：服务端返回的顺序按创建时间，最新建的排最前，
                // 而测试期建的多是离线相机 → 用户一进界面默认选中的就是死的。
                // 按「在线优先、同组内保持服务端原序」重排，避免默认踩坑。
                var ordered = cameras
                    .OrderByDescending(c => c.IsOnline)
                    .ToList();

                int onlineCount = ordered.Count(c => c.IsOnline);
                Log($"[网络] 在线 {onlineCount} / 共 {ordered.Count} 个相机");

                foreach (var c in ordered)
                {
                    var item = _network.ToSourceItem(c);
                    if (item is not null)
                    {
                        _sourceCombo.Items.Add(item);
                    }
                    else
                    {
                        skipped++;
                        Log($"[网络] 相机 {c.Id} 无可用播放地址（rtsp_url 与 stream_path 均缺失），已跳过");
                    }
                }

                if (skipped > 0)
                {
                    _sourceCombo.Items.Add(new SourceItemPlaceholder($"（{skipped} 个相机无播放地址）"));
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

        if (!_camera.Start(item.DeviceIndex))
        {
            SetStatus("摄像头打开失败");
            _backendLabel.Text = "后端: -";
            MessageBox.Show(this,
                $"无法打开摄像头：{item.Display}\n详情见下方日志。",
                "打开失败", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        var cap = _camera.Capture!;
        _stopButton.Enabled = true;
        _backendLabel.Text = $"后端: 摄像头采集 ({cap.Width}x{cap.Height} {cap.PixelFormat})";
    }

    private void StartPlayer(SourceItem item)
    {
        _sourceKindLabel.Text = item.Kind == SourceKind.NetworkCamera ? "源: 网络相机" : "源: 本地文件";

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

        // 先读取服务端当前模式，使下拉框与真实状态一致
        string? mode = _network.GetAiMode(item.CameraId);
        if (mode is not null)
        {
            Log($"[AI] 服务端当前模式: {mode}");
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
                _suppressAiModeEvent = true;
                _aiModeCombo.SelectedIndex = i;
                _suppressAiModeEvent = false;
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

        if (!_network.SetAiMode(cameraId, mode.Id))
        {
            Log($"[AI] 模式下发失败: {_network.LastError}");
            SetStatus($"AI 模式下发失败：{_network.LastError}");
            return;
        }

        Log($"[AI] 模式已下发: {mode.Id}");
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
    /// </summary>
    private void UpdateAiModeAvailability(SourceItem? item)
    {
        bool supported = item?.SupportsAi == true;

        _aiModeCombo.Enabled = supported;

        if (!supported)
        {
            // 置灰的同时清空选择，避免看起来「选了但没生效」
            _aiModeCombo.SelectedIndex = -1;
            return;
        }

        if (_aiModeCombo.Items.Count == 0)
        {
            PopulateAiModes();
        }

        if (_aiModeCombo.SelectedIndex < 0 && _aiModeCombo.Items.Count > 0)
        {
            _aiModeCombo.SelectedIndex = 0;
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
        Log($"video-server    : {_network.BaseUrl}");
        Log($"系统            : {Environment.OSVersion} / {(Environment.Is64BitProcess ? "x64" : "x86")}");
        Log("=== 自检结束 ===");
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _player.Dispose();
            _camera.Dispose();
            _network.Dispose();
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
