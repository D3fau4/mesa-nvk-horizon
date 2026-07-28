#!/usr/bin/env bash
# Collects the built .nro files into one directory with a manifest that
# ties each artefact to the toolchain that produced it.
#
#   scripts/package-horizon.sh [outdir]     # default: build/pkg
#
# The point of the manifest is the Phase 2 goal itself: when a .nro is
# copied to an SD card and run on console, the result recorded in
# STATUS.md has to be attributable to an exact toolchain. A sha256 next
# to the pinned devkitA64/libnx/image versions is what makes "the ten
# tests passed" a statement about a specific build.
#
# Idempotent: artefacts are copied only when their content differs, and
# the manifest is rewritten only when it changes.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

OUT="${1:-build/pkg}"

# Prefer the Meson output; fall back to the Makefile's. Both are
# supported build paths and either may be the one that was just run.
if [ -n "$(find "$HORIZON_BUILD_DIR" -maxdepth 1 -name '*.nro' 2>/dev/null)" ]; then
    SRC="$HORIZON_BUILD_DIR"
    SRC_DESC="meson ($HORIZON_BUILD_DIR)"
elif [ -n "$(find build -maxdepth 1 -name '*.nro' 2>/dev/null)" ]; then
    SRC=build
    SRC_DESC="makefile (build)"
    # Say so. Falling back silently here once packaged stale Makefile
    # artefacts right after a Meson configure had failed, which reads as
    # a successful build of something it never built.
    echo "package-horizon: no .nro in $HORIZON_BUILD_DIR;" >&2
    echo "                 falling back to the Makefile output in build/" >&2
else
    echo "error: no .nro found; run scripts/build-horizon.sh or" >&2
    echo "       scripts/build-switch.sh first." >&2
    exit 1
fi

mkdir -p "$OUT"
copied=0
kept=0
dropped=0

# Anything in the destination that the source no longer has, before
# copying. The manifest below hashes every .nro *in $OUT*, and its whole
# job is attributing an artefact to one build — so a leftover from a
# previous packaging run would be listed under this run's toolchain and
# image digest.
#
# It is not hypothetical: tests 12 and 13 exist only while Mesa's
# archives do. Package 13, remove the archives, build again (the
# Makefile now prunes the two stale .nro from build/), package again —
# without this, $OUT keeps the previous run's t_threads.nro and
# t_ostime.nro and the new manifest claims them.
for old in "$OUT"/*.nro; do
    [ -e "$old" ] || continue
    name=$(basename "$old")
    if [ ! -f "$SRC/$name" ]; then
        echo "package-horizon: dropping $name — not in $SRC"
        rm -f "$old"
        dropped=$((dropped + 1))
    fi
done

for nro in "$SRC"/*.nro; do
    name=$(basename "$nro")
    if [ -f "$OUT/$name" ] && cmp -s "$nro" "$OUT/$name"; then
        kept=$((kept + 1))
    else
        cp "$nro" "$OUT/$name"
        copied=$((copied + 1))
    fi
done

manifest="$OUT/MANIFEST.txt"
tmp="$manifest.tmp.$$"

{
    echo "mesa-nvk-horizon — packaged .nro artefacts"
    echo
    echo "built from : $SRC_DESC"
    echo "toolchain  : $HORIZON_TOOLCHAIN_DESC"
    echo "image      : $(horizon_image_digest)"
    echo
    echo "# Switch toolchain, as READ from the environment at packaging"
    echo "# time. This project neither pins nor updates it — libnx and"
    echo "# devkitA64 are whatever \$DEVKITPRO or the image provides."
    echo "# Recording it here is what makes a hardware result measured"
    echo "# with these .nro attributable to a specific build."
    echo "#"
    echo "# Rebuild these exact artefacts against the same toolchain:"
    # In local mode there is no image reference to hand back — printing
    # repo@local would be a command that cannot work, in the one field
    # whose entire job is attribution.
    if [ "$(horizon_image_digest)" = "local" ]; then
        echo "#   (built against the local devkitA64 at \$DEVKITPRO; there is"
        echo "#    no image reference to reproduce it. The versions below are"
        echo "#    the whole record — a devkitPro install that has since been"
        echo "#    updated cannot be recovered from here.)"
    else
        echo "#   HORIZON_NX_IMAGE=${HORIZON_NX_IMAGE_REPO}@$(horizon_image_digest) \\"
        echo "#       scripts/build-horizon.sh"
    fi
    echo
    scripts/print-toolchain-versions.sh 2>/dev/null | sed 's/^/  /'
    echo
    echo "# Pinned inputs"
    echo "  mesa    : $MESA_TAG ($MESA_COMMIT)"
    echo "  meson   : $MESON_VERSION"
    echo
    echo "# Artefacts (sha256)"
    # Sorted so the manifest is stable across runs and diffable.
    (cd "$OUT" && sha256sum ./*.nro | sort -k2)
} > "$tmp"

if [ -f "$manifest" ] && cmp -s "$tmp" "$manifest"; then
    rm -f "$tmp"
    manifest_state="unchanged"
else
    mv "$tmp" "$manifest"
    manifest_state="written"
fi

echo "package-horizon: $OUT — $copied copied, $kept already current," \
     "$dropped dropped, manifest $manifest_state"
