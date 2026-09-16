#!/usr/bin/env bash
# Open the video-server ports so other machines on the LAN can reach it.
#
# The server binds 0.0.0.0, so it already listens on every local address; a
# host firewall is what usually blocks LAN clients. This script opens the ports
# with ufw (Debian/Ubuntu) or firewalld (RHEL/Fedora/CentOS).
#
# Usage:
#   ./scripts/firewall-add.sh                      # ports from config/config.yaml
#   ./scripts/firewall-add.sh config/config.joint.yaml
#   ./scripts/firewall-add.sh 8081 8554 8889 8888  # explicit ports
#
# Undo: sudo ./scripts/firewall-remove.sh

set -u

CFG="config/config.yaml"
PORTS=""

if [ "$#" -gt 0 ]; then
  if [[ "$1" =~ ^[0-9]+$ ]]; then
    PORTS="$*"
  else
    CFG="$1"
  fi
fi

if [ -z "$PORTS" ]; then
  if [ ! -f "$CFG" ]; then
    echo "ERROR: config not found: $CFG" >&2
    exit 1
  fi
  HTTP=$(grep -E '^[[:space:]]*http_port:' "$CFG" | head -1 | awk -F: '{print $2}' | tr -d ' ')
  # The first bare "port:" is rtsp.port, the second is webrtc.port.
  RTSP=$(grep -E '^[[:space:]]{2}port:' "$CFG" | head -1 | awk -F: '{print $2}' | tr -d ' ')
  WEBRTC=$(grep -E '^[[:space:]]{2}port:' "$CFG" | sed -n '2p' | awk -F: '{print $2}' | tr -d ' ')
  HLS=$(grep -E '^[[:space:]]*hls_port:' "$CFG" | head -1 | awk -F: '{print $2}' | tr -d ' ')
  PORTS="${HTTP:-8080} ${RTSP:-8554} ${WEBRTC:-8889} ${HLS:-8888}"
fi

echo "ports: $PORTS"

if command -v ufw >/dev/null 2>&1; then
  for p in $PORTS; do
    sudo ufw allow "${p}/tcp" comment 'video-server' || exit 1
    echo "[OK] ufw tcp $p"
  done
  echo "Also allow mediamtx UDP media if clients use UDP transport:"
  echo "  sudo ufw allow from any to any app mediamtx   # or the explicit UDP ports"
elif command -v firewall-cmd >/dev/null 2>&1; then
  for p in $PORTS; do
    sudo firewall-cmd --permanent --add-port="${p}/tcp" || exit 1
    echo "[OK] firewalld tcp $p"
  done
  sudo firewall-cmd --reload
else
  echo "ERROR: neither ufw nor firewall-cmd found; open these ports manually: $PORTS" >&2
  exit 1
fi

echo
echo "Reachable addresses:"
ip -4 -o addr show scope global | awk '{print $2, $4}' | cut -d/ -f1
