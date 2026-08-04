#!/usr/bin/env python3
"""Splits STATUS.md into the state and the record it had grown around
(decision D17), and proves the split moved everything unchanged.

    scripts/split-status.py [--check]

WHY THIS IS A SCRIPT AND NOT AN EDIT. STATUS.md is 7749 lines, and most
of it is *evidence*: dated measurements, verbatim console output,
rejected designs and the reasons they were rejected. Moving evidence by
hand is how evidence gets silently reworded, so nothing here is
retyped. The file is cut at section boundaries into contiguous chunks,
each chunk is assigned a destination, and the chunks are concatenated
back together and compared with the original **byte for byte** before a
single file is written. A split that does not reassemble is refused.

WHAT STAYS AND WHAT MOVES. CLAUDE.md says STATUS.md is the current
phase, what is done, what was actually tested, known failures, pending
decisions and the next concrete task. Those stay. The dated working
record — every phase's narrative, every review round, the commit logs —
moves to docs/history/ in its original order, so a reader following a
link lands on the same text in the same sequence.

--check recomputes each history file's digest against the manifest and
reports drift. History is evidence: it may be appended to deliberately,
but an edit that nobody meant to make should be visible, and the
manifest is what makes it visible.

Copyright (c) mesa-nvk-horizon contributors
SPDX-License-Identifier: MIT
"""
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATUS = os.path.join(ROOT, "STATUS.md")
HISTORY = os.path.join(ROOT, "docs", "history")
MANIFEST = os.path.join(HISTORY, "MANIFEST.sha256")

STAY = "STATUS.md"

# The cut, in the original's order. Each entry is the title of the
# section a chunk STARTS at, and the destination it goes to; a chunk
# runs until the next entry begins. Titles are matched exactly against
# the "## " lines, so a renamed heading fails loudly instead of
# silently taking the wrong slice.
CUTS = [
    ("## Current state — read this first", STAY),
    ("## Phase 5 — step 0a: the shader window, asked instead of assumed (2026-08-04)",
     "phase-5.md"),
    ("## Phase 4 — step 1: what `nvkmd` requires, against what `horizon_gpu` has (2026-07-28)",
     "phase-4.md"),
    ("## Phase 3 — the state it closed in (previously \"Current phase\")",
     "phase-3.md"),
    ("## Phase 1 (previous phase)", "phase-1.md"),
    ("## Known failures / limitations", STAY),
    ("## Pending decisions", STAY),
    ("## Codex PR review (2026-07-26) — findings addressed",
     "reviews-2026-07-26.md"),
    ("## Phase 2 — toolchain (2026-07-26)", "phase-2.md"),
    ("## The degraded-baseline opt-in (2026-07-28)", "phase-4-continued.md"),
    ("## Commit log for Phase 4", "commit-logs.md"),
]

INDEX = """## The working record

Everything above is the state. The dated record it was built from —
every phase's narrative, every review round, every hardware run and the
commit logs — lives in `docs/history/`, moved there verbatim by
`scripts/split-status.py` (decision D17) and in its original order:

| file | what is in it |
|---|---|
| `docs/history/phase-5.md` | Phase 5, off-screen rendering: the nine items, five hardware batches, and the texture failure that has not reproduced |
| `docs/history/phase-4.md` | Phase 4, `nvkmd_horizon`: the interface reading, the libc symbols, Rust, the build machine, and the first console runs |
| `docs/history/phase-4-continued.md` | Phase 4's second half: the degraded baseline, the never-executed-path audit, the push dump, the exit criterion, and the PR #6 review rounds |
| `docs/history/phase-3.md` | Phase 3, Mesa on Horizon: newlib gaps, physical memory, threads and timers, the TLS miscompile, and the PR #4 reviews |
| `docs/history/phase-2.md` | Phase 2, the toolchain |
| `docs/history/phase-1.md` | Phase 1, `horizon/` standalone, and its exit criteria |
| `docs/history/reviews-2026-07-26.md` | the two early review rounds |
| `docs/history/phase-4-narrative.md` | the step-by-step Phase 4 narrative that had been living inside the Current state block |
| `docs/history/commit-logs.md` | the per-phase commit logs |

`scripts/check-history-intact.sh` recomputes their digests against
`docs/history/MANIFEST.sha256`. History is evidence: appending to it is
a deliberate act that updates the manifest in the same commit, and any
other change shows up as a failing gate.

"""


def read_lines(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read().split("\n")


def sha(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def do_check():
    if not os.path.exists(MANIFEST):
        print("check-history: %s does not exist; run the split first"
              % os.path.relpath(MANIFEST, ROOT), file=sys.stderr)
        return 1
    bad = 0
    seen = set()
    with open(MANIFEST, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            digest, name = line.split(None, 1)
            seen.add(name)
            path = os.path.join(HISTORY, name)
            if not os.path.exists(path):
                print("HISTORY (%s): listed in the manifest and missing "
                      "from disk." % name)
                bad = 1
                continue
            with open(path, "r", encoding="utf-8") as h:
                got = sha(h.read())
            if got != digest:
                print("HISTORY (%s): content differs from the manifest." % name)
                print("  manifest: %s" % digest)
                print("  on disk:  %s" % got)
                print("  History is evidence. If the change was meant, say so")
                print("  in the commit and update MANIFEST.sha256 with it.")
                bad = 1
    for name in sorted(os.listdir(HISTORY)):
        if name.endswith(".md") and name not in seen:
            print("HISTORY (%s): on disk and not in the manifest." % name)
            bad = 1
    if bad == 0:
        print("check-history-intact: OK (%d file(s))" % len(seen))
    return bad


def do_split():
    lines = read_lines(STATUS)
    original = "\n".join(lines)

    # Locate each cut's starting line, exactly.
    starts = []
    for title, dest in CUTS:
        hits = [i for i, l in enumerate(lines) if l == title]
        if len(hits) != 1:
            print("split-status: %d line(s) match %r; expected exactly one."
                  % (len(hits), title), file=sys.stderr)
            print("split-status: refusing to cut a file it does not "
                  "recognise.", file=sys.stderr)
            return 1
        starts.append((hits[0], dest))
    if starts != sorted(starts):
        print("split-status: the cuts are not in the file's order; refusing.",
              file=sys.stderr)
        return 1

    # Chunk 0 is everything before the first cut: the file's own header.
    chunks = [("\n".join(lines[:starts[0][0]]) + "\n", STAY)]
    for k, (line_no, dest) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else len(lines)
        body = "\n".join(lines[line_no:end])
        if k + 1 < len(starts):
            body += "\n"
        chunks.append((body, dest))

    # THE GUARANTEE. Reassembled in order, the chunks must be the file
    # that was read, to the byte. Nothing is written until they are.
    rebuilt = "".join(c for c, _ in chunks)
    if rebuilt != original:
        print("split-status: the chunks do not reassemble into STATUS.md.",
              file=sys.stderr)
        print("split-status: nothing was written.", file=sys.stderr)
        return 1
    print("split-status: %d chunks reassemble into STATUS.md exactly "
          "(sha256 %s)" % (len(chunks), sha(original)[:16]))

    os.makedirs(HISTORY, exist_ok=True)

    # History files, in order, each the concatenation of its chunks.
    by_dest = {}
    order = []
    for body, dest in chunks:
        if dest == STAY:
            continue
        if dest not in by_dest:
            by_dest[dest] = []
            order.append(dest)
        by_dest[dest].append(body)

    manifest = ["# Digests of the working record, as split out of "
                "STATUS.md by scripts/split-status.py.",
                "# Checked by scripts/check-history-intact.sh.",
                "# Pre-split STATUS.md: sha256 %s" % sha(original)]
    for dest in order:
        text = "".join(by_dest[dest])
        with open(os.path.join(HISTORY, dest), "w", encoding="utf-8") as f:
            f.write(text)
        manifest.append("%s  %s" % (sha(text), dest))
        print("split-status: %-28s %6d lines" % (dest, text.count("\n")))

    with open(MANIFEST, "w", encoding="utf-8") as f:
        f.write("\n".join(manifest) + "\n")

    # STATUS.md: the chunks that stay, with the index inserted after the
    # header and the Current state block.
    kept = [c for c, d in chunks if d == STAY]
    new_status = kept[0] + kept[1] + INDEX + "".join(kept[2:])
    with open(STATUS, "w", encoding="utf-8") as f:
        f.write(new_status)
    print("split-status: STATUS.md %6d lines (was %d)"
          % (new_status.count("\n"), len(lines) - 1))
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--check":
        sys.exit(do_check())
    if len(sys.argv) > 1:
        print("usage: scripts/split-status.py [--check]", file=sys.stderr)
        sys.exit(1)
    sys.exit(do_split())
