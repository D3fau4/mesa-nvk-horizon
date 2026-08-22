#!/usr/bin/env bash
# Builds the .tar.gz that installs the NVK driver into a devkitPro
# portlibs prefix, with a pkg-config file that makes it consumable.
#
#   scripts/package-portlibs.sh [outfile]
#       # default: build/mesa-nvk-horizon-portlibs.tar.gz
#
# The tarball's root IS the prefix, so it is extracted with
#
#   tar -xzf mesa-nvk-horizon-portlibs.tar.gz -C "$DEVKITPRO/portlibs/switch"
#
# and every file lands where pkg-config will find it. scripts/install-horizon.sh
# is that extraction plus the bookkeeping to undo it; `make install` is
# that script. CI runs THIS one and publishes what comes out, so there is
# one procedure and not two that can drift.
#
# HOW THIS DIFFERS FROM scripts/package-horizon.sh, which also makes a
# package: that one collects what goes to a console — .nro with a
# manifest attributing them to a build. This collects what a DIFFERENT
# PROJECT LINKS. Two artefacts, two audiences; neither is a superset of
# the other.
#
# THE ARCHIVES ARE FATTENED ON THE WAY IN. Meson writes thin archives
# and a copy of one does not link anywhere else; scripts/fatten-archives.sh
# carries the whole explanation. This is the difference between a
# package that works and build/pkg, which has shipped an unusable
# libhorizon_gpu.a since the beginning.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

OUT="${1:-build/mesa-nvk-horizon-portlibs.tar.gz}"

# The staging root is the prefix itself, not <root>/$PREFIX. Nothing
# here needs to know where it will be installed: the tarball is rooted
# at the prefix and nvk.pc locates itself with ${pcfiledir}, so $PREFIX
# is a fact about the extraction and not about the package. It also
# keeps every path in this script relative and free of whatever
# characters somebody's prefix contains.
STAGE=build/portlibs-stage
# Beside the staging tree rather than inside it: it must not end up in
# the tarball.
WORK=build/portlibs-work

# ---------------------------------------------------------------------
# GATES. All of them before anything is written.
#
# The order is scripts/package-horizon.sh's, and for its reason: once
# files are staged, a half-made directory looks exactly like a finished
# one, and the tarball made from it looks like a package.
# ---------------------------------------------------------------------

# Unlike package-horizon.sh, "there is no NVK build" is an error here
# and not a case. That script's .nro-only package is a complete thing
# without a driver; a driver package without a driver is not.
if [ ! -d "$MESA_NVK_BUILD_DIR" ]; then
    echo "error: $MESA_NVK_BUILD_DIR does not exist, so this tree has no" \
         "NVK driver to package. scripts/build-mesa-nvk.sh is what" \
         "produces it (docs/BUILDING.md §4)." >&2
    exit 1
fi
horizon_nvk_archive_gate || exit 1
horizon_nvk_build_stamp_gate || exit 1

# libhorizon_gpu.a FROM THE MESON BUILD AND NOWHERE ELSE.
#
# There are two of them and they are different libraries: the Makefile
# writes build/libhorizon_gpu.a and Meson writes
# $HORIZON_BUILD_DIR/libhorizon_gpu.a. The seventeen archives beside it
# here were built against the Meson one — build-mesa-nvk.sh and
# configure-mesa-nvk.sh stage exactly that copy into
# build/toolchain/horizon-gpu/lib/ for the driver build to compile
# against. Shipping the other one would put two builds of one library in
# a single package, which is the mismatch package-horizon.sh's whole
# header is about.
#
# So NO FALLBACK to build/. package-horizon.sh has one and is right to:
# for a .nro package either build path's output is the real thing. Here
# it would silently pick the archive the driver was not built against.
GPU_LIB="$HORIZON_BUILD_DIR/libhorizon_gpu.a"
[ -f "$GPU_LIB" ] || {
    echo "error: no libhorizon_gpu.a in $HORIZON_BUILD_DIR." >&2
    echo "       The NVK archives are built against that one, not" \
         "against the Makefile's build/libhorizon_gpu.a, so this will" \
         "not substitute it. Run scripts/build-horizon.sh." >&2
    exit 1
}
COMPAT_LIB="$HORIZON_COMPAT_LIBDIR/libhorizon_compat.a"
[ -f "$COMPAT_LIB" ] || {
    echo "error: no libhorizon_compat.a in $HORIZON_COMPAT_LIBDIR;" \
         "run scripts/build-compat.sh first." >&2
    exit 1
}

# THE LICENCE TRAVELS WITH THE BINARIES, for the reason
# package-horizon.sh states at length: MIT requires the notice in "all
# copies or substantial portions", a tarball of built artefacts is a
# copy, and LICENSES/README.md is where the third-party position
# (libnx's ISC) is recorded.
for _lic in LICENSE LICENSES/README.md; do
    [ -f "$_lic" ] || {
        echo "error: $_lic is missing; refusing to package binaries" >&2
        echo "       without the licence they are distributed under." >&2
        exit 1
    }
done

# The Vulkan headers a consumer compiles against. vk_video is not
# optional: mesa/include/vulkan/vulkan_core.h includes
# "vk_video/vulkan_video_codec_h264std.h" and friends, quoted, so they
# resolve against the same -I root and must sit beside vulkan/.
for _hdr_dir in mesa/include/vulkan mesa/include/vk_video; do
    [ -d "$_hdr_dir" ] || {
        echo "error: $_hdr_dir is missing. The Vulkan headers come from" \
             "the pinned Mesa checkout; run scripts/fetch-mesa.sh." >&2
        exit 1
    }
done

# WHICH ARCHIVES GET --whole-archive, read out of meson.build rather
# than assumed to be "the first one". $HORIZON_NVK_TEST_LIBS is
# documented as nvk_whole_libs followed by nvk_test_libs, so the split
# is recoverable — but recovering it by counting would put the number 1
# in this file, and a second whole-archived library would then be
# emitted without the flag it exists to get.
NVK_WHOLE=$(sed -n '/^nvk_whole_libs = \[/,/^]/p' meson.build |
            grep -o "'[^']*'" | tr -d "'")
if [ -z "$NVK_WHOLE" ]; then
    echo "error: no nvk_whole_libs list found in meson.build, so this" \
         "cannot tell which archives need --whole-archive — and one" \
         "emitted without it links to a NULL dispatch entry that nothing" \
         "reports. See meson.build's own comment." >&2
    exit 1
fi

# FLATTENING IS THIS FILE'S DECISION, SO IT IS CHECKED HERE.
# package-horizon.sh keeps the archives under their Mesa-relative paths
# and says why: src/util/libmesa_util.a exists in both $MESA_BUILD_DIR
# and $MESA_NVK_BUILD_DIR. Only the NVK ones are installed here, and
# their basenames are distinct today — but that is a measurement of this
# build, not something Meson guarantees, and two targets in different
# subdirectories may share a name.
_dupes=$(for _lib in $HORIZON_NVK_TEST_LIBS; do basename "$_lib"; done |
         LC_ALL=C sort | uniq -d)
if [ -n "$_dupes" ]; then
    echo "error: these archive names collide when flattened into" \
         "lib/nvk/: $(printf '%s ' $_dupes)" >&2
    echo "       Installing them would leave one overwriting the other." >&2
    exit 1
fi

# ---------------------------------------------------------------------
# STAGE
# ---------------------------------------------------------------------

# NOT `rm -rf "$STAGE"`. In container mode the archives below are
# written by root on the host side of the bind mount — the same
# condition build-mesa-nvk.sh:151-161 already documents for
# $MESA_NVK_BUILD_DIR — so a wipe would need privileges the caller does
# not have. Staging is idempotent instead: every writer compares before
# it writes, and anything no longer wanted is pruned by name at the end.
# `make clean` is what removes the tree wholesale.
mkdir -p "$WORK" "$STAGE/lib/nvk" "$STAGE/lib/pkgconfig" \
         "$STAGE/include/horizon_gpu" "$STAGE/include/nvk" \
         "$STAGE/licenses/mesa-nvk-horizon" "$STAGE/share/mesa-nvk-horizon"

# Copy only when the content differs, so a re-run touches nothing.
# install(1) is not used anywhere here for exactly that reason: it
# always writes, so every re-run would dirty every file and "installing
# twice changes nothing" could not be true.
stage_file() {
    if [ -f "$2" ] && cmp -s "$1" "$2"; then
        return 0
    fi
    cp "$1" "$2"
}

# The archives, in one pass through the toolchain. In container mode
# horizon_run costs a docker run per call, so nineteen calls would be
# nineteen containers; the job file is what makes it one.
: > "$WORK/jobs"
for _lib in $HORIZON_NVK_TEST_LIBS; do
    printf '%s\t%s\n' "$MESA_NVK_BUILD_DIR/$_lib" \
                      "$STAGE/lib/nvk/$(basename "$_lib")" >> "$WORK/jobs"
done
printf '%s\t%s\n' "$GPU_LIB"    "$STAGE/lib/libhorizon_gpu.a"    >> "$WORK/jobs"
printf '%s\t%s\n' "$COMPAT_LIB" "$STAGE/lib/libhorizon_compat.a" >> "$WORK/jobs"
horizon_run bash scripts/fatten-archives.sh "$WORK/jobs"

for _h in horizon/include/horizon_gpu/*.h; do
    stage_file "$_h" "$STAGE/include/horizon_gpu/$(basename "$_h")"
done
for _d in vulkan vk_video; do
    mkdir -p "$STAGE/include/nvk/$_d"
    for _h in "mesa/include/$_d"/*.h; do
        stage_file "$_h" "$STAGE/include/nvk/$_d/$(basename "$_h")"
    done
done

stage_file LICENSE            "$STAGE/licenses/mesa-nvk-horizon/LICENSE"
stage_file LICENSES/README.md "$STAGE/licenses/mesa-nvk-horizon/LICENSES.md"

# ---------------------------------------------------------------------
# nvk.pc
#
# Everything about this line is load-bearing and the reasons are in the
# file itself, where the person debugging a link will be looking.
# ---------------------------------------------------------------------
{
    cat <<'PC_HEAD'
# mesa-nvk-horizon — the NVK Vulkan driver with the Horizon OS
# kernel-mode backend, as a static link into your own .nro.
#
# GENERATED by scripts/package-portlibs.sh. Editing it is pointless:
# reinstalling rewrites it, and share/mesa-nvk-horizon/installed-files.txt
# lists it so `make uninstall` removes it.
#
# THERE IS STILL NO ICD, NO LOADER AND NO SHARED LIBRARY (docs/USAGE.md).
# Nothing finds this driver at run time. vkCreateInstance is a direct
# call into the archives below.
#
# prefix is derived from where this file is, not written in: the tarball
# this comes from is extracted into whatever devkitPro the reader has,
# and a prefix baked in at build time would be right only on the machine
# that built it. pcfiledir is pkgconf's own variable and resolves to the
# directory holding this file.
PC_HEAD
    echo 'prefix=${pcfiledir}/../..'
    echo 'devkitpro=${prefix}/../..'
    echo 'includedir=${prefix}/include'
    echo 'libdir=${prefix}/lib'
    echo 'nvkdir=${libdir}/nvk'
    echo
    echo "Name: nvk"
    echo "Description: Mesa NVK Vulkan driver with the Horizon OS backend, static"
    echo "Version: ${MESA_TAG#mesa-}"
    echo "Requires:"
    cat <<'PC_LIBS'
# WHY THE LINE LOOKS LIKE THIS. It reproduces meson.build's
# idep_nvk_driver, which is the only form of it that has ever linked.
#
#   --start-group/--end-group, spelled out. These archives have genuine
#   cycles (nvk -> vulkan_runtime -> nir -> mesa_util, and nvk needs all
#   three). Meson wraps a dependency's link_args in a group for you;
#   a .pc gets no such wrapper, so without this the link fails.
#
#   --whole-archive on libnvk.a AND NOTHING ELSE. A Vulkan driver
#   reaches its entry points through dispatch tables generated with
#   --weak, and an archive member nothing but a weak reference points at
#   is never pulled in: what reaches the binary is a NULL table entry
#   and an `nm -u` that says nothing is wrong. Measured on hardware, at
#   the cost of a console round trip — vkEnumeratePhysicalDevices
#   returned 0 and the binary contained no GetPhysicalDeviceProperties2
#   at all. Whole-archiving the rest costs size for nothing.
#
#   libvulkan_runtime.a, libvulkan_lite_runtime.a and
#   libvulkan_instance.a are deliberately absent: libnvk.a already
#   contains them (179 members, 67 of them from
#   libvulkan_lite_runtime.a.p). Listing one beside it gives
#   "multiple definition of vk_instance_end_renderdoc_capture".
#
#   Full paths inside the group rather than -l names. Two reasons: -l is
#   ambiguous against any other Mesa in this prefix, and freedesktop
#   pkg-config emits every -l AFTER the other flags, which would put
#   libnvk.a outside the --whole-archive markers and outside the group.
#   pkgconf, which is what devkitPro ships, preserves written order —
#   full paths are correct under both. The trailing -l names are -l
#   precisely because the end is where both put them.
#
#   -lstdc++ because NVK compiles nvkcl.cpp and its exception
#   personality routine lives there; -lzstd -lz because enabling the
#   shader cache put compress.c on the reachable side of libmesa_util.a;
#   -lnx last. libhorizon_compat.a is inside the group and therefore
#   ahead of -lnx: it defines sysconf, which newlib declares and does
#   not define, and without it nvk_physical_device_create() fails and
#   zero devices are enumerated.
PC_LIBS
    printf 'Libs: -Wl,--start-group'
    for _lib in $NVK_WHOLE; do
        printf ' -Wl,--whole-archive ${nvkdir}/%s -Wl,--no-whole-archive' \
               "$(basename "$_lib")"
    done
    for _lib in $HORIZON_NVK_TEST_LIBS; do
        case " $NVK_WHOLE " in
            *" $_lib "*) continue ;;
        esac
        printf ' ${nvkdir}/%s' "$(basename "$_lib")"
    done
    printf ' ${libdir}/libhorizon_gpu.a ${libdir}/libhorizon_compat.a'
    printf ' -Wl,--end-group'
    printf ' -L${libdir} -L${devkitpro}/libnx/lib'
    printf ' -lstdc++ -lzstd -lz -lnx\n'
    cat <<'PC_CFLAGS'
# -I${includedir}/nvk FIRST, so <vulkan/vulkan.h> resolves to the
# headers this driver was compiled against before any other vulkan/ that
# happens to be on the command line. It is also what makes vk_video
# work: vulkan_core.h includes "vk_video/..." quoted, which tries the
# including file's own directory (include/nvk/vulkan/vk_video/, absent)
# and then the -I path (include/nvk/vk_video/, present). Point -I at
# include/nvk/vulkan by hand and you get headers that cannot find their
# own siblings.
#
# -DVK_USE_PLATFORM_VI_NN because without it <vulkan/vulkan.h> hides
# VK_NN_vi_surface — VkViSurfaceCreateInfoNN, PFN_vkCreateViSurfaceNN,
# the extension name macro — so you could not name the surface type this
# driver advertises. It is the define the driver itself was built with.
PC_CFLAGS
    echo 'Cflags: -I${includedir}/nvk -I${includedir} -DVK_USE_PLATFORM_VI_NN'
} > "$WORK/nvk.pc"
stage_file "$WORK/nvk.pc" "$STAGE/lib/pkgconfig/nvk.pc"

# ---------------------------------------------------------------------
# The manifest, and the record of what produced this.
# ---------------------------------------------------------------------
MANIFEST_REL=share/mesa-nvk-horizon/installed-files.txt
RECORD_REL=share/mesa-nvk-horizon/INSTALL.txt

# Every path relative to the prefix, sorted, and listing itself and
# INSTALL.txt — scripts/install-horizon.sh removes exactly this set and
# nothing else, so anything it places has to be in here.
#
# NO HASHES IN THIS FILE. It has to be a pure function of the file SET,
# or a rebuild that changed one byte would rewrite the one file
# uninstalling depends on. What changed goes in INSTALL.txt beside it,
# where churn is the correct behaviour.
(
    cd "$STAGE" && find . -type f \
        ! -path "./$MANIFEST_REL" ! -path "./$RECORD_REL" -print
    echo "./$MANIFEST_REL"
    echo "./$RECORD_REL"
) | sed 's|^\./||' | LC_ALL=C sort > "$WORK/manifest"
stage_file "$WORK/manifest" "$STAGE/$MANIFEST_REL"

# Deterministic on purpose, so that two runs over one tree produce one
# tarball: no date, no build id, and no horizon_image_digest — that one
# shells out to `docker image inspect` and can answer differently
# between two runs, which package-horizon.sh already learned the hard
# way by calling it twice inside one manifest.
#
# It hashes every installed file except itself, which is what lets
# scripts/install-horizon.sh decide whether an install is already
# current by comparing this one file.
{
    echo "mesa-nvk-horizon — NVK for Horizon OS, staged for a devkitPro portlibs prefix"
    echo
    echo "mesa    : $MESA_TAG ($MESA_COMMIT)"
    echo "files   : $(wc -l < "$WORK/manifest" | tr -d ' ')"
    echo
    echo "# sha256 of every installed file except this one"
    (cd "$STAGE" && LC_ALL=C sort "$MANIFEST_REL" |
         grep -v "^$RECORD_REL\$" | tr '\n' '\0' | xargs -0 sha256sum)
} > "$WORK/record"
stage_file "$WORK/record" "$STAGE/$RECORD_REL"

# Anything the staging tree still holds that the manifest does not name.
# Without this, an archive dropped from $HORIZON_NVK_TEST_LIBS or a
# header deleted upstream would stay in the tarball forever, and the
# manifest would not mention it — so no uninstall would ever remove it
# either.
(cd "$STAGE" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) \
    > "$WORK/present"
_stale=$(LC_ALL=C comm -23 "$WORK/present" "$STAGE/$MANIFEST_REL")
if [ -n "$_stale" ]; then
    echo "package-portlibs: dropping $(printf '%s\n' "$_stale" | wc -l | tr -d ' ')" \
         "file(s) no longer part of the package"
    printf '%s\n' "$_stale" | while IFS= read -r _f; do
        rm -f "$STAGE/$_f"
    done
fi

# ---------------------------------------------------------------------
# GATE: nothing thin got through.
#
# Not a warning. An installed thin archive is a prefix that looks
# healthy and fails at somebody else's link, which is the entire failure
# this package exists to stop.
# ---------------------------------------------------------------------
_thin=""
for _a in "$STAGE"/lib/*.a "$STAGE"/lib/nvk/*.a; do
    [ -f "$_a" ] || continue
    [ "$(head -c 7 "$_a")" = '!<thin>' ] && _thin="$_thin $_a"
done
if [ -n "$_thin" ]; then
    echo "error: these staged archives are still thin:$_thin" >&2
    echo "       They name objects they do not contain, so they link" \
         "nowhere but this tree. scripts/fatten-archives.sh is what" \
         "should have converted them." >&2
    exit 1
fi

# ---------------------------------------------------------------------
# The tarball.
# ---------------------------------------------------------------------

# Reproducible, so that two runs over one tree give one artefact and
# `sha256sum` is a statement about the build rather than about the
# clock. --mtime is the build's own success stamp, which is already what
# "this build" means everywhere else in this repository.
STAMP="$MESA_NVK_BUILD_DIR/.horizon-build-ok"
if [ -f "$STAMP" ]; then
    _mtime=$(stat -c %Y "$STAMP")
else
    # No stamp is only reachable on a tree with no mesa/src, since
    # horizon_nvk_build_stamp_gate demands one otherwise.
    _mtime=0
fi

mkdir -p "$(dirname "$OUT")"

# Compressing 225 MiB takes the best part of a minute, and `make
# install` calls this script every time it runs. Everything above is
# already idempotent and cheap on a second pass — the archives compare
# equal and are left alone — so this is the one step worth skipping when
# there is provably nothing new to put in it.
#
# -print -quit stops at the first file newer than the tarball, so the
# check costs a directory walk and not a comparison of every file.
if [ -f "$OUT" ] && [ -f "$OUT.sha256" ] &&
   [ -z "$(find "$STAGE" -type f -newer "$OUT" -print -quit)" ]; then
    echo "package-portlibs: $OUT is newer than everything staged;" \
         "not rebuilt"
else
    tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$_mtime" \
        -cf "$WORK/out.tar" -C "$STAGE" .
    # -n so gzip stores neither a timestamp nor an original name of its
    # own; without it the two runs differ in bytes nothing read.
    gzip -9 -n -c "$WORK/out.tar" > "$WORK/out.tar.gz"
    if [ -f "$OUT" ] && cmp -s "$WORK/out.tar.gz" "$OUT"; then
        echo "package-portlibs: $OUT unchanged"
    else
        cp "$WORK/out.tar.gz" "$OUT"
    fi
    (cd "$(dirname "$OUT")" && sha256sum "$(basename "$OUT")") > "$OUT.sha256"
fi

echo "package-portlibs: $OUT —" \
     "$(wc -l < "$STAGE/$MANIFEST_REL" | tr -d ' ') files," \
     "$(wc -c < "$OUT" | tr -d ' ') bytes"
echo "package-portlibs: install it with"
echo "                    tar -xzf $OUT -C \"\$DEVKITPRO/portlibs/switch\""
echo "                  or with scripts/install-horizon.sh, which also"
echo "                  records what it placed so it can be removed again"
