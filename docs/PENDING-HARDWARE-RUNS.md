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

## 1 A wait submit no longer drains the pipeline, and only half of that has run

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
way `vk_present/drawn_frame` sections G and H do for the acquire and
`vk_core/device_memory` section D does for the zero fill.

**Done when** one run has compared the two shapes on a workload with
cross-channel waits in it, with `horizon_gpu_channel_get_stats()`'s
`wait_submits` and `bare_fence_submits` beside the times. If they do not
differ in time, this comes out, because then it is complexity for
nothing.

---

## 2 The per-submit syncpoint read is conditional, and the cost was never taken

**Class HW + X.** `platform/teardown` passes on a console (87/87 for the
suite, 2026-09-07), which is the half that matters for correctness: it
is the only caller of `horizon_gpu_channel_add_retirement`, so the read
still happens there and the callbacks still fire.

The other half is a number nobody has taken. `gpu_submit/submit`'s log
carries no per-submit cost with and without `HORIZON_GPU_EAGER_REAP=1`,
for the same reason as section 1: nothing sets the variable.

**Done when** `gpu_submit/submit` has reported the per-submit cost both
ways. If the difference is inside the noise, say so and consider taking
the branch back out.

---

## 3 The acquire deferral has never been measured where it could pay

**Class HW.** `vk_present/drawn_frame` section H measured it where it
cannot, deliberately and with a result — see
`docs/MEASURED-ON-HARDWARE.md`. Under FIFO with 8.3 ms of CPU work in
the frame, the acquire fell from 5436 us to 112 us and the frame did
not move: 16182 us against 16360 us, beside a control pair 184 us
apart. The display paces the loop; what the thread does not spend
working it spends blocked, and the blocked totals agree to 2%
(7473 us against 7614 us).

That is the whole of what a refresh-paced loop can show. The two
configurations where the deferral could produce a frame time have not
been run:

- **a thread with more work in it than a refresh.** Section H's own
  numbers say why this is the interesting case: the CPU-wait half's
  acquire cost 5436 us with work in the frame and 12932 us without,
  because the work happens before the next acquire and the compositor
  releases the buffer meanwhile. The wait is self-limiting until the
  frame stops fitting. Somewhere past that the two shapes must diverge.
- **IMMEDIATE with work.** Section H is FIFO because that is where the
  release fence costs a refresh. Section G is IMMEDIATE but acquires
  with a fence, so it pays the same dependency two calls later and
  measures nothing. Neither has run IMMEDIATE with a semaphore and work.

**Done when** one of those two has been measured, both ways round, with
a control. If neither separates, `0063`-`0068` keep only the CPU
saving they have already shown — 15.2 ms a frame off this thread on the
semaphore path — and that is what they must be described as buying.

---

## 4 `gpu_fault` passes and then the process dies

**Class HW.** Seen once, on 2026-09-07, build `3409462` / mesa
`47be3e0`.

The suite reported `PASS (44/44) [2/2 cases]` and wrote its whole log,
`horizon-build-id` line included. Some time afterwards, idling on
testfw's "press + to exit" screen with nothing pressed, the process
ended with the system's own "the software was closed because an error
occurred" dialog. Nothing about it is in the log — the log ends at the
RESULT line, which is where testfw stops writing.

So the two cases are not in doubt and the teardown may be: this is the
one suite that provokes an MMU fault on purpose, `tests/README.md` has
said since it was written that its after-effects on the console are
unconfirmed, and this is the first run to see one. It was not seen on
the earlier run of the same day, on a build without `0070`-`0074` —
which is a difference, and not yet evidence, because a fault's
after-effects are exactly the kind of thing that varies run to run.

**Done when** `gpu_fault` has been run three times on one build with
somebody watching what happens after the RESULT line, and either it
returns to hbmenu every time or the crash report is fetched from
`sdmc:/atmosphere/crash_reports` and says what died. If it turns out
to be the fault's after-effects rather than anything this branch
changed, that belongs in `docs/MEASURED-ON-HARDWARE.md` and this
section leaves.

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
  compositor returns one fence; and the A/B pair exists. What it buys
  is now a question of its own, section 3 above.
- **Submissions are batched across `exec()`, and nothing has run it.**
  `vk_core/submit_batching` 471/471, the image-producing suites pass
  beside it, the pushes-per-kickoff line is recorded for a real
  application (`mean 1`, so nothing to save there), and the A/B pair
  exists (6598 us batched against 6179 us unbatched, one sample each).
  Which is the outcome the section predicted for a workload submitting
  one command buffer at a time, and the patch stays for the ones that
  do not.
- **`vk_wsi/concurrency` aborts the process, and it is not this
  branch.** It was not a race and not the ownership hand-off: libnx's
  `framebufferBegin` ends the process on any failed
  `nwindowDequeueBuffer`, and two of those failures are the ordinary
  "no buffer free yet" the zero-copy acquire retries on. Section D is
  the only place in the suite that asks for two images. `0071` gives
  the copy fallback its own dequeue with that retry policy; `vk_wsi` is
  571/571.
- **`currentExtent` reports the swapchain this process registered one
  connect ago.** `0070` latches the layer's size instead of re-reading
  the queue's, `0074` takes that latch from libnx rather than from the
  field this backend writes, and `vk_wsi/swapchain` section H cycles
  the resolution down and back three times to say so. `suboptimal` is
  273/273.
- **The uninitialised allocation path is worth routing something
  through.** `NVKMD_MEM_NO_ZERO_INIT` exists through `nvkmd.h` and both
  backends, `vkAllocateMemory` is the one consumer that takes it, and
  `vk_core/device_memory` measured what it saves: 1346 us of an 8 MiB
  allocation, 47%. The command pool took it, broke `vkCmdUpdateBuffer`
  on a console, and gave it back — which is also how `0073` was found.
