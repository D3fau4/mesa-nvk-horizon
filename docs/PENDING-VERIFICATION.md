# Pending verification

**This file is a debt ledger, and it is meant to be deleted.**

Everything below was written between commits `97f5fc5` and `51536da` and
has *not* been run on a Nintendo Switch. The project's evidence
discipline has three classes and never collapses them:

| Class | Means | Does **not** prove |
|---|---|---|
| **H** — host | Built and run via `scripts/run-host-tests.sh` | Anything about the Switch |
| **X** — cross | Cross-compiled for aarch64 Horizon; a `.nro` exists | That it runs, or is correct |
| **HW** — hardware | Ran on a real console, with the log | Only what the log actually shows |

Almost everything here is **X**. A successful compile is never described
as working.

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
scripts/build-switch.sh -j4                 # produces the .nro files
# copy build/*.nro to sdmc:/switch/horizon_gpu_tests/
```

Each test prints `RESULT: PASS (n/n)` on screen and writes
`sdmc:/horizon_gpu_tests/<name>.log`. **A result without the
`horizon-build-id` line in its log cannot be attributed to a build** and
does not count.

---

# 1. Measurements that decide something

These are not regression tests. Each one answers a question the code
currently cannot answer, and what it answers changes what gets built
next.

## 1.1 `t_sparse` — decision D12

Sparse residency is off (`nvkmd_info::has_sparse = false`, and seven
`VkPhysicalDeviceFeatures` follow it) because the layer had no way to
unbind part of a reservation and leave the rest addressable. That was
recorded as a decision, never as a measurement.

`horizon_gpu_vm_reserve_sparse()` now exists (`horizon/vm/vm.c`) and
**nothing calls it**. `has_sparse` is still `false`. That is deliberate.

Three arms, each on its own channel because a fault loses one:

| Arm | What it writes to | Reading |
|---|---|---|
| A | a never-bound sparse page | survives ⇒ the reservation resolves to nothing; faults ⇒ `NvAllocSpaceFlags_Sparse` does not do that here |
| B | the middle block, bound | must survive, and the payload must arrive |
| C | the same address, after unbinding | **the crux** — survives ⇒ unbinding restores the sparse state; faults ⇒ it punches a hole |

**If C survives**: D12 can be reopened. The remaining work is partial
bind/unbind inside one mapping (`horizon_va_set` needs sub-interval
removal, host-testable first), then `NVKMD_VA_SPARSE` in
`nvkmd_horizon_va.c`, then a **new** patch flipping `has_sparse` — not
an edit to `0022`, which is what set it false.

**If C faults**: D12 closes properly, with a measurement behind it
instead of an absence. Record the notifier type from the log.

**Also unresolved either way**: patch `0022` records that `has_sparse =
true` makes NVK ask for a *bind context* — a third GPFIFO channel. Today
`has_transfer_queue` is `false` precisely because there is one channel.
How many channels a process may open has never been measured, and
promising sparse without knowing is a promise about an unknown.

**Done when**: `t_sparse` has run, arm C's answer is in a commit
message, and either D12 is reopened with the work above or closed with
the log quoted.

## 1.2 `t_syncpt_incr` — whether a host signal is buildable

`nvk_horizon_sync_signal` does not touch a syncpoint. The ioctl it
needed (`NVHOST_IOCTL_CTRL_SYNCPT_INCR`) was always in libnx and nothing
had called it; `horizon_gpu_syncpt_incr()` now wraps it.

The danger is in `horizon/include/horizon_gpu/sync.h` and is worth
repeating: libnx tracks a channel's next fence as `fence.value +
fence_incr`, and this layer keeps a 64-bit shadow on top. Both count
increments the **GPU** will make. One made behind them does not fail —
it makes every fence that channel already handed out look reached one
submit early. This test does that deliberately, to a channel it creates
for the purpose.

It answers: is the increment honoured; does it satisfy a threshold the
GPU never reached; does the channel survive, still submit, still retire.
The counter-versus-shadow drift is printed, not just asserted.

**If it passes**: the next step needs a syncpoint that is *not* a
channel's. `nvioctlChannel_GetSyncpt(fd, module_id, &syncpt)` is the
candidate and **the module ids are not documented in libnx** — if
acquisition fails, expose `has_cpu_signal = false` and leave today's
behaviour alone. Do not invent a module id.

**If it fails**: the fase closes with the test and the reason. That is
still more than existed before.

**Done when**: `t_syncpt_incr` has run and either a dedicated syncpoint
is wired into `nvk_horizon_sync_signal` or the log's reason is recorded.

## 1.3 `t_fence_wait_many` — the ceiling under every wait

`horizon_gpu_fence_wait` (`horizon/sync/syncpt.c`) blocks in
`nvFenceWait` for 100 ms chunks and **deliberately ignores what the
chunk returned** — expiry is the loop's pulse. That is right for a chunk
that timed out and wrong for one that could not be armed: the wait still
honours its deadline and still returns the right answer, while spending
the whole time re-reading a counter as fast as the ioctl comes back.
Silently, on a core, with nothing looking.

Whether that is reachable depends on how many concurrent syncpoint waits
the platform will register. Nothing in this tree knows the number. A
Vulkan application with a thread per queue reaches whatever it is as a
matter of course.

The test ramps 1..32 threads twice: raw `nvFenceWait` to find the
ceiling and name the failing `Result`, then through the layer to time
the batch — a batch near one timeout ran concurrently, a much longer one
serialised.

**Done when**: the ceiling and the batch timings are in a commit
message, and — if the layer degrades — `horizon_gpu_fence_wait` stops
treating an unarmed chunk as a pulse.

## 1.4 `t_vk_timestamp` — `timestampPeriod`

`mesa/src/nouveau/vulkan/nvk_physical_device.c:932` publishes
`timestampPeriod = 1.0f` for every device NVK drives. True of the GPUs
nouveau was written for; never checked here. It is the only thing
turning a query result into a duration, so if it is wrong every
profiler, frame-time overlay and `VK_EXT_calibrated_timestamps` is wrong
by the same factor and none of them can tell.

**Two clocks, and they must not be conflated.** Verified in the tree:

- the **query** clock — `vkCmdWriteTimestamp`, which NVK emits as
  `NV9097_SET_REPORT_SEMAPHORE`. This is the one `timestampPeriod`
  describes.
- the **device** clock — `horizon_gpu_device_get_timestamp`, the
  `GET_GPU_TIME` ioctl, which patch `0025` hands to NVK's
  `get_gpu_timestamp`, and therefore what calibrated timestamps would
  pair with a CPU clock.

Calibrated timestamps are only correct if those are one domain. The test
measures both against `armGetSystemTick` over half a second and prints
their ratio.

**`timestampPeriod` is deliberately unchanged.** Editing it on a
prediction would give that prediction the authority of a measurement.

**Done when**: the run has printed nanoseconds per tick for both clocks
and their ratio, and `timestampPeriod` is either confirmed at `1.0f` or
changed in a new patch quoting the measurement.

## 1.5 `minImageCount` — the number patch `0052` now asks for

`0052` replaced the hardcoded `minImageCount = 2` with
`bqQuery(NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS)` + 1. The selector value
`3` is written out in `wsi_horizon.c` because libnx's
`display/types.h` transcribes the head of Android's query enum and stops
before it.

**What this queue actually answers has never been read.** `t_nwindow`
already drives the producer API one level below `NWindow` and is where
that measurement belongs.

**Done when**: `t_nwindow` reports the queue's answer, and the fallback
path in `wsi_horizon_min_image_count` is either confirmed as unreachable
or kept with the reason.

---

# 2. Cross-compiled, never run

## 2.1 The `.nro` files

| Test | Why it is new or changed |
|---|---|
| `t_sparse` | new — §1.1 |
| `t_syncpt_incr` | new — §1.2 |
| `t_fence_wait_many` | new — §1.3 |
| `t_vk_timestamp` | new — §1.4 |
| `t_init` | now asserts `gpu_va_bit_count == HORIZON_CMDS_GPU_VA_BITS` |

**`t_init` is the regression risk in this batch.** Device creation now
*fails* when the queried VA width differs from the constant the command
builder encodes. That is the intended behaviour — a truncated GPU
address is a valid address the GPU will write to — but it means a chip
reporting anything other than 40 no longer comes up at all. Every other
test depends on device creation, so **run `t_init` first**: if it fails
on the width check, nothing else in the suite is meaningful.

**Done when**: the whole `.nro` suite has been run once against this
build and the results recorded.

## 2.2 The Mesa patches — not compiled

`0050`, `0051` and `0052` **apply cleanly** — verified three times by
resetting `mesa/` to `MESA_COMMIT` and re-running
`scripts/apply-mesa-patches.sh`, 52 of 52, no fuzz, no rejects. That
proves the diffs fit. It proves nothing about compiling.

They have not been built because the native half of the pipeline needs
`LLVMSPIRVLib` and `libclc`, which only the toolchain image carries:

```sh
env -u DEVKITPRO scripts/build-toolchain-image.sh   # then
env -u DEVKITPRO scripts/ci-build-archives.sh
```

(`build-toolchain-image.sh` does nothing while `$DEVKITPRO` is set — it
concludes the toolchain is this machine.)

The riskiest part is **not** the C. It is the meson change in `0051`,
which splits `idep_horizon_gpu_util` so that `src/vulkan/wsi` can depend
on `horizon_gpu` without the shader cache being enabled. Variable
visibility across `subdir()` boundaries is exactly what only a real
`meson setup` catches.

`t_vk_timestamp.c` and `tests/common/vkfw.c` do compile clean with
`-Werror` against the real Vulkan headers (`-fsyntax-only`), which is a
type check and not a link.

**Done when**: `scripts/ci-build-archives.sh` completes, including the
two artefact-dependent gates it ends with.

---

# 3. Not started

From the approved plan, still untouched:

- **Fase C steps 3–4** — a dedicated syncpoint and wiring it into
  `nvk_horizon_sync_signal`. Blocked on §1.2.
- **Fase D** — the WSI can scale. `nvk_wsi.c` sets
  `force_swapchain_to_currentExtent = true` and `wsi_horizon.c`
  publishes a fixed extent; the compositor scales, and `BqBufferInput`
  has a `scalingMode` field this backend already fills from
  `nw->scaling_mode`. Lifting it allows rendering at 720p and presenting
  at 1080p docked. `nwindowSetDimensions` **cannot be called while
  buffers are registered**, so it goes after `nwindowReleaseBuffers` and
  before the first `nwindowConfigureBuffer`. Patch `0040`'s
  dock/undock → `VK_SUBOPTIMAL_KHR` logic has to be rechecked when the
  extent is the application's choice rather than the window's.
- **Fase E steps 2–4** — partial bind/unbind and `has_sparse`. Blocked
  on §1.1.

---

# 4. Things that were confirmed correct

Recorded so nobody "fixes" them. None of these needs action; they are
here because the cost of re-litigating them is higher than the cost of
the paragraph.

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
- Querying `big_page_size` instead of hardcoding it (`device.c:138`).
