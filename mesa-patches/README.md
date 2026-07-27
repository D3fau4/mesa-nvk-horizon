# `mesa-patches/`

Our entire delta against Mesa lives here, as a numbered `git format-patch`
series applied on top of `MESA_COMMIT` (`toolchain/versions.env`).

**`mesa/` is a pinned checkout, never our source.** No Mesa file is ever copied
into this repository and edited — that is rejected design 7 in `CLAUDE.md`, and
it is the specific failure mode of the reference ports, which carry whole edited
Mesa files *and* an overlapping patch, so the real delta cannot be read
(`docs/reference-analysis.md` § Inconsistencies). A patch series is reviewable,
rebasable onto a newer Mesa, and can be sent upstream.

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
mesa-nvk-horizon: Phase <N> item <M> (<item name from docs/milestones.md>)
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
  renaming, no drive-by cleanups (`docs/milestones.md` Phase 3 exit criteria).
- **No copied text from the Phase 0 reference ports.** They are GPL-2.0 /
  AGPL-3.0; Mesa is MIT. Derive facts and hardware knowledge, never source text.
  Any literal reuse needs an explicit decision recorded in `STATUS.md`
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
