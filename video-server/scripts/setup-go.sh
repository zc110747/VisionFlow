#!/usr/bin/env bash
# Download and install a local Go toolchain into .toolchain/go (no system install needed).
set -euo pipefail
cd "$(dirname "$0")/.."

GO_VER="${GO_VER:-1.27.0}"
GO_DIR=".toolchain/go"
GO_TAR=".toolchain/go.tar.gz"

if [ -x "$GO_DIR/go/bin/go" ]; then
  echo "go already present at $GO_DIR/go/bin/go"
  exit 0
fi

mkdir -p .toolchain
ARCH="$(uname -m)"
case "$ARCH" in
  x86_64) GO_ARCH=amd64 ;;
  aarch64|arm64) GO_ARCH=arm64 ;;
  *) echo "unsupported arch $ARCH"; exit 1 ;;
esac

URL="https://golang.google.cn/dl/go${GO_VER}.linux-${GO_ARCH}.tar.gz"
echo "downloading go ${GO_VER} (linux/${GO_ARCH}) from ${URL}..."
curl -sSL -o "$GO_TAR" "$URL"

echo "extracting..."
tar -C "$GO_DIR" --strip-components=1 -xzf "$GO_TAR"
rm -f "$GO_TAR"
echo "done: $GO_DIR/go/bin/go"
