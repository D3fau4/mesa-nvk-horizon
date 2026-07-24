# CLAUDE.md — working rules for this repository

`mesa-nvk-horizon` builds a **native Horizon OS backend for Mesa/NVK** so that Vulkan
runs on the Nintendo Switch's Tegra X1 (GM20B, Maxwell 2nd gen) through `libnx` and the
`nv` system services — with no DRM emulation layer in between.

## Target architecture (non-negotiable)

```
Vulkan application
        │
        ▼
Mesa / NVK                         ← upstream, pinned, patched only via mesa-patches/
        │
        ▼
nvkmd_horizon                      ← NVK's kernel-mode-driver backend, Horizon flavour
        │
        ▼
horizon_gpu  (this repo, horizon/) ← Vulkan-free, WSI-free GPU abstraction
        │
        ├── NvMap            (memory objects)
        ├── NvAddressSpace   (GPU VA)
        ├── NvGpuChannel     (GPFIFO channels)
        ├── GPFIFO           (command submission)
        ├── NvFence          (completion)
        └── syncpoints       (monotonic counters)
        │
        ▼
libnx / nv services / Horizon OS
```

## Explicitly rejected designs

These are the failure modes of the reference ports (see `docs/reference-analysis.md`).
Do not reintroduce them:

1. **No simulated `/dev/dri` device.** No render node, no synthetic file descriptor sentinel.
2. **No fake `open` / `openat` / `stat` / `fstat` / `mmap` / `munmap` / `close` wrappers**
   (`-Wl,--wrap=...` interposition of libc is banned as an architecture; a genuinely missing
   newlib function may still be provided in `compat/`, documented individually).
3. **No re-implementation of the nouveau DRM uAPI.** No `drm_nouveau_gem_new`,
   no `drm_nouveau_exec`, no `drm_nouveau_vm_bind`, no `drmSyncobj*` family.
4. **No synthetic GEM handles.** Memory objects are `NvMap`-backed and referenced by
   explicit typed handles owned by a context.
5. **No global variables for device, window, swapchain or channel.** Every entry point
   takes an explicit context or object pointer. No `g_dev`, no `g_swapchain`.
6. **No CPU wait after every submit.** Submission is asynchronous. A CPU stall is allowed
   only where Vulkan semantics require it (`vkWaitForFences`, `vkQueueWaitIdle`,
   `vkDeviceWaitIdle`, swapchain acquire) or when the documented debug-synchronous mode
   is explicitly enabled.
7. **No whole Mesa files copied into this repo and edited.** Mesa changes live as tracked
   patches in `mesa-patches/`, or upstream. `mesa/` is a pinned checkout, never our source.

## Layer rules

| Layer | May depend on | Must NOT depend on |
|---|---|---|
| `horizon/` | libnx, newlib, C11 | Vulkan, Mesa, NVK, WSI, `nwindow` semantics beyond a surface-info struct |
| `nvkmd_horizon` (in `mesa-patches/`) | NVK internals, `horizon/` | libnx directly, `nwindow` |
| WSI Horizon (in `mesa-patches/`) | Vulkan WSI runtime, `horizon/` surface info, libnx `nwindow` | `nvkmd_horizon` internals |
| `compat/` | newlib, libnx | Mesa, NVK, `horizon/` |

`horizon/` must compile and its tests must run **without Mesa present**.

## Coding rules

- Explicit-width integer types (`uint32_t`, `uint64_t`, `size_t`). No bare `int` for
  sizes, offsets, handles or GPU addresses.
- Check every `Result` from libnx. Never discard one silently.
- Every constant gets a name and a comment citing its source (switchbrew, envytools,
  deko3d, nouveau headers).
- Check for overflow on every size/offset/alignment computation.
- Every allocation has exactly one documented owner. Partially-initialised objects are
  torn down in reverse order on the error path.
- No mass refactors. No mixing functional changes with renames or reformatting.
- Failures are reported, never hidden behind a sleep or a global wait.

## Process rules

- Follow the phase order in `docs/milestones.md`. Deviating requires a documented blocker
  in `STATUS.md`.
- `STATUS.md` is updated with every unit of work: current phase, what is done, what was
  actually tested, known failures, pending decisions, next concrete task.
- Distinguish rigorously between **host build**, **cross build**, and **verified on real
  hardware**. Never claim Switch behaviour from a successful compile.
- Record the exact commands run and their results.
- Small, thematic local commits. **Never `git push` without explicit authorisation.**
- Never delete a file without explaining why. Never overwrite the user's work.
- Any code adapted from another project keeps its attribution and licence header, and is
  listed in `LICENSES/` and in `docs/reference-analysis.md`.

## Licence hazard

The reference ports analysed in Phase 0 are **GPL-2.0 / AGPL-3.0** (they disagree with each
other — see `docs/reference-analysis.md`). Mesa/NVK is **MIT**. Copying reference code into
this tree would make the result copyleft and un-upstreamable. Default policy: **derive facts
and hardware knowledge, not source text.** Any literal reuse needs an explicit decision
recorded in `STATUS.md` under pending decisions.
