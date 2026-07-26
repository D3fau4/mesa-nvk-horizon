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
# Python modules Mesa's generators need, kept separate from Meson's
# install so either can be reprovisioned without disturbing the other.
HORIZON_PYTHON_DIR="build/toolchain/python"

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

    # The Switch toolchain belongs to the environment (see the
    # ENVIRONMENT header in toolchain/versions.env), so the image is
    # referenced by its tag and whatever that tag currently resolves to
    # is what gets used. Nothing here pins or updates it.
    #
    # HORIZON_NX_IMAGE overrides the reference entirely — including with
    # a digest, when rebuilding the artefacts behind a recorded hardware
    # run (build/pkg/MANIFEST.txt prints the exact command).
    HORIZON_IMAGE="${HORIZON_NX_IMAGE:-${HORIZON_NX_IMAGE_REPO}:${HORIZON_NX_IMAGE_TAG}}"
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
    _hz_bins="${HORIZON_DEVKITPRO}/${HORIZON_TOOLCHAIN_BINDIR_REL}:${HORIZON_DEVKITPRO}/${HORIZON_TOOLS_BINDIR_REL}"
    if [ "$HORIZON_IN_CONTAINER" -eq 0 ]; then
        # A local install's rustc is already on the developer's PATH.
        PATH="${_hz_bins}:${PATH}" "$@"
    else
        # The image keeps rustup outside the default PATH; Mesa's build
        # needs rustc for NAK/NIL, so add it here rather than making
        # every caller know where it lives.
        docker run --rm \
            -e DEVKITPRO="$HORIZON_DEVKITPRO" \
            -e "PATH=${_hz_bins}:${RUST_CARGO_HOME_IN_IMAGE}/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
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
horizon_image_digest() {
    if [ "$HORIZON_IN_CONTAINER" -eq 0 ]; then
        echo "local"
        return 0
    fi
    docker image inspect --format '{{range .RepoDigests}}{{println .}}{{end}}' \
        "$HORIZON_IMAGE" 2>/dev/null |
        grep -m1 "^${HORIZON_NX_IMAGE_REPO}@" |
        sed 's/.*@//' |
        grep . || echo unknown
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
