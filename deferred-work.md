# Deferred work

## Deferred from: code review (2026-09-09)

Adversarial review of `git diff main...HEAD -- horizon compat` (merge base
`43c78f5`, HEAD `4ef796c` on `re-vulkan-clean`), group 1 of 3: 25 files
under `horizon/`, +2303/-229.

Each item below is real and caused by that change, but closes with a test
rather than an edit, and `tests/` was outside the reviewed group. Class
letters are the ones in CLAUDE.md: **H** host, **X** cross, **HW** hardware.

- **The shipped CRC-32 has no known-answer coverage.** Every cross build
  passes `-march=armv8-a+crc+crypto` (`Makefile:25`,
  `toolchain/horizon-aarch64.cross:124,149,166,178`), so
  `__ARM_FEATURE_CRC32` is defined, `horizon/cache/crc32.c:128-159` is what
  runs and the table at `crc32.c:47` is compiled out.
  `scripts/run-host-tests.sh:11,13` builds with `cc` and no `-march`, so
  `tests/host/h_blob_cache.c`'s published vectors and its regeneration of
  all 256 table entries only ever exercise the table. The comment at
  `crc32.c:136-138` claims an aarch64 host-suite run under user-mode
  emulation; no such path exists in the tree. **Done when** the published
  vectors have been checked in a build where `__ARM_FEATURE_CRC32` is
  defined — a case in a console suite, or a second `run` line in
  `run-host-tests.sh` that cross-builds `h_blob_cache` with `+crc` when a
  cross compiler and an emulator are present, with the same `note:` on skip
  that `h_nvkmd_sync` already uses.

- **`live_bytes` is a published field nothing reads, and the overwrite
  branch that maintains it is unasserted.** `bc_recompute_live` — a full
  walk after every mutation, so the three counters were correct by
  construction — is gone; `stats.entries`, `stats.keys` and
  `stats.live_bytes` are now maintained incrementally in `bc_index_set`
  (`horizon/cache/blob_cache.c:352-372`) and `bc_index_erase`
  (`:406-415`). `live_bytes` appears nowhere in `tests/`, `tools/` or
  `mesa-patches/`. `h_blob_cache`'s overwrite section stores twice under
  one key and asserts only which value wins, never a count. **Done when**
  `h_blob_cache` asserts `entries == 1` after two puts under one key, and
  `live_bytes` after put / overwrite / remove / compaction against the
  summed payloads reachable through the index.

- **`vm_map`'s refusal to map an object in pages larger than its own
  alignment is exercised by nothing.** `horizon/vm/vm.c:272-281` is new and
  guards a mapping the kernel accepts and then silently fails to resolve
  (measured 2026-08-24). All 19 `horizon_gpu_vm_map` call sites in `tests/`
  pass a legal pairing, and `horizon/vm/` is in none of the eight host
  suites, so the guard has no automated coverage of any class. **Done
  when** a case asserts that a 4 KiB-aligned object mapped into a big-page
  reservation returns `HORIZON_GPU_ERR_INVALID_ARG` — two lines next to the
  existing big-page section of `tests/gpu_memory/va_map.c`.

- **`horizon_gpu_heap_borrowed_regions` has no test, and `mem_create`'s
  borrowed-block walk has no assertion.** The function
  (`horizon/memory/mem.c:145-181`) is called once per device
  (`horizon/device/device.c:112`) to emit the warning that explains the
  crash, and appears nowhere in `tests/`, `tools/` or `mesa-patches/`;
  `tests/gpu_memory/borrowed_pages.c` answers the same question with its own
  `svcQueryMemory` walk and never calls it. The retry loop's own case,
  section F of that file, reads the quarantine counters and then asserts
  `t_check(t, true, ...)`. **Done when** section E compares
  `horizon_gpu_heap_borrowed_regions`' three numbers against its own walk
  inside section B's lend, and a case asserts that an allocation offered a
  borrowed block still succeeds with `horizon_gpu_heap_quarantine_stats`
  grown by the bytes set aside.

- **The wait pacing cannot be observed by the case that motivated it.**
  `horizon/sync/syncpt.c:176-197,229-247` and the same block in
  `horizon/channel/channel.c:1189-1265` exist because of a full-core spin
  measured on 2026-08-24. `tests/gpu_submit/fence_wait_many.c` part 2
  asserts only that every wait returned TIMEOUT, and its batch wall time is
  a `t_note`; a spinning wait and a paced wait both burn the caller's whole
  deadline and both return TIMEOUT. **Done when** part 2 asserts on cost
  rather than verdict — `batch_ns` against a generous multiple of `WAIT_NS`
  with N threads on a stalled fence, or a per-channel counter of wait chunks
  issued.

- **Three constants in `horizon/include/horizon_gpu/surface.h` cite no
  source.** `HORIZON_GPU_GOB_WIDTH_B` (64), `HORIZON_GPU_GOB_HEIGHT_ROWS`
  (8) and `HORIZON_GPU_MAX_BLOCK_HEIGHT_LOG2` (5) each carry a description
  and no provenance, against the rule that every constant names where it
  came from. The block-height ceiling carried a derivation that was wrong —
  "the display block's [field] is 3 bits wide, so 5 is the ceiling", when
  three bits hold 0..7 — which has been removed rather than replaced.
  **Done when** each of the three cites the header or page it was taken
  from (envytools, deko3d or the nouveau headers), or is shown to be
  measurable and measured.
