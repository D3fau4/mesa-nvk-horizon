/*
 * Host unit tests — scanout layout checks (horizon/surface/surface.c).
 *
 * This is the arithmetic that decides whether a swapchain image goes to
 * the display block directly or gets copied row by row. Until it moved
 * out of the WSI backend the only way to exercise it was to run a
 * presenting test on a console, where the overflow cases below cannot
 * be produced on purpose at all: no allocator will hand out an image
 * whose stride times its aligned height wraps 32 bits. Here they are
 * ordinary function arguments.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../horizon/include/horizon_gpu/surface.h"
#include "hostfw.h"

/* A description that passes every check, used as the base each case
 * perturbs in exactly one field. 1280x720 RGBA8, block height 16 GOBs
 * (log2 = 4), so a block covers 8 << 4 = 128 rows and 720 rounds out to
 * 768. Stride 5120 = 1280 * 4, which is 80 whole GOBs. Scanout extent
 * is therefore 5120 * 768 = 3932160 bytes. */
static horizon_gpu_scanout_desc base(void)
{
    horizon_gpu_scanout_desc d = {
        .width = 1280,
        .height = 720,
        .bytes_per_pixel = 4,
        .row_stride_B = 5120,
        .block_height_log2 = 4,
        .offset_B = 0,
        .size_B = 3932160,
        .pte_kind = HORIZON_GPU_PTE_KIND_GENERIC_16BX2,
        .block_linear = true,
        .gob_layout_is_display = true,
    };
    return d;
}

/* Runs a description and reports the verdict only. */
static horizon_gpu_scanout_verdict v_of(const horizon_gpu_scanout_desc *d)
{
    return horizon_gpu_scanout_plan(d, NULL, NULL);
}

int main(void)
{
    /* ---- the geometry the checks are written against --------------- */
    H_CHECK(HORIZON_GPU_GOB_WIDTH_B == 64, "a GOB is 64 bytes wide");
    H_CHECK(HORIZON_GPU_GOB_HEIGHT_ROWS == 8, "a GOB is 8 rows tall");
    H_CHECK(HORIZON_GPU_GOB_WIDTH_B * HORIZON_GPU_GOB_HEIGHT_ROWS == 512,
            "so a GOB is 512 bytes");

    /* ---- the case that must pass, and its two derived numbers ------ */
    horizon_gpu_scanout_desc d = base();
    uint32_t size_B = 0, stride_px = 0;
    H_CHECK(horizon_gpu_scanout_plan(&d, &size_B, &stride_px) ==
            HORIZON_GPU_SCANOUT_OK, "1280x720 RGBA8 block-linear is OK");
    H_CHECK(size_B == 3932160, "scanout extent is stride * 768");
    H_CHECK(stride_px == 1280, "stride in pixels is the width");

    /* An allocation exactly one byte short of the rounded-out height is
     * the failure this whole calculation exists to catch. */
    d = base();
    d.size_B = 3932160 - 1;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_ALLOCATION_TOO_SMALL,
            "one byte short of a whole block is refused");

    /* An allocation sized for the *unrounded* height — 5120 * 720 —
     * looks generous and is not: the display block reads past it. This
     * is the mistake a reader who forgets the block rounding makes. */
    d = base();
    d.size_B = (uint64_t)5120 * 720;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_ALLOCATION_TOO_SMALL,
            "sizing for the unrounded height is refused");

    /* A larger allocation is fine; only "smaller" is a problem. */
    d = base();
    d.size_B = 3932160 + 4096;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_OK, "a larger allocation is OK");

    /* ---- one check per rejection, in the order they are applied ---- */
    d = base();
    d.block_linear = false;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_NOT_BLOCK_LINEAR,
            "pitch-linear is refused");

    d = base();
    d.gob_layout_is_display = false;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_GOB_LAYOUT,
            "the wrong GOB sector ordering is refused");

    d = base();
    d.pte_kind = HORIZON_GPU_PTE_KIND_PITCH;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_PTE_KIND,
            "a PTE kind other than generic 16Bx2 is refused");

    d = base();
    d.block_height_log2 = HORIZON_GPU_MAX_BLOCK_HEIGHT_LOG2;
    d.size_B = UINT32_MAX;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_OK,
            "the largest expressible block height is accepted");

    d = base();
    d.block_height_log2 = HORIZON_GPU_MAX_BLOCK_HEIGHT_LOG2 + 1;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_BLOCK_HEIGHT,
            "one past the largest expressible block height is refused");

    d = base();
    d.row_stride_B = 0;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_STRIDE_NOT_PIXELS,
            "a zero stride is refused");

    d = base();
    d.row_stride_B = 5122; /* not a multiple of 4 */
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_STRIDE_NOT_PIXELS,
            "a stride that is not whole pixels is refused");

    /* Narrower than the image, but still whole pixels and whole GOBs —
     * so it passes every neighbouring check and only this one sees it. */
    d = base();
    d.row_stride_B = 4096; /* 1024 px < 1280 px, and 64 whole GOBs */
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_STRIDE_TOO_NARROW,
            "a stride narrower than the image is refused");

    /* Exactly the image width is not narrower. */
    d = base();
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_OK,
            "a stride exactly the image width is accepted");

    d = base();
    d.row_stride_B = 5152; /* whole pixels, not whole GOBs */
    d.size_B = UINT32_MAX;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_STRIDE_NOT_GOBS,
            "a stride that is not whole GOBs is refused");

    d = base();
    d.offset_B = (uint64_t)UINT32_MAX + 1;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_FIELDS_TOO_WIDE,
            "an offset past 32 bits is refused");

    d = base();
    d.size_B = (uint64_t)UINT32_MAX + 1;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_FIELDS_TOO_WIDE,
            "a size past 32 bits is refused");

    /* Height that cannot be rounded out to a whole block without
     * wrapping. Reachable only from here. */
    d = base();
    d.height = UINT32_MAX;
    d.size_B = UINT32_MAX;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_HEIGHT_OVERFLOW,
            "a height that cannot be rounded out is refused");

    /* Stride times rounded height past 32 bits, with both fields
     * individually legal. */
    d = base();
    d.height = 65536;
    d.row_stride_B = 65536;
    d.width = 16384;
    d.size_B = UINT32_MAX;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_SIZE_OVERFLOW,
            "a scanout extent past 32 bits is refused");

    /* ---- degenerate arguments ------------------------------------- */
    H_CHECK(horizon_gpu_scanout_plan(NULL, NULL, NULL) ==
            HORIZON_GPU_SCANOUT_INVALID_ARG, "a null description");

    d = base();
    d.bytes_per_pixel = 0;
    H_CHECK(v_of(&d) == HORIZON_GPU_SCANOUT_INVALID_ARG,
            "zero bytes per pixel");

    /* A zero-height image rounds out to zero and reads nothing; it is
     * degenerate but not a layout the display block cannot express, and
     * the answer must not depend on the allocation being non-empty. */
    d = base();
    d.height = 0;
    d.size_B = 0;
    size_B = 0xdeadbeef;
    H_CHECK(horizon_gpu_scanout_plan(&d, &size_B, NULL) ==
            HORIZON_GPU_SCANOUT_OK && size_B == 0,
            "a zero-height image reads nothing");

    /* ---- the outputs are untouched unless the verdict is OK -------- */
    d = base();
    d.block_linear = false;
    size_B = 0xdeadbeef;
    stride_px = 0xdeadbeef;
    H_CHECK(horizon_gpu_scanout_plan(&d, &size_B, &stride_px) !=
            HORIZON_GPU_SCANOUT_OK &&
            size_B == 0xdeadbeef && stride_px == 0xdeadbeef,
            "a rejected description writes neither output");

    /* ---- every verdict has its own phrase -------------------------- */
    static const horizon_gpu_scanout_verdict all[] = {
        HORIZON_GPU_SCANOUT_OK,
        HORIZON_GPU_SCANOUT_NOT_BLOCK_LINEAR,
        HORIZON_GPU_SCANOUT_GOB_LAYOUT,
        HORIZON_GPU_SCANOUT_PTE_KIND,
        HORIZON_GPU_SCANOUT_BLOCK_HEIGHT,
        HORIZON_GPU_SCANOUT_STRIDE_NOT_PIXELS,
        HORIZON_GPU_SCANOUT_STRIDE_TOO_NARROW,
        HORIZON_GPU_SCANOUT_STRIDE_NOT_GOBS,
        HORIZON_GPU_SCANOUT_FIELDS_TOO_WIDE,
        HORIZON_GPU_SCANOUT_HEIGHT_OVERFLOW,
        HORIZON_GPU_SCANOUT_SIZE_OVERFLOW,
        HORIZON_GPU_SCANOUT_ALLOCATION_TOO_SMALL,
        HORIZON_GPU_SCANOUT_INVALID_ARG,
    };
    const size_t n = sizeof(all) / sizeof(all[0]);
    bool distinct = true;
    for (size_t i = 0; i < n; i++) {
        const char *a = horizon_gpu_scanout_verdict_str(all[i]);
        if (!a || a[0] == '\0') {
            distinct = false;
            break;
        }
        for (size_t j = i + 1; j < n; j++) {
            if (strcmp(a, horizon_gpu_scanout_verdict_str(all[j])) == 0) {
                distinct = false;
                break;
            }
        }
    }
    H_CHECK(distinct, "every verdict has its own non-empty phrase");
    H_CHECK(strcmp(horizon_gpu_scanout_verdict_str(
                       (horizon_gpu_scanout_verdict)999),
                   "unknown scanout verdict") == 0,
            "an unknown verdict says so");

    return h_summary("h_scanout");
}
