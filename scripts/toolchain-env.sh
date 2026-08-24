# shellcheck shell=sh
# Sourced, not executed. Resolves the pinned toolchain and exposes the
# few helpers every other script needs, so no script has to decide for
# itself where devkitPro is or how to reach it.
#
#   . scripts/toolchain-env.sh   # from the repository root
#
# Provides:
#   $HORIZON_DEVKITPRO         devkitPro prefix as the *compiler* sees it
#   $HORIZON_IN_CONTAINER      1 when commands must run in the image
#   $HORIZON_BUILD_DIR         Meson build directory
#   $HORIZON_CROSS_CONST_FILE  generated [constants] cross file
#   $HORIZON_MESON_DIR         pinned Meson install (gitignored)
#   $MESA_BUILD_DIR            where Mesa is configured and built
#   $HORIZON_MESA_TEST_LIBS    archives tests 12 and 13 link, relative
#                              to $MESA_BUILD_DIR
#   horizon_run <cmd>...       run in the toolchain (host or container)
#   horizon_meson <args>...    run the pinned Meson in the toolchain
#   horizon_ensure_meson       install the pinned Meson if missing
#   horizon_mesa_libs_present  are those archives all there right now
#   horizon_setup_mode <dir> [identity files...]  "", --reconfigure, --wipe
#   horizon_record_cross_id    stamp what horizon_setup_mode checked
#
# Two execution modes:
#   - $DEVKITPRO set  -> run directly on this machine.
#   - otherwise       -> run inside the image pinned by digest in
#                        toolchain/versions.env. devkitPro's package
#                        servers answer 403 from some networks, so the
#                        image is the supported fallback (CLAUDE.md).
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT

# shellcheck source=../toolchain/versions.env
. toolchain/versions.env

HORIZON_BUILD_DIR="${HORIZON_BUILD_DIR:-build/meson}"
HORIZON_CROSS_CONST_FILE="${HORIZON_CROSS_CONST_FILE:-build/toolchain/devkitpro.cross}"
HORIZON_CROSS_FILE="toolchain/horizon-aarch64.cross"
HORIZON_MESON_DIR="build/toolchain/meson-${MESON_VERSION}"
# Python modules Mesa's generators need, kept separate from Meson's
# install so either can be reprovisioned without disturbing the other.
HORIZON_PYTHON_DIR="build/toolchain/python"
# Where scripts/{configure,build}-mesa.sh build the pinned Mesa. Defined
# here rather than in each of them because four things have to agree on
# it: those two, meson.build (tests 12 and 13 link the archives it
# contains, through -Dmesa_build_dir) and the Makefile. A caller that
# overrode it in only some of them got the two Mesa tests silently
# skipped while the build reported success.
MESA_BUILD_DIR="${MESA_BUILD_DIR:-build/mesa-probe}"

# One spelling of that path, whatever the caller typed. Two consumers
# compare it literally — the stamp beside the directory
# (${MESA_BUILD_DIR}.crossid) and the Makefile's clean rule — so
# build/mesa-probe/ and build/mesa-probe must not look like two
# different directories to them. The Makefile normalises independently
# with $(abspath), because it is also a standalone entry point.
while [ "${MESA_BUILD_DIR%/}" != "$MESA_BUILD_DIR" ] &&
      [ -n "${MESA_BUILD_DIR%/}" ]; do
    MESA_BUILD_DIR="${MESA_BUILD_DIR%/}"
done

# An absolute path is accepted — meson.build resolves one without a
# second code path — but only inside the tree when the toolchain is a
# container. horizon_run bind-mounts $PWD and nothing else, so Meson
# would configure into the container's own filesystem, which does not
# survive the run. Measured: a file written to /var/tmp/... inside the
# container is readable there and absent on the host a moment later,
# while the same write under $PWD is on the host. The next
# build-mesa.sh would then find no build.ninja and reconfigure from
# scratch, forever. Rejected with a reason rather than left to be
# discovered as "the Mesa build keeps disappearing".
case "$MESA_BUILD_DIR" in
    "$PWD" | "$PWD"/*) ;;   # inside the mounted tree
    /*)
        if [ -z "${DEVKITPRO:-}" ]; then
            echo "error: MESA_BUILD_DIR=$MESA_BUILD_DIR is outside $PWD," >&2
            echo "       and the toolchain container mounts only \$PWD, so" >&2
            echo "       the build directory would not survive the run." >&2
            echo "       Use a path inside the tree, or install devkitA64" >&2
            echo "       locally so no container is involved." >&2
            return 1 2>/dev/null || exit 1
        fi
        ;;
esac

# The archives tests 12 and 13 link, relative to $MESA_BUILD_DIR. Named
# here so a script can ask whether Mesa is built without restating them;
# the Makefile and meson.build carry their own copies, because each
# build system stays readable on its own, and
# scripts/check-mesa-test-parity.sh is what keeps the three in step.
HORIZON_MESA_TEST_LIBS="src/c11/impl/libmesa_util_c11.a src/util/libmesa_util.a src/util/blake3/libblake3.a"

# Where scripts/build-compat.sh archives libhorizon_compat.a. It sits
# with the other provisioned inputs rather than in a build directory,
# because the cross file names it and therefore every build needs it to
# exist *before* meson setup runs (see build-compat.sh).
HORIZON_COMPAT_LIBDIR="build/toolchain/lib"

# Where core, alloc and compiler_builtins for $RUST_TARGET live. Its
# value depends on which toolchain mode is in use and is therefore set
# below, next to $HORIZON_DEVKITPRO, which is resolved the same way:
# both are paths *as the compiler sees them*, and in container mode
# that is a path inside the image, not in this tree.
#
# It is needed before meson setup either way — the cross file names it,
# and Meson's Rust sanity check compiles a program against it.
#
# Where bindgen, cbindgen and libclang live is not a variable at all in
# container mode: the derived image installs them on PATH and in the
# system library path (toolchain/Dockerfile).
HORIZON_RUST_SYSROOT_REL="build/toolchain/rust-sysroot"

# Where scripts/build-mesa-clc.sh installs the two *build-machine*
# binaries the cross build's -Dmesa-clc=system looks for on PATH:
# mesa_clc and vtn_bindgen2. In the tree rather than in the image
# because they are built from the pinned Mesa checkout, which is not in
# the image and changes with MESA_COMMIT.
HORIZON_NATIVE_TOOLS_DIR="build/toolchain/native-tools"

# Where scripts/build-mesa-nvk.sh configures and builds the driver.
# Named here because two scripts and meson.options have to agree on
# it, and scripts/check-dispatch-complete.sh reads a generated file
# out of it.
# $MESA_NVK_BUILD_DIR is the name, because it is the one the scripts
# that actually build there already read (build-mesa-nvk.sh:19,
# configure-mesa-nvk.sh:88). This used to read $NVK_BUILD_DIR, which
# nothing sets — so the moment a caller pointed MESA_NVK_BUILD_DIR
# somewhere else, everything here went on describing build/mesa-nvk: a
# state string naming a directory the build never used, which is
# precisely the failure horizon_mesa_state exists to prevent, one layer
# further down.
#
# Exported so a caller who sets neither gets the same answer from every
# script, and so the value a function reads is the value the build used
# rather than whatever was in scope when this file happened to be
# sourced.
MESA_NVK_BUILD_DIR="${MESA_NVK_BUILD_DIR:-build/mesa-nvk}"
# Kept as the old name for anything still reading it; same value, one
# source of truth.
NVK_BUILD_DIR_DEFAULT="$MESA_NVK_BUILD_DIR"

# The NVK equivalent of $HORIZON_MESA_TEST_LIBS: every archive the NVK
# tests link, in the same order meson.build lists them
# (nvk_whole_libs then nvk_test_libs), relative to $MESA_NVK_BUILD_DIR.
# scripts/check-mesa-test-parity.sh fails if the two ever disagree.
#
# THIS USED TO BE A SENTINEL — libnvk.a and the Rust runtime only, with
# a comment saying they were "the pair whose absence means that answer
# will be no". That is true and it is the wrong direction. The callers
# use horizon_nvk_libs_present() to decide whether meson.build will
# build t_vulkan, and meson.build asks fs.exists() over ALL of them, so
# the pair being present proved nothing about the answer. Measured on a
# clean tree: the two sentinels were there, six others were not,
# horizon_nvk_libs_present said yes and meson.build said no. A check
# that reports success without having verified what its caller asks it
# about is worse than no check.
HORIZON_NVK_TEST_LIBS="src/nouveau/vulkan/libnvk.a
src/nouveau/rust_runtime/libnouveau_rust_runtime.a
src/nouveau/compiler/libnak.a
src/nouveau/nil/liblibnil_format_table.a
src/nouveau/mme/libnouveau_mme.a
src/nouveau/headers/libnvidia_headers_c.a
src/vulkan/util/libvulkan_util.a
src/vulkan/wsi/libvulkan_wsi.a
src/compiler/spirv/libvtn.a
src/compiler/nir/libnir.a
src/compiler/libcompiler.a
src/compiler/rust/libcompiler_c_helpers.a
src/util/libxmlconfig.a
src/util/libmesa_util.a
src/util/libmesa_util_simd.a
src/util/blake3/libblake3.a
src/c11/impl/libmesa_util_c11.a"

if [ -n "${DEVKITPRO:-}" ]; then
    HORIZON_IN_CONTAINER=0
    HORIZON_DEVKITPRO="$DEVKITPRO"
    HORIZON_TOOLCHAIN_DESC="local devkitA64 at \$DEVKITPRO"
    # Built in the tree by scripts/build-rust-sysroot.sh: on a
    # developer's machine there is no image to have baked one in.
    HORIZON_RUST_SYSROOT="$PWD/$HORIZON_RUST_SYSROOT_REL"

    # UNLESS THIS *IS* THE IMAGE. A CI job whose container is the
    # toolchain has $DEVKITPRO set and lands here — correctly, there is
    # nothing to drive — but the image already carries a sysroot for
    # $RUST_TARGET, built when it was. Ignoring it means every run
    # rebuilds core and alloc into the workspace: minutes, for a
    # directory sitting at $HORIZON_IMAGE_RUST_SYSROOT.
    #
    # /etc/mesa-nvk-horizon-toolchain is the same marker
    # horizon_image_digest reads, so "we are inside the image" is asked
    # once and answered the same way everywhere.
    if [ -r /etc/mesa-nvk-horizon-toolchain ] &&
       [ -d "$HORIZON_IMAGE_RUST_SYSROOT" ]; then
        HORIZON_RUST_SYSROOT="$HORIZON_IMAGE_RUST_SYSROOT"
        HORIZON_TOOLCHAIN_DESC="the toolchain image itself (\$DEVKITPRO set inside it)"
    fi
else
    command -v docker >/dev/null 2>&1 || {
        echo "error: \$DEVKITPRO is not set and docker is unavailable." >&2
        echo "       Install devkitA64 or start dockerd; see CLAUDE.md." >&2
        return 1 2>/dev/null || exit 1
    }
    HORIZON_IN_CONTAINER=1
    # Inside the image the prefix is the image's, not this machine's.
    HORIZON_DEVKITPRO="$HORIZON_NX_IMAGE_DEVKITPRO"
    HORIZON_RUST_SYSROOT="$HORIZON_IMAGE_RUST_SYSROOT"

    # The Switch toolchain belongs to the environment (see the
    # ENVIRONMENT header in toolchain/versions.env), so the image is
    # referenced by its tag and whatever that tag currently resolves to
    # is what gets used. Nothing here pins or updates it.
    #
    # HORIZON_NX_IMAGE overrides the reference entirely — including with
    # a digest, when rebuilding the artefacts behind a recorded hardware
    # run (build/pkg/MANIFEST.txt prints the exact command).
    HORIZON_BASE_IMAGE="${HORIZON_NX_IMAGE:-${HORIZON_NX_IMAGE_REPO}:${HORIZON_NX_IMAGE_TAG}}"

    # Mesa's Rust half needs three things the base image does not have
    # and a container cannot install for itself (no network): libclang,
    # bindgen/cbindgen, and a Rust sysroot for a tier-3 target.
    # scripts/build-toolchain-image.sh adds them in one layer on top of
    # the base image; toolchain/Dockerfile explains each.
    #
    # The derived image is used when it exists, and the base image when
    # it does not — so everything built before Phase 4 keeps building
    # with exactly the toolchain it was built with, and the extra layer
    # is only paid for by the part that needs it.
    HORIZON_DERIVED_IMAGE="${HORIZON_NX_DERIVED_IMAGE:-$HORIZON_NX_DERIVED_REPO:$HORIZON_NX_IMAGE_TAG}"
    if docker image inspect "$HORIZON_DERIVED_IMAGE" >/dev/null 2>&1; then
        HORIZON_IMAGE="$HORIZON_DERIVED_IMAGE"
        HORIZON_TOOLCHAIN_DESC="$HORIZON_DERIVED_IMAGE (from $HORIZON_BASE_IMAGE)"
    else
        HORIZON_IMAGE="$HORIZON_BASE_IMAGE"
        HORIZON_TOOLCHAIN_DESC="$HORIZON_BASE_IMAGE"
    fi
fi

export HORIZON_BUILD_DIR HORIZON_CROSS_CONST_FILE HORIZON_CROSS_FILE
export HORIZON_MESON_DIR HORIZON_IN_CONTAINER HORIZON_DEVKITPRO
export HORIZON_TOOLCHAIN_DESC HORIZON_COMPAT_LIBDIR MESA_BUILD_DIR
export HORIZON_MESA_TEST_LIBS HORIZON_RUST_SYSROOT
export HORIZON_NATIVE_TOOLS_DIR NVK_BUILD_DIR_DEFAULT MESA_NVK_BUILD_DIR

# True when every archive tests 12 and 13 link is present. Both build
# paths decide whether to build those two tests on exactly this
# question; they just ask it at different times, which is what
# scripts/build-horizon.sh has to reconcile.
horizon_mesa_libs_present() {
    for _hz_mesa_lib in $HORIZON_MESA_TEST_LIBS; do
        [ -f "$MESA_BUILD_DIR/$_hz_mesa_lib" ] || return 1
    done
    unset _hz_mesa_lib
    return 0
}

# What a configured build directory assumed about Mesa: whether the
# archives were there AND which directory they were looked for in.
# configure-horizon.sh records this line, build-horizon.sh compares it,
# and a difference reconfigures.
#
# The directory is half of it, not decoration. With presence alone,
# switching $MESA_BUILD_DIR between two directories that both hold the
# archives left both sides reading "present", so nothing reconfigured —
# and -Dmesa_build_dir stayed at the old value inside build.ninja, so
# the two tests went on linking the archives of a Mesa build the caller
# had stopped asking for. Measured: `MESA_BUILD_DIR=build/mesa-alt
# scripts/build-horizon.sh` left build.ninja naming mesa-probe.
#
# One function, called from both sides, so the two cannot record and
# compare different things.
# Which of $HORIZON_NVK_TEST_LIBS are not in $MESA_NVK_BUILD_DIR, one
# per line; empty when the build is complete.
#
# The names, not just a yes/no, because every caller that refuses has to
# say what is missing — a refusal that only says "incomplete" sends the
# reader to run the whole build again rather than to the one archive
# that did not get made.
horizon_nvk_missing_libs() {
    for _hz_nvk_lib in $HORIZON_NVK_TEST_LIBS; do
        [ -f "$MESA_NVK_BUILD_DIR/$_hz_nvk_lib" ] || echo "$_hz_nvk_lib"
    done
    unset _hz_nvk_lib
}

# True when the archives t_vulkan links are present. Separate from
# horizon_mesa_libs_present because they live in a different build
# directory, produced by a different script, at a different time.
horizon_nvk_libs_present() {
    [ -z "$(horizon_nvk_missing_libs)" ]
}

# THE BUILD THAT PRODUCED THOSE ARCHIVES ACTUALLY SUCCEEDED, AND IT
# COMPILED WHAT THE TREE SAYS. Moved here verbatim from
# scripts/package-horizon.sh, which is no longer the only caller:
# scripts/package-portlibs.sh asks the identical question before it
# installs the same archives into a toolchain prefix, where a stale
# answer is worse — a package is a directory somebody chose to copy, an
# install is already in the path of every later build on that machine.
#
# The whole rationale is package-horizon.sh's and is kept with the code:
#
#   The staleness gate there asks whether an artefact is older than the
#   archives it links. It cannot ask whether those archives are older
#   than the source they were built from — and on 2026-08-08 that is
#   exactly what shipped: a t_vk_swapchain.nro built minutes earlier,
#   linked against a libvulkan_wsi.a from three days before, missing the
#   one patch the run was meant to test. The .nro was newer than the
#   archive, so that gate passed with nothing to say.
#
#   The build had failed and said so; the command that ran it printed
#   "built" regardless, because an unconditional echo followed a
#   filtered pipeline. A gate is the answer to that, not more care.
#
#   AGAINST THE BUILD'S OWN SUCCESS STAMP, not against an archive. The
#   first version compared every file under mesa/src with
#   libvulkan_wsi.a, and that is wrong in the ordinary case: Ninja does
#   not touch an archive whose inputs did not change, so editing
#   anything outside WSI leaves libvulkan_wsi.a older than the source
#   that was just correctly compiled, and a good build reads as stale.
#   Reported by Codex on PR #8, and right.
#
#   scripts/build-mesa-nvk.sh touches .horizon-build-ok as its last act,
#   under `set -e`, so the stamp exists only if every step succeeded and
#   is newer than everything that run compiled.
#
# THE `[ -d mesa/src ]` GUARD IS INSIDE THIS FUNCTION AND NOT AT THE
# CALL SITES. It is a policy and not an optimisation: a tree with no
# Mesa checkout has no stamp to demand, and package-horizon.sh's
# .nro-only package is legitimate there. Hoisting it out would put that
# policy in two places and let them drift.
#
# `find ... 2>/dev/null | head -5` takes head's exit status, which is
# what makes it survive a caller's `set -e`. It is written that way on
# purpose; do not "tidy" it.
horizon_nvk_build_stamp_gate() {
    [ -d mesa/src ] || return 0

    _hz_stamp="$MESA_NVK_BUILD_DIR/.horizon-build-ok"
    if [ ! -f "$_hz_stamp" ]; then
        echo "error: $_hz_stamp does not exist, so no complete" \
             "scripts/build-mesa-nvk.sh run has produced the archives in" \
             "$MESA_NVK_BUILD_DIR. Run it and try again." >&2
        unset _hz_stamp
        return 1
    fi
    _hz_newer_src=$(find mesa/src -type f \( -name '*.c' -o -name '*.h' \
                         -o -name '*.rs' -o -name '*.build' \) \
                    -newer "$_hz_stamp" 2>/dev/null | head -5)
    if [ -n "$_hz_newer_src" ]; then
        echo "error: these Mesa sources are newer than the last successful" \
             "build ($_hz_stamp):" >&2
        echo "$_hz_newer_src" | sed 's/^/         /' >&2
        echo "       The archives do not contain them, so anything built" \
             "against them is built against source that is no longer what" \
             "the tree says. Run scripts/build-mesa-nvk.sh and try again." >&2
        unset _hz_stamp _hz_newer_src
        return 1
    fi
    unset _hz_stamp _hz_newer_src
    return 0
}

# ALL SEVENTEEN OR NONE, over the full list rather than the two
# representative archives the staleness gate checks. What it stops is a
# package — or now an install — shipping twelve of seventeen and calling
# it the driver.
#
# The caller decides whether "no NVK build at all" is an error:
# package-horizon.sh calls this only when it has already established
# there is one, because a Makefile-only package legitimately has none,
# while package-portlibs.sh refuses outright because an install with no
# driver in it is not an install.
horizon_nvk_archive_gate() {
    _hz_nvk_missing=$(horizon_nvk_missing_libs | tr '\n' ' ')
    if [ -n "$_hz_nvk_missing" ]; then
        echo "error: NVK archives exist but these named in" \
             "\$HORIZON_NVK_TEST_LIBS are missing from" \
             "$MESA_NVK_BUILD_DIR: $_hz_nvk_missing" >&2
        echo "       scripts/build-mesa-nvk.sh is what produces them." >&2
        unset _hz_nvk_missing
        return 1
    fi
    unset _hz_nvk_missing
    return 0
}

# Copy one archive somewhere it will still link — see
# scripts/fatten-archives.sh for what that means and why a plain cp of a
# Meson archive does not.
#
# For a caller with ONE archive to move. Anything with a list of them
# should build a job file and call scripts/fatten-archives.sh through
# horizon_run itself: this spends a container invocation per archive in
# container mode, which is exactly what that script exists not to do.
horizon_fatten_archive() {
    _hz_fa_job=$(mktemp "build/fatten-job.XXXXXX")
    printf '%s\t%s\n' "$1" "$2" > "$_hz_fa_job"
    if horizon_run bash scripts/fatten-archives.sh "$_hz_fa_job" >/dev/null; then
        rm -f "$_hz_fa_job"
        unset _hz_fa_job
        return 0
    fi
    rm -f "$_hz_fa_job"
    unset _hz_fa_job
    return 1
}

# Both halves, because the Meson path bakes both answers into
# build.ninja at configure time and t_vulkan depends on the second one.
#
# The NVK half was missing here and the consequence was specific:
# build-mesa-nvk.sh runs build-horizon.sh *before* it compiles NVK, so
# on a clean tree the horizon build is configured at the one moment the
# NVK archives are guaranteed absent, and meson.build's fs.exists()
# answers "no" — t_vulkan, the phase's exit-criterion test, is left out
# of build.ninja. Every later run compared only the core-Mesa half,
# found it unchanged, and never reconfigured, so the omission was
# permanent for that build directory.
#
# It went unnoticed because the hardware .nro came from the Makefile
# path, which re-evaluates its $(wildcard) on every invocation and
# therefore never had the problem. That is the same asymmetry the
# comment in build-horizon.sh describes for core Mesa: the fix landed
# there for one directory and this is the second one.
horizon_mesa_state() {
    _hz_st_mesa=absent
    _hz_st_nvk=absent
    horizon_mesa_libs_present && _hz_st_mesa=present
    horizon_nvk_libs_present && _hz_st_nvk=present
    echo "$_hz_st_mesa $MESA_BUILD_DIR $_hz_st_nvk $MESA_NVK_BUILD_DIR"
    unset _hz_st_mesa _hz_st_nvk
}

# Run a command with the cross toolchain reachable. The image's default
# PATH omits devkitA64/bin and portlibs/switch/bin (measured; recorded
# in versions.env), and the Meson cross file resolves its [binaries] by
# bare name, so all three bin directories are prepended in both modes.
#
# In container mode the tree is mounted at the *same* path as on the
# host, so paths in diagnostics, depfiles and Meson's build.ninja are
# valid on both sides — and no container workdir is hardcoded
# (scripts/check-no-abs-paths.sh).
#
# $HORIZON_IMAGE_RUST_TOOLS_BIN is prepended too. `docker run -e PATH`
# overrides the image's own ENV PATH, so the Dockerfile setting it is
# not enough — it has to be repeated here or bindgen is not found.
horizon_run() {
    _hz_bins="${HORIZON_DEVKITPRO}/${HORIZON_TOOLCHAIN_BINDIR_REL}:${HORIZON_DEVKITPRO}/${HORIZON_TOOLS_BINDIR_REL}:${HORIZON_DEVKITPRO}/${HORIZON_PORTLIBS_BINDIR_REL}:${PWD}/${HORIZON_NATIVE_TOOLS_DIR}/bin"
    if [ "$HORIZON_IN_CONTAINER" -eq 0 ]; then
        # A local install's rustc, bindgen and cbindgen are already on
        # the developer's PATH; this mode installs nothing of its own.
        PATH="${_hz_bins}:${PATH}" "$@"
    else
        # The image keeps rustup outside the default PATH; Mesa's build
        # needs rustc for NAK/NIL, so add it here rather than making
        # every caller know where it lives.
        #
        # MSYS_NO_PATHCONV=1 is what makes the mount above work under Git
        # Bash on Windows. MSYS rewrites anything that looks like an
        # absolute Unix path in an argument to a native binary, so the
        # -w argument reached docker.exe rewritten into the drive-letter
        # spelling and the daemon rejected it:
        #   docker: Error response from daemon: the working directory
        #   '<drive-letter path>' is invalid, it needs to be an absolute
        #   path
        # The variable is meaningless everywhere else, and the container
        # still sees the tree at the same path the host calls it, which
        # is the property the paragraph above depends on.
        MSYS_NO_PATHCONV=1 \
        docker run --rm \
            -e DEVKITPRO="$HORIZON_DEVKITPRO" \
            -e "PATH=${_hz_bins}:${HORIZON_IMAGE_RUST_TOOLS_BIN}:${RUST_CARGO_HOME_IN_IMAGE}/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
            -v "$PWD":"$PWD" -w "$PWD" \
            "$HORIZON_IMAGE" "$@"
    fi
}

# Meson is pure Python, so the pinned copy installed on the host runs
# unchanged under the image's python3 (3.11.2). That matters: the image
# has neither meson nor pip, and containers here have no network.
horizon_meson() {
    horizon_run env \
        "PYTHONPATH=$PWD/$HORIZON_MESON_DIR:$PWD/$HORIZON_PYTHON_DIR" \
        python3 "$PWD/$HORIZON_MESON_DIR/bin/meson" "$@"
}

# ninja on a build directory Meson generated, for the one thing `meson
# compile` cannot express: a target named by its output path rather than
# by its declared name. Meson resolves `src/compiler/nir/libnir.a` to
# "target not found" (measured); ninja takes it verbatim, and the path is
# the same string meson.build and $HORIZON_NVK_TEST_LIBS already carry.
#
# PYTHONPATH is not optional even though ninja is not Python: ninja
# re-runs `meson --internal regenerate` whenever build.ninja is out of
# date, and without it that subprocess cannot import meson and the build
# fails with "rebuilding 'build.ninja': subcommand failed".
horizon_ninja() {
    horizon_run env \
        "PYTHONPATH=$PWD/$HORIZON_MESON_DIR:$PWD/$HORIZON_PYTHON_DIR" \
        ninja "$@"
}

# Meson reads the machine files only when a build directory is FIRST
# configured. `setup --reconfigure` keeps the [built-in options] it
# recorded then, so an edit to the cross file never reaches an existing
# build directory — it silently keeps building with the old toolchain
# description.
#
# Measured, and it is not theoretical: after -lhorizon_compat was added
# to c_link_args, a build directory configured before that change and
# then --reconfigure'd still had zero occurrences of the flag in
# build.ninja, and t_sysinfo failed to link with
#   undefined reference to `sysconf'
# `setup --wipe` does re-read them (11 occurrences afterwards). This is
# the ordinary upgrade path — a developer who already has build/meson —
# so it has to be detected rather than left to whoever reads the diff.
#
# The same is true of the set of project options. `setup --reconfigure`
# validates every -D against the options it recorded on the first
# configure, before it re-reads meson.options — so adding an option and
# passing it in the same commit fails with
#   ERROR: Unknown option: "mesa_build_dir".
# on any directory configured before it existed (measured). Callers pass
# such files as extra arguments below, and a change to one wipes the
# directory the same way a cross-file change does.
#
# Echoes the setup mode to use: "" for a fresh directory,
# "--reconfigure" when those inputs are unchanged, "--wipe" when they are
# not. The recorded identity lives *beside* the build directory, because
# --wipe empties the directory itself. A configured directory with no
# stamp is treated as changed: it predates this check, which is exactly
# the case that broke.
#
# The directory and the extra files are remembered here, and
# horizon_record_cross_id takes no arguments as a result. Passing the
# list twice was the same shape of defect as the one this mechanism was
# added to fix: the next option file added would be handed to one call
# site and not the other, and the stamp would then record a different
# identity from the one that was checked.
#
# The mode is returned in $HORIZON_SETUP_MODE rather than echoed, which
# is what makes remembering possible at all: `mode=$(horizon_setup_mode
# ...)` runs the function in a subshell, so anything it assigned was
# gone by the time the caller reached horizon_record_cross_id. Measured
# — the first run after the change failed with
#   error: horizon_record_cross_id before horizon_setup_mode
horizon_setup_mode() { # builddir [extra identity files...]
    HORIZON_ID_DIR="$1"
    shift
    HORIZON_ID_FILES="$*"
    if [ ! -f "$HORIZON_ID_DIR/meson-info/meson-info.json" ]; then
        HORIZON_SETUP_MODE=""
        return 0
    fi
    # shellcheck disable=SC2086 # deliberate word splitting: a file list
    if [ "$(horizon_cross_id ${HORIZON_ID_FILES:-})" = \
         "$(cat "$HORIZON_ID_DIR.crossid" 2>/dev/null)" ]
    then
        HORIZON_SETUP_MODE="--reconfigure"
    else
        HORIZON_SETUP_MODE="--wipe"
    fi
}

# Every input must exist, and is checked before the pipeline rather than
# after it: cat's exit status is discarded by a pipeline, so a renamed or
# deleted identity file used to degrade the hash silently back to the
# cross files alone — precisely the staleness the stamp exists to catch,
# failing open.
#
# Each file's path is hashed with its contents. Without that separator,
# moving a line from one hashed file to another leaves the concatenation
# unchanged, and the two are not interchangeable inputs.
horizon_cross_id() { # [extra identity files...]
    for _hz_id in "$HORIZON_CROSS_FILE" "$HORIZON_CROSS_CONST_FILE" "$@"; do
        [ -f "$_hz_id" ] || {
            echo "error: identity input $_hz_id does not exist" >&2
            return 1
        }
    done
    for _hz_id in "$HORIZON_CROSS_FILE" "$HORIZON_CROSS_CONST_FILE" "$@"; do
        printf '=== %s\n' "$_hz_id"
        cat "$_hz_id"
    done | sha256sum | cut -d' ' -f1
}

# Called after a successful setup, never before: a failed configure must
# not leave a stamp claiming the directory matches. Uses what
# horizon_setup_mode was given, so the two cannot disagree.
horizon_record_cross_id() {
    [ -n "${HORIZON_ID_DIR:-}" ] || {
        echo "error: horizon_record_cross_id before horizon_setup_mode" >&2
        return 1
    }
    # shellcheck disable=SC2086 # deliberate word splitting: a file list
    horizon_cross_id ${HORIZON_ID_FILES:-} > "$HORIZON_ID_DIR.crossid"
}

# Mesa's code generators (milestone item 6). Separate from
# horizon_ensure_meson because horizon/ does not need them and should
# not pay for them; Mesa cannot configure at all without mako.
# Idempotent: a second call with the pins unchanged does nothing.
horizon_ensure_python_deps() {
    if [ -d "$HORIZON_PYTHON_DIR/mako" ] && [ -d "$HORIZON_PYTHON_DIR/yaml" ]; then
        echo "mesa generator deps: already installed in $HORIZON_PYTHON_DIR"
        return 0
    fi
    echo "installing mesa generator deps into $HORIZON_PYTHON_DIR"
    mkdir -p "$HORIZON_PYTHON_DIR"
    # On the host, where the network is: the image has neither pip nor
    # outbound connectivity.
    python3 -m pip install --quiet --no-cache-dir \
        --target "$HORIZON_PYTHON_DIR" \
        "mako==${MESA_PYTHON_MAKO_VERSION}" \
        "pyyaml==${MESA_PYTHON_PYYAML_VERSION}"
}

# The content digest of the image actually in use. Since the toolchain
# is taken from the environment rather than pinned, this is the only
# thing that identifies what a given build ran against, so it goes into
# the artefact manifest. Recording what was used is what this project
# does instead of controlling it.
#
# Prints "local" when building against a machine-local devkitA64, and
# "unknown" if the image is not present locally (nothing has pulled it
# yet, or docker cannot answer).
# The identity of the image a build actually ran in, for the manifest
# that ties a hardware result to the toolchain that produced it.
#
# Two kinds of image reach $HORIZON_IMAGE and only one of them has a
# registry digest. The base image is pulled, so it carries a RepoDigest.
# The derived image (toolchain/Dockerfile, the layer that adds libclang,
# bindgen and the Rust sysroot) is built locally and never pushed, so it
# has *no* RepoDigests at all — and since Phase 4 it is the image almost
# every build uses.
#
# Filtering RepoDigests for the base repository therefore returned
# "unknown" for the normal case, and package-horizon.sh recorded a
# rebuild command of the form <base-repo>@unknown: not merely imprecise
# but unusable, which is the one thing a provenance record must not be.
#
# A locally built image always has an Id, so that is what identifies it,
# tagged with which kind it is so nobody mistakes a local content hash
# for something they can `docker pull`.
#
# The whole reference is returned, not the bare hash, because the caller
# cannot reconstruct the repository half: it used to assume
# $HORIZON_NX_IMAGE_REPO and that assumption is exactly what was wrong
# whenever the derived image was in use. One of four answers, each
# distinguishable by its prefix:
#
#   local                    built against a local devkitA64, no image
#   <repo>@sha256:...        pullable; use it verbatim in HORIZON_NX_IMAGE
#   local-image-id:sha256:.. built here and never pushed; not pullable
#   unknown                  docker could not describe it at all
#
# Takes the image to describe, defaulting to $HORIZON_IMAGE — which is
# what almost every caller wants, because that is the image its commands
# run in. scripts/fetch-rust-crates.sh is the exception and passes
# $HORIZON_BASE_IMAGE: what it builds a vendor closure for is the
# compiler the *sysroot* is built with, and that is the base image even
# when a derived one exists (the header there records what that cost).
horizon_image_digest() { # [image]
    if [ "$HORIZON_IN_CONTAINER" -eq 0 ]; then
        # RUNNING *INSIDE* THE DERIVED IMAGE IS NOT THE SAME AS RUNNING
        # ON A DEVELOPER'S MACHINE, and until this existed both were
        # reported as "local".
        #
        # A CI job whose container *is* the toolchain has $DEVKITPRO set,
        # so everything above puts it in local mode — correctly, there is
        # no container to drive. But then package-horizon.sh recorded
        # `image: local` and every trace of what produced the binaries
        # was gone, which is the one thing that manifest exists to carry.
        # There is no docker inside such a job to ask, so the image
        # answers for itself: toolchain/Dockerfile writes this file with
        # the identity it was built under.
        #
        # The identity rather than a registry digest, deliberately. A
        # digest says where an image was pushed; this says how it was
        # built — base image and ID, the pinned bindgen/cbindgen/LLVM
        # versions, the resolved .deb closure, the Dockerfile's own hash
        # — which is what somebody reproducing a hardware result needs.
        if [ -r /etc/mesa-nvk-horizon-toolchain ]; then
            _hz_id=$(sed -n 's/^identity=//p' /etc/mesa-nvk-horizon-toolchain)
            _hz_base=$(sed -n 's/^base=//p' /etc/mesa-nvk-horizon-toolchain)
            if [ -n "$_hz_id" ] && [ "$_hz_id" != unknown ]; then
                echo "in-image:${_hz_base:-unknown}#${_hz_id}"
                unset _hz_id _hz_base
                return 0
            fi
            unset _hz_id _hz_base
        fi
        echo "local"
        return 0
    fi
    # Prefer a digest for one of the repositories this project actually
    # names. The first attempt at this widened the match to any '@',
    # which was more than the problem needed: the derived image fails
    # because it has no RepoDigests at all, and the .Id branch below is
    # what answers that. All the widening changed was the multi-tagged
    # case, where it would hand back whichever repository docker listed
    # first — possibly one the reader has no access to — in the field
    # whose whole job is telling them how to reproduce the build.
    _hz_img="${1:-$HORIZON_IMAGE}"
    for _hz_repo in "$HORIZON_NX_DERIVED_REPO" "$HORIZON_NX_IMAGE_REPO"; do
        [ -n "$_hz_repo" ] || continue
        _hz_ref=$(docker image inspect \
                      --format '{{range .RepoDigests}}{{println .}}{{end}}' \
                      "$_hz_img" 2>/dev/null |
                  grep -m1 "^${_hz_repo}@")
        if [ -n "$_hz_ref" ]; then
            echo "$_hz_ref"
            unset _hz_repo _hz_ref _hz_img
            return 0
        fi
    done
    unset _hz_repo _hz_ref
    _hz_id=$(docker image inspect --format '{{.Id}}' "$_hz_img" \
                 2>/dev/null | grep -m1 .)
    unset _hz_img
    if [ -n "$_hz_id" ]; then
        echo "local-image-id:$_hz_id"
        return 0
    fi
    echo unknown
}

# pip writes the *installing* interpreter's absolute path into the
# launcher's shebang — here the host's /usr/local/bin/python3, which
# does not exist inside the toolchain image (it has /usr/bin/python3).
# horizon_meson sidesteps that by running the launcher through python3
# explicitly, but Meson's own `--internal exe` wrapper — which every
# custom_target that captures its output goes through — re-invokes the
# launcher *by path* from a plain /bin/sh, where the shebang is what
# runs it. Measured: Mesa's generated sources all failed with
# "/bin/sh: 1: .../bin/meson: not found".
#
# Rewriting it to env python3 makes one install work on both sides. This
# is the same rule as scripts/check-no-abs-paths.sh enforces on tracked
# files — an installing machine's path must not decide whether a build
# works — applied to a generated one the gate cannot see.
horizon_fix_meson_shebang() {
    _hz_launcher="$HORIZON_MESON_DIR/bin/meson"
    [ -f "$_hz_launcher" ] || return 0
    if [ "$(head -n 1 "$_hz_launcher")" = '#!/usr/bin/env python3' ]; then
        return 0
    fi
    # Not `sed -i`: GNU takes no argument for it and BSD/macOS requires
    # one, so the GNU spelling fails outright on a local devkitPro
    # install on macOS — and it fails on the *first* configure there,
    # before Meson ever runs. Rewriting through a temp file is the same
    # idiom the rest of scripts/ uses and needs no sed at all. chmod
    # before the move: the launcher has to stay executable.
    _hz_tmp="$_hz_launcher.tmp.$$"
    {
        printf '%s\n' '#!/usr/bin/env python3'
        tail -n +2 "$_hz_launcher"
    } > "$_hz_tmp"
    chmod +x "$_hz_tmp"
    mv "$_hz_tmp" "$_hz_launcher"
    echo "meson: rewrote the launcher shebang to /usr/bin/env python3"
}

# Idempotent: a second call with the pin unchanged does nothing.
horizon_ensure_meson() {
    if [ -x "$HORIZON_MESON_DIR/bin/meson" ]; then
        echo "meson ${MESON_VERSION}: already installed in $HORIZON_MESON_DIR"
        horizon_fix_meson_shebang
        return 0
    fi
    echo "installing pinned meson ${MESON_VERSION} into $HORIZON_MESON_DIR"
    mkdir -p "$HORIZON_MESON_DIR"
    # Installed on the host, where the network is: the toolchain image
    # has no pip and no outbound connectivity.
    python3 -m pip install --quiet --no-cache-dir \
        --target "$HORIZON_MESON_DIR" "meson==${MESON_VERSION}"
    horizon_fix_meson_shebang
}
