@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ============================================================================
REM  start_oneclick.bat - start video-server with one click
REM
REM  video-server spawns and owns MediaMTX itself, so this script never starts
REM  a second one: a stray MediaMTX would grab :8554 and the monitor would poll
REM  an API that does not exist on it.
REM
REM  Steps:
REM    [1/4] preflight  pick the freshest binary, resolve config, read the port
REM    [2/4] stop leftovers so the port and the .exe handle are free
REM    [3/4] start video-server detached, stdout+stderr to logs\video-server.log
REM    [4/4] wait for /api/health, then print local + LAN URLs
REM
REM  Every failure path pauses and prints a reason plus a hint.
REM
REM  Usage:
REM    start_oneclick.bat                    start with config\config.joint.yaml
REM    start_oneclick.bat --no-browser       do not open a browser
REM    start_oneclick.bat --config config.yaml        file name or path, both work
REM    start_oneclick.bat --no-kill          keep an already running instance
REM    start_oneclick.bat --no-pause         unattended (CI)
REM
REM  Config override without flags:  set VS_CONFIG=config.yaml
REM
REM  For the full chain - server + UVC camera push + WebRTC playback - use
REM  scripts\start-joint.bat instead. This one is server-only on purpose.
REM ============================================================================

REM ---- paths ----
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "LOGS=%ROOT%\logs"
if not exist "%LOGS%" mkdir "%LOGS%"

REM ---- defaults ----
set NO_BROWSER=0
set NO_KILL=0
set NOPAUSE=0
set "CFG_NAME="
set "_ctx="

for %%a in (%*) do (
  if defined _ctx (
    if "!_ctx!"=="config" set "CFG_NAME=%%~a"
    set "_ctx="
  ) else (
    if /i "%%a"=="--no-browser" set NO_BROWSER=1
    if /i "%%a"=="--no-kill"    set NO_KILL=1
    if /i "%%a"=="--no-pause"   set NOPAUSE=1
    if /i "%%a"=="--config"     set "_ctx=config"
  )
)

set "FAIL_STEP=startup"
set "FAIL_REASON="
set "FAIL_HINT1="
set "FAIL_HINT2="
set "FAIL_HINT3="

echo ============================================================
echo  video-server one-click start
echo  root : %ROOT%
echo ============================================================

REM ============================================================================
REM  [1/4] preflight
REM ============================================================================
echo.
echo [1/4] preflight

REM Newest wins: build_onelick.bat falls back to video-server.new.exe when the
REM canonical name is write-locked by a running instance, and running a stale
REM binary is the single most confusing way this can fail.
call :pick_exe
if not defined VS_EXE (
  set "FAIL_STEP=1/4 preflight"
  set "FAIL_REASON=no video-server*.exe found under %ROOT%"
  set "FAIL_HINT1=Build it first: build_onelick.bat   or   scripts\build.bat"
  goto :fail
)
if not exist "%VS_EXE%" (
  set "FAIL_STEP=1/4 preflight"
  set "FAIL_REASON=video-server binary not found: %VS_EXE%"
  set "FAIL_HINT1=Build it first: build_onelick.bat   or   scripts\build.bat"
  goto :fail
)
echo   binary : %VS_NAME%

REM The joint config is the default because port 8080 on this machine is taken
REM by the "ApplicationWebServer" service - with config.yaml the server cannot
REM bind and startup looks broken for no visible reason.
if not defined CFG_NAME (
  set "CFG_NAME=config.joint.yaml"
  if defined VS_CONFIG set "CFG_NAME=%VS_CONFIG%"
)
REM Accept "config.yaml", "config\config.yaml" and an absolute path alike -
REM users type the first, older scripts in this repo pass the second, and the
REM server is always given the path relative to ROOT because that is its CWD.
set "CFG="
set "CFG_ARG="
if exist "%ROOT%\config\%CFG_NAME%" (
  set "CFG=%ROOT%\config\%CFG_NAME%"
  set "CFG_ARG=config\%CFG_NAME%"
)
if not defined CFG (
  if exist "%ROOT%\%CFG_NAME%" (
    set "CFG=%ROOT%\%CFG_NAME%"
    set "CFG_ARG=%CFG_NAME%"
  )
)
if not defined CFG (
  if exist "%CFG_NAME%" (
    set "CFG=%CFG_NAME%"
    set "CFG_ARG=%CFG_NAME%"
  )
)
if not defined CFG (
  set "FAIL_STEP=1/4 preflight"
  set "FAIL_REASON=config file not found: %CFG_NAME%  - looked under %ROOT%\config and %ROOT%"
  set "FAIL_HINT1=Available configs are in %ROOT%\config"
  set "FAIL_HINT2=Pass the file name only: start_oneclick.bat --config config.yaml"
  goto :fail
)
echo   config : %CFG_ARG%

REM Read the HTTP port back out of the YAML instead of hardcoding it here -
REM the port lives in exactly one place.
set HTTP_PORT=8080
findstr /r /c:"http_port:" "%CFG%" >nul
if errorlevel 1 (
  echo   [WARN] no http_port key in %CFG_ARG% - assuming %HTTP_PORT%
)
for /f "usebackq tokens=1,2 delims=:" %%a in (`findstr /r /c:"http_port:" "%CFG%"`) do (
  for /f "tokens=1" %%v in ("%%b") do set "HTTP_PORT=%%v"
)

REM A non-numeric port means the YAML did not parse the way we expected. Catch
REM it here rather than polling http://localhost:<garbage> for 40 seconds.
REM
REM set /a round-trips: a real number comes back identical, anything else
REM ("[unclosed", "1 2", "") does not. findstr with an anchored ^[0-9]*$ looks
REM like the obvious tool here but is a trap - $ does not swallow the CR that
REM echo emits, so a perfectly valid "8081" is rejected.
set "PORT_NUM="
set /a "PORT_NUM=%HTTP_PORT%" 2>nul
REM Compared with !VAR! on purpose: delayed expansion happens after parsing, so
REM a value containing ^ or & cannot corrupt the command line.
if not "!PORT_NUM!"=="!HTTP_PORT!" (
  set "FAIL_STEP=1/4 preflight"
  set "FAIL_REASON=http_port in %CFG_ARG% is not a number: %HTTP_PORT%"
  set "FAIL_HINT1=The YAML is malformed or the key is indented wrong - the server would reject it too"
  set "FAIL_HINT2=Validated file: %CFG%"
  goto :fail
)
if !PORT_NUM! LSS 1 (
  set "FAIL_STEP=1/4 preflight"
  set "FAIL_REASON=http_port in %CFG_ARG% is out of range: %HTTP_PORT%"
  set "FAIL_HINT1=Use a port between 1 and 65535"
  goto :fail
)
if !PORT_NUM! GTR 65535 (
  set "FAIL_STEP=1/4 preflight"
  set "FAIL_REASON=http_port in %CFG_ARG% is out of range: %HTTP_PORT%"
  set "FAIL_HINT1=Use a port between 1 and 65535"
  goto :fail
)
echo   port   : %HTTP_PORT%

if not exist "%ROOT%\web\dist\index.html" (
  echo   [WARN] web\dist\index.html missing - the REST API works, the Web UI does not
  echo          build it: build_onelick.bat
)

echo [OK]    preflight done

REM ============================================================================
REM  [2/4] stop leftovers
REM ============================================================================
echo.
echo [2/4] stopping leftovers
if "%NO_KILL%"=="1" (
  echo   skipped   --no-kill
  goto :after_kill
)
taskkill /F /IM video-server.exe >nul 2>&1
taskkill /F /IM mediamtx.exe    >nul 2>&1
REM Give Windows a moment to release the port, otherwise the new instance can
REM race the dying one and the health check below times out for no reason.
C:\Windows\System32\timeout.exe /t 2 >nul 2>&1
echo [OK]    stopped
:after_kill

REM ============================================================================
REM  [3/4] start
REM ============================================================================
echo.
echo [3/4] starting video-server
set "VS_LOG=%LOGS%\video-server.log"
REM Detached via start so this window returns. Redirections live INSIDE the
REM cmd /c string - PowerShell-style redirection outside would not apply to the
REM child process.
start "video-server" /D "%ROOT%" cmd /c ""%VS_EXE%" "%CFG_ARG%" > "%VS_LOG%" 2>&1"
echo   started  logs -^> %VS_LOG%

REM ============================================================================
REM  [4/4] wait for health
REM ============================================================================
echo.
echo [4/4] waiting for /api/health on :%HTTP_PORT%
set READY=0
set "ALIVE=1"
for /l %%i in (1,1,40) do (
  if "!READY!"=="0" (
    C:\Windows\System32\timeout.exe /t 1 >nul 2>&1
    curl -s -o nul -w "%%{http_code}" http://localhost:%HTTP_PORT%/api/health 2>nul | findstr "200" >nul
    if not errorlevel 1 set READY=1
  )
)

if "!READY!"=="1" goto :healthy

REM Not healthy - find out whether the process is even alive before blaming the
REM wait loop. A dead process almost always means "could not bind" or "bad
REM config", both of which are written to the log.
set "ALIVE=0"
tasklist /FI "IMAGENAME eq video-server.exe" 2>nul | findstr /i "video-server.exe" >nul
if not errorlevel 1 set "ALIVE=1"

set "FAIL_STEP=4/4 health check"
if "!ALIVE!"=="0" (
  set "FAIL_REASON=video-server exited during startup - /api/health never answered on port %HTTP_PORT%"
  set "FAIL_HINT1=Most common cause: the port is already in use. Check with: netstat -ano | findstr :%HTTP_PORT%"
  set "FAIL_HINT2=Or a bad config key, or MediaMTX failed to launch. Last log lines are printed below."
) else (
  set "FAIL_REASON=video-server is running but /api/health did not answer 200 within 40s on port %HTTP_PORT%"
  set "FAIL_HINT1=The port in config\%CFG_NAME% may differ from the one this script polled."
  set "FAIL_HINT2=Last log lines are printed below."
)
set "FAIL_HINT3=full log: %VS_LOG%"
goto :fail

:healthy
echo [OK]    healthy

REM ---- summary ----
echo.
set "HEALTH="
for /f "usebackq tokens=*" %%i in (`curl -s http://localhost:%HTTP_PORT%/api/health 2^>nul`) do set "HEALTH=%%i"
if defined HEALTH echo   health    : !HEALTH!

REM Ask the server for the LAN address instead of recomputing it here: it ranks
REM real NICs above virtual ones (VMware / WSL / Hyper-V / vEthernet), so this
REM prints the same IP the RTSP and WebRTC URLs advertise.
set "LAN_IP="
for /f "usebackq tokens=*" %%i in (`powershell -NoProfile -Command "$r = Invoke-RestMethod -Uri 'http://localhost:%HTTP_PORT%/api/net/addresses'; $r.public_host" 2^>nul`) do set "LAN_IP=%%i"

set "SHOW_LAN=0"
if defined LAN_IP if not "%LAN_IP%"=="127.0.0.1" set SHOW_LAN=1

echo ============================================================
echo  video-server is up
echo ------------------------------------------------------------
echo  Web UI      : http://localhost:%HTTP_PORT%/
echo  REST API    : http://localhost:%HTTP_PORT%/api/cameras
echo  Health      : http://localhost:%HTTP_PORT%/api/health
if "%SHOW_LAN%"=="1" (
  echo  --- from another machine on the LAN - source: GET /api/net/addresses ---
  echo  Web UI      : http://%LAN_IP%:%HTTP_PORT%/
  echo  If it times out, open the firewall first:
  echo     scripts\firewall-add.bat %HTTP_PORT% 8554
)
echo ------------------------------------------------------------
echo  Log         : %VS_LOG%
echo  Stop        : scripts\stop-joint.bat
echo  Full chain  : scripts\start-joint.bat   server + camera push + WebRTC
echo  Verify      : python scripts\verify_joint.py
echo ============================================================
echo.

if "%NO_BROWSER%"=="0" start "" http://localhost:%HTTP_PORT%/

if "%NOPAUSE%"=="1" exit /b 0
pause
exit /b 0

REM ============================================================================
REM  :pick_exe - set VS_NAME / VS_EXE to the freshest video-server*.exe
REM  dir /o-d lists newest first. Kept at top level: a ")" inside a ( ) block
REM  closes it early, and cmd does not strip ^ inside double quotes, so an
REM  escaped one would just reach the shell as a literal caret.
REM ============================================================================
:pick_exe
set "VS_NAME="
set "VS_EXE="
for /f "delims=" %%i in ('dir /b /o-d "%ROOT%\video-server*.exe" 2^>nul') do (
  if not defined VS_NAME set "VS_NAME=%%i"
)
if not defined VS_NAME exit /b 0
set "VS_EXE=%ROOT%\%VS_NAME%"
exit /b 0

REM ============================================================================
REM  :fail - single exit for every failure path
REM  Reached only by goto, so it always runs at top level. When the server died
REM  at startup, the tail of its log is the fastest way to the real cause, so
REM  print it here - pipe-free PowerShell, since ^| inside double quotes is not
REM  an escape in cmd and would reach PowerShell verbatim.
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
if exist "%VS_LOG%" (
  echo  last lines of %VS_LOG%:
  echo  ..........................................................
  powershell -NoProfile -Command "Get-Content -LiteralPath '%VS_LOG%' -Tail 20"
  echo  ..........................................................
)
echo ============================================================
echo.
if "%NOPAUSE%"=="1" exit /b 1
pause
exit /b 1
