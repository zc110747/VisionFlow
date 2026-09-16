@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ============================================================================
REM  build_onelick.bat - build the WHOLE project in one shot
REM
REM    [1/5] preflight   go / node / Visual Studio
REM    [2/5] web UI      Vue + Vite   -> web\dist
REM    [3/5] video-server  Go          -> video-server.exe
REM    [4/5] carmera-agent MSVC + Ninja -> carmera-agent\build-msvc\src\camera-agent.exe
REM    [5/5] summary
REM
REM  Every failure path pauses and prints a reason plus a hint.
REM
REM  Usage:
REM    build_onelick.bat                  build everything
REM    build_onelick.bat --skip-web       skip the Web UI build
REM    build_onelick.bat --skip-agent     skip carmera-agent
REM    build_onelick.bat --clean          wipe carmera-agent\build-msvc first
REM    build_onelick.bat --backend sim    agent backend: gstreamer | sim | auto
REM    build_onelick.bat --no-pause       unattended (CI); failures also skip pause
REM
REM  Notes for whoever edits this next:
REM    - cmd does NOT strip ^ inside double quotes, so PowerShell one-liners
REM      must never contain ^| or ^) - prefer pure cmd constructs.
REM    - every "set" that is read back inside the same ( ) block needs !VAR!.
REM    - a ")" inside a ( ) block closes it early: the size/mtime reporting
REM      below is done at top level for that reason.
REM ============================================================================

REM ---- paths: this script sits at the video-server root ----
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
for %%i in ("%ROOT%\..") do set "PARENT=%%~fi"
set "AGENT_DIR=%PARENT%\carmera-agent"
set "LOGS=%ROOT%\logs"
if not exist "%LOGS%" mkdir "%LOGS%"

REM ---- defaults ----
set SKIP_WEB=0
set SKIP_AGENT=0
set CLEAN=0
set NOPAUSE=0
set BACKEND=gstreamer

for %%a in (%*) do (
  if /i "%%a"=="--skip-web"   set SKIP_WEB=1
  if /i "%%a"=="--skip-agent" set SKIP_AGENT=1
  if /i "%%a"=="--clean"      set CLEAN=1
  if /i "%%a"=="--no-pause"   set NOPAUSE=1
  if /i "%%a"=="sim"          set BACKEND=sim
  if /i "%%a"=="gstreamer"    set BACKEND=gstreamer
  if /i "%%a"=="auto"         set BACKEND=auto
)

set "FAIL_STEP=startup"
set "FAIL_REASON="
set "FAIL_HINT1="
set "FAIL_HINT2="
set "FAIL_HINT3="

echo ============================================================
echo  One-click build: video-server + web UI + carmera-agent
echo  root  : %ROOT%
echo  agent : %AGENT_DIR%
echo ============================================================

REM ============================================================================
REM  [1/5] preflight
REM ============================================================================
echo.
echo [1/5] preflight

REM ---- go: video-server cannot be built without it ----
set "GO_BIN=go"
set "GO_VER="
where go >nul 2>nul
if errorlevel 1 (
  if exist "%ROOT%\.toolchain\go\go\bin\go.exe" (
    set "GO_BIN=%ROOT%\.toolchain\go\go\bin\go.exe"
  ) else (
    set "FAIL_STEP=1/5 preflight"
    set "FAIL_REASON=Go toolchain not found - 'go' is not on PATH and .toolchain\go\go\bin\go.exe does not exist"
    set "FAIL_HINT1=Install Go from https://go.dev/dl, or run scripts\setup-go.bat to fetch it locally"
    goto :fail
  )
)
for /f "tokens=3" %%v in ('"%GO_BIN%" version 2^>nul') do set "GO_VER=%%v"
if not defined GO_VER set GO_VER=unknown
echo   go    : %GO_BIN%  version %GO_VER%

REM ---- node/npm: only needed for the Web UI ----
set "NODE_OK=0"
set "NPM_OK=0"
where node >nul 2>nul
if not errorlevel 1 set NODE_OK=1
where npm  >nul 2>nul
if not errorlevel 1 set NPM_OK=1
if "%NODE_OK%"=="1" (
  echo   node  : available
) else (
  echo   node  : MISSING
)

REM ---- Visual Studio: only needed for carmera-agent ----
set "VSPATH="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%p in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    if not defined VSPATH set "VSPATH=%%p"
  )
)
if not defined VSPATH (
  if exist "D:\Software\vs" set "VSPATH=D:\Software\vs"
)
if defined VSPATH (
  echo   vs    : %VSPATH%
) else (
  echo   vs    : MISSING
)

if "%SKIP_AGENT%"=="0" (
  if not defined VSPATH (
    set "FAIL_STEP=1/5 preflight"
    set "FAIL_REASON=Visual Studio not found - carmera-agent needs the MSVC x64 toolchain"
    set "FAIL_HINT1=Install VS2022 with the Desktop development with C++ workload"
    set "FAIL_HINT2=or pass --skip-agent to build only video-server and the web UI"
    goto :fail
  )
)

echo [OK]    preflight done

REM ============================================================================
REM  [2/5] web UI
REM ============================================================================
echo.
echo [2/5] web UI
if "%SKIP_WEB%"=="1" (
  echo   skipped   --skip-web
  goto :after_web
)
if not exist "%ROOT%\web\package.json" (
  echo   skipped   no web\package.json in this tree
  goto :after_web
)
if "%NODE_OK%"=="0" (
  if exist "%ROOT%\web\dist\index.html" (
    echo   [WARN] node not found - keeping the existing web\dist
    goto :after_web
  )
  set "FAIL_STEP=2/5 web UI"
  set "FAIL_REASON=node/npm not on PATH and web\dist\index.html does not exist - the Web UI cannot be built"
  set "FAIL_HINT1=Install Node.js, or pass --skip-web if you only need the API"
  goto :fail
)
if "%NPM_OK%"=="0" (
  set "FAIL_STEP=2/5 web UI"
  set "FAIL_REASON=node was found but npm was not - npm is required to build the Web UI"
  set "FAIL_HINT1=Reinstall Node.js with npm, or pass --skip-web"
  goto :fail
)

REM --clean also rebuilds the Web UI from scratch. web\dist is a build product,
REM so removing it is safe - it is regenerated by the build right below.
if "%CLEAN%"=="1" (
  if exist "%ROOT%\web\dist" (
    echo   clean: removing web\dist
    rmdir /s /q "%ROOT%\web\dist"
  )
)

REM "node_modules exists" only proves it was installed ONCE, not that it is
REM complete: a dependency added to package.json afterwards is simply missing,
REM and Vite then dies with "Rollup failed to resolve import". So install is
REM skipped on the happy path because it is slow, but a failed build triggers
REM one install-and-retry before this is reported as a real failure.
set "WEB_INSTALLED=0"
set "NPM_RC=0"

if exist "%ROOT%\web\node_modules" (
  echo   npm install  node_modules present, skipping
) else (
  echo   npm install  first run, this takes a while ...
  call :npm_install
)
if not "!NPM_RC!"=="0" goto :web_install_failed

call :npm_build
if not "!NPM_RC!"=="0" (
  if "!WEB_INSTALLED!"=="0" (
    echo   [WARN] build failed - a missing dependency is the most common cause
    echo          running npm install, then retrying once ...
    call :npm_install
    if not "!NPM_RC!"=="0" goto :web_install_failed
    call :npm_build
  )
)
if not "!NPM_RC!"=="0" (
  set "FAIL_STEP=2/5 web UI"
  set "FAIL_REASON=npm run build failed with exit code !NPM_RC! - see the Vite / TypeScript errors above"
  set "FAIL_HINT1=Dependencies were reinstalled and it still failed, so this is a source-level error"
  set "FAIL_HINT2=Reproduce by hand: cd /d %ROOT%\web ^&^& npm run build"
  goto :fail
)
if not exist "%ROOT%\web\dist\index.html" (
  set "FAIL_STEP=2/5 web UI"
  set "FAIL_REASON=npm run build reported success but web\dist\index.html is missing"
  set "FAIL_HINT1=Check the build output directory configured in web\vite.config.*"
  goto :fail
)
echo   [OK]    web\dist

:after_web

REM ============================================================================
REM  [3/5] video-server
REM ============================================================================
echo.
echo [3/5] video-server  go build

REM Windows holds an exclusive handle on a running .exe, so writing it fails
REM with "Access is denied". Detect up front so the error message can say why
REM instead of dumping a linker error that looks like a code problem.
set "VS_LOCKED=0"
tasklist /FI "IMAGENAME eq video-server.exe" 2>nul | findstr /i "video-server.exe" >nul
if not errorlevel 1 set "VS_LOCKED=1"
tasklist /FI "IMAGENAME eq video-server.new.exe" 2>nul | findstr /i "video-server.new.exe" >nul
if not errorlevel 1 set "VS_LOCKED=1"
if "!VS_LOCKED!"=="1" (
  echo   [WARN] a video-server instance is running - the .exe it loaded is write-locked
)

set "VS_OUT=%ROOT%\video-server.exe"
set "VS_RETRIED=0"
pushd "%ROOT%"
"%GO_BIN%" build -trimpath -o "%VS_OUT%" ./cmd/video-server
set "GO_RC=!errorlevel!"
popd

if not "!GO_RC!"=="0" (
  if "!VS_LOCKED!"=="1" (
    echo   retrying as video-server.new.exe ...
    set "VS_RETRIED=1"
    pushd "%ROOT%"
    "%GO_BIN%" build -trimpath -o "%ROOT%\video-server.new.exe" ./cmd/video-server
    set "GO_RC=!errorlevel!"
    popd
    REM Point the summary at what was actually produced, not at the file the
    REM first attempt failed to overwrite.
    if "!GO_RC!"=="0" set "VS_OUT=%ROOT%\video-server.new.exe"
  )
)

REM Default reason is a plain compile error; the two special cases overwrite it.
if not "!GO_RC!"=="0" (
  set "FAIL_STEP=3/5 video-server"
  set "FAIL_REASON=go build returned exit code !GO_RC! - see the compiler errors above"
  set "FAIL_HINT1=Reproduce by hand: cd /d %ROOT% ^&^& go build ./..."
  set "FAIL_HINT2=Missing modules? run: go mod download"
  if "!VS_RETRIED!"=="1" (
    set "FAIL_REASON=go build failed with exit code !GO_RC! - the video-server.new.exe fallback failed too, so this is a real compile error"
    set "FAIL_HINT1=Reproduce by hand: cd /d %ROOT% ^&^& go build ./..."
    set "FAIL_HINT2=Both names blocked? check for running instances: tasklist | findstr video-server"
  ) else (
    if "!VS_LOCKED!"=="1" (
      set "FAIL_REASON=go build failed with exit code !GO_RC! - the target .exe is write-locked by a running instance"
      set "FAIL_HINT1=Stop it first: scripts\stop-joint.bat"
      set "FAIL_HINT2=or: taskkill /F /IM video-server.exe"
    )
  )
  goto :fail
)
echo   [OK]    %VS_OUT%

REM ============================================================================
REM  [4/5] carmera-agent
REM ============================================================================
echo.
echo [4/5] carmera-agent  cmake + ninja
if "%SKIP_AGENT%"=="1" (
  echo   skipped   --skip-agent
  goto :after_agent
)
if not exist "%AGENT_DIR%\build_oneclick.bat" (
  set "FAIL_STEP=4/5 carmera-agent"
  set "FAIL_REASON=agent build script not found: %AGENT_DIR%\build_oneclick.bat"
  set "FAIL_HINT1=Expected the carmera-agent checkout next to video-server, under %PARENT%"
  set "FAIL_HINT2=Clone it there, or pass --skip-agent"
  goto :fail
)

set "AGENT_ARGS=--no-pause"
if "%CLEAN%"=="1" set "AGENT_ARGS=!AGENT_ARGS! clean"
set "AGENT_ARGS=!AGENT_ARGS! %BACKEND%"
echo   backend : %BACKEND%
echo   calling : build_oneclick.bat %AGENT_ARGS%

pushd "%AGENT_DIR%"
call "%AGENT_DIR%\build_oneclick.bat" %AGENT_ARGS%
set "CA_RC=!errorlevel!"
popd

if not "!CA_RC!"=="0" (
  set "FAIL_STEP=4/5 carmera-agent"
  set "FAIL_REASON=camera-agent build failed with exit code !CA_RC! - see the CMake / compiler output above"
  set "FAIL_HINT1=Most common cause: GStreamer not found. Pass --backend sim to build without it"
  set "FAIL_HINT2=Full configure once: cd /d %AGENT_DIR% ^&^& build_oneclick.bat clean"
  goto :fail
)
if not exist "%AGENT_DIR%\build-msvc\src\camera-agent.exe" (
  set "FAIL_STEP=4/5 carmera-agent"
  set "FAIL_REASON=agent build reported success but build-msvc\src\camera-agent.exe is missing"
  set "FAIL_HINT1=Check the install target in carmera-agent\CMakeLists.txt"
  goto :fail
)
echo   [OK]    %AGENT_DIR%\build-msvc\src\camera-agent.exe

:after_agent

REM ============================================================================
REM  [5/5] summary
REM ============================================================================
echo.
echo [5/5] summary
echo ------------------------------------------------------------
call :show_file "%VS_OUT%" "video-server"
if "%SKIP_AGENT%"=="0" call :show_file "%AGENT_DIR%\build-msvc\src\camera-agent.exe" "camera-agent"
if exist "%ROOT%\web\dist\index.html" (
  echo   web dist     : %ROOT%\web\dist
) else (
  echo   web dist     : NOT BUILT
)
echo ------------------------------------------------------------
echo  BUILD OK
echo.
echo  Next:
echo    start_oneclick.bat          start video-server only, opens the Web UI
echo    scripts\start-joint.bat     full chain: server + camera push + WebRTC
echo    scripts\verify_joint.py     end-to-end acceptance, PASS/FAIL counters
echo ============================================================
echo.
if "%NOPAUSE%"=="1" exit /b 0
pause
exit /b 0

REM ============================================================================
REM  :web_install_failed - npm install failed
REM ============================================================================
:web_install_failed
set "FAIL_STEP=2/5 web UI"
set "FAIL_REASON=npm install failed with exit code !NPM_RC!"
set "FAIL_HINT1=Check the npm output above - offline, proxy or registry errors are the usual cause"
set "FAIL_HINT2=Corporate network? set a registry: npm config set registry https://registry.npmmirror.com"
set "FAIL_HINT3=Or build without the Web UI: build_onelick.bat --skip-web"
goto :fail

REM ============================================================================
REM  :npm_install / :npm_build
REM  Both share the NPM_RC convention, and both restore the working directory
REM  before returning - no goto leaves from inside, so pushd/popd always pair.
REM ============================================================================
:npm_install
pushd "%ROOT%\web"
call npm install --no-audit --no-fund
set "NPM_RC=!errorlevel!"
popd
if "!NPM_RC!"=="0" (
  set "WEB_INSTALLED=1"
  echo   [OK]    npm install
) else (
  echo   [FAIL]  npm install exited !NPM_RC!
)
exit /b 0

:npm_build
echo   npm run build ...
pushd "%ROOT%\web"
call npm run build
set "NPM_RC=!errorlevel!"
popd
if "!NPM_RC!"=="0" (
  echo   [OK]    npm run build
) else (
  echo   [FAIL]  npm run build exited !NPM_RC!
)
exit /b 0

REM ============================================================================
REM  :show_file <path> <label> - print size + mtime, or a missing marker
REM  Top level on purpose: %%~zf needs no delayed expansion, and keeping it out
REM  of any ( ) block avoids the ")" closing-the-block trap entirely.
REM ============================================================================
:show_file
set "SF_PATH=%~1"
set "SF_LABEL=%~2"
if exist "%SF_PATH%" (
  for %%f in ("%SF_PATH%") do (
    set "SF_BYTES=%%~zf"
    set "SF_TIME=%%~tf"
  )
  set /a "SF_MB=!SF_BYTES! / 1048576"
  echo   %SF_LABEL% : !SF_BYTES! bytes  ~!SF_MB! MB   !SF_TIME!
) else (
  echo   %SF_LABEL% : MISSING  %SF_PATH%
)
exit /b 0

REM ============================================================================
REM  :fail - single exit for every failure path
REM  Reached with goto (never by falling through), so it always runs at top
REM  level where !VAR! and unbalanced punctuation in the message are safe.
REM ============================================================================
:fail
echo.
echo ============================================================
echo  BUILD FAILED   step !FAIL_STEP!
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
