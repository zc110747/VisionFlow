@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ============================================================================
REM  Open the video-server ports in Windows Defender Firewall (inbound).
REM
REM  WHY THIS EXISTS
REM    The server binds 0.0.0.0, so it already answers on every local address.
REM    Windows Firewall drops unsolicited inbound traffic anyway, so without
REM    these rules another machine sees "connection timed out" while the
REM    server is perfectly healthy - by far the most common "LAN does not
REM    work" cause.
REM
REM  MUST RUN AS ADMINISTRATOR (right-click -> Run as administrator).
REM  Adding firewall rules requires elevation; the script tells you if it is
REM  not elevated instead of failing silently.
REM
REM  USAGE
REM    scripts\firewall-add.bat
REM        ports read from config\config.yaml
REM    scripts\firewall-add.bat config\config.joint.yaml
REM        ports read from that config file
REM    scripts\firewall-add.bat 8081 8554 8889 8888
REM        explicit TCP ports (a numeric first argument switches to this mode)
REM
REM  WHAT IT OPENS
REM    TCP  <http_port> <rtsp_port> <webrtc_port> <hls_port>   from the config
REM    any  port used by mediamtx.exe (program rule)
REM        The UDP side of media (RTP/RTCP, WebRTC ICE) has no fixed port in
REM        this project's config, so it is allowed via the binary instead of
REM        hardcoding numbers here.
REM
REM  UNDO:  scripts\firewall-remove.bat
REM ============================================================================

REM ---- paths: this script lives in <video-server>\scripts ----
set SCRIPT_DIR=%~dp0
if "%SCRIPT_DIR:~-1%"=="\" set SCRIPT_DIR=%SCRIPT_DIR:~0,-1%
for %%i in ("%SCRIPT_DIR%\..") do set "ROOT=%%~fi"

REM ---- elevation check: 'net session' only succeeds when elevated ----
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [ERROR] Not elevated. Right-click this script and choose
  echo         "Run as administrator" - adding firewall rules requires it.
  echo.
  pause
  exit /b 1
)

REM ---- figure out the port list ----
set "PORTS="
set "CFG=%ROOT%\config\config.yaml"

if "%~1"=="" goto :readcfg
echo %~1| findstr /r "^[0-9][0-9]*$" >nul
if %errorlevel%==0 (
  set "PORTS=%*"
  goto :haveports
)
set "CFG=%~1"

:readcfg
if not exist "%CFG%" (
  echo [ERROR] config not found: %CFG%
  pause
  exit /b 1
)
echo [*] reading ports from %CFG%

for /f "usebackq tokens=1,2 delims=:" %%a in (`findstr /r /c:"http_port:" "%CFG%"`) do (
  for /f "tokens=1" %%v in ("%%b") do set "HTTP_PORT=%%v"
)
set "RTSP_PORT="
for /f "usebackq tokens=1,2 delims=:" %%a in (`findstr /r /c:"^  port:" "%CFG%"`) do (
  if "!RTSP_PORT!"=="" for /f "tokens=1" %%v in ("%%b") do set "RTSP_PORT=%%v"
)
for /f "usebackq tokens=1,2 delims=:" %%a in (`findstr /r /c:"api_port:" "%CFG%"`) do (
  for /f "tokens=1" %%v in ("%%b") do set "API_PORT=%%v"
)

if not defined HTTP_PORT set HTTP_PORT=8080
if not defined RTSP_PORT set RTSP_PORT=8554

REM webrtc.port and mediamtx.hls_port share the key name "port:", so the first
REM bare one is rtsp.port and the second is webrtc.port.
set "WEBRTC_PORT="
set "SEEN_PORT=0"
for /f "usebackq tokens=1,2 delims=:" %%a in (`findstr /r /c:"^  port:" "%CFG%"`) do (
  for /f "tokens=1" %%v in ("%%b") do (
    set /a SEEN_PORT+=1
    if "!SEEN_PORT!"=="1" set "RTSP_PORT=%%v"
    if "!SEEN_PORT!"=="2" set "WEBRTC_PORT=%%v"
  )
)
set "HLS_PORT="
for /f "usebackq tokens=1,2 delims=:" %%a in (`findstr /r /c:"hls_port:" "%CFG%"`) do (
  for /f "tokens=1" %%v in ("%%b") do set "HLS_PORT=%%v"
)
if not defined WEBRTC_PORT set WEBRTC_PORT=8889
if not defined HLS_PORT set HLS_PORT=8888

set "PORTS=%HTTP_PORT% %RTSP_PORT% %WEBRTC_PORT% %HLS_PORT%"

:haveports
echo [*] ports to open: %PORTS%

REM ---- (1) inbound TCP rules, one per port ----
for %%P in (%PORTS%) do (
  netsh advfirewall firewall delete rule name="video-server TCP %%P" >nul 2>&1
  netsh advfirewall firewall add rule name="video-server TCP %%P" dir=in action=allow protocol=TCP localport=%%P profile=any >nul
  if !errorlevel!==0 (echo [OK]    TCP %%P) else (echo [FAIL]  TCP %%P)
)

REM ---- (2) program rule for mediamtx: covers its UDP media ports ----
set "MTX=%ROOT%\mediamtx\mediamtx.exe"
if exist "%MTX%" (
  netsh advfirewall firewall delete rule name="video-server mediamtx" >nul 2>&1
  netsh advfirewall firewall add rule name="video-server mediamtx" dir=in action=allow program="%MTX%" enable=yes profile=any >nul
  if !errorlevel!==0 (echo [OK]    mediamtx.exe program rule) else (echo [FAIL]  mediamtx.exe program rule)
) else (
  echo [note]  %MTX% not found - skipped the mediamtx program rule ^(UDP media^)
)

REM ---- (3) show the URLs other machines can now use ----
echo.
echo ============================================================
echo  Firewall rules added. Reachable addresses:
for /f "usebackq tokens=*" %%i in (`powershell -NoProfile -Command "Get-NetIPAddress -AddressFamily IPv4 ^| Where-Object { $_.IPAddress -ne '127.0.0.1' } ^| ForEach-Object { $_.IPAddress }"`) do (
  for %%P in (%PORTS%) do (
    if "%%P"=="%HTTP_PORT%" echo    http://%%i:%%P/
  )
)
echo ============================================================
echo.
echo  Verify from another machine:  curl http://^<this-ip^>:%HTTP_PORT%/api/health
echo  Remove the rules again:       scripts\firewall-remove.bat
echo.
endlocal
exit /b 0
