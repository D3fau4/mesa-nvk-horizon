#!/usr/bin/env bash
# Downloads the Meson wrap subprojects Mesa's Rust half needs, on the
# host, into mesa/subprojects/.
#
#   scripts/fetch-mesa-subprojects.sh [--force] [name...]
#
# WHY. Mesa resolves paste, rustc-hash and syn — and their transitive
# crates — through `dependency(..., fallback : [...])`, which downloads
# a wrap at *configure* time. Configure runs inside the toolchain image,
# and containers here have no network, so it fails with
#
#   ERROR: could not get https://crates.io/api/v1/crates/syn/2.0.87/download;
#          is the internet available?
#
# Same host/container split as every other fetch script here. Nothing is
# pinned by this file: each wrap carries its own source_hash, which
# Meson verifies, and those hashes belong to the pinned Mesa commit.
#
# The default list is every Rust wrap in the tree rather than the exact
# closure. The closure is only knowable by configuring, configuring is
# what this unblocks, and each crate is a few hundred kilobytes — so
# the alternative is a download-configure-download loop that costs more
# than the extra files.
#
# Idempotent: Meson leaves an already-downloaded subproject alone.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

FORCE=0
if [ "${1:-}" = "--force" ]; then
    FORCE=1
    shift
fi

[ -f mesa/meson.build ] || {
    echo "error: mesa/ is not populated (no meson.build)." >&2
    echo "       Run scripts/fetch-mesa.sh first." >&2
    exit 1
}

horizon_ensure_meson

if [ "$#" -gt 0 ]; then
    set -- "$@"
else
    # Every *-rs wrap. `set --` from a glob so the list is whatever the
    # pinned tree actually ships, not a copy of it that can go stale.
    subs=""
    for w in mesa/subprojects/*-rs.wrap; do
        [ -f "$w" ] || continue
        name=$(basename "$w" .wrap)
        subs="$subs $name"
    done
    # shellcheck disable=SC2086 # deliberate word splitting: a name list
    set -- $subs
fi

echo "fetch-mesa-subprojects: downloading $# wrap(s)"

# Run on the host, where the network is — not through horizon_run.
# `meson subprojects download` needs nothing from the cross toolchain:
# it fetches a tarball, checks its hash and unpacks it.
if [ "$FORCE" -eq 1 ]; then
    for name in "$@"; do
        dir=$(sed -n 's/^directory *= *//p' "mesa/subprojects/$name.wrap")
        [ -n "$dir" ] && rm -rf "mesa/subprojects/$dir"
    done
fi

# -j1: `meson subprojects download` otherwise runs each wrap's fetch in
# its own thread of one ThreadPoolExecutor, and on Windows the
# subprojects-directory lock (msvcrt.locking(), in Meson's DirectoryLock)
# is not safely reentrant across threads of the same process the way
# POSIX fcntl locks are — two threads racing to lock the same
# .wraplock fail with "OSError: [Errno 36] Resource deadlock avoided"
# instead of one waiting for the other. Serial downloads sidestep the
# race; the cost is small; each wrap is a few hundred kilobytes.
PYTHONPATH="$PWD/$HORIZON_MESON_DIR:$PWD/$HORIZON_PYTHON_DIR" \
    python3 "$PWD/$HORIZON_MESON_DIR/bin/meson" \
    subprojects download --sourcedir mesa -j1 "$@"

# Nothing here writes into mesa/ beyond what Meson unpacks, and that is
# deliberate: Mesa's own subprojects/.gitignore already ignores every
# downloaded directory (`*` with `!*.wrap`), so `git -C mesa status`
# stays clean and scripts/apply-mesa-patches.sh — which refuses to run
# on a dirty tree — is unaffected. Checked rather than assumed: an
# earlier version of this script wrote its own .gitignore there and, by
# doing so, was itself the only thing making the tree dirty.
if [ -n "$(git -C mesa status --porcelain 2>/dev/null |
           grep -v '^?? \.gitkeep$' || true)" ]; then
    echo "warning: mesa/ is not clean after downloading subprojects." >&2
    echo "         scripts/apply-mesa-patches.sh will refuse to run." >&2
    git -C mesa status --porcelain | grep -v '^?? \.gitkeep$' >&2
fi

echo "fetch-mesa-subprojects: done"
