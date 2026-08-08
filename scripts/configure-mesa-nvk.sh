#!/usr/bin/env bash
# Configures the pinned Mesa checkout with the nouveau Vulkan driver and
# the Horizon kernel-mode-driver backend.
#
#   scripts/configure-mesa-nvk.sh [extra meson setup args...]
#
# This is the Phase 4 build. scripts/configure-mesa.sh remains the
# Phase 3 one — Mesa's non-driver core, no drivers, no Rust — and the
# two use different build directories on purpose: they answer different
# questions and must not share configured state.
#
# What this adds over that one:
#
#   -Dvulkan-drivers=nouveau    the driver itself
#   -Dmesa-clc=system           NVK compiles two OpenCL C files into
#                               SPIR-V at build time. mesa_clc is a
#                               *build-machine* tool; building it inside
#                               the cross build would resolve LLVM,
#                               clang and SPIRV-Tools as *host machine*
#                               dependencies, for aarch64-horizon, where
#                               none of them exist.
#                               scripts/build-mesa-clc.sh supplies it.
#   -Dplatforms=vi              the window system: Horizon's VI
#                               compositor, reached through libnx's
#                               NWindow. It defines VK_USE_PLATFORM_VI_NN,
#                               which is what makes VK_NN_vi_surface and
#                               the Horizon WSI backend exist at all.
#                               'auto' never selects it — no
#                               host_machine.system() maps to it — so it
#                               has to be named here.
#   -Dhorizon-gpu-dir=...       where this project's horizon_gpu layer
#                               is, since it lives outside Mesa
#   -Dxmlconfig=disabled        driconf parses an XML file with expat and
#                               matches executable names with POSIX
#                               regex. Neither exists here, and there is
#                               no per-application configuration file on
#                               a console anyway; Mesa hardcodes the
#                               defaults when this is off.
#
# Idempotent: reconfigures an existing directory in place, and wipes it
# when the cross files changed — Meson only reads those on a first
# configure.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

[ -f mesa/meson.build ] || {
    echo "error: mesa/ is not populated (no meson.build)." >&2
    echo "       Run scripts/fetch-mesa.sh first." >&2
    exit 1
}

echo "toolchain: $HORIZON_TOOLCHAIN_DESC"

horizon_ensure_meson
horizon_ensure_python_deps
scripts/gen-cross-file.sh
scripts/build-compat.sh

# The Rust wraps Mesa resolves at configure time. Downloaded on the
# host, because containers here have no network.
scripts/fetch-mesa-subprojects.sh

# mesa_clc and vtn_bindgen2, on PATH through horizon_run.
scripts/build-mesa-clc.sh

# --- horizon_gpu, staged as a prefix ---------------------------------
#
# Mesa is a pinned checkout and never the place our code lives, so the
# GPU layer is named by a build option rather than reached through a
# relative path into this repository. Staging it as a prefix — an
# include/ and a lib/ — is what makes that option a plain directory
# instead of two.
#
# Copied rather than symlinked: the Meson build runs inside a container
# that mounts $PWD, and a symlink pointing outside the mount would
# resolve to nothing there.
HORIZON_GPU_PREFIX=build/toolchain/horizon-gpu

if [ ! -f "$HORIZON_BUILD_DIR/libhorizon_gpu.a" ]; then
    echo "configure-mesa-nvk: building horizon_gpu first"
    scripts/build-horizon.sh
fi

rm -rf "$HORIZON_GPU_PREFIX"
mkdir -p "$HORIZON_GPU_PREFIX/include" "$HORIZON_GPU_PREFIX/lib"
cp -a horizon/include/horizon_gpu "$HORIZON_GPU_PREFIX/include/"
cp "$HORIZON_BUILD_DIR/libhorizon_gpu.a" "$HORIZON_GPU_PREFIX/lib/"
echo "configure-mesa-nvk: horizon_gpu staged in $HORIZON_GPU_PREFIX"

MESA_NVK_BUILD_DIR="${MESA_NVK_BUILD_DIR:-build/mesa-nvk}"

set -- \
    --buildtype=plain \
    -Db_staticpic=false \
    --cross-file "$HORIZON_CROSS_CONST_FILE" \
    --cross-file "$HORIZON_CROSS_FILE" \
    -Dgallium-drivers= \
    -Dvulkan-drivers=nouveau \
    -Dplatforms=vi \
    -Dopengl=false \
    -Dllvm=disabled \
    -Dshader-cache=disabled \
    -Dxmlconfig=disabled \
    -Dmesa-clc=system \
    -Dprecomp-compiler=system \
    -Dhorizon-gpu-dir="$PWD/$HORIZON_GPU_PREFIX" \
    "$@"

horizon_setup_mode "$MESA_NVK_BUILD_DIR"
case "$HORIZON_SETUP_MODE" in
    --wipe)
        echo "cross files changed since $MESA_NVK_BUILD_DIR was configured;"
        echo "wiping it — Meson only reads them on a first configure"
        ;;
    --reconfigure) echo "reconfiguring $MESA_NVK_BUILD_DIR" ;;
    *)             echo "configuring $MESA_NVK_BUILD_DIR" ;;
esac

# shellcheck disable=SC2086 # one flag or deliberately empty
horizon_meson setup $HORIZON_SETUP_MODE "$@" "$MESA_NVK_BUILD_DIR" mesa
horizon_record_cross_id

echo "configure-mesa-nvk: build it with"
echo "  scripts/build-mesa-nvk.sh"
