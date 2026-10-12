@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ============================================================================
REM  build_oneclick.bat - build desktop-server in one shot
REM
REM    [1/4] preflight   dotnet SDK / ffmpeg runtime / source tree
REM    [2/4] restore     NuGet restore
REM    [3/4] build       dotnet build -> src\VisionFlow.Desktop\bin\<Config>\...
REM    [4/4] summary
REM
REM  Every failure path pauses and prints a reason plus a hint.
REM
REM  Usage:
REM    build_oneclick.bat                    build Debug (x64)
REM    build_oneclick.bat Release            build Release
REM    build_oneclick.bat --clean            remove bin\obj first
REM    build_oneclick.bat --no-pause         unattended (CI); failures also skip pause
REM
REM  Env overrides:
REM    FFMPEG_ROOT   ffmpeg shared build root (needs avutil-60 / avcodec-62,
REM                  i.e. ffmpeg 8.0/8.1 - it must match FFmpeg.AutoGen 8.0.0)
REM
REM  Notes for whoever edits this next:
REM    - never use a bare "dotnet": resolve it to an absolute path first, because
REM      this machine has no dotnet on PATH in some shells.
REM    - the ffmpeg check mirrors the csproj VerifyFFmpegPresent target exactly,
REM      so a failure here is reported with a hint instead of a raw MSBuild error.
REM    - cmd does NOT strip ^ inside double quotes; keep PowerShell out of this.
REM ============================================================================

REM ---- paths: this script sits at the desktop-server root ----
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "PROJ=%ROOT%\src\VisionFlow.Desktop\VisionFlow.Desktop.csproj"

REM ---- defaults ----
set "CONFIG=Debug"
set "CLEAN=0"
set "NOPAUSE=0"
set "_ctx="

for %%a in (%*) do (
  if defined _ctx (
    if "!_ctx!"=="config" set "CONFIG=%%~a"
    set "_ctx="
  ) else (
    if /i "%%a"=="--clean"    set CLEAN=1
    if /i "%%a"=="--no-pause" set NOPAUSE=1
    if /i "%%a"=="--config"   set "_ctx=config"
    if /i "%%a"=="debug"      set CONFIG=Debug
    if /i "%%a"=="release"    set CONFIG=Release
  )
)

set "FAIL_STEP=startup"
set "FAIL_REASON="
set "FAIL_HINT1="
set "FAIL_HINT2="
set "FAIL_HINT3="

echo ============================================================
echo  One-click build: desktop-server
echo  root   : %ROOT%
echo  config : %CONFIG%
echo ============================================================

REM ============================================================================
REM  [1/4] preflight
REM ============================================================================
echo.
echo [1/4] preflight

if not exist "%PROJ%" (
  set "FAIL_STEP=1/4 preflight"
  set "FAIL_REASON=project file not found: %PROJ%"
  set "FAIL_HINT1=Expected src\VisionFlow.Desktop\VisionFlow.Desktop.csproj under %ROOT%"
  set "FAIL_HINT2=Make sure this script sits at the desktop-server root, not inside src\"
  goto :fail
)

REM ---- dotnet: prefer PATH, fall back to the standard install location ----
set "DOTNET_BIN="
for /f "delims=" %%i in ('where dotnet 2^>nul') do (
  if not defined DOTNET_BIN set "DOTNET_BIN=%%i"
)
if not defined DOTNET_BIN (
  if exist "%ProgramFiles%\dotnet\dotnet.exe" set "DOTNET_BIN=%ProgramFiles%\dotnet\dotnet.exe"
)
if not defined DOTNET_BIN (
  if exist "%ProgramFiles(x86)%\dotnet\dotnet.exe" set "DOTNET_BIN=%ProgramFiles(x86)%\dotnet\dotnet.exe"
)
if not defined DOTNET_BIN (
  set "FAIL_STEP=1/4 preflight"
  set "FAIL_REASON=dotnet SDK not found - 'dotnet' is not on PATH and %ProgramFiles%\dotnet\dotnet.exe does not exist"
  REM Square brackets, not parentheses: a ")" inside a ( ) block closes it early
  REM and truncates the line - these hints only print on failure, so the damage
  REM would show up exactly when the message matters most.
  set "FAIL_HINT1=Install the .NET 9 SDK [x64] from https://dotnet.microsoft.com/download"
  set "FAIL_HINT2=This project targets net9.0-windows and needs the Windows desktop SDK"
  goto :fail
)

REM Read the SDK version. "dotnet --version" prints the resolved SDK, which is
REM what actually builds the project - not the runtime list.
set "DOTNET_VER=unknown"
for /f "tokens=*" %%v in ('"%DOTNET_BIN%" --version 2^>nul') do set "DOTNET_VER=%%v"
echo   dotnet : %DOTNET_BIN%  (SDK %DOTNET_VER%)

REM ---- ffmpeg runtime: the hard dependency, version-locked to AutoGen 8.0.0 ----
REM Mirrors the VerifyFFmpegPresent target in the csproj. Checking it here turns
REM a bare MSBuild error into an actionable hint with the correct folder name.
set "FF_ROOT="
if defined FFMPEG_ROOT set "FF_ROOT=%FFMPEG_ROOT%"
if not defined FF_ROOT set "FF_ROOT=D:\data\agent-tools\ffmpeg-n8.1-latest-win64-gpl-shared"

if not exist "%FF_ROOT%\bin\avcodec-62.dll" (
  set "FAIL_STEP=1/4 preflight"
  set "FAIL_REASON=ffmpeg runtime not found or wrong major version: %FF_ROOT%\bin\avcodec-62.dll"
  set "FAIL_HINT1=FFmpeg.AutoGen 8.0.0 needs ffmpeg 8.0/8.1 - avutil-60 / avcodec-62 / avformat-62"
  set "FAIL_HINT2=Point FFMPEG_ROOT at a matching shared build, or set it in this shell:"
  set "FAIL_HINT3=  set FFMPEG_ROOT=D:\path\to\ffmpeg-n8.1-latest-win64-gpl-shared"
  goto :fail
)
echo   ffmpeg : %FF_ROOT%

REM The version pair is the whole reason this build can silently produce a binary
REM that dies at the first av_* call. Verify the astride DLL is there too.
if not exist "%FF_ROOT%\bin\avutil-60.dll" (
  set "FAIL_STEP=1/4 preflight"
  set "FAIL_REASON=ffmpeg runtime is present but avutil-60.dll is missing under %FF_ROOT%\bin"
  set "FAIL_HINT1=This looks like an ffmpeg 9.x / master build with avutil-61 - it will not load"
  set "FAIL_HINT2=Use ffmpeg-n8.1-latest-win64-gpl-shared, which ships avutil-60"
  goto :fail
)

echo [OK]    preflight done

REM ============================================================================
REM  [2/4] clean (optional)
REM ============================================================================
echo.
echo [2/4] clean
if "%CLEAN%"=="1" (
  echo   removing src\VisionFlow.Desktop\bin and obj ...
  if exist "%ROOT%\src\VisionFlow.Desktop\bin" rmdir /s /q "%ROOT%\src\VisionFlow.Desktop\bin"
  if exist "%ROOT%\src\VisionFlow.Desktop\obj" rmdir /s /q "%ROOT%\src\VisionFlow.Desktop\obj"
  echo   [OK]    cleaned
) else (
  REM No parentheses in this message on purpose: a ")" inside a ( ) block closes
  REM the block early, which silently truncates the line.
  echo   skipped   --clean not given - incremental build
)

REM ============================================================================
REM  [3/4] restore + build
REM ============================================================================
echo.
echo [3/4] build  %CONFIG%

set "BIN_DIR=%ROOT%\src\VisionFlow.Desktop\bin\%CONFIG%\net9.0-windows\win-x64"
set "EXE_PATH=%BIN_DIR%\VisionFlow.Desktop.exe"

REM Windows holds an exclusive handle on a running .exe, so the copy step fails
REM with "Access is denied" - which then reads like a code problem. Detect it up
REM front so the message can say what is actually wrong.
set "EXE_LOCKED=0"
tasklist /FI "IMAGENAME eq VisionFlow.Desktop.exe" 2>nul | findstr /i "VisionFlow.Desktop.exe" >nul
if not errorlevel 1 set "EXE_LOCKED=1"
if "!EXE_LOCKED!"=="1" (
  echo   [WARN] VisionFlow.Desktop.exe is running - its file is write-locked
  echo          the build may fail while copying the output; close the app first
)

echo   dotnet build -c %CONFIG% ...
pushd "%ROOT%"
"%DOTNET_BIN%" build "%PROJ%" -c %CONFIG% -v minimal --nologo
set "BUILD_RC=!errorlevel!"
popd

if not "!BUILD_RC!"=="0" (
  set "FAIL_STEP=3/4 build"
  set "FAIL_REASON=dotnet build returned exit code !BUILD_RC! - see the compiler output above"
  set "FAIL_HINT1=Reproduce by hand: dotnet build "%PROJ%" -c %CONFIG%"
  if "!EXE_LOCKED!"=="1" (
    set "FAIL_REASON=dotnet build failed with exit code !BUILD_RC! - the output .exe is write-locked by a running instance"
    set "FAIL_HINT1=Close the VisionFlow Desktop window, then retry"
    set "FAIL_HINT2=or from another shell: taskkill /F /IM VisionFlow.Desktop.exe"
  )
  goto :fail
)

if not exist "%EXE_PATH%" (
  set "FAIL_STEP=3/4 build"
  set "FAIL_REASON=build reported success but the executable is missing: %EXE_PATH%"
  set "FAIL_HINT1=Check TargetFramework / RuntimeIdentifier in VisionFlow.Desktop.csproj"
  goto :fail
)

REM ffmpeg DLLs are copied by the CopyFFmpegRuntime MSBuild target - confirm,
REM because a missing DLL is a runtime crash, not a build error.
if not exist "%BIN_DIR%\avcodec-62.dll" (
  set "FAIL_STEP=3/4 build"
  set "FAIL_REASON=build succeeded but ffmpeg DLLs were not copied into %BIN_DIR%"
  set "FAIL_HINT1=The CopyFFmpegRuntime target did not run - check the csproj AfterTargets"
  set "FAIL_HINT2=Workaround: copy %FF_ROOT%\bin\*.dll into %BIN_DIR% by hand"
  goto :fail
)
echo   [OK]    %EXE_PATH%

REM ============================================================================
REM  [4/4] summary
REM ============================================================================
echo.
echo [4/4] summary
echo ------------------------------------------------------------
call :show_file "%EXE_PATH%" "exe"
call :show_file "%BIN_DIR%\VisionFlow.Desktop.dll" "dll"
call :show_file "%BIN_DIR%\avcodec-62.dll" "avcodec"
echo ------------------------------------------------------------
echo  BUILD OK   (%CONFIG%, x64)
echo.
echo  Next:
echo    run_onestep.bat             build if needed, then launch the app
echo    run_tests.bat               run the headless acceptance tests
echo ============================================================
echo.

if "%NOPAUSE%"=="1" exit /b 0
pause
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
  set /a "SF_KB=!SF_BYTES! / 1024"
  echo   %SF_LABEL% : !SF_BYTES! bytes  ~!SF_KB! KB   !SF_TIME!
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
