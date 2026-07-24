/*
 * Host unit tests — alignment/overflow helpers (horizon/memory/align.h).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#include "../../horizon/memory/align.h"
#include "hostfw.h"

int main(void)
{
    H_CHECK(horizon_is_pow2_u64(1), "pow2(1)");
    H_CHECK(horizon_is_pow2_u64(0x20000), "pow2(0x20000)");
    H_CHECK(!horizon_is_pow2_u64(0), "!pow2(0)");
    H_CHECK(!horizon_is_pow2_u64(0x3000), "!pow2(0x3000)");
    H_CHECK(horizon_is_pow2_u64(UINT64_C(1) << 63), "pow2(2^63)");

    H_CHECK(horizon_is_aligned_u64(0x2000, 0x1000), "aligned");
    H_CHECK(!horizon_is_aligned_u64(0x2800, 0x1000), "!aligned");

    uint64_t v;
    H_CHECK(horizon_add_u64(1, 2, &v) && v == 3, "add ok");
    H_CHECK(!horizon_add_u64(UINT64_MAX, 1, &v), "add overflow");
    H_CHECK(horizon_mul_u64(0x1000, 0x1000, &v) && v == 0x1000000,
            "mul ok");
    H_CHECK(!horizon_mul_u64(UINT64_MAX / 2, 3, &v), "mul overflow");

    H_CHECK(horizon_align_up_u64(1, 0x1000, &v) && v == 0x1000,
            "align_up(1)");
    H_CHECK(horizon_align_up_u64(0x1000, 0x1000, &v) && v == 0x1000,
            "align_up(exact)");
    H_CHECK(horizon_align_up_u64(0, 0x1000, &v) && v == 0, "align_up(0)");
    H_CHECK(!horizon_align_up_u64(UINT64_MAX - 5, 0x1000, &v),
            "align_up overflow");
    H_CHECK(!horizon_align_up_u64(8, 3, &v), "align_up non-pow2");

    H_CHECK(horizon_range_fits_u64(0, 0x1000, 0x1000), "range exact");
    H_CHECK(horizon_range_fits_u64(0x800, 0x800, 0x1000), "range tail");
    H_CHECK(!horizon_range_fits_u64(0x801, 0x800, 0x1000), "range past");
    H_CHECK(!horizon_range_fits_u64(UINT64_MAX, 2, UINT64_MAX),
            "range wrap");

    return h_summary("h_align");
}
