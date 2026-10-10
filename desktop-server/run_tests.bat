@echo off
REM ============================================================
REM  VisionFlow Desktop - full acceptance test (requirements 1-4)
REM
REM  Asserts on the ASCII "RESULT key=value" lines printed by the
REM  headless probes. Do NOT assert on the localized human-readable
REM  lines: they are Chinese, and matching them in a .bat breaks
REM  under the console code page (GBK), producing garbage output.
REM
REM  Stages map to the four requirements:
REM    1-2  local file playback + hardware decode fallback
REM    3    local camera enumeration + capture
REM    4    network camera via video-server REST API
REM    5    AI metadata parsing + overlay rendering
REM
REM  Environment-dependent stages (camera, network) are reported as
REM  SKIP when the resource is absent. A SKIP is not a failure: no
REM  webcam and no running video-server are both legitimate setups.
REM
REM  Usage: run_tests.bat
REM ============================================================
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
set "BIN=%ROOT%src\VisionFlow.Desktop\bin\Debug\net9.0-windows\win-x64"
set "EXE=%BIN%\VisionFlow.Desktop.exe"
set "DATA=%ROOT%_testdata"
set "LOG=%TEMP%\vf_acceptance.log"
set "PASS=0"
set "FAIL=0"
set "SKIP=0"

echo ============================================
echo  VisionFlow Desktop - Acceptance Test
echo ============================================
echo.

if not exist "%EXE%" (
    echo [ERROR] Executable not found:
    echo         %EXE%
    echo         Run: dotnet build -c Debug
    echo.
    pause
    exit /b 1
)

if not exist "%DATA%\h264_720p.mp4" (
    echo [ERROR] Test data not found in %DATA%
    echo.
    pause
    exit /b 1
)

REM ---------- 1. Environment selfcheck ----------
echo [1/7] Environment selfcheck
"%EXE%" --selfcheck >nul 2>&1
call :Check "selfcheck exit code" !errorlevel! 0

REM ---------- 2. H264 hardware decode ----------
echo [2/7] H264 hardware decode
"%EXE%" --decodetest "%DATA%\h264_720p.mp4" --frames 30 --out "%TEMP%\vf_a1" > "%LOG%" 2>&1
call :Check "decode succeeded" !errorlevel! 0
call :Assert "backend=Hardware"  "RESULT backend=Hardware"
call :Assert "no fallback"       "RESULT fallback=None"

REM ---------- 3. H264 software decode ----------
echo [3/7] H264 software decode
"%EXE%" --decodetest "%DATA%\h264_720p.mp4" --soft --frames 30 --out "%TEMP%\vf_a2" > "%LOG%" 2>&1
call :Check "decode succeeded" !errorlevel! 0
call :Assert "backend=Software"  "RESULT backend=Software"

REM ---------- 4. Auto fallback chain ----------
echo [4/7] Auto fallback when hardware unavailable
"%EXE%" --decodetest "%DATA%\h264_720p.mp4" --failsim --frames 30 --out "%TEMP%\vf_a3" > "%LOG%" 2>&1
call :Check "decode succeeded after fallback" !errorlevel! 0
call :Assert "fell back to Software"      "RESULT backend=Software"
call :Assert "reason=DeviceCreateFailed"  "RESULT fallback=DeviceCreateFailed"

REM ---------- 5. Local camera capture ----------
echo [5/7] Local camera enumeration and capture
"%EXE%" --camtest --list > "%LOG%" 2>&1
call :AssertFound "camera enumeration ran" "RESULT devices="
findstr /C:"RESULT devices=0" "%LOG%" >nul 2>&1
if !errorlevel! equ 0 (
    echo   SKIP  no local camera present
    set /a SKIP+=1
) else (
    "%EXE%" --camtest --device 0 --frames 15 > "%LOG%" 2>&1
    call :Check "camera capture succeeded" !errorlevel! 0
    call :Assert "capture verdict PASS"     "RESULT verdict=PASS"
    call :Assert "device opened"            "RESULT opened=True"
    call :Assert "negotiated RGB32"         "RESULT pixfmt=Rgb32"
)

REM ---------- 6. Network camera (video-server) ----------
echo [6/7] Network camera via video-server REST API
"%EXE%" --nettest > "%LOG%" 2>&1
findstr /C:"RESULT reachable=False" "%LOG%" >nul 2>&1
if !errorlevel! equ 0 (
    echo   SKIP  video-server not reachable on 127.0.0.1:8081
    set /a SKIP+=1
) else (
    call :Assert "server reachable"        "RESULT reachable=True"
    call :AssertFound "camera list returned" "RESULT cameras="
    call :AssertFound "aimode readable"      "RESULT aimode="
)

REM ---------- 7. AI overlay rendering ----------
echo [7/7] AI overlay rendering and skeleton topology
"%EXE%" --overlaytest --out "%TEMP%\vf_a6" > "%LOG%" 2>&1
call :Check "overlay probe succeeded" !errorlevel! 0
call :Assert "all overlay assertions pass" "RESULT verdict=PASS"
call :Assert "zero overlay failures"       "RESULT failures=0"

echo.
echo ============================================
echo  Result: PASS=%PASS%  FAIL=%FAIL%  SKIP=%SKIP%
echo ============================================

if %FAIL% equ 0 (
    echo  STATUS: ALL PASSED
) else (
    echo  STATUS: FAILURES DETECTED
)
echo.
pause
exit /b %FAIL%

REM ============================================================
REM  Helpers - keep exit paths separated from the label bodies
REM  so a successful call cannot fall through into the failure
REM  message and corrupt the exit code.
REM ============================================================

:Assert
findstr /C:%~2 "%LOG%" >nul 2>&1
if !errorlevel! equ 0 (
    echo   PASS  %~1
    set /a PASS+=1
) else (
    echo   FAIL  %~1
    set /a FAIL+=1
)
exit /b 0

REM Assert that a line with the given prefix exists at all
REM (used when the exact value is environment-dependent).
:AssertFound
findstr /C:%~2 "%LOG%" >nul 2>&1
if !errorlevel! equ 0 (
    echo   PASS  %~1
    set /a PASS+=1
) else (
    echo   FAIL  %~1
    set /a FAIL+=1
)
exit /b 0

:Check
if "%~2"=="%~3" (
    echo   PASS  %~1
    set /a PASS+=1
) else (
    echo   FAIL  %~1  [got=%~2 want=%~3]
    set /a FAIL+=1
)
exit /b 0
