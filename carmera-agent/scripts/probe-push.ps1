$root = 'D:\user_project\git\SOC-Agent\001-carmera_rtmp_net\carmera-agent'
$gst  = 'C:\Program Files\gstreamer\1.0\msvc_x86_64\bin'
$mtxDir = 'D:\data\agent-tools\mediamtx_v1.20.1_windows_amd64'
$lines = @()

Get-Process mediamtx -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600
$mtxLog = Join-Path $root 'mediamtx.log'
$mtx = Start-Process -FilePath "$mtxDir\mediamtx.exe" -WorkingDirectory $mtxDir `
       -RedirectStandardOutput $mtxLog -RedirectStandardError "$mtxLog.err" -NoNewWindow -PassThru
Start-Sleep -Seconds 2
$lines += "mediamtx pid=$($mtx.Id)"

$out = Join-Path $root 'probe-push.out'
$err = Join-Path $root 'probe-push.err'
if (Test-Path $out) { Remove-Item $out -Force }
if (Test-Path $err) { Remove-Item $err -Force }

# Correct form: rtspclientsink does its own payloading - feed it the encoded stream.
$desc = 'videotestsrc num-buffers=60 ! videoconvert ! x264enc tune=zerolatency speed-preset=veryfast ! h264parse ! rtspclientsink location=rtsp://127.0.0.1:8554/probeB'
$lines += "pipeline: $desc"

$p = Start-Process -FilePath "$gst\gst-launch-1.0.exe" -ArgumentList ($desc -split ' ') `
     -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow -PassThru
$exited = $p.WaitForExit(15000)
if (-not $exited) { Stop-Process -Id $p.Id -Force; $lines += "gst-launch: TIMEOUT -> killed" }
else { $lines += "gst-launch: exited code=$($p.ExitCode)" }

$lines += "--- gst-launch stderr ---"
$lines += ((Get-Content $err -ErrorAction SilentlyContinue | Out-String).Trim())
$lines += "--- mediamtx.log tail ---"
$lines += (Get-Content $mtxLog -Tail 15 -ErrorAction SilentlyContinue | Out-String)

Set-Content -Path (Join-Path $root 'e2e.log') -Value $lines -Encoding UTF8
