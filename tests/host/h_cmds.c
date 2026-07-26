/*
 * Host unit tests — GPFIFO command emitters
 * (horizon/include/horizon_gpu/cmds.h). Expected dwords hand-derived
 * from the sources cited in cmds.h (cla06f.h method addresses, envytools
 * pushbuffer header format, nvgpu sequence shapes).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#include "../../horizon/include/horizon_gpu/cmds.h"
#include "hostfw.h"

int main(void)
{
    /* Header encoding: incrementing op (1<<29) | count<<16 | subch<<13 |
     * method>>2. */
    H_CHECK(horizon_cmd_hdr_incr(0, 0x70, 1) == 0x2001001C,
            "hdr(0, SYNCPOINTA, 1) == 0x2001001C (nvgpu's exact dword)");
    H_CHECK(horizon_cmd_hdr_incr(0, 0x74, 1) == 0x2001001D,
            "hdr(0, SYNCPOINTB, 1) == 0x2001001D");
    H_CHECK(horizon_cmd_hdr_incr(0, 0x78, 1) == 0x2001001E,
            "hdr(0, WFI, 1) == 0x2001001E");
    H_CHECK(horizon_cmd_hdr_incr(3, 0x0000, 1) == 0x20016000,
            "hdr(subch 3, SET_OBJECT, 1)");

    /* Fence increment list. */
    uint32_t buf[HORIZON_CMDS_FENCE_INCR_DWORDS];
    uint32_t n = horizon_cmds_fence_incr(buf, 42);
    H_CHECK(n == HORIZON_CMDS_FENCE_INCR_DWORDS, "incr dword count");
    H_CHECK(buf[0] == 0x2001001E && buf[1] == 0, "incr: WFI first");
    H_CHECK(buf[2] == 0x2001001C && buf[3] == 0, "incr: payload 0");
    H_CHECK(buf[4] == 0x2001001D, "incr: SYNCPOINTB header");
    H_CHECK(buf[5] == ((42u << 8) | 1u), "incr: (id<<8) | OPERATION_INCR");
    H_CHECK(horizon_cmds_fence_incr(buf, 0x1000) == 0,
            "id beyond 12-bit index rejected");

    /* Syncpoint wait list. */
    uint32_t wbuf[HORIZON_CMDS_SYNCPT_WAIT_DWORDS];
    n = horizon_cmds_syncpt_wait(wbuf, 7, 0xDEADBEEF);
    H_CHECK(n == HORIZON_CMDS_SYNCPT_WAIT_DWORDS, "wait dword count");
    H_CHECK(wbuf[0] == 0x2001001C && wbuf[1] == 0xDEADBEEF,
            "wait: threshold payload");
    H_CHECK(wbuf[2] == 0x2001001D, "wait: SYNCPOINTB header");
    H_CHECK(wbuf[3] == ((7u << 8) | (1u << 4) | 0u),
            "wait: (id<<8) | WAIT_SWITCH | OPERATION_WAIT");

    /* SET_OBJECT binds, subchannels 0..4, class in bits 15:0. */
    uint32_t sbuf[HORIZON_CMDS_SET_OBJECTS_DWORDS];
    const uint32_t classes[HORIZON_CMDS_NUM_SUBCHANNELS] = {
        0xB197, 0xB1C0, 0xA140, 0x902D, 0xB0B5,
    };
    n = horizon_cmds_set_objects(sbuf, classes);
    H_CHECK(n == HORIZON_CMDS_SET_OBJECTS_DWORDS, "setobj dword count");
    /* Hand-derived headers, not the implementation's own formula recomputed
     * (that would still pass if horizon_cmd_hdr_incr broke): (1<<29) |
     * (1<<16) | (subch<<13), subch = 0..4. */
    static const uint32_t setobj_hdrs[HORIZON_CMDS_NUM_SUBCHANNELS] = {
        0x20010000, 0x20012000, 0x20014000, 0x20016000, 0x20018000,
    };
    int ok = 1;
    for (uint32_t s = 0; s < HORIZON_CMDS_NUM_SUBCHANNELS; s++) {
        if (sbuf[2 * s] != setobj_hdrs[s])
            ok = 0;
        if (sbuf[2 * s + 1] != classes[s])
            ok = 0;
    }
    H_CHECK(ok, "setobj: one SET_OBJECT per subchannel with its class");

    /* A class number that does not fit in NVCLASS's 16 bits is rejected,
     * not truncated into a different, valid-looking class. */
    uint32_t bad_classes[HORIZON_CMDS_NUM_SUBCHANNELS] = {
        0xB197, 0xB1C0, 0x10000, 0x902D, 0xB0B5,
    };
    H_CHECK(horizon_cmds_set_objects(sbuf, bad_classes) == 0,
            "setobj: out-of-range class rejected, not truncated");

    /* NOPs. */
    uint32_t nbuf[8];
    n = horizon_cmds_nop(nbuf, 8, 4);
    H_CHECK(n == 8, "nop dword count");
    H_CHECK(nbuf[0] == 0x20010002 && nbuf[1] == 0,
            "nop: hdr(0, NOP=0x08, 1) + 0");
    H_CHECK(horizon_cmds_nop(nbuf, 8, 0) == 0, "nop: zero pairs is a no-op");
    H_CHECK(horizon_cmds_nop(nbuf, 8, 5) == 0,
            "nop: pairs that would overrun the buffer is rejected");

    return h_summary("h_cmds");
}
