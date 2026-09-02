#!/usr/bin/env bash
# Builds libhorizon_gpu.a through Meson. With no target this builds only
# the archive; `test` also builds every test .nro this configuration can
# produce (mirrors the Makefile's `lib`/`test` split).
#
#   scripts/build-horizon.sh [lib|test] [extra meson compile args...]
#
# Idempotent: configures first if needed, then lets ninja decide what
# actually has to be rebuilt. A second run with no source change is a
# no-op.
#
# This is the Meson path. scripts/build-switch.sh is the Makefile path,
# which is the one whose output was verified on real hardware; keep both
# working.
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
# The recorded state is "<present|absent> <directory>" per build
# directory, not just the first half: -Dmesa_build_dir is baked in at
# configure time too, so pointing $MESA_BUILD_DIR at a *different*
# directory that also has the archives has to reconfigure as well. It
# did not, and the tests went on linking the old one (measured; see
# horizon_mesa_state).
#
# Two directories, not one. The NVK build dir joined it because t_vulkan
# is gated on *its* archives and they are produced after this script
# runs, so tracking only core Mesa left the exit-criterion test out of
# build.ninja permanently — see horizon_mesa_state for the sequence.
now=$(horizon_mesa_state)
then=$(cat "$HORIZON_BUILD_DIR.mesalibs" 2>/dev/null || true)
if [ "$now" != "$then" ]; then
    echo "Mesa state is now [$now], $HORIZON_BUILD_DIR was configured for"
    echo "[${then:-unrecorded}]; reconfiguring so meson.build asks again"
    scripts/configure-horizon.sh
fi

# Meson reserves `test` (and `all`) as target names, so meson.build calls
# the .nro alias `test-nros`. Translate here rather than there: this
# script's documented arguments are the Makefile's goals, and those two
# names are what every caller and CLAUDE.md already say.
case "${1:-}" in
    test|all) shift; set -- test-nros "$@" ;;
esac

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
