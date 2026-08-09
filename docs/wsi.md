# WSI — presenting through `nwindow`

Target: a `VK_KHR_swapchain` implementation over Horizon's VI compositor via libnx
`nwindow`, with no global state, real double/triple buffering, and a genuine zero-copy
path whose fallback is explicit and logged.

This is **Phase 6**. It is implemented, as patches 0049-0056 in `mesa-patches/`.
The design below was written from the audit in `docs/reference-analysis.md`
before any of it existed; where building it disproved something, the paragraph
says so and names what disproved it. Those corrections are § 2.2, § 2.3, § 2.5,
§ 3.1, § 4 and § 5 — the whole of what the design got wrong is in this file
rather than in a changelog.

**It has now run on a console** (2026-08-05): zero-copy on the first attempt,
90 frames at a mean of 16662 us with 89 of 89 intervals inside 10% of a 60 Hz
refresh, and two swapchains coexisting over one window. Three defects came out
of that run and two of them are fixed here; see `STATUS.md`. Two paragraphs
below were disproved by it rather than by reading, and say so: § 2.3's retry
set and the alignment prediction that did not happen.

---

## 1. What the reference achieved, and what it did not

The reference's `winsys/wsi/wsi_common_switch.c` does present on real hardware, and its
two hard-won facts are load-bearing:

1. `NvGraphicBuffer.header.num_ints` must be set to
   `(sizeof(NvGraphicBuffer) - sizeof(NativeHandle)) / 4`, with `num_fds = 0`. Left at 0,
   `bqSetPreallocatedBuffer` returns success but marshals an empty buffer, and
   `nwindowDequeueBuffer` blocks forever on frame 0.
2. `nvFenceInit()` must be called before `nwindowDequeueBuffer` / `nvMultiFenceWait`.

### 1.1 There are two different WSI backends in the reference tree

This matters more than any single finding, and it is easy to get wrong:

| Copy | Lines | `minImageCount` | Swapchain-recreate handling |
|---|---|---|---|
| Inside `patches/switch-nvk-mesa-25.0.7.patch` (`+++ b/src/vulkan/wsi/wsi_common_switch.c`, `@@ -0,0 +1,640 @@`) | 640 | **3** (triple) | `g_zc_owner` ownership transfer |
| Tracked standalone `winsys/wsi/wsi_common_switch.c` | 616 | **2** (double) | none |

`winsys/wsi/apply-wsi-switch.sh:15` copies the **616-line stale** file over the 640-line one
the patch just installed, silently reverting both the triple-buffering change and the
recreate fix. `BUILD_AND_RUN.md:65-67` calls running that script "REQUIRED";
`REPRODUCE.md:52-54` says the tracked file "is NOT the build source". Following the build
document produces a worse driver than ignoring it.

So triple buffering **does** exist in the reference — in the big patch, which is present in
`master` and `switch-port/triple-buffer` and absent from the other two (whose small patch
contains no WSI backend at all). The branch name is accurate about the patch and
misleading about the tracked file.

**The `g_zc_owner` global is instructive rather than merely bad.** It was added to fix a
hardware-confirmed `0xf59` crash: a `NWindow`'s scanout buffers are registered globally via
`nwindowConfigureBuffer`, and Vulkan creates the new swapchain *before* destroying the old
one, so the second registration collides with the first. The reference solved a genuine
ownership problem with a file-scope pointer. Our design solves the same problem by making
**the surface** own the registration (§ 2.5) — the constraint is real, the global is not
the only answer.

### 1.2 What it did not achieve

- **"Zero-copy" removes the CPU memcpy but not a per-frame GPU copy.** The swapchain
  requests `WSI_IMAGE_TYPE_CPU` (`wsi_common_switch.c:494-496`), and Mesa's
  `wsi_cpu_image_needs_buffer_blit` returns `true` unless `wants_linear` is set — which it
  is not. So `get_blit_type` yields `WSI_SWAPCHAIN_BUFFER_BLIT`, and every present submits
  a full `CmdCopyImageToBuffer` into a host-visible buffer that the zero-copy path then
  never reads. The `WSI_SWAPCHAIN_NO_BLIT` path was planned and never implemented.
- **Presentation is CPU-synchronised.** `nwindowQueueBuffer` is called with a `NULL` fence
  (`wsi_common_switch.c:374`); the driver instead does an infinite
  `WaitForFences(..., ~0ull)` first. The `acquire_fence` field is stored and never read.
- **Mutable process-global statics** live in the present/acquire paths
  (`wsi_common_switch.c:314`, `:408-410`) — a debug counter and eight profiler
  accumulators, shared across all swapchains and threads, updated non-atomically. The patch
  version adds `g_zc_owner` on top of these.

---

## 2. Design

### 2.1 No global state

```c
struct horizon_wsi_swapchain {
   struct wsi_swapchain      base;
   NWindow                  *window;        /* supplied by the app's surface  */
   uint32_t                  image_count;   /* 2..4, actually honoured        */
   struct horizon_wsi_image *images;        /* heap-allocated, image_count    */
   bool                      zero_copy;
   VkExtent2D                extent;
   VkFormat                  format;
   /* fallback state, only valid when !zero_copy */
   Framebuffer               fb;
   bool                      fb_created;
};
```

Every counter, debug flag and profiler accumulator lives in this struct. Two swapchains
must be creatable, presentable and destroyable independently — that is a Phase 6 exit
criterion, and it is the check that makes the no-globals rule enforceable rather than
aspirational.

`nwindowGetDefault()` is called by the **application**, passed through
`VkViSurfaceCreateInfoNN`, and stored on the surface. The WSI never reaches for a default
window.

### 2.2 Slot ownership

The reference uses an implicit identity map between `nwindow` slot and image index, with
only a bounds check. We keep the identity mapping (it is what
`nwindowConfigureBuffer(slot, gb)` establishes) but track state explicitly:

```c
enum wsi_horizon_slot_state {
   WSI_HORIZON_SLOT_FREE,      /* owned by the compositor, not dequeued       */
   WSI_HORIZON_SLOT_ACQUIRED,  /* dequeued, handed to the app                 */
   WSI_HORIZON_SLOT_QUEUED,    /* handed back to the compositor               */
};
```

**Corrected: there is no RENDERING state, and there cannot be.** The design
listed four; the implementation has three. Nothing at this layer can observe the
application starting to render — the WSI hands an image over at
`vkAcquireNextImageKHR` and hears nothing again until `vkQueuePresentKHR`, so
ACQUIRED covers both and a fourth state would be a name for a transition that
never fires. Disproved by writing the vtable: there is no entry point between
`acquire_next_image` and `queue_present` (`wsi_common_private.h`, `struct
wsi_swapchain`).

**And the producer API is not `nwindowDequeueBuffer`.** libnx's `NWindow` keeps
a single `cur_slot` and refuses a second dequeue before the first is queued or
cancelled (`native_window.c`: `if (!nw->slots_configured || nw->cur_slot >= 0)`).
Vulkan does not permit that restriction: with `minImageCount = 2` and a
three-image swapchain an application may hold two images acquired, and blocking
indefinitely for them is *valid usage*. So dequeue, queue and cancel go through
libnx's `bq*` wrappers on `nw->bq` — public API, public field — and this backend
keeps the ownership itself. Registration and teardown stay on
`nwindowConfigureBuffer` and `nwindowReleaseBuffers`. `t_nwindow` measures how
many slots the BufferQueue will actually hand out at once, because the design
above rests on the answer being at least two.

Illegal transitions are programming errors and are reported. `release_images` — used on
`VK_ERROR_OUT_OF_DATE_KHR` recovery — must return `ACQUIRED` slots to the compositor with
`nwindowCancelBuffer`. The reference only clears a `busy` flag
(`wsi_common_switch.c:441-449`), permanently leaking dequeued slots from the
BufferQueue's point of view.

### 2.5 Scanout-buffer registration is owned by the surface

`nwindowConfigureBuffer` registers buffers on the `NWindow`, which belongs to the
**surface**, not to any one swapchain. Vulkan's recreation contract creates the new
swapchain before destroying the old, so two swapchains legitimately coexist over one window.

```c
struct horizon_wsi_surface {
   NWindow                      *window;
   struct horizon_wsi_swapchain *registered;  /* current owner, may be NULL */
};
```

- `create_swapchain` claims the registration: if `surface->registered` is non-NULL and is
  not us, release its buffers, mark it non-presentable, and take ownership.
- `destroy_swapchain` releases only if `surface->registered == self`.

**Corrected: releasing its buffers is a disconnect, not a per-slot cancel.**
`nwindowReleaseBuffers` disconnects the producer, and the BufferQueue frees
every slot it believes the producer holds. Cancelling slot by slot first would
*additionally* hand back buffers the application is still rendering into —
which is a worse race than the one it would prevent, since the old swapchain's
`VkImage`s stay valid until `vkDestroySwapchainKHR` and its presents are already
being refused. So: mark non-presentable, disconnect, and bring the bookkeeping
into line. Teardown cancels outstanding slots only while this swapchain is still
the owner.

This is the same algorithm as the reference's `g_zc_owner`, with the state on the object
that actually owns it. It is also what makes "two independent swapchains" a meaningful
Phase 6 test rather than a formality.

### 2.3 Buffer count

- `minImageCount = 2`, `maxImageCount = 4`.
- The requested count is **clamped into `[minImageCount, maxImageCount]`**, not silently
  truncated downward only. A request of 1 must not produce a one-image swapchain.
- ~~`nwindowSetBufferCount` is called to match. The reference never calls it.~~
  **Corrected: there is no such function.** libnx has no `nwindowSetBufferCount`
  and no `bqSetBufferCount`; the buffer count is however many slots
  `nwindowConfigureBuffer` registered, and nothing else says it. Disproved by
  reading `libnx/include/switch/display/native_window.h` and
  `buffer_producer.h`, neither of which declares one. The criticism of the
  reference was therefore also wrong, and it is withdrawn here rather than left
  standing.
- **A full BufferQueue does not always say `WOULD_BLOCK`.** Measured: a
  two-image swapchain with both buffers queued gets `LibnxBinderError_NoInit`
  — Android's `NO_INIT` — and an acquire that retried only on `WOULD_BLOCK`
  turned it into `VK_ERROR_OUT_OF_DATE_KHR` at the third frame. Three images
  never reach the condition. The retry set is `WOULD_BLOCK`, `NO_INIT` and
  `INVALID_OPERATION`; anything else still ends the acquire.
- **How many slots the producer may hold: all of them.** Measured on
  hardware — 3 of 3 registered buffers dequeued at once, and 2 of 2. libnx's
  single `cur_slot` is its `NWindow` wrapper's rule, not the platform's.
- The knob that does exist is the **swap interval**, per queued buffer
  (`BqBufferInput::swapInterval`). `VK_PRESENT_MODE_FIFO_KHR` is interval 1 and
  `VK_PRESENT_MODE_IMMEDIATE_KHR` is 0 — which libnx documents as honoured only
  with three or more buffers registered, so a two-image swapchain gets FIFO
  behaviour whatever it asks for, and the backend says so at creation.
- Double (2) and triple (3) buffering must be observably different in frame pacing; Phase 6
  records measurements for both.

### 2.4 Format gating

Only formats whose block size the `NvGraphicBuffer` layout can express are offered, and the
requested format is **re-validated in `create_swapchain`**. The reference offers only
RGBA8/BGRA8 but never re-checks `pCreateInfo->imageFormat`, so a non-4-byte format silently
corrupts the pixel stride.

---

## 3. Zero-copy

The rendered `VkImage` *is* the scanout buffer. Requirements:

| Requirement | Source |
|---|---|
| Image is block-linear with a NIL-computed `block_height_log2` | NIL |
| `row_stride_B` divides exactly by the format's bytes-per-pixel | validated |
| The image's memory is a single `NvMap` whose id we can obtain | `nvkmd_mem_export_scanout_id`, not `horizon/surface/` — nothing in the struct needs horizon_gpu's own code, so it lives in `wsi_common.h` where the driver fills it in |
| `size_B` and `offset_B` fit in the graphic buffer's 32-bit fields | validated |
| The display kind is compatible with the image's PTE kind | validated, not forced |

If any fails, zero-copy is **declined** with a logged reason and the copy fallback is used.
"Declined" is a normal outcome, not an error.

### 3.1 Eliminating the residual blit

Zero-copy is only real once the swapchain uses `WSI_SWAPCHAIN_NO_BLIT`. That requires the
Horizon WSI backend to present renderable images directly rather than going through
`wsi_cpu_image_params`. This is tracked as a Phase 6 work item, not an optimisation:
without it, "zero-copy" still costs a full-resolution GPU copy per frame.

**Done, and here is how.** A new `WSI_IMAGE_TYPE_HORIZON` yields
`WSI_SWAPCHAIN_NO_BLIT` from `get_blit_type`, and its images are ordinary
`VK_IMAGE_TILING_OPTIMAL` ones with dedicated device-local memory — no modifier,
no dma-buf, no CPU mapping. `wsi_cpu_image_params` is what the *fallback* uses,
and only the fallback.

**The one thing the design did not say: how the WSI learns the layout.**
`vkGetImageSubresourceLayout` is invalid usage on an OPTIMAL image, so the row
stride, the block height, the page-table kind and the memory's name all come
from the driver, through two callbacks on `wsi_device::horizon` — the same shape
as the `win32` and `metal` callback structs Mesa already has. Underneath them,
nvkmd gains two operations no DRM platform needs: `export_scanout_id` (an NvMap
id, where DRM exports a dma-buf) and `get_syncpt_fence` (where DRM exports a
sync file).

**And the check that would have been silently wrong.** NIL picks the sector
ordering inside a GOB from the device type: an SoC gets `TegraColor`, a desktop
Fermi gets `FermiColor` (`nil/tiling.rs:144-151`). Both are 512-byte 64x8 GOBs,
so accepting the wrong one passes every size and stride check and puts a
scrambled image on screen. `TegraColor` is what Horizon's display block reads as
"generic 16Bx2", and it is the only one accepted.

**And it is right, confirmed by an operator on 2026-08-08.** The scrambling this
paragraph predicts is what the pattern test exists to catch — four bars, a
border, a diagonal, a corner square, held on screen for two seconds — and the
answer from the console is that the image comes out correct every time it has
been shown. That is the only evidence there can be: a presented frame cannot be
read back, and a GPU readback would write and read with the same layout and
agree with itself. Scope of the claim: 1280x720 handheld, `R8G8B8A8_UNORM`,
`block_height_log2 = 4`. Another resolution, format or block height is another
agreement and has not been shown to anybody.

### 3.1.1 The alignment that turned out not to matter

Predicted before the first run, and it did not happen: libnx creates its
framebuffers' `NvMap` with `align = 0x20000` while a swapchain image's memory
is aligned to whatever NIL asked for, which is smaller.
`nwindowConfigureBuffer` accepted ours. So 128 KiB is libnx's habit rather than
the display block's requirement — at least at 1280x720, RGBA8, block height 16.
Recorded because the prediction was written down, and a prediction that is
never marked either way is not a prediction.

### 3.2 Fallback

CPU copy into a libnx `Framebuffer` (`framebufferCreate` + `framebufferMakeLinear`), with
`armDCacheInvalidate` on the source before reading GPU-written pixels. The fallback is
documented, measured, and never selected silently — the chosen path and its reason are
logged once per swapchain creation.

---

## 4. Acquire and present

### Acquire

```
nwindowDequeueBuffer(window, &slot, &multi_fence)   ── check the Result
  ├── validate slot ∈ [0, image_count)
  │     └── invalid → nwindowCancelBuffer + VK_ERROR_OUT_OF_DATE_KHR
  ├── record multi_fence as the slot's producer fence
  ├── slot state → ACQUIRED
  └── return slot
```

~~The producer fence becomes a **wait dependency of the application's first submit** writing
that image. It is not waited on by the CPU.~~ The reference CPU-waits it with a 1 s timeout
and discards the result (`wsi_common_switch.c:328`), so a timeout is treated as "the
compositor is finished" and the app may render over a buffer still being scanned out.

**Corrected: the fence is waited on by the CPU, inside acquire, and that is the
right answer today.** Two things disproved the design:

1. `nvkmd_horizon_ctx_wait` performs a submit's waits **on the CPU** — this
   backend has no GPU-side syncpoint acquire (`nvkmd_horizon_ctx.c`, and the
   comment there says so). Expressing the release fence as a dependency of the
   application's submit would therefore have moved the same stall later without
   removing it, while costing a `vk_sync` that has to be created from a fence no
   channel of ours produced.
2. CLAUDE.md's rule 6 lists **swapchain acquire** among the places a CPU stall is
   allowed, alongside `vkWaitForFences`. This is that place.

What is kept from the design is everything the reference got wrong about it: the
caller's `timeout` is honoured rather than replaced by a fixed second, the
`Result` is checked rather than discarded, and a failed wait cancels the slot
back to the compositor instead of rendering into it.

When cross-channel GPU waits exist (`docs/synchronization.md` § 4), this becomes
a `vk_sync` and the stall goes. The one line that has to change is in
`wsi_horizon_acquire_zero_copy`.

#### The retry loop, and the two things about it that were wrong

Neither was in this document when it was written; both cost hardware runs, so
they belong here.

**The window's release event is a level, not an edge.** `nwindowCreate` obtains
it with `binderGetNativeHandle(&nw->bq, 0x0f, &nw->event)` and libnx's own
`nwindowDequeueBuffer` loops `eventWait(UINT64_MAX)` then
`bqDequeueBuffer(async=true)` while the result is `WOULD_BLOCK`. Measured on
console: **84327 `eventWait` returns in five seconds, 59 us apiece.** It is
permanently signalled and carries no information about a buffer having come
back. A retry loop built on it has no idle in it at all.

**So the loop must sleep, and not sleeping starves the compositor it waits
for.** Measured on the failing window: 8320 rounds in one second, both dequeue
modes reached, both answering `NO_INIT`, and **989 ms of the 1000 ms budget
spent inside `bqDequeueBuffer`** — roughly 17000 binder transactions a second
into the compositor's own service. A buffer arrived 146 us after the asking
stopped, and a three-second budget changed nothing (23192 rounds, same answer).
Three images never showed it because a slot is nearly always free and the loop
never spins; two images spin on every frame. The acquire now sleeps an eighth
of a refresh between rounds (`svcSleepThread`), and two images present 90 of 90.

**The `async` flag is not a preference.** Android's producer reads it as *this
producer is in asynchronous mode* — `queueBuffer` never blocks and older frames
are dropped — which costs the queue one buffer held in reserve. libnx passes
`true` on any window that has a release event. The acquire asks that first and
falls back to `async=false` in the same round when the queue says it would
block, except at a zero timeout, where `async=false` is the mode Android permits
to block in the server and `VK_NOT_READY` has to be prompt.

Five patches were written against wrong readings of this failure before the loop
was made to report its own rounds and per-mode timings. That instrument should
have come first: every earlier diagnosis was inferred from a probe that ran
afterwards, on a window in a different state.

### Present

```
  ├── take the render-completion fence for this image
  ├── convert it to an NvMultiFence
  ├── nwindowQueueBuffer(window, slot, &multi_fence)   ── check the Result
  └── slot state → QUEUED
```

Passing the fence lets the display block wait GPU-side. The reference passes `NULL` and
discards the return value, so a failed queue is reported to the application as
`VK_SUCCESS`.

**And a fence handed over is only worth anything if nobody has already waited for
it.** Mesa's WSI submits a pre-present job that waits on the application's
present semaphores; `nvkmd_horizon_ctx_wait` was doing that wait on the CPU, so
by the time `bqQueueBuffer` ran the render had already finished and the fence
was decoration. Patch 0056 skips the CPU wait for a dependency already submitted
to the **same channel**, which a GPFIFO executes in order — the hardware
enforces the ordering whether or not anyone waits for it. Waits on any other
channel still happen on the CPU, as before.

**When there is no fence to hand over.** `get_fence` bounds its wait for the
submit to *reach* a channel, not for the work to finish. If that times out, or
the driver has no such callback at all, the present waits for the render on the
CPU and says so once — because queueing with neither a fence nor a wait puts a
half-drawn frame on screen, which is the class of silent success this backend
exists to avoid.

### The silent-no-op hazard

In the reference, if `zero_copy` is set but a slot was never configured, and no fallback
framebuffer exists either, `queue_present` falls through both branches and returns
`VK_SUCCESS` having presented nothing (`wsi_common_switch.c:437-438`). Our present path has
no fall-through: every path either queues, copies, or returns an error.

---

## 5. Resize and recreation

- `nwindowSetDimensions` on creation.
- A dimension change detected at acquire returns `VK_ERROR_OUT_OF_DATE_KHR`.
  **Corrected: the change has to be read from the consumer, not from the
  window.** `nwindowGetDimensions` returns `NWindow::width` when it is set, and
  this backend is what sets it — at creation, through `nwindowSetDimensions`. A
  check written that way compares the swapchain's extent with itself and can
  never fire. The consumer's own answer is `NWindow::default_*`, which libnx
  fills from the BufferQueue's output at connect and at every queue
  (`native_window.c`, `_nwindowUpdate`); the zero-copy path records
  `BqBufferOutput::width/height` from each queue for itself, and the copy path
  picks up what libnx recorded. Docking the console, which takes the display to
  1920x1080, is what exercises it.
- Surface capabilities read the same field for the same reason, so a swapchain
  created after a mode change is not handed the previous one's extent.
- Recreation with an `oldSwapchain` must return every slot to the compositor before
  releasing buffers, in a defined order, and must be safe when the old swapchain has
  outstanding acquired images.
- Partial configuration failure during creation (some slots registered, one fails) unwinds
  every already-registered slot before falling back. The reference's behaviour in this case
  is untested and its safety is unknown.

---

## 6. Error handling

Every libnx call in the WSI path is checked: `nvFenceInit`, `nwindowDequeueBuffer`,
`nwindowQueueBuffer`, `nwindowCancelBuffer`, `nwindowConfigureBuffer`,
`nwindowSetDimensions`, `nwindowSetBufferCount`, `nvMultiFenceWait`. The reference checks
only `nwindowDequeueBuffer`.

`nvFenceInit()` is refcounted or scoped to the device rather than called per swapchain;
the reference calls it on every swapchain creation and never calls `nvFenceExit`.

Surface support (`vkGetPhysicalDeviceSurfaceSupportKHR`) reports the queue families that
can actually present, not unconditional `true`.

---

## 7. Threads

Written on 2026-08-09, after `t_vk_wsi_mt` ran on hardware. Until then
"anything multi-threaded" was on `STATUS.md`'s *unverified* list while
the code reasoned about concurrent creation and eviction in its
comments, which is the worst combination: an argument nobody had
executed.

### 7.1 What the application owes, and what the backend owes

Vulkan externally synchronises a **swapchain** in
`vkAcquireNextImageKHR`, `vkQueuePresentKHR`,
`vkReleaseSwapchainImagesEXT` and `vkDestroySwapchainKHR`; a **surface**
in `vkCreateSwapchainKHR`; and a **queue** in `vkQueueSubmit` and
`vkQueuePresentKHR`. Everything a swapchain keeps to itself — slot
states, `next_image`, `reported`, `slots_requested`, the release fences
— is therefore the application's to protect, and this backend takes no
lock for any of it. That is not laziness; a lock there would be a lock
the specification already paid for.

**What is left is the state two swapchains share, and that is the
surface.** `surface->lock` covers `surface->owner` and every
swapchain's `presentable` flag, because those are the one thing an
application cannot synchronise: it does not know that creating S2
retires S1.

The rule the lock enforces, stated once: **only the swapchain that owns
the window's registration may touch the window.** Three operations
touch it — cancelling slots, `nwindowReleaseBuffers`, and closing the
copy fallback's `Framebuffer` — and all three run under the lock, after
an ownership test.

### 7.2 The pairing that is legal and unsynchronised, and where it bit

`vkDestroySwapchainKHR(old)` beside `vkAcquireNextImageKHR(new)` names
two different objects, so the specification requires no synchronisation
between them and an application may do it. So may the single-threaded
version of the same thing, which is just the ordinary recreation order:
create the new swapchain naming the old, then destroy the old.

**That is where the one real defect was.** `framebufferClose` is a
window operation wearing a private name: libnx's implementation calls
`nwindowReleaseBuffers(fb->win)` unconditionally, which disconnects the
producer and frees every slot the BufferQueue believes it holds,
whoever registered them. Teardown called it outside the lock with no
ownership test, so destroying a superseded copy-fallback swapchain
disconnected the swapchain that had replaced it. On hardware the
console took a system fatal — `0x290 (2144-0001)` in `qlaunch` — rather
than merely losing the survivor's presents. Patch **0070** moves the
close to the two moments a swapchain stops owning the window, both
under the lock.

The zero-copy path never had it: it registers through
`nwindowConfigureBuffer` and releases through
`wsi_horizon_release_window`, which has tested ownership since 0053.
`t_vk_swapchain`'s section D runs the identical sequence on that path
and has always passed, which is exactly why this was not found earlier.

### 7.3 Two things that look wrong and are not

Recorded because the next reader will look at them too.

- **`wsi_horizon_swapchain_release_images` cancels slots with no
  ownership test.** Reachable only on a swapchain that still owns the
  window: eviction sets every slot of an evicted swapchain to `FREE`
  before releasing it, so a retired swapchain has nothing to cancel.
  Fragile rather than wrong, and left alone.
- **The `presentable` check in `queue_present` is a TOCTOU.** It is
  read under the lock, the lock is dropped, and then `bqQueueBuffer`
  runs. Only a `vkCreateSwapchainKHR` naming this swapchain as
  `oldSwapchain` can clear the flag, and that call externally
  synchronises `oldSwapchain` — so a conforming application cannot be
  presenting on it at the time. Unreachable, not absent.

### 7.4 The one genuine race left, and why it stays

`vkGetPhysicalDeviceSurfaceCapabilitiesKHR` requires no external
synchronisation and reads `NWindow::default_width` / `default_height`,
which a concurrent present writes. Two aligned 32-bit fields: the worst
outcome is a width from before a display mode change paired with a
height from after, which makes a later `vkCreateSwapchainKHR` fail with
`VK_ERROR_INITIALIZATION_FAILED`. Closing it would mean taking a lock
on a structure this backend does not own. Measured instead: run 17 made
11678 such queries from a second thread beside a present loop with no
failure.
