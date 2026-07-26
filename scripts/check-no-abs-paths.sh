#!/usr/bin/env bash
# Absolute-path gate (docs/milestones.md Phase 2 exit criterion):
#
#   "grep for /home/, /work, D:\, /mnt/ in toolchain/ and scripts/
#    returns nothing"
#
# The point is reproducibility: everything must resolve through
# $DEVKITPRO, $PWD or a variable read from toolchain/versions.env, so the
# tree builds on someone else's machine and inside a container without
# edits. A machine-specific path baked into a script or a cross file is
# the failure mode this gate exists to catch.
#
# Scope: toolchain/ and scripts/ are the criterion. Makefile and
# meson.build are checked too — they are build inputs subject to the same
# rule, and they are clean today, so including them costs nothing and
# stops the rule being silently bypassed by putting the path there
# instead.
#
# Not flagged, deliberately: /opt/devkitpro and other paths *inside* the
# pinned container image. Those are properties of the image (pinned by
# digest in toolchain/versions.env), not of anyone's machine. They must
# still come from a versions.env variable rather than be typed inline —
# that is a review rule, not something this grep can decide.
#
# Exit code 0 = clean, 1 = violation (printed).
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -u
cd "$(dirname "$0")/.."

fail=0

# This script necessarily contains every pattern it searches for.
EXCL=(--exclude=check-no-abs-paths.sh)

TARGETS=(toolchain scripts Makefile meson.build)

# Drop targets that do not exist yet: the gate is added before the Meson
# build and before toolchain/ has content, and a missing file must not be
# reported as a violation.
present=()
for t in "${TARGETS[@]}"; do
    [ -e "$t" ] && present+=("$t")
done

if [ "${#present[@]}" -eq 0 ]; then
    echo "check-no-abs-paths: nothing to check" >&2
    exit 0
fi

report() { # label, matches
    echo "ABSOLUTE PATH ($1):"
    echo "$2"
    fail=1
}

check() { # label, extended-regex
    local out
    out=$(grep -RInE "${EXCL[@]}" -- "$2" "${present[@]}" 2>/dev/null)
    [ -n "$out" ] && report "$1" "$out"
    return 0
}

# 1. Home directories: the classic "works on my machine" path. /Users/ is
#    the macOS spelling of the same mistake.
check "home directory"        '/home/|/Users/'

# 2. Hardcoded container working directory. Mount the tree at "$PWD"
#    inside the container instead: the path is then identical inside and
#    outside, which also makes compiler diagnostics line up with the
#    files on the host.
check "container workdir"     '/work($|[^a-zA-Z0-9_])'

# 3. Windows drive letters.
check "windows drive letter"  '[A-Za-z]:\\'

# 4. Mount points — /mnt/ and /media/ are per-machine by definition.
check "mount point"           '/mnt/|/media/'

if [ "$fail" -eq 0 ]; then
    echo "check-no-abs-paths: OK (${present[*]}: no machine-specific paths)"
fi
exit "$fail"
