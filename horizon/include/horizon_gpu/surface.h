/*
 * horizon_gpu — scanout layout: can the display block read this image as
 * it stands?
 *
 * WHY THIS LIVES HERE AND NOT IN THE WSI. The arithmetic below is a
 * property of the GPU's block-linear layout and of the fixed-width
 * fields the window system describes a buffer with. It mentions no
 * Vulkan type, no libnx type and no window: it takes numbers and returns
 * a verdict. Kept inside the WSI backend it was reachable only by
 * cross-compiling and running on a console, which is the one place a
 * wrong answer is expensive to find. Here it is an ordinary translation
 * unit that the host suites compile under ASan and UBSan, so the
 * overflow cases are exercised by a machine rather than reasoned about.
 *
 * This is the "surface-info struct" the layer rules reserve as the sole
 * exception to horizon/ being window-system-free: a description of a
 * buffer, not a way to reach one. Nothing here opens a display, creates
 * a layer or touches an NWindow.
 *
 * THE LAYOUT, in the terms the checks use. Memory is addressed in GOBs
 * of HORIZON_GPU_GOB_WIDTH_B bytes by HORIZON_GPU_GOB_HEIGHT_ROWS rows,
 * so one GOB is 512 bytes. A block is `1 << block_height_log2` GOBs
 * tall, so it covers `GOB_HEIGHT_ROWS << block_height_log2` rows and
 * occupies `512 << block_height_log2` bytes. `row_stride_B` is the
 * distance between one row of blocks and the next, and the display
 * block reads whole blocks — which is why an allocation has to cover
 * the image height rounded out to a whole block, not just the height.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_GPU_SURFACE_H
#define HORIZON_GPU_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

#include "vm.h" /* HORIZON_GPU_PTE_KIND_GENERIC_16BX2 */

#ifdef __cplusplus
extern "C" {
#endif

/* GOB geometry. A GOB is 64 bytes wide by 8 rows, so 512 bytes; a block
 * of `1 << block_height_log2` GOBs is 512 << block_height_log2 bytes and
 * covers 8 << block_height_log2 rows. */
#define HORIZON_GPU_GOB_WIDTH_B     UINT32_C(64)
#define HORIZON_GPU_GOB_HEIGHT_ROWS UINT32_C(8)

/* The largest block height a buffer description can express: the field
 * that carries it is a log2 and the display block's is 3 bits wide, so
 * 5 (32 GOBs) is the ceiling any taller block would be silently
 * truncated past. */
#define HORIZON_GPU_MAX_BLOCK_HEIGHT_LOG2 UINT32_C(5)

/* What the allocation actually is — facts about memory the driver has
 * already committed, never a request. A prediction made before
 * allocation would be a different claim, and the interesting failures
 * are exactly the ones where prediction and allocation disagree. */
typedef struct horizon_gpu_scanout_desc {
    uint32_t width;              /* pixels                              */
    uint32_t height;             /* pixels                              */
    uint32_t bytes_per_pixel;
    uint32_t row_stride_B;       /* distance between rows of blocks     */
    uint32_t block_height_log2;  /* block height in GOBs, as a log2     */
    uint64_t offset_B;           /* where the image starts in the object */
    uint64_t size_B;             /* how much was allocated              */
    uint8_t  pte_kind;
    bool     block_linear;       /* false for a pitch-linear image      */
    /* True when the GOB's internal sector ordering is the one this
     * platform's display block reads. A boolean rather than the layout
     * itself, because naming the layout would mean naming the graphics
     * driver's own enumeration inside this layer. */
    bool     gob_layout_is_display;
} horizon_gpu_scanout_desc;

/* Why an image cannot be scanned out as it stands. Every value other
 * than OK is an ordinary answer, not an error: the caller's response is
 * to copy the image instead, naming the reason.
 *
 * APPENDED, NEVER INSERTED OR RENUMBERED — like horizon_gpu_status,
 * these numbers reach hardware logs that are kept as evidence. */
typedef enum horizon_gpu_scanout_verdict {
    HORIZON_GPU_SCANOUT_OK                  = 0,
    HORIZON_GPU_SCANOUT_NOT_BLOCK_LINEAR    = 1,
    HORIZON_GPU_SCANOUT_GOB_LAYOUT          = 2,
    HORIZON_GPU_SCANOUT_PTE_KIND            = 3,
    HORIZON_GPU_SCANOUT_BLOCK_HEIGHT        = 4,
    HORIZON_GPU_SCANOUT_STRIDE_NOT_PIXELS   = 5,
    HORIZON_GPU_SCANOUT_STRIDE_TOO_NARROW   = 6,
    HORIZON_GPU_SCANOUT_STRIDE_NOT_GOBS     = 7,
    HORIZON_GPU_SCANOUT_FIELDS_TOO_WIDE     = 8,
    HORIZON_GPU_SCANOUT_HEIGHT_OVERFLOW     = 9,
    HORIZON_GPU_SCANOUT_SIZE_OVERFLOW       = 10,
    HORIZON_GPU_SCANOUT_ALLOCATION_TOO_SMALL = 11,
    HORIZON_GPU_SCANOUT_INVALID_ARG         = 12,
} horizon_gpu_scanout_verdict;

/* Decides whether `desc` can be handed to the display block directly,
 * and computes the two derived numbers a buffer description needs.
 *
 * On HORIZON_GPU_SCANOUT_OK, `*out_scanout_size_B` is the byte extent
 * the display block will read — the stride times the height rounded out
 * to a whole block, which is what has to fit inside the allocation — and
 * `*out_stride_px` is the row stride expressed in pixels, which is the
 * unit the buffer description carries it in.
 *
 * On any other verdict neither output is written. Both out-pointers may
 * be NULL if only the verdict is wanted. */
horizon_gpu_scanout_verdict
horizon_gpu_scanout_plan(const horizon_gpu_scanout_desc *desc,
                         uint32_t *out_scanout_size_B,
                         uint32_t *out_stride_px);

/* A stable, human-readable phrase for a verdict, in the voice a log
 * line wants: "the image is pitch-linear ...". Never NULL. */
const char *
horizon_gpu_scanout_verdict_str(horizon_gpu_scanout_verdict verdict);

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_GPU_SURFACE_H */
