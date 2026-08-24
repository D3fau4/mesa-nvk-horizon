#!/usr/bin/env bash
# Materialises mesa/ at the commit pinned in toolchain/versions.env
# (decision D2/D3).
#
#   scripts/fetch-mesa.sh [--force]
#
# mesa/ is a pinned checkout, never our source: our changes to Mesa live
# as patches in mesa-patches/ instead. D3 chose a script-fetched
# checkout over a submodule so the pin lives in exactly one place —
# MESA_COMMIT in toolchain/versions.env — instead of being split between
# a gitlink and a text file, and so a patched Phase 3 tree is not
# permanently a dirty submodule.
#
# Idempotent: if mesa/ is already at MESA_COMMIT it does nothing. It
# never discards a modified tree; --force is required for that, and even
# then it says what it is dropping.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# Same reason as scripts/apply-mesa-patches.sh, which carries the long
# version: with GIT_WORK_TREE set, the git-dir assertion below still
# passes while every write lands somewhere else entirely.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
      GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_NAMESPACE \
      GIT_CEILING_DIRECTORIES GIT_DISCOVERY_ACROSS_FILESYSTEM

# shellcheck source=../toolchain/versions.env
. toolchain/versions.env

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

DEST=mesa

at_pinned_commit() {
    [ -d "$DEST/.git" ] || return 1
    [ "$(git -C "$DEST" rev-parse HEAD 2>/dev/null)" = "$MESA_COMMIT" ]
}

# mesa/.gitkeep belongs to *this* repository — it is what keeps the
# directory present in our tree — and is therefore always untracked from
# Mesa's point of view. It is not a local modification, and treating it
# as one would make every run report a dirty tree and refuse to work.
mesa_dirty() {
    [ -d "$DEST/.git" ] || return 1
    [ -n "$(git -C "$DEST" status --porcelain 2>/dev/null |
            grep -v '^?? \.gitkeep$')" ]
}

# HEAD is MESA_COMMIT plus commits on top — which is exactly what
# scripts/apply-mesa-patches.sh produces. The tree is CLEAN in that
# state, so neither at_pinned_commit nor mesa_dirty fires and the fetch
# below would check the tag back out, silently un-applying the series.
descends_from_pinned() {
    [ -d "$DEST/.git" ] || return 1
    git -C "$DEST" cat-file -e "${MESA_COMMIT}^{commit}" 2>/dev/null || return 1
    git -C "$DEST" merge-base --is-ancestor "$MESA_COMMIT" HEAD 2>/dev/null
}

if at_pinned_commit; then
    if mesa_dirty; then
        echo "fetch-mesa: $DEST is at $MESA_TAG but has local modifications."
        echo "            Leaving them alone — they are probably applied"
        echo "            mesa-patches/. Use --force to discard."
        [ "$FORCE" -eq 0 ] && exit 0
    else
        echo "fetch-mesa: $DEST already at $MESA_TAG ($MESA_COMMIT); nothing to do"
        exit 0
    fi
fi

if descends_from_pinned; then
    n=$(git -C "$DEST" rev-list --count "${MESA_COMMIT}..HEAD")
    echo "fetch-mesa: $DEST is at $MESA_TAG plus $n local commit(s) — the"
    echo "            applied mesa-patches/ series. Nothing to do."
    if mesa_dirty; then
        echo "            (It also has uncommitted changes.)"
    fi
    echo "            Use --force to reset it to $MESA_COMMIT, dropping"
    echo "            those commits; scripts/apply-mesa-patches.sh puts"
    echo "            the series back."
    [ "$FORCE" -eq 0 ] && exit 0
fi

if [ "$FORCE" -eq 0 ] && mesa_dirty; then
    echo "error: $DEST has local modifications and is not at $MESA_COMMIT." >&2
    echo "       Refusing to overwrite. Inspect it, or re-run with --force." >&2
    exit 1
fi

mkdir -p "$DEST"

# Path 1: git. `git init` + fetch rather than `git clone`, because
# mesa/ already exists (it holds the tracked .gitkeep) and clone
# refuses a non-empty directory. Fetching the tag and then checking the
# SHA is more honest than trusting the tag: tags can be moved.
fetch_via_git() {
    # Test for the directory, never `git -C "$DEST" rev-parse
    # --git-dir`: mesa/ sits inside this repository, so before it has a
    # .git of its own that query succeeds by walking up and answering
    # for the PARENT. Every later `git -C "$DEST" ...` would then
    # operate on mesa-nvk-horizon itself — repointing our origin at Mesa
    # and checking Mesa's tree out over our working tree.
    [ -d "$DEST/.git" ] || git init -q "$DEST"

    # Belt and braces: prove mesa/ really is its own repository before
    # running anything that writes. A wrong answer here is destructive,
    # so it aborts rather than falling through to the archive path.
    gitdir=$(git -C "$DEST" rev-parse --absolute-git-dir 2>/dev/null || true)
    want="$(cd "$DEST" && pwd)/.git"
    if [ "$gitdir" != "$want" ]; then
        echo "error: $DEST did not become its own git repository." >&2
        echo "       git reports its git-dir as: ${gitdir:-<none>}" >&2
        echo "       expected: $want" >&2
        echo "       Refusing to run git commands that would act on the" >&2
        echo "       enclosing repository instead." >&2
        exit 1
    fi

    if git -C "$DEST" remote get-url origin >/dev/null 2>&1; then
        git -C "$DEST" remote set-url origin "$MESA_REPO"
    else
        git -C "$DEST" remote add origin "$MESA_REPO"
    fi
    git -C "$DEST" fetch --depth 1 --quiet origin \
        "refs/tags/${MESA_TAG}:refs/tags/${MESA_TAG}"
    git -C "$DEST" checkout -q --detach "refs/tags/${MESA_TAG}"
}

# Path 2: tarball. Some networks block git-over-https but allow plain
# HTTPS GETs. GitLab serves an archive addressed by *commit*, so the
# content is the pinned commit by construction even though the result
# has no .git to verify against.
fetch_via_archive() {
    url="${MESA_ARCHIVE_BASE}/${MESA_COMMIT}/mesa-${MESA_COMMIT}.tar.gz"
    tarball="build/toolchain/mesa-${MESA_COMMIT}.tar.gz"
    mkdir -p build/toolchain
    echo "fetch-mesa: downloading $url"
    curl -fsSL --retry 3 -o "$tarball.part" "$url"
    mv "$tarball.part" "$tarball"
    # Keep .gitkeep, replace everything else.
    find "$DEST" -mindepth 1 -maxdepth 1 ! -name .gitkeep -exec rm -rf {} +
    tar -xzf "$tarball" -C "$DEST" --strip-components=1
    printf '%s\n' "$MESA_COMMIT" > "$DEST/.mesa-nvk-horizon-commit"
    echo "fetch-mesa: extracted from archive; no .git, commit recorded in"
    echo "            $DEST/.mesa-nvk-horizon-commit (addressed by SHA, so"
    echo "            the content is the pinned commit by construction)"
}

echo "fetch-mesa: fetching $MESA_TAG ($MESA_COMMIT) into $DEST/"

# stderr is deliberately not swallowed: when the git path fails, the
# reason is the only useful thing on screen.
if fetch_via_git; then
    got=$(git -C "$DEST" rev-parse HEAD)
    if [ "$got" != "$MESA_COMMIT" ]; then
        echo "error: $MESA_TAG resolved to $got, not the pinned" >&2
        echo "       $MESA_COMMIT. The tag moved, or versions.env is" >&2
        echo "       wrong. Refusing to continue." >&2
        exit 1
    fi
    echo "fetch-mesa: checked out $MESA_TAG, verified HEAD = $MESA_COMMIT"
else
    echo "fetch-mesa: git fetch failed; falling back to the archive"
    fetch_via_archive
fi

echo "fetch-mesa: $DEST ready ($(du -sh "$DEST" | cut -f1))"
