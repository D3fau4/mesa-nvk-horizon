#!/usr/bin/env bash
# Send an .nro to a Switch sitting at hbmenu's NetLoader screen, and stream
# its nxlinkStdio() log back to this terminal.
#
# Requires: Docker Desktop, and the console already in NetLoader mode
# (hbmenu -> Y) so it prints "Waiting for nxlink to connect... IP Addr: X, Port: Y".
# Only the IP is needed here — nxlink always targets sys-botbase's fixed
# transfer port (28280); NetLoader's on-screen port is that same value.
#
# Usage: nxlink-send.sh <switch-ip> <path\to\file.nro> [-- args-for-nro...]
set -euo pipefail

if [ $# -lt 2 ]; then
  echo "Usage: $0 <switch-ip> <path\\to\\file.nro> [-- args-for-nro...]" >&2
  exit 1
fi

SWITCH_IP="$1"
NRO_PATH="$2"
shift 2

# Git Bash passes Windows paths through untouched, but Docker's -v needs
# forward slashes, and an absolute Unix-style path inside the *container*
# argument (/data/...) gets silently rewritten to "C:/Program Files/Git/data/..."
# by MSYS's path-conversion unless MSYS_NO_PATHCONV=1 is set (see below).
WIN_PATH=$(echo "$NRO_PATH" | sed 's#\\#/#g')
NRO_DIR=$(dirname "$WIN_PATH")
NRO_FILE=$(basename "$WIN_PATH")

echo "Sending ${NRO_FILE} to ${SWITCH_IP}, log server on :28771 ..." >&2

# -s        : start the log server after the upload completes (without this,
#             the console's reverse stdio connection has nothing listening and
#             nxlinkStdio() reports "connection failed (errno 116)").
# -p 28771  : publish the log-server port so the console (outside the
#             container's network namespace) can connect back into it.
MSYS_NO_PATHCONV=1 docker run --rm \
  -p 28771:28771/tcp \
  -v "${NRO_DIR}:/data" \
  ghcr.io/d3fau4/nx-dev:latest \
  nxlink -s -a "${SWITCH_IP}" "/data/${NRO_FILE}" "$@"
