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

    /* ---- sub-interval removal: what a partial unbind needs -------
     *
     * A fresh set per case, so a failure names one behaviour instead of
     * a state built by the case before it. */
    {
        horizon_va_set r;
        horizon_va_set_init(&r);

        /* Nothing to remove from. */
        H_CHECK(horizon_va_set_remove_range(&r, 0x1000, 0x1000) ==
                HORIZON_GPU_ERR_INVALID_ARG, "range: empty set rejected");

        H_CHECK(horizon_va_set_add(&r, 0x1000, 0x4000) == HORIZON_GPU_OK,
                "range: add [1000,5000)");

        /* Degenerate arguments, before anything is modified. */
        H_CHECK(horizon_va_set_remove_range(&r, 0x2000, 0) ==
                HORIZON_GPU_ERR_INVALID_ARG, "range: zero size rejected");
        H_CHECK(horizon_va_set_remove_range(&r, UINT64_MAX, 2) ==
                HORIZON_GPU_ERR_OVERFLOW, "range: wrapping rejected");
        /* Below every interval, and reaching past the end of one. */
        H_CHECK(horizon_va_set_remove_range(&r, 0x0, 0x100) ==
                HORIZON_GPU_ERR_INVALID_ARG,
                "range: below the first interval rejected");
        H_CHECK(horizon_va_set_remove_range(&r, 0x800, 0x1000) ==
                HORIZON_GPU_ERR_INVALID_ARG,
                "range: straddling the start rejected");
        H_CHECK(horizon_va_set_remove_range(&r, 0x4000, 0x2000) ==
                HORIZON_GPU_ERR_INVALID_ARG,
                "range: reaching past the end rejected");
        H_CHECK(horizon_va_set_count(&r) == 1,
                "range: a rejected removal changed nothing");

        /* The middle: one interval becomes two. */
        H_CHECK(horizon_va_set_remove_range(&r, 0x2000, 0x1000) ==
                HORIZON_GPU_OK, "range: hole [2000,3000) punched");
        H_CHECK(horizon_va_set_count(&r) == 2, "range: split made two");
        H_CHECK(r.items[0].offset == 0x1000 && r.items[0].size == 0x1000,
                "range: left remainder is [1000,2000)");
        H_CHECK(r.items[1].offset == 0x3000 && r.items[1].size == 0x2000,
                "range: right remainder is [3000,5000)");
        /* The hole is free, and only the hole. */
        H_CHECK(!horizon_va_set_overlaps(&r, 0x2000, 0x1000),
                "range: the hole is free again");
        H_CHECK(horizon_va_set_overlaps(&r, 0x1fff, 0x2),
                "range: the left remainder is still live");
        H_CHECK(horizon_va_set_overlaps(&r, 0x2fff, 0x2),
                "range: the right remainder is still live");
        H_CHECK(horizon_va_set_add(&r, 0x2000, 0x1000) == HORIZON_GPU_OK,
                "range: the hole can be bound again");
        H_CHECK(horizon_va_set_count(&r) == 3, "range: re-bound in place");

        /* A range covering two of them is two requests, not one. */
        H_CHECK(horizon_va_set_remove_range(&r, 0x1800, 0x1000) ==
                HORIZON_GPU_ERR_INVALID_ARG,
                "range: spanning two intervals rejected");
        H_CHECK(horizon_va_set_count(&r) == 3,
                "range: the rejected span changed nothing");

        horizon_va_set_fini(&r);
    }

    {
        horizon_va_set r;
        horizon_va_set_init(&r);
        H_CHECK(horizon_va_set_add(&r, 0x1000, 0x4000) == HORIZON_GPU_OK,
                "prefix: add [1000,5000)");
        H_CHECK(horizon_va_set_remove_range(&r, 0x1000, 0x1000) ==
                HORIZON_GPU_OK, "prefix: [1000,2000) removed");
        H_CHECK(horizon_va_set_count(&r) == 1 &&
                r.items[0].offset == 0x2000 && r.items[0].size == 0x3000,
                "prefix: [2000,5000) survives");

        H_CHECK(horizon_va_set_remove_range(&r, 0x4000, 0x1000) ==
                HORIZON_GPU_OK, "suffix: [4000,5000) removed");
        H_CHECK(horizon_va_set_count(&r) == 1 &&
                r.items[0].offset == 0x2000 && r.items[0].size == 0x2000,
                "suffix: [2000,4000) survives");

        H_CHECK(horizon_va_set_remove_range(&r, 0x2000, 0x2000) ==
                HORIZON_GPU_OK, "whole: the rest removed");
        H_CHECK(horizon_va_set_count(&r) == 0, "whole: the set is empty");
        horizon_va_set_fini(&r);
    }

    /* A gap between two live intervals belongs to neither, and a
     * removal that lands in it must say so rather than pick a side. */
    {
        horizon_va_set r;
        horizon_va_set_init(&r);
        H_CHECK(horizon_va_set_add(&r, 0x1000, 0x1000) == HORIZON_GPU_OK,
                "gap: add [1000,2000)");
        H_CHECK(horizon_va_set_add(&r, 0x3000, 0x1000) == HORIZON_GPU_OK,
                "gap: add [3000,4000)");
        H_CHECK(horizon_va_set_remove_range(&r, 0x2000, 0x1000) ==
                HORIZON_GPU_ERR_INVALID_ARG, "gap: the gap is not live");
        H_CHECK(horizon_va_set_count(&r) == 2, "gap: nothing changed");
        horizon_va_set_fini(&r);
    }

    /* Splitting repeatedly grows the array, which is the path that can
     * fail; every hole must still be exactly where it was punched. */
    {
        horizon_va_set r;
        horizon_va_set_init(&r);
        H_CHECK(horizon_va_set_add(&r, 0, 0x100000) == HORIZON_GPU_OK,
                "grow: one interval of 0x100000");
        int gok = 1;
        for (uint64_t k = 0; k < 32; k++)
            if (horizon_va_set_remove_range(&r, 0x1000 + k * 0x2000,
                                            0x1000) != HORIZON_GPU_OK)
                gok = 0;
        H_CHECK(gok, "grow: 32 holes punched");
        H_CHECK(horizon_va_set_count(&r) == 33, "grow: 33 intervals left");
        int hok = 1;
        for (uint64_t k = 0; k < 32; k++) {
            if (horizon_va_set_overlaps(&r, 0x1000 + k * 0x2000, 0x1000))
                hok = 0;
            if (!horizon_va_set_overlaps(&r, k * 0x2000, 0x1000))
                hok = 0;
        }
        H_CHECK(hok, "grow: every hole free, every interval live");
        horizon_va_set_fini(&r);
    }

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
