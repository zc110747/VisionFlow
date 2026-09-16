# Build Camera Agent with MSVC (Visual Studio generator).
# Run from a normal PowerShell. Requires:
#   - Visual Studio 2022 (MSVC) installed
#   - GStreamer 1.0 MSVC runtime+devel installed, and PKG_CONFIG_PATH pointing
#     at its lib\pkgconfig (so find_package(PkgConfig) can locate gstreamer-1.0).
param()

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Build = Join-Path $Root "build-msvc"

if (-not (Test-Path $Build)) { New-Item -ItemType Directory -Path $Build | Out-Null }

# Example: $env:PKG_CONFIG_PATH = "C:\gstreamer\1.0\msvc_x86_64\lib\pkgconfig"
cmake -S $Root -B $Build -G "Visual Studio 17 2022" -A x64
cmake --build $Build --config Release

Write-Host ""
Write-Host "Build OK."
Write-Host "Run: $Build\Release\camera-agent.exe --list"
