/*
 * horizon_gpu — the keyed blob store declared in
 * horizon_gpu/blob_cache.h. Read that header first: it states what this
 * file is for, why the format is content-validated rather than
 * rename-atomic, and what the concurrency contract is.
 *
 * ON-DISK FORMAT, in one place so it can be checked against the code.
 * Every field is little-endian and is read and written byte by byte
 * through the get_u32le/put_u32le helpers below — never by casting a
 * buffer to a struct, which would make the format depend on this
 * compiler's padding and alignment rules.
 *
 *   file header, 64 bytes at offset 0
 *     0   magic[8]           "HZBLOBC1"
 *     8   format_version     u32, HORIZON_BC_FORMAT_VERSION
 *     12  header_size        u32, 64 + align8(driver_key_size)
 *     16  key_size           u32, HORIZON_GPU_BLOB_CACHE_KEY_SIZE
 *     20  driver_key_size    u32
 *     24  driver_key_crc     u32, crc32 of the driver key bytes
 *     28  state              u32, HORIZON_BC_STATE_*
 *     32  reserved[7]        u32 x 7
 *     60  header_crc         u32, crc32 over bytes [0, 60)
 *     64  driver key bytes, then zero padding to header_size
 *
 *   record, 56 bytes then payload then zero padding to a multiple of 8
 *     0   magic              u32, HORIZON_BC_REC_MAGIC
 *     4   flags              u32, HORIZON_BC_REC_*
 *     8   key[32]
 *     40  payload_size       u32
 *     44  payload_crc        u32, crc32 of the payload bytes
 *     48  seq                u32, monotonic within the file
 *     52  header_crc         u32, crc32 over bytes [0, 52)
 *
 * NO PATH OPERATION HAPPENS AFTER open(). Not one: no rename, no
 * remove, no second fopen, no stat. This is not tidiness. libnx's fsdev
 * devoptab routes every path-based entry point — open, stat, unlink,
 * mkdir, rename, diropen — through fsdev_fixpath(), which copies into
 * the single file-scope buffer `__nx_dev_path_buf` with no lock of any
 * kind (nx/source/runtime/devices/fs_dev.c: the buffer at line 211, and
 * `char *fs_path = __nx_dev_path_buf` at 405, 1111, 1197, 1225, 1276).
 * Two threads doing path work at once corrupt each other's path, and
 * the symptom is a write to the wrong file. Everything this module does
 * after open() goes through the FILE handle — read, write, seek,
 * ftruncate, fsync — none of which touches that buffer. So compaction
 * rewrites the file *in place* rather than doing the usual
 * write-temp-then-rename, and the caller may hand the open cache to a
 * worker thread without inheriting a data race it cannot see.
 *
 * WHY THE DRIVER KEY IS STORED WHOLE AND COMPARED BYTE FOR BYTE. A
 * digest would be smaller, and a digest collision here does not mean a
 * miss — it means handing a shader compiled by a different driver to
 * this one, which is a GPU fault with no error path. The key is a few
 * dozen bytes; there is no reason to be clever about it.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */

/* Before any include. fileno(), fsync() and ftruncate() are POSIX, and
 * the host test build compiles with -std=c11 (scripts/run-host-tests.sh),
 * where glibc hides all three unless a feature-test macro asks for them.
 * newlib reads the same macro (__POSIX_VISIBLE), so one define serves
 * both builds. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "horizon_gpu/blob_cache.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "crc32.h"

/* ------------------------------------------------------------------ */
/* Format constants                                                    */
/* ------------------------------------------------------------------ */

#define HORIZON_BC_FORMAT_VERSION 1u

#define HORIZON_BC_FILE_HDR_SIZE  64u
#define HORIZON_BC_REC_HDR_SIZE   56u

/* Record alignment. 8 keeps every record header 8-byte aligned in the
 * file, which is not required by this code (it reads bytes) but keeps
 * the format readable by anything that does map it. */
#define HORIZON_BC_ALIGN          8u

static const uint8_t horizon_bc_file_magic[8] = {
    'H', 'Z', 'B', 'L', 'O', 'B', 'C', '1'
};

/* "HZR1", little-endian. */
#define HORIZON_BC_REC_MAGIC UINT32_C(0x31525A48)

/* Record kinds. Exactly one of DATA/KEY/TOMBSTONE is set. */
#define HORIZON_BC_REC_DATA      UINT32_C(0x1)
#define HORIZON_BC_REC_KEY       UINT32_C(0x2)
#define HORIZON_BC_REC_TOMBSTONE UINT32_C(0x4)
#define HORIZON_BC_REC_KIND_MASK (HORIZON_BC_REC_DATA | \
                                  HORIZON_BC_REC_KEY | \
                                  HORIZON_BC_REC_TOMBSTONE)

/* File states, in the header at offset 28.
 *
 * Compaction rewrites live records towards the front of the file it is
 * already holding open, which means that between its first write and its
 * last the file is neither the old one nor the new one. CLEAN is stamped
 * and flushed only once it is the new one; a file found in COMPACTING
 * was interrupted — by a flat battery, or by Horizon killing the applet
 * — and cannot be parsed, so it is reset. That is the whole cost of an
 * interrupted compaction: a cold cache, and one recompile. */
#define HORIZON_BC_STATE_CLEAN      UINT32_C(0)
#define HORIZON_BC_STATE_COMPACTING UINT32_C(1)

/* Upper bound on the driver key, so a corrupt header cannot ask for an
 * allocation of four gigabytes before anything has been validated. */
#define HORIZON_BC_MAX_DRIVER_KEY 4096u

/* Index load factor, as a percentage, above which the table doubles. */
#define HORIZON_BC_INDEX_MAX_LOAD 70u

/* How full, as a percentage of max_size, a compaction leaves the file.
 *
 * 50: each rewrite hands back half the ceiling, so the next one is half
 * a file's worth of puts away instead of one put away. The cost is that
 * a cache in its steady state holds about half of what the ceiling
 * allows, which for a shader cache is entries that would have been
 * evicted soon anyway — the alternative measured 70 compactions for 100
 * puts on a console (run 27). */
#define HORIZON_BC_COMPACT_WATERMARK 50u

/* Smallest index the store ever allocates. Power of two. */
#define HORIZON_BC_INDEX_MIN_CAP 64u

/* stdio buffer, in bytes, installed on the cache file.
 *
 * WHY IT IS SET AND NOT LEFT TO THE C LIBRARY. Opening a cache means
 * walking every record header in the file, and newlib's default BUFSIZ
 * is 1024 — so a scan of a few thousand entries becomes a few thousand
 * fsFileRead calls into the FS sysmodule.
 *
 * IT IS ALSO THE THRESHOLD IN bc_skip_forward(). A buffer only helps a
 * scan that reads forward; run 27 measured the version that seeked to
 * every record at 201 us per entry, which is one I/O each and the exact
 * opposite of what this buffer is for. The scan now walks forward and
 * only seeks over a payload larger than one refill. */
#define HORIZON_BC_STREAM_BUF (64u * 1024u)

/* ------------------------------------------------------------------ */
/* Small helpers: little-endian access and checked arithmetic          */
/* ------------------------------------------------------------------ */

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b)
        return false;
    *out = a + b;
    return true;
}

/* Round `v` up to a multiple of HORIZON_BC_ALIGN, refusing to wrap. */
static bool align_up_u64(uint64_t v, uint64_t *out)
{
    uint64_t r = v % HORIZON_BC_ALIGN;

    if (r == 0) {
        *out = v;
        return true;
    }
    return add_u64(v, HORIZON_BC_ALIGN - r, out);
}

/* ------------------------------------------------------------------ */
/* The index                                                           */
/* ------------------------------------------------------------------ */

/* One slot per key, holding the two things a key can independently have.
 *
 * TWO HALVES AND NOT ONE KIND. The slot used to carry a single `flags`
 * that was either DATA or KEY, so storing data under a key that had been
 * marked with put_key() replaced the mark, and has_key() went back to
 * false. That contradicts what blob_cache.h promises and what Mesa's
 * disk_cache.h requires — the stored-key index is independent of the
 * cached entries, and upstream keeps them in genuinely separate storage.
 * Nothing in NVK calls put_key(), which is why no test had caught it.
 * The halves are separate now, and a test puts both on one key. */
typedef struct bc_slot {
    uint8_t  key[HORIZON_GPU_BLOB_CACHE_KEY_SIZE];

    /* The data entry, from put(). */
    uint64_t data_offset;
    uint32_t data_size;
    uint32_t data_seq;
    bool     has_data;

    /* The key-only mark, from put_key(). No payload, so only where its
     * record is — compaction has to be able to copy it. */
    uint64_t mark_offset;
    uint32_t mark_seq;
    bool     has_mark;

    bool     used;     /* has_data || has_mark */
} bc_slot;

/* One record's worth of index, which is what compaction moves. A slot
 * with both halves yields two of these. */
typedef struct bc_rec {
    uint8_t  key[HORIZON_GPU_BLOB_CACHE_KEY_SIZE];
    uint64_t offset;
    uint32_t size;
    uint32_t seq;
    uint32_t flags;    /* HORIZON_BC_REC_DATA or _KEY */
} bc_rec;

struct horizon_gpu_blob_cache {
    FILE    *file;
    char    *stream_buf;    /* owned; must outlive `file` (setvbuf)     */
    char    *path;

    uint8_t *driver_key;
    uint32_t driver_key_size;

    uint64_t header_size;   /* where the record chain starts */
    uint64_t write_off;     /* end of the last intact record */
    uint64_t max_size;
    uint32_t next_seq;
    bool     read_only;

    bc_slot *slots;
    uint32_t slot_cap;      /* power of two */
    uint32_t slot_used;

    horizon_gpu_blob_cache_stats stats;
};

/* The first four key bytes are already a hash: keys come from BLAKE3.
 * Reading them as a little-endian word rather than casting keeps this
 * defined on any alignment. */
static uint32_t bc_key_hash(const uint8_t *key)
{
    return get_u32le(key);
}

static bool bc_key_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, HORIZON_GPU_BLOB_CACHE_KEY_SIZE) == 0;
}

/* Find the slot holding `key`, or the first free slot on its probe
 * chain. Never fails: the table is kept below 100% load. */
static bc_slot *bc_index_probe(horizon_gpu_blob_cache *c, const uint8_t *key)
{
    uint32_t mask = c->slot_cap - 1u;
    uint32_t i = bc_key_hash(key) & mask;

    for (uint32_t n = 0; n < c->slot_cap; n++) {
        bc_slot *s = &c->slots[i];
        if (!s->used || bc_key_eq(s->key, key))
            return s;
        i = (i + 1u) & mask;
    }
    return NULL; /* unreachable while load < 100% */
}

static bool bc_index_grow(horizon_gpu_blob_cache *c, uint32_t new_cap)
{
    bc_slot *old = c->slots;
    uint32_t old_cap = c->slot_cap;

    bc_slot *fresh = calloc(new_cap, sizeof(*fresh));
    if (fresh == NULL)
        return false;

    c->slots = fresh;
    c->slot_cap = new_cap;
    c->slot_used = 0;

    for (uint32_t i = 0; i < old_cap; i++) {
        if (!old[i].used)
            continue;
        bc_slot *dst = bc_index_probe(c, old[i].key);
        *dst = old[i];
        c->slot_used++;
    }

    free(old);
    return true;
}

/* Insert or replace one half of a key's slot. `flags` is DATA or KEY;
 * the other half is left exactly as it was. */
static bool bc_index_set(horizon_gpu_blob_cache *c, const uint8_t *key,
                         uint64_t offset, uint32_t size, uint32_t seq,
                         uint32_t flags)
{
    if ((uint64_t)(c->slot_used + 1u) * 100u >
        (uint64_t)c->slot_cap * HORIZON_BC_INDEX_MAX_LOAD) {
        if (c->slot_cap > UINT32_MAX / 2u)
            return false;
        if (!bc_index_grow(c, c->slot_cap * 2u))
            return false;
    }

    bc_slot *s = bc_index_probe(c, key);
    if (s == NULL)
        return false;

    if (!s->used) {
        memcpy(s->key, key, HORIZON_GPU_BLOB_CACHE_KEY_SIZE);
        s->used = true;
        c->slot_used++;
    }

    if (flags == HORIZON_BC_REC_DATA) {
        s->data_offset = offset;
        s->data_size = size;
        s->data_seq = seq;
        s->has_data = true;
    } else {
        s->mark_offset = offset;
        s->mark_seq = seq;
        s->has_mark = true;
    }
    return true;
}

/* Look up a live half. NULL when the key has nothing of that kind. */
static bc_slot *bc_index_find(horizon_gpu_blob_cache *c, const uint8_t *key,
                              uint32_t kind)
{
    if (c->slot_used == 0)
        return NULL;

    bc_slot *s = bc_index_probe(c, key);
    if (s == NULL || !s->used)
        return NULL;
    if (kind == HORIZON_BC_REC_DATA ? !s->has_data : !s->has_mark)
        return NULL;
    return s;
}

/* Drop one half of a key's slot, keeping the other.
 *
 * Deleting from an open-addressed table cannot just clear the slot —
 * that would cut the probe chains running through it. The slot is
 * cleared and everything after it in the same cluster is reinserted,
 * which is the standard repair and is cheap at this load factor. That
 * only happens once *both* halves are gone. */
static void bc_index_erase(horizon_gpu_blob_cache *c, const uint8_t *key,
                           uint32_t kind)
{
    bc_slot *s = bc_index_probe(c, key);
    if (s == NULL || !s->used)
        return;

    if (kind == HORIZON_BC_REC_DATA)
        s->has_data = false;
    else
        s->has_mark = false;

    if (s->has_data || s->has_mark)
        return;

    uint32_t mask = c->slot_cap - 1u;
    uint32_t i = (uint32_t)(s - c->slots);

    c->slots[i].used = false;
    c->slot_used--;

    uint32_t j = (i + 1u) & mask;
    while (c->slots[j].used) {
        bc_slot moved = c->slots[j];
        c->slots[j].used = false;
        c->slot_used--;
        bc_slot *dst = bc_index_probe(c, moved.key);
        *dst = moved;
        c->slot_used++;
        j = (j + 1u) & mask;
    }
}

/* ------------------------------------------------------------------ */
/* File primitives                                                     */
/* ------------------------------------------------------------------ */

static bool bc_seek(FILE *f, uint64_t off)
{
    /* long is 32-bit on some targets; the cache is bounded well below
     * 2 GiB by HORIZON_GPU_BLOB_CACHE_MAX_ENTRY_SIZE and max_size, but
     * a corrupt header could still produce a large number, so the
     * conversion is checked rather than assumed. */
    if (off > (uint64_t)LONG_MAX)
        return false;
    return fseek(f, (long)off, SEEK_SET) == 0;
}

static bool bc_read_at(FILE *f, uint64_t off, void *buf, size_t len)
{
    if (!bc_seek(f, off))
        return false;
    return fread(buf, 1, len, f) == len;
}

/* Read the next `len` bytes from wherever the stream already is.
 *
 * WHY THE SCAN USES THIS AND NOT bc_read_at(). Run 27 opened a 201-entry
 * cache on a console in 40498 us — 201 us per record, to read a 56-byte
 * header out of a 50 KB file. That is one I/O per record, not a
 * sequential read, and the reason is the fseek in bc_read_at(): whether
 * a C library keeps its buffer across a seek is a property of that
 * library, and newlib gates the optimisation on its own seek function
 * and on fstat describing a regular file with a block size — libnx's
 * fstat sets S_IFREG and leaves st_blksize alone. Rather than depend on
 * how that resolves, the scan never seeks: it reads forward, and the
 * buffer beneath it is used the way a buffer is meant to be. */
static bool bc_read_next(FILE *f, void *buf, size_t len)
{
    return fread(buf, 1, len, f) == len;
}

/* Step `len` bytes forward, consuming them or seeking over them —
 * whichever moves fewer bytes.
 *
 * Consuming keeps the stream buffer, and for a payload smaller than the
 * buffer it is strictly cheaper: the bytes were going to be fetched
 * anyway as part of the block the *next* header sits in. A seek throws
 * the buffer away and the next read refills all of it, so it only pays
 * once the payload is larger than a refill — at which point consuming
 * would mean reading a whole shader binary just to get past it. The
 * threshold is therefore the buffer size, and it is a comparison and not
 * a guess. */
static bool bc_skip_forward(FILE *f, uint64_t at, uint64_t len)
{
    uint8_t sink[512];

    if (len > HORIZON_BC_STREAM_BUF)
        return bc_seek(f, at + len);

    while (len > 0) {
        size_t chunk = len < sizeof(sink) ? (size_t)len : sizeof(sink);

        if (fread(sink, 1, chunk, f) != chunk)
            return false;
        len -= chunk;
    }
    return true;
}

static bool bc_file_size(FILE *f, uint64_t *out)
{
    if (fseek(f, 0, SEEK_END) != 0)
        return false;
    long end = ftell(f);
    if (end < 0)
        return false;
    *out = (uint64_t)end;
    return true;
}

/* Push everything as far towards the storage device as the platform
 * allows. fflush() empties the C library's buffer; fsync() is what
 * makes the FS sysmodule commit it on Horizon (libnx fsdev_fsync ->
 * fsFileFlush). */
static bool bc_sync(FILE *f)
{
    if (fflush(f) != 0)
        return false;

    int fd = fileno(f);
    if (fd < 0)
        return false;

    return fsync(fd) == 0;
}

/* ------------------------------------------------------------------ */
/* Header and record serialisation                                     */
/* ------------------------------------------------------------------ */

static void bc_build_file_header(uint8_t hdr[HORIZON_BC_FILE_HDR_SIZE],
                                 uint64_t header_size,
                                 const uint8_t *driver_key,
                                 uint32_t driver_key_size,
                                 uint32_t state)
{
    memset(hdr, 0, HORIZON_BC_FILE_HDR_SIZE);
    memcpy(hdr, horizon_bc_file_magic, sizeof(horizon_bc_file_magic));
    put_u32le(hdr + 8, HORIZON_BC_FORMAT_VERSION);
    put_u32le(hdr + 12, (uint32_t)header_size);
    put_u32le(hdr + 16, HORIZON_GPU_BLOB_CACHE_KEY_SIZE);
    put_u32le(hdr + 20, driver_key_size);
    put_u32le(hdr + 24, horizon_crc32(driver_key, driver_key_size));
    put_u32le(hdr + 28, state);
    put_u32le(hdr + 60, horizon_crc32(hdr, 60));
}

/* Write a fresh, empty file: header plus the driver key, nothing else.
 * Used both when the file does not exist and when what is there cannot
 * be trusted. */
static horizon_gpu_result bc_write_fresh_header(horizon_gpu_blob_cache *c)
{
    uint8_t hdr[HORIZON_BC_FILE_HDR_SIZE];
    uint64_t key_span = c->header_size - HORIZON_BC_FILE_HDR_SIZE;

    bc_build_file_header(hdr, c->header_size, c->driver_key,
                         c->driver_key_size, HORIZON_BC_STATE_CLEAN);

    if (!bc_seek(c->file, 0))
        return horizon_gpu_err(HORIZON_GPU_ERR_IO);
    if (fwrite(hdr, 1, sizeof(hdr), c->file) != sizeof(hdr))
        return horizon_gpu_err(HORIZON_GPU_ERR_IO);

    if (key_span > 0) {
        uint8_t pad[HORIZON_BC_ALIGN] = { 0 };

        if (c->driver_key_size > 0 &&
            fwrite(c->driver_key, 1, c->driver_key_size, c->file) !=
                c->driver_key_size)
            return horizon_gpu_err(HORIZON_GPU_ERR_IO);

        size_t padding = (size_t)(key_span - c->driver_key_size);
        if (padding > 0 && fwrite(pad, 1, padding, c->file) != padding)
            return horizon_gpu_err(HORIZON_GPU_ERR_IO);
    }

    if (!bc_sync(c->file))
        return horizon_gpu_err(HORIZON_GPU_ERR_IO);

    /* Anything that used to be beyond the header is not ours. */
    if (ftruncate(fileno(c->file), (off_t)c->header_size) != 0)
        return horizon_gpu_err(HORIZON_GPU_ERR_IO);

    c->write_off = c->header_size;
    c->next_seq = 1;
    return horizon_gpu_ok();
}

/* Is the header on disk one we wrote, for this driver key? */
static bool bc_header_matches(horizon_gpu_blob_cache *c, uint64_t file_size)
{
    uint8_t hdr[HORIZON_BC_FILE_HDR_SIZE];

    if (file_size < HORIZON_BC_FILE_HDR_SIZE)
        return false;
    if (!bc_read_at(c->file, 0, hdr, sizeof(hdr)))
        return false;
    if (memcmp(hdr, horizon_bc_file_magic, sizeof(horizon_bc_file_magic)) != 0)
        return false;
    if (get_u32le(hdr + 60) != horizon_crc32(hdr, 60))
        return false;
    if (get_u32le(hdr + 8) != HORIZON_BC_FORMAT_VERSION)
        return false;
    if (get_u32le(hdr + 28) != HORIZON_BC_STATE_CLEAN)
        return false; /* a compaction was interrupted; see the state doc */
    if (get_u32le(hdr + 16) != HORIZON_GPU_BLOB_CACHE_KEY_SIZE)
        return false;
    if (get_u32le(hdr + 20) != c->driver_key_size)
        return false;
    if (get_u32le(hdr + 12) != (uint32_t)c->header_size)
        return false;
    if (file_size < c->header_size)
        return false;
    if (get_u32le(hdr + 24) !=
        horizon_crc32(c->driver_key, c->driver_key_size))
        return false;

    /* The CRC above is a pre-filter. The comparison that decides is the
     * bytes themselves — see the file banner. */
    if (c->driver_key_size > 0) {
        uint8_t *disk = malloc(c->driver_key_size);
        if (disk == NULL)
            return false;
        bool ok = bc_read_at(c->file, HORIZON_BC_FILE_HDR_SIZE, disk,
                             c->driver_key_size) &&
                  memcmp(disk, c->driver_key, c->driver_key_size) == 0;
        free(disk);
        if (!ok)
            return false;
    }

    return true;
}

/* Total bytes a record with `payload` bytes of payload occupies. */
static bool bc_record_span(uint64_t payload, uint64_t *out)
{
    uint64_t total;

    if (!add_u64(HORIZON_BC_REC_HDR_SIZE, payload, &total))
        return false;
    return align_up_u64(total, out);
}

/* ------------------------------------------------------------------ */
/* Opening: scan, validate, repair                                     */
/* ------------------------------------------------------------------ */

/* Walk the record chain from header_size, feeding the index, and stop at
 * the first record that does not validate. Sets c->write_off to the end
 * of the last intact record. */
static horizon_gpu_result bc_scan(horizon_gpu_blob_cache *c,
                                  uint64_t file_size)
{
    uint8_t hdr[HORIZON_BC_REC_HDR_SIZE];
    uint64_t off = c->header_size;

    c->next_seq = 1;

    /* The one seek in the whole scan: to its start. Everything after
     * this walks forward. */
    if (!bc_seek(c->file, off))
        return horizon_gpu_err(HORIZON_GPU_ERR_IO);

    for (;;) {
        uint64_t hdr_end;

        if (!add_u64(off, HORIZON_BC_REC_HDR_SIZE, &hdr_end))
            break;
        if (hdr_end > file_size)
            break;
        if (!bc_read_next(c->file, hdr, sizeof(hdr)))
            break;

        if (get_u32le(hdr + 0) != HORIZON_BC_REC_MAGIC)
            break;
        if (get_u32le(hdr + 52) != horizon_crc32(hdr, 52))
            break;

        uint32_t flags = get_u32le(hdr + 4);
        uint32_t payload = get_u32le(hdr + 40);
        uint32_t seq = get_u32le(hdr + 48);

        if ((flags & HORIZON_BC_REC_KIND_MASK) != flags)
            break;
        if (flags != HORIZON_BC_REC_DATA && flags != HORIZON_BC_REC_KEY &&
            flags != HORIZON_BC_REC_TOMBSTONE)
            break;
        if (flags != HORIZON_BC_REC_DATA && payload != 0)
            break;
        if ((uint64_t)payload > HORIZON_GPU_BLOB_CACHE_MAX_ENTRY_SIZE)
            break;

        uint64_t span, rec_end;
        if (!bc_record_span(payload, &span))
            break;
        if (!add_u64(off, span, &rec_end))
            break;
        if (rec_end > file_size)
            break;

        const uint8_t *key = hdr + 8;
        if (flags == HORIZON_BC_REC_TOMBSTONE) {
            /* remove() is disk_cache_remove(): it drops the entry, and
             * a key-only mark is not an entry. */
            bc_index_erase(c, key, HORIZON_BC_REC_DATA);
        } else if (!bc_index_set(c, key, off, payload, seq, flags)) {
            return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);
        }

        if (seq >= c->next_seq)
            c->next_seq = seq + 1u;

        /* Step over the payload without giving up the buffer when the
         * payload is small enough for that to be the cheaper move. */
        if (!bc_skip_forward(c->file, off + HORIZON_BC_REC_HDR_SIZE,
                             span - HORIZON_BC_REC_HDR_SIZE))
            break;

        off = rec_end;
    }

    c->write_off = off;

    if (off != file_size) {
        c->stats.was_truncated = true;
        c->stats.discarded_bytes = file_size - off;

        if (!c->read_only) {
            if (ftruncate(fileno(c->file), (off_t)off) != 0)
                return horizon_gpu_err(HORIZON_GPU_ERR_IO);
        }
    }

    return horizon_gpu_ok();
}

static void bc_recompute_live(horizon_gpu_blob_cache *c)
{
    c->stats.entries = 0;
    c->stats.keys = 0;
    c->stats.live_bytes = 0;

    for (uint32_t i = 0; i < c->slot_cap; i++) {
        if (!c->slots[i].used)
            continue;
        if (c->slots[i].has_data) {
            c->stats.entries++;
            c->stats.live_bytes += c->slots[i].data_size;
        }
        if (c->slots[i].has_mark)
            c->stats.keys++;
    }
}

static void bc_free(horizon_gpu_blob_cache *c)
{
    if (c == NULL)
        return;
    if (c->file != NULL)
        fclose(c->file);
    /* After fclose, never before: setvbuf hands this buffer to stdio. */
    free(c->stream_buf);
    free(c->path);
    free(c->driver_key);
    free(c->slots);
    free(c);
}

horizon_gpu_result
horizon_gpu_blob_cache_open(const horizon_gpu_blob_cache_config *config,
                            horizon_gpu_blob_cache **out)
{
    if (out == NULL)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    *out = NULL;

    if (config == NULL || config->path == NULL || config->path[0] == '\0')
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (config->driver_key == NULL && config->driver_key_size != 0)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (config->driver_key_size > HORIZON_BC_MAX_DRIVER_KEY)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    horizon_gpu_blob_cache *c = calloc(1, sizeof(*c));
    if (c == NULL)
        return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);

    c->read_only = config->read_only;
    c->max_size = config->max_size != 0
                      ? config->max_size
                      : HORIZON_GPU_BLOB_CACHE_DEFAULT_MAX_SIZE;
    c->driver_key_size = (uint32_t)config->driver_key_size;

    size_t path_len = strlen(config->path);
    c->path = malloc(path_len + 1);
    if (c->path == NULL) {
        bc_free(c);
        return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);
    }
    memcpy(c->path, config->path, path_len + 1);

    if (c->driver_key_size > 0) {
        c->driver_key = malloc(c->driver_key_size);
        if (c->driver_key == NULL) {
            bc_free(c);
            return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);
        }
        memcpy(c->driver_key, config->driver_key, c->driver_key_size);
    }

    uint64_t key_span;
    if (!align_up_u64(c->driver_key_size, &key_span) ||
        !add_u64(HORIZON_BC_FILE_HDR_SIZE, key_span, &c->header_size)) {
        bc_free(c);
        return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);
    }

    c->slots = calloc(HORIZON_BC_INDEX_MIN_CAP, sizeof(*c->slots));
    if (c->slots == NULL) {
        bc_free(c);
        return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);
    }
    c->slot_cap = HORIZON_BC_INDEX_MIN_CAP;

    /* "r+b" first so an existing file is not destroyed by the attempt to
     * open it; only if that fails is a new one created.
     *
     * A read-only cache asks for "rb" and never creates. Opening for
     * update would demand write permission the caller has said it does
     * not want and may not have — a genuinely read-only file, or a card
     * mounted read-only, would fail to open at all, and a cache that
     * advertises read-only service would then serve nothing. */
    bool created = false;

    c->file = fopen(c->path, c->read_only ? "rb" : "r+b");
    if (c->file == NULL) {
        if (c->read_only) {
            bc_free(c);
            return horizon_gpu_err(HORIZON_GPU_ERR_IO);
        }
        c->file = fopen(c->path, "w+b");
        if (c->file == NULL) {
            bc_free(c);
            return horizon_gpu_err(HORIZON_GPU_ERR_IO);
        }
        created = true;
    }

    c->stream_buf = malloc(HORIZON_BC_STREAM_BUF);
    if (c->stream_buf != NULL &&
        setvbuf(c->file, c->stream_buf, _IOFBF, HORIZON_BC_STREAM_BUF) != 0) {
        /* Refused: stdio keeps whatever buffer it already had, which is
         * correct but slower. Not a failure — the cache works either
         * way, and saying so is better than failing to open over it. */
        free(c->stream_buf);
        c->stream_buf = NULL;
    }

    uint64_t file_size = 0;
    if (!bc_file_size(c->file, &file_size)) {
        bc_free(c);
        return horizon_gpu_err(HORIZON_GPU_ERR_IO);
    }

    if (bc_header_matches(c, file_size)) {
        horizon_gpu_result r = bc_scan(c, file_size);
        if (horizon_gpu_failed(r)) {
            bc_free(c);
            return r;
        }
    } else {
        /* Not ours, wrong version, damaged, or written by a different
         * driver build. All four are the same thing to us: what is here
         * cannot be read, so it is replaced.
         *
         * `created` separates the one case that is not a repair: a file
         * that did not exist a moment ago has nothing to reset, and a
         * caller reading was_reset to decide whether to warn should not
         * be told that a first run threw something away. */
        c->stats.was_reset = !created;
        c->stats.discarded_bytes = file_size;

        if (c->read_only) {
            /* Nothing to serve, and nothing may be written. Present an
             * empty cache rather than failing: a read-only cache that
             * cannot be read is still a cache that misses. */
            c->write_off = c->header_size;
            c->next_seq = 1;
        } else {
            horizon_gpu_result r = bc_write_fresh_header(c);
            if (horizon_gpu_failed(r)) {
                bc_free(c);
                return r;
            }
        }
    }

    bc_recompute_live(c);
    c->stats.file_size = c->write_off;

    *out = c;
    return horizon_gpu_ok();
}

void horizon_gpu_blob_cache_close(horizon_gpu_blob_cache *cache)
{
    bc_free(cache);
}

/* ------------------------------------------------------------------ */
/* Appending                                                           */
/* ------------------------------------------------------------------ */

static horizon_gpu_result bc_compact(horizon_gpu_blob_cache *c,
                                     uint64_t incoming);

/* Append one record. `data` may be NULL when size is 0 (KEY, TOMBSTONE).
 *
 * Nothing about the index is touched until the bytes are on disk, so a
 * failed append leaves a cache that misses rather than one that lies. */
static horizon_gpu_result bc_append(horizon_gpu_blob_cache *c,
                                    const uint8_t *key, uint32_t flags,
                                    const void *data, uint32_t size)
{
    uint8_t hdr[HORIZON_BC_REC_HDR_SIZE];
    uint8_t pad[HORIZON_BC_ALIGN] = { 0 };
    uint64_t span;

    if (!bc_record_span(size, &span))
        return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);

    uint64_t new_end;
    if (!add_u64(c->write_off, span, &new_end))
        return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);

    if (new_end > c->max_size) {
        horizon_gpu_result r = bc_compact(c, span);
        if (horizon_gpu_failed(r))
            return r;
        if (!add_u64(c->write_off, span, &new_end))
            return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);
    }

    uint32_t seq = c->next_seq;
    uint64_t at = c->write_off;

    memset(hdr, 0, sizeof(hdr));
    put_u32le(hdr + 0, HORIZON_BC_REC_MAGIC);
    put_u32le(hdr + 4, flags);
    memcpy(hdr + 8, key, HORIZON_GPU_BLOB_CACHE_KEY_SIZE);
    put_u32le(hdr + 40, size);
    put_u32le(hdr + 44, horizon_crc32(data, size));
    put_u32le(hdr + 48, seq);
    put_u32le(hdr + 52, horizon_crc32(hdr, 52));

    size_t padding = (size_t)(span - HORIZON_BC_REC_HDR_SIZE - size);
    bool ok = bc_seek(c->file, at) &&
              fwrite(hdr, 1, sizeof(hdr), c->file) == sizeof(hdr) &&
              (size == 0 || fwrite(data, 1, size, c->file) == size) &&
              (padding == 0 || fwrite(pad, 1, padding, c->file) == padding) &&
              bc_sync(c->file);

    if (!ok) {
        /* Put the file back the way it was, so the next open does not
         * have to discover the stump. Best effort: if even this fails
         * the stump is still harmless, because it will not validate. */
        int rewound = ftruncate(fileno(c->file), (off_t)at);
        (void)rewound; /* best effort; see the comment above */
        (void)fflush(c->file);
        return horizon_gpu_err(HORIZON_GPU_ERR_IO);
    }

    if (flags == HORIZON_BC_REC_TOMBSTONE) {
        bc_index_erase(c, key, HORIZON_BC_REC_DATA);
    } else if (!bc_index_set(c, key, at, size, seq, flags)) {
        /* On disk and unreachable. Not a lie — the next open will index
         * it — but this handle must say something went wrong. */
        c->write_off = new_end;
        c->next_seq = seq + 1u;
        return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);
    }

    c->write_off = new_end;
    c->next_seq = seq + 1u;
    c->stats.puts++;
    c->stats.file_size = c->write_off;
    bc_recompute_live(c);

    return horizon_gpu_ok();
}

/* ------------------------------------------------------------------ */
/* Compaction                                                          */
/* ------------------------------------------------------------------ */

static int bc_slot_cmp_newest_first(const void *a, const void *b)
{
    const bc_rec *x = a, *y = b;

    if (x->seq > y->seq)
        return -1;
    if (x->seq < y->seq)
        return 1;
    return 0;
}

/* Order two snapshot slots by file offset, ascending. */
static int bc_slot_cmp_offset(const void *a, const void *b)
{
    const bc_rec *x = a, *y = b;

    if (x->offset < y->offset)
        return -1;
    if (x->offset > y->offset)
        return 1;
    return 0;
}

/* Copy `len` bytes from `src` to `dst` within the same open file, using
 * a bounded staging buffer. Only ever called with dst <= src (see
 * bc_compact), so the copy runs forwards without overwriting what it has
 * not read yet. */
static bool bc_move_range(FILE *f, uint64_t dst, uint64_t src, uint64_t len)
{
    uint8_t buf[4096];

    while (len > 0) {
        size_t chunk = len < sizeof(buf) ? (size_t)len : sizeof(buf);

        if (!bc_read_at(f, src, buf, chunk))
            return false;
        if (!bc_seek(f, dst) || fwrite(buf, 1, chunk, f) != chunk)
            return false;

        src += chunk;
        dst += chunk;
        len -= chunk;
    }
    return true;
}

/* Reclaim space by rewriting the file in place, keeping the newest live
 * entries that fit in max_size with room left for `incoming` bytes.
 *
 * WHY IN PLACE, when write-a-new-file-and-rename is the usual answer.
 * Because renaming is a path operation, and path operations are exactly
 * what this module may not do after open() — libnx routes all of them
 * through one unlocked global buffer (see the file banner). Working on
 * the open handle also removes the window where neither file is the
 * cache: there is only ever one file, and the header says whether it can
 * be trusted.
 *
 * WHY IT IS SAFE TO OVERWRITE THE FILE IT IS READING. Records are copied
 * in increasing source order, and the destination advances only over the
 * records that are kept while the source advances over all of them. So
 * dst <= src at every step, and a forward copy never overwrites bytes it
 * has still to read. The check is asserted below rather than assumed.
 *
 * WHY A CRASH HERE IS SURVIVABLE. The header is stamped COMPACTING and
 * flushed before the first byte moves, and CLEAN only once the last one
 * has. A file found in COMPACTING is reset on open, because it is
 * genuinely neither the old cache nor the new one. */
static horizon_gpu_result bc_compact(horizon_gpu_blob_cache *c,
                                     uint64_t incoming)
{
    uint8_t fhdr[HORIZON_BC_FILE_HDR_SIZE];
    bc_rec *live = NULL;
    uint32_t live_n = 0;

    if (c->read_only)
        return horizon_gpu_err(HORIZON_GPU_ERR_STATE);

    /* Budget: a WATERMARK BELOW THE CEILING, less the record about to be
     * appended.
     *
     * Not the ceiling itself, and run 27 is why. Compacting to the
     * ceiling leaves the file exactly at it, so the very next put
     * crosses it and compacts again: 100 puts into a 32 KiB cache
     * measured 70 compactions and 2.58 s on a console, ~36 ms each. That
     * is the steady state of any full cache, not a corner of the test.
     * Compacting to a fraction of the ceiling means each rewrite buys
     * back that much room and the next one is (1 - fraction) of the file
     * away.
     *
     * A single entry larger than the whole budget is still written by
     * the caller afterwards — the ceiling is a policy on accumulation,
     * not a reason to refuse the one thing somebody asked to store. */
    uint64_t watermark = c->max_size / 100u * HORIZON_BC_COMPACT_WATERMARK;
    uint64_t budget = watermark > incoming ? watermark - incoming : 0;

    if (c->slot_used > 0) {
        /* Two per slot at most: a key can carry both an entry and a
         * key-only mark, and each is its own record on disk. */
        live = calloc((size_t)c->slot_used * 2u, sizeof(*live));
        if (live == NULL)
            return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);

        for (uint32_t i = 0; i < c->slot_cap; i++) {
            const bc_slot *sl = &c->slots[i];

            if (!sl->used)
                continue;
            if (sl->has_data) {
                bc_rec *r = &live[live_n++];
                memcpy(r->key, sl->key, HORIZON_GPU_BLOB_CACHE_KEY_SIZE);
                r->offset = sl->data_offset;
                r->size = sl->data_size;
                r->seq = sl->data_seq;
                r->flags = HORIZON_BC_REC_DATA;
            }
            if (sl->has_mark) {
                bc_rec *r = &live[live_n++];
                memcpy(r->key, sl->key, HORIZON_GPU_BLOB_CACHE_KEY_SIZE);
                r->offset = sl->mark_offset;
                r->size = 0;
                r->seq = sl->mark_seq;
                r->flags = HORIZON_BC_REC_KEY;
            }
        }

        /* Newest first, so that what survives is what was written most
         * recently. */
        qsort(live, live_n, sizeof(*live), bc_slot_cmp_newest_first);
    }

    /* Choose the survivors against the budget, then put them back into
     * file order: the copy below depends on ascending source offsets. */
    uint32_t keep_n = 0;
    uint64_t kept_bytes = c->header_size;

    for (uint32_t i = 0; i < live_n; i++) {
        uint32_t size = live[i].flags == HORIZON_BC_REC_DATA ? live[i].size : 0;
        uint64_t span;

        if (!bc_record_span(size, &span))
            continue;
        if (kept_bytes + span > budget)
            continue; /* older and smaller entries may still fit */

        live[keep_n++] = live[i];
        kept_bytes += span;
    }
    /* Guarded: qsort's first argument is declared non-null, so passing
     * the NULL of an empty snapshot is undefined even with a count of
     * zero — UBSan says so, and it is right. */
    if (keep_n > 0)
        qsort(live, keep_n, sizeof(*live), bc_slot_cmp_offset);

    /* Stamp COMPACTING before anything moves. */
    bc_build_file_header(fhdr, c->header_size, c->driver_key,
                         c->driver_key_size, HORIZON_BC_STATE_COMPACTING);
    if (!bc_seek(c->file, 0) ||
        fwrite(fhdr, 1, sizeof(fhdr), c->file) != sizeof(fhdr) ||
        !bc_sync(c->file)) {
        free(live);
        return horizon_gpu_err(HORIZON_GPU_ERR_IO);
    }

    horizon_gpu_result res = horizon_gpu_ok();
    uint64_t dst = c->header_size;

    for (uint32_t i = 0; i < keep_n; i++) {
        uint32_t size = live[i].flags == HORIZON_BC_REC_DATA ? live[i].size : 0;
        uint64_t span;

        if (!bc_record_span(size, &span)) {
            res = horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);
            break;
        }
        if (dst > live[i].offset) {
            /* The invariant above failed, which would mean the copy is
             * about to eat its own input. Stop rather than corrupt. */
            res = horizon_gpu_err(HORIZON_GPU_ERR_STATE);
            break;
        }
        if (dst != live[i].offset &&
            !bc_move_range(c->file, dst, live[i].offset, span)) {
            res = horizon_gpu_err(HORIZON_GPU_ERR_IO);
            break;
        }
        dst += span;
    }

    if (horizon_gpu_succeeded(res)) {
        if (fflush(c->file) != 0 ||
            ftruncate(fileno(c->file), (off_t)dst) != 0 || !bc_sync(c->file))
            res = horizon_gpu_err(HORIZON_GPU_ERR_IO);
    }

    free(live);

    if (horizon_gpu_failed(res)) {
        /* The header still says COMPACTING and stays that way: the next
         * open resets the file, which is the honest outcome for a
         * rewrite that stopped halfway. */
        return res;
    }

    /* Back to CLEAN, and only now. */
    bc_build_file_header(fhdr, c->header_size, c->driver_key,
                         c->driver_key_size, HORIZON_BC_STATE_CLEAN);
    if (!bc_seek(c->file, 0) ||
        fwrite(fhdr, 1, sizeof(fhdr), c->file) != sizeof(fhdr) ||
        !bc_sync(c->file))
        return horizon_gpu_err(HORIZON_GPU_ERR_IO);

    /* Rebuild the index from what is on disk rather than from what we
     * believe we wrote. The records kept their sequence numbers, so
     * newest-wins still means the same thing afterwards. */
    memset(c->slots, 0, (size_t)c->slot_cap * sizeof(*c->slots));
    c->slot_used = 0;

    uint64_t file_size = 0;
    if (!bc_file_size(c->file, &file_size))
        return horizon_gpu_err(HORIZON_GPU_ERR_IO);

    horizon_gpu_result r = bc_scan(c, file_size);
    if (horizon_gpu_failed(r))
        return r;

    c->stats.compactions++;
    bc_recompute_live(c);
    c->stats.file_size = c->write_off;

    return horizon_gpu_ok();
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

horizon_gpu_result
horizon_gpu_blob_cache_put(horizon_gpu_blob_cache *cache, const uint8_t *key,
                           const void *data, size_t size)
{
    if (cache == NULL || key == NULL || data == NULL)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (size == 0 || (uint64_t)size > HORIZON_GPU_BLOB_CACHE_MAX_ENTRY_SIZE)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (cache->read_only)
        return horizon_gpu_err(HORIZON_GPU_ERR_STATE);

    return bc_append(cache, key, HORIZON_BC_REC_DATA, data, (uint32_t)size);
}

horizon_gpu_result
horizon_gpu_blob_cache_get(horizon_gpu_blob_cache *cache, const uint8_t *key,
                           void **out_data, size_t *out_size)
{
    if (cache == NULL || key == NULL || out_data == NULL)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    *out_data = NULL;
    if (out_size != NULL)
        *out_size = 0;

    bc_slot *s = bc_index_find(cache, key, HORIZON_BC_REC_DATA);
    if (s == NULL) {
        cache->stats.misses++;
        return horizon_gpu_err(HORIZON_GPU_ERR_NOT_FOUND);
    }

    uint8_t hdr[HORIZON_BC_REC_HDR_SIZE];
    uint32_t size = s->data_size;
    uint64_t off = s->data_offset;

    /* Re-read and re-validate the record header rather than trusting the
     * index. The index is memory; the file is what somebody else's
     * homebrew, a bad card or a yanked console may have changed. */
    if (!bc_read_at(cache->file, off, hdr, sizeof(hdr)) ||
        get_u32le(hdr + 0) != HORIZON_BC_REC_MAGIC ||
        get_u32le(hdr + 52) != horizon_crc32(hdr, 52) ||
        get_u32le(hdr + 4) != HORIZON_BC_REC_DATA ||
        get_u32le(hdr + 40) != size ||
        memcmp(hdr + 8, key, HORIZON_GPU_BLOB_CACHE_KEY_SIZE) != 0) {
        bc_index_erase(cache, key, HORIZON_BC_REC_DATA);
        bc_recompute_live(cache);
        cache->stats.misses++;
        return horizon_gpu_err(HORIZON_GPU_ERR_NOT_FOUND);
    }

    uint8_t *buf = malloc(size);
    if (buf == NULL)
        return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);

    if (fread(buf, 1, size, cache->file) != size ||
        horizon_crc32(buf, size) != get_u32le(hdr + 44)) {
        /* A payload that fails its checksum is a miss, never an answer.
         * It is also dropped from the index so the next lookup does not
         * pay for the same disappointment. */
        free(buf);
        bc_index_erase(cache, key, HORIZON_BC_REC_DATA);
        bc_recompute_live(cache);
        cache->stats.misses++;
        return horizon_gpu_err(HORIZON_GPU_ERR_NOT_FOUND);
    }

    *out_data = buf;
    if (out_size != NULL)
        *out_size = size;
    cache->stats.hits++;
    return horizon_gpu_ok();
}

horizon_gpu_result
horizon_gpu_blob_cache_put_key(horizon_gpu_blob_cache *cache,
                               const uint8_t *key)
{
    if (cache == NULL || key == NULL)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (cache->read_only)
        return horizon_gpu_err(HORIZON_GPU_ERR_STATE);

    return bc_append(cache, key, HORIZON_BC_REC_KEY, NULL, 0);
}

bool horizon_gpu_blob_cache_has_key(horizon_gpu_blob_cache *cache,
                                    const uint8_t *key)
{
    if (cache == NULL || key == NULL)
        return false;

    return bc_index_find(cache, key, HORIZON_BC_REC_KEY) != NULL;
}

horizon_gpu_result
horizon_gpu_blob_cache_remove(horizon_gpu_blob_cache *cache,
                              const uint8_t *key)
{
    if (cache == NULL || key == NULL)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (cache->read_only)
        return horizon_gpu_err(HORIZON_GPU_ERR_STATE);

    /* Absent is not an error, and it is also not a write: a tombstone
     * for something that was never there would grow the file for
     * nothing. */
    /* Absent is not an error, and it is also not a write. A key that
     * only carries a mark has no entry to remove. */
    bc_slot *s = bc_index_probe(cache, key);
    if (s == NULL || !s->used || !s->has_data)
        return horizon_gpu_ok();

    return bc_append(cache, key, HORIZON_BC_REC_TOMBSTONE, NULL, 0);
}

horizon_gpu_result horizon_gpu_blob_cache_flush(horizon_gpu_blob_cache *cache)
{
    if (cache == NULL)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (cache->read_only)
        return horizon_gpu_ok();

    return bc_sync(cache->file) ? horizon_gpu_ok()
                                : horizon_gpu_err(HORIZON_GPU_ERR_IO);
}

void horizon_gpu_blob_cache_get_stats(const horizon_gpu_blob_cache *cache,
                                      horizon_gpu_blob_cache_stats *out)
{
    if (cache == NULL || out == NULL)
        return;

    *out = cache->stats;
}
