/*
 * Test 21 — the shader disk cache, on the SD card it will actually live
 * on, through both halves of it.
 *
 * WHAT ONLY A CONSOLE CAN ANSWER. tests/host/h_blob_cache.c already
 * takes the storage engine apart under ASan and UBSan: truncation,
 * flipped bytes, an interrupted compaction, a file from another driver
 * build. All of that runs on a filesystem that is not Horizon's. What
 * it cannot say is whether libnx's `fsdev` devoptab and the FS
 * sysmodule behave the way the format assumes — whether ftruncate()
 * shortens a file, whether fsync() returns, whether a file written and
 * closed reads back after the process that wrote it has exited, and
 * what any of it costs on an SD card. Section A asks those.
 *
 * Section B asks the question above it: does Mesa's own disk_cache API
 * work here. It links the same libmesa_util.a the driver does and calls
 * disk_cache_create/put/get exactly as vk_pipeline_cache.c does, so a
 * pass means the driver's path works, not that a lookalike does.
 *
 * Section C is the one that needs two runs. A cache is only a cache if
 * it survives the process that filled it, so the test leaves entries
 * behind and reports, on the next launch, whether they came back. The
 * FIRST RUN OF THIS TEST ON A GIVEN CONSOLE REPORTS A COLD CACHE AND
 * THAT IS A PASS; run it twice and read the note lines.
 *
 * Uses no horizon_gpu and no nv services: this is the C library, the SD
 * card, and Mesa's util. It needs no display either, so it is cheap to
 * run early when triaging a console.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

#include "horizon_gpu/blob_cache.h"

/* Mesa's headers are not written against this project's -Wextra -Werror
 * policy, and disk_cache.h carries an inline stub —
 * disk_cache_get_function_identifier(), the branch taken where there is
 * no dladdr, which is every Horizon build — whose parameters are unused
 * by construction. Silenced around the include only, so that the policy
 * still applies to every line of this test. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "util/disk_cache.h"
#pragma GCC diagnostic pop

#include "common/testfw.h"

const char *const test_name = "t_shader_cache";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

/* Everything this test writes lives here, and never in
 * sdmc:/mesa_shader_cache — a test must not be able to poison the cache
 * a driver will later read. */
#define TEST_DIR  "sdmc:/horizon_gpu_tests"
#define STORE_PATH TEST_DIR "/t_shader_cache.hzc"
/* Section C's cache, deliberately kept across runs. */
#define PERSIST_PATH TEST_DIR "/t_shader_cache_persist.hzc"

static const char drv_a[] = "t_shader_cache/driver-a";
static const char drv_b[] = "t_shader_cache/driver-b";

static void key_for(uint32_t n, uint8_t key[HORIZON_GPU_BLOB_CACHE_KEY_SIZE])
{
    for (uint32_t i = 0; i < HORIZON_GPU_BLOB_CACHE_KEY_SIZE; i++)
        key[i] = (uint8_t)(n * 2654435761u >> (8 * (i % 4))) ^ (uint8_t)i;
}

static horizon_gpu_blob_cache *
store_open(const char *path, const char *drv, size_t drv_len,
           uint64_t max_size)
{
    const horizon_gpu_blob_cache_config cfg = {
        .path = path,
        .driver_key = drv,
        .driver_key_size = drv_len,
        .max_size = max_size,
        .read_only = false,
    };
    horizon_gpu_blob_cache *c = NULL;

    if (horizon_gpu_failed(horizon_gpu_blob_cache_open(&cfg, &c)))
        return NULL;
    return c;
}

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

static uint64_t now_us(void)
{
    /* armGetSystemTick counts at armGetSystemTickFreq(), which is the
     * 19.2 MHz ARM generic timer on this SoC. Microseconds because the
     * numbers being reported are SD-card latencies. */
    return armTicksToNs(armGetSystemTick()) / 1000u;
}

/* The size of the file on the card, or -1.
 *
 * ONLY VALID WITH THE STORE CLOSED, and run 26 is why this comment
 * exists. Opening a file that is already open for writing fails on this
 * platform — the SD card's device layer refuses it, which STATUS.md
 * already records for the test framework's own log file. This helper
 * then returned -1, and the two callers that used it while the cache was
 * open both got it wrong in opposite directions: one compared it as a
 * signed long (`-1 < 4096`, true, a check that could not fail and
 * reported success anyway) and the other cast it to uint64_t (2^64-1,
 * the single FAIL in the log).
 *
 * So: callers assert on it through file_size_checked() below, and while
 * the store is open they read horizon_gpu_blob_cache_stats::file_size
 * instead, which the host suite already checks against the real size. */
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

/* file_size_of() plus the check that it worked, so that a -1 fails the
 * test rather than being compared as if it were a size. Returns -1 on
 * failure, having already recorded the failure. */
static long file_size_checked(test_ctx *t, const char *path, const char *what)
{
    long n = file_size_of(path);

    t_check(t, n >= 0, "%s (the file could be measured at all)", what);
    return n;
}

/* ---------------------------------------------------------------- */
/* Section A — the store, on the real filesystem                     */
/* ---------------------------------------------------------------- */

static void section_a(test_ctx *t)
{
    horizon_gpu_blob_cache *c;
    horizon_gpu_blob_cache_stats st;
    uint64_t t0;

    t_note(t, "--- A: the blob store on sdmc: ---");

    remove(STORE_PATH);

    /* A1. An empty cache, and the SD card accepting a new file at all. */
    c = store_open(STORE_PATH, drv_a, sizeof(drv_a), 0);
    if (!t_check(t, c != NULL, "A1 open a new cache on the SD card"))
        return;

    horizon_gpu_blob_cache_get_stats(c, &st);
    t_check(t, st.entries == 0 && !st.was_reset && !st.was_truncated,
            "A1 a new cache is empty and needed no repair");
    t_check(t, is_miss(c, 1), "A1 every key misses in an empty cache");

    /* A2. miss -> store -> hit, with the cost of each on real storage. */
    t0 = now_us();
    t_check(t, put_n(c, 7, 4096), "A2 store a 4 KiB entry");
    t_note(t, "A2 put(4096) took %llu us", (unsigned long long)(now_us() - t0));

    t0 = now_us();
    t_check(t, get_is(c, 7, 4096), "A2 read it back");
    t_note(t, "A2 get(4096) took %llu us", (unsigned long long)(now_us() - t0));

    /* A3. Many entries, and what the file costs. */
    t0 = now_us();
    bool all = true;
    for (uint32_t i = 100; i < 300; i++)
        all = put_n(c, i, 1 + (i % 97)) && all;
    t_check(t, all, "A3 200 entries stored");
    t_note(t, "A3 200 puts took %llu us (%llu us each)",
           (unsigned long long)(now_us() - t0),
           (unsigned long long)((now_us() - t0) / 200));

    horizon_gpu_blob_cache_close(c);

    /* A4. Reopen: does the SD card give back what was written, and how
     * long does rebuilding the index take? This is the number that
     * decides whether the 64 KiB stdio buffer was the right guess. */
    t0 = now_us();
    c = store_open(STORE_PATH, drv_a, sizeof(drv_a), 0);
    uint64_t open_us = now_us() - t0;
    if (!t_check(t, c != NULL, "A4 reopen the cache"))
        return;

    horizon_gpu_blob_cache_get_stats(c, &st);
    t_note(t, "A4 open with %u entries took %llu us (%llu us/entry), "
              "file %llu bytes",
           st.entries, (unsigned long long)open_us,
           (unsigned long long)(st.entries ? open_us / st.entries : open_us),
           (unsigned long long)st.file_size);
    t_check(t, st.entries == 201, "A4 all 201 entries came back");
    t_check(t, !st.was_reset && !st.was_truncated,
            "A4 the file the SD card returned needed no repair");
    t_check(t, get_is(c, 7, 4096), "A4 the 4 KiB entry survived");

    all = true;
    for (uint32_t i = 100; i < 300; i++)
        all = get_is(c, i, 1 + (i % 97)) && all;
    t_check(t, all, "A4 and so did the other 200");

    /* A5. The key-only namespace. */
    uint8_t key[HORIZON_GPU_BLOB_CACHE_KEY_SIZE];
    key_for(9001, key);
    t_check(t, horizon_gpu_succeeded(horizon_gpu_blob_cache_put_key(c, key)),
            "A5 put_key");
    t_check(t, horizon_gpu_blob_cache_has_key(c, key), "A5 has_key");
    t_check(t, is_miss(c, 9001), "A5 a key-only mark is invisible to get()");
    horizon_gpu_blob_cache_close(c);

    /* A6. A different driver build must reset the file rather than read
     * it. On the SD card this also exercises ftruncate() through
     * fsFileSetSize, which nothing else here does on a non-empty file. */
    c = store_open(STORE_PATH, drv_b, sizeof(drv_b), 0);
    if (t_check(t, c != NULL, "A6 open under a different driver identity")) {
        horizon_gpu_blob_cache_get_stats(c, &st);
        t_check(t, st.was_reset, "A6 the other build's file is reset");
        t_check(t, st.entries == 0 && is_miss(c, 7),
                "A6 and none of its entries is served");
        /* Measured with the store CLOSED. Run 26 made this check while
         * it was open, got -1 back, compared it as a signed long and
         * announced a pass for something it had never looked at. */
        uint64_t after_reset = st.file_size;
        horizon_gpu_blob_cache_close(c);

        long on_card = file_size_checked(t, STORE_PATH,
                                         "A6 the reset file is readable");
        t_check(t, on_card >= 0 && (uint64_t)on_card == after_reset,
                "A6 the store's own size is the size on the card");
        t_check(t, on_card >= 0 && on_card < 4096,
                "A6 ftruncate() actually shortened the file on this "
                "filesystem");
    }

    /* A7. Truncation, produced here rather than simulated: write, close,
     * cut the tail off with a plain rewrite, reopen. */
    remove(STORE_PATH);
    c = store_open(STORE_PATH, drv_a, sizeof(drv_a), 0);
    if (t_check(t, c != NULL, "A7 open for the truncation case")) {
        for (uint32_t i = 0; i < 5; i++)
            put_n(c, i, 100);
        horizon_gpu_blob_cache_close(c);

        long size = file_size_checked(t, STORE_PATH,
                                      "A7 the written file is readable");
        bool cut = false;
        if (size > 40) {
            uint8_t *buf = malloc((size_t)size - 40);
            FILE *f = fopen(STORE_PATH, "rb");
            if (buf != NULL && f != NULL &&
                fread(buf, 1, (size_t)size - 40, f) == (size_t)size - 40) {
                fclose(f);
                f = fopen(STORE_PATH, "wb");
                cut = f != NULL &&
                      fwrite(buf, 1, (size_t)size - 40, f) ==
                          (size_t)size - 40;
                if (f != NULL)
                    fclose(f);
            } else if (f != NULL) {
                fclose(f);
            }
            free(buf);
        }
        t_check(t, cut, "A7 cut the last record short");

        c = store_open(STORE_PATH, drv_a, sizeof(drv_a), 0);
        if (t_check(t, c != NULL, "A7 a torn file still opens")) {
            horizon_gpu_blob_cache_get_stats(c, &st);
            t_check(t, st.was_truncated, "A7 the tear was noticed");
            t_check(t, st.entries == 4, "A7 the four whole entries are there");
            bool earlier = true;
            for (uint32_t i = 0; i < 4; i++)
                earlier = get_is(c, i, 100) && earlier;
            t_check(t, earlier, "A7 everything before the tear survived");
            t_check(t, is_miss(c, 4), "A7 the torn entry is gone, not wrong");
            t_check(t, put_n(c, 4, 100),
                    "A7 and the repaired file takes a new entry");
            horizon_gpu_blob_cache_close(c);
        }
    }

    /* A8. Compaction, in place, on the real filesystem. */
    remove(STORE_PATH);
    c = store_open(STORE_PATH, drv_a, sizeof(drv_a), 32u * 1024u);
    if (t_check(t, c != NULL, "A8 open with a 32 KiB ceiling")) {
        all = true;
        t0 = now_us();
        for (uint32_t i = 0; i < 100; i++)
            all = put_n(c, i, 1024) && all;
        t_check(t, all, "A8 100 KiB of entries into a 32 KiB cache");
        horizon_gpu_blob_cache_get_stats(c, &st);
        t_note(t, "A8 %u compactions over %llu us, file %llu bytes",
               st.compactions, (unsigned long long)(now_us() - t0),
               (unsigned long long)st.file_size);
        t_check(t, st.compactions > 0, "A8 it compacted rather than grew");
        /* Run 26 measured 70 compactions for 100 puts, because compaction
         * used to refill the file to the ceiling and the very next put
         * crossed it again. It now compacts to a watermark below the
         * ceiling, so a compaction has to buy many puts. 100 KiB into a
         * 32 KiB cache cannot need more than a handful. */
        t_check(t, st.compactions <= 10,
                "A8 and did not compact on nearly every put");
        t_check(t, st.file_size <= 32u * 1024u,
                "A8 and stayed inside the ceiling");
        t_check(t, get_is(c, 99, 1024), "A8 the newest entry is still there");
        horizon_gpu_blob_cache_close(c);

        long on_card = file_size_checked(t, STORE_PATH,
                                         "A8 the compacted file is readable");
        t_check(t, on_card >= 0 && (uint64_t)on_card <= 32u * 1024u,
                "A8 and the card agrees it is inside the ceiling");

        c = store_open(STORE_PATH, drv_a, sizeof(drv_a), 32u * 1024u);
        if (t_check(t, c != NULL, "A8 a compacted cache reopens")) {
            horizon_gpu_blob_cache_get_stats(c, &st);
            t_check(t, !st.was_reset && !st.was_truncated,
                    "A8 cleanly — the in-place rewrite left a valid file");
            t_check(t, get_is(c, 99, 1024),
                    "A8 and the newest entry survived it");
            horizon_gpu_blob_cache_close(c);
        }
    }

    remove(STORE_PATH);
}

/* ---------------------------------------------------------------- */
/* Section B — Mesa's disk_cache API                                 */
/* ---------------------------------------------------------------- */

static void section_b(test_ctx *t)
{
    t_note(t, "--- B: Mesa's disk_cache API ---");

    /* Point the cache somewhere this test owns. MESA_SHADER_CACHE_DIR is
     * read through os_get_option, and setenv works in-process even
     * though libnx never populates environ from outside. */
    setenv("MESA_SHADER_CACHE_DIR", TEST_DIR "/mesa_cache", 1);
    setenv("MESA_SHADER_CACHE_SHOW_STATS", "1", 1);

    struct disk_cache *cache = disk_cache_create("t_shader_cache", "id0", 0);
    if (!t_check(t, cache != NULL,
                 "B1 disk_cache_create returns a cache — the thing that "
                 "returned NULL on this platform until now"))
        return;

    static const char payload[] =
        "a shader binary, as far as the cache is concerned";
    cache_key key;
    disk_cache_compute_key(cache, "some nir", 8, key);

    size_t size = 0;
    void *got = disk_cache_get(cache, key, &size);
    t_check(t, got == NULL, "B2 miss before anything is stored");
    free(got);

    uint64_t t0 = now_us();
    disk_cache_put(cache, key, payload, sizeof(payload), NULL);
    disk_cache_wait_for_idle(cache);
    t_note(t, "B3 put + wait_for_idle took %llu us",
           (unsigned long long)(now_us() - t0));

    t0 = now_us();
    got = disk_cache_get(cache, key, &size);
    t_note(t, "B3 get took %llu us", (unsigned long long)(now_us() - t0));
    t_check(t, got != NULL, "B3 hit after the store");
    t_check(t, got != NULL && size == sizeof(payload) &&
                   memcmp(got, payload, sizeof(payload)) == 0,
            "B3 and the bytes are the bytes that went in");
    free(got);

    /* An entry well past the 64 KiB that Mesa's blob-callback path caps
     * at, because a pipeline binary can be larger than that and a cache
     * that silently misses on the big ones is worse than no cache. */
    const size_t big_size = 256u * 1024u;
    uint8_t *big = malloc(big_size);
    if (big != NULL) {
        for (size_t i = 0; i < big_size; i++)
            big[i] = (uint8_t)(i * 7u + 3u);

        cache_key big_key;
        disk_cache_compute_key(cache, "a big one", 9, big_key);
        t0 = now_us();
        disk_cache_put(cache, big_key, big, big_size, NULL);
        disk_cache_wait_for_idle(cache);
        t_note(t, "B4 put(256 KiB) + wait took %llu us",
               (unsigned long long)(now_us() - t0));

        size = 0;
        void *back = disk_cache_get(cache, big_key, &size);
        t_check(t, back != NULL && size == big_size &&
                       memcmp(back, big, big_size) == 0,
                "B4 a 256 KiB entry survives put and get");
        free(back);
        free(big);
    }

    /* put_key/has_key, which on this backend are records and not an
     * mmap'd table. */
    cache_key mark;
    disk_cache_compute_key(cache, "a mark", 6, mark);
    t_check(t, !disk_cache_has_key(cache, mark), "B5 has_key before put_key");
    disk_cache_put_key(cache, mark);
    t_check(t, disk_cache_has_key(cache, mark), "B5 has_key after put_key");

    disk_cache_destroy(cache);

    /* B6. The whole point: a new cache object, over the same directory,
     * finds what the previous one wrote. This is the in-process half of
     * "survives a relaunch"; section C is the other half. */
    cache = disk_cache_create("t_shader_cache", "id0", 0);
    if (t_check(t, cache != NULL, "B6 reopen the cache")) {
        size = 0;
        got = disk_cache_get(cache, key, &size);
        t_check(t, got != NULL && size == sizeof(payload) &&
                       memcmp(got, payload, sizeof(payload)) == 0,
                "B6 the entry written by the previous cache object is served");
        free(got);

        /* A different driver id must not read the first one's entries. */
        disk_cache_destroy(cache);
        cache = disk_cache_create("t_shader_cache", "id1", 0);
        if (t_check(t, cache != NULL, "B7 open with a different driver id")) {
            size = 0;
            got = disk_cache_get(cache, key, &size);
            t_check(t, got == NULL,
                    "B7 a different driver id does not read the first's "
                    "entries");
            free(got);
            disk_cache_destroy(cache);
        }
    }

    unsetenv("MESA_SHADER_CACHE_DIR");
    unsetenv("MESA_SHADER_CACHE_SHOW_STATS");
}

/* ---------------------------------------------------------------- */
/* Section C — across launches                                       */
/* ---------------------------------------------------------------- */

static void section_c(test_ctx *t)
{
    horizon_gpu_blob_cache *c;
    horizon_gpu_blob_cache_stats st;

    t_note(t, "--- C: across launches ---");

    c = store_open(PERSIST_PATH, drv_a, sizeof(drv_a), 0);
    if (!t_check(t, c != NULL, "C1 open the cache kept between runs"))
        return;

    horizon_gpu_blob_cache_get_stats(c, &st);

    bool warm = st.entries > 0;
    t_note(t, "C1 this launch found %u entries (%s)", st.entries,
           warm ? "WARM — a previous launch filled it"
                : "COLD — first run on this console, which is a pass");

    if (warm) {
        bool all = true;
        for (uint32_t i = 0; i < 32; i++)
            all = get_is(c, 500 + i, 256) && all;
        t_check(t, all,
                "C2 every entry the previous launch wrote came back intact");
        t_check(t, !st.was_reset,
                "C2 and the file did not have to be reset to do it");
        t_check(t, !st.was_truncated,
                "C2 nor truncated — the previous launch's writes were whole");
    } else {
        t_note(t, "C2 skipped: nothing to read back yet. Run this test a "
                  "second time and C2 becomes three real checks.");
    }

    /* Refill for the next launch either way, so a second run always has
     * something to find. */
    bool wrote = true;
    for (uint32_t i = 0; i < 32; i++)
        wrote = put_n(c, 500 + i, 256) && wrote;
    t_check(t, wrote, "C3 wrote 32 entries for the next launch to find");

    horizon_gpu_blob_cache_close(c);
    t_note(t, "C3 %s is left on the card on purpose. Delete it to make the "
              "next run cold again.", PERSIST_PATH);
}

int run_test(test_ctx *t)
{
    mkdir(TEST_DIR, 0777);

    section_a(t);
    section_b(t);
    section_c(t);

    return 0;
}
