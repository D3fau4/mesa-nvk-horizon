# Tests — running them on a Nintendo Switch

Thirty-five standalone `.nro` homebrew apps. Each prints one line per check and a final
machine-checkable verdict — `RESULT: PASS (n/n)` or `RESULT: FAIL (k/n)` —
to the console **and** to `sdmc:/horizon_gpu_tests/<name>.log`, so results
can be reported back as plain text (known-risks R2).

They come in three groups, and **which of them you get depends on what you built**:

| Group | Count | Needs | Built by |
|---|---|---|---|
| `horizon_gpu` — Phases 1, 5 and 6 | 18 | nothing but the toolchain | both build paths |
| Mesa's own code, measured on hardware | 3 | Mesa's core built | both build paths |
| Vulkan, through NVK | 14 | the full NVK driver built | **the Meson path only** |

So the Makefile produces at most 21 and the Meson path at most 35 — see
[`../docs/BUILDING.md`](../docs/BUILDING.md) for why there are two and how they differ.

There are also six host-side suites that need no console at all; they are at the bottom
of this file.

## Building

With devkitA64 installed (`$DEVKITPRO` set):

```sh
make            # or: scripts/build-switch.sh
```

Without a local toolchain, with Docker available:

```sh
scripts/build-switch.sh     # uses ghcr.io/d3fau4/nx-dev:latest
```

Outputs land in `build/*.nro`.

`t_threads`, `t_ostime` and `t_shader_cache` link the archives Mesa's own
build produced, so they need Mesa built first:

```sh
scripts/configure-mesa.sh && scripts/build-mesa.sh
```

Without that, both build paths skip those three with a message and produce
the other eighteen — and the Makefile path also deletes any `.nro` a
previous build with Mesa present had left, so `build/` never mixes
artefacts from two builds. Nothing else in this group needs Mesa; the
fourteen Vulkan tests need all of it, and NVK besides.

`$MESA_BUILD_DIR` selects where Mesa was built (default `build/mesa-probe`)
and is honoured by all four of `scripts/{configure,build}-mesa.sh`, the
Makefile and the Meson build; set it for the Mesa build and for the test
build alike. How the path is spelled does not matter —
`build/mesa-probe`, `build/mesa-probe/` and `./build/mesa-probe` are the
same directory to every consumer, including `make clean`.

`make clean` leaves that directory alone, so `make clean && make` still
produces all twenty. It also leaves `build/toolchain` alone, since the
pinned Meson and Mesa's Python dependencies are installed there over the
network. Everything else under `build/`, including the Meson build
directory, is removed.

Building Mesa *after* configuring the Meson build directory is fine:
`scripts/build-horizon.sh` notices that the archives have appeared and
reconfigures, so it does not keep producing eighteen.

`scripts/check-mesa-test-parity.sh` checks that the Makefile and the
Meson build still agree on which tests these two are, which archives
they link, and which defines and include paths they compile with. Run it
after touching either build system.

## Running on the console

1. Copy the `.nro` files to `sdmc:/switch/horizon_gpu_tests/`.
2. Launch them from hbmenu **in this order** (later tests assume the
   areas earlier tests validate):

   | # | Test | Verifies |
   |---|------|----------|
   | 1 | `t_init` | nv bring-up/teardown, GM20B fields, repeatability |
   | 2 | `t_alloc` | aligned allocation, overflow rejection |
   | 3 | `t_nvmap` | NvMap create/close, id/handle validity |
   | 4 | `t_va_reserve` | VA reserve/release, alignment, exhaustion |
   | 5 | `t_map` | fixed-VA map/unmap, kinds, cleared-VA invariant, big pages |
   | 6 | `t_channel` | channel create/destroy, syncpoint ids, Zcull, N channels |
   | 7 | `t_submit` | minimal submit executes; 2 submits in flight; R3/R10 probes |
   | 8 | `t_syncpt` | exactly one increment per submit |
   | 9 | `t_fence_wait` | wait-after-completion; timeout path timing |
   | 10 | `t_teardown` | full reverse teardown, zero leaks, twice per process |
   | 11 | `t_sysinfo` | `compat/sysconf.c`: page size bounded from both sides, process memory, `_SC_NPROCESSORS_*` |
   | 12 | `t_threads` | Mesa's C11 threads shim: `mtx_timedlock` and `cnd_timedwait` expiry (both-sided timing on the calls that must expire; an upper bound only on the ones that must not wait), mutual exclusion, condvars, TSS, and the CPU count |
   | 13 | `t_ostime` | Mesa's `os_time.c`: monotonicity, resolution, rate against the ARM counter, `os_time_sleep` accuracy |
   | 13a | `t_shader_cache` | the shader disk cache, on the SD card it will live on **and** through Mesa's own `disk_cache_*` API. **Run it twice**: its section C leaves entries behind and reports on the next launch whether they came back, so the first run on any console is cold, which is a pass and is not the measurement. See [`../docs/shader-cache.md`](../docs/shader-cache.md) |
   | 14 | `t_gpuwrite` | does a GPU write become visible to the CPU — the question nothing below `t_vulkan` had asked |
   | 15 | `t_uncached` | the UNCACHED cache policy (decision D14) on real memory |
   | 16 | `t_pbsize` | how large a single GPFIFO entry this hardware will execute (decision D15) |
   | 17 | `t_va_window` | what the address-space allocator does around the shader local/shared memory window |
   | 18 | `t_fault` | a deliberate MMU fault, and the fence that must not lie about it |

   Tests 11–13a use no `horizon_gpu` and need no nv services, so they are
   also the cheapest to run first when triaging a console.

   `t_fault` provokes a fault on purpose and is best run **last**: whether the console
   survives it on exit is one of the open items in `STATUS.md`.

   Then the display, which needs the console's own framebuffers out of the way:

   | # | Test | Verifies |
   |---|------|----------|
   | 19 | `t_display` | owning the display and drawing nothing — the `nwindow` the console configured |
   | 20 | `t_nwindow` | the VI compositor through raw `bq*`, with no Vulkan in the way |

   Finally the Vulkan tests, which exist only if you built NVK. Run them in this order:
   each reads its result back through machinery the previous one proved.

   | # | Test | Verifies |
   |---|------|----------|
   | 21 | `t_vulkan` | the mandatory Phase 4 sequence, ending in a CPU readback that validates |
   | 22 | `t_vk_transfer` | buffer↔buffer and buffer↔image copies — every later test reads back through one |
   | 23 | `t_vk_compute` | compute dispatch: the first shader this project runs, and therefore NAK and NIL |
   | 24 | `t_vk_image` | off-screen images and clears |
   | 25 | `t_vk_triangle` | the first draw call |
   | 26 | `t_vk_texture` | textures: upload, read back, and sample three ways |
   | 27 | `t_vk_depth` | depth, four draws in one render pass differing only in push constants |
   | 28 | `t_vk_format` | format coverage — everything before this proved one format |
   | 29 | `t_vk_submits` | two or more submits in flight with no CPU wait between them |
   | 30 | `t_vk_caps` | what the driver claims, measured against what the backend can do |
   | 31 | `t_vk_swapchain` | a `VK_KHR_swapchain` on the compositor: pacing, buffering, recreation, errors |
   | 32 | `t_vk_wsi_mt` | the same swapchain under concurrency and under length |
   | 33 | `t_vk_suboptimal` | `VK_SUBOPTIMAL_KHR` against `VK_ERROR_OUT_OF_DATE_KHR`, and the line between them. Its section D needs somebody to **dock or undock the console while it runs** — nothing in the process can resize a VI layer, so that is the one part no run has executed |
   | 34 | `t_vk_immediate` | whether `VK_PRESENT_MODE_IMMEDIATE_KHR` does anything through Vulkan: FIFO, then IMMEDIATE, then FIFO again, 240 frames each, so the reference is shown to be stable rather than assumed |

3. Each test ends with "Press + to exit". The verdict is on screen and in
   `sdmc:/horizon_gpu_tests/<name>.log`.
4. Report back the log files (or just their `RESULT:` lines plus any
   `FAIL`/`note` lines). The `note` lines carry the measurements the
   design needs: the syncpoint value at channel creation (R5), the R3
   entry-flags outcome and the R10 syncpoint-wait outcome in `t_submit`,
   and the exhaustion behaviour in `t_va_reserve`. For `t_threads` and
   `t_ostime` the `note` lines are the measurement: the elapsed time each
   timed call actually took, the raw `InfoType_CoreMask`, and the clock
   resolution.

Beyond the build step above for tests 12 and 13, nothing here requires a
specific firmware beyond homebrew-capable CFW, or network access. If a test hangs longer than ~30 s, hold the power
button, note which test it was, and report that too — every wait in the
suite is bounded, so a hang is itself a finding.

## Getting a log off the console

A test that sets `test_uses_display` starts no console, so its log file
*is* the whole record and nothing of it reaches a screen. `testfw` used
to stream itself over nxlink and that was removed at the user's
direction (`STATUS.md`, 2026-08-08): the socket driver was the one
variable that correlated with run 14's MMU fault, and streaming per line
also puts network I/O inside the very loops the swapchain tests measure
the pacing of.

`tools/logcat/` reads the file back afterwards instead — a separate
program, launched separately, once the measured run has ended. The
`.nro` under test still links no socket, and nothing about reading a
file that is already on disk can perturb what it records.

```sh
scripts/build-logcat.sh                       # -> build/logcat.nro
```

Copy `build/logcat.nro` to the card beside the tests and launch it, or send
it over NetLoader (console at hbmenu → Y) with devkitPro's `nxlink`:

```sh
nxlink -s -a <ip> build/logcat.nro
nxlink -s -a <ip> build/logcat.nro -- t_vk_immediate
```

*(Earlier revisions of this file pointed at a wrapper script under
`.claude/`, which was removed from the repository in `825d2f4` and does not
exist in any clone.)*

With no argument it lists `sdmc:/horizon_gpu_tests` with sizes; with
one it prints that log, resolving a bare stem against that directory and
taking anything containing `:` as a whole path. **Check for the trailing
`===== END … (n bytes) =====` marker**: without it the stream was cut
short and what you are reading is not the whole file. That failure looked
exactly like a homebrew hanging at the point the text stopped, and cost
one run being diagnosed as a driver hang that had not happened.

## Host-side unit tests (no console needed)

```sh
scripts/run-host-tests.sh
```

builds seven suites over the pure-logic modules (alignment/overflow, VA interval set,
wrap-safe syncpoint math, command emitters, status strings, logging, and the shader
cache's blob store) with the host compiler and sanitizers. These run anywhere, and CI runs them on every push and pull
request.

---

For what the logs mean, the runtime environment variables, applet mode against full
memory, and what to include when reporting a run, see
[`../docs/USAGE.md`](../docs/USAGE.md).
