# mesa-nvk-horizon

Native **Horizon OS** backend for **Mesa/NVK**, targeting the Nintendo Switch's
**Tegra X1 / GM20B** (Maxwell 2nd generation) through `libnx` and the Horizon `nv`
system services.

The goal is a Vulkan driver whose kernel-mode layer speaks Horizon natively —
`NvMap`, `NvAddressSpace`, `NvGpuChannel`, GPFIFO, `NvFence`, syncpoints — instead of
emulating the Linux nouveau DRM uAPI underneath NVK.

```
Vulkan application → Mesa/NVK → nvkmd_horizon → horizon_gpu → libnx → Horizon OS
```

## Status

**Phase 0 — audit of the existing reference ports. Complete.**
Nothing here builds or runs yet. See [`STATUS.md`](STATUS.md) for the authoritative state.

## Layout

| Path | Contents |
|---|---|
| `docs/` | Architecture, memory model, synchronisation, WSI, milestones, risks, reference audit |
| `horizon/` | The Horizon GPU abstraction — C, no Vulkan, no Mesa, no WSI |
| `mesa/` | Pinned Mesa checkout (submodule / reproducible fetch). **Not our source.** |
| `mesa-patches/` | Tracked patches against pinned Mesa: platform support, `nvkmd_horizon`, WSI |
| `compat/` | Narrowly-scoped newlib/libnx gap fillers, each individually justified |
| `toolchain/` | devkitA64 pinning, Meson cross files, Rust target for NAK |
| `scripts/` | Reproducible fetch / configure / build / package scripts |
| `tests/` | Host and on-device tests for `horizon/` |
| `examples/` | Minimal Vulkan programs used as bring-up milestones |

## Documentation

- [`docs/reference-analysis.md`](docs/reference-analysis.md) — audit of the four reference ports
- [`docs/architecture.md`](docs/architecture.md) — layers, responsibilities, forbidden dependencies
- [`docs/memory-model.md`](docs/memory-model.md) — addresses, `NvMap`, VA, tiling, coherency, ownership
- [`docs/synchronization.md`](docs/synchronization.md) — syncpoints, fences, async submission
- [`docs/wsi.md`](docs/wsi.md) — `nwindow` swapchain, buffering, zero-copy
- [`docs/milestones.md`](docs/milestones.md) — phase order and exit criteria
- [`docs/known-risks.md`](docs/known-risks.md) — open risks and pending decisions
- [`CLAUDE.md`](CLAUDE.md) — working rules

## Licence

Project code: **MIT**, to stay compatible with Mesa/NVK and upstreamable.
See [`LICENSES/`](LICENSES/). Mesa and libnx retain their own licences.

> The reference ports studied in Phase 0 are GPL-2.0/AGPL-3.0 licensed. No source text
> from them is copied into this repository. See `docs/reference-analysis.md` § Licensing.

## Legal

This project contains no Nintendo code, no NVIDIA proprietary blobs, and no
copyrighted firmware. It targets homebrew execution environments on hardware the user
owns.
