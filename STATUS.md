# STATUS

**Last updated:** 2026-08-04
**Branch:** `claude/phase-5-offscreen-rendering-vnl6q9`

---

## Current state — read this first

*Everything below this section is the working record: dated, append-mostly, and
long. This block is the state itself, and it is the part that must be true.*

| | |
|---|---|
| **Phase** | **5 COMPLETE, 2026-08-04.** All nine items met on hardware, each by CPU readback of the result. Item 9's extra requirement met with eight submits in flight, no CPU wait between them |
| **What runs on a Switch** | Transfers (**202/202**), a compute shader compiled by NAK (**37/37**), off-screen images and clears (**72/72**), a rasterised triangle with interpolated vertex colours (**84/84**), **sampled textures with mip levels and bilinear filtering** (**1685/1685**), the depth test with the depth buffer read back (**66/66**), twelve colour formats (**282/282**), eight submits outstanding at once (**287/287**), the mandatory sequence (**62/62**). All 16 `horizon/` tests pass on console |
| **Next concrete task** | Phase 6. The exit crash is characterised, its two experiments are built, and running them costs a reboot — the owner's call, not a blocker |
| **The debt batch** | Seven of seven back. Six PASS, and `t_fault` 14/15 whose one failure was a real defect it was built to find. `t_vulkan` 62/62 · `t_threads` **67/67** and `t_ostime` **43/43**, Phase 3's debt closed on a console at last · `t_vk_caps` **52/52**, patch 0046's gating right in both directions and patch 0045's `alloc_tiled_mem` executed · `t_pbsize` **69/69**, D15's number · `t_display` **3/3**, console-less reporting works · `t_fault` **20/20** after the latch it earned, plus an exit crash it also earned |
| **Known failures** | **`t_fault` crashes the console on exit.** All 15 checks pass, the log is written and closed, and pressing + to leave takes the system down — after everything the test reports, including a clean teardown. A real application that takes a GPU fault would hit the same path. Run 3 ruled out the worse cause: the nv session survives a fault completely, and a fresh device, channel and submit all work afterwards. Whether the settle changed the crash is the open half. **One unexplained single occurrence stays on the record**: `t_vk_texture` run 1 returned zeros for texel rows 4 and 5 of an 8x8 tiled source, and 32 subsequent attempts under the same configuration have not reproduced it. Every mechanism that could produce it has been excluded by a run that would have shown it; intermittency has not |
| **Open, not blocking** | The L2 writeback is unconditional, one per submit |
| **Open decisions** | **D7 only**, and it is written up and waiting for a person to file it. D15 and D17 are closed |
| **Never verified on hardware** | Nothing outstanding: every measurement this project owed itself has been made on a console. What is not *explained* is `t_fault`'s exit crash |


---

## The debt batch on hardware (2026-08-04)

**Class: hardware (HW).** Six logs came back, every one a pass. The
seventh, `t_fault`, produced no log at all — see below.

### Patch 0045 executed, three weeks after it was written

    ok  VK_EXT_image_drm_format_modifier is advertised
    ok  the driver offers 7 DRM format modifier(s) for R8G8B8A8_UNORM
    ok  DRM_FORMAT_MOD_LINEAR can be a colour attachment
    ok  every texel of the linear image holds what the tiled shadow
        rendered: 4096/4096 words are 0xff996633 (all of them)

`nvkmd_dev_alloc_tiled_mem` was a NULL function pointer until patch
0045, and then nothing reached it, because a `VK_IMAGE_TILING_LINEAR`
image can never be a colour attachment in NVK — upstream, every chip.
The path that does reach it is the one patch 0045 opened itself:
setting `has_alloc_tiled` is what put `EXT_image_drm_format_modifier`
in the extension list, and a `DRM_FORMAT_MOD_LINEAR` image *is*
allowed to be an attachment. Rendering into one makes NVK allocate a
tiled shadow through `alloc_tiled_mem`, draw into that, and copy the
result back into the linear layout when the pass ends. All 4096 texels
came back right, so the whole round trip works.

The device offers **seven** modifiers for R8G8B8A8_UNORM —
`0x3000000000fe010` through `…15`, and `0x0` — all with the same
feature word `0x1dd83`. Worth having written down before Phase 6 asks
which layout an `nwindow` buffer wants.

### Patch 0046's gating, right in both directions

    ok  VK_KHR_external_memory_fd is not advertised
    ok  VK_EXT_external_memory_dma_buf is not advertised
    ok  VK_EXT_map_memory_placed is not advertised
    ok  VK_EXT_image_drm_format_modifier IS advertised
    ok  no sparse feature is advertised

Three absent, one present, and patch 0029's sparse gating with them. A
gate that only ever says no is indistinguishable from an extension list
that was never built, which is why the fourth row is there.

### D15's number: there is no limit to bound

    ok    32 dwords: the release at the END of the entry ran
    …
    ok  524288 dwords: the release at the END of the entry ran
    note D15: every rung up to 524288 dwords (2048 KiB) executed to its
         last dword; no entry-size limit was found in this range

Verified by the release at the *end* of each entry, not by the submit
being accepted — the interesting failure is an entry the channel takes
and then does not finish. Nothing is added to `horizon_gpu_submit`, and
nothing is paid at channel creation.

One defect of my own in that test, fixed afterwards: the payload was
`0x5a5a0000 | (dwords & 0xffff)`, and both 131072 and 524288 are zero
in the low sixteen bits, so the two largest rungs — the interesting
ones — carried the same payload. The check was still sound, because the
target is zeroed and flushed before every rung, so a rung that did not
run reads zero rather than a neighbour's value. But a payload that says
it names a rung should name it, and it now does, by position.

### Phase 3's debt, closed on a console

`t_threads` **67/67** and `t_ostime` **43/43** — the same numbers they
gave on the emulator, now from the hardware they were always owed to.
`os_time_get_nano` resolves to a **52 ns** step over 80066 samples, and
over a 100 ms sleep it differs from the ARM system counter by **2 ns**.

### Phase 6's harness half is done

`t_display` ran with no console and reported itself through the SD-card
log alone, which is the whole question:

    note this test owns the display: no console was started, and this
         file is the whole record
    ok   nwindowGetDimensions -> 0x00000000 (1280 x 720)

So a swapchain test can have the default window and still be reported.
The alternative — nxlink over the network — is not needed.

### `t_fault`: the fix fired, found the defect next to it, then found a worse one

**Run 1: 14/15. Run 2 with the latch: 15/15.**

    ok   the submit is accepted — the fault is the GPU's, not the kickoff's
    [horizon_gpu:E] channel 0xce51cd050: fault notification 31 (MMU fault) — marking lost
    ok   the wait does NOT report success for work that faulted
    ok   and it names the channel lost rather than timing out
    ok   the notifier recorded the fault (type=31 'MMU fault')
    ok   a submit to the lost channel is refused
    ok   the faulted channel tears down cleanly
    ok   the device tears down after a faulted channel

**The fence/notifier fix works.** It was written on 2026-08-04 because
`vkWaitForFences` had returned `VK_SUCCESS` for MMU-faulted work, and
nothing had faulted since. A deliberate fault reaches it: nvgpu's
recovery force-incremented the syncpoint exactly as predicted, the
notifier was consulted anyway, and the wait reported **channel lost**.
The fault type is **31, MMU fault**.

Run 1's single failure was a defect in `horizon/`, not in the test.
libnx's `nvGpuChannelGetErrorNotification` does a non-blocking
`eventWait` on the channel's error *event*, so **the first reader
consumes the notification** — and the first reader is
`channel_check_fault`, on the wait path. It logged the type and threw
it away, so a caller that received `HORIZON_GPU_ERR_CHANNEL_LOST` and
asked why was told "no error recorded", for a channel lost because of
one. `horizon_gpu_channel_get_error` now latches it. Run 2 says the
latch works.

### AND THEN THE REAL FINDING: `t_fault` crashes the console on exit

Reported by the owner, on every run: **every check passes, the log is
written and closed, and pressing + to leave takes the system down.**

This is worse than the thing the test was written to catch, and it is
not a test defect. It happens strictly after everything `t_fault`
reports — the channel, the reservation and the device all tear down
with `ok`. What is left between the last line of the log and the crash
is the `+` loop, `consoleExit`, and libnx's `__appExit`.

**Why it matters beyond this test.** A real Vulkan application on this
driver can take a GPU fault. If the process then brings the system down
when it exits, that is a defect anything shipping would hit, and it is
invisible to every test that does not fault on purpose.

### Run 3 answered half of it: the session is fine

    note settling for 2000 ms before teardown
    ok   the empty reservation is released after the fault
    ok   the faulted channel tears down cleanly
    ok   the device tears down after a faulted channel
    ok   after the fault: a second device opens
    ok   after the fault: a second channel opens
    ok   after the fault: a submit on the new channel is accepted
    ok   after the fault: the new channel's work completes
    ok   after the fault: THE GPU STILL WORKS IN THIS PROCESS (0x600d0001)

**20/20.** A whole second device, channel, submit and readback, all
correct, after a faulted channel had been torn down. So the `nv`
session survives an MMU fault completely: the process is *healthy* when
it reaches the exit path, and the crash is not a broken state being
carried forward while the test reported `ok`. That was the worse of the
two possibilities and it is ruled out.

`horizon_gpu_device_destroy` calls `nvFenceExit()` and `nvExit()`,
which libnx reference-counts, so the two create/destroy pairs in this
run are balanced and the first version — one device — had no
double-exit either.

**What is still open is the other half**, and the log cannot answer it
because the crash happens after the file is closed: did the 2-second
settle change anything? If it still crashes, the cause is in the exit
path itself rather than a race with nvgpu's recovery, and the next
experiment is an `atexit` marker to find which teardown step is the
last one reached. If it stopped crashing, the cause is the race, and
the fix is a bounded wait on something observable in
`horizon_gpu_channel_destroy` for a lost channel — never a sleep.

Asked of the owner rather than assumed.


### One thing about the Phase 3 debt, said rather than slipped in

`t_threads` and `t_ostime` link Mesa's own `libmesa_util.a` and
`libmesa_util_c11.a` rather than sources recompiled with flags of our
own, because the object under test has to be the object Mesa builds.
They were built for this batch against **`build/mesa-nvk`** — the full
NVK build — instead of the separate `build/mesa-probe` the default
points at, with `MESA_BUILD_DIR=build/mesa-nvk`.

That is not a shortcut. `build/mesa-nvk` is the Mesa the driver
actually uses, built with the same cross file and the same defines, so
the measurement describes the build that ships rather than a second one
kept alive only for these two tests. The default is left alone because
somebody who wants only the Phase 3 tests should not have to build the
whole Vulkan driver first.

## The working record

Everything above is the state. The dated record it was built from —
every phase's narrative, every review round, every hardware run and the
commit logs — lives in `docs/history/`, moved there verbatim by
`scripts/split-status.py` (decision D17) and in its original order:

| file | what is in it |
|---|---|
| `docs/history/phase-5.md` | Phase 5, off-screen rendering: the nine items, five hardware batches, and the texture failure that has not reproduced |
| `docs/history/phase-4.md` | Phase 4, `nvkmd_horizon`: the interface reading, the libc symbols, Rust, the build machine, and the first console runs |
| `docs/history/phase-4-continued.md` | Phase 4's second half: the degraded baseline, the never-executed-path audit, the push dump, the exit criterion, and the PR #6 review rounds |
| `docs/history/phase-3.md` | Phase 3, Mesa on Horizon: newlib gaps, physical memory, threads and timers, the TLS miscompile, and the PR #4 reviews |
| `docs/history/phase-2.md` | Phase 2, the toolchain |
| `docs/history/phase-1.md` | Phase 1, `horizon/` standalone, and its exit criteria |
| `docs/history/reviews-2026-07-26.md` | the two early review rounds |
| `docs/history/phase-4-narrative.md` | the step-by-step Phase 4 narrative that had been living inside the Current state block |
| `docs/history/commit-logs.md` | the per-phase commit logs |

`scripts/check-history-intact.sh` recomputes their digests against
`docs/history/MANIFEST.sha256`. History is evidence: appending to it is
a deliberate act that updates the manifest in the same commit, and any
other change shows up as a failing gate.

## Known failures / limitations

- **R6 (cache coherency) still unmeasured** — needs a GPU write primitive
  (inline-to-memory/DMA), deferred to the first GPU write (early
  Phase 4/5). Deliberate, recorded deviation.
- **Kickoff rejection code map** still unpopulated: no kickoff was ever
  rejected during the run (good), so BUSY-vs-other classification remains
  based on the libnx-side queue precheck only.
- The submit rollback paths touch libnx's public `NvGpuChannel` fields;
  correct against libnx as pinned in the nx-dev image.

## Pending decisions

| # | Decision | State |
|---|---|---|
| D1 | Literal reuse from GPL/AGPL reference | **no**; nothing copied |
| D4 | Switch available | **yes — closed.** Full run, confirmation re-run, and the Phase 4 hardware run all done |
| D2 | Mesa version to pin | **closed at Phase 2 start: `mesa-26.1.5`** @ `6a02618ccf6c5651ecb9cccbde571eb61fd73592` |
| D3 | Mesa checkout mechanism | **closed at Phase 2 start: script-fetched**, not a submodule |
| D5 | Cache policy per memory type | **closed by the hardware, and not the way it was framed** — one policy per advertised type: type 0 (`DEVICE_LOCAL HOST_VISIBLE HOST_CACHED`) is `HORIZON_GPU_MEM_CACHED` with nvkmd's own maintenance, which it performs because `util_has_cache_ops()` is true on AArch64; type 1 (`HOST_VISIBLE HOST_COHERENT`) is UNCACHED, see D14. The framing that was wrong: this row assumed the first GPU write would be made visible *by* CPU cache maintenance. R6 measured the opposite — the four-arm `t_gpuwrite` matrix failed identically with the CPU mapping cached and uncached, and what made the write appear was a GPU-side L2 writeback. The policy is real and still required; it was never what blocked the readback. Was: — blocked on R6 (first GPU write) |
| D6 | Timeline semaphores vs upload queue | **closed: advertise, by emulation (patch 0022)** — the runtime's `vk_sync_timeline` is registered over the binary syncpoint fence D11 settled on, so timelines are advertised without touching the upload queue. Was: — Phase 4. Original wording, `docs/known-risks.md:403`: advertise timeline semaphores, or fix the upload queue instead |
| D7 | Report the devkitA64 TLS miscompile upstream | **actionable, not closed** — the report is written out in `docs/devkita64-tls-report.md`: summary, four-line reproducer, both disassemblies, why it is worth fixing and how this tree works around it. It has not been filed, because this container cannot reach devkitPro's tracker and the finding belongs to whoever posts it. Filing it is a copy-and-paste; nothing here is blocked either way |
| D8 | Whether `CLOCK_MONOTONIC` here may be relied on as monotonic | **closed by design (items 6-10)** — it may not, so the absolute deadline is converted to a relative duration exactly once, at the start of the wait, and horizon_gpu waits on libnx's monotonic ticks. A date change cannot move a deadline that no longer exists. Was: — `t_ostime` measured `TIME_MONOTONIC` returning wall-clock time (epoch seconds), so it is the real-time clock. Monotonic across both measured intervals; a date change would step it. `vk_sync` waits take *absolute* timeouts built from `os_time_get_absolute_timeout`, so this has to be answered before the sync type is designed |
| D9 | `nvkmd` pdev/dev split: one `horizon_gpu_device` serving both, or GM20B facts queryable without a device | **closed by the hardware (items 1-2)** — the `nv` session is per process and the GM20B characteristics are only queryable once it is up, so there is no describing the device without opening it. The pdev owns the `horizon_gpu_device`; every `nvkmd_dev` shares it under a reference count. Was: — Phase 4 item 1. nouveau opens the render node twice (`_pdev.c:70`, `_dev.c:40`); Horizon's `nv` session is per process |
| D10 | The four chipset-derived `nv_device_info` fields (`sm`, `mp_per_tpc`, `max_warps_per_mp`, shared-memory sizes) | **closed: moved upstream (items 1-2)** — they are now in `src/nouveau/headers/nv_device_info_chipset.c`, next to the struct they fill, unchanged, and `nouveau_device.c` calls them. Was: — Phase 4 item 2. They are pure functions of the chipset living in `src/nouveau/winsys/nouveau_device.c`, which Horizon does not build: duplicate them into `nvkmd_horizon`, or move them upstream next to `nv_device_info.h`. They describe the chip, not the kernel driver |
| D11 | `vk_sync` type: Horizon-native over syncpoints, or the runtime's `vk_sync_timeline` emulation | **closed: native (items 6-10)** — a binary vk_sync over a channel fence, with the runtime's timeline emulation on top. The emulation needs a binary type underneath regardless, and a syncpoint fence is what a submit produces. Was: — Phase 4 item 8, and the largest single piece of the phase. The native route needs a CPU-side syncpoint increment (`nvioctlNvhostCtrl_SyncptIncr`) that `horizon_gpu` does not expose, and an owner for a syncpoint no channel created. Depends on D8 |
| D12 | Sparse binding: implement the bind context, or add a kmd capability and turn the feature off | **closed: the capability (patch 0029)** — `nvkmd_info` gains `has_sparse`; nouveau answers true, horizon false, and the nine sparse features plus `VK_QUEUE_SPARSE_BINDING_BIT` follow it. **The reason first recorded for this was wrong** and the correction stands with the decision: it was not that `NVGPU_AS` has no sparse reservation — `NvAllocSpaceFlags_Sparse` exists. What is missing is *partial* unbind, which is what sparse residency needs. Was: — Phase 4 item 6. `sparseBinding` is `cls_eng3d >= MAXWELL_B` and GM20B's queried 3D class is `0xb197` = MAXWELL_B, so NVK advertises it on this chip unless the condition changes |
| D14 | An uncached memory policy in `horizon/` | **closed on hardware — `t_uncached` PASS 19/19** (`docs/hw-logs/t_uncached.log`). `horizon/memory/mem.c` implements `HORIZON_GPU_MEM_UNCACHED` with `svcSetMemoryAttribute(MemAttr_IsUncached)` over the rounded range, undone on every error path and at destroy; patch 0030 maps `NVKMD_MEM_COHERENT` onto it. All three unknowns the test separates were answered on console: the kernel accepts our heap allocations, the resulting mapping is Normal-NC (ordinary loads, stores and `memcpy` work — Device memory would have faulted), and the GPU reads an un-flushed command list written through it. Was: — open, raised with the owner; does NOT block Phase 4. `horizon_gpu` offers only `HORIZON_GPU_MEM_CACHED` — no longer true, and this row said so long after the code and the log had landed |
| D13 | Where the single `#[global_allocator]` and `#[panic_handler]` live | **closed by measurement (step 4)** — they cannot live in both NAK and NIL: two `no_std` Rust staticlibs fail to link with `multiple definition of `__rust_alloc`` and four more. NAK and NIL become rlibs; one new staticlib links both and carries the pair |
| D16 | `vk_sync_wait` on a sync that was never submitted: return `VK_TIMEOUT` at once, or block until the deadline | **closed: block (patch 0044), verified on hardware — `vkWaitForFences(200 ms)` on a never-submitted fence returned `VK_TIMEOUT` after 200 ms, where the old code answered in microseconds (`docs/hw-logs/t_vulkan-run6-D16-PASS.log`, PASS 60/60)** — owner said address it now. A `mtx_t` and `cnd_t` in `nvk_horizon_sync`, broadcast from `signal()`, `set_fence()` and `move()` — the three transitions that can release a waiter — and deliberately not from `reset()`, which makes the object *less* reachable. The condvar wait is chunked at 100 ms rather than handed the caller's deadline, because `cnd_timedwait` takes an absolute `TIME_UTC` deadline and D8 measured that clock to be the real-time clock: chunking bounds how far a date change can move a wait, and costs no wake-up latency because a broadcast ends the chunk immediately. `move()` needed care of its own — it copies the payload struct, which would have copied the destination's live mutex and condvar over with the source's, including one held on that line. Primitives verified on console rather than assumed (`t_threads`, 67/67, exercises `cnd_wait`/`signal`/`broadcast`/`timedwait`). `t_vulkan` gained the check that discriminates the fix from the bug. **Not yet run on hardware.** Was: — open, raised with the owner (Codex P1, PR #6) — `nvk_horizon_sync_wait` returns `VK_TIMEOUT` immediately when the state is not PENDING, including for `OS_TIMEOUT_INFINITE`. The finding is correct: the sync type advertises CPU wait *and* CPU signal, so another thread is permitted to submit or signal while this one waits, and Vulkan allows waiting on a fence no queue has touched yet — it must block, not report a timeout that has not happened. Fixing it properly means a mutex and condition variable inside `nvk_horizon_sync`, signalled from both the signal path and the submit path: a change to the sync object's shape and the first threading primitive in `nvkmd_horizon`, which is why it is a decision and not a commit. Nothing exercises it today (every test is single-threaded and submits before it waits), but Phase 5 item 9 — several submits in flight — is where it starts to matter |
| D17 | Split this file: state in `STATUS.md`, narrative in `docs/history/` | **CLOSED 2026-08-04.** `scripts/split-status.py` cut the file at section boundaries into contiguous chunks, **verified that the chunks reassemble into the original byte for byte before writing anything**, and wrote `docs/history/` plus `MANIFEST.sha256`. 7749 lines became 171 here and nine files there. That answers "what guarantees nothing is edited in transit". For afterwards, `scripts/check-history-intact.sh` compares every history file against its digest — broken three ways to confirm it fails: an undeclared edit, an undeclared file, a declared file gone. History may still be appended to; the manifest update in the same commit is what declares it, and puts old and new digests side by side in the diff |
| D15 | Adopt `nxvk`'s channel warm-up/calibration ramp (`docs/reference-analysis.md` § 12.5.2) in `horizon/channel/` | **CLOSED 2026-08-04: no, and here is the number.** The ramp diagnoses a ring-size fault by kicking synthetic push buffers of increasing size at every channel creation, CPU-waiting each rung. The entry *count* was already bounded by construction — `horizon_gpu_submit` refuses what will not fit `GPFIFO_QUEUE_SIZE` — so the open question was the *size of one entry*, which was checked only for being non-zero. `t_pbsize` walked it on hardware: **every rung from 32 up to 524288 dwords (2 MiB) executed to its last dword**, each verified by a semaphore release at the END of the entry rather than by the submit being accepted. There is no limit to bound in that range, so nothing is added to `horizon_gpu_submit` and nothing is paid at channel creation. Should a limit ever appear it is a number to enforce, not a ramp to run |

### Note on the D9 collision (merge of `main` into the Phase 4 branch, 2026-08-04)

Two decisions were given the number **D9** independently: this branch used it from
Phase 4 step 1 onwards for the `nvkmd` pdev/dev split, and `main` used it — via the
`nxvk` audit merged as PR #5 — for the channel warm-up ramp. Merging the two histories
put both rows in this table.

The branch's D9 kept the number, because it is load-bearing: it is cited in four places
inside `mesa-patches/0018-…`, which is applied source, and in six places in this file.
The `nxvk` row was cited nowhere — `docs/reference-analysis.md` § 12.5.2 and § 12.5.5
describe the idea without naming a number — so renumbering it to **D15** changes no other
file and loses no history. Nothing was dropped; only one identifier moved.

`main`'s D8 row still read **open**. It is superseded here, not by argument but by the
work: items 6-10 convert the absolute deadline to a relative duration exactly once, which
is what the question was blocking.

### Four rows were stale, and what found them (2026-08-04)

Merging `main` put this table under inspection and four rows did not survive it. All
four had been answered in the body of this file, in a patch, or by a log in
`docs/hw-logs/`, and none of the answers had been carried back up to the summary. The
rows now read closed:

| Row | Answered by | How long it sat wrong |
|---|---|---|
| D5 — cache policy per memory type | the `t_gpuwrite` matrix (R6) | since R6, and the row's *premise* was wrong, not just its state |
| D6 — timeline semaphores | patch 0022 | since the patch landed |
| D12 — sparse binding | patch 0029 (`has_sparse = false` for horizon) | since the patch landed |
| D14 — uncached memory policy | `docs/hw-logs/t_uncached.log`, PASS 19/19 | since the log landed |

D14 is the worst of the four: the row asserted "`horizon_gpu` offers only
`HORIZON_GPU_MEM_CACHED`" while `horizon/memory/mem.c` had implemented the uncached
policy, patch 0030 had wired `NVKMD_MEM_COHERENT` to it, and a passing hardware log for
it was committed in this same tree. Anyone reading only the summary table would have
concluded the opposite of what the repository contains.

**What found it was not a re-read of this file.** It was the auto-generated description
on PR #6, which listed "Implemented UNCACHED cache policy (decision D14)" as done. That
contradicted the table, so one of the two had to be wrong — and it was the table. A
generated summary disagreeing with the hand-maintained one is a cheap and apparently
effective check; worth repeating rather than resenting.

D5 is the one worth keeping in view. The others were bookkeeping. D5 was a wrong belief
about the hardware that survived in the summary for as long as it survived in the code:
the row assumed CPU cache maintenance was what would make a GPU write visible. The
console said otherwise. That is the same mistake, in the same place, that
`tests/t_vulkan.c` carried in its memory-type note until `fc3c636`.

### D2 — Mesa version: `mesa-26.1.5`

Current stable series at Phase 2 start (released 2026-07-15; 26.2 had
branched but was only at rc2). Chosen over the reference port's 25.0.7
for a modern `nvkmd` surface — which is exactly the interface Phase 4
implements — and a credible upstreaming path. This is the
recommendation `docs/known-risks.md` R14 already carried.

Accepted cost: line references in `docs/reference-analysis.md` describe
a 25.0.7 tree and are now approximate. They still point at the right
file and concept.

Pinned as `MESA_TAG` / `MESA_COMMIT` in `toolchain/versions.env`. The
commit, not just the tag, because tags can be moved;
`scripts/fetch-mesa.sh` verifies the SHA it actually got.

### D3 — Mesa checkout: script-fetched, not a submodule

`mesa/` stays gitignored and is populated by `scripts/fetch-mesa.sh`.
Reasons, in order of weight:

1. `docs/milestones.md` Phase 2 item 8 says *every* version is pinned in
   `toolchain/versions.env`. A submodule splits the Mesa pin between a
   gitlink and that file — two sources of truth that can disagree.
2. Phase 3 applies `mesa-patches/` into `mesa/`. A patched submodule is
   permanently dirty, and its recorded SHA can drift by accident.
3. A clone with `--recursive` would pull the full Mesa history for
   everyone, including CI, whether or not they build Mesa.

The fallback the submodule option was meant to provide is kept anyway:
if git-over-https is blocked, the script downloads GitLab's archive
addressed by *commit*, so the content is the pinned commit by
construction.

Measured here: `git ls-remote` and `git fetch` against
`gitlab.freedesktop.org` both work from this environment, so the git
path is the one actually exercised (503 MB checked out, SHA verified).

---

