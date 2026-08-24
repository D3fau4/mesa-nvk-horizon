#!/usr/bin/env bash
# Configures the pinned Mesa checkout for Horizon/aarch64 with no
# drivers at all — the "non-driver core" of Phase 3's exit criterion.
#
#   scripts/configure-mesa.sh [extra meson setup args...]
#
# Why no drivers: Phase 3 is "Mesa configures for horizon and builds the
# non-driver core". src/meson.build builds gtest, c11/impl,
# android_stub, util, compiler and tool unconditionally, which is that
# core; every driver is behind a with_* condition. It also keeps the
# Rust question out of the way — without the nouveau Vulkan driver Mesa
# never calls add_languages('rust'), so R13 does not block here.
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
# $MESA_BUILD_DIR comes from there now, so meson.build and the Makefile
# resolve it to the same directory this script builds in.

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
# Before meson setup, never after. Mesa decides HAVE_SYSCONF with
# cc.has_function('sysconf'), which is a link test, and compat/ is what
# defines that symbol on this platform.
scripts/build-compat.sh

# --- horizon_gpu, staged as a prefix ---------------------------------
#
# src/util/disk_cache_horizon.c stores entries through horizon_gpu's blob
# cache, so even this driverless build needs the layer's headers and
# archive. Same staging as scripts/configure-mesa-nvk.sh, and the same
# reason for copying rather than symlinking: the Meson build runs inside
# a container that mounts $PWD, where a symlink out of the mount resolves
# to nothing.
HORIZON_GPU_PREFIX=build/toolchain/horizon-gpu

if [ ! -f "$HORIZON_BUILD_DIR/libhorizon_gpu.a" ]; then
    echo "configure-mesa: building horizon_gpu first"
    scripts/build-horizon.sh
fi

rm -rf "$HORIZON_GPU_PREFIX"
mkdir -p "$HORIZON_GPU_PREFIX/include" "$HORIZON_GPU_PREFIX/lib"
cp -a horizon/include/horizon_gpu "$HORIZON_GPU_PREFIX/include/"
# Fattened rather than copied: Meson's libhorizon_gpu.a is thin and
# names its objects relative to its own directory, so a plain cp puts an
# unreadable archive here. scripts/configure-mesa-nvk.sh carries the
# measurement; scripts/fatten-archives.sh carries the mechanism.
horizon_fatten_archive "$HORIZON_BUILD_DIR/libhorizon_gpu.a" \
                       "$HORIZON_GPU_PREFIX/lib/libhorizon_gpu.a"
echo "configure-mesa: horizon_gpu staged in $HORIZON_GPU_PREFIX"

# The identity the shader cache keys on. See scripts/gen-driver-id.sh for
# why it is a digest of sources and not the build stamp.
HORIZON_DRIVER_ID=$(scripts/gen-driver-id.sh)
echo "configure-mesa: driver id $HORIZON_DRIVER_ID"

# --buildtype=plain for the same reason the horizon build uses it: the
# cross file states every flag explicitly, and Meson must not add -O/-g
# of its own on top.
#
# -Dshader-cache=enabled, and the four years of "later phase" this line
# used to promise are over. What it said before, and it was true when it
# was written: disk_cache.c and disk_cache_os.c are entirely inside
# #ifdef ENABLE_SHADER_CACHE and open with `#include <sys/mman.h>`, which
# newlib does not have; the feature also needs flock, posix_fallocate and
# memfd_create, all measured absent, and a writable cache directory this
# project had not designed. That was recorded as a decision rather than
# hidden, and the pagaré was written down in the Phase 4 record: "If a
# later phase enables the shader cache ... flock comes back and needs
# an answer then."
#
# The answer is four patches and a module, and none of it makes newlib
# pretend to have anything:
#
#   0078  disk_cache.c's <ftw.h>/<sys/mman.h>/<sys/file.h>/<fcntl.h>/
#         <dirent.h> are removed — the file uses none of them, so the
#         generic half needed nothing but honesty about its includes.
#   0079  mesa_cache_db.c is gated on HAVE_FLOCK, exactly the way
#         fossilize_db.c beside it already was.
#   0080  the driver identity stops being Mesa's version alone, because a
#         disk cache keyed on an identity that does not change when this
#         port changes serves last build's binaries to this build.
#   0081  disk_cache_os.c is replaced on Horizon by disk_cache_horizon.c,
#         which stores entries through horizon/cache/'s append-only,
#         CRC-validated blob store. No mmap, no flock, no rename.
#
# The cache directory that "had not been designed" is sdmc:/mesa_shader_cache.
#
# This build has no driver, so nothing in it will ever call the cache —
# but it compiles and links src/util, which is where every one of those
# patches lands, and it does so in minutes rather than the hour the full
# NVK build takes. That is what makes it worth enabling here: it is the
# cheap way to find out that this still builds.
#
# "$@" comes last so a caller can override any of these, and so the
# options a given measurement was taken with are visible on the command
# line rather than buried here.
# -Db_staticpic=false is not a preference. Meson otherwise appends -fPIC
# to every static-library object, after the cross file's -fPIE, and on
# this toolchain `-mtp=soft -fPIC` MISCOMPILES thread-local storage.
# Measured, on devkitA64 gcc 15.2.0, from a four-line file:
#
#   -mtp=soft -fPIE   bl __aarch64_read_tp
#                     add x0, x0, #0x0, lsl #12  R_AARCH64_TLSLE_ADD_TPREL_HI12
#                     add x0, x0, #0x0           R_AARCH64_TLSLE_ADD_TPREL_LO12_NC
#   -mtp=soft -fPIC   bl __aarch64_read_tp
#                     lsl x0, x0, #1             <- no relocation at all
#
# The second form doubles the thread pointer instead of adding the
# variable's offset to it, and carries no TLS relocation for the linker
# to fix up. Every access to a _Thread_local reads and writes a wild
# address. All three objects in the Mesa build that use TLS —
# u_call_once.c.o, u_debug.c.o, u_qsort.cpp.o — were built that way, and
# it is what hung t_threads on the first run: os_get_option_cached()
# takes a statically initialised simple_mtx whose lock goes through the
# thread_local in u_call_once.c.
#
# The same option is already in this project's own meson.build
# default_options, where it was added for a smaller reason (matching the
# hardware-verified Makefile output byte for byte). Nothing built here is
# a shared library — Horizon has no dynamic loader, which patch 0007
# records — so -fPIC buys nothing on this platform and costs this.
#
# scripts/build-mesa.sh runs scripts/check-tls-relocs.sh over the objects
# after every build, so overriding this option — which the trailing "$@"
# below deliberately allows — fails there rather than shipping silently.
set -- \
    --buildtype=plain \
    -Db_staticpic=false \
    --cross-file "$HORIZON_CROSS_CONST_FILE" \
    --cross-file "$HORIZON_CROSS_FILE" \
    -Dgallium-drivers= \
    -Dvulkan-drivers= \
    -Dplatforms= \
    -Dopengl=false \
    -Dllvm=disabled \
    -Dshader-cache=enabled \
    -Dhorizon-gpu-dir="$PWD/$HORIZON_GPU_PREFIX" \
    -Dhorizon-driver-id="$HORIZON_DRIVER_ID" \
    "$@"

horizon_setup_mode "$MESA_BUILD_DIR"
case "$HORIZON_SETUP_MODE" in
    --wipe)
        echo "cross files changed since $MESA_BUILD_DIR was configured;"
        echo "wiping it — Meson only reads them on a first configure"
        ;;
    --reconfigure) echo "reconfiguring $MESA_BUILD_DIR" ;;
    *)             echo "configuring $MESA_BUILD_DIR" ;;
esac

# shellcheck disable=SC2086 # one flag or deliberately empty
horizon_meson setup $HORIZON_SETUP_MODE "$@" "$MESA_BUILD_DIR" mesa
horizon_record_cross_id
