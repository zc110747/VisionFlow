@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM One-click launcher for the Camera Agent demo.
REM
REM External tools (mediamtx / ffplay) are invoked by bare name and must be on
REM PATH. NOTHING about this machine is hardcoded here, so the repo stays
REM portable: put the tools on PATH once per machine and this script just works.
REM Only the project binary is referenced relative to this script.
REM
REM Runtime logs are written under tests\finished\.
REM
REM Flow: (1) pre-check tools + binary
REM       (2) stop running related programs
REM       (3) start MediaMTX -> camera-agent -> ffplay in order
REM       (4) verify each component and print startup info / access method
REM
REM ---- Configuration (no need to edit this file) -------------------------
REM Every setting can be overridden by an environment variable, or by passing
REM the matching command-line argument. Precedence: argument > env var > default.
REM
REM   CAMERA_ID       camera index, see: camera-agent --list   (default 1)
REM   CAMERA_AUTO     1 = let the camera negotiate its native resolution/fps,
REM                   which is the most portable choice since every UVC device
REM                   exposes a different set of modes               (default 1)
REM   CAMERA_WIDTH    capture width   (only used when CAMERA_AUTO=0; default 1280)
REM   CAMERA_HEIGHT   capture height  (only used when CAMERA_AUTO=0; default 720)
REM   CAMERA_FPS      capture fps     (only used when CAMERA_AUTO=0; default 30)
REM   CAMERA_SOURCE   force a GStreamer source element, e.g. mfvideosrc,
REM                   dshowvideosrc, ksvideosrc                  (default: auto)
REM   STREAM_ID       RTSP path name                        (default camera01)
REM   RTSP_HOST       RTSP server host                     (default 127.0.0.1)
REM   RTSP_PORT       RTSP server port                          (default 8554)
REM
REM Examples:
REM   start-camera-agent.bat
REM   start-camera-agent.bat --camera 3 --auto
REM   start-camera-agent.bat --camera 1 --no-auto --width 640 --height 480 --fps 30
REM   set CAMERA_ID=1 && start-camera-agent.bat
REM
REM NOTE: a .bat launched by double-click inherits the system/user PATH from the
REM registry, NOT the PATH of an already-open terminal. After editing PATH,
REM reopen the terminal (or log off/on) so the change is visible.

set ROOT=%~dp0
if "%ROOT:~-1%"=="\" set ROOT=%ROOT:~0,-1%
set FINISHED=%ROOT%\tests\finished
if not exist "%FINISHED%" mkdir "%FINISHED%"

REM Run from the project root so mediamtx finds mediamtx.yml and camera-agent
REM finds config/camera-agent.yaml regardless of where the .bat is launched from.
cd /d "%ROOT%"

REM ---- defaults (only applied when not already set in the environment) ----
if not defined CAMERA_ID     set CAMERA_ID=0
if not defined CAMERA_AUTO   set CAMERA_AUTO=1
if not defined CAMERA_WIDTH  set CAMERA_WIDTH=1280
if not defined CAMERA_HEIGHT set CAMERA_HEIGHT=720
if not defined CAMERA_FPS    set CAMERA_FPS=30
if not defined STREAM_ID     set STREAM_ID=camera01
if not defined RTSP_HOST     set RTSP_HOST=127.0.0.1
if not defined RTSP_PORT     set RTSP_PORT=8554

REM ---- parse args ----
set NOPAUSE=0
set LATENCY=
set "_ctx="
for %%a in (%*) do (
  if defined _ctx (
    if "!_ctx!"=="camera"  set "CAMERA_ID=%%~a"
    if "!_ctx!"=="width"   set "CAMERA_WIDTH=%%~a"
    if "!_ctx!"=="height"  set "CAMERA_HEIGHT=%%~a"
    if "!_ctx!"=="fps"     set "CAMERA_FPS=%%~a"
    if "!_ctx!"=="source"  set "CAMERA_SOURCE=%%~a"
    if "!_ctx!"=="stream"  set "STREAM_ID=%%~a"
    if "!_ctx!"=="host"    set "RTSP_HOST=%%~a"
    if "!_ctx!"=="port"    set "RTSP_PORT=%%~a"
    set "_ctx="
  ) else (
    if /i "%%a"=="--no-pause"      set NOPAUSE=1
    if /i "%%a"=="--latency-probe" set LATENCY=--latency-probe
    if /i "%%a"=="--auto"          set CAMERA_AUTO=1
    if /i "%%a"=="--no-auto"       set CAMERA_AUTO=0
    if /i "%%a"=="--camera"        set "_ctx=camera"
    if /i "%%a"=="--width"         set "_ctx=width"
    if /i "%%a"=="--height"        set "_ctx=height"
    if /i "%%a"=="--fps"           set "_ctx=fps"
    if /i "%%a"=="--source"        set "_ctx=source"
    if /i "%%a"=="--stream"        set "_ctx=stream"
    if /i "%%a"=="--server"        set "_ctx=host"
    if /i "%%a"=="--host"          set "_ctx=host"
    if /i "%%a"=="--port"          set "_ctx=port"
  )
)

REM ---- (1) pre-check: project binary ----
set EXE=%ROOT%\build-msvc\src\camera-agent.exe
if not exist "%EXE%" (
  echo [ERROR] camera-agent.exe not found at:
  echo         %EXE%
  echo         Build it first: build_oneclick.bat  - or: build_oneclick.bat sim
  goto :fail
)
echo [OK]    camera-agent.exe

REM ---- (1) pre-check: external tools on PATH (bare names, no hardcoded dirs) ----
call :need mediamtx || goto :fail
call :need ffplay     || goto :fail

REM ---- (2) stop running related programs ----
echo [1/3] Stopping running programs: mediamtx / camera-agent / ffplay
taskkill /F /IM mediamtx.exe >nul 2>&1
taskkill /F /IM camera-agent.exe >nul 2>&1
taskkill /F /IM ffplay.exe >nul 2>&1
C:\Windows\System32\timeout.exe /t 1 >nul 2>&1

REM ---- (2.5) validate / auto-pick the camera index -----------------------
REM A wrong index makes the capture element fail to start and the agent exits
REM immediately, which looks like "the script does nothing". Ask the binary
REM which cameras actually exist - same enumeration as "--list" - and fall back
REM to the first available index when the configured one is not present.
REM If enumeration itself fails - no camera, or GStreamer not on PATH - keep the
REM configured index so the later diagnostics still say something useful.
"%EXE%" --list > "%FINISHED%\cameras.txt" 2>nul
set "AVAIL_IDS="
if exist "%FINISHED%\cameras.txt" (
  for /f "tokens=2" %%i in ('findstr /b /c:"Camera " "%FINISHED%\cameras.txt"') do (
    set "AVAIL_IDS=!AVAIL_IDS! %%i"
  )
  del "%FINISHED%\cameras.txt" >nul 2>&1
)
if defined AVAIL_IDS (
  set "_old_id=%CAMERA_ID%"
  set "_cid_found=0"
  for %%i in (!AVAIL_IDS!) do if "%%i"=="%CAMERA_ID%" set "_cid_found=1"
  if "!_cid_found!"=="0" (
    for /f "tokens=1" %%i in ("!AVAIL_IDS!") do set "CAMERA_ID=%%i"
    echo [AUTO]   camera index !_old_id! is not available - using index !CAMERA_ID!
    echo          available camera indices:!AVAIL_IDS!
  )
) else (
  echo [note]   could not enumerate cameras - keeping index %CAMERA_ID%
)

REM ---- (3) assemble camera-agent arguments ----
set "AGENT_ARGS=--camera %CAMERA_ID% --stream %STREAM_ID% --server %RTSP_HOST% --port %RTSP_PORT%"
if "%CAMERA_AUTO%"=="1" (
  set "AGENT_ARGS=%AGENT_ARGS% --auto"
) else (
  set "AGENT_ARGS=%AGENT_ARGS% --width %CAMERA_WIDTH% --height %CAMERA_HEIGHT% --fps %CAMERA_FPS%"
)
if defined CAMERA_SOURCE set "AGENT_ARGS=%AGENT_ARGS% --source %CAMERA_SOURCE%"

REM ---- (3) start programs in order ----
echo [2/3] Starting MediaMTX (RTSP server) ...
start "MediaMTX" cmd /c "mediamtx > %FINISHED%\mediamtx.log 2>&1"

REM wait until mediamtx is ready (poll its log)
set MT_READY=0
for /l %%i in (1,1,15) do (
  if "!MT_READY!"=="0" (
    ping -n 2 127.0.0.1 >nul 2>&1
    findstr /i /c:"listener" /c:"%RTSP_PORT%" "%FINISHED%\mediamtx.log" >nul 2>&1 && set MT_READY=1
  )
)
if "%MT_READY%"=="1" ( echo [OK]    mediamtx ready ) else ( echo [WARN]  mediamtx not ready - see tests\finished\mediamtx.log )

echo [2/3] Starting camera-agent (push stream) ...
echo        args: %AGENT_ARGS%
start "camera-agent" cmd /c "%EXE% %AGENT_ARGS% %LATENCY% --log-level info > %FINISHED%\agent.log 2>&1"

REM ---- (4) wait for the stream to be live BEFORE opening the viewer --------
REM The viewer must not be started before the agent is actually publishing.
REM ffplay exits immediately when it cannot connect, and the agent needs a few
REM seconds to negotiate the camera format - launching ffplay right after the
REM agent used to make the viewer window silently never appear.
REM Poll for STREAMING rather than for "error" strings: GStreamer/GIO print
REM benign warnings such as "Failed to load module: giolibproxy.dll" that have
REM nothing to do with the stream, and matching those aborts the wait early.
set ST_OK=0
for /l %%i in (1,1,20) do (
  if "!ST_OK!"=="0" (
    findstr /i /c:"STREAMING" "%FINISHED%\agent.log" >nul 2>&1 && set ST_OK=1
    if "!ST_OK!"=="0" ping -n 2 127.0.0.1 >nul 2>&1
  )
)
if "%ST_OK%"=="1" (
  echo [OK]    camera-agent STREAMING
) else (
  echo [WARN]  camera-agent did not reach STREAMING within 20s.
  echo         See tests\finished\agent.log. Common causes:
  echo           - wrong camera index - run: camera-agent --list
  echo           - the camera is already in use by another app: Teams/Zoom/OBS
  echo           - MediaMTX is not running, or --port does not match
)

REM ---- (5) wait until MediaMTX has actually accepted the publisher --------
REM The agent reports STREAMING as soon as its pipeline starts pushing, but
REM MediaMTX needs a moment more to register the path. Launching ffplay in that
REM gap makes it fail DESCRIBE with "404 Not Found" and exit immediately.
REM Grace period first: mediamtx may also buffer its log, so do not rely on the
REM log alone for readiness. ping -n is used instead of timeout.exe because
REM timeout.exe needs a console and misbehaves when stdin is redirected.
ping -n 4 127.0.0.1 >nul 2>&1
set MT_PUB=0
if "%ST_OK%"=="1" (
  for /l %%i in (1,1,20) do (
    if "!MT_PUB!"=="0" (
      findstr /i /c:"is publishing" "%FINISHED%\mediamtx.log" >nul 2>&1 && set MT_PUB=1
      findstr /i /c:"stream is available" "%FINISHED%\mediamtx.log" >nul 2>&1 && set MT_PUB=1
      if "!MT_PUB!"=="0" ping -n 2 127.0.0.1 >nul 2>&1
    )
  )
)
if "%MT_PUB%"=="1" ( echo [OK]    mediamtx publishing ) else ( echo [note]  mediamtx log has not confirmed the publisher yet )
REM Fall back to the agent's own status: it is streaming, and by now MediaMTX
REM has certainly had time to register the path, so the viewer is safe to open.
if "%MT_PUB%"=="0" if "%ST_OK%"=="1" set MT_PUB=1

REM ---- (6) open the viewer ----
if "%MT_PUB%"=="1" (
  echo [3/3] Starting ffplay viewer ...
  REM NOTE: text inside these if-blocks must not contain parentheses - an
  REM       unescaped ) closes the block early and breaks the script.
  REM Low-latency viewer flags:
  REM   -rtsp_transport tcp     force interleaved TCP, matches mediamtx rtspTransport
  REM   -fflags nobuffer        skip input format buffering
  REM   -flags low_delay        decoder low-delay hint
  REM   -probesize/-analyzeduration   minimal stream probing/analysis wait
  REM   -framedrop              drop late frames to stay real-time
  REM   -max_delay 0            minimise demuxer buffering
  REM NOTE: never pass "-rtsp_flags nobuffer". rtsp_flags does not accept that
  REM       value, and ffplay aborts with "Invalid argument" before it ever
  REM       opens a window - the viewer then silently never appears.
  start "ffplay" ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay -probesize 32768 -analyzeduration 0 -framedrop -max_delay 0 rtsp://%RTSP_HOST%:%RTSP_PORT%/%STREAM_ID%
) else (
  echo [3/3] Skipping ffplay: the stream is not up yet.
  echo       Start it manually once the camera is streaming:
  echo         ffplay -rtsp_transport tcp -fflags nobuffer rtsp://%RTSP_HOST%:%RTSP_PORT%/%STREAM_ID%
)

REM ---- (4) print startup info and access method ----
echo.
echo ============================================================
echo  Camera Agent demo started
echo  Camera     : index %CAMERA_ID% - change with --camera N or set CAMERA_ID=N
echo  RTSP stream: rtsp://%RTSP_HOST%:%RTSP_PORT%/%STREAM_ID%
if "%MT_PUB%"=="1" (
  echo  Viewer     : ffplay window opened automatically
) else (
  echo  Viewer     : not started - the stream never came up
)
echo  Logs       : tests\finished\  (agent.log / mediamtx.log)
echo  Stop       : run this script again, or close the 3 windows
echo ============================================================
echo.
goto :done

:need
set "_t=%~1"
where %_t% >nul 2>&1
if errorlevel 1 (
  echo [ERROR] '%_t%' was not found on PATH.
  echo         This script calls external tools by bare name and hardcodes no
  echo         directories. Add the folder containing %_t%.exe to the system or
  echo         user PATH, then REOPEN the terminal - a .bat launched by
  echo         double-click inherits the registry PATH, not an open shell's PATH.
  exit /b 1
)
echo [OK]    %_t%
exit /b 0

:fail
echo.
echo Aborted - fix the error above, then run this script again.
echo.
if "%NOPAUSE%"=="0" pause
exit /b 1

:done
if "%NOPAUSE%"=="0" pause
exit /b 0
