@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM One-click build for Camera Agent (MSVC cl.exe + Ninja).
REM
REM Self-contained: locates Visual Studio via vswhere, sets up the native
REM MSVC x64 environment with vcvarsall.bat, then configures and builds with
REM the VS-bundled CMake/Ninja. No external MinGW/MSYS needed.
REM
REM Usage:
REM   build_oneclick.bat              build gstreamer backend (default)
REM   build_oneclick.bat sim          build SIM backend (no GStreamer needed)
REM   build_oneclick.bat auto         let CMake pick gstreamer or sim
REM   build_oneclick.bat clean        wipe build-msvc first, then build
REM   build_oneclick.bat gstreamer clean

set ROOT=%~dp0
if "%ROOT:~-1%"=="\" set ROOT=%ROOT:~0,-1%
cd /d "%ROOT%"

set BACKEND=gstreamer
set CLEAN=0
set NOPAUSE=0
for %%a in (%*) do (
  if /i "%%a"=="sim"       set BACKEND=sim
  if /i "%%a"=="auto"      set BACKEND=auto
  if /i "%%a"=="gstreamer" set BACKEND=gstreamer
  if /i "%%a"=="clean"     set CLEAN=1
  if /i "%%a"=="--no-pause" set NOPAUSE=1
)

REM ---- locate Visual Studio ----
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set VSPATH=
if exist %VSWHERE% (
  for /f "usebackq tokens=*" %%p in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    if not defined VSPATH set VSPATH=%%p
  )
)
if not defined VSPATH set VSPATH=D:\Software\vs
if not exist "%VSPATH%" (
  echo [ERROR] Visual Studio not found - looked in %VSPATH%
  echo         Install VS2022 with the "Desktop development with C++" workload.
  pause
  exit /b 1
)

REM ---- native MSVC x64 environment ----
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (
  echo [ERROR] vcvarsall.bat failed for '%VSPATH%'.
  pause
  exit /b 1
)

REM ---- add VS-bundled CMake/Ninja to PATH (vcvarsall does not) ----
set CMAKE_BIN=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin
set NINJA_BIN=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja
if exist "%CMAKE_BIN%\cmake.exe" set "PATH=%CMAKE_BIN%;%PATH%"
if exist "%NINJA_BIN%\ninja.exe" set "PATH=%NINJA_BIN%;%PATH%"

echo ============================================================
echo  Camera Agent build  (backend=%BACKEND%)
echo  VS     : %VSPATH%
where cmake >nul 2>&1 && echo  cmake  : OK || echo  cmake  : MISSING
where ninja >nul 2>&1 && echo  ninja  : OK || echo  ninja  : MISSING
echo ============================================================

set BUILD=%ROOT%\build-msvc
if "%CLEAN%"=="1" (
  echo --- clean: removing %BUILD% ---
  if exist "%BUILD%" rmdir /s /q "%BUILD%"
)

echo --- configure ---
cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCAMERA_AGENT_BACKEND=%BACKEND%
if errorlevel 1 (
  echo [ERROR] CMake configure failed.
  pause
  exit /b 1
)

echo --- build ---
cmake --build "%BUILD%"
if errorlevel 1 (
  echo [ERROR] Build failed.
  pause
  exit /b 1
)

echo.
echo Build OK: %BUILD%
echo   camera-agent : %BUILD%\src\camera-agent.exe
echo   run tests    : ctest --test-dir "%BUILD%" --output-on-failure
echo.
if "%NOPAUSE%"=="0" pause
