#!/usr/bin/env bash
# Builds the nouveau Vulkan driver for Horizon.
#
#   scripts/build-mesa-nvk.sh [ninja targets...]
#
# Configures first if the build directory is not there, so this is the
# one command a developer needs. With no targets it builds everything
# the configuration asks for; libnvk.a and libnouveau_rust_runtime.a are
# the two that matter.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

MESA_NVK_BUILD_DIR="${MESA_NVK_BUILD_DIR:-build/mesa-nvk}"
# Same path configure-mesa-nvk.sh stages into.
HORIZON_GPU_PREFIX=build/toolchain/horizon-gpu

if [ ! -f "$MESA_NVK_BUILD_DIR/build.ninja" ]; then
    scripts/configure-mesa-nvk.sh
fi

# Re-stage horizon_gpu before every build, not only at configure time.
#
# The NVK build compiles against a *copy* of horizon/include/horizon_gpu
# and links a *copy* of libhorizon_gpu.a, both placed by
# configure-mesa-nvk.sh. Staging them only there means an edit to the
# layer's headers or a rebuild of its archive never reaches this build
# until someone happens to reconfigure — and the symptom is a compile
# error about a symbol that plainly exists, or worse, a link against
# yesterday's library with no symptom at all.
#
# Measured: adding HORIZON_GPU_MEM_UNCACHED to horizon/include and
# rebuilding here failed with
#   error: 'HORIZON_GPU_MEM_UNCACHED' undeclared ... did you mean
#          'HORIZON_GPU_MEM_CACHED'?
# with the enumerator sitting in the header the whole time.
#
# `lib` only: this runs before the NVK build exists, so no test here has
# anything valid of NVK's to link against yet. The full set is rebuilt
# below, after NVK's own archives are current.
scripts/build-horizon.sh lib
rm -rf "$HORIZON_GPU_PREFIX"
mkdir -p "$HORIZON_GPU_PREFIX/include" "$HORIZON_GPU_PREFIX/lib"
cp -a horizon/include/horizon_gpu "$HORIZON_GPU_PREFIX/include/"
# Fattened rather than copied: Meson's libhorizon_gpu.a is thin and
# names its objects relative to its own directory, so a plain cp puts an
# unreadable archive here. scripts/configure-mesa-nvk.sh carries the
# measurement; scripts/fatten-archives.sh carries the mechanism.
horizon_fatten_archive "$HORIZON_BUILD_DIR/libhorizon_gpu.a" \
                       "$HORIZON_GPU_PREFIX/lib/libhorizon_gpu.a"

# With no targets: the default set, and then the archives the tests
# link.
#
# THEY ARE NOT THE SAME SET, which is the whole reason this is here.
# Mesa marks an internal static library build_by_default only when
# something in the configuration links it, and this configuration links
# nothing — there is no shared object and no executable, just archives
# for a test in another build directory to pull from. So `meson compile`
# with no arguments produces libnvk.a and the Rust runtime and stops,
# and libnir.a, libvtn.a, libcompiler.a, libvulkan_util.a,
# libvulkan_wsi.a and libxmlconfig.a are never built at all.
#
# Measured on a clean tree, following the documented bring-up exactly:
# after this script reported success, six of the eighteen archives
# meson.build looks for were absent, so its fs.exists() answered "no"
# and t_vulkan — the Phase 4 exit-criterion test — was silently left out
# of build.ninja. It had only ever worked because someone once asked for
# those targets by hand.
#
# ninja rather than `meson compile <target>`: meson resolves a target by
# its declared *name*, and `meson compile src/compiler/nir/libnir.a`
# answers "target not found" (measured). The ninja path is the exact
# string meson.build already lists, so the two cannot drift in spelling.
if [ "$#" -gt 0 ]; then
    horizon_meson compile -C "$MESA_NVK_BUILD_DIR" "$@"
else
    horizon_meson compile -C "$MESA_NVK_BUILD_DIR"
    # shellcheck disable=SC2086 # the list is a space-separated set
    horizon_ninja -C "$MESA_NVK_BUILD_DIR" $HORIZON_NVK_TEST_LIBS
fi

# The same property scripts/build-mesa.sh checks on the C objects, on
# the artefacts this build produces. -mtp=soft -fPIC miscompiles
# thread-local storage on this toolchain — measured, see
# scripts/check-tls-relocs.sh — and a Rust
# staticlib is one more place TLS could appear.
#
# This has to be the gate, not a count. The miscompile's signature is a
# call to __aarch64_read_tp with NO relocation following it, so the
# broken build reports *zero* TLS relocations — the same number a build
# with no thread-locals at all reports, and the number a bare `grep -c`
# prints as if it were good news. check-tls-relocs.sh is what
# distinguishes the two, by looking for the call rather than for the
# relocation; build-mesa.sh has always run it and this path did not.
# Assert the artefacts exist BEFORE gating on them.
#
# Without this the gate is green on an empty directory: check-tls-relocs
# exits 0 with "nothing to check" when it finds no objects, and the loop
# below used to skip a missing archive with `|| continue`. A build that
# produced nothing at all would then report a clean TLS result — which
# is the same "zero is not good news" shape this whole block exists to
# stop, one level up.
for _lib in "src/nouveau/vulkan/libnvk.a" \
            "src/nouveau/rust_runtime/libnouveau_rust_runtime.a"; do
    if [ ! -f "$MESA_NVK_BUILD_DIR/$_lib" ]; then
        echo "build-mesa-nvk: $_lib is missing after a successful build;" \
             "refusing to report a TLS result for artefacts that do not" \
             "exist" >&2
        exit 1
    fi
done

scripts/check-tls-relocs.sh "$MESA_NVK_BUILD_DIR"

# The counts stay, after the gate rather than instead of it: they say
# how much TLS the two archives actually carry, which the pass/fail
# result deliberately does not.
for _lib in "src/nouveau/vulkan/libnvk.a" \
            "src/nouveau/rust_runtime/libnouveau_rust_runtime.a"; do
    _n=$(horizon_run "${DEVKITA64_TOOL_PREFIX}objdump" -r \
             "$MESA_NVK_BUILD_DIR/$_lib" 2>/dev/null |
         grep -c 'R_AARCH64_TLS' || true)
    echo "build-mesa-nvk: $_lib — $_n TLS relocation(s)"
done

# RE-LINK THE TESTS, and this is a correction rather than a tidy-up.
#
# scripts/build-horizon.sh runs near the top of this file, to re-stage
# horizon_gpu before the NVK build compiles against it — and it builds
# the whole Meson directory, including the test .nro files, which link
# NVK's archives. Running it only there meant every test binary was
# linked against the archives as they were BEFORE this run built them:
# one driver build behind, silently, on every single run. Measured:
# after a change to src/vulkan/wsi/wsi_horizon.c, `ninja -C build/meson`
# still had 22 targets to do once this script had "finished".
#
# So the tests are built again here, after the archives they link exist
# in their new form. The second run is a no-op when nothing changed.
scripts/build-horizon.sh test

# THE SUCCESS STAMP, and it is the only honest answer to "are these
# archives current?".
#
# `set -e` is in force, so reaching this line means every step above
# succeeded. Nothing else in the tree can say that: the archives'
# modification times cannot, because Ninja does not touch an archive
# whose inputs did not change — edit only NVK and libvulkan_wsi.a stays
# exactly as it was, correctly. A gate comparing sources against one
# archive therefore reads a perfectly good incremental build as stale,
# which is what the first version of the check in
# scripts/package-horizon.sh did.
#
# Written last, after the tests are relinked, so it also covers them.
#
# Through horizon_run, not a bare `touch`: in container mode
# $MESA_NVK_BUILD_DIR was created by `meson setup` running *inside* the
# toolchain image (horizon_meson, dispatched through horizon_run), which
# runs as the image's own user — root, absent a USER in
# toolchain/Dockerfile — so the directory is root-owned on the host side
# of the bind mount. A host-level `touch` into it fails with "Permission
# denied": creating a file needs write access to the *directory*, which
# the host user does not have, even though every archive inside it is
# perfectly readable. Measured on the first real GitHub Actions run —
# every other write in this script already goes through horizon_meson or
# horizon_ninja and never hit this; this was the one bare exception.
horizon_run touch "$MESA_NVK_BUILD_DIR/.horizon-build-ok"

echo "build-mesa-nvk: $MESA_NVK_BUILD_DIR"
