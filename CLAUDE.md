# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A native **Horizon OS** backend for **Mesa/NVK**, so Vulkan runs on the Nintendo Switch's
Tegra X1 (GM20B, Maxwell 2nd gen) through `libnx` and the `nv` system services — with no
DRM emulation layer in between.

```
Vulkan app → Mesa/NVK → nvkmd_horizon → horizon_gpu → libnx → Horizon OS
```

Only `horizon/`, `compat/`, `tests/`, `scripts/` and the two build files are our source.
`mesa/` is a pinned, gitignored checkout; our entire Mesa delta is the numbered patch
series in `mesa-patches/`.

### Stale references

`docs/`, `CONTRIBUTING.md`, `MANIFEST.sha256` and `scripts/check-history-intact.sh` do
not exist, but are still cited widely: comments, script headers, `README.md` and the
patch series name `docs/architecture.md`, `docs/milestones.md`,
`docs/reference-analysis.md`, `CONTRIBUTING.md` and "Phase N item M" throughout.
**Do not follow those links or assume the files exist**; do not re-create them unless
asked. When adding a Mesa patch, keep the four-field header format below even though
the milestones list its first field cites is gone.

The record of what was done, what was actually tested and what is still open lives in
the commit messages.

## Commands

### Host tests — the only thing that needs no toolchain

```sh
scripts/run-host-tests.sh          # 8 suites of horizon/'s pure logic, ASan+UBSan
```

There is no per-test flag. To run one suite, compile it the way the script does:

```sh
cc -std=c11 -O1 -g -Wall -Wextra -Werror -Ihorizon/include \
   -fsanitize=address,undefined -fno-sanitize-recover=all \
   -o build/host-tests/h_blob_cache \
   tests/host/h_blob_cache.c horizon/cache/blob_cache.c horizon/cache/crc32.c \
&& ./build/host-tests/h_blob_cache
```

Suite → sources: `h_align` (header only), `h_va_space` + `horizon/vm/va_space.c`,
`h_syncpt_math` (header only), `h_cmds` + `horizon/submit/cmds.c`,
`h_scanout` + `horizon/surface/surface.c`,
`h_status` + `horizon/debug/status.c`, `h_log` + `horizon/debug/log.c`,
`h_blob_cache` + `horizon/cache/{blob_cache,crc32}.c`.

### Gates — run these before pushing; CI runs the same four

```sh
scripts/check-mesa-test-parity.sh  # the Makefile and meson.build still agree
scripts/check-layering.sh          # horizon/ stayed Vulkan-, Mesa- and nwindow-free
scripts/check-no-abs-paths.sh      # no /home/, /work, D:\, /mnt/ in tracked build inputs
scripts/run-host-tests.sh
```

`scripts/check-dispatch-complete.sh` and `scripts/check-tls-relocs.sh` need built
artefacts; `scripts/ci-build-archives.sh` runs them at the end against what it just built.

### Cross build (`libhorizon_gpu.a` + the test `.nro`s)

```sh
scripts/build-switch.sh -j4        # devkitA64 when $DEVKITPRO is set, else Docker
make install PREFIX=...            # into devkitPro portlibs; PREFIX as an *argument*
```

`build-switch.sh` execs `make` directly when `$DEVKITPRO` is set, otherwise runs it inside
`ghcr.io/d3fau4/nx-dev:latest` (override with `HORIZON_NX_IMAGE`), mounting the tree at
the *same* path inside the container as outside.

`make install`/`uninstall` are one script called twice — never add files to one side only.
Pass `PREFIX`/`DESTDIR` as make arguments, not environment: devkitPro's Cygwin make on
Windows drops the environment form silently.

### Full build, including Mesa/NVK (tens of minutes)

```sh
scripts/ci-build-archives.sh       # what CI runs; the whole pipeline in order
```

That is: `fetch-mesa.sh` → `apply-mesa-patches.sh` → `fetch-mesa-subprojects.sh` →
`fetch-rust-tools.sh` → `fetch-rust-crates.sh` → `fetch-clc-deps.sh` →
`build-toolchain-image.sh` (or `build-rust-sysroot.sh` with a local devkitA64) →
`build-mesa-clc.sh` → `configure-mesa.sh` → `build-mesa.sh` → `build-mesa-nvk.sh` →
`build-horizon.sh`, then the two artefact-dependent gates. It ends by asserting that
every `.a` the tests link exists and that every `.nro` `meson.build` names links them —
if you touch `mesa-patches/`, this is what tells you first.

The Meson path (`meson.build`) is configured through `scripts/configure-horizon.sh`; it is
**cross-only and errors out on a host build** by design. Individual steps go through
`horizon_run` / `horizon_meson` / `horizon_ninja` in `scripts/toolchain-env.sh`, which
spawns a per-command `docker run` when `$DEVKITPRO` is unset and runs directly when it is
set. The toolchain image has no pip and no outbound network — fetch/setup steps must run
on the host, never wrapped in `docker run`.

### On a console

Copy the `.nro` to `sdmc:/switch/horizon_gpu_tests/`. Each prints `RESULT: PASS (n/n)` and
logs to `sdmc:/horizon_gpu_tests/<name>.log`, including the `horizon-build-id` line — a
result without it cannot be attributed to a build.

## Architecture

### `horizon/` — the Vulkan-free GPU abstraction

C11 + libnx + newlib only. Modules: `device/` (nv service session, GPU characteristics),
`memory/` (`NvMap` objects, `align.h` overflow-checked arithmetic), `vm/` (`NvAddressSpace`,
VA reservation and mapping), `channel/` (`NvGpuChannel`, GPFIFO), `submit/` (command
building, submission), `sync/` (syncpoints, `NvFence`), `cache/` (the shader cache's
storage), `debug/` (logging, status strings), `surface/` (block-linear scanout layout —
the "surface-info struct" the layer rules reserve, and pure arithmetic, so the host
suites reach it).

Two properties drive its shape:

- **It compiles and its tests run without Mesa or libnx present.** That is why
  `horizon/cache/` lives here rather than beside the driver: the host tests hand the same
  translation unit truncated and bit-flipped files under sanitizers.
- **`horizon/include/` is libnx-free.** No `<switch...>` include, no `Nv*` type, no
  `Result` — `horizon_gpu_result` mirrors libnx's `Result` and the `.c` files
  `static_assert` the equivalence. `check-layering.sh` enforces both.

`horizon_gpu_status` values are **appended, never inserted or renumbered**: the numbers
appear in hardware logs kept as evidence.

The blob cache performs **no path operation after `open()`** — libnx routes
`open`/`stat`/`unlink`/`mkdir`/`rename` through one unlocked global buffer, so a second
thread doing path work corrupts the first one's path.

### Layer rules (enforced by `scripts/check-layering.sh`)

| Layer | May depend on | Must NOT depend on |
|---|---|---|
| `horizon/` | libnx, newlib, C11 | Vulkan, Mesa, NVK, WSI, `nwindow` beyond a surface-info struct |
| `nvkmd_horizon` (in `mesa-patches/`) | NVK internals, `horizon/` | libnx directly, `nwindow` |
| WSI Horizon (in `mesa-patches/`) | Vulkan WSI runtime, `horizon/` surface info, libnx `nwindow` | `nvkmd_horizon` internals |
| `disk_cache_horizon` (in `mesa-patches/`) | Mesa's `util/`, `horizon/`'s blob cache | Vulkan, NVK, WSI, libnx directly |
| `compat/` | newlib, libnx | Mesa, NVK, `horizon/` |

### Two build systems, deliberately duplicated

The **Makefile** is the reference path — it produced the `.nro`s verified on hardware, and
must stay readable without running a script. **`meson.build`** exercises the cross file and
is what the Mesa work plugs into. They restate the same facts about the Mesa-linking tests
(`t_threads`, `t_ostime`, `t_shader_cache`: which archives, which defines — `-DHAVE_PTHREAD
-DHAVE_STRUCT_TIMESPEC -DENABLE_SHADER_CACHE`, copied from what Mesa's own configure decided
here). **Edit one, edit the other**; `check-mesa-test-parity.sh` fails otherwise.

Those three tests are skipped when Mesa's archives are absent from `$MESA_BUILD_DIR`
(default `build/mesa-probe`); the Makefile then prunes their stale artefacts so a manifest
never attributes the previous build's binaries to this one.

### `mesa-patches/`

49 numbered `git format-patch` files applied on `MESA_COMMIT` from `toolchain/versions.env`.
A patch is identified by **its commit subject and its diff** — the applier matches both, so
changing either after it has been applied is reported as divergence. Every patch message
carries, in this order:

```
mesa-nvk-horizon: Phase <N> item <M> (<item name>)
Why: <the assumption Mesa makes that does not hold on newlib/libnx>
Evidence: <the exact measurement — configure line, compiler error, command>
Upstream: <yes|no> — <why>
```

`Evidence` is what was actually observed, quoted — not a rationale. Prefer writing the fix
as a general trait of the libc or the compiler over `#ifdef HORIZON`. One functional change
per patch, bisectable, no drive-by cleanups. Regenerate with
`git -C mesa format-patch -o ../mesa-patches "$MESA_COMMIT"`, then re-read the whole diff.
`.patch` files are byte-exact by contract (`.editorconfig`, `.gitattributes`).

## Rejected designs — do not reintroduce

These are the failure modes of the GPL reference ports studied early on; several are
grepped for by `check-layering.sh`.

1. No simulated `/dev/dri`, no render node, no synthetic fd sentinel.
2. No fake `open`/`openat`/`stat`/`mmap`/`close` wrappers. `-Wl,--wrap=` interposition of
   libc is banned **as an architecture** — a genuinely missing newlib function may still go
   in `compat/`, documented individually.
3. No re-implementation of the nouveau DRM uAPI (`drm_nouveau_gem_new`, `drm_nouveau_exec`,
   `drm_nouveau_vm_bind`, `drmSyncobj*`).
4. No synthetic GEM handles — memory objects are `NvMap`-backed, referenced by explicit
   typed handles owned by a context.
5. No global state for device, window, swapchain or channel. Every entry point takes an
   explicit context pointer. No `g_dev`, no `g_swapchain`.
6. No CPU wait after every submit. Submission is asynchronous; a stall is allowed only
   where Vulkan requires it (`vkWaitForFences`, `vkQueueWaitIdle`, `vkDeviceWaitIdle`,
   swapchain acquire) or in the documented debug-synchronous mode.
7. No Mesa file copied into this tree and edited.

## Code

- Explicit-width types (`uint32_t`, `uint64_t`, `size_t`). No bare `int` for sizes, offsets,
  handles or GPU addresses.
- Check every `Result` from libnx; never discard one silently.
- Every constant gets a name and a comment citing its source — switchbrew, envytools,
  deko3d, nouveau headers. A magic number with no provenance is a guess.
- Check overflow on every size/offset/alignment computation; `horizon/memory/align.h` is
  the arithmetic to use and it is host-tested.
- One documented owner per allocation; partially-initialised objects are torn down in
  reverse order on the error path.
- No mass refactors; never mix a functional change with renames or reformatting.
- Failures are reported, never hidden behind a sleep or a global wait.
- Every file carries `SPDX-License-Identifier: MIT` and the project copyright line.
- 4-space indent, 79-column C, LF (`.editorconfig`). `-Wall -Wextra -Werror` everywhere.

## Evidence discipline

Three classes, never collapsed into each other:

| Class | Means | Does **not** prove |
|---|---|---|
| **H** — host | Built and run via `scripts/run-host-tests.sh` | Anything about the Switch |
| **X** — cross | Cross-compiled for aarch64 Horizon; a `.nro` exists | That it runs, or is correct |
| **HW** — hardware | Ran on a real console, with the log | Only what the log actually shows |

A successful compile is never described as working. State which class you have, and record
the exact commands run and their results.

## Working habits

- Small, thematic commits. Subject says what changed and, where it fits, what was wrong
  before. **Never `git push` without explicit authorisation.**
- Never delete a file without explaining why.
