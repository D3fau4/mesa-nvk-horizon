#!/usr/bin/env bash
# Builds every static archive the tests link, and proves each one is
# there afterwards.
#
#   scripts/ci-build-archives.sh
#
# WHY THIS EXISTS. Until now nothing ever compiled Mesa or NVK outside a
# developer's machine. The gates check layering, paths, manifests and
# host logic; the cross gate builds `libhorizon_gpu.a` and the eighteen
# `.nro` that need nothing else. The other nineteen archives — the two
# Mesa core ones and the seventeen the Vulkan tests link — were built by
# whoever happened to run the scripts, which means a broken patch in
# `mesa-patches/` could sit in the tree with every check green.
#
# WHAT "EVERY ARCHIVE" MEANS, and it is not a list kept here. It is
# $HORIZON_MESA_TEST_LIBS and $HORIZON_NVK_TEST_LIBS from
# scripts/toolchain-env.sh, which meson.build reads through the same
# names and scripts/check-mesa-test-parity.sh fails on if the two ever
# disagree. A second copy of the list in this file would be a third
# place to drift.
#
# THE ORDER IS NOT A PREFERENCE. Material is fetched on the host because
# containers here have no network (CLAUDE.md); the derived image is
# built before anything that needs Rust or libclang; mesa_clc is native
# and must exist before the cross build reads `-Dmesa-clc=system`.
#
# NETWORK STEPS ARE RETRIED. Measured while writing this: a single
# `pip` read timeout against files.pythonhosted.org failed the whole
# chain sixteen seconds in, and the identical command succeeded on the
# next run. A fetch that fails once is not a broken tree, and a CI job
# that reports it as one teaches people to ignore it.
#
# This is a CROSS BUILD. It says nothing about a console.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

step() { printf '\n=== %s\n' "$*" >&2; }

# Three attempts, backing off, and only around the steps that reach the
# network. A build step that fails is a real failure and is not retried:
# retrying a compile error just takes three times as long to report it.
retry() { # description, command...
    _rt_what="$1"; shift
    _rt_n=1
    while :; do
        if "$@"; then return 0; fi
        if [ "$_rt_n" -ge 3 ]; then
            echo "ci-build-archives: $_rt_what failed 3 times; giving up" >&2
            return 1
        fi
        _rt_sleep=$((_rt_n * 10))
        echo "ci-build-archives: $_rt_what failed (attempt $_rt_n);" \
             "retrying in ${_rt_sleep}s" >&2
        sleep "$_rt_sleep"
        _rt_n=$((_rt_n + 1))
    done
}

if [ -n "${DEVKITPRO:-}" ]; then
    echo "ci-build-archives: using the local devkitA64 at \$DEVKITPRO" >&2
else
    docker info >/dev/null 2>&1 || {
        echo "error: \$DEVKITPRO is not set and the docker daemon is not" >&2
        echo "       reachable. This job drives containers itself — it does" >&2
        echo "       not run inside one — so the runner needs the docker" >&2
        echo "       CLI and a usable /var/run/docker.sock." >&2
        exit 1
    }
    echo "ci-build-archives: container mode, base image $HORIZON_BASE_IMAGE" >&2
fi

# --- 1. material, all of it host-side ---------------------------------
step "fetching the pinned Mesa checkout"
retry "fetch-mesa"             scripts/fetch-mesa.sh

step "applying the mesa-patches series"
scripts/apply-mesa-patches.sh

step "fetching Mesa's Rust subprojects"
retry "fetch-mesa-subprojects" scripts/fetch-mesa-subprojects.sh

step "fetching bindgen, cbindgen and libclang"
retry "fetch-rust-tools"       scripts/fetch-rust-tools.sh

step "vendoring the Rust standard-library dependencies"
retry "fetch-rust-crates"      scripts/fetch-rust-crates.sh

step "fetching the LLVM/libclc closure"
retry "fetch-clc-deps"         scripts/fetch-clc-deps.sh

# --- 2. the toolchain -------------------------------------------------
#
# Skipped entirely with a local devkitA64: there is no image in that
# mode, and build-rust-sysroot.sh covers the sysroot instead.
if [ "${HORIZON_IN_CONTAINER:-0}" = "1" ]; then
    step "building the derived toolchain image"
    scripts/build-toolchain-image.sh
else
    step "building the Rust sysroot for $RUST_TARGET"
    scripts/build-rust-sysroot.sh
fi

step "building mesa_clc and vtn_bindgen2 for this machine"
scripts/build-mesa-clc.sh

# --- 3. the archives --------------------------------------------------
step "building Mesa's non-driver core"
scripts/configure-mesa.sh
scripts/build-mesa.sh

step "building the nouveau Vulkan driver"
scripts/build-mesa-nvk.sh

# --- 4. prove it, rather than trusting an exit status -----------------
#
# Every script above can succeed while producing less than it should:
# meson skips targets whose inputs are absent, and says so in one line
# among thousands. The exit status is not the claim being made here.
step "every archive the tests link"

missing=0
report() { # build-dir, archive-list, label
    for lib in $2; do
        if [ -f "$1/$lib" ]; then
            printf '  %-10s %-52s %s\n' "$3" "$lib" \
                "$(wc -c < "$1/$lib" | tr -d ' ') bytes"
        else
            printf '  %-10s %-52s MISSING\n' "$3" "$lib"
            missing=$((missing + 1))
        fi
    done
}

report "$MESA_BUILD_DIR"     "$HORIZON_MESA_TEST_LIBS" "mesa-core"
report "$MESA_NVK_BUILD_DIR" "$HORIZON_NVK_TEST_LIBS"  "nvk"

if [ "$missing" -ne 0 ]; then
    echo >&2
    echo "ci-build-archives: $missing archive(s) missing — see MISSING above" >&2
    exit 1
fi

# The helpers the rest of the tree decides with, asked directly. If
# these disagree with the loop above, the loop is reading a different
# directory than the build did, which is worth failing over.
horizon_mesa_libs_present || {
    echo "ci-build-archives: horizon_mesa_libs_present says no" >&2; exit 1; }
horizon_nvk_libs_present || {
    echo "ci-build-archives: horizon_nvk_libs_present says no" >&2; exit 1; }

# --- 5. and the end-to-end claim --------------------------------------
#
# The archives existing is not the same as the build being able to use
# them, and here that is not a figure of speech: Meson writes THIN
# archives. Measured — libnvk.a is 96 KiB, begins with `!<thin>` and
# names 179 members it does not contain. Every check above, and
# meson.build's own fs.exists(), is therefore satisfied by a file whose
# objects could all be missing.
#
# Linking is what cannot be faked. 18 horizon + 2 Mesa + 12 NVK = 32,
# and t_vk_swapchain.nro comes out at 14 MiB against t_init.nro's 237
# KiB, which is the driver actually being in there.
step "rebuilding the .nro against them"
scripts/build-horizon.sh

n=$(find "$HORIZON_BUILD_DIR" -maxdepth 1 -name '*.nro' | wc -l)
echo "ci-build-archives: $n .nro in $HORIZON_BUILD_DIR"
if [ "$n" -ne 32 ]; then
    echo "error: expected 32 .nro with every archive present" >&2
    echo "       (18 horizon_gpu + 2 Mesa + 12 NVK); got $n." >&2
    echo "       A lower count means meson skipped tests whose archives" >&2
    echo "       it could not find — check the configure output above." >&2
    exit 1
fi

# --- 6. the two gates that were manual only for want of artefacts -----
#
# .github/workflows/gates.yml used to say these "stay manual until
# somebody decides that cost is worth paying", because they read a
# linked Horizon ELF and devkitA64 objects and CI had neither. This job
# has just produced both, so the cost is already paid and running them
# here is free. Naming a gap was the right thing to do while it was
# one; leaving it named after it closed would not be.
step "the gates that need built artefacts"
scripts/check-dispatch-complete.sh
scripts/check-tls-relocs.sh

echo
echo "ci-build-archives: OK — every archive built, 32 .nro link them, and"
echo "ci-build-archives: the dispatch and TLS gates ran against them."
echo "ci-build-archives: this is a CROSS BUILD; no console has run any of it."
