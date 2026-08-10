/*
 * horizon_gpu — CRC-32 (IEEE 802.3 / zlib polynomial). See crc32.h for
 * why this layer carries its own.
 *
 * Table-driven, one byte at a time. The table is built once on first
 * use rather than written out as 256 constants: a generated table in a
 * source file is 256 numbers nobody can check by reading, while the
 * four lines that build it are the definition itself.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "crc32.h"

/* Reflected form of the IEEE 802.3 polynomial 0x04C11DB7 (RFC 1952 § 8).
 * Reflected because the octets are fed least-significant bit first,
 * which is what makes this the same CRC-32 as gzip, PNG and zlib. */
#define HORIZON_CRC32_POLY UINT32_C(0xEDB88320)

/* Zero-initialised, and entry 1 is the sentinel: for this polynomial
 * table[1] is 0x77073096, so a table that has never been built is the
 * only one whose entry 1 is zero. */
static uint32_t horizon_crc32_table[256];

static void horizon_crc32_build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (unsigned bit = 0; bit < 8; bit++)
            c = (c & 1u) ? (HORIZON_CRC32_POLY ^ (c >> 1)) : (c >> 1);
        horizon_crc32_table[i] = c;
    }
}

uint32_t horizon_crc32_init(void)
{
    return UINT32_C(0xFFFFFFFF);
}

uint32_t horizon_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (p == NULL || len == 0)
        return crc;

    /* Built on first use. Deliberately not guarded by a lock: every
     * writer computes the same 256 values and writes them to the same
     * places, so a race produces the table either way. The blob cache
     * serialises its callers anyway (blob_cache.h, CONCURRENCY). */
    if (horizon_crc32_table[1] == 0)
        horizon_crc32_build_table();

    for (size_t i = 0; i < len; i++)
        crc = horizon_crc32_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);

    return crc;
}

uint32_t horizon_crc32_final(uint32_t crc)
{
    return crc ^ UINT32_C(0xFFFFFFFF);
}

uint32_t horizon_crc32(const void *data, size_t len)
{
    return horizon_crc32_final(
        horizon_crc32_update(horizon_crc32_init(), data, len));
}
