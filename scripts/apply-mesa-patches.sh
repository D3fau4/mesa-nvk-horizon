#!/usr/bin/env bash
# Applies mesa-patches/*.patch onto the pinned mesa/ checkout with
# `git am`, so our Mesa delta stays a reviewable patch series and never
# an edited copy of a Mesa file (CLAUDE.md rejected design 7).
#
#   scripts/apply-mesa-patches.sh            # apply what is missing
#   scripts/apply-mesa-patches.sh --list     # report state, write nothing
#
# The series is applied on top of MESA_COMMIT, in file-name order. Each
# patch is a `git format-patch` file, identified by its commit subject
# *and* its diff — see mesa-patches/README.md for the header convention.
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

# Git takes these from the environment and they override where a command's
# repository and working tree actually are. GIT_WORK_TREE is the sharp
# one: with it set, `rev-parse --absolute-git-dir` still answers
# mesa/.git — so the assertion below passes — while `status` and `am`
# operate on a completely different tree. Measured, not hypothesised.
# Clearing them is what makes the guards mean what they say.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
      GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_NAMESPACE \
      GIT_CEILING_DIRECTORIES GIT_DISCOVERY_ACROSS_FILESYSTEM

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

dest_abs="$(cd "$DEST" && pwd)"

# BOTH SIDES OF THE COMPARISONS BELOW GO THROUGH `cd ... && pwd`, and that
# is portability rather than style. Git for Windows answers rev-parse in
# its own spelling of an absolute path (C:/Users/...) while the shell's
# pwd answers in the MSYS one (/c/Users/...). The two name the same
# directory and compare unequal as strings, so the assertions fired on a
# tree that was perfectly correct. Canonicalising git's answer the same
# way the expectation was built compares directories instead of spellings,
# and keeps the guard exactly as strict as it was: mesa/.git and nothing
# else. A path git names but the shell cannot enter falls through
# unchanged and still fails the test.
canon() {
    [ -n "$1" ] && (cd "$1" 2>/dev/null && pwd) || printf '%s' "$1"
}

gitdir=$(git -C "$DEST" rev-parse --absolute-git-dir 2>/dev/null || true)
[ "$(canon "$gitdir")" = "$dest_abs/.git" ] || die \
    "error: $DEST is not its own git repository." \
    "       git reports its git-dir as: ${gitdir:-<none>}" \
    "       expected: $dest_abs/.git" \
    "       Refusing to run git commands that would act on the" \
    "       enclosing repository instead."

# The git-dir alone does not say where writes land. core.worktree in
# mesa/.git/config redirects the working tree while leaving the git-dir
# exactly as asserted above, and no environment variable is involved, so
# clearing the environment does not cover this. Ask git where it would
# actually write.
toplevel=$(git -C "$DEST" rev-parse --show-toplevel 2>/dev/null || true)
[ "$(canon "$toplevel")" = "$dest_abs" ] || die \
    "error: $DEST's git repository has a different working tree." \
    "       git reports its top level as: ${toplevel:-<none>}" \
    "       expected: $dest_abs" \
    "       git am would apply the series there, not here. Refusing."

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

# A subject is a label, not an identity. Correcting a patch's diff
# without touching its subject line would otherwise be invisible here:
# the script would report the whole series as applied and every build
# afterwards would be testing the superseded code, which is exactly the
# opposite of what mesa-patches/README.md promises. patch-id hashes the
# diff — ignoring context line numbers, whitespace at line ends and the
# commit message — so it answers "is this the same change?" rather than
# "is this the same title?".
#
# Residual, stated because it is not covered: a change confined to the
# commit *message body* below the subject, with an identical diff, still
# reads as applied. Regenerating the series after such an edit is the
# way to keep mesa/ honest.
file_patch_id() { # patch file
    git patch-id --stable < "$1" 2>/dev/null | cut -d' ' -f1
}

commit_patch_id() { # commit sha
    git -C "$DEST" diff-tree -p --no-commit-id "$1" 2>/dev/null |
        git patch-id --stable 2>/dev/null | cut -d' ' -f1
}

applied_sha=()
applied_subject=()
while IFS=' ' read -r sha subject; do
    if [ -n "$sha" ]; then
        applied_sha+=("$sha")
        applied_subject+=("$subject")
    fi
done < <(git -C "$DEST" log --reverse --format='%H %s' "${MESA_COMMIT}..HEAD")

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
    if [ "$i" -lt "${#applied_sha[@]}" ]; then
        [ "${applied_subject[$i]}" = "$subject" ] || die \
            "error: $DEST's history diverges from the series at" \
            "       commit $((i + 1))." \
            "         in $DEST: ${applied_subject[$i]}" \
            "         in $p: $subject" \
            "       Not guessing. Inspect $DEST, or reset it with" \
            "       scripts/fetch-mesa.sh --force and re-run."
        [ "$(commit_patch_id "${applied_sha[$i]}")" = "$(file_patch_id "$p")" ] || die \
            "error: commit $((i + 1)) in $DEST has the same subject as" \
            "       $p but a different diff." \
            "         $(git -C "$DEST" rev-parse --short "${applied_sha[$i]}")  ${applied_subject[$i]}" \
            "       The patch has been regenerated or edited since it was" \
            "       applied. Reporting it applied would leave every later" \
            "       build testing the superseded code. Reset with" \
            "       scripts/fetch-mesa.sh --force and re-run to pick up" \
            "       the current series."
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

if [ "${#applied_sha[@]}" -gt "${#patches[@]}" ]; then
    die \
        "error: $DEST has ${#applied_sha[@]} commits on top of" \
        "       $MESA_COMMIT but the series has only ${#patches[@]}." \
        "       Something applied commits this script did not. Nothing" \
        "       was written."
fi

if [ "$LIST" -eq 1 ]; then
    echo "apply-mesa-patches: ${#applied_sha[@]} applied, ${#pending[@]} pending" \
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
