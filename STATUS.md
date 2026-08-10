# STATUS

**Last updated:** 2026-08-09
**Branch:** `multihilo-wsi`

---

## Current state — read this first

*Everything below this section is the working record: dated, append-mostly, and
long. This block is the state itself, and it is the part that must be true.*

| | |
|---|---|
| **Phase** | **PHASE 6 IS COMPLETE, layout included.** The operator confirms the pattern renders correctly on the console — the one thing no measurement in this phase could produce, because every number here is about frames arriving and none about what was inside them. Run 16 is the run in which every piece of coverage this branch built actually executed. `t_vk_swapchain` **PASS 125/125** and `t_nwindow` **PASS 119/119** on one build. Three images present 90 of 90 at a mean of exactly **16666 us with 89 of 89 intervals inside 10%**; the infinite-timeout session ran for the first time at **20 of 20, 19 of 19 within 10%**; the copy fallback presented the pattern for the first time; both `VK_TIMEOUT` and `VK_NOT_READY` were produced and asserted. No MMU fault, no hang. A `VK_KHR_swapchain` presents on a Nintendo Switch through NVK over Horizon's VI compositor, zero-copy, at 89 of 89 intervals inside 10% of a 60 Hz refresh |
| **What runs on a Switch** | *Runs 11 and 12, 2026-08-08.* **A VK_KHR_swapchain presenting zero-copy**: `vkCreateViSurfaceNN` over the default window; 90 frames at mean 16664 us with **89 of 89 intervals inside 10% of a refresh**; two images and three both presenting 90 of 90, pacing **25169 us against 16807 us** under the same bursty load; two swapchains coexisting over one window with the superseded one reporting `VK_ERROR_OUT_OF_DATE_KHR`; either present path on request, each named by the driver; 120 pattern frames whose layout the operator confirmed. `t_nwindow` measures the same through raw `bq*` with no Vulkan present. *Run 17, 2026-08-09:* **and it does all of that from more than one thread.** `t_vk_wsi_mt` **PASS 50/50** — a render thread on core 1 and a present thread on core 2 driving one swapchain for 600 frames at a mean of **16608 us**; 50 swapchain generations whose predecessor is destroyed on another thread while the survivor presents; 3000 frames over 14 generations with a thread allocating, creating images and querying the surface throughout (10610 of each). *Run 19* repeats it in **full-memory mode** (3155 MiB against applet mode's 237 MiB) at PASS 50/50 with the same numbers. *Run 20, after the first round of PR 9 review fixes*, is the same test with its own teardown made spec-correct: **PASS 52/52**, full memory. **Run 20 is the newest hardware evidence and the tree has moved past it**: patches 0072 and 0073 and the `t_vk_wsi_mt` corrections from the full review are cross-build-verified only, and 52/52 is not the count the corrected test will report |
| **Next concrete task** | **Nothing is blocked.** The layout answer arrived and the phase's last open question with it. What remains is either not Phase 6 (`t_fault` on exit, `t_vk_texture`'s one occurrence, D7), never had a test (display mode change, docked resolution, `VK_SUBOPTIMAL_KHR`, `IMMEDIATE` through Vulkan), or is unreachable by design (patch **0068**, which needs a lost device and nothing provokes one any more). **`docs/milestones.md` ends at Phase 6**: what comes next is a decision, not a task |
| **Known failures** | **1. One unexplained MMU fault, in run 14, never reproduced** — runs 15 and 16 were clean through the identical sequence. It stays on the record as an unexplained single occurrence and is **not being investigated further**: the only correlated variable was nxlink, and nxlink has been removed at the user's direction. **2.** `t_fault` and the console on exit: run 14 has it PASS 20/20 and running last, so whether the console survived is unconfirmed. **3.** `t_vk_texture`'s one unexplained occurrence stays on the record, though run 14 put it at 1685/1685 |
| **Closed by run 4** | **The leak (0058's reference cycle) — fixed by 0060**, `device destroy refused` absent from the whole log. **The exit crash — it was the leak**, and the console now returns to the homebrew menu on `+`. That hypothesis was written after run 1 and run 4 is the first run in which it could be tested, because runs 2 and 3 both still leaked |
| **The check that lied, and it was mine** | `t_log_scan`'s predecessor read the log back to fail the test if the driver said it could not destroy something. It opened the file a second time while it was still open for writing, the SD card's device layer refused, and the helper answered "not found" — so run 2 printed **"ok the driver tore down every object it created"** four lines after `device destroy refused: live mem=33`. It now reads through the log's own handle (`"w+"`) and returns *whether the scan happened* separately from what it found; a scan that could not run fails the test. **Run 4 is the first run it executed in**, and its `ok` there is the evidence the leak is gone |
| **The artefact now names itself** | Run 3 cost an afternoon because a `.nro` on an SD card looks exactly like the one it replaced and nothing in the log said otherwise. `scripts/gen-build-id.sh` stamps every build in both build paths, `testfw` prints `note horizon-build-id <stamp>` as the second line of every log, and `scripts/package-horizon.sh` reads the stamp back **out of the binaries** into the manifest and refuses a package holding more than one build or an artefact carrying none. Both refusals were provoked and observed before the gate was believed |
| **Exit criteria** | **1. Met** (89 of 89 intervals within 10% of 16666 us, run 13). **3. Met.** **4. Met**, and since run 13 the check that proves it is no longer gated on zero-copy succeeding. **2. Met as throughput, and only as throughput** — the structural half stands (3 concurrent slots against 2) and the pacing half is a 50% difference in throughput (25170 us against 16837 us, run 13), *not* the burst absorption the tests originally claimed: `over_1p5_refresh` is **45 with three images and 45 with two**, twice measured |
| **THE LAYOUT IS CONFIRMED** | **The operator reports the pattern renders correctly on the console, every time it has been shown** (2026-08-08). Four coloured bars, the 16px border, the black diagonal corner to corner, the yellow square — the appearance a wrong stride, a wrong block height or a wrong GOB sector ordering would each destroy in a way that is not subtle. This is the only evidence in the phase that is about *what was in* the frames rather than that they arrived, and it is human by necessity: nothing can read a presented frame back, and a GPU readback would write and read with the same layout and agree with itself. **The zero-copy path is closed** — showing 1 ran in runs 14, 15 and 16. Showing 2, on the copy fallback, has only existed since run 16, so whether "every time" covers it is worth one word from the operator |
| **What is still unverified** | Any display mode change, docked resolution, and `VK_PRESENT_MODE_IMMEDIATE_KHR` through Vulkan. **Multi-threaded WSI is no longer on this list** — `t_vk_wsi_mt` covers it and found the defect patch **0070** fixes — but read what it does *not* cover: it never breaks the external synchronisation Vulkan requires of a swapchain, a queue or a command pool, so it says nothing about a driver that would need locks the specification does not ask for. Two surfaces over two `NWindow`s is untested, because this test uses `nwindowGetDefault()` and there is only one of those |
| **Open, not blocking** | Two unconditional L2 operations per submit. The acquire's CPU wait, now quantified: **acquire mean 15712 us of a 16671 us frame** on the zero-copy path against **5 us** on the copy path, where the wait sits in the present instead |
| **Open decisions** | **D7 only.** D18 (`minImageCount`) closed: it stays **2** — two images present 90 of 90; the compositor was never the limit, the retry loop was. D19 closed by run 13: **`async=false` is deleted** (patch 0067), because `async=true` plus the sleep presented 90 of 90 on two images without it. **D20** — the untracked Mesa commit, which this row previously called D18 as well — closed 2026-08-09 by exporting it as patch **0071**; see the collision note beneath the decisions table |
| **Never verified on hardware** | Patch **0068** — it has been in three builds and never fired, because no device has been lost since run 14, so the path it fixes remains untaken, and with nxlink gone there is no known way to provoke one. **Patches 0072 and 0073**, and the `testfw`/`vkfw`/`t_vk_wsi_mt` changes from the PR 9 review: cross build only, no console run yet, and the next run is what confirms them. Everything else has now run: 0069 and the rewritten control are in run 16's PASS, 0071 is in run 20's, and `t_vk_swapchain`'s infinite-timeout coverage executed for the first time at 20 of 20 |


---

## Multi-threaded WSI, and the one real defect it found (2026-08-09)

`t_vk_wsi_mt` is new and **PASS 50/50** on hardware (run 17), and
**PASS 52/52** after the review below (run 20). It exists
because "anything multi-threaded" had been on the *unverified* list
since the phase began, while the WSI's own comments reasoned at length
about concurrent creation, eviction and teardown — reasoning nothing
had ever executed.

### The defect

**`vkDestroySwapchainKHR` on a superseded copy-fallback swapchain
disconnected the window from the swapchain that had replaced it, and
took the console down with it.**

libnx's `framebufferClose` calls `nwindowReleaseBuffers(fb->win)`
unconditionally (`nx/source/display/framebuffer.c`), and
`nwindowReleaseBuffers` disconnects the producer whenever the window is
connected and has slots — *whoever* registered them. This backend called
it from teardown outside the surface lock and with no ownership test at
all. The surface-owner design that patch 0053 built exists precisely to
stop one swapchain reaching into a window another one owns; the
framebuffer was the one hole left in it.

It needs no threads to reproduce. It is the ordinary recreation
sequence — create the new swapchain naming the old one, then destroy the
old one — with the copy fallback selected. It is also legal concurrency:
`vkDestroySwapchainKHR` externally synchronises only the swapchain it is
given, so an application may run it beside another thread's acquire and
present on a different one, and section B of the test does exactly that.

**Why every recorded Phase 6 run missed it.** `t_vk_swapchain`'s section
D runs this same sequence and passes — on the zero-copy path, which owns
no `Framebuffer` and gives the window back through
`wsi_horizon_release_window`, which has tested ownership since 0053.
`MESA_VK_WSI_HORIZON_FORCE_COPY` existed and was used, but never across
a recreation.

**Fixed by patch 0070**: the framebuffer is closed at the moment the
swapchain stops owning the window — at eviction in
`wsi_horizon_claim_window`, or at teardown when it is still the owner —
both under the surface lock, both testing ownership, and still exactly
once.

### The evidence, in both directions

Same test, same tree, one patch apart.

| | run 17, with 0070 | run 18, 0070 reverted | run 19, with 0070 |
|---|---|---|---|
| build | `2026-08-09T14:35:22.614Z 6f833f5-dirty mesa:597ea0a` | `2026-08-09T14:57:27.459Z 6f833f5-dirty mesa:3fe711d` | `2026-08-09T15:20:15.777Z 6f833f5-dirty mesa:85638f8` |
| memory | applet, 237 MiB | applet, 237 MiB | **game, 3155 MiB** |
| result | **PASS (50/50)** | **the console took a system fatal** | **PASS (50/50)** |
| last line of the log | `RESULT: PASS (50/50)` | `ok A/copy: an acquire on the retired swapchain -> OUT_OF_DATE_KHR` — and nothing after it | `RESULT: PASS (50/50)` |

Run 19 is run 17 repeated in full-memory mode at the operator's request,
on the final commit. Both passes agree closely — section C at 16613 us
against 16608 us, the soak at 16731 us against 16747 us — so nothing
here depends on which memory ceiling the homebrew was launched under.
`mesa:597ea0a` and `mesa:85638f8` are the same tree: the second is the
first with the hardware evidence written into its commit message.

The next statement in the test after that last line is the
`vkDestroySwapchainKHR` on the retired copy-fallback swapchain.
`atmosphere/fatal_reports/01786287644_0100000000001000.log` records
**`Result: 0x290 (2144-0001)` in process `qlaunch`** — the system's own
home menu, not this process — on firmware 22.5.0 / Atmosphère 1.11.2.
Kept as `docs/hw-logs/t_vk_wsi_mt-run18-qlaunch-fatal-0x290.txt`.

That is a stronger finding than the one predicted from reading the
source. The reading said the survivor would stop presenting, because a
dequeue on a disconnected BufferQueue answers `NO_INIT` and the acquire
loop reads that as "the queue is full, come back later". What actually
happens is that the compositor's own side of the disconnect kills
`qlaunch`. **Both runs were in applet mode**, where this homebrew shares
`qlaunch`'s layer; whether game mode confines the damage to the game
process was not tested, and deliberately crashing the console a second
time to find out was not judged worth it.

### What the test actually covers, and what it refuses to do

Every section holds an application-side mutex for exactly what Vulkan
says is externally synchronised — the swapchain in acquire, present and
destroy; the queue in submit and present; a command pool per swapchain
because recording into a buffer synchronises its pool — **and nothing
more**. Where the specification requires no synchronisation, the test
deliberately provides none: `vkDestroySwapchainKHR(old)` runs beside
`vkAcquireNextImageKHR(new)`, and `vkCreateBuffer`, `vkAllocateMemory`
and the `vkGetPhysicalDeviceSurface*KHR` queries run beside a present
loop with no lock at all. A failure is then the driver's, not the
test's.

| | |
|---|---|
| A | recreate then destroy the old one, single-threaded, once per present path. The regression case |
| B | the same with the destroy on its own thread — 20 generations per path, 800 frames |
| C | a render thread and a present thread on one swapchain, 600 frames, **mean 16608 us against a 16666 us refresh** |
| D | 30 generations of recreation churn with a reaper thread, image count and present path alternating |
| E | 300 presents beside a thread doing 1068 buffers, 1068 images and 1068 surface queries |
| F | 3000 frames over 14 generations with that thread running throughout — 50.2 s, mean 16747 us, 10610 of each |

Threads are pinned where the kernel allows it and the test says which
cores it got: **core mask 0x7, render on core 1, present on core 2**. On
one core this would be interleaving; on three it is parallelism, and
which one happened is recorded rather than assumed.

### What the audit looked at and did *not* change

The rest of the concurrency review came out clean, and saying so is part
of the result:

- **The surface-owner protocol is correctly locked.** `presentable` is
  written and read under `surface->lock`; eviction resets the evicted
  swapchain's slot state under it; teardown tests ownership under it.
- **`wsi_horizon_swapchain_release_images` looks unsafe and is not.**
  It cancels slots with no ownership test, but eviction has already set
  every slot of an evicted swapchain to `FREE`, so there is nothing for
  it to cancel. Left alone deliberately: it is fragile rather than
  wrong, and the fix for fragile is a comment, not a patch nobody can
  point a hardware run at.
- **The `presentable` check in present is a TOCTOU and is unreachable.**
  Only a `vkCreateSwapchainKHR` naming this swapchain as `oldSwapchain`
  can clear it, and that call externally synchronises `oldSwapchain`, so
  a conforming application cannot be presenting on it at the time.
- **`vkGetPhysicalDeviceSurfaceCapabilitiesKHR` reads `NWindow` fields a
  concurrent present writes**, and the specification requires no
  synchronisation for it. Two aligned `u32`s: the worst case is a width
  from before a mode change with a height from after, which fails a
  later swapchain creation with `VK_ERROR_INITIALIZATION_FAILED`. Not
  fixed, because the fix would be a lock on a structure this backend
  does not own. Run 17 did 11678 of these queries beside presents with
  no failure.

### What the PR 9 review changed, and run 20

An automated review of PR 9 raised five points, **all of them about the
test and none about patch 0070 or the defect**. Four were real and are
fixed here; the fifth was a real code defect whose stated consequence was
not reachable, and is fixed anyway because it was two lines.

- **Teardown destroyed objects the device might still be using.**
  `mt_sc_destroy` waited on each in-flight fence and *threw the result
  away*, then destroyed the fences, the command pool and the swapchain
  regardless. A `VK_TIMEOUT` there is exactly the GPU stall this file's
  two-second waits exist to catch, and the response to it was undefined
  behaviour — on this platform, a console that stops answering instead of
  a legible verdict. Teardown now quiesces first and destroys nothing if
  it cannot: the result is recorded and reported by the main thread as
  its own check.
- **The render fence was the wrong proof for the present semaphores.**
  It says the render submission retired; it says nothing about the
  `vkQueuePresentKHR` still waiting on `render_done[i]`, which
  `vkDestroySemaphore` requires to have completed. The same quiesce now
  adds a `vkQueueWaitIdle` under the queue lock, which is the only thing
  on a queue that says so. `t_vk_swapchain` had been doing this since it
  was written; this file was the outlier.
- **The churn worker's stop flag was `volatile bool`,** written by one
  thread and read by another — a data race in C11 whatever it compiles
  to. Now `atomic_bool`, which is what `t_threads.c` and `t_ostime.c` in
  this same suite already use.
- **Section F never checked that its worker started.** A failed
  `pthread_create` left `first_error` at `VK_SUCCESS` and the section
  passed on a soak that had quietly been single-threaded. Section E had
  the check; F now does too.
- **Section A's two early returns leaked
  `MESA_VK_WSI_HORIZON_FORCE_COPY`.** Not reachable today — every later
  section that depends on the override sets it on entry — but sections C
  and E do not, so it was a trap waiting for a reordering.

**Run 20**, full memory, `2026-08-09T18:29:27.522Z e7870a0-dirty
mesa:ebf2e31`: **PASS 52/52**, the two extra checks being the ones added
above. It agrees with run 19 everywhere the change does not touch —
section C at **16607 us** against 16613 — and differs where it does:
section F's mean moved from 16731 us to **16882 us**, which is the added
`vkQueueWaitIdle` at each of the 14 generation boundaries, about 32 ms
each, spread over 3000 frames. The stall is real, it is at teardown only,
and it does not weaken sections B, D and F: the destroy itself still runs
against the survivor's acquire and present, which is the unsynchronised
pairing under test.

Kept as `docs/hw-logs/t_vk_wsi_mt-run20-review-fixes-PASS.log`.

**Run 20 was not built against run 19's Mesa, and the comparison above
is qualified by that.** The build id says `mesa:ebf2e31`; run 19's said
`mesa:85638f8`. `85638f8` is patch 0070 and is an ancestor, so the fix
under test was present — but the checkout carries **one commit that
`mesa-patches/` does not**: `ebf2e31`, "vulkan/wsi: Horizon, report the
layout that was registered", which adds the geometry of the registered
image to the INFO line and records `scanout_size_B` at registration. Its
effect is visible in the log as 50 `layout: stride 5120 B …` notes that
runs 17 and 19 do not contain at all.

That commit is **untracked work sitting in an ignored directory**:
`mesa-patches/` holds 70 patches, the checkout holds 71 commits, and
`tests/t_vk_mode.c` — the test whose hardware run that commit's message
cites as `PASS 56/56` — **does not exist in this tree**. A clean
checkout rebuilt from `mesa-patches/` therefore does not reproduce the
binary run 20 measured. Nothing here has been exported or reconstructed:
it is somebody's unfinished change and this entry records the
discrepancy rather than resolving it. **Pending decision.**

What it does not put in doubt: the extra commit adds a log line and a
recorded field, it is nowhere near teardown, ownership or the surface
lock, and every check run 20 makes is a check runs 17 and 19 also made
and passed.

**CLOSED the same day, as D20** (it was opened as a second D18; see the
collision note beneath the decisions table). The commit is exported as
patch **0071** and the series now applies 73 of 73 with nothing pending,
so the tree run 20 measured is reproducible from this repository. Its
`Evidence` header could not be carried across as written — it cited
`t_vk_mode ... PASS 56/56`, a test and a log that do not exist here — so
it now cites the run 20 log, which contains the same measurement and has
a recorded digest. `tests/t_vk_mode.c` stays deleted: reconstructing
somebody's removed work on a guess is worse than recording that it
went.

### The PR 9 review, and what it changed (2026-08-09)

The branch was reviewed after run 20. The defect and the two-directional
evidence held up; almost everything around them did not. Three classes,
and the third is the one worth keeping:

**1. The invariant was documented rather than enforced.**
`wsi_horizon_close_framebuffer`'s comment named two callers and there
were three. The third, on `vkCreateSwapchainKHR`'s failure path, held no
lock and tested no ownership — the exact shape of the defect 0070 had
just fixed, under a comment asserting it could not happen. It was
unreachable (`fb_created` was set on `init_fallback`'s last line, so no
failure path saw it true), which made it a trap for the next `goto`
rather than a live bug. The same function called `framebufferClose` raw
on two of its own error paths. Patch **0072** gives the close one home —
`wsi_horizon_release_window`, which already locked and already tested —
and replaces the comment's claim with an `assert`. Patch **0073** stops
the four INFO messages naming a swapchain that is not a handle yet,
which is what produced **188** `vk_log*() called with client-invisible
object` lines in run 20's own log.

**2. `t_vk_wsi_mt`'s central claim was untrue.** Its header said
reporting was single-threaded and no worker called `t_note`. `vkfw`
installs a debug-utils messenger, Mesa calls a messenger on whichever
thread produced the message, and the callback calls `t_note` and
increments a shared counter — so four of the file's five worker kinds
reached it through the driver. That is a data race in the reporting path
of the one file whose argument is that any undefined behaviour it
observes is the driver's. Fixed in the framework, where it belongs:
`testfw` serialises its own output and `vkfw` its message ring. The test
also left the acquire fence with a signal pending on two paths and then
destroyed it, and its deliberate leak-on-quiesce-failure was justified
by "the process is about to print a failure and exit" — which it did
not, running three more sections and then destroying the surface out
from under a live swapchain. Sections C and E turned out never to have
run on the copy fallback at all.

**3. The record did not match the branch.** `docs/hw-logs/README.md`,
whose stated job is to describe the logs "because a reader opens the
log, not `STATUS.md`", had no entry for any of the five runs this branch
rests on — including the crash log and the `qlaunch` fatal report. The
manifest passed the whole time, and could not have caught it: it
compares digests of files that exist and is blind to an undescribed one.
`docs/wsi.md` §7.1 stated an invariant §7.3 contradicted two paragraphs
later. And this file called D18 both closed and open, three thousand
lines apart, about two different decisions.

**None of it has run on hardware.** Run 20 remains the newest evidence
for this backend and predates every patch and every test change above.
Its PASS 52/52 is not the number the corrected test will report — C and
E now run twice and every section gained a path check — and its 188
client-invisible warnings are the measurement 0073 exists to take to
zero. The verification behind these changes is a cross build and nothing
more: every touched translation unit compiled with `aarch64-none-elf-gcc`
from `ghcr.io/d3fau4/nx-dev`, using meson's own recorded command line
with `-fsyntax-only`, with no diagnostics.

Three Windows portability defects in this repository's own tooling were
found in the course of running those gates, and fixed:
`apply-mesa-patches.sh` compared a POSIX path spelling against git's
native one and refused every tree; `toolchain-env.sh` lost its container
mount to MSYS path conversion; and `split-status.py` wrote CRLF, so
regenerating `MANIFEST.sha256` rewrote all 170 lines and destroyed the
one property that file exists for.

### Also in this change

- `vkfw_result_str` now names the window-system results. Every
  recreation check in these tests used to read
  `-> VkResult -1000001004` where it meant `OUT_OF_DATE_KHR`.

---

## The layout is right, and a human is the instrument that says so (2026-08-08)

The operator, asked what was on the television: **the pattern comes out
correctly, every time.**

That closes the last open question of Phase 6, and it is worth being
precise about why it needed a person. Nothing can read a presented
frame back — it is gone, handed to the compositor. A GPU readback would
not help either: it would write and read with the same layout and agree
with itself, which is a tautology and not a check. The compositor is
the other party to the agreement about how pixels are arranged in
memory, and the eye is the only instrument that sees its side.

So the test presents four coloured bars, a 16-pixel border, a black
diagonal from corner to corner and a yellow square, writes down in the
log what that should look like, and asks. A wrong stride steps the
diagonal; a wrong block height bands the image; a wrong GOB sector
ordering scrambles it at 16-byte granularity. None of those is subtle,
and none of them was there.

**What this covers.** Showing 1, on the zero-copy path — the images the
compositor scans out are the images the application rendered into — ran
in runs 14, 15 and 16, and is confirmed. Showing 2, on the copy
fallback, has only existed since run 16, so whether the operator's
"every time" reaches it is one word away and is recorded as such rather
than assumed.

**What it does not cover**, and this is the honest boundary: 1280x720
handheld, `VK_FORMAT_R8G8B8A8_UNORM`, `block_height_log2 = 4`. A docked
resolution, a different format or a different block height would each
be a different agreement, and none of them has been shown to anybody.

---

## nxlink removed, and the MMU fault closed unexplained (2026-08-08)

**Decided by the user**, in as many words: ignore the nxlink question,
and the code can go. Both were done.

### What was removed

The whole streaming path in `tests/common/testfw.c` — `t_nx_start`,
`t_nx_raw`, `t_nx_replay`, `t_nx_stop`, the status line, and the socket
headers. 230 lines out, 7 in. It is recoverable in one command: the
feature is commit `9b4f976` and nothing else has touched it since.

**What deliberately stayed**, because it is independent of nxlink and
confirmed on hardware:

- the **write-after-`fclose`** fix. `testfw` used to write the "press +
  to exit" note to a `FILE *` it had already closed — the handle closed,
  the pointer not cleared, so the `t.log != NULL` guard was still true.
  Every log up to run 14 ends at `RESULT`; runs 15 and 16 are the first
  in this project to carry that line.
- `t_sink`, which composes each line once and writes it to the console
  and the log rather than formatting it twice.
- `t_check` and `t_note` are byte-identical to their pre-nxlink form,
  and that was checked by diff rather than by eye — the first attempt at
  this removal deleted both of them along with the socket code, and the
  build caught it as "`t_vemit` defined but not used".

### The MMU fault, and why it is being left alone

One occurrence, run 14, on the graphics channel of `t_vk_swapchain`,
immediately after the pattern's 120 presents. Runs 15 and 16 went
through the identical sequence and were clean, so it has **never
reproduced**. The only variable that differed was nxlink, and with
nxlink gone there is no known way to provoke it — which also means
patch **0068**, whose whole job is to stop a lost device hanging the
acquire, has no route to being exercised.

That is the honest position and it is not a good one: an unexplained
GPU fault stays on the record, and the code written to survive it stays
unverified. Both are stated rather than quietly dropped.

---

## Run 16 — everything this branch built finally ran (2026-08-08)

Build `2026-08-08T20:46:09.792Z 8b5b1de mesa:57e85ae`, both tests, no
nxlink. **`t_vk_swapchain` PASS 125/125**, **`t_nwindow` PASS 119/119**.
`docs/hw-logs/t_vk_swapchain-run16-PASS.log` and
`docs/hw-logs/t_nwindow-run16-PASS.log`.

This is the first run that reached the end of `t_vk_swapchain`. Runs 14
and 15 both hung before the last third of the file, for two unrelated
reasons, so the checks after that point had never executed at all.

### First-time evidence, none of which existed before this run

- **The infinite-timeout session through Vulkan**: `2 images, FIFO,
  infinite acquire timeout: 20 of 20 frames presented`, 19 of 19
  intervals inside 10% of a refresh, acquire mean 15794 us. Patch 0067
  removed a dequeue mode on the strength of `t_nwindow`'s raw `bq*`
  measurement; this is the same question answered through the API.
- **The copy fallback presented the pattern**: `the copy path presented
  the pattern 120 times`. The fallback had never displayed anything an
  eye could falsify.
- **`VK_TIMEOUT` and `VK_NOT_READY` were produced and asserted.**
  `an acquire that cannot be satisfied within its deadline returns
  VK_TIMEOUT`, and the same acquire at zero returns `VK_NOT_READY`.
  Before this, "no acquire returned VK_TIMEOUT" was a sentence about
  something this test had never seen.
- **The rewritten control measured its old premise instead of assuming
  it**: `the application could hold 1 of this swapchain's 2 images at
  once`. One, not two — which is exactly what broke run 15 — and
  `every image the control held was presented back (1 of 1)`.

### The numbers, and they are the best recorded

| | run 16 | run 13 |
|---|---|---|
| 3 images, FIFO | 90 of 90, **89 of 89** within 10%, mean **16666 us** | 89 of 89, 16666 us |
| 2 images, FIFO | 90 of 90, 87 of 89 | 90 of 90, 87 of 89 |
| infinite timeout | **20 of 20, 19 of 19** within 10% | never reached |
| copy fallback | 90 of 90, **89 of 89** within 10%, acquire mean **6 us** | 87 of 89 |
| bursty, 3 vs 2 | 16855 us vs 25176 us, 45 and 45 | 16837 vs 25170, 45 and 45 |

Exit criterion 2's corrected claim has now reproduced three times, with
`over_1p5_refresh` at 45 and 45 every time. The claim it replaced would
have failed three times.

### What run 16 does not settle

**The MMU fault.** Run 14 streamed over nxlink and faulted; runs 15 and
16 did not stream and did not fault. One faulting run against two clean
ones, with nxlink the only difference — a hypothesis with three data
points, not a finding.

**Patch 0068** has now been in three builds without firing, because no
device has been lost since run 14. The path it fixes is still untaken.

**The pattern.** It has presented 120 of 120 frames on the zero-copy
path in three runs and on the copy fallback in one, and **nobody has
said what was on screen**. Every number here is about delivery. The
layout evidence for this phase does not exist yet and no amount of code
can produce it.

---

## Run 15 — the fault did not reproduce, and the hang was mine (2026-08-08)

Build `2026-08-08T20:23:02.777Z c2c17b9 mesa:4d90ca5`, both tests,
launched from the homebrew menu: `note nxlink: no host, so nothing is
streamed`. Logs are `docs/hw-logs/t_nwindow-run15-PASS.log` and
`docs/hw-logs/t_vk_swapchain-run15-HUNG-NO-FAULT.log`.

### No MMU fault

`t_vk_swapchain` ran the whole way through both swapchains with a
healthy device — and the numbers are the best this project has
recorded:

| | run 15 | run 13 |
|---|---|---|
| 3 images, FIFO | 90 of 90, **89 of 89** within 10% | 90 of 90, 89 of 89 |
| 3 images, acquire | mean 15956 us | 15733 us |
| 2 images, FIFO | 90 of 90, 87 of 89 | 90 of 90, 87 of 89 |
| record+submit | **369 us** | 564 us |
| bursty, 3 vs 2 | 16815 us vs 25162 us | 16837 vs 25170 |

`t_nwindow` PASS 119/119 again, including `no deadline, 2 buffers:
20 of 20 frames`.

**The MMU fault of run 14 did not come back.** The one difference
between the two runs is nxlink: run 14 streamed, run 15 did not. That
is **one run each**, which makes it a hypothesis and not a finding, and
it is the thing run 16 is for. What can be said is that the fault is
not inherent to the sequence — the same 120 pattern presents followed
by the same session ran clean.

### The hang, and it was entirely mine

The acquire-refusal control assumed both images of a two-image
swapchain could be held at once. Straight after a FIFO session they
cannot: the compositor is still holding what it was given, so the
first acquire returned `VK_SUCCESS` and the second `VK_TIMEOUT`. **That
is the control succeeding** — a refusal is exactly what it exists to
observe — but the code read it as the setup falling through, took the
`else` branch, printed a note, and **returned nothing**.

So a two-image swapchain was left with one usable buffer. One buffer
cannot make progress in FIFO: the compositor will not release the frame
it is scanning out until it is handed another, and there was no other.
The next session asked for an image with no deadline and waited
forever. The driver's own warning is the last line of the log —
`no buffer in 3000 ms and this swapchain still owns the window; last
dequeue said 0x0000115d` — and there is no `RESULT` after it.

Three fixes, and the third is the one that matters beyond this test:

- **The control now drives the acquire until it is refused**, whatever
  number of images that takes, and asserts on the refusal it gets. The
  premise it used to assume is now the thing it measures.
- **Every image is presented back on every path**, and that is a
  `t_check` rather than an assumption. An acquired image that is never
  presented is not a memory leak — it is a swapchain that cannot run.
- **Patch 0069**: the zero-copy acquire refuses to block forever when
  every image is with the application. The copy fallback has done this
  since 0053; the same condition existed on both paths and was checked
  on one. That asymmetry is what turned a test bug into a hang with no
  cause on screen.

**The caller was at fault, and that is the point.** Vulkan permits the
acquire to block when an application holds more images than it
presents. Blocking silently and forever turns someone's bug into a
hang; this backend has the state to name it instead.

### What run 15 says about run 14's fixes

Patch 0068 (a lost device ends the acquire) was in this build and never
fired, because the device never died. It remains unverified.

---

## Run 14 — 28 of 29 pass, and the one that did not is a real defect (2026-08-08)

All 29 tests, one build (`2026-08-08T18:48:25.691Z 9b4f976 mesa:815dca2`),
every one streamed over nxlink. Logs are `docs/hw-logs/*-run14-*.log`.

### What passed, and what it settles

- **28 PASS**, including every `horizon/` test, nine of the ten Vulkan
  regression tests, and `t_vk_texture` at 1685/1685.
- **`t_nwindow` PASS 119/119.** Two buffers present **90 of 90** with
  **87 of 89** intervals inside 10% of a refresh, and the new
  `no deadline` session dequeued **20 of 20 frames at `UINT64_MAX`**,
  ~2.25 ms each. Patch 0067's single dequeue policy holds at both
  finite and infinite timeouts through raw `bq*`.
- **nxlink works.** `note nxlink: streaming this log live to
  192.168.1.104` in all 29 logs — and it is the reason there is
  anything to read about the failure below.
- **The `fclose` defect is confirmed fixed on hardware.** `note the run
  is finished; press + to exit` appears at the end of `t_nwindow`'s log
  — the first time that line has existed in any log in this project.
- **`t_fault` PASS 20/20**, and the syncpoint values prove it ran
  **last** (its fence is `26:298193`, above every other test), so it
  did not contaminate anything.

### The failure: an MMU fault nobody asked for

```
[horizon_gpu:E] channel 0x7a8f640010: fault notification 31 (MMU fault) — marking lost
```

`t_vk_swapchain`, immediately after the pattern had presented **120 of
120 frames successfully**, and before the third frame of the session
that followed. Every submit and fence wait afterwards returned
`VK_ERROR_DEVICE_LOST`, correctly: `horizon_gpu` caught the notifier,
marked the channel lost, and NVK reported it rather than returning
success for work that never ran. That part of the stack behaved.

**What is known.** The syncpoint counters order the session:
`t_vk_swapchain` ran **first** of the Vulkan tests (syncpt 26 at
184082, against 187916 for the next one), so this is not fallout from
another test. An unintentional MMU fault has never appeared in this
project before — the only other occurrences in `docs/hw-logs/` are
`t_fault`'s own, which it provokes on purpose. Run 13 went through the
identical sequence — same pattern, same 120 presents, same session
after it — and did not fault.

**What is not known: why.** Between run 13 and run 14 the executed code
at that point differs only by nxlink's socket driver being resident,
since patch 0067 changes nothing for a finite-timeout acquire. So the
candidates are: intermittent and pre-existing; a latent mapping defect
that a different heap layout exposed; or something else. **Nothing here
can distinguish them, and this is not going to be guessed at.** Run 15
repeats `t_vk_swapchain` to find out whether it reproduces at all.

### And then it hung, which was ours

With the device lost, every present failed, so the application kept
both images of a two-image swapchain, so the queue could never free
one — and the test's `timeout = UINT64_MAX` session asked for a buffer
forever. **The run produced no `RESULT` line**: one fault cost the
entire verdict, and the twenty-odd checks after that point never ran.

The hazard was written down before the run — the run-order document and
the test's own comment both said this could hang rather than fail — and
being predicted is not the same as being acceptable. Two fixes:

- **Patch 0068**: `vkAcquireNextImageKHR` returns `VK_ERROR_DEVICE_LOST`
  rather than waiting. Checked every round in the blocking loop, because
  the device can be lost *while* it runs, which is the case that hung;
  and once at entry, before the `VK_ERROR_OUT_OF_DATE_KHR` paths, so the
  copy fallback is covered too — an application told to recreate its
  swapchain on a dead device will recreate it and ask again.
- **The test no longer starts an unbounded wait it has reason to think
  cannot end**, and says in the log that the coverage did not run
  rather than skipping in silence.

### A phase boundary, so this is not so vague next time

"Somewhere between 120 pattern presents and frame 2 of the next
session" is as precise as run 14's log can be, because nothing asked in
between: the fault surfaces at the first fence wait that happens after
it. `t_vk_swapchain` now calls `vkDeviceWaitIdle` at the phase boundary
and fails a named check there. Between phases only, never inside one,
so no timing measurement sees it.

---

## The log, on the developer's machine, while it happens (2026-08-08)

`testfw` now streams every line it writes to an nxlink host, and
replays the whole log file down the same socket when the run ends.

**Why it is not just convenience.** Two tests own the display and start
no console, so their stdout goes nowhere and the SD card file is the
entire record — readable only after the run, by taking the card out.
Every hardware lesson in this project has come through that loop. And
one of those tests can now *hang* rather than fail: the acquire session
at `timeout = UINT64_MAX` has no deadline left to expire. A hang leaves
an SD log that stops mid-run with no indication of where; a live stream
shows the last line that made it out, which is the diagnosis.

**Sent twice, on purpose.** Live lines are the test's own. The
end-of-run replay carries what the live stream cannot: Mesa's
diagnostics arrive on `stderr`, `stderr` is `dup2`'d onto the log file,
and those are historically the lines that say *why* a run failed.

**What is deliberately not done.** Neither `stdout` nor `stderr` is
redirected to the socket — `nxlinkConnectToHost(false, false)`.
Redirecting `stderr` would take Mesa's messages out of the artefact;
redirecting `stdout` would blank the console on the tests that have
one. Nothing that exists today loses anything.

### Two things read out of machine code rather than assumed

- **`nxlinkConnectToHost(false, false)` really does connect and hand
  back the fd.** The header only promises a socket; the behaviour with
  neither redirect requested is not documented. Disassembled from
  libnx's own `nxlink_stdio.o`: the host check, `socket`, `connect` to
  port 28771 and the return of the fd all happen before the two
  `tbnz` tests that gate the `dup2` calls.
- **The socket comes back blocking.** The same disassembly sets
  `O_NONBLOCK` for the `connect` and masks it off again afterwards. A
  host that stops reading would therefore stall a console inside
  `write()`, and a test stuck writing a log line is indistinguishable
  from a test stuck doing the thing it measures. Every write now
  carries `SO_SNDTIMEO` of one second; a timeout closes the stream,
  says so in the file, and the run continues without a network.
- **A run not launched by nxlink pays nothing.** Verified in the
  compiled object, not in the source: the guard folds to
  `sub w0, w0, #1` / `cmn w0, #3` / `b.ls`, and `socketInitialize` is
  only reachable through that branch. Every measurement in
  `docs/hw-logs/` stays comparable with the ones taken after this.

### The defect it uncovered

`testfw`'s "the run is finished; press + to exit" note — added in the
PR #7 review for the tests with no console — **has never appeared in
any log**. It wrote to `t.log` *after* `fclose()`, three lines above,
with the handle closed but the pointer not cleared, so its
`t.log != NULL` guard was still true. Check any log in
`docs/hw-logs/`: they all end at `RESULT`. Writing to a closed stream
is undefined behaviour, not merely a lost line. The log is now closed
once, at the end, after everything that writes to it. The same class —
a handle closed while the pointer guarding its use stays live — does
not occur anywhere else in the tree; the other two `fclose` calls are
on locals that die immediately.

### What this does not do

It is not a measurement channel. A socket in the picture is a
difference in what the timings were measured against, so every log now
carries a `note nxlink:` line saying which kind of run it was, and
**pacing evidence should be taken without nxlink**. Whether streaming
perturbs the frame numbers has not been measured, and the tests print
no lines inside their timed loops, which is a reason to expect little
rather than evidence of none.

Cross build green in both build paths (meson and the Makefile), under
`-Wall -Wextra -Werror`. **No hardware behind any of it.**

---

## Run 13 — the answer, and what it retired (2026-08-08)

Build `2026-08-08T15:40:28.620Z b958bd0 mesa:587ac72`, both tests, one
build. `t_nwindow` PASS 118/118, `t_vk_swapchain` PASS 120/120.
Recorded as `docs/hw-logs/t_nwindow-run13-PASS.log` and
`docs/hw-logs/t_vk_swapchain-run13-PASS.log`.

### The question it was built to answer

Patch 0065 restricted `async=false` to an infinite timeout, and since
every acquire in both tests carries a finite budget, run 13 is the first
run in which that mode could not be reached at all. What was left had to
carry the two-buffer case on its own, and it did:

| | 2 images / buffers | 3 |
|---|---|---|
| `t_vk_swapchain` FIFO | 90 of 90, **87 of 89** within 10% | 90 of 90, **89 of 89** |
| acquire | mean 15719 us / max 21303 us | mean 15733 us / max 16075 us |
| `t_nwindow` raw `bq*` | 90 of 90, **87 of 89** within 10% | 90 of 90, 86 of 89 |
| dequeue | mean 2254 us / max 6581 us | mean 230 us / max 13202 us |

So `async=true` plus a real sleep is sufficient by itself. **Patch 0067
deletes the second mode**, and with it the `blocking` parameter that
existed only to select it — which also settles the naming finding from
the PR #8 review by deletion rather than by renaming.

The 2026-08-05 measurement that had put `async=false` there in the first
place — `async=true` answering NO_INIT 78166 times running while
`async=false` produced a buffer in 145 us — was taken from a loop with
no idle in it. It measured the spin, not the mode.

### What run 13 also settled, without being asked

- **Both PASS logs are from one build.** The review found the previous
  pair were `5995c12` and `d41e12a` narrated as one body of evidence.
  This pair is one stamp, and `scripts/package-horizon.sh` is what
  makes that checkable rather than claimed.
- **Exit criterion 4's check is no longer gated on zero-copy.** It ran
  ungated and passed: `zero-copy by default, the copy fallback when
  forced`, both named by the driver in one run.
- **The eviction refusal**, added blind after the review: `a swapchain
  over a window that already has one, with no oldSwapchain, is refused
  with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`, and the incumbent still
  acquired afterwards.
- **Criterion 2 measured the same way a second time** — 45 and 45 over
  1.5 refreshes, 25170 us against 16837 us. The corrected claim
  reproduces; the original one would have failed again.

### The NO_INIT overload, seen from outside

`t_nwindow` line 64: the dequeue that ends the two-buffer count returns
**`0x0000115d`** (`LibnxBinderError_NoInit`), while the three-buffer one
returns **`0x00001d5d`** (`WouldBlock`). Same condition — no buffer free
— reported two different ways depending on how full the queue is. That
is the overload patch 0066 bounded by ownership rather than by result
code, and this is the first log that shows both codes side by side in
one run.

### What run 13 did not answer

**How the pattern looked.** Both showings presented 120 of 120 frames,
which says they reached the compositor and nothing about their layout.
The operator's two answers are still outstanding, and the second one is
the entire layout evidence for the copy fallback.

---

## The fallback had no layout evidence, and now it is asked (2026-08-08)

The last thing on this branch that was unmeasured *and* measurable
without waiting for anything: the copy fallback's memory layout.

The state of the evidence before this change. The pattern — four
coloured bars, a border, a diagonal, a corner square — is the only
oracle in the phase that can catch a wrong stride, a wrong block height
or a wrong GOB sector ordering, because every other frame the test
presents is a solid colour and a solid colour is the same image under
any swizzle. It was presented on the zero-copy path only. The fallback
presented 90 of 90 frames in run 12 and that number is worth exactly
what it says: frames reached the compositor. It is not a statement
about what was in them.

The two paths do not share the code that fills the presented buffer —
zero-copy hands the compositor the image the application rendered into,
the fallback blits into a buffer of its own — so a layout error in the
fallback is invisible in the zero-copy showing, and the phase would
have shipped with one of its two present paths never having displayed
anything an eye could falsify.

`t_vk_swapchain` now shows the pattern twice: showing 1 in section F on
whichever path the driver picks, showing 2 in section E on the
swapchain already forced onto the fallback, reusing the same buffer.
The operator is asked twice and told explicitly that the two answers
are different evidence. Presenting all 120 frames is a `t_check` in
both cases; the appearance is the operator's line in the report.

**This is a test change, not a driver change.** Nothing in
`mesa-patches/` moved. Cross build green (`build/meson`, one ninja
invocation, one build id). Nothing here is verified on hardware: the
answer this adds does not exist until run 13 comes back.

---

## The branch review — twenty-one findings, twenty-one real (2026-08-08)

Every one was checked against the code before being accepted. None was
noise. Fixed across `8e3d16a`, `6b5ee33`, patch **0065**, patch **0066**
and the commit this section lands in.

### The three that were about the record, not the code

- **Exit criterion 2 was overstated** — its own section above.
- **The two PASS logs are two different builds**, `5995c12` and
  `d41e12a`, narrated as one body of evidence while
  `scripts/package-horizon.sh` refuses exactly that pairing. Stated in
  `docs/hw-logs/README.md`.
- **Patch 0065's commit message cited `t_nwindow` as evidence for the
  opposite of what it does.** It claimed a finite timeout gets
  async=true and the sleep "which is what t_nwindow uses"; `nw_dequeue`
  asked both modes every round regardless of the budget. The spec
  argument for 0065 stands on its own — nothing can bound how long
  async=false blocks — but the evidence cited did not support it, and
  the regression risk to the two-image case is unmeasured. `t_nwindow`
  now applies the same policy, so a run measures one policy in both.

### Eleven in the driver (0066)

`presentable` read without the lock that writes it; the acquire loop
never re-checking it, so an evicted swapchain on an infinite timeout
waited forever; `LibnxBinderError_NoInit` overloaded with no boundary —
now bounded by ownership, since a producer that still owns its window
and says NO_INIT is full and one that does not is gone; the
`eventActive` guard left dead by 0064, failing valid windows with a
false message; a TOCTOU between reading ownership and cancelling slots
in teardown; no check that the row stride covers the width; IMMEDIATE
still degrading to FIFO on the copy fallback, which 0065 fixed on one
path only; the fallback narrowing rows silently; and `try_dequeue`
reporting the cancel's failure instead of the fault, with a negative
slot handed to `bqCancelBuffer`.

Two are noted and not changed: `blocking` is the negation of the
`async` argument it controls — a naming hazard in the function this
series misdiagnosed across four patches — and `consumer_running_behind`
is never updated since 0055, because the zero-copy path bypasses
`nwindowQueueBuffer`. Renaming the parameter touches every call site in
the file and is deliberately not mixed into a correctness patch.

### Five in tests and tooling

Exit criterion 4's check was inside `if (zc)`, so the case where the
decision is most in doubt was the one case that could not fail it; it
fails now. `vkfw`'s message buffer was documented as a ring and
implemented as a truncating prefix, keeping the first sixteen — and exit
criterion 4 asserts against it; it is a ring now, and
`VKFW_MESSAGE_CHARS` went from 192 to 384 because the asserted message
was within about 35 characters of the old limit.
`scripts/package-horizon.sh` could check zero artefacts and exit 0,
which is its own stated rule unapplied to its own counter; it fails, and
was broken in both directions. `check-mesa-test-parity.sh`'s narrowing
now prints what it does not compare and why. And the build stamp gained
milliseconds, because two builds in the same second read identical and
telling two builds apart is its whole job.

---

## The exit-criterion-2 claim was wrong, and my own log says so (2026-08-08)

Raised in review of PR #8, verified against the logs, and correct.

The claim was that triple buffering absorbs a bursty load where double
cannot: 16807 us against 25169 us through Vulkan, 16793 against 24918
through raw `bq*`. The means are real. **The mechanism is not**, and the
same log lines refute it:

```
3 images, FIFO, bursty load: mean 16807 us, min 495, max 33245;
    0 within 10% of 16666 us; 45 longer than 1.5 refreshes
2 images, FIFO, bursty load: mean 25169 us, min 9592, max 40525;
    0 within 10% of 16666 us; 45 longer than 1.5 refreshes
```

**Zero of 89 intervals paced correctly in either case, and both put 45
of 89 over 1.5 refreshes — the same number.** `t_nwindow` says 44 and
44. Whatever three buffers did, absorbing the bursts is not it, and a
mean that lands near the refresh because the load alternates on and off
is an artefact of the load, not evidence about the queue.

What the measurement does support: **double buffering took 50% longer
to deliver the same 90 frames under the same load** — 2.24 s against
1.50 s. That is a difference in throughput, and it is what the check
`m2 * 10 > m3 * 11` actually tests. The rationale written above that
check — "two buffers must round every overrun up to a whole extra
refresh; three can absorb it" — is wrong and is corrected in both tests.

Exit criterion 2 asks for the difference "recorded with numbers, not
adjectives". The numbers are recorded and they differ. The adjective was
mine.

---

## Codex review on PR #8 — seven right, two wrong (2026-08-08)

**Class: review, not hardware.** Ten findings from the automated
reviewer on `ce3311a`.

### Four were about the artefact-identity machinery, and all four landed

Fixed in `8e3d16a`, each gate broken in both directions first:

1. **The reverse staleness gate was wrong in the ordinary case.** It
   compared every file under `mesa/src` against `libvulkan_wsi.a`, and
   Ninja does not touch an archive whose inputs did not change — so
   editing anything outside WSI leaves that archive older than source
   that was just correctly compiled. Demonstrated here: after a clean
   rebuild, `nvk_device.c` is 13:02:47 and `libvulkan_wsi.a` is
   13:02:00, so the gate would have refused a good build. Now
   `build-mesa-nvk.sh` touches `.horizon-build-ok` as its last act under
   `set -e`, and the gate compares against that.
2. **Every gate ran after the package was published.** A rejected
   package was already in the output directory looking valid. All gates
   now run before anything is written; a failing run leaves
   `MANIFEST.txt` byte-identical.
3. **A partial build leaving one archive was accepted.** Both are now
   required.
4. **The build id described only the outer repository.** `mesa/` is a
   separate gitignored checkout holding the code every driver-linked
   test actually links, so an edit under `mesa/src` left the stamp
   looking clean — a hole in the exact mechanism built to stop
   misattribution, and the same family as run 11's stale archive. The
   stamp now carries mesa's HEAD and dirty state.

### Two were wrong, and are answered on the PR

- **`framebufferBegin` stride.** Claimed to be in pixels; libnx's header
  says "distance **in bytes** between rows of pixels in memory", and
  states the addressing convention on the next line. `fb_stride * bpp`
  would overshoot by 4×.
- **The fallback acquire needs a lock.** `vkAcquireNextImageKHR`
  requires host access to `swapchain` to be externally synchronised, so
  concurrent calls on one swapchain are invalid usage. The zero-copy
  path's surface lock is for a different thing: registration shared
  *between* swapchains.

The first of those did surface something real anyway — see "what is
still unverified": the copy fallback has **no layout oracle**, because
the pattern only runs on the zero-copy path and solid colours hide any
stride error.

### Four are real driver findings, and each needs a hardware run

Not taken unilaterally; two of them change advertised behaviour.

1. **`async=false` can overrun a finite `timeout`.** It is skipped at
   timeout 0 but not at finite non-zero, and Android permits that mode
   to block in the server. Measured at 134–158 us on this compositor,
   which is evidence it does not, not that it cannot.
2. **`vkCreateSwapchainKHR` should return
   `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`** when the surface already has a
   non-retired swapchain and `oldSwapchain` does not name it. It
   currently evicts the owner instead. Section D of
   `t_vk_swapchain` already passed `oldSwapchain`, so it was legitimate
   either way and **could not tell a conformant driver from this one**
   — the gap was in the test's coverage, not in what it asserted.
3. **IMMEDIATE with two images silently becomes FIFO** (`swap_interval`
   is 0 only at three or more images).
4. **A failed `bqCancelBuffer` still marks the slot `FREE`.** Less
   severe than reported on the zero-copy path — the next slot comes from
   `bqDequeueBuffer`, so a slot the queue still holds cannot be handed
   out twice — but it is wrong bookkeeping and loses the slot until
   teardown.


---

## Run 12 — Phase 6 is done (2026-08-08)

**Class: hardware (HW).** `t_vk_swapchain` **PASS 117/117**, build
`2026-08-08T12:35:43Z d41e12a`. Log in
`docs/hw-logs/t_vk_swapchain-run12-PASS.log`.

The driver in it contains patch 0064, which the run-11 binary did not:
`wsi_horizon.c:1851` in the log against 1847 in run 11, and the
two-image sessions that had failed eleven times now present.

```
  ok   2 images, FIFO: 90 of 90 frames presented
  ok   under the same bursty load two images pace at least 10% slower
       than three (25169 us vs 16807 us)
```

### All four exit criteria, with the numbers

1. **A swapchain presents at the display's rate.** 89 of 89 intervals
   within 10% of 16666 us, mean 16664 us, through the zero-copy path.
2. **Triple differs from double, measurably.** 25169 us against 16807 us
   under the same bursty load through Vulkan; 24918 us against 16793 us
   through raw `bq*` in `t_nwindow`. Structurally, 3 slots dequeued at
   once against 2.
3. **Two swapchains coexist over one window and destroy
   independently.** The superseded one reports
   `VK_ERROR_OUT_OF_DATE_KHR`; the survivor presents 20 of 20 after the
   other is destroyed.
4. **The zero-copy decision is runtime-observable and says why.** Both
   paths named by the driver through the debug-utils messenger in one
   run, the fallback carrying its reason.

And the layout evidence, which is the one thing no readback could give:
the operator confirmed the four bars, the border, the diagonal and the
corner square in run 2, and the pattern code has not changed since.

### D18 is closed: `minImageCount` stays 2

Two images present 90 of 90 frames. The compositor was never the limit;
the retry loop was.

### What it cost, and what that says

Twelve hardware runs. The two-buffer failure took nine of them, and the
cause — a retry loop with no idle in it, spinning binder at ~17000
transactions a second into the compositor it was waiting for — was
inferred wrongly five times before the loop was made to report its own
rounds and per-mode timings. **That instrument should have been the
first thing built, not the sixth.** Every earlier diagnosis was read off
a probe that ran afterwards, on a window in a different state, and each
one produced a patch that changed the failure's spelling and not the
failure.

Two artefacts also shipped as something they were not — run 3's stale
`.nro` and run 11's stale driver archive — and both are now gated: every
log states its build, and packaging refuses a directory holding more
than one build, an unstamped artefact, or archives older than the Mesa
sources they were meant to be built from.


---

## Run 11 — t_nwindow PASSES, and the Vulkan test measured a stale driver (2026-08-08)

**Class: hardware (HW).** `t_nwindow` **PASS 118/118** — the first PASS
this phase — and `t_vk_swapchain` 114/116. Build
`2026-08-08T12:24:55Z 5995c12`. Logs in `docs/hw-logs/*run11*`.

### The sleep is the fix

```
  ok   2 buffers, interval 1: 90 of 90 frames presented
  ok   under the same bursty load two buffers pace at least 10% slower
       than three (24918 us vs 16793 us)
  ok   slow lane, 2 buffers: 10 frames presented with a three-second
       budget per dequeue (10)
```

**Exit criterion 2's numeric half has run, at the eleventh attempt.**
Double buffering paces 24918 us against triple's 16793 us under the same
bursty load — a 48% difference, where the check needs 10%. The slow lane
went from `frame 2 gave up after 3000094 us (23192 rounds)` to every
frame in ~2.3 ms and two rounds each.

Two-buffer dequeues now carry release fences — **88 of 90**, where every
previous run recorded zero.

### And the Vulkan test measured a driver from three days earlier

`t_vk_swapchain` still fails at two images, and the reason is not the
driver's: `build/mesa-nvk/src/vulkan/wsi/libvulkan_wsi.a` was dated 5
August. **Patch 0064 never reached it.** The `.nro` was built minutes
before shipping and linked against that archive.

The NVK build that should have produced it failed — the Docker daemon
was down — and said so. The command running it ended in an
unconditional `echo "built"` after a filtered pipeline, so a failure was
reported as a success. That is the second time this project has shipped
an artefact that was not what it claimed, and the first one cost a run
too.

The packaging staleness gate could not catch it. It asks whether an
artefact is older than the archives it links; here the artefact was
*newer*. It now also asks whether any tracked source under `mesa/src` is
newer than the archives, and refuses to package if one is. Broken in
both directions before being believed: touching `wsi_horizon.c` fails it
by name, rebuilding passes it again.

What that log does still show is real — three images at 89 of 89
intervals inside 10% of a refresh, both present paths, two coexisting
swapchains, no leak, no exit crash. Only the two-image lines measured
the wrong driver.


---

## Run 10 — we were starving the compositor (2026-08-08)

**Class: hardware (HW), twice.** `t_nwindow` 113/118 in applet mode and
113/118 in title-takeover mode, build `2026-08-05T18:45:48Z b246f2d`.
Logs in `docs/hw-logs/t_nwindow-run10-*`.

### The failing dequeue reported itself, and it was the loop

```
  note 2 buffers, interval 1: the failing dequeue made 8320 round(s);
       async=true 490818 us total, last 0x0000115d;
       async=false 499026 us total, last 0x0000115d
  note 2 buffers, interval 1: THE TIME — a buffer came back after 146 us
       (0 refresh(es)), in async=true
```

Both modes reached, both answering `NO_INIT`, and together **989 ms of
the 1000 ms budget spent inside `bqDequeueBuffer`** — roughly 17000
binder transactions a second into the compositor's own service. The loop
had no idle in it at all, because the release event is a level and
`eventWait` returns immediately.

And then `async=true` returned a buffer in **146 us, first attempt**,
after failing 8320 times in the second before it. The only thing between
the two was a `t_note` writing a line to the SD card. Run 9 has the same
shape with a different pause: `async=false` blocked 10 ms and delivered.

The slow lane closes it: a **three-second** budget changes nothing —
23192 rounds, same answer — and the two frames that did go through took
171 us and 149 us. **More asking never helps; stopping always does.**

So the producer was starving the consumer it was waiting for. Three
buffers never showed it because a slot is nearly always free and the
loop never spins; two buffers spin on every frame.

### Every earlier reading, and what was wrong with it

- **0059, 0061, 0063 — the dequeue mode.** Both modes are reached and
  both fail. The mode was never it.
- **0062 — slicing the wait.** It dressed a spin up as a wait, which is
  what hid this for five runs. Superseded by 0064.
- **Run 7's `async=false` in 145 us**, and run 9's 9996 us, and run 10's
  146 us in `async=true`: all the same fact seen three ways. What
  delivered the buffer was not the mode — it was the pause before it.
- **Run 9's "one buffer per second".** Wrong. It is one buffer per
  *pause*, and the second was our own budget.

### Applet mode is not the variable

The operator ran the same binary in applet mode and in title-takeover
(full memory) mode. Identical to within noise: 8320 rounds against 8270,
989 ms against 990 ms inside binder, 23192 against 23032 in the slow
lane, 146 us against 138 us for the buffer that arrives once the asking
stops. That was a live hypothesis and it is now closed.

### The fix

`svcSleepThread` for an eighth of a refresh between rounds, in
`t_nwindow` and as **patch 0064** in the WSI. Nothing against a 16.7 ms
frame, and enough to leave the compositor alone. The caller's deadline
stays at the top of the loop, so `vkAcquireNextImageKHR(timeout = 0)`
still answers `VK_NOT_READY` without ever reaching the sleep.


---

## Run 9 — one buffer per second, not one per refresh (2026-08-05)

**Class: hardware (HW).** `t_nwindow` 109/113, build
`2026-08-05T18:38:50Z df5cd57`. Log in
`docs/hw-logs/t_nwindow-run9-one-buffer-per-second-FAIL.log`.

Both two-buffer sessions, identically:

```
  note asked for 9 ms in BOTH modes — 1 round(s), release event fired 0
       time(s); last async=true 0x0000115d, last async=false 0x00000000
  note THE TIME — a buffer came back after 9996 us (0 refresh(es)), in
       async=false
  note after the late buffer, 1 of 10 further frames presented
```

### It is a rate, and the recovery arm is what proves it

The paced dequeue gives up after one second. The probe that runs next
gets a buffer from `async=false` in ~10 ms. The late buffer is then used
rather than returned — **one** frame goes through, and the next dequeue
burns its whole second again. That repeats, so it is not a startup
transient.

**A two-buffer window here delivers about one buffer per second.** The
same log has `3 buffers, interval 1: 90 of 90 frames presented`, mean
16344 us, 87 of 90 dequeues carrying a release fence. Two-buffer
sessions carry **zero** release fences, in every run so far.

### Two readings still fit, and run 10 separates them

Either `async=false` blocks and delivers in ~10 ms, and the paced loop
is somehow not reaching it; or a buffer becomes free at about one second
and the 10 ms is only how long the ask took once it nearly was.

Eight runs have inferred what the failing second was spent on, and run 9
showed the inference is unfalsifiable: `nw_dequeue` returns one Result,
and everything about that second had to be guessed from a diagnostic
running afterwards on a window in a different state. So `nw_producer`
now records rounds, cumulative time in each mode, and the last result of
each, and the failure prints them.

And a **slow lane**: two buffers, ten frames, a **three-second** budget
per dequeue, every dequeue's duration printed. Every dequeue in this file
is otherwise cut off at one second, which is exactly where the boundary
sits.

If those come back at ~1 s each, two-buffer FIFO on this compositor runs
at about 1 Hz and **D18 resolves to `minImageCount = 3`**. If they come
back at ~10 ms, the one-second budget was the whole defect.


---

## Run 8 — the fix did not move it, and that sharpened the question (2026-08-05)

**Class: hardware (HW).** `t_nwindow` 111/113 and `t_vk_swapchain`
114/116, build `2026-08-05T18:30:39Z dec5a7a`. Logs in
`docs/hw-logs/*run8*`.

`nw_dequeue` and the WSI acquire both asked `async=true` and then
`async=false` on every iteration. The two-buffer sessions still stop at
the third frame.

### What the same log says next to that

```
  note 2 buffers, interval 1: asked for 5000 ms — 77900 dequeue(s) in
       libnx's mode, ... last result 0x0000115d
  note 2 buffers, interval 1: async=false returned 0x00000000 after 134 us
  note 2 buffers, interval 1: dequeue failed at frame 2 -> 0x00006359
```

`nw_dequeue` had been asking **both** modes for the entire second before
the diagnostic ran, and both failed for that whole second. Then five
seconds of `async=true` passed and one `async=false` succeeded in 134 us.

**So `async=false` does not work at t = 0 and does work by t = 6 s.**
Run 7's measurement was real; its reading was too narrow. The mode
matters, but only once something has freed a buffer — and nothing on a
two-buffer window frees one for at least a second.

### The question, restated

Not which mode. **When does a buffer become free on a two-buffer window,
and does the window keep going once one does?** Every log so far is
blind to both: the ask asked one mode for five seconds and the other
once, and no session has ever continued past the stall.

Run 9 measures both. The diagnostic asks both modes every round and
prints the elapsed time and which mode won, turning "somewhere between
1 s and 6 s" into a number. Then it takes the late buffer and attempts
**ten more frames**, counting them: a startup transient and a permanent
stall are different defects and look identical in every log to date.

Patch 0063 stays. Its cost is one extra dequeue per acquire round when
the queue says it would block, and run 7's measurement — `async=false`
answering where `async=true` will not — is untouched by run 8.


---

## Run 7 — the mode that works, and it is the synchronous one (2026-08-05)

**Class: hardware (HW).** `t_nwindow` 111/113, build
`2026-08-05T18:24:09Z 12863c4`. Log in
`docs/hw-logs/t_nwindow-run7-async-false-works-FAIL.log`.

### async=false works, on the window that had just failed

```
  note 2 buffers, interval 1: asked for 5000 ms — 78166 dequeue(s) in
       libnx's mode, release event fired 78165 time(s), last result
       0x0000115d
  note 2 buffers, interval 1: async=false returned 0x00000000 after 145 us
  ok   2 buffers, interval 1: async=false produced a buffer where libnx's
       async=true could not, 78166 times running
```

Twice in the run — 145 us and 158 us — and it did **not** block inside
the compositor, which was the one behaviour that could not be ruled out
until it was tried.

**And position is ruled out.** The same probe ran before any paced
session and after all of them; `probe BEFORE, 2 buffers` and `probe
AFTER, 2 buffers` both get a buffer in ~100 us. Where a session sits in
the run is not the variable.

### The reading, and every observation it has to fit

Android's producer takes `async` to mean *this producer is in
asynchronous mode*: `queueBuffer` never blocks and older frames are
dropped, which costs the queue one buffer held in reserve. A two-buffer
queue with both buffers out cannot spare it. FIFO presentation is
synchronous by definition, so `async=false` is the mode that describes
what a swapchain is actually doing.

Everything measured across seven runs fits:

- **Three buffers** usually have a free slot, so `async=true` answers.
- **The reconstructed probe** queues two frames inside one refresh, the
  consumer drops one and frees its slot, so `async=true` answers there
  too — which is exactly why the reconstruction never reproduced the
  failure.
- **The concurrency count** says `WOULD_BLOCK` when three slots are
  exhausted and `NO_INIT` when two are. Two different conditions: the
  count running out, and the reserve not being satisfiable.

### And patch 0061 had the right mode all along

It used it wrongly. `async=false` was asked *only* after an `eventWait`
that had already spent the whole budget, and `async=true` was never
asked again — so the two runs that followed refuted the implementation,
not the idea, and I read them as refuting the idea. Both modes are now
asked on every iteration, `async=true` first because it is libnx's path
and the common case, with 0062's slicing keeping any one call from
eating the deadline.

**Patch 0063** carries it into the WSI, with one difference: the
fallback is skipped when the caller passed a zero timeout.
`vkAcquireNextImageKHR(timeout = 0)` must answer `VK_NOT_READY`
promptly, and `async=false` is the mode Android *permits* to block in
the server. It did not block on the window measured; that promise is not
worth risking on one measurement.

### D18 is not being taken

`minImageCount = 3` was the fallback if two buffers turned out to be
impossible here. They are not impossible. The decision stays open until
run 8 either presents 90 of 90 frames on two buffers or does not.


---

## Run 6 — the release event is a level, not an edge (2026-08-05)

**Class: hardware (HW).** `t_nwindow` 79/81 and `t_vk_swapchain` 114/116,
build `2026-08-05T18:11:16Z 503d008`. Logs in `docs/hw-logs/*run6*`.

Patch 0062 did not fix the two-buffer sessions. It was still the right
change — the old code returned a *mislabelled* timeout after one retry in
the wrong mode — but it was necessary, not sufficient.

### The two numbers, from the real failure

The diagnostic ran attached to the dequeue that actually failed:

```
  note 2 buffers, interval 1: asked for 5000 ms — 84328 dequeue(s) in
       libnx's mode, release event fired 84327 time(s), last result
       0x0000115d
```

**84327 `eventWait` returns in five seconds is 59 us apiece.** The event
comes back immediately every time: it is permanently signalled, a level
and not an edge, and it carries no information about a buffer having
been released. Waiting on it is a spin — in libnx's own
`nwindowDequeueBuffer` loop as much as in ours. Every design here that
read it as "a buffer came back" was reading a level as an edge, and the
only reason three buffers work is that the dequeue is probed *first*.

**And `async=true` answered `0x115d` — `LibnxBinderError_NoInit` — 84328
times running.** Thirty lines later in the same log, the reconstructed
probe's two-buffer window answered the same call with a buffer in 97 us.

### What that rules out, and what is left

Ruled out: the dequeue mode (0059, 0061), the release event (0062's
slicing), and the compositor simply never freeing a buffer — it freed
one in 97 us on a window in the same process with the same buffer count.

Left: **state**. Two windows, identical last three calls, opposite
answers. The one thing nothing has controlled for is *where in the run
each session happens* — the failing ones are the 2nd and 4th sessions,
the probe was the 6th and 7th.

So the same probe now runs **BEFORE** any paced session and **AFTER**
all of them, everything else held constant. If BEFORE starves and AFTER
does not, the variable is what the window has been through. If both
behave alike, position is ruled out and the difference is inside the
paced code path, which is the next thing to bisect.

The diagnostic also now tries `async=false` once at the end of the real
failure — the one mode never reached on a genuinely starved window, and
the one that Android's BufferQueue allows to block in the server. It may
not return; the log says so a line in advance and every line before it
is already flushed.

### The fallback, stated now rather than after another run

The numeric half of exit criterion 2 has never run, across four attempts
at it. If run 7 does not resolve the two-buffer case, the honest
conclusion is that this compositor cannot sustain FIFO on two buffers,
and then **`minImageCount` must become 3** — the surface currently
advertises 2 and `t_vk_swapchain` asserts it, which would make both a
promise the driver cannot keep. That is a decision, and it is recorded
here as pending rather than taken quietly.


---

## Run 5 — the probe answered, and it was the wrong question (2026-08-05)

**Class: hardware (HW).** `t_nwindow` 79/81, build
`2026-08-05T17:57:42Z 0c22d69`. Log in
`docs/hw-logs/t_nwindow-run5-probe-did-not-reproduce-FAIL.log`.

### The probe did not reproduce the failure

`nw_probe_starvation` built a session that looked like the failing one
and asked it the question. At two buffers:

```
  note starvation probe, 2 buffers: 1 dequeue(s) in libnx's mode over
       0 ms, release event fired 0 time(s), last result 0x00000000
  ok   starvation probe, 2 buffers: the compositor handed a buffer back
  note starvation probe, 2 buffers: it came back after 104 us
```

Sixty lines above, in the same log, the real two-buffer sessions still
die at frame 2. **A reconstruction that never reaches the state it was
built to examine reports success about nothing**, and four `ok` lines
say so in a way that reads like an answer. Fourth time this project has
produced a check like that; second one I wrote myself.

The instrument is now attached to the failure instead: when a dequeue in
`nw_session_present` fails, *that* window — in exactly the state that
made it fail — is the one asked for five seconds. There is nothing left
to reconstruct.

### What the log did measure, and it located the defect

Two things.

**A buffer came back with the release event never firing.** 104 us, zero
event fires. The compositor can free a slot without signalling, so a
producer that waits on the event alone stalls where one that probes
first does not.

**And the failing result names the culprit.** `0x00006359` is
`MAKERESULT(Module_Libnx, LibnxError_Timeout)` — the test's own budget,
not anything the BufferQueue said. The only branch that returns it is
the deadline check at the top of an iteration, which is reachable *only
after an `eventWait` succeeded*. So the sequence was:

1. ask in async=true → no buffer yet
2. wait on the release event, **with the whole remaining second as the
   timeout** → the event fires
3. ask once in async=false → fails
4. round the loop → the budget is gone → `LibnxError_Timeout`

**async=true was never asked again.** The compositor was not starving
the producer; the loop spent its entire deadline in one wait and then
gave up on one attempt in a mode nothing on this platform uses. Patch
0061 introduced step 3, and patch 0059 before it aimed at the same
non-problem.

### The fix, in both implementations

One mode — libnx's async=true, which `nwindowDequeueBuffer` uses on any
window that has a release event — and the wait sliced to two refreshes,
so a release that does not arrive costs one slice and another attempt
instead of the whole acquire. A timeout from `eventWait` is not a
failure; it is the reason to ask again, and the caller's deadline is
checked at the top of the loop where it belongs.

`t_nwindow` in commit `4a9a5f1`-adjacent; the WSI as **patch 0062**,
which also corrects the comment that stated 0061's reading as measured
fact. Cross build green, not verified on hardware.

Run 6 answers either way: if the fix is right the two-buffer sessions
present 90/90 and the pacing comparison finally runs; if it is wrong the
attached diagnostic reports from the real failure with numbers.


---

## Run 4 — the leak is gone, the crash was the leak, and two still starves (2026-08-05)

**Class: hardware (HW).** `t_nwindow` 48/50 and `t_vk_swapchain` 114/116,
both stamped `2026-08-05T14:31:36Z e0bb31f` in their second line. Logs in
`docs/hw-logs/*run4*`.

### Two answers, and the second one took four runs to ask

**Patch 0060 fixed the leak.** `device destroy refused` does not appear
anywhere in the swapchain log. Runs 2 and 3 both ended with `live mem=33
va_ranges=33 mappings=33`. The check that says so is the `t_log_scan`
version, which fails when it cannot read the log rather than reporting
"not found" — so unlike run 2, its `ok` is evidence.

**The exit crash was the leak.** The operator reports the console
returning to the homebrew menu on `+`. The hypothesis was written after
run 1 and this is the first run in which it could be tested at all,
because runs 2 and 3 both still leaked.

That closes the reference cycle 0058 created: a VA binding held a
reference to the memory it binds, and every `nvkmd_mem` carries its own
VA, so each object held the other. 0060 excludes the self-binding.

### And the diagnosis behind 0061 was wrong

Two registered buffers still present two frames and then stop. The
failing result is the one worth reading:

```
  note 2 buffers, interval 1: dequeue failed at frame 2 -> 0x00006359
```

`0x6359` is `MAKERESULT(Module_Libnx, LibnxError_Timeout)` — **this
test's own one-second wait expiring**, not an error from the
BufferQueue. The release event never fires. Patches 0059 and 0061 both
chose between dequeue modes; no dequeue mode is ever reached.

The same failure appears through Vulkan, in an independent
implementation: `vkAcquireNextImageKHR at frame 2 -> VK_TIMEOUT`.

What the log does distinguish, and it is new:

```
3 registered, all 3 held: 0x00001d5d = LibnxBinderError_WouldBlock
2 registered, both held:  0x0000115d = LibnxBinderError_NoInit
```

Different conditions, not one limit. And with three buffers the release
path plainly works — `largest pending-buffer count 2`, a dequeue that
waited 11869 us, 87 of 90 dequeues carrying a release fence.

### What libnx actually does, read rather than assumed

`nx/source/display/native_window.c`, `nwindowDequeueBuffer`:

```c
if (eventActive(&nw->event)) {
    do {
        eventWait(&nw->event, UINT64_MAX);
        rc = bqDequeueBuffer(&nw->bq, true /* async */, ...);
    } while (R_VALUE(rc) == MAKERESULT(Module_LibnxBinder,
                                       LibnxBinderError_WouldBlock));
} else
    rc = bqDequeueBuffer(&nw->bq, false /* async */, ...);
```

So `async=true` is libnx's path on any window that has a release event —
which the default window does — and `async=false` is a mode nothing on
this platform is known to use. Patch 0061 switched to `async=false`
after the event fired, which is neither libnx's path nor reachable when
the event does not fire.

### The experiment that replaces the guessing

`nw_probe_starvation` (commit `ba1f549`) hands the compositor every
registered buffer and then does nothing but ask, for five seconds — 300
refreshes — reporting how many dequeues it made, **how many times the
release event fired**, the last result, and how long the first success
took. Run at three buffers and then at two, so the two-buffer answer has
a working one beside it.

Its last act tries `async=false`, the mode Android's BufferQueue allows
to block inside the server. It may not return, and that would itself be
a distinct answer from "the compositor never releases". It is last on
purpose: every line above it is already on the SD card, and the log says
in advance what a missing next line means.

**Nothing in the driver changes until this reports.** Two patches have
now been written against a reading of this failure and neither touched
it.

### The thing this makes suspicious

The surface advertises `minImageCount = 2` and `t_vk_swapchain` asserts
it. If two images cannot sustain FIFO on this compositor, that number is
a promise the driver cannot keep, and both the value and the check that
blesses it are wrong. Not changed yet: the probe decides whether the
limit is the platform's or ours.


---

## Run 3 — it measured the previous build, and the fault is mine (2026-08-05)

**Class: hardware (HW), and it tested nothing that was asked of it.**
`t_nwindow` 48/50 and `t_vk_swapchain` 113/115, the same counts and the
same failures as run 2. Logs in `docs/hw-logs/*run3-STALE-BINARIES*`.

The `.nro` that ran were run 2's. Patches 0060 and 0061 were not in
them, so what these two logs record is the behaviour those patches were
written to change, measured a second time.

### Why it was read wrong first

They are not copies of run 2's logs. Every timing differs, every heap
address differs, the syncpoint initial values differ:

```
run 2:  the NvMap has an id the compositor can look up (36412)
run 3:  the NvMap has an id the compositor can look up (37092)
run 2:  3 buffers at interval 0: mean 8152 us against 16352 us
run 3:  3 buffers at interval 0: mean 8177 us against 16353 us
```

A genuine second execution of unchanged code and a fix that changed
nothing produce the same artefact. Nothing else in either log
distinguishes one build from the other.

What separated them was luck. Mesa's `vk_logi` prints `__FILE__` and
`__LINE__`:

```
../src/vulkan/wsi/wsi_horizon.c:1781: the swapchain presents zero-copy
```

and the source that was supposed to be running has that call at 1805.
The shipped zip was then checked and did contain the intended build —
it holds `"the log could not be read back"`, a string that exists only
in the run-3 binaries, and its sha256 matches the manifest. So the
batch was right and what reached the SD card was not.

### The defect is not the SD card

It is that **the logs carried no build identity**, and the only reason
this was caught is that a line number happened to move. Fixed in
`d75f7b8`:

- `scripts/gen-build-id.sh` emits a UTC stamp plus the repository HEAD
  (`-dirty` when the tree has edits).
- Both build paths regenerate it on every build — `FORCE` in the
  Makefile, `build_always_stale` in Meson. Verified in both: a rebuild
  with no source change advances the stamp inside the `.nro`.
- `testfw`'s `main()` prints it as the second line of every log, as one
  string literal with its marker included, so the bytes in the binary
  and the bytes in the log are the same bytes.
- `scripts/package-horizon.sh` reads that marker back out of each
  `.nro`, records it in the manifest, and **refuses** to write a
  manifest over a directory holding more than one build, or one
  artefact carrying no stamp.

Both refusals were provoked before being believed — an unstamped `.nro`
gives `error: no build id in: t_display.nro`, two stamps give `error: 2
different build ids`, and restoring the one-build source passes again.

### What run 3 is still worth

One thing: run 2's failures are reproducible rather than one-off.
`live mem=33 va_ranges=33 mappings=33` appeared again, and both
two-buffer sessions died at frame 2 again. That is a second sample of a
build that is about to be replaced, which is worth little, but it is not
nothing.

The three questions run 3 was to answer — 0061, 0060, and then the exit
crash — are all still open, in that order, for run 4.


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
| D20 | The Mesa commit `ebf2e31` that `mesa-patches/` did not carry (was numbered D18 — see the collision note below) | **CLOSED 2026-08-09 by exporting it as patch 0071.** The commit is now tracked, `scripts/apply-mesa-patches.sh --list` reports **73 applied, 0 pending, 73 in `mesa-patches/`**, and a clean checkout rebuilt from the series reproduces the tree run 20 was measured on. Two things about it were not fit to track and were fixed in the export rather than carried: it had none of the four header fields `mesa-patches/README.md` requires, and its `Evidence` line cited `t_vk_mode, 2026-08-09, handheld, PASS 56/56` — **a test and a log that do not exist in this tree**, so the one field this project treats as non-optional pointed at nothing. The citation was replaced with `docs/hw-logs/t_vk_wsi_mt-run20-review-fixes-PASS.log`, which carries the identical measurement (stride 5120 B, block height 32 GOBs log2 5, kind 0xfe, allocation 3932160 B) and has its digest in `MANIFEST.sha256`. **`tests/t_vk_mode.c` was not reconstructed** — it is somebody's deleted work and rebuilding it on a guess would be worse than recording that it went. Was: **open, raised with the owner.** The `mesa/` checkout is at `ebf2e31` ("vulkan/wsi: Horizon, report the layout that was registered"); `mesa-patches/` holds 70 patches and `85638f8` (patch 0070) is its parent. So the checkout has 71 commits against 70 tracked patches, and `mesa/` is ignored by this repository, which is why nothing in `git status` shows it. The commit's own message cites `t_vk_mode, 2026-08-09, handheld, PASS 56/56` — and **`tests/t_vk_mode.c` does not exist in this tree**, nor does a log for it under `docs/hw-logs/`. A clean checkout rebuilt from `mesa-patches/` does not reproduce the binary run 20 was measured on, which is the rule in CLAUDE.md this breaks. Three ways out: export it as patch 0071 and restore the test and its log, drop it from the checkout and re-run, or keep it and say so at every run. Not chosen here — it is unfinished work belonging to whoever wrote it, and reconstructing a file that was deleted is not something to do on a guess |
| D15 | Adopt `nxvk`'s channel warm-up/calibration ramp (`docs/reference-analysis.md` § 12.5.2) in `horizon/channel/` | **CLOSED 2026-08-04: no, and here is the number.** The ramp diagnoses a ring-size fault by kicking synthetic push buffers of increasing size at every channel creation, CPU-waiting each rung. The entry *count* was already bounded by construction — `horizon_gpu_submit` refuses what will not fit `GPFIFO_QUEUE_SIZE` — so the open question was the *size of one entry*, which was checked only for being non-zero. `t_pbsize` walked it on hardware: **every rung from 32 up to 524288 dwords (2 MiB) executed to its last dword**, each verified by a semaphore release at the END of the entry rather than by the submit being accepted. There is no limit to bound in that range, so nothing is added to `horizon_gpu_submit` and nothing is paid at channel creation. Should a limit ever appear it is a number to enforce, not a ramp to run |

### Note on the D18 collision (found in the PR 9 review, 2026-08-09)

Two decisions were given the number **D18**. The first, from Phase 6, is
`minImageCount` staying at 2; it was closed by run 13 and the summary block at the
top of this file has said so since. The second was opened on 2026-08-09 for the
Mesa commit `ebf2e31` that `mesa-patches/` did not carry.

So the file said both "**Open decisions: D7 only.** D18 closed" at the top and
"D18 | ... **open, raised with the owner**" in the table, about different
decisions, three thousand lines apart. Either row read alone was true.

Resolved the way the D9 collision was: the older claimant keeps the number. D18
is `minImageCount`, which is cited by that name in the run 13 record and in
`docs/wsi.md`. The Mesa commit becomes **D20** — not D19, which is
`async=false` — and is cited nowhere else, so renumbering it changes no other
file and loses no history.

Worth naming rather than quietly renumbering, because this is the second time.
The number is assigned wherever the decision is first written down, and the two
places that happens — the summary block and the table — do not check each other.

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

