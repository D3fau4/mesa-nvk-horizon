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

**A `VK_KHR_swapchain` presents on a Nintendo Switch, zero-copy, at the display refresh
rate.** Phases 0 through 6 are complete; `docs/milestones.md` ends there, so what comes
next is a decision rather than a task.

That sentence is a claim about real hardware, so here is what is behind it. This project
keeps **host build**, **cross build** and **verified on a console** strictly apart and
never collapses them; every row below is the third kind, with its log.

| What runs on a console | Evidence |
|---|---|
| `nv` bring-up, `NvMap`, GPU VA, channels, GPFIFO submission, syncpoints, teardown with leak accounting | the ten Phase 1 tests, [`t_init`](docs/hw-logs/t_init-run14-PASS.log) … [`t_teardown`](docs/hw-logs/t_teardown-run14-PASS.log), run 14 — all PASS, 258 checks between them |
| The first Vulkan result: `vkCmdFillBuffer` → `vkQueueSubmit` → `vkWaitForFences` → CPU readback validates | [`t_vulkan`](docs/hw-logs/t_vulkan-PASS-20260804.log) **PASS 56/56**, 0/1024 words wrong |
| Transfers, compute, off-screen images, a triangle, textures, depth, format coverage, concurrent submits | run 14: [transfer 203/203](docs/hw-logs/t_vk_transfer-run14-PASS.log), [compute 37/37](docs/hw-logs/t_vk_compute-run14-PASS.log), [image 72/72](docs/hw-logs/t_vk_image-run14-PASS.log), [triangle 84/84](docs/hw-logs/t_vk_triangle-run14-PASS.log), [texture 1685/1685](docs/hw-logs/t_vk_texture-run14-PASS.log), [depth 66/66](docs/hw-logs/t_vk_depth-run14-PASS.log), [format 282/282](docs/hw-logs/t_vk_format-run14-PASS.log), [submits 287/287](docs/hw-logs/t_vk_submits-run14-PASS.log) |
| The swapchain: `vkCreateViSurfaceNN`, 2–4 images, zero-copy with a documented copy fallback, recreation, `VK_ERROR_OUT_OF_DATE_KHR` | [`t_vk_swapchain` run 16](docs/hw-logs/t_vk_swapchain-run16-PASS.log) **PASS 125/125**, and [`t_nwindow` run 16](docs/hw-logs/t_nwindow-run16-PASS.log) **PASS 119/119** measuring the same through raw `bq*` with no Vulkan present |
| Frame pacing | 90 frames at a mean of **16666 µs**, with **89 of 89 intervals inside 10%** of a 60 Hz refresh |
| The swapchain driven from more than one thread | [`t_vk_wsi_mt` run 20](docs/hw-logs/t_vk_wsi_mt-run20-review-fixes-PASS.log) **PASS 52/52** — a render thread and a present thread on separate cores, 600 frames; 50 swapchain generations destroyed on one thread while the survivor presents on another |

167 console logs are kept in [`docs/hw-logs/`](docs/hw-logs/README.md), verbatim,
**including the failing ones** — among them the run where reverting a single patch took
the console down, which is the strongest evidence in the directory precisely because it
is a failure.

### What is not done

- **There is no installable driver.** No ICD, no loader, no shared library you can drop
  next to an application. The driver is linked statically into standalone test `.nro`.
  See [`docs/USAGE.md`](docs/USAGE.md) before expecting otherwise.
- **`VK_SUBOPTIMAL_KHR` has never been *returned* on hardware.** Run 21 put
  [`t_vk_suboptimal`](docs/hw-logs/t_vk_suboptimal-run21-PASS.log) at **PASS 273/273**
  and measured the rule around it over 2303 frames with zero false positives — but its
  section D did not execute, because nothing in the process can resize a VI layer. It
  needs somebody to dock the console while it runs.
- **Never verified on hardware:** docked resolution, and two surfaces over two
  `NWindow`s. Patch `0068` is cross-built only and unreachable by design — it needs a
  lost device, and nothing provokes one any more.
- **Known failures, all recorded and none hidden:** one unexplained MMU fault in run 14
  that never reproduced and is not being investigated further; whether the console
  survives `t_fault` on exit is unconfirmed; one unexplained occurrence in `t_vk_texture`.
- **Open decisions:** one — filing the devkitA64 TLS miscompile report upstream
  ([`docs/devkita64-tls-report.md`](docs/devkita64-tls-report.md)).

[`STATUS.md`](STATUS.md) is the authoritative state. Its *Current state* block at the top
is the part to read; everything below it is the dated working record.

## Build

```sh
scripts/run-host-tests.sh     # pure logic, no toolchain, no console
scripts/build-switch.sh -j4   # 18 .nro — devkitA64, or Docker as a fallback
```

The cross build needs devkitA64 (`$DEVKITPRO`) or Docker; with neither installed the
scripts fall back to `ghcr.io/d3fau4/nx-dev:latest`. Building the Vulkan half needs Mesa
and a Rust toolchain for the Switch target, which costs considerably more.

Full instructions, both build paths and why they differ, the pinned versions and every
script: [`docs/BUILDING.md`](docs/BUILDING.md).

## Run it on a console

Copy the `.nro` to `sdmc:/switch/horizon_gpu_tests/` and launch from hbmenu. Each prints
`RESULT: PASS (n/n)` on screen and to `sdmc:/horizon_gpu_tests/<name>.log`.

[`docs/USAGE.md`](docs/USAGE.md) covers what the logs mean, the runtime environment
variables, applet mode against full memory, and what to include when reporting a run —
**which is the most useful contribution anyone can make here**, since nobody working on
this project is guaranteed to have a console in front of them.

## Layout

| Path | Contents |
|---|---|
| `horizon/` | The Horizon GPU abstraction — C, no Vulkan, no Mesa, no WSI. 4092 lines, no stubs. |
| `mesa-patches/` | Our entire delta against Mesa: 75 tracked patches — platform support, `nvkmd_horizon`, WSI |
| `mesa/` | Pinned Mesa checkout, fetched by `scripts/fetch-mesa.sh` and gitignored. **Not a submodule, and not our source.** |
| `compat/` | Narrowly-scoped newlib/libnx gap fillers, each individually justified. Currently one: `sysconf`. |
| `toolchain/` | Meson cross file, the derived-image Dockerfile, Rust target JSON, and every pinned version |
| `scripts/` | 36 reproducible fetch / configure / build / check / package / CI scripts |
| `tests/` | 6 host suites and 34 on-device tests |
| `docs/` | Design, milestones, risks, the reference audit, the working record and 167 hardware logs |
| `examples/` | Reserved. Empty today. |
| `Makefile`, `meson.build` | The two cross-build paths; they do not build the same set — see `docs/BUILDING.md` |

## Documentation

Indexed in [`docs/README.md`](docs/README.md). The ones worth naming here:

- [`docs/BUILDING.md`](docs/BUILDING.md) — build it
- [`docs/USAGE.md`](docs/USAGE.md) — run it on a console, and report what it did
- [`docs/architecture.md`](docs/architecture.md) — layers, responsibilities, forbidden dependencies
- [`docs/memory-model.md`](docs/memory-model.md) — addresses, `NvMap`, VA, tiling, coherency, ownership
- [`docs/synchronization.md`](docs/synchronization.md) — syncpoints, fences, async submission
- [`docs/wsi.md`](docs/wsi.md) — `nwindow` swapchain, buffering, zero-copy
- [`docs/milestones.md`](docs/milestones.md) — the phases and their exit criteria
- [`docs/known-risks.md`](docs/known-risks.md) — R1–R18, open risks and their mitigations
- [`docs/reference-analysis.md`](docs/reference-analysis.md) — the Phase 0 audit of the reference ports

## Contributing

[`CONTRIBUTING.md`](CONTRIBUTING.md) — the layer rules, the seven designs this project
has already rejected, the evidence discipline, and what to run before opening a pull
request. [`CLAUDE.md`](CLAUDE.md) is the same contract written as instructions for an
assistant.

## Licence

Project code: **MIT** ([`LICENSE`](LICENSE)), to stay compatible with Mesa/NVK and
upstreamable. Policy and third-party components: [`LICENSES/`](LICENSES/README.md). Mesa
and libnx retain their own licences.

> The reference ports studied in Phase 0 are GPL-2.0/AGPL-3.0 licensed. No source text
> from them is copied into this repository. See `docs/reference-analysis.md` § Licensing.

## Legal

This project contains no Nintendo code, no NVIDIA proprietary blobs, and no
copyrighted firmware. It targets homebrew execution environments on hardware the user
owns.
