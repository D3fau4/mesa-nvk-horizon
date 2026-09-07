# Tests — running them on a Nintendo Switch

**Fourteen** standalone `.nro` homebrew apps, one per **suite**. A suite is a
subsystem; inside it, each **case** is one of the tests this project used to ship as
a `.nro` of its own. Fifty-three cases in fourteen binaries.

Each suite prints one line per check, one verdict per case, and one
machine-checkable verdict for the whole run — to the console **and** to
`sdmc:/horizon_gpu_tests/<suite>.log`, so results can be reported back as plain text
(known-risks R2):

```
== vk_shaders (7 cases) ==
  note horizon-build-id 2026-09-07T09:14:03Z-a1b2c3d
-- vk_shaders/compute_dispatch --
  ok   vkCreateComputePipelines -> 0
  ...
CASE: PASS vk_shaders/compute_dispatch (18/18) in 903 ms
-- vk_shaders/dynamic_loop --
  ...
CASE: FAIL vk_shaders/dynamic_loop (12/13) in 412 ms
...
RESULT: FAIL (131/132) [1 of 7 cases failed: dynamic_loop]
```

**A case that gives up early does not take the suite with it.** It is recorded as a
failed case and the next one runs. That is the whole reason the framework has a
per-case verdict rather than one entry point: grouping tests is only safe if one
broken area cannot hide the areas behind it.

**Between cases the framework puts the environment back.** Options here are
environment variables and a case sets them on itself; as one `.nro` per test that was
airtight, and inside a suite it is not. A case that leaks one now gets a `note` saying
which. What it does *not* undo is an option a library read once and cached in a
static — that is why `vk_cache` is still its own `.nro`.

## The suites

They come in three groups, and **which of them you get depends on what you built**:

| Group | Suites | Needs | Built by |
|---|---|---|---|
| `horizon_gpu` | 6 | nothing but the toolchain | both build paths |
| Mesa's own code | 1 | Mesa's core built | both build paths |
| Vulkan, through NVK | 7 | the full NVK driver built | **the Meson path only** |

So the Makefile produces at most 7 and the Meson path at most 14.

There are also eight host-side suites that need no console at all; they are at the
bottom of this file.

## Building

With devkitA64 installed (`$DEVKITPRO` set):

```sh
make test            # or: scripts/build-switch.sh test
```

Without a local toolchain, with Docker available:

```sh
scripts/build-switch.sh test     # uses ghcr.io/d3fau4/nx-dev:latest
```

(`make`/`scripts/build-switch.sh` with no target builds only
`libhorizon_gpu.a`, with none of these `.nro`s.)

Outputs land in `build/*.nro`.

`mesa_runtime` links the archives Mesa's own build produced, so it needs Mesa built
first:

```sh
scripts/configure-mesa.sh && scripts/build-mesa.sh
```

Without that, both build paths skip it with a message and produce the other six — and
the Makefile path also deletes any `.nro` a previous build with Mesa present had left,
so `build/` never mixes artefacts from two builds. Nothing else in that group needs
Mesa; the seven Vulkan suites need all of it, and NVK besides.

`$MESA_BUILD_DIR` selects where Mesa was built (default `build/mesa-probe`)
and is honoured by all four of `scripts/{configure,build}-mesa.sh`, the
Makefile and the Meson build; set it for the Mesa build and for the test
build alike. How the path is spelled does not matter —
`build/mesa-probe`, `build/mesa-probe/` and `./build/mesa-probe` are the
same directory to every consumer, including `make clean`.

`make clean` leaves that directory alone, so `make clean && make` still
produces all seven. It also leaves `build/toolchain` alone, since the
pinned Meson and Mesa's Python dependencies are installed there over the
network. Everything else under `build/`, including the Meson build
directory, is removed.

Building Mesa *after* configuring the Meson build directory is fine:
`scripts/build-horizon.sh` notices that the archives have appeared and
reconfigures, so it does not keep producing six.

`scripts/check-mesa-test-parity.sh` checks that the Makefile and the Meson build still
agree on which suites exist, **which cases are in each of them**, which archives they
link, and which defines and include paths they compile with — and that every case both
name has a file. Run it after touching either build system.

## Running on the console

1. Copy the `.nro` files to `sdmc:/switch/horizon_gpu_tests/`.
2. Launch them from hbmenu **in this order**. Later suites assume the areas earlier
   suites validate, and the cases inside a suite are already in their own dependency
   order — that used to be a numbered list in this file and is now a property of
   `tests/<suite>/suite.c`, which is the thing that decides it.

   ### The seven that need no NVK

   | # | Suite | Cases, in run order | Verifies |
   |---|-------|---------------------|----------|
   | 1 | `platform` | `nv_bringup`, `teardown`, `sysconf` | nv bring-up/teardown and the GM20B fields; full reverse teardown twice per process with zero leaks; `compat/sysconf.c` against the real kernel |
   | 2 | `gpu_memory` | `alloc`, `nvmap`, `va_reserve`, `va_map`, `uncached`, `shader_window` | aligned allocation and overflow rejection; NvMap create/close; VA reserve/release and exhaustion; fixed-VA map/unmap, kinds, big pages; the UNCACHED policy (D14); what the allocator does around the shader local/shared memory window |
   | 3 | `gpu_submit` | `channel`, `submit`, `syncpt_per_submit`, `syncpt_cpu_incr`, `fence_wait`, `fence_wait_many`, `gpu_write`, `pushbuf_size` | channel create/destroy and Zcull; a minimal submit executing, two in flight; exactly one increment per submit; advancing a syncpoint from the CPU; wait-after-completion and the timeout path; how many waits the platform holds at once; a GPU write becoming visible to the CPU; how large a single GPFIFO entry this hardware executes (D15) |
   | 4 | `mesa_runtime` | `disk_cache`, `c11_threads`, `os_time` | the shader disk cache on the SD card **and** through Mesa's own `disk_cache_*` API; Mesa's C11 threads shim (`mtx_timedlock`/`cnd_timedwait` expiry, mutual exclusion, condvars, TSS, CPU count); Mesa's `os_time.c` (monotonicity, resolution, rate against the ARM counter, sleep accuracy). **Run this suite twice** — see below |
   | 5 | `display` | `console_handoff`, `nwindow` | owning the display and drawing nothing; the VI compositor through raw `bq*`, with no Vulkan in the way. **Owns the display: no console** |
   | 6 | `dock` | `mode_change` | whether a display mode change reaches this process at all. **Needs somebody to dock or undock the console while it runs**; it watches for sixty seconds and reports what it saw, including nothing |
   | 7 | `gpu_fault` | `sparse`, `mmu_fault` | what the chip does at the edges of a sparse reservation (D12); a deliberate MMU fault, the fence that must not lie about it, and teardown afterwards. **Run this last** |

   `platform` and `mesa_runtime` use no `horizon_gpu` beyond bring-up and need almost
   nothing of the driver, so they are the cheapest to run first when triaging a
   console.

   `gpu_fault` is last on purpose: both of its cases lose a channel by design, and
   the console is **not** entirely well afterwards. Measured three times on
   2026-09-07: the suite passes 44/44 and writes its whole log, and then the process
   is killed 45 to 60 seconds later with nobody touching it — no `+` pressed, no
   crash report written, its own `atexit` marker never reached. Expect to relaunch
   hbmenu after running it, and do not read the death as a failure of the cases.
   `docs/MEASURED-ON-HARDWARE.md` has the three runs and what they exclude.

   ### The seven that exist only if you built NVK

   | # | Suite | Cases, in run order | Verifies |
   |---|-------|---------------------|----------|
   | 8 | `vk_core` | `bringup`, `transfer`, `upload_chunks`, `device_memory`, `image_clear`, `capabilities`, `timestamps`, `concurrent_submits`, `submit_batching`, `sparse_binding` | the mandatory Phase 4 sequence ending in a validated CPU readback; buffer↔buffer and buffer↔image copies, which every later case reads its result back through; the command-buffer upload chunk patch `0073` cleans, forced and read back; what `vkAllocateMemory` costs and whether memory nobody zeroed is usable; several command buffers inside one submit; off-screen images and clears; what the driver claims measured against what the backend can do; what a GPU timestamp tick is worth; two or more submits in flight with no CPU wait between them; sparse binding through `vkQueueBindSparse` |
   | 9 | `vk_shaders` | `compute_dispatch`, `dynamic_loop`, `packed_bound_loop`, `nested_control_flow`, `nested_control_flow_frag`, `crs_matrix`, `fragment_kill`, `descriptor_set1` | the first shader this project runs, and therefore NAK and NIL; a loop whose trip count comes from memory, in the shape of Godot's Forward+ cluster loops; the same loop with those loops' exact packed min/max bound idiom; twelve levels of nested control flow in compute and again in fragment, where sm50's `crs_size()` reserves nothing behind the convergence stack; a loop behind a `kill`; whether a fragment shader reads descriptor set 1 correctly, including after the GPU writes that set in the same command buffer; and, in `crs_matrix`, eight fragment shaders that vary the convergence-stack depth and the register count independently, each drawn with the nest unentered, entered uniformly and entered divergently |
   | 10 | `vk_render` | `triangle`, `texture`, `depth`, `formats`, `multi_target`, `zcull`, `draw_volume` | the first draw call; textures — upload, read back, sample three ways; depth, four draws in one render pass differing only in push constants; twelve colour formats against the bytes their encodings demand; a pass that declares three render targets and writes one; whether Zcull is correct, by rendering one workload twice (once with `NVK_HORIZON_ZCULL=0`) and comparing pixel for pixel; 512 draws with the pipeline changing between them, the same draws across 64 render passes, and an alpha blend chain checked against the CPU |
   | 11 | `vk_pipelines` | `pipeline_volume` | 96 distinct compute pipelines back to back — the only thing that makes the contiguous shader heap grow past the chunk it binds at device creation — plus create/destroy churn over reused heap addresses, and the per-pipeline compile times an application feels as stutter |
   | 12 | `vk_cache` | `shader_reuse` | the only case that asks what the shader cache is *for*: is a shader compiled on one launch still compiled on the next. **Run it twice** — see below |
   | 13 | `vk_wsi` | `swapchain`, `suboptimal`, `concurrency` | a `VK_KHR_swapchain` on the compositor — pacing, buffering, recreation, errors; `VK_SUBOPTIMAL_KHR` against `VK_ERROR_OUT_OF_DATE_KHR` and the line between them; the same swapchain under concurrency and over length. **Owns the display: no console.** `suboptimal`'s section D needs somebody to **dock or undock the console while it runs** — nothing in the process can resize a VI layer, so that is the one part no run has executed. `concurrency` used to end the process in its section D and no longer does — see the note under the table |
   | 14 | `vk_present` | `drawn_frame`, `present_modes` | a frame a graphics pipeline drew, rather than the `vkCmdClearColorImage` every other presenting case uses — and its section F, the only place in the tree that acquires with a binary semaphore and waits for it on the GPU; FIFO against IMMEDIATE against FIFO, 240 frames each, so the reference is shown to be stable rather than assumed. **Owns the display: no console** |

3. Each run ends with "Press + to exit". The verdict is on screen and in
   `sdmc:/horizon_gpu_tests/<suite>.log`.
4. Report back the log files (or just the `RESULT:` and `CASE:` lines plus any
   `FAIL`/`note` lines). The `note` lines carry the measurements the design needs: the
   syncpoint value at channel creation (R5), the R3 entry-flags outcome and the R10
   syncpoint-wait outcome in `gpu_submit/submit`, and the exhaustion behaviour in
   `gpu_memory/va_reserve`. For `mesa_runtime` the `note` lines *are* the measurement:
   the elapsed time each timed call actually took, the raw `InfoType_CoreMask`, and the
   clock resolution.

### The one that used to end the process

`vk_wsi/concurrency` section D killed the process for as long as it
existed, and the shape of the failure is worth keeping because the next
libnx convenience wrapper to be used from a driver will do the same
thing. `framebufferBegin` calls `diagAbortWithResult` when
`nwindowDequeueBuffer` fails, so there is no return path for the
framework to record a failed case through — the process simply dies on
the HOME menu with the crash dialog. Two of the three failures it
aborts on are not failures at all: they are "the compositor has not
released a buffer yet", which is what the zero-copy acquire has always
slept on and retried. Section D is the only place in the suite that
asks for two images, and a two-image copy-fallback swapchain reaches
that condition on its third frame every time.

Patch `0071` gave the copy fallback its own dequeue with the acquire's
retry policy behind it. `vk_wsi` now runs all three cases to the end.

### The two that need two launches

Neither is a failure on its first run, and neither can be faked within one process.

- **`mesa_runtime`.** Its `disk_cache` case leaves entries behind and reports, on the
  next launch, whether they came back. The first run on any console reports a cold
  cache and that is a pass. Nothing else in the suite cares how many times it has run.
- **`vk_cache`.** It uses the driver's own cache directory with no environment
  overrides, so it needs the same build launched twice — **do not rebuild in
  between**, because a rebuild changes the driver identity and empties the cache by
  design, which the case detects and reports rather than calling a loss. **A cold run
  deletes `sdmc:/mesa_shader_cache/*.hzc` first**, so that a run calling itself cold is
  one; any shader another homebrew left there is recompiled once, which is the whole
  cost of clearing a cache. Delete `sdmc:/horizon_gpu_tests/t_vk_cache_launch.txt` to
  make the next run cold.

Beyond the build step above for `mesa_runtime`, nothing here requires a specific
firmware beyond homebrew-capable CFW, or network access. If a suite hangs longer than
~30 s in one case, hold the power button, note which suite it was and which
`-- suite/case --` banner the log stops after, and report that too — every wait in the
tree is bounded, so a hang is itself a finding.

## Adding a test

**Add a case to the suite it belongs to. Do not add a `.nro`.**

1. Write `tests/<suite>/<case>.c`. It opens with
   `TEST_CASE_DECL(<suite>, <case>) { ... }` and owns everything it creates: a case
   leaves the process as it found it, which is what lets the next case start from a
   known state. Every Vulkan case here already does that — it builds its own
   `VkInstance` and `VkDevice` and destroys them.
2. Name it in `tests/<suite>/suite.c`, in the position its dependencies want.
3. Name it in **both** build systems: `CASES_<suite>` in the `Makefile` and the
   `<suite>` entry of `horizon_suites`/`mesa_suites`/`nvk_suites` in `meson.build`. If
   it uses a shader, add its `'<suite>/<case>'` entry to `nvk_case_shaders` too.
4. `scripts/check-mesa-test-parity.sh` fails if you did 3 in only one of them, or if
   the file from 1 is missing, or if its `#include "<x>.spv.h"` lines and its shader
   entry disagree.

A new `.nro` needs a technical reason that grouping would break, and the reason goes
in the new `tests/<suite>/suite.c`'s header. The four that exist are the shape of it:
a console *and* an operator (`dock`), a deliberate fault whose after-effects are
unconfirmed (`gpu_fault`), a measurement that a busier process would change
(`vk_pipelines`), and state destroyed across launches on a protocol of its own
(`vk_cache`). "These feel like different things" is not one of them.

## Getting a log off the console

A suite that sets `test_uses_display` starts no console, so its log file
*is* the whole record and nothing of it reaches a screen. `testfw` used
to stream itself over nxlink and that was removed at the user's
direction (2026-08-08): the socket driver was the one
variable that correlated with run 14's MMU fault, and streaming per line
also puts network I/O inside the very loops the swapchain cases measure
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
nxlink -s -a <ip> build/logcat.nro -- vk_present
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

builds eight suites over the pure-logic modules (alignment/overflow, VA interval set,
wrap-safe syncpoint math, command emitters, block-linear scanout layout, status strings,
logging, and the shader cache's blob store) with the host compiler and sanitizers. These run anywhere, and CI runs them on every push and pull
request.

These are a different mechanism from the console suites above — their own `main()` in
`tests/host/hostfw.h`, the host compiler, ASan and UBSan — and they are not affected by
anything on this page.

---

Each run prints `RESULT: PASS (n/n)` or `RESULT: FAIL (k/n)` and one `CASE:` line per
case; those lines, plus the `note horizon-build-id …` line and the full console output,
are what to include when reporting a run.
