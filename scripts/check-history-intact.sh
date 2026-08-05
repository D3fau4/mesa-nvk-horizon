#!/usr/bin/env bash
# Verifies that the working record moved out of STATUS.md has not
# changed without anybody saying so (decision D17).
#
#   scripts/check-history-intact.sh
#
# WHY THIS EXISTS. STATUS.md was 7749 lines, and most of it was
# *evidence*: dated measurements, verbatim console output, rejected
# designs and the reasons they were rejected. D17 asked for the split
# and named the hazard in the same breath — moving evidence is how
# evidence gets lost or silently reworded — so the split was done by
# scripts/split-status.py, which refuses to write anything unless the
# chunks it cut reassemble into the original file byte for byte.
#
# That proves the move. This proves everything after it. Each file in
# docs/history/ has its digest in docs/history/MANIFEST.sha256, and this
# compares them.
#
# WHAT A FAILURE MEANS, AND WHAT IT DOES NOT. It does not mean history
# may never change: a later phase can legitimately append to a record,
# and a typo is a typo. It means the change was not declared. Updating
# the manifest in the same commit is the declaration, and it puts the
# old and new digests side by side in the diff, where a reviewer can see
# that a line of evidence was rewritten rather than added to.
#
# Exit code 0 = every file matches its digest and the manifest lists
# every file, 1 = it does not (printed).
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -u
cd "$(dirname "$0")/.."

exec python3 scripts/split-status.py --check
