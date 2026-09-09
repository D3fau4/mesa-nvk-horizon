/*
 * horizon_gpu — pure syncpoint arithmetic: 64-bit shadow extension and
 * timeout conversion. Pure C11, libnx-free, unit-tested on the host
 * (tests/host/h_syncpt_math.c).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_SYNC_SYNCPT_MATH_H
#define HORIZON_SYNC_SYNCPT_MATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Extends a 32-bit hardware syncpoint value to 64 bits against a 64-bit
 * reference (the per-channel shadow of requested increments). Valid
 * while the true value lies within (reference - 2^31, reference + 2^31);
 * submit keeps the in-flight window far below that. */
static inline uint64_t horizon_syncpt_extend(uint64_t reference, uint32_t hw)
{
    uint64_t candidate = (reference & ~UINT64_C(0xffffffff)) | hw;
    if (candidate + UINT64_C(0x80000000) < reference) {
        /* hw wrapped past the reference's low word */
        candidate += UINT64_C(0x100000000);
    } else if (candidate > reference + UINT64_C(0x80000000) &&
               candidate >= UINT64_C(0x100000000)) {
        /* hw is behind a reference that just crossed a wrap boundary */
        candidate -= UINT64_C(0x100000000);
    }
    return candidate;
}

/* Nanoseconds -> microseconds for libnx waits: rounds up (so a nonzero
 * wait never becomes a busy-poll zero) and saturates at INT32_MAX µs.
 * The reference lost a factor of 1000 here once (drm_shim.c:980-982);
 * this is the single conversion point. */
static inline int32_t horizon_timeout_ns_to_us_clamped(uint64_t ns)
{
    uint64_t us = ns / 1000 + (ns % 1000 != 0);
    if (us > (uint64_t)INT32_MAX)
        return INT32_MAX;
    return (int32_t)us;
}

/* --- Recovering a syncpoint's value from the wait predicate alone -------
 *
 * NVHOST_IOCTL_CTRL_SYNCPT_READ ("what is the counter?") and
 * NVHOST_IOCTL_CTRL_SYNCPT_WAIT ("has (id, threshold) been reached?") are
 * different ioctls, and an environment may implement the second without
 * the first. The second is enough, because the thresholds a counter C has
 * reached are exactly the modular half-space
 *
 *     { T : (int32_t)(C - T) >= 0 }  =  { C - 2^31 + 1, ..., C }
 *
 * whose upper end *is* C. Anchoring inside that half-space makes the
 * predicate monotone in the offset from the anchor, so a binary search
 * finds C in 31 probes, plus one to place the anchor and one to verify.
 *
 * This file never learns what an ioctl is: the predicate is the caller's,
 * so the search is pure arithmetic and unit-tested as such
 * (tests/host/h_syncpt_math.c). */

typedef enum horizon_syncpt_search_status {
    HORIZON_SYNCPT_SEARCH_OK = 0,
    /* The predicate itself could not be answered. */
    HORIZON_SYNCPT_SEARCH_PROBE_FAILED,
    /* The counter advanced past the result while the search ran. */
    HORIZON_SYNCPT_SEARCH_MOVED,
} horizon_syncpt_search_status;

/* Answers "has `threshold` been reached?". Returns false — leaving
 * *out_reached untouched — when the question could not be put. */
typedef bool (*horizon_syncpt_probe_fn)(void *ctx, uint32_t threshold,
                                        bool *out_reached);

/* Binary-searches the syncpoint's current value using `probe` alone.
 *
 * The result is never an overestimate: every "reached" answer stays true
 * for a monotonically increasing counter, so the value returned is one the
 * counter had at or before the last probe. The final probe turns that into
 * an exact test — it asks whether value + 1 has been reached, which is
 * true precisely when the counter has moved on — so OK means the returned
 * value is the counter's value at that instant, not an approximation.
 * `out_probes` (optional) receives the number of probes actually issued,
 * on every path including failure. */
static inline horizon_syncpt_search_status
horizon_syncpt_search_value(horizon_syncpt_probe_fn probe, void *ctx,
                            uint32_t *out_value, uint32_t *out_probes)
{
    uint32_t probes = 0;
    bool reached = false;

#define HORIZON_SYNCPT_PROBE(t)                                              \
    do {                                                                     \
        if (!probe(ctx, (t), &reached)) {                                    \
            if (out_probes != NULL)                                          \
                *out_probes = probes;                                        \
            return HORIZON_SYNCPT_SEARCH_PROBE_FAILED;                       \
        }                                                                    \
        probes++;                                                            \
    } while (0)

    /* Place the anchor. For any v exactly one of {v, v + 2^31} lies in the
     * half-space — the two conditions partition the 2^32 possible values
     * of C - v — so a single probe always finds one. */
    HORIZON_SYNCPT_PROBE(0);
    const uint32_t anchor = reached ? UINT32_C(0) : UINT32_C(0x80000000);

    /* Largest d in [0, 2^31 - 1] with reached(anchor + d). d = 0 holds by
     * construction, so the invariant is true before the first iteration. */
    uint32_t lo = 0, hi = UINT32_C(0x7fffffff);
    while (lo < hi) {
        /* Round the midpoint up so it is always strictly above `lo`;
         * rounding down would let lo == mid spin forever. */
        const uint32_t mid = lo + (hi - lo + 1) / 2;
        HORIZON_SYNCPT_PROBE(anchor + mid);
        if (reached)
            lo = mid;
        else
            hi = mid - 1;
    }

    const uint32_t value = anchor + lo;

    /* A counter that moved would have made this an underestimate — the
     * direction that makes a later fence look signalled early, which is
     * the failure worth catching. */
    HORIZON_SYNCPT_PROBE(value + 1);
    if (reached) {
        if (out_probes != NULL)
            *out_probes = probes;
        return HORIZON_SYNCPT_SEARCH_MOVED;
    }

#undef HORIZON_SYNCPT_PROBE

    if (out_probes != NULL)
        *out_probes = probes;
    *out_value = value;
    return HORIZON_SYNCPT_SEARCH_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_SYNC_SYNCPT_MATH_H */
