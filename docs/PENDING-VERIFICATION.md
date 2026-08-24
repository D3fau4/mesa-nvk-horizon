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

## 1.1 Fase D — the docked transitions of a scaled swapchain

The WSI can scale, and the half of it that this machine can reach has
been measured. `nvk_wsi.c` no longer sets
`force_swapchain_to_currentExtent`; the surface publishes
`minImageExtent` 16x16 against a `maxImageExtent` of the layer's own
size, and an application may render smaller and let the compositor scale
through the `scalingMode` every queue already carries.

RUN ON A CONSOLE 2026-08-24 — `t_nwindow` registered and presented at
1280x720, 960x540, 640x360, 320x180, 160x90, 64x36 and 16x16 on the same
window; `t_vk_swapchain` section G ran a 640x360 swapchain on a 1280x720
layer for 20 frames with **0 SUBOPTIMAL**, and `t_vk_suboptimal`,
`t_vk_wsi_mt`, `t_vk_present_draw`, `t_vk_immediate` and `t_vk_caps` are
unchanged.

**What has NOT been run is the display changing under a scaled
swapchain.** `wsi_horizon_extent_changed` now has five cases and only
the three undocked ones have executed:

| created | display | expected | run? |
|---|---|---|---|
| native | undocked | not suboptimal | yes |
| native | docked | SUBOPTIMAL | patch 0040, run 21 |
| scaled | undocked | not suboptimal | yes, section G |
| scaled | docked | SUBOPTIMAL | **no** |
| scaled | docked then undocked | not suboptimal | **no** |

The reasoning is in the function's comment: `reported` echoes this
backend's own `nwindowSetDimensions` until the consumer changes the mode
and is the display's size afterwards, so a scaled swapchain must ignore
its own echo and compare against the output it was created for. The two
unrun rows are the ones that reasoning is load-bearing for.

Nothing in this process can resize a VI layer, which is why
`t_vk_suboptimal`'s section D has always needed somebody to dock or
undock the console while it runs. This adds one more thing to do during
that same run.

**Done when**: somebody docks the console during `t_vk_suboptimal`
section D **and** during `t_vk_swapchain` section G, and the two rows
above are confirmed or corrected.

## 1.2 Fase E steps 3 and 4 — sparse residency

D12 is reopened, on a measurement rather than an absence. RUN ON A
CONSOLE 2026-08-24, `t_sparse`: a write to a never-bound sparse page is
swallowed; memory binds into the middle of a sparse reservation and the
payload arrives; and **after unbinding, a write to that address is
swallowed again and does not reach the memory that was unbound**. So
partial residency is expressible on this hardware.

Both things that blocked the work are done:

- **Step 2, the bookkeeping.** `horizon_va_set_remove_range` cuts a live
  interval, leaving whatever lies before and after it live. Host-tested
  under ASan+UBSan; `h_va_space` went from 21 checks to 55. Nothing
  calls it yet, **and it may turn out that nothing should.**
  `nvioctlNvhostAsGpu_UnmapBuffer` takes an address and no length, so a
  partial unbind has to unmap the whole mapping and map the two
  remainders back — and `horizon_gpu_vm_unmap` followed by two
  `horizon_gpu_vm_map` calls already keeps the interval set right by
  themselves. Whoever writes step 3 should decide that first: if the
  remap design is the one, this primitive is scaffolding for a building
  that was put up another way and it should be deleted, not kept
  because it is tested.
- **The channel budget.** `has_sparse = true` makes NVK ask for a bind
  context, which is a third GPFIFO channel, and patch 0022 recorded that
  as a promise about an unknown. It is no longer unknown: `t_channel`
  measured **44 channels open at once** before libnx's nv session ran
  out of transfer memory. The one-channel constraint was never the
  hardware's.

**Step 3 is written and is class X.** Patch `0055`:
`nvkmd_horizon_alloc_va` accepts `NVKMD_VA_SPARSE`, and
`nvkmd_horizon_va_unbind` finds the binding that *contains* a range
rather than the one that equals it — unmapping it and mapping the ends
that survive back, because `nvioctlNvhostAsGpu_UnmapBuffer` takes an
address and no length. Nothing reaches any of it yet.

**Step 4 is one line, and it is deliberately not taken.**
`nvkmd_info::has_sparse` gates EIGHT `VkPhysicalDeviceFeatures` at once
(`nvk_physical_device.c:395-402`) and makes NVK ask for a bind context.
Flipping it un-run would put all seventeen Vulkan tests behind a path no
console has executed.

**Done when**: `has_sparse` is flipped in `nvkmd_horizon_pdev.c` and
`t_vk_sparse` has run — and either it passes, in which case the flip
stays in the commit that quotes its log, or it does not and the flip
comes back out with the reason.

`has_transfer_queue` is false for the same one-channel reason and is now
equally unblocked, though nothing has asked for it.

### 1.2.1 What sparse costs, which arm D of `t_sparse` decides

Everything `t_sparse` measured ran at `as_big_page_size` — 128 KiB,
the address space's own binding granularity. `horizon_gpu_vm_map`
refuses to map an object in pages larger than its own alignment,
because `MapBufferEx` accepts that pairing and the GPU's writes go
nowhere. `vkAllocateMemory` takes no alignment and this backend gives an
ordinary allocation 4 KiB.

So sparse needs one of two things, and they cost very differently:

- a sparse reservation that binds in **4 KiB pages**, and device memory
  needs nothing special; or
- **every device memory object aligned to 128 KiB**, which is 32x waste
  on a small allocation.

`t_sparse` arm D asks the first directly: a small-page sparse
reservation, a 4 KiB-aligned object bound into it, and a write through
it. Class X, never run.

## 1.3 The query pool's reset and write in one submission

`nvk_CmdResetQueryPool` writes 0 to a query's availability word through
an NV9097 report semaphore and then acquires on that word from the host
engine; `nvk_CmdWriteTimestamp2` writes the report and releases 1 to it.
Three accesses to one address, from two engines, inside one submission.

`t_vk_timestamp` splits them into two submits, and says in as many words
that this is not a fix. It was done while the real cause was still
unknown, so that the timestampPeriod measurement could be made at all —
and the real cause turned out to be elsewhere entirely: stale dirty CPU
cache lines on a fresh allocation, fixed in `horizon/memory/mem.c`.

So the combined form may well work now, and nothing has tried it.

`t_vk_timestamp` now carries a probe that asks directly: the reset and
the timestamp in ONE command buffer, six times, because the failure was
intermittent at about half and one attempt could not tell it from luck.
It runs on a query slot of its own and reports rather than demands, so a
failure there costs the timestampPeriod measurement nothing. Class X,
never run.

**Done when**: that probe has run, and either it passes — in which case
`write_timestamp`'s two-submit split comes out and the probe with it —
or it does not, and the reason is recorded here instead.

## 1.4 The next console session, in order

Everything above except the docking is one sitting. Build with
`scripts/ci-build-archives.sh`, then:

1. **`t_sparse`** — arm D. Decides § 1.2.1, and everything after it in
   this list depends on which way it goes.
2. **`t_vk_timestamp`** — the combined-form probe. Closes § 1.3 either
   way.
3. Flip `has_sparse` to `true` in `nvkmd_horizon_pdev.c`, rebuild, then
   **`t_vk_sparse`** — and immediately after it `t_vulkan`,
   `t_vk_transfer` and `t_vk_swapchain`, because the flip makes NVK ask
   for a bind context at device creation and that path is what could
   break every Vulkan test at once rather than only the sparse one.
4. If any of step 3 fails, the flip comes back out and § 1.2 records
   why. That is a result, not a failure.

Then, whenever somebody is holding the console: dock or undock it during
`t_vk_suboptimal` section D **and** during `t_vk_swapchain` section G,
which closes § 1.1.

---

# 2. Things that were confirmed correct

Recorded so nobody "fixes" them. None of these needs action; they are
here because the cost of re-litigating them is higher than the cost of
the paragraph.

## Measured on hardware, 2026-08-24

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
