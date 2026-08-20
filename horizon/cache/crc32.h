/*
 * horizon_gpu — CRC-32 (the IEEE 802.3 / zlib polynomial).
 *
 * WHY THIS IS NOT MESA'S. horizon/ may not include a Mesa header
 * (CLAUDE.md layer rules, enforced by scripts/check-layering.sh), and
 * the blob cache's whole robustness story is a checksum, so it needs one
 * that is in this layer. It is forty lines and it is exercised against
 * published test vectors in tests/host/h_blob_cache.c.
 *
 * The values this produces are byte-for-byte the ordinary CRC-32: the
 * reflected polynomial 0xEDB88320 (the reverse of 0x04C11DB7), an
 * initial value of ~0, and a final complement — the same convention as
 * zlib's crc32(), gzip and PNG. Source: IEEE 802.3 clause 3.2.9 and
 * RFC 1952 § 8.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_CACHE_CRC32_H
#define HORIZON_CACHE_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-shot: crc32 of `len` bytes at `data`. crc32(NULL, 0) is 0. */
uint32_t horizon_crc32(const void *data, size_t len);

/* Incremental. Start with horizon_crc32_init(), feed with
 * horizon_crc32_update(), finish with horizon_crc32_final(). Splitting a
 * buffer at any point gives the same answer as one call. */
uint32_t horizon_crc32_init(void);
uint32_t horizon_crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t horizon_crc32_final(uint32_t crc);

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_CACHE_CRC32_H */
