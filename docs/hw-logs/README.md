# Hardware logs

Console runs, kept verbatim. A log here is evidence, so none of them is edited
or deleted after the fact — including the ones later found to have measured
less than they claimed. This file is where that is said, because a reader opens
the log, not `STATUS.md`.

## Superseded runs

A `-PASS` in a filename means the run reported PASS. It does **not** mean every
check in it measured what its text says.

### `t_gpuwrite-run1.log`, `t_gpuwrite-run2-matrix.log`, `t_gpuwrite-run3-l2fix.log`, `t_gpuwrite-run4-PASS.log`

Every one of these predates the fix in `ad5b973`, and all four contain the line
that gives it away — `6 dwords` in `run1`, before the L2 writeback was added to
the block, and `9 dwords` after:

```
  ok   <arm>: fence increment encoded (N dwords)
```

`run1` is the two-arm version (`cached`, `noncacheable`) and reports
`FAIL (25/27)`; the four-arm matrix starts at `run2-matrix`.

That check is the artefact of the defect. The test embedded its own fence
increment in the span it submitted while `horizon_gpu_submit()` also appended
the channel's, so the hardware counter advanced twice per submit and the
accounting once. From the **second arm onwards** the threshold handed to
`wait_fence` had already been passed by the previous arm's extra increment, so
the wait returned before that arm's GPU write had run and the readback raced
the GPU instead of following it.

What survives, and what does not:

- **Arm A is sound in all four runs.** It is the first submit on its channel,
  so its wait was correct.
- **Arm D passing is sound.** It was the arm most exposed to the early return;
  a payload already present when you look early is present.
- **Arms B and C measured nothing.** In `run2-matrix` in particular, B failing
  was read as "identical with CPU caching on and off" and C failing as "MEMBAR
  does not help". Neither failure is evidence of anything.

`t_gpuwrite-run5-review-fixes-PASS.log` is the first run with the corrected
test — 47/47, which is 51 minus the four removed encode checks.

It does **not** re-run the original no-flush-versus-flush experiment, and
cannot: `0cca09d` made the channel's fence block emit the L2 writeback on every
submit, so no no-flush arm exists any more. The four arms now differ only by a
redundant `MEM_OP`. Their all passing is consistent with the L2 finding and is
not a test of it.

### Logs carrying the wrong-cause memory-type note

`t_vulkan.log`, `t_vulkan-run3-l2fix.log`, `t_vulkan-PASS-20260804.log`, and
the emulator runs `t_vulkan-emu-run3.log` and `t_vulkan-emu-run5-reverted.log`
contain

```
  note memory type 0: ... — the readback rests on cache maintenance (D5)
```

which names the wrong cause: the console showed the readback rested on the
GPU-side L2 writeback, not on CPU cache maintenance. Corrected in `fc3c636`.

That `t_vulkan-PASS-20260804.log` is on this list matters — it is the log
quoted in `STATUS.md` as the Phase 4 exit criterion. **The PASS stands**; the
readback is real and the fill was verified. Only the explanatory note beside it
was wrong, and it is one line of commentary, not a check.

A log carrying `GPU-side visibility is the channel's L2 writeback` instead was
built after the correction: `t_vulkan-run5-review-fixes-PASS.log` and
`t_vulkan-run6-D16-PASS.log`.

`t_vulkan-run2.log` and `t_vulkan-run3.log` never reach that note at all —
both abort at `vkQueueSubmit` with an MMU fault (`nv 0x00000d5c`, notifier 31),
which is R18, fixed by patch 0038.

### `t_vulkan-pushdump-truncated.log`

The diagnostic build blackscreens on console and its log stops mid-MME-microcode
upload. Parked deliberately: `t_gpuwrite` localised the defect below Vulkan and
this instrument was not needed.

## Emulator runs

Files with `-emu-` in the name are **not hardware**. They are kept because the
emulator answered questions hardware did not need to be spent on — the MMU
fault not reproducing there is itself a finding — but nothing in them is a
statement about a Switch.
