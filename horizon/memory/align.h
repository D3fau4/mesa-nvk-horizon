/*
 * horizon_gpu — alignment and overflow-checked arithmetic helpers.
 *
 * Every size/offset/alignment computation in the layer goes through these
 * (CLAUDE.md: check for overflow on every such computation). Pure C11,
 * libnx-free, unit-tested on the host (tests/host/h_align.c).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_MEMORY_ALIGN_H
#define HORIZON_MEMORY_ALIGN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline bool horizon_is_pow2_u64(uint64_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

static inline bool horizon_is_aligned_u64(uint64_t v, uint64_t align)
{
    /* Caller guarantees `align` is a power of two. */
    return (v & (align - 1)) == 0;
}

/* out = a + b, false on wrap. */
static inline bool horizon_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    return !__builtin_add_overflow(a, b, out);
}

/* out = a * b, false on wrap. */
static inline bool horizon_mul_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    return !__builtin_mul_overflow(a, b, out);
}

/* out = v rounded up to `align` (a power of two), false on wrap.
 * The check happens before the rounding is used, not after. */
static inline bool horizon_align_up_u64(uint64_t v, uint64_t align,
                                        uint64_t *out)
{
    if (!horizon_is_pow2_u64(align))
        return false;
    uint64_t sum;
    if (!horizon_add_u64(v, align - 1, &sum))
        return false;
    *out = sum & ~(align - 1);
    return true;
}

/* True when [offset, offset + size) fits inside a container of
 * `container_size` bytes, with no intermediate wrap. */
static inline bool horizon_range_fits_u64(uint64_t offset, uint64_t size,
                                          uint64_t container_size)
{
    uint64_t end;
    if (!horizon_add_u64(offset, size, &end))
        return false;
    return end <= container_size;
}

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_MEMORY_ALIGN_H */
