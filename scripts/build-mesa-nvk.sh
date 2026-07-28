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

if [ ! -f "$MESA_NVK_BUILD_DIR/build.ninja" ]; then
    scripts/configure-mesa-nvk.sh
fi

horizon_meson compile -C "$MESA_NVK_BUILD_DIR" "$@"

# The same property scripts/build-mesa.sh checks on the C objects, on
# the artefacts this build produces. -mtp=soft -fPIC miscompiles
# thread-local storage on this toolchain (STATUS.md), and a Rust
# staticlib is one more place TLS could appear.
for _lib in "src/nouveau/vulkan/libnvk.a" \
            "src/nouveau/rust_runtime/libnouveau_rust_runtime.a"; do
    [ -f "$MESA_NVK_BUILD_DIR/$_lib" ] || continue
    _n=$(horizon_run "${DEVKITA64_TOOL_PREFIX}objdump" -r \
             "$MESA_NVK_BUILD_DIR/$_lib" 2>/dev/null |
         grep -c 'R_AARCH64_TLS' || true)
    echo "build-mesa-nvk: $_lib — $_n TLS relocation(s)"
done

echo "build-mesa-nvk: $MESA_NVK_BUILD_DIR"
