#!/usr/bin/env bash
# Turns thin archives into archives that carry their objects.
#
#   scripts/fatten-archives.sh <jobfile>
#
# The job file has one `<source>\t<destination>` pair per line, both
# relative to the repository root. Run it through horizon_run, which is
# what puts the cross toolchain's ar on PATH in both modes:
#
#   horizon_run bash scripts/fatten-archives.sh build/install-tmp.$$/jobs
#
# WHY THIS EXISTS. Meson writes THIN archives: a list of paths to the
# objects, not the objects. Measured on this tree, sixteen of the
# seventeen NVK archives open with `!<thin>`, and libnvk.a is 96 KiB
# naming 179 members it does not contain — scripts/ci-build-archives.sh
# says the same thing in its own comment. Copied anywhere else, every
# one of those paths is wrong, and the failure lands at the consumer's
# link, in their build, about our file:
#
#   error opening thin archive member
#
# That is not hypothetical, it is a bug this repository already has in
# two places. STATUS.md records build/pkg/lib/libhorizon_gpu.a shipping
# thin, worked around once by hand-copying the .a.p directory. And
# build/toolchain/horizon-gpu/lib/libhorizon_gpu.a, staged by
# configure-mesa.sh, configure-mesa-nvk.sh and build-mesa-nvk.sh, is
# already broken today — `ar t` on it answers "No such file or
# directory". It is harmless only because nothing links it yet.
#
# ONE CONTAINER INVOCATION, AND THE LIST IN A FILE. Both are
# scripts/check-tls-relocs.sh's lessons, arrived at the same way: in
# container mode horizon_run starts a docker run per call, so a
# container per archive turns seconds into minutes; and a list passed as
# arguments hits the 32767-character command line Windows gives a native
# binary, after which docker.exe never runs at all. The file lives in
# the tree, which horizon_run mounts at the same path on both sides.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# DEVKITA64_TOOL_PREFIX only. The tools themselves are resolved by name
# off the PATH horizon_run sets, exactly as check-tls-relocs.sh resolves
# aarch64-none-elf-ar and -nm, so this works unchanged in both modes.
. toolchain/versions.env

# gcc-ar and gcc-nm rather than ar and nm: they pass --plugin, so an LTO
# object's symbols reach the index instead of quietly not being in it.
# Same binary the Makefile already calls $(AR).
FA_AR="${DEVKITA64_TOOL_PREFIX}gcc-ar"
FA_NM="${DEVKITA64_TOOL_PREFIX}gcc-nm"

if [ "$#" -ne 1 ] || [ ! -f "$1" ]; then
    echo "usage: scripts/fatten-archives.sh <jobfile>" >&2
    echo "       one source and destination per line, tab-separated" >&2
    exit 1
fi
FA_JOBS="$1"

# Up front, because the failure otherwise arrives disguised. Without
# this, a missing ar makes `ar t` print "command not found" and produce
# nothing, and the empty-member-list check below then blames the build
# for objects that are on disk the whole time. Observed exactly that
# with $DEVKITPRO=/opt/devkitpro under Git Bash, where the toolchain is
# reachable only from devkitPro's own shell.
for FA_TOOL in "$FA_AR" "$FA_NM"; do
    command -v "$FA_TOOL" >/dev/null 2>&1 || {
        echo "error: $FA_TOOL is not on PATH." >&2
        echo "       Run this through horizon_run, which puts the cross" \
             "toolchain there in both modes; if it already is, then" \
             "\$DEVKITPRO does not name a devkitA64 this shell can see." >&2
        exit 1
    }
done
unset FA_TOOL

FA_WORK="build/fatten-tmp.$$"
# Assigned before the trap is installed and the directory created after
# it, in that order: under `set -u` a trap that fires before the
# variable exists is an error inside an error path.
trap 'rm -rf "$FA_WORK"' EXIT INT TERM
mkdir -p "$FA_WORK"

# `ar t`/`ar tv` under Git Bash come back CRLF-terminated — devkitA64's
# ar is a native Windows binary. THIS IS LOAD-BEARING, not tidiness:
# without it every member path carries a trailing carriage return, so
# all 179 of libnvk.a's look like files that do not exist, and the
# fattening below would produce an empty archive and exit 0. Measured
# with od -c.
fa_members() { "$FA_AR" t "$1" | tr -d '\r'; }

# (basename, size) for every member, sorted. The thin side prints full
# paths and the fat side bare names, so the directory comes off both.
# Dates and modes are not compared: -D changed them on purpose.
fa_sizes() {
    "$FA_AR" tv "$1" | tr -d '\r' |
        awk '{ n = $NF; sub(/.*\//, "", n); print n, $3 }' | LC_ALL=C sort
}

# The symbol index a linker actually uses. `nm --print-armap` prints one
# "<symbol> in <member>" line per index entry; the member half comes off
# because the fat archive names its members differently by construction.
# No C symbol contains a space, so " in " appears only in those lines.
fa_armap() {
    "$FA_NM" --print-armap "$1" 2>/dev/null | tr -d '\r' |
        sed -n 's/ in .*$//p' | LC_ALL=C sort -u
}

fa_copied=0
fa_fattened=0
fa_current=0

while IFS="$(printf '\t')" read -r fa_src fa_dst; do
    [ -n "${fa_src:-}" ] || continue
    [ -n "${fa_dst:-}" ] || {
        echo "error: no destination for $fa_src in $FA_JOBS" >&2
        exit 1
    }
    [ -f "$fa_src" ] || {
        echo "error: $fa_src does not exist" >&2
        exit 1
    }
    mkdir -p "$(dirname "$fa_dst")"

    # `!<thin>` is what GNU ar writes at the head of a thin archive and
    # `!<arch>` at the head of an ordinary one. Seven bytes, because the
    # eighth is the newline and there is no reason to carry it through a
    # command substitution.
    #
    # DETECTED, NOT LISTED. On this build libnouveau_rust_runtime.a is
    # real (rustc wrote it, not Meson) and so is libhorizon_compat.a,
    # but which archive is which is a property of whoever built it, not
    # a fact about this repository.
    fa_magic=$(head -c 7 "$fa_src")
    if [ "$fa_magic" = '!<arch>' ]; then
        if [ -f "$fa_dst" ] && cmp -s "$fa_src" "$fa_dst"; then
            fa_current=$((fa_current + 1))
        else
            cp "$fa_src" "$fa_dst"
            fa_copied=$((fa_copied + 1))
        fi
        continue
    fi
    if [ "$fa_magic" != '!<thin>' ]; then
        echo "error: $fa_src is not an archive (starts '$fa_magic')" >&2
        exit 1
    fi

    fa_members "$fa_src" > "$FA_WORK/members"
    # An empty list means `ar t` failed, not that the archive is empty —
    # and an archive of nothing would satisfy every check below by being
    # consistently nothing.
    if ! grep -q . "$FA_WORK/members"; then
        echo "error: $fa_src names no members. Either ar could not read" \
             "it or the objects it points at are gone; scripts/build-mesa-nvk.sh" \
             "is what produces them." >&2
        exit 1
    fi
    # A response file is whitespace-separated (libiberty's expandargv),
    # so a member path with a space in it would be read as two paths,
    # both missing. None of the ~800 members of this build has one, and
    # that is a measurement of this build rather than a property of
    # Meson.
    if grep -q '[[:space:]"'\''\\]' "$FA_WORK/members"; then
        echo "error: a member path in $fa_src contains whitespace or a" \
             "quote, which an ar response file cannot express." >&2
        exit 1
    fi
    fa_missing=$(while IFS= read -r m; do
                     [ -f "$m" ] || echo "$m"
                 done < "$FA_WORK/members" | head -5)
    if [ -n "$fa_missing" ]; then
        echo "error: $fa_src points at members that are not there:" >&2
        echo "$fa_missing" | sed 's/^/         /' >&2
        echo "       Its .a.p directory was removed. Rebuild with" \
             "scripts/build-mesa-nvk.sh." >&2
        exit 1
    fi

    # `q`, NOT `r`. `ar r` replaces an existing member of the same name
    # and matches on the BASENAME — binutils' normalize() returns
    # lbasename() unless P is given — so two objects that differ only in
    # their directory collapse into one, silently, and the archive links
    # less than it says it holds. libnvk.a pulls 67 of its 179 members
    # from ../../vulkan/runtime/libvulkan_lite_runtime.a.p/, so members
    # from several directories in one archive is the normal case here,
    # not an edge. `q` is quick append: it never searches and never
    # replaces. Two members may then share a name, which the linker does
    # not care about — it resolves through the index below, not by name.
    #
    # `s` writes that index, `c` suppresses the creation notice, and `D`
    # zeroes timestamps, uids, gids and modes. D IS NOT THE DEFAULT
    # HERE: `ar --help` of this binutils (2.46) lists "[U] use actual
    # timestamps and uids/gids (default)". Without it every run produces
    # different bytes, the cmp below never matches, and reinstalling
    # rewrites hundreds of MiB that differ only in a clock reading.
    # Measured with it: two runs over libnvk.a are byte-identical.
    #
    # The destination is removed first because `q` appends. Left in
    # place, a second run would hand the consumer an archive with every
    # member twice.
    rm -f "$FA_WORK/fat.a"
    "$FA_AR" qcsD "$FA_WORK/fat.a" "@$FA_WORK/members"

    # PROVE IT, rather than trust an exit status. Each of these
    # disproves one specific failure: the size multiset catches a member
    # replaced by another of the same name (the `r` failure above) and a
    # member truncated on the way in, and the index catches an `s` that
    # did not do what it was passed for. An unverified flag is a flag
    # nobody has checked.
    fa_sizes "$fa_src"        > "$FA_WORK/sz.thin"
    fa_sizes "$FA_WORK/fat.a" > "$FA_WORK/sz.fat"
    if ! cmp -s "$FA_WORK/sz.thin" "$FA_WORK/sz.fat"; then
        echo "error: fattening $fa_src changed its members:" >&2
        diff "$FA_WORK/sz.thin" "$FA_WORK/sz.fat" | head -10 | \
            sed 's/^/         /' >&2
        exit 1
    fi
    fa_armap "$fa_src"        > "$FA_WORK/ix.thin"
    fa_armap "$FA_WORK/fat.a" > "$FA_WORK/ix.fat"
    if ! cmp -s "$FA_WORK/ix.thin" "$FA_WORK/ix.fat"; then
        echo "error: fattening $fa_src changed its symbol index:" >&2
        diff "$FA_WORK/ix.thin" "$FA_WORK/ix.fat" | head -10 | \
            sed 's/^/         /' >&2
        exit 1
    fi

    # cmp before replacing, so a rebuild that produced identical bytes
    # leaves the destination alone — mtime included. That is the whole
    # of "reinstalling changes nothing" for the expensive half.
    if [ -f "$fa_dst" ] && cmp -s "$FA_WORK/fat.a" "$fa_dst"; then
        fa_current=$((fa_current + 1))
    else
        cp "$FA_WORK/fat.a" "$fa_dst"
        fa_fattened=$((fa_fattened + 1))
    fi
done < "$FA_JOBS"

echo "fatten-archives: $fa_fattened fattened, $fa_copied copied whole," \
     "$fa_current already current"
