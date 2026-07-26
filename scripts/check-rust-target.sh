#!/usr/bin/env bash
# Drift check for the Rust target used by NAK/NIL.
#
# We do NOT define a custom Rust target: rustc already ships
# aarch64-nintendo-switch-freestanding (tier 3, os = "horizon").
# toolchain/aarch64-horizon.json is a *snapshot* of that built-in
# target's spec as emitted by the pinned nightly, kept only so a change
# to it is noticed here instead of surfacing as a mysterious codegen or
# link difference later. Tier-3 specs are not stable across nightlies.
#
#   scripts/check-rust-target.sh            # compare, report drift, exit 0
#   scripts/check-rust-target.sh --strict   # exit 1 on drift
#   scripts/check-rust-target.sh --update   # accept the current spec
#
# Drift is reported but does not fail by default: the nightly comes from
# the environment, which this project does not pin (toolchain/versions.env,
# ENVIRONMENT section), so it is expected to move. Use --strict where a
# change is the thing you want to catch.
#
# See docs/rust-toolchain.md for why no sysroot is built.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

SNAPSHOT=toolchain/aarch64-horizon.json

# rustc lives at $CARGO_HOME/bin in the image; on a machine with a local
# devkitA64 it is wherever the user's rustup put it.
if [ "$HORIZON_IN_CONTAINER" -eq 1 ]; then
    RUSTC_PATH="${RUST_CARGO_HOME_IN_IMAGE}/bin"
else
    RUSTC_PATH="${PATH}"
fi

actual=$(horizon_run env "PATH=${RUSTC_PATH}:/usr/local/bin:/usr/bin:/bin" sh -c "
    rustc -Z unstable-options --print target-spec-json \
        --target '${RUST_TARGET}' |
    python3 -c 'import json,sys; json.dump(json.load(sys.stdin), sys.stdout, indent=2, sort_keys=True); print()'
" 2>/dev/null)

if [ -z "$actual" ]; then
    echo "error: could not read the target spec for $RUST_TARGET." >&2
    echo "       Is the pinned nightly present in the toolchain?" >&2
    exit 1
fi

if [ "${1:-}" = "--update" ]; then
    printf '%s\n' "$actual" > "$SNAPSHOT"
    echo "check-rust-target: re-snapshotted $SNAPSHOT from the current environment"
    exit 0
fi

if [ ! -f "$SNAPSHOT" ]; then
    echo "error: $SNAPSHOT is missing; run with --update" >&2
    exit 1
fi

if printf '%s\n' "$actual" | diff -u "$SNAPSHOT" - > /dev/null; then
    echo "check-rust-target: OK ($RUST_TARGET matches the snapshot)"
    exit 0
fi

echo "check-rust-target: DRIFT — the built-in $RUST_TARGET spec changed"
printf '%s\n' "$actual" | diff -u "$SNAPSHOT" - || true
echo
echo "Review the diff: a changed panic strategy, feature set or linker"
echo "flavour changes how NAK/NIL get built."
echo "Accept with: scripts/check-rust-target.sh --update"

if [ "${1:-}" = "--strict" ]; then
    echo "  --strict: treating that as a failure." >&2
    exit 1
fi
# The environment's nightly is not pinned by this project, so a moved
# target spec is news, not a broken build.
exit 0
