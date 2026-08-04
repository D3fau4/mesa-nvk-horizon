#!/usr/bin/env bash
# Thread-local storage gate.
#
#   scripts/check-tls-relocs.sh [object-or-archive-tree...]
#
# Defaults to $MESA_BUILD_DIR and build/toolchain/lib (compat/). Silently
# reports "nothing to check" when neither has been built, so a bare clone
# is not a failure.
#
# WHY THIS EXISTS — a measured miscompile, not a hypothetical.
#
# On devkitA64 gcc 15.2.0, `-mtp=soft -fPIC` generates wrong code for
# every access to a _Thread_local object. From a four-line test file:
#
#   -mtp=soft -fPIE   bl  __aarch64_read_tp
#                     add x0, x0, #0x0, lsl #12   R_AARCH64_TLSLE_ADD_TPREL_HI12
#                     add x0, x0, #0x0            R_AARCH64_TLSLE_ADD_TPREL_LO12_NC
#   -mtp=soft -fPIC   bl  __aarch64_read_tp
#                     lsl x0, x0, #1              (no relocation emitted)
#
# The second doubles the thread pointer instead of adding the variable's
# offset to it, and emits no relocation for the linker to fix up. Every
# read and write through that pointer lands at a wild address. It is
# silent: the compile succeeds, the link succeeds, and the program hangs
# or corrupts memory the first time a thread-local is touched.
#
# It reached this tree because Meson appends -fPIC to static-library
# objects unless b_staticpic=false, after the cross file's -fPIE. All
# three objects in the Mesa build that use TLS were affected, and it hung
# tests/t_threads.c on its first run (STATUS.md, emulator 2026-07-28).
#
# HOW IT CHECKS. An object that calls __aarch64_read_tp is accessing a
# thread-local; correct code for that access carries at least one
# R_AARCH64_TLS* relocation. An object with the call and no relocation is
# the broken form. That test is about the property, not about a flag, so
# it keeps working if a future toolchain gets this wrong a different way.
#
# It cannot see objects whose TLS offset needs no relocation in principle
# — there is no such case in this ABI, since the offset is always a
# link-time quantity — nor archives already installed by someone else.
#
# Exit code 0 = clean, 1 = a miscompiled object (printed).
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -u
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

if [ "$#" -gt 0 ]; then
    TREES=("$@")
else
    TREES=("$MESA_BUILD_DIR" "$HORIZON_COMPAT_LIBDIR")
fi

present=()
for t in "${TREES[@]}"; do
    [ -d "$t" ] && present+=("$t")
done

if [ "${#present[@]}" -eq 0 ]; then
    echo "check-tls-relocs: nothing to check (no built objects)"
    exit 0
fi

# Archives as well as loose objects, because a Rust staticlib has no
# loose objects at all: `find -name '*.o'` over build/mesa-nvk finds 821
# of them and none under src/nouveau/rust_runtime, whose members exist
# only inside libnouveau_rust_runtime.a. Checking only loose objects
# would have left the Rust half — "one more place TLS could appear",
# which is why build-mesa-nvk.sh looked at it in the first place —
# scanned by nothing.
#
# Granularity differs between the two, and the difference is worth
# stating rather than hiding: a loose object is judged on its own, while
# an archive is judged whole. An archive whose members disagreed (one
# with relocations, one without) would pass. That is not a shape this
# toolchain produces — an archive comes out of one cargo or one meson
# invocation with one set of flags — and per-member extraction costs a
# temporary directory per archive for a case that cannot arise here.
mapfile -t objs < <(find "${present[@]}" \
                        \( -name '*.o' -o -name '*.a' \) -type f | sort)

if [ "${#objs[@]}" -eq 0 ]; then
    echo "check-tls-relocs: nothing to check (no objects under ${present[*]})"
    exit 0
fi

# One container invocation for all of them: starting a container per
# object turned a two-second check into minutes.
report=$(horizon_run sh -c '
    ok=0; bad=0
    for o in "$@"; do
        aarch64-none-elf-nm "$o" 2>/dev/null |
            grep -q "U __aarch64_read_tp" || continue
        if aarch64-none-elf-readelf -r "$o" 2>/dev/null |
               grep -q "R_AARCH64_TLS"; then
            ok=$((ok+1))
        else
            bad=$((bad+1))
            echo "BAD $o"
        fi
    done
    echo "COUNT $ok $bad"
' sh "${objs[@]}" 2>/dev/null)

bad_list=$(printf '%s\n' "$report" | sed -n 's/^BAD //p')
counts=$(printf '%s\n' "$report" | sed -n 's/^COUNT //p')
ok=${counts%% *}
bad=${counts##* }

if [ -z "$counts" ]; then
    echo "check-tls-relocs: could not read the objects (toolchain unavailable?)" >&2
    exit 1
fi

if [ "$bad" -ne 0 ]; then
    echo "MISCOMPILED THREAD-LOCAL ACCESS in $bad object(s):"
    printf '  %s\n' $bad_list
    echo
    echo "These call __aarch64_read_tp and carry no R_AARCH64_TLS relocation."
    echo "On devkitA64 that is the -mtp=soft -fPIC form, which doubles the"
    echo "thread pointer instead of adding the variable's offset. Configure"
    echo "with -Db_staticpic=false so Meson stops appending -fPIC to"
    echo "static-library objects; see scripts/configure-mesa.sh."
    exit 1
fi

echo "check-tls-relocs: OK ($ok object(s)/archive(s) use TLS, all with" \
     "relocations, ${#objs[@]} scanned)"
exit 0
