$root = 'D:\user_project\git\SOC-Agent\001-carmera_rtmp_net\carmera-agent'
$gst  = 'C:\Program Files\gstreamer\1.0\msvc_x86_64\bin'
$ff   = 'D:\data\agent-tools\ffmpeg-master-latest-win64-gpl-shared\bin'
$lines = @()

# How many frames does each Windows capture source actually deliver in 10s?
# The camera advertises 240x240 @ 8fps, so ~80 frames is the expectation.
$sources = @('dshowvideosrc', 'mfvideosrc')

foreach ($s in $sources) {
  # Forward slashes: gst_parse_launch treats "\" as an escape character.
  $file = ("$root/frames-$s.h264") -replace '\\', '/'
  $err  = Join-Path $root "frames-$s.err"

  $desc = "$s device-index=0 ! videoconvert ! video/x-raw,width=240,height=240,framerate=8/1 ! x264enc tune=zerolatency speed-preset=veryfast ! h264parse ! filesink location=$file"
  $p = Start-Process -FilePath "$gst\gst-launch-1.0.exe" -WorkingDirectory $root -ArgumentList ($desc -split ' ') `
       -RedirectStandardOutput (Join-Path $root "frames-$s.out") -RedirectStandardError $err -NoNewWindow -PassThru
  if (-not $p.WaitForExit(10000)) { Stop-Process -Id $p.Id -Force }
  Start-Sleep -Milliseconds 800

  $lines += "=== $s ==="
  if (Test-Path $file) {
    $sz = (Get-Item $file).Length
    $lines += "file size: $sz bytes"
    $cnt = & "$ff\ffprobe.exe" -v error -count_frames -select_streams v:0 `
           -show_entries stream=nb_read_frames -of default=nw=1:nk=1 $file 2>&1
    $lines += "frames decoded: $cnt"
  } else {
    $lines += "no output file produced"
  }
  $errText = ((Get-Content $err -ErrorAction SilentlyContinue | Out-String).Trim())
  if ($errText) { $lines += "stderr: $errText" }
  $lines += ""
}

Set-Content -Path (Join-Path $root 'e2e.log') -Value $lines -Encoding UTF8
