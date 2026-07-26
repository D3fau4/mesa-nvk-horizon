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
run h_status      tests/host/h_status.c      horizon/debug/status.c
run h_log         tests/host/h_log.c         horizon/debug/log.c

exit "$status"
