/*
 * horizon_gpu — a keyed blob store on a Horizon filesystem.
 *
 * WHY THIS EXISTS. Mesa's shader disk cache (`src/util/disk_cache`) was
 * turned off for this port because its OS layer is written against
 * mmap, flock, fcntl(F_SETLK), posix_fallocate, memfd_create, ftw and
 * getpwuid_r — every one of them measured absent on devkitA64's newlib.
 * This module is the storage half of the answer: a single-file,
 * append-only, CRC-validated store that needs nothing libnx's `fsdev`
 * devoptab does not already provide
 * (fopen/fread/fwrite/fseek/ftell/ftruncate/fflush/fsync/rename/remove).
 * `src/util/disk_cache_horizon.c`, added by mesa-patches/0076, is the
 * other half and is the only thing that speaks Mesa.
 *
 * WHY IT IS IN horizon_gpu AND NOT IN THE MESA PATCH. Because a format
 * whose whole value is what it does with a *broken* file has to be
 * testable against broken files, and a file inside a patch cannot be
 * compiled by scripts/run-host-tests.sh. Everything here is C11 plus
 * <stdio.h>: no libnx, no Mesa, no Vulkan, so the host build compiles
 * the same translation unit the Switch build does and runs it under
 * ASan/UBSan against truncated, flipped and garbage files.
 *
 * ROBUSTNESS MODEL, and it is not the usual one. On Horizon `rename()`
 * over an existing target fails (fsFsRenameFile), so the write-temp-
 * then-rename trick that gives POSIX callers atomic replacement is not
 * available. Integrity therefore comes from the *content*: every record
 * carries a CRC32 of its header and a CRC32 of its payload, and a write
 * cut short by a flat battery simply fails to validate the next time the
 * file is opened, at which point the torn tail is truncated away and
 * everything written before it is still there.
 *
 * CONCURRENCY. Not thread-safe, by contract: the caller serialises.
 * There is deliberately no inter-process lock, because Horizon runs one
 * homebrew title at a time and a lock file with no way to detect a stale
 * holder would turn one crash into a permanently read-only cache.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_GPU_BLOB_CACHE_H
#define HORIZON_GPU_BLOB_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Key width, in bytes. 32 because Mesa's cache_key is CACHE_KEY_SIZE ==
 * BLAKE3_KEY_LEN == 32 (mesa/src/util/disk_cache.h). The store does not
 * care how a key was computed; it only ever compares them. */
#define HORIZON_GPU_BLOB_CACHE_KEY_SIZE 32u

/* Largest single entry the store will accept, in bytes. 64 MiB is far
 * above any shader binary NVK produces and far below anything that could
 * be a plausible size read out of a corrupt header, which is the point:
 * it is a sanity bound on parsing, not a policy on shaders. */
#define HORIZON_GPU_BLOB_CACHE_MAX_ENTRY_SIZE UINT64_C(0x4000000)

/* Default ceiling on the file, in bytes, when the config asks for 0.
 * 64 MiB: Mesa's own default is 1 GiB, which is a desktop number — this
 * is a shared SD card holding somebody's games. */
#define HORIZON_GPU_BLOB_CACHE_DEFAULT_MAX_SIZE UINT64_C(0x4000000)

typedef struct horizon_gpu_blob_cache horizon_gpu_blob_cache;

typedef struct horizon_gpu_blob_cache_config {
    /* Path of the cache file, e.g. "sdmc:/mesa_shader_cache/nvk.hzc".
     * Copied; the caller keeps ownership. Parent directories are NOT
     * created here — creating a directory is a policy decision and it
     * belongs to whoever chose the path. */
    const char *path;

    /* Opaque identity of whatever produced the entries: driver build,
     * compiler flags, format version. It is not stored, only hashed into
     * the file header, and a file whose header disagrees is discarded
     * rather than read. This is what makes "the driver was updated" and
     * "the file is garbage" the same, already-tested code path.
     * May be NULL only when driver_key_size is 0. */
    const void *driver_key;
    size_t driver_key_size;

    /* Ceiling on the file, in bytes. 0 selects the default above.
     * Enforced by compaction: when a put would cross it, the live
     * entries are rewritten newest-first into as much of the budget as
     * they fit and the rest are dropped. */
    uint64_t max_size;

    /* Open without ever writing. A read-only cache still validates and
     * still serves hits; it just never truncates, appends or compacts.
     * Note that this means a corrupt tail stays on disk. */
    bool read_only;
} horizon_gpu_blob_cache_config;

typedef struct horizon_gpu_blob_cache_stats {
    uint32_t entries;      /* live data entries in the index            */
    uint32_t keys;         /* live key-only marks (put_key/has_key)     */
    uint64_t file_size;    /* bytes the file occupies right now         */
    uint64_t live_bytes;   /* payload bytes reachable through the index */
    uint32_t hits;         /* get() calls that returned data            */
    uint32_t misses;       /* get() calls that did not                  */
    uint32_t puts;         /* records appended by this handle           */
    uint32_t compactions;  /* times the file was rewritten for size     */

    /* What the last open() had to repair. Zero/false on a clean file;
     * a test asserts on these rather than on side effects, and a driver
     * can log them. */
    /* Bytes, not records: a damaged tail is by definition a run of
     * bytes that could not be parsed into records, so the number of
     * records in it is not knowable and reporting one would be a
     * guess. */
    uint64_t discarded_bytes;
    bool was_reset;           /* header unusable — started from empty   */
    bool was_truncated;       /* a damaged tail was cut off             */
} horizon_gpu_blob_cache_stats;

/* Open (creating if absent) the store at config->path.
 *
 * Never fails because the file is damaged: a file with an unusable
 * header is reset to an empty one, and a file whose record chain breaks
 * partway is truncated at the last intact record. Both are reported
 * through the stats, not through the result. It fails only on a bad
 * argument, on running out of memory, or on I/O the filesystem refused.
 *
 * On success *out owns the file handle until horizon_gpu_blob_cache_close.
 */
horizon_gpu_result
horizon_gpu_blob_cache_open(const horizon_gpu_blob_cache_config *config,
                            horizon_gpu_blob_cache **out);

/* Close and free. NULL is a no-op. */
void horizon_gpu_blob_cache_close(horizon_gpu_blob_cache *cache);

/* Store `size` bytes under `key`, replacing any previous entry for it.
 *
 * Returns HORIZON_GPU_ERR_STATE on a read-only cache,
 * HORIZON_GPU_ERR_INVALID_ARG for size 0 or above
 * HORIZON_GPU_BLOB_CACHE_MAX_ENTRY_SIZE, and HORIZON_GPU_ERR_IO if the
 * write did not reach the filesystem — in which case the index is left
 * untouched, so a failed put is a miss and never a wrong hit. */
horizon_gpu_result
horizon_gpu_blob_cache_put(horizon_gpu_blob_cache *cache,
                           const uint8_t *key,
                           const void *data, size_t size);

/* Retrieve the entry stored under `key`.
 *
 * On success *out_data is a malloc'd buffer the caller frees with
 * free(), and *out_size is its length. Returns
 * HORIZON_GPU_ERR_NOT_FOUND when there is no entry, when the payload
 * fails its CRC (a damaged entry is a miss, never a wrong answer), and
 * when the record cannot be read back. */
horizon_gpu_result
horizon_gpu_blob_cache_get(horizon_gpu_blob_cache *cache,
                           const uint8_t *key,
                           void **out_data, size_t *out_size);

/* Record `key` with no data attached.
 *
 * A separate namespace from put()/get(), because that is what Mesa's
 * disk_cache_put_key()/disk_cache_has_key() promise: "disk_cache_has_key()
 * will only return true for keys passed to disk_cache_put_key()"
 * (mesa/src/util/disk_cache.h). A key stored here is invisible to get(),
 * and an entry stored by put() is invisible to has_key(). */
horizon_gpu_result
horizon_gpu_blob_cache_put_key(horizon_gpu_blob_cache *cache,
                               const uint8_t *key);

/* Whether put_key() was called with `key` and the mark is still there. */
bool
horizon_gpu_blob_cache_has_key(horizon_gpu_blob_cache *cache,
                               const uint8_t *key);

/* Drop the entry stored under `key`. Absent is not an error. */
horizon_gpu_result
horizon_gpu_blob_cache_remove(horizon_gpu_blob_cache *cache,
                              const uint8_t *key);

/* Flush everything written so far all the way to the filesystem. put()
 * already does this per entry; this exists so a caller that has turned
 * that off can still make a durability point of its own. */
horizon_gpu_result horizon_gpu_blob_cache_flush(horizon_gpu_blob_cache *cache);

/* Copy out the counters. Both arguments must be non-NULL. */
void horizon_gpu_blob_cache_get_stats(const horizon_gpu_blob_cache *cache,
                                      horizon_gpu_blob_cache_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_GPU_BLOB_CACHE_H */
