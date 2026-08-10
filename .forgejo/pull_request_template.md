<!--
Copyright (c) mesa-nvk-horizon contributors
SPDX-License-Identifier: MIT

Delete any section that does not apply. Do not delete the evidence section.
-->

## What this changes

<!-- One paragraph. If it fixes something, say what was wrong before, not just what is
     right now — that is the form the commit history here uses. -->

## Evidence

<!-- REQUIRED. This project never collapses these three classes; see CONTRIBUTING.md §1.
     Tick what you actually did, and leave the rest unticked. An unticked box is
     information, not a failure. -->

- [ ] **H — host.** `scripts/run-host-tests.sh` passes. Output:
- [ ] **X — cross.** It cross-compiles and produces the `.nro`. Command:
- [ ] **HW — hardware.** It ran on a real console. `RESULT:` line, build id, and the log
      added under `docs/hw-logs/` with its digest in `MANIFEST.sha256`:

<!-- If you have no console, say so here. That is normal and the change is still
     welcome — it just means the claim stops at X. -->

## Gates

- [ ] `scripts/check-layering.sh`
- [ ] `scripts/check-no-abs-paths.sh`
- [ ] `scripts/check-history-intact.sh`
- [ ] `scripts/check-mesa-test-parity.sh`
- [ ] Not applicable — this touches no code (documentation only)

## Record

- [ ] `STATUS.md` updated: what was done, what was tested, what is still open
- [ ] Nothing under `docs/hw-logs/` or `docs/history/` was edited — or, if it was, its
      digest is updated in the same commit
- [ ] No file was deleted without saying why

## If this touches Mesa

- [ ] The change is a patch in `mesa-patches/`, not an edited copy of a Mesa file
- [ ] Its commit message carries all four fields (`mesa-nvk-horizon:`, `Why:`,
      `Evidence:`, `Upstream:`) — see `mesa-patches/README.md`
- [ ] One functional change per patch; no drive-by reformatting
- [ ] No source text copied from the GPL/AGPL reference ports

## Anything unresolved

<!-- Open questions, things you could not test, decisions a reviewer has to make. -->
