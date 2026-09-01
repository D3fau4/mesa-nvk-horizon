# Pending hardware runs

**This file is a debt ledger, and it is meant to be deleted.** Every
section below is work that was cross-compiled and never executed on a
console, and every one carries a **Done when** line saying what a run
has to report for the section to close.

When the last section closes, *delete this file*. Do not leave a
hollowed-out version behind saying "all clear": an empty ledger is just
another stale reference. That is the rule that killed this file's
predecessor, `docs/PENDING-VERIFICATION.md`, on 2026-08-25, and it
applies here unchanged.

It is deliberately **not** the same filename. That one is cited by
`mesa-patches/0054` and `0055`, and what those two point at is now in
`docs/MEASURED-ON-HARDWARE.md` — reviving the name would make those
citations resolve to a file that no longer holds what they meant.

The counterpart file is `docs/MEASURED-ON-HARDWARE.md`: facts that a
console established, class **HW**, none of which needs action. Nothing
with a **Done when** line belongs there. A section that closes here
moves there only if a measurement settles something that would
otherwise be measured twice; otherwise it just leaves.

| Class | Means | Does **not** prove |
|---|---|---|
| **H** — host | Built and run via `scripts/run-host-tests.sh` | Anything about the Switch |
| **X** — cross | Cross-compiled for aarch64 Horizon; a `.nro` exists | That it runs, or is correct |
| **HW** — hardware | Ran on a real console, with the log | Only what the log actually shows |

---

## 1 Zcull is now bound, and nothing has run it

**Class X.** `mesa-patches/0059` and `0060` and `tests/t_vk_zcull.c`
cross-build; no console has executed any of them.

Until 0060, the physical device advertised `has_zcull_info` and every
channel was created with `bind_zcull` false, so NVK programmed the
on-chip Zcull state on channels that had no context-switch save area.
0060 asks for the bind on contexts created with `NVKMD_ENGINE_3D`; 0059
stops advertising Zcull where `nvGpuGetZcullCtxSize()` is 0, and adds
`NVK_HORIZON_ZCULL=0`.

Zcull only ever *rejects*, so a fault here is silent: a fragment wrongly
culled is geometry that is not drawn, with no notifier and nothing in a
log. `t_vk_zcull` is built around that. It renders one depth workload
twice in one process — section A with `NVK_HORIZON_ZCULL=0`, section B
with it on — and compares the colour and depth images pixel for pixel,
plus an analytic check on each half so a fault affecting both equally
does not compare equal.

Three things the run has to report:

- whether A and B are identical. **If they are not, the action is to
  stop advertising Zcull** — set `has_zcull_info` false in 0059
  unconditionally, and drop 0060 — not to debug it from here.
- whether `nvGpuChannelZcullBind` succeeds at all. It is on the channel
  creation path, so a refusal fails `vkCreateDevice` rather than
  degrading. `NVK_HORIZON_ZCULL=0` is the way back without a rebuild,
  and a refusal means 0060 has to make the bind non-fatal or go.
- the two wall times the test notes. Whether Zcull is faster on this
  workload is unknown; the workload was built to be checkable, not to be
  culled well.

**Done when** `t_vk_zcull` has run on a console and the answer has been
acted on: kept and recorded if A and B match, withdrawn if they do not.
The seventeen existing Vulkan tests have to be re-run alongside it —
every one of them that clears a depth attachment now takes a different
path through `nvk_CmdBeginRendering`.

## 2 A wait submit no longer drains the pipeline

**Class H + X.** `horizon_cmds_fence_incr_bare` is covered by
`tests/host/h_cmds.c` (46/46 under ASan and UBSan) and the whole thing
cross-builds; no console has executed it.

`horizon_gpu_submit_waits` now emits an increment-only fence block and
skips the L2-invalidate prologue, on the argument that its command list
is host methods with no memory effect: nothing to invalidate, no engine
work for a wait-for-idle to wait for, and nothing dirty to write back —
the writes being waited on were flushed by the fence block of the
channel that made them. Two GPFIFO entries instead of three, and no
pipeline drain per cross-channel wait.

**This changes the submit path every test goes through**, so the whole
suite is what has to be re-run, not only the WSI tests. Patch 0035 means
only cross-channel waits reach it, so the paths that exercise it are the
upload queue and presentation: `t_vk_wsi_mt`, `t_vk_present_draw`,
`t_vk_submits`, `t_submit`.

`HORIZON_GPU_FULL_BARRIER_WAITS=1` restores the old shape, so both can
be measured in one run rather than across two builds.
`horizon_gpu_channel_get_stats()` reports `wait_submits` and
`bare_fence_submits`, so "it took the cheap path" is a number.

**Done when** the suite has passed on a console with the new shape, and
one run has compared it against `HORIZON_GPU_FULL_BARRIER_WAITS=1` on a
workload with cross-channel waits in it. If the two differ in
correctness, this comes out; if they do not differ in time either, it
still comes out, because then it is complexity for nothing.

## 3 The per-submit syncpoint read is now conditional

**Class X.** Reading the syncpoint is libnx, so no host suite reaches
this line; it cross-builds and no console has run it.

`horizon_gpu_channel_reap` skipped the `SyncptRead` ioctl when no
retirement is registered — which, on the path NVK takes, is always:
`horizon_gpu_channel_add_retirement` has exactly one caller in the tree
and it is `tests/t_teardown.c`. The fault check stays unconditional; it
is the safety property and it costs an event wait, not an ioctl.

**Done when** `t_teardown` still passes (it is the test that registers
retirements, so the read still happens there and the callbacks must
still fire), and `t_submit` has reported the per-submit cost with and
without `HORIZON_GPU_EAGER_REAP=1`. If the difference is inside the
noise, say so and consider taking the branch back out.

## 4 Three new test binaries, none of which has ever run

**Class X.** `t_vk_zcull`, `t_vk_pipelines` and `t_vk_draws` build as
`.nro` under `-Wall -Wextra -Werror` and link every archive
`meson.build` names. Nothing more than that is known about them.

`t_vk_pipelines` is the first thing in this project that makes the
shader heap grow past the chunk `nvk_heap_ensure_first_chunk` binds at
device creation: 96 distinct specializations of a 448-instruction
shader, which is at least 4.7 KiB of machine code each. Its sections C
and D are measurements rather than assertions — the cold compile
distribution, and what a second build of the same specializations costs
in the same process.

`t_vk_draws` is the first to issue hundreds of draws with the pipeline
changing between them, and the first to blend. Its section C tolerance
of 3/255 is derived from the round-off of twelve blend steps; the worst
error actually seen is reported, so the first run says how much of that
bound this hardware uses.

**Done when** all three have passed on a console, their measurements are
recorded, and — for `t_vk_draws` section C — the tolerance has been
narrowed to what was actually observed or the derivation corrected.
