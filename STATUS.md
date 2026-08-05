# STATUS

**Last updated:** 2026-08-05
**Branch:** `claude/phase-6-horizon-wsi-5hu1jt`

---

## Current state — read this first

*Everything below this section is the working record: dated, append-mostly, and
long. This block is the state itself, and it is the part that must be true.*

| | |
|---|---|
| **Phase** | **Phase 6 has run twice. Three of four exit criteria are met and three defects are open**, two of them fixed here and unrun (patches 0060, 0061). Run 2 (2026-08-05) confirmed the swapchain — **89 of 89 intervals inside 10% of a 60 Hz refresh**, zero-copy chosen by the driver — and **the pattern was reported correct by the operator**, which is the layout evidence this phase could get no other way. It also showed my run-1 leak fix making the leak worse, and a check of mine passing while 33 objects leaked |
| **What runs on a Switch** | *Run 2, 2026-08-05.* **A VK_KHR_swapchain presenting through the zero-copy path**: `vkCreateViSurfaceNN` over the default window, 90 frames at **mean 16671 us with 89 of 89 intervals within 10% of a refresh**; 120 pattern frames whose four bars, border, diagonal and corner square the operator confirmed; two swapchains coexisting over one window, the superseded one reporting `VK_ERROR_OUT_OF_DATE_KHR`, the survivor presenting 20/20 after the other is destroyed; and the same application taking either present path on request. `t_nwindow`: **3 of 3 registered buffers dequeued at once, and 2 of 2**. Run 1 also re-ran all thirteen Phase 5 tests against the changed submit path, all PASS |
| **Next concrete task** | **Run 3: `t_nwindow` and `t_vk_swapchain` with patches 0060 and 0061.** They answer, in order: does the two-image swapchain present at all (0061), does anything still leak (0060), and — only once nothing leaks — **is the exit crash caused by the leak or not**. Run 2 could not answer the last one because the leak was still there, worse |
| **Known failures** | **1. `t_vk_swapchain` crashes the console on exit**, both runs, after `RESULT` is written and on pressing +. **Still untested as a hypothesis**: run 2 was meant to test whether the leak causes it, and the leak was still present, so nothing was learned. **2. FIXED, unrun:** the run-1 leak fix (0058) created a reference cycle — every `nvkmd_mem` carries its own VA, and referencing the memory from that binding makes the two hold each other. 14 leaked objects became 33, with 33 VAs and 33 mappings. Patch 0060 excludes the self-binding. **3. FIXED, unrun:** every two-image swapchain still failed, now with `VK_TIMEOUT` instead of an error: an async dequeue on a two-buffer queue never succeeds, so retrying it just burns the deadline. Patch 0061 dequeues blocking once the release event has fired, which is what libnx does. **4.** `t_fault` still takes the console down on exit. **5.** `t_vk_texture`'s one unexplained occurrence stays on the record |
| **The check that lied, and it was mine** | `t_log_scan`'s predecessor read the log back to fail the test if the driver said it could not destroy something. It opened the file a second time while it was still open for writing, the SD card's device layer refused, and the helper answered "not found" — so run 2 printed **"ok the driver tore down every object it created"** four lines after `device destroy refused: live mem=33`. It now reads through the log's own handle (`"w+"`) and returns *whether the scan happened* separately from what it found; a scan that could not run fails the test. This is the third time in this project a check has reported success without verifying anything, and the first one I wrote knowing that history |
| **Exit criteria** | **1. Met.** 89/89 intervals within 10% of 16666 us. **2. Half met, twice.** The structural half is measured — 3 concurrent slots against 2 — and the pacing comparison has still never run, because both two-image runs died. Patch 0061 is what unblocks it. **3. Met.** **4. Met**, both paths named by the driver through the debug-utils messenger in the same run |
| **What is still unverified** | Whether the exit crash is the leak. Any display mode change. Anything multi-threaded. `VK_PRESENT_MODE_IMMEDIATE_KHR` through Vulkan — `t_nwindow` measured the swap interval underneath it at **8152 us against 16352 us** |
| **Open, not blocking** | Two unconditional L2 operations per submit. The acquire's CPU wait, now quantified: **acquire mean 15712 us of a 16671 us frame** on the zero-copy path against **5 us** on the copy path, where the wait sits in the present instead |
| **Open decisions** | **D7 only** |
| **Never verified on hardware** | Patches **0060** and **0061** and the binaries built with them; `t_fault` as it stands |


---

## Run 2 — the swapchain holds up, my fix does not (2026-08-05)

**Class: hardware (HW).** `t_nwindow` 48/50 and `t_vk_swapchain`
114/116. Logs in `docs/hw-logs/*run2*`.

### What was confirmed

**The pattern was right.** Reported by the operator: the four bars, the
white border, the black diagonal and the yellow corner all appeared as
the log said they should. That is the whole of the evidence that NVK's
block-linear layout and the compositor's agree — every other frame this
suite presents is a solid colour, which looks identical under any wrong
swizzle, and a GPU readback would write and read with the same layout
and agree with itself. **`TegraColor` was the right GOB ordering, the
generic 16Bx2 page-table kind was the right kind, and the stride and
block height are what the display block expects.**

**And the swapchain paces exactly.** 89 of 89 intervals within 10% of
16666 us, mean 16671 us. Both present paths named by the driver in the
same run.

### Three findings, and two of them are corrections to run 1's fixes

**1. The leak fix made the leak worse.** Patch 0058 gave every VA
binding a reference to the memory it binds. But every `nvkmd_mem` here
carries a VA of its own, bound at offset 0 the moment it is created, so
referencing the memory from *that* binding makes the two objects hold
each other:

```
run 1:  device destroy refused: live mem=14 va_ranges=0  mappings=0
run 2:  device destroy refused: live mem=33 va_ranges=33 mappings=33
```

The VAs leaking too is the signature. Patch 0060 excludes the
self-binding, which `_va == _mem->va` identifies exactly — alloc_mem
assigns `mem->va` and then binds, so the comparison is a fact at that
moment and not an assumption about ordering.

**2. Retrying NO_INIT was not enough.** Patch 0059 added it to the
retryable set, and the two-image runs then spun until their deadline
and returned `VK_TIMEOUT` instead. The mode is the problem, not the
retry: libnx passes the producer's `async` flag as true when it has a
release event to wait on and false when it does not, and async mode
asks the queue to keep a buffer in reserve — which a two-buffer queue
cannot do. Patch 0061 probes non-blocking, then dequeues **blocking**
once the release event has fired. That is libnx's own loop.

**3. And a check of mine reported success while 33 objects leaked.**

```
[horizon_gpu:E] device destroy refused: live mem=33 va_ranges=33 mappings=33
MESA: error: horizon_gpu_device_destroy failed: status 9
ok   the driver tore down every object it created: nothing in this log says it could not
```

`t_log_contains` opened the log a second time while it was still open
for writing; the SD card's device layer refused, and the helper's one
return value could not tell "not found" from "could not look", so it
said not found. It is `t_log_scan` now: it reads through the log's own
handle, and it returns whether the scan happened separately from what it
found. A scan that could not run fails the test.

**This is the third time this project has produced a check that reports
success without verifying anything**, and the first one written by
someone who had just finished writing that sentence about the other two.
The pattern is the same every time: a helper with one return value for
two different questions.

### The exit crash learned nothing

Run 2 was meant to test whether the crash follows the leak. The leak was
still there — worse — so the experiment did not run. It is still
untested, and run 3 is the first time it can be.


---

## Phase 6 on the console — three criteria met, three defects found (2026-08-05)

**Class: hardware (HW).** Fifteen `.nro` in the order given, on the
owner's Switch. **Thirteen PASS, two FAIL, 2986 checks.** Logs in
`docs/hw-logs/*phase6*`.

### A Vulkan swapchain presents on a Switch

```
note vk info [wsi_horizon.c:1750]: wsi_horizon: zero-copy: the swapchain
     images are the scanout buffers (3 images, 1280x720, swap interval 1)
ok   3 images, FIFO: 90 of 90 frames presented
note 3 images, FIFO: 89 intervals, mean 16662 us, min 16150 us,
     max 17238 us; 89 within 10% of 16666 us; 0 longer than 1.5 refreshes
ok   90 frames were presented through 3 images, so the compositor
     released at least 87 of them
```

**Zero-copy on the first attempt.** The alignment failure predicted in
the previous section did not happen: `nwindowConfigureBuffer` accepted
memory aligned to whatever NIL asked for, so libnx's 128 KiB is a habit
and not a requirement — at least for a buffer of this size and shape.
The page-table kind and the GOB ordering both passed, which means NIL
classified GM20B as an SoC and produced `TegraColor`, as
`nvk_wsi_get_image_info` requires.

**89 of 89 intervals inside 10% of a 60 Hz refresh** is exit criterion 1,
and there is no weaker reading of it: the swapchain has three images, 90
frames went through them, and this process releases none — the
compositor released at least 87.

### THE NUMBER, and it is larger than the design needed

```
note THE NUMBER: with 3 registered buffers, 3 slot(s) could be dequeued at once
note with 2 registered buffers, 2 slot(s) could be dequeued at once
```

The BufferQueue hands the producer **every** buffer it registered. So
the single-`cur_slot` restriction is libnx's `NWindow` wrapper and not
the platform's, and driving `bq*` directly — which the whole slot-
ownership design rests on — is sound. `minImageCount = 2` is safe.

### Three defects, and where each came from

**1. Every swapchain image leaked.** Fourteen of them, and then the
device itself:

```
[horizon_gpu:E] mem 0x...: destroy refused, 1 live mapping(s) at va=0x400000000
MESA: error: horizon_gpu_mem_destroy failed: status 7
[horizon_gpu:E] device destroy refused: live mem=14 va_ranges=0 mappings=0 channels=0
```

`wsi_destroy_image` frees the memory **before** destroying the image
(`wsi_common.c`), which Vulkan permits and Mesa has always done;
`horizon_gpu_mem_destroy` refuses while a mapping is alive
(memory-model § 7), and `nvkmd_mem_unref` returns void, so nothing
retried. The final counters are what fix the order: `va_ranges=0
mappings=0` means the unbind *did* happen — later, when the image was
destroyed — so what was left was memory whose free had already been
refused. Patch 0058 makes the binding hold a reference; whichever of the
two comes last destroys the object, and the order stops mattering.

**No Phase 5 test showed this**, because they destroy the image first.
It is a defect any application that frees memory before its images would
hit, and the swapchain is simply what did it.

**2. Every two-image swapchain died at frame 2.**

```
MESA: error: wsi_horizon: bqDequeueBuffer failed: 0x0000115d
FAIL 2 images, FIFO: 2 of 90 frames presented
```

`0x115d` is `Module_LibnxBinder`, `LibnxBinderError_NoInit` — Android's
`NO_INIT` (`-ENODEV`) through `binderConvertErrorCode`. The acquire
retried only on `WOULD_BLOCK`, which is what libnx's own loop expects.
Three images never reach the condition, so nothing before this run had
seen it. `t_nwindow` failed identically at frame 2, which is what says
the fault is in the retry set and not in the swapchain. Patch 0059.

**The cost of this one is exit criterion 2**: the pacing comparison
between two and three images has no data in this run. Its structural
half — 3 concurrent slots against 2 — did measure.

**3. `t_vk_swapchain` takes the console down on exit**, reported by the
owner, after `RESULT` is written and on pressing +. The cause is not
established. The hypothesis is defect 1: with the device destroy
refused, `nvExit`, `nvMapExit` and `nvFenceExit` never ran and fourteen
NvMaps were alive when the process ended. `t_nwindow` registers and
releases the same kind of scanout buffers, tears everything down
cleanly, and exits fine — which is what makes the hypothesis worth
testing rather than assuming. Patch 0058 removes the condition; the
re-run is the test.

### And a check that would have caught the first one

`t_log_contains` in `testfw`: the test reads back its own log — which
`main()` has dup2'd stderr into — and fails if the driver said it could
not destroy something. Some of what a driver reports has no Vulkan
representation at all, and until now a test could only pass beside it.
This run is the reason it exists.

### The numbers worth keeping

| | |
|---|---|
| swapchain, 3 images, FIFO | mean **16662 us**, 89/89 within 10% of a refresh |
| acquire, zero-copy | mean **15729 us** — the frame's wait is here |
| acquire, copy fallback | mean **4 us** — the wait is in the present instead |
| record + submit | **584 us** zero-copy, **470 us** copy |
| `t_nwindow`, 3 buffers, interval 1 | mean **16380 us**, 86/89 within 10% |
| `t_nwindow`, 3 buffers, interval 0 | mean **8162 us** — the swap interval is the knob |
| `t_nwindow`, fill + flush of one 3.75 MB buffer by CPU | **2173 us** |
| release fences seen | **87 of 90** dequeues, first on syncpoint 103 |


---

## Phase 6 — the Horizon WSI, written and unrun (2026-08-05)

**Class: cross build (CB).** Everything below builds and nothing below
has executed. The distinction matters more in this phase than in any
previous one, because a swapchain's output cannot be read back: a test
that passes here proves less per check than a Phase 5 test did, and the
gap is made up by evidence of other kinds, listed in the state block
above under what the suite does not measure.

### The question that had to be answered before anything was designed

libnx's `nwindowDequeueBuffer` keeps a single `cur_slot` and refuses a
second dequeue before the first is queued or cancelled. Vulkan does not
permit that: with `VkSurfaceCapabilitiesKHR::minImageCount` = 2 and a
three-image swapchain, an application may hold **two** images acquired,
and blocking indefinitely for them is valid usage. Offering
`minImageCount = 2` — which is what makes double buffering expressible
at all, and therefore what exit criterion 2 rests on — makes that case
legal.

So either the BufferQueue underneath libnx hands out two slots at once,
or `docs/wsi.md` § 2.3 cannot stand as written. `t_nwindow` measures the
number, with no Vulkan anywhere near it, and the WSI backend drives
libnx's `bq*` producer API directly on that basis — public API, on
`nw->bq`, a public field — keeping the slot ownership itself.
Registration and teardown still go through `nwindowConfigureBuffer` and
`nwindowReleaseBuffers`, and `nw->cur_slot` is neither read nor written.

**If the answer comes back 1, the design changes and this paragraph is
what says so.**

### Six things the design got wrong, corrected in `docs/wsi.md`

Each one is marked in the document with what disproved it:

| § | The design said | What it is |
|---|---|---|
| 2.2 | four slot states, including RENDERING | three: nothing at this layer can see an application start to render |
| 2.3 | `nwindowSetBufferCount` is called | there is no such function in libnx, and the criticism of the reference for not calling it is withdrawn |
| 2.5 | eviction releases the old swapchain's buffers slot by slot | a disconnect, which frees them all; a per-slot cancel would hand back buffers the application is still rendering into |
| 3.1 | zero-copy needs `WSI_SWAPCHAIN_NO_BLIT` | done, and the part the design did not say is how the WSI learns an OPTIMAL image's layout: two driver callbacks and two new nvkmd operations |
| 4 | the release fence becomes a wait dependency of the application's submit | a CPU wait inside acquire, because `nvkmd_horizon_ctx_wait` does waits on the CPU and the dependency would move the stall rather than remove it |
| 5 | a dimension change is detected at acquire | it is, but not through `nwindowGetDimensions` — that returns what this backend itself set, so the check compared the extent with itself |

### The GOB check that would have passed for the wrong reason

NIL picks the sector ordering inside a GOB from the **device type**: an
SoC gets `TegraColor`, a desktop Fermi gets `FermiColor`
(`nil/tiling.rs:144-151`). Both are 512-byte, 64x8 GOBs, so accepting
the wrong one passes every size, stride and block-height check in the
backend and puts a scrambled image on screen. `TegraColor` is what
Horizon's display block reads as "generic 16Bx2", and it is the only
one `nvk_wsi_get_image_info` accepts.

This is the finding this phase would most like to have caught with a
test and cannot: see the pattern in `t_vk_swapchain` section F, and the
reason a GPU readback proves nothing about it.

### The one change to the submit path, and why it is not an optimisation

`nvkmd_horizon_ctx_wait` performs a submit's waits on the CPU. For a
wait on work **already submitted to the same channel** that is not a
limitation but a wait that has already happened: a GPFIFO executes its
entries in order, so the submit that waits cannot begin before the work
it waits on has finished, whether or not anyone waits for it.

It is the ordinary case and not a corner. A swapchain present waits on
the semaphore the application's render submit signalled, on the same
queue, every frame — so with the CPU wait in place, handing the
compositor a syncpoint fence to wait on saved nothing, and patch 0052
would have been decoration.

The bound is exact: only a sync that is PENDING and whose fence came
from this same channel. What it costs is stated rather than discovered —
a wait on a faulted channel no longer reports `CHANNEL_LOST` from the
wait; the error moves to the submit that follows, which `t_fault`
measured on hardware as refused.

**This is why the Phase 5 suite is in the batch.**

### The Phase 6 batch, in order

Every test writes `sdmc:/horizon_gpu_tests/<name>.log`. The two new ones
own the display, so **they start no console and the screen shows what
they present**; everything they report is in the log file.

| # | Test | What to expect on screen | What its log is for |
|---|---|---|---|
| 1 | `t_nwindow` | about nine seconds of solid colours sweeping through hues, five times over | **THE NUMBER**: "with 3 registered buffers, N slot(s) could be dequeued at once". Also the frame cadence at 2 and 3 buffers, with and without a bursty load, and at swap interval 0 |
| 2 | `t_vk_swapchain` | a **still pattern for two seconds** — four vertical bars red/green/blue/white, a white border, a black diagonal, a yellow top-left square — then about twelve seconds of colour sweeps | the four exit criteria. **Report what the pattern looked like**: it is the only evidence that the driver and the compositor agree about the memory layout |
| 3-15 | the Phase 5 suite, in its usual order | the console, as before | patch 0056 moved the submit path under all of them |
| last | `t_fault` | it takes the console down on exit; run it last or not at all | unchanged debt |

If `t_nwindow` reports fewer than two concurrent slots, stop: everything
after it is measuring a design that has to change.

### The most likely way zero-copy gets declined on first contact

Written down before the run so the log can be read against it rather
than explained afterwards. libnx creates its framebuffers' NvMap with
`nvMapCreate(..., align = 0x20000, ...)` — 128 KiB — while a swapchain
image's memory is allocated with whatever alignment NVK's memory
requirements ask for, which is NIL's and much smaller. Whether the
display block needs the larger one is not known here; nothing in
switchbrew says, and libnx's choice may be a requirement or a habit.

If it is a requirement, `nwindowConfigureBuffer` fails and the backend
falls back to the copy path with "the compositor refused one of the
scanout buffers" in the log. That is the designed outcome and not a
crash, which is the point of validating and falling back rather than
predicting. The fix, if it is needed, is a larger allocation alignment
for scanout images, and it is one patch.

The other candidates, in the order they would show: a page-table kind
that is not generic 16Bx2 (would mean compression is on, which
`nvkmd_info::has_compression` says it is not), and a GOB sector ordering
that is not TegraColor (would mean NIL classified this chip as something
other than an SoC).


---

## The re-run: the corrected binaries on a console (2026-08-05)

**Class: verified on real hardware.** Thirteen `.nro`, in the order they
were given, on the owner's Switch. **Thirteen PASS, 2942 checks, no
failure and no crash.** Logs in `docs/hw-logs/*-pr7-rerun-PASS.log`.

| test | this run | previous | what moved |
|---|---|---|---|
| `t_vulkan` | **62/62** | 62/62 | — |
| `t_submit` | **32/32** | 30/30 | +2, the two overflow refusals below |
| `t_pbsize` | **77/77** | 69/69 | +8, the odd rungs and the payload assertion |
| `t_display` | **3/3** | 3/3 | — |
| `t_vk_compute` | **37/37** | 37/37 | — |
| `t_vk_transfer` | **203/203** | 202/202 | +1, the spill check |
| `t_vk_image` | **72/72** | 72/72 | — |
| `t_vk_triangle` | **84/84** | 84/84 | — |
| `t_vk_texture` | **1685/1685** | 1685/1685 | — |
| `t_vk_depth` | **66/66** | 66/66 | the barrier under it, not the count |
| `t_vk_format` | **282/282** | 282/282 | — |
| `t_vk_caps` | **52/52** | 52/52 | — |
| `t_vk_submits` | **287/287** | 287/287 | — |

The three counts that changed changed by exactly what the review said
they would, which is the point of predicting them:

- **`t_submit` +2.** `a span count of UINT32_MAX is refused, not
  wrapped` and `one span more than the queue can hold with its own two
  entries is refused`. The guard that was written to stop an overflow
  overflowed; both directions now execute on hardware and both refuse.
- **`t_pbsize` +8.** The rungs were one dword short of the size they
  named — `filler = dwords - 5` is odd, `pairs = filler / 2` truncates —
  so D15's 524288-dword boundary was never actually tested at 524288.
  The rungs are odd now and the test asserts the total it encoded. The
  log's last rung reads `524289 dwords: the release at the END of the
  entry ran (0x5a5a0800)`: **no entry-size limit exists in this range**,
  which is the answer D15 wanted and did not previously have.
- **`t_vk_transfer` +1.** `F: 0 of 9 probes wrote outside their region`.

### Item 7's asterisk is gone

`t_vk_depth` was the one item whose evidence carried a caveat: its
`srcStageMask` gave `LATE_FRAGMENT_TESTS` alone and omitted
`EARLY_FRAGMENT_TESTS`, leaving any depth write done in the early stage
outside the dependency — and `nvk_shader.c:787` shows
`SET_API_MANDATED_EARLY_Z` is a real code path here, so the risk was not
theoretical. With both stages in the mask all seven content checks land:

```
  ok   colour: 4096/4096 pixels hold what the depth test left there
  ok   depth: 4096/4096 pixels hold the value the surviving fragment wrote
  ok   the draw behind everything was rejected at every pixel (0 blue)
  ok   nothing is left of the far draw (0 red)
  ok   every pixel was drawn to (0 still the clear colour)
  ok   the draw with depth writes off left the depth buffer alone (0 pixels hold 0.5)
  ok   every pixel's depth was written by some draw (0 still 1.0)
```

A two-submit variant — render, fence, then read back, so the ordering
would rest on a fence rather than on a stage mask — was designed while
these logs were outstanding and **was never written**: the barrier is
now correct and measured correct, and swapping a working mechanism for
a different one after the fact would replace evidence with a change.
The option is recorded here rather than in code, which is where it
belongs until something needs it.

### What this run does not say

- **`t_fault` did not run.** It was left out on purpose — it takes the
  console down on exit, so it would have ended the session and
  everything ordered after it. It is the one binary in this tree whose
  current build has never run on a console, and the `atexit` marker
  built to localise the crash is still unexecuted.
- **The texture anomaly is still unreproduced, not explained.** This run
  adds 24 more independent builds of the exact 8x8 optimal configuration
  that once returned zeros (16 `run 1 shape` + 8 `checked after`), all
  correct, largest deviation 1 of 255. That is 56 attempts against one
  occurrence. The diagnostic machinery stays in the test.
- **115 us per serialised round trip**, against 126 us on the previous
  build. Nothing between the two runs was aimed at that number and no
  experiment isolates it, so it is a datum, not a result.

The logs were checked to be a fresh run rather than a re-zip of the
previous one: `t_vk_texture.log` is the same length as run 8's to the
byte — the test is deterministic — but its channel addresses and
syncpoint baselines differ (`0x25d6b306e0`/`48366` against
`0x6b57f306e0`/`28324`), which only a different boot produces.

---

## Second review of PR #7 — thirty findings, thirty real (2026-08-04)

**Class: cross build (CB).** A full adversarial pass over the whole
branch, and nothing in it was wrong. Grouped by what it says about this
tree rather than by file.

### The fault fix was applied to one of three paths

`horizon_gpu_channel_wait_fence` got the notifier check. The premise —
nvgpu force-increments a faulted channel's syncpoints, so the counter
says "finished" for work that never ran — applies just as exactly to
`horizon_gpu_channel_reap`, which **reads that counter and fires
retirement callbacks on it**, recycling buffers for abandoned work and
returning `ok`. It is on the submit hot path, and `wait_idle`'s return
value comes from it. Fixed: reap consults the notifier before the
syncpoint, returns `CHANNEL_LOST`, and runs nothing.

`horizon_gpu_channel_get_error` had the latch in one branch of two. The
success path with a never-fired notification answered "none" for a
latched, lost channel — the exact bug the latch was added for, in the
other half of the same function.

Both headers understated their contracts. `wait_fence` now says in the
header what it does: **it returns `CHANNEL_LOST` for a fence that
genuinely reached its threshold**, which every caller sees, and no
header said.

### The L2 prologue, and the cost that was asserted

Three things. The justification is single-channel and
`L2_SYSMEM_INVALIDATE` is device-global, with nothing said about what a
prologue invalidate on one channel does to another channel's in-flight
work — and this suite runs multi-channel. The cost is *asserted*
("the same shape and order of cost as the writeback that has always
been there") in a file where every other claim carries a number. And
the flush passed `CHANNEL_PROLOGUE_CMDS_OFFSET + n*4` where a length
belongs — correct only because the fence block is at zero, unpinned by
any `_Static_assert` while every other layout relationship in that file
has one.

The length is now computed as a length with an assertion behind it. The
two open ones are recorded in the state block rather than answered:
**two L2 operations per submit, not one**, and a queue depth
permanently 0x7fe rather than 0x800.

And the `num_spans` guard's stated rationale was wrong — it claimed an
unbounded count would read past the caller's array, which it would not;
the wrap is the real and sufficient reason. A guard with a wrong reason
invites the next person to weaken it.

### The fixture, which every failure line goes through

`vkfw_result_str`'s header promised "falls back to the number" and
returned the literal string `"VkResult"`, so every result outside its
list logged with the code discarded. `vkfw_submit_and_wait` destroyed
the fence unconditionally — including after `VK_TIMEOUT`, when a
pending submit still owns it, which is invalid usage triggering exactly
on a hang. `vkfw_finish` tore down the pool and device with no
`vkDeviceWaitIdle`, on a fixture that deliberately exposes non-waiting
submit. All three fixed.

`vkfw_expect_words` takes a word count and no size, and four of the six
findings in the *previous* review were readback checks measuring the
wrong extent — all of them through this helper. It cannot validate what
it is given; the header now says so instead of leaving it to be
discovered.

### The display path, which Phase 6 depends on

Dropping `consoleUpdate` dropped the vsync block with it, so the exit
loop was an **unthrottled spin on a core** until somebody pressed + —
on the one path `t_display` exists to validate. The "Press + to exit"
prompt went to a `printf` with no console behind it, so a console-less
run left the operator no indication it had finished. `t_display` passed
`(u32 *)&w` for a `u64`, which reads correctly only by little-endian
accident and is what CLAUDE.md's explicit-width rule is for. And its
first check asserted a constant defined two lines above it: it could
not fail, and it did not test the thing its own message claimed. It now
asks the console whether it was initialised.

### Whether the depth finding could have affected item 7's result — checked

The review said `t_vk_depth`'s barrier named only `LATE_FRAGMENT_TESTS`
while the hardware may write depth in the early stage. Confirmed in the
driver rather than assumed: `nvk_shader.c:787` sets
`SET_API_MANDATED_EARLY_Z` from the shader's `EarlyFragmentTests`
execution mode, which `depth_frag_pc` does not declare. That disables
only *mandated* early-Z; opportunistic early-Z stays available, and a
fragment shader that writes no depth and never discards is exactly the
case a Maxwell does it for.

So the risk was real and not theoretical. **The values were right —
4096/4096 depth texels — but the barrier did not guarantee they would
be.** Item 7's evidence is a correct answer that was not synchronised
to be correct, which is the same class as a test that passes for the
wrong reason, and only a re-run makes it unconditional.

**The other four of the five are not in that position.** Items 5, 6, 8
and 9 changed only through `vkfw` and `testfw`, and every change there
is on a failure or teardown path — the fence that must not be destroyed
after a timeout, the drain before teardown, the result string. None of
those runs timed out or took a failure path; every wait in their logs
is `VK_SUCCESS`. Their measurements stand as made.

### Gates that did not exist, for defects already found by eye

Nothing compared the Makefile's `TESTS` against meson's
`horizon_tests` — the one list that had just diverged, found by a
reviewer rather than by a gate. Comparison 6b now does, broken to
confirm it fails.

`docs/hw-logs/` — 41 files, the evidence every Phase 5 claim rests on —
had no manifest and no check, while the narrative *about* that evidence
had both. Now covered, and `--check` no longer looks only at `*.md`, so
a record added with any other name is visible.

**And the manifests are described honestly now.** They are a
change-*declaration* mechanism, not an integrity guarantee: an edit
declared in the same commit passes by design. The pre-split digest is a
provenance note, not a check — the file it describes is gone and the
reassembly cannot be re-run. `phase-4-narrative.md` is the one history
file the script did not cut; it was lifted out by hand afterwards and
its provenance is that commit, which the script now says.

**CI now exists for the half that needs nothing.**
`.github/workflows/gates.yml` runs history-intact, test parity,
layering, no-abs-paths and the host tests on every push and pull
request — bash, python and a C compiler, no container. That makes
`split-status.py`'s promise true: an undeclared edit to the record now
fails something other than somebody's memory.

**The other half is named, not pretended.** `check-dispatch-complete`
reads a linked Horizon ELF and `check-tls-relocs` scans devkitA64
objects; both need the cross-toolchain image, which is a container pull
and minutes per run. They stay manual until the owner decides that cost
is worth paying. The workflow file says so and can be deleted without
anything depending on it.

### The patches

0048 carried none of the four trailers `mesa-patches/README.md` calls
mandatory, under a README saying "Evidence is not optional" — and it is
the one patch here that changes `nvk_CreateDevice` for every device on
the platform. It has them now, with the hardware evidence. Its subject
was a bare `[PATCH]`; it is `[PATCH 48/48]`.

0045, 0046 and 0047 all still said "Not run on a console" while this
same PR ships the logs of those runs. Corrected to say what was
measured and where the log is.

And 0040 — the revert of the window block-off — reasoned from an
emulator failure to a claim about the hardware that `t_va_window` later
disproved. It stays in the series, because the series is the record of
what was believed and when, but the correction is now in its message so
a rebase or an upstream submission carries it with the claim.

### What this review is evidence of

The first pass found six; this one found thirty and every one held. The
pattern across both is the same and it is worth naming: **the failures
are in the things that report, not in the things that compute.** A
guard whose rationale was wrong, a latch in one branch of two, a
promise in a header the code did not keep, a state block whose summary
row contradicted its own body, gates for defects already found by eye.
The GPU work has been right; the machinery that says whether it is
right has been where the defects are.

Not run on hardware. Everything in the suite changed.


## Codex review of PR #7 — six findings, six real (2026-08-04)

**Class: cross build (CB).** Every one held up against the code. Two of
them are defects in shipped behaviour, four are tests that were
measuring less than they claimed.

| where | finding | verdict |
|---|---|---|
| `horizon/submit/submit.c` | `num_spans + 2` wraps | **real, and the sharpest** |
| `tests/t_pbsize.c` | every rung was one dword short of its name | **real** |
| `tests/t_vk_caps.c` | the DRM modifier extension was never enabled | **real** |
| `tests/t_vk_depth.c` | the depth barrier missed EARLY_FRAGMENT_TESTS | **real** |
| `Makefile` | three standalone tests only exist on the Meson path | **real** |
| `tests/t_vk_transfer.c` | case C checked the prefix and not the suffix | **real** |

### The overflow, which is the one that stings

    if (num_spans + 2 > GPFIFO_QUEUE_SIZE)

`num_spans` is `uint32_t`, so `UINT32_MAX + 2` is 1, the guard passes,
and the validation loop below walks four billion entries of an array
with one in it. **The check written to stop an overflow overflowed** —
and the comment above it says, in as many words, that it exists so the
arithmetic further down cannot "wrap silently (CLAUDE.md:
overflow-check every size computation)".

Now `num_spans > GPFIFO_QUEUE_SIZE - 2`, with a `_Static_assert` that
the queue is at least two entries so the subtraction cannot underflow,
and a note at the second sum saying it is safe *because of* this bound
rather than on its own. `t_submit` gained the regression: a count of
`UINT32_MAX` must be refused, and so must `GPFIFO_QUEUE_SIZE - 1`,
which is the boundary either side of the real limit. On the old code
the first of those did not return.

### The rungs were a dword short, every one of them

`t_pbsize`'s release is five dwords and its filler comes in NOP pairs
of two, so `filler = dwords - 5` was odd and `pairs = filler / 2`
truncated. Every entry was `dwords - 1`, and the log said `dwords`. The
log from the hardware run says it plainly once you look:
`32 dwords: 13 NOP pair(s) encoded (26 dwords)` — 26 + 5 = 31.

**The 524288-dword boundary D15 reports was never tested at 524288.**
The conclusion survives — a limit does not sit one dword below a power
of two — but the number in the record was not the number measured. The
rungs are now odd (33, 129, … 524289) so the arithmetic is exact, and
`run_rung` asserts the entry is the size it names before submitting.

### The extension was advertised, checked, and never enabled

`t_vk_caps` created a `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT` image
on a device where `vkfw_init` had enabled no extensions at all. That is
invalid usage, and with no validation layers it is a wrong answer
rather than an error: the test passed while measuring something other
than the enabled-extension path patches 0045 and 0046 are about.
`vkfw_init_ext` now takes the list, and the test enables the one it
uses.

### The depth barrier waited on the wrong half of the pipeline

None of `t_vk_depth`'s shaders writes `gl_FragDepth` or discards, so
the implementation may do the depth test and write in the **early**
fragment-test stage — and the barrier before the copy named only
`LATE_FRAGMENT_TESTS`. It passed on this hardware, which is exactly how
a synchronisation bug survives. Both stages now.

### Two build paths, one of them missing three tests

`t_fault`, `t_pbsize` and `t_display` went into `meson.build` and not
into the `Makefile`'s `TESTS`, so `scripts/build-switch.sh` — a
supported path, and the one whose output was verified on hardware
first — never built or packaged them. Added.

### And a bounds check with one side

`t_vk_transfer`'s case C poisoned the whole destination and then
verified only the words *before* the `vkCmdUpdateBuffer` range. An
implementation writing past the end passed. That is the same shape as
the L2 defect this suite found in item 1, and the neighbouring case B
checks both sides. The suffix check is there now.

**What this review is evidence of.** Four of the six are tests that
reported success while measuring less than they said — the failure mode
this project keeps naming and keeps producing anyway. The two in
shipped code were both in guards: one that overflowed, one that
synchronised half of what it needed to. Worth writing down that the
adversarial pass found things the author's own passes did not.

Not run on hardware. `t_submit`, `t_pbsize`, `t_vk_caps`, `t_vk_depth`
and `t_vk_transfer` all changed and all need a re-run.


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

### And the settle changed nothing, so it is the exit path itself

Confirmed by the owner: with two seconds of slack between the fault and
the teardown, it still crashes on `+`. So it is not a simple race with
nvgpu's recovery either.

**What is established:**

| | |
|---|---|
| the fence/notifier fix | works — a real fault reaches it and the wait says lost |
| teardown after a fault | clean: reservation, channel and device all `ok` |
| the `nv` session | survives completely; a second device, channel and submit all work |
| a 2 s settle | changes nothing |
| the crash | is after every line the process reports, in the exit path |

**What is not established** is which exit step, and that is the only
thing left. `horizon_gpu_device_destroy` calls `nvFenceExit()` and
`nvExit()`, both reference-counted by libnx and balanced here, so it is
not a double-exit. What remains is `consoleExit`, the return out of
`main`, and `__appExit` — which for this process means handing the GPU
back to a compositor that shares it.

An `atexit` marker is now built into `t_fault` so the next person to
run it gets that answer for free: present means the crash is in
`__appExit` or later, absent means it is earlier. It is not worth a
reboot of its own.

**Proportion, stated as a decision.** This is a defect that only a
process which deliberately MMU-faults the GPU can reach. It does not
affect any correct workload, the fence/notifier fix it was built to
verify is verified, and every remaining question costs the owner a
console reboot for one bit. So it is recorded, instrumented, and left —
not chased. Phase 6 puts a swapchain next to the compositor, which is
where this is most likely to reappear with better context; if it does,
the marker is already in place.


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

