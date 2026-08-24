# `mesa-patches/`

Our entire delta against Mesa lives here, as a numbered `git format-patch`
series applied on top of `MESA_COMMIT` (`toolchain/versions.env`).

**`mesa/` is a pinned checkout, never our source.** No Mesa file is ever copied
into this repository and edited — that is rejected design 7 in `CLAUDE.md`, and
it is the specific failure mode of the reference ports, which carry whole edited
Mesa files *and* an overlapping patch, so the real delta cannot be read. A patch
series is reviewable, rebasable onto a newer Mesa, and can be sent upstream.

## Layout

```
mesa-patches/0001-<slug>.patch
mesa-patches/0002-<slug>.patch
```

File-name order is apply order. A patch is identified by **its commit subject
and its diff**: `scripts/apply-mesa-patches.sh` matches both against what
`mesa/`'s history records, so a patch whose body is corrected without its
subject changing is caught rather than reported as already applied. Renaming a
file is harmless; changing a subject or a diff after the patch has been applied
is not — the applier reports divergence and refuses to guess. The one thing it
cannot see is a change confined to the commit message *below* the subject with
an identical diff; regenerate the series after such an edit.

## Required header

Everything below the `Subject:` line, before the diff, is the commit message.
It must carry these four fields, in this order:

```
mesa-nvk-horizon: Phase <N> item <M> (<item name from the milestones list>)
Why: <the assumption Mesa makes that does not hold on newlib/libnx>
Evidence: <the exact measurement — configure line, compiler error, command>
Upstream: <yes|no> — <why>
```

`Evidence` is not optional and is not a rationale: it is what was actually
observed on this toolchain, quoted. A patch justified by reasoning alone is a
guess, and this project distinguishes host / cross / hardware evidence
rigorously (`CLAUDE.md` § Process rules).

`Upstream: yes` means the change is written as a general fix — a trait of the
libc or the compiler, not "if Horizon". Prefer that formulation; it is what
makes the patch defensible upstream and what keeps this series small.

## Rules a patch must obey

- **No mixed changes.** A patch is one functional change. No reformatting, no
  renaming, no drive-by cleanups (Phase 3 exit criterion).
- **No copied text from the Phase 0 reference ports.** They are GPL-2.0 /
  AGPL-3.0; Mesa is MIT. Derive facts and hardware knowledge, never source text.
  Any literal reuse needs an explicit recorded decision
  (`CLAUDE.md` § Licence hazard).
- **Bisectable.** Each patch must leave the tree consistent on its own, so a
  series is split where the intermediate state still builds — not merged to
  avoid thinking about the order.

## Applying

```sh
scripts/fetch-mesa.sh            # mesa/ at MESA_COMMIT
scripts/apply-mesa-patches.sh    # apply what is missing
scripts/apply-mesa-patches.sh --list
```

The applier is idempotent — a second run says `all N patches already applied` —
and non-destructive: a dirty tree, a history that is not a prefix of this
series, a `mesa/` that is not its own git repository, or a tree fetched through
the archive fallback (no `.git`) each stop it before it writes anything.

`scripts/fetch-mesa.sh` recognises `MESA_COMMIT + N commits` as a normal state
and leaves it alone; `--force` is what resets to the bare pinned commit.

## Regenerating

Edit in `mesa/` as ordinary commits on top of `MESA_COMMIT`, then:

```sh
git -C mesa format-patch -o ../mesa-patches "$MESA_COMMIT"
```

That rewrites the whole series, so re-read the diff before committing: the
numbering, the subjects and the headers above are the contract the applier
relies on.

## The 2026-08-23 compaction (84 → 49)

The series was compacted on 2026-08-23: every fix-of-a-patch-in-this-series was
folded into the patch that introduced the code, the review-finding batches were
folded into their targets, and the shader-window block-off/revert/redo cycle
collapsed to its final state. **The tree after applying the 49-patch series is
byte-identical to the tree after the old 84-patch series**
(`git rev-parse HEAD^{tree}` = `8119de7c70a7691fc4b98e8e377ab7d789c6ec8e` for
both), so the compaction changed no code — only how the same delta is told.
Each merged patch's message keeps the four-field header, quotes the hardware
evidence that supports its final state, and lists what it absorbed under
`Absorbs:`; the full pre-compaction narrative is in this directory's own git
history.

Dated hardware-run entries and other point-in-time records keep the old numbers,
because they describe the series as it was when those runs happened. The map,
old → new:

| old | new | old | new |
|---|---|---|---|
| 0001–0017 | unchanged | 0049 | 0031 |
| 0018 (+0025, 0033) | 0018 | 0050 | 0032 |
| 0019 (+0023, 0028, 0030, 0031, 0034, 0043) | 0019 | 0051 | 0033 |
| 0020 (+0022, 0024, 0026, 0027, 0032, 0036, 0044) | 0020 | 0052 | 0034 |
| 0021 | 0021 | 0053 (+0055¹, 0057, 0059, 0061–0067, 0069–0073, 0075) | 0037 |
| 0029 (+0040's comment correction) | 0022 | 0054 (+0055's nvk_wsi.c hunk) | 0038 |
| 0038 | 0023 | 0056 | 0035 |
| 0035 | 0024 | 0058 (+0060) | 0036 |
| 0041 | 0025 | 0068 | 0039 |
| 0042 | 0026 | 0074 | 0040 |
| 0045 | 0027 | 0076 | 0041 |
| 0046 | 0028 | 0077 | 0042 |
| 0037 + 0039² + 0047 | 0029 | 0078 / 0079 | 0043 / 0044 |
| 0048 | 0030 | 0080 / 0081 | 0045 / 0046 |
| | | 0082 / 0083 / 0084 | 0047 / 0048 / 0049 |

¹ 0055's `nvk_wsi.c` hunk went to new 0038 with the patch it corrects.
² 0039's fixed-VA half; its dev-side block-off and 0040's revert of it cancel,
and new 0029 carries the corrected pdev-side block-off.
