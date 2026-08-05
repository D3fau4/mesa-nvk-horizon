## The degraded-baseline opt-in (2026-07-28)

The emulator stops at `horizon_gpu_channel_create`, so steps 5–9 of the
mandatory sequence — buffer, allocation, bind, fill, submit, wait,
readback — have never been executed anywhere, and the next console run
would be the first time that code runs at all. This adds the opt-in
recorded as the previous next task, so that code can be shaken out
before the console round trip rather than during it.

Shape, following the rule that nothing degrades by default:

- `horizon_gpu_device_create_info::allow_untrusted_syncpt_baseline`, or
  `HORIZON_GPU_UNTRUSTED_SYNCPT_BASELINE=1`. Off unless asked for by
  name, exactly like `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER`.
- With it, a failed initial `SyncptRead` no longer fails channel
  creation; the shadow baseline becomes 0 and is marked untrusted.
  `horizon_gpu_channel_syncpt_baseline_trusted()` and
  `horizon_gpu_device_untrusted_syncpt_seen()` report it, and both the
  opt-in and each degraded channel are logged at `ERROR` level.
- `t_vulkan` decides at run time: its bare-channel probe now doubles as
  the platform test. The read works → nothing is enabled, the run is a
  normal one. The read fails → the test enables the mode, says so, and
  **fails the run at the end whatever the readback said**, because a
  fence built on a baseline nobody measured can report "reached" without
  the GPU having done anything. One `.nro`, and the real-hardware path
  is bit-for-bit the previous behaviour.

Documented as `docs/synchronization.md` § 9. No Mesa patch was needed:
NVK's channels inherit it from the device.

What this does **not** do: it does not let the emulator satisfy
anything. `horizon_gpu_fence_wait` reads the same syncpoint, so a
degraded run is expected to fail at `vkWaitForFences` instead of at
`vkCreateDevice` — four more steps of coverage, and an honest failure.

Tested: host tests 103/103, cross build, six gates, series re-applied
twice from the pinned base. **Not run on hardware or emulator yet** —
the artefacts are in `build/pkg`.

## `t_sysinfo` 18/19 on hardware — the binary was stale (2026-07-28)

The 2026-07-27 console logs fail exactly one check in both process
modes: every rung of the `svcMapMemory` ladder is rejected with
`0x0000dc01`. That is not a new failure and it needs no further console
time to explain — it is the **already-fixed** one, run again from an
older `.nro`.

The proof is in the log's own text:

```
note svcMapMemory(0x1000): rejected 0x0000dc01
```

Commit `1857448` (2026-07-27, *"tests: map the granularity probe into
the stack region"*) changed that probe to use `virtmemFindStack` instead
of `virtmemFindAslr`, and added a decoder that appends the kernel
description to exactly that line — `0xdc01` is kernel description 110,
`KernelError_InvalidMemoryRange`, i.e. wrong region, not a granularity
refusal. The logged line carries no decoded suffix, and the current
source cannot print it without one. So the `.nro` that produced this log
predates the fix.

Status: the fix exists in the tree and has **never been run on
hardware**. `t_sysinfo.nro` travels in the next package; if the region
was the whole story it goes to 19/19, and if it does not, the new line
names the kernel description and that is a different investigation with
a real starting point.

## D14 closed on the build side — `t_uncached` (2026-07-28)

D14's remaining debt was a hardware test for the UNCACHED policy.
`tests/t_uncached.c` is that test, registered in both build paths. It
asks the three questions the kernel alone can answer, separately, so a
failure names which:

1. **Does `svcSetMemoryAttribute` accept our allocations?** A CACHED
   allocation of identical size and alignment runs first as the control,
   so an allocator problem cannot be read as a D14 problem.
2. **Is the result usable by ordinary C?** Uncached on AArch64 is
   Normal-NC if the kernel is generous and Device-nGnRE if it is not,
   and multi-register accesses to Device memory fault. A dword loop and
   a `memcpy` are the check, preceded by a note — if the log stops
   there, that *is* the answer.
3. **Do CPU writes reach the GPU with no cache maintenance?** The test
   builds a NOP command list in uncached memory, **never calls
   `horizon_gpu_mem_flush`**, submits and waits. Completion means the
   GPU read what the CPU wrote. The missing flush is the test, not an
   oversight, and it needs no new engine emitters — the list the GPU
   fetches is the CPU write being measured.

Teardown is checked too: a second uncached allocation after the first is
freed would notice a `destroy` that forgot to clear the attribute.

Tested: cross build clean under `-Werror`, six gates, host tests
103/103. **Never executed** — it is a hardware measurement.

## Audit of the never-executed path — one crash found (2026-07-28)

Steps 5–9 of the mandatory sequence have never run anywhere, so the
console run would have been their first execution. Reading them first
was worth one round trip, and it found a null dereference that would
have ended the run inside `vkCreateDevice` with no log line at all.

**The defect.** `nvkmd_horizon_alloc_mem` never filled
`nvkmd_mem::va`. NVK reads that field directly and not as a special
case: `nvk_CreateBuffer` allocates a VA for a buffer only when it is
sparse or capture/replay (`nvk_buffer.c:119-121`), so every ordinary
buffer takes the memory's —

```
buffer->vk.device_address = mem->mem->va->addr + memoryOffset;
                                 -- nvk_buffer.c:271
```

— and it is not confined to `vkBindBufferMemory` either.
`nvk_device.c:295` reads `dev->zero_page->va->addr` while
`vkCreateDevice` is still running, and `nvk_cmd_buffer.c:195` and
`:261` read it for every push and every upload. `nvkmd_mem_init`
(`nvkmd.c:66-83`) zeroes the field and nothing in nvkmd core fills it;
the nouveau backend allocates the VA and binds the memory inside its
own `alloc_mem` (`nvkmd_nouveau_mem.c:56-66`). Patch 0031 does the
same here, and frees the VA before the memory because releasing a
reservation unmaps what is bound in it while `horizon_gpu_mem_destroy`
refuses with `ERR_BUSY` for exactly that reason.

**What else the audit checked, and cleared:**

- `vkCmdFillBuffer` needs no shader: it drives the NV90B5 copy engine
  (`nvk_cmd_copy.c:897-957`), so NAK is not on this path.
- NVK's subchannel table (`nv_push.h:68-96`) is 0=3D, 1=compute,
  2=NV9039, 3=NV902D, 4=NV90B5 — identical to
  `horizon_cmds_set_objects`. And NVK does not emit the binds itself;
  `nvk_queue_subchannels_from_engines` is annotated *"these line up with
  nouveau_ws_context_create"*, i.e. on nouveau the kernel does it. Ours
  is the in-stream replacement (R7), and `t_submit` has it passing on
  console: *"SET_OBJECT list completed without fault"*, *"no error
  notification after engine binds"*.
- `nvkmd_horizon_ctx_wait` passes a NULL `vk_device` into the sync
  type's `wait()`. Safe: `__vk_log_impl` returns early on a NULL
  instance when `MESA_DEBUG=0`, which this build is
  (`-DMESA_DEBUG=0`, confirmed in `build.ninja`). It does mean the
  error text is dropped — a diagnostics gap, not a fault.
- `horizon_gpu_channel_wait_fence` reads the syncpoint before testing
  the deadline, so `vkWaitForFences(timeout=0)` polls rather than
  reporting a false timeout.

**The submit path, checked against nouveau's.** `nvkmd_ctx_exec`
carries a per-entry `incomplete` flag that this backend ignores. That
is correct, and now verified rather than assumed: nouveau uses it only
to decide when to flush its batched push array, so that an incomplete
run is never split across two ioctls
(`nvkmd_nouveau_ctx.c:179-195`) — it never reaches a GPFIFO entry, and
only `no_prefetch` becomes a flag. This backend submits every entry of
a call in one `horizon_gpu_submit`, so the run cannot be split and
there is nothing to honour.

The same reading did find a half-done guard. `nvkmd_horizon_ctx_exec`
refuses a push whose byte count is not a multiple of four, with a
comment about silent truncation — but an entry also carries its address
with the low two bits unavailable and its length as a 21-bit dword
count, so a misaligned address or a push of 8 MiB or more truncates the
same way through two other doors. nouveau asserts both
(`nvkmd_nouveau_ctx.c:198-199`); patch 0032 refuses both, because an
assert is enough where the values go on to a validating kernel and this
is not that. Neither is reachable from NVK today — push memory is
dword-aligned by construction and command-buffer chunks are far below
8 MiB — so this closes the class, not an instance.

**A second live defect: `nonCoherentAtomSize`.** `nvkmd_pdev_info::
nc_atom_size_B` was filled from the services' compression page size. It
is a CPU cache-maintenance granularity, not a GPU one:
`vkFlushMappedMemoryRanges` and `vkInvalidateMappedMemoryRanges` run
`dc cvac` / `dc civac`, so it has to be the cache write-back granule.
`cache_ops.h:65` says so outright — *"Vulkan drivers should return this
as nonCoherentAtomSize"* — and nouveau uses `util_cache_granularity()`
(`nvkmd_nouveau_pdev.c:113`).

Not merely imprecise: GM20B reports `compression_page=0x20000`
(measured, `t_init` on console, 2026-07-27), and
`nvk_device_memory.c:461` rounds a flush range **up** to this number
before handing it to the cache ops. A 4 KiB allocation flushed with
`VK_WHOLE_SIZE` therefore ran `dc civac` across 128 KiB — 124 KiB past
the end of the object, a data abort if the pages beyond it are not
mapped and silent maintenance of unrelated memory if they are. And it is
on the mandatory sequence's own path: `t_vulkan` flushes its poison and
invalidates before the readback, both with `VK_WHOLE_SIZE`. Patch 0033.

Also settled while reading it: `t_vulkan` picks the first HOST_VISIBLE
memory type, and NVK lists cached first on an SoC
(`nvk_physical_device.c:1581-1591`), so the mandatory sequence exercises
the **cached** path with explicit maintenance — D5's mechanism. D14's
uncached type is type 1 and `t_vulkan` never touches it; `t_uncached` is
its only coverage. Worth knowing before reading either log.

**One latent hazard found and deliberately not fixed.** Of the fifteen
ops `nvkmd.c` dispatches, this backend leaves two NULL:
`alloc_tiled_mem` and `import_dma_buf`. Neither is on the Phase 4 path
— `vkAllocateMemory` takes the tiled branch only when
`pte_kind != 0 || tile_mode != 0` (`nvk_device_memory.c:227`), and
`t_vulkan` allocates plain buffer memory — but they differ in how safe
that is:

- `import_dma_buf` is genuinely unreachable: it needs an import-fd
  struct, which needs an external-memory extension this driver does not
  advertise.
- `alloc_tiled_mem` is **not** guarded. `nvkmd_dev_alloc_tiled_mem`
  (`nvkmd.c:118`) calls it through the pointer with no check, and any
  tiled image reaches it through ordinary Vulkan with no extension
  involved (`nvk_cmd_draw.c:1055`, `nvk_device_memory.c:228`). It is a
  null call waiting for Phase 5.

Left as it is on purpose: a stub returning an error would be a Phase 5
design decision taken early, and Phase 5 is where tiled memory gets its
real answer. Recorded here so it is found by reading rather than by
crashing.

Tested: cross build, six gates, host tests 103/103, series re-applied
twice from the pinned base (33 patches). Not executed.

## Hardware run, 2026-07-28 — four tests, and the first real Vulkan fault

Four logs from the owner's console, this build.

| Test | Result |
|---|---|
| `t_uncached` | **PASS 19/19** — D14 holds on hardware |
| `t_threads` | **PASS 67/67** |
| `t_ostime` | **PASS 43/43** |
| `t_vulkan` | FAIL 36/37, aborted at `vkCreateDevice -> -13` |

**D14 is closed on hardware.** The uncached policy answered all three
questions it was built to ask: `svcSetMemoryAttribute` accepts our
reservations; the resulting memory is usable with ordinary C — `memcpy`
round-trips, so Horizon gives Normal-NC and not Device, and the crash
this test warned about did not happen; and the GPU read a 64-pair NOP
list written by the CPU **with no cache maintenance at all** and
completed it. That is the whole property `NVKMD_MEM_COHERENT` needs.

`t_threads` and `t_ostime` were owed from Phase 3 on hardware rather
than on the emulator. Both paid.

### The syncpoint question is settled

```
[horizon_gpu:I] channel 0x3df8f20050: created, syncpt id=26 (live channels now 1)
[horizon_gpu:I] channel 0x3df8f20050: up, syncpt=26 initial=77282 zcull=off
  ok   probe: the syncpoint baseline is readable (id=26, value=77282)
```

The degraded mode did not engage, as designed. NVK's own channel came up
on the same syncpoint. Everything the emulator blamed on the driver was
the emulator.

### `vkCreateDevice -> -13`, and why the log did not say why

The only Mesa output was two lines that turned out to be noise:

```
MESA: error: nvkmd_horizon: VA 0x80fa000 freed with 0x10000 still bound at +0x0
MESA: error: nvkmd_horizon: VA 0x80f9000 freed with 0x1000 still bound at +0x0
```

Freeing a bound VA is nvkmd's ordinary path — `nvkmd_horizon_mem_free`
releases the VA its object owns without unbinding, and
`nvk_mem_arena_finish` states it outright ("Freeing the VA will unbind
all the memory"). `nvkmd_nouveau_mem_free` does the same. Logging it as
a driver bug was wrong, and it was actively harmful: it was the only
thing printed, so it read as the cause.

The real failure was silent because **every `vk_errorf` in this build
is**. `vk_log.c` returns before printing unless the instance has debug
logging on or a messenger is registered, and the `#if !MESA_DEBUG` guard
is compiled in at `--buildtype=plain`. `enable_debug_logging` is set by
the Intel drivers and nobody else.

### Diagnosis, from the numbers alone

`nvkmd_horizon_alloc_va` chose the big-page half whenever the
reservation's own size and alignment were multiples of the big page.
That reasons about the wrong object. `horizon_gpu_vm_map` applies the
reservation's page size to **each mapping inside it** — both offsets
aligned to it, size rounded up to it and still having to fit the memory
object — and a reservation is not always bound as one piece.
`nvk_mem_arena` binds `NVK_MEM_ARENA_MIN_SIZE` (64 KiB) chunks at 64 KiB
offsets into a contiguous VA that is megabytes wide.

So `dev->images` asked for a contiguous VA of `1024 * 1024 *
sizeof(nil_descriptor)`, which met the old condition and landed in the
big-page half (`as_big_page = 0x20000`, from the log). Its first chunk,
`nvk_mem_arena_mem_size_B(0) = 0x10000`, was bound at offset 0;
`horizon_gpu_vm_map` rounded `0x10000` up to `0x20000`;
`horizon_range_fits_u64(0, 0x20000, 0x10000)` is false →
`HORIZON_GPU_ERR_OVERFLOW` → `nvkmd_horizon_result` default →
`VK_ERROR_UNKNOWN` = **-13**.

Every number in the log is accounted for: exactly two memory objects
alive at the failure (zero_page `0x1000` at 0x80f9000, arena chunk
`0x10000` at 0x80fa000), freed in reverse order, and 0x80fa000 is not
`0x20000`-aligned because it is the chunk's own GART VA, not the
arena's.

### Fixed (patch 0034)

- The small-page half is now the default; the reservation's size and
  alignment no longer decide. The block-linear exception stays, because
  the Maxwell MMU only fills those kinds in big pages.
- Freeing a bound VA is silent again.
- `t_vulkan` registers a `VK_EXT_debug_utils` messenger, chained into
  `VkInstanceCreateInfo::pNext` **and** created as an object, so the two
  windows are both covered. This is the API's own answer to "tell me
  why", it costs the test one struct, and Mesa stays untouched. Finding
  this defect cost a full console round trip that a messenger would have
  answered in one line.

**Known risk, recorded not guessed at.** The block-linear path inherits
the same rounding hazard: a tiled image whose memory object is not a
multiple of the big page will have its bind size rounded past the end of
the object and fail. The fix is to round the *allocation* up where the
kind is block-linear, which is `nvkmd_horizon_mem`'s decision. Untested
and untouched because Phase 4's sequence binds no images.

Tested: host 103/103, cross build, six gates, series re-applied twice
from the pinned base. **Not yet run on hardware.**

## Hardware run, 2026-07-28 (second) — `vkCreateDevice` passes, the GPU faults

The page-half fix worked. `vkCreateDevice -> 0`, and the sequence gets
eleven steps further before stopping:

```
  ok   probe: the syncpoint baseline is readable (id=26, value=92772)
  ok   vkCreateDebugUtilsMessengerEXT -> 0 (driver errors will be reported below)
[horizon_gpu:I] channel 0x274d9296e0: up, syncpt=26 initial=92772 zcull=off
[horizon_gpu:I] channel 0x274d969020: up, syncpt=27 initial=2 zcull=off
  ok   vkCreateDevice -> 0
  ok   vkGetDeviceQueue
  ok   vkCreateBuffer -> 0
  ok   a host-visible memory type the buffer accepts
  ok   vkAllocateMemory -> 0
  ok   vkBindBufferMemory -> 0
  ok   vkMapMemory -> 0
  ok   vkFlushMappedMemoryRanges (poison) -> 0
  ok   vkCreateCommandPool -> 0
  ok   vkAllocateCommandBuffers -> 0
  ok   vkBeginCommandBuffer -> 0
  ok   vkEndCommandBuffer -> 0
  ok   vkCreateFence -> 0
[horizon_gpu:E] Kickoff failed: 0x00000d5c (notifier: 31 'MMU fault')
  note vk warning [nvkmd_horizon_ctx.c:180]: horizon_gpu_submit(1 spans) failed: status 4, nv 0x00000d5c (VK_ERROR_UNKNOWN)
  FAIL vkQueueSubmit -> -4
RESULT: FAIL (51/52) [aborted early]
```

Two channels came up on this device — the upload queue's (syncpt 26,
sharing the probe's) and the queue's own (syncpt 27, `initial=2`, a
fresh counter). Everything through `vkCreateDevice` therefore includes
successful submits: `horizon_gpu_channel_bind_engines` runs at context
creation on both, and the descriptor-table uploads went through the
upload queue. The first submit that fails is NVK's own work.

**The debug-utils messenger paid for itself immediately.** That `note vk
warning` line is the first driver message this project has ever seen on
hardware; every previous failure was silent.

`0x00000d5c` decodes as module 348 (`Module_LibnxNvidia`), description 6
= `LibnxNvidiaError_Timeout`. So the kickoff timed out *and* the error
notifier had an MMU fault recorded. The fault is the event; the timeout
is the channel already wedged behind it.

### Why this could not be diagnosed from the log, and what was changed

nvgpu's error notifier carries the fault **type** and nothing else, and
libnx exposes no ioctl for the faulting address. "Notifier 31" is all
the kernel will ever say. The message also named neither channel, with
two live.

The addresses are ours, though, and so is the map they were supposed to
be in. Patch 0035: `nvkmd_horizon_dev` keeps every live VA on a list,
and a failed submit walks it for each push it submitted, answering one
of three things — the VA and binding that contains the address, that it
is inside a reservation with **nothing bound there**, or that no
reservation covers it at all. The failure names its channel too.

The middle answer is the diagnosis for a fault of this shape, printed on
the spot instead of costing a console round trip per hypothesis.

### Hypotheses **not** acted on

Recorded because they were considered and rejected on evidence, not
shipped as guesses:

- *Engines never bound.* `nvkmd_horizon_create_ctx` calls
  `horizon_gpu_channel_bind_engines` at creation. Also an unbound
  subchannel gives notifier 25 (illegal method), not 31.
- *The small-page half is exhausted by patch 0034.* Measured: the region
  is `pages=0x3f7fff` × 4 KiB = **15.87 GiB** (`t_init`, run 1), and NVK
  reserves ≈ 4.03 GiB in it — 4 GiB for the contiguous `shader_heap`
  (`cls_eng3d` 0xb197 = MAXWELL_B < VOLTA_A), ≈32 MiB for `images`,
  128 KiB for `samplers`; `event_heap` and `qmd_heap` are not contiguous
  and reserve nothing. Margin 4:1.
- *`sync_types[0]` is binary while `nvk_mem_stream_init` asks it for a
  timeline.* This is real — nouveau's `sync_types[0]` is a DRM syncobj,
  which is binary *and* timeline, and the `assert` in `vk_sync_init`
  that would catch the mismatch is compiled out (`-DNDEBUG` confirmed in
  the build). But the obvious fix is wrong: `nvkmd_horizon_ctx_signal`
  calls `nvk_horizon_sync_set_fence`, which `container_of`s to
  `nvk_horizon_sync`. Putting the emulated timeline first would hand it
  a `vk_sync_timeline` and corrupt memory. As it stands the mismatch
  degenerates to "wait for the latest fence", which over-waits and is
  safe. **Pending decision, not a fix to guess at.**

### Audit of the fault path while waiting for the console

Five more candidates read through and **rejected on evidence**. Recorded
because a rejected hypothesis is worth as much as a fix when the next
measurement costs a console round trip:

- *`no_prefetch` is dropped by the backend.* True — `struct
  nvkmd_ctx_exec` carries it and `nvkmd_horizon_ctx_exec` reads only
  `addr` and `size_B`, while `nvkmd_nouveau_ctx` turns it into
  `DRM_NOUVEAU_EXEC_PUSH_NO_PREFETCH`. Harmless here:
  `HORIZON_GPU_SUBMIT_DEFAULT` is already `NOT_MAIN | NO_PREFETCH`, so
  horizon never prefetches any span. The flag is redundantly always on,
  which is the safe direction.
- *`incomplete` is dropped too.* It means "the next push must be in the
  same submit ioctl", and `nvkmd_horizon_ctx_exec` puts every span in one
  `horizon_gpu_submit` call. Satisfied by construction.
- *Subchannel assignment differs from NVK's.* It does not. NVK
  (`nv_push.h`): 0 = NV9097 3D, 1 = NV90C0 compute, 2 = NV9039 M2MF,
  3 = NV902D 2D, 4 = NV90B5 copy. horizon (`channel.c:408`):
  `threed_class, compute_class, inline_to_memory_class, twod_class,
  dma_copy_class`. Identical, and `vkCmdFillBuffer` emits NV90B5 —
  subchannel 4, `dma_copy_class` 0xb0b5 as queried on this console.
- *Engines never bound on the queue's channel.* `nvkmd_horizon_create_ctx`
  calls `horizon_gpu_channel_bind_engines` at creation, and an unbound
  subchannel reports notifier 25, not 31.
- *The push address comes from somewhere exotic.* It does not:
  `nvk_cmd_buffer.c:195` uses `cmd->push_mem->mem->va->addr + offset`,
  and `nvk_cmd_mem_create` is a plain `nvkmd_dev_alloc_mapped_mem`. Both
  candidate addresses — the push and `vkCmdFillBuffer`'s destination
  (`mem->mem->va->addr + memoryOffset`) — come from patch 0031's per-mem
  VA.

Both candidates therefore live in the same place, and reading cannot
separate them. So patch 0035 also dumps the **whole VA map** on a submit
failure — every reservation, its page size, and what is bound in it —
not just the pushes. `vkCmdFillBuffer`'s destination is baked into the
push by NVK and this layer never sees it, so naming only the pushes
would leave half the candidates unaccounted for. The dump is capped at
64 reservations and says how many it dropped.

Tested: host 103/103, cross build, six gates, series re-applied twice
from the pinned base. **Not yet run on hardware.**

## BLOCKER LIFTED: a console is available again (2026-08-04)

The owner has a Nintendo Switch again. Phase 4's exit criterion is
**unblocked**, and the artefacts for the run are built and packaged
(`build/pkg`, manifest included, sha256 per artefact).

What this run is measuring is not the same thing the last console run
measured. **Five defects were found and fixed since 2026-07-28 and not
one of them has executed on hardware:** patch 0034 (the page half),
0036 (`get_value`, a call through NULL), 0037 (window collision
detection), 0038 (R18 — privileged GR register writes disabled, which
is the candidate fix for the MMU fault that ended the last console
run), and the degraded reap in `horizon/`. Whatever the log says, it
is the first hardware evidence about any of them.

Two `t_vulkan` artefacts are shipped so one round trip covers both
outcomes, because round trips are the expensive thing here and one has
already been wasted on a misread log:

- `t_vulkan.nro` — `T_VULKAN_PUSH_DUMP 0`, `T_VULKAN_DEBUG_SYNC 0`.
  This is the exit-criterion run: no CPU stall inserted anywhere, and
  a log short enough to paste here whole.
- `t_vulkan-pushdump.nro` — the same test with `NVK_DEBUG=push_dump,vm`
  compiled in. Print-only, so it changes no behaviour, but it decodes
  thousands of lines. **Only to be run if the first one fails.**

Verified by content rather than by intent: the string `push_dump,vm`
is present in the second artefact and absent from the first.

The degraded baseline cannot engage on hardware — `t_vulkan`'s probe
enables it only where channel creation fails for want of the syncpoint
read, and real nvgpu implements that read. The final `t_check(!degraded)`
stays as the guard.

## PHASE 4 EXIT CRITERION MET — hardware, 2026-08-04

`t_vulkan` **PASS (56/56)** on a Nintendo Switch. The mandatory Vulkan
sequence runs end to end and the CPU reads back exactly what the GPU
wrote:

```
  ok   readback: 0/1024 words wrong; first at [0] = 0x00000000, expected 0xa5c3f00d
  note readback: 1024/1024 words are 0xa5c3f00d — the GPU wrote it
  ok   the run was neither degraded nor diagnostic (such a run verifies nothing)
RESULT: PASS (56/56)
```

Every condition the milestone attached to it holds, and each is checked
by the run itself rather than asserted here:

- **Real hardware.** Not the emulator: the syncpoint baseline was
  readable (`id=26, value=47515`), which is the read the emulator does
  not implement.
- **No wait-idle inserted.** `T_VULKAN_DEBUG_SYNC` is 0 in the artefact —
  no `vkQueueWaitIdle`, no `vkDeviceWaitIdle`, no debug-synchronous
  stall. Submission stayed asynchronous.
- **Not degraded and not diagnostic.** The last check before the result
  is exactly that, and it passed.
- **The readback is the GPU's work, not memory that already held it.**
  The buffer was poisoned with `~0xa5c3f00d` and flushed out of the CPU
  cache before the submit, so the pattern could only arrive by being
  written.

Artefact provenance is in `build/pkg/MANIFEST.txt` (sha256 per .nro,
devkitA64 r29.2-1, gcc 15.2.0-7, image `ghcr.io/d3fau4/nx-dev:latest`).
The full log is `docs/hw-logs/t_vulkan-PASS-20260804.log` and reproduced
verbatim at the end of this section.

**The Phase 3 debt closed in the same batch.** `t_threads` (67/67) and
`t_ostime` (43/43) had never run on a console; they have now, and they
pass. Nothing from Phase 3 is owed.

### What it took, in the order it was found

Four defects stood between the last console run and this one, and only
the first was on anybody's list:

1. **R18** — privileged GR register writes reset a homebrew channel.
   Recorded in Phase 0 as a Phase 5 risk; it fired in Phase 4. Patch 0038
   disabled them behind `has_priv_reg_writes`, and the MMU fault stopped
   reproducing.
2. **The GPU's L2 held every write.** Measured, not guessed: a four-arm
   matrix showed no flush never made the write visible, `MEMBAR` did not
   help, and `L2_FLUSH_DIRTY` did. NVK raises the right barrier for this
   (`VK_ACCESS_2_HOST_READ_BIT` → `NVK_BARRIER_HOST_WFI_FLUSH_SYSMEM`)
   but below Hopper emits only `NV906F_SET_REFERENCE`, with no writeback.
3. **The fence's WFI used the wrong scope** — `CURRENT_SCG_TYPE` under a
   comment claiming it meant "all", while `clb06f.h:141-143` says `ALL`
   is 1. A copy-engine transfer is not in the graphics scheduling class
   group, so the fence never waited for `vkCmdFillBuffer`'s NV90B5 work.
4. **The L2 writeback was emitted before the wait**, so anything
   completing during the wait was never flushed.

Defects 3 and 4 were invisible to every test below `t_vulkan`, and the
reason is worth keeping: they all wrote with **host** methods, which the
PBDMA executes inline and which therefore need no waiting. Only
asynchronous engine work exposes them. That is also why `t_gpuwrite`
passed 51/51 while the ordering was still wrong.

**What found them was a test, not a hunch.** Nothing below `t_vulkan` had
ever asked memory a question — `t_submit` proves command lists execute,
fences order, and cross-channel GPU waits unblock, but every one of those
observations goes through a syncpoint, which on Tegra is a host1x counter
and not memory. `t_gpuwrite` was written to close that gap and closed it
in one run.

### Honest accounting of the last step

The L2 flush (defect 2) was **measured**. The scope and ordering fixes
(defects 3 and 4) were **reasoned** — stated as such in `STATUS.md`
before the run, not after — and this run is what turned them into
measurement. The prediction was recorded first and it held.

### Still open, and not hidden by a green result

- **The shader local/shared window overlap.** `MESA: error:
  nvkmd_horizon: reservation [0xa14a000, 0x10a14a000) overlaps the shader
  local/shared memory window [0xfe000000, 0x100000000)` is still printed
  on every `vkCreateDevice`. It did not stop this sequence, which uses no
  shaders. It is not fixed, and blocking off the window was tried once
  and broke `vkCreateDevice` outright (patches 0039/0040).
- **The L2 flush is unconditional**, one dirty-L2 writeback per submit.
  Narrowing it Mesa-side, at the barrier where NVK already knows a host
  read is coming, is a pending decision — now measurable against a
  working baseline rather than guessed at.
- **`t_vulkan`'s memory-type note is now stale.** It says the readback
  rests on cache maintenance (D5); the measurement says it rests on the
  GPU L2 writeback. The note is left untouched in the artefact that
  produced the log above, and corrected separately so this log
  corresponds exactly to a commit.
- One Vulkan sequence passing is not a driver. Phase 5 is unchanged.

### The passing log, verbatim

```
[horizon_gpu:I] device up: gm20b arch=0x120 impl=0xb rev=0xa1 gpc=1 tpc/gpc=2 big_page=0x20000 as_big_page=0x20000 va_bits=40
== t_vulkan ==
  ok   probe: horizon_gpu_device_create -> 0
[horizon_gpu:I] channel 0x47fbb21050: created, syncpt id=26 (live channels now 1)
[horizon_gpu:I] channel 0x47fbb21050: up, syncpt=26 initial=47515 zcull=off
  ok   probe: the syncpoint baseline is readable (id=26, value=47515)
  ok   the non-conformance opt-in is set in the environment
  ok   GetInstanceProcAddr(vkCreateInstance)
  ok   vkCreateInstance -> 0
  ok   VK_EXT_debug_utils entry points resolved
  ok   vkCreateDebugUtilsMessengerEXT -> 0 (driver errors will be reported below)
  ok   GetInstanceProcAddr(vkEnumeratePhysicalDevices)
  ok   GetInstanceProcAddr(vkGetPhysicalDeviceProperties)
  ok   GetInstanceProcAddr(vkGetPhysicalDeviceQueueFamilyProperties)
  ok   GetInstanceProcAddr(vkGetPhysicalDeviceMemoryProperties)
  ok   GetInstanceProcAddr(vkCreateDevice)
  ok   GetInstanceProcAddr(vkGetDeviceQueue)
  ok   GetInstanceProcAddr(vkCreateBuffer)
  ok   GetInstanceProcAddr(vkGetBufferMemoryRequirements)
  ok   GetInstanceProcAddr(vkAllocateMemory)
  ok   GetInstanceProcAddr(vkBindBufferMemory)
  ok   GetInstanceProcAddr(vkMapMemory)
  ok   GetInstanceProcAddr(vkCreateCommandPool)
  ok   GetInstanceProcAddr(vkAllocateCommandBuffers)
  ok   GetInstanceProcAddr(vkBeginCommandBuffer)
  ok   GetInstanceProcAddr(vkCmdFillBuffer)
  ok   GetInstanceProcAddr(vkEndCommandBuffer)
  ok   GetInstanceProcAddr(vkCreateFence)
  ok   GetInstanceProcAddr(vkQueueSubmit)
  ok   GetInstanceProcAddr(vkWaitForFences)
  ok   GetInstanceProcAddr(vkFlushMappedMemoryRanges)
  ok   GetInstanceProcAddr(vkInvalidateMappedMemoryRanges)
  ok   GetInstanceProcAddr(vkDestroyFence)
  ok   GetInstanceProcAddr(vkDestroyCommandPool)
  ok   GetInstanceProcAddr(vkFreeMemory)
  ok   GetInstanceProcAddr(vkDestroyBuffer)
  ok   GetInstanceProcAddr(vkDestroyDevice)
  ok   GetInstanceProcAddr(vkDestroyInstance)
[horizon_gpu:I] device up: gm20b arch=0x120 impl=0xb rev=0xa1 gpc=1 tpc/gpc=2 big_page=0x20000 as_big_page=0x20000 va_bits=40
WARNING: NVK is not a conformant Vulkan implementation, testing use only.
  ok   vkEnumeratePhysicalDevices -> 0
  ok   one physical device, got 1
  ok   vkEnumeratePhysicalDevices(list) -> 0
  note device: NVIDIA gm20b (NVK gm20b) (api 1.3.354)
  ok   at least one queue family, got 1
[horizon_gpu:I] channel 0x47fbb2a6e0: created, syncpt id=26 (live channels now 1)
[horizon_gpu:I] channel 0x47fbb2a6e0: up, syncpt=26 initial=47515 zcull=off
MESA: error: nvkmd_horizon: reservation [0xa14a000, 0x10a14a000) overlaps the shader local/shared memory window [0xfe000000, 0x100000000). Shader local and shared accesses go through that aperture; anything bound here will be read as locals and vice versa.
[horizon_gpu:I] channel 0x47fbb6a020: created, syncpt id=27 (live channels now 2)
[horizon_gpu:I] channel 0x47fbb6a020: up, syncpt=27 initial=16 zcull=off
  ok   vkCreateDevice -> 0
  ok   vkGetDeviceQueue
  ok   vkCreateBuffer -> 0
  ok   a host-visible memory type the buffer accepts
  note memory type 0: flags 0xb = DEVICE_LOCAL HOST_VISIBLE HOST_CACHED — the readback rests on cache maintenance (D5)
  ok   vkAllocateMemory -> 0
  ok   vkBindBufferMemory -> 0
  ok   vkMapMemory -> 0
  ok   vkFlushMappedMemoryRanges (poison) -> 0
  ok   vkCreateCommandPool -> 0
  ok   vkAllocateCommandBuffers -> 0
  ok   vkBeginCommandBuffer -> 0
  ok   vkEndCommandBuffer -> 0
  ok   vkCreateFence -> 0
  ok   vkQueueSubmit -> 0
  ok   vkWaitForFences -> 0
  ok   vkInvalidateMappedMemoryRanges -> 0
  ok   readback: 0/1024 words wrong; first at [0] = 0x00000000, expected 0xa5c3f00d
  note readback: 1024/1024 words are 0xa5c3f00d — the GPU wrote it
  ok   the run was neither degraded nor diagnostic (such a run verifies nothing)
RESULT: PASS (56/56)
```

## The console run of 2026-08-04, and what it moved

### R18 is fixed, and that is measured now, not argued

The MMU fault that ended the last console run **does not reproduce**.
`vkCreateDevice -> 0`, `vkQueueSubmit -> 0`, `vkWaitForFences -> 0`, and
`t_vulkan` reached its readback for the first time anywhere: **55/56**.
Patch 0038 — the privileged GR register writes disabled behind
`has_priv_reg_writes` — was the right diagnosis. R18 was recorded in
Phase 0 as a Phase 5 risk; it fired in Phase 4 and is now closed on
hardware.

That is the first hardware evidence for any of the five fixes made since
2026-07-28, and four of the five are exercised by this run reaching the
readback at all.

`t_init` on console also settled two things the emulator could not:
`zcull ctx_size = 0x10200` against the emulator's obviously-wrong `0x1`
— the check tightened blind from `> 0` to `>= 0x1000` was right and
would have rejected the emulator value — and a GPU timestamp that is
real and monotonic.

### The readback failed, and the cause is not where it was expected

Every one of the 1024 words came back as the poison, `0x5a3c0ff2`, which
is exactly `~0xa5c3f00d`. The memory type is `DEVICE_LOCAL HOST_VISIBLE
HOST_CACHED` with no COHERENT, so the standing expectation — recorded in
the test's own note — was cache maintenance, D5.

**It is not.** `t_gpuwrite`, written for this and run on the same
console, removes Vulkan, Mesa and NVK entirely and asks the bottom layer
with the channel's own semaphore release. It failed **identically on
CPU-cached and on CPU-uncached memory**:

```
cached:   target[0] before invalidate = 0x3f0011fe, after = 0x3f0011fe
uncached: target[0] before invalidate = 0x3f0011fe, after = 0x3f0011fe
          VERDICT the write never reached this memory
```

Two things follow, and both narrow the search hard:

- **CPU cache maintenance is eliminated.** It cannot explain a failure
  that is identical with caching on and off. D5 is not what is breaking
  the readback.
- **The defect is below NVK.** No Mesa code runs in `t_gpuwrite`. Whatever
  is wrong is in the channel, the mapping, or the GPU's own write path,
  and every Vulkan-level explanation — the shader local/shared window
  overlap included — is a separate question that this failure does not
  need.

Note also what still worked: the fence reached in both arms, so the
pushbuffer executed, and the GPU **read** its command list from a mapping
made exactly like the target's. GPU reads from small-page mappings work.
Only the write is missing.

### Why nothing below t_vulkan had caught this

`t_submit` proves a command list executes, that fences order by
submission, and that a GPU-side cross-channel syncpoint wait unblocks
(R10). Every one of those observations is made through a **syncpoint**,
which on Tegra is a host1x counter and not memory. Until `t_gpuwrite`
this project had never asked memory a question below the Vulkan layer.
That is the gap `STATUS.md` had recorded as owed, and it is exactly the
gap the bug was hiding in.

### It is the GPU's L2, and the fix is measured

The matrix ran and answered all four ways at once
(`docs/hw-logs/t_gpuwrite-run2-matrix.log`):

| arm | mapping | extra command | result |
|---|---|---|---|
| A | L2-cacheable | none | **FAILED** — payload nowhere in the allocation |
| B | **non**-cacheable | none | **PASSED** |
| C | L2-cacheable | `MEMBAR` | **FAILED** — a barrier is not enough |
| D | L2-cacheable | `L2_FLUSH_DIRTY` | **PASSED** |

A GPU write lands in the GPU's L2 first, and nothing about a syncpoint
increment obliges that L2 to reach the memory a CPU reads. B shows the
write bypassing L2 when the mapping is not cacheable; D shows it reaching
memory when the L2 is written back explicitly; C rules out the cheaper
barrier, which is worth having measured rather than assumed.

**Where NVK stands on this, checked in the tree.** NVK does raise the
right barrier — `VK_ACCESS_2_HOST_READ_BIT` maps to
`NVK_BARRIER_HOST_WFI_FLUSH_SYSMEM` (`nvk_cmd_buffer.c:533`) — but below
Hopper that barrier emits only `NV906F_SET_REFERENCE`
(`nvk_cmd_buffer.c:783-795`), with no L2 writeback at all. On a desktop
part that is enough. On GM20B it is not, and arm C is the measurement
that says so.

### The fix, and where it went

`horizon_gpu_channel` now begins every submit's fence block with
`MEM_OP_D(L2_FLUSH_DIRTY)`, in exactly the order arm D measured: the
work, then the flush, then the WFI and syncpoint increment that
`horizon_cmds_fence_incr` already emitted. The block is built once per
channel, so a submit costs three extra dwords.

It went into `horizon/` rather than into a Mesa patch because that is
where the defect is: `t_gpuwrite` runs no Mesa code and still failed, and
a fence that signals while its writes are invisible is a fence that lies.
Fixing it here also fixes it for every consumer at once.

**Unconditional, and the cost is acknowledged rather than hidden.** This
layer cannot know which submits a CPU will later read from, and a fence
that is only sometimes truthful is worse than one that always costs a
flush. A dirty-L2 writeback per submit is not free, and narrowing it —
Mesa-side, at the `HOST_WFI_FLUSH_SYSMEM` barrier where NVK already knows
a host read is coming — is a **pending decision**, not something to guess
at before the unconditional version is confirmed on hardware.

### What the arms guard now

With the flush in the channel, all four arms are expected to pass, and
the matrix has stopped diagnosing and started guarding. Arm A is the
direct regression: an L2-cacheable mapping with no flush of its own,
which works only because the channel flushes. The arms can no longer
prove *why* it works, only that it does — the proof is the run above,
kept in `docs/hw-logs/`. That is also the "break the gate on purpose"
evidence for this fix: the failing measurement already exists and is
recorded, rather than being simulated after the fact.

### Run 3: the fix works, and t_vulkan still fails — which localises it again

`t_gpuwrite` **51/51**, all four arms, arm A included: the L2 writeback in
the channel's fence block does make a GPU write visible to the CPU. And it
cost nothing elsewhere — `t_submit` 30/30, `t_syncpt` 48/48,
`t_fence_wait` 14/14, with the flush now on every submit's fence path.

`t_vulkan` failed **identically**: 55/56, 1024/1024 words, same value.

That pair is more informative than either log alone. GPU→CPU visibility
now works below Vulkan and NVK's readback still does not, and NVK submits
through `horizon_gpu_submit` (`nvkmd_horizon_ctx.c:175`), so it *is*
getting the flush. The difference is what does the writing:

- `t_gpuwrite` writes with a **host** semaphore release, executed inline
  by the PBDMA.
- `vkCmdFillBuffer` writes with **NV90B5**, the copy engine
  (`nvk_cmd_copy.c:897`) — asynchronous engine work on subchannel 4.

The copy engine is bound: `nvkmd_horizon_create_ctx` calls
`horizon_gpu_channel_bind_engines`, which does all five subchannels with
queried class numbers, and `cls_copy` is reported to NVK
(`nvkmd_horizon_pdev.c:158`). So the fill is dispatched. What was wrong is
that nothing waited for it.

### Two defects in our own fence block, both found by reading the header

**1. The WFI scope was the wrong constant, under a comment asserting the
opposite of the header.** `horizon_cmds_fence_incr` emitted

```c
buf[n++] = 0; /* WFI scope: all preceding work in this channel */
```

`clb06f.h:141-143` says `SCOPE` 0 is `CURRENT_SCG_TYPE` and `ALL` is 1. A
copy-engine transfer is not in the graphics scheduling class group, so the
narrower scope never waited for it — and no test could see that, because
every test below `t_vulkan` used host methods only, which are inline and
need no waiting.

**2. The L2 writeback was emitted before the wait, not after.** The fence
block was flush → WFI → increment. Anything that completed *during* the
wait was therefore never flushed. For host methods this is invisible —
they finish before the flush is even fetched — which is exactly why
`t_gpuwrite` passed 51/51 with the order wrong.

The two compose into one symptom and one fix: **WFI(SCOPE_ALL) → dirty-L2
writeback → syncpoint increment**, which is what the emitter now produces.
The flush moved out of `channel.c` into `horizon_cmds_fence_incr`, where
the ordering is one unit and documented as load-bearing rather than
incidental.

Host checks cover both, and the scope was broken back to 0 on purpose to
confirm the check fails (`38/39`).

**Honesty about what this is.** The L2 flush was measured. **This fix is
reasoned, not measured** — the constant is certainly wrong because the
header says so, and flushing before waiting is certainly wrong ordering,
but that these two are what break `vkCmdFillBuffer` is inference from the
host-method/engine-work split, not an observation. `t_vulkan` is the
measurement, and it is the next run.

If it still fails, the localisation continues rather than restarts: the
next instrument is a copy-engine arm in `t_gpuwrite` — a real NV90B5 fill
at the horizon level, reproducing `vkCmdFillBuffer` with no Mesa in the
picture. Deliberately not built yet, for the reason that has held all
week: one measured change at a time.

### The measurement this still owes

`t_vulkan` has **not** been re-run since the fix. The chain from "the
bottom layer can now make a GPU write visible" to "the Phase 4 exit
criterion passes" is plausible and unproven, and the readback is the only
thing that can prove it.

### The superseded plan (kept: it is why the matrix existed)

A GPU write lands in the GPU's L2 first, and nothing about a syncpoint
increment obliges that L2 to reach the memory a CPU reads. The failing
mapping was created L2-cacheable
(`NVGPU_AS_MAP_BUFFER_FLAGS_CACHEABLE`), so the next run varies that and
what is around it rather than guessing:

| arm | mapping | extra command | decisive about |
|---|---|---|---|
| A | L2-cacheable | none | the baseline, re-measured in the same run |
| B | **non**-cacheable | none | the mapping attribute |
| C | L2-cacheable | `MEMBAR` | whether a barrier suffices |
| D | L2-cacheable | `L2_FLUSH_DIRTY` | an explicit writeback — the fix to want |

D is the outcome to hope for: it keeps GPU caching on and pays only
where a CPU is going to read, where B would disable caching wholesale.

If all four fail, that is equally definite — it is not L2 residency, and
the CPU and GPU are not looking at the same memory. A failing arm
therefore also scans the whole allocation for the payload and reports
the offset if it landed anywhere, which separates "wrong offset" from
"did not land" without another round trip.

`MEM_OP_C`/`MEM_OP_D` and the semaphore encoding were both taken from
`mesa/src/nouveau/headers/nvidia/classes/clb06f.h` — the channel class
GM20B's own characteristics report, `gpfifo=0xb06f` — and not recalled.
Host checks cover every field and were broken on purpose: four ways for
the semaphore and the search, twice for MEM_OP.

### The push dump is parked

`t_vulkan-pushdump.nro` blackscreens on console and its log truncates in
the middle of the MME microcode upload, which alone decodes to thousands
of lines. It is not needed now: `t_gpuwrite` localised the defect below
Vulkan, so NVK's push stream is not where the answer is.

### The state it was blocked in (2026-07-28)

The owner no longer had access to a Nintendo Switch; only an emulator
was available. Phase 4's exit criterion was **blocked**, not abandoned,
by the owner's decision recorded here.

The criterion cannot be met on the emulator, and this is not a
formality. The emulator answers `NVHOST_IOCTL_CTRL_SYNCPT_READ` with
`0x55c` (`LibnxNvidiaError_NotImplemented`), and the fence is built on
that counter. "The CPU reads the pattern the GPU wrote" is a statement
about *ordering*; without the counter there is no ordering, so a
matching pattern would prove nothing — it could be right by accident
with the GPU never having run. `t_vulkan` already refuses to print PASS
in that state, and that refusal stays.

What the emulator is still good for, and what work continues on it:

- It executes the code. The `get_value` defect fixed in patch 0036 is a
  jump through a NULL pointer; it would crash there exactly as it would
  on console.
- The degraded baseline (`HORIZON_GPU_UNTRUSTED_SYNCPT_BASELINE`,
  synchronization.md § 9) is enabled by `t_vulkan`'s own probe when the
  platform cannot read the syncpoint, so the run reaches the steps after
  `vkCreateDevice` instead of stopping there.
- If the MMU fault of run 3 reproduces, it can be fixed without a
  console in the loop.

What it cannot establish, and must never be recorded as: fence
behaviour, submit completion, or anything the syncpoint orders.

`T_VULKAN_DEBUG_SYNC` is **off** for emulator builds. The mode waits on
each submit's fence, and that wait needs the same missing read, so it
would replace every notifier reading with the same failure.

Every result obtained from here until a console returns is labelled
**emulator** in this file, never "hardware".

### Patch 0036 — a NULL call, found by reading

`nvk_mem_stream_init` creates its stream sync on `sync_types[0]` with
`VK_SYNC_IS_TIMELINE`. On nouveau that is a DRM syncobj, binary *and*
timeline; here it is `nvk_horizon_sync_type`, which is binary. The
runtime's guard is an `assert` in `vk_sync_init` and this is a `-DNDEBUG`
build, so the mismatch never surfaced as an error.

It surfaces as a crash instead: `vk_sync_get_value` calls
`sync->type->get_value` with **no NULL check**, and this type had no
`get_value`. Reached from mem-stream chunk recycling as soon as a
chunk's idle time point is ahead of what the stream has seen pass.

Fixed by giving the type the timeline bookkeeping that use needs: the
object carries the value its pending fence will complete and the largest
value known to have completed; `get_value` polls the fence with a
zero-timeout `channel_wait_fence` (which also notices a faulted channel
and needs no device pointer); `wait` returns at once for a value already
passed. `nvkmd_horizon_ctx_signal` now passes `signal_value` through
rather than dropping it, which is what left that counter at zero.

Binary use is unchanged: `wait_value` is 0 there and `passed_value`
starts at 0, so the new early-out is never taken.

**Still a pending decision**: whether this type should advertise
`VK_SYNC_FEATURE_TIMELINE` and replace the runtime's emulation the way
nouveau's syncobj does. That would change the device's timeline mode
from EMULATED to ASSISTED across the whole driver, and reordering
`sync_types` alone would hand `nvkmd_horizon_ctx_signal` a
`vk_sync_timeline` to `container_of` — worse than the mismatch it fixes.
Not done mid-bring-up.

## The push dump, and what is actually in the stream (emulator, 2026-07-28)

`NVK_DEBUG=push_dump,vm` on the emulator produced 4596 lines: every VA
operation in order, and every push decoded. The emulator reaches
`vkQueueSubmit -> 0`, so this is the same stream real nvgpu faults on.

Four submits on the queue's channel, matching the fence `(2, 4)` the
run waited on: `bind_engines`, the queue context init, the queue state
update, the fill.

**Every address in the stream, checked against the map:**

| Method | Value | Mapped? |
|---|---|---|
| `SET_TEX_HEADER_POOL_B`, max index 0x3ff | `0x8002000` | yes — 32 KiB used of 64 KiB bound |
| `SET_TEX_SAMPLER_POOL_B` | `0xa012000` | yes — two chunks, 128 KiB |
| `OFFSET_OUT` (the fill) | `0x1_0a064000` | yes — mem\<0x28\>, 128 KiB bound |
| `SET_SHADER_LOCAL_MEMORY_WINDOW` | `0xff000000` | aperture, see below |
| `SET_SHADER_SHARED_MEMORY_WINDOW` | `0xfe000000` | aperture, see below |
| **`SET_PROGRAM_REGION`** (3D and compute) | **`0xa052000`** | **no — `alloc va [0xa052000, 0x10a052000)`, never bound** |

So the fill's destination and the pushes themselves were never the
problem, and neither were the descriptor pools. Two things came out of
this that were not visible any other way.

### Finding A — the shader memory windows collide with our address space

`nvk_push_dispatch_state_init` programs the local and shared memory
windows at fixed addresses, `0xff000000` and `0xfe000000`. They are
apertures, not allocations: a shader's LDL/STL/LDS/STS go through them.
Upstream knows nothing protects them and says so:

> "Reduce likelihood of collision with real buffers by placing the hole
> at the top of the 4G area. This will have to be dealt with for real
> eventually by blocking off that area from the VM." Really?!? TODO: Fix
> this for realz.

On this platform it is not a likelihood. The small-page region begins at
`0x8000000` and NVK's reservations march up through 4 GiB, so the shader
heap's own reservation `[0xa052000, 0x10a052000)` **covers both
windows**. The day a shader uses local or shared memory it will read
whatever the heap has bound there — its own code, most likely.

First detected (patch 0037), then **fixed** (patch 0039) — because the
premise of the detection was wrong. This file said
`NVGPU_AS_IOCTL_ALLOC_SPACE` has no "place it here" form. It has one:
`NvAllocSpaceFlags_FixedOffset`, sitting in libnx's `ioctl.h` next to the
call we were already making. The correction is worth stating plainly:
the aperture was always reservable, and "open decision" was a conclusion
drawn from a fact nobody had checked.

`horizon_gpu_vm_reserve_fixed` now exists, and reports a range that came
back at a different address as a failure rather than accepting it. Two
things were built on it; **one worked and one did not**, and the
emulator said which within one run.

**Kept: `NVKMD_VA_ALLOC_FIXED` is implemented**, closing extension 5 of
the six Phase 4 step 1 enumerated.

**Backed out (patch 0040): reserving the window aperture.** Patch 0039
reserved `[0xfe000000, 0x100000000)` at device creation so nothing else
could be given it. It worked — the overlap warning stopped — and it
broke device creation:

```
vk warning: horizon_gpu_vm_reserve(0x100000000, 4096, 0x0) failed:
            nv 0x00000f5c   (LibnxNvidiaError_InsufficientMemory)
FAIL vkCreateDevice -> -13
```

The window sits at 3.97 GiB and NVK's shader heap wants a **contiguous
4 GiB**, so blocking the aperture splits the small-page region at
precisely the wrong address and the heap no longer fits below it.

Weighed honestly: the heap reserves 4 GiB but binds 64 KiB chunks from
the bottom, so a chunk only reaches the window after roughly 3.9 GiB of
shader code — a real defect, and an unreachable one. Against that, the
reservation is an immediate and total failure. Detection stays, the
block-off goes.

This is the emulator earning its keep: a fix that read as obviously
correct was wrong, and one run said so.

### D12's recorded reason was wrong too

The same paragraph that claimed `ALLOC_SPACE` had no fixed form also
justified D12 with "NVGPU_AS has no sparse reservation".
`NvAllocSpaceFlags_Sparse` sits in the same enum. A sparse reservation
**is** expressible.

The decision stands, on a true basis now: what is missing is **partial
unbind**. Sparse binding rebinds and unbinds arbitrary sub-ranges of a
reservation over its lifetime; `horizon_gpu` maps and unmaps whole
mappings, which is exactly why `nvkmd_horizon_va_unbind` demands an
exact `(offset, range)` match. `nvkmd_horizon_ctx_bind` is implemented
and does binds immediately, so that half is not the obstacle either.
Sparse needs the split implemented in `horizon_gpu`, plus the bind
context's third channel. Known, bounded, and not needed by Phase 4.

### Finding B — the leading hypothesis for the hardware MMU fault

The queue context init writes **two GPU privileged registers from the
pushbuffer**, via `CALL_MME_MACRO(23)` = `NVK_MME_SET_PRIV_REG`:

```
mthd 38b8 NV9097_CALL_MME_MACRO(23) .V = 0x0
mthd 38bc NV9097_CALL_MME_DATA(23)  .V = 0x8          <- mask, bit 3
mthd 38bc NV9097_CALL_MME_DATA(23)  .V = 0x419f78     <- gr_gpcs_tpcs_sm_disp_ctrl

mthd 38b8 NV9097_CALL_MME_MACRO(23) .V = 0x0
mthd 38bc NV9097_CALL_MME_DATA(23)  .V = 0x4000
mthd 38bc NV9097_CALL_MME_DATA(23)  .V = 0x419e44     <- sms_hww_warp_esp_report_mask
```

This is the only thing in the whole stream that is not an ordinary
memory operation, and it is the only one whose success depends on what
the *kernel* permits a user channel to do. Desktop nouveau allows it;
Horizon's nvgpu is not going to be as permissive, and an emulator would
neither implement nor enforce it — which is exactly why the fault does
not reproduce there.

NVK's own comment says what the writes are for: enabling FP helper
invocation memory loads, so that one dEQP subgroup test stops failing
occasionally. Not required for anything Phase 4 does.

### Finding B is R18, and it was already in this repository

Written up as a hypothesis above, then found already recorded — Phase 0
put it in `docs/known-risks.md` as **R18**, derived from the reference
ports' hardware experience:

> **R18 — Privileged GR register writes reset a homebrew channel.** The
> reference neuters `nvk_mme_set_priv_reg` in `nvk_cmd_draw.c` because
> privileged graphics-register writes are rejected for homebrew channels
> and reset the channel, **after which every submit times out**.

That is the third hardware run, symptom for symptom: `vkCreateDevice`
returns 0 because submission is asynchronous, and the next kickoff on
that channel fails with `LibnxNvidiaError_Timeout` and an error notifier
set. R18 expected it in Phase 5, at the first draw. It arrives in Phase 4
because these two writes happen while the *queue's context* is being
initialised, long before anything is drawn.

So this is not a guess to defer. It is a documented, hardware-derived
fact that we knowingly parked, and it has come due.

**Fixed as patch 0038, the way R18 asked for.** R18's own mitigation was
"do not carry this patch forward blindly … determine what NVK loses by
skipping them. Record the answer rather than inheriting the no-op." So:

- The gate is a kmd capability, `nvkmd_info::has_priv_reg_writes` — the
  same shape as D12's `has_sparse`, and for the same reason. The class
  check says the *chip* supports SET_PRIV_REG; whether the driver
  underneath lets an unprivileged channel use it is a separate question.
  True for nouveau, where NVK's behaviour is untouched; false for
  horizon.
- What is lost is recorded at the declaration, from NVK's own comments:
  bit 3 of `gr_gpcs_tpcs_sm_disp_ctrl` enables FP helper invocation
  memory loads, without which one dEQP subgroups test fails
  occasionally; `sms_hww_warp_esp_report_mask` disables Out Of Range
  Address exceptions for a case involving an empty fragment shader.
  Neither is reachable by anything this port runs today.

**Unverified on hardware.** The reasoning is: R18's recorded fact, the
timeout symptom matching it exactly, and the push dump showing these are
the only two operations in the whole stream that are not ordinary memory
access. That is strong, and it is still not a measurement. The console
run that confirms it is the same one that closes Phase 4.

## The last unmeasured link: what the readback rests on

Audited by reading, because nothing can execute it. The test picks the
first HOST_VISIBLE memory type the buffer accepts, and NVK's list for
this backend puts a `HOST_VISIBLE | HOST_CACHED` type before the
coherent ones. So the buffer is very likely **cached**, which means:

- `NVKMD_MEM_COHERENT` is not set, so D14's uncached mapping does not
  apply and the flush/invalidate are **not** no-ops;
- the poison flush before the submit, and the invalidate before the
  readback, both have to do real work;
- and that work is Mesa's `cache_ops_aarch64.c` (`dc cvac` / `dc civac`),
  because `util_has_cache_ops()` is true on aarch64 and `nvkmd_mem_sync_*`
  defers to it.

**Nothing has measured those on Horizon, in either direction.**
`t_uncached` passed on hardware, but it measured the *uncached* path —
and CPU→GPU at that. The readback needs GPU→CPU through a *cached*
mapping. The two share no mechanism.

What makes it plausible: the fill's `LAUNCH_DMA` carries
`FLUSH_ENABLE = TRUE` (read off the push dump), which flushes the GPU's
side, and `dc civac` cleans to the point of coherency, which on a UMA
part is DRAM. Plausible is not measured.

So `t_vulkan` now reports the memory type it chose and which mechanism
the result rests on, in one line, because that is the first question
anyone will ask if the readback comes back wrong.

**Recorded gap, not built on a guess:** a horizon-level test of GPU→CPU
visibility would need a GPU command that writes to memory, and
`horizon_gpu` has only fence-increment, syncpoint-wait, `SET_OBJECT` and
NOP. Adding a semaphore-release builder to `horizon/` to measure
something `t_vulkan` already measures end to end is not worth it unless
`t_vulkan`'s readback actually fails; then it is the right next step.

## The unchecked-premise audit (2026-07-28)

Three claims in this tree about what the platform cannot do turned out
to be about what `horizon_gpu` had not wrapped. They are worth listing
together, because the pattern cost more than any one of them:

| Claim | Reality | Outcome |
|---|---|---|
| "`ALLOC_SPACE` has no place-it-here form" | `NvAllocSpaceFlags_FixedOffset` | `NVKMD_VA_ALLOC_FIXED` implemented — **extension 5 closed** |
| "NVGPU_AS has no sparse reservation" (D12's reason) | `NvAllocSpaceFlags_Sparse` | D12's decision stands, on a true basis: partial unbind is what is missing |
| "horizon_gpu has no GPU timestamp query" | `nvGpuGetTimestamp` | implemented — **milestone item 8, extension 1 closed** |

All three sat in libnx's `ioctl.h`, in the same file the code was
already calling into. The sweep that found them was mechanical: list
every `nvioctl*` libnx exports, then grep this tree for statements that
the platform lacks something.

The timestamp one is the worst of the three, because **this file already
knew**. Step 1's interface table says of `get_gpu_timestamp`: "libnx has
the same clock: `nvGpuGetTimestamp` (`nvidia/gpu.h:16`) over
`nvioctlNvhostCtrlGpu_GetGpuTime` (`nvidia/ioctl.h:262`) → extension 1".
The survey was correct; the implementation comment written afterwards
said "horizon_gpu has no GPU timestamp query yet" and made the function
`UNREACHABLE`. A survey is only worth what the code built from it
remembers.

**Extension 6 closed too**, and it is a fourth instance of the same
pattern. `has_zcull_info` was hardcoded false because "Zcull belongs to
the channel and nothing in Phase 4 uses it" — but the query is a *device*
property, libnx caches it at `nvGpuInit` (`nvGpuGetZcullInfo`), and
`horizon_gpu` already binds Zcull context buffers. Only the wrapper was
missing.

The mapping is field by field on purpose: the Horizon struct and
`drm_nouveau_get_zcull_info` carry the same ten numbers in different
orders — `subregion_count` last in one, third from last in the other — so
a struct copy would put a count where an alignment belongs and nothing
would complain. Checked before writing, which is the only reason this
note exists rather than a bug.

### Where the six extensions stand

| # | Subject | State |
|---|---|---|
| 1 | GPU timestamp | **closed** (0041) |
| 2 | GPFIFO push semantics | retired earlier |
| 3 | CPU syncpoint increment | exists; deliberately not implemented — see below |
| 4 | sparse reservation flag | exists; gated by D12, which stands on partial-unbind grounds |
| 5 | fixed VA | **closed** (0039/0040) |
| 6 | Zcull geometry | **closed** (0042) |

Three closed in one sitting, none needing hardware, all of them
previously recorded as blocked by the platform.

**Extension 3 checked and left alone.** CPU syncpoint increment
(`nvioctlNvhostCtrl_SyncptIncr`) exists too, and
`nvk_horizon_sync_signal` does a CPU-only signal without it. That is not
a defect: `nvkmd_horizon_ctx_wait` is *also* CPU-side, so a CPU signal is
observed by every wait that can see it. Implementing the increment would
add an unused path until Phase 5 builds GPU-side waits. Verified rather
than assumed — which is the whole point of this section.

Two of the six extensions Phase 4 step 1 enumerated are now closed that
were previously recorded as blocked, and neither needed hardware.

The GPU timestamp has one wrinkle worth stating: nvkmd's
`get_gpu_timestamp` cannot report failure. Returning zero would be a
wrong answer offered as a real one, so a failed read logs and repeats
the last good value — a clock that stalls is wrong in a way a caller can
notice, one that jumps to zero is not.

## If the fence wait works, the baseline stops being a guess

Recorded before the run rather than after, because it is the point of
the experiment and should not read as hindsight.

`nvFenceWait(id, threshold, 0)` is a *predicate*: it returns success iff
the syncpoint has reached `threshold`. A predicate is enough to recover
the value. Syncpoint comparison wraps, so for a current value C the
thresholds it has reached form the half-space `(int32_t)(C - T) >= 0`,
and a binary search over that half-space finds C in about 32 calls.

That matters because the untrusted baseline is the only thing still
making a degraded channel's fences unsound. The shadow is initialised
from the value at creation; with no read, § 9 assumes zero and says so.
With the search the value is **measured** — through a different ioctl,
but measured — and the channel can be trusted normally: correct
thresholds, a correct 64-bit shadow, real fences.

Cost: ~32 ioctls once per channel, only where the read is unavailable.
Nothing changes on a platform that can read the counter.

**Not wired up, deliberately.** Wiring it rests entirely on `nvFenceWait`
working where `SyncptRead` does not, and that is exactly what the
pending emulator run measures. Wiring it first would stack unvalidated
work on an unvalidated assumption — the mistake the window block-off
already made once today.

**The arithmetic is built and proven, though**, because that half does
not depend on the pending measurement at all: whether the ioctl answers
is a question about the platform, whether the search is correct is a
question about arithmetic, and only the second can be settled today.
`horizon_syncpt_search_value` (`horizon/sync/syncpt_math.h`) takes the
predicate as a callback, so the file never learns what an ioctl is, and
`tests/host/h_syncpt_math.c` drives it against an oracle that answers
only yes/no — every wrap corner, a 4096-value deterministic sweep, a
counter advancing mid-search, and the predicate failing at the first
probe and mid-search. **Nothing calls it**; it is inert until the run
says otherwise.

Two properties came out of writing it that the plan above only guessed
at, and both are better than guessed:

- The count is exactly **33 probes**, not "about 32": one to place the
  anchor, 31 halvings, one to verify.
- The verification probe is **exact, not a heuristic**. A monotonically
  increasing counter can only make the result stale, never too large — a
  "reached" answer never becomes false — so asking whether `value + 1`
  has been reached is true precisely when the result went stale. `OK`
  means the value *is* the counter's value at that instant.

Gate discipline: the new checks were broken on purpose three ways —
anchor never flipping to the upper half, the verification verdict
ignored, and an off-by-one on the recovered value — and each was caught
by the checks that should catch it (`28/30`, `28/30`, `26/30`), then
restored to `30/30`. Host suite is now **114/114**.

The mesa gates were not re-run for this change and did not need to be:
the diff is three files, none under `mesa/` or `mesa-patches/`, and the
series is unchanged at 42 patches.

So that run has three outcomes and all three are actionable:

| Result | Meaning | Next |
|---|---|---|
| readback validates | the wait works *and* the counter starts at zero there | build the search anyway, so the assumption stops being load-bearing |
| readback wrong, wait succeeded | the wait works, the threshold is offset | build the search; it fixes exactly this |
| the wait fails too | that emulator implements neither | the emulator cannot close this loop; back to hardware |

## Codex PR review, PR #6 (2026-08-04) — 12 findings, 12 real

`chatgpt-codex-connector` reviewed PR #6 at `8fdade7`: **5 × P1, 7 × P2, and
every one of them held up.** The PR #4 review scored 7 real out of 8; this one
did not produce a single false positive, which is worth recording before
anything else, because it changes how much weight the next one gets.

Each was checked against the code before it was touched. Eleven are fixed
below. One is a design change and is a **pending decision** rather than a
commit — see D16.

### The one that mattered: the four-arm matrix was not waiting

`tests/t_gpuwrite.c` embedded its own `horizon_cmds_fence_incr` in the span it
submitted, while `horizon_gpu_submit()` **unconditionally appends the
channel's own fence block** and advances `shadow_target` by exactly one. So
every arm advanced the hardware counter twice and the accounting once, and
the hardware ran permanently one increment ahead.

The consequence is specific, and it is this test's own measurement it
destroys:

| Arm | Threshold waited on | Hardware at that moment | Wait |
|---|---|---|---|
| A | `S+1` | `S+1` after A's own increment | **correct** — returns after the release |
| B | `S+2` | already `S+2`, from A's extra increment | returns **immediately** |
| C | `S+3` | already `S+4` | returns immediately |
| D | `S+4` | already `S+6` | returns immediately |

Arms B, C and D read the target while the GPU was still working on it. They
did not measure cache behaviour; they raced it.

**What this does and does not cost the Phase 4 conclusion.** The two
load-bearing observations survive, because they are the two the race cannot
manufacture:

- **A failed** — and A is the one arm whose wait was correct. "No flush → not
  visible" stands on a sound observation.
- **D passed** — and D was the arm *most* exposed to the early return, since
  it had the largest gap between threshold and hardware. A payload that is
  already present when you look early is present. "L2 flush → visible"
  stands.

What is weakened is the middle: **B failing was read as "identical with CPU
caching on and off", and C failing as "MEMBAR does not help".** Both of those
arms raced, so neither failure is now evidence of anything. The
cache-irrelevance claim in particular rested on A-versus-B and now rests on A
alone.

That claim is not retracted — `t_uncached` tests the same property from the
other side and passes — but it is no longer supported by the experiment that
was cited for it, and `docs/hw-logs/t_gpuwrite-run2-matrix.log` should be read
with this in mind. **The corrected test has not been run on hardware.** Until
it has, the matrix's middle two arms are unmeasured, not re-confirmed.

Also worth saying plainly: this defect was introduced by me, in the very test
written to be the instrument that settled the question, and it survived four
hardware runs because the arm it cannot affect is the first one.

### The rest, and what each was hiding

| Finding | Real? | Why it was invisible |
|---|---|---|
| P1 `meson.build:368` — `t_vulkan` never rebuilt once NVK archives appear | yes | `build-mesa-nvk.sh` runs `build-horizon.sh` *before* compiling NVK, so `fs.exists()` is asked at the one moment the archives are guaranteed absent; `horizon_mesa_state` compared only core Mesa, so no later run reconfigured. Unnoticed because the hardware `.nro` came from the **Makefile** path, which re-evaluates `$(wildcard)` every time |
| P1 `build-mesa-nvk.sh:60` — TLS relocations counted, not gated | yes | The miscompile's signature is **zero** relocations, so the broken build printed the same reassuring `0` as a clean one. `build-mesa.sh` has always run the real gate; this path never did |
| P1 `channel.c:647` — degraded destroy skips the in-flight check | yes | "Unknowable" was too strong: the counter cannot be *read* on that platform, but the fence can still be *asked* through `nvFenceWait` — which this file's own wait path already relies on for exactly that case |
| P2 `channel.c:579` — degraded wait returns after one chunk | yes | Any finite timeout became 100 ms. A two-second wait failed with 1.9 s left to signal in |
| P2 `vm.c:99` — mismatched fixed-VA reservation leaked | yes | `AllocSpace` *succeeded*; only the userspace wrapper was freed, so the kernel reservation became unreachable. An anomaly path that has never fired — which is why nothing caught it |
| P2 `mem.c:155` — uncached restore result discarded | yes | A direct CLAUDE.md violation ("never discard a libnx `Result`"), in the one place where discarding it hands uncached pages back to `malloc` — corruption in unrelated code, far from the cause |
| P2 `t_vulkan.c:658` — null fence after a failed create | yes | `vkQueueSubmit` accepts `VK_NULL_HANDLE` legitimately, so the existing guard could not fire, and `vkWaitForFences` would then take the process down before the framework could print FAIL |
| P2 patch 0019 — binding freed before the unmap result is checked | yes | `horizon_gpu_vm_unmap` keeps the mapping alive on failure *so it can be retried*; its only pointer had already been freed. One failed unmap stranded a VA range permanently |
| P2 `toolchain-env.sh:168` — manifest digest unusable | yes | `RepoDigests` was filtered for the **base** repo while `$HORIZON_IMAGE` is normally the locally built **derived** image, which has no RepoDigests at all. Every manifest since Phase 4 recorded `<base-repo>@unknown` |
| P2 `fetch-rust-crates.sh:77` — vendored crates never refreshed | yes | The script's header says it pins nothing and cannot drift. True of the contents, false of the caching |

### Gate coverage this exposed, beyond the finding

Applying the TLS gate to the NVK build showed the gate itself had a hole:
`find -name '*.o'` finds **821** objects under `build/mesa-nvk` and **none**
under `src/nouveau/rust_runtime`, whose members exist only inside
`libnouveau_rust_runtime.a`. The Rust staticlib — "one more place TLS could
appear", the reason that code was looking at all — was scanned by nothing.
The gate now takes archives too (845 items).

**Broken on purpose, per the rule that an untested gate is not a gate.**
Compiled the four-line reproducer from the script's own header both ways,
archived the `-fPIC` one and deleted its loose object:

```
$ ./scripts/check-tls-relocs.sh build/probe/tlsgate-badonly
MISCOMPILED THREAD-LOCAL ACCESS in 1 object(s):
  build/probe/tlsgate-badonly/libbad.a
exit=1
$ find build/probe/tlsgate-badonly -name '*.o' | wc -l
0          <- what the gate used to look for: it would have exited 0
$ ./scripts/check-tls-relocs.sh build/probe/tlsgate-good
check-tls-relocs: OK (2 object(s)/archive(s) use TLS, all with relocations, 2 scanned)
exit=0
```

Both directions: it fails on the real miscompile and does not fire on the
clean form. The build-state fix was checked the same way — it reconfigured and
produced `t_vulkan` (16 `.nro` on the Meson path, up from 15), and a second
run did **not** reconfigure.

### What was verified, and what was not

**Host build and cross build only. Nothing here has run on hardware.**

- host tests 132/132; `check-layering`, `check-mesa-test-parity`,
  `check-no-abs-paths`, `check-dispatch-complete` all OK
- cross build, Makefile path: 15 `.nro`, `-Werror` clean
- cross build, Meson path: 16 `.nro` including `t_vulkan`
- `scripts/apply-mesa-patches.sh` from the pinned commit: 43 patches apply
  cleanly, and a second run is a no-op
- NVK rebuilt end to end; patch 0043 compiles; the new TLS gate passes over it

**Superseded below — both have now run on a Switch.** Left as written so the
prediction stays legible next to the result.

### Hardware, same day: both corrected tests pass

`docs/hw-logs/t_gpuwrite-run5-review-fixes-PASS.log` — **PASS (47/47)**, and
the arithmetic is the whole audit: 51 − 4, one removed "fence increment
encoded" check per arm. Nothing else changed count, so nothing else changed.

`docs/hw-logs/t_vulkan-run5-review-fixes-PASS.log` — **PASS (56/56)**, with
the corrected memory-type note in it (`the readback uses CPU cache
maintenance; GPU-side visibility is the channel's L2 writeback`), so this log
is from a binary that carries the correction rather than one that predates it.

**What the run re-measures.** All four arms now wait on a threshold that only
their own submit can reach, and all four report
`before invalidate == after == 0xc0ffee01`. Arm A is CPU/GPU-cacheable and arm
B is not; with real waits under them, "the payload is there either way, with
no cache maintenance" is now a sound observation instead of a raced one.
**That restores the cache-irrelevance claim** — which the entry above
correctly said had been left resting on arm A alone — on fresh evidence rather
than on the run that was reinterpreted.

**What it does not re-measure, and this matters more.** The original
experiment was *no flush versus flush*, and **there is no longer a no-flush
arm to run.** The fix in `0cca09d` made the channel's fence block emit WFI
SCOPE_ALL + `L2_FLUSH_DIRTY` on *every* submit, so all four arms get the
writeback from the channel and differ only by a redundant extra `MEM_OP`. The
matrix is degenerate now: four arms passing is consistent with the L2 finding
and is not a test of it.

So the honest position on `t_gpuwrite-run2-matrix.log` is unchanged by this
run. Arm A's failure there stands (correct wait, no flush, not visible), arm
D's success stands, and arms B and C remain unmeasured — this run cannot
recover them because the configuration they distinguished no longer exists.

Re-testing the L2 finding directly would need something this test cannot
currently express: a submit whose channel fence block omits the writeback.
That is an instrument to build if the finding is ever doubted, not a gap in
what is deployed — recorded here so the limit is found by reading rather than
by someone trusting a degenerate matrix.

## D16 closed — a wait may arrive before there is a fence (2026-08-04)

The twelfth Codex finding, and the one held back as a decision. Owner said
address it now, so patch 0044 does.

**The old reasoning had the right premise and the wrong conclusion.** It read:
"nothing submitted and nothing signalled: this can only ever become signalled
by someone else, so waiting is waiting for the deadline." True — and then it
returned `VK_TIMEOUT` instead of waiting for that deadline, instantly, and for
`OS_TIMEOUT_INFINITE` too. "Someone else" is not a reason to stop waiting; it
is the thing being waited for. The type advertises `CPU_WAIT` and `CPU_SIGNAL`
together, so another thread signalling or submitting mid-wait is a pattern
Vulkan permits, and waiting on a fence no queue has touched is explicitly
legal.

**Shape of the fix.** `mtx_t` + `cnd_t` in the sync. Broadcast from the three
transitions that can release a waiter — `signal()`, `set_fence()`, `move()` —
and pointedly *not* from `reset()`, which makes the object less reachable and
has nobody to release. `wait()` loops on the condvar until the sync acquires a
fence, then hands the remaining timeout to `horizon_gpu` exactly as before.

**Where the two decisions collide.** `cnd_timedwait` takes an *absolute*
`TIME_UTC` deadline — precisely what D8 was closed by getting rid of, since
`t_ostime` measured that clock to be the real-time clock. Handing it the
caller's whole deadline would have reintroduced the bug D8 fixed, inside the
fix for D16. So the condvar waits in **100 ms chunks**, each one's absolute
deadline built from now and discarded, with the remaining time tracked as a
duration. A date change can perturb the chunk in flight and nothing else. It
costs no latency: a broadcast ends a chunk immediately, and a wait shorter
than a chunk is not rounded up to one. Same shape and same figure as
`horizon/channel/channel.c`, for the same reason.

**`move()` was a trap.** It copies the payload struct wholesale. With the two
new members that would have copied the destination's live mutex and condvar
over with the source's — including the mutex held on that very line — and then
destroyed the wrong pair in `finish()`. They are preserved across the copy the
way `base` already was, and the two objects are locked in address order so
concurrent moves between the same pair cannot deadlock.

**The primitives were checked, not assumed.** `t_threads` already exercises
`cnd_wait`, `cnd_signal`, `cnd_broadcast` and `cnd_timedwait` against their
deadlines and passed **67/67** on console — `cnd_timedwait(200 ms)` returned
`thrd_timedout` at 200 ms, and a broadcast woke four waiters in 7 ms. This
introduces no untested dependency.

### The test, and why it is single-threaded

`t_vulkan` gained a D16 block after the exit criterion, so it cannot perturb
it. A fresh unsignalled fence is waited on for 200 ms.

**Both versions return `VK_TIMEOUT`. Only the clock separates them** — the old
code executed no blocking call at all between entry and its return, so it
answered in microseconds; the fixed one answers when asked. The check is
therefore on elapsed time (`>= 150 ms`, `< 2000 ms`), measured with
`armGetSystemTick`, which is the monotonic clock and the one thing here a date
change cannot move.

Traced to be sure the check reaches the code it is aimed at: `vkWaitForFences`
→ `vk_common_WaitForFences` → `vk_sync_wait_many` → `__vk_sync_wait_many`,
which for `wait_count == 1` calls `sync->type->wait` directly. No runtime
short-circuit stands between the test and `nvk_horizon_sync_wait`.

**Its discriminating power is established by construction, not by a measured
failing run** — I cannot run the old binary on the console to watch it fail.
That is weaker evidence than the TLS gate's break-test, and is stated as such.

### Hardware: D16 holds, and the test had a blind spot

`docs/hw-logs/t_vulkan-run6-D16-PASS.log` — **PASS (60/60)**, which is 56 + the
four D16 checks.

```
  note D16: vkWaitForFences(200 ms) on a never-submitted fence returned 2 after 200 ms
  ok   D16: waiting on a never-submitted fence times out (2)
  ok   D16: it waited rather than answering at once (200 ms, expected >= 150)
  ok   D16: and it stopped waiting when asked (200 ms, expected < 2000)
```

Return code 2 is `VK_TIMEOUT`, after **200 ms**. The old code answered in
microseconds, so this is the discrimination the check was built for, now
measured rather than argued. The exit criterion above it is untouched —
readback still 0/1024 wrong.

**But 200 ms is exactly two 100 ms chunks, and that hides something.** A loop
that clamps its final chunk to the time remaining and a loop that always waits
a whole chunk both land on 200 ms. The run therefore says nothing about
`min(remaining, chunk)` — and that clamp is the whole reason a *short* wait
stays short. Without it a 5 ms `vkWaitForFences` would block for 100 ms: a
twentyfold overshoot, on the call an application uses to poll without
spinning.

Chosen badly by me, not by chance: 200 ms is a round number and I picked it
for the lower bound without noticing it was also a whole multiple of the chunk
I had just written.

A second check now asks for **5 ms** and requires under 50 ms. Loose on
purpose — a scheduler owes nobody millisecond precision, and what is under
test is an arithmetic clamp, not timer accuracy. **Not yet run on hardware.**

### Verified

Cross build only for the sub-chunk check; the rest is on console above.

- NVK rebuilt from a `mesa/` reset to the pinned commit; the series is now 44
  patches, applies cleanly, and a second run is a no-op
- `check-tls-relocs`: OK, 4 of 845 use TLS, all with relocations
- Meson path: 16 `.nro` including `t_vulkan` with the D16 check compiled in
- host tests 132/132 and the four source gates OK

## Adversarial review of the fix round, PR #6 (2026-08-04) — every finding held

A second review, this time of `8fdade7..9948669` — the commits that fixed the
first twelve findings. **Nothing in it was wrong.** Two rounds now with no false
positive, and this one was worse for me than the first, because the first found
old defects and this one found defects I had just introduced *while fixing
old defects*.

### The one that had to be reverted

**The degraded-destroy fence poll asks a value that means nothing, and is
biased toward saying yes.** When the baseline is untrusted, the read at channel
creation failed too, so `syncpt_value_at_create` is **0** and `shadow_target`
counts submits from zero — it is not the hardware counter, and the code that
sets it says so in as many words: *"its fences are not measurements."* Whether
Horizon resets a syncpoint at channel creation is **R5, still open**. If it
does not, a threshold of 3 is compared against a live counter in the thousands,
which is already past it — so `nvFenceWait` succeeds instantly and the destroy
reports that work retired **having verified nothing**.

That is worse than the "not checked" it replaced. And
`horizon_gpu_channel_wait_fence` says of this same number that "reached" is
only as good as an assumption nobody verified — so the change had two functions
in one file drawing opposite conclusions from one unreliable value, with the
destroy biased toward false assurance.

Reverted. The rejected design is recorded in the code, where the next person to
have the same idea will read it.

I should have caught this. I *quoted* the untrusted-baseline reasoning in the
commit message while adding a check that depends on the baseline being
trustworthy.

### The gate I had just "fixed" was counting everything twice

`check-tls-relocs.sh` scanned loose objects **and** archives. All 821 loose
objects under `build/mesa-nvk` live in `<archive>.p/` directories and are
exactly the members of the 24 archives beside them — so it scanned the same
code twice and reported **845**, in a script whose entire purpose is a
trustworthy count. The "4 objects use TLS" it printed was one archive plus the
same three objects inside it.

Worse, the whole-archive check was blind in exactly the place it had been added
for. I wrote that a mixed archive "is not a shape this toolchain produces". It
is precisely the shape of `libnouveau_rust_runtime.a`, which bundles the
`-Zbuild-std` core/alloc objects with the crate's own from separate
compilations. **Demonstrated rather than argued:**

```
$ nm libmixed.a | grep -q "U __aarch64_read_tp"   -> yes
$ readelf -r libmixed.a | grep -q R_AARCH64_TLS   -> yes  (from the GOOD member)
  whole-archive verdict: OK   <- the miscompiled member is invisible
```

Now archives are expanded and each member judged alone, and only archives with
no loose objects are expanded, so nothing is counted twice:

```
before:  845 scanned, 4 use TLS      (821 objects + the 24 archives holding them)
after:  1207 scanned, 3 use TLS      (821 loose + 386 members of 2 archives with none)
```

386 Rust staticlib members are now checked individually; before, they were one
opaque blob that could not fail. Break-tested: a mixed archive with no loose
objects is caught and the offending **member** is named.

### The rest

| Finding | What it was |
|---|---|
| NVK build dir has three names | `toolchain-env.sh` keyed on `$NVK_BUILD_DIR`, which **nothing sets**; the scripts that build there read `$MESA_NVK_BUILD_DIR`. Point that anywhere else and the state string named a directory the build never used — *the same failure the commit was written to close*, one layer down. Unified. Verified: an override now yields `absent build/elsewhere` instead of `present build/mesa-nvk` |
| `mem.c` create unwind | Returned the *cleanup's* error, discarding the `nvMapCreate` failure that caused the unwind. The caller learned about the mop-up and never why creation failed |
| `memory.h` contract | `destroy` can now fail after destroying the object, and the header documented only BUSY. A caller applying the ordinary retry convention would hit a use-after-free of a pointer this layer calls single-owner. Both failures and their opposite meanings are now in the header |
| TLS gate green on nothing | `check-tls-relocs` exits 0 on an empty tree and the count loop skipped missing archives, so a build that produced *nothing* reported a clean TLS result — the same "zero is not good news" shape the commit message condemned. The artefacts are now asserted to exist first |
| `horizon_image_digest` | Widened to match any `@`, which the rationale never supported: the derived-image case is handled by the `.Id` branch, and all the widening did was return an arbitrary repository for a multi-tagged image. Now tries this project's repositories in order |
| Called twice per manifest | Two independent `docker image inspect` runs that can disagree, in one file whose job is naming a single toolchain. Evaluated once |
| `print-toolchain-versions.sh` | Still labelled the result "image digest" when it is now a full reference or a local id — a provenance field misnaming its contents, in the change that exists to fix provenance fields |
| `fetch-rust-crates.sh` | Keyed its cache on a value that can be `unknown`, which never equals the recorded one — re-downloading the whole vendor closure every run, in the one script whose purpose is offline vendoring. An unidentifiable toolchain now does not invalidate the cache |
| Shell hygiene | `_hz_mesa`, `_hz_nvk`, `_hz_lib` leaked into the caller's shell, `_hz_lib` shared with another function. Renamed and unset |
| `vm.c` | Returned `HORIZON_GPU_ERR_NV` with an empty `.nv` — an NV-flavoured error with nothing to look up, for a call that *succeeded* and merely answered wrongly. Now `VA_EXHAUSTED`, which is what "a fixed request came back elsewhere" means |
| `t_vulkan` teardown | My new `return 1` joined sixteen others that jump over the destroy block and leak the `VkDevice` — channels and NvMap objects never released. The comment even noted it was behaving "unlike the checks above it": it saw the asymmetry and picked the leaking side. Now `goto teardown`. The other sixteen are left for a change of their own |
| Superseded logs | `t_gpuwrite-run4-PASS.log` was still named `-PASS`, still showing `fence increment encoded`, unannotated. `STATUS.md` had the retraction; the file a reader opens did not. `docs/hw-logs/README.md` now says which runs measured less than they claim — and two of my first drafts of *that* were wrong until checked against the logs |
| This file is 362 KB | Decision **D17**. The reviewer drew the causal line: this is *how* four decision rows outlived their answers, and how the opening paragraph went on saying a hardware run was owed after it had happened — which it was still doing when the review arrived. A "Current state" block now leads the file; the real split is a decision because most of this file is evidence |

### Verified

Host and cross build. **None of this round has run on hardware.**

- host tests 132/132; `check-layering`, `check-mesa-test-parity`,
  `check-no-abs-paths`, `check-dispatch-complete` OK
- Makefile path 15 `.nro`, Meson path 16, both `-Werror` clean
- TLS gate over the real NVK build: 3 of 1207, all with relocations
- gate break-tested in both directions on a mixed archive

## Next concrete task

**The emulator is exhausted.** It answered everything it could:

- `vkCreateDevice -> 0`, then every step through `vkQueueSubmit -> 0`;
- it stops at `vkWaitForFences`, which is where a platform with no
  syncpoint read *must* stop — waits are deliberately not degraded;
- the MMU fault does **not** reproduce there, which is itself the
  finding: it is specific to real nvgpu;
- and with `NVK_DEBUG=push_dump,vm` it handed over the whole push stream,
  which is how R18 and the shader-window collision were found without a
  console in the loop.

There is no further question the emulator can answer about *this
sequence*. Five defects were found and fixed since the last console run —
patches 0034 (page half), 0036 (`get_value`, a call through NULL), 0037
(window collision, detected), 0038 (R18), plus the degraded reap in
`horizon/` — and **none of the five has been executed on hardware.**

### Two runs are pending, and they are different runs

**1. `t_init.nro` on the emulator — thirty seconds, and it owes nothing
to a console.** The GPU timestamp and Zcull geometry (extensions 1 and 6)
were closed by *reading libnx's headers*. That code has never executed
anywhere. `t_init` now calls both: the timestamp for being non-zero and
not going backwards across two reads, Zcull for a context size that
`channel.c` independently agrees with. If either closure is wrong, this
says so without spending the console.

**2. `t_vulkan.nro` on a real Switch — the criterion.** One run, and it
goes either way usefully: it closes Phase 4, or it produces a complete
diagnosis, because the build carries the debug-utils messenger, the
VA-map dump on failure, the decoded push dump and the memory-type line.

### Why the tree stops adding changes here

A deliberate call, recorded because it is a decision and not an
omission. Today's window block-off looked obviously right — the premise
was true, the mechanism existed, the overlap warning duly disappeared —
and it broke `vkCreateDevice` outright. One emulator run said so.

There are now five fixes and three extension closures in this tree that
no hardware has executed, and Phase 4 turns on a *single* console run
whenever one is available. Each further unvalidated change raises the
chance that that run fails on something introduced today rather than on
what is left to find. The marginal value of more Phase 4-path changes is
negative until one of the two runs above lands.

Work that does *not* touch the Phase 4 path — WSI (Phase 6), or
documentation — carries no such cost. Phase 5's GPU-side waits do touch
it, through `nvkmd_horizon_ctx_wait`, and are held for the same reason.

Held for the console, unchanged:

**Run `t_vulkan.nro` on a real Switch.** That is Phase 4's exit
criterion and the only thing left in it:

- the sequence runs,
- the CPU-side validation of the written pattern passes,
- no wait-idle was inserted to make it pass — there is none in the test
  and none in `exec()`,
- the console log is pasted into this file.

`scripts/package-horizon.sh` produces the artefact set and the manifest.
Nothing in this environment can produce the log; it is a hardware
measurement and it belongs to whoever holds the console.

Four things are owed alongside it and travel in the same package, none
of which blocks the criterion above:

- `t_threads` and `t_ostime` on hardware rather than on the emulator
  (Phase 3).
- `t_uncached`, D14's measurement, never executed anywhere.
- `t_sysinfo` re-run from a current binary; the 18/19 above was a stale
  `.nro`.

