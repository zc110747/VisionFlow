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

# Drop the hard-coded format=I420 so videoconvert can negotiate the format the
# selected encoder actually supports (NV12 for nvh264enc, I420 for x264enc).
$cases = [ordered]@{
  'nvh264enc_noformat' = 'dshowvideosrc device-index=0 ! videoconvert ! video/x-raw,width=240,height=240,framerate=8/1 ! nvh264enc bitrate=4000 ! h264parse ! rtspclientsink location=rtsp://127.0.0.1:8554/probeHW'
  'x264enc_noformat'   = 'dshowvideosrc device-index=0 ! videoconvert ! video/x-raw,width=240,height=240,framerate=8/1 ! x264enc bitrate=4000 tune=zerolatency speed-preset=veryfast ! h264parse ! rtspclientsink location=rtsp://127.0.0.1:8554/probeSW'
}

foreach ($k in $cases.Keys) {
  $err = Join-Path $root "fmt-$k.err"
  if (Test-Path $err) { Remove-Item $err -Force }
  $lines += "=== $k ==="
  $p = Start-Process -FilePath "$gst\gst-launch-1.0.exe" -ArgumentList ($cases[$k] -split ' ') `
       -RedirectStandardOutput (Join-Path $root "fmt-$k.out") -RedirectStandardError $err -NoNewWindow -PassThru
  if (-not $p.WaitForExit(9000)) { Stop-Process -Id $p.Id -Force; $lines += 'ran 9s (GOOD)' }
  else { $lines += "exited early code=$($p.ExitCode) (BAD)" }
  $errText = ((Get-Content $err -ErrorAction SilentlyContinue | Out-String).Trim())
  if ($errText) { $lines += $errText } else { $lines += '(clean)' }
  $lines += ""
}

$lines += "--- mediamtx.log tail ---"
$lines += (Get-Content $mtxLog -Tail 14 -ErrorAction SilentlyContinue | Out-String)

Set-Content -Path (Join-Path $root 'e2e.log') -Value $lines -Encoding UTF8
