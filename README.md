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

## ⚠️ No support, AI-generated

This project is **unsupported**. It is written entirely by an AI coding
assistant as a personal experiment, for my own testing. There is no guarantee of
correctness, no maintenance commitment, and no support channel. Use it, fork it, or
read it at your own risk — but do not expect issues to be triaged or pull requests to
be reviewed on any schedule.

## Build

```sh
scripts/run-host-tests.sh     # pure logic, no toolchain, no console
scripts/build-switch.sh -j4   # devkitA64, or Docker as a fallback
make install                  # into devkitPro's portlibs, for other projects
```

The cross build needs devkitA64 (`$DEVKITPRO`) or Docker; with neither installed the
scripts fall back to `ghcr.io/d3fau4/nx-dev:latest`. Building the Vulkan half needs Mesa
and a Rust toolchain for the Switch target, which costs considerably more.

`make install` is for linking the driver from *your* homebrew rather than running ours:
it puts the archives, headers and an `nvk.pc` into `$DEVKITPRO/portlibs/switch`, so
`pkg-config --cflags --libs nvk` gives you a working link line and nothing has to know
where this repository is. It is still a static link — there is no ICD and no loader.

## Run it on a console

Copy the `.nro` to `sdmc:/switch/horizon_gpu_tests/` and launch from hbmenu. Each prints
`RESULT: PASS (n/n)` on screen and to `sdmc:/horizon_gpu_tests/<name>.log`.

## Layout

| Path | Contents |
|---|---|
| `horizon/` | The Horizon GPU abstraction — C, no Vulkan, no Mesa, no WSI. |
| `mesa-patches/` | Our entire delta against Mesa — platform support, `nvkmd_horizon`, WSI |
| `mesa/` | Pinned Mesa checkout, fetched by `scripts/fetch-mesa.sh` and gitignored. **Not a submodule, and not our source.** |
| `compat/` | Narrowly-scoped newlib/libnx gap fillers, each individually justified. |
| `toolchain/` | Meson cross file, the derived-image Dockerfile, Rust target JSON, and every pinned version |
| `scripts/` | Reproducible fetch / configure / build / check / package / CI scripts |
| `tests/` | Host suites and on-device tests |
| `examples/` | Reserved. Empty today. |
| `Makefile`, `meson.build` | The two cross-build paths; they do not build the same set |

## Contributing

[`CONTRIBUTING.md`](CONTRIBUTING.md) — the layer rules, the designs this project has
already rejected, and the evidence discipline. [`CLAUDE.md`](CLAUDE.md) is the same
contract written as instructions for an assistant.

## Licence

Project code: **MIT** ([`LICENSE`](LICENSE)), the same licence as Mesa/NVK, to stay
compatible and upstreamable. Policy and third-party components:
[`LICENSES/`](LICENSES/README.md). Mesa and libnx retain their own licences.

> The reference ports studied during early research are GPL-2.0/AGPL-3.0 licensed. No
> source text from them is copied into this repository — only publicly documented
> behaviour, re-expressed independently.

## Legal

This project contains no Nintendo code, no NVIDIA proprietary blobs, and no
copyrighted firmware. It targets homebrew execution environments on hardware the user
owns.
