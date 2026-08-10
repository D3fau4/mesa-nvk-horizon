#!/usr/bin/env bash
# Creates a GitHub release and uploads assets to it, via gh.
#
#   scripts/ci-github-release.sh <tag> <notes-file> <asset>...
#
# Reads the token from the environment the GitHub runner already sets:
#
#   GH_TOKEN or GITHUB_TOKEN   the run's automatic token, or a personal
#                              one with 'contents: write' when run by hand
#
# WHY gh, AND NOT scripts/ci-forgejo-release.sh's APPROACH. That script
# hand-rolls the release API in python3/urllib because a third-party
# action has to resolve on whatever forge instance the job runs on, and
# on a Forgejo instance `actions/*` points at GitHub and fetches nothing;
# jq is not guaranteed to be on that runner's image either. Neither
# concern applies here: this only ever runs against real GitHub, where
# `gh` is preinstalled on every GitHub-hosted runner and authenticates for
# free from $GH_TOKEN. Reaching for the simpler tool once the constraint
# that ruled it out is gone is the same discipline as everywhere else in
# scripts/ — CLAUDE.md's "no mass refactors" is about not touching what
# does not need to change, not about carrying complexity nothing needs
# any more. ci-forgejo-release.sh itself is untouched: docs/RELEASING.md
# still uses it for the actual Forgejo instance.
#
# Idempotent, same as the Forgejo version: a release that already exists
# for the tag is reused rather than duplicated, and an asset of the same
# name is replaced.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

[ "$#" -ge 3 ] || {
    echo "usage: $0 <tag> <notes-file> <asset>..." >&2
    exit 2
}

TAG="$1"; shift
NOTES="$1"; shift

[ -f "$NOTES" ] || { echo "error: no notes file at $NOTES" >&2; exit 1; }
for a in "$@"; do
    [ -f "$a" ] || { echo "error: no asset at $a" >&2; exit 1; }
done

command -v gh >/dev/null 2>&1 || {
    echo "error: gh (the GitHub CLI) is not on PATH." >&2
    echo "       GitHub-hosted runners carry it already; running by hand" >&2
    echo "       needs https://cli.github.com installed." >&2
    exit 1
}

# gh reads GH_TOKEN first and falls back to GITHUB_TOKEN on its own; this
# only exists to fail with a clear reason instead of gh's generic
# "not logged in" when running by hand without either set.
: "${GH_TOKEN:=${GITHUB_TOKEN:-}}"
export GH_TOKEN
[ -n "$GH_TOKEN" ] || {
    echo "error: neither \$GH_TOKEN nor \$GITHUB_TOKEN is set." >&2
    echo "       The workflow passes the run's automatic token; when" >&2
    echo "       running by hand, use a token with 'contents: write'." >&2
    exit 1
}

# Same convention ci-forgejo-release.sh uses: a v0.* tag is a
# pre-release, because that is what "there has been no release yet" from
# docs/RELEASING.md's Versioning section means in practice.
prerelease=false
case "$TAG" in
v0.*) prerelease=true ;;
esac

if gh release view "$TAG" >/dev/null 2>&1; then
    echo "ci-github-release: release $TAG already exists; reusing it"
    gh release edit "$TAG" --title "$TAG" --notes-file "$NOTES"
else
    if [ "$prerelease" = true ]; then
        gh release create "$TAG" --title "$TAG" --notes-file "$NOTES" --prerelease
    else
        gh release create "$TAG" --title "$TAG" --notes-file "$NOTES"
    fi
    echo "ci-github-release: created release $TAG"
fi

# --clobber replaces an asset of the same name instead of erroring, the
# same idempotency ci-forgejo-release.sh's per-asset delete-then-upload
# gives.
gh release upload "$TAG" "$@" --clobber

echo "ci-github-release: release $TAG: $(gh release view "$TAG" --json url -q .url)"
