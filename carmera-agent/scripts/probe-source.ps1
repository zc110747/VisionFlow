$root = 'D:\user_project\git\SOC-Agent\001-carmera_rtmp_net\carmera-agent'
$gst  = 'C:\Program Files\gstreamer\1.0\msvc_x86_64\bin'
$lines = @()

# The only camera here is exposed as a Media Foundation device, so try every
# Windows capture source and see which one actually links and produces frames.
$sources = @('dshowvideosrc', 'ksvideosrc', 'mfvideosrc')

foreach ($s in $sources) {
  $out = Join-Path $root "src-$s.out"
  $err = Join-Path $root "src-$s.err"
  if (Test-Path $err) { Remove-Item $err -Force }

  $desc = "$s device-index=0 ! videoconvert ! video/x-raw,format=I420,width=240,height=240,framerate=8/1 ! fakesink sync=false"
  $p = Start-Process -FilePath "$gst\gst-launch-1.0.exe" -ArgumentList ($desc -split ' ') `
       -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow -PassThru
  if (-not $p.WaitForExit(7000)) { Stop-Process -Id $p.Id -Force; $verdict = 'ran 7s without exiting (GOOD)' }
  else { $verdict = "exited early code=$($p.ExitCode)" }

  $errText = ((Get-Content $err -ErrorAction SilentlyContinue | Out-String).Trim())
  if (-not $errText) { $errText = '(clean - no errors)' }

  $lines += "=== $s : $verdict ==="
  $lines += $errText
  $lines += ""
}

Set-Content -Path (Join-Path $root 'e2e.log') -Value $lines -Encoding UTF8
