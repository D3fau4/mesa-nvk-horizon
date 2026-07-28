# Synchronisation model

The single largest structural defect of the reference ports is that submission is
synchronous: the CPU waits for the GPU after every submit, and Vulkan wait-semaphores are
resolved by blocking the CPU before submission
(`winsys/drm_shim.c:862-868` and `:975-985` in the reference — see
`docs/reference-analysis.md` § Synchronous operations). This document defines the model
that replaces it.

---

## 1. The hardware primitive: syncpoints

Tegra's host1x provides **syncpoints**: monotonically increasing 32-bit counters. A GPU
channel is allocated one syncpoint at creation (`nvGpuChannelGetSyncpointId`).

A **fence** is a value type:

```c
typedef struct {
    uint32_t syncpoint_id;   /* which counter                                  */
    uint32_t threshold;      /* the value the counter must reach               */
} horizon_gpu_fence;
```

A fence is *signalled* when `counter(syncpoint_id) >= threshold`, in the modular sense.

### 1.1 Monotonicity and wraparound

The counter is 32-bit and **wraps**. The reference performs a plain `cur >= fence.value`
comparison (`drm_shim.c:997`) and handles wraparound nowhere. This project must:

- Never compare fence thresholds with `<` / `>=` directly.
- Use a wrap-safe predicate:
  ```c
  /* True when `cur` has reached or passed `threshold`, tolerating 32-bit wrap.
   * Valid while the in-flight window stays below 2^31 increments. */
  static inline bool horizon_syncpt_reached(uint32_t cur, uint32_t threshold)
  {
      return (int32_t)(cur - threshold) >= 0;
  }
  ```
- Maintain, per channel, a **64-bit shadow** of the syncpoint value that the CPU increments
  by the same amount it requests from the GPU. Vulkan timeline semaphores need 64-bit
  monotonic values; the shadow provides them, and only the low 32 bits are ever compared
  against hardware.
- Assert in debug builds that the number of increments in flight never approaches 2^31.

### 1.2 Increment discipline

Two things must agree, or the channel silently stalls forever:

1. `nvGpuChannelIncrFence` tells libnx that the channel's expected value grows by one.
2. An actual **syncpoint-increment command** must be present in the submitted stream, or
   the GPU never bumps the counter.

The reference learned this the hard way and documents it at `drm_shim.c:941-953`. Our
submit layer therefore treats "request an increment" and "emit the increment command" as
one indivisible operation in `horizon/submit/`, never as two callable steps.

The increment command also carries the **GPU L2 flush** bit, so completion implies
visibility. See `docs/memory-model.md` § Coherency.

---

## 2. Asynchronous submission

```
horizon_gpu_submit(chan, entries, n, &out_fence)
    ├── append GPFIFO entries
    ├── request one syncpoint increment
    ├── append the increment+flush command list
    ├── kickoff
    └── return the resulting fence  ── DOES NOT WAIT
```

Rules:

- `horizon_gpu_submit` **never** calls `nvFenceWait`. It returns as soon as the kickoff is
  accepted.
- Multiple submits may be in flight on one channel. Their fences are ordered because a
  single channel's syncpoint increments in submission order.
- Submits on *different* channels are unordered; a cross-channel dependency must be
  expressed explicitly (see § 4).
- The channel error notifier is **not** polled after every submit (the reference does,
  adding round trips — `drm_shim.c:992-1020`). It is polled when a wait times out, when a
  submit is rejected, and optionally every N submits in debug mode.

### 2.1 Kickoff back-pressure

The reference retries a failed kickoff up to 400 times with `svcSleepThread(250 µs)`, while
holding its global device lock (`drm_shim.c:666-676`). That is a sleep hiding a full ring.

Our submit layer instead:

- Tracks how many GPFIFO entries are outstanding and how many have retired (via the
  syncpoint shadow), so ring fullness is a *known* quantity rather than an error code.
- When the ring is genuinely full, waits on the fence of the oldest outstanding submit —
  a real dependency wait, with a caller-supplied timeout, and **not** holding a lock that
  blocks unrelated operations.
- Surfaces the distinction to the caller: "ring full, waited" is logged; "kickoff rejected
  for another reason" is an error, never retried blindly.

---

## 3. Resource lifetime and recycling

A submit references memory. That memory must not be freed, unmapped or rewritten until the
submit has completed.

Every `horizon_gpu_channel` keeps a **retirement list**: `(fence, callback, context)`
entries appended at submit time. `horizon_gpu_channel_reap(chan)` reads the current
syncpoint value once and retires everything whose threshold has been reached, without
blocking.

`reap` is called:
- at the start of every submit (cheap: one syncpoint read),
- from `horizon_gpu_channel_wait_idle`,
- explicitly by the caller when it wants to reclaim.

This is what makes the drain-after-submit unnecessary. Command buffers, staging buffers and
descriptor pools are recycled on fence retirement, not on a CPU stall.

`nouveau_gem_cpu_prep` in the reference is a documented no-op precisely because it has no
per-BO fence tracking (`drm_shim.c:561-567`). We track at submit granularity, and a memory
object records the highest fence that referenced it.

---

## 4. Dependencies between submits

The reference resolves Vulkan wait-semaphores by blocking the CPU before submitting
(`drm_shim.c:862-868`). This is the reason its own smoke tests must call
`vkQueueWaitIdle` before every present — the WSI's internal present submit would otherwise
deadlock against a CPU-side semaphore wait (`winsys/smoke/nvk_swapchain.c:395-401`).

Our model distinguishes three cases:

| Dependency | Resolution |
|---|---|
| Same channel, earlier submit | Implicit. The channel's syncpoint is ordered; nothing to emit. |
| Different channel, GPU-side | Emit a **syncpoint wait command** in the dependent channel's stream. The CPU does not block. |
| Host signal (`vkSignalSemaphore`, host events) | The CPU-side wait is legitimate here; it is what the application asked for. |

Cross-channel GPU waits are the one facility the reference never implemented. Until
`horizon/sync/` supports them, `nvkmd_horizon` must expose **one channel per queue family
in use** and route cross-queue dependencies through an explicit, documented CPU wait that
is *reported*, not hidden. This limitation is recorded in `docs/known-risks.md`.

---

## 5. Vulkan-level mapping

### Binary semaphores

Backed by a fence slot: `{ has_fence, horizon_gpu_fence }`. Signalling a binary semaphore
records the submit's completion fence. Waiting on it emits a GPU-side syncpoint wait on the
consuming channel (§ 4), or is a no-op when both submits are on the same channel and the
signal precedes the wait.

Reset semantics: a binary semaphore is unsignalled after its wait is consumed. This is
bookkeeping in `nvkmd_horizon`, not in `horizon/`.

### Timeline semaphores

Backed by the per-channel 64-bit syncpoint shadow plus a mapping table from timeline point
to fence threshold. `vkGetSemaphoreCounterValue` reads the hardware counter, extends it
against the shadow, and returns the 64-bit value.

The reference advertises `DRM_CAP_SYNCOBJ_TIMELINE` but its `drmSyncobjTimelineWait`
**discards the requested points** and degrades to a binary wait (`drm_shim.c:1428-1435`),
and the non-empty EXEC path never updates the stored timeline value (`drm_shim.c:970-973`).
We must not repeat either: timeline points are tracked or the feature is not advertised.

### Fences (`VkFence`)

A `VkFence` is a `horizon_gpu_fence` plus a signalled flag. `vkWaitForFences` maps to
`horizon_gpu_fence_wait` with the caller's timeout. `vkGetFenceStatus` maps to
`horizon_gpu_fence_poll`, which reads the syncpoint and never blocks.

### `vkQueueWaitIdle`

Waits on the fence of the most recent submit on that queue's channel, then reaps. It does
**not** iterate or sleep.

### `vkDeviceWaitIdle`

`vkQueueWaitIdle` on every queue, in creation order, then a final reap pass over all
channels. It must not be used internally by the driver to make anything else work.

---

## 6. Timeouts

- Every wait takes an explicit timeout. There is **no** internal infinite wait.
- The libnx `nvFenceWait` timeout is in **microseconds**, not nanoseconds. The reference
  documents having been bitten by this (`drm_shim.c:980-982`: a value meant as 2 s was
  2000 s). `horizon/sync/` takes nanoseconds at its public boundary, matching Vulkan, and
  converts once, with a saturating clamp and an explicit test.
- `UINT64_MAX` from Vulkan is honoured as "no deadline", implemented as a bounded wait in a
  loop that re-checks the channel error notifier each iteration — so a faulted channel
  produces `VK_ERROR_DEVICE_LOST` instead of an eternal hang.
- A timeout is returned to the caller. It is never silently downgraded to success. The
  reference caps every syncobj wait at 3 s and returns as if it had waited properly
  (`drm_shim.c:1389-1393`) — that is a correctness bug, not a debug aid.

---

## 7. Acquire and present

Detailed in `docs/wsi.md`. The synchronisation contract:

- **Acquire** returns a slot plus the compositor's producer fence. That fence becomes a
  wait dependency of the first submit that writes the image — it is *not* waited on by the
  CPU. The reference CPU-waits it (`wsi_common_switch.c:328`) and ignores the result.
- **Present** must hand the render-completion fence to `nwindowQueueBuffer` so the display
  block waits GPU-side. The reference passes `NULL` and CPU-waits instead
  (`wsi_common_switch.c:370-374`). Passing a real fence is a Phase 6 requirement, not an
  optimisation.

---

## 8. Debug-synchronous mode

Enabled by `HORIZON_GPU_SYNC=1` (env) or a device-creation flag. It is a **diagnostic**,
never a fix:

- After every submit, wait on the resulting fence with a bounded timeout.
- Read the syncpoint value and the channel error notifier; decode and log the reason
  (31 = MMU fault, 25 = illegal method, 32 = PBDMA error, 8 = idle timeout).
- On an MMU fault, log the faulting GPU VA and resolve it against the VA allocator to name
  the offending allocation.
- Log every GPFIFO entry submitted (VA, dword count, flags).

Rules:
- The mode is **off** by default and the code paths it enables are compiled in but never
  taken otherwise.
- No test may pass only with the mode enabled. If it does, that is the bug.
- `STATUS.md` records whether a result was obtained with the mode on or off.

## 9. Untrusted syncpoint baselines

Enabled by `HORIZON_GPU_UNTRUSTED_SYNCPT_BASELINE=1` (env) or a device-creation flag.
Like § 8 it is a diagnostic, but a narrower and more dangerous one, so it is described
separately.

Channel creation reads the hardware syncpoint once and initialises the 64-bit shadow
(§ 1.1) from it. That read failing is normally fatal: without a baseline, every threshold
this channel computes is offset by an unknown amount, and a wait can return "reached"
before the GPU has done anything. Some environments do not implement
`NVHOST_IOCTL_CTRL_SYNCPT_READ` at all — the 2026-07-27 comparison in `STATUS.md` has one
that answers `0x55c` (`LibnxNvidiaError_NotImplemented`) where a real console answers with
a value. On those, nothing above channel creation can be executed at all.

With the opt-in, such a channel comes up with a baseline of zero, marked untrusted:

- `horizon_gpu_channel_syncpt_baseline_trusted()` is false for that channel.
- `horizon_gpu_device_untrusted_syncpt_seen()` is true for its device, and stays true.
- Both the opt-in and each degraded channel are logged at `ERROR` level, so no log from
  such a run can be mistaken for a clean one.

Rules:
- Off by default. Nothing degrades unless the application asks for it by name.
- A degraded channel's fences are arithmetic, not observation. **No result obtained on
  one may be reported as hardware behaviour**, including a readback that matches: the
  wait it passed through proves nothing. `t_vulkan` enforces this by failing any run in
  which it enabled the mode, whatever the readback said.
- It is not a fallback for a real syncpoint failure on hardware that has syncpoints.
  There, a failed read is a defect and stays fatal.
