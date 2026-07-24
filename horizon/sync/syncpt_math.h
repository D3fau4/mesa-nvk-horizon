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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Extends a 32-bit hardware syncpoint value to 64 bits against a 64-bit
 * reference (the per-channel shadow of requested increments,
 * docs/synchronization.md § 1.1). Valid while the true value lies within
 * (reference - 2^31, reference + 2^31); submit keeps the in-flight window
 * far below that. */
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
 * this is the single conversion point (docs/synchronization.md § 6). */
static inline int32_t horizon_timeout_ns_to_us_clamped(uint64_t ns)
{
    uint64_t us = ns / 1000 + (ns % 1000 != 0);
    if (us > (uint64_t)INT32_MAX)
        return INT32_MAX;
    return (int32_t)us;
}

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_SYNC_SYNCPT_MATH_H */
