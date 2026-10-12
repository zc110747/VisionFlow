@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ============================================================================
REM  run_onestep.bat - build if needed, then launch desktop-server
REM
REM    [1/4] preflight   locate the exe, or fall back to building it
REM    [2/4] build       only when the exe is missing or --build was given
REM    [3/4] start       launch VisionFlow.Desktop.exe detached
REM    [4/4] summary
REM
REM  Every failure path pauses and prints a reason plus a hint.
REM
REM  Usage:
REM    run_onestep.bat                       build if missing, then start
REM    run_onestep.bat --build               always rebuild first
REM    run_onestep.bat Release               launch the Release build
REM    run_onestep.bat --with-server         also start video-server (port 8081)
REM    run_onestep.bat --no-pause            unattended (CI)
REM
REM  Why --with-server exists:
REM    the desktop client talks to video-server on http://127.0.0.1:8081, which is
REM    exactly the http_port in video-server\config\config.joint.yaml. Without the
REM    server the app still runs - local video and the local camera work - but the
REM    "network camera" group stays empty. --with-server starts that side too.
REM    It calls video-server\start_oneclick.bat, reusing whatever logic that script
REM    already has for port / config / health checks instead of duplicating it.
REM
REM  Notes for whoever edits this next:
REM    - never use a bare "dotnet": resolve it to an absolute path first.
REM    - "start" is used to detach the GUI so this window can return; the child
REM      keeps its own console-less WinExe window.
REM    - cmd does NOT strip ^ inside double quotes; keep PowerShell out of this.
REM ============================================================================

REM ---- paths: this script sits at the desktop-server root ----
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "PROJ=%ROOT%\src\VisionFlow.Desktop\VisionFlow.Desktop.csproj"

REM The repo root is the parent of desktop-server. video-server lives there.
for %%i in ("%ROOT%\..") do set "PARENT=%%~fi"
set "VS_DIR=%PARENT%\video-server"

REM ---- defaults ----
set "CONFIG=Debug"
set "DO_BUILD=0"
set "WITH_SERVER=0"
set "NOPAUSE=0"
set "_ctx="

for %%a in (%*) do (
  if defined _ctx (
    if "!_ctx!"=="config" set "CONFIG=%%~a"
    set "_ctx="
  ) else (
    if /i "%%a"=="--build"       set DO_BUILD=1
    if /i "%%a"=="--with-server" set WITH_SERVER=1
    if /i "%%a"=="--no-pause"    set NOPAUSE=1
    if /i "%%a"=="--config"      set "_ctx=config"
    if /i "%%a"=="debug"         set CONFIG=Debug
    if /i "%%a"=="release"       set CONFIG=Release
  )
)

set "FAIL_STEP=startup"
set "FAIL_REASON="
set "FAIL_HINT1="
set "FAIL_HINT2="
set "FAIL_HINT3="

echo ============================================================
echo  One-step launch: desktop-server
echo  root   : %ROOT%
echo  config : %CONFIG%
echo ============================================================

REM ============================================================================
REM  [1/4] preflight
REM ============================================================================
echo.
echo [1/4] preflight

set "BIN_DIR=%ROOT%\src\VisionFlow.Desktop\bin\%CONFIG%\net9.0-windows\win-x64"
set "EXE_PATH=%BIN_DIR%\VisionFlow.Desktop.exe"

REM A stale binary is the most confusing failure mode, so "missing" is what
REM triggers a build - not "old". --build forces it regardless.
if not exist "%EXE_PATH%" (
  echo   exe    : not found - will build
  set "DO_BUILD=1"
) else (
  echo   exe    : %EXE_PATH%
)

REM ---- optional: bring up video-server so the network camera group populates ----
if "%WITH_SERVER%"=="1" (
  if not exist "%VS_DIR%\start_oneclick.bat" (
    set "FAIL_STEP=1/4 preflight"
    set "FAIL_REASON=--with-server was given but %VS_DIR%\start_oneclick.bat does not exist"
    set "FAIL_HINT1=Expected the video-server checkout next to desktop-server, under %PARENT%"
    set "FAIL_HINT2=Run without --with-server to use only local video and the local camera"
    goto :fail
  )
  echo   server : %VS_DIR%\start_oneclick.bat
) else (
  echo   server : not started - pass --with-server to start video-server on :8081
)

echo [OK]    preflight done

REM ============================================================================
REM  [2/4] build (only when needed)
REM ============================================================================
echo.
echo [2/4] build
if "%DO_BUILD%"=="0" (
  echo   skipped   exe exists - use --build to force a rebuild
  goto :after_build
)

REM resolve dotnet before shelling out to the build script, so a missing SDK is
REM reported here with a hint rather than as a nested script failure.
set "DOTNET_BIN="
for /f "delims=" %%i in ('where dotnet 2^>nul') do (
  if not defined DOTNET_BIN set "DOTNET_BIN=%%i"
)
if not defined DOTNET_BIN (
  if exist "%ProgramFiles%\dotnet\dotnet.exe" set "DOTNET_BIN=%ProgramFiles%\dotnet\dotnet.exe"
)
if not defined DOTNET_BIN (
  set "FAIL_STEP=2/4 build"
  set "FAIL_REASON=dotnet SDK not found - 'dotnet' is not on PATH and %ProgramFiles%\dotnet\dotnet.exe does not exist"
  set "FAIL_HINT1=Install the .NET 9 SDK [x64] from https://dotnet.microsoft.com/download"
  set "FAIL_HINT2=Or build once by hand, then run this script again"
  goto :fail
)

REM Delegate to build_oneclick.bat so the ffmpeg version check and the
REM write-lock detection stay in exactly one place.
if not exist "%ROOT%\build_oneclick.bat" (
  set "FAIL_STEP=2/4 build"
  set "FAIL_REASON=build_oneclick.bat not found next to this script"
  set "FAIL_HINT1=Build by hand: dotnet build "%PROJ%" -c %CONFIG%"
  goto :fail
)

echo   calling build_oneclick.bat %CONFIG% --no-pause ...
pushd "%ROOT%"
call "%ROOT%\build_oneclick.bat" %CONFIG% --no-pause
set "BUILD_RC=!errorlevel!"
popd

if not "!BUILD_RC!"=="0" (
  set "FAIL_STEP=2/4 build"
  set "FAIL_REASON=build_oneclick.bat returned exit code !BUILD_RC! - the build did not produce a usable exe"
  set "FAIL_HINT1=The reason and hint above come from the build script itself"
  set "FAIL_HINT2=Reproduce by hand: build_oneclick.bat %CONFIG%"
  goto :fail
)

if not exist "%EXE_PATH%" (
  set "FAIL_STEP=2/4 build"
  set "FAIL_REASON=build reported success but the executable is still missing: %EXE_PATH%"
  set "FAIL_HINT1=Check TargetFramework / RuntimeIdentifier in VisionFlow.Desktop.csproj"
  goto :fail
)
echo   [OK]    built

:after_build

REM ============================================================================
REM  [3/4] start video-server first (optional), then the app
REM ============================================================================
echo.
echo [3/4] starting

if "%WITH_SERVER%"=="1" (
  echo   starting video-server - it will take a moment to pass its health check ...
  REM --no-browser: the user asked for the desktop client, not a Web UI tab.
  pushd "%VS_DIR%"
  call "%VS_DIR%\start_oneclick.bat" --no-browser
  set "SRV_RC=!errorlevel!"
  popd
  if not "!SRV_RC!"=="0" (
    set "FAIL_STEP=3/4 start"
    set "FAIL_REASON=video-server failed to start, exit code !SRV_RC!"
    set "FAIL_HINT1=The reason and hint above come from video-server\start_oneclick.bat"
    set "FAIL_HINT2=The desktop app can still run without it - local video and camera only"
    set "FAIL_HINT3=Rerun this script without --with-server to skip the server"
    goto :fail
  )
  echo   [OK]    video-server up
) else (
  echo   skipped   video-server - --with-server not given
)

REM 8081 is what NetworkCameraClient defaults to. If the user overrides the
REM server port they must set VIDEO_SERVER_URL too - say so instead of letting
REM them wonder why the camera list is empty.
if "%WITH_SERVER%"=="0" (
  echo   [INFO] the app will look for video-server on http://127.0.0.1:8081
  echo          if your server uses another port, set VIDEO_SERVER_URL before launch
)

REM Detached: the WinForms window is the deliverable, this console must return.
start "VisionFlow Desktop" /D "%BIN_DIR%" "%EXE_PATH%"
echo   started  VisionFlow.Desktop.exe

REM Give it a moment, then confirm it is actually alive - "start" succeeds even
REM if the process dies immediately (e.g. missing ffmpeg DLL), which would make
REM the summary below a lie.
C:\Windows\System32\timeout.exe /t 3 >nul 2>&1
set "ALIVE=0"
tasklist /FI "IMAGENAME eq VisionFlow.Desktop.exe" 2>nul | findstr /i "VisionFlow.Desktop.exe" >nul
if not errorlevel 1 set "ALIVE=1"

if "!ALIVE!"=="0" (
  set "FAIL_STEP=3/4 start"
  set "FAIL_REASON=VisionFlow.Desktop.exe exited within 3s of launch"
  set "FAIL_HINT1=Almost always a missing or mismatched ffmpeg DLL next to the exe"
  set "FAIL_HINT2=Check %BIN_DIR% for avcodec-62.dll / avutil-60.dll"
  set "FAIL_HINT3=Rebuild with build_oneclick.bat to restore the ffmpeg runtime copy"
  goto :fail
)
echo   [OK]    running

REM ============================================================================
REM  [4/4] summary
REM ============================================================================
echo.
echo [4/4] summary
echo ------------------------------------------------------------
echo  app      : VisionFlow.Desktop.exe  (%CONFIG%, x64)
echo  binary   : %EXE_PATH%
if "%WITH_SERVER%"=="1" (
  echo  server   : http://127.0.0.1:8081   [started]
) else (
  echo  server   : http://127.0.0.1:8081   [not started]
)
echo ------------------------------------------------------------
echo  RUNNING
echo.
echo  In the app:
echo    video source  -> pick "file" / "local device" / "network camera"
echo    AI mode       -> enabled for file and network sources, off for a local camera
echo.
echo  Stop the app     : taskkill /F /IM VisionFlow.Desktop.exe
echo  Stop the server  : %VS_DIR%\scripts\stop-joint.bat
echo  Headless tests   : run_tests.bat
echo ============================================================
echo.

if "%NOPAUSE%"=="1" exit /b 0
pause
exit /b 0

REM ============================================================================
REM  :fail - single exit for every failure path
REM  Reached with goto (never by falling through), so it always runs at top
REM  level where !VAR! and unbalanced punctuation in the message are safe.
REM ============================================================================
:fail
echo.
echo ============================================================
echo  START FAILED   step !FAIL_STEP!
echo ------------------------------------------------------------
echo  reason : !FAIL_REASON!
if defined FAIL_HINT1 echo  hint   : !FAIL_HINT1!
if defined FAIL_HINT2 echo           !FAIL_HINT2!
if defined FAIL_HINT3 echo           !FAIL_HINT3!
echo ------------------------------------------------------------
echo  root   : %ROOT%
echo ============================================================
echo.
if "%NOPAUSE%"=="1" exit /b 1
pause
exit /b 1
