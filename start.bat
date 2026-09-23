@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ============================================================================
REM  VisionFlow root launcher - one entry point for the whole joint run.
REM
REM  Same behaviour as video-server\scripts\start-joint-ai.bat, but the paths
REM  are resolved from THIS file's location (the repository root), so it can be
REM  run - or double-clicked - from the top of the tree:
REM
REM    VisionFlow\start.bat                 (this file)
REM    VisionFlow\video-server\             (server + embedded Web UI)
REM    VisionFlow\carmera-agent\            (GStreamer camera agent)
REM
REM  Topology (one MediaMTX only - video-server launches it):
REM
REM    UVC camera -> camera-agent.exe (GStreamer) --RTSP push--> MediaMTX
REM              -> video-server.exe monitor auto-registers the path
REM              -> Web UI draws AI bounding boxes over the video
REM
REM  AI + metadata are ON by default: the agent runs person detection and pushes
REM  results to the server, which stores them and serves them at
REM  GET /api/cameras/{id}/metadata. The Web UI (VideoPlayer.vue) polls that
REM  endpoint and paints bounding boxes on a canvas overlay.
REM  Use --no-ai / --no-metadata to fall back to a plain stream.
REM
REM  External tools (go / node / npm / curl / mediamtx / GStreamer) are called by
REM  bare name and must be on PATH. Only the two project binaries are referenced
REM  relative to this script.
REM
REM  Environment overrides (argument > env var > default):
REM    CAMERA_ID       camera index, see: camera-agent --list   (default: auto)
REM    STREAM_ID       RTSP path name                           (default camera01)
REM    CAMERA_SOURCE   force a GStreamer source element         (default: auto)
REM    NO_BROWSER      1 = do not open the browser              (default 0)
REM    ENABLE_AI       1 = run person detection in the agent     (default 1)
REM    ENABLE_METADATA 1 = push AI results to the server         (default 1)
REM    AI_FPS          AI inference rate, range 5-12             (default 8)
REM    AI_MODE         AI mode: detect|pose|hand                (default detect)
REM    METADATA_URL    full ingest URL                           (default below)
REM    VS_CONFIG       server config file name inside config\    (default config.joint.yaml)
REM    NO_PAUSE        1 = do not wait for a keypress at the end (default 0)
REM    PROBE           1 = run the API smoke test after startup    (default 0)
REM
REM  Examples:
REM    start.bat
REM    start.bat --camera 0 --stream cam01
REM    start.bat --no-browser
REM    start.bat --no-ai                 (plain stream, no boxes)
REM    start.bat --ai-fps 10             (10 inferences/sec)
REM    start.bat --ai-mode hand          (hand 3D landmarks + gesture, no server UI needed)
REM    start.bat --source videotestsrc   (no physical camera needed)
REM    start.bat --probe                 (also self-test the running stack)
REM
REM  NOTE: --auto is always passed to the agent. The UVC camera here only does
REM        240x240@8fps natively, so forcing the 1280x720 from camera-agent.yaml
REM        makes caps negotiation fail and the agent loops on reconnect forever.
REM
REM  NOTE: a physical UVC camera is required for real video. With no camera
REM        attached, pass --source videotestsrc (synthetic stream) or expect the
REM        agent to keep retrying and %STREAM_ID% never to register.
REM ============================================================================

REM ---- paths: this script lives in the repository root ----
set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"
for %%i in ("%REPO%") do set "REPO=%%~fi"
set "ROOT=%REPO%\video-server"
set "AGENT_DIR=%REPO%\carmera-agent"
set LOGS=%ROOT%\logs
if not exist "%LOGS%" mkdir "%LOGS%"

REM Use the joint config by default: it moves HTTP off 8080, which the
REM "ApplicationWebServer" service on this machine grabs, so the demo does not
REM randomly fail to bind. Override with: set VS_CONFIG=config.yaml
set CFG_NAME=config.joint.yaml
if defined VS_CONFIG set CFG_NAME=%VS_CONFIG%
set CFG=%ROOT%\config\%CFG_NAME%
REM Pick the NEWEST video-server*.exe: build.bat writes video-server.new.exe
REM when the canonical name is locked by a running instance, and running a
REM stale binary is the last thing you want while debugging playback.
REM The pick lives in :pick_exe because it runs twice - once here, once again
REM after the optional rebuild below.
call :pick_exe
REM ---- pick the camera-agent binary: prefer the GStreamer (real-camera) build --
REM Only the GStreamer backend publishes a real RTSP stream that MediaMTX / WebRTC
REM can play. The SIM build is a headless simulator that NEVER opens a network
REM connection, so launching it here yields "no stream is available on path ...".
set CA_EXE=
set "CA_GST_DIR=%AGENT_DIR%\build-msvc-gst"
set "CA_SIM_DIR=%AGENT_DIR%\build-msvc"
if exist "%CA_GST_DIR%\src\camera-agent.exe" (
  set "CA_EXE=%CA_GST_DIR%\src\camera-agent.exe"
) else if exist "%CA_SIM_DIR%\src\camera-agent.exe" (
  set "CA_EXE=%CA_SIM_DIR%\src\camera-agent.exe"
)
REM backend marker (written by scripts\build-msvc.ps1) drives the SIM warning
set "CA_BACKEND=unknown"
if exist "%CA_GST_DIR%\backend.txt" ( set /p CA_BACKEND=<"%CA_GST_DIR%\backend.txt" )
if exist "%CA_SIM_DIR%\backend.txt" ( set /p CA_BACKEND=<"%CA_SIM_DIR%\backend.txt" )
if /i "%CA_BACKEND%"=="sim" (
  echo.
  echo [WARN]  camera-agent.exe is a SIM build - it does NOT publish an RTSP stream.
  echo         WebRTC will report: no stream is available on path '%STREAM_ID%'.
  echo         For real video + AI tracking boxes, build the GStreamer backend:
  echo           cd %AGENT_DIR% ^&^& scripts\build-msvc.ps1 -Backend gstreamer
  echo         then re-run this script. (Continuing so you can see the server UI.)
  echo.
)

echo ============================================================
echo  VisionFlow joint run (AI): video-server + carmera-agent --ai --metadata
echo  repo   root: %REPO%
echo  server root: %ROOT%
echo  agent  root: %AGENT_DIR%
echo ============================================================

REM ---- defaults ----
if not defined STREAM_ID      set STREAM_ID=camera01
if not defined NO_BROWSER     set NO_BROWSER=0
if not defined NO_PAUSE       set NO_PAUSE=0
if not defined PROBE          set PROBE=0
REM AI + metadata ON by default: the whole point of this script is the boxes.
if not defined ENABLE_AI      set ENABLE_AI=1
if not defined ENABLE_METADATA set ENABLE_METADATA=1
if not defined AI_FPS         set AI_FPS=8
set "CAMERA_ARG="
set "SOURCE_ARG="
set "AI_ARG="

REM ---- parse args ----
set "_ctx="
for %%a in (%*) do (
  if defined _ctx (
    if "!_ctx!"=="camera" set "CAMERA_ID=%%~a"
    if "!_ctx!"=="stream" set "STREAM_ID=%%~a"
    if "!_ctx!"=="source" set "CAMERA_SOURCE=%%~a"
    if "!_ctx!"=="aifps"  set "AI_FPS=%%~a"
    if "!_ctx!"=="aimode" set "AI_MODE=%%~a"
    set "_ctx="
  ) else (
    if /i "%%a"=="--no-browser"   set NO_BROWSER=1
    if /i "%%a"=="--no-pause"     set NO_PAUSE=1
    if /i "%%a"=="--probe"        set PROBE=1
    if /i "%%a"=="--camera"       set "_ctx=camera"
    if /i "%%a"=="--stream"       set "_ctx=stream"
    if /i "%%a"=="--source"       set "_ctx=source"
    if /i "%%a"=="--ai-fps"       set "_ctx=aifps"
    if /i "%%a"=="--ai-mode"      set "_ctx=aimode"
    if /i "%%a"=="--ai"           set ENABLE_AI=1
    if /i "%%a"=="--no-ai"        set ENABLE_AI=0
    if /i "%%a"=="--metadata"     set ENABLE_METADATA=1
    if /i "%%a"=="--no-metadata"  set ENABLE_METADATA=0
  )
)
if defined CAMERA_ID   set "CAMERA_ARG=--camera %CAMERA_ID%"
if defined CAMERA_SOURCE set "SOURCE_ARG=--source %CAMERA_SOURCE%"

REM ---- (1) stop leftovers from a previous run --------------------------
REM FIRST, before anything may write video-server.exe: Windows holds an
REM exclusive handle on a running .exe, so a rebuild would fail with "Access
REM is denied" and silently leave the stale binary in place.
echo [*] Stopping leftovers: video-server / mediamtx / camera-agent
taskkill /F /T /IM video-server.exe     >nul 2>&1
taskkill /F /T /IM video-server.new.exe >nul 2>&1
taskkill /F /IM camera-agent.exe >nul 2>&1
taskkill /F /IM mediamtx.exe    >nul 2>&1
C:\Windows\System32\timeout.exe /t 2 >nul 2>&1
echo [OK]    stopped

REM ---- (2) locate go + rebuild web UI and server when sources are newer ----
REM The box overlay lives in the embedded web UI (web/dist), which go:embed
REM captures at COMPILE TIME. So a stale video-server.exe would serve the old
REM UI even though web/src changed. We rebuild BOTH when either the Go sources
REM or web/src are newer than the binary / dist.
REM
REM Non-fatal by design: if go/node are missing or the tree does not compile,
REM keep the existing binary and say so instead of refusing to start.
set "GO_BIN=go"
where go >nul 2>nul
if errorlevel 1 (
  if exist "%ROOT%\.toolchain\go\go\bin\go.exe" (
    set "GO_BIN=%ROOT%\.toolchain\go\go\bin\go.exe"
    echo using bundled go: %GO_BIN%
  ) else (
    echo [WARN]  go not found - skipping rebuild, using existing binary
    set "GO_BIN="
  )
)

set "REBUILD="
if defined GO_BIN (
  if not exist "%VS_EXE%" (
    set "REBUILD=STALE"
  ) else (
    for /f "usebackq tokens=*" %%i in (`powershell -NoProfile -Command "$e = Get-Item -LiteralPath '%VS_EXE%'; $g = @(Get-ChildItem -LiteralPath '%ROOT%\cmd', '%ROOT%\internal' -Recurse -Filter '*.go'); $m = $e.LastWriteTime; foreach ($f in $g) { if ($f.LastWriteTime -gt $m) { $m = $f.LastWriteTime } }; if ($m -gt $e.LastWriteTime) { 'STALE' }" 2^>nul`) do set "REBUILD=%%i"
    if "%REBUILD%"=="" (
      if not exist "%ROOT%\web\dist\index.html" set "REBUILD=STALE"
    )
    REM web/dist is embedded into the exe at COMPILE time, so a dist NEWER
    REM than the binary means the running UI is stale even when no Go file
    REM changed. Without this, a frontend-only change that was already built -
    REM by hand, by npm run build, or by an agent - starts the OLD web UI and
    REM the new code looks like it silently did nothing.
    REM NOTE: no parentheses in these comments. Inside a parenthesised block a
    REM ")" closes the block early, swallowing the rest of the branch.
    if "%REBUILD%"=="" (
      for /f "usebackq tokens=*" %%i in (`powershell -NoProfile -Command "if ((Get-Item -LiteralPath '%ROOT%\web\dist\index.html').LastWriteTime -gt (Get-Item -LiteralPath '%VS_EXE%').LastWriteTime) { 'STALE' }" 2^>nul`) do set "REBUILD=%%i"
    )
    if "%REBUILD%"=="" (
      for /f "usebackq tokens=*" %%i in (`powershell -NoProfile -Command "$d = Get-Item -LiteralPath '%ROOT%\web\dist\index.html'; $g = @(Get-ChildItem -LiteralPath '%ROOT%\web\src' -Recurse); $m = $d.LastWriteTime; foreach ($f in $g) { if ($f.LastWriteTime -gt $m) { $m = $f.LastWriteTime } }; if ($m -gt $d.LastWriteTime) { 'STALE' }" 2^>nul`) do set "REBUILD=%%i"
    )
  )
)

if "%REBUILD%"=="STALE" (
  echo [BUILD]  sources newer than %VS_NAME% - rebuilding web UI + server ...
  pushd "%ROOT%"
  where node >nul 2>nul
  if errorlevel 1 (
    echo [WARN]  node missing - cannot rebuild web UI; using existing dist
  ) else (
    pushd web
    if not exist node_modules call npm install --no-audit --no-fund
    call npm run build
    if errorlevel 1 (
      echo [WARN]  web build failed - continuing with existing dist
    ) else (
      echo [OK]    web UI built
    )
    popd
  )
  if defined GO_BIN (
    "%GO_BIN%" build -trimpath -o "%ROOT%\video-server.exe" ./cmd/video-server
    set "VS_RC=!errorlevel!"
    if "!VS_RC!"=="0" (
      echo [OK]    rebuilt video-server.exe
    ) else (
      "%GO_BIN%" build -trimpath -o "%ROOT%\video-server.new.exe" ./cmd/video-server
      if not errorlevel 1 (
        echo [OK]    rebuilt as video-server.new.exe
      ) else (
        echo [WARN]  server build failed - continuing with existing binary
      )
    )
  )
  popd
  call :pick_exe
)

REM ---- (3) pre-check project binaries ----
if not exist "%VS_EXE%" (
  echo [ERROR] video-server.exe not found: %VS_EXE%
  echo         Build it: video-server\scripts\build.bat
  goto :fail
)
echo [OK]    video-server.exe  binary=%VS_NAME%

if not exist "%CA_EXE%" (
  echo [ERROR] camera-agent.exe not found: %CA_EXE%
  echo         Build it: cd %AGENT_DIR% ^&^& build_oneclick.bat
  goto :fail
)
echo [OK]    camera-agent.exe

if not exist "%CFG%" (
  echo [ERROR] server config not found: %CFG%
  goto :fail
)
echo [OK]    config\%CFG_NAME%

REM ---- (4) stop any *other* agent-side MediaMTX -------------------------
REM ONE MediaMTX ONLY: video-server launches the instance whose control API the
REM monitor polls. A leftover camera-agent\mediamtx.yml copy would fight over
REM :8554. The step (1) kill already covers this; the check below only reports.
set "MTX_N="
for /f %%c in ('tasklist /FI "IMAGENAME eq mediamtx.exe" 2^>nul ^| find /c /i "mediamtx.exe"') do set "MTX_N=%%c"
if not "%MTX_N%"=="0" echo [WARN]  %MTX_N% mediamtx.exe still alive before start

REM ---- (5) read ports out of the server config -------------------------
set HTTP_PORT=8080
set RTSP_PORT=8554
for /f "usebackq tokens=1,2 delims=:" %%a in (`findstr /r /c:"http_port:" "%CFG%"`) do (
  for /f "tokens=1" %%v in ("%%b") do set "HTTP_PORT=%%v"
)
for /f "usebackq tokens=1,2 delims=:" %%a in (`findstr /r /c:"^  port:" "%CFG%"`) do (
  if "!RTSP_PORT_SET!"=="" (
    for /f "tokens=1" %%v in ("%%b") do set "RTSP_PORT=%%v"
    set RTSP_PORT_SET=1
  )
)
echo [OK]    ports: http=%HTTP_PORT% rtsp=%RTSP_PORT%

REM ---- (6) start video-server (it spawns MediaMTX) ---------------------
echo [1/3] Starting video-server ...
start "video-server" /D "%ROOT%" cmd /c "video-server.exe config\%CFG_NAME% > "%LOGS%\video-server.log" 2>&1"

set READY=0
for /l %%i in (1,1,40) do (
  if "!READY!"=="0" (
    C:\Windows\System32\timeout.exe /t 1 >nul 2>&1
    curl -s -o nul -w "%%{http_code}" http://localhost:%HTTP_PORT%/api/health 2>nul | findstr "200" >nul && set READY=1
  )
)
if "%READY%"=="0" (
  echo [ERROR] video-server did not become ready - see %LOGS%\video-server.log
  goto :fail
)
echo [OK]    video-server healthy on :%HTTP_PORT%

REM ---- (7) pick a camera index that actually exists --------------------
if not defined CAMERA_ID (
  "%CA_EXE%" --list > "%LOGS%\cameras.txt" 2>nul
  set "AVAIL_IDS="
  if exist "%LOGS%\cameras.txt" (
    for /f "tokens=2" %%i in ('findstr /b /c:"Camera " "%LOGS%\cameras.txt"') do set "AVAIL_IDS=!AVAIL_IDS! %%i"
    del "%LOGS%\cameras.txt" >nul 2>&1
  )
  if defined AVAIL_IDS (
    for /f "tokens=1" %%i in ("!AVAIL_IDS!") do set "CAMERA_ID=%%i"
    echo [AUTO]   camera index auto-picked: !CAMERA_ID! - available:!AVAIL_IDS!
  ) else (
    set CAMERA_ID=0
    echo [note]   could not enumerate cameras - falling back to index 0
  )
  set "CAMERA_ARG=--camera !CAMERA_ID!"
)

REM ---- (8) start camera-agent, pushing at the SAME MediaMTX ------------
REM AI / metadata flags are appended only when enabled. In this script they are
REM ON by default, so the browser overlay has frames to draw.
if "%ENABLE_AI%"=="1" set "AI_ARG=--ai --ai-fps %AI_FPS%"
REM Delayed expansion (!AI_ARG!) matters here: the mode and the metadata flags are
REM appended in sequence and %AI_ARG% would be frozen at parse time.
if defined AI_MODE if "%ENABLE_AI%"=="1" set "AI_ARG=!AI_ARG! --ai-mode !AI_MODE!"
if "%ENABLE_METADATA%"=="1" (
  if not defined METADATA_URL set "METADATA_URL=http://127.0.0.1:%HTTP_PORT%/api/metadata"
  REM Delayed expansion on purpose: %AI_ARG% inside a parenthesised block
  REM would be frozen at parse time, before the --ai line above ran.
  set "AI_ARG=!AI_ARG! --metadata --metadata-url !METADATA_URL! --metadata-camera-id %STREAM_ID%"
)
echo [2/3] Starting camera-agent --auto --stream %STREAM_ID% ...
set "AGENT_ARGS=%CAMERA_ARG% --stream %STREAM_ID% --server 127.0.0.1 --port %RTSP_PORT% --auto %SOURCE_ARG% %AI_ARG%"
echo        args: %AGENT_ARGS%
start "camera-agent" /D "%AGENT_DIR%" cmd /c ""%CA_EXE%" %AGENT_ARGS% --log-level info > "%LOGS%\agent.log" 2>&1"

REM ---- (9) wait for the server to auto-register the stream -------------
echo [3/3] Waiting for video-server to auto-register %STREAM_ID% ...
set CONN=0
for /l %%i in (1,1,30) do (
  if "!CONN!"=="0" (
    C:\Windows\System32\timeout.exe /t 2 >nul 2>&1
    curl -s http://localhost:%HTTP_PORT%/api/cameras 2>nul | findstr /i /c:"%STREAM_ID%" >nul && set CONN=1
  )
)
if "%CONN%"=="1" (
  echo [OK]    %STREAM_ID% registered
) else (
  echo [WARN]  %STREAM_ID% not registered within 60s - see %LOGS%\agent.log
)

REM ---- (9b) optional API smoke test (--probe) --------------------------
REM Same curl-based approach as the readiness checks above, so it needs neither
REM python nor any extra tooling. The heavy end-to-end acceptance - real camera,
REM real RTSP pull, stream resilience - still lives in scripts\verify_joint.py.
REM It runs as a subroutine so the batch parser never sees a nested if-block
REM whose echoed text contains a ")" (that silently prints BOTH branches).
set "PROBE_FAIL=0"
if "%PROBE%"=="1" call :probe

REM ---- (10) open the Web UI --------------------------------------------
if "%NO_BROWSER%"=="0" start "" http://localhost:%HTTP_PORT%/

REM ---- summary ---------------------------------------------------------
set "LAN_IP="
for /f "usebackq tokens=*" %%i in (`powershell -NoProfile -Command "$r = Invoke-RestMethod -Uri 'http://localhost:%HTTP_PORT%/api/net/addresses'; $r.public_host" 2^>nul`) do set "LAN_IP=%%i"

set "SHOW_LAN=0"
if defined LAN_IP if not "%LAN_IP%"=="127.0.0.1" set SHOW_LAN=1

echo.
echo ============================================================
echo  VisionFlow joint run (AI) started
echo  Web UI      : http://localhost:%HTTP_PORT%/
echo  REST API    : http://localhost:%HTTP_PORT%/api/cameras
echo  RTSP stream : rtsp://127.0.0.1:%RTSP_PORT%/%STREAM_ID%
if "%SHOW_LAN%"=="1" (
  echo  --- from another machine on the LAN - source: GET /api/net/addresses ---
  echo  Web UI      : http://%LAN_IP%:%HTTP_PORT%/
  echo  RTSP stream : rtsp://%LAN_IP%:%RTSP_PORT%/%STREAM_ID%
  echo  If it times out, open the firewall first:
  echo     video-server\scripts\firewall-add.bat %HTTP_PORT% %RTSP_PORT%
)
echo  Camera      : index %CAMERA_ID%  - negotiated natively via --auto
REM Single-line ifs on purpose: a ")" inside an echoed string would CLOSE a
REM parenthesised if-block early and print BOTH branches (and eat the bracket).
REM Outside a block the same characters are literal, so no escaping is needed.
if "%ENABLE_AI%"=="1" echo  AI          : person detection ON - inference rate %AI_FPS% fps (range 5-12)
if not "%ENABLE_AI%"=="1" echo  AI          : OFF (--no-ai) - plain stream, no boxes
if "%ENABLE_METADATA%"=="1" echo  Metadata    : !METADATA_URL!
if "%ENABLE_METADATA%"=="1" echo                read back: GET http://localhost:%HTTP_PORT%/api/cameras/%STREAM_ID%/metadata
if "%ENABLE_METADATA%"=="1" echo  Box overlay : VideoPlayer.vue polls the metadata above and draws boxes
if "%ENABLE_METADATA%"=="1" echo                on a canvas over the video (label = class + confidence pct)
if not "%ENABLE_METADATA%"=="1" echo  Metadata    : OFF (--no-metadata) - the Web UI has no boxes to draw
echo  Logs        : %LOGS%\  (video-server.log / agent.log)
echo  Verify      : python video-server\scripts\verify_joint.py --stream %STREAM_ID%
echo  Stop        : video-server\scripts\stop-joint.bat
if "%PROBE%"=="1" if "%PROBE_FAIL%"=="0" echo  Probe       : all checks passed
if "%PROBE%"=="1" if not "%PROBE_FAIL%"=="0" echo  Probe       : FAILED - see the [FAIL] lines above
echo ============================================================
echo.
goto :done

:fail
echo.
echo [ABORTED] Fix the error above, then run this script again.
echo.
if "%NO_PAUSE%"=="0" pause
exit /b 1

:done
echo [DONE]    Services are running in separate windows; this launcher has finished.
echo.
if "%NO_PAUSE%"=="0" pause
exit /b 0

REM ============================================================================
REM  :pick_exe - set VS_NAME / VS_EXE to the freshest video-server*.exe
REM ============================================================================
:pick_exe
set "VS_NAME="
for /f "delims=" %%i in ('dir /b /o-d "%ROOT%\video-server*.exe" 2^>nul') do (
  if not defined VS_NAME set "VS_NAME=%%i"
)
if not defined VS_NAME set "VS_NAME=video-server.exe"
set VS_EXE=%ROOT%\%VS_NAME%
exit /b 0

REM ============================================================================
REM  :probe - opt-in API smoke test (--probe). Prints PASS/FAIL per endpoint and
REM           counts failures in PROBE_FAIL so the summary can report the total.
REM           Needs only curl and findstr, both already required above.
REM
REM  The HTTP status is captured with `curl -w "%%{http_code}"` redirected to a
REM  file - the same construct the readiness loop uses above. Do NOT move that
REM  format string into a `for /f` backquote: %%{ there is not a for-variable
REM  and curl would receive a mangled -w template.
REM ============================================================================
:probe
set "TB=%LOGS%\.probe_body.txt"
set "LCODE=%LOGS%\.probe_code.txt"
set "REQ=%LOGS%\.probe_req.json"
echo.
echo [PROBE] API smoke test on http://localhost:%HTTP_PORT%/

call :code "http://localhost:%HTTP_PORT%/api/health"
call :pok "GET /api/health" 200
call :has "GET /api/health reports ok" "media_server"

call :code "http://localhost:%HTTP_PORT%/api/cameras"
call :pok "GET /api/cameras" 200
call :has "GET /api/cameras lists %STREAM_ID%" "%STREAM_ID%"

call :code "http://localhost:%HTTP_PORT%/api/cameras/%STREAM_ID%/metadata"
call :pok "GET /api/cameras/%STREAM_ID%/metadata" 200
call :has "metadata carries a frame" "frame_id"

call :code "http://localhost:%HTTP_PORT%/api/cameras/%STREAM_ID%/aimode"
call :pok "GET /api/cameras/%STREAM_ID%/aimode" 200
call :has "aimode answers with a mode" "mode"

call :code "http://localhost:%HTTP_PORT%/"
call :pok "GET / serves the Web UI" 200
call :has "Web UI html is embedded" "app"

REM Mode-switch round trip, then restore the default so the stored UI state is
REM the same as a fresh install would have.
> "%REQ%" echo {"mode":"ai-y-pose"}
call :code_post "http://localhost:%HTTP_PORT%/api/cameras/%STREAM_ID%/aimode"
call :pok "POST aimode ai-y-pose" 200
> "%REQ%" echo {"mode":"ai-y"}
call :code_post "http://localhost:%HTTP_PORT%/api/cameras/%STREAM_ID%/aimode"
call :pok "POST aimode ai-y (restore)" 200

REM Negative control: an unsupported mode must be rejected, not silently stored.
> "%REQ%" echo {"mode":"ai-bogus"}
call :code_post "http://localhost:%HTTP_PORT%/api/cameras/%STREAM_ID%/aimode"
call :pok "POST aimode rejects ai-bogus" 400

del "%REQ%" >nul 2>&1
del "%TB%" >nul 2>&1
del "%LCODE%" >nul 2>&1
if "%PROBE_FAIL%"=="0" echo [PROBE] all checks passed
if not "%PROBE_FAIL%"=="0" echo [PROBE] %PROBE_FAIL% check(s) FAILED - see the [FAIL] lines above
exit /b 0

REM :code - GET %~1, body into %TB%, HTTP status into PCODE
:code
set "PCODE=000"
curl -s -o "%TB%" -w "%%{http_code}" "%~1" > "%LCODE%" 2>nul
if exist "%LCODE%" set /p PCODE=<"%LCODE%"
exit /b 0

REM :code_post - POST the JSON in %REQ% to %~1, body into %TB%, status into PCODE
:code_post
set "PCODE=000"
curl -s -o "%TB%" -w "%%{http_code}" -X POST -H "Content-Type: application/json" -d @"%REQ%" "%~1" > "%LCODE%" 2>nul
if exist "%LCODE%" set /p PCODE=<"%LCODE%"
exit /b 0

REM :pok - expect PCODE to equal %~2
:pok
if "%PCODE%"=="%~2" echo   [PASS] %~1
if not "%PCODE%"=="%~2" echo   [FAIL] %~1  (got %PCODE%, expected %~2)
if not "%PCODE%"=="%~2" set /a PROBE_FAIL+=1
exit /b 0

REM :has - expect the last response body to contain %~2
:has
findstr /c:"%~2" "%TB%" >nul 2>&1
if not errorlevel 1 echo   [PASS] %~1
if errorlevel 1 echo   [FAIL] %~1  (no "%~2" in the response)
if errorlevel 1 set /a PROBE_FAIL+=1
exit /b 0
