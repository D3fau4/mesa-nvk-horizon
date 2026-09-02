# GM20B hang snapshot

This note describes diagnostic instrumentation.  It is not a hardware-run
record and none of the differences listed below is, by itself, a diagnosed
cause.

## Enabling it

Set this before creating the Vulkan device:

```text
HORIZON_GPU_HANG_SNAPSHOT=1
```

The option is deliberately off by default.  It adds two GPFIFO entries around
every caller-supplied push.  Each entry is a host semaphore release with
`RELEASE_WFI` enabled, so the resulting run is more serialized than a normal
run.  The marker target and marker command lists live in one CPU-uncached,
GPU-uncached allocation.  Marker command lists are write-once and never wrap;
after 4096 markers the recorder announces exhaustion and later submits run
without markers.

Mesa logs, for the same option:

- the Vulkan queue-submit ordinal and `VkCommandBuffer` object;
- every `nvk_cmd_push` GPU-VA range handed to nvkmd;
- the queried 3D/compute classes and GPC/TPC topology;
- SLM GPU VA, allocation size, bytes per warp, bytes per TPC, MP/TPC and
  maximum warps/MP whenever queue SLM state changes.

`horizon_gpu` logs the same push ranges with the channel, its own submit
ordinal, fence, and the payload values of the before/after markers.  The VA
ranges make the two layers directly correlatable without parsing the command
stream.

## Why the dump does not contain real USERD or registers

The Switch 1 libnx channel path is:

```text
nvChannelCreate("/dev/nvhost-gpu")
NVGPU_AS_IOCTL_BIND_CHANNEL
NVGPU_IOCTL_CHANNEL_ALLOC_GPFIFO_EX2
NVGPU_IOCTL_CHANNEL_ALLOC_OBJ_CTX
NVGPU_IOCTL_CHANNEL_SUBMIT_GPFIFO2 / KICKOFF_PB
```

`NvGpuChannel` retains the nvhost fd, object id, syncpoint fence, and a CPU
staging array of GPFIFO entries.  Neither `ALLOC_GPFIFO_EX2` nor the submit
ioctl returns a USERD mapping, GPFIFO mapping, or hardware channel id.
`NVGPU_IOCTL_CHANNEL_SET_USER_DATA` and `GET_USER_DATA` only store an opaque
64-bit application value and are unrelated.

`NVGPU_IOCTL_CHANNEL_SETUP_BIND` would return `userd_gpu_va`,
`gpfifo_gpu_va`, `usermode_mmio_gpu_va`, and `hw_channel_id`, while taking the
USERD/GPFIFO dma-buf handles and offsets.  That ABI is for Switch 2 and is not
the GM20B/libnx path used here.  A GPU VA also does not make the kernel-owned
USERD CPU-readable by this process.

If a future privileged backend supplies a CPU mapping, GM20B USERD is one
`0x200`-byte channel entry and the required reconstruction is:

```text
PB_GET = *(u32 *)(USERD + 0x44) |
         ((u64)*(u32 *)(USERD + 0x60) << 32)
GP_GET = *(u32 *)(USERD + 0x88)
GP_PUT = *(u32 *)(USERD + 0x8c)
```

CCSR and PBDMA are privileged registers.  A normal Horizon application cannot
open `/dev/nvhost-dbg-gpu` without the `GpuDebug` permission, libnx exposes no
register-ops wrapper for this path, and the legacy channel creation ABI does
not reveal the channel id.  Issuing an undocumented guessed register-ops ABI
from this backend would risk resetting the channel and has therefore not been
implemented.

For a privileged implementation, the GM20B register set to capture is:

| State | Register |
|---|---:|
| CCSR channel instance | `0x00800000 + 8 * chid` |
| CCSR channel state | `0x00800004 + 8 * chid` |
| PBDMA GP_PUT | `0x00040000 + 0x2000 * pbdma` |
| PBDMA GP_GET | `0x00040014 + 0x2000 * pbdma` |
| PBDMA GET / GET_HI | `0x00040018/1c + 0x2000 * pbdma` |
| PBDMA ACQUIRE | `0x00040030 + 0x2000 * pbdma` |
| PBDMA GP_BASE / GP_BASE_HI | `0x00040048/4c + 0x2000 * pbdma` |
| PBDMA GP_FETCH | `0x00040050 + 0x2000 * pbdma` |
| PBDMA PB_FETCH / PB_FETCH_HI | `0x00040054/58 + 0x2000 * pbdma` |
| PBDMA PUT / PUT_HI | `0x0004005c/60 + 0x2000 * pbdma` |
| PBDMA STATUS | `0x00040100 + 0x2000 * pbdma` |
| PBDMA INTR_0 | `0x00040108 + 0x2000 * pbdma` |
| PBDMA CHANNEL | `0x00040120 + 0x2000 * pbdma` |
| FIFO PBDMA assignment/status | `0x00003080 + 4 * pbdma` |

`CCSR_CHANNEL` has status in bits 27:24 and busy in bit 28.  Status values are
`idle`, `pending`, `pending_ctx_reload`, `pending_acquire`,
`pending_acq_ctx_reload`, `on_pbdma`, `on_pbdma_and_eng`, `on_eng`,
`on_eng_pending_acquire`, `on_eng_pending`, and the corresponding context
reload variants through value 14.

## Recovery timing

There is no recovery implementation in this repository.  In nvgpu's
scheduler-error path the kernel identifies a busy engine performing a context
switch, sets `NVGPU_CHANNEL_FIFO_ERROR_IDLE_TIMEOUT`, and immediately invokes
FIFO recovery.  Userland observes the error event afterwards.  Therefore no
ioctl issued from `channel_check_fault` can recover the live pre-reset USERD,
CCSR, or PBDMA state.

The uncached in-stream marker solves a narrower but reliable problem: it was
written before recovery, remains in system memory afterwards, and is captured
before the backend asks for the error record or marks the channel lost.

## Reading a snapshot

Markers bracket a caller push, not arbitrary dwords inside it:

| Last surviving evidence | Meaning |
|---|---|
| breadcrumb `0` | no instrumented push boundary on this channel executed |
| `before-span` for a work push | PBDMA reached the push's preceding boundary; the push did not reach its WFI-ordered completion boundary |
| `after-span` for a work push | all engine work through that complete push finished; inspect `next-not-reached` or the final fence block |
| `before-span`, `kind=acquire` | the GPU entered the host wait list but did not complete it; inspect the waited syncpoint/threshold logs |
| `after-span`, `kind=acquire` | the wait list completed; the acquire is not the blocking point |

If privileged state becomes available, combine it as follows:

- `GP_GET` before the GPFIFO entry and no before marker: frontend/GPFIFO did
  not reach the push.
- `PB_GET` inside the push, `CCSR pending_acquire` or
  `on_eng_pending_acquire`, and an active PBDMA `ACQUIRE`: semaphore or
  syncpoint wait.
- consumed GP entry, `PB_GET` advanced through the push, CCSR `on_eng`/busy,
  and no completion marker: GR accepted work and did not finish it.
- consumed GP entry, `PB_GET` after the push, and an after marker: the push
  completed; look at the next entry or the fence's WFI/L2/syncpoint sequence.
- `PBDMA INTR_0 != 0` or a stalled `PB_FETCH` with GR not busy: investigate
  method fetch, GPFIFO encoding, or PBDMA rather than GR.

Because `RELEASE_WFI` serializes each boundary, a marker can perturb timing.
Run once with it enabled to localize progress and confirm any candidate with a
normal run afterwards.

## B197 and SLM audit

The class path is query-driven end to end:

```text
nvGpuGetCharacteristics().threed_class
 -> horizon_gpu_device_info.threed_class
 -> nv_device_info.cls_eng3d
 -> nvk_cmd_buffer_3d_cls()
 -> SET_OBJECT
```

libnx additionally allocates its 3D object context as `NvClassNumber_3D`,
which is `0xB197`.  On GM20B both paths therefore select `MAXWELL_B`, not
`MAXWELL_A` (`0xB097`).

NVK computes SLM as:

```text
bytes_per_warp = align(slm_bytes_per_lane * 32 + crs_bytes_per_warp, 0x200)
bytes_per_tpc  = align(bytes_per_warp * max_warps_per_mp * mp_per_tpc, 0x8000)
allocation     = align(bytes_per_tpc * tpc_count, 0x20000)
```

For chipset `0x12b`, the shared chipset helpers select SM 5.3, 64 maximum
warps/MP, and one MP/TPC.  Horizon fills `tpc_count` from the queried
`num_gpc * num_tpc_per_gpc`, which is `1 * 2 = 2` on GM20B.  The graphics
queue emits SLM address A/B, allocation size C/D, and bytes-per-warp E;
compute emits the same base and both per-TPC non-throttled/throttled sizes.
Graphics and pre-Volta compute use the `0xff000000` local-memory window, and
the Horizon VA backend reserves the corresponding aperture.

No arithmetic inconsistency is visible in this path.  The new log records the
actual values used by Forward+ instead of changing them.

## Conceptual initialization differences

NVK upstream and this port share `nvk_push_draw_state_init`; Horizon changes
the KMD capabilities and channel setup around it.  Deko3d/NVN uses a separate
Switch-specific state sequence.

| Area | NVK/Horizon | Upstream NVK/nouveau | deko3d/NVN |
|---|---|---|---|
| 3D class | queried `0xB197` | queried from nouveau | `MAXWELL_B` |
| SPA version | no explicit `SET_SPA_VERSION` in NVK | same NVK sequence | explicitly 5.3 for 3D and compute |
| SLM window | `0xff000000` and VA reserved | `0xff000000`; upstream comment says collision is not fully blocked | `0x01000000` |
| SLM backing | grows from shader SLM+CRS demand; full allocation and per-warp/per-TPC programmed | same | queue work buffer, explicitly programmed at initialization |
| ZCULL context | bound on advertised 3D contexts by patch 0060 | kernel/channel-specific | allocated and bound unless disabled |
| ZCULL methods | NVK dynamic allocation/load/store/clear path | same | separate Switch-specific configuration |
| privileged state | `has_priv_reg_writes=false`; NVK skips two MME writes | nouveau advertises true and NVK performs them | several explicit PGRAPH masked writes |
| tiled cache | unused | unused by NVK | private PGRAPH initialization plus tiled-cache methods; feature initially disabled |

Specifically, the two NVK workarounds Horizon omits because
`has_priv_reg_writes=false` clear bit 3 of `gr_gpcs_tpcs_sm_disp_ctrl` (memory
loads in helper invocations) and bit 14 of
`gr_gpcs_tpcs_sms_hww_warp_esr_report_mask` (out-of-range address exceptions).
Deko3d does not express the same pair as an equivalent abstraction: its Switch
sequence has explicit masked writes to `0x00418800`, `0x00419a08`,
`0x00419f78`, `0x00404468`, and `0x00419a04`, in addition to the private tile
cache block.  The fact that the lists differ does not demonstrate that one of
those bits causes the timeout; it identifies privileged state this KMD cannot
reproduce from a normal application.

Deko3d's private tile-cache initialization writes the `0x00418e40/44` selector
registers and `0x00418e58..6c` data registers, then sets tile size/configuration
methods and leaves its unknown feature disabled.  Its absence from NVK is a
state difference, not evidence that the tile cache caused this hang.

The most consequential code-level mismatch in the pre-0060 generated tree was
ZCULL: `has_zcull_info` could be true while every nvkmd context passed a null
channel-create descriptor, whose default was `bind_zcull=false`.  Patch 0060
now binds the context only for advertised 3D contexts.  It must be tested on a
console before treating it as the hang fix.

## What the snapshot found, and what is left

**It was used on 2026-09-02 and it worked.** The full record is in
`docs/MEASURED-ON-HARDWARE.md`; in one line, the breadcrumb plus
`NVK_HORIZON_PUSH_SPLIT=64` (patch 0062) put Godot's Forward+ hang inside
one 67-dword span, and `NVK_DEBUG=push_sync` decoded that span as the
scene shader's colour `DRAW_INDEXED`. The same cube's depth-prepass draw,
earlier in the same push, completes.

The hypotheses this note listed have been answered:

1. **Missing Zcull context — no.** Patch 0060 binds it (`zcull=bound` in
   the log) and the hang is unchanged; `NVK_HORIZON_ZCULL=0` turns the
   whole capability off and the hang is unchanged. Single variable, both
   directions.
2. **Incomplete B197/context initialisation — still open, and now the
   most likely place left.** The final marker is a `before-span` on a
   work push whose only work-issuing method is the draw, which is what
   this note said would put the failure in GR and make SPA and the two
   privileged Maxwell-B workarounds the next controlled comparisons.
3. **Cross-channel acquire — no.** Every marker on the failing submit is
   `kind=work`; no `kind=acquire` span is involved.
4. **SLM backing/programming — no.** `NAK_DEBUG=crsinfo` shows the scene
   fragment shader is the only shader in the process with a non-zero
   `crs_size`, and `NAK_DEBUG=crsbig` gives every shader one: the whole
   frame then runs on shader local memory and dies in the same span.
   `t_vk_crsfrag` passes 105/105 with twelve divergent nesting levels in
   a fragment shader.
5. **GPFIFO/PBDMA fetch — no.** The before-span marker executed, so the
   frontend reached the push and consumed its entry.
6. **Deko3d-only tiled cache — untested, and still the lowest.**

What is not yet distinguished is which property of that one draw does it:
its fragment shader (3932 instructions, 112 GPRs) or the state only it
sets. Of the five pipeline-state differences the 2026-08-23 Forward+ /
Mobile dump found, four are now excluded individually — Zcull, the depth
prepass and its `DEPTH_FUNC EQUAL`, the register count, and
`SET_CT_SELECT.TARGET_COUNT 3` with two `DISABLED` targets (`t_vk_mrt`,
96/96). The RGBA16F colour target is the one that has not been asked on
its own.

## The hypotheses as they were written

## Current hypotheses, in test order

1. **Missing ZCULL context in builds through patch 0058.**  This is the one
   demonstrated initialization inconsistency and it directly affects state
   saved during a graphics context switch.  Re-run with patches 0059/0060;
   compare `NVK_HORIZON_ZCULL=0` if the timeout remains.
2. **Incomplete GM20B-specific B197/context initialization.**  Unlike deko3d,
   NVK does not explicitly program SPA 5.3 and Horizon cannot execute NVK's
   two privileged Maxwell-B workarounds.  This is a concrete state difference,
   not yet a demonstrated defect.  A final `before-span` work marker after
   patch 0060 would place the failure in GR and make SPA/privileged state the
   next controlled comparisons.
3. **Cross-channel acquire/syncpoint wait.**  GPU-side waits exist for upload
   and other foreign-channel fences.  A `kind=acquire` before marker without
   its after marker demonstrates this branch; otherwise it is excluded.
4. **SLM backing/programming mismatch.**  Forward+ is more likely to grow SLM,
   but the audited topology and alignment arithmetic are internally
   consistent.  The actual logged allocation must disagree with demand or
   topology before changing sizes.
5. **GPFIFO/PBDMA fetch or malformed push.**  Alignment, 21-bit length and
   entry-capacity checks are present, larger streams have succeeded, and no
   PBDMA error is reported.  A missing before marker or future PBDMA interrupt
   evidence would raise this hypothesis.
6. **Deko3d-only tiled-cache initialization.**  Lowest without evidence: NVK
   does not use that feature upstream and deko3d initializes it disabled.
