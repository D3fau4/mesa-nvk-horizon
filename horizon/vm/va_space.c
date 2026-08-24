/*
 * horizon_gpu — live-interval set implementation. Pure C11, libnx-free.
 *
 * A sorted array keeps lookups deterministic and the memory footprint
 * proportional to live mappings; Phase 1 workloads hold at most a handful
 * of intervals per reservation.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "va_space.h"
#include "../memory/align.h"

void horizon_va_set_init(horizon_va_set *set)
{
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}

void horizon_va_set_fini(horizon_va_set *set)
{
    free(set->items);
    horizon_va_set_init(set);
}

/* Index of the first interval with items[i].offset >= offset. */
static uint32_t va_set_lower_bound(const horizon_va_set *set, uint64_t offset)
{
    uint32_t lo = 0, hi = set->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (set->items[mid].offset < offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

bool horizon_va_set_overlaps(const horizon_va_set *set, uint64_t offset,
                             uint64_t size)
{
    uint64_t end;
    if (size == 0 || !horizon_add_u64(offset, size, &end))
        return true; /* a degenerate or wrapping range is never addable */

    uint32_t i = va_set_lower_bound(set, offset);
    /* Predecessor may reach into [offset, end). */
    if (i > 0) {
        const horizon_va_interval *p = &set->items[i - 1];
        if (p->offset + p->size > offset)
            return true;
    }
    /* Successor (or exact-offset match) may start before `end`. */
    if (i < set->count && set->items[i].offset < end)
        return true;
    return false;
}

horizon_gpu_status horizon_va_set_add(horizon_va_set *set, uint64_t offset,
                                      uint64_t size)
{
    if (size == 0)
        return HORIZON_GPU_ERR_INVALID_ARG;
    uint64_t end;
    if (!horizon_add_u64(offset, size, &end))
        return HORIZON_GPU_ERR_OVERFLOW;
    if (horizon_va_set_overlaps(set, offset, size))
        return HORIZON_GPU_ERR_BUSY;

    if (set->count == set->capacity) {
        uint32_t new_cap = set->capacity ? set->capacity * 2 : 8;
        horizon_va_interval *items =
            realloc(set->items, (size_t)new_cap * sizeof(*items));
        if (!items)
            return HORIZON_GPU_ERR_OUT_OF_MEMORY;
        set->items = items;
        set->capacity = new_cap;
    }

    uint32_t i = va_set_lower_bound(set, offset);
    memmove(&set->items[i + 1], &set->items[i],
            (size_t)(set->count - i) * sizeof(*set->items));
    set->items[i] = (horizon_va_interval){ .offset = offset, .size = size };
    set->count++;
    return HORIZON_GPU_OK;
}

horizon_gpu_status horizon_va_set_remove(horizon_va_set *set, uint64_t offset)
{
    uint32_t i = va_set_lower_bound(set, offset);
    if (i >= set->count || set->items[i].offset != offset)
        return HORIZON_GPU_ERR_INVALID_ARG;
    memmove(&set->items[i], &set->items[i + 1],
            (size_t)(set->count - i - 1) * sizeof(*set->items));
    set->count--;
    return HORIZON_GPU_OK;
}

horizon_gpu_status horizon_va_set_remove_range(horizon_va_set *set,
                                               uint64_t offset,
                                               uint64_t size)
{
    if (size == 0)
        return HORIZON_GPU_ERR_INVALID_ARG;
    uint64_t end;
    if (!horizon_add_u64(offset, size, &end))
        return HORIZON_GPU_ERR_OVERFLOW;

    /* The interval that starts at or before `offset`. lower_bound lands
     * on the first one starting at or after it, so an exact start is
     * `i` and anything else can only be `i - 1`. */
    uint32_t i = va_set_lower_bound(set, offset);
    if (!(i < set->count && set->items[i].offset == offset)) {
        if (i == 0)
            return HORIZON_GPU_ERR_INVALID_ARG;
        i--;
    }

    const uint64_t iv_off = set->items[i].offset;
    const uint64_t iv_end = iv_off + set->items[i].size;
    /* iv_end cannot wrap: horizon_va_set_add rejected the range that
     * would have made it. */
    if (offset < iv_off || end > iv_end)
        return HORIZON_GPU_ERR_INVALID_ARG;

    const bool keep_left = offset > iv_off;
    const bool keep_right = end < iv_end;

    if (!keep_left && !keep_right) {
        /* The whole interval; the same work horizon_va_set_remove does. */
        memmove(&set->items[i], &set->items[i + 1],
                (size_t)(set->count - i - 1) * sizeof(*set->items));
        set->count--;
        return HORIZON_GPU_OK;
    }

    if (keep_left && !keep_right) {
        set->items[i].size = offset - iv_off;
        return HORIZON_GPU_OK;
    }

    if (!keep_left && keep_right) {
        set->items[i].offset = end;
        set->items[i].size = iv_end - end;
        return HORIZON_GPU_OK;
    }

    /* A hole in the middle: one interval becomes two, so this is the
     * only path that needs a slot. Grown BEFORE anything is modified —
     * a failed realloc must leave the set exactly as it was, or the
     * caller's next unbind would be bookkeeping against a set that
     * already half-recorded this one. */
    if (set->count == set->capacity) {
        uint32_t new_cap = set->capacity ? set->capacity * 2 : 8;
        horizon_va_interval *items =
            realloc(set->items, (size_t)new_cap * sizeof(*items));
        if (!items)
            return HORIZON_GPU_ERR_OUT_OF_MEMORY;
        set->items = items;
        set->capacity = new_cap;
    }

    memmove(&set->items[i + 2], &set->items[i + 1],
            (size_t)(set->count - i - 1) * sizeof(*set->items));
    set->items[i].size = offset - iv_off;
    set->items[i + 1] = (horizon_va_interval){ .offset = end,
                                               .size = iv_end - end };
    set->count++;
    return HORIZON_GPU_OK;
}
