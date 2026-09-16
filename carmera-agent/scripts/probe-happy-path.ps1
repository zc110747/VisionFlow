$root = 'D:\user_project\git\SOC-Agent\001-carmera_rtmp_net\carmera-agent'
$gst   = 'C:\Program Files\gstreamer\1.0\msvc_x86_64\bin'
$ff    = 'D:\data\agent-tools\ffmpeg-master-latest-win64-gpl-shared\bin'
$mtxDir = 'D:\data\agent-tools\mediamtx_v1.20.1_windows_amd64'
$env:PATH = "$gst;$ff;$env:PATH"
$lines = @()

Get-Process mediamtx, camera-agent -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800

$mtxLog = Join-Path $root 'mediamtx.log'
$mtx = Start-Process -FilePath "$mtxDir\mediamtx.exe" -WorkingDirectory $mtxDir `
       -RedirectStandardOutput $mtxLog -RedirectStandardError "$mtxLog.err" -NoNewWindow -PassThru
Start-Sleep -Seconds 2

$agentLog = Join-Path $root 'agent.log'
$agent = Start-Process -FilePath (Join-Path $root 'build-msvc\src\camera-agent.exe') -ArgumentList `
    '--camera','0','--width','240','--height','240','--fps','8',
    '--stream','camera01','--log-level','debug','--duration','180' `
    -RedirectStandardOutput $agentLog -RedirectStandardError "$agentLog.err" -NoNewWindow -PassThru
Start-Sleep -Seconds 15
$lines += "agent pid=$($agent.Id) alive=$(-not $agent.HasExited)"

# 1) Does the SDP describe a live H264 stream?
$sdp = Join-Path $root 'ffprobe.log'
$q = Start-Process -FilePath "$ff\ffprobe.exe" -ArgumentList `
     '-rtsp_transport','tcp','-show_streams','-v','error','rtsp://127.0.0.1:8554/camera01' `
     -RedirectStandardOutput $sdp -RedirectStandardError "$sdp.err" -NoNewWindow -PassThru
if (-not $q.WaitForExit(15000)) { Stop-Process -Id $q.Id -Force; $lines += 'ffprobe: timeout' }
else { $lines += "ffprobe: exit=$($q.ExitCode)" }
$lines += "--- ffprobe output ---"
$lines += ((Get-Content $sdp -ErrorAction SilentlyContinue | Select-String -Pattern 'codec_name|width|height|r_frame_rate|avg_frame_rate' | Out-String).Trim())

# 2) Pull real frames
$cap = Join-Path $root 'capture-happy'
if (-not (Test-Path $cap)) { New-Item -ItemType Directory -Path $cap | Out-Null }
$fflog = Join-Path $root 'ffmpeg.log'
$p = Start-Process -FilePath "$ff\ffmpeg.exe" -ArgumentList `
     '-rtsp_transport','tcp','-i','rtsp://127.0.0.1:8554/camera01',
     '-frames:v','5','-y',"$cap/f_%03d.jpg" `
     -RedirectStandardOutput $fflog -RedirectStandardError "$fflog.err" -NoNewWindow -PassThru
if (-not $p.WaitForExit(30000)) { Stop-Process -Id $p.Id -Force; $lines += 'ffmpeg: TIMEOUT (no frames in 30s)' }
else { $lines += "ffmpeg: exit=$($p.ExitCode)" }
$jpgs = Get-ChildItem $cap -Filter '*.jpg' -ErrorAction SilentlyContinue
$lines += "frames captured: $($jpgs.Count)"
foreach ($j in $jpgs) { $lines += "  $($j.Name) $($j.Length) bytes" }

$lines += "--- agent.log tail 25 ---"
$lines += (Get-Content $agentLog -Tail 25 -ErrorAction SilentlyContinue | Out-String)
$lines += "--- mediamtx.log tail 12 ---"
$lines += (Get-Content $mtxLog -Tail 12 -ErrorAction SilentlyContinue | Out-String)

if (-not $agent.HasExited) { Stop-Process -Id $agent.Id -Force }
Get-Process mediamtx -ErrorAction SilentlyContinue | Stop-Process -Force
$lines += "[done] stopped"

Set-Content -Path (Join-Path $root 'e2e.log') -Value $lines -Encoding UTF8
