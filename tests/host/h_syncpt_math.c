/*
 * Host unit tests — wrap-safe syncpoint arithmetic
 * (horizon/include/horizon_gpu/sync.h, horizon/sync/syncpt_math.h).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#include "../../horizon/include/horizon_gpu/sync.h"
#include "../../horizon/sync/syncpt_math.h"
#include "hostfw.h"

int main(void)
{
    /* Wrap-safe reached predicate (docs/synchronization.md § 1.1). */
    H_CHECK(horizon_gpu_syncpt_reached(5, 5), "equal reached");
    H_CHECK(horizon_gpu_syncpt_reached(6, 5), "past reached");
    H_CHECK(!horizon_gpu_syncpt_reached(4, 5), "behind not reached");
    /* Across the 32-bit wrap: current wrapped to 2, threshold near max. */
    H_CHECK(horizon_gpu_syncpt_reached(2, 0xFFFFFFF0),
            "reached across wrap");
    H_CHECK(!horizon_gpu_syncpt_reached(0xFFFFFFF0, 2),
            "not reached across wrap");
    /* The naive >= comparison gives the wrong answer for both cases
     * above; guard the exact boundary too. */
    H_CHECK(horizon_gpu_syncpt_reached(0, 0x80000001u),
            "just inside the 2^31 window");
    H_CHECK(!horizon_gpu_syncpt_reached(0, 1), "0 vs 1");

    /* 64-bit shadow extension. */
    H_CHECK(horizon_syncpt_extend(100, 100) == 100, "extend identity");
    H_CHECK(horizon_syncpt_extend(100, 90) == 90, "extend behind");
    H_CHECK(horizon_syncpt_extend(0x100000005ull, 0xFFFFFFF0u) ==
            0xFFFFFFF0ull, "extend borrows across wrap boundary");
    H_CHECK(horizon_syncpt_extend(0xFFFFFFF0ull, 2) == 0x100000002ull,
            "extend carries across wrap boundary");
    H_CHECK(horizon_syncpt_extend(0x2FFFFFFF5ull, 0xFFFFFFF6u) ==
            0x2FFFFFFF6ull, "extend same epoch high");
    /* Degenerate low references never underflow. */
    H_CHECK(horizon_syncpt_extend(5, 0xFFFFFFF0u) == 0xFFFFFFF0ull,
            "no underflow for low reference");

    /* ns -> µs conversion: round up, saturate (docs/synchronization.md
     * § 6 — the reference's 1000x unit bug). */
    H_CHECK(horizon_timeout_ns_to_us_clamped(0) == 0, "0 ns");
    H_CHECK(horizon_timeout_ns_to_us_clamped(1) == 1, "1 ns rounds up");
    H_CHECK(horizon_timeout_ns_to_us_clamped(1000) == 1, "1 us exact");
    H_CHECK(horizon_timeout_ns_to_us_clamped(1001) == 2, "1001 ns");
    H_CHECK(horizon_timeout_ns_to_us_clamped(UINT64_C(2000000000)) ==
            2000000, "2 s = 2,000,000 us (not 2,000,000,000)");
    H_CHECK(horizon_timeout_ns_to_us_clamped(UINT64_MAX) == INT32_MAX,
            "saturates at INT32_MAX");

    return h_summary("h_syncpt_math");
}
