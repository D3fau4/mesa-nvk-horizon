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
set -- \
    --cross-file "$HORIZON_CROSS_CONST_FILE" \
    --cross-file "$HORIZON_CROSS_FILE" \
    "$@"

if [ -f "$HORIZON_BUILD_DIR/meson-info/meson-info.json" ]; then
    echo "reconfiguring $HORIZON_BUILD_DIR"
    horizon_meson setup --reconfigure "$@" "$HORIZON_BUILD_DIR" .
else
    echo "configuring $HORIZON_BUILD_DIR"
    horizon_meson setup "$@" "$HORIZON_BUILD_DIR" .
fi
