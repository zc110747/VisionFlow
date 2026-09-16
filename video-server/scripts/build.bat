@echo off
setlocal
cd /d "%~dp0\.."

:: Locate Go: prefer PATH, fall back to the bundled local toolchain (.toolchain/go).
set GO_BIN=go
where go >nul 2>nul
if %errorlevel% neq 0 (
  if exist ".toolchain\go\go\bin\go.exe" (
    set "GO_BIN=%CD%\.toolchain\go\go\bin\go.exe"
    echo using bundled go: %GO_BIN%
  ) else (
    echo ERROR: go not found on PATH and .toolchain\go\go\bin\go.exe missing.
    echo Install Go from https://go.dev/dl, or run scripts\setup-go.bat to fetch it locally.
    exit /b 1
  )
)

echo [1/3] installing web dependencies
cd web
call npm install --no-audit --no-fund
if %errorlevel% neq 0 exit /b %errorlevel%

echo [2/3] building web ui
call npm run build
if %errorlevel% neq 0 exit /b %errorlevel%
cd ..

echo [3/3] building video-server
"%GO_BIN%" build -trimpath -o video-server.exe ./cmd/video-server
if %errorlevel% equ 0 (
  echo build complete: video-server.exe
  endlocal
  exit /b 0
)

REM video-server.exe could not be written. The usual cause is that the old
REM binary is still running (Windows keeps an exclusive handle on a running
REM .exe), so fall back to a second name instead of failing the build.
echo [WARN] video-server.exe is locked - is an old instance still running?
echo        Retrying as video-server.new.exe ...
"%GO_BIN%" build -trimpath -o video-server.new.exe ./cmd/video-server
if %errorlevel% neq 0 (
  echo.
  echo [ERROR] build failed. Stop the running server first:
  echo         scripts\stop-joint.bat   or   taskkill /F /IM video-server.exe
  exit /b 1
)
echo build complete: video-server.new.exe  (run.bat uses it automatically)

endlocal
