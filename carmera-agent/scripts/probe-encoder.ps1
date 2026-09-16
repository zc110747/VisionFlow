$root = 'D:\user_project\git\SOC-Agent\001-carmera_rtmp_net\carmera-agent'
$gst  = 'C:\Program Files\gstreamer\1.0\msvc_x86_64\bin'
$lines = @()

# Isolate the encoder: does nvh264enc accept this camera's tiny resolution?
$cases = [ordered]@{
  'nvh264enc_240x240' = 'videotestsrc ! video/x-raw,format=I420,width=240,height=240,framerate=8/1 ! nvh264enc bitrate=4000 ! fakesink sync=false'
  'x264enc_240x240'   = 'videotestsrc ! video/x-raw,format=I420,width=240,height=240,framerate=8/1 ! x264enc tune=zerolatency speed-preset=veryfast ! fakesink sync=false'
  'nvh264enc_640x480' = 'videotestsrc ! video/x-raw,format=I420,width=640,height=480,framerate=30/1 ! nvh264enc bitrate=4000 ! fakesink sync=false'
}

foreach ($k in $cases.Keys) {
  $err = Join-Path $root "enc-$k.err"
  if (Test-Path $err) { Remove-Item $err -Force }

  $p = Start-Process -FilePath "$gst\gst-launch-1.0.exe" -ArgumentList ($cases[$k] -split ' ') `
       -RedirectStandardOutput (Join-Path $root "enc-$k.out") -RedirectStandardError $err -NoNewWindow -PassThru
  if (-not $p.WaitForExit(7000)) { Stop-Process -Id $p.Id -Force; $verdict = 'ran 7s (GOOD)' }
  else { $verdict = "exited early code=$($p.ExitCode) (BAD)" }

  $errText = ((Get-Content $err -ErrorAction SilentlyContinue | Out-String).Trim())
  if (-not $errText) { $errText = '(clean)' }

  $lines += "=== $k : $verdict ==="
  $lines += $errText
  $lines += ""
}

Set-Content -Path (Join-Path $root 'e2e.log') -Value $lines -Encoding UTF8
