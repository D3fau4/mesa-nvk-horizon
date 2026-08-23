# Running it on a console

Read this before assuming what you have: **there is no ICD you can drop next to an
application, no loader, and no shared library.** Nothing on a console finds this driver
at run time. What exists is a Vulkan implementation you link *into* your own homebrew,
statically — plus a set of standalone `.nro` programs that each exercise one part of it
and print a machine-checkable verdict.

That is what "Phase 6 complete" means in [`STATUS.md`](../STATUS.md): the driver
presents a `VK_KHR_swapchain` on real hardware, and the way you see it do that is by
running `t_vk_swapchain.nro`.

**There is now something to install, and it is not an ICD.** `make install` puts the
driver into devkitPro's portlibs prefix as an ordinary static package — archives,
headers and an `nvk.pc` — so another project can build against it with
`pkg-config --cflags --libs nvk` and never name a directory of this repository. See
[`BUILDING.md`](BUILDING.md) §6. The result is still a static link into your own
`.nro`; the paragraph above is unchanged by it.

## What you need

- A Nintendo Switch running homebrew — any CFW that gets you to hbmenu. Nothing here
  needs a specific firmware version, network access, or any Nintendo file.
- An SD card.
- The `.nro` files: build them ([`BUILDING.md`](BUILDING.md)) or take them from a
  release. **Read what a release does and does not claim** — see
  [`RELEASING.md`](RELEASING.md) — because a released artefact is cross-built and has
  not necessarily run on any console.

This project contains no Nintendo code, no NVIDIA blobs and no firmware. It targets
homebrew execution environments on hardware you own.

## Running

1. Copy the `.nro` files to `sdmc:/switch/horizon_gpu_tests/`.
2. Launch one from hbmenu.
3. Read the verdict. Every test ends with a line of the form

   ```
   RESULT: PASS (52/52)
   ```

   or `RESULT: FAIL (k/n)`, on screen **and** written to
   `sdmc:/horizon_gpu_tests/<name>.log`.
4. Press **+** to exit.

The second line of every log is the one that matters afterwards:

```
note horizon-build-id 2026-08-10T12:27:40.224Z 9053687 mesa:597ea0a
```

That is the artefact's identity — a timestamp, this repository's commit, and the Mesa
commit — stamped in at build time and read back out of the binaries when packaging.
**Quote it whenever you report a result.** A `.nro` on an SD card looks exactly like the
one it replaced; this project once spent an afternoon on a measurement of a three-day-old
build, and this line is the answer to that.

### Which order

Later tests assume what earlier ones validate, so on a console you have not run before,
go in the order in [`tests/README.md`](../tests/README.md). `t_sysinfo`, `t_threads` and
`t_ostime` use no GPU at all and need no `nv` services, so they are the cheapest first
step when triaging a console that is behaving strangely.

If a test hangs for more than about 30 seconds, hold the power button and report that:
every wait in the suite is bounded, so a hang is itself a finding.

### Applet mode and full memory

Launching from the album applet gives homebrew **237 MiB**; launching through a title
takeover gives **3155 MiB**. Both are supported and both have been measured here — the
multi-threaded WSI test passes identically in each — but they are different environments
and a report should say which one it was. Memory-pressure behaviour in particular is not
comparable across the two.

## Environment variables

These are read at runtime, so you can change behaviour without rebuilding — through
whatever mechanism your launcher offers for setting the environment.

| Variable | Values | What it does |
|---|---|---|
| `HORIZON_GPU_LOG` | `0`–`4` | Log level for the `horizon_gpu` layer: 0 none, 1 error, 2 warn (default), 3 info, 4 debug. Output is tagged `[horizon_gpu:W]` and goes to the same log file as the test's own output. |
| `HORIZON_GPU_SYNC` | `1` | Debug-synchronous mode: wait on the CPU after every submit. This is the *only* sanctioned way to insert those waits — the driver never does it on its own — and it makes an async-ordering bug reproducible at the cost of all the performance. |
| `HORIZON_GPU_UNTRUSTED_SYNCPT_BASELINE` | `1` | Accept a syncpoint baseline the layer did not establish itself. Diagnostic; see [`synchronization.md`](synchronization.md). |
| `MESA_VK_WSI_HORIZON_FORCE_COPY` | `1` | Force the copy fallback instead of the zero-copy present path. Both paths are real and each names itself in the log; this is how you exercise the one the format/tiling combination would not have chosen. |
| `MESA_VK_WSI_HORIZON_ACQUIRE_STATS` | `1` | One line a second saying where the zero-copy acquire's wait went: how much of it was spent asking a BufferQueue that had no buffer, and how much inside `nvMultiFenceWait` on the fence the compositor released the slot with. The two mean opposite things — the first is a consumer that has not handed a buffer back, the second is one that handed it back and had not finished reading it — and from outside the driver they are one number. Off by default: a per-second line from inside `vkAcquireNextImageKHR` is not something an application should pay for or read. |
| `MESA_VK_NVKMD_HORIZON_SUBMIT_STATS` | `1` | One line a second per GPU channel saying what submits and fence waits cost: how many submits went out, how much time `nvkmd_horizon_ctx_wait` spent waiting on the CPU for *another* channel (and how many waits it skipped because this channel had already ordered them), and — for the fence waits that blocked — the mean and maximum, split into the part spent before the fence existed at all and the part spent on the syncpoint, plus how old the fence was when the wait returned. Those splits are opposite diagnoses of one slow `vkWaitForFences`; see [`synchronization.md`](synchronization.md) § 10. Off by default. |
| `NVK_DEBUG` | see NVK | Upstream NVK's own debug flags. |

`T_VULKAN_DEBUG_SYNC` is **not** one of these — it is a compile-time `#define` in
`tests/t_vulkan.c`, and it is 0 in every artefact whose result has been recorded.

## Reading a log

```
note horizon-build-id 2026-08-10T12:27:40.224Z 9053687 mesa:597ea0a
ok   nv services came up
note syncpt id 12 value at channel creation 0
...
RESULT: PASS (52/52)
```

- `ok` / `FAIL` lines are the individual checks, and the counts in `RESULT:` are of
  those.
- `note` lines are measurements, not decoration. Several of them are the entire point of
  the test they appear in: the syncpoint value at channel creation, the GPFIFO
  entry-flags outcome, the VA exhaustion behaviour, the frame intervals in the swapchain
  tests. Include them when you report.
- Mesa's own output (`vk_logi`, `mesa_loge`) is interleaved into the same file: stderr
  is redirected into the log so driver messages and test messages share one timeline.

`docs/hw-logs/` holds 167 of these from earlier runs, plus one console fatal report, with
a [narrative index](hw-logs/README.md) explaining what each one settled. Failing logs are
kept deliberately — including ones later found to have measured less than they claimed.

## Reporting a run

Open a **Hardware run report** issue. Passing runs are as welcome as failing ones: they
are how a change written without a console in the room gets promoted from *cross-built*
to *verified*, which is the distinction this whole project is organised around
([`known-risks.md`](known-risks.md) R2).

Include:

- the `note horizon-build-id …` line,
- which `.nro`,
- the `RESULT:` line,
- the log file itself, attached rather than retyped,
- hardware or emulator, CFW version, applet or full memory, docked or handheld,
- and, if the console produced one, the fatal report from
  `sdmc:/atmosphere/fatal_reports/`.

## What to expect, honestly

- **The 18 `horizon_gpu` tests** exercise the layer under Vulkan: `nv` bring-up, memory,
  address space, channels, submission, syncpoints, teardown, and the `nwindow` path.
  These need no Mesa build.
- **The 14 `t_vk_*` tests** are the Vulkan ones — buffer fill and readback, transfers,
  compute, a triangle, textures, depth, formats, concurrent submits, capabilities, the
  swapchain, the multi-threaded swapchain, `VK_SUBOPTIMAL_KHR` and
  `VK_PRESENT_MODE_IMMEDIATE_KHR`. They only exist if you built NVK.
- **One of them wants your hands.** `t_vk_suboptimal`'s section D needs the console
  **docked or undocked while the test is running** — nothing in the process can resize a
  VI layer, so that is the one part of it no run has executed, and
  `VK_SUBOPTIMAL_KHR` has still never been *returned* on hardware. Thirty seconds of
  somebody's attention closes it; if you have a dock, this is the single most valuable
  run you can send.
- **What has never been verified**: docked resolution, and two surfaces over two
  `NWindow`s. The current known failures are listed in `STATUS.md` and are not hidden.
- **This is pre-1.0 software driving a GPU through system services.** It has taken a
  console down before — `qlaunch` fatal `0x290`, kept in `docs/hw-logs/` as evidence and
  fixed by patch `0070`. Nothing here can write to system storage, but a hang or a
  forced power-off is a real possibility. Run it when you are willing to reboot.
