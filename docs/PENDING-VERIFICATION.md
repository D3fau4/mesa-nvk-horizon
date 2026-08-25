# Pending verification

**This file is a debt ledger, and it is meant to be deleted.**

Everything below is work that has *not* been run on a Nintendo Switch,
or a measurement that has been made and not yet acted on. The project's
evidence discipline has three classes and never collapses them:

| Class | Means | Does **not** prove |
|---|---|---|
| **H** — host | Built and run via `scripts/run-host-tests.sh` | Anything about the Switch |
| **X** — cross | Cross-compiled for aarch64 Horizon; a `.nro` exists | That it runs, or is correct |
| **HW** — hardware | Ran on a real console, with the log | Only what the log actually shows |

## How this file dies

Each section below has a **Done when** line. When every section's
condition is met — the run happened, the log says what it had to say,
and whatever the measurement decided has been acted on — **delete this
file**. Do not leave a hollowed-out version behind saying "all clear":
the commit messages are the permanent record, and an empty ledger is
just another stale reference.

If a measurement comes back and closes a question the *other* way — a
capability that turns out not to exist — that is a result, not a
failure. Record it in the commit that acts on it, then strike the
section.

## Running any of this

```sh
scripts/build-switch.sh -j4                 # the .nro files, no Mesa
scripts/ci-build-archives.sh                # everything, Mesa included
# copy build/*.nro (or build/meson/*.nro) to sdmc:/switch/horizon_gpu_tests/
```

Each test prints `RESULT: PASS (n/n)` on screen and writes
`sdmc:/horizon_gpu_tests/<name>.log`. **A result without the
`horizon-build-id` line in its log cannot be attributed to a build** and
does not count. `tools/logcat/` reads a log back over nxlink; see
`tests/README.md`.

---

# 1. Open work

## 1.1 A dock cannot make a swapchain suboptimal here

This is closed as a measurement, not as a feature. The answer is that
the transition patch 0040 was written for cannot be delivered on this
platform, and the three runs below are why.

`t_dock`, docking and undocking three times with a buffer queued every
50 ms so every source was being refreshed:

- `appletGetOperationMode()` moves, and `AppletHookType_OnOperationMode`
  fires. **A dock does reach this process.**
- `nwindowGetDimensions()` and `NWindow::default_*` — what
  `wsi_horizon_get_extent` reads — never move. 1280x720 throughout. The
  compositor scales this layer to the television rather than resizing
  it, so the BufferQueue has nothing to report.
- `appletGetDefaultDisplayResolution()` moves to 1920x1080 **and back
  to 1280x720 while still docked**, about 350 ms after the mode and
  2.8 s before it fell again. It is a transient around the transition,
  not a state.

`t_vk_swapchain` section G then ran with the console docked for the
whole 90-second window — `appletGetOperationMode() went 1 -> 1` — and
the surface reported **1280x720 from beginning to end**, sampled inside
the loop rather than once at the end.

So no source available to this process reports a stable docked output
size. A swapchain cannot be told the output changed, because as far as
everything it can read is concerned, it did not.

**What was tried and reverted.** Patch `0057` made
`wsi_horizon_get_extent` read `appletGetDefaultDisplayResolution()`, and
`wsi_horizon_extent_changed` compare that against the size at creation.
It passed every undocked regression — `t_vk_suboptimal` 273/273 twice,
`t_vk_wsi_mt` 71/71, `t_vk_present_draw` 183/183, `t_vk_immediate`
442/442, `t_vk_swapchain` 144/144 — and it is still wrong, because the
value it reads is a 2.8-second excursion. It would make
`VkSurfaceCapabilitiesKHR::currentExtent` flicker to 1920x1080 around
every dock, so an application querying inside that window would build a
1080p swapchain for a layer that is 720p and stays 720p. A stable wrong
answer beats an unstable one; `default_*` at least matches the layer.

**This explains a line patch 0040 has carried since August**:
"VK_SUBOPTIMAL_KHR HAS NEVER BEEN RETURNED ON HARDWARE." It was written
as coverage nobody had managed to get. It is not coverage. There is
nothing to cover.

**If it is ever wanted**, what it needs is a source that is stable while
docked — `viGetDisplayResolution` on a display handle this process does
not have, or an applet path nobody here has found. Not a longer wait
with a hand on the console: three runs have now had that.

---

# 2. Things that were confirmed correct

Recorded so nobody "fixes" them. None of these needs action; they are
here because the cost of re-litigating them is higher than the cost of
the paragraph.

## Measured on hardware, 2026-08-24

### Docked is a different machine, and two tests had to learn it

The suite had only ever run handheld. Running it in the dock found two
differences that are the compositor's and the clocks', not this
driver's:

- **The compositor attaches no release fence when docked.** Handheld,
  87 of 90 dequeues carried one, the first on syncpoint 103; docked, 0
  of 90 — while still presenting all 90 frames at the refresh, so the
  buffers were being released without a fence attached. A producer must
  cope with that, because the queue is allowed not to give one.
  `t_nwindow` reports the count and no longer fails on it.
- **Docking raises the clocks enough to stop the buffer-count
  experiment running.** The bursty-load check compares two buffers
  against three and wants two to be 10% slower. Handheld the two-buffer
  mean ran about 25 ms against a 16.7 ms refresh; docked, both counts
  sat on the refresh — 16944 us against 16851, with 45 of 89 intervals
  overrunning either way. Nothing regressed: the producer stopped being
  the bottleneck. `t_nwindow` and `t_vk_swapchain` now make that
  comparison only when two buffers actually fall behind the display.

- **Sparse residency works, through the whole stack.** `t_sparse` asked
  the address space (an unbound page swallows a write; unbinding a bound
  block puts that state back) and `t_vk_sparse` asked the same three
  questions through `vkQueueBindSparse` — PASS 74/74, block size
  0x20000, 0 of 32768 words wrong on the readback. `has_sparse` is true
  (patches `0055`, `0056`) and all seventeen Vulkan tests pass with the
  bind context it makes NVK ask for. Two of the eight features it gates
  have been exercised: `sparseBinding` and `sparseResidencyBuffer`.
- **Sparse exists only in big pages.** A reservation with
  `NvAllocSpaceFlags_Sparse` and a 0x1000 page size is refused —
  `0x0000275c`, `LibnxNvidiaError_IoctlFailed` — where the same call at
  `as_big_page_size` is accepted. `NVKMD_VA_SPARSE` forces the big-page
  half regardless of the PTE kind because of it.
- **A query pool's reset and timestamp belong in one submission.** Six
  of six after the `mem.c` cache fix; the two-submit split that was
  working around it is gone.
- **A dock reaches the process but not the layer.** See § 1.1 — the
  operation mode and its applet hook both move,
  `appletGetDefaultDisplayResolution()` follows to 1920x1080, and
  `NWindow::default_*` never moves at all.

- **`gpu_va_bit_count` is 40**, and `t_init` now fails device creation if
  a chip reports anything else, because a truncated GPU address is a
  valid address the GPU will write to.
- **`NVHOST_IOCTL_CTRL_SYNCPT_INCR` does not exist here.** `nv=0x275c` —
  `Module_LibnxNvidia`, `LibnxNvidiaError_IoctlFailed`. There is no
  CPU-side syncpoint increment on this platform, so a dedicated
  syncpoint would not help either: what is missing is the increment, not
  the ownership of the counter. `nvk_horizon_sync_signal` keeps the
  behaviour it has, and Fase C steps 3-4 are closed.
- **32 concurrent `nvFenceWait` calls all arm**, and 32 of them cost one
  wait's wall time (301 ms against 300 ms), so the platform does not
  serialise them. No ceiling was found below `MAX_THREADS`.
- **`nvFenceWait` reports a timeout as `0x00000d5c`** —
  `Module_LibnxNvidia`, `LibnxNvidiaError_Timeout` — and not as
  `KERNELRESULT(TimedOut)`. `horizon/sync/nv_wait.h` is the one place
  that decides what the Result means.
- **A threshold past the syncpoint's maximum answers success in zero
  milliseconds.** nvhost calls such a threshold expired rather than
  block on an increment nothing will make. Neither wait loop treats a
  chunk that did not block as its pulse any more.
- **The GPU timer is 1/614.4 MHz = 1.627604 ns per tick**, measured to
  4-10 ppm across three windows and three runs, and 614.4 MHz is 32x the
  19.2 MHz reference `armGetSystemTick` counts. The clock
  `vkCmdWriteTimestamp` records is the same domain. `timestampPeriod`
  was `1.0f` and is now this.
- **The BufferQueue keeps 0 buffers for its consumer**, before and after
  registration, so patch 0052's `minUndequeued + 1` is clamped up to
  `WSI_HORIZON_MIN_IMAGES` and the number it publishes is unchanged.
- **44 GPFIFO channels can be open at once**, the refusal being libnx's
  nv session transfer memory rather than the kernel.
- **`t_fault`: the console survives an MMU fault; the process does not.**
  Notifier type 31, channel marked lost, teardown clean, and the GPU
  still works in-process afterwards — but the atexit marker is never
  written, so the process is killed during exit, after the log is
  closed. Recovering needs one press of A on the error dialog and a
  relaunch, not a power cycle.

## Measured earlier, and still true

- `NvMultiFence` holds at most **4** fences.
- A GOB is **64 bytes × 8 rows = 512 bytes**; a block is
  `512 << block_height_log2` bytes and covers `8 << block_height_log2`
  rows.
- `HORIZON_GPU_PTE_KIND_GENERIC_16BX2 = 0xfe` is the kind the display
  block reads.
- `VK_FORMAT_R8G8B8A8_UNORM → PIXEL_FORMAT_RGBA_8888 →
  NvColorFormat_A8B8G8R8`, byte order inverted and correct.
- `HORIZON_GPU_SMALL_PAGE_SIZE = 0x1000`.
- **The PTE kind belongs on the GPU mapping, not on the allocation.**
  `mem.c` creates every `NvMap` with `NvKind_Pitch` and `vm.c` passes
  the real kind to `MapBufferEx`. This is not obvious and it is right.
- **A big-page mapping needs big-page-aligned backing.** `MapBufferEx`
  accepts a 4 KiB-aligned object with `page=0x20000`, returns the
  requested VA, and the GPU's writes to it go nowhere — no fault, no
  error, no data. `horizon_gpu_vm_map` refuses that pairing now.
- **A fresh allocation's cache lines must be flushed before the GPU
  writes it.** `aligned_alloc` plus the zero-fill leaves every line
  dirty, and `dc civac` cleans before invalidating — so an invalidate
  meant to see the GPU's write instead destroys it. Harmless for every
  object the CPU writes first, fatal for the first one it does not.
- Querying `big_page_size` instead of hardcoding it (`device.c:138`).
