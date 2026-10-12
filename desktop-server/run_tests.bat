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
REM    4    network camera via the EMBEDDED server (RTSP ingest)
REM    5    AI metadata parsing + overlay rendering
REM
REM  Stage 6 exercises the embedded server: MediaMTX is launched in
REM  process, ffmpeg plays the role of camera-agent (pushes to :8554)
REM  and the metadata/aimode HTTP contract on :8099 is checked.
REM
REM  Environment-dependent stages (camera, mediamtx.exe) are reported
REM  as SKIP when the resource is absent. A SKIP is not a failure: no
REM  webcam and no MediaMTX binary are both legitimate setups.
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
echo [1/9] Environment selfcheck
"%EXE%" --selfcheck >nul 2>&1
call :Check "selfcheck exit code" !errorlevel! 0

REM ---------- 2. H264 hardware decode ----------
echo [2/9] H264 hardware decode
"%EXE%" --decodetest "%DATA%\h264_720p.mp4" --frames 30 --out "%TEMP%\vf_a1" > "%LOG%" 2>&1
call :Check "decode succeeded" !errorlevel! 0
call :Assert "backend=Hardware"  "RESULT backend=Hardware"
call :Assert "no fallback"       "RESULT fallback=None"

REM ---------- 3. H264 software decode ----------
echo [3/9] H264 software decode
"%EXE%" --decodetest "%DATA%\h264_720p.mp4" --soft --frames 30 --out "%TEMP%\vf_a2" > "%LOG%" 2>&1
call :Check "decode succeeded" !errorlevel! 0
call :Assert "backend=Software"  "RESULT backend=Software"

REM ---------- 4. Auto fallback chain ----------
echo [4/9] Auto fallback when hardware unavailable
"%EXE%" --decodetest "%DATA%\h264_720p.mp4" --failsim --frames 30 --out "%TEMP%\vf_a3" > "%LOG%" 2>&1
call :Check "decode succeeded after fallback" !errorlevel! 0
call :Assert "fell back to Software"      "RESULT backend=Software"
call :Assert "reason=DeviceCreateFailed"  "RESULT fallback=DeviceCreateFailed"

REM ---------- 5. Local camera capture ----------
echo [5/9] Local camera enumeration and capture
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

REM ---------- 6. Embedded server (RTSP ingest + metadata) ----------
echo [6/9] Embedded server: RTSP ingest, metadata and aimode endpoints
REM Use a DEDICATED log for this stage. The suite shares %LOG% across all
REM stages, and this stage takes ~20s (it launches MediaMTX and pushes a
REM real RTSP stream); the redirect handle can still be flushing when the
REM next stage truncates the same file, which silently turns the tail
REM assertions (mtx_stopped / port released) into false FAILs.
set "SLOG=%TEMP%\vf_server_stage.log"
"%EXE%" --servertest --seconds 15 > "%SLOG%" 2>&1
set "SRV_RC=!errorlevel!"
findstr /C:"RESULT verdict=NO_BINARY" "%SLOG%" >nul 2>&1
if !errorlevel! equ 0 (
    echo   SKIP  mediamtx.exe not found
    set /a SKIP+=1
    goto :server_done
)

REM NOTE: "call :label" must not appear inside a parenthesised block -
REM cmd resolves the label before the block body runs and reports
REM "The system cannot find the batch label specified - Check".
call :CheckLog "server probe exit code" !SRV_RC! 0 "%SLOG%"
call :AssertLog "server probe verdict PASS"   "RESULT verdict=PASS"              "%SLOG%"
call :AssertLog "server started"              "CHK server_started=ok"            "%SLOG%"
call :AssertLog "mediamtx process running"    "CHK mtx_process_running=ok"       "%SLOG%"
call :AssertLog "metadata listener up"        "CHK metadata_listening=ok"        "%SLOG%"
call :AssertLog "mediamtx control API"        "CHK api_paths_list_reachable=ok"  "%SLOG%"
call :AssertLog "health endpoint"             "CHK health_200=ok"                "%SLOG%"
call :AssertLog "frame POST accepted"         "CHK post_frame_204=ok"            "%SLOG%"
call :AssertLog "status POST accepted"        "CHK post_status_204=ok"           "%SLOG%"
call :AssertLog "malformed JSON rejected"     "CHK post_malformed_400=ok"        "%SLOG%"
call :AssertLog "missing camera_id rejected"  "CHK post_no_camera_id_400=ok"     "%SLOG%"
call :AssertLog "unknown type rejected"       "CHK post_unknown_type_400=ok"     "%SLOG%"
call :AssertLog "camera merged into list"     "CHK camera_from_metadata_listed=ok" "%SLOG%"
call :AssertLog "default aimode is ai-y"      "CHK default_mode_ai_y=ok"         "%SLOG%"
call :AssertLog "aimode set accepted"         "CHK post_aimode_pose_200=ok"      "%SLOG%"
call :AssertLog "aimode readback matches"     "CHK readback_ai_y_pose=ok"        "%SLOG%"
call :AssertLog "invalid aimode rejected"     "CHK post_invalid_mode_400=ok"     "%SLOG%"
call :AssertLog "ai-off accepted"             "CHK post_ai_off_200=ok"           "%SLOG%"
call :AssertLog "RTSP push became ready"      "CHK mtx_path_ready=ok"            "%SLOG%"
call :AssertLog "path carries video tracks"   "CHK path_has_tracks=ok"           "%SLOG%"
call :AssertLog "pulled stream decoded"       "CHK rtsp_pull_decode=ok"          "%SLOG%"
call :AssertLog "camera online after push"    "CHK camera_online_after_push=ok"  "%SLOG%"
call :AssertLog "mediamtx stopped"            "CHK mtx_stopped=ok"               "%SLOG%"
call :AssertLog "metadata stopped"            "CHK metadata_stopped=ok"          "%SLOG%"
call :AssertLog "port 8099 released"          "CHK metadata_port_released=ok"    "%SLOG%"
:server_done

REM ---------- 7. AI overlay rendering ----------
echo [7/9] AI overlay rendering and skeleton topology
"%EXE%" --overlaytest --out "%TEMP%\vf_a6" > "%LOG%" 2>&1
call :Check "overlay probe succeeded" !errorlevel! 0
call :Assert "all overlay assertions pass" "RESULT verdict=PASS"
call :Assert "zero overlay failures"       "RESULT failures=0"

REM ---------- 8. AI overlay on real decoded frame ----------
echo [8/9] AI overlay composited onto real decoded frame
"%EXE%" --realframe "%DATA%\h264_720p.mp4" --frames 5 --out "%TEMP%\vf_a7" > "%LOG%" 2>&1
if !errorlevel! equ 2 (
    echo   SKIP  test video absent (realframe probe)
    set /a SKIP+=1
) else (
    call :Check "realframe probe succeeded" !errorlevel! 0
    call :Assert "overlay on real frame verdict PASS" "RESULT verdict=PASS"
    call :Assert "zero realframe failures"          "RESULT failures=0"
)

REM ---------- 9. Camera cross-thread event regression ----------
echo [9/9] Camera cross-thread event marshaling regression
"%EXE%" --threadtest > "%LOG%" 2>&1
if !errorlevel! equ 2 (
    echo   SKIP  no local camera for threadtest
    set /a SKIP+=1
) else (
    call :Check "threadtest probe succeeded" !errorlevel! 0
    call :Assert "no cross-thread exceptions"      "RESULT phaseA_exceptions=0"
    call :Assert "handler runs on UI thread"        "RESULT handler_on_ui_thread="
    call :Assert "worker-thread event confirmed"    "RESULT phaseB_worker_thread_event=True"
    call :Assert "threadtest verdict PASS"          "RESULT verdict=PASS"
)

echo.
echo ============================================
echo  Result: PASS=%PASS%  FAIL=%FAIL%  SKIP=%SKIP%
echo ============================================

goto :finish

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

REM Same as :Assert but searches an explicitly named log file.
REM Needed because one stage writes its own log (see stage 6).
:AssertLog
findstr /C:%~2 %~3 >nul 2>&1
if !errorlevel! equ 0 (
    echo   PASS  %~1
    set /a PASS+=1
) else (
    echo   FAIL  %~1
    set /a FAIL+=1
)
exit /b 0

:CheckLog
if "%~2"=="%~3" (
    echo   PASS  %~1
    set /a PASS+=1
) else (
    echo   FAIL  %~1  [got=%~2 want=%~3]
    set /a FAIL+=1
)
exit /b 0

REM ============================================================
REM  Terminal label. Reached only via "goto :finish" from above,
REM  so the helper bodies can never be fallen into, and the
REM  summary is printed exactly once.
REM ============================================================
:finish
if %FAIL% equ 0 (
    echo  STATUS: ALL PASSED
) else (
    echo  STATUS: FAILURES DETECTED
)
echo.
pause
exit /b %FAIL%
