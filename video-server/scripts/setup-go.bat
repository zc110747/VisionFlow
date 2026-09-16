@echo off
setlocal
cd /d "%~dp0\.."
:: Download and install a local Go toolchain into .toolchain/go (no system install needed).
:: Used by build.bat when `go` is not on PATH.

set GO_VER=1.27.0
set GO_ZIP=.toolchain\go.zip
set GO_DIR=.toolchain\go

if exist "%GO_DIR%\go\bin\go.exe" (
  echo go already present at %GO_DIR%\go\bin\go.exe
  goto :eof
)

if not exist ".toolchain" mkdir .toolchain

echo downloading go %GO_VER% (windows/amd64)...
curl -sSL -o "%GO_ZIP%" "https://golang.google.cn/dl/go%GO_VER%.windows-amd64.zip"
if %errorlevel% neq 0 (
  echo download failed; try https://go.dev/dl/go%GO_VER%.windows-amd64.zip manually
  exit /b 1
)

echo extracting...
powershell -Command "Expand-Archive -Force -Path '%GO_ZIP%' -DestinationPath '%GO_DIR%'"
if %errorlevel% neq 0 exit /b 1

echo done: %GO_DIR%\go\bin\go.exe
endlocal
