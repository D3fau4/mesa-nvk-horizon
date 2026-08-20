# The shader disk cache on Horizon

NVK recompiled every shader on every launch. Mesa already has the mechanism that
stops that — `util/disk_cache`, of which NVK is already a client — and this port had
it switched off. This document says why it was off, what turns it on, what the file
on the SD card looks like, and what has and has not been shown to work.

**Measured on a Nintendo Switch (run 31, 2026-08-11):** `vkCreateComputePipelines`
**5342 µs cold against 442 µs warm — 91% of the compile, 12× faster**, with the
shader that came off the card computing all 4096 of its output words correctly and
the driver reporting `hits = 2, misses = 0`.

## Why it was off

One Meson option, `-Dshader-cache=disabled`, in `scripts/configure-mesa.sh` and
`scripts/configure-mesa-nvk.sh`. It was recorded as a decision rather than hidden, and
the reason was accurate: `src/util/disk_cache_os.c` is written against `mmap`,
`flock`, `fcntl(F_SETLK)`, `posix_fallocate`, `ftw` and `getpwuid_r`. On devkitA64's
newlib, `sys/mman.h` does not exist at all, and `flock`, `posix_fallocate` and
`memfd_create` are declared-but-absent — they fail at *link*, which is worse than a
compile error because it fails wherever the object happens to be pulled in.

`docs/history/phase-4.md` left the pagaré explicit:

> `getuid` and `flock` are still unresolved in the archives. They are unreachable
> *today*, with `-Dshader-cache=disabled` … If a later phase enables the shader
> cache … `flock` comes back and needs an answer then.

## The shape of the answer

```
NVK  →  disk_cache_create / put / get / compute_key      (Mesa's API, unchanged)
            │
            ▼
   src/util/disk_cache.c                    ← UPSTREAM'S, reused as is
   key derivation, the put queue, zstd/zlib compression, statistics
            │
            ▼
   src/util/disk_cache_horizon.c            ← mesa-patches/0081
   the OS layer only: entry layout, the lock, the cache path
            │
            ▼
   horizon_gpu_blob_cache_*                 ← horizon/cache/, this repo, MIT
   the file format: records, CRCs, the index, compaction
            │
            ▼
   fopen / fread / fwrite / fseek / ftruncate / fflush / fsync
   (what libnx's `fsdev` devoptab actually provides)
```

Only `disk_cache_os.c` is replaced. The generic half is upstream's, which is the
point: `disk_cache_compute_key()` hashes `CACHE_VERSION`, the driver id, the GPU name,
`sizeof(void *)` and the driver flags into every key, and a parallel implementation of
that is a thing that can silently stop discriminating on something that matters.

Four patches, and two of them are meant for upstream:

| | | Upstream |
|---|---|---|
| 0078 | `util/disk_cache: include only what disk_cache.c uses` | yes — the file uses none of the five headers it opens with |
| 0079 | `util/mesa_cache_db: gate on flock, like the Fossilize database beside it` | yes — a libc trait, written the way its sibling writes it |
| 0080 | `nouveau/vk: let the build name a driver identity Mesa's version cannot` | no |
| 0081 | `util/disk_cache: add a Horizon backend` | no |

## The file

One append-only file per GPU, at `sdmc:/mesa_shader_cache/<gpu_name>.hzc`.
Little-endian throughout, read and written byte by byte rather than by casting a
buffer to a struct.

```
file header, 64 bytes at offset 0
  0   magic[8]           "HZBLOBC1"
  8   format_version     u32
  12  header_size        u32   64 + align8(driver_key_size)
  16  key_size           u32   32, matching Mesa's CACHE_KEY_SIZE
  20  driver_key_size    u32
  24  driver_key_crc     u32
  28  state              u32   CLEAN | COMPACTING
  32  reserved[7]        u32
  60  header_crc         u32   over bytes [0, 60)
  64  the driver key itself, then zero padding

record, 56 bytes then payload then zero padding to a multiple of 8
  0   magic              u32
  4   flags              u32   DATA | KEY | TOMBSTONE
  8   key[32]
  40  payload_size       u32
  44  payload_crc        u32
  48  seq                u32   monotonic within the file
  52  header_crc         u32   over bytes [0, 52)
```

The driver key is stored **whole and compared byte for byte**, not digested. A digest
collision here does not mean a miss; it means a shader compiled by a different driver
handed to this one, which is a GPU fault with no error path.

Entries are what `disk_cache_horizon.c` puts in the payload: four bytes of
uncompressed length, then the deflated blob. Compression is Mesa's own
(`util_compress_deflate`), enabled because devkitPro's portlibs supply zlib 1.3.1 and
zstd.

## Robustness, and why it is not the usual mechanism

On Horizon `rename()` over an existing target fails (`fsFsRenameFile`), so the
write-a-temp-file-then-rename trick that gives POSIX callers atomic replacement is not
available. Integrity comes from the content instead:

- **A write cut short** — a flat battery, an applet killed — leaves a record whose
  checksum or bounds do not hold. The next `open()` stops the scan there, truncates the
  file back to the end of the last intact record, and reports how many bytes it threw
  away. Everything written before the tear is still served.
- **A flipped byte in a payload** is caught by the payload CRC at `get()` time. The
  entry becomes a miss and is dropped from the index; the entries either side are
  untouched. A miss costs one recompile. A wrong answer costs a GPU fault.
- **A flipped byte in a record header** ends the chain: nothing after it can be
  located, so it and everything following it are truncated away.
- **A file from another driver build**, a garbage file, a zero-length file, a damaged
  magic and a damaged header checksum are all the same path — the file is reset to an
  empty one, and the caller is told (`was_reset`, `discarded_bytes`).
- **An interrupted compaction** is the one case the file cannot recover from by
  reading, because between the first byte moved and the last the file is neither the
  old cache nor the new one. The header is stamped `COMPACTING` and flushed before the
  rewrite and `CLEAN` after it; a file found in `COMPACTING` is reset.

## No path operation after `open()`

libnx routes every path-based devoptab entry point — `open`, `stat`, `unlink`,
`mkdir`, `rename`, `diropen` — through `fsdev_fixpath()`, which copies into the single
file-scope buffer `__nx_dev_path_buf` with no lock of any kind
(`nx/source/runtime/devices/fs_dev.c`: the buffer at line 211, and
`char *fs_path = __nx_dev_path_buf` at 405, 1111, 1197, 1225 and 1276). Two threads
doing path work at once corrupt each other's path, and the symptom is a write to the
wrong file.

`disk_cache.c` calls the backend from its put queue, i.e. off the calling thread. So
the store performs **no path operation at all** once it is open: it works entirely
through the `FILE` handle, and compaction rewrites the file **in place** rather than
through a temporary. The only path operations in the whole feature are the `mkdir` and
the `fopen` inside `disk_cache_create`, on the caller's thread.

## Limitations

- **One process.** There is no inter-process lock, deliberately: Horizon runs one
  homebrew title at a time, and a lock file with no way to detect a stale holder would
  turn one crash into a permanently read-only cache.
- ~~One writer per file within a process.~~ **Fixed after review.** Two `VkInstance`s
  do resolve to the same path, and two independent stores over one file would each
  keep their own append offset and index — the second overwriting the first's records.
  `disk_cache_horizon.c` now keeps a process-wide registry keyed by path and hands out
  refcounted references, so one file has one store and one lock. (It would not have
  returned a *wrong* shader even then: `get()` re-reads the record header and compares
  the key before it believes the index, so the failure mode was a cache that quietly
  stopped working.)
- **The store is not reentrant.** `disk_cache_horizon.c` serialises every call with a
  `simple_mtx` — one per open file, held in the registry entry, so caches sharing a
  file share the lock. It also means `get()` waits behind a `put()` that is writing to
  the SD card.
- **`fsync` per entry.** Measured at ~5.3 ms on a console (run 27). Correct, on a
  background thread, and buying less than it appears to — see the results below.
- **The ceiling is compiled in at 64 MiB.** Upstream defaults to 1 GiB, which is a
  desktop number for a card that also holds somebody's games. `MESA_SHADER_CACHE_DIR`,
  `MESA_SHADER_CACHE_MAX_SIZE` and friends are still honoured, but libnx does not
  populate `environ`, so on a console they are only reachable from inside the process
  (which is how the tests drive them).
- **Compaction keeps the newest entries, not the most recently used.** There is no
  access time in the format. It compacts to half the ceiling rather than to the
  ceiling, so a full cache holds about half of what `max_size` allows — the price of
  not rewriting the file on nearly every write.
- **`put_key`/`has_key` are durable here, and upstream's are not.** Upstream keeps them
  in an mmap'd 64 K-slot table where a collision silently overwrites; there is no mmap
  here, so they are records. Nothing in NVK calls either.

### NVK stops recompiling — run 29, 2026-08-11

`t_vk_cache`, `docs/hw-logs/t_vk_cache-run29-FAIL.log`. The driver's own cache
directory, no `MESA_SHADER_CACHE_DIR`, and no `VkPipelineCache` passed to the create,
so nothing in the process could answer it except the disk:

```
MESA: info: disk cache: sdmc:/mesa_shader_cache/nvk_012b.hzc, 2 entries
note vkCreateComputePipelines took 646 µs on a warm cache
MESA: info: disk shader cache:  hits = 2, misses = 0
```

**Two hits, no misses.** That is the feature working, and it is the first time it has
been shown.

The run reports `FAIL (37/39)`, and **both failures are the test's own**. It expected
`out[id] = id` from a shader that documents `out[id] = (id * 2654435769) ^
2781138957`; at id 0 the multiply vanishes and the answer is the XOR constant,
`0xa5c4d00d`, which is exactly what the console returned. The test also dispatched 64
groups of a `LocalSize 64` shader into a 64-word buffer — 4096 invocations, 4032 past
the end. Both are fixed, with a static check on `expect_word(0)` and a poisoned tail
that a wrong dispatch size would disturb.

### And it is correct in full — run 30, 2026-08-11

`t_vk_cache` with those two bugs fixed, **`RESULT: PASS (44/44)`**
(`docs/hw-logs/t_vk_cache-run30-PASS.log`):

```
ok   the expected-value function matches the shader (expect_word(0) = 0xa5c4d00d)
ok   the cached shader's output: 4096/4096 words match
ok   the words past the dispatch are untouched: 64/64 words are 0xdeadbeef
MESA: info: disk shader cache:  hits = 2, misses = 0
```

**Every one of the 4096 words**, from a shader NVK did not compile, and nothing
written past the last invocation. `vkCreateComputePipelines` took 432 µs warm (646 µs
in run 29 — the same operation, the difference is card noise).

### And this is what it saves — run 31, 2026-08-11

`RESULT: PASS (45/45)` (`docs/hw-logs/t_vk_cache-run31-PASS.log`):

```
note vkCreateComputePipelines took 442 us on a warm cache
note against 5342 us when this build's cache was cold: 4900 us saved, 91% of the compile
ok   a warm cache creates the pipeline faster than a cold one (442 us against 5342 us)
```

**12× faster. 91% of the compile gone.** On the driver's own cache path, with no
environment override and no `VkPipelineCache` passed to the create.

The cold baseline is self-certified rather than trusted: the test records one *only*
when the driver itself reported `0 entries` at startup, and a cold run clears
`sdmc:/mesa_shader_cache` before the driver opens anything. Runs 29 and 29 both wrote
`0` to the marker rather than record a warm create as cold, which is what makes the
number in run 31 mean something.

One shader is one shader. What this does not say is what a real game saves, where
there are hundreds of pipelines and the per-shader compile is far larger than a
64-invocation write of a multiply and an XOR — the ratio is a floor rather than a
figure to quote.

- **A read-only cache needs no write permission.** `read_only` opens with `"rb"`, so
  a file or a card that genuinely denies writes still serves hits.

## The driver identity, which is the part that could have been dangerous

On Horizon there is no dynamic loader, so NVK cannot read an ELF build id, and
`mesa-patches/0018` derived the driver identity from `PACKAGE_VERSION` and
`MESA_GIT_SHA1`. Both describe the *pinned Mesa checkout*. Neither moves when
`mesa-patches/` or `horizon/` change — and those contain NAK, NIL and the whole
platform layer, i.e. most of what decides what a shader compiles to.

While that identity was only `driverUUID` and the pipeline cache UUID, the cost of it
being stale was an application reusing its own blob. Keying a *disk* cache on it would
mean the previous build's binaries handed to this build's hardware, with no error path
and no line in any log.

`scripts/gen-driver-id.sh` closes it: a SHA-256 over `MESA_COMMIT`, every patch in
`mesa-patches/`, every `.c`/`.h` under `horizon/` and `compat/`, `toolchain/versions.env`
(which pins the image and therefore the compiler and the Rust toolchain) and
`toolchain/*.cross` (which carries `-march`, `-mtune`, `-mtp` and the rest of the
code-generation flags). Passed to Mesa as `-Dhorizon-driver-id` and mixed into
`driver_build_sha`. It is a digest of sources and not a timestamp on purpose — a
timestamp would cold-start the cache on every rebuild of unchanged source and would
break reproducible builds.

What it still cannot see, said plainly: a toolchain substituted without telling the
repository — a local `$DEVKITPRO` whose gcc differs from the pinned image's, or an
image republished under the same tag. Sources, flags and `versions.env` would all be
identical and the compiler would not. Closing that means either hashing the built
driver (but the identity is needed to *configure* the build that produces it) or
running the cross compiler at configure time (a container round-trip per configure).
Neither is worth it for a hazard that requires deliberately building against an
unpinned toolchain, and `scripts/print-toolchain-versions.sh` is what a build should be
reported with.

## What has been shown, and what has not

**Host, under ASan and UBSan** — `scripts/run-host-tests.sh`, `h_blob_cache`
138 checks: the CRC against published vectors, an empty cache, miss → store → hit,
persistence across a reopen, 200 entries through several index growths, overwrite,
remove, the key-only namespace, a driver-build change, a write cut short mid-payload,
a flipped payload byte, a flipped record-header byte, an interrupted compaction, a
record claiming more than the file holds, an all-zero key, a damaged newest copy of a
key, a zero-length file, a garbage file, a damaged magic, a damaged header checksum,
the size ceiling and its compaction, a ceiling smaller than one entry, read-only mode,
and every argument a caller can get wrong.

**Cross** — the driver compiles and links with `ENABLE_SHADER_CACHE` defined, and the
pagaré is discharged:

```
$ nm -u build/mesa-probe/src/util/libmesa_util.a | sort -u \
    | grep -E 'mmap|flock|posix_fallocate|memfd_create|getpwuid|nftw'
(no output)
```

**Hardware — run 27, 2026-08-11**, `t_shader_cache`, build
`2026-08-11T00:07:01.401Z d0514c1-dirty mesa:3ba5227`, `RESULT: FAIL (46/47)`
(`docs/hw-logs/t_shader_cache-run27-FAIL.log`).

What it established, and none of it was known before:

- `disk_cache_create()` returns a live cache on a Switch, and the driver says so
  itself — `disk cache: …/t_shader_cache.hzc, 0 entries`, then `2 entries`, then
  `disk shader cache: hits = 2, misses = 1`. Mesa's API works end to end, including a
  **256 KiB entry**, which is what permanently rules out the 64 KiB ceiling of the
  blob-callback path.
- A file cut short mid-payload recovers on the real card: the tear is noticed, the
  four whole entries survive, the torn one is a miss and not a wrong answer, and the
  repaired file takes new writes.
- A different driver identity resets the file and serves nothing from it —
  `did not match this driver build …; started over (624 bytes discarded)`.
- An in-place compaction leaves a file that reopens clean, with the newest entry
  intact.

The one failure was **in the test, not the store**, and it had a twin that was worse:
`file_size_of()` opened the cache file a second time while the store still held it
open, which this platform refuses, and returned −1. One caller compared that as a
signed `long` (`-1 < 4096` — a check that could not fail, and it reported a pass); the
other cast it to `uint64_t` and got the FAIL. Both are fixed, and callers now assert
that the measurement happened at all.

**Section C reported a cold cache**, which is what a first launch is meant to report.

**Run 28, 2026-08-11**, same test with those fixes in, build
`2026-08-11T17:02:09.068Z 1f9f128-dirty mesa:3ba5227`, **`RESULT: PASS (56/56)`**
(`docs/hw-logs/t_shader_cache-run28-PASS.log`). It closed the two things run 27 could
not:

- `C1 this launch found 32 entries (WARM — a previous launch filled it)`, and all
  three C2 checks: every entry the previous launch wrote came back intact, the file
  did not have to be reset to do it, and it was not truncated either. **Entries
  outlive the process that wrote them** — the claim a cache exists to make.
- `A6 ftruncate() actually shortened the file on this filesystem`, this time measured
  with the store closed and beside `A6 the store's own size is the size on the card`.
  The check that could not fail is now one that can.

And the two defects run 27 exposed are fixed, measured on the same console:

| | run 27 | run 28 |
|---|---|---|
| open with 201 entries | 40498 µs — **201 µs/entry** | **1605 µs — 7 µs/entry** |
| 100 puts into a 32 KiB ceiling | **70 compactions**, 2.58 s | **5 compactions**, 0.64 s |

A 25× faster open and 14× fewer rewrites. The file sizes are real numbers now rather
than `-1`: 25688 bytes for 201 entries, 21688 after compaction, and the card agrees
with the store about both.

### The numbers, and what changed because of them

| | run 27 | after |
|---|---|---|
| `put` of 4 KiB | 8148 µs | — |
| 200 `put`s of ~50 B | 5292 µs each | — |
| `get` of 4 KiB | 3408 µs | — |
| Mesa `disk_cache_put` + `wait_for_idle` | 4606 µs | — |
| Mesa `disk_cache_get` | 291 µs | — |
| open with 201 entries | **40498 µs — 201 µs/entry** | **1605 µs — 7 µs/entry** (run 28) |
| 100 `put`s into a 32 KiB ceiling | **70 compactions, 2.58 s** | **5 compactions, 0.64 s** (run 28) |

Two of those were structural defects rather than costs:

- **201 µs per entry to open** is one I/O per record, not a sequential read. The scan
  seeked to every record header, and whether a C library keeps its buffer across a
  seek is a property of that library — so the scan now never seeks: it reads forward,
  and steps over a payload by consuming it unless the payload is larger than one
  buffer refill, in which case seeking moves fewer bytes.
- **70 compactions for 100 puts** was in the arithmetic: compaction filled the file to
  the ceiling, so the next put crossed it again. It now compacts to a watermark at
  half the ceiling, which on the host takes 200 puts into a 32 KiB cache from ~140
  rewrites to 11.

`fsync` per entry, at ~5 ms, is a cost and not a defect (run 28 measured 5413 µs,
within noise of run 27's 5292 µs) — it is on a background thread
and compiling a shader takes far longer. It is worth saying plainly that it buys less
than it looks: the format already recovers from an unflushed tail by truncating it,
which is exactly what a missing `fsync` would cost. Changing it is a decision with a
measurement attached, not a tidy-up, and it has not been taken.
