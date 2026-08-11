# Hardware logs

Console runs, kept verbatim. A log here is evidence, so none of them is edited
or deleted after the fact — including the ones later found to have measured
less than they claimed. This file is where that is said, because a reader opens
the log, not `STATUS.md`.

## What the cache saves

### `t_vk_cache-run31-PASS.log`

Run 31, 2026-08-11, `2026-08-11T18:58:20.516Z e6f6165-dirty mesa:3ba5227`,
`RESULT: PASS (45/45)`. The last number Phase 7 was missing:

```
note vkCreateComputePipelines took 442 us on a warm cache
note against 5342 us when this build's cache was cold: 4900 us saved, 91% of the compile
ok   a warm cache creates the pipeline faster than a cold one (442 us against 5342 us)
```

**12× faster, 91% of the compile gone**, on the driver's own cache path with no
environment override and no `VkPipelineCache` passed to the create — so the only thing
that could have answered it is the disk. And the shader that came back is right:
`4096/4096 words match`, with the 64-word poisoned tail untouched.

**The 5342 µs is self-certified.** The cold run's own log was not kept, but the test
records a cold baseline *only* when the driver reported `0 entries` at startup, and a
cold run clears `sdmc:/mesa_shader_cache` itself before the driver opens anything. A
number in the marker file therefore cannot be a warm create wearing a cold label —
runs 29 and 29 both wrote `0` there rather than lie about it, which is what makes the
5342 in this one mean something.

## The shader off the card, correct in full

### `t_vk_cache-run30-PASS.log`

Run 30, 2026-08-11, `2026-08-11T18:46:12.351Z 28bbe35-dirty mesa:3ba5227`,
`RESULT: PASS (44/44)`. What run 29 measured and could not prove:

```
MESA: info: disk cache: sdmc:/mesa_shader_cache/nvk_012b.hzc, 2 entries
note vkCreateComputePipelines took 432 us on a warm cache
ok   the cached shader's output: 4096/4096 words match
ok   the words past the dispatch are untouched: 64/64 words are 0xdeadbeef
MESA: info: disk shader cache:  hits = 2, misses = 0
```

**Every one of the 4096 words**, from a shader NVK did not compile, plus a poisoned
tail that a wrong dispatch would have disturbed and did not. Run 29's two failures
were the test's, and this is the same test with them fixed.

**What it still does not measure is the saving.** Line 26 says it: `the driver's own
cache was already populated when this launch started`, so the run had no cold
baseline to compare 432 µs against, and the test refused to invent one — it recorded
`0` in its marker and said why. That refusal is the design working; it is also the
second run in a row where the intended cold measurement did not happen because the
driver's cache had to be deleted by hand first. The test now clears that directory
itself on a cold run.

## NVK stops recompiling, and a test that read its own result wrong

### `t_vk_cache-run29-FAIL.log`

Run 29, 2026-08-11, `2026-08-11T18:37:03.904Z b5454be-dirty mesa:3ba5227`,
`RESULT: FAIL (37/39)`. **Read the two FAIL lines as what they are: the test being
wrong about a console that was right.**

The evidence the run was actually gathering:

```
MESA: info: disk cache: sdmc:/mesa_shader_cache/nvk_012b.hzc, 2 entries
note vkCreateComputePipelines took 646 us on a warm cache
MESA: info: disk shader cache:  hits = 2, misses = 0
```

The driver's own cache, on its own path, with no environment override — **two hits
and no misses. NVK compiled nothing.** That is the thing the whole phase exists for,
and this log is the first place it appears.

The failure is `the cached shader's output: 0/64 words match`, with
`first mismatch at word 0: got 0xa5c4d00d, want 0x00000000`. The test expected
`out[id] = id`. `comp_write_id` documents in its own header that it stores
`(id * 2654435769) ^ 2781138957`, and `expect_word(0)` is exactly **0xa5c4d00d** —
the multiply vanishes at id 0 and the XOR constant is all that is left. **The cached
shader computed the correct value and the test did not know what correct was.**

The same test also dispatched 64 groups of a `LocalSize 64` shader into a 64-word
buffer: 4096 invocations, 4032 of them writing past the end of a runtime array. Both
are fixed, and the fixed test carries a static check that `expect_word(0)` is that
value, plus a 64-word poisoned tail that a wrong dispatch size would disturb.

**What this log does not establish**: that all 4096 words are right. It compared
against the wrong array and printed only the first mismatch, so exactly one word of
the shader's output is known to be correct here.

## The shader disk cache, working

### `t_shader_cache-run28-PASS.log`

Run 28, 2026-08-11, `2026-08-11T17:02:09.068Z 1f9f128-dirty mesa:3ba5227`,
`RESULT: PASS (56/56)`. The run that closed the claim a cache exists to make:

```
note C1 this launch found 32 entries (WARM — a previous launch filled it)
ok   C2 every entry the previous launch wrote came back intact
```

Entries written by one process, read back by the next, off a real SD card. It also
turned run 27's vacuous `ftruncate` check into three real ones, and re-measured the
two defects run 27 exposed: opening 201 entries went from 40498 µs to **1605 µs**
(201 → 7 µs per entry) and 100 writes into a full 32 KiB cache from **70 compactions
to 5**.

One line in this log is correct and reads like a fault. At B1:
`did not match this driver build …; started over (96 bytes discarded)` — 96 bytes is
exactly an empty header, i.e. the file the *previous* run's B7 left behind, B7 being
the check that opens under a deliberately different driver id. The driver is refusing
last run's leftovers, which is what it should do. The test has since been changed to
clear its own cache first so the message only appears where it is provoked.

What this log does **not** show is NVK not recompiling. Nothing here touches the
driver's shader compilation; `t_vk_cache` is the test for that and had not been
written when this ran.

## The shader disk cache, and a check that could not fail

### `t_shader_cache-run27-FAIL.log`

Run 27, 2026-08-11, `2026-08-11T00:07:01.401Z d0514c1-dirty mesa:3ba5227`,
`RESULT: FAIL (46/47)`. The first console run of the shader cache, and the first
evidence that `disk_cache_create()` returns anything but NULL on this platform.

**Read two lines of this log carefully, and neither is the FAIL.**

`FAIL A8 and stayed inside the ceiling` is the test's fault, not the store's:
`file_size_of()` opened the cache file a second time while the store still held it
open, which this platform refuses, and the −1 it returned was cast to `uint64_t`.

The line that matters is four lines above it and says `ok`:

```
ok   A6 ftruncate() actually shortened the file on this filesystem
```

Same −1, compared as a signed `long` against 4096. **That check could not fail**, and
it reported success for something it never looked at. `ftruncate` on this filesystem
is therefore *not* evidenced by this log, despite the `ok`. It is on the unverified
list until a run with the fixed test says otherwise.

What the log does establish: Mesa's `disk_cache_*` API working end to end on a Switch
including a 256 KiB entry; recovery from a file cut short mid-payload on the real
card; another driver build's file being reset rather than read; and an in-place
compaction reopening clean. It also carries the first latencies, two of which were
structural defects — 201 µs per entry to open, and 70 compactions for 100 writes —
both since fixed. See `docs/shader-cache.md` and `STATUS.md`.

Section C reported a cold cache, which a first run must. Nothing here shows that
entries outlive the process that wrote them.

## `VK_SUBOPTIMAL_KHR`, and the half of it a console cannot show

### `t_vk_suboptimal-run21-PASS.log`

Run 21, 2026-08-10, `2026-08-10T13:44:43.834Z 13336c0-dirty mesa:c5e9e66`,
game mode (3155 MiB), `RESULT: PASS (273/273)`. The test for patch **0074**,
which is the first change in this project to return `VK_SUBOPTIMAL_KHR` at all.

**Read the pass carefully, because it is not the whole claim.** Sections A, B,
C and E would pass on the *old* driver too: with the console undocked and
nothing resizing the layer, a backend that never returns `VK_SUBOPTIMAL_KHR`
and one that returns it only on a resize are indistinguishable. What this log
does establish is the other half — the half a wrong implementation fails:

- **2303 frames with the rule checked in both directions**, on both present
  paths, and **zero disagreements**. The rule is
  `SUBOPTIMAL ⟺ currentExtent ≠ the swapchain's imageExtent`, checked per
  frame against the surface rather than against a flag in the driver, so a
  false positive from any cause is a failure. Section D alone contributes
  **1803 frames** — 30 s of continuous presenting with the acquire and the
  present each asserted every frame.
- **The two results stay distinct.** On both paths, a superseded swapchain
  answers `VK_ERROR_OUT_OF_DATE_KHR` from acquire *and* present while the
  surface's extent is checked to be unchanged, so that result cannot be coming
  from the resize check; and an acquire that cannot be satisfied answers
  `VK_TIMEOUT` after three images were held, not a success code.
- **Eight recreation generations** alternating present path, each presenting
  its 20 frames in full, then a swapchain created after all of them were
  destroyed — which is what says the window came back.
- Nothing retained: `no slot was left with the compositor` and `the driver
  tore down every object it created`.

**The mode change itself did not happen**, and the log says so in as many
words:

```
  note D: no mode change happened (the wait expired after 1803 frames), so
       SUBOPTIMAL was never provoked and THIS COVERAGE DID NOT RUN. Nothing in
       this process can resize a VI layer; only docking the console can.
```

That is the honest boundary. The producer side of a BufferQueue can read the
consumer's default buffer size and cannot set it, so **`VK_SUBOPTIMAL_KHR` has
never been observed on hardware** — only the rule around it, and the absence of
false positives. A run in which somebody docks the console during section D is
what would close it.

**That the binary under test carried the change was checked, not assumed** —
run 11 in this directory is what a stale driver looks like. Both
`wsi_horizon_swapchain_queue_present` and
`wsi_horizon_swapchain_acquire_next_image` in `t_vk_suboptimal.elf` build
`0x3B9ACDEB` (1000001003, `VK_SUBOPTIMAL_KHR`) into `w0` with a
`mov`/`movk` pair and return it.

The build id names `mesa:c5e9e66` and `git -C mesa log` does not contain that
hash, which is not a discrepancy: it is patch 0074's commit before this run's
evidence was written into its message, and amending a message changes the hash
and nothing else. Same relationship patch 0071 records between `mesa:597ea0a`
and `mesa:85638f8` for run 20.

Two side results this run also produced, both first-time evidence:

- **Zero `vk_log*() called with client-invisible object` warnings.** Run 20's
  log carries 188 of them and patch **0073** exists to take that to zero; this
  is the first console run since, and it is zero.
- Patch **0072** (closing the framebuffer is releasing the window) is exercised
  by section C's copy-path recreation and by four of section E's generations,
  and neither the console nor `qlaunch` went down — which is the failure run 18
  produced from the same sequence.

## The framebuffer defect, proved in both directions

Five files, and they are the strongest evidence in this directory: the same
test and the same tree, one patch apart, failing and passing on demand.

### `t_vk_wsi_mt-run17-PASS.log`, `t_vk_wsi_mt-run18-fix-reverted-CRASH.log`, `t_vk_wsi_mt-run18-qlaunch-fatal-0x290.txt`

`t_vk_wsi_mt` is the multi-threaded swapchain test. Run 17 is it passing with
patch 0070 applied; run 18 is the **same binary with only that patch reverted**,
and it took the console down.

Run 17, `mesa:597ea0a`, applet mode:

```
RESULT: PASS (50/50)
```

Run 18, `mesa:3fe711d` — the revert — ends at line 53 with 29 checks and no
`RESULT` line at all:

```
  ok   A/copy: an acquire on the retired swapchain -> OUT_OF_DATE_KHR (expected VK_ERROR_OUT_OF_DATE_KHR)
```

**The absence of anything after that line is the finding.** The next statement
in the test is the `vkDestroySwapchainKHR` on the retired copy-fallback
swapchain — the call that, without 0070, reaches into a window a *different*
swapchain owns by then and disconnects it.

What happened next is not in that log, because the process did not survive to
write it. `t_vk_wsi_mt-run18-qlaunch-fatal-0x290.txt` is Atmosphère's own fatal
report:

```
Result:                          0x290 (2144-0001)
Program ID:                      0100000000001000
Process Name:                    qlaunch
Firmware:                        22.5.0 (Atmosphère 1.11.2-master-5388824be)
```

`qlaunch` is the system's home menu, **not this process**. Reading the source
predicted that the survivor would merely stop presenting — a dequeue on a
disconnected BufferQueue answers `NO_INIT`, which the acquire loop reads as
"come back later". The console's own side of the disconnect is what turned that
into a system fatal, and no amount of source reading would have said so.

### `t_vk_wsi_mt-run19-full-memory-PASS.log`

Run 17 repeated in full-memory mode (3155 MiB against applet mode's 237 MiB),
`mesa:85638f8`, which is patch 0070's own commit. `RESULT: PASS (50/50)`, and it
agrees with run 17 where it should: section C at **16613 us** against 16608, the
soak at 16731 us against 16747. So nothing in the finding depends on which
memory ceiling the homebrew was launched under.

### `t_vk_wsi_mt-run20-review-fixes-PASS.log`

`RESULT: PASS (52/52)`, full memory, `mesa:ebf2e31`. The two extra checks are
the ones the PR 9 review added to the test's own teardown. Section C at
**16607 us**; section F's mean moved from 16731 to 16882 us, which is the added
`vkQueueWaitIdle` at each of the 14 generation boundaries spread over 3000
frames.

**Two things this log is not.** It was built from a `mesa/` checkout carrying
one commit `mesa-patches/` did not have at the time — recorded as D18 in
`STATUS.md` and since closed by exporting it as patch 0071 — so the tree that
produced it was not reproducible from this repository when it was taken. And it
is the newest hardware evidence for this backend, which means it **predates
patches 0072 and 0073 and the t_vk_wsi_mt corrections from the same review**.
Its 188 `vk_log*() called with client-invisible object` lines are the
measurement 0073 exists to take to zero; its 52 checks are not the number the
corrected test will report.

## The two logs Phase 6 rests on

`t_nwindow-run11-PASS.log` (118/118) and `t_vk_swapchain-run12-PASS.log`
(117/117).

**They are two different builds** — `5995c12` and `d41e12a`, eleven minutes and
one commit apart — and `scripts/package-horizon.sh` refuses to package a
directory holding two stamps for exactly that reason. Reading them as one body
of evidence is therefore a claim about two builds, not one, and it is made here
knowingly: the commit between them (`d41e12a`) touched `STATUS.md`,
`docs/hw-logs/` and `scripts/package-horizon.sh`, and the only reason
`t_vk_swapchain` was rebuilt at all is that its driver archive had been stale.
`t_nwindow`'s binary is byte-identical across the two. Raised in review of
PR #8, and a fair hit: nothing in the pairing was checked before it was
narrated.

Between them every Phase 6 exit criterion is measured:

1. **A swapchain presents at the display's rate.** 89 of 89 intervals within
   10% of 16666 us, mean 16664 us, zero-copy.
2. **Triple buffering differs from double, in numbers — but read the whole
   line.** Under the same bursty load, two images pace at **25169 us** against
   three at **16807 us** through Vulkan (24918 against 16793 through raw
   `bq*`): double buffering takes 50% longer to deliver the same 90 frames.
   That is a throughput difference. It is **not** the burst absorption the
   tests claimed: the same lines report `0 within 10% of 16666 us` for both,
   and `45 longer than 1.5 refreshes` for both (44 and 44 in `t_nwindow`).
   Whatever three buffers did, absorbing the bursts is not it. Structurally,
   3 slots dequeued at once against 2, which stands unqualified.
3. **Two swapchains coexist over one window and destroy independently.** The
   superseded one reports `VK_ERROR_OUT_OF_DATE_KHR`; the survivor presents
   20 of 20 after the other is gone.
4. **The zero-copy decision is runtime-observable and says why.** Both paths
   named by the driver through the debug-utils messenger in one run, the copy
   fallback carrying `zero-copy was declined because
   MESA_VK_WSI_HORIZON_FORCE_COPY asked for it`.

The layout evidence is separate and human: the operator confirmed the four
bars, the border, the diagonal and the corner square in run 2, and the pattern
code has not changed since.

## The first PASS, and a second stale-artefact failure

### `t_nwindow-run11-PASS.log`

**118/118.** The sleep between dequeue rounds is the fix, and this is the log
that says so:

```
  ok   2 buffers, interval 1: 90 of 90 frames presented
  ok   under the same bursty load two buffers pace at least 10% slower than
       three (24918 us vs 16793 us)
  note slow lane, 2 buffers: frame 3 dequeued in 2324 us (2 round(s))
  ok   slow lane, 2 buffers: 10 frames presented with a three-second budget
       per dequeue (10)
```

The pacing comparison is Phase 6's second exit criterion in numbers, and it had
never run in ten attempts. The slow lane went from `frame 2 gave up after
3000094 us (23192 rounds)` to every frame in ~2.3 ms and two rounds.

### `t_vk_swapchain-run11-STALE-DRIVER-FAIL.log`

114/116, the same two-image failure — **because the driver inside it was three
days old**. `build/mesa-nvk/src/vulkan/wsi/libvulkan_wsi.a` was dated 5 August;
patch 0064 was written on the 8th and never reached it. The `.nro` was built
minutes before shipping and linked against that archive.

The build that should have produced it failed (the Docker daemon was down) and
said so, but the command running it ended in an unconditional `echo "built"`
after a filtered pipeline, so the failure was reported as a success.

The packaging staleness gate could not catch it: it asks whether an artefact is
older than the archives it links, and here the artefact was *newer*. It now
also asks the other direction — whether any tracked source under `mesa/src` is
newer than the archives — and refuses to package if one is. Broken in both
directions before being believed: touching `wsi_horizon.c` fails the gate by
name, rebuilding passes it again.

The log is kept because what it does show is real: three images at 89 of 89
intervals inside 10% of a refresh, both present paths, two coexisting
swapchains, no leak, no exit crash. Only the two-image lines are a measurement
of the wrong driver.

## We were starving the compositor

### `t_nwindow-run10-applet-starving-the-compositor-FAIL.log`, `t_nwindow-run10-full-memory-identical-FAIL.log`

The first log in which the failing dequeue reports itself instead of being
inferred from a probe that runs afterwards on a window in a different state:

```
  note 2 buffers, interval 1: the failing dequeue made 8320 round(s);
       async=true 490818 us total, last 0x0000115d;
       async=false 499026 us total, last 0x0000115d
  note 2 buffers, interval 1: THE TIME — a buffer came back after 146 us
       (0 refresh(es)), in async=true
```

Both dequeue modes were reached, both answered `NO_INIT`, and together they
account for **989 ms of the 1000 ms budget**. The loop had no idle in it: the
release event is a level, `eventWait` returned at once, and what was left was
~17000 binder transactions a second into the compositor's own service.

Then, three lines later, `async=true` returned a buffer in **146 us on its
first attempt** — after failing 8320 times in the second before. The only thing
that happened in between was a `t_note` writing one line to the SD card. Run 9
has the same shape with a different pause: `async=false` blocked for 10 ms and
delivered.

The slow lane closes it. A **three-second** budget changes nothing — 23192
rounds, same `NO_INIT`, and the two frames that did go through took **171 us
and 149 us**. More asking never helps; stopping always does.

**The producer was starving the consumer it was waiting for.** Three buffers
never showed it because a slot is nearly always free and the loop never spins.
Two buffers spin on every frame. Fixed by sleeping an eighth of a refresh
between rounds instead of calling `eventWait` on an event that cannot wait —
in `t_nwindow`, and as patch 0064 in the WSI, which supersedes 0062's slicing.

**Applet mode is not the variable.** The operator ran the same binary in applet
mode and in title-takeover (full memory) mode. The numbers are the same to
within noise: 8320 rounds against 8270, 989 ms against 990 ms in binder, 23192
rounds against 23032 in the slow lane, 146 us against 138 us for the buffer
that arrives once the asking stops.

## One buffer per second, not one per refresh

### `t_nwindow-run9-one-buffer-per-second-FAIL.log`

The diagnostic asked both dequeue modes every round and printed a time, and the
failure path used the late buffer instead of returning it. Both two-buffer
sessions say the same thing:

```
  note 2 buffers, interval 1: asked for 9 ms in BOTH modes — 1 round(s),
       release event fired 0 time(s); last async=true 0x0000115d,
       last async=false 0x00000000
  note 2 buffers, interval 1: THE TIME — a buffer came back after 9996 us
       (0 refresh(es)), in async=false
  note 2 buffers, interval 1: after the late buffer, 1 of 10 further frames
       presented
```

Read with the paced dequeue that had just given up one second earlier, that is
a rate, not a transient. One frame goes through, the next dequeue burns its
whole second, the probe that follows gets a buffer from `async=false` in ~10
ms, one more frame goes through, and it repeats. **A two-buffer window on this
compositor delivers roughly one buffer per second, where a three-buffer window
delivers one every 16 ms** — the same log has `3 buffers, interval 1: 90 of 90
frames presented`, mean 16344 us, with 87 of 90 dequeues carrying a release
fence. Two-buffer sessions carry **zero**.

Two readings still fit the 9996 us, and run 10 separates them: either
`async=false` blocks and delivers in ~10 ms and the paced loop is not reaching
it, or a buffer becomes free at about one second and the 10 ms is only how long
the ask took once it nearly was. So run 10 records what the failing second was
actually spent on — rounds, cumulative time in each mode, the last result of
each, from the loop itself rather than from a probe that runs afterwards on a
window in a different state — and runs ten frames with a **three-second**
budget, printing every dequeue's duration.

If those come back at ~1 s each, two-buffer FIFO here runs at about 1 Hz and
`minImageCount = 2` is a promise the driver cannot keep. If they come back at
~10 ms, the one-second budget was the whole defect.

## async=false does not work at t=0 and does work by t=6 s

### `t_nwindow-run8-both-modes-fail-early-FAIL.log`, `t_vk_swapchain-run8-FAIL.log`

Run 8 carried the fix run 7's evidence pointed at — `nw_dequeue` asks both
dequeue modes on every iteration — and **the failure did not move**. Two
buffers still stop at the third frame, in raw `bq*` and through Vulkan alike.

That is not a refutation of `async=false`; it is a much sharper statement of
the problem, because both logs put the two facts side by side:

```
  note 2 buffers, interval 1: asked for 5000 ms — 77900 dequeue(s) in libnx's
       mode, ... last result 0x0000115d
  note 2 buffers, interval 1: async=false returned 0x00000000 after 134 us
  FAIL 2 buffers, interval 1: 2 of 90 frames presented
  note 2 buffers, interval 1: dequeue failed at frame 2 -> 0x00006359
```

`nw_dequeue` had been asking **both** modes for the whole second before that
diagnostic ran, and both failed for the whole second. Then five seconds of
`async=true` went by, and one `async=false` succeeded in 134 us. So
`async=false` does not work at t = 0 and does work by t = 6 s, and a single
late attempt cannot say which second in between it started working in.

The question is therefore no longer which mode to ask in. It is **when a buffer
becomes free at all on a two-buffer window**, and whether the window keeps
going once one does. Both are measured from run 9: the diagnostic asks both
modes every round and prints the elapsed time and the winning mode, and then
attempts ten more frames and counts them — a startup transient and a permanent
stall look identical in every log so far.

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
