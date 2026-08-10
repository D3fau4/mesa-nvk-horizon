#!/usr/bin/env bash
# Builds build/logcat.nro, the SD-card log reader (tools/logcat/).
#
#   scripts/build-logcat.sh [extra make args...]
#
# Goes through horizon_run rather than calling docker itself, so it uses
# the same toolchain resolution as every other build here: a local
# devkitA64 when $DEVKITPRO is set, otherwise the pinned container image
# (CLAUDE.md — devkitPro's package servers may be unreachable while
# ghcr.io is not). horizon_run is also where the Git-Bash mount fix
# lives, and duplicating that here is how it would drift.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

mkdir -p build

# DEVKITPRO reaches the container through horizon_run's own -e; this is
# for the local path, where the Makefile reads it from this environment.
horizon_run make -C tools/logcat "$@"
