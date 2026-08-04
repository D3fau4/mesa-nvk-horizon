/*
 * horizon_gpu — GPFIFO command-list emitters (see cmds.h for the source
 * citations of every constant).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "horizon_gpu/cmds.h"

uint32_t horizon_cmds_fence_incr(uint32_t buf[HORIZON_CMDS_FENCE_INCR_DWORDS],
                                 uint32_t syncpt_id)
{
    if (syncpt_id > HORIZON_SYNCPT_ID_MAX)
        return 0;

    uint32_t n = 0;
    /* Wait-for-idle first, so the increment means "work done", not
     * "methods fetched" (nvgpu gk20a job-end precedent).
     *
     * SCOPE must be ALL. This was 0 with a comment claiming 0 meant "all
     * preceding work in this channel"; clb06f.h:141-143 says 0 is
     * CURRENT_SCG_TYPE and ALL is 1, so the comment asserted the opposite
     * of the header. A copy-engine transfer is not in the graphics
     * scheduling class group, so the narrower scope does not wait for it
     * — which is invisible to any test that only uses host methods.
     */
    buf[n++] = horizon_cmd_hdr_incr(0, HORIZON_NVA06F_WFI, 1);
    buf[n++] = HORIZON_WFI_SCOPE_ALL;
    /* Then the dirty-L2 writeback, and the order is the whole point: a
     * GPU write lands in the GPU's L2 first (measured on hardware
     * 2026-08-04, t_gpuwrite), and flushing before the wait would leave
     * anything that completed *during* the wait sitting in L2. Wait
     * first, then flush, then say it is done.
     */
    buf[n++] = horizon_cmd_hdr_incr(0, HORIZON_NVA06F_MEM_OP_C, 2);
    buf[n++] = 0; /* operand unused by a non-TLB operation */
    buf[n++] = HORIZON_MEM_OP_L2_FLUSH_DIRTY << HORIZON_MEM_OP_D_OP_SHIFT;
    buf[n++] = horizon_cmd_hdr_incr(0, HORIZON_NVA06F_SYNCPOINTA, 1);
    buf[n++] = 0; /* payload is unused for an increment */
    buf[n++] = horizon_cmd_hdr_incr(0, HORIZON_NVA06F_SYNCPOINTB, 1);
    buf[n++] = (syncpt_id << HORIZON_SYNCPOINTB_INDEX_SHIFT) |
               HORIZON_SYNCPOINTB_OP_INCR;
    return n;
}

uint32_t
horizon_cmds_syncpt_wait(uint32_t buf[HORIZON_CMDS_SYNCPT_WAIT_DWORDS],
                         uint32_t syncpt_id, uint32_t threshold)
{
    if (syncpt_id > HORIZON_SYNCPT_ID_MAX)
        return 0;

    uint32_t n = 0;
    buf[n++] = horizon_cmd_hdr_incr(0, HORIZON_NVA06F_SYNCPOINTA, 1);
    buf[n++] = threshold;
    buf[n++] = horizon_cmd_hdr_incr(0, HORIZON_NVA06F_SYNCPOINTB, 1);
    /* WAIT_SWITCH lets the host context-switch the channel while it
     * blocks, as nvgpu's wait command does. */
    buf[n++] = (syncpt_id << HORIZON_SYNCPOINTB_INDEX_SHIFT) |
               HORIZON_SYNCPOINTB_WAIT_SWITCH_EN | HORIZON_SYNCPOINTB_OP_WAIT;
    return n;
}

uint32_t
horizon_cmds_semaphore_release(uint32_t buf[HORIZON_CMDS_SEM_RELEASE_DWORDS],
                               uint64_t gpu_va, uint32_t payload)
{
    /* OFFSET_LOWER is bits 31:2 and OFFSET_UPPER is bits 7:0 of the next
     * method, so the address must be 4-byte aligned and must fit 40 bits
     * — which is exactly the GPU VA width this address space reports.
     * Reject rather than truncate: a truncated address is still a valid
     * address, and the GPU would write to it. */
    if ((gpu_va & UINT64_C(3)) != 0)
        return 0;
    if ((gpu_va >> 40) != 0)
        return 0;

    uint32_t n = 0;
    /* SEMAPHOREA..D are consecutive methods, so one increasing-methods
     * header covers all four. */
    buf[n++] = horizon_cmd_hdr_incr(0, HORIZON_NVA06F_SEMAPHOREA, 4);
    buf[n++] = (uint32_t)(gpu_va >> 32) & 0xFFu;
    buf[n++] = (uint32_t)gpu_va & 0xFFFFFFFCu;
    buf[n++] = payload;
    buf[n++] = HORIZON_SEMAPHORED_OP_RELEASE |
               HORIZON_SEMAPHORED_RELEASE_WFI_EN |
               HORIZON_SEMAPHORED_RELEASE_SIZE_4B;
    return n;
}

uint32_t horizon_cmds_mem_op(uint32_t buf[HORIZON_CMDS_MEM_OP_DWORDS],
                             uint32_t operation)
{
    /* OPERATION is 5 bits (31:27). Reject rather than truncate: a
     * truncated operation code is a *different, valid* operation. */
    if (operation > 0x1Fu)
        return 0;

    uint32_t n = 0;
    /* MEM_OP_C and MEM_OP_D are consecutive; the operands are only read
     * by the TLB-invalidate operations, so C is zero here. */
    buf[n++] = horizon_cmd_hdr_incr(0, HORIZON_NVA06F_MEM_OP_C, 2);
    buf[n++] = 0;
    buf[n++] = operation << HORIZON_MEM_OP_D_OP_SHIFT;
    return n;
}

uint32_t
horizon_cmds_set_objects(uint32_t buf[HORIZON_CMDS_SET_OBJECTS_DWORDS],
                         const uint32_t classes[HORIZON_CMDS_NUM_SUBCHANNELS])
{
    /* NVA06F_SET_OBJECT_NVCLASS is bits 15:0 (cla06f.h); a queried class
     * number that does not fit would otherwise be silently mangled into a
     * different, valid-looking class and submitted to hardware — reject
     * instead, matching every other "reject rather than truncate" check
     * in this layer. */
    for (uint32_t subch = 0; subch < HORIZON_CMDS_NUM_SUBCHANNELS; subch++) {
        if (classes[subch] > 0xFFFFu)
            return 0;
    }

    uint32_t n = 0;
    for (uint32_t subch = 0; subch < HORIZON_CMDS_NUM_SUBCHANNELS; subch++) {
        buf[n++] = horizon_cmd_hdr_incr(subch, HORIZON_NVA06F_SET_OBJECT, 1);
        buf[n++] = classes[subch];
    }
    return n;
}

uint32_t horizon_cmds_nop(uint32_t *buf, uint32_t buf_dwords, uint32_t pairs)
{
    /* Each pair is 2 dwords; refuse rather than overrun the caller's
     * buffer if it would not fit. */
    if (pairs > buf_dwords / 2)
        return 0;

    uint32_t n = 0;
    for (uint32_t i = 0; i < pairs; i++) {
        buf[n++] = horizon_cmd_hdr_incr(0, HORIZON_NVA06F_NOP, 1);
        buf[n++] = 0;
    }
    return n;
}
