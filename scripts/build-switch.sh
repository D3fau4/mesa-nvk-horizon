#!/usr/bin/env bash
# Cross-compiles libhorizon_gpu.a and the ten Phase 1 test .nros.
#
# Uses a local devkitA64 when $DEVKITPRO is set; otherwise falls back to
# a container image with the toolchain (default: the project's nx-dev
# image; override with HORIZON_NX_IMAGE). No machine-specific absolute
# paths (milestones.md Phase 2 gate).
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

IMAGE="${HORIZON_NX_IMAGE:-ghcr.io/d3fau4/nx-dev:latest}"

if [ -n "${DEVKITPRO:-}" ]; then
    exec make "$@"
fi

if ! command -v docker >/dev/null; then
    echo "error: neither \$DEVKITPRO nor docker is available" >&2
    exit 1
fi

# The tree is mounted at the *same* path inside the container as outside.
# Two reasons: it keeps a hardcoded container workdir out of this script
# (scripts/check-no-abs-paths.sh), and it makes compiler diagnostics and
# generated depfiles name paths that also resolve on the host.
exec docker run --rm -e DEVKITPRO=/opt/devkitpro \
    -v "$PWD":"$PWD" -w "$PWD" "$IMAGE" make "$@"
