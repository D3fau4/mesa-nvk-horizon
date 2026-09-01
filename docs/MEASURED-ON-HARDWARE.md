# Measured on hardware

Facts about this platform that were established by running code on a
Nintendo Switch, kept in one place **so nobody re-litigates them**. None
of it needs action. Each entry is here because the cost of measuring it
again is higher than the cost of the paragraph.

**This file is not a debt ledger, and it must never become one.** Its
predecessor, `docs/PENDING-VERIFICATION.md`, was: it tracked work that
had been cross-compiled and never run, and every section carried a
"Done when" line. It was deleted when its last section closed — as its
own rules demanded, rather than being hollowed out into an "all clear"
that says nothing. Work that is still owed belongs in a commit message
or in a new ledger, not appended here.

The project's evidence discipline has three classes and never collapses
them. Everything below is class **HW**:

| Class | Means | Does **not** prove |
|---|---|---|
| **H** — host | Built and run via `scripts/run-host-tests.sh` | Anything about the Switch |
| **X** — cross | Cross-compiled for aarch64 Horizon; a `.nro` exists | That it runs, or is correct |
| **HW** — hardware | Ran on a real console, with the log | Only what the log actually shows |

A result whose log has no `horizon-build-id` line cannot be attributed
to a build and does not belong here.

---

## A dock cannot make a swapchain suboptimal here

The transition patch `0040` was written for cannot be delivered on this
platform. This is the single most expensive thing on this page to
re-measure, because getting it wrong costs a person standing at a
console plugging it in and out, so it is written out in full.

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

## The Forward+ hang is not the loop, the stack, the fragment stage or the kill

Four tests built to reproduce Godot 4.1's Forward+ hang outside Godot all
pass on GM20B (2026-09-01, build `1b49911-dirty mesa:5554de6-dirty`, logs
in `sdmc:/horizon_gpu_tests/`):

| Test | Result | What it takes off the table |
|---|---|---|
| `t_vk_loop` | PASS 38/38 | a loop whose trip count comes from memory, including every bound 0 |
| `t_vk_loop2` | PASS 38/38 | the cluster loops' exact bound idiom: packed min/max, prmt-style extract, bcsel |
| `t_vk_crs` | PASS 35/35 | twelve nesting levels around a loop — the unbacked convergence stack |
| `t_vk_kill` | PASS 83/83 | a fragment loop, and a fragment loop entered after half the lanes are killed |

Together with the passes recorded elsewhere, **the whole "it is the control
flow, the compiler, the fragment stage or the kill" family is excluded on
hardware.** `t_vk_kill` is the one that closes it: its three cases are
256/256 texels right, including "bound 4, half the lanes killed first"
with 128 texels killed. A counted fragment loop with a kill in front of it
works on this chip.

**This contradicts a reading of the Godot measurement, and the Godot
reading is the one that was wrong.** Bisecting inside the engine on
2026-08-29 produced "with the bound forced to a compile-time four, the body
replaced by an increment, and the whole induction replaced by a plain
counted loop, it still hung", which was taken to mean the loop itself hangs
the channel. It cannot mean that: the loop it was reduced to is the loop
`t_vk_kill` runs. Whatever still hung was not the shape that was left.

**Do not re-measure this by shrinking Godot's shader further.** Painting a
value into `frag_color` to inspect it changes register allocation enough to
hide the bug, so a reduction inside the engine cannot be trusted to still
contain the fault it started with. The way to ask a question about this
hang is a test in `tests/`, which is why these four exist.

What survives is the reading of descriptor set 1 from the fragment stage.
Not from a missing cache flush: the descriptor writer does clean its dirty
range at `nvk_descriptor_set.c` `nvk_descriptor_writer_finish`, at
`set->mem_offset_B + dirty_start` aligned both ends to `nc_atom_size_B`,
and on aarch64 `util_has_cache_ops()` is true and `cache_ops_aarch64.c`
(`dc cvac` + `dsb sy`) is the implementation compiled in, not the null stub.

## A fragment shader reads descriptor set 1 correctly

`t_vk_set1`, PASS 170/170 on GM20B (2026-09-01). Four numbers per texel --
an SSBO from set 0 as the control, an SSBO and a UBO from set 1, and the
set-1 SSBO's runtime-array length -- all right on 256/256 texels in five
cases: the two sets bound in one `vkCmdBindDescriptorSets`, in two calls in
Godot's order, in two calls in the other order, and then the same read after
the **GPU** wrote the set-1 SSBO in the same command buffer, once with
`vkCmdFillBuffer` and once with `vkCmdCopyBuffer` -- which is what Godot's
`buffer_clear` and `buffer_update` are.

So the read of set 1 from the fragment stage is not broken on this chip, and
neither is the write side: a transfer write with a TRANSFER_WRITE to
SHADER_READ barrier is visible to the draw that follows it. **The hypothesis
that Godot's Forward+ shader hangs because it reads a wrong descriptor is
excluded.**

## The convergence-stack fix is already in the Godot build that fails

`crs_size()` in `sm50.rs` counts one stack slot per nesting level and
reserves nothing below 16, where nouveau counts two for the same field. The
working tree carries a fix that multiplies by two, and it is tempting to
conclude that Godot's `max_crs_depth = 13` shader therefore runs on the
16-entry on-chip stack with nothing behind it.

**It does not, because that fix is already compiled into the binary that
hangs.** `bin/godot.nx.template_debug.arm64.elf` (2026-08-29) contains the
`crsbig` and `crsinfo` strings the fix adds, as does the
`libnouveau_rust_runtime.a` installed in the portlibs prefix it linked
against. Whatever hangs the Forward+ draw, it is not the size of the
convergence stack.

Recorded because it was re-derived and nearly re-tested at the cost of a
full Godot rebuild. Two traps made it look unproven: `build/mesa-nvk`'s
`libnak.a` is the **C helpers** archive and is not where `sm50.rs` lands
(that is `libnak_rs.rlib`, bundled into `libnouveau_rust_runtime.a`), so its
old mtime means nothing; and **`strings` does not exist on the Windows
development box**, so a `strings ... | grep -c` check reports 0 matches for
every file whether or not the string is there. `grep -ac <string> <archive>`
is the check that works.

## Godot's Forward+ hang is not the loop, and not the kill either

Three runs on 2026-09-01, all ending in `fault notification 8 (fifo idle
timeout)` on the first 3D phase:

| Build | What it removes | Result |
|---|---|---|
| `bench_fp_dbg` | nothing — the stock Forward+ bench | 2D phases 1-3 fine (60, 20, 58 fps); dies in phase 4, `3d_cubes_200` |
| `probe13` | all seven `discard;` sites in the scene shader | dies, on a single cube |
| `probe12` | the loop itself — a literal bound, so NIR unrolls it and no back edge is left | dies, on a single cube |

`probe13` kills the hypothesis that a `discard` leaves the warp and a later
loop waits forever for lanes that no longer exist. `t_vk_kill` had already
said the same from the other side: a fragment loop entered after half the
lanes are killed is right on 256/256 texels.

**`probe12` is the one that matters, and it was written as a control.** With
the loop unrolled and no back edge anywhere, it still hangs. So the loop is
not the cause, and the whole probe series that reduced it — bound, body,
induction, range read, subgroup reduce — was not measuring the loop.

**What it was measuring is dead-code elimination.** Every "fast" reading in
that series came from a build whose loop is never entered, and a loop that
never runs makes its accumulator a constant, which lets DCE take the lighting
that consumes it with it. The same trap is already recorded for
instrumentation: overriding `frag_color` to paint a value DCE's the lighting
and the cluster loops, and a build that does it runs at 60 fps for the wrong
reason. "The loop is free when it is not entered" and "the shader is mostly
gone when it is not entered" are the same measurement.

So a Forward+ fragment shader that actually does its work hangs the channel,
and nothing yet distinguishes which part of that work does it. What is
excluded on hardware: the loop, the back edge, the kill, the convergence
stack, the descriptor read (`t_vk_set1`), every optional NAK pass, the
caches and barriers, and the attachment-less cluster-builder pass.

The one measurement in that series worth keeping is the NAK dump of the
scene fragment shader: **4244 instructions, 104 GPRs, 16 warps/SM**, compiled
with `USE_SUBGROUPS`. 104 GPRs is what holds occupancy down to 16 of 64
warps, and register pressure is the one property that both a loop and an
added `frag_color` paint change.

## The series' "NOT RUN ON A CONSOLE" lines are frozen at their writing date

A Mesa patch's message records what had been measured *when it was
written*, and it is never rewritten afterwards: a patch is identified by
its subject and its diff, so editing one to refresh a claim is exactly
the divergence the applier exists to report. Four patches therefore
carry statements that were true in August and are no longer the current
state. **This file is where the current state lives**, not the patch.

- **`0035`** (a wait on this channel's own syncpoint) says the Phase 5
  suite "is part of what has to be re-run for it, not just the swapchain
  tests. NOT RUN ON A CONSOLE." That re-run has happened: all seventeen
  `t_vk_*` tests pass on hardware on the current series.
- **`0038`** (`VK_NN_vi_surface`) says "NOT RUN ON A CONSOLE." It has
  run. `vkCreateViSurfaceNN` is how every surface on this platform is
  created, and the five tests that create one all pass — `t_vk_swapchain`
  143/143 and `t_vk_suboptimal` 273/273 among them, both measured with
  the console docked.
- **`0040`** says "VK_SUBOPTIMAL_KHR HAS NEVER BEEN RETURNED ON
  HARDWARE." Still literally true, and no longer a gap in coverage: see
  the section above. There is nothing to cover.
- **`0054`** defers "the display changing under a *scaled* swapchain" to
  `docs/PENDING-VERIFICATION.md`, a file that no longer exists. The
  question was settled: `t_vk_swapchain` section G ran docked for its
  whole 90-second window and the surface never moved off 1280x720.

## Measured 2026-08-24

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
- **A dock reaches the process but not the layer.** See the section at the top of this file — the
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
