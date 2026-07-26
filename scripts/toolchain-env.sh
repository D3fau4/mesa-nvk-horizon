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
#   horizon_run <cmd>...       run in the toolchain (host or container)
#   horizon_meson <args>...    run the pinned Meson in the toolchain
#   horizon_ensure_meson       install the pinned Meson if missing
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

if [ -n "${DEVKITPRO:-}" ]; then
    HORIZON_IN_CONTAINER=0
    HORIZON_DEVKITPRO="$DEVKITPRO"
    HORIZON_TOOLCHAIN_DESC="local devkitA64 at \$DEVKITPRO"
else
    command -v docker >/dev/null 2>&1 || {
        echo "error: \$DEVKITPRO is not set and docker is unavailable." >&2
        echo "       Install devkitA64 or start dockerd; see CLAUDE.md." >&2
        return 1 2>/dev/null || exit 1
    }
    HORIZON_IN_CONTAINER=1
    # Inside the image the prefix is the image's, not this machine's.
    HORIZON_DEVKITPRO="$HORIZON_NX_IMAGE_DEVKITPRO"
    HORIZON_IMAGE="${HORIZON_NX_IMAGE:-${HORIZON_NX_IMAGE_REPO}@${HORIZON_NX_IMAGE_DIGEST}}"
    HORIZON_TOOLCHAIN_DESC="$HORIZON_IMAGE"
fi

export HORIZON_BUILD_DIR HORIZON_CROSS_CONST_FILE HORIZON_CROSS_FILE
export HORIZON_MESON_DIR HORIZON_IN_CONTAINER HORIZON_DEVKITPRO
export HORIZON_TOOLCHAIN_DESC

# Run a command with the cross toolchain reachable. The image's default
# PATH omits devkitA64/bin (measured; recorded in versions.env), and the
# Meson cross file resolves its [binaries] by bare name, so the bin
# directory is prepended in both modes.
#
# In container mode the tree is mounted at the *same* path as on the
# host, so paths in diagnostics, depfiles and Meson's build.ninja are
# valid on both sides — and no container workdir is hardcoded
# (scripts/check-no-abs-paths.sh).
horizon_run() {
    if [ "$HORIZON_IN_CONTAINER" -eq 0 ]; then
        PATH="${HORIZON_DEVKITPRO}/${HORIZON_TOOLCHAIN_BINDIR_REL}:${PATH}" \
            "$@"
    else
        docker run --rm \
            -e DEVKITPRO="$HORIZON_DEVKITPRO" \
            -e "PATH=${HORIZON_DEVKITPRO}/${HORIZON_TOOLCHAIN_BINDIR_REL}:${HORIZON_DEVKITPRO}/tools/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
            -v "$PWD":"$PWD" -w "$PWD" \
            "$HORIZON_IMAGE" "$@"
    fi
}

# Meson is pure Python, so the pinned copy installed on the host runs
# unchanged under the image's python3 (3.11.2). That matters: the image
# has neither meson nor pip, and containers here have no network.
horizon_meson() {
    horizon_run env "PYTHONPATH=$PWD/$HORIZON_MESON_DIR" \
        python3 "$PWD/$HORIZON_MESON_DIR/bin/meson" "$@"
}

# Idempotent: a second call with the pin unchanged does nothing.
horizon_ensure_meson() {
    if [ -x "$HORIZON_MESON_DIR/bin/meson" ]; then
        echo "meson ${MESON_VERSION}: already installed in $HORIZON_MESON_DIR"
        return 0
    fi
    echo "installing pinned meson ${MESON_VERSION} into $HORIZON_MESON_DIR"
    mkdir -p "$HORIZON_MESON_DIR"
    # Installed on the host, where the network is: the toolchain image
    # has no pip and no outbound connectivity.
    python3 -m pip install --quiet --no-cache-dir \
        --target "$HORIZON_MESON_DIR" "meson==${MESON_VERSION}"
}
