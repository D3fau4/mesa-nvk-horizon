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

### 9.1 What a degraded channel does with a failed read

The opt-in is not enough on its own: `horizon_gpu_submit` reaps before it
queues (§ 3), and reaping reads the syncpoint, so on a platform without
the read *every submit* fails — including the engine bind, which happens
at channel creation. Measured on the emulator, 2026-07-28:
`horizon_gpu_channel_bind_engines failed: ... nv 0x0000055c`.

So on an untrusted-baseline channel, and only there, a failed read is
answered rather than propagated:

- `horizon_gpu_channel_reap` reports **nothing retired** and succeeds.
- `horizon_gpu_channel_destroy` skips the in-flight check and says so,
  because refusing would strand the channel and the device with it.
- Waits are **not** degraded. `horizon_gpu_fence_wait` and
  `horizon_gpu_channel_wait_fence` still fail, which is the point: a
  wait that cannot be ordered must not return success.

Of the two possible answers to a failed read, "nothing retired" is the
safe one. The other — treating everything as complete — would let a wait
return without the GPU having run, and that is the one answer this
project must never give. The cost is that nothing is ever recycled on
such a channel, which a bring-up run can afford and a real one would
not.

### 9.2 Asking the fence's question a different way

The counter and the fence are two different questions, and a platform can
answer one and not the other:

- `NVHOST_IOCTL_CTRL_SYNCPT_READ` — "what is the counter now?"
- `NVHOST_IOCTL_CTRL_SYNCPT_WAIT`, under `nvFenceWait` — "has (id,
  threshold) been reached?"

Everything in this layer was built on the first, and the emulator
measured on 2026-07-28 does not implement it — while games on that same
emulator wait on fences constantly, which is the second. The wait was
never even reached there: `horizon_gpu_fence_wait` read the counter at
the top of its loop and returned that error before calling `nvFenceWait`
at the bottom.

So on a channel whose baseline could not be read, and only there, the
read's failure falls through to the wait: `nvFenceWait(fence, 0)` for a
poll, and for the remaining deadline in a wait. The channel notifier is
still re-checked between chunks, so a faulted channel cannot hang.

**What this does and does not buy.** It is not a weaker answer — it is
the kernel answering the exact question a fence asks, with no counter and
no shadow involved. What it cannot rescue is the *threshold*: that was
computed from a baseline nobody read, so "reached" is only as sound as
that assumption. Where the real counter starts at zero for a fresh
channel the assumption holds and the answer is right; where it does not,
the wait returns early and the data the fence was ordering is not there
yet — which a readback notices. The channel stays untrusted either way,
and `t_vulkan` still refuses to call such a run a pass.

Where the read works it stays the primary path. It is one ioctl for any
number of fences on the same syncpoint, and it feeds the 64-bit shadow,
which this cannot.

### 9.3 Recovering the value from the predicate

§ 9.2 stops one step short: the wait answers the fence's question, but the
*threshold* it is asked about was still computed from a baseline nobody
measured. That baseline is the last thing keeping a degraded channel
untrustworthy, and it does not have to stay unmeasured — the predicate is
enough to recover the counter outright.

The thresholds a counter `C` has reached are exactly the modular
half-space

```
{ T : (int32_t)(C - T) >= 0 }  =  { C - 2^31 + 1, ..., C }
```

whose upper end *is* `C`. So `C` is recoverable by finding the largest
reached threshold. Two facts make that a binary search rather than a scan:

1. For any `v`, exactly one of `v` and `v + 2^31` is in the half-space —
   the two conditions partition the 2³² possible values of `C - v`. One
   probe therefore always lands an **anchor** inside it.
2. Measured as an offset `d` from an anchor, `reached(anchor + d)` is
   monotone over `d ∈ [0, 2^31 - 1]`: true up to `d = C - anchor`, false
   after. The circular order that makes a plain `>=` wrong is gone.

31 halvings then pin `d`, for **33 probes total** — one anchor, 31
halvings, one verification.

**The result is never an overestimate.** For a monotonically increasing
counter a "reached" answer never becomes false, so every `d` the search
accepted is still reached at the end: the value returned is one the
counter genuinely had. A counter that moves underneath the search can
therefore only make the result *stale* — the direction that would make a
later fence look signalled early, which is the one worth catching.

The last probe catches it exactly, not heuristically. It asks whether
`value + 1` has been reached, which is true precisely when the counter has
moved past the result. So `OK` does not mean "probably right": it means
the value returned is the counter's value at that instant.

`horizon_syncpt_search_value` (`horizon/sync/syncpt_math.h`) is the
arithmetic, taking the predicate as a callback so the file never learns
what an ioctl is. It is unit-tested on the host against an oracle that
answers only yes/no — every wrap corner, a 4096-value sweep, a counter
advancing mid-search, and the predicate being unavailable at the first
probe and mid-search (`tests/host/h_syncpt_math.c`).

**Nothing calls it yet, deliberately.** The arithmetic is validated; the
premise is not. Whether `nvFenceWait` answers where `SyncptRead` does not
is what the pending emulator run measures, and wiring a search onto an
unmeasured predicate would be the mistake the shader-window block-off
already made once — a change that looked obviously right and broke
`vkCreateDevice` outright. The half that could be proven today is proven;
the half that depends on a measurement waits for it.

When it is wired, the payoff is that "untrusted baseline" stops being a
category: the value would be *measured*, by a different ioctl than the one
that failed, and the thresholds, the 64-bit shadow and the fences derived
from it become as sound as on a platform that can read the counter. The
cost is 33 ioctls once per channel, and only where the read is missing.

---

## 10. The submit and fence-wait meter

Enabled by `MESA_VK_NVKMD_HORIZON_SUBMIT_STATS=1`. Like § 8 and § 9 it is a diagnostic,
but unlike them it changes nothing: it reads two clocks around waits that were going to
happen anyway and prints what it found. No path behaves differently with it on.

It exists because **"`vkWaitForFences` took N milliseconds" is one number and at least
four different things produce it**, and an application cannot tell them apart from
outside the driver:

| what the meter reports | what it means when it is the large one |
|---|---|
| time on the syncpoint | the GPU genuinely took that long |
| time **before a fence existed** | the caller waited on a `VkFence` nothing had submitted with yet — the D16 condition-variable wait in `nvkmd_horizon_sync.c` |
| CPU waits for another channel | a submit was blocked in `nvkmd_horizon_ctx_wait`, waiting on the CPU for a fence belonging to a different channel (§ 4), with the queue held behind it |
| fence age when the wait returned | how old the fence actually was. An application that believes it is waiting for work from three frames ago, against a mean age of a millisecond, is not pipelining at all |

Two lines a second, per channel:

```
nvkmd_horizon: channel 0x…: over 1004 ms: 183 submit(s); 0 CPU wait(s) for another
channel totalling 0 us, 171 wait(s) skipped as already ordered by this channel
nvkmd_horizon: channel 0x…: over 1004 ms: 61 fence wait(s) blocked, mean 812 us
(max 3140), of which 0 us before a fence existed and 812 us on the syncpoint;
mean fence age when the wait returned 1204 us
```

Reading it:

- **Per channel, not per device.** The exec channel and the upload channel report
  separately rather than averaging into each other. A cross-channel wait therefore appears
  twice — once as a CPU wait on the channel that performed it, once as a fence wait on the
  channel that owned the fence. Those are two true statements about one event, not double
  counting inside either line.
- **Only waits that blocked are counted.** A wait that found the sync already signalled
  cost nothing and would dilute the mean; one that timed out measures the caller's
  patience rather than the fence. Same rule as the WSI acquire meter, for the same reason.
- **`submit(s)` is the denominator for everything else**, and answers "is this latency per
  submission or per frame?" without the application having to guess: divide by the frames
  in the same second.
- The skipped-wait count is § 4's same-channel optimisation working. A submit that waits
  on a semaphore its own channel already ordered needs no wait at all, and a large skipped
  count next to a zero CPU-wait total is that being confirmed rather than assumed.
