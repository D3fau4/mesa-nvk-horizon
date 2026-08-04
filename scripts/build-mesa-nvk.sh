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
scripts/build-horizon.sh
rm -rf "$HORIZON_GPU_PREFIX"
mkdir -p "$HORIZON_GPU_PREFIX/include" "$HORIZON_GPU_PREFIX/lib"
cp -a horizon/include/horizon_gpu "$HORIZON_GPU_PREFIX/include/"
cp "$HORIZON_BUILD_DIR/libhorizon_gpu.a" "$HORIZON_GPU_PREFIX/lib/"

horizon_meson compile -C "$MESA_NVK_BUILD_DIR" "$@"

# The same property scripts/build-mesa.sh checks on the C objects, on
# the artefacts this build produces. -mtp=soft -fPIC miscompiles
# thread-local storage on this toolchain (STATUS.md), and a Rust
# staticlib is one more place TLS could appear.
#
# This has to be the gate, not a count. The miscompile's signature is a
# call to __aarch64_read_tp with NO relocation following it, so the
# broken build reports *zero* TLS relocations — the same number a build
# with no thread-locals at all reports, and the number a bare `grep -c`
# prints as if it were good news. check-tls-relocs.sh is what
# distinguishes the two, by looking for the call rather than for the
# relocation; build-mesa.sh has always run it and this path did not.
scripts/check-tls-relocs.sh "$MESA_NVK_BUILD_DIR"

# The counts stay, after the gate rather than instead of it: they say
# how much TLS the two archives actually carry, which the pass/fail
# result deliberately does not.
for _lib in "src/nouveau/vulkan/libnvk.a" \
            "src/nouveau/rust_runtime/libnouveau_rust_runtime.a"; do
    [ -f "$MESA_NVK_BUILD_DIR/$_lib" ] || continue
    _n=$(horizon_run "${DEVKITA64_TOOL_PREFIX}objdump" -r \
             "$MESA_NVK_BUILD_DIR/$_lib" 2>/dev/null |
         grep -c 'R_AARCH64_TLS' || true)
    echo "build-mesa-nvk: $_lib — $_n TLS relocation(s)"
done

echo "build-mesa-nvk: $MESA_NVK_BUILD_DIR"
