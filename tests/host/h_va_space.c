/*
 * Host unit tests — live-interval set (horizon/vm/va_space.c).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#include "../../horizon/vm/va_space.h"
#include "hostfw.h"

int main(void)
{
    horizon_va_set set;
    horizon_va_set_init(&set);

    H_CHECK(horizon_va_set_add(&set, 0x1000, 0x1000) == HORIZON_GPU_OK,
            "add [1000,2000)");
    H_CHECK(horizon_va_set_add(&set, 0x3000, 0x1000) == HORIZON_GPU_OK,
            "add [3000,4000)");
    H_CHECK(horizon_va_set_count(&set) == 2, "count 2");

    /* Overlap detection in every direction. */
    H_CHECK(horizon_va_set_add(&set, 0x1800, 0x100) == HORIZON_GPU_ERR_BUSY,
            "inside rejected");
    H_CHECK(horizon_va_set_add(&set, 0x800, 0x1000) == HORIZON_GPU_ERR_BUSY,
            "left-overlap rejected");
    H_CHECK(horizon_va_set_add(&set, 0x1fff, 0x2) == HORIZON_GPU_ERR_BUSY,
            "right-edge overlap rejected");
    H_CHECK(horizon_va_set_add(&set, 0x0, 0x10000) == HORIZON_GPU_ERR_BUSY,
            "spanning rejected");
    H_CHECK(horizon_va_set_add(&set, 0x1000, 0x1000) == HORIZON_GPU_ERR_BUSY,
            "exact duplicate rejected");

    /* Adjacent is fine. */
    H_CHECK(horizon_va_set_add(&set, 0x2000, 0x1000) == HORIZON_GPU_OK,
            "adjacent [2000,3000) ok");
    H_CHECK(horizon_va_set_count(&set) == 3, "count 3");

    /* Degenerate/wrapping ranges. */
    H_CHECK(horizon_va_set_add(&set, 0x9000, 0) ==
            HORIZON_GPU_ERR_INVALID_ARG, "zero size rejected");
    H_CHECK(horizon_va_set_add(&set, UINT64_MAX, 2) ==
            HORIZON_GPU_ERR_OVERFLOW, "wrapping rejected");
    H_CHECK(horizon_va_set_overlaps(&set, UINT64_MAX, 2),
            "wrapping counts as overlap");

    /* Removal frees the interval for reuse; unknown offsets rejected. */
    H_CHECK(horizon_va_set_remove(&set, 0x2000) == HORIZON_GPU_OK,
            "remove [2000,3000)");
    H_CHECK(horizon_va_set_remove(&set, 0x2000) ==
            HORIZON_GPU_ERR_INVALID_ARG, "double remove rejected");
    H_CHECK(horizon_va_set_remove(&set, 0x2800) ==
            HORIZON_GPU_ERR_INVALID_ARG, "non-start offset rejected");
    H_CHECK(horizon_va_set_add(&set, 0x2000, 0x1000) == HORIZON_GPU_OK,
            "re-add after remove");

    /* Growth beyond the initial capacity keeps invariants. */
    horizon_va_set big;
    horizon_va_set_init(&big);
    int ok = 1;
    for (uint64_t i = 0; i < 100; i++)
        if (horizon_va_set_add(&big, i * 0x2000, 0x1000) != HORIZON_GPU_OK)
            ok = 0;
    H_CHECK(ok && horizon_va_set_count(&big) == 100, "100 intervals");
    ok = 1;
    for (uint64_t i = 0; i < 100; i++)
        if (horizon_va_set_overlaps(&big, i * 0x2000 + 0x1000, 0x1000))
            ok = 0;
    H_CHECK(ok, "gaps stay free");
    for (uint64_t i = 0; i < 100; i += 2)
        horizon_va_set_remove(&big, i * 0x2000);
    H_CHECK(horizon_va_set_count(&big) == 50, "half removed");

    horizon_va_set_fini(&big);
    horizon_va_set_fini(&set);
    H_CHECK(set.count == 0 && set.items == NULL, "fini resets");

    return h_summary("h_va_space");
}
