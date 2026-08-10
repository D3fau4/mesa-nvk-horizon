# Contributing

This project builds a native Horizon OS backend for Mesa/NVK, so that Vulkan runs on the
Nintendo Switch's Tegra X1 through `libnx` and the `nv` services with no DRM emulation
layer in between. Contributions are welcome. Most of what follows is unusual for a
hobby driver, and all of it exists because something went wrong once.

Start with [`STATUS.md`](STATUS.md) — specifically its *Current state* block at the top,
which is the only part of that file you must read. It says what phase the project is in,
what is verified, what is known broken, and what the next concrete task is.

---

## 1. The rule everything else rests on: say which kind of evidence you have

There are exactly three, and they are never collapsed into each other:

| Class | Means | What it does **not** prove |
|---|---|---|
| **H** — host | Built and run on your own machine (`scripts/run-host-tests.sh`) | Anything about the Switch |
| **X** — cross | Cross-compiled for `aarch64` Horizon; a `.nro` exists | That it runs, or that it is correct |
| **HW** — hardware | Ran on a real console, with the log committed to `docs/hw-logs/` | Only what the log actually shows |

A successful compile is never described as working. "The driver now handles X" is a
claim about **HW** and needs a log. If you do not have a console, say so and mark the
change **X** — that is a normal and useful contribution, and
[`docs/known-risks.md`](docs/known-risks.md) R2 exists because the project has spent
most of its life in exactly that position.

The corollary: a measurement with no log behind it does not go into `STATUS.md`.

## 2. Layers, and what may depend on what

`horizon/` is a Vulkan-free, WSI-free, Mesa-free GPU abstraction. That is not a
stylistic preference — it is the property that makes the design testable without Mesa
present, and [`scripts/check-layering.sh`](scripts/check-layering.sh) fails the build if
it is broken.

| Layer | May depend on | Must **not** depend on |
|---|---|---|
| `horizon/` | libnx, newlib, C11 | Vulkan, Mesa, NVK, WSI, `nwindow` beyond a surface-info struct |
| `nvkmd_horizon` (in `mesa-patches/`) | NVK internals, `horizon/` | libnx directly, `nwindow` |
| WSI Horizon (in `mesa-patches/`) | Vulkan WSI runtime, `horizon/` surface info, libnx `nwindow` | `nvkmd_horizon` internals |
| `compat/` | newlib, libnx | Mesa, NVK, `horizon/` |

Full rationale in [`docs/architecture.md`](docs/architecture.md).

## 3. Designs this project has already rejected

These are the failure modes of the reference ports audited in Phase 0
([`docs/reference-analysis.md`](docs/reference-analysis.md)). A patch reintroducing one
will be declined regardless of whether it works:

1. **No simulated `/dev/dri` device.** No render node, no synthetic file-descriptor
   sentinel.
2. **No fake `open` / `openat` / `stat` / `mmap` / `close` wrappers.**
   `-Wl,--wrap=` interposition of libc is banned as an architecture. A genuinely missing
   newlib function may still be provided in `compat/`, documented individually — that is
   what `compat/sysconf.c` is.
3. **No re-implementation of the nouveau DRM uAPI.** No `drm_nouveau_gem_new`, no
   `drm_nouveau_exec`, no `drm_nouveau_vm_bind`, no `drmSyncobj*`.
4. **No synthetic GEM handles.** Memory objects are `NvMap`-backed, referenced by
   explicit typed handles owned by a context.
5. **No global state** for device, window, swapchain or channel. Every entry point takes
   an explicit context or object pointer.
6. **No CPU wait after every submit.** Submission is asynchronous. A stall is allowed
   only where Vulkan requires it (`vkWaitForFences`, `vkQueueWaitIdle`,
   `vkDeviceWaitIdle`, swapchain acquire) or in the documented debug-synchronous mode.
7. **No Mesa files copied into this repository and edited.** Mesa changes are tracked
   patches in `mesa-patches/`, or they go upstream.

## 4. Code

- Explicit-width integer types (`uint32_t`, `uint64_t`, `size_t`). No bare `int` for
  sizes, offsets, handles or GPU addresses.
- Check every `Result` from libnx. Never discard one silently.
- Every constant gets a name and a comment citing its source — switchbrew, envytools,
  deko3d, nouveau headers. A magic number with no provenance is a guess.
- Check for overflow on every size/offset/alignment computation.
  `horizon/memory/align.h` is the arithmetic to use, and it is host-tested.
- Every allocation has exactly one documented owner. Partially-initialised objects are
  torn down in reverse order on the error path.
- No mass refactors, and no mixing a functional change with renames or reformatting.
- Failures are reported, never hidden behind a sleep or a global wait.
- Every file carries `SPDX-License-Identifier: MIT` and the project copyright line.

## 5. Changing Mesa

Our entire delta against Mesa is a numbered `git format-patch` series in
[`mesa-patches/`](mesa-patches/README.md), applied on top of the `MESA_COMMIT` pinned in
`toolchain/versions.env`. `mesa/` is a pinned checkout and is never our source.

Every patch commit message must carry these four fields, in this order:

```
mesa-nvk-horizon: Phase <N> item <M> (<item name from docs/milestones.md>)
Why: <the assumption Mesa makes that does not hold on newlib/libnx>
Evidence: <the exact measurement — configure line, compiler error, command>
Upstream: <yes|no> — <why>
```

`Evidence:` is not a rationale and is not optional: it is what was actually observed,
quoted. Prefer writing the fix as a general one — a trait of the libc or the compiler,
not `#ifdef HORIZON` — which is what makes it defensible upstream and keeps the series
small. Read `mesa-patches/README.md` before adding one.

## 6. Before you open a pull request

```sh
scripts/run-host-tests.sh          # six suites of horizon/'s pure logic
scripts/check-layering.sh          # horizon/ stayed Vulkan-, Mesa- and nwindow-free
scripts/check-no-abs-paths.sh      # no machine-specific paths
scripts/check-history-intact.sh    # the record and the logs match their manifests
scripts/check-mesa-test-parity.sh  # the Makefile and the Meson build still agree
```

**GitHub Actions runs all five automatically** on every push and pull request
([`.github/workflows/ci.yml`](.github/workflows/ci.yml)), then goes on to build Mesa, the
Rust half and NVK the same way `scripts/ci-build-archives.sh` does by hand — see below.
Run the five yourself before you push anyway: CI feedback on a build that takes tens of
minutes is a bad first signal for a typo, and a red run you already expected is not worth
the wait. The Forgejo instance's own workflow is still off, kept commented in
[`.forgejo/workflows-disabled/`](.forgejo/workflows-disabled/), with a note on each saying
what it did and what its runs taught.

One command does the rest, locally, before CI does it again:
[`scripts/ci-build-archives.sh`](scripts/ci-build-archives.sh) builds Mesa, the Rust half
and NVK and then asserts that **every** `.a` the tests link exists and that every `.nro`
`meson.build` names links them. If you touch `mesa-patches/`, that is what will tell you
first — CI runs the identical command, but only after you push.

Two further gates — `check-dispatch-complete.sh` and `check-tls-relocs.sh` — need built
artefacts. `scripts/ci-build-archives.sh` ends by running them against what it just
built; on their own they need a build first.

Build instructions are in [`docs/BUILDING.md`](docs/BUILDING.md); running the result on
a console is [`docs/USAGE.md`](docs/USAGE.md).

## 7. The record

- **`STATUS.md`** is updated with every unit of work: current phase, what is done, what
  was actually tested, known failures, pending decisions, next concrete task. A change
  that does not touch it is usually a change that has not said what it proved.
- **`docs/hw-logs/`** holds console runs verbatim. They are evidence, so nothing there
  is edited or deleted after the fact — including logs later found to have measured less
  than they claimed. Filenames carry their own verdict (`-PASS`, `-FAIL`, `-CRASH`).
- Both `docs/hw-logs/` and `docs/history/` are covered by `MANIFEST.sha256`. Changing a
  file there means updating its digest **in the same commit**: that is the declaration,
  and it puts the old and new digests side by side in the diff where a reviewer can see
  that a line of evidence was rewritten rather than added to.
- Commits are small and thematic. The subject line says what changed and, where it fits,
  what was wrong before.

## 8. Licence, and one hazard worth stating plainly

This project is **MIT** ([`LICENSE`](LICENSE)), deliberately, so that the Mesa patches
stay upstreamable.

The reference ports studied in Phase 0 are **GPL-2.0 / AGPL-3.0** — and they contradict
each other; see [`docs/reference-analysis.md`](docs/reference-analysis.md) § Licensing.
Copying source text from them into this tree would make the result copyleft and close
the upstreaming path for good.

**Derive facts and hardware knowledge, not source text.** Register and class numbers,
service-call ordering and documented failure modes are facts. Implementation expression
is not. Any literal reuse needs an explicit decision recorded in `STATUS.md` and the
four steps in [`LICENSES/README.md`](LICENSES/README.md). Until then the answer is no.

By contributing you agree your work is licensed MIT under the same terms.

## 9. Reporting rather than coding

The most valuable contribution to this project is usually **a console run**. Nobody
developing it is guaranteed to have hardware in front of them, which is risk R2 and has
shaped everything above. If you have a Switch and ten minutes, run the `.nro` and file
the log: use the *Hardware run report* issue template, and include the
`note horizon-build-id …` line the test prints — without it the result cannot be
attributed to a build, and an unattributable result has cost this project an afternoon
before.
