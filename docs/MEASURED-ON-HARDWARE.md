# Measured on hardware

Facts about this platform that were established by running code on a
Nintendo Switch, kept in one place **so nobody re-litigates them**. None
of it needs action. Each entry is here because the cost of measuring it
again is higher than the cost of the paragraph.

**This file is not a debt ledger, and it must never become one.** Its
predecessor, `docs/PENDING-VERIFICATION.md`, was: it tracked work that
had been cross-compiled and never run, and every section carried a
"Done when" line. It was deleted when its last section closed — as its
own rules demanded, rather than being hollowed out into an "all clear"
that says nothing. Work that is still owed belongs in a commit message
or in a new ledger, not appended here.

The project's evidence discipline has three classes and never collapses
them. Everything below is class **HW**:

| Class | Means | Does **not** prove |
|---|---|---|
| **H** — host | Built and run via `scripts/run-host-tests.sh` | Anything about the Switch |
| **X** — cross | Cross-compiled for aarch64 Horizon; a `.nro` exists | That it runs, or is correct |
| **HW** — hardware | Ran on a real console, with the log | Only what the log actually shows |

A result whose log has no `horizon-build-id` line cannot be attributed
to a build and does not belong here.

**The test names below are the ones the runs used, and several no longer
exist as files.** The suite refactor turned fifty-three `.nro` into
fourteen, each holding several cases, and a result is now identified as
`<suite>/<case>`: `t_vk_zcull` is `vk_render/zcull`, `t_nwindow` is
`display/nwindow`, and so on for the rest. They are not renamed here on
purpose — this file records what a particular run reported, and a run
reported the name it printed. `tests/<suite>/suite.c` is the mapping,
and `git log --follow` on any of these paths reaches the old file.

---

## The fourteen suites again, after the WSI and allocator work

2026-09-07, build `3409462` plus the commits above it, mesa `47be3e0`
(`MESA_COMMIT` + 74 patches), handheld, 1280x720, full-memory takeover,
netloaded one at a time. Every log carries the `horizon-build-id` line.

| Suite | Result |
|---|---|
| `platform` | **PASS 87/87** [3/3] |
| `gpu_memory` | **PASS 133/133** [6/6] |
| `gpu_submit` | **PASS 324/324** [8/8] |
| `mesa_runtime` | **PASS 166/166** [3/3] |
| `display` | **PASS 160/160** [2/2] |
| `vk_core` | **PASS 1340/1340** [9/9] |
| `vk_shaders` | **PASS 518/518** [7/7] |
| `vk_render` | **PASS 2637/2637** [7/7] |
| `vk_pipelines` | **PASS 225/225** [1/1] — cold compile mean 111801 us, warm 5 us |
| `vk_cache` | **PASS 43/43** cold and **45/45** warm, two launches; 5275 us cold against 448 us warm, 91% |
| `vk_present` | **PASS 1038/1038** [2/2] |
| `vk_wsi` | **PASS 571/571** [3/3] — `swapchain` 227/227, `suboptimal` 273/273, `concurrency` 71/71 |
| `gpu_fault` | **PASS 44/44** [2/2], and then the process died — see below |
| `dock` | **PASS 4/4** [1/1] in 60066 ms — nobody docked, and the case says so rather than passing quietly: "0 change(s) seen ... NOTHING moved. Either nobody touched the console, or a dock does not reach a process launched this way" |

**The two failures the previous run had are gone**, and neither was
fixed by making a test agree with a driver: `vk_wsi/suboptimal` went
272/273 to 273/273 because `currentExtent` stopped lagging, and
`vk_wsi/concurrency` went from ending the process to 71/71 because the
copy fallback stopped calling a libnx function that aborts.

**`gpu_fault` passes and then the process dies.** The suite reported
`PASS (44/44) [2/2 cases]` and wrote its whole log, build-id line
included; some time later, while idling on its "press + to exit"
screen, the process ended with the system's own "the software was
closed because an error occurred" dialog. That is not in the log and
was not seen on 2026-09-07's first run. What the suite does is provoke
an MMU fault on purpose, and `tests/README.md` has always said its
after-effects on the console are unconfirmed. Recorded here as an
observation; what is owed about it is in
`docs/PENDING-HARDWARE-RUNS.md`.

## `currentExtent` is the layer's size now, and stays it

The defect: `wsi_horizon_get_extent` re-read `NWindow::default_*` on
every surface query, and that field has two authors — the consumer, and
this backend, through the registration `nwindowSetDimensions` performs
at every swapchain creation. Since `0054` let an application choose an
extent smaller than the layer, a swapchain smaller than the layer
taught the SURFACE that the output had shrunk, one connect later.

`0070` latches the layer once and answers every query from the latch;
`0074` takes that latch from `nwindowGetDimensions` rather than from
`default_*`, which works because `0070` also made
`wsi_horizon_release_window` put `NWindow::width` back to the layer's
size whenever this backend gives the window up. One field, written by
us to the layer on release and read by us on latch.

Measured, `vk_wsi/swapchain` section H, three cycles of
1280x720 → 640x360 → 1280x720 in one process with the way back done by
recreation:

- 3 of 3 cycles presented every frame, 36 at half and 36 at the layer;
- 0 creations refused, where the previous build refused two;
- the surface reported 1280x720 at the top of every cycle;
- 0 presents called SUBOPTIMAL at either size;
- and the surface still reported 1280x720 afterwards.

The queue's lag is still there and is now visible rather than
believed. `vk_wsi/suboptimal` opens a second surface after
`vk_wsi/swapchain` has finished with the window, and the driver says:

    wsi_horizon: taking 1280x720 as the layer; the queue's default
    buffer size says 640x360, which is this process's previous
    registration arriving a connect late

On the build before `0074` the same line was a warning that it had
taken 640x360.

## `vk_wsi/concurrency` was libnx aborting, not a race

`framebufferBegin` calls `diagAbortWithResult` on any failed
`nwindowDequeueBuffer`, so the copy fallback ended the process rather
than returning an error. Two of the results it aborts on —
`LibnxBinderError_WouldBlock` and `LibnxBinderError_NoInit` — are the
ordinary "the compositor has not released a buffer yet" that the
zero-copy acquire has always slept on and retried; the second is what a
two-image swapchain gets when both its buffers are queued, which
`wsi_horizon_dequeue_would_block`'s comment has recorded since it was
written.

It was deterministic, not a race. Section D is the only place in
`tests/vk_wsi` that asks for **two** images — every other
`mt_sc_create` in that file asks for three — and it alternates the
image count 2/3 with the present path on `(gen % 4) >= 2`, so its
generation 2 is the first swapchain in the process that is both two
images and on the copy path. The third frame of that generation finds
both buffers queued. That is why it reproduced identically on the
series truncated to `0062`: the path has been there since `0037`.

`0071` gives the copy fallback its own dequeue with the acquire's retry
policy behind it. `vk_wsi/concurrency` is 71/71, including its section
D (30 of 30 generations, 900 frames, each generation's predecessor
destroyed on another thread while it presented) and its section F
(3000 frames over 14 generations with a device-work thread beside it).

## An upload chunk that nobody cleaned

Found by taking the allocator's zero fill away from command memory and
watching `vk_core/transfer` break.

`nvk_cmd_buffer_upload_alloc` has two paths. The fast one carves a
range out of `cmd->upload_mem` and advances `used_B`, which is what
`flush_mem_list` cleans. The other takes a brand-new chunk, hands the
caller offset 0 of it, and never touches `used_B` — so when that chunk
is not adopted as `cmd->upload_mem`, nothing ever sets `used_B`, the
clean is skipped, and the bytes the CPU memcpy'd stay in the CPU's
cache while the GPU reads memory.

    FAIL C inline update landed: 48/64 words match
    first mismatch at word 0: got 0xff97803c, want 0xa5dac00d (16 wrong)

One cache line of a 256-byte `vkCmdUpdateBuffer`, arriving as heap
rubbish. With the allocator filling, the same gap delivered that
chunk's zeros instead, which is why nothing had ever caught it —
`vk_core/transfer` had passed for weeks. `0073` sets `used_B` on that
path; the fill stays on command memory regardless, because the rule
`NVKMD_MEM_NO_ZERO_INIT` was introduced under is that a consumer takes
it only where every observable byte is shown to be written first, and
this one has now been shown not to be.

## What skipping the zero fill saves a real allocation

`vk_core/device_memory` section D, 2026-09-07, device-local memory,
sixty allocations per shape, three A/B pairs alternating which half
runs first, `NVK_HORIZON_ZERO_ALL_MEM=1` restoring the fill in the same
process at the same clock. The driver says once per device when it has
actually skipped a fill, and the case checks for that line — without it
a difference of zero cannot be told from a flag that never arrived.

| Allocation | not filled | filled | saved | worst single, filled → not |
|---|---|---|---|---|
| 64 KiB | 209 us | 226 us | 17 us, 7% | 734 → 491 us |
| 1 MiB | 365 us | 534 us | 169 us, 31% | 973 → 791 us |
| 8 MiB | **1462 us** | **2859 us** | **1397 us, 48%** | **3442 → 1841 us** |

Which agrees with what `gpu_memory/alloc` measured for the fill alone
at the `horizon_gpu` level (21 / 182 / 1403 us) — the fill is the whole
of the difference, and at 8 MiB it is half of what `vkAllocateMemory`
costs.

**That is a per-allocation number and not a frame time.** Whether an
application stutters depends on whether it allocates inside a frame,
which is the application's behaviour; the worst-single-allocation
column is the size of the pause it would take if it does. Nothing
measured here presents.

The path is correct where it is used: sections A, B and C of the same
case write and read back every byte of an unfilled allocation through a
CPU mapping, through a GPU fill and copy, and across 3000 commands
recorded over several command-buffer chunks.

## Deferring the acquire buys CPU, and not frames, even with work to overlap

`vk_present/drawn_frame` section G measured this with a fence acquire
and could not have shown a saving: on that path the compositor's
release fence is the payload of the fence the application passed, so
the thread waits for it two calls later whether the driver deferred it
or not.

Section H asks the same question so the answer can be a frame time.
Both halves acquire with a **semaphore**, which is the path the
deferral exists for; the only difference is
`MESA_VK_WSI_HORIZON_CPU_ACQUIRE_WAIT`; the frame contains 8.3 ms of
real CPU work — a hash over a buffer it also writes back, its result
carried out and printed — placed between the acquire and the submit,
which is the one window where the two shapes differ; and the same pair
runs again with no work at all as a control. FIFO, 1280x720, three
draws, three images, three pairs of 60 frames each way, A/B and B/A
alternated, one warm-up discarded. All times are `armGetSystemTick`,
the CPU's own clock, so `timestampPeriod` does not enter and the
1.627604 ns per tick recorded further down this file is neither applied
nor applied twice.

| | frame mean | p50 | p95 | p99 | CPU working | CPU blocked | of which acquire |
|---|---|---|---|---|---|---|---|
| work, deferred | 16083 us | 16658 | 16776 | 31755 | 8353 us | 7371 us | **113 us** |
| work, CPU wait | 16377 us | 16663 | 16717 | 30108 | 8353 us | 7571 us | **5393 us** |
| control, deferred | 16049 us | 16658 | 16830 | 30937 | 0 | 15640 us | 285 us |
| control, CPU wait | 16289 us | 16638 | 17386 | 31242 | 0 | 15639 us | 12970 us |

**The acquire is 48 times cheaper and the frame is not.** 294 us
between the two halves with work, beside 240 us between the two halves
without any — the difference is the size of the control's own spread.
Reproduced on two builds an hour apart, with the same conclusion.

The data says why, and it is structural rather than a property of this
console. Under FIFO the display paces the loop, and what the thread
does not spend working it spends blocked: 7371 us against 7571 us, the
same total to 2%. The wait is also self-limiting — the CPU-wait half's
acquire cost 5393 us with work in the frame and 12970 us without,
because the work happens before the next acquire and the compositor
releases the buffer meanwhile. Both shapes fit inside a refresh, so
both take a refresh.

So `0063`-`0068` buy **CPU time on the semaphore path and not frames
per second**, and the acquire falling from 15483 us to 252 us (section
F) must not be reported as the second thing. Where they could still
produce a frame time is `docs/PENDING-HARDWARE-RUNS.md` section 3.

## The fourteen suites, run on a console

2026-09-07, build `06d33ec` plus the test commits above it, mesa
`2e85cc8` (`MESA_COMMIT` + 69 patches), handheld, 1280x720, full-memory
takeover, netloaded one at a time. Every log carries the
`horizon-build-id` line. This is the first run of the suite architecture
on hardware, and of everything `0063`-`0069` changed.

| Suite | Result |
|---|---|
| `platform` | **PASS 87/87** [3/3] |
| `gpu_memory` | **PASS 133/133** [6/6] |
| `gpu_submit` | **PASS 324/324** [8/8] |
| `mesa_runtime` | **PASS 166/166** [3/3], twice — cold then warm |
| `display` | **PASS**, log only: it owns the display |
| `dock` | **PASS 4/4** [1/1] |
| `vk_core` | **PASS 1282/1282** [8/8] |
| `vk_shaders` | **PASS 518/518** [7/7] |
| `vk_render` | **PASS 2637/2637** [7/7] |
| `vk_pipelines` | **PASS 225/225** [1/1] |
| `vk_cache` | **PASS 43/43** cold and **45/45** warm, two launches |
| `vk_present` | **PASS 772/772** [2/2] |
| `gpu_fault` | **PASS 44/44** [2/2] — the MMU fault is recorded, the fence does not report success for work that faulted, the faulted channel tears down cleanly and the GPU still works in the process afterwards |
| `vk_wsi` | `swapchain` **PASS 158/158**; `suboptimal` **272/273**; `concurrency` ends the process. Both failures were fixed later the same day — see the run at the top of this file |

**Grouping fifty-three `.nro` into fourteen did not cost a case.** Every
case with a recorded standalone result reproduced it, and no case was
hidden by one ahead of it. What it did cost is written in
`docs/PENDING-HARDWARE-RUNS.md`, not here.

## A dock does reach the process, and still cannot resize the layer

`dock/mode_change`, 2026-09-07, with somebody docking and undocking
inside the sixty-second window: **four mode changes**, at 54104, 55734,
57486 and 57560 ms; `AppletHookType_OnOperationMode` fired six times;
the mode went 0 to 1 to 0 to 1 to 0.

`nwindowGetDimensions`, `NWindow::default_*` and
`appletGetDefaultDisplayResolution` were **1280x720 throughout, with a
buffer queued on every poll**, so the consumer's answer was being
refreshed the whole time. That is the same answer 2026-08-24 recorded
for `t_dock`, now with the mode changes happening inside a process that
was watching all three sources rather than inferred from two endpoints.

## Deferring the compositor's release fence buys CPU, not frames

`vk_present/drawn_frame`, 2026-09-07, both shapes measured in one
process at one clock — `MESA_VK_WSI_HORIZON_CPU_ACQUIRE_WAIT=1` is the
opt-out `0066` added for exactly this.

Section F, FIFO, clear frames, 180 frames each:

| | acquire mean | frame interval |
|---|---|---|
| acquired by fence, waited on the CPU | **15483 us** (max 21453) | 16515 us |
| acquired by semaphore, waited on the GPU | **252 us** (max 10959) | 16453 us |

with `nvkmd_horizon` reporting **61 wait(s) handed to the host engine**
in the semaphore run against **0** in the fence one. That counter, not
the acquire meter's, is what says the dependency reached the channel
instead of the thread.

Section G, IMMEDIATE, three draws, 180 frames, run three times so the
reference is shown to be stable rather than assumed:

| | interval mean | p50 | p95 | p99 |
|---|---|---|---|---|
| as built | 8390 us | 7073 | 16554 | 17924 |
| `CPU_ACQUIRE_WAIT=1` | 8371 us | 912 | 16656 | 16898 |
| as built again | 8326 us | 3467 | 16554 | 28371 |

**+13 us: 0% of the reference.** The driver's acquire meter over those
same runs says **7577 us mean, 7488 of it on the release fence** with
the CPU wait, against **108 us mean and 0 us on the release fence** as
built. The wait does leave the acquire, by a factor of 70, and the frame
does not get shorter.

**Those two are the same result and neither is a disappointment.** The
GPFIFO is in order, so a host-engine wait still holds every submit
behind it on that channel; what the change frees is the thread, and only
on the *semaphore* path. Section G acquires with a FENCE and then waits
for it, so the same dependency is paid two calls later — which is why G
measures nothing and F measures 15.2 ms. An application that wants this
has to let the GPU do the waiting.

Two unknowns closed on the way: no `horizon_gpu_fence_wait(...) failed`
anywhere in the run, so `NVHOST_IOCTL_CTRL_SYNCPT_READ` does answer for
a syncpoint this process does not own; and no "released a slot with N
fences, which is not a number this can carry", so one fence is what the
compositor returns.

## Batching pushes across exec() removes kickoffs, and not time

`vk_core/submit_batching`, 2026-09-07, **PASS 471/471**, with
`MESA_VK_NVKMD_HORIZON_SUBMIT_STATS=1` set inside the case.

| what ran | what the meter said |
|---|---|
| 1, 2, 4 and 8 command buffers per submission | `18 exec call(s) carrying 18 push(es) went out as 6 kickoff(s), mean 3` |
| 80 command buffers, crossing the 64-push backstop | `85 exec call(s) ... as 5 kickoff(s), mean 17` |
| the same eight with `NVK_HORIZON_BATCH_EXEC=0` | `10 exec call(s) ... as 10 kickoff(s), mean 1` |

The accumulation does what it says, the backstop takes a kickoff
mid-submission as designed, and the switch restores the old shape
exactly.

**The wall time did not follow.** Section E, one sample each: eight
command buffers in one submission took **6598 us batched and 6179 us
unbatched** — 6% the wrong way, which is to say no saving was measured.

**And a presenting workload has nothing to save.** Every
`exec call(s) ... kickoff(s)` line `vk_present` printed reads
`N exec call(s) carrying N push(es) went out as N kickoff(s), mean 1`.
The saving is N-1 kickoffs per `vkQueueSubmit` and this application's N
is 1.

## What zeroing an allocation costs

`gpu_memory/alloc`, 2026-09-07, mean of 8 at each size, cached policy,
`horizon_gpu_mem_create` against `horizon_gpu_mem_create_uninit` — which
skips the `memset` and keeps the `armDCacheFlush`, because that flush is
for the previous tenant's dirty lines and not for the fill:

| size | zero-filled | uninitialised |
|---|---|---|
| 64 KiB | 90 us | 69 us |
| 1 MiB | 328 us | 146 us |
| 8 MiB | **1992 us** | **589 us** |

The fill is 55% of a 1 MiB allocation and 70% of an 8 MiB one: 1.4 ms of
a frame, once, whenever an 8 MiB resource is created. At 64 KiB it is
21 us and not worth routing anything for.

## Zcull changes no pixel, and costs about nothing

`vk_render/zcull`, 2026-09-07: the same depth workload twice in one
process, the first time with `NVK_HORIZON_ZCULL=0`.

- **0 of 65536 pixels differ** between the two, and each half is
  analytically right on its own (0 of 65536 wrong), so a fault
  affecting both equally would not have compared equal.
- 586 ms with Zcull off, 570 ms with it on, over 12 passes of 48 draws.

`0059` and `0060` stay.

## What a cold pipeline compile costs, and what the cache saves

`vk_pipelines/pipeline_volume`, 2026-09-07, 96 distinct specializations
of a 448-instruction shader:

- **cold: min 107469 us, mean 112918 us, p99 166940 us, 10840 ms in
  total.** A hundred and thirteen milliseconds per pipeline is seven
  frames, and it is the shape of the stutter an application feels the
  first time it draws something new.
- **warm, in the same process: min 5 us, mean 6 us, p99 22 us, 0 ms in
  total.** Building the same 96 again cost 0.0% of the first build.
- across launches, `vk_cache/shader_reuse`: `vkCreateComputePipelines`
  took **5370 us cold and 1126 us warm**, 79% saved, with the driver
  reporting a disk-cache hit rather than "hits = 0".

`vk_render/draw_volume` section C, whose 3/255 tolerance was derived
from the round-off of twelve blend steps, used **none of it: the worst
channel error over 16384 pixels was 0**. The bound stays a derivation of
what blending may round to rather than a record of what it did here.

---

## Zcull bound, and Mobile and the depth path unregressed

The runs above are the first on a build carrying patches 0057-0062, and
0059/0060 change what every render pass that clears depth does. Two checks
on 2026-09-02, both clean:

- `t_vk_depth` **PASS 68/68** — four depth-tested draws through
  `nvk_CmdBeginRendering` with `zcull=bound`, colour and depth right on
  4096/4096 pixels each.
- Godot 4.1 **Forward Mobile**, the same eight-phase bench that
  `godot-nx-renderer-bench/RESULTADOS.md` recorded, runs to the end:
  no `VK_ERROR_DEVICE_LOST`, no `fault notification`, all eight phases
  including `3d_shadow_800` (1600 draws) and `3d_omni_16`. 60 fps in the
  three cheap 3D phases, 30 at `3d_cubes_2000`, which is where it was.

## Godot's Forward+ hang is one draw, and it is the scene shader's colour draw

Measured on GM20B on 2026-09-02 with `bench_fp_min` — Forward+, one cube,
one phase — in debug-synchronous mode, with the hang recorder on
(`HORIZON_GPU_SYNC=1 HORIZON_GPU_LOG=3 HORIZON_GPU_HANG_SNAPSHOT=1`).

The channel dies on the third submit of the first 3D frame, as before.
The recorder names the span:

```
frontend: breadcrumb=15 submit=8 kind=work span=0 phase=before-span
          push=[0x11174000,0x11183fd0)
```

16372 dwords, which is one whole command-buffer chunk. `NVK_HORIZON_PUSH_SPLIT=64`
(patch 0062) cuts the same push into 238 spans and names one of them:

```
frontend: breadcrumb=893 submit=8 kind=work span=169 phase=before-span
          push=[0x1118126c,0x11181378)
```

67 dwords at **offset 0x349b** of that chunk. `NVK_DEBUG=push_sync` dumps
the chunk's methods, and 0x349b..0x34dd is `SET_RT_LAYER`, the constant
buffer binds, `CALL_MME_MACRO(0)` (`NVK_MME_SELECT_CB0`) and
**`CALL_MME_MACRO(12)` — `NVK_MME_DRAW_INDEXED`, 0x24 = 36 indices**,
under `SET_WRITE_MASK 0xf`, `SET_CT_SELECT.TARGET_COUNT 3` and
`SET_PIPELINE_REGISTER_COUNT(5) = 0x70`. That is the cube's colour draw.

**The same cube's depth-prepass draw completes.** It is the other
`DRAW_INDEXED 0x24` in the same push, at offset 0x32c7 under
`SET_WRITE_MASK 0x0` and a 24-GPR fragment shader, and the breadcrumb
walked past it.

So the failure is one draw, and what distinguishes it from the draw that
works, in the same pushbuffer with the same geometry, is its fragment
shader and its colour-target state.

## The colour draw is a hang, not a slow draw

Same run at `rendering/scaling_3d/scale=0.25` — the 3D render target is
320x180, so the cube covers about forty fragments instead of six hundred.
The channel dies in **span 169 of the same submit**, at the same offset.

Sixteen times less fragment work changes nothing, so the cost is not per
fragment and the draw is not merely slow. That retires the 2026-08-23
reading of this failure as "a fragment shader running ~8500x too slow":
whatever happens, it happens once per draw and it does not finish.

## Zcull is not the Forward+ hang

`NVK_HORIZON_ZCULL=0` (patch 0059) turns the capability off at the
physical device, and the log shows it took: `nvkmd_horizon: Zcull is not
advertised (NVK_HORIZON_ZCULL=0)` and `zcull=off` on all three channels.
The hang is identical — same submit, same span, same
`fault notification 8 (fifo idle timeout)`, `error_type=4`.

This is the single-variable version of a test that had been run
conflated: switching off the depth prepass was read as switching off
Zcull, and it is not — the colour pass clears depth either way, so
`nvk_CmdBeginRendering` takes the same Zcull path.

Patch 0060's Zcull context bind also works and is not the fix: the same
runs log `channel ...: up, syncpt=27 initial=13775 zcull=bound`, and the
hang is unchanged. That closes section 1 of
`docs/PENDING-HARDWARE-RUNS.md` only for "does the bind succeed"; the
`t_vk_zcull` A/B it asks for has still not been run.

## Shader local memory works, and the Forward+ shader is the only user of it

`NAK_DEBUG=crsinfo` with `MESA_SHADER_CACHE_DISABLE=true` reports every
shader NAK compiles. Of the **forty-two** in Godot's Forward+ process,
exactly one has a non-zero convergence-stack size:

```
NAK crs: stage=Fragment instrs=3932 gprs=112 max_crs_depth=13 crs_size=1024
```

Every other shader is `crs_size=0`. `crs_size` is the whole of the
per-warp shader local memory the queue then programs — the driver logs
`SLM va=0x111b5000 allocation=0x20000 bytes_per_warp=0x400
bytes_per_tpc=0x10000 gpc_count=1 tpc_count=2 mp_per_tpc=1
max_warps_per_mp=64`, and 0x400 is that shader's 1024 bytes. Nothing in
the process spills registers.

That correlation is not the cause. `NAK_DEBUG=crsbig` gives **every**
shader 4096 bytes of convergence stack, so shader local memory is
allocated and programmed at device creation (`bytes_per_warp=0x1000`,
`allocation=0x80000`) and every draw in the frame uses it. All the 2D and
HUD draws render, and the hang is in the same span 169 of the same draw.

`t_vk_crsfrag` closes it from the other side: **PASS 105/105**, four cases
of twelve nesting levels around a loop in a *fragment* shader, including a
checkerboard where the two lanes of every quad take different depths, 128
of 256 texels twelve levels deep. A fragment shader whose convergence
stack does not fit the sixteen on-chip slots works on this chip.

Also measured while chasing this: **nvgpu ignores the alignment argument
of a non-fixed `ALLOC_SPACE`.** nouveau allocates the same buffer with
`nouveau_bo_new(dev, domain, 1 << 17, size, ...)`
(`nvc0_screen_resize_tls_area`), so the reference driver's local memory
base is 128 KiB aligned; asking `nvkmd_dev_alloc_mem` for 0x20000 here
returns the same 4 KiB-aligned `0x111b5000` it returned without the
request. It does not matter — `t_vk_crsfrag` passes against that base —
but a future reader should not spend a build finding out.

## Three render targets with two disabled is not the Forward+ hang either

`t_vk_mrt`, **PASS 96/96** on GM20B. Three cases of the same fragment
shader and the same one image read back: one colour target; three targets
all real; and three targets of which two have no image view, which reaches
the hardware as `SET_CT_SELECT.TARGET_COUNT 3` with
`SET_COLOR_TARGET_FORMAT(1)` and `(2)` = `DISABLED`. All 256/256 texels
right in every case.

That was the last of the five pipeline-state differences the 2026-08-23
Forward+/Mobile dump found still untested on its own.

## A dock cannot make a swapchain suboptimal here

The transition patch `0040` was written for cannot be delivered on this
platform. This is the single most expensive thing on this page to
re-measure, because getting it wrong costs a person standing at a
console plugging it in and out, so it is written out in full.

`t_dock`, docking and undocking three times with a buffer queued every
50 ms so every source was being refreshed:

- `appletGetOperationMode()` moves, and `AppletHookType_OnOperationMode`
  fires. **A dock does reach this process.**
- `nwindowGetDimensions()` and `NWindow::default_*` — what
  `wsi_horizon_get_extent` reads — never move. 1280x720 throughout. The
  compositor scales this layer to the television rather than resizing
  it, so the BufferQueue has nothing to report.
- `appletGetDefaultDisplayResolution()` moves to 1920x1080 **and back
  to 1280x720 while still docked**, about 350 ms after the mode and
  2.8 s before it fell again. It is a transient around the transition,
  not a state.

`t_vk_swapchain` section G then ran with the console docked for the
whole 90-second window — `appletGetOperationMode() went 1 -> 1` — and
the surface reported **1280x720 from beginning to end**, sampled inside
the loop rather than once at the end.

So no source available to this process reports a stable docked output
size. A swapchain cannot be told the output changed, because as far as
everything it can read is concerned, it did not.

**What was tried and reverted.** Patch `0057` made
`wsi_horizon_get_extent` read `appletGetDefaultDisplayResolution()`, and
`wsi_horizon_extent_changed` compare that against the size at creation.
It passed every undocked regression — `t_vk_suboptimal` 273/273 twice,
`t_vk_wsi_mt` 71/71, `t_vk_present_draw` 183/183, `t_vk_immediate`
442/442, `t_vk_swapchain` 144/144 — and it is still wrong, because the
value it reads is a 2.8-second excursion. It would make
`VkSurfaceCapabilitiesKHR::currentExtent` flicker to 1920x1080 around
every dock, so an application querying inside that window would build a
1080p swapchain for a layer that is 720p and stays 720p. A stable wrong
answer beats an unstable one; `default_*` at least matches the layer.

**This explains a line patch 0040 has carried since August**:
"VK_SUBOPTIMAL_KHR HAS NEVER BEEN RETURNED ON HARDWARE." It was written
as coverage nobody had managed to get. It is not coverage. There is
nothing to cover.

**If it is ever wanted**, what it needs is a source that is stable while
docked — `viGetDisplayResolution` on a display handle this process does
not have, or an applet path nobody here has found. Not a longer wait
with a hand on the console: three runs have now had that.

---

## The Forward+ hang is not the loop, the stack, the fragment stage or the kill

Four tests built to reproduce Godot 4.1's Forward+ hang outside Godot all
pass on GM20B (2026-09-01, build `1b49911-dirty mesa:5554de6-dirty`, logs
in `sdmc:/horizon_gpu_tests/`):

| Test | Result | What it takes off the table |
|---|---|---|
| `t_vk_loop` | PASS 38/38 | a loop whose trip count comes from memory, including every bound 0 |
| `t_vk_loop2` | PASS 38/38 | the cluster loops' exact bound idiom: packed min/max, prmt-style extract, bcsel |
| `t_vk_crs` | PASS 35/35 | twelve nesting levels around a loop — the unbacked convergence stack |
| `t_vk_kill` | PASS 83/83 | a fragment loop, and a fragment loop entered after half the lanes are killed |

Together with the passes recorded elsewhere, **the whole "it is the control
flow, the compiler, the fragment stage or the kill" family is excluded on
hardware.** `t_vk_kill` is the one that closes it: its three cases are
256/256 texels right, including "bound 4, half the lanes killed first"
with 128 texels killed. A counted fragment loop with a kill in front of it
works on this chip.

**This contradicts a reading of the Godot measurement, and the Godot
reading is the one that was wrong.** Bisecting inside the engine on
2026-08-29 produced "with the bound forced to a compile-time four, the body
replaced by an increment, and the whole induction replaced by a plain
counted loop, it still hung", which was taken to mean the loop itself hangs
the channel. It cannot mean that: the loop it was reduced to is the loop
`t_vk_kill` runs. Whatever still hung was not the shape that was left.

**Do not re-measure this by shrinking Godot's shader further.** Painting a
value into `frag_color` to inspect it changes register allocation enough to
hide the bug, so a reduction inside the engine cannot be trusted to still
contain the fault it started with. The way to ask a question about this
hang is a test in `tests/`, which is why these four exist.

What survives is the reading of descriptor set 1 from the fragment stage.
Not from a missing cache flush: the descriptor writer does clean its dirty
range at `nvk_descriptor_set.c` `nvk_descriptor_writer_finish`, at
`set->mem_offset_B + dirty_start` aligned both ends to `nc_atom_size_B`,
and on aarch64 `util_has_cache_ops()` is true and `cache_ops_aarch64.c`
(`dc cvac` + `dsb sy`) is the implementation compiled in, not the null stub.

## A fragment shader reads descriptor set 1 correctly

`t_vk_set1`, PASS 170/170 on GM20B (2026-09-01). Four numbers per texel --
an SSBO from set 0 as the control, an SSBO and a UBO from set 1, and the
set-1 SSBO's runtime-array length -- all right on 256/256 texels in five
cases: the two sets bound in one `vkCmdBindDescriptorSets`, in two calls in
Godot's order, in two calls in the other order, and then the same read after
the **GPU** wrote the set-1 SSBO in the same command buffer, once with
`vkCmdFillBuffer` and once with `vkCmdCopyBuffer` -- which is what Godot's
`buffer_clear` and `buffer_update` are.

So the read of set 1 from the fragment stage is not broken on this chip, and
neither is the write side: a transfer write with a TRANSFER_WRITE to
SHADER_READ barrier is visible to the draw that follows it. **The hypothesis
that Godot's Forward+ shader hangs because it reads a wrong descriptor is
excluded.**

## The convergence-stack fix is already in the Godot build that fails

`crs_size()` in `sm50.rs` counts one stack slot per nesting level and
reserves nothing below 16, where nouveau counts two for the same field. The
working tree carries a fix that multiplies by two, and it is tempting to
conclude that Godot's `max_crs_depth = 13` shader therefore runs on the
16-entry on-chip stack with nothing behind it.

**It does not, because that fix is already compiled into the binary that
hangs.** `bin/godot.nx.template_debug.arm64.elf` (2026-08-29) contains the
`crsbig` and `crsinfo` strings the fix adds, as does the
`libnouveau_rust_runtime.a` installed in the portlibs prefix it linked
against. Whatever hangs the Forward+ draw, it is not the size of the
convergence stack.

Recorded because it was re-derived and nearly re-tested at the cost of a
full Godot rebuild. Two traps made it look unproven: `build/mesa-nvk`'s
`libnak.a` is the **C helpers** archive and is not where `sm50.rs` lands
(that is `libnak_rs.rlib`, bundled into `libnouveau_rust_runtime.a`), so its
old mtime means nothing; and **`strings` does not exist on the Windows
development box**, so a `strings ... | grep -c` check reports 0 matches for
every file whether or not the string is there. `grep -ac <string> <archive>`
is the check that works.

## Godot's Forward+ hang is not the loop, and not the kill either

Three runs on 2026-09-01, all ending in `fault notification 8 (fifo idle
timeout)` on the first 3D phase:

| Build | What it removes | Result |
|---|---|---|
| `bench_fp_dbg` | nothing — the stock Forward+ bench | 2D phases 1-3 fine (60, 20, 58 fps); dies in phase 4, `3d_cubes_200` |
| `probe13` | all seven `discard;` sites in the scene shader | dies, on a single cube |
| `probe12` | the loop itself — a literal bound, so NIR unrolls it and no back edge is left | dies, on a single cube |

`probe13` kills the hypothesis that a `discard` leaves the warp and a later
loop waits forever for lanes that no longer exist. `t_vk_kill` had already
said the same from the other side: a fragment loop entered after half the
lanes are killed is right on 256/256 texels.

**`probe12` is the one that matters, and it was written as a control.** With
the loop unrolled and no back edge anywhere, it still hangs. So the loop is
not the cause, and the whole probe series that reduced it — bound, body,
induction, range read, subgroup reduce — was not measuring the loop.

**What it was measuring is dead-code elimination.** Every "fast" reading in
that series came from a build whose loop is never entered, and a loop that
never runs makes its accumulator a constant, which lets DCE take the lighting
that consumes it with it. The same trap is already recorded for
instrumentation: overriding `frag_color` to paint a value DCE's the lighting
and the cluster loops, and a build that does it runs at 60 fps for the wrong
reason. "The loop is free when it is not entered" and "the shader is mostly
gone when it is not entered" are the same measurement.

So a Forward+ fragment shader that actually does its work hangs the channel,
and nothing yet distinguishes which part of that work does it. What is
excluded on hardware: the loop, the back edge, the kill, the convergence
stack, the descriptor read (`t_vk_set1`), every optional NAK pass, the
caches and barriers, and the attachment-less cluster-builder pass.

The one measurement in that series worth keeping is the NAK dump of the
scene fragment shader: **4244 instructions, 104 GPRs, 16 warps/SM**, compiled
with `USE_SUBGROUPS`. 104 GPRs is what holds occupancy down to 16 of 64
warps, and register pressure is the one property that both a loop and an
added `frag_color` paint change.

## The Forward+ hang is one command buffer, and it is not size, time or a race

Measured 2026-09-01 by putting the driver in debug-synchronous mode
(`HORIZON_GPU_SYNC=1`, and `HORIZON_GPU_LOG=3` because the line it prints
is INFO and the default level is WARN). Every submit then waits for its own
fence, so each one is timed and sized on its own:

```
-- phase 4/8: 3d_cubes_200
[sync-mode] fence 27:12057 wait=ok      339 us dw=37
[sync-mode] fence 27:12058 wait=ok      139 us dw=72
[sync-mode] fence 27:12059 wait=timeout 2000099 us dw=21406
channel: fault notification 8 (fifo idle timeout) — marking lost
```

**One command buffer, the third of the first 3D frame, with every submit
before it already retired.** That is the whole failure, and it excludes
three families at once:

- **Not a race between submits.** Debug-synchronous waits for each one; the
  GPU is idle when this buffer is pushed and it still never retires.
- **Not a size limit.** 1716 submits went through that channel in the same
  run and the largest that completed was **87099 dwords**, four times the
  21406 that hangs. The 2D phases push `dw=85219` every frame.
- **Not a time budget.** The same hang happens with
  `rendering/scaling_3d/scale=0.25`, which is sixteen times fewer 3D
  fragments. (`window/size/viewport_*` does **not** work for this on NX —
  the display server pins the window to 1280x720, and a run that changes it
  produces byte-identical 2D numbers, which is how to tell it was ignored.)

So the engine chokes on the contents of that one buffer, whatever the load.
Together with `t_vk_loop`/`t_vk_loop2`/`t_vk_crs`/`t_vk_kill`/`t_vk_set1`
passing and with `probe12` and `probe13`, nothing about the fragment shader
explains it: not the loop, the back edge, the kill, the convergence stack,
the descriptors, or the instruction latencies.

**nvgpu's error record was read, and it names a timeout.**
`nvGpuChannelGetErrorInfo()` returns `NvError` — a type and 31 words, where a
graphics exception keeps the class, the method and the offset the engine
stopped on. On this fault it answers `type=0x00000004` with **all 31 words
zero**.

That type is not "unclassified": switchbrew documents the enumeration for
`NVGPU_IOCTL_CHANNEL_GET_ERROR_INFO`, which is exclusive to this platform, as
`0=no_error, 1=mmu_error, 2=gr_error, 3=pbdma_error, 4=timeout`. So the
kernel positively classified this death as a **timeout**, and by the same
token as *not* an MMU error, *not* a graphics exception and *not* a PBDMA
error. The empty info block agrees: `type == 2` is the case with a documented
payload — intr_value, addr, data_hi, data_lo, class_num — and this is not
that case. Nothing in the command buffer was rejected as illegal, malformed
or out of bounds. The engine accepted the work and never finished it.

That is the last thing the kernel knows, and it agrees with the
debug-synchronous picture.

**The obvious next number is not reachable on this console.** Where the
GPFIFO GET pointer stopped would say how far into those 21406 dwords the
engine got, and it lives in the channel's USERD page. The only ioctl that
hands a process the GPU VA of its own USERD is
`NVGPU_IOCTL_CHANNEL_SETUP_BIND` (`userd_gpu_va`, `usermode_mmio_gpu_va`),
and switchbrew marks it `[S2]` — Switch 2 only, alongside `CHANNEL_OPEN` and
the `nvhost-tsg-gpu` device. This console's channel path is
`ALLOC_GPFIFO_EX2` + `SUBMIT_GPFIFO2`, and neither returns a USERD address;
reading PBDMA or CCSR registers directly needs the `GpuDebug` permission,
which an application does not have. Progress inside a pushbuffer therefore
has to be measured by something the pushbuffer itself writes to memory, not
by asking the kernel where it got to.

## The series' "NOT RUN ON A CONSOLE" lines are frozen at their writing date

A Mesa patch's message records what had been measured *when it was
written*, and it is never rewritten afterwards: a patch is identified by
its subject and its diff, so editing one to refresh a claim is exactly
the divergence the applier exists to report. Four patches therefore
carry statements that were true in August and are no longer the current
state. **This file is where the current state lives**, not the patch.

- **`0035`** (a wait on this channel's own syncpoint) says the Phase 5
  suite "is part of what has to be re-run for it, not just the swapchain
  tests. NOT RUN ON A CONSOLE." That re-run has happened: all seventeen
  `t_vk_*` tests pass on hardware on the current series.
- **`0038`** (`VK_NN_vi_surface`) says "NOT RUN ON A CONSOLE." It has
  run. `vkCreateViSurfaceNN` is how every surface on this platform is
  created, and the five tests that create one all pass — `t_vk_swapchain`
  143/143 and `t_vk_suboptimal` 273/273 among them, both measured with
  the console docked.
- **`0040`** says "VK_SUBOPTIMAL_KHR HAS NEVER BEEN RETURNED ON
  HARDWARE." Still literally true, and no longer a gap in coverage: see
  the section above. There is nothing to cover.
- **`0054`** defers "the display changing under a *scaled* swapchain" to
  `docs/PENDING-VERIFICATION.md`, a file that no longer exists. The
  question was settled: `t_vk_swapchain` section G ran docked for its
  whole 90-second window and the surface never moved off 1280x720.

## Measured 2026-08-24

### Docked is a different machine, and two tests had to learn it

The suite had only ever run handheld. Running it in the dock found two
differences that are the compositor's and the clocks', not this
driver's:

- **The compositor attaches no release fence when docked.** Handheld,
  87 of 90 dequeues carried one, the first on syncpoint 103; docked, 0
  of 90 — while still presenting all 90 frames at the refresh, so the
  buffers were being released without a fence attached. A producer must
  cope with that, because the queue is allowed not to give one.
  `t_nwindow` reports the count and no longer fails on it.
- **Docking raises the clocks enough to stop the buffer-count
  experiment running.** The bursty-load check compares two buffers
  against three and wants two to be 10% slower. Handheld the two-buffer
  mean ran about 25 ms against a 16.7 ms refresh; docked, both counts
  sat on the refresh — 16944 us against 16851, with 45 of 89 intervals
  overrunning either way. Nothing regressed: the producer stopped being
  the bottleneck. `t_nwindow` and `t_vk_swapchain` now make that
  comparison only when two buffers actually fall behind the display.

- **Sparse residency works, through the whole stack.** `t_sparse` asked
  the address space (an unbound page swallows a write; unbinding a bound
  block puts that state back) and `t_vk_sparse` asked the same three
  questions through `vkQueueBindSparse` — PASS 74/74, block size
  0x20000, 0 of 32768 words wrong on the readback. `has_sparse` is true
  (patches `0055`, `0056`) and all seventeen Vulkan tests pass with the
  bind context it makes NVK ask for. Two of the eight features it gates
  have been exercised: `sparseBinding` and `sparseResidencyBuffer`.
- **Sparse exists only in big pages.** A reservation with
  `NvAllocSpaceFlags_Sparse` and a 0x1000 page size is refused —
  `0x0000275c`, `LibnxNvidiaError_IoctlFailed` — where the same call at
  `as_big_page_size` is accepted. `NVKMD_VA_SPARSE` forces the big-page
  half regardless of the PTE kind because of it.
- **A query pool's reset and timestamp belong in one submission.** Six
  of six after the `mem.c` cache fix; the two-submit split that was
  working around it is gone.
- **A dock reaches the process but not the layer.** See the section at the top of this file — the
  operation mode and its applet hook both move,
  `appletGetDefaultDisplayResolution()` follows to 1920x1080, and
  `NWindow::default_*` never moves at all.

- **`gpu_va_bit_count` is 40**, and `t_init` now fails device creation if
  a chip reports anything else, because a truncated GPU address is a
  valid address the GPU will write to.
- **`NVHOST_IOCTL_CTRL_SYNCPT_INCR` does not exist here.** `nv=0x275c` —
  `Module_LibnxNvidia`, `LibnxNvidiaError_IoctlFailed`. There is no
  CPU-side syncpoint increment on this platform, so a dedicated
  syncpoint would not help either: what is missing is the increment, not
  the ownership of the counter. `nvk_horizon_sync_signal` keeps the
  behaviour it has, and Fase C steps 3-4 are closed.
- **32 concurrent `nvFenceWait` calls all arm**, and 32 of them cost one
  wait's wall time (301 ms against 300 ms), so the platform does not
  serialise them. No ceiling was found below `MAX_THREADS`.
- **`nvFenceWait` reports a timeout as `0x00000d5c`** —
  `Module_LibnxNvidia`, `LibnxNvidiaError_Timeout` — and not as
  `KERNELRESULT(TimedOut)`. `horizon/sync/nv_wait.h` is the one place
  that decides what the Result means.
- **A threshold past the syncpoint's maximum answers success in zero
  milliseconds.** nvhost calls such a threshold expired rather than
  block on an increment nothing will make. Neither wait loop treats a
  chunk that did not block as its pulse any more.
- **The GPU timer is 1/614.4 MHz = 1.627604 ns per tick**, measured to
  4-10 ppm across three windows and three runs, and 614.4 MHz is 32x the
  19.2 MHz reference `armGetSystemTick` counts. The clock
  `vkCmdWriteTimestamp` records is the same domain. `timestampPeriod`
  was `1.0f` and is now this.
  - **Every GPU time in this repository measured before 2026-08-24 is
    that factor low, and this is arithmetic on the line above rather
    than a new measurement.** Godot multiplies a timestamp difference by
    `timestampPeriod` (`RenderingDeviceDriverVulkan::timestamp_query_
    result_to_time`), so with the old `1.0f` it reported raw ticks as
    nanoseconds. What that touches, and it is the whole list: patch
    `0048`'s "2000 cubes 12.99 -> 12.94 ms", which is 21.14 -> 21.06 ms
    in the units it claimed; and `0049`'s "the GPU had 0.18 ms of work
    to do", which is 0.29 ms. Neither conclusion changes — `0048`'s is a
    difference and `0049`'s is an order of magnitude — but 2000 cubes
    turns out to spend about 21 ms of its 33 ms frame on the GPU, which
    a reader comparing it with a post-`0053` number would otherwise get
    wrong in the other direction. **Do not apply the factor again**: any
    number from a build carrying `0053` is already in nanoseconds, and
    CPU render times were never affected because they come from the OS
    clock.
- **The BufferQueue keeps 0 buffers for its consumer**, before and after
  registration, so patch 0052's `minUndequeued + 1` is clamped up to
  `WSI_HORIZON_MIN_IMAGES` and the number it publishes is unchanged.
- **44 GPFIFO channels can be open at once**, the refusal being libnx's
  nv session transfer memory rather than the kernel.
- **`t_fault`: the console survives an MMU fault; the process does not.**
  Notifier type 31, channel marked lost, teardown clean, and the GPU
  still works in-process afterwards — but the atexit marker is never
  written, so the process is killed during exit, after the log is
  closed. Recovering needs one press of A on the error dialog and a
  relaunch, not a power cycle.

## Measured earlier, and still true

- `NvMultiFence` holds at most **4** fences.
- A GOB is **64 bytes × 8 rows = 512 bytes**; a block is
  `512 << block_height_log2` bytes and covers `8 << block_height_log2`
  rows.
- `HORIZON_GPU_PTE_KIND_GENERIC_16BX2 = 0xfe` is the kind the display
  block reads.
- `VK_FORMAT_R8G8B8A8_UNORM → PIXEL_FORMAT_RGBA_8888 →
  NvColorFormat_A8B8G8R8`, byte order inverted and correct.
- `HORIZON_GPU_SMALL_PAGE_SIZE = 0x1000`.
- **The PTE kind belongs on the GPU mapping, not on the allocation.**
  `mem.c` creates every `NvMap` with `NvKind_Pitch` and `vm.c` passes
  the real kind to `MapBufferEx`. This is not obvious and it is right.
- **A big-page mapping needs big-page-aligned backing.** `MapBufferEx`
  accepts a 4 KiB-aligned object with `page=0x20000`, returns the
  requested VA, and the GPU's writes to it go nowhere — no fault, no
  error, no data. `horizon_gpu_vm_map` refuses that pairing now.
- **A fresh allocation's cache lines must be flushed before the GPU
  writes it.** `aligned_alloc` plus the zero-fill leaves every line
  dirty, and `dc civac` cleans before invalidating — so an invalidate
  meant to see the GPU's write instead destroys it. Harmless for every
  object the CPU writes first, fatal for the first one it does not.
- Querying `big_page_size` instead of hardcoding it (`device.c:138`).
