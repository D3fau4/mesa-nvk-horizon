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
# tests/t_threads.c on its first run (emulator, 2026-07-28).
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

# Loose objects, plus the members of archives that have none.
#
# Neither half of that is arbitrary, and the first attempt at it got
# both wrong, so the reasoning is written out.
#
# WHY ARCHIVES AT ALL. A Rust staticlib has no loose objects: over
# build/mesa-nvk, `find -name '*.o'` turns up 821 and not one of them is
# under src/nouveau/rust_runtime, whose members exist only inside
# libnouveau_rust_runtime.a. Scanning loose objects alone leaves the
# Rust half — "one more place TLS could appear", which is why
# build-mesa-nvk.sh was looking in the first place — checked by nothing.
#
# WHY NOT ARCHIVES AS WELL AS OBJECTS. Because that counts everything
# twice. All 821 of those loose objects live under a `<archive>.p/`
# directory and are exactly the members of the 24 archives beside them,
# so scanning both scanned the same code twice and reported 845 — in a
# script whose entire job is to make a count trustworthy.
#
# WHY MEMBERS AND NOT WHOLE ARCHIVES. `nm` and `readelf -r` on an
# archive answer for the archive: one member calling __aarch64_read_tp
# and another carrying the only relocation reads as clean. It was
# tempting to wave that away as a shape the toolchain cannot produce —
# except the artefact this was added for is exactly that shape.
# libnouveau_rust_runtime.a bundles the -Zbuild-std core/alloc objects
# together with the crate's own, from separate compilations. The blind
# spot was aimed straight at the target.
#
# So: an archive is expanded into its members and each is judged alone,
# and only when nothing else already covers it.
tls_probe_dir="build/probe/tls-members"
rm -rf "$tls_probe_dir"

mapfile -t loose < <(find "${present[@]}" -name '*.o' -type f | sort)
mapfile -t archives < <(find "${present[@]}" -name '*.a' -type f | sort)

objs=("${loose[@]}")
expanded=0

for a in "${archives[@]}"; do
    # Meson names the object directory after the target: libfoo.a ->
    # libfoo.a.p/. When it holds objects, they ARE this archive's
    # members and are already in `loose`.
    if compgen -G "$a.p/*.o" >/dev/null; then
        continue
    fi
    dest="$tls_probe_dir/$(echo "$a" | tr '/' '_')"
    mkdir -p "$dest"
    # `ar x` writes into the working directory, so it is run from the
    # destination with the archive named absolutely.
    if ! horizon_run sh -c 'cd "$1" && aarch64-none-elf-ar x "$2"' \
             sh "$dest" "$PWD/$a" 2>/dev/null; then
        echo "check-tls-relocs: could not expand $a" >&2
        exit 1
    fi
    mapfile -t members < <(find "$dest" -name '*.o' -type f | sort)
    objs+=("${members[@]}")
    expanded=$((expanded + 1))
done

if [ "${#objs[@]}" -eq 0 ]; then
    echo "check-tls-relocs: nothing to check (no objects under ${present[*]})"
    exit 0
fi

# One container invocation for all of them: starting a container per
# object turned a two-second check into minutes.
#
# THE LIST GOES IN A FILE, NOT ON THE COMMAND LINE, and that is not
# tidiness. Passed as arguments, this reached 1203 objects and 126 kB on
# a full NVK build — past the 32767-character command line Windows gives
# a native binary, so docker.exe never ran, `report` came back empty,
# and the branch below blamed the toolchain. The file lives in the tree,
# which horizon_run mounts at the same path on both sides, so nothing
# about how the container is started has to change.
#
# Its stderr is NOT discarded any more, for the same reason: `2>/dev/null`
# on this line is what turned a specific docker error into the guess
# underneath it.
printf '%s\n' "${objs[@]}" > "$tls_probe_dir/objects.txt"
report=$(horizon_run sh -c '
    ok=0; bad=0
    while IFS= read -r o; do
        [ -n "$o" ] || continue
        aarch64-none-elf-nm "$o" 2>/dev/null |
            grep -q "U __aarch64_read_tp" || continue
        if aarch64-none-elf-readelf -r "$o" 2>/dev/null |
               grep -q "R_AARCH64_TLS"; then
            ok=$((ok+1))
        else
            bad=$((bad+1))
            echo "BAD $o"
        fi
    done < "$1"
    echo "COUNT $ok $bad"
' sh "$tls_probe_dir/objects.txt")

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

rm -rf "$tls_probe_dir"
echo "check-tls-relocs: OK ($ok object(s) use TLS, all with relocations," \
     "${#objs[@]} scanned: ${#loose[@]} loose + members of $expanded" \
     "archive(s) with none)"
exit 0
