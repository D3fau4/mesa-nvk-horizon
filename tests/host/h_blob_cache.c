/*
 * Host unit tests — the shader-cache blob store (horizon/cache/).
 *
 * WHY THESE RUN ON THE HOST AND NOT ONLY ON A CONSOLE. The value of this
 * module is entirely in what it does with a file that is *wrong*:
 * truncated by a flat battery, flipped by a failing SD card, or written
 * by a different driver build. Producing those files on a Switch means
 * an operator with a card reader; producing them here is an fseek and a
 * byte. So the damage cases live here, under ASan/UBSan, and
 * tests/t_shader_cache.c runs the same shapes against a real SD card to
 * answer the one question this cannot: whether Horizon's filesystem
 * behaves the way the format assumes.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../../horizon/cache/crc32.h"
#include "../../horizon/include/horizon_gpu/blob_cache.h"
#include "hostfw.h"

#define BC_DIR  "build/host-tests/bc"
#define BC_PATH BC_DIR "/cache.hzc"

/* A driver identity, and a different one, for the "the driver was
 * updated" case. */
static const char drv_a[] = "nvk_012b/driver-build-aaaaaaaa";
static const char drv_b[] = "nvk_012b/driver-build-bbbbbbbb";

static void key_for(uint32_t n, uint8_t key[HORIZON_GPU_BLOB_CACHE_KEY_SIZE])
{
    /* Deterministic, and deliberately not sequential in the low bytes:
     * the index hashes the first four bytes, so keys that differ only in
     * a high byte would all land in one bucket and the probe chains
     * would never be exercised. */
    for (uint32_t i = 0; i < HORIZON_GPU_BLOB_CACHE_KEY_SIZE; i++)
        key[i] = (uint8_t)(n * 2654435761u >> (8 * (i % 4))) ^ (uint8_t)i;
}

static horizon_gpu_blob_cache *open_at(const char *path, const char *drv,
                                       size_t drv_len, uint64_t max_size,
                                       bool read_only)
{
    horizon_gpu_blob_cache_config cfg = {
        .path = path,
        .driver_key = drv,
        .driver_key_size = drv_len,
        .max_size = max_size,
        .read_only = read_only,
    };
    horizon_gpu_blob_cache *c = NULL;

    if (horizon_gpu_failed(horizon_gpu_blob_cache_open(&cfg, &c)))
        return NULL;
    return c;
}

static horizon_gpu_blob_cache *open_a(void)
{
    return open_at(BC_PATH, drv_a, sizeof(drv_a), 0, false);
}

/* Put `size` bytes derived from `n`, so the payload can be checked
 * without keeping it around. */
static bool put_n(horizon_gpu_blob_cache *c, uint32_t n, size_t size)
{
    uint8_t key[HORIZON_GPU_BLOB_CACHE_KEY_SIZE];
    uint8_t *buf = malloc(size);
    bool ok;

    if (buf == NULL)
        return false;
    for (size_t i = 0; i < size; i++)
        buf[i] = (uint8_t)(n + i);

    key_for(n, key);
    ok = horizon_gpu_succeeded(horizon_gpu_blob_cache_put(c, key, buf, size));
    free(buf);
    return ok;
}

static bool get_is(horizon_gpu_blob_cache *c, uint32_t n, size_t size)
{
    uint8_t key[HORIZON_GPU_BLOB_CACHE_KEY_SIZE];
    void *data = NULL;
    size_t got = 0;
    bool ok;

    key_for(n, key);
    if (horizon_gpu_failed(horizon_gpu_blob_cache_get(c, key, &data, &got)))
        return false;
    ok = got == size;
    for (size_t i = 0; ok && i < size; i++)
        ok = ((const uint8_t *)data)[i] == (uint8_t)(n + i);
    free(data);
    return ok;
}

static bool is_miss(horizon_gpu_blob_cache *c, uint32_t n)
{
    uint8_t key[HORIZON_GPU_BLOB_CACHE_KEY_SIZE];
    void *data = (void *)0x1;
    size_t got = 1;
    horizon_gpu_result r;

    key_for(n, key);
    r = horizon_gpu_blob_cache_get(c, key, &data, &got);
    return r.status == HORIZON_GPU_ERR_NOT_FOUND && data == NULL && got == 0;
}

static long file_size_of(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;

    if (f == NULL)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    n = ftell(f);
    fclose(f);
    return n;
}

/* Cut `bytes` off the end of the file. */
static bool truncate_by(const char *path, long bytes)
{
    long size = file_size_of(path);
    uint8_t *buf;
    FILE *f;
    size_t keep;

    if (size < bytes)
        return false;
    keep = (size_t)(size - bytes);

    buf = malloc(keep ? keep : 1);
    if (buf == NULL)
        return false;

    f = fopen(path, "rb");
    if (f == NULL || fread(buf, 1, keep, f) != keep) {
        if (f)
            fclose(f);
        free(buf);
        return false;
    }
    fclose(f);

    f = fopen(path, "wb");
    if (f == NULL) {
        free(buf);
        return false;
    }
    bool ok = fwrite(buf, 1, keep, f) == keep;
    fclose(f);
    free(buf);
    return ok;
}

static bool flip_byte_at(const char *path, long off)
{
    FILE *f = fopen(path, "r+b");
    uint8_t b;

    if (f == NULL)
        return false;
    if (fseek(f, off, SEEK_SET) != 0 || fread(&b, 1, 1, f) != 1) {
        fclose(f);
        return false;
    }
    b ^= 0xFFu;
    if (fseek(f, off, SEEK_SET) != 0 || fwrite(&b, 1, 1, f) != 1) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static bool write_raw(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    bool ok;

    if (f == NULL)
        return false;
    ok = len == 0 || fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

int main(void)
{
    /* ---------------------------------------------------------------
     * CRC-32, against published vectors. Everything below trusts this
     * function to tell a good record from a bad one, so it is checked
     * against numbers that come from outside this repository first.
     * --------------------------------------------------------------- */
    /* The 256 constants in crc32.c, regenerated here from the polynomial
     * they are supposed to be and compared one by one.
     *
     * The table stopped being built at run time because a lazily built
     * global is a data race, and the objection to writing it out was that
     * nobody can check 256 numbers by reading them. Nobody has to: this
     * is the check, and it runs every time the suite does. It reaches the
     * table through horizon_crc32_update() one byte at a time — feeding a
     * single byte b to a crc of 0 yields table[b] ^ 0, which is exactly
     * the entry. */
    {
        uint32_t poly = 0xEDB88320u;
        bool all = true;
        uint32_t first_bad = 256;

        for (uint32_t i = 0; i < 256; i++) {
            uint32_t want = i;
            for (unsigned bit = 0; bit < 8; bit++)
                want = (want & 1u) ? (poly ^ (want >> 1)) : (want >> 1);

            uint8_t b = (uint8_t)i;
            uint32_t got = horizon_crc32_update(0, &b, 1);
            if (got != want && first_bad == 256) {
                first_bad = i;
                all = false;
            }
        }
        H_CHECK(all, "all 256 table entries match the IEEE 802.3 polynomial");
    }

    H_CHECK(horizon_crc32("", 0) == 0, "crc32 of nothing is 0");
    H_CHECK(horizon_crc32("123456789", 9) == 0xCBF43926u,
            "crc32 check value (the standard \"123456789\")");
    H_CHECK(horizon_crc32("a", 1) == 0xE8B7BE43u, "crc32 of \"a\"");
    H_CHECK(horizon_crc32(NULL, 0) == 0, "crc32(NULL, 0) is defined");
    {
        uint32_t c = horizon_crc32_init();
        c = horizon_crc32_update(c, "1234", 4);
        c = horizon_crc32_update(c, "56789", 5);
        H_CHECK(horizon_crc32_final(c) == 0xCBF43926u,
                "crc32 split across calls matches one call");
    }

    mkdir("build", 0777);
    mkdir("build/host-tests", 0777);
    mkdir(BC_DIR, 0777);
    remove(BC_PATH);

    /* ---------------------------------------------------------------
     * An empty cache. A first run has nothing to serve and must say so
     * without inventing a repair it did not perform.
     * --------------------------------------------------------------- */
    {
        horizon_gpu_blob_cache *c = open_a();
        horizon_gpu_blob_cache_stats st;

        H_CHECK(c != NULL, "open a cache that does not exist yet");
        if (!(c != NULL))
            return h_summary("h_blob_cache");

        horizon_gpu_blob_cache_get_stats(c, &st);
        H_CHECK(st.entries == 0 && st.keys == 0, "a fresh cache is empty");
        H_CHECK(!st.was_reset, "creating a file is not resetting one");
        H_CHECK(!st.was_truncated, "nothing to truncate in a new file");
        H_CHECK(is_miss(c, 1), "every key misses in an empty cache");

        horizon_gpu_blob_cache_get_stats(c, &st);
        H_CHECK(st.misses == 1 && st.hits == 0, "the miss was counted");
        horizon_gpu_blob_cache_close(c);
    }

    /* ---------------------------------------------------------------
     * miss -> store -> hit, and then the same after a reopen, which is
     * the whole point: "reopen" is what relaunching the application
     * looks like from here.
     * --------------------------------------------------------------- */
    {
        horizon_gpu_blob_cache *c = open_a();

        H_CHECK(is_miss(c, 7), "miss before the store");
        H_CHECK(put_n(c, 7, 300), "store");
        H_CHECK(get_is(c, 7, 300), "hit after the store");
        horizon_gpu_blob_cache_close(c);

        c = open_a();
        H_CHECK(c != NULL, "reopen");
        H_CHECK(get_is(c, 7, 300), "hit after reopening — it persisted");
        {
            horizon_gpu_blob_cache_stats st;
            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(st.entries == 1, "one entry came back");
            H_CHECK(!st.was_reset && !st.was_truncated,
                    "a clean file needed no repair");
        }
        horizon_gpu_blob_cache_close(c);
    }

    /* ---------------------------------------------------------------
     * Many entries, of many sizes, surviving a reopen. 200 entries also
     * pushes the index through several growths (it starts at 64).
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        const uint32_t n_entries = 200;
        horizon_gpu_blob_cache *c = open_a();
        bool all = true;

        for (uint32_t i = 0; i < n_entries; i++)
            all = put_n(c, i, 1 + (i % 97)) && all;
        H_CHECK(all, "200 entries stored");

        all = true;
        for (uint32_t i = 0; i < n_entries; i++)
            all = get_is(c, i, 1 + (i % 97)) && all;
        H_CHECK(all, "200 entries read back");
        horizon_gpu_blob_cache_close(c);

        c = open_a();
        all = true;
        for (uint32_t i = 0; i < n_entries; i++)
            all = get_is(c, i, 1 + (i % 97)) && all;
        H_CHECK(all, "200 entries survive a reopen");
        {
            horizon_gpu_blob_cache_stats st;
            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(st.entries == n_entries, "and the index counts them all");
        }
        horizon_gpu_blob_cache_close(c);
    }

    /* ---------------------------------------------------------------
     * Overwriting a key, and removing one.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        horizon_gpu_blob_cache *c = open_a();
        uint8_t key[HORIZON_GPU_BLOB_CACHE_KEY_SIZE];

        H_CHECK(put_n(c, 3, 40), "store v1");
        H_CHECK(put_n(c, 3, 400), "store v2 under the same key");
        H_CHECK(get_is(c, 3, 400), "the newer value wins");
        horizon_gpu_blob_cache_close(c);

        c = open_a();
        H_CHECK(get_is(c, 3, 400), "and it still wins after a reopen");

        key_for(3, key);
        H_CHECK(horizon_gpu_succeeded(horizon_gpu_blob_cache_remove(c, key)),
                "remove");
        H_CHECK(is_miss(c, 3), "removed is a miss");
        H_CHECK(horizon_gpu_succeeded(horizon_gpu_blob_cache_remove(c, key)),
                "removing what is not there is not an error");
        horizon_gpu_blob_cache_close(c);

        c = open_a();
        H_CHECK(is_miss(c, 3), "and it stays removed across a reopen");
        horizon_gpu_blob_cache_close(c);
    }

    /* ---------------------------------------------------------------
     * put_key/has_key is a separate namespace. Mesa's disk_cache.h is
     * explicit that has_key() must not answer for a key stored by put().
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        horizon_gpu_blob_cache *c = open_a();
        uint8_t k1[HORIZON_GPU_BLOB_CACHE_KEY_SIZE];
        uint8_t k2[HORIZON_GPU_BLOB_CACHE_KEY_SIZE];

        key_for(11, k1);
        key_for(12, k2);

        H_CHECK(!horizon_gpu_blob_cache_has_key(c, k1), "has_key before");
        H_CHECK(horizon_gpu_succeeded(horizon_gpu_blob_cache_put_key(c, k1)),
                "put_key");
        H_CHECK(horizon_gpu_blob_cache_has_key(c, k1), "has_key after");
        H_CHECK(is_miss(c, 11), "a key-only mark is invisible to get()");

        H_CHECK(put_n(c, 12, 20), "store data under another key");
        H_CHECK(!horizon_gpu_blob_cache_has_key(c, k2),
                "put() does not make has_key() true");
        horizon_gpu_blob_cache_close(c);

        c = open_a();
        H_CHECK(horizon_gpu_blob_cache_has_key(c, k1),
                "the key-only mark persists");
        H_CHECK(get_is(c, 12, 20), "and the data entry beside it does too");
        horizon_gpu_blob_cache_close(c);
    }

    /* ---------------------------------------------------------------
     * ONE KEY CARRYING BOTH, which is where the two namespaces stop
     * being a claim and start being a property.
     *
     * The index used to hold one kind per key, so put_key() then put()
     * on the same key replaced the mark and has_key() went back to
     * false — contradicting this module's own header and Mesa's
     * disk_cache.h, which require the stored-key index to be
     * independent of the cached entries. Nothing in NVK calls put_key(),
     * and the earlier tests used a different key for each namespace, so
     * nothing looked. A reviewer did.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        horizon_gpu_blob_cache *c = open_a();
        uint8_t k[HORIZON_GPU_BLOB_CACHE_KEY_SIZE];

        key_for(77, k);

        H_CHECK(c != NULL, "open for the both-halves case");
        if (c != NULL) {
            /* mark, then data */
            H_CHECK(horizon_gpu_succeeded(
                        horizon_gpu_blob_cache_put_key(c, k)), "put_key");
            H_CHECK(put_n(c, 77, 128), "put() under the same key");
            H_CHECK(horizon_gpu_blob_cache_has_key(c, k),
                    "put() does not erase the key-only mark");
            H_CHECK(get_is(c, 77, 128), "and the data is there beside it");

            /* the other order, on a second key */
            uint8_t k2[HORIZON_GPU_BLOB_CACHE_KEY_SIZE];
            key_for(78, k2);
            H_CHECK(put_n(c, 78, 64), "put() first this time");
            H_CHECK(horizon_gpu_succeeded(
                        horizon_gpu_blob_cache_put_key(c, k2)),
                    "put_key() under the same key");
            H_CHECK(get_is(c, 78, 64), "put_key() does not erase the data");
            H_CHECK(horizon_gpu_blob_cache_has_key(c, k2), "and the mark is set");

            {
                horizon_gpu_blob_cache_stats st;
                horizon_gpu_blob_cache_get_stats(c, &st);
                H_CHECK(st.entries == 2 && st.keys == 2,
                        "two entries and two marks over two keys");
            }

            /* remove() drops the entry and leaves the mark: it is
             * disk_cache_remove(), and a mark is not an entry. */
            H_CHECK(horizon_gpu_succeeded(
                        horizon_gpu_blob_cache_remove(c, k)), "remove");
            H_CHECK(is_miss(c, 77), "the entry is gone");
            H_CHECK(horizon_gpu_blob_cache_has_key(c, k),
                    "and the key-only mark survived it");
            horizon_gpu_blob_cache_close(c);

            c = open_a();
            H_CHECK(c != NULL, "reopen");
            if (c != NULL) {
                H_CHECK(horizon_gpu_blob_cache_has_key(c, k),
                        "both halves survive a reopen: the mark");
                H_CHECK(is_miss(c, 77), "and the removed entry stays removed");
                H_CHECK(get_is(c, 78, 64) &&
                            horizon_gpu_blob_cache_has_key(c, k2),
                        "and the key carrying both comes back with both");
                horizon_gpu_blob_cache_close(c);
            }
        }
    }

    /* ---------------------------------------------------------------
     * The same, through a compaction — the rewrite has to carry two
     * records for a key that has two, not one.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        const uint64_t cap = 16u * 1024u;
        horizon_gpu_blob_cache *c =
            open_at(BC_PATH, drv_a, sizeof(drv_a), cap, false);
        uint8_t k[HORIZON_GPU_BLOB_CACHE_KEY_SIZE];

        key_for(900, k);
        H_CHECK(c != NULL, "open with a ceiling, for the compaction case");
        if (c != NULL) {
            horizon_gpu_blob_cache_stats st;

            /* Enough to force compactions, then the key that carries
             * both halves LAST, so newest-first guarantees both of its
             * records survive the budget. Asserting on a key that might
             * legitimately have been evicted would be a check that
             * cannot fail, and this file already has a comment about
             * one of those. */
            for (uint32_t i = 0; i < 80; i++)
                put_n(c, i, 512);

            H_CHECK(put_n(c, 900, 256), "the entry that must survive");
            H_CHECK(horizon_gpu_succeeded(
                        horizon_gpu_blob_cache_put_key(c, k)),
                    "and the mark on the same key");

            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(st.compactions > 0, "it compacted");
            horizon_gpu_blob_cache_close(c);

            c = open_at(BC_PATH, drv_a, sizeof(drv_a), cap, false);
            H_CHECK(c != NULL, "and the compacted file reopens");
            if (c != NULL) {
                horizon_gpu_blob_cache_get_stats(c, &st);
                H_CHECK(!st.was_reset && !st.was_truncated, "cleanly");
                H_CHECK(get_is(c, 900, 256),
                        "the newest key's entry survived the rewrite");
                H_CHECK(horizon_gpu_blob_cache_has_key(c, k),
                        "and so did its key-only mark — two records for "
                        "one key, both carried");
                horizon_gpu_blob_cache_close(c);
            }
        }
    }

    /* ---------------------------------------------------------------
     * A different driver build must not read the old build's entries.
     * A wrong hit here is a shader compiled by another compiler handed
     * to this one, so the test is that it is a reset and not a miss.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        horizon_gpu_blob_cache *c = open_a();
        horizon_gpu_blob_cache_stats st;

        H_CHECK(put_n(c, 5, 64), "store under driver A");
        horizon_gpu_blob_cache_close(c);

        c = open_at(BC_PATH, drv_b, sizeof(drv_b), 0, false);
        H_CHECK(c != NULL, "open under driver B");
        if (c != NULL) {
            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(st.was_reset, "driver B resets driver A's file");
            H_CHECK(st.entries == 0, "and starts empty");
            H_CHECK(is_miss(c, 5), "driver A's entry is not served");
            H_CHECK(put_n(c, 5, 64) && get_is(c, 5, 64),
                    "driver B can use the file it reset");
            horizon_gpu_blob_cache_close(c);
        }

        /* And back: driver A now finds driver B's file, not its own. */
        c = open_a();
        H_CHECK(c != NULL, "reopen under driver A");
        if (c != NULL) {
            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(st.was_reset && st.entries == 0,
                    "driver A resets driver B's file in turn");
            horizon_gpu_blob_cache_close(c);
        }
    }

    /* ---------------------------------------------------------------
     * A write cut short. This is the flat-battery case, and it is the
     * reason the format validates content instead of trusting rename().
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        horizon_gpu_blob_cache *c = open_a();
        horizon_gpu_blob_cache_stats st;

        for (uint32_t i = 0; i < 5; i++)
            put_n(c, i, 100);
        horizon_gpu_blob_cache_close(c);

        /* Cut into the middle of the last record: 40 bytes is inside the
         * 100-byte payload, so the record header is intact and only the
         * bounds check can catch it. */
        H_CHECK(truncate_by(BC_PATH, 40), "cut the last record short");

        c = open_a();
        H_CHECK(c != NULL, "a torn file still opens");
        if (c != NULL) {
            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(st.was_truncated, "the tear was noticed");
            H_CHECK(st.discarded_bytes > 0, "and the damage measured");
            H_CHECK(st.entries == 4, "the four whole entries are there");

            bool earlier = true;
            for (uint32_t i = 0; i < 4; i++)
                earlier = get_is(c, i, 100) && earlier;
            H_CHECK(earlier, "everything written before the tear survived");
            H_CHECK(is_miss(c, 4), "the torn entry is gone, not wrong");

            /* And the file is usable again, which is the part that
             * matters: a cache that survives one power cut but cannot be
             * written to afterwards has not survived it. */
            H_CHECK(put_n(c, 4, 100), "a new entry can be appended after");
            horizon_gpu_blob_cache_close(c);

            c = open_a();
            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(!st.was_truncated, "and the repaired file reopens clean");
            H_CHECK(get_is(c, 4, 100), "with the entry written after the tear");
            horizon_gpu_blob_cache_close(c);
        }
    }

    /* ---------------------------------------------------------------
     * A flipped byte in a payload. The record header still validates,
     * so only the payload CRC stands between a bad card and a shader
     * that was never compiled.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        horizon_gpu_blob_cache *c = open_a();
        long hdr_end;

        for (uint32_t i = 0; i < 3; i++)
            put_n(c, i, 128);
        horizon_gpu_blob_cache_close(c);

        /* The first record starts at the file header end (64 + the
         * padded driver key) and its payload 56 bytes after that. */
        hdr_end = 64 + (long)((sizeof(drv_a) + 7u) & ~(size_t)7u);
        H_CHECK(flip_byte_at(BC_PATH, hdr_end + 56 + 10),
                "flip a byte inside the first payload");

        c = open_a();
        H_CHECK(c != NULL, "a file with a bad payload still opens");
        if (c != NULL) {
            horizon_gpu_blob_cache_stats st;

            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(!st.was_truncated,
                    "a payload flip does not break the record chain");
            H_CHECK(st.entries == 3, "so all three records are indexed");
            H_CHECK(is_miss(c, 0), "but the damaged one reads as a miss");
            H_CHECK(get_is(c, 1, 128) && get_is(c, 2, 128),
                    "and the entries either side are untouched");
            horizon_gpu_blob_cache_close(c);
        }
    }

    /* ---------------------------------------------------------------
     * A flipped byte in a record header. Nothing after it can be
     * located any more, so the chain has to stop there.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        horizon_gpu_blob_cache *c = open_a();
        long hdr_end, second;

        for (uint32_t i = 0; i < 4; i++)
            put_n(c, i, 64);
        horizon_gpu_blob_cache_close(c);

        hdr_end = 64 + (long)((sizeof(drv_a) + 7u) & ~(size_t)7u);
        second = hdr_end + 56 + 64; /* 64 is already a multiple of 8 */
        H_CHECK(flip_byte_at(BC_PATH, second + 41),
                "flip a byte in the second record's size field");

        c = open_a();
        H_CHECK(c != NULL, "a file with a bad record header opens");
        if (c != NULL) {
            horizon_gpu_blob_cache_stats st;

            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(st.was_truncated, "the chain stopped at the bad record");
            H_CHECK(st.entries == 1, "only what preceded it is indexed");
            H_CHECK(get_is(c, 0, 64), "and that one still reads");
            H_CHECK(is_miss(c, 1) && is_miss(c, 2) && is_miss(c, 3),
                    "the bad record and everything after it are gone");
            horizon_gpu_blob_cache_close(c);
        }
    }

    /* ---------------------------------------------------------------
     * Files that are not ours at all.
     * --------------------------------------------------------------- */
    {
        static const uint8_t garbage[300] = { 0xDE, 0xAD, 0xBE, 0xEF };
        horizon_gpu_blob_cache *c;
        horizon_gpu_blob_cache_stats st;

        /* Zero length. */
        remove(BC_PATH);
        H_CHECK(write_raw(BC_PATH, garbage, 0), "make a zero-length file");
        c = open_a();
        H_CHECK(c != NULL, "a zero-length file opens");
        if (c != NULL) {
            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(st.was_reset && st.entries == 0,
                    "and is reset rather than parsed");
            H_CHECK(put_n(c, 1, 32) && get_is(c, 1, 32),
                    "and works afterwards");
            horizon_gpu_blob_cache_close(c);
        }

        /* Garbage. */
        remove(BC_PATH);
        H_CHECK(write_raw(BC_PATH, garbage, sizeof(garbage)),
                "make a garbage file");
        c = open_a();
        H_CHECK(c != NULL, "a garbage file opens");
        if (c != NULL) {
            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(st.was_reset && st.entries == 0, "and is reset");
            H_CHECK(st.discarded_bytes == sizeof(garbage),
                    "reporting exactly what it threw away");
            horizon_gpu_blob_cache_close(c);
        }

        /* A good file whose magic has been damaged. */
        remove(BC_PATH);
        c = open_a();
        put_n(c, 1, 32);
        horizon_gpu_blob_cache_close(c);
        H_CHECK(flip_byte_at(BC_PATH, 2), "damage the file magic");
        c = open_a();
        H_CHECK(c != NULL, "a file with a bad magic opens");
        if (c != NULL) {
            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(st.was_reset && st.entries == 0, "and is reset");
            horizon_gpu_blob_cache_close(c);
        }

        /* A good file whose header CRC has been damaged, magic intact:
         * the checksum is what has to catch this one. */
        remove(BC_PATH);
        c = open_a();
        put_n(c, 1, 32);
        horizon_gpu_blob_cache_close(c);
        H_CHECK(flip_byte_at(BC_PATH, 20), "damage the file header body");
        c = open_a();
        H_CHECK(c != NULL, "a file with a bad header CRC opens");
        if (c != NULL) {
            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(st.was_reset && st.entries == 0,
                    "and is reset by the checksum, not by the magic");
            horizon_gpu_blob_cache_close(c);
        }
    }

    /* ---------------------------------------------------------------
     * An interrupted compaction. Compaction rewrites the file in place,
     * so between its first write and its last the file is neither the
     * old cache nor the new one. The header state is what says so, and
     * a file caught in that window must be reset rather than parsed.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        horizon_gpu_blob_cache *c = open_a();
        horizon_gpu_blob_cache_stats st;
        uint8_t hdr[64];
        FILE *f;

        for (uint32_t i = 0; i < 4; i++)
            put_n(c, i, 100);
        horizon_gpu_blob_cache_close(c);

        /* Stamp COMPACTING by hand, checksum and all, so this is the
         * state an interrupted rewrite really leaves behind and not a
         * corrupt header wearing its name. */
        f = fopen(BC_PATH, "r+b");
        H_CHECK(f != NULL && fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr),
                "read the file header back");
        if (f != NULL) {
            hdr[28] = 1; /* HORIZON_BC_STATE_COMPACTING */
            hdr[29] = hdr[30] = hdr[31] = 0;
            {
                uint32_t crc = horizon_crc32(hdr, 60);
                hdr[60] = (uint8_t)(crc & 0xFFu);
                hdr[61] = (uint8_t)((crc >> 8) & 0xFFu);
                hdr[62] = (uint8_t)((crc >> 16) & 0xFFu);
                hdr[63] = (uint8_t)((crc >> 24) & 0xFFu);
            }
            H_CHECK(fseek(f, 0, SEEK_SET) == 0 &&
                        fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr),
                    "stamp the header COMPACTING with a valid checksum");
            fclose(f);
        }

        c = open_a();
        H_CHECK(c != NULL, "a file caught mid-compaction opens");
        if (c != NULL) {
            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(st.was_reset,
                    "and is reset, not parsed — it is neither cache");
            H_CHECK(st.entries == 0 && is_miss(c, 0),
                    "so none of the half-moved records is served");
            H_CHECK(put_n(c, 0, 100) && get_is(c, 0, 100),
                    "and it works again immediately");
            horizon_gpu_blob_cache_close(c);
        }
    }

    /* ---------------------------------------------------------------
     * Sizes that a corrupt header could ask for. Each of these is a
     * number the parser must refuse rather than trust; the checksum
     * catches most of them, so the sizes here are made consistent with
     * their checksums on purpose.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        horizon_gpu_blob_cache *c = open_a();
        long hdr_end;
        uint8_t rec[56];
        FILE *f;

        put_n(c, 0, 64);
        put_n(c, 1, 64);
        horizon_gpu_blob_cache_close(c);

        hdr_end = 64 + (long)((sizeof(drv_a) + 7u) & ~(size_t)7u);

        /* Claim a payload that runs past the end of the file, and fix
         * the header checksum so only the bounds check can stop it. */
        f = fopen(BC_PATH, "r+b");
        H_CHECK(f != NULL, "reopen the file by hand");
        if (f != NULL) {
            H_CHECK(fseek(f, hdr_end, SEEK_SET) == 0 &&
                        fread(rec, 1, sizeof(rec), f) == sizeof(rec),
                    "read the first record header");
            rec[40] = 0x00; rec[41] = 0x00; rec[42] = 0x10; rec[43] = 0x00;
            {
                uint32_t crc = horizon_crc32(rec, 52);
                rec[52] = (uint8_t)(crc & 0xFFu);
                rec[53] = (uint8_t)((crc >> 8) & 0xFFu);
                rec[54] = (uint8_t)((crc >> 16) & 0xFFu);
                rec[55] = (uint8_t)((crc >> 24) & 0xFFu);
            }
            H_CHECK(fseek(f, hdr_end, SEEK_SET) == 0 &&
                        fwrite(rec, 1, sizeof(rec), f) == sizeof(rec),
                    "claim a 1 MiB payload in a 300-byte file");
            fclose(f);
        }

        c = open_a();
        H_CHECK(c != NULL, "a record claiming more than the file holds opens");
        if (c != NULL) {
            horizon_gpu_blob_cache_stats st;

            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(st.entries == 0,
                    "the impossible record is refused by the bounds check");
            H_CHECK(st.was_truncated, "and everything after it goes with it");
            horizon_gpu_blob_cache_close(c);
        }
    }

    /* ---------------------------------------------------------------
     * A key of all zeroes is an ordinary key. It is worth its own check
     * because a zero key is what an uninitialised buffer looks like, and
     * a store that treated it as "absent" would turn a caller's bug into
     * a silent cache miss that nothing explains.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        horizon_gpu_blob_cache *c = open_a();
        uint8_t zero[HORIZON_GPU_BLOB_CACHE_KEY_SIZE] = { 0 };
        void *data = NULL;
        size_t got = 0;

        H_CHECK(horizon_gpu_succeeded(
                    horizon_gpu_blob_cache_put(c, zero, "zero", 4)),
                "store under an all-zero key");
        H_CHECK(horizon_gpu_succeeded(
                    horizon_gpu_blob_cache_get(c, zero, &data, &got)) &&
                    got == 4 && memcmp(data, "zero", 4) == 0,
                "and read it back");
        free(data);
        horizon_gpu_blob_cache_close(c);

        c = open_a();
        data = NULL;
        H_CHECK(horizon_gpu_succeeded(
                    horizon_gpu_blob_cache_get(c, zero, &data, &got)),
                "an all-zero key survives a reopen");
        free(data);
        horizon_gpu_blob_cache_close(c);
    }

    /* ---------------------------------------------------------------
     * The same key stored twice, where the newer copy is the damaged
     * one. The store does NOT fall back to the older copy: newest wins
     * is the rule, and a damaged newest is a miss. That is a deliberate
     * choice — falling back would mean serving a value the caller
     * already replaced, and a miss costs one recompile.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        horizon_gpu_blob_cache *c = open_a();
        long hdr_end, second;

        H_CHECK(put_n(c, 9, 64), "store the first copy");
        H_CHECK(put_n(c, 9, 64), "store a second copy under the same key");
        horizon_gpu_blob_cache_close(c);

        hdr_end = 64 + (long)((sizeof(drv_a) + 7u) & ~(size_t)7u);
        second = hdr_end + 56 + 64;
        H_CHECK(flip_byte_at(BC_PATH, second + 56 + 5),
                "damage the newer copy's payload");

        c = open_a();
        if (c != NULL) {
            H_CHECK(is_miss(c, 9),
                    "a damaged newest copy is a miss, not the older value");
            horizon_gpu_blob_cache_close(c);
        }
    }

    /* ---------------------------------------------------------------
     * A ceiling smaller than a single entry. The ceiling governs
     * accumulation; it is not a reason to refuse the one thing somebody
     * asked to store, and it must not loop or crash trying.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        horizon_gpu_blob_cache *c =
            open_at(BC_PATH, drv_a, sizeof(drv_a), 128, false);

        H_CHECK(c != NULL, "open with a ceiling below one record");
        if (c != NULL) {
            H_CHECK(put_n(c, 1, 4096), "an entry larger than the ceiling"
                                       " is still stored");
            H_CHECK(get_is(c, 1, 4096), "and read back");
            H_CHECK(put_n(c, 2, 4096), "and a second one after it");
            H_CHECK(get_is(c, 2, 4096), "which is also readable");
            horizon_gpu_blob_cache_close(c);

            c = open_at(BC_PATH, drv_a, sizeof(drv_a), 128, false);
            H_CHECK(c != NULL, "and the result reopens");
            horizon_gpu_blob_cache_close(c);
        }
    }

    /* ---------------------------------------------------------------
     * The size ceiling. An SD card is somebody's games; a cache that
     * grows without bound is a bug even when every entry in it is valid.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        const uint64_t cap = 32u * 1024u;
        horizon_gpu_blob_cache *c =
            open_at(BC_PATH, drv_a, sizeof(drv_a), cap, false);
        horizon_gpu_blob_cache_stats st;
        bool all = true;

        H_CHECK(c != NULL, "open with a 32 KiB ceiling");
        if (c != NULL) {
            for (uint32_t i = 0; i < 200; i++)
                all = put_n(c, i, 1024) && all;
            H_CHECK(all, "200 KiB of entries into a 32 KiB cache");

            horizon_gpu_blob_cache_get_stats(c, &st);
            H_CHECK(st.compactions > 0, "it compacted rather than grew");
            H_CHECK(st.file_size <= cap, "and stayed inside the ceiling");
            /* Run 27 measured 70 compactions for 100 puts on a console,
             * because compaction used to refill the file to the ceiling
             * and the next put crossed it again. 200 puts of 1 KiB into
             * 32 KiB, compacting to half of it, needs on the order of
             * 200/16 rewrites; 30 is a bound that a return to the old
             * behaviour cannot slip under. */
            H_CHECK(st.compactions <= 30,
                    "and did not compact on nearly every put");
            H_CHECK(st.entries > 0, "keeping something rather than nothing");
            H_CHECK(get_is(c, 199, 1024), "the newest entry is still there");
            horizon_gpu_blob_cache_close(c);

            c = open_at(BC_PATH, drv_a, sizeof(drv_a), cap, false);
            H_CHECK(c != NULL, "a compacted cache reopens");
            if (c != NULL) {
                horizon_gpu_blob_cache_get_stats(c, &st);
                H_CHECK(!st.was_reset && !st.was_truncated,
                        "cleanly, with no repair needed");
                H_CHECK(get_is(c, 199, 1024),
                        "and the newest entry survived the rewrite");
                H_CHECK((long)st.file_size == file_size_of(BC_PATH),
                        "the reported size is the size on disk");
                horizon_gpu_blob_cache_close(c);
            }
        }
    }

    /* ---------------------------------------------------------------
     * Entries larger than the scan's stream buffer. The open scan walks
     * forward and steps over payloads, choosing between consuming them
     * and seeking past them at a threshold of one buffer refill — so an
     * entry either side of that threshold takes a different path through
     * bc_skip_forward(), and both have to land on the next record header
     * exactly. Sizes here straddle 64 KiB on purpose.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        const size_t sizes[] = { 100, 64u * 1024u - 8, 64u * 1024u,
                                 64u * 1024u + 8, 200u * 1024u, 64 };
        const uint32_t n_sizes = (uint32_t)(sizeof(sizes) / sizeof(sizes[0]));
        horizon_gpu_blob_cache *c = open_at(BC_PATH, drv_a, sizeof(drv_a),
                                            8u * 1024u * 1024u, false);
        bool all = true;

        H_CHECK(c != NULL, "open for the large-entry scan");
        if (c != NULL) {
            for (uint32_t i = 0; i < n_sizes; i++)
                all = put_n(c, i, sizes[i]) && all;
            H_CHECK(all, "entries either side of one buffer refill stored");
            horizon_gpu_blob_cache_close(c);

            c = open_at(BC_PATH, drv_a, sizeof(drv_a), 8u * 1024u * 1024u,
                        false);
            H_CHECK(c != NULL, "and the file reopens");
            if (c != NULL) {
                horizon_gpu_blob_cache_stats st;

                horizon_gpu_blob_cache_get_stats(c, &st);
                H_CHECK(!st.was_reset && !st.was_truncated,
                        "with no record lost stepping over a large payload");
                H_CHECK(st.entries == n_sizes, "and every entry indexed");

                all = true;
                for (uint32_t i = 0; i < n_sizes; i++)
                    all = get_is(c, i, sizes[i]) && all;
                H_CHECK(all, "and every one of them reads back");
                horizon_gpu_blob_cache_close(c);
            }
        }
    }

    /* ---------------------------------------------------------------
     * Read-only. A cache opened this way still answers; it just never
     * writes, including never repairing.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        horizon_gpu_blob_cache *c = open_a();
        uint8_t key[HORIZON_GPU_BLOB_CACHE_KEY_SIZE];

        put_n(c, 42, 80);
        horizon_gpu_blob_cache_close(c);

        c = open_at(BC_PATH, drv_a, sizeof(drv_a), 0, true);
        H_CHECK(c != NULL, "open read-only");
        if (c != NULL) {
            key_for(43, key);
            H_CHECK(get_is(c, 42, 80), "a read-only cache serves hits");
            H_CHECK(horizon_gpu_blob_cache_put(c, key, "x", 1).status ==
                        HORIZON_GPU_ERR_STATE,
                    "and refuses a put");
            H_CHECK(horizon_gpu_blob_cache_put_key(c, key).status ==
                        HORIZON_GPU_ERR_STATE,
                    "and a put_key");
            H_CHECK(horizon_gpu_blob_cache_remove(c, key).status ==
                        HORIZON_GPU_ERR_STATE,
                    "and a remove");
            horizon_gpu_blob_cache_close(c);
        }

        /* Read-only against a file that does not exist is an error, not
         * a silently empty cache: the caller asked to read something. */
        c = open_at(BC_DIR "/no-such-file.hzc", drv_a, sizeof(drv_a), 0, true);
        H_CHECK(c == NULL, "read-only refuses to create");
    }

    /* ---------------------------------------------------------------
     * Arguments. Every one of these is a caller bug, and none of them
     * may be a crash.
     * --------------------------------------------------------------- */
    remove(BC_PATH);
    {
        horizon_gpu_blob_cache *c = NULL;
        horizon_gpu_blob_cache_config cfg = {
            .path = BC_PATH,
            .driver_key = drv_a,
            .driver_key_size = sizeof(drv_a),
        };
        uint8_t key[HORIZON_GPU_BLOB_CACHE_KEY_SIZE];

        key_for(1, key);

        H_CHECK(horizon_gpu_blob_cache_open(NULL, &c).status ==
                    HORIZON_GPU_ERR_INVALID_ARG, "open(NULL config)");
        H_CHECK(horizon_gpu_blob_cache_open(&cfg, NULL).status ==
                    HORIZON_GPU_ERR_INVALID_ARG, "open(NULL out)");

        cfg.path = NULL;
        H_CHECK(horizon_gpu_blob_cache_open(&cfg, &c).status ==
                    HORIZON_GPU_ERR_INVALID_ARG, "open(NULL path)");
        cfg.path = "";
        H_CHECK(horizon_gpu_blob_cache_open(&cfg, &c).status ==
                    HORIZON_GPU_ERR_INVALID_ARG, "open(empty path)");
        cfg.path = BC_PATH;

        cfg.driver_key = NULL;
        H_CHECK(horizon_gpu_blob_cache_open(&cfg, &c).status ==
                    HORIZON_GPU_ERR_INVALID_ARG,
                "open(NULL driver key with a non-zero size)");
        cfg.driver_key = drv_a;

        cfg.driver_key_size = 1u << 20;
        H_CHECK(horizon_gpu_blob_cache_open(&cfg, &c).status ==
                    HORIZON_GPU_ERR_INVALID_ARG, "open(absurd driver key)");
        cfg.driver_key_size = sizeof(drv_a);

        /* A cache with no driver key at all is legal — the caller may
         * have nothing to distinguish builds by. */
        cfg.driver_key = NULL;
        cfg.driver_key_size = 0;
        remove(BC_PATH);
        H_CHECK(horizon_gpu_succeeded(horizon_gpu_blob_cache_open(&cfg, &c)) &&
                    c != NULL,
                "open with no driver key");
        horizon_gpu_blob_cache_close(c);

        remove(BC_PATH);
        c = open_a();
        H_CHECK(horizon_gpu_blob_cache_put(c, key, NULL, 4).status ==
                    HORIZON_GPU_ERR_INVALID_ARG, "put(NULL data)");
        H_CHECK(horizon_gpu_blob_cache_put(c, NULL, "x", 1).status ==
                    HORIZON_GPU_ERR_INVALID_ARG, "put(NULL key)");
        H_CHECK(horizon_gpu_blob_cache_put(c, key, "x", 0).status ==
                    HORIZON_GPU_ERR_INVALID_ARG, "put(zero size)");
        H_CHECK(horizon_gpu_blob_cache_put(
                    c, key, "x",
                    (size_t)HORIZON_GPU_BLOB_CACHE_MAX_ENTRY_SIZE + 1)
                        .status == HORIZON_GPU_ERR_INVALID_ARG,
                "put(above the entry ceiling)");
        H_CHECK(horizon_gpu_blob_cache_get(c, NULL, NULL, NULL).status ==
                    HORIZON_GPU_ERR_INVALID_ARG, "get(NULL everything)");
        H_CHECK(!horizon_gpu_blob_cache_has_key(NULL, key),
                "has_key(NULL cache) is false, not a crash");
        H_CHECK(horizon_gpu_succeeded(horizon_gpu_blob_cache_flush(c)),
                "flush");
        horizon_gpu_blob_cache_close(c);
        horizon_gpu_blob_cache_close(NULL);
        H_CHECK(true, "close(NULL) is a no-op");
    }

    remove(BC_PATH);
    return h_summary("h_blob_cache");
}
