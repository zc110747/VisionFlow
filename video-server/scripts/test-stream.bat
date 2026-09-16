@echo off
setlocal
set FFMPEG=tools\ffmpeg-master-latest-win64-gpl\bin\ffmpeg.exe
if not exist "%FFMPEG%" (
  echo ffmpeg not found at %FFMPEG%
  exit /b 1
)
set STREAM=camera01
if not "%~1"=="" set STREAM=%~1
echo pushing test pattern to rtsp://localhost:8554/%STREAM%
"%FFMPEG%" -re -f lavfi -i testsrc=size=1280x720:rate=30 -pix_fmt yuv420p -c:v libx264 -preset ultrafast -g 30 -f rtsp rtsp://localhost:8554/%STREAM%
endlocal
