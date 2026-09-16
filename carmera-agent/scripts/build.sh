#!/usr/bin/env bash
# Build Camera Agent (SIM backend by default; auto-detects GStreamer otherwise).
# Usage: ./scripts/build.sh [cmake-extra-args...]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAMERA_AGENT_BUILD_DIR:-$ROOT/build}"

cmake -S "$ROOT" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
  "$@"

cmake --build "$BUILD" --config "${CMAKE_BUILD_TYPE:-Release}"

echo
echo "Build OK."
echo "Run:   $BUILD/camera-agent --list"
echo "Or:    $BUILD/camera-agent --version"
