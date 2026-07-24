# WSI — presenting through `nwindow`

Target: a `VK_KHR_swapchain` implementation over Horizon's VI compositor via libnx
`nwindow`, with no global state, real double/triple buffering, and a genuine zero-copy
path whose fallback is explicit and logged.

This is **Phase 6**. Nothing here is implemented yet. The design below is derived from the
audit in `docs/reference-analysis.md`.

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
enum horizon_wsi_slot_state {
   HORIZON_WSI_SLOT_FREE,      /* owned by the compositor, not dequeued       */
   HORIZON_WSI_SLOT_ACQUIRED,  /* dequeued, handed to the app                 */
   HORIZON_WSI_SLOT_RENDERING, /* app submitted work writing it               */
   HORIZON_WSI_SLOT_QUEUED,    /* handed back to the compositor               */
};
```

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

This is the same algorithm as the reference's `g_zc_owner`, with the state on the object
that actually owns it. It is also what makes "two independent swapchains" a meaningful
Phase 6 test rather than a formality.

### 2.3 Buffer count

- `minImageCount = 2`, `maxImageCount = 4`.
- The requested count is **clamped into `[minImageCount, maxImageCount]`**, not silently
  truncated downward only. A request of 1 must not produce a one-image swapchain.
- `nwindowSetBufferCount` is called to match. The reference never calls it.
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
| The image's memory is a single `NvMap` whose id we can obtain | `horizon/surface/` |
| `size_B` and `offset_B` fit in the graphic buffer's 32-bit fields | validated |
| The display kind is compatible with the image's PTE kind | validated, not forced |

If any fails, zero-copy is **declined** with a logged reason and the copy fallback is used.
"Declined" is a normal outcome, not an error.

### 3.1 Eliminating the residual blit

Zero-copy is only real once the swapchain uses `WSI_SWAPCHAIN_NO_BLIT`. That requires the
Horizon WSI backend to present renderable images directly rather than going through
`wsi_cpu_image_params`. This is tracked as a Phase 6 work item, not an optimisation:
without it, "zero-copy" still costs a full-resolution GPU copy per frame.

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

The producer fence becomes a **wait dependency of the application's first submit** writing
that image. It is not waited on by the CPU. The reference CPU-waits it with a 1 s timeout
and discards the result (`wsi_common_switch.c:328`), so a timeout is treated as "the
compositor is finished" and the app may render over a buffer still being scanned out.

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

### The silent-no-op hazard

In the reference, if `zero_copy` is set but a slot was never configured, and no fallback
framebuffer exists either, `queue_present` falls through both branches and returns
`VK_SUCCESS` having presented nothing (`wsi_common_switch.c:437-438`). Our present path has
no fall-through: every path either queues, copies, or returns an error.

---

## 5. Resize and recreation

- `nwindowSetDimensions` on creation.
- A dimension change detected at acquire returns `VK_ERROR_OUT_OF_DATE_KHR`.
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
