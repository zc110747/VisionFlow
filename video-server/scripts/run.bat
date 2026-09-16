@echo off
setlocal
cd /d "%~dp0\.."

REM Pick the NEWEST video-server*.exe. build.bat falls back to
REM video-server.new.exe when the canonical name is locked by a running
REM instance, so "newest wins" means you never run a stale binary by accident.
set "VS_BIN="
for /f "delims=" %%i in ('dir /b /o-d video-server*.exe 2^>nul') do (
  if not defined VS_BIN set "VS_BIN=%%i"
)
if not defined VS_BIN (
  echo ERROR: no video-server*.exe found. Build it first: scripts\build.bat
  exit /b 1
)
echo using %VS_BIN%

REM Optional overrides:
REM   run.bat                      -> config\config.yaml, bind from the config
REM   run.bat config\config.joint.yaml
"%VS_BIN%" %*
endlocal
