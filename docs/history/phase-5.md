## Phase 5 — step 0a: the shader window, asked instead of assumed (2026-08-04)

**Class: cross build (X).** `t_va_window.nro` is built by both build
paths and has **not been run on a console**. Nothing below is a hardware
result; the arithmetic is read off logs that already exist.

Phase 5 is where a shader runs for the first time, and two of its nine
items (compute dispatch, triangle) are shaders reading through the
aperture that `vkCreateDevice` has been warning about since Phase 4.
The warning is real. The reasoning underneath it was never checked.

### What the pinned tree actually says

The aperture is programmed by NVK, at addresses the hardware fixes:

| | address | where |
|---|---|---|
| `SET_SHADER_SHARED_MEMORY_WINDOW` | `0xfe000000` | `nvk_cmd_dispatch.c:85` |
| `SET_SHADER_LOCAL_MEMORY_WINDOW` | `0xff000000` | `nvk_cmd_dispatch.c:82`, `nvk_cmd_draw.c:583` |

Both are the 32-bit pre-Volta methods (`NVA0C0_SET_SHADER_*`), which is
why the aperture sits inside the low 4 GiB at all. Adjacent, 16 MiB
each: one 32 MiB span `[0xfe000000, 0x100000000)` ending exactly at
4 GiB.

The reservation it collides with is NVK's shader heap:
`nvk_device.c:336` asks `nvk_heap_init` for a **contiguous** heap when
`cls_eng3d < VOLTA_A`, and `nvk_mem_arena_init` answers that with one
`nvkmd_dev_alloc_va` of `NVK_MEM_ARENA_MAX_SIZE` — `1 << 32`,
`nvk_mem_arena.h:17-19` — at device creation.

### The arithmetic 0039 was never held to

`t_va_reserve` on hardware asked for `region_pages + 0x100000` pages and
got `0x4f7fff`, so the small-page region is `0x3f7fff` pages ≈ **15.87
GiB**, starting at `0x8000000`. With the window blocked, **about 12 GiB
of small-page space remains above `0x100000000`** — contiguous, and far
more than the 4 GiB the heap wants.

So patch 0039's failure —

```
horizon_gpu_vm_reserve(0x100000000, 4096, 0x0) failed:
            nv 0x00000f5c   (LibnxNvidiaError_InsufficientMemory)
```

— is **not explained by there being no room**. It is a property of
Horizon's `nvhost-as-gpu` that nobody has measured. The recorded
conclusion ("the window splits the region at precisely the wrong
address and the heap no longer fits below it") assumed the allocator
would only ever look below. Nothing established that.

### What `t_va_window` asks

A `horizon_gpu`-only `.nro` — no Vulkan, no Mesa, so the answer is about
the address space and not about the driver. Seven probes, each releasing
what it took:

| | question |
|---|---|
| P1 | where a 4 GiB non-fixed small-page reservation lands today, and whether it covers the window |
| P2 | whether the aperture itself can be reserved `FixedOffset` |
| P3 | 0039's exact failure, with the window held — measured, with its `nv` code |
| P4/P5 | a 4 GiB reservation placed **fixed** at `0x100000000` and at `0x200000000`, both entirely above the aperture |
| P6 | a 64 KiB fixed reservation at `0x180000000`, to separate "`FixedOffset` does not work up there" from "4 GiB does not fit up there" |
| P7 | heap-fixed-high **first**, then the window — the order the real fix would use, which 0039 did not |

The findings are notes; the checks are the invariants that hold whatever
the findings are. One of them earns its place on its own: **if P3
succeeds, its range must not overlap the reservation P2 is holding.** A
kernel that hands out a range overlapping a live reservation would mean
no reservation in this driver means anything, which is a larger finding
than the window.

### The two strategies it decides between

- **A — block the aperture and put the heap above it.** Needs P7. Removes
  the collision outright.
- **B — leave the aperture alone, keep the detection, and fail loudly if
  anything is ever *bound* inside it.** Always available: it changes no
  reservation, so it cannot break `vkCreateDevice` the way 0039 did.

Worth saying plainly, because it is the reason B is not a consolation
prize: **the collision looks unreachable in practice under either
layout.** The heap's own reservation covers the window, so nothing else
can be given that range; the heap binds 64 KiB chunks from the bottom
and the doubling in `nvk_contiguous_mem_arena_mem_offset_B` puts the
first chunk that reaches `0xfe000000` past 2 GiB of shader code. A
tripwire on bind converts an unreachable hazard into a visible failure
the day it stops being unreachable, and costs nothing.

### One correction to what this file said

The state block claimed `alloc_tiled_mem` "is a NULL function pointer
any tiled image reaches". The NULL pointer is real — `nvkmd.c:111` calls
`dev->ops->alloc_tiled_mem` with no capability check and no assert — but
the reachability was wrong. The `vkAllocateMemory` route needs
`pte_kind != 0 || tile_mode != 0` (`nvk_device_memory.c:228`), and both
stay zero here: they are set only under `image->can_compress`, and
`nvk_image_can_compress` (`nvk_image.c:812`) returns false
unconditionally when `kmd_info.has_compression` is false, which patch
0018 sets. **The reachable caller is the other one**:
`ensure_linear_tiled_shadow_mem_locked` (`nvk_cmd_draw.c:1055`), which
fires when a `VK_IMAGE_TILING_LINEAR` image is used as an attachment —
that is, precisely if a Phase 5 test renders straight into a linear
image to make readback easy. It moves the hazard from item 3 to item 5,
and the NULL pointer has to go regardless.

---

## PHASE 5 COMPLETE — item 6 met, with one failure recorded as unexplained (2026-08-04)

**Class: hardware (HW).** `t_vk_texture` **PASS 1685/1685**. Twenty-four
fresh iterations of the configuration that failed once — sixteen in run
1's exact shape, eight attributable — plus the three characterised
baselines. **Zero failing iterations.**

    53 nearest checks, 4096/4096 pixels each
    27 linear checks, 4096/4096 within 2 (largest deviation 1 of 255,
       and 0 on the 64x64)
    21 source readbacks, every texel of every level as it went in

**Item 6 is met.** Every measurement item 6 requires — the descriptor,
the sampler, addressing, texel fetch, mip selection by explicit LOD, the
filter unit, and the source's own integrity — passes by CPU readback,
across three surface layouts and 24 independent builds of the one that
once failed.

### And the failure that was seen once stays on the record

Batch 5, `t_vk_texture` run 1: level 0 texel rows 4 and 5 of an 8x8
tiled source read as transparent black. Rows 0-3, rows 6-7 and the
whole of mip level 1 were correct. Two independent cases agreed on it
to the byte — case A missed exactly those two texel rows, case C missed
exactly the pixels whose bilinear footprint touched them, and its first
bad pixel is what blending 15/16 of texel (0,3) with 1/16 of a *zero*
texel gives. That is not a misread log. Something returned zeros.

**What has been ruled out, each by a run that would have shown it:**

| candidate | ruled out by |
|---|---|
| the source's usage flags (NIL layout) | run-1 shape has no `TRANSFER_SRC` and passes |
| the readback and barriers added in run 2 | "checked after" has none before sampling and passes |
| the upload versus the texture unit | "checked after" copies the source out afterwards; intact every time |
| surfaces narrower than a GOB | the 64x64 baseline passes, and so does the 8x8 |
| tiling itself | the 8x8 linear baseline passes |
| L2 coherence | closed by construction: `horizon_cmds_fence_incr` emits wait-for-idle, dirty writeback, then the increment |
| a fixed trigger in the call sequence | 24 fresh iterations of it, zero failures |

**What is not ruled out:** something intermittent, or something about
console state that a single binary does not reproduce — run 1 was the
sixth of eleven binaries on a console that had been working for a while.
Allocation order and the addresses that follow from it were not
isolated either, because run 3 made that experiment unnecessary by
ruling out the mechanism it would have tested.

**The decision, recorded as a decision and not an omission.** Phase 5
does not stall on a defect that has resisted 32 attempts and whose
mechanism every available experiment has excluded. What stays behind is
the machinery to diagnose it if it recurs: the source is verified on
the way in, failures report a range rather than a first, the source
pattern names its own position so a wrong texel says which one it was,
and the "checked after" variant distinguishes the upload from the
texture unit without changing the shape of the run. The next occurrence
is diagnosed, not merely seen.

### Phase 5 exit criteria

All nine items verified on hardware by CPU readback of the result, not
by absence of errors. Item 9's additional requirement — two or more
submits in flight with no intervening CPU wait — was met with eight,
demonstrated twice: an unsignalled fence at the instant the last submit
returned, and issuing costing 681 us against 540827 us of execution.

---

## Item 6, run 3: the call sequence is not the trigger (2026-08-04)

**Class: hardware (HW), then cross build (CB).** `t_vk_texture`
**PASS 340/340** — all three variants of the 8x8 tiled source, including
**run 1 shape**: no `TRANSFER_SRC`, no readback, the same sequence of
Vulkan calls that produced 3072/4096 in batch 5.

So the matrix answered, and its answer is the third branch: **neither
candidate is the trigger.**

- not the usage flag: run-1 shape has no `TRANSFER_SRC` and passes;
- not the early readback: "checked after" has none before the sampling
  and passes, and its copy-out afterwards shows the image intact;
- and not L2 coherence, which is closed by construction as well as by
  this run — `horizon_cmds_fence_incr` already emits wait-for-idle, a
  dirty-L2 writeback and then the increment, so a fence means the
  previous submit's writes are in memory before the next submit's
  invalidate prologue runs.

### What is left is the hypothesis a replica cannot test

Run 1 was the sixth of eleven binaries on a console that had been
working for a while. Runs 2 and 3 were one binary on its own. If the
failure is intermittent, reproducing the *shape* of the call sequence
is worth nothing and **repetition** is worth everything.

So the two 8x8 rows are now repeated: the run-1 shape sixteen times and
the attributable variant eight, each iteration building, uploading and
sampling a fresh source image. Any single failing iteration is named in
the log, and for the attributable variant the copy-out afterwards says
whether the image was right all along — that is, whether the texture
unit read something the copy engine did not.

### And if that passes too

Then the honest record is: **a real failure was observed once, with a
signature two independent cases agreed on to the byte, and it has
resisted thirty-two further attempts under the same configuration.**
That is not a conclusion that item 6 works and it is not a conclusion
that it is broken. The attribution machinery stays in the test so that
the next occurrence is diagnosed rather than merely seen, and Phase 5
does not stall on it — the other eight items are met and the exit
criterion for item 6 is satisfied by a test that checks everything item
6 requires.

Gates 4/4, host tests 6/6, 23 `.nro`. Not run yet.

---

## Item 6, run 2: the failure did not reproduce, and that closes nothing (2026-08-04)

**Class: hardware (HW), then cross build (CB).** `t_vk_texture`
**PASS 213/213** — including the same 8x8 tiled, two-level source that
returned 3072/4096 in run 1. Its level 0 and level 1 both came back out
of the image byte for byte, so at that point in that run the upload had
landed correctly, and all four thousand sampled pixels were right.

**Item 6 is not met.** A pass that cannot reproduce a failure it has
already seen does not explain it, and run 2 changed **three things at
once**:

1. `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` was added to the source image —
   usage flags feed NIL's layout choice;
2. a copy-out and two extra barriers now sit between the upload and the
   sampling;
3. the source image is allocated after the render target and the
   buffers instead of before them, so it lands at different addresses.

Both runs report `vkAllocateMemory(image, 0x1000 align 0x20000)`, so
the layout's *size and alignment* did not change; that weakens (1)
without eliminating it, since usage can change a PTE kind without
changing a size.

Declaring the item met here would be the failure mode this repository
keeps guarding against — a check reporting success without having
measured the thing in question. Worse than usual, because a real
hardware failure was observed and would be papered over.

### The experiment, which separates two of the three

The 8x8 tiled source now runs three ways:

| variant | usage | between upload and sampling |
|---|---|---|
| checked first | + TRANSFER_SRC | copy out, check, then sample (run 2) |
| checked after | + TRANSFER_SRC | nothing; copy out and check afterwards |
| run 1 shape | no TRANSFER_SRC | nothing, and no copy out at all |

Between upload and sampling, **checked after** is identical to **run 1
shape** except for the usage flag, and identical to **checked first**
except for the readback. So the next log reads directly:

- *after passes, run-1-shape fails* → the usage flag, i.e. the layout;
- *both fail* → the early readback was masking it, whatever the flags,
  and the "checked after" readback says whether the upload or the
  texture unit is at fault;
- *all three pass* → neither, and what is left uncontrolled is
  allocation order, the addresses that follow from it, and run 1's data
  pattern. That is the next experiment, not this one.

**A limit of the method, recorded because it is real:** a source
without `TRANSFER_SRC` cannot be read back at all, so if that flag is
the trigger, the verification added in run 2 is structurally unable to
see it. That is exactly why "run 1 shape" is in the table despite being
unable to attribute its own failure — the other two rows attribute it.

The 64x64 tiled and 8x8 linear sources stay as baselines; both passed
in run 2 (`largest deviation 0 of 255` on the 64x64 linear filter,
1 of 255 on the two 8x8 ones).

Gates 4/4, host tests 6/6, 23 `.nro`. Not run yet.

---

## HARDWARE BATCH 5 — items 5, 7, 8 and 9 met; item 6 found a real defect (2026-08-04)

**Class: hardware (HW).** Ten of eleven.

| | | |
|---|---|---|
| `t_vulkan` | **PASS 62/62** | the control, tenth consecutive pass |
| `t_gpuwrite` | **PASS 47/47** | `horizon_gpu` only |
| `t_submit` | **PASS 30/30** | the entry queue with two own entries per submit |
| `t_vk_transfer` | **PASS 202/202** | item 1, unchanged |
| `t_vk_compute` | **PASS 37/37** | item 2, unchanged |
| `t_vk_image` | **PASS 72/72** | items 3 and 4 — **without the workaround** |
| `t_vk_triangle` | **PASS 84/84** | **item 5 met** |
| `t_vk_depth` | **PASS 66/66** | **item 7 met** |
| `t_vk_format` | **PASS 282/282** | **item 8 met**, 12 of 12 formats claimed |
| `t_vk_submits` | **PASS 287/287** | **item 9 met** |
| `t_vk_texture` | **FAIL 93/95** | item 6 — two cases, one cause, below |

### Patch 0048 is verified

`t_vk_image` is 72/72 with `warm_shader_heap` gone — the four checks it
used to contribute are the four missing from 76. The first render pass
on a device that has compiled nothing no longer MMU-faults, so the
shader heap's first chunk being bound at `vkCreateDevice` is what run 1
was missing, confirmed by removing the workaround rather than by adding
one.

### Item 5: exactly 1540, and interpolation to the bit

    ok   the coverage model counts 1540 pixels, expected 1540
    ok   A: 1540 pixels hold the triangle colour, expected 1540
    ok   A: 2556 pixels hold the clear colour, expected 2556
    ok   B: 1540 pixels came back opaque, expected 1540
    note B: largest deviation from the computed colour: 0 of 255

The four boundary probes all land where the geometry puts them —
(31,31) covered and (32,31) background, (4,4) covered with (3,4) and
(4,3) background — so the half-pixel argument holds on hardware. And
case B's interpolated colours match the computed barycentrics **with
zero deviation across all 1540 covered pixels**, which is a stronger
result than the tolerance of 2 the test allows.

### Item 9: the numbers

    note calibration: 200000 iterations over 4096 invocations took 1335250 us
    ok   job 0 was still running when all 8 submits had been issued (VK_NOT_READY)
    ok   issuing 8 submits took 681 us against 540827 us of execution
    note 16 empty submits: 1495 us batched, 2018 us one at a time
         (126 us per round trip serialised)

**Eight submits outstanding at one instant, and issuing them cost 1/794
of what running them cost.** Both halves of the criterion, measured.

Three numbers worth keeping, none of them a Phase 5 problem:

- **~85 us of CPU per `vkQueueSubmit`** (681 us for eight). That is the
  dominant per-submit cost — batching sixteen empty submits saved only
  26% precisely because the CPU side does not overlap with itself.
- **126 us per serialised round trip**, L2-invalidate prologue and
  syncpoint epilogue included. The GPU half of a trivial submit is
  therefore about 33 us.
- The spin loop ran at **6.1e8 iterations/s across the whole GPU**,
  which is far slower than 2 SMs at a docked clock would give. Most
  likely the GPU was at its idle clock; possibly NAK's loop is poor.
  Not investigated, recorded.

### Item 6: rows 4 and 5 of an 8x8 source read as zero

Two failures, one cause, and the arithmetic identifies it exactly.

    FAIL A: nearest, implicit LOD: 3072/4096 pixels hold the texel they sampled
    note first wrong pixel (0,32): got 0x00000000, want 0xff148008
    FAIL C: linear, implicit LOD: 2560/4096 pixels are within 2
    note first outside tolerance at (0,28): got 7 92 16 239, want 8 100 17 255

Case A misses 1024 pixels = 16 output rows = **2 source texel rows**,
starting at output row 32, which is texel row 4. Case C misses 1536 =
24 output rows, which is exactly the set of pixels whose bilinear
footprint touches texel rows 4 or 5. And the value confirms it: at
(0,28) the v weight on texel row 4 is 1/16, and blending 15/16 of texel
(0,3) with 1/16 of a **zero** texel gives 8/92/16/239 against the
observed 7/92/16/239 — the one count of difference being R = 7.5, a tie
this hardware rounds down.

So: **level 0 texel rows 4 and 5 read as transparent black. Rows 0-3
and 6-7 are correct, and level 1 is entirely correct.**

In the tiled surface those two rows are image bytes 128..191 — the
third 64-byte group of a Maxwell GOB's swizzle, for a surface 32 bytes
wide (`offset = (y/2)*64 + ((x%32)/16)*32 + (y%2)*16 + (x%16)`). An 8x8
RGBA8 image is half a GOB wide and exactly one GOB tall.

**What the log cannot say, and why that is my defect and not the
driver's.** Whether the upload never landed or the sampler reads the
wrong place. The test built every expected value on the assumption that
the upload worked and never looked at the source. A test that cannot
attribute its own failure has done half its job.

`t_vk_texture` is rewritten for the next run:

- **The source is read back and verified** through
  `vkCmdCopyImageToBuffer` before anything samples it, so the answer is
  attributed: readback wrong means the upload or the layout it wrote
  into, readback right with sampling wrong means the descriptor. If the
  source does not survive, the sampling cases are skipped rather than
  reporting three more failures for one cause.
- **Three sources instead of one**: the 8x8 tiled that failed, a 64x64
  tiled (a full GOB wide, eight tall), and an 8x8 linear. If the fault
  is about surfaces narrower than a GOB, the first fails and the second
  does not; if the linear one passes, tiling is implicated and the
  sampler is not.
- **Failures report a range, not a first**: "wrong from (0,32) to
  (63,47)" identifies a band of rows where "first wrong at (0,32)"
  needed arithmetic afterwards to become one.
- The source pattern now names its own position — `r` identifies `x`
  and `g` identifies `y`, since multiplying by an odd number is a
  bijection mod 256 — so a sampler reading the wrong texel says **which
  one** it read.

Not run yet. Gates 4/4, host tests 6/6, 23 `.nro`.

---

## Items 5 to 9 written; the whole phase is now measurable (2026-08-04)

**Class: cross build (CB).** Five new tests, none of them run on
hardware yet. They go out as batch 5 together with the four that
already pass, because `t_vk_image` now depends on patch 0048 and has to
be re-measured.

| Test | Item | What it measures |
|---|---|---|
| `t_vk_triangle` | 5 | the first draw call: vertex processing, rasterisation, the ROP |
| `t_vk_texture` | 6 | descriptors, samplers, mip levels, and the filter unit |
| `t_vk_depth` | 7 | the depth test, and the depth buffer read back as a value |
| `t_vk_format` | 8 | twelve colour formats against the bytes their encodings require |
| `t_vk_submits` | 9 | several submits in flight at once, twice, in two currencies |

**What makes each of them a measurement rather than a smoke test.**

*Item 5.* The triangle is stated in pixels — (4,4), (59.5,4), (4,59.5)
— with its three edges half a pixel away from every sample point, so
coverage is decided by geometry and not by the top-left fill rule:
exactly **1540** of 4096 pixels. The test evaluates the same edge
functions the rasteriser must and checks its own model against that
count *before* asking the GPU anything, then compares every pixel
positionally. Case B draws the same triangle from a vertex buffer with
interpolated colours and a dynamic viewport, so a failure in B alone
names attribute fetch, interpolation or dynamic state.

*Item 6.* The full-target triangle makes a pixel centre sample at texel
coordinate `(px+0.5)/8`, never within a sixteenth of a texel boundary,
so the nearest-filtered cases are **exact**: output pixel `(px,py)`
holds source texel `(px/8, py/8)` at level 0 and `(px/16, py/16)` at
level 1. The linear case's expected values are computed from the same
four texels the filter unit weighs; the fractional position is always
an odd sixteenth, so the weights are exact in any subtexel format with
four or more bits and only the final round to UNORM8 is inexact —
tolerance 2 of 255, with the largest deviation actually seen reported
either way.

*Item 7.* Four draws, one render pass, differing only in twenty bytes
of push constant and a scissor rectangle. Draw 3 is behind everything
and covers everything, so a degenerate depth test paints the whole
target rather than failing subtly. Draw 4 passes the test with
`depthWriteEnable` off, which is the case that separates the two halves
of the depth state: **a driver that ignored it passes every colour
check and fails on one depth value.** D32_SFLOAT, so 0.75 and 0.25 are
exact and the depth comparison is an equality.

*Item 8.* Each format entry carries the value the shader writes and the
texel that must result, with the conversion derived in its comment
rather than computed from a format-description table — a table would be
another thing that can be wrong in the same direction as the driver.
The load op is `DONT_CARE`, not `CLEAR`: the draw covers every texel, so
a clear would give the attachment a second writer and a correct texel
could have come from either. A format the driver does not claim is
skipped with its `optimalTilingFeatures` printed, and **how many of the
twelve were claimed is reported** — "twelve formats passed" and "the
ones this chip supports passed" are different statements.

*Item 9.* Eight calibrated compute jobs submitted back to back with
nothing between them; the first job's fence is read the instant the
eighth `vkQueueSubmit` returns and must be `VK_NOT_READY`. `VK_SUCCESS`
there is reported as a **failure**, because it would mean the
measurement never got to observe what it came for. The second currency
is wall clock: sixteen empty submits batched against the same sixteen
serialised, which synchronous submission would make equal. The
serialised figure divided by sixteen is also the first measurement of
this project's per-submit overhead, L2-invalidate prologue and
syncpoint epilogue included.

**Two things worth recording about how these were built.**

`vkfw_gfx_create` now holds the graphics-pipeline boilerplate that
items 5 to 8 all need, with the state it fixes justified where it is
set (cull mode NONE because winding in framebuffer space depends on
Vulkan's y-down NDC and a culled triangle comes back as the clear
colour, which says nothing about what is being measured). `t_vk_triangle`
was moved onto it in the same commit that introduced it, so the helper
never existed without a caller.

And nothing prints inside a timed region in `t_vk_submits`: `t_check`
writes to the SD card, so a check inside the loop being timed would
have been measuring the SD card. Every timed loop calls Vulkan directly
and stores its results in an array.

**Batch 5 is eleven binaries.** Nine Vulkan tests and the two
`horizon_gpu` ones that cover the submit path item 9 leans on.

Gates 4/4, host tests 6/6, cross build 23 `.nro`, series of **48**
patches applying from a reset `mesa/` and idempotent on the second run.

---

## Patch 0048 — the shader heap's first chunk, and the workaround comes out (2026-08-04)

**Class: cross build (CB).** Not verified on hardware yet; it goes out
with the next batch.

Items 3 and 4 were met in batch 4, but with a caveat that was written
down every time they were reported: `t_vk_image` passed **because the
test compiled a shader it never used**. That is a workaround living in a
test for a defect in the driver, and it hides the defect from every
other test that will ever render.

**What the defect is.** Pre-Volta, `SET_PROGRAM_REGION` is programmed
once, from the queue's init push buffer, with
`nvk_heap_contiguous_base_address(&dev->shader_heap)`. That heap is
contiguous on these chips: `nvk_mem_arena_init` reserves the whole 4 GiB
up front and binds **nothing** until the first upload. So between
`vkCreateDevice` and the first compiled shader, the address the engines
have been given is a valid VA that maps nothing. The instruction cache
pre-fetches — NVK's own comment at `nvk_device.c:337` says so, which is
why shader BOs are overallocated by 2 KiB — and a pre-fetch from that
base is a read of unmapped address space. nvgpu faults on it.

**The evidence is a controlled experiment, not a reading.** Run 1:
clear-only command buffer, fresh device, MMU fault, nothing written.
Run 3: the same test with a throwaway compute pipeline compiled first
and nothing else changed, PASS 76/76. `t_vk_compute`, which compiles a
shader before anything else, never faulted on any run.

**The fix** is `nvk_heap_ensure_first_chunk()`, called from
`nvk_device.c` under the same `cls_eng3d < VOLTA_A` that decides the
heap is contiguous two lines above. It costs one 64 KiB allocation
(`NVK_MEM_ARENA_MIN_SIZE`) on a device that was going to make it as soon
as anything was compiled.

**And the test now proves it.** `warm_shader_heap()` is gone, and so is
`t_vk_image`'s `#include "comp_write_id.spv.h"` and its entry in
`nvk_test_shaders`. The test reaches its first render pass having
created no shader module, no pipeline and no descriptor set layout —
that last one matters, because `nvk_descriptor_set_layout.c:399` also
uploads to the shader heap, so creating one would have bound the chunk
just as effectively and just as invisibly.

**What was not measuring this.** Nothing stopped anyone from putting an
`#include "…spv.h"` back into `t_vk_image`: every generated header lands
in one build directory, so a test can include a header meson did not
give it and still compile, as long as some other test in the same build
asked for that shader. `check-mesa-test-parity.sh` grew a seventh
comparison — per test, `nvk_test_shaders` against the test's own
`#include` lines — and it was broken three ways to prove it fails: a
test including a shader meson does not list, meson listing a shader the
test does not include, and the extraction itself returning nothing. All
three fail; restored, it passes.

Gates 4/4, host tests 6/6, cross build 18 `.nro`, series **48** patches
applying from a reset `mesa/` and idempotent on the second run.

---

## HARDWARE BATCH 4 — the L2 fix holds; items 1, 2, 3 and 4 are met (2026-08-04)

**Class: hardware (HW).** Six for six, logs in `docs/hw-logs/*-run4-l2fix-PASS.log`:

| | | |
|---|---|---|
| `t_gpuwrite` | **PASS 47/47** | `horizon_gpu` only. The submit path with the new prologue |
| `t_submit` | **PASS 30/30** | the entry queue and back-pressure, now two own entries per submit |
| `t_vulkan` | **PASS 62/62** | the control, ninth consecutive pass |
| `t_vk_compute` | **PASS 37/37** | item 2, plus the two new poison checks |
| `t_vk_transfer` | **PASS 202/202** | **item 1 met** |
| `t_vk_image` | **PASS 76/76** | items 3 and 4 |

The transfer result is the one that was in doubt:

```
ok   B words before the region untouched: 257/257 words are 0xdeadbeef
ok   B words after  the region untouched: 3226/3226 words are 0xdeadbeef
ok   F [4, 260)   ok   F [8, 264)   ok   F [16, 272)   ok   F [0, 4)   ok   F [0, 12)
note F: 0 of 9 probes wrote outside their region
```

Nine regions, five of which used to spill, all exact. One
`L2_SYSMEM_INVALIDATE` before the work was the whole fix, and nothing
regressed: `t_gpuwrite` and `t_submit` — the two tests that measure the
submit path directly, with no driver in them — pass unchanged.

**Items 1, 2, 3 and 4 of Phase 5 are met on hardware by CPU readback.**
Items 3 and 4 still lean on the test uploading a shader the driver
should upload itself; that is the next driver change, not a hardware
question.

One correction landed with this: probe F's summary line still called a
spill "the copy engine's transfer granularity" — the explanation that
was withdrawn two runs ago — and would have printed it beside "0 of 9".
A passing log carrying a wrong explanation is worse than a failing one.

---

## HARDWARE BATCH 3 — items 3 and 4 met, and the L2 was only half coherent (2026-08-04)

**Class: hardware (HW).** Logs: `t_vk_image-run3-PASS.log`,
`t_vk_transfer-run3-FAIL.log`.

### Items 3 and 4 are met, and the hypothesis is confirmed

`t_vk_image` **PASS 76/76**. Every clear landed, verified texel by texel:

```
ok  optimal 64x64: 4096/4096 words are 0x78563412 (all of them)
ok  optimal 67x53: 3551/3551 words are 0x78563412 (all of them)
ok  optimal 64x64, two layers: 4096/4096 words are 0x78563412
ok  layer 1 holds the second colour: 4096/4096 words are 0xf0debc9a
ok  the readback buffer past the image is untouched
```

67 × 53 = 3551, so the non-power-of-two extent came back exactly, with
no stride padding leaking into the readback. Layer 0 kept the first
colour while layer 1 took the second, so the clear respects its
subresource range.

**The only difference from run 1 is the warm-up shader.** The 3D engine
MMU-faults when it begins a render pass with `SET_PROGRAM_REGION`
pointing into NVK's shader heap while that heap has nothing bound — and
does not fault once a single shader has been uploaded. That is now
measured, not hypothesised.

So items 3 and 4 pass **with a workaround in the test**, which is not
the same as passing. The driver fix belongs in NVK — the shader heap's
first chunk must exist before the 3D engine can be given a program
region — and until it does, `t_vk_image` states in its own log that it
compiled a shader it never used.

`alloc_tiled_mem` remains unexercised: a linear image still cannot be a
colour attachment (`FORMAT_NOT_SUPPORTED`).

### The transfer failure is ours, and it is coherence, not the copy engine

The poison verification passed every time — `the poison reached memory
(4096/4096 words)` before each case — and case B still failed while
probe F's identical region passed. That killed the stale-poison
explanation and left the numbers, which turn out to fit one model
exactly:

| probe | previous submit had touched | result |
|---|---|---|
| `[0, 4)` | `[0, 32)` | 28 bytes after — one 32-byte line minus the four written |
| `[0, 12)` | `[0, 32)` | 20 bytes after — again to the 32-byte boundary |
| `[4, 260)` | `[0, 256)` | 4 bytes before |
| `[8, 264)` | `[0, 260)` | 8 bytes before |
| `[16, 272)` | `[0, 264)` | 16 bytes before |
| `[32, 288)` | `[0, 272)` | exact — both ends 32-byte aligned |
| `[64, 324)` | `[0, 32)` | exact — the tail line was not resident |
| `[1028, 3480)` | `[64, 324)` | exact — neither edge line was resident |
| case B, same region | case A, the **whole** buffer | both edges wrong |

**Every spill is the distance back to a 32-byte boundary, and it happens
exactly when the previous submit had touched that line.** The model:
`horizon_cmds_fence_incr` writes dirty L2 back after work, so a GPU
write reaches memory — but the line stays resident, *clean*. A later CPU
write updates memory and leaves that line alone. When the GPU next
writes part of that line it merges into its own stale copy instead of
fetching memory, and the writeback then sends the whole line back,
silently reverting bytes the caller never asked to be written.

The copy engine is exact. **Only half of the coherence was ever done**:
dirty lines out after work, nothing invalidated before it. Every submit
now begins with one `L2_SYSMEM_INVALIDATE` — the constant was already
named in `cmds.h` and `horizon_cmds_mem_op` could already emit it;
nothing had ever put one in a submit.

It also puts this GPU's L2 line at **32 bytes**, from eleven data points
that agree.

**Not verified yet.** The fix is built and has not run on a console.

---

## HARDWARE BATCH 2 — the window fix holds, and two of my own defects (2026-08-04)

**Class: hardware (HW).** Logs: `t_vulkan-run8-window-blocked-PASS.log`,
`t_vk_compute-run2-window-blocked-PASS.log`,
`t_vk_transfer-run2-FAIL.log`, `t_vk_image-run2-CRASH.log`.

| | result | |
|---|---|---|
| `t_vulkan` | **PASS 62/62** | unchanged with the window blocked and the fence fix |
| `t_vk_compute` | **PASS 35/35** | unchanged. Item 2 stays met |
| `t_vk_transfer` | FAIL 167/174 | probe F contradicts case B — see below |
| `t_vk_image` | **CRASHED THE CONSOLE** | my bug, not the driver's |

### Patch 0047 is verified on hardware

The overlap message appeared once per log in batch 1 and **appears zero
times in all four logs of batch 2**, while `vkCreateDevice` succeeds in
every one of them and both passing tests are bit-for-bit as good as
before. The shader local/shared window is now reserved and no
reservation lands on it. Strategy A works; the emulator's answer was the
only thing that ever said otherwise.

### The console crash is mine

`t_vk_image` stops dead after

```
ok   warm-up: vkCreateShaderModule -> VK_SUCCESS
ok   warm-up: vkCreatePipelineLayout -> VK_SUCCESS
```

with no further output and no `RESULT` line. The next call is
`vkCreateComputePipelines`, and the warm-up handed it a pipeline layout
with **no descriptor sets** while `comp_write_id` declares
`DescriptorSet 0, Binding 0`. That is invalid usage
(`VUID-VkComputePipelineCreateInfo-layout-07987`); there are no
validation layers here to say so, and NVK resolves the binding against a
set that does not exist.

The comment that justified it said: "the shader declares a storage
buffer, but nothing is dispatched, and an empty pipeline layout is enough
to compile and upload." **That was an assumption about the driver
written as a fact, in the same commit that fixed two other instances of
exactly that mistake.** Fixed: the warm-up now builds the descriptor set
layout the shader actually declares. It still dispatches nothing, so the
layout costs nothing — it just has to be true.

**The MMU-fault experiment therefore never ran.** Items 3 and 4 are
exactly where they were.

### The transfer result withdraws a conclusion instead of confirming one

Probe F's last entry is case B's region, byte for byte, in the same
process — and the two disagree:

```
FAIL B words before the region untouched: first mismatch at word 256
FAIL B words after  the region untouched: 2 wrong
ok   F [1028, 3480): exactly case B, so the two must agree
```

A copy engine does not answer one request two ways. So at least one of
them was measuring something other than the copy, and the only
assumption underneath both is that **the poison reached memory before
the GPU ran**. Nothing checked it. The batch-1 conclusion drawn from
case B alone — "the copy engine's transfer granularity is 16 bytes" — is
**withdrawn**; it rested on one unverified data point, and the other F
results do not fit it either (a start of 16 rounding down to 0, a
4-byte copy touching 32 bytes, and a 32-byte start landing exactly).

`vkfw_buffer_poison` now writes, flushes, **invalidates and reads back**,
and fails loudly if a single word of poison did not survive. Every
readback in this suite rests on that step and nothing was measuring it —
the same shape as the fence that reported success without consulting the
notifier, one level further down, and found the same way: by two
measurements of one thing disagreeing.

---

## FIRST HARDWARE BATCH OF PHASE 5 — item 2 met, three real defects (2026-08-04)

**Class: hardware (HW).** Five `.nro` from `build/pkg`, run by the owner
on a real console. Logs in `docs/hw-logs/`:
`t_vulkan-run7-phase5-batch1-PASS.log`, `t_va_window-run1-PASS.log`,
`t_vk_compute-run1-PASS.log`, `t_vk_transfer-run1-FAIL.log`,
`t_vk_image-run1-FAIL.log`.

| | result | |
|---|---|---|
| `t_vulkan` | **PASS 62/62** | the revalidation. Everything committed after run 6 works, including the sub-chunk D16 check |
| `t_va_window` | **PASS 22/22** | the probe answered, and the answer overturns a recorded conclusion |
| `t_vk_compute` | **PASS 35/35** | **item 2 met** |
| `t_vk_transfer` | FAIL 82/84 | item 1, one real defect |
| `t_vk_image` | FAIL 43/46 | items 3 and 4 blocked, two real defects |

### ITEM 2 IS MET: a shader compiled by NAK ran on this GPU

```
ok   vkCreateShaderModule(584 bytes) -> VK_SUCCESS
ok   vkCreateComputePipelines -> VK_SUCCESS  (NAK compiled a shader)
ok   the dispatch computed every word: 4096/4096 words match
ok   nothing was written past the last invocation: 64/64 words are 0xdeadbeef
```

4096 invocations, each computing `(id * 2654435769) ^ 0xa5c3f00d`, every
word different from every other, every one of them right, and the 64
words past the last invocation still holding their poison. **This is the
first machine code NAK has produced that has ever executed**, and it is
verified by CPU readback rather than by absence of errors — the phase's
exit criterion, for this item.

### The window: the emulator was wrong, and this file recorded its answer as fact

`t_va_window` P3 asked the exact question patch 0039 failed on, on real
hardware, and got the opposite answer:

```
note small-page region [0x8000000, 0x3fffff000) page=0x1000
note P2 window fixed @0xfe000000: ok size=0x2000000
note P3 heap 4 GiB non-fixed, window held: ok base=0x100000000 clears the window
note P4 heap 4 GiB fixed @0x100000000: ok
note P7 heap first, fixed @0x100000000: ok  /  then window fixed: ok
note VERDICT strategy A (block the window, place the heap above it) is VIABLE
```

Patch 0040 backed the block-off out because the 4 GiB reservation failed
with `InsufficientMemory`, and recorded the reason as "the window splits
the small-page region at precisely the wrong address and the heap no
longer fits below it". **That measurement was on the emulator**, and the
explanation was a fact about the hardware that nobody had asked the
hardware. The real allocator places the reservation *above* the hole;
about 12 GiB of small-page space sits up there. Same shape as the
syncpoint failure of 2026-07-28.

Patch 0047 blocks the window off again, on the pdev this time rather
than the dev, because the address space belongs to the pdev (D9).

### `vkWaitForFences` returned VK_SUCCESS for work that MMU-faulted

The worst of the three, and the one that made the others hard to read.

`t_vk_image` case 1 submitted, waited, and was told the work was done.
The readback buffer still held its poison, every word of it. The fault
appeared one case later, at the *next* kickoff:

```
[horizon_gpu:E] Kickoff failed: 0x00000d5c (notifier: 31 'MMU fault')
```

The syncpoint had genuinely reached the threshold, because **nvgpu's
channel recovery force-increments a faulted channel's syncpoints** to
their maximum submitted value so waiters do not hang. The counter
therefore says "finished" for work that never ran.

`horizon_gpu_channel_wait_fence` did check the error notifier — but
*after* the two success returns, never before them:

```c
if (horizon_gpu_syncpt_reached(hw, fence.threshold))
    return horizon_gpu_ok();          /* <- never looked */
if (channel_check_fault(chan))
    return horizon_gpu_err(HORIZON_GPU_ERR_CHANNEL_LOST);
```

Both success paths now go through `channel_reached_or_lost`, which
consults the notifier first. Deliberately stricter than "did this fence
complete before the fault": the notifier is sticky and per-channel, so
work that genuinely finished before a later fault is also reported lost.
That is the safe direction; the unsafe one is what was measured.

**This is a `horizon/` change, which CLAUDE.md permits only for a real
and measured bug. It is both.**

### Items 3 and 4: the first render pass MMU-faults

The clear never happened, on a plain 64×64 two-layer OPTIMAL image. What
the map dump printed alongside the fault is the lead:

```
map VA 0xa14a000+0x100000000 page 0x1000  (nothing bound)
```

That is NVK's shader heap: 4 GiB of contiguous VA reserved at device
creation, first chunk bound only when a shader is first uploaded, and
`SET_PROGRAM_REGION` points at its base. **`t_vk_image` compiled no
shader; `t_vk_compute`, which did, ran on the same console without
faulting.** So the hypothesis is that the 3D engine begins a render pass
with its program region pointing at unbacked address space.

`t_vk_image` is now the experiment: it creates a throwaway compute
pipeline before the first clear, purely so NVK uploads something to the
shader heap, and says so in its own log. If the clears pass, that was
why — and the fix then belongs in the driver, not in a test. The cases
are also reordered simplest-first; run 1 began with the two-layer image,
which confounded layered rendering with the first render pass of any
kind.

Also learned, and unhelpful: **a linear image cannot be a colour
attachment here** — `vkGetPhysicalDeviceImageFormatProperties` answers
`FORMAT_NOT_SUPPORTED`. The linear-tiled-shadow path, and with it
`alloc_tiled_mem`, is therefore *still* unexercised on hardware.

### `vkCmdCopyBuffer` writes outside its region

`t_vk_transfer` asked for bytes `[1028, 3480)` and the bytes that changed
were `[1024, 3488)`: start rounded down, end rounded up, both to a
multiple of 16. The two extra words at each end held the *source's*
pattern, so the copy genuinely transferred them.

NVK does no rounding — `nvk_CmdCopyBuffer2` programs `OFFSET_IN`,
`OFFSET_OUT` and `LINE_LENGTH_IN` with the exact byte address and length
(`nvk_cmd_copy.c:373-415`) — so this is the NV90B5 copy engine's
transfer granularity, and Vulkan puts no alignment requirement on
`VkBufferCopy`. Everything else in the test passed, including the
buffer→image→buffer round trip in **both** tilings, so block-linear
surface interpretation by the copy engine is correct.

`t_vk_transfer` gains probe F, which measures the granularity instead of
inferring it from one data point: nine regions, and for each the first
and last byte that actually changed. That decides whether the fix is
"NVK must handle the unaligned head and tail another way" or "this
engine cannot be asked for unaligned regions at all".

---

## Phase 5 — items 2, 3 and 4 have tests, and the first batch is ready (2026-08-04)

**Class: cross build (X).** 18 `.nro` from the Meson path, packaged with
a manifest. **None of this has run on a console**, which is the whole
point of the batch below.

### `t_vk_image` — items 3 and 4

One test for two items, because an off-screen image is not observable on
its own: creating one and binding memory produces nothing a readback can
look at, and writing to it with a copy is item 1. What makes an image
*off-screen* is being rendered to, and the smallest thing that renders
to one is a clear — which in NVK is not a blit and not a shader but a
render pass (`nvk_cmd_clear.c:298-325` builds a `VkRenderingInfo` with
`LOAD_OP_CLEAR` and calls `nvk_CmdBeginRendering`). **So this is the
first time the 3D engine is asked to bind a render target on this
platform, and it asks without a shader in the picture** — which is why
it goes before item 2 rather than after.

Three cases, each for something a single case would let through:

| case | what only it can catch |
|---|---|
| optimal 64×64, two layers | a clear that ignores its subresource range — layer 1 gets a second colour and layer 0 must still hold the first |
| optimal 67×53 | a stride bug. An all-powers-of-two extent has no padding for NIL to get wrong |
| linear 64×64 | the linear-tiled shadow, i.e. `nvkmd_dev_alloc_tiled_mem` — the NULL pointer patch 0045 replaced. **This case is what executes it** |

Plus, on every case, the words past the last texel must still hold their
poison: a copy that used the image's padded stride instead of the packed
one would fill the region *and* spill past it.

### `t_vk_compute` — item 2, and the real unknown

Two failure points nothing before it could have:
`vkCreateComputePipelines`, where NAK compiles, and `vkCmdDispatch`,
where the result runs. They are separate check lines on purpose — a
pipeline that fails to create is a compiler problem and says so before
anything is submitted.

The shader computes `out[id] = (id * 2654435769) ^ 0xa5c3f00d`. Not a
constant, because a dispatch that ran and computed nothing would still
fill a buffer with a constant; every word differs from every other, so a
shader that wrote the right value at the wrong index fails on the index.
The buffer is 64 words longer than the dispatch and the shader has no
bounds test of its own, so the tail catches a dispatch of the wrong size
or a local size that disagrees with `OpExecutionMode`.

It touches no local and no shared memory, deliberately: the shader
local/shared window stays out of the first shader ever executed here, so
a failure has one fewer possible cause.

The dispatch ends with a barrier to `VK_PIPELINE_STAGE_HOST_BIT` /
`VK_ACCESS_HOST_READ_BIT`. `horizon_gpu`'s fence increment flushes dirty
L2 on every submit, so the readback would very likely work without it —
which is exactly why it is there. The test should measure the dispatch,
not that one unconditional writeback covers for a missing dependency.

### The first hardware batch

Five `.nro`, in this order, because each one's failure would explain the
next one's:

1. **`t_vulkan`** — revalidation. Everything committed after
   `t_vulkan-run6` has never run: the second review round's fixes and the
   sub-chunk D16 check. It is also the control: if this no longer passes,
   nothing below it means anything.
2. **`t_va_window`** — the allocator probe. Decides the window strategy.
3. **`t_vk_transfer`** — item 1.
4. **`t_vk_image`** — items 3 and 4, and the first render-target bind.
5. **`t_vk_compute`** — item 2, and the first shader.

---

## Phase 5 — step 0c: the fixture, the shaders, and a bring-up that only worked by hand (2026-08-04)

**Class: cross build (X).** 16 `.nro` from the Meson path, 14 from the
Makefile path, all gates green. Nothing run on a console.

### `tests/common/vkfw`

Seven Phase 5 binaries need the same forty lines before they can ask
anything: no loader, so entry points come through
`vk_icdGetInstanceProcAddr`; a debug messenger, because a release build
drops every `vk_errorf` unless one is registered; the non-conformance
opt-in; a queue; a pool. `vkfw` is that, once, with the dispatch table
inside the fixture rather than at file scope.

**What it deliberately does not do is wrap submission.** Item 9 has to
put two submits in flight with no CPU wait between them, so a
submit-and-wait helper would be the one thing the exit criterion
forbids. The pieces are separate — `vkfw_cmd_begin`,
`vkfw_cmd_end_submit`, `vkfw_submit_and_wait` — and only the last one
waits.

`vkfw_buffer_poison` is Phase 4's lesson made structural: fill the
destination with something the GPU will not write, **and flush it**, so
a later match can only have come from the GPU. Poison left in a dirty
cache line is not poison in memory, and that failure would read back as
success.

### Shaders as SPIR-V assembly

`tests/shaders/*.spvasm` → `spirv-as` → `spirv-val` → a C array, by
`scripts/spv-embed.py`. The tools are already in the derived image,
which builds SPIRV-Tools with `SPIRV_SKIP_EXECUTABLES=OFF` for
`mesa_clc`; the container has no network and the base image has no
glslang, so this costs nothing and pins nothing new.

Validation is a build failure, not a warning: NAK has never compiled a
shader that then ran, so when the first one misbehaves the question will
be "driver or shader", and `spirv-val` answers half of it before the
`.nro` exists. Broken both ways to check the step can fail — a dropped
operand is rejected by `spirv-as`, a missing `Block` decoration by
`spirv-val` (`VUID-StandaloneSpirv-OpTypeRuntimeArray-04680`).

### The bring-up only ever worked because someone did it by hand

Following the documented sequence on a clean tree,
`scripts/build-mesa-nvk.sh` reports success and **six of the seventeen
archives the tests link do not exist**: `libnir.a`, `libvtn.a`,
`libcompiler.a`, `libvulkan_util.a`, `libvulkan_wsi.a`,
`libxmlconfig.a`. Mesa marks an internal static library
`build_by_default` only where something in the configuration links it,
and this configuration links nothing — it produces archives for a test
in another build directory. `meson compile` with no arguments therefore
stops after `libnvk.a` and the Rust runtime.

`meson.build` then asks `fs.exists()` over all seventeen, answers "no",
and leaves `t_vulkan` out of `build.ninja` with no message anyone would
see. The Makefile path does not build the NVK tests at all, so nothing
contradicted it.

**And the check that was supposed to notice could not.**
`HORIZON_NVK_TEST_LIBS` held two archives with a comment calling them
"the pair whose absence means that answer will be no" — true, and the
wrong direction, because callers use `horizon_nvk_libs_present()` to
predict `meson.build`'s answer and their *presence* proved nothing.
Measured here: the two sentinels present, six others absent, the
function said yes, `meson.build` said no.

Fixed in three places: the list becomes all seventeen;
`build-mesa-nvk.sh` builds them (through a new `horizon_ninja`, because
`meson compile src/compiler/nir/libnir.a` answers "target not found"
while ninja takes the path verbatim); and
`check-mesa-test-parity.sh` gains a sixth comparison so the two copies
of the list cannot drift. Broken three ways to check it can fail —
an entry dropped from one side, an entry added to the other, and the
extraction itself breaking, which reports "extracted nothing" rather
than agreement.

`check-dispatch-complete.sh` had the same shape of blind spot: its
default was `t_vulkan.elf`, and the NULL-dispatch hole it exists to
catch is a property of one link line. With six more tests coming, it now
defaults to every driver-linking ELF in the build directory. Verified in
both directions: a non-driver ELF fails the check, and a failure inside
the loop propagates.

### Item 1's test exists

`t_vk_transfer` — five copies, because they are different machinery:
whole buffer→buffer; a region, with the words outside it checked to
still hold their poison, which is the only way to catch a copy that
ignores its bounds; `vkCmdUpdateBuffer`, where the data rides in the
command stream; and the buffer→image→buffer round trip in both tilings.
The optimal-tiling round trip is the first thing this project asks the
GPU to interpret a surface layout for, and it is self-checking: whatever
the layout is, reading it back the same way must return what went in.

---

## Phase 5 — step 0b: the NULL op, and the two more like it (2026-08-04)

**Class: cross build (X).** `libnvk.a` builds with both patches and
carries `nvkmd_horizon_alloc_tiled_mem`; `check-tls-relocs` reports 0
TLS relocations. Nothing here has run on a console.

`alloc_tiled_mem` was the second of the three known mines. Two things
came out of fixing it, and the second is the larger one.

### The op, implemented rather than stubbed

It is implementable here and cheaply, because this platform already
made the decision the op needs: **a block-linear layout is a property of
the GPU mapping, not of the memory object** (memory-model § 1 #9), and
`alloc_mem` already reserves and binds a VA for everything it creates.
So the two entry points differ in exactly one value — the PTE kind that
VA carries — and the body becomes one function taking the kind.

That kind also settles the hazard `nvkmd_horizon_alloc_va` had written
down and handed on: a non-pitch kind sends the reservation to the
big-page half, `horizon_gpu_vm_map` rounds a bind's size up to the
reservation's page size and then requires the rounded range to fit the
memory object, so an object that is not a whole number of big pages
cannot be bound whole. The allocation is where that is fixable, and
raising the alignment to `bind_align_B` is the whole fix, since
`horizon_gpu_mem_create` rounds the size up to the alignment.

`tile_mode` is not consumed. In the nouveau backend it is a field of the
kernel's BO; Horizon has no such field, and everything needed to read a
block-linear surface comes from the PTE kind or from the descriptor NIL
writes. Written down in the code rather than dropped silently.

`has_alloc_tiled` becomes true. Its one other consumer is
`EXT_image_drm_format_modifier`, which therefore joins the advertised
set — nothing enables it here, and an advertised extension costs nothing
until an application asks for it.

### The same class, twice more

The comment this backend carried said the NULL entries were safe because
"nvkmd.h's inline wrappers assert on that before dispatching". **They do
not.** `nvkmd_dev_alloc_tiled_mem` is out-of-line in `nvkmd.c:111-118`
and calls through the pointer with no check and no assert;
`nvkmd_dev_import_dma_buf` (`nvkmd.c:154-160`) is the same. The claim
had never been checked.

Checking it found two more live instances:

| NULL op | reached through | advertised? |
|---|---|---|
| `alloc_tiled_mem` | `ensure_linear_tiled_shadow_mem_locked`, `nvk_cmd_draw.c:1055` | no extension needed at all |
| `import_dma_buf` / `export_dma_buf` | `VkImportMemoryFdInfoKHR`, `vkGetMemoryFdKHR` | `KHR_external_memory_fd`, `EXT_external_memory_dma_buf` — **flat `true`** |
| `overmap` | `vkUnmapMemory2KHR` with `VK_MEMORY_UNMAP_RESERVE_BIT_EXT` | `EXT_map_memory_placed` — **flat `true`** |

`nvk_get_device_extensions` already had the right pattern —
`EXT_image_drm_format_modifier` follows `kmd_info.has_alloc_tiled` — and
these three were simply left unconditional, which is correct on nouveau
where every flag is true and is a way to advertise an unimplementable
extension anywhere else. Patch 0046 makes them follow the same flags.
Upstreamable as written: nothing changes for nouveau.

### And the argument for not leaving a NULL

The original comment ended "a caller that ignored the capability faults
here rather than silently getting a wrong allocation", which is a real
preference — loud beats wrong. It does not hold on this platform. A jump
through a NULL function pointer on a Switch is not loud: there is no
debugger, the `.nro` dies with no log line, and the last thing written
to the SD card is whatever check ran before it. A `VkResult` naming the
operation is strictly more informative, and an extension that is never
advertised is better still, because then nothing conforming can ask.

---

## Phase 5 — the series did not apply on a fresh clone (2026-08-04)

**Class: host (H).** Found by doing the bring-up from nothing in a new
environment, which is a thing this project had not done since the series
was written.

`scripts/fetch-mesa.sh` then `scripts/apply-mesa-patches.sh`:

```
error: .gitkeep: already exists in working directory
Patch failed at 0017 nouveau,compiler: build the Rust half without a
                     standard library
error: the series does not apply cleanly to 6a02618ccf6c…
```

**Patch 0017 created `/.gitkeep` at Mesa's root.** It is our file:
`.gitignore` keeps `mesa/` out of the tree except for `!/mesa/.gitkeep`,
which is what makes the directory survive a clone. When the series was
generated inside the checkout, `git add -A` swept it up, and a zero-byte
scaffolding file of ours became part of a patch about `#![no_std]`.

**Why no gate caught it.** Once the series has been applied once,
`.gitkeep` is tracked *by Mesa*, so `git reset --hard $MESA_COMMIT`
deletes it — and the next apply recreates it, cleanly. The
"apply-mesa-patches twice on a reset `mesa/`" check therefore only ever
exercised the state the bug had already produced. The one broken case is
the first apply on a clone that has never been patched, and no run
started there. Observed here in both directions: with the stray hunk the
first apply fails; without it, two consecutive applies succeed and the
second correctly reports "all 44 patches already applied". As a bonus
the reset stops deleting a file this repository tracks, which it had
been doing silently on every reset.

**This edits an earlier patch rather than adding one at the end**, which
is not the idiom this project uses for corrections. The idiom cannot
apply: a new patch at position 45 cannot make patch 17 applicable, and
until 17 applies there is no tree to patch. The change removes a
three-line empty-file creation and its two summary lines; no Mesa
content moves, and the diffstat count goes 78 → 77 to stay true. Flagged
to the owner rather than done quietly.

---

