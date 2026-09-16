$root = 'D:\user_project\git\SOC-Agent\001-carmera_rtmp_net\carmera-agent'
$log  = Join-Path $root 'e2e.log'
$gst  = 'C:\Program Files\gstreamer\1.0\msvc_x86_64\bin'
$lines = @()

Get-Process mediamtx -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$mtx = Start-Process -FilePath 'D:\data\agent-tools\mediamtx_v1.20.1_windows_amd64\mediamtx.exe' `
       -WorkingDirectory 'D:\data\agent-tools\mediamtx_v1.20.1_windows_amd64' `
       -RedirectStandardOutput (Join-Path $root 'mediamtx.log') `
       -RedirectStandardError  (Join-Path $root 'mediamtx.log.err') -NoNewWindow -PassThru
Start-Sleep -Seconds 2
$lines += "mediamtx pid=$($mtx.Id)"

$gl = Join-Path $root 'gstlaunch.log'
if (Test-Path $gl) { Remove-Item $gl -Force }

$desc = 'dshowvideosrc device-index=0 ! videoconvert ! video/x-raw,format=I420,width=240,height=240,framerate=8/1 ! x264enc bitrate=4000 key-int-max=30 tune=zerolatency speed-preset=veryfast ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 ! rtspclientsink location=rtsp://127.0.0.1:8554/camera01'
$lines += "pipeline: $desc"
$lines += ""

$g = Start-Process -FilePath "$gst\gst-launch-1.0.exe" -ArgumentList ($desc -split ' ') `
      -RedirectStandardOutput $gl -RedirectStandardError "$gl.err" -NoNewWindow -PassThru
Start-Sleep -Seconds 14
if (-not $g.HasExited) { Stop-Process -Id $g.Id -Force }
Start-Sleep -Milliseconds 500

$lines += "--- gst-launch stdout ---"
$lines += (Get-Content $gl -ErrorAction SilentlyContinue | Out-String)
$lines += "--- gst-launch stderr ---"
$lines += (Get-Content "$gl.err" -ErrorAction SilentlyContinue | Out-String)
$lines += "--- mediamtx.log (publisher events) ---"
$lines += (Get-Content (Join-Path $root 'mediamtx.log') -Tail 12 -ErrorAction SilentlyContinue | Out-String)

Set-Content -Path $log -Value $lines -Encoding UTF8
