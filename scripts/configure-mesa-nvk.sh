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
    scripts/build-horizon.sh lib
fi

rm -rf "$HORIZON_GPU_PREFIX"
mkdir -p "$HORIZON_GPU_PREFIX/include" "$HORIZON_GPU_PREFIX/lib"
cp -a horizon/include/horizon_gpu "$HORIZON_GPU_PREFIX/include/"
# FATTENED, NOT COPIED, and this was a live bug rather than a
# precaution. Meson writes libhorizon_gpu.a as a THIN archive: it holds
# paths to the objects in libhorizon_gpu.a.p/, resolved relative to the
# archive's own directory. Copied here, those paths point at a
# libhorizon_gpu.a.p/ that does not exist beside it, and the staged
# archive is not readable at all — measured on this tree before the fix:
#
#   $ aarch64-none-elf-gcc-ar t build/toolchain/horizon-gpu/lib/libhorizon_gpu.a
#   ar: build/toolchain/horizon-gpu/lib/libhorizon_gpu.a: No such file or directory
#
# It went unnoticed because nothing has linked it yet: -Dhorizon-gpu-dir
# supplies an include directory and a find_library that locates the file
# without linking it. The day anything in that build does link it, this
# is a link failure inside Mesa about a file of ours. See
# scripts/fatten-archives.sh.
horizon_fatten_archive "$HORIZON_BUILD_DIR/libhorizon_gpu.a" \
                       "$HORIZON_GPU_PREFIX/lib/libhorizon_gpu.a"
echo "configure-mesa-nvk: horizon_gpu staged in $HORIZON_GPU_PREFIX"

# The identity NVK reports as driverUUID and the pipeline cache UUID, and
# that the shader disk cache keys on. Mesa's version alone does not
# change when this port changes (mesa-patches/0043); this does.
HORIZON_DRIVER_ID=$(scripts/gen-driver-id.sh)
echo "configure-mesa-nvk: driver id $HORIZON_DRIVER_ID"

MESA_NVK_BUILD_DIR="${MESA_NVK_BUILD_DIR:-build/mesa-nvk}"

# --buildtype=plain ALREADY DISABLES ASSERTIONS, AND NOTHING HERE SHOULD
# "FIX" THAT BY ADDING -Db_ndebug. Written down because the chain is
# three files long and reading any one of them alone gives the opposite
# answer:
#
#   - Mesa asks for 'b_ndebug=if-release' in its project() default
#     options (mesa/meson.build:10-12), so the choice is delegated to
#     the buildtype;
#   - Meson 1.11.2 treats `if-release` as release for BOTH `release` and
#     `plain` (mesonbuild/compilers/compilers.py:270-280, applied at
#     :337), which is the step that is easy to miss;
#   - that reaches C as -DNDEBUG (compilers/mixins/clike.py) and Rust as
#     `-C debug-assertions=no -C overflow-checks=no`
#     (compilers/rust.py:524-526).
#
# CHECKED ON A CONFIGURED DIRECTORY, not inferred: on 2026-09-10 the
# compile command for src/compiler/nir/nir_validate.c in build/mesa-nvk
# carried -DNDEBUG (with -g -O2, which come from the cross file and not
# from the buildtype). So nir_validate_shader is the empty stub and the
# assert()s are not compiled. Grepping this repository for NDEBUG finds
# nothing and means nothing; the question actually being asked is
#
#   ninja -C build/mesa-nvk -t commands src/compiler/nir/libnir.a |
#     grep -m1 nir_validate
#
# and the archive has to be NAMED. A bare `ninja -t commands` lists the
# default target's commands, which is not everything the build has
# rules for — nir_validate.c is not in it, and reading that as "no such
# command" is the wrong answer. `-t compdb` is the other way round: it
# lists every compile rule, so it answers the same question with a
# filter over its JSON.
#
# WHAT IS NOT SET, AND HOW TO SET IT FOR AN A/B. Meson emits no
# `-C opt-level` at all for `plain` (compilers/rust.py:36-44 maps it to
# an empty list), so rustc uses its own default of 0 — confirmed the
# same day on the same directory: none of the thirteen cross-targeted
# rustc invocations carried one. NAK and NIL are therefore unoptimised
# Rust. The variant to compare against is one option, passed through to
# meson by the "$@" below:
#
#   MESA_NVK_BUILD_DIR=build/mesa-nvk-o2 \
#     scripts/configure-mesa-nvk.sh -Drust_args=-Copt-level=2
#
# which was verified to reach exactly the thirteen cross crates —
# nak_rs, nil, compiler, bitview, nv_push_rs, nvidia_headers,
# nouveau_rust_runtime and the rest — and none of the eight
# build-machine proc-macro crates, which are build tools and not the
# driver.
#
# A SEPARATE BUILD DIRECTORY IS PART OF THE PROCEDURE, and so is
# MESA_SHADER_CACHE_DISABLE=true in the run: scripts/gen-driver-id.sh
# digests sources and cross files and NOT meson options, so the two
# builds share a driver id, a pipelineCacheUUID and a cache file. The
# second one would otherwise be served the first one's compiled shaders.
# tests/vk_pipelines/compile_identity is the case that measures the pair.

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
    -Dshader-cache=enabled \
    -Dhorizon-driver-id="$HORIZON_DRIVER_ID" \
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
