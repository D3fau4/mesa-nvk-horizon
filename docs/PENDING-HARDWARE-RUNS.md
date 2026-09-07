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

## 1 `vk_wsi/concurrency` aborts the process, and it is not this branch

**Class HW.** Reproduced three times on 2026-09-07, at the same point
every time, on two different driver builds.

`vk_wsi/concurrency` section D — recreation churn, generation n
destroyed by a reaper thread while generation n+1 presents, image count
and present path alternating — kills the process:

    Result: 0x2B59 (2345-0021)   LibnxError_BadGfxDequeueBuffer
    Type:   User Break
    PC:     svcBreak  <- diagAbortWithResult <- framebufferBegin
                      <- wsi_horizon_present_fallback (wsi_horizon.c:2311)
                      <- wsi_common_queue_present
                      <- mt_present (tests/vk_wsi/concurrency.c:673)

libnx's `framebufferBegin` aborts rather than returning when
`nwindowDequeueBuffer` fails, so a producer error on the copy path ends
the process and takes the suite's remaining cases with it. The log stops
after `D: the first generation -> VK_SUCCESS`, 52 checks in, every time.

**IT IS NOT `0063`-`0069`.** The same `.nro` built against mesa at
`266290e` — the series truncated to `0062`, `libnvk.a` rebuilt,
`vk_wsi.nro` relinked, 14786616 bytes against 14790712 — aborts at the
identical point with the identical 52 checks. The acquire patches cannot
reach it either by construction: `gpu_acquire_wait` requires
`chain->zero_copy` and this is the copy fallback.

The shape of it is a race the file already half-anticipates: the reaper
destroying generation n calls `nwindowReleaseBuffers` on the window
generation n+1 is presenting through, and both live on the process's one
default `NWindow`. `wsi_horizon_release_window` tests
`surface->owner == chain` under the surface lock, so the *bookkeeping*
is ordered; what is not is libnx's Framebuffer, which the successor is
inside `framebufferBegin` on while the loser's disconnect runs.

**Done when** section D runs to its end on a console, or the ownership
hand-off between a retiring copy-path swapchain and its successor is
shown to be correct and something else is found. Until then `vk_wsi` is
a suite that loses its third case; `swapchain` and `suboptimal` run
ahead of it and report.

---

## 2 `currentExtent` reports the swapchain this process registered one connect ago

**Class HW.** Measured 2026-09-07 while chasing the failure above.

`wsi_horizon_get_extent` reads `NWindow::default_width/height` and its
comment calls that "the consumer's answer: libnx fills it from the
BufferQueue's output at connect and at every queue". On this hardware it
is filled **at connect only**, and the value it gets is what this
process's *previous* registration set — `nwindowSetDimensions` at
registration, to the swapchain's own extent.

So it lags by exactly one connect, and the consequences are not
cosmetic. Measured, in one process:

- `vk_wsi/swapchain` section G presents 5410 frames on a 640x360
  swapchain over a 1280x720 layer and reads `currentExtent` 1280x720
  throughout — its own regression guard passes while the queue has
  already been left at 640x360.
- the next case connects, `default_*` is refilled to 640x360, and
  `vk_wsi/suboptimal` — whose capabilities query, taken before that
  connect, said 1280x720 — spends 120 frames with a 1280x720 swapchain
  on a surface claiming 640x360. 240 rule disagreements, and then
  `vkCreateSwapchainKHR(1280x720)` twice returns
  `VK_ERROR_INITIALIZATION_FAILED`, because `imageExtent > extent` is
  the check `wsi_horizon_surface_create_swapchain` makes against the
  same lagging value.

**An application meets this, not just a test.** One that renders at half
resolution and later wants full is told its surface shrank, and can only
get back up inside the lag. Nothing in the Vulkan API it can call
distinguishes that from a display that really did change.

**And a process cannot fully undo it.** `tests/vk_wsi/swapchain.c` now
presents one full-size generation after section G, which puts the
*queue* back; the next thing to connect then reads the layer's size
again, and `vk_wsi/suboptimal` goes from 170/175 to 272/273 on that
alone. A *second* restoring generation cannot even be created —
`INITIALIZATION_FAILED`, because the first one's own connect refreshed
`default_*` to the queue's previous value and creation is gated on it.
The two values are never both the layer's size at the same time, so
there is no sequence of swapchains that puts a process all the way back.
The one failure left in `suboptimal` is its section A sizing its
swapchain from a query taken before its own first connect, which is what
an application would do too.

**Done when** `wsi_horizon_get_extent` answers with the layer's size
rather than the producer's last request, or it is established that
`default_*` is the only source available and the limitation is written
into `0040`'s comment as a known one. `display/nwindow` measured
1280x720 for the layer on 2026-08-24 and `nwindowGetDimensions` is what
returned it, so there is a candidate.

---

## 3 A wait submit no longer drains the pipeline, and only half of that has run

**Class HW + X.** The suite passes with the new shape: fourteen suites
on 2026-09-07, `gpu_submit` 324/324, `vk_core/concurrent_submits` and
`vk_present/drawn_frame` among the passes, and `nvkmd_horizon`'s meter
reporting cross-channel waits taken (`61 wait(s) handed to the host
engine` in `vk_present/drawn_frame` section F). What has **not** run is
the comparison.

`horizon_gpu_submit_waits` emits an increment-only fence block and skips
the L2-invalidate prologue. `HORIZON_GPU_FULL_BARRIER_WAITS=1` restores
the old shape and **nothing in the tree sets it** — homebrew is launched
with no environment, so an A/B needs a case that sets it on itself, the
way `vk_present/drawn_frame` section G does for the acquire and
`vk_core/submit_batching` section E does for the batching.

**Done when** one run has compared the two shapes on a workload with
cross-channel waits in it, with `horizon_gpu_channel_get_stats()`'s
`wait_submits` and `bare_fence_submits` beside the times. If they do not
differ in time, this comes out, because then it is complexity for
nothing.

---

## 4 The per-submit syncpoint read is conditional, and the cost was never taken

**Class HW + X.** `platform/teardown` passes on a console (87/87 for the
suite, 2026-09-07), which is the half that matters for correctness: it
is the only caller of `horizon_gpu_channel_add_retirement`, so the read
still happens there and the callbacks still fire.

The other half is a number nobody has taken. `gpu_submit/submit`'s log
carries no per-submit cost with and without `HORIZON_GPU_EAGER_REAP=1`,
for the same reason as section 3: nothing sets the variable.

**Done when** `gpu_submit/submit` has reported the per-submit cost both
ways. If the difference is inside the noise, say so and consider taking
the branch back out.

---

## 5 The uninitialised allocation path is worth routing something through

**Class HW.** `gpu_memory/alloc` measured it on 2026-09-07 and the
numbers are in `docs/MEASURED-ON-HARDWARE.md`: the zero fill is 21 us of
a 64 KiB allocation, 182 us of a 1 MiB one and **1403 us of an 8 MiB
one**. That answers the routing question this file used to ask with a
yes for anything of a megabyte or more, and with a no for the 4 KiB
channel command buffer.

`horizon_gpu_mem_create_uninit` therefore stays. **Nothing routes
through it yet**, which is the work:

- `nvkmd_horizon_mem.c` is one entry point for `VkDeviceMemory` the
  application maps, NVK's descriptor tables, query pools, the shader
  heap, and command-buffer and mem-stream chunks. Vulkan promises the
  application nothing about the contents of `vkAllocateMemory`, and
  `nv_push` writes every dword of a command-buffer chunk before the
  submit names `[addr, addr+range)` — so both of those qualify on their
  own terms.
- What has to be audited one by one is the rest of that list: anything
  NVK itself reads before writing would get garbage, and the failure
  would be a wrong descriptor rather than an error.
- The hang recorder keeps the fill unconditionally: the zeros are how
  "the GPU never reached this slot" is recognised.

**Done when** an `NVKMD_MEM_*` bit exists through `nvkmd.h` and both
backends, the consumers that take it are named with the reason each one
is safe, and a console run shows the allocation time moving on a
workload that creates real resources. If the audit finds no consumer
that can be shown safe, delete the entry point rather than leave it as
an unused promise.

---

## Closed on 2026-09-07

Kept as a list rather than as text, because what they settled is in
`docs/MEASURED-ON-HARDWARE.md` and the point of this file is what is
still owed.

- **Zcull is now bound, and nothing has run it.** `vk_render/zcull`:
  0 of 65536 pixels differ with and without, 570 ms against 586 ms.
  `0059` and `0060` stay.
- **Three new test binaries, none of which has ever run.**
  `vk_render/zcull` 276/276, `vk_pipelines/pipeline_volume` 225/225,
  `vk_render/draw_volume` 130/130, measurements recorded.
- **The acquire no longer waits for the compositor, and nothing has run
  it.** All four questions answered: the picture is right by eye and by
  four presenting suites; the foreign-syncpoint read works; the
  compositor returns one fence; and the A/B pair exists. It closes
  against a criterion its own text set — "if the frame time does not
  move, the patches still come out" — that the measurement then split:
  the frame time does not move and 15.2 ms of CPU per frame does. The
  patches stay on the second half of that, and the first half is written
  down beside it so the decision can be revisited on the evidence rather
  than on the memory of it.
- **Submissions are batched across `exec()`, and nothing has run it.**
  `vk_core/submit_batching` 471/471, the image-producing suites pass
  beside it, the pushes-per-kickoff line is recorded for a real
  application (`mean 1`, so nothing to save there), and the A/B pair
  exists (6598 us batched against 6179 us unbatched, one sample each).
  Which is the outcome section 6 predicted for a workload submitting one
  command buffer at a time, and the patch stays for the ones that do
  not.
