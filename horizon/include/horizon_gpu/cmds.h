/*
 * horizon_gpu — GPFIFO command-list emitters. Pure C11, libnx-free,
 * unit-tested on the host (tests/host/h_cmds.c).
 *
 * Method addresses and field layouts are hardware facts re-derived from
 * permissively-licensed sources, cited per constant:
 *  - NVIDIA open-gpu-doc class header cla06f.h (KEPLER_CHANNEL_GPFIFO_A,
 *    MIT), whose host methods are carried unchanged by the Maxwell
 *    channel class (B06F) that GM20B's characteristics report.
 *  - The Linux Tegra `nvgpu` driver's gk20a channel-sync code is the
 *    usage precedent for the syncpoint wait/increment sequences on this
 *    exact silicon (facts, not copied text).
 *  - Pushbuffer header format per envytools rnndb (fifo/nv_ram_fifo /
 *    gf100+ pushbuffer encoding), as also used by Mesa's nv_push.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_SUBMIT_CMDS_H
#define HORIZON_SUBMIT_CMDS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channel (host) methods — byte addresses, open-gpu-doc cla06f.h. */
#define HORIZON_NVA06F_SET_OBJECT  UINT32_C(0x0000)
#define HORIZON_NVA06F_NOP         UINT32_C(0x0008)
#define HORIZON_NVA06F_SYNCPOINTA  UINT32_C(0x0070) /* PAYLOAD 31:0        */
#define HORIZON_NVA06F_SYNCPOINTB  UINT32_C(0x0074) /* OPERATION/INDEX     */
#define HORIZON_NVA06F_WFI         UINT32_C(0x0078)

/* SYNCPOINTB fields (cla06f.h): OPERATION 0:0, WAIT_SWITCH 4:4,
 * SYNCPT_INDEX 19:8. */
#define HORIZON_SYNCPOINTB_OP_WAIT         UINT32_C(0)
#define HORIZON_SYNCPOINTB_OP_INCR         UINT32_C(1)
#define HORIZON_SYNCPOINTB_WAIT_SWITCH_EN  (UINT32_C(1) << 4)
#define HORIZON_SYNCPOINTB_INDEX_SHIFT     8u
#define HORIZON_SYNCPT_ID_MAX              UINT32_C(0xFFF) /* 12-bit index */

/* Fixed subchannel assignment for the engine binds, matching the NVN /
 * deko3d convention the reference confirmed on hardware
 * (reference-analysis § 6): 0=3D, 1=compute, 2=inline-to-memory, 3=2D,
 * 4=copy. The class numbers themselves are queried, never assumed. */
#define HORIZON_CMDS_NUM_SUBCHANNELS 5u

/* Emitted dword counts. */
#define HORIZON_CMDS_FENCE_INCR_DWORDS   6u
#define HORIZON_CMDS_SET_OBJECTS_DWORDS  (2u * HORIZON_CMDS_NUM_SUBCHANNELS)
#define HORIZON_CMDS_SYNCPT_WAIT_DWORDS  4u

/* Pushbuffer method header, "increasing methods" opcode (001b in bits
 * 31:29; count 28:16; subchannel 15:13; dword method address 12:0) —
 * envytools gf100+ pushbuffer format. */
static inline uint32_t horizon_cmd_hdr_incr(uint32_t subch, uint32_t method,
                                            uint32_t count)
{
    return (UINT32_C(1) << 29) | (count << 16) | (subch << 13) |
           (method >> 2);
}

/* WFI + syncpoint increment (the shape Linux nvgpu submits for every job
 * end on gk20a/gm20b: wait-for-idle so completion means the work is done,
 * then SYNCPOINTA payload + SYNCPOINTB incr). Returns the dword count, or
 * 0 when syncpt_id exceeds the 12-bit index field. */
uint32_t horizon_cmds_fence_incr(uint32_t buf[HORIZON_CMDS_FENCE_INCR_DWORDS],
                                 uint32_t syncpt_id);

/* GPU-side syncpoint wait: SYNCPOINTA payload=threshold + SYNCPOINTB
 * wait|switch (known-risks R10 — to be validated on hardware in Phase 1).
 * Returns the dword count, or 0 on invalid syncpt_id. */
uint32_t
horizon_cmds_syncpt_wait(uint32_t buf[HORIZON_CMDS_SYNCPT_WAIT_DWORDS],
                         uint32_t syncpt_id, uint32_t threshold);

/* In-stream SET_OBJECT binds for subchannels 0..4 (known-risks R7).
 * `classes` in subchannel order. Returns the dword count. */
uint32_t
horizon_cmds_set_objects(uint32_t buf[HORIZON_CMDS_SET_OBJECTS_DWORDS],
                         const uint32_t classes[HORIZON_CMDS_NUM_SUBCHANNELS]);

/* `pairs` NOP methods (2 dwords each) into buf. Returns the dword count. */
uint32_t horizon_cmds_nop(uint32_t *buf, uint32_t pairs);

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_SUBMIT_CMDS_H */
