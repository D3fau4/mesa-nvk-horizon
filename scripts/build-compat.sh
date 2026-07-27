#!/usr/bin/env bash
# Builds compat/ into build/toolchain/lib/libhorizon_compat.a.
#
#   scripts/build-compat.sh [--force]
#
# compat/ holds functions devkitA64's newlib declares but does not
# define (today: sysconf). CLAUDE.md permits exactly that, and nothing
# more — no linker-level symbol wrapping, and nothing that shadows a
# function the C library already provides. scripts/check-layering.sh
# enforces both.
#
# WHY THIS IS A SCRIPT AND NOT A MESON TARGET. The cross file names
# -lhorizon_compat in its link arguments, because from Meson's point of
# view compat/ completes the C library: `cc.has_function('sysconf')` is
# a *link* test, and Meson links a test program during its own sanity
# check at setup time. So the archive has to exist before any
# `meson setup` runs — including the one that would build it. Making it
# a target of meson.build would be circular. It is provisioning, in the
# same sense as horizon_ensure_meson.
#
# The compile flags mirror the Makefile's, which is the build that was
# verified on real hardware. They are duplicated here rather than
# derived, because deriving them would mean parsing a Makefile from
# shell; keep the two in step by hand.
#
# Idempotent: rebuilds only when a source or this script is newer than
# the archive. --force rebuilds unconditionally.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

LIB="$HORIZON_COMPAT_LIBDIR/libhorizon_compat.a"
OBJDIR="build/toolchain/compat-obj"

# Same groups, same order, as the Makefile's ARCH and CFLAGS.
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
CFLAGS="-g -O2 -Wall -Wextra -Werror -std=gnu11 -ffunction-sections
        -D__SWITCH__ $ARCH -I${HORIZON_DEVKITPRO}/libnx/include"

shopt -s nullglob
sources=(compat/*.c)
shopt -u nullglob

if [ ${#sources[@]} -eq 0 ]; then
    echo "error: compat/ has no sources, but the cross file links" >&2
    echo "       -lhorizon_compat. Either add the source back or drop" >&2
    echo "       the link argument; an empty archive would link but" >&2
    echo "       would silently stop providing sysconf." >&2
    exit 1
fi

# What this archive was built from AND against, hashed. mtimes alone are
# not enough: the Switch toolchain is deliberately not pinned
# (toolchain/versions.env), so it moves underneath us. A newer image or
# a different $DEVKITPRO brings different newlib and libnx headers, and
# sysconf.c compiles constants and struct layouts out of them — nothing
# in compat/ would be newer than the archive, so an mtime check would
# call it up to date and Mesa would link an object built against the
# previous environment. Precisely the wrong moment to be stale: right
# after updating the toolchain.
#
# Covers the sources' *content* too, so a revert that restores an older
# file rebuilds rather than being missed for having an older mtime.
build_identity() {
    {
        echo "flags: $CFLAGS"
        echo "toolchain: $HORIZON_TOOLCHAIN_DESC"
        echo "image: $(horizon_image_digest)"
        horizon_run aarch64-none-elf-gcc --version 2>/dev/null | head -n 1
        cat "${sources[@]}"
    } | sha256sum | cut -d' ' -f1
}

STAMP="$HORIZON_COMPAT_LIBDIR/.build-id"

identity=$(build_identity)

if [ "$FORCE" -eq 0 ] && [ -f "$LIB" ] &&
   [ "$identity" = "$(cat "$STAMP" 2>/dev/null)" ]; then
    echo "build-compat: $LIB up to date (${#sources[@]} source(s))"
    exit 0
fi

mkdir -p "$OBJDIR" "$HORIZON_COMPAT_LIBDIR"

objects=()
for src in "${sources[@]}"; do
    obj="$OBJDIR/$(basename "${src%.c}").o"
    echo "build-compat: CC $src"
    # shellcheck disable=SC2086 # CFLAGS is a deliberate word list
    horizon_run aarch64-none-elf-gcc $CFLAGS -c "$src" -o "$obj"
    objects+=("$obj")
done

# Rebuild the archive from scratch: `ar r` into a stale archive would
# keep members whose sources have since been deleted.
rm -f "$LIB"
horizon_run aarch64-none-elf-gcc-ar rcs "$LIB" "${objects[@]}"

# After the archive exists, never before: a failed build must not leave a
# stamp claiming it is current.
printf '%s\n' "$identity" > "$STAMP"

echo "build-compat: wrote $LIB (${#objects[@]} object(s))"
