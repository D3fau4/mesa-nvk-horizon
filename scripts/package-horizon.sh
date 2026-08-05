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

# Once, not per use. It shells out to `docker image inspect`, and it was
# called twice inside this one manifest — two independent answers that
# can disagree, in a file whose entire purpose is saying which single
# toolchain produced these artefacts.
_pkg_img=$(horizon_image_digest)

{
    echo "mesa-nvk-horizon — packaged .nro artefacts"
    echo
    echo "built from : $SRC_DESC"
    echo "toolchain  : $HORIZON_TOOLCHAIN_DESC"
    echo "image      : $_pkg_img"
    echo
    echo "# Switch toolchain, as READ from the environment at packaging"
    echo "# time. This project neither pins nor updates it — libnx and"
    echo "# devkitA64 are whatever \$DEVKITPRO or the image provides."
    echo "# Recording it here is what makes a hardware result measured"
    echo "# with these .nro attributable to a specific build."
    echo "#"
    echo "# Rebuild these exact artefacts against the same toolchain:"
    # Three ways this can end, and only one of them is a command. The
    # field's entire job is attribution, so a reference that cannot be
    # resolved is worse than an honest refusal to print one — which is
    # what the old <base-repo>@unknown was, for the derived image that
    # nearly every build since Phase 4 has actually used.
    case "$_pkg_img" in
    local)
        echo "#   (built against the local devkitA64 at \$DEVKITPRO; there is"
        echo "#    no image reference to reproduce it. The versions below are"
        echo "#    the whole record — a devkitPro install that has since been"
        echo "#    updated cannot be recovered from here.)"
        ;;
    local-image-id:*)
        echo "#   (built in ${HORIZON_IMAGE}, an image built here and never"
        echo "#    pushed, so there is no digest to pull: ${_pkg_img#local-image-id:}"
        echo "#    identifies it on the machine that built it and nowhere else."
        echo "#    Rebuild the image with scripts/build-toolchain-image.sh; the"
        echo "#    base it derives from is recorded above.)"
        ;;
    unknown)
        echo "#   (docker could not describe ${HORIZON_IMAGE}; the toolchain"
        echo "#    behind these artefacts is NOT recorded. Treat any hardware"
        echo "#    result measured with them as unattributed.)"
        ;;
    *)
        echo "#   HORIZON_NX_IMAGE=$_pkg_img \\"
        echo "#       scripts/build-horizon.sh"
        ;;
    esac
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

# STALENESS GATE: an artefact older than what it links is not this
# build's artefact.
#
# This manifest exists to attribute a .nro to one exact build, and a
# copy step that only looks at content cannot tell a current binary from
# one linked against the previous driver. The failure it is here to stop
# was real: scripts/build-mesa-nvk.sh built the tests BEFORE it built
# the archives they link, so every packaged Vulkan test was one driver
# build behind and the manifest said nothing about it.
#
# Compared by modification time against the NVK archives, and only for
# the tests that link them — meson.build's own nvk_tests list, read
# here rather than guessed from a name prefix, so a test added there is
# covered without anyone remembering this file. The horizon_gpu tests
# link none of it and are legitimately older.
#
# A tree with no NVK build has nothing to compare against and this says
# so rather than passing quietly.
_nvk_dir="${MESA_NVK_BUILD_DIR:-build/mesa-nvk}"
_nvk_tests=$(sed -n '/^nvk_tests = \[/,/^]/p' meson.build |
             grep -o "'t_[a-z_0-9]*'" | tr -d "'")
if [ -z "$_nvk_tests" ]; then
    echo "error: no nvk_tests list found in meson.build; the staleness" \
         "gate cannot tell which artefacts link the driver, and a gate" \
         "that checks nothing must not report success" >&2
    exit 1
fi
_newest_lib=""
for _lib in "$_nvk_dir/src/nouveau/vulkan/libnvk.a" \
            "$_nvk_dir/src/vulkan/wsi/libvulkan_wsi.a"; do
    [ -f "$_lib" ] || continue
    if [ -z "$_newest_lib" ] || [ "$_lib" -nt "$_newest_lib" ]; then
        _newest_lib="$_lib"
    fi
done

if [ -z "$_newest_lib" ]; then
    echo "package-horizon: no NVK archives in $_nvk_dir, so nothing here" \
         "links them and there is no staleness to check" >&2
else
    _stale=0
    _checked=0
    for _t in $_nvk_tests; do
        # The artefact in the BUILD directory, not the packaged copy.
        # Copying here is idempotent — a file whose content already
        # matches is left alone, mtime included — so comparing the
        # packaged copy would report a build that was correctly rebuilt
        # and produced identical bytes as permanently stale. The
        # question this gate asks is about what is being packaged.
        _nro="$SRC/$_t.nro"
        [ -f "$_nro" ] || continue
        _checked=$((_checked + 1))
        if [ "$_newest_lib" -nt "$_nro" ]; then
            echo "error: $_t.nro is older than $_newest_lib" >&2
            _stale=$((_stale + 1))
        fi
    done
    echo "package-horizon: $_checked driver-linked artefact(s) checked" \
         "against $_newest_lib"
    if [ "$_stale" -gt 0 ]; then
        echo "package-horizon: $_stale artefact(s) predate the driver they" \
             "link. Run scripts/build-horizon.sh and package again; a" \
             "manifest over stale binaries attributes a hardware result to" \
             "the wrong build." >&2
        exit 1
    fi
fi

echo "package-horizon: $OUT — $copied copied, $kept already current," \
     "$dropped dropped, manifest $manifest_state"
