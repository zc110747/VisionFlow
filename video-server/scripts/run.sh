#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec ./video-server config/config.yaml
