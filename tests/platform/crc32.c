/*
 * The CRC-32 the console actually runs, against vectors from outside
 * this repository.
 *
 * WHY THIS CASE EXISTS AND WHERE IT IS. horizon/cache/crc32.c has two
 * implementations of one function. The table-driven loop is what a host
 * build compiles, and tests/host/h_blob_cache.c pins it hard: published
 * vectors, and all 256 table entries regenerated from the polynomial
 * and compared one by one. THE CONSOLE RUNS NEITHER OF THOSE LINES.
 * Every cross build here passes -march=armv8-a+crc+crypto (Makefile,
 * toolchain/horizon-aarch64.cross), so __ARM_FEATURE_CRC32 is defined,
 * the table is compiled out, and what ships is the CRC32B/CRC32X
 * branch — which had no known-answer coverage of any class until this
 * case. `scripts/run-host-tests.sh` builds with `cc` and no -march and
 * cannot reach it; a comment in crc32.c claimed an aarch64 host-suite
 * run under user-mode emulation, and no such build exists in the tree.
 *
 * IN `platform` BECAUSE IT NEEDS NOTHING. No nv service, no GPU, no
 * display — it is arithmetic, like `sysconf` beside it, and it belongs
 * with the cheapest things a triage run reaches first. A wrong checksum
 * here means the shader cache silently rejects every entry it wrote, or
 * worse accepts one it should not, and that is a failure the Vulkan
 * suites would report as something else entirely.
 *
 * WHAT IT ASKS, IN THREE STEPS THAT EACH REST ON THE ONE BEFORE.
 *
 *   A  this build really is the hardware one. Every check below is
 *      about the instruction path, so a build that quietly lost +crc
 *      would pass them all while testing the table again.
 *   B  the byte path against the polynomial: feeding a single byte b to
 *      a running value of 0 must give the table entry for b, which is
 *      recomputed here from HORIZON_CRC32_POLY's definition. 256 of
 *      them, the same check the host suite makes of the table.
 *   C  the whole-word path against the byte path, at every alignment
 *      and every length, plus published vectors for both. The word loop
 *      is the part with no host twin: it steps bytes to an 8-byte
 *      boundary, consumes __crc32d words, then a tail, and the three of
 *      them can disagree with each other in ways one number cannot see.
 *
 * THE VECTORS COME FROM zlib. Python's zlib.crc32 over the same bytes,
 * which is the IEEE 802.3 / RFC 1952 CRC-32 this module claims to be:
 *
 *   ""                       0x00000000
 *   "a"                      0xE8B7BE43
 *   "123456789"              0xCBF43926   (the standard check value)
 *   64 zero bytes            0x758D6336
 *   bytes 0x00..0xFF         0x29058C73
 *   the pangram (43 bytes)   0x414FA339
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "../../horizon/cache/crc32.h"
#include "common/testfw.h"

/* The reflected IEEE 802.3 polynomial, spelled out again rather than
 * included: this case is here to check crc32.c against something, and
 * borrowing crc32.c's own constant would make half of that circular.
 * Source: RFC 1952 § 8, the reverse of 0x04C11DB7. */
#define CRC32_POLY UINT32_C(0xEDB88320)

/* Long enough to cover several whole words after the worst alignment
 * skew, and odd at the end so the tail loop is never empty. */
#define BUF_LEN 259u

/* One published vector. */
typedef struct {
    const char *what;
    const uint8_t *data;
    size_t len;
    uint32_t want;
} crc_vector;

static const uint8_t pangram[] =
    "The quick brown fox jumps over the lazy dog";
static const uint8_t zeros64[64] = { 0 };
static uint8_t all_bytes[256];

TEST_CASE_DECL(platform, crc32)
{
    /* --- A: this is the build that ships ---------------------------- */
#if defined(__ARM_FEATURE_CRC32)
    const bool hw_path = true;
#else
    const bool hw_path = false;
#endif
    t_check(t, hw_path,
            "__ARM_FEATURE_CRC32 is defined, so the CRC32B/CRC32X branch "
            "of horizon/cache/crc32.c is what these checks reach");
    if (!hw_path) {
        t_note(t, "this .nro was built without +crc: everything below is "
                  "about the table, which the host suite already covers, "
                  "and nothing here says anything about the shipped path");
    }

    for (uint32_t i = 0; i < 256u; i++)
        all_bytes[i] = (uint8_t)i;

    /* --- B: the byte path against the polynomial -------------------- */
    {
        uint32_t first_bad = 256u;
        for (uint32_t i = 0; i < 256u; i++) {
            uint32_t want = i;
            for (unsigned bit = 0; bit < 8u; bit++)
                want = (want & 1u) ? (CRC32_POLY ^ (want >> 1))
                                   : (want >> 1);

            const uint8_t b = (uint8_t)i;
            if (horizon_crc32_update(0, &b, 1) != want && first_bad == 256u)
                first_bad = i;
        }
        if (first_bad != 256u) {
            t_note(t, "first byte whose single-byte update disagrees with "
                      "the polynomial: 0x%02x", (unsigned)first_bad);
        }
        t_check(t, first_bad == 256u,
                "all 256 single-byte updates match the IEEE 802.3 "
                "polynomial");
    }

    /* --- C: published vectors --------------------------------------- */
    {
        const crc_vector vectors[] = {
            { "the empty input", (const uint8_t *)"", 0, 0x00000000u },
            { "\"a\"", (const uint8_t *)"a", 1, 0xE8B7BE43u },
            { "the standard \"123456789\"", (const uint8_t *)"123456789", 9,
              0xCBF43926u },
            { "64 zero bytes", zeros64, sizeof(zeros64), 0x758D6336u },
            { "the 43-byte pangram", pangram, sizeof(pangram) - 1,
              0x414FA339u },
            { "every byte 0x00..0xFF", all_bytes, sizeof(all_bytes),
              0x29058C73u },
        };
        const uint32_t n = (uint32_t)(sizeof(vectors) / sizeof(vectors[0]));

        for (uint32_t i = 0; i < n; i++) {
            const uint32_t got =
                horizon_crc32(vectors[i].data, vectors[i].len);
            t_check(t, got == vectors[i].want,
                    "crc32 of %s is 0x%08x (zlib says 0x%08x)",
                    vectors[i].what, (unsigned)got,
                    (unsigned)vectors[i].want);
        }

        t_check(t, horizon_crc32(NULL, 0) == 0,
                "crc32(NULL, 0) is defined and is 0");
    }

    /* --- C: the word path against the byte path --------------------- *
     *
     * One buffer, read at all eight alignments and every length up to
     * BUF_LEN. The one-call answer runs the word loop; the byte-at-a-
     * time answer runs only the byte loops, which B has just pinned
     * against the polynomial. Anything the word loop gets wrong — the
     * step to the 8-byte boundary, the memcpy'd load, the tail — makes
     * the two disagree, and no single vector can show that.
     *
     * `skew + BUF_LEN` stays inside the array, so the read is in
     * bounds at every alignment. */
    {
        static uint8_t buf[8u + BUF_LEN];
        uint32_t mismatches = 0;
        uint32_t first_skew = 0, first_len = 0;

        for (uint32_t i = 0; i < sizeof(buf); i++)
            buf[i] = (uint8_t)(i * 31u + 7u);

        for (uint32_t skew = 0; skew < 8u; skew++) {
            const uint8_t *p = buf + skew;
            for (uint32_t len = 0; len <= BUF_LEN; len++) {
                uint32_t byte_at_a_time = horizon_crc32_init();
                for (uint32_t k = 0; k < len; k++)
                    byte_at_a_time =
                        horizon_crc32_update(byte_at_a_time, p + k, 1);
                byte_at_a_time = horizon_crc32_final(byte_at_a_time);

                if (horizon_crc32(p, len) != byte_at_a_time) {
                    if (mismatches == 0) {
                        first_skew = skew;
                        first_len = len;
                    }
                    mismatches++;
                }
            }
        }

        if (mismatches != 0) {
            t_note(t, "%u of %u (alignment, length) pairs disagree; first "
                      "at alignment %u length %u", mismatches,
                   8u * (BUF_LEN + 1u), first_skew, first_len);
        }
        t_check(t, mismatches == 0,
                "the whole-buffer answer equals the byte-at-a-time answer "
                "at every alignment 0..7 and every length 0..%u",
                (unsigned)BUF_LEN);
    }

    /* Splitting a buffer at an arbitrary point must not change the
     * answer either: the blob cache checksums a header and a payload
     * through one running value, so this is the shape it uses. */
    {
        uint32_t c = horizon_crc32_init();
        c = horizon_crc32_update(c, "1234", 4);
        c = horizon_crc32_update(c, "56789", 5);
        t_check(t, horizon_crc32_final(c) == 0xCBF43926u,
                "a crc split across two updates matches one call");
    }

    return 0;
}
