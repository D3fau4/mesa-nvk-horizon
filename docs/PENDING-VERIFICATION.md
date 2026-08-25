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

## 1.1 The WSI reads a source that cannot see a dock

Fase D landed — `nvk_wsi.c` no longer forces the swapchain to
`currentExtent`, the surface publishes 16x16 to the layer's own size,
and a 640x360 swapchain on a 1280x720 layer presents 20 of 20 frames
with 0 SUBOPTIMAL (patch `0054`, `t_vk_swapchain` section G). What was
left was the two rows of `wsi_horizon_extent_changed`'s table that need
the display to change under a scaled swapchain.

**They cannot happen, and `t_dock` says why.** RUN ON A CONSOLE
2026-08-24, docking and undocking three times, with a buffer queued
every 50 ms so the consumer's answer was being refreshed throughout:

```
CHANGE 1 at 7201 ms: mode 0 -> 1, dimensions 1280x720 -> 1280x720,
  default_ 1280x720 -> 1280x720, DisplayResolution 1280x720 -> 1280x720
CHANGE 2 at 7554 ms: mode 1 -> 1, dimensions 1280x720 -> 1280x720,
  default_ 1280x720 -> 1280x720, DisplayResolution 1280x720 -> 1920x1080
```

Three things, and they disagree:

- `appletGetOperationMode()` moves, and `AppletHookType_OnOperationMode`
  fires. **A dock does reach this process.**
- `nwindowGetDimensions()` and `NWindow::default_*` never move —
  1280x720 from beginning to end. The compositor scales this layer to
  the television rather than resizing it, so the BufferQueue never hears
  about the mode change.
- `appletGetDefaultDisplayResolution()` **does** follow it, to 1920x1080
  and back, about 350 ms after the mode.

`wsi_horizon_get_extent` reads `NWindow::default_*`. That is the second
one — the one that cannot move. So `VkSurfaceCapabilitiesKHR::
currentExtent` never changes, `wsi_horizon_extent_changed` never returns
true, and no swapchain can be suboptimal because of a dock.

**This explains a line patch 0040 has carried since August**:
"VK_SUBOPTIMAL_KHR HAS NEVER BEEN RETURNED ON HARDWARE." That was
written as coverage nobody had managed to get. It was not coverage. It
was a source that cannot move.

**What to do about it**, and it is bounded: `wsi_horizon_get_extent`
reads `appletGetDefaultDisplayResolution()`, falling back to
`NWindow::default_*` when it fails. Undocked the two agree — both
1280x720 — so the undocked behaviour of every existing test is
unchanged by construction, which is why this is a small change and not
a redesign.

It was written and then **reverted unbuilt-upon**, because the
regression run it needs was interrupted. A change on the live path of
every `get_extent` call is not something to land on reasoning.

**Done when**: `wsi_horizon_get_extent` reads the display resolution,
`t_vk_swapchain`, `t_vk_suboptimal`, `t_vk_wsi_mt`, `t_vk_present_draw`
and `t_vk_immediate` pass undocked, and one run has somebody dock the
console during `t_vk_swapchain` section G or `t_vk_suboptimal`
section D — which can now actually fire.

**Also worth deciding then**: with the surface reporting 1920x1080 while
the layer stays 1280x720, `maxImageExtent` allows a 1080p swapchain on a
720p layer. The compositor would scale it down. Nothing has measured
that and section G should, in the same run.

---

# 2. Things that were confirmed correct

Recorded so nobody "fixes" them. None of these needs action; they are
here because the cost of re-litigating them is higher than the cost of
the paragraph.

## Measured on hardware, 2026-08-24

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
