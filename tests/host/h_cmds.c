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

    /* Host semaphore release — the GPU write used by t_gpuwrite. Field
     * positions per clb06f.h:74-95 for the class GM20B reports. */
    uint32_t sem[HORIZON_CMDS_SEM_RELEASE_DWORDS];
    n = horizon_cmds_semaphore_release(sem, UINT64_C(0x8712345678), 0xC0FFEE01u);
    H_CHECK(n == HORIZON_CMDS_SEM_RELEASE_DWORDS, "sem: dword count");
    /* One increasing-methods header covering SEMAPHOREA..D: opcode 001b,
     * count 4, subch 0, dword method address 0x10 >> 2 = 4. */
    H_CHECK(sem[0] == 0x20040004u, "sem: hdr(0, SEMAPHOREA=0x10, 4)");
    H_CHECK(sem[1] == 0x87u, "sem: OFFSET_UPPER is VA bits 39:32");
    H_CHECK(sem[2] == 0x12345678u, "sem: OFFSET_LOWER is VA bits 31:2");
    H_CHECK(sem[3] == 0xC0FFEE01u, "sem: payload verbatim");
    /* RELEASE(2) | RELEASE_WFI_EN(0 at bit 20) | RELEASE_SIZE_4BYTE(1 at
     * bit 24) = 0x01000002. A 16-byte release would also write a
     * timestamp over the following three dwords. */
    H_CHECK(sem[4] == 0x01000002u, "sem: RELEASE | WFI enabled | 4-byte");

    /* An address the encoding cannot carry is rejected rather than
     * truncated — a truncated GPU address is still an address, and the
     * GPU would write to it. */
    H_CHECK(horizon_cmds_semaphore_release(sem, UINT64_C(0x1002), 1) == 0,
            "sem: misaligned address rejected");
    H_CHECK(horizon_cmds_semaphore_release(sem, UINT64_C(1) << 40, 1) == 0,
            "sem: address beyond 40 VA bits rejected");
    H_CHECK(horizon_cmds_semaphore_release(sem, 0, 0) ==
            HORIZON_CMDS_SEM_RELEASE_DWORDS,
            "sem: VA 0 is encodable (rejection is about width, not value)");

    /* MEM_OP — the L2/barrier operations, clb06f.h:112-136. */
    uint32_t mop[HORIZON_CMDS_MEM_OP_DWORDS];
    n = horizon_cmds_mem_op(mop, HORIZON_MEM_OP_L2_FLUSH_DIRTY);
    H_CHECK(n == HORIZON_CMDS_MEM_OP_DWORDS, "memop: dword count");
    /* hdr: opcode 001b, count 2, subch 0, dword method 0x30 >> 2 = 0xc. */
    H_CHECK(mop[0] == 0x2002000Cu, "memop: hdr(0, MEM_OP_C=0x30, 2)");
    H_CHECK(mop[1] == 0, "memop: operand unused by a non-TLB operation");
    H_CHECK(mop[2] == (0x10u << 27), "memop: L2_FLUSH_DIRTY at OPERATION 31:27");
    H_CHECK(horizon_cmds_mem_op(mop, HORIZON_MEM_OP_MEMBAR) == n &&
            mop[2] == (0x05u << 27), "memop: MEMBAR encoding");
    /* An operation wider than the 5-bit field would become a different,
     * valid operation. Reject. */
    H_CHECK(horizon_cmds_mem_op(mop, 0x20) == 0,
            "memop: out-of-range operation rejected, not truncated");

    return h_summary("h_cmds");
}
