@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ============================================================================
REM  Remove the inbound firewall rules created by scripts\firewall-add.bat.
REM
REM  MUST RUN AS ADMINISTRATOR (right-click -> Run as administrator).
REM
REM  USAGE
REM    scripts\firewall-remove.bat                    (ports from config\config.yaml)
REM    scripts\firewall-remove.bat config\config.joint.yaml
REM    scripts\firewall-remove.bat 8081 8554 8889 8888
REM ============================================================================

set SCRIPT_DIR=%~dp0
if "%SCRIPT_DIR:~-1%"=="\" set SCRIPT_DIR=%SCRIPT_DIR:~0,-1%
for %%i in ("%SCRIPT_DIR%\..") do set "ROOT=%%~fi"

net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [ERROR] Not elevated. Right-click this script and choose
  echo         "Run as administrator".
  echo.
  pause
  exit /b 1
)

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
set HTTP_PORT=8080
set RTSP_PORT=8554
set WEBRTC_PORT=8889
set HLS_PORT=8888
set "SEEN_PORT=0"
for /f "usebackq tokens=1,2 delims=:" %%a in (`findstr /r /c:"http_port:" "%CFG%"`) do (
  for /f "tokens=1" %%v in ("%%b") do set "HTTP_PORT=%%v"
)
for /f "usebackq tokens=1,2 delims=:" %%a in (`findstr /r /c:"^  port:" "%CFG%"`) do (
  for /f "tokens=1" %%v in ("%%b") do (
    set /a SEEN_PORT+=1
    if "!SEEN_PORT!"=="1" set "RTSP_PORT=%%v"
    if "!SEEN_PORT!"=="2" set "WEBRTC_PORT=%%v"
  )
)
for /f "usebackq tokens=1,2 delims=:" %%a in (`findstr /r /c:"hls_port:" "%CFG%"`) do (
  for /f "tokens=1" %%v in ("%%b") do set "HLS_PORT=%%v"
)
set "PORTS=%HTTP_PORT% %RTSP_PORT% %WEBRTC_PORT% %HLS_PORT%"

:haveports
echo [*] removing rules for ports: %PORTS%
for %%P in (%PORTS%) do (
  netsh advfirewall firewall delete rule name="video-server TCP %%P" >nul 2>&1
  if !errorlevel!==0 (echo [OK]    removed TCP %%P) else (echo [note]  no rule for TCP %%P)
)
netsh advfirewall firewall delete rule name="video-server mediamtx" >nul 2>&1
if !errorlevel!==0 (echo [OK]    removed mediamtx program rule) else (echo [note]  no mediamtx program rule)
echo.
echo done - the server is now unreachable from other machines again.
echo.
endlocal
exit /b 0
