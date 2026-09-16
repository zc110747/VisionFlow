@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ============================================================================
REM  Stop the joint run: video-server, camera-agent and the MediaMTX child.
REM
REM  Order matters. video-server is killed with /T so its MediaMTX child goes
REM  with it; the standalone mediamtx.exe kill afterwards is a safety net for
REM  an orphan left behind by a crashed server.
REM ============================================================================

echo [*] Stopping joint run ...

taskkill /F /T /IM video-server.exe     >nul 2>&1
taskkill /F /T /IM video-server.new.exe >nul 2>&1
taskkill /F /IM camera-agent.exe        >nul 2>&1
taskkill /F /IM mediamtx.exe        >nul 2>&1
taskkill /F /IM ffplay.exe          >nul 2>&1

C:\Windows\System32\timeout.exe /t 2 >nul 2>&1

set "LEFT="
for %%p in (video-server.exe video-server.new.exe camera-agent.exe mediamtx.exe) do (
  tasklist /FI "IMAGENAME eq %%p" 2>nul | findstr /i "%%p" >nul && set "LEFT=!LEFT! %%p"
)

if defined LEFT (
  echo [WARN]  still running:%LEFT%
  echo         Kill them manually, or check for a second MediaMTX on :8554.
  exit /b 1
)

echo [OK]    all joint-run processes stopped
exit /b 0
