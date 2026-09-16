$root = 'D:\user_project\git\SOC-Agent\001-carmera_rtmp_net\carmera-agent'
$gst  = 'C:\Program Files\gstreamer\1.0\msvc_x86_64\bin'
$lines = @()

# Which pipeline description can rtspclientsink actually link?
# A) current code: explicit rtph264pay before rtspclientsink
# B) drop rtph264pay - rtspclientsink selects its own payloader
$candidates = [ordered]@{
  'A_rtph264pay_then_rtspclientsink' =
    'videotestsrc num-buffers=30 ! videoconvert ! x264enc tune=zerolatency speed-preset=veryfast ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 ! rtspclientsink location=rtsp://127.0.0.1:8554/probeA'
  'B_h264parse_then_rtspclientsink' =
    'videotestsrc num-buffers=30 ! videoconvert ! x264enc tune=zerolatency speed-preset=veryfast ! h264parse ! rtspclientsink location=rtsp://127.0.0.1:8554/probeB'
}

foreach ($k in $candidates.Keys) {
  $out = Join-Path $root "probe-$k.out"
  $err = Join-Path $root "probe-$k.err"
  if (Test-Path $out) { Remove-Item $out -Force }
  if (Test-Path $err) { Remove-Item $err -Force }

  $p = Start-Process -FilePath "$gst\gst-launch-1.0.exe" `
       -ArgumentList ($candidates[$k] -split ' ') `
       -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow -PassThru
  $exited = $p.WaitForExit(12000)
  if (-not $exited) { Stop-Process -Id $p.Id -Force; $verdict = 'TIMEOUT (still running)' }
  else { $verdict = "exited code=$($p.ExitCode)" }

  $errText = (Get-Content $err -ErrorAction SilentlyContinue | Out-String).Trim()
  if (-not $errText) { $errText = '(no stderr)' }

  $lines += "=== $k : $verdict ==="
  $lines += $errText
  $lines += ""
}

Set-Content -Path (Join-Path $root 'e2e.log') -Value $lines -Encoding UTF8
