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

# --buildtype=plain for the same reason the horizon build uses it: the
# cross file states every flag explicitly, and Meson must not add -O/-g
# of its own on top.
#
# -Dshader-cache=disabled is a decision, not a workaround, and is
# recorded as one in STATUS.md. disk_cache.c and disk_cache_os.c are
# entirely inside #ifdef ENABLE_SHADER_CACHE and open with
# `#include <sys/mman.h>`, which newlib does not have; the feature also
# needs flock, posix_fallocate and memfd_create, all measured absent,
# and a writable cache directory this project has not designed. It is
# an optional on-disk cache, not part of the non-driver core, so it is
# turned off rather than patched into a shape nothing has tested. Making
# it work on Horizon is its own piece of work, for a later phase.
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
# scripts/check-tls-relocs.sh fails the build if it ever comes back.
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
    -Dshader-cache=disabled \
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
