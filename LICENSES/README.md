# Licensing

The licence *text* is `/LICENSE` at the repository root, where forges and packaging
tools look for it; `MIT.txt` here is a byte-identical copy kept so this directory reads
on its own. Both travel with the binaries: `scripts/package-horizon.sh` copies `LICENSE`
and this file into every package it builds. This file is the licence *policy* — what is
covered, what is external, and what may never be copied in.

## This repository

All original code in `horizon/`, `compat/`, `mesa-patches/`, `toolchain/`, `scripts/`,
`tests/` and `examples/` is **MIT** (`MIT.txt`), chosen to stay compatible with Mesa/NVK
and to keep an upstreaming path open. Every source file carries
`SPDX-License-Identifier: MIT`.

## Third-party components

| Component | Licence | Where |
|---|---|---|
| Mesa 3D / NVK | MIT | `mesa/` (pinned checkout, not vendored here) |
| libnx | ISC | external dependency, provided by devkitPro |
| devkitA64 toolchain | GPL / various | external, not redistributed here |

## Reference material — NOT incorporated

The Phase 0 audit studied four snapshots of the `switch-nvk` project. Those snapshots are
licensed **GPL-2.0** (its `master` branch) and **AGPL-3.0** (its `switch-port/*` branches);
the two disagree, and the README of every branch claims GPL-2.0-or-later. See
`docs/reference-analysis.md` § Licensing.

**No source text from those snapshots is present in this repository.** The audit records
hardware and API facts (register/class numbers, service call ordering, documented failure
modes), which are not themselves copyrightable expression.

If any literal reuse is ever proposed, it must be:
1. recorded as a pending decision in `STATUS.md`,
2. approved explicitly by the project owner,
3. accompanied by the upstream copyright header and an entry in this file,
4. placed in a clearly-delimited directory with its own licence file.

Until then the default answer is: **re-derive, do not copy.**
