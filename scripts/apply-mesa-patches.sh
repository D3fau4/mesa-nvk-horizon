#!/usr/bin/env bash
# Applies mesa-patches/*.patch onto the pinned mesa/ checkout with
# `git am`, so our Mesa delta stays a reviewable patch series and never
# an edited copy of a Mesa file (CLAUDE.md rejected design 7).
#
#   scripts/apply-mesa-patches.sh            # apply what is missing
#   scripts/apply-mesa-patches.sh --list     # report state, write nothing
#
# The series is applied on top of MESA_COMMIT, in file-name order. Each
# patch is a `git format-patch` file whose commit subject is its
# identity — see mesa-patches/README.md for the header convention.
#
# Idempotent: a second run applies nothing and says so. Non-destructive:
# every path that could write is guarded, and a tree it does not fully
# understand is a reason to stop, never to reset.
#
# WHY THE GIT-DIR ASSERTION BELOW EXISTS. mesa/ sits inside this
# repository. Before it has a .git of its own, `git -C mesa rev-parse`
# succeeds by walking up and answering for the PARENT — which during
# Phase 2 caused a fetch script to run its checkout against
# mesa-nvk-horizon itself (recorded at the end of STATUS.md). Anything
# here that writes therefore proves first that mesa/ really is its own
# repository, and aborts if it is not.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=../toolchain/versions.env
. toolchain/versions.env

DEST=mesa
PATCHDIR=mesa-patches

# git needs an identity to record a commit. Taking it from the
# developer's global config would make the applied commits differ
# between machines for no reason; the series is regenerated from these
# files anyway, so a fixed, obviously-non-personal identity is honest.
AM_NAME='mesa-nvk-horizon'
AM_EMAIL='mesa-nvk-horizon@localhost'

LIST=0
case "${1:-}" in
    --list) LIST=1 ;;
    '') ;;
    *)
        echo "error: unknown argument '$1'." >&2
        echo "       usage: scripts/apply-mesa-patches.sh [--list]" >&2
        exit 1
        ;;
esac

die() { # message...
    for line in "$@"; do echo "$line" >&2; done
    exit 1
}

# --- guards, all before anything that writes ------------------------

[ -d "$DEST" ] || die \
    "error: $DEST/ does not exist." \
    "       Run scripts/fetch-mesa.sh first."

# Test the directory; never ask git whether mesa/ is a repository (see
# the header). A missing .git here is the tarball path of
# fetch-mesa.sh, which cannot carry a patch series.
[ -d "$DEST/.git" ] || die \
    "error: $DEST/ has no .git, so it came from the archive fallback." \
    "       git am needs a repository. Re-fetch over git:" \
    "         rm -rf $DEST && mkdir -p $DEST && scripts/fetch-mesa.sh" \
    "       (the archive path is addressed by commit and is content-" \
    "       correct, but it cannot record commits.)"

gitdir=$(git -C "$DEST" rev-parse --absolute-git-dir 2>/dev/null || true)
want="$(cd "$DEST" && pwd)/.git"
[ "$gitdir" = "$want" ] || die \
    "error: $DEST is not its own git repository." \
    "       git reports its git-dir as: ${gitdir:-<none>}" \
    "       expected: $want" \
    "       Refusing to run git commands that would act on the" \
    "       enclosing repository instead."

git -C "$DEST" cat-file -e "${MESA_COMMIT}^{commit}" 2>/dev/null || die \
    "error: the pinned commit $MESA_COMMIT is not in $DEST." \
    "       This is not the tree toolchain/versions.env describes." \
    "       Run scripts/fetch-mesa.sh."

head_commit=$(git -C "$DEST" rev-parse HEAD)
if [ "$head_commit" != "$MESA_COMMIT" ] &&
   ! git -C "$DEST" merge-base --is-ancestor "$MESA_COMMIT" HEAD; then
    die \
        "error: $DEST HEAD ($head_commit) is not $MESA_COMMIT and does" \
        "       not descend from it. The series applies on top of the" \
        "       pinned commit and nowhere else." \
        "       Run scripts/fetch-mesa.sh --force to get back to it." \
        "       (--force discards whatever is there; look first.)"
fi

# mesa/.gitkeep belongs to *this* repository and is always untracked
# from Mesa's point of view — same exclusion scripts/fetch-mesa.sh
# makes, and for the same reason.
dirty=$(git -C "$DEST" status --porcelain | grep -v '^?? \.gitkeep$' || true)
if [ -n "$dirty" ]; then
    die \
        "error: $DEST has uncommitted changes; git am would refuse or" \
        "       apply on top of them. Nothing was written." \
        "$dirty" \
        "       Commit them into a patch, or discard them, then re-run."
fi

# --- what is in the series, and what is already applied --------------

shopt -s nullglob
patches=("$PATCHDIR"/*.patch)
shopt -u nullglob

if [ ${#patches[@]} -eq 0 ]; then
    echo "apply-mesa-patches: no patches in $PATCHDIR/; nothing to do"
    exit 0
fi

# git mailinfo does the unwrapping (a Subject: folded across lines) and
# the [PATCH n/m] stripping, so the comparison below is against the
# subject git am will actually record.
patch_subject() { # patch file
    git mailinfo /dev/null /dev/null < "$1" 2>/dev/null |
        sed -n 's/^Subject: //p'
}

applied=()
while IFS= read -r s; do
    if [ -n "$s" ]; then
        applied+=("$s")
    fi
done < <(git -C "$DEST" log --reverse --format=%s "${MESA_COMMIT}..HEAD")

# The applied commits must be a prefix of the series, in order. Anything
# else — a reordered series, a hand-made commit on top, a patch edited
# after it was applied — is a state this script must not guess about.
pending=()
i=0
for p in "${patches[@]}"; do
    subject=$(patch_subject "$p")
    [ -n "$subject" ] || die \
        "error: $p has no Subject: line; it is not a git format-patch" \
        "       file. See mesa-patches/README.md."
    if [ "$i" -lt "${#applied[@]}" ]; then
        [ "${applied[$i]}" = "$subject" ] || die \
            "error: $DEST's history diverges from the series at" \
            "       commit $((i + 1))." \
            "         in $DEST: ${applied[$i]}" \
            "         in $p: $subject" \
            "       Not guessing. Inspect $DEST, or reset it with" \
            "       scripts/fetch-mesa.sh --force and re-run."
        if [ "$LIST" -eq 1 ]; then
            echo "  applied  $(basename "$p")  $subject"
        fi
    else
        if [ "$LIST" -eq 1 ]; then
            echo "  pending  $(basename "$p")  $subject"
        fi
        pending+=("$p")
    fi
    i=$((i + 1))
done

if [ "${#applied[@]}" -gt "${#patches[@]}" ]; then
    die \
        "error: $DEST has ${#applied[@]} commits on top of" \
        "       $MESA_COMMIT but the series has only ${#patches[@]}." \
        "       Something applied commits this script did not. Nothing" \
        "       was written."
fi

if [ "$LIST" -eq 1 ]; then
    echo "apply-mesa-patches: ${#applied[@]} applied, ${#pending[@]} pending" \
         "(${#patches[@]} in $PATCHDIR/)"
    exit 0
fi

if [ "${#pending[@]}" -eq 0 ]; then
    echo "apply-mesa-patches: all ${#patches[@]} patches already applied;" \
         "nothing to do"
    exit 0
fi

# --- apply -----------------------------------------------------------

echo "apply-mesa-patches: applying ${#pending[@]} of ${#patches[@]} onto" \
     "$(git -C "$DEST" rev-parse --short HEAD)"

# Absolute paths so git's own cwd (inside $DEST) cannot change what they
# mean. $PWD, never a literal (scripts/check-no-abs-paths.sh).
abs_pending=()
for p in "${pending[@]}"; do
    abs_pending+=("$PWD/$p")
    echo "  $(basename "$p")"
done

if ! git -C "$DEST" \
        -c "user.name=$AM_NAME" -c "user.email=$AM_EMAIL" \
        am "${abs_pending[@]}"; then
    echo "apply-mesa-patches: git am failed; aborting it to leave $DEST" >&2
    echo "                    as it was found." >&2
    git -C "$DEST" am --abort || true
    die "error: the series does not apply cleanly to $MESA_COMMIT." \
        "       Regenerate it against the pinned tree; see" \
        "       mesa-patches/README.md."
fi

echo "apply-mesa-patches: $DEST at $(git -C "$DEST" rev-parse --short HEAD)" \
     "— $MESA_COMMIT + ${#patches[@]} patches"
