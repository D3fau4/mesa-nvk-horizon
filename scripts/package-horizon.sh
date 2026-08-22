#!/usr/bin/env bash
# Collects the built .nro files, libhorizon_gpu.a, libhorizon_compat.a,
# the horizon_gpu public headers and, when this build has one, the
# seventeen NVK driver archives, into one directory with a manifest that
# ties each artefact to the toolchain that produced it.
#
#   scripts/package-horizon.sh [outdir]     # default: build/pkg
#
# The libraries and headers are what a project consuming horizon_gpu (or
# the NVK driver itself) actually links against — the .nro are this
# tree's own tests, not something another project embeds. All of it
# travels in the same package because it is one build: a .a pulled from
# a different run than the headers it was compiled against, or than the
# other archives it links against, is exactly the kind of mismatch the
# build id and the manifest exist to make detectable rather than silent.
#
# libhorizon_gpu.a and libhorizon_compat.a are on every build this
# script will accept at all (both build paths produce them before a
# single test); the NVK archives are not (docs/BUILDING.md §4) and are
# included only when $MESA_NVK_BUILD_DIR actually holds them.
#
# The point of the manifest is the Phase 2 goal itself: when a .nro is
# copied to an SD card and run on console, the result recorded in
# STATUS.md has to be attributable to an exact toolchain. A sha256 next
# to the pinned devkitA64/libnx/image versions is what makes "the ten
# tests passed" a statement about a specific build — and the same
# attribution is what tells a consuming project which commit's ABI a
# copy of libhorizon_gpu.a actually implements.
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
_nvk_tests=$(sed -n '/^nvk_tests = \[/,/^]/p' meson.build |
             grep -o "'t_[a-z_0-9]*'" | tr -d "'")
if [ -z "$_nvk_tests" ]; then
    echo "error: no nvk_tests list found in meson.build; the staleness" \
         "gate cannot tell which artefacts link the driver, and a gate" \
         "that checks nothing must not report success" >&2
    exit 1
fi
# AND THE OTHER DIRECTION, WHICH COST A HARDWARE RUN: the build's own
# success stamp, and no Mesa source newer than it.
#
# The gate and the whole story behind it now live in
# scripts/toolchain-env.sh, unchanged and with their `[ -d mesa/src ]`
# guard, because scripts/package-portlibs.sh became a second caller that
# has to ask exactly the same question before it installs these archives
# into a toolchain prefix. Two copies of a gate are two gates that can
# drift, and this one already cost a hardware run once.
horizon_nvk_build_stamp_gate || exit 1

# BOTH ARCHIVES, NOT WHICHEVER SURVIVED. A partial build that leaves
# only one of them used to be accepted: the loop below `continue`d past
# the missing one, the "no NVK archives" branch did not fire either, and
# an old .nro could be validated against libnvk.a alone while also
# linking WSI. Reported by Codex on PR #8.
_required_libs="$MESA_NVK_BUILD_DIR/src/nouveau/vulkan/libnvk.a
$MESA_NVK_BUILD_DIR/src/vulkan/wsi/libvulkan_wsi.a"
_missing=""
for _lib in $_required_libs; do
    [ -f "$_lib" ] || _missing="$_missing $_lib"
done
# The stamp is re-tested here rather than remembered from the gate
# above: it used to be a $_stamp variable set inside that block and read
# out here, so moving the block into a function would have left this
# line reading an unset variable — an immediate `set -eu` abort, not a
# subtle change.
if [ -n "$_missing" ] &&
   [ -f "$MESA_NVK_BUILD_DIR/.horizon-build-ok" ]; then
    echo "error: a successful build is recorded but these archives the" \
         "tests link are missing:$_missing" >&2
    exit 1
fi

_newest_lib=""
for _lib in "$MESA_NVK_BUILD_DIR/src/nouveau/vulkan/libnvk.a" \
            "$MESA_NVK_BUILD_DIR/src/vulkan/wsi/libvulkan_wsi.a"; do
    [ -f "$_lib" ] || continue
    if [ -z "$_newest_lib" ] || [ "$_lib" -nt "$_newest_lib" ]; then
        _newest_lib="$_lib"
    fi
done

if [ -z "$_newest_lib" ]; then
    echo "package-horizon: no NVK archives in $MESA_NVK_BUILD_DIR, so nothing here" \
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
    # ITS OWN RULE, APPLIED TO ITS OWN COUNTER. This refuses outright
    # when the nvk_tests list is empty, on the stated principle that a
    # gate which checks nothing must not report success — and then
    # printed "0 driver-linked artefact(s) checked" and exited 0, which
    # is what happens whenever it falls back to the Makefile output in
    # build/. Raised in review of PR #8.
    if [ "$_checked" -eq 0 ]; then
        echo "error: the NVK archives exist but none of the tests that" \
             "link them ($_nvk_tests) is in $SRC, so this checked nothing." \
             "Build the driver-linked tests and package again, or package" \
             "a directory that has them." >&2
        exit 1
    fi
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

# BUILD IDENTITY, read out of the binaries rather than out of the tree.
#
# Every .nro carries the stamp scripts/gen-build-id.sh generated for the
# build that produced it (testfw.c prints it as the second line of every
# log). Reading it back here answers two different questions that the
# sha256 list cannot:
#
#   - which build these artefacts are, in a form the operator can match
#     against the first lines of the log they send back;
#   - whether they are all the SAME build. A directory holding two
#     stamps is a mix — some tests rebuilt, some left over — and a
#     manifest that attributes the whole set to one toolchain would be
#     lying about part of it.
#
# The marker is grepped, not the date pattern alone: "horizon-build-id "
# is the same token the log line starts with, so what this reads and
# what the operator reads are the same bytes.
#
# Read from $SRC, and every gate below runs before anything is copied.
# They used to run after: the manifest was already published and the
# rejected .nro were already in the output directory, so a later manual
# copy of that directory looked like a valid package and attributed
# exactly the stale binaries the gates exist to stop. Reported by Codex
# on PR #8, and right.
#
# NO `strings`, because it is not everywhere. Git Bash on Windows ships
# no binutils, so `strings -a` was "command not found" for every
# artefact and this script then reported the one thing it must never
# report wrongly: "no build id in: <all 31 of them>". The stamp was in
# each of those binaries the whole time.
#
# `grep -a -o` reads the same bytes with a tool POSIX requires. The
# marker is anchored the same way it was — it is the token the log line
# starts with, so what this reads and what the operator reads stay the
# same bytes.
#
# LC_ALL=C IS NOT TIDINESS, AND THE CLASS IS NOT `[!-~]`. Both were
# found by packaging from inside the toolchain image, which sets
# LANG=en_US.UTF-8:
#
#   - In a UTF-8 locale a range expression is read by collation order,
#     not by code point, so `[!-~]` matched *nothing* after the marker
#     and this printed "no build id in:" over all 33 artefacts — the one
#     thing the comment above says it must never report wrongly, for the
#     second time and from the other direction.
#
#   - `[!-~]` excludes the space, so even where it worked it stopped at
#     the first one. The stamp is three fields:
#         2026-08-10T15:26:39.381Z a228127-dirty mesa:7abd4c2
#     and everything from the repository commit onwards was being
#     dropped — exactly the fields scripts/gen-build-id.sh exists to
#     carry. `[ -~]` is printable ASCII including the space, and the
#     stamp is a NUL-terminated C string, so the NUL ends the match.
#
# Verified against the embedded stamp: what comes out equals what
# `strings` shows, in both locales.
_pkg_ids=""
_pkg_unstamped=""
for nro in "$SRC"/*.nro; do
    [ -e "$nro" ] || continue
    _id=$(LC_ALL=C grep -a -o 'horizon-build-id [ -~]*' "$nro" 2>/dev/null |
              head -1 | sed 's/^horizon-build-id //')
    if [ -z "$_id" ]; then
        _pkg_unstamped="$_pkg_unstamped $(basename "$nro")"
    else
        _pkg_ids="$_pkg_ids$_id
"
    fi
done
_pkg_id_count=$(printf '%s' "$_pkg_ids" | sort -u | grep -c . || true)
_pkg_id=$(printf '%s' "$_pkg_ids" | sort -u | tr '\n' ' ' | sed 's/ *$//')

# Before the manifest is written, not after: a manifest naming one build
# over a directory holding two is exactly the claim this refuses to make.
if [ -n "$_pkg_unstamped" ]; then
    echo "error: no build id in:$_pkg_unstamped" >&2
    echo "       Every .nro links testfw, which embeds one. An artefact" \
         "without it cannot be told apart from the copy it replaced on" \
         "an SD card, which is the failure this field exists to stop." >&2
    exit 1
fi
if [ "$_pkg_id_count" -gt 1 ]; then
    echo "error: $_pkg_id_count different build ids in $SRC: $_pkg_id" >&2
    echo "       These artefacts are not one build. Rebuild (both paths:" \
         "scripts/build-mesa-nvk.sh covers the driver-linked tests) and" \
         "package again." >&2
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

# THE LIBRARY ITSELF, NOT JUST THE TESTS THAT LINK IT.
#
# libhorizon_gpu.a lands at $SRC's root either way: the Makefile builds
# it at build/libhorizon_gpu.a, and meson.build's static_library('horizon_gpu', ...)
# is declared at the top level, so it comes out at
# $HORIZON_BUILD_DIR/libhorizon_gpu.a — the same directory $SRC already
# points at, whichever path produced it.
#
# Required, not optional: both build paths produce it before they build
# a single test (the tests link it), so its absence here means $SRC does
# not hold a real build — packaging binaries without the library the
# whole package exists to distribute would be the silent kind of
# incomplete this project's gates exist to refuse.
_pkg_gpu_lib="$SRC/libhorizon_gpu.a"
[ -f "$_pkg_gpu_lib" ] || {
    echo "error: no libhorizon_gpu.a in $SRC; a real build produces one" \
         "before it builds any test that links it. Run" \
         "scripts/build-horizon.sh or scripts/build-switch.sh first." >&2
    exit 1
}
[ -f "$HORIZON_COMPAT_LIBDIR/libhorizon_compat.a" ] || {
    echo "error: no libhorizon_compat.a in $HORIZON_COMPAT_LIBDIR;" \
         "run scripts/build-compat.sh first." >&2
    exit 1
}
mkdir -p "$OUT/lib"
cp "$_pkg_gpu_lib" "$OUT/lib/libhorizon_gpu.a"
cp "$HORIZON_COMPAT_LIBDIR/libhorizon_compat.a" "$OUT/lib/libhorizon_compat.a"

# THE HEADERS THAT LIBRARY IS COMPILED AGAINST, not whatever copy a
# consumer's own tree happens to have. horizon/include/horizon_gpu/ is
# the only public header surface (CLAUDE.md's target architecture) —
# re-staged wholesale, the same way scripts/build-mesa-nvk.sh re-stages
# it before every NVK build, rather than diffed file by file: it is a
# handful of small headers, not something worth the idempotent-copy
# bookkeeping the .nro loop above pays for because testfw.o (and so
# every .nro) really does change on every single build.
rm -rf "$OUT/include"
mkdir -p "$OUT/include"
cp -a horizon/include/horizon_gpu "$OUT/include/horizon_gpu"

# THE NVK DRIVER ITSELF, WHEN THIS BUILD ACTUALLY HAS ONE.
#
# Unlike libhorizon_gpu.a, this is not on every build: the Makefile-only
# path never configures Mesa at all, and even the Meson path can be
# configured without NVK (docs/BUILDING.md §4). $_newest_lib, set by the
# staleness gate above, is already this script's own answer to "does
# $MESA_NVK_BUILD_DIR hold a real build" — asked again here would be a
# second place that question could drift from the first.
#
# $HORIZON_NVK_TEST_LIBS is the same list scripts/ci-build-archives.sh
# and scripts/check-mesa-test-parity.sh already read out of
# scripts/toolchain-env.sh — the seventeen archives meson.build's own
# nvk_whole_libs/nvk_test_libs link the t_vk_* tests against. Copied
# under their own relative path, not flattened to a bare basename:
# src/util/libmesa_util.a exists in BOTH $MESA_BUILD_DIR (the minimal
# probe build tests 12/13 link) and here — a different file built by a
# different configure — and flattening the two would silently let one
# overwrite the other.
#
# ALL SEVENTEEN OR NONE. A build that produced some of them and not
# others is not a build this refuses elsewhere to trust (the staleness
# gate above already requires both libnvk.a and libvulkan_wsi.a before
# $_newest_lib is set at all) — but asking again here, over the full
# list rather than the two representative archives that gate checks, is
# what stops a package from shipping twelve of seventeen and calling it
# the driver.
#
# The check itself is horizon_nvk_archive_gate in
# scripts/toolchain-env.sh, shared with scripts/package-portlibs.sh. The
# `if [ -n "$_newest_lib" ]` around it stays HERE and was deliberately
# not moved with it: this script's answer to "no NVK build at all" is to
# omit lib/nvk/ and still succeed, which docs/BUILDING.md §5 documents,
# while an install with no driver in it is not an install and refuses.
# That difference is the caller's, so the condition is the caller's.
if [ -n "$_newest_lib" ]; then
    horizon_nvk_archive_gate || exit 1
    rm -rf "$OUT/lib/nvk"
    mkdir -p "$OUT/lib/nvk"
    for _lib in $HORIZON_NVK_TEST_LIBS; do
        mkdir -p "$OUT/lib/nvk/$(dirname "$_lib")"
        cp "$MESA_NVK_BUILD_DIR/$_lib" "$OUT/lib/nvk/$_lib"
    done
    echo "package-horizon: $(printf '%s\n' $HORIZON_NVK_TEST_LIBS | wc -l)" \
         "NVK driver archive(s) staged under $OUT/lib/nvk/"
else
    rm -rf "$OUT/lib/nvk"
    echo "package-horizon: no NVK build in $MESA_NVK_BUILD_DIR;" \
         "lib/nvk/ omitted (the .nro-only package is still complete for" \
         "horizon_gpu itself)"
fi

# THE LICENCE TRAVELS WITH THE BINARIES.
#
# This directory is what gets tarred into a release and copied onto an
# SD card, and MIT is explicit that "the above copyright notice and this
# permission notice shall be included in all copies or substantial
# portions of the Software" — a distribution of built artefacts is a
# copy. Packaging them without it published the software while omitting
# the one condition its licence attaches. Found in review of PR #10.
#
# LICENSES/README.md goes with it because our MIT text is not the whole
# picture: every .nro links libnx, which is ISC, and that file is where
# the third-party position is recorded. libnx ships no licence file in
# $DEVKITPRO/libnx for us to copy — checked in the image — so this
# points at the component and its licence rather than inventing the
# text of one.
for _pkg_lic in LICENSE LICENSES/README.md; do
    [ -f "$_pkg_lic" ] || {
        echo "error: $_pkg_lic is missing; refusing to package binaries" >&2
        echo "       without the licence they are distributed under." >&2
        exit 1
    }
done
cp LICENSE "$OUT/LICENSE"
cp LICENSES/README.md "$OUT/LICENSES.md"

manifest="$OUT/MANIFEST.txt"
tmp="$manifest.tmp.$$"

# Once, not per use. It shells out to `docker image inspect`, and it was
# called twice inside this one manifest — two independent answers that
# can disagree, in a file whose entire purpose is saying which single
# toolchain produced these artefacts.
_pkg_img=$(horizon_image_digest)

{
    echo "mesa-nvk-horizon — packaged horizon_gpu library and test artefacts"
    echo
    echo "built from : $SRC_DESC"
    echo "toolchain  : $HORIZON_TOOLCHAIN_DESC"
    echo "image      : $_pkg_img"
    echo "build id   : ${_pkg_id:-(none of these artefacts carries one)}"
    echo
    echo "# The build id is printed by every test as the second line of"
    echo "# its log:"
    echo "#"
    echo "#   note horizon-build-id ${_pkg_id:-<stamp>}"
    echo "#"
    echo "# A log whose stamp is not that one came from other binaries."
    echo "# It is read here out of the .nro themselves, not out of the"
    echo "# source tree, so it describes what is in this directory."
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
    in-image:*)
        # Built by a job whose container was the toolchain itself. There
        # is no docker in there to ask for a digest, so the image was
        # asked instead: toolchain/Dockerfile records the identity it was
        # built under, and that is what this carries. It says how the
        # toolchain was built rather than where it was pushed, which is
        # the more useful of the two for reproducing a hardware result.
        _pkg_in=${_pkg_img#in-image:}
        echo "#   (built inside the toolchain image, derived from"
        echo "#    ${_pkg_in%%#*}."
        echo "#    Its build identity is ${_pkg_in#*#}."
        echo "#    Rebuild that image with scripts/build-toolchain-image.sh;"
        echo "#    it is current when its identity matches this string.)"
        unset _pkg_in
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
    echo "# Library (sha256) — link against this and include/horizon_gpu/"
    (cd "$OUT" && sha256sum ./lib/*.a | sort -k2)
    echo
    if [ -d "$OUT/lib/nvk" ]; then
        echo "# NVK driver archives (sha256) — Mesa's own build products, linked"
        echo "# exactly as meson.build's nvk_whole_libs/nvk_test_libs and the"
        echo "# t_vk_* tests already do; see those for the link order and group"
        echo "# semantics (--start-group/--end-group is load-bearing here)."
        (cd "$OUT" && find lib/nvk -name '*.a' -exec sha256sum {} + | sort -k2)
        echo
    fi
    echo "# Test artefacts (sha256)"
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

echo "package-horizon: $OUT — $copied .nro copied, $kept already current," \
     "$dropped dropped, libhorizon_gpu.a + libhorizon_compat.a + headers" \
     "staged, manifest $manifest_state"
