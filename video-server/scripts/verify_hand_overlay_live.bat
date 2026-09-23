@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ============================================================================
REM  verify_hand_overlay_live.bat - pixel-level acceptance for the hand overlay.
REM
REM  Answers the one question the offline checks cannot: does the skeleton the
REM  browser paints actually sit ON the hand? Needs a human in front of the
REM  camera holding a hand up for about 30 seconds.
REM
REM  Why this is a .bat and not just the .py: on this machine the browser
REM  daemon cannot be booted from inside a Python subprocess - the whole command
REM  is killed with no output - while the identical CLI calls issued from a
REM  shell work. So the shell drives agent-browser here and Python only waits
REM  and asserts. See the .py header for the full story.
REM
REM  Order matters: the hand is confirmed FIRST, then the browser opens and is
REM  driven to completion in a few seconds. A browser session that sits idle
REM  for the ~2 minutes it takes an operator to raise a hand gets its daemon
REM  reaped, and every later CLI call then fails with "Chrome not found".
REM
REM  Result: a PASS/FAIL count, and a screenshot under %TEMP%\vf-shots\.
REM  Images are never written into the repository.
REM ============================================================================

set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "SHOT=%TEMP%\vf-shots"
set "DOM=%SHOT%\dom-live.json"
set "FRAME=%SHOT%\frame-at-paint.json"
set "CA=camera01"
set "HOST=127.0.0.1:8081"
REM Seconds to wait for a hand. Override to smoke-test the plumbing with no
REM operator present:  set WAIT_HAND=3  before running this script.
if not defined WAIT_HAND set "WAIT_HAND=180"

REM ---- args ----
set "_ctx="
for %%a in (%*) do (
  if defined _ctx (
    if "!_ctx!"=="camera" set "CA=%%~a"
    set "_ctx="
  ) else (
    if /i "%%a"=="--camera" set "_ctx=camera"
  )
)

echo ============================================================
echo  Hand overlay live acceptance - camera %CA% at %HOST%
echo ============================================================

REM ---- locate python -------------------------------------------------------
set "PY="
for /f "delims=" %%i in ('where python 2^>nul') do if not defined PY set "PY=%%i"
if not defined PY (
  echo [ERROR] python not found on PATH
  goto :fail
)
echo [OK]    python: %PY%

REM ---- locate agent-browser ------------------------------------------------
set "AB="
if exist "%APPDATA%\npm\agent-browser.cmd" set "AB=%APPDATA%\npm\agent-browser.cmd"
if not defined AB (
  for /f "delims=" %%i in ('where agent-browser 2^>nul') do if not defined AB set "AB=%%i"
)
if not defined AB (
  echo [ERROR] agent-browser not found. Install it:  npm install -g agent-browser
  goto :fail
)
echo [OK]    agent-browser: %AB%

REM ---- locate a Chromium ---------------------------------------------------
REM agent-browser keeps its own browser cache, which is empty on this machine.
REM The Chromium playwright already downloaded is fully usable through
REM --executable-path, so look for it there. NOTE: `dir /s` cannot take a
REM wildcard in a path COMPONENT - only in the last element - so the revision
REM directory is enumerated first and the inner path is built from it. The
REM obvious one-liner ("dir /s ...chromium-*\chrome-win64\chrome.exe") matches
REM nothing and fails silently, which reads as "no browser installed".
set "CHROME="
for /f "delims=" %%d in ('dir /b /ad /o-n "%LOCALAPPDATA%\ms-playwright\chromium-*" 2^>nul') do (
  if not defined CHROME if exist "%LOCALAPPDATA%\ms-playwright\%%d\chrome-win64\chrome.exe" set "CHROME=%LOCALAPPDATA%\ms-playwright\%%d\chrome-win64\chrome.exe"
  if not defined CHROME if exist "%LOCALAPPDATA%\ms-playwright\%%d\chrome-win\chrome.exe" set "CHROME=%LOCALAPPDATA%\ms-playwright\%%d\chrome-win\chrome.exe"
)
if not defined CHROME if exist "%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe" set "CHROME=%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe"
if not defined CHROME if exist "%ProgramFiles%\Google\Chrome\Application\chrome.exe" set "CHROME=%ProgramFiles%\Google\Chrome\Application\chrome.exe"
if not defined CHROME (
  echo [ERROR] no Chromium found. Install one:  agent-browser install
  goto :fail
)
echo [OK]    chromium: %CHROME%

if not exist "%SHOT%" mkdir "%SHOT%"
del /q "%FRAME%" >nul 2>&1

pushd "%ROOT%"

REM ---- (1) confirm the agent sees a hand, browser not started yet ----------
echo.
echo [1/4] Waiting up to %WAIT_HAND%s for a hand - raise one at the camera now ...
"%PY%" -u scripts\verify_hand_overlay_live.py --host %HOST% --camera %CA% --no-browser --wait-hand %WAIT_HAND%
if errorlevel 1 (
  echo [ABORT] no hand was seen - nothing to verify
  popd
  goto :fail
)

REM ---- (2) read the JS out of the .py so it has a single source ------------
REM Dump it to a file, then read the FILE with `for /f`. Two dead ends worth
REM recording: embedding the python one-liner in a `for /f` backquote fails with
REM "The syntax of the command is incorrect" (nested double quotes inside a
REM backquoted command are not parsed the way you would expect), and `set /p`
REM is no good either because it truncates at 1023 characters - EVAL_JS is
REM longer than that.
"%PY%" -c "import sys; sys.path.insert(0,'scripts'); import verify_hand_overlay_live as m; sys.stdout.write(m.EVAL_JS)" > "%SHOT%\eval.js"
set "JS="
for /f "usebackq delims=" %%j in ("%SHOT%\eval.js") do set "JS=%%j"
if not defined JS (
  echo [ERROR] could not extract EVAL_JS from the python module
  popd
  goto :fail
)

REM ---- (3) fast browser session --------------------------------------------
REM EVERY agent-browser call needs `call`. agent-browser is a .cmd shim, and
REM invoking one batch file from another WITHOUT `call` does not return: the
REM child REPLACES the parent. The failure mode is nasty - this script simply
REM stopped right after "opening the camera page", skipped steps 3 and 4, and
REM exited 0, i.e. reported success for work it never did.
echo.
echo [2/4] opening the camera page ...
call "%AB%" open "http://%HOST%/camera/%CA%" --executable-path "%CHROME%"
call "%AB%" wait --load load >nul 2>&1
echo [3/4] letting the page poll a few frames ...
REM The page repaints on a 200 ms metadata poll; give it a couple of seconds on
REM top of the load so the canvas is guaranteed to hold a hand frame.
%SystemRoot%\System32\timeout.exe /t 9 >nul 2>&1

call "%AB%" eval "%JS%" > "%DOM%"
set "EVAL_RC=!errorlevel!"
echo        eval rc=!EVAL_RC!
for %%s in ("%DOM%") do echo        dom: %%~zs bytes

REM The frame the canvas painted from, read straight after the paint. Passing
REM it to the assert step is what makes the comparison honest: the operator
REM lowers their arm the moment they think they are done, and reading live then
REM would report "no hand" and hide the result.
REM Callers that pin VF_FRAME_JSON keep their own frame - see step 4.
if not defined VF_FRAME_JSON "%PY%" -c "import urllib.request; op=urllib.request.build_opener(urllib.request.ProxyHandler({})); open(r'%FRAME%','wb').write(op.open('http://%HOST%/api/cameras/%CA%/metadata', timeout=6).read())"

call "%AB%" screenshot "%SHOT%\hand-overlay-live.png"

REM ---- (4) assert -----------------------------------------------------------
echo.
echo [4/4] asserting painted pixels against the hand ...
set "VF_FROM_DOM=%DOM%"
REM Pre-setting VF_FRAME_JSON re-asserts a saved capture with no operator
REM present: that frame is used for the correlation and step 1 returns at once.
REM The verdict still reads the LIVE canvas, so a saved capture re-checks the
REM arithmetic rather than the drawing - a PASS still needs a real hand.
if not defined VF_FRAME_JSON set "VF_FRAME_JSON=%FRAME%"
"%PY%" -u scripts\verify_hand_overlay_live.py --host %HOST% --camera %CA%
set "RC=!errorlevel!"

echo.
echo Screenshot: %SHOT%\hand-overlay-live.png
echo.
if "!RC!"=="0" (
  echo [PASS] hand overlay verified against the live hand
  popd
  goto :done
)
echo [FAIL] overlay check failed - see the [FAIL] lines above
popd
goto :fail

:fail
echo.
if "%NO_PAUSE%"=="" pause
exit /b 1

:done
echo.
if "%NO_PAUSE%"=="" pause
exit /b 0
