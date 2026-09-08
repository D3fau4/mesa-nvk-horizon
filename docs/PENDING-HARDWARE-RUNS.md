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

**The numbers below have holes in them.** A section that closes leaves
this file rather than being renumbered, because commit messages cite
these numbers and renumbering would make those citations point at
something else. What left is in the closed lists at the bottom.

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
a control. If neither separates, what `0063`-`0068` have shown is the
narrower thing: 15.2 ms a frame out of `vkAcquireNextImageKHR` on the
semaphore path, with the loop's total blocked time unchanged. That is
where the wait falls and not how long the thread waits, and it is what
they must be described as buying — this section said "the CPU saving
they have already shown — 15.2 ms a frame off this thread" until
2026-09-07, which section H's own blocked-time columns contradict.

**AND NOT BY REPEATING SECTION H.** A second FIFO run with the same
shape cannot answer anything this one did not; only one of the two
configurations above can. Anything else is a re-measurement.

---

## 4 Something kills the process a minute after the MMU fault

**Class HW.** Section 4 used to be "`gpu_fault` passes and then the
process dies", seen once and asked for three runs. It got them on
2026-09-07 — build `9cd9b80` plus this session's commits, mesa
`47be3e0` plus `0075`-`0076` — and what they settled is in
`docs/MEASURED-ON-HARDWARE.md`: three runs, three deaths, 45 to 60
seconds after launch, with no controller input, no `+` pressed and (in
two of the three) nothing so much as taking a screenshot. The suite
reports `PASS (44/44)` and writes its whole log first, every time.

What those runs also settled is what it is **not**: not the exit crash
`mmu_fault.c` has warned about since 2026-08-04, because nothing was
pressed; not the exit path at all, because the `atexit` marker was
never written; not a CPU exception in this process, because Atmosphère
wrote no crash report and `fatal_errors` is empty; not the applet loop
giving up, because that would return out of `main`; and not idling as
such, because `vk_core` idled three minutes at the same prompt in the
same session and exited cleanly.

**What is left is that the process is terminated from outside**, tens
of seconds after a channel that took an MMU fault was closed. Who does
it, and whether anything this project owns can avoid it, is not
established.

**Done when** one of these is known: which process or service ends it
(nvservices' own log, `pm:dmnt`, or an Atmosphère log level that
records a termination); or whether a build of `gpu_fault` that runs
only `sparse` survives, which would confirm the fault is the trigger
rather than something else in the suite; or whether a process that
closes the faulted channel and then does *nothing else* — no second
device, no settle — dies the same way. Any of the three narrows it from
"something" to a suspect. Until then the suite stays as it is: it
passes, and it costs a relaunch.

---

## 5 A consumer that resizes the layer has never been seen

**Class X.** `0076` gives the size-latch a way to follow the consumer:
when `wsi_horizon_extent_changed` gets past all three of its exclusions
— our own registration, the connect-late echo of the previous one, and
the output the swapchain was created for — what is left is a size the
consumer reported and this process never asked for, and the latch takes
it. Without that, a real resize would leave `currentExtent` and
`maxImageExtent` reporting a size that is gone, so the recreation the
SUBOPTIMAL result asks for could not ask for the new size.

**Nothing on this console reaches the branch.** `dock/mode_change`
watched `nwindowGetDimensions`, `NWindow::default_*` and
`appletGetDefaultDisplayResolution` across four dock and undock
transitions on 2026-09-07, with a buffer queued on every poll, and all
three said 1280x720 throughout. The same is true of the SUBOPTIMAL
result itself: its only live condition is a consumer-side resize, and
this platform has not been observed to have one.

**And the branch had a second way of not firing, which review found
rather than a run.** `0086`: the latch is a one-entry cache keyed on
whichever window asked last, and `wsi_horizon_layer_moved` used to
update it only when that key already named this swapchain's window —
so a capabilities query for a second `NWindow` was enough to make a
real resize report on the first one a no-op. Nothing in the tests opens
a second window, so no run could have shown it and none can show it
fixed either; it is here because it is the same unobserved branch, and
the wrong `maxImageExtent` at the end of it is the same one.

**And a third way, from the Codex review of PR #26 — this one is not
fixed.** The latch is only ever refreshed from
`wsi_horizon_layer_moved`, which runs while a live swapchain is
observing a present result. Two cases therefore keep a stale latch and
no code in the tree notices: a consumer resize that happens *between*
swapchains, with nothing presenting to see it, and a freshly
initialised `NWindow` that malloc happens to place at the address the
one-entry cache still holds — pointer equality is the whole key. The
review asked for "a lifecycle/revalidation path that does not treat
pointer equality as proof", and there is nothing to build one out of:
`0070`'s own Evidence is that `nwindowGetDimensions` returns
`NWindow::width` when it is set, which is this backend's registration,
so revalidating reads back our own echo. `NWindow::default_*` at
connect and at queue is the consumer's only voice, and it is what the
latch already holds. So this is written down rather than fixed, and it
closes with the rest of the section.

**Done when** a run has produced a consumer-reported output size that
is not this process's own — dock/undock while presenting is the only
candidate anybody has proposed, and it has now been measured not to do
it — or when the conclusion is drawn the other way: that on this
platform the layer cannot move, in which case both this branch and the
SUBOPTIMAL machinery it hangs off are dead code and should be argued
about on those terms rather than kept for a case nobody can produce.

---

## 7 A wait the channel already carries is no longer queued again, and nothing has run it

**Class X.** `0080` gives `nvkmd_horizon_ctx` a memory of the highest
threshold it has acquired on each syncpoint, and drops a fence that
memory covers instead of pushing one more host-engine acquire for it.
The case it exists for is every frame's: `nvk_queue.c` waits on the
upload stream's time point on every submission that carries command
buffers, and `nvk_upload_queue_flush()` reports the last time point
whether or not anything was pushed, so between two uploads the same
upload-channel fence went out as a full submit — kickoff, increment,
wait-ring slot, reap — with every `vkQueueSubmit`.

**Done when** a run with `MESA_VK_NVKMD_HORIZON_SUBMIT_STATS=1` set by
the case on itself (`vk_core/submit_batching` prints the meter) shows,
over a loop of submissions with no upload between them, one wait
"handed to the host engine" for the upload channel and then
`ctx_waits_skipped` growing by one per submit — and
`horizon_gpu_channel_get_stats().wait_submits` on the exec channel no
longer growing with the submit count. The four presenting suites must
pass beside it: the memo is only ever allowed to drop a wait the ring
already enforces, and a picture that is wrong is the symptom if that
claim is false.

---

## 8 The borrowed-heap walk can go further than 32 blocks now, and nothing has taken it that far

**Class X.** `mem_create` proves every block malloc hands it is
writable before anything touches it, and sets aside — never frees —
the ones that are not, asking again until it gets one that is ours.
The walk had two bounds and the wrong one always fired first: 32
attempts, or 48 MiB set aside. For any allocation smaller than the
borrowed span the attempt count came first, and by a long way — 32
blocks of 4 KiB is 128 KiB, against borrowed regions this console has
reported at 8814592 and 23404544 bytes on two launches of one build.
An allocation landing at the start of such a span therefore refused
with `OUT_OF_MEMORY` while the budget still had 46 MiB of room, which
is a device creation or an application start-up that fails. Raised in
review of PR #25; the cap is gone, the byte budget bounds the walk on
its own, and the case the cap was really for — an allocator offering
the same address twice — is now detected by comparing addresses.

**No run has walked past more than one block.** The two runs that
measured the spans on 2026-09-08 allocated in megabytes, where 32
attempts was never the binding constraint, and the Godot crash that
started all of this was a swapchain image. The small-allocation case
is the one nothing has produced on purpose.

**Done when** a console log shows the summary line this adds — `walked
past N borrowed heap block(s)` with N above 32 — followed by the suite
passing, so that the long walk is known to end in a usable block and
not in a slower failure; or when a run establishes that newlib's malloc
never puts a small GPU allocation inside a borrowed span in the first
place, in which case the length of the walk was never what mattered
and this closes the other way.

---

## Closed on 2026-09-08

- **The Forward+ reproducer does not reproduce it at 1280x720 either.**
  Section 6 asked for the matrix at an extent with a real number of
  warps in flight. It got it on 2026-09-08: all eight variants render at
  1280x720, G and H — 112 registers, with and without a convergence
  stack — to the texel. Occupancy is off the list; the measurement is in
  the counterpart file.
- **A sparse bind waits for its acquires, and the case that shows it
  has not run.** It ran. `vk_core/sparse_binding` section D, on a
  console: `MEASURED D: the fill was still pending when
  vkQueueBindSparse was called`, and `MEASURED D: the block holds the
  fill the unbind was ordered behind (0 of 32768 words wrong, first
  0x1d1dd1d1)` — which is both halves of what the section asked for, on
  a build carrying `0082`. `vk_wsi` 622/622 and `vk_present` 1038/1038
  beside it, so the picture is not wrong. Suite `PASS 1916/1916`
  [10/10].

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
- **`gpu_fault` passes and then the process dies.** Three runs on one
  build said what happens after the RESULT line: it dies 45 to 60
  seconds later with nothing touching it, writes no crash report, and
  never reaches its own `atexit` marker. Section 4 above is what is
  left of the question.
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
