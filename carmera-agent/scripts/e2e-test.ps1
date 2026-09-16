# End-to-end acceptance test for Camera Agent (real GStreamer backend).
#
#   start MediaMTX -> agent pushes -> ffmpeg pulls real frames
#   -> kill MediaMTX -> agent must survive and back off (1/2/5/10s)
#   -> restart MediaMTX -> agent must resume STREAMING on its own
#
# External tools (mediamtx, ffmpeg/ffplay, gstreamer) are expected on PATH.
# The project tool (camera-agent.exe) is located relative to this script.
# All generated logs / captured frames go under tests/finished/.
#
# Usage: .\scripts\e2e-test.ps1

param(
    # By default the camera runs at its own native format (--auto). Hardcoded
    # 240x240@8 came from one specific UVC device and breaks caps negotiation
    # on every other one. Pass -ForceCaps to pin a format on purpose.
    [switch] $ForceCaps,
    [int]    $Width   = 1280,
    [int]    $Height  = 720,
    [int]    $Fps     = 30,
    [string] $Stream  = 'camera01'
)

$ErrorActionPreference = 'Continue'

# Workaround: this host session can inject case-duplicate PATH variants
# (Path / PATH / path). PowerShell 5.1's Start-Process builds the child env
# block with a case-insensitive dictionary and throws "duplicate key" when it
# meets them. Merge every case-variant of PATH into one canonical 'Path' key
# before launching anything. This is a no-op in a clean terminal.
$pathVariants = @([System.Environment]::GetEnvironmentVariables().Keys) |
    Where-Object { ([string]$_).ToLowerInvariant() -eq 'path' }
if ($pathVariants.Count -gt 1) {
    $merged = @()
    foreach ($v in $pathVariants) {
        foreach ($p in ([string][System.Environment]::GetEnvironmentVariable($v) -split ';')) {
            if ($p -ne '') { $merged += $p }
        }
    }
    $merged = $merged | Select-Object -Unique
    # SetEnvironmentVariable key lookups are case-insensitive, so clearing any
    # one variant clears all of them; set the canonical key afterwards.
    foreach ($v in $pathVariants) {
        [System.Environment]::SetEnvironmentVariable([string]$v, $null, 'Process')
    }
    [System.Environment]::SetEnvironmentVariable('Path', ($merged -join ';'), 'Process')
}

# Project root = parent of the directory holding this script (scripts/).
$scriptDir = $PSScriptRoot
$root      = Split-Path $scriptDir -Parent
$finished  = Join-Path $root 'tests\finished'
if (-not (Test-Path $finished)) { New-Item -ItemType Directory -Path $finished | Out-Null }

# Project tool, referenced by relative path (portable across machines).
$exe      = Join-Path $root 'build-msvc\src\camera-agent.exe'
$agentLog = Join-Path $finished 'agent.log'
$mtxLog   = Join-Path $finished 'mediamtx.log'
$mtxErr   = Join-Path $finished 'mediamtx.err'
$ffLog    = Join-Path $finished 'ffmpeg.log'
$ffErr    = Join-Path $finished 'ffmpeg.err'
$capture  = Join-Path $finished ("capture-" + (Get-Date -Format 'HHmmss'))

if (-not (Test-Path $exe)) { throw "camera-agent.exe not found at $exe - build first (scripts\build-msvc.ps1 -Backend gstreamer)." }
if (-not (Test-Path $capture)) { New-Item -ItemType Directory -Path $capture | Out-Null }

# External tools are resolved from PATH; no absolute paths are baked in.
function Start-MediaMtx {
    Start-Process -FilePath 'mediamtx' -WorkingDirectory $root `
        -RedirectStandardOutput $mtxLog -RedirectStandardError $mtxErr `
        -NoNewWindow -PassThru
}

$R = @()

# ---- phase 1: server up, agent pushes ------------------------------------
Get-Process mediamtx, camera-agent -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600

$mtx = Start-MediaMtx
Start-Sleep -Seconds 2
$R += "[phase1] mediamtx pid=$($mtx.Id)"

if ($ForceCaps) {
    $agentArgs = @('--camera','0','--width',$Width,'--height',$Height,'--fps',$Fps)
} else {
    # --auto lets the camera pick its native format: one flag that works on
    # every device instead of a resolution that only fits one.
    $agentArgs = @('--camera','0','--auto')
}
$R += "[phase1] capture mode: $(if ($ForceCaps) { "$($Width)x$($Height)@$($Fps) (forced)" } else { 'auto (native)' })"

$agent = Start-Process -FilePath $exe -ArgumentList ($agentArgs + @(
    '--stream',$Stream,'--log-level','debug','--duration','300')) `
    -RedirectStandardOutput $agentLog -RedirectStandardError "$agentLog.err" `
    -NoNewWindow -PassThru
Start-Sleep -Seconds 12
$R += "[phase1] camera-agent pid=$($agent.Id) alive=$(-not $agent.HasExited)"

# ---- phase 2: pull real frames with ffmpeg -------------------------------
$p = Start-Process -FilePath 'ffmpeg.exe' -ArgumentList `
     '-rtsp_transport','tcp','-i',"rtsp://127.0.0.1:8554/$Stream",
     '-frames:v','3','-y',(Join-Path $capture 'frame_%03d.jpg') `
     -RedirectStandardOutput $ffLog -RedirectStandardError $ffErr -NoNewWindow -PassThru
$ffOk = $p.WaitForExit(25000)
if (-not $ffOk) { Stop-Process -Id $p.Id -Force }
$jpgs = Get-ChildItem $capture -Filter '*.jpg' -ErrorAction SilentlyContinue
$R += "[phase2] ffmpeg frames captured: $($jpgs.Count)"
foreach ($j in $jpgs) { $R += "[phase2]   $($j.Name)  $($j.Length) bytes" }

# ---- phase 3: kill the server, agent must survive ------------------------
Stop-Process -Id $mtx.Id -Force
$R += "[phase3] mediamtx killed at $(Get-Date -Format HH:mm:ss)"
Start-Sleep -Seconds 20
$recon = @()
if (Test-Path $agentLog) {
    $recon = Select-String -Path $agentLog -Pattern 'Reconnecting in' | ForEach-Object { $_.Line }
}
$R += "[phase3] agent alive=$(-not $agent.HasExited)"
$R += "[phase3] backoff attempts seen: $($recon.Count)"
foreach ($l in $recon) { $R += "[phase3]   $l" }

# ---- phase 4: restart server, agent must resume --------------------------
$restartT = Get-Date
$mtx2 = Start-MediaMtx
Start-Sleep -Seconds 30
# Auto-resume is proven from MediaMTX's own log: when the agent reconnects,
# MediaMTX prints "stream is available and online" with a timestamp later than
# the restart moment. MediaMTX flushes its log line-by-line, so unlike the
# agent's buffered stdout this is safe to read mid-run. (The agent's own
# "reconnected after N attempts" line is also delayed by the kStableGraceSec
# backoff grace, so we do not key off it.)
$resumed = $false
if (Test-Path $mtxLog) {
    $online = Select-String -Path $mtxLog -Pattern 'stream is available and online' | ForEach-Object { $_.Line }
    foreach ($l in $online) {
        if ($l -match '(\d{4}/\d{2}/\d{2} (\d{2}:\d{2}:\d{2}))') {
            $ts = [datetime]::ParseExact($Matches[1], 'yyyy/MM/dd HH:mm:ss', $null)
            if ($ts -ge $restartT) { $resumed = $true; break }
        }
    }
}
$R += "[phase4] mediamtx restarted pid=$($mtx2.Id)"
$R += "[phase4] auto-resume detected: $resumed"

Start-Sleep -Seconds 5
$R += "[phase4] agent alive=$(-not $agent.HasExited)"

# ---- teardown ------------------------------------------------------------
if (-not $agent.HasExited) { Stop-Process -Id $agent.Id -Force }
Get-Process mediamtx -ErrorAction SilentlyContinue | Stop-Process -Force
# Let the killed agent's buffered stdout flush into agent.log before we read it.
Start-Sleep -Seconds 2
$R += "[done] processes stopped"

$R += ""
$R += "=== agent.log (tail 60) ==="
$R += (Get-Content $agentLog -Tail 60 -ErrorAction SilentlyContinue | Out-String)
$R += "=== mediamtx.log (tail 25) ==="
$R += (Get-Content $mtxLog -Tail 25 -ErrorAction SilentlyContinue | Out-String)
$R += "=== ffmpeg stderr (tail 20) ==="
$R += (Get-Content $ffErr -Tail 20 -ErrorAction SilentlyContinue | Out-String)

Set-Content -Path (Join-Path $finished 'e2e.log') -Value $R -Encoding UTF8
