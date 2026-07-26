#!/usr/bin/env bash
# Configures the pinned Mesa checkout for Horizon/aarch64 with no
# drivers at all — the "non-driver core" of Phase 3's exit criterion
# (docs/milestones.md).
#
#   scripts/configure-mesa.sh [extra meson setup args...]
#
# Why no drivers: Phase 3 is "Mesa configures for horizon and builds the
# non-driver core". src/meson.build builds gtest, c11/impl,
# android_stub, util, compiler and tool unconditionally, which is that
# core; every driver is behind a with_* condition. It also keeps the
# Rust question out of the way — without the nouveau Vulkan driver Mesa
# never calls add_languages('rust'), so R13 does not block here
# (docs/rust-toolchain.md).
#
# The build directory is deliberately NOT the horizon one: this
# configures Mesa's meson.build, not ours, and the two must not share
# state.
#
# Idempotent: on an already-configured build directory it reconfigures
# in place rather than failing or wiping it.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

MESA_BUILD_DIR="${MESA_BUILD_DIR:-build/mesa-probe}"

[ -f mesa/meson.build ] || {
    echo "error: mesa/ is not populated (no meson.build)." >&2
    echo "       Run scripts/fetch-mesa.sh first." >&2
    exit 1
}

echo "toolchain: $HORIZON_TOOLCHAIN_DESC"

horizon_ensure_meson
# Mesa's generators are Python and refuse to configure without mako
# (milestone item 6); the toolchain image ships neither them nor pip.
horizon_ensure_python_deps
scripts/gen-cross-file.sh

# --buildtype=plain for the same reason the horizon build uses it: the
# cross file states every flag explicitly, and Meson must not add -O/-g
# of its own on top.
#
# "$@" comes last so a caller can override any of these, and so the
# options a given measurement was taken with are visible on the command
# line rather than buried here.
set -- \
    --buildtype=plain \
    --cross-file "$HORIZON_CROSS_CONST_FILE" \
    --cross-file "$HORIZON_CROSS_FILE" \
    -Dgallium-drivers= \
    -Dvulkan-drivers= \
    -Dplatforms= \
    -Dopengl=false \
    -Dllvm=disabled \
    "$@"

if [ -f "$MESA_BUILD_DIR/meson-info/meson-info.json" ]; then
    echo "reconfiguring $MESA_BUILD_DIR"
    horizon_meson setup --reconfigure "$@" "$MESA_BUILD_DIR" mesa
else
    echo "configuring $MESA_BUILD_DIR"
    horizon_meson setup "$@" "$MESA_BUILD_DIR" mesa
fi
