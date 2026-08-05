# Hardware logs

Console runs, kept verbatim. A log here is evidence, so none of them is edited
or deleted after the fact — including the ones later found to have measured
less than they claimed. This file is where that is said, because a reader opens
the log, not `STATUS.md`.

## The two logs Phase 6 currently rests on

`t_nwindow-run4-starves-at-two-FAIL.log` and
`t_vk_swapchain-run4-leak-fixed-FAIL.log`, both stamped
`2026-08-05T14:31:36Z e0bb31f` in their second line — the first runs whose
build is stated in the artefact itself.

Both report `FAIL`, and both are the evidence for three separate results:

- **The leak is gone.** `device destroy refused` does not appear anywhere in
  the swapchain log, where run 2 and run 3 both had `live mem=33 va_ranges=33
  mappings=33`. The teardown check that reports it is the `t_log_scan` version
  that fails when it cannot read, so its `ok` is worth something now.
- **The exit crash was the leak.** Reported by the operator: the console
  returns to the homebrew menu on `+`. Three runs tried to ask this question
  and this is the one that got to ask it.
- **Two buffers still starve, and the earlier diagnosis was wrong.** The
  failing result is `0x00006359` = `MAKERESULT(Module_Libnx,
  LibnxError_Timeout)` — the test's own wait expiring, not an error from the
  BufferQueue. The release event never fires. Patches 0059 and 0061 both aimed
  at which dequeue mode to use, and no dequeue mode is ever reached.

## The mode that works

### `t_nwindow-run7-async-false-works-FAIL.log`

Two results, and between them they end a question that cost five hardware runs.

**`async=false` works, immediately, on the window that had just failed:**

```
  note 2 buffers, interval 1: asked for 5000 ms — 78166 dequeue(s) in libnx's
       mode, release event fired 78165 time(s), last result 0x0000115d
  note 2 buffers, interval 1: async=false returned 0x00000000 after 145 us
  ok   2 buffers, interval 1: async=false produced a buffer where libnx's
       async=true could not, 78166 times running
```

Twice in the run, 145 us and 158 us, and it did **not** block inside the
compositor — the one behaviour that could not be ruled out until it was tried.

**And position is ruled out.** The same probe ran before any paced session and
after all of them; `probe BEFORE, 2 buffers` and `probe AFTER, 2 buffers` both
get a buffer in ~100 us. Where a session happens in the run is not the
variable.

The reading is that Android's producer takes `async` to mean "this producer is
in asynchronous mode" — `queueBuffer` never blocks, older frames are dropped —
which costs the queue one buffer held in reserve. A two-buffer queue with both
buffers out cannot spare it. FIFO presentation is synchronous by definition, so
`async=false` is the mode that describes what a swapchain is doing. Every other
observation in this file fits: three buffers usually have a free slot, and the
reconstructed probe drops a frame and frees one, so both answer `async=true`
without trouble. And the concurrency count says `WOULD_BLOCK` when three slots
are exhausted but `NO_INIT` when two are — two different conditions, which is
what "the count ran out" versus "the reserve cannot be met" looks like.

Patch 0061 had this mode and used it wrongly: it asked for it only after an
`eventWait` that had already spent the entire budget, and never asked
`async=true` again. Both modes are now asked on every iteration, with 0062's
slicing keeping any one call from eating the deadline. Patch 0063.

## The release event is not an edge

### `t_nwindow-run6-event-is-not-an-edge-FAIL.log`

The first log in which the diagnostic sat on the *real* failure rather than a
reconstruction, and it produced the two numbers that rule out everything tried
so far:

```
  note 2 buffers, interval 1: asked for 5000 ms — 84328 dequeue(s) in libnx's
       mode, release event fired 84327 time(s), last result 0x0000115d
```

**84327 `eventWait` returns in five seconds is 59 us apiece.** The release
event returns immediately every time, so it is permanently signalled and
carries no information about a buffer coming back. Waiting on it is a spin —
in libnx's own `nwindowDequeueBuffer` loop as much as in ours. Every design in
this project that treated it as "a buffer was released" was treating a level as
an edge.

**And `async=true` answered `0x115d` (`LibnxBinderError_NoInit`) 84328 times
running** on that window — while the same call, on the reconstructed probe's
two-buffer window thirty lines further down the same log, returned a buffer in
97 us. Two windows in one process, same buffer count, same last three calls,
opposite answers. The dequeue mode is not the variable and the event is not the
variable.

Everything else in the run is as good as it has been: 3 buffers 90/90 at mean
16502 us, 87 of 90 dequeues carrying a compositor-signalled release fence, and
`t_vk_swapchain-run6-FAIL.log` beside it with 89 of 89 intervals inside 10% of
a refresh, both present paths, two coexisting swapchains, no leak and no exit
crash.

## A probe that reported success next to the failure it was built to explain

### `t_nwindow-run5-probe-did-not-reproduce-FAIL.log`

`nw_probe_starvation` was written to answer why a two-buffer window stops at
the third frame. It built a session that looked like the failing one, handed
the compositor every buffer, and asked for one back for up to five seconds.
It reports, at 2 buffers:

```
  note starvation probe, 2 buffers: 1 dequeue(s) in libnx's mode over 0 ms,
       release event fired 0 time(s), last result 0x00000000
  ok   starvation probe, 2 buffers: the compositor handed a buffer back
       within 5000 ms
  note starvation probe, 2 buffers: it came back after 104 us
```

Sixty lines above it in the same file:

```
  FAIL 2 buffers, interval 1: 2 of 90 frames presented
  note 2 buffers, interval 1: dequeue failed at frame 2 -> 0x00006359
```

**The reconstruction never reached the state it was built to examine**, so its
four `ok` lines are not evidence about the failure. That is the same shape as
the checks listed under "Superseded runs" below — a green line that measured
something other than what its text implies — and it is the fourth in this
project.

The log is still worth reading for two things it did measure. A buffer came
back to a fully-queued two-buffer window in 104 us **with the release event
never firing**, which says the compositor can free a slot without signalling —
so a producer that waits on the event alone will stall where one that probes
first will not. And the failing result itself, `0x00006359` =
`MAKERESULT(Module_Libnx, LibnxError_Timeout)`, is the test's own budget
expiring; the only branch that returns it is the deadline check at the top of
an iteration, which is reachable only after an `eventWait` **succeeded**. That
is what located the defect: the loop spent its whole second inside one wait and
then gave up after a single retry in a dequeue mode nothing on this platform
uses. Fixed in `t_nwindow` and, as patch 0062, in the WSI.

## Runs that measured the wrong build

### `t_nwindow-run3-STALE-BINARIES.log`, `t_vk_swapchain-run3-STALE-BINARIES.log`

Real console runs, and they measured **nothing that was asked of them**. They
were run to test patches 0060 and 0061; the `.nro` on the SD card were the
previous batch's, so what these logs record is the behaviour patches 0060 and
0061 were written to change, measured again.

They are not copies of the run-2 logs — every timing differs, every heap
address differs, the syncpoint initial values differ. They are a genuine second
execution of the same binaries, which is exactly why the first reading of them
was wrong: a re-run of unchanged code looks like a fix that did nothing.

What gave it away was luck. Mesa's `vk_logi` prints `__FILE__` and `__LINE__`,
and

```
../src/vulkan/wsi/wsi_horizon.c:1781: the swapchain presents zero-copy
```

names line 1781 while the source that was meant to be running has that call at
1805. Nothing else in either log distinguishes one build from another.

Fixed in `d75f7b8`: every test now prints its build stamp as the second line of
its log —

```
  note horizon-build-id 2026-08-05T14:31:36Z e0bb31f
```

— the stamp is regenerated on every build in both build paths, the packaging
manifest reads it back out of the `.nro` and refuses a directory holding more
than one, and the run instructions say which stamp to expect. From run 4
onwards a log whose stamp is not the expected one says so in its own first
lines.

Nothing in these two files should be quoted as a measurement of the current
driver. What they do still show, being a second sample of the run-2 build, is
that run 2's failures are reproducible rather than one-off: `live mem=33` on
teardown, and the two-buffer sessions dying at frame 2.

## Superseded runs

A `-PASS` in a filename means the run reported PASS. It does **not** mean every
check in it measured what its text says.

### `t_gpuwrite-run1.log`, `t_gpuwrite-run2-matrix.log`, `t_gpuwrite-run3-l2fix.log`, `t_gpuwrite-run4-PASS.log`

Every one of these predates the fix in `ad5b973`, and all four contain the line
that gives it away — `6 dwords` in `run1`, before the L2 writeback was added to
the block, and `9 dwords` after:

```
  ok   <arm>: fence increment encoded (N dwords)
```

`run1` is the two-arm version (`cached`, `noncacheable`) and reports
`FAIL (25/27)`; the four-arm matrix starts at `run2-matrix`.

That check is the artefact of the defect. The test embedded its own fence
increment in the span it submitted while `horizon_gpu_submit()` also appended
the channel's, so the hardware counter advanced twice per submit and the
accounting once. From the **second arm onwards** the threshold handed to
`wait_fence` had already been passed by the previous arm's extra increment, so
the wait returned before that arm's GPU write had run and the readback raced
the GPU instead of following it.

What survives, and what does not:

- **Arm A is sound in all four runs.** It is the first submit on its channel,
  so its wait was correct.
- **Arm D passing is sound.** It was the arm most exposed to the early return;
  a payload already present when you look early is present.
- **Arms B and C measured nothing.** In `run2-matrix` in particular, B failing
  was read as "identical with CPU caching on and off" and C failing as "MEMBAR
  does not help". Neither failure is evidence of anything.

`t_gpuwrite-run5-review-fixes-PASS.log` is the first run with the corrected
test — 47/47, which is 51 minus the four removed encode checks.

It does **not** re-run the original no-flush-versus-flush experiment, and
cannot: `0cca09d` made the channel's fence block emit the L2 writeback on every
submit, so no no-flush arm exists any more. The four arms now differ only by a
redundant `MEM_OP`. Their all passing is consistent with the L2 finding and is
not a test of it.

### `t_pbsize-run1-debt-batch-PASS.log`, `t_pbsize-run2-payload-PASS.log`

Both end on a line that names a size they did not reach:

```
  note D15: every rung up to 524288 dwords (2048 KiB) executed to its last dword
```

The rung builder computed `filler = dwords - 5` and then `pairs =
filler / 2`, and `dwords - 5` is odd for every even rung, so the
division truncated and each entry was submitted **one dword short of
the size in its own label**. D15's question is whether a 524288-dword
entry executes; these two runs asked it about 524287.

The PASSes are real — every rung they *did* submit ran to its last
dword, and no limit appeared — but the boundary itself was never
tested at the boundary. `t_pbsize-run3-pr7-rerun-PASS.log` uses odd
rungs and asserts the total it encoded, and answers the question at
**524289**.

### `t_vk_depth-run5-batch5-PASS.log`

66/66, and the count is not the problem. The barrier between the render
and the read-back gave `VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT`
alone as the source stage, omitting
`VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT`. A depth write performed
in the early stage is therefore outside the dependency, and
`nvk_shader.c:787` shows `SET_API_MANDATED_EARLY_Z` is a real code path
on this hardware rather than a theoretical one — so some of the depth
values this log reports were read back without an ordering guarantee.

They were, as it happens, correct. That is a fact about how the copy
happened to schedule, not evidence that the test measured what it says.
`t_vk_depth-run6-pr7-rerun-PASS.log` is the same 66/66 with the correct
stage masks, and it is the log item 7 rests on.

### Logs carrying the wrong-cause memory-type note

`t_vulkan.log`, `t_vulkan-run3-l2fix.log`, `t_vulkan-PASS-20260804.log`, and
the emulator runs `t_vulkan-emu-run3.log` and `t_vulkan-emu-run5-reverted.log`
contain

```
  note memory type 0: ... — the readback rests on cache maintenance (D5)
```

which names the wrong cause: the console showed the readback rested on the
GPU-side L2 writeback, not on CPU cache maintenance. Corrected in `fc3c636`.

That `t_vulkan-PASS-20260804.log` is on this list matters — it is the log
quoted in `STATUS.md` as the Phase 4 exit criterion. **The PASS stands**; the
readback is real and the fill was verified. Only the explanatory note beside it
was wrong, and it is one line of commentary, not a check.

A log carrying `GPU-side visibility is the channel's L2 writeback` instead was
built after the correction: `t_vulkan-run5-review-fixes-PASS.log` and
`t_vulkan-run6-D16-PASS.log`.

`t_vulkan-run2.log` and `t_vulkan-run3.log` never reach that note at all —
both abort at `vkQueueSubmit` with an MMU fault (`nv 0x00000d5c`, notifier 31),
which is R18, fixed by patch 0038.

### `t_vulkan-pushdump-truncated.log`

The diagnostic build blackscreens on console and its log stops mid-MME-microcode
upload. Parked deliberately: `t_gpuwrite` localised the defect below Vulkan and
this instrument was not needed.

## Emulator runs

Files with `-emu-` in the name are **not hardware**. They are kept because the
emulator answered questions hardware did not need to be spent on — the MMU
fault not reproducing there is itself a finding — but nothing in them is a
statement about a Switch.
