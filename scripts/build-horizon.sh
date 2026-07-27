#!/usr/bin/env bash
# Builds libhorizon_gpu.a and the ten Phase 1 test .nros through Meson.
#
#   scripts/build-horizon.sh [extra meson compile args...]
#
# Idempotent: configures first if needed, then lets ninja decide what
# actually has to be rebuilt. A second run with no source change is a
# no-op.
#
# This is the Meson path. scripts/build-switch.sh is the Makefile path,
# which is the one whose output was verified on real hardware; keep both
# working (STATUS.md).
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

if [ ! -f "$HORIZON_BUILD_DIR/build.ninja" ]; then
    echo "no configured build in $HORIZON_BUILD_DIR; configuring first"
    scripts/configure-horizon.sh
fi

horizon_meson compile -C "$HORIZON_BUILD_DIR" "$@"

# No `ninja -t cleandead` here: this build has edges that come and go —
# tests 12 and 13 exist only while Mesa's archives do — but Meson already
# runs restat+cleandead itself after regenerating build.ninja
# (mesonbuild/backend/ninjabackend.py:705, for ninja >= 1.12 or >= 1.10
# without dyndeps; this build has ninja 1.11.1 and no dyndeps). Measured:
# removing $MESA_BUILD_DIR and reconfiguring left 11 .nro on disk before
# this script ran at all. Adding a second cleandead cleaned 0 files.
count=$(find "$HORIZON_BUILD_DIR" -maxdepth 1 -name '*.nro' | wc -l)
echo "build-horizon: $count .nro in $HORIZON_BUILD_DIR"
