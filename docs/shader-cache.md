# The shader disk cache on Horizon

NVK recompiles every shader on every launch. Mesa already has the mechanism that
stops that — `util/disk_cache`, of which NVK is already a client — and this port had
it switched off. This document says why it was off, what turns it on, what the file
on the SD card looks like, and what has and has not been shown to work.

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
   src/util/disk_cache_horizon.c            ← mesa-patches/0079
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
| 0076 | `util/disk_cache: include only what disk_cache.c uses` | yes — the file uses none of the five headers it opens with |
| 0077 | `util/mesa_cache_db: gate on flock, like the Fossilize database beside it` | yes — a libc trait, written the way its sibling writes it |
| 0078 | `nouveau/vk: let the build name a driver identity Mesa's version cannot` | no |
| 0079 | `util/disk_cache: add a Horizon backend` | no |

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
- **One writer per file within a process.** Two `VkInstance`s would open the same path
  twice. Nothing in NVK does this today; if something does, it needs a
  process-wide registry keyed by path.
- **The store is not reentrant.** `disk_cache_horizon.c` serialises every call with a
  `simple_mtx`, which also means `get()` waits behind a `put()` that is writing to the
  SD card.
- **`fsync` per entry.** Correct, and unmeasured. `t_shader_cache` section A reports
  the cost so the decision to batch it can be made with a number.
- **The ceiling is compiled in at 64 MiB.** Upstream defaults to 1 GiB, which is a
  desktop number for a card that also holds somebody's games. `MESA_SHADER_CACHE_DIR`,
  `MESA_SHADER_CACHE_MAX_SIZE` and friends are still honoured, but libnx does not
  populate `environ`, so on a console they are only reachable from inside the process
  (which is how the tests drive them).
- **Compaction keeps the newest entries, not the most recently used.** There is no
  access time in the format.
- **`put_key`/`has_key` are durable here, and upstream's are not.** Upstream keeps them
  in an mmap'd 64 K-slot table where a collision silently overwrites; there is no mmap
  here, so they are records. Nothing in NVK calls either.

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
`mesa-patches/` and every `.c`/`.h` under `horizon/`, passed to Mesa as
`-Dhorizon-driver-id` and mixed into `driver_build_sha`. It is a digest of sources and
not a timestamp on purpose — a timestamp would cold-start the cache on every rebuild
of unchanged source and would break reproducible builds.

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

**Hardware — nothing.** No console has run any of this. `tests/t_shader_cache.c` is
built and shipped for exactly that purpose; see `tests/README.md` for the run order
and note that **section C needs two launches** — the first one on any console reports
a cold cache, which is a pass and is not the measurement.
