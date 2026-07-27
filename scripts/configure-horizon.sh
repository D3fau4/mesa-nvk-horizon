#!/usr/bin/env bash
# Configures the Meson cross build of libhorizon_gpu.a and the ten
# Phase 1 test .nros.
#
#   scripts/configure-horizon.sh [extra meson setup args...]
#
# Idempotent: on an already-configured build directory it reconfigures
# in place rather than failing or wiping it, so running it twice is
# harmless and cheap.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

echo "toolchain: $HORIZON_TOOLCHAIN_DESC"

horizon_ensure_meson
scripts/gen-cross-file.sh
# Before meson setup, never after: the cross file links -lhorizon_compat
# and Meson links a test program during its compiler sanity check.
scripts/build-compat.sh

# Cross-file order matters only in that the generated file must be
# readable; Meson shares [constants] across all of them. Passing the
# generated one first keeps the reading order the same as the
# dependency order.
#
# -Dmesa_build_dir is how $MESA_BUILD_DIR reaches meson.build: Meson runs
# inside the toolchain container, which receives only the variables
# horizon_run names, so reading the environment from meson.build would
# see nothing there. Tests 12 and 13 link the archives in that directory
# and are skipped when it holds none.
set -- \
    --cross-file "$HORIZON_CROSS_CONST_FILE" \
    --cross-file "$HORIZON_CROSS_FILE" \
    -Dmesa_build_dir="$MESA_BUILD_DIR" \
    "$@"

# meson.options is part of this directory's identity as well: a -D for an
# option added after the directory was configured is rejected before
# Meson re-reads the file that declares it.
horizon_setup_mode "$HORIZON_BUILD_DIR" meson.options
case "$HORIZON_SETUP_MODE" in
    --wipe)
        echo "cross files or meson.options changed since $HORIZON_BUILD_DIR"
        echo "was configured; wiping it — Meson only reads them on a first"
        echo "configure"
        ;;
    --reconfigure) echo "reconfiguring $HORIZON_BUILD_DIR" ;;
    *)             echo "configuring $HORIZON_BUILD_DIR" ;;
esac

# shellcheck disable=SC2086 # one flag or deliberately empty
horizon_meson setup $HORIZON_SETUP_MODE "$@" "$HORIZON_BUILD_DIR" .
horizon_record_cross_id

# What meson.build's fs.exists() just decided about tests 12 and 13.
# scripts/build-horizon.sh compares it against the state on disk then;
# see the note there for why that comparison has to exist. Beside the
# build directory for the same reason the cross-id stamp is: --wipe
# empties the directory itself.
if horizon_mesa_libs_present; then
    echo present > "$HORIZON_BUILD_DIR.mesalibs"
else
    echo absent > "$HORIZON_BUILD_DIR.mesalibs"
fi
