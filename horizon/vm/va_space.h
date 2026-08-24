/*
 * horizon_gpu — live-interval bookkeeping for a VA reservation.
 *
 * Tracks the [offset, offset+size) segments currently mapped inside one
 * reservation, rejecting overlaps (memory-model § 3: a FIXED map must lie
 * strictly inside a reservation the caller owns, and mapping over a live
 * mapping is a programming error, detected and reported).
 *
 * Pure C11, libnx-free, unit-tested on the host (tests/host/h_va_space.c).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_VM_VA_SPACE_H
#define HORIZON_VM_VA_SPACE_H

#include <stdbool.h>
#include <stdint.h>

#include "horizon_gpu/result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct horizon_va_interval {
    uint64_t offset;
    uint64_t size;
} horizon_va_interval;

typedef struct horizon_va_set {
    horizon_va_interval *items; /* sorted by offset, non-overlapping */
    uint32_t count;
    uint32_t capacity;
} horizon_va_set;

void horizon_va_set_init(horizon_va_set *set);
void horizon_va_set_fini(horizon_va_set *set);

/* True when [offset, offset+size) overlaps any live interval. A wrapping
 * range is reported as overlapping (it can never be legally added). */
bool horizon_va_set_overlaps(const horizon_va_set *set, uint64_t offset,
                             uint64_t size);

/* Adds [offset, offset+size). Fails with INVALID_ARG on size == 0,
 * OVERFLOW on wrap, BUSY on overlap, OUT_OF_MEMORY on allocation. */
horizon_gpu_status horizon_va_set_add(horizon_va_set *set, uint64_t offset,
                                      uint64_t size);

/* Removes the interval starting exactly at `offset`. Fails with
 * INVALID_ARG when no interval starts there. */
horizon_gpu_status horizon_va_set_remove(horizon_va_set *set,
                                         uint64_t offset);

/* Removes [offset, offset+size), which must lie entirely inside ONE
 * live interval. Whatever of that interval lies before or after the
 * range stays live, so one call can turn one interval into two.
 *
 * This is what a partial unbind needs. Vulkan sparse binding lets an
 * application unbind a sub-range of a range it bound earlier, and D12 —
 * measured on hardware 2026-08-24, tests/t_sparse.c — says the hardware
 * allows it: unbinding inside a sparse reservation restores the
 * resolve-to-nothing state rather than punching a hole. The bookkeeping
 * had no way to express it; this is that way.
 *
 * Deliberately refuses a range that spans two intervals or reaches
 * outside every one of them. Those are two different requests wearing
 * one shape, and a caller that meant either should say so by calling
 * twice: silently removing "as much as is live" would make an unbind of
 * the wrong range look like a successful unbind of the right one.
 *
 * Fails INVALID_ARG on size == 0 or a range no single interval contains,
 * OVERFLOW on wrap, OUT_OF_MEMORY when a split needs a slot and cannot
 * get one — in which case the set is unchanged. */
horizon_gpu_status horizon_va_set_remove_range(horizon_va_set *set,
                                               uint64_t offset,
                                               uint64_t size);

static inline uint32_t horizon_va_set_count(const horizon_va_set *set)
{
    return set->count;
}

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_VM_VA_SPACE_H */
