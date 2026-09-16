# Build Camera Agent with MSVC (cl.exe) + Ninja.
#
# Why not the "Visual Studio 17 2022" generator: MSBuild.exe on this machine
# crashes with an access violation as soon as it builds a real project, so the
# VS generator cannot produce binaries. The Ninja generator drives cl.exe
# directly and does not touch MSBuild at all.
#
# Why PowerShell and not bash: MSYS2 bash hands a colon-separated PATH to
# MSYS2-built cmake, which forwards it to cmd.exe; cmd.exe then fails to find
# rc.exe / mt.exe and linking dies. Running from PowerShell keeps PATH in
# native Windows form end to end.
#
# Usage:
#   .\scripts\build-msvc.ps1                 # gstreamer backend (default, auto-detected)
#   .\scripts\build-msvc.ps1 -Backend sim    # SIM backend (headless, no camera, NO RTSP publish)
#   .\scripts\build-msvc.ps1 -Clean          # wipe the build dir first
#
# A GStreamer build is written to build-msvc-gst; a SIM build to build-msvc.
# The joint launch scripts (start-joint*.bat) prefer the GStreamer build because
# only it publishes a real RTSP stream that MediaMTX / WebRTC can play. Running a
# SIM binary with them yields "no stream is available on path 'camera01'".

param(
    [ValidateSet('gstreamer', 'sim', 'auto')]
    [string]$Backend  = 'auto',
    [string]$BuildDir = 'build-msvc',
    [switch]$Clean
)

# Convention: a GStreamer build goes to its own dir (build-msvc-gst) so the SIM
# build is not clobbered, and the joint launch scripts prefer it for the
# real-camera (AI) demo. Only redirect when the caller did not override -BuildDir.
if ($Backend -eq 'gstreamer' -and $BuildDir -eq 'build-msvc') {
    $BuildDir = 'build-msvc-gst'
}

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

# ---- PATHEXT ---------------------------------------------------------------
# Some non-interactive hosts (CI, sandboxes, scheduled tasks) start PowerShell
# with a stripped PATHEXT (observed: ".CPL" only). PowerShell uses PATHEXT to
# decide whether a file is an *application* or a *document*: with .EXE missing,
# `& some.exe | ...` fails with "Cannot run a document in the middle of a
# pipeline", and an un-piped call silently does nothing. That turns the whole
# build into a silent no-op, so restore the standard list before doing anything.
if ($env:PATHEXT -notmatch '\.EXE') {
    $env:PATHEXT = '.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC;.CPL'
}

# ---- Program Files root ---------------------------------------------------
# %ProgramFiles(x86)% is EMPTY in some non-interactive hosts (CI, sandboxes,
# scheduled tasks). Fall back to the well-known path instead of silently
# producing "\Microsoft Visual Studio\Installer\vswhere.exe".
$Pf86 = ${env:ProgramFiles(x86)}
if (-not $Pf86 -or -not (Test-Path $Pf86)) { $Pf86 = 'C:\Program Files (x86)' }
if (-not (Test-Path $Pf86)) { throw "Program Files (x86) not found (tried '$Pf86')." }

# ---- Locate the Visual Studio installation -------------------------------
$vswhere = Join-Path $Pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
$VsPath = $null
if (Test-Path $vswhere) {
    $VsPath = & $vswhere -latest -products * `
              -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
              -property installationPath 2>$null | Select-Object -First 1
}
if (-not $VsPath -or -not (Test-Path $VsPath)) { $VsPath = 'D:\Software\vs' }
if (-not (Test-Path $VsPath)) { throw "Visual Studio not found (looked in '$VsPath')." }

# ---- Newest MSVC toolset --------------------------------------------------
$MsvcBase = Join-Path $VsPath 'VC\Tools\MSVC'
if (-not (Test-Path $MsvcBase)) { throw "MSVC toolset not found under '$MsvcBase'." }
$Toolset = Get-ChildItem $MsvcBase -Directory |
           Sort-Object { [version]$_.Name } | Select-Object -Last 1
$Vc = Join-Path $MsvcBase $Toolset.Name
$Cl = Join-Path $Vc 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path $Cl)) { throw "cl.exe not found at '$Cl'." }

# ---- Newest Windows SDK ---------------------------------------------------
$Kits = Join-Path $Pf86 'Windows Kits\10'
if (-not (Test-Path $Kits)) { throw "Windows 10 SDK not found at '$Kits'." }
$Sdk = Get-ChildItem (Join-Path $Kits 'Include') -Directory |
       Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
       Sort-Object { [version]$_.Name } | Select-Object -Last 1
$SdkVer = $Sdk.Name

"MSVC  : $($Toolset.Name)"
"SDK   : $SdkVer"
"VS    : $VsPath"

# ---- Compiler environment (native Windows form) ---------------------------
$env:INCLUDE = @(
    (Join-Path $Vc 'include'),
    "$Kits\Include\$SdkVer\ucrt",
    "$Kits\Include\$SdkVer\um",
    "$Kits\Include\$SdkVer\shared",
    "$Kits\Include\$SdkVer\winrt",
    "$Kits\Include\$SdkVer\cppwinrt"
) -join ';'

$env:LIB = @(
    (Join-Path $Vc 'lib\x64'),
    "$Kits\Lib\$SdkVer\ucrt\x64",
    "$Kits\Lib\$SdkVer\um\x64"
) -join ';'

# CMake/Ninja bundled with VS are native Windows binaries - using them keeps
# PATH untouched by any MSYS layer. rc.exe/mt.exe come from the SDK bin dir.
$VsCMake = Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$env:PATH = @(
    (Join-Path $Vc 'bin\Hostx64\x64'),
    "$Kits\bin\$SdkVer\x64",
    "$VsCMake\CMake\bin",
    "$VsCMake\Ninja",
    $env:PATH
) -join ';'

# ---- Configure + build ----------------------------------------------------
$Build = Join-Path $Root $BuildDir
if ($Clean -and (Test-Path $Build)) { Remove-Item -Recurse -Force $Build }

$cmakeArgs = @(
    '-S', $Root,
    '-B', $Build,
    '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_CXX_COMPILER=$Cl",
    "-DCAMERA_AGENT_BACKEND=$Backend"
)

"--- configure ---"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)." }

# Emit a backend marker so the joint launch scripts can refuse a SIM build
# (SIM never publishes an RTSP stream -> WebRTC "no stream is available").
$cacheFile = Join-Path $Build 'CMakeCache.txt'
if (Test-Path $cacheFile) {
    $line = Select-String -Path $cacheFile -Pattern 'CAMERA_AGENT_BACKEND:STRING=(.+)' |
            Select-Object -First 1
    if ($line -and $line -match 'CAMERA_AGENT_BACKEND:STRING=(.+)') {
        Set-Content -Path (Join-Path $Build 'backend.txt') -Value $Matches[1].Trim()
        "Backend marker: $($Matches[1].Trim()) -> $(Join-Path $Build 'backend.txt')"
    }
}

"--- build ---"
& cmake --build $Build
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }

""
"Build OK: $Build"
"Next:"
"  & '$Build\src\camera-agent.exe' --list"
"  & ctest --test-dir '$Build' --output-on-failure"
