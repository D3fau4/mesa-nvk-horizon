#!/usr/bin/env bash
# Builds and runs the host-side unit tests for horizon/'s pure-logic
# modules (no libnx, no devkitA64 — the same sources the Switch build
# compiles). Uses ASan+UBSan when the compiler supports them.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
OUT="${OUT:-build/host-tests}"
CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Werror -Ihorizon/include"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"

if ! $CC $CFLAGS $SAN -x c -o /dev/null - <<<'int main(void){return 0;}' \
        2>/dev/null; then
    echo "note: sanitizers unavailable with $CC; running without them"
    SAN=""
fi

mkdir -p "$OUT"
status=0

run() { # name, sources...
    local name="$1"; shift
    if $CC $CFLAGS $SAN -o "$OUT/$name" "$@"; then
        if ! "$OUT/$name"; then
            status=1
        fi
    else
        echo "RESULT: FAIL (build) $name"
        status=1
    fi
}

run h_align       tests/host/h_align.c
run h_va_space    tests/host/h_va_space.c    horizon/vm/va_space.c
run h_syncpt_math tests/host/h_syncpt_math.c
run h_cmds        tests/host/h_cmds.c        horizon/submit/cmds.c
run h_scanout     tests/host/h_scanout.c     horizon/surface/surface.c
run h_status      tests/host/h_status.c      horizon/debug/status.c
run h_log         tests/host/h_log.c         horizon/debug/log.c
run h_blob_cache  tests/host/h_blob_cache.c  horizon/cache/blob_cache.c \
                                             horizon/cache/crc32.c

# THE ONE SUITE THAT COMPILES A MESA FILE, conditionally. h_nvkmd_sync
# builds mesa/src/nouveau/vulkan/nvkmd/horizon/nvkmd_horizon_sync.c — the
# vk_sync of the Horizon backend — against Mesa's headers and a
# syncpoint the test simulates, to pin the interleaving mesa-patches/
# 0081 fixes (review of PR #24 asked for it). Two things have to exist
# for that, and a bare clone has neither: mesa/ with the series applied,
# and the headers Mesa generates at configure time (vk_extensions.h and
# friends), which come from a configured NVK build directory. When
# either is missing the suite is skipped and says so; skipping is not a
# pass, and the note is what a reader of the log has to see.
#
# -Wno-unused-parameter for this suite alone: Mesa's vk_sync_type
# callbacks take a device most of them do not use, and the project's
# -Wextra -Werror is not Mesa's policy for Mesa's file.
MESA_SYNC_SRC=mesa/src/nouveau/vulkan/nvkmd/horizon/nvkmd_horizon_sync.c
NVK_GEN_DIR="${MESA_NVK_BUILD_DIR:-build/mesa-nvk}"
if [ -f "$MESA_SYNC_SRC" ] &&
   [ -f "$NVK_GEN_DIR/src/vulkan/runtime/vk_physical_device_features.h" ]; then
    MESA_INC="-Imesa/src -Imesa/include -Imesa/src/nouveau/vulkan \
              -Imesa/src/nouveau/vulkan/nvkmd \
              -Imesa/src/nouveau/vulkan/nvkmd/horizon \
              -Imesa/src/vulkan/runtime -Imesa/src/vulkan/util \
              -Imesa/src/nouveau/headers -Imesa/src/compiler \
              -Imesa/src/compiler/nir -Imesa/src/util \
              -I$NVK_GEN_DIR -I$NVK_GEN_DIR/src \
              -I$NVK_GEN_DIR/src/vulkan/util \
              -I$NVK_GEN_DIR/src/vulkan/runtime \
              -I$NVK_GEN_DIR/src/nouveau/headers \
              -I$NVK_GEN_DIR/src/nouveau/vulkan -I$NVK_GEN_DIR/src/compiler \
              -I$NVK_GEN_DIR/src/compiler/nir -I$NVK_GEN_DIR/src/util"
    # shellcheck disable=SC2086
    if $CC -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
           -D__SWITCH__ -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC \
           -Ihorizon/include -Itests/host $MESA_INC $SAN -pthread \
           -o "$OUT/h_nvkmd_sync" tests/host/h_nvkmd_sync.c "$MESA_SYNC_SRC"; then
        if ! "$OUT/h_nvkmd_sync"; then
            status=1
        fi
    else
        echo "RESULT: FAIL (build) h_nvkmd_sync"
        status=1
    fi
else
    echo "note: h_nvkmd_sync skipped — needs mesa/ with the series applied" \
         "and the generated headers of a configured $NVK_GEN_DIR"
fi

exit "$status"
