$root = 'D:\user_project\git\SOC-Agent\001-carmera_rtmp_net\carmera-agent'
$gst  = 'C:\Program Files\gstreamer\1.0\msvc_x86_64\bin'
$lines = @()

# Does the hardware encoder keep producing, or does it stall?
# Metric: bytes written to filesink in 10s (same source, same resolution).
$cases = [ordered]@{
  'x264enc'   = 'x264enc bitrate=4000 tune=zerolatency speed-preset=veryfast'
  'nvh264enc' = 'nvh264enc bitrate=4000'
  'openh264enc' = 'openh264enc bitrate=4000000'
}

foreach ($k in $cases.Keys) {
  $file = ("$root/enc-flow-$k.h264") -replace '\\', '/'
  $err  = Join-Path $root "enc-flow-$k.err"

  $desc = "dshowvideosrc device-index=0 ! videoconvert ! video/x-raw,width=240,height=240,framerate=8/1 ! $($cases[$k]) ! h264parse ! filesink location=$file"
  $p = Start-Process -FilePath "$gst\gst-launch-1.0.exe" -WorkingDirectory $root -ArgumentList ($desc -split ' ') `
       -RedirectStandardOutput (Join-Path $root "enc-flow-$k.out") -RedirectStandardError $err -NoNewWindow -PassThru
  if (-not $p.WaitForExit(10000)) { Stop-Process -Id $p.Id -Force; $verdict = 'ran full 10s' }
  else { $verdict = "exited early code=$($p.ExitCode)" }
  Start-Sleep -Milliseconds 800

  $sz = 0
  if (Test-Path $file) { $sz = (Get-Item $file).Length }
  $lines += "=== $k : $verdict : $sz bytes ==="
  $errText = ((Get-Content $err -ErrorAction SilentlyContinue | Out-String).Trim())
  if ($errText) { $lines += "  stderr: $errText" }
}

Set-Content -Path (Join-Path $root 'e2e.log') -Value $lines -Encoding UTF8
