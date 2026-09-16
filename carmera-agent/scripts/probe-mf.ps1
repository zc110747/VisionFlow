$root = 'D:\user_project\git\SOC-Agent\001-carmera_rtmp_net\carmera-agent'
$gst   = 'C:\Program Files\gstreamer\1.0\msvc_x86_64\bin'
$ff    = 'D:\data\agent-tools\ffmpeg-master-latest-win64-gpl-shared\bin'
$mtxDir = 'D:\data\agent-tools\mediamtx_v1.20.1_windows_amd64'
$env:PATH = "$gst;$ff;$env:PATH"
$lines = @()

Get-Process mediamtx -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600
$mtxLog = Join-Path $root 'mediamtx.log'
$mtx = Start-Process -FilePath "$mtxDir\mediamtx.exe" -WorkingDirectory $mtxDir `
       -RedirectStandardOutput $mtxLog -RedirectStandardError "$mtxLog.err" -NoNewWindow -PassThru
Start-Sleep -Seconds 2

# mfvideosrc (media foundation) is what gst-device-monitor recommended for this UVC device.
$desc = "mfvideosrc device-index=0 ! videoconvert ! video/x-raw,width=240,height=240,framerate=8/1 " +
        "! nvh264enc ! h264parse ! rtspclientsink location=rtsp://127.0.0.1:8554/mf01"
$p = Start-Process -FilePath "$gst\gst-launch-1.0.exe" -ArgumentList ($desc -split ' ') `
     -RedirectStandardOutput (Join-Path $root 'mf.out') -RedirectStandardError (Join-Path $root 'mf.err') -NoNewWindow -PassThru
$lines += "gst-launch mfvideosrc pid=$($p.Id)"
Start-Sleep -Seconds 6

# Pull frames from MediaMTX
$cap = Join-Path $root 'capture-mf'
if (-not (Test-Path $cap)) { New-Item -ItemType Directory -Path $cap | Out-Null }
$fflog = Join-Path $root 'ffmpeg-mf.log'
$fp = Start-Process -FilePath "$ff\ffmpeg.exe" -ArgumentList `
     '-rtsp_transport','tcp','-i','rtsp://127.0.0.1:8554/mf01',
     '-frames:v','5','-y',"$cap/f_%03d.jpg" `
     -RedirectStandardOutput $fflog -RedirectStandardError "$fflog.err" -NoNewWindow -PassThru
if (-not $fp.WaitForExit(25000)) { Stop-Process -Id $fp.Id -Force; $lines += 'ffmpeg: TIMEOUT (no frames)' }
else { $lines += "ffmpeg: exit=$($fp.ExitCode)" }
$jpgs = Get-ChildItem $cap -Filter '*.jpg' -ErrorAction SilentlyContinue
$lines += "frames captured via mfvideosrc: $($jpgs.Count)"

if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Get-Process mediamtx -ErrorAction SilentlyContinue | Stop-Process -Force
$lines += "--- mf.err tail ---"
$lines += ((Get-Content (Join-Path $root 'mf.err') -Tail 8 -ErrorAction SilentlyContinue | Out-String).Trim())
$lines += "[done]"
Set-Content -Path (Join-Path $root 'e2e.log') -Value $lines -Encoding UTF8
