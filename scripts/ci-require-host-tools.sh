#!/usr/bin/env bash
# Checks, before anything slow starts, that this machine has the tools
# the fetch steps run *on the host*.
#
#   scripts/ci-require-host-tools.sh
#
# WHY THIS EXISTS, and it is a real failure rather than a precaution.
# The first `archives` run on Forgejo got 50 seconds in — Mesa cloned,
# 73 patches applied, 29 Rust wraps downloaded — and then died on
#
#     scripts/fetch-rust-tools.sh: line 125: cargo: command not found
#
# because ghcr.io/catthehacker/ubuntu:runner-latest ships no Rust. Worse,
# ci-build-archives.sh wraps the fetches in a retry, so it tried three
# times over 30 seconds for a binary that was never going to appear. A
# retry is for a flaky network; a missing tool is a fact about the
# machine, and facts about the machine are what this asks first.
#
# WHY THE HOST AND NOT THE IMAGE. The toolchain image has no network
# (CLAUDE.md), so everything downloaded is downloaded out here and
# mounted in. `cargo vendor` in particular is the network step of
# scripts/fetch-rust-tools.sh; its own comment notes that the host's
# cargo version does not matter because nothing compiled is produced.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# tool:why, so a failure says what wanted it rather than just naming it.
TOOLS="
git:fetch-mesa.sh clones the pinned checkout and apply-mesa-patches.sh git-ams the series
python3:the pinned Meson, spv-embed.py and split-status.py all need it
curl:fetch-rust-tools.sh and fetch-clc-deps.sh download the crates and the LLVM debs
tar:unpacking the .crate and .deb payloads
sha256sum:every download is verified against the digest in toolchain/versions.env
cargo:fetch-rust-tools.sh runs 'cargo vendor' for bindgen and cbindgen
"

missing=""
while IFS= read -r line; do
    [ -n "$line" ] || continue
    tool=${line%%:*}
    why=${line#*:}
    if command -v "$tool" >/dev/null 2>&1; then
        printf '  %-10s %s\n' "$tool" "ok"
    else
        printf '  %-10s MISSING — %s\n' "$tool" "$why"
        missing="$missing $tool"
    fi
done <<EOF
$TOOLS
EOF

# rustc only matters with a local devkitA64: in container mode
# fetch-rust-crates.sh reads the sysroot out of the image instead.
# cargo drags rustc in anyway on any normal install, so this is a note,
# not a second requirement.
if [ -n "${DEVKITPRO:-}" ] && ! command -v rustc >/dev/null 2>&1; then
    printf '  %-10s MISSING — %s\n' rustc \
        "with \$DEVKITPRO set, fetch-rust-crates.sh reads rustc's sysroot here"
    missing="$missing rustc"
fi

if [ -n "$missing" ]; then
    echo >&2
    echo "error: missing on this machine:$missing" >&2
    echo >&2
    case "$missing" in
    *cargo*|*rustc*)
        echo "       Rust is not in most CI runner images. It is needed here" >&2
        echo "       only to vendor sources — nothing is compiled with it —" >&2
        echo "       so any recent toolchain does:" >&2
        echo >&2
        # Printed with single quotes on purpose. Escaping the variables
        # instead would put a letter, a colon and a backslash next to
        # each other in this file, and scripts/check-no-abs-paths.sh
        # reads that as a Windows drive letter — it failed on exactly
        # this line before it was rewritten this way.
        echo "         curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \\" >&2
        echo '             | sh -s -- -y --profile minimal --no-modify-path' >&2
        echo '         export PATH="$HOME/.cargo/bin:$PATH"' >&2
        echo >&2
        ;;
    esac
    echo "       docs/BUILDING.md lists what the build expects to find." >&2
    exit 1
fi

echo "ci-require-host-tools: OK"
