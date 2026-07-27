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

# The two build paths ask the same question — are Mesa's archives there —
# at different times. The Makefile re-evaluates its $(wildcard) on every
# invocation; meson.build asks fs.exists() once, at configure time, and
# the answer is baked into build.ninja. So building Mesa *after*
# configuring this directory left it producing eleven .nro for good,
# with the explanatory message() only ever printed during a configure
# nobody was going to run again. Nothing in the cross-file identity
# covers it either: the archives are not an input to the toolchain.
#
# Comparing against what configure recorded is the smallest thing that
# closes it. `setup --reconfigure` re-executes meson.build, so fs.exists()
# is asked again; a missing stamp means the directory predates this
# check, which is the case that broke.
if horizon_mesa_libs_present; then now=present; else now=absent; fi
then=$(cat "$HORIZON_BUILD_DIR.mesalibs" 2>/dev/null || true)
if [ "$now" != "$then" ]; then
    echo "Mesa archives are $now, $HORIZON_BUILD_DIR was configured with them"
    echo "${then:-unrecorded}; reconfiguring so meson.build asks again"
    scripts/configure-horizon.sh
fi

horizon_meson compile -C "$HORIZON_BUILD_DIR" "$@"

# No `ninja -t cleandead` here: this build has edges that come and go —
# tests 12 and 13 exist only while Mesa's archives do — but Meson already
# runs restat+cleandead itself after regenerating build.ninja
# (mesonbuild/backend/ninjabackend.py:705 in meson 1.11.2, the version
# $MESON_VERSION pins in toolchain/versions.env — a line number is worth
# nothing without the version it belongs to; for ninja >= 1.12 or >= 1.10
# without dyndeps; this build has ninja 1.11.1 and no dyndeps). Measured:
# removing $MESA_BUILD_DIR and reconfiguring left 11 .nro on disk before
# this script ran at all. Adding a second cleandead cleaned 0 files.
count=$(find "$HORIZON_BUILD_DIR" -maxdepth 1 -name '*.nro' | wc -l)
echo "build-horizon: $count .nro in $HORIZON_BUILD_DIR"
