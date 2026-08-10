# Documentation index

Most of this directory is **evidence** rather than prose: 164 console logs and the
working record of how the driver got to where it is. The design documents are the nine
files listed first.

If you are here for one thing, it is probably [`../STATUS.md`](../STATUS.md) — its
*Current state* block at the top is the authoritative answer to what works.

## Getting something running

| Document | What it answers |
|---|---|
| [`BUILDING.md`](BUILDING.md) | How to build: host tests, the two cross-build paths, the full Mesa/NVK chain, the pins, the gates, and what all 33 scripts do. |
| [`USAGE.md`](USAGE.md) | How to run the result on a console, what the logs mean, which environment variables exist, and how to report a run. |
| [`RELEASING.md`](RELEASING.md) | What a release contains, what it claims, and how to cut one that is actually hardware-verified. |
| [`../CONTRIBUTING.md`](../CONTRIBUTING.md) | The layer rules, the rejected designs, the evidence discipline, and what to run before opening a pull request. |

## Design

The order below is roughly the order in which the design was settled, and each document
is self-contained.

| Document | What it covers |
|---|---|
| [`architecture.md`](architecture.md) | The five layers, their responsibilities, and the dependencies each is forbidden. The document `scripts/check-layering.sh` enforces. |
| [`memory-model.md`](memory-model.md) | Every address, handle and layout concept kept separate: the binding chain, GPU VA, alignment and overflow, cache coherency, tiling, ownership, leak accounting. |
| [`synchronization.md`](synchronization.md) | Syncpoints as the hardware primitive; asynchronous submission; resource recycling; how it maps onto Vulkan fences and semaphores; timeouts; the debug-synchronous mode. |
| [`wsi.md`](wsi.md) | The `nwindow` swapchain: slot ownership, zero-copy and its fallback, acquire and present, recreation, error handling, threads. |
| [`milestones.md`](milestones.md) | The phase plan, in order, with exit criteria tagged **H** (host), **X** (cross) or **HW** (hardware). Phases 0–6 and what each had to prove. |
| [`known-risks.md`](known-risks.md) | R1–R18: every open risk with its owner phase, impact, and either a mitigation or an explicit *unresolved*. Entries leave only when the risk is retired. |
| [`reference-analysis.md`](reference-analysis.md) | The Phase 0 audit of five third-party ports — what they do, what they simulate, what they got wrong, and the hard split between re-derivable **facts** and licence-encumbered **expression**. The document the project's legal posture rests on. |
| [`rust-toolchain.md`](rust-toolchain.md) | How NAK and NIL's Rust half builds for a tier-3 Horizon target, and why no custom target JSON is needed. Answers risk R13. |
| [`devkita64-tls-report.md`](devkita64-tls-report.md) | A ready-to-file upstream bug report: devkitA64 gcc 15.2.0 miscompiles thread-local access under `-mtp=soft -fPIC`. Reproducer, both disassemblies, and this project's workaround. Decision D7 — still unfiled. |

## The record

| Path | What it is |
|---|---|
| [`hw-logs/`](hw-logs/README.md) | 164 console runs kept verbatim, plus one Atmosphère fatal report. Filenames carry their own verdict (`-PASS`, `-FAIL`, `-CRASH`, `-HUNG`). Its README is a narrative index of what each run settled — start there, not with the files. |
| `history/` | The dated working record moved out of `STATUS.md` when that file reached 7749 lines (decision D17), verbatim and in original order: phases 1–5, the review rounds, and the commit logs. |

Both directories are covered by a `MANIFEST.sha256` and checked by
[`../scripts/check-history-intact.sh`](../scripts/check-history-intact.sh). **A log is
evidence: nothing here is edited or deleted after the fact**, including the runs later
found to have measured less than they claimed. Changing a file means updating its digest
in the same commit, which puts the old and new hashes side by side in the diff where a
reviewer can see that a line of evidence was rewritten rather than added to.

## Elsewhere in the repository

- [`../mesa-patches/README.md`](../mesa-patches/README.md) — the patch-series contract:
  apply order, how a patch is identified, and the four fields every patch commit message
  must carry.
- [`../tests/README.md`](../tests/README.md) — the tests, what each verifies, and the
  order to run them in on a console.
- [`../LICENSES/README.md`](../LICENSES/README.md) — the licence policy, the third-party
  table, and why no source text from the reference ports is in this tree.
- [`../CLAUDE.md`](../CLAUDE.md) — the same engineering contract as `CONTRIBUTING.md`,
  written as instructions for an assistant.
