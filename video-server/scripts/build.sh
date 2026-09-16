#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

echo "[1/3] installing web dependencies"
( cd web && npm install --no-audit --no-fund )

echo "[2/3] building web ui"
( cd web && npm run build )

echo "[3/3] building video-server"
go build -o video-server ./cmd/video-server

echo "build complete: ./video-server"
