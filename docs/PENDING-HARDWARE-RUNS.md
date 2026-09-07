# Pending hardware runs

**This file is a debt ledger, and it is meant to be deleted.** Every
section below is work that was cross-compiled and never executed on a
console, and every one carries a **Done when** line saying what a run
has to report for the section to close.

When the last section closes, *delete this file*. Do not leave a
hollowed-out version behind saying "all clear": an empty ledger is just
another stale reference. That is the rule that killed this file's
predecessor, `docs/PENDING-VERIFICATION.md`, on 2026-08-25, and it
applies here unchanged.

It is deliberately **not** the same filename. That one is cited by
`mesa-patches/0054` and `0055`, and what those two point at is now in
`docs/MEASURED-ON-HARDWARE.md` — reviving the name would make those
citations resolve to a file that no longer holds what they meant.

The counterpart file is `docs/MEASURED-ON-HARDWARE.md`: facts that a
console established, class **HW**, none of which needs action. Nothing
with a **Done when** line belongs there. A section that closes here
moves there only if a measurement settles something that would
otherwise be measured twice; otherwise it just leaves.

| Class | Means | Does **not** prove |
|---|---|---|
| **H** — host | Built and run via `scripts/run-host-tests.sh` | Anything about the Switch |
| **X** — cross | Cross-compiled for aarch64 Horizon; a `.nro` exists | That it runs, or is correct |
| **HW** — hardware | Ran on a real console, with the log | Only what the log actually shows |

**The test names below are the ones in the tree today.** The suite refactor
turned fifty-three `.nro` into fourteen, each holding several cases, and a
result is now identified as `<suite>/<case>` — so what an older log calls
`t_vk_zcull` this file calls `vk_render/zcull`, and it is the same code. The
mapping for every one of them is `tests/<suite>/suite.c`.

---

## 1 Zcull is now bound, and nothing has run it

**Class X.** `mesa-patches/0059` and `0060` and `tests/vk_render/zcull.c`
cross-build; no console has executed any of them.

Until 0060, the physical device advertised `has_zcull_info` and every
channel was created with `bind_zcull` false, so NVK programmed the
on-chip Zcull state on channels that had no context-switch save area.
0060 asks for the bind on contexts created with `NVKMD_ENGINE_3D`; 0059
stops advertising Zcull where `nvGpuGetZcullCtxSize()` is 0, and adds
`NVK_HORIZON_ZCULL=0`.

Zcull only ever *rejects*, so a fault here is silent: a fragment wrongly
culled is geometry that is not drawn, with no notifier and nothing in a
log. `vk_render/zcull` is built around that. It renders one depth workload
twice in one process — section A with `NVK_HORIZON_ZCULL=0`, section B
with it on — and compares the colour and depth images pixel for pixel,
plus an analytic check on each half so a fault affecting both equally
does not compare equal.

Three things the run has to report:

- whether A and B are identical. **If they are not, the action is to
  stop advertising Zcull** — set `has_zcull_info` false in 0059
  unconditionally, and drop 0060 — not to debug it from here.
- whether `nvGpuChannelZcullBind` succeeds at all. It is on the channel
  creation path, so a refusal fails `vkCreateDevice` rather than
  degrading. `NVK_HORIZON_ZCULL=0` is the way back without a rebuild,
  and a refusal means 0060 has to make the bind non-fatal or go.
- the two wall times the test notes. Whether Zcull is faster on this
  workload is unknown; the workload was built to be checkable, not to be
  culled well.

**Done when** `vk_render/zcull` has run on a console and the answer has been
acted on: kept and recorded if A and B match, withdrawn if they do not.
The seventeen existing Vulkan tests have to be re-run alongside it —
every one of them that clears a depth attachment now takes a different
path through `nvk_CmdBeginRendering`.

## 2 A wait submit no longer drains the pipeline

**Class H + X.** `horizon_cmds_fence_incr_bare` is covered by
`tests/host/h_cmds.c` (46/46 under ASan and UBSan) and the whole thing
cross-builds; no console has executed it.

`horizon_gpu_submit_waits` now emits an increment-only fence block and
skips the L2-invalidate prologue, on the argument that its command list
is host methods with no memory effect: nothing to invalidate, no engine
work for a wait-for-idle to wait for, and nothing dirty to write back —
the writes being waited on were flushed by the fence block of the
channel that made them. Two GPFIFO entries instead of three, and no
pipeline drain per cross-channel wait.

**This changes the submit path every test goes through**, so the whole
suite is what has to be re-run, not only the WSI tests. Patch 0035 means
only cross-channel waits reach it, so the paths that exercise it are the
upload queue and presentation: `vk_wsi/concurrency`, `vk_present/drawn_frame`,
`vk_core/concurrent_submits`, `gpu_submit/submit`.

`HORIZON_GPU_FULL_BARRIER_WAITS=1` restores the old shape, so both can
be measured in one run rather than across two builds.
`horizon_gpu_channel_get_stats()` reports `wait_submits` and
`bare_fence_submits`, so "it took the cheap path" is a number.

**Done when** the suite has passed on a console with the new shape, and
one run has compared it against `HORIZON_GPU_FULL_BARRIER_WAITS=1` on a
workload with cross-channel waits in it. If the two differ in
correctness, this comes out; if they do not differ in time either, it
still comes out, because then it is complexity for nothing.

## 3 The per-submit syncpoint read is now conditional

**Class X.** Reading the syncpoint is libnx, so no host suite reaches
this line; it cross-builds and no console has run it.

`horizon_gpu_channel_reap` skipped the `SyncptRead` ioctl when no
retirement is registered — which, on the path NVK takes, is always:
`horizon_gpu_channel_add_retirement` has exactly one caller in the tree
and it is `tests/platform/teardown.c`. The fault check stays unconditional; it
is the safety property and it costs an event wait, not an ioctl.

**Done when** `platform/teardown` still passes (it is the test that registers
retirements, so the read still happens there and the callbacks must
still fire), and `gpu_submit/submit` has reported the per-submit cost with and
without `HORIZON_GPU_EAGER_REAP=1`. If the difference is inside the
noise, say so and consider taking the branch back out.

## 4 Three new test binaries, none of which has ever run

**Class X.** `vk_render/zcull`, `vk_pipelines/pipeline_volume` and `vk_render/draw_volume` build as
`.nro` under `-Wall -Wextra -Werror` and link every archive
`meson.build` names. Nothing more than that is known about them.

`vk_pipelines/pipeline_volume` is the first thing in this project that makes the
shader heap grow past the chunk `nvk_heap_ensure_first_chunk` binds at
device creation: 96 distinct specializations of a 448-instruction
shader, which is at least 4.7 KiB of machine code each. Its sections C
and D are measurements rather than assertions — the cold compile
distribution, and what a second build of the same specializations costs
in the same process.

`vk_render/draw_volume` is the first to issue hundreds of draws with the pipeline
changing between them, and the first to blend. Its section C tolerance
of 3/255 is derived from the round-off of twelve blend steps; the worst
error actually seen is reported, so the first run says how much of that
bound this hardware uses.

**Done when** all three have passed on a console, their measurements are
recorded, and — for `vk_render/draw_volume` section C — the tolerance has been
narrowed to what was actually observed or the derivation corrected.

## 5 The acquire no longer waits for the compositor, and nothing has run it

**Class X.** `mesa-patches/0063`-`0066` cross-build:
`scripts/ci-build-archives.sh` over `ghcr.io/d3fau4/nx-dev:latest`
(aarch64-none-elf-gcc 15.2.0, meson 1.11.2, `-Wall -Wextra -Werror`)
compiles all five files they touch with no warning, links `libnvk.a` and
`libvulkan_wsi.a`, and ends with 53 `.nro` linking them and both
artefact gates clean. `vk_wsi/swapchain`, `vk_wsi/concurrency`,
`vk_present/drawn_frame` and `display/nwindow` are among those 14. Nothing else is
known: a `.nro` exists, and that is all class X ever means.

`wsi_horizon_acquire_zero_copy` used to `nvMultiFenceWait` on the fence
the compositor released the slot with — 13.8 ms of a 16.7 ms frame, the
figure `0049`'s comment carries. The fence is now handed to the driver
instead, as the payload of the semaphore and the fence the application
passed to `vkAcquireNextImageKHR`, and the wait happens on the host
engine through the path `0049` built. `MESA_VK_WSI_HORIZON_CPU_ACQUIRE_
WAIT=1` restores the old shape in the same build.

Four things the run has to report, in this order:

- **whether the picture is right.** This is the only change on this
  branch that can put a frame into a buffer the compositor is still
  reading, and that fault has no error, no notifier and no log line —
  it is a torn band on screen and nothing else. `vk_present/drawn_frame`
  section F is the case that drives the GPU-side path (`vk_wsi/swapchain`,
  `vk_wsi/concurrency` and the other sections of F's own file take the CPU one,
  which is the other half and equally worth running); a human looking at
  the screen is the instrument, because no Vulkan call this test can
  make would see it. **If tearing appears, the action is to set
  `gpu_acquire_wait` false unconditionally in `0066`**, not to debug it
  from the log.
- **whether `NVHOST_IOCTL_CTRL_SYNCPT_READ` answers for a syncpoint this
  process does not own.** `horizon_gpu_fence_wait` reads the counter
  before it waits, and that read has only ever been made against a
  channel's own syncpoint. It matters only for
  `vkWaitForFences` on the acquire fence — the GPU path reads nothing
  from the CPU — and a refusal would show as
  `horizon_gpu_fence_wait(...) failed` from `nvk_horizon_sync_wait`.
  `nvFenceWait` on these same fences has worked since `0037`, so if the
  read is refused the fix is a wait that does not read first.
- **whether the compositor ever returns more than one fence.**
  `NvMultiFence` holds four and all four are carried; one is what this
  is expected to see. The acquire meter says nothing about it, so the
  way to know is a log line at the point of failure, which is why an
  unrepresentable count is reported rather than truncated — and, since
  `0067`, why a driver that declines the set ends the acquire instead of
  letting `wsi_common.c` signal the semaphore anyway.
- **the number.** `MESA_VK_WSI_HORIZON_ACQUIRE_STATS=1` and
  `MESA_VK_NVKMD_HORIZON_SUBMIT_STATS=1`, twice in one session —
  once as built, once with `MESA_VK_WSI_HORIZON_CPU_ACQUIRE_WAIT=1` —
  same resolution, same dock state, same shader-cache state, with
  `HORIZON_GPU_SYNC`, the hang recorder and `NVK_HORIZON_PUSH_SPLIT`
  all off. Expected: the acquire line's "on the compositor's release
  fence" figure goes to nothing and its "handed that fence to the GPU"
  count becomes the frame count, while `nvkmd_horizon`'s "wait(s)
  handed to the host engine" rises by one per frame **in the semaphore
  run and stays at zero in the fence run**. Those two counters do not
  say the same thing and neither alone is the claim: the acquire meter
  counts the WSI deferring the fence, which happens on both paths, and
  only the `nvkmd_horizon` one is incremented after
  `horizon_gpu_submit_waits()` has taken the batch. **A frame-rate claim
  needs the application's own frame times, not these counters** — what
  they can show is that the stall moved, not what it bought.

  And it may buy nothing, which is a result and not a failure. The
  GPFIFO is in order, so a host-engine wait still stalls every submit
  behind it on that channel: what the change frees is the CPU, and that
  is worth something only where the CPU was the thing running out of
  frame. Section F reports the frame interval beside the acquire mean
  for exactly that reason — the acquire getting shorter is not the
  measurement.

One failure mode changes shape and is not a regression, but should be
recognised if it appears: a compositor that never releases a buffer used
to wedge the acquiring thread and now wedges the graphics channel, since
the host engine is what is holding at the syncpoint. Both were already
unbounded — an acquire at `timeout = UINT64_MAX` had no deadline to
expire — and the new shape is the more visible one: nvgpu times the
channel out and the error notifier says `4`, which `horizon/channel`
already names "timeout", so it arrives as `VK_ERROR_DEVICE_LOST` rather
than as silence.

**Done when** all four have been answered on a console: the picture
checked by eye and by the four tests, the two unknowns above settled,
and one A/B pair recorded. If the picture is wrong the patches come out;
if the picture is right and the frame time does not move, the patches
still come out, because then it is complexity for nothing.
