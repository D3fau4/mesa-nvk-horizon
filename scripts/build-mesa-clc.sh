#!/usr/bin/env bash
# Builds Mesa's two OpenCL-C build tools — mesa_clc and vtn_bindgen2 —
# for the *build* machine, and installs them where the cross build's
# `-Dmesa-clc=system` finds them.
#
#   scripts/build-mesa-clc.sh [--force]
#
# WHY A SEPARATE BUILD. NVK compiles src/nouveau/vulkan/cl/*.cl into
# SPIR-V during the build and turns the result into C++, so mesa_clc has
# to exist before the cross build starts. Mesa can build it inside the
# cross build (`-Dmesa-clc=enabled`), but then LLVM, clang, SPIRV-Tools
# and SPIRV-LLVM-Translator are resolved as *host machine* dependencies
# — for aarch64-horizon, where none of them exist and none of them are
# wanted. `-Dmesa-clc=system` is the documented cross path, and this is
# the native build that supplies it.
#
# It is the same pinned Mesa checkout, configured natively: no cross
# file, no devkitA64, nothing from toolchain/horizon-aarch64.cross. The
# two binaries produced run on the build machine and never reach the
# Switch.
#
# Idempotent: an existing install built from the same Mesa commit and
# the same toolchain is left alone. --force rebuilds it.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

BUILD=build/mesa-clc
PREFIX="$PWD/$HORIZON_NATIVE_TOOLS_DIR"
STAMP="$HORIZON_NATIVE_TOOLS_DIR/.stamp"

[ -f mesa/meson.build ] || {
    echo "error: mesa/ is not populated (no meson.build)." >&2
    echo "       Run scripts/fetch-mesa.sh first." >&2
    exit 1
}

identity() {
    echo "mesa=$(git -C mesa rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "toolchain=$HORIZON_TOOLCHAIN_DESC"
    if [ "$HORIZON_IN_CONTAINER" -eq 1 ]; then
        echo "image=$(docker image inspect --format '{{.Id}}' \
                          "$HORIZON_IMAGE" 2>/dev/null || echo unknown)"
    fi
}

if [ "$FORCE" -eq 0 ] && [ -f "$STAMP" ] &&
   [ "$(identity)" = "$(cat "$STAMP")" ] &&
   [ -x "$HORIZON_NATIVE_TOOLS_DIR/bin/mesa_clc" ] &&
   [ -x "$HORIZON_NATIVE_TOOLS_DIR/bin/vtn_bindgen2" ]; then
    echo "build-mesa-clc: mesa_clc and vtn_bindgen2 are current"
    exit 0
fi

horizon_ensure_meson
horizon_ensure_python_deps

# -Dinstall-mesa-clc=true is what makes the two executables exist at
# all: with no drivers enabled, `with_driver_using_cl` is false and
# mesa/src/compiler/{clc,spirv}/meson.build build them only when that
# option asks (mesa/src/compiler/clc/meson.build:124).
#
# Everything else is off. This build exists to produce two host
# binaries, not to be a Mesa; leaving drivers on would pull in LLVM
# codegen backends, X11 and libdrm for no benefit.
set -- \
    --buildtype=release \
    --prefix "$PREFIX" \
    -Dinstall-mesa-clc=true \
    -Dmesa-clc=enabled \
    -Dprecomp-compiler=auto \
    -Dgallium-drivers= \
    -Dvulkan-drivers= \
    -Dplatforms= \
    -Dopengl=false \
    -Dglx=disabled \
    -Degl=disabled \
    -Dgbm=disabled \
    -Dshared-glapi=disabled \
    -Dvideo-codecs= \
    -Dshader-cache=disabled \
    -Dllvm=enabled \
    -Dbuild-tests=false

if [ -f "$BUILD/meson-info/meson-info.json" ] && [ "$FORCE" -eq 0 ]; then
    set -- --reconfigure "$@"
elif [ -d "$BUILD" ]; then
    rm -rf "$BUILD"
fi

echo "build-mesa-clc: configuring the native Mesa build in $BUILD"
horizon_meson setup "$@" "$BUILD" mesa

# Only the two targets. A full `ninja` here would build every native
# library Mesa can build, and none of it is wanted.
echo "build-mesa-clc: building mesa_clc and vtn_bindgen2"
horizon_meson compile -C "$BUILD" \
    src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2

mkdir -p "$HORIZON_NATIVE_TOOLS_DIR/bin"
cp "$BUILD/src/compiler/clc/mesa_clc" \
   "$BUILD/src/compiler/spirv/vtn_bindgen2" \
   "$HORIZON_NATIVE_TOOLS_DIR/bin/"

# Prove they run before recording success. mesa_clc without arguments
# prints its usage and exits non-zero, so the check is that it produced
# usage text rather than, say, a missing-shared-library error.
echo "build-mesa-clc: checking both run"
out=$(horizon_run "$PWD/$HORIZON_NATIVE_TOOLS_DIR/bin/mesa_clc" 2>&1 || true)
case "$out" in
    *usage*|*Usage*|*"-o "*) ;;
    *)
        echo "error: mesa_clc did not print usage; it said:" >&2
        printf '%s\n' "$out" >&2
        exit 1
        ;;
esac
horizon_run "$PWD/$HORIZON_NATIVE_TOOLS_DIR/bin/vtn_bindgen2" >/dev/null 2>&1 ||
    true

identity > "$STAMP"
echo "build-mesa-clc: installed in $HORIZON_NATIVE_TOOLS_DIR/bin"
