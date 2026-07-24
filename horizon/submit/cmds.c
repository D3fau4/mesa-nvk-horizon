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
     * "methods fetched" (nvgpu gk20a job-end precedent). */
    buf[n++] = horizon_cmd_hdr_incr(0, HORIZON_NVA06F_WFI, 1);
    buf[n++] = 0; /* WFI scope: all preceding work in this channel */
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
horizon_cmds_set_objects(uint32_t buf[HORIZON_CMDS_SET_OBJECTS_DWORDS],
                         const uint32_t classes[HORIZON_CMDS_NUM_SUBCHANNELS])
{
    uint32_t n = 0;
    for (uint32_t subch = 0; subch < HORIZON_CMDS_NUM_SUBCHANNELS; subch++) {
        buf[n++] = horizon_cmd_hdr_incr(subch, HORIZON_NVA06F_SET_OBJECT, 1);
        /* NVA06F_SET_OBJECT_NVCLASS is bits 15:0 (cla06f.h). */
        buf[n++] = classes[subch] & 0xFFFF;
    }
    return n;
}

uint32_t horizon_cmds_nop(uint32_t *buf, uint32_t pairs)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < pairs; i++) {
        buf[n++] = horizon_cmd_hdr_incr(0, HORIZON_NVA06F_NOP, 1);
        buf[n++] = 0;
    }
    return n;
}
