/*
 * horizon_gpu — scanout layout checks (see surface.h for what the
 * numbers mean and why this is not inside the WSI backend).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "horizon_gpu/surface.h"

#include "../memory/align.h"

horizon_gpu_scanout_verdict
horizon_gpu_scanout_plan(const horizon_gpu_scanout_desc *desc,
                         uint32_t *out_scanout_size_B,
                         uint32_t *out_stride_px)
{
    if (!desc || desc->bytes_per_pixel == 0)
        return HORIZON_GPU_SCANOUT_INVALID_ARG;

    if (!desc->block_linear)
        return HORIZON_GPU_SCANOUT_NOT_BLOCK_LINEAR;
    if (!desc->gob_layout_is_display)
        return HORIZON_GPU_SCANOUT_GOB_LAYOUT;
    if (desc->pte_kind != HORIZON_GPU_PTE_KIND_GENERIC_16BX2)
        return HORIZON_GPU_SCANOUT_PTE_KIND;
    if (desc->block_height_log2 > HORIZON_GPU_MAX_BLOCK_HEIGHT_LOG2)
        return HORIZON_GPU_SCANOUT_BLOCK_HEIGHT;

    if (desc->row_stride_B == 0 ||
        (desc->row_stride_B % desc->bytes_per_pixel) != 0)
        return HORIZON_GPU_SCANOUT_STRIDE_NOT_PIXELS;

    /* A stride narrower than the image is the one failure the alignment,
     * kind, overflow and total-size checks around it cannot see: the
     * stride handed to the display block would be smaller than the
     * width, and every row after the first would be read from the wrong
     * bytes. Computed in 64 bits so a wide image cannot wrap the
     * comparison it is meant to fail. */
    uint64_t row_bytes = 0;
    if (!horizon_mul_u64(desc->width, desc->bytes_per_pixel, &row_bytes))
        return HORIZON_GPU_SCANOUT_SIZE_OVERFLOW;
    if ((uint64_t)desc->row_stride_B < row_bytes)
        return HORIZON_GPU_SCANOUT_STRIDE_TOO_NARROW;

    if ((desc->row_stride_B % HORIZON_GPU_GOB_WIDTH_B) != 0)
        return HORIZON_GPU_SCANOUT_STRIDE_NOT_GOBS;

    /* The description carries offset and size in 32-bit fields. An image
     * that does not fit them is refused here, once, rather than left as
     * a truncated size to be discovered as a display reading half an
     * image. */
    if (desc->offset_B > UINT32_MAX || desc->size_B > UINT32_MAX)
        return HORIZON_GPU_SCANOUT_FIELDS_TOO_WIDE;

    /* The display block reads rows of GOBs, so the allocation has to
     * cover the height rounded out to a whole block. An image short by
     * one row is one the display block reads past. */
    const uint64_t block_rows =
        (uint64_t)HORIZON_GPU_GOB_HEIGHT_ROWS << desc->block_height_log2;
    uint64_t height_aligned = 0;
    if (!horizon_align_up_u64(desc->height, block_rows, &height_aligned))
        return HORIZON_GPU_SCANOUT_HEIGHT_OVERFLOW;
    if (height_aligned > UINT32_MAX)
        return HORIZON_GPU_SCANOUT_HEIGHT_OVERFLOW;

    uint64_t scanout_size_B = 0;
    if (!horizon_mul_u64(desc->row_stride_B, height_aligned,
                         &scanout_size_B))
        return HORIZON_GPU_SCANOUT_SIZE_OVERFLOW;
    if (scanout_size_B > UINT32_MAX)
        return HORIZON_GPU_SCANOUT_SIZE_OVERFLOW;

    if (scanout_size_B > desc->size_B)
        return HORIZON_GPU_SCANOUT_ALLOCATION_TOO_SMALL;

    if (out_scanout_size_B)
        *out_scanout_size_B = (uint32_t)scanout_size_B;
    if (out_stride_px)
        *out_stride_px = desc->row_stride_B / desc->bytes_per_pixel;
    return HORIZON_GPU_SCANOUT_OK;
}

const char *
horizon_gpu_scanout_verdict_str(horizon_gpu_scanout_verdict verdict)
{
    switch (verdict) {
    case HORIZON_GPU_SCANOUT_OK:
        return "the image can be scanned out as it stands";
    case HORIZON_GPU_SCANOUT_NOT_BLOCK_LINEAR:
        return "the image is pitch-linear and the display block was "
               "asked for a block-linear layout";
    case HORIZON_GPU_SCANOUT_GOB_LAYOUT:
        return "the image's GOB sector ordering is not the one this "
               "display block reads";
    case HORIZON_GPU_SCANOUT_PTE_KIND:
        return "the image's page-table kind is not generic 16Bx2, so the "
               "compositor would read it with the wrong sector ordering";
    case HORIZON_GPU_SCANOUT_BLOCK_HEIGHT:
        return "the image's block height is larger than the buffer "
               "description can express";
    case HORIZON_GPU_SCANOUT_STRIDE_NOT_PIXELS:
        return "the image's row stride is not a whole number of pixels";
    case HORIZON_GPU_SCANOUT_STRIDE_TOO_NARROW:
        return "the image's row stride is narrower than the image";
    case HORIZON_GPU_SCANOUT_STRIDE_NOT_GOBS:
        return "the image's row stride is not a whole number of GOBs";
    case HORIZON_GPU_SCANOUT_FIELDS_TOO_WIDE:
        return "the image does not fit the 32-bit offset and size fields "
               "a buffer is described with";
    case HORIZON_GPU_SCANOUT_HEIGHT_OVERFLOW:
        return "the image's height cannot be rounded out to a whole block";
    case HORIZON_GPU_SCANOUT_SIZE_OVERFLOW:
        return "the image's scanout size overflows 32 bits";
    case HORIZON_GPU_SCANOUT_ALLOCATION_TOO_SMALL:
        return "the allocation is smaller than the region the display "
               "block would read";
    case HORIZON_GPU_SCANOUT_INVALID_ARG:
        return "invalid argument";
    }
    return "unknown scanout verdict";
}
