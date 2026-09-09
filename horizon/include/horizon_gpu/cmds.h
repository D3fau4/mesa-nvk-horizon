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

/* WFI SCOPE is bit 0: CURRENT_SCG_TYPE = 0, ALL = 1 (clb06f.h:141-143).
 * The narrower scope does not cover a copy-engine transfer, which is not
 * in the graphics scheduling class group. */
#define HORIZON_WFI_SCOPE_ALL      UINT32_C(1)
#define HORIZON_NVA06F_SEMAPHOREA  UINT32_C(0x0010) /* OFFSET_UPPER 7:0    */
#define HORIZON_NVA06F_SEMAPHOREB  UINT32_C(0x0014) /* OFFSET_LOWER 31:2   */
#define HORIZON_NVA06F_SEMAPHOREC  UINT32_C(0x0018) /* PAYLOAD 31:0        */
#define HORIZON_NVA06F_SEMAPHORED  UINT32_C(0x001C) /* OPERATION/flags     */

/* SEMAPHORED fields. Verified against the Maxwell channel class header
 * for the exact class GM20B's characteristics report (gpfifo=0xb06f),
 * mesa/src/nouveau/headers/nvidia/classes/clb06f.h:81-95 — not recalled.
 * OPERATION is 4:0; RELEASE_WFI is 20:20 and its *enabled* encoding is 0;
 * RELEASE_SIZE is 24:24 with 4BYTE = 1. */
#define HORIZON_SEMAPHORED_OP_RELEASE      UINT32_C(2)
#define HORIZON_SEMAPHORED_RELEASE_WFI_EN  UINT32_C(0)
#define HORIZON_SEMAPHORED_RELEASE_SIZE_4B (UINT32_C(1) << 24)

/* GPU virtual addresses are this wide. SEMAPHOREA carries OFFSET_UPPER in
 * bits 7:0 and SEMAPHOREB carries OFFSET_LOWER in bits 31:2, so the pair
 * encodes exactly 40 bits and an address above that cannot be expressed —
 * it would be truncated into a different, valid address.
 *
 * This file is libnx-free and cannot query anything, so the width is a
 * constant here. It is not left as an assumption: the device layer reads
 * gpu_va_bit_count from the GPU's characteristics and refuses to create a
 * device whose address space is a different width (device.c), so a chip
 * that reported something else would fail loudly at init rather than
 * silently truncate addresses at submit time. */
#define HORIZON_CMDS_GPU_VA_BITS 40u

/* MEM_OP_C/D — memory-barrier and L2 operations, also host methods, so
 * they need no engine object either. MEM_OP_A/B were removed for gm20x
 * and C/D carry their functionality (clb06f.h:112-136). OPERATION is
 * bits 31:27 of MEM_OP_D; the operands are only used by the TLB
 * invalidate operations, so the barrier and flush forms write zero. */
#define HORIZON_NVA06F_MEM_OP_C    UINT32_C(0x0030)
#define HORIZON_NVA06F_MEM_OP_D    UINT32_C(0x0034)
#define HORIZON_MEM_OP_D_OP_SHIFT  27u
#define HORIZON_MEM_OP_MEMBAR              UINT32_C(0x05)
#define HORIZON_MEM_OP_L2_SYSMEM_INVALIDATE UINT32_C(0x0e)
#define HORIZON_MEM_OP_L2_FLUSH_DIRTY      UINT32_C(0x10)

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
#define HORIZON_CMDS_FENCE_INCR_DWORDS   9u
#define HORIZON_CMDS_FENCE_INCR_BARE_DWORDS 4u
#define HORIZON_CMDS_SET_OBJECTS_DWORDS  (2u * HORIZON_CMDS_NUM_SUBCHANNELS)
#define HORIZON_CMDS_SYNCPT_WAIT_DWORDS  4u
#define HORIZON_CMDS_SEM_RELEASE_DWORDS  5u
#define HORIZON_CMDS_MEM_OP_DWORDS       3u

/* Pushbuffer method header, "increasing methods" opcode (001b in bits
 * 31:29; count 28:16; subchannel 15:13; dword method address 12:0) —
 * envytools gf100+ pushbuffer format. Every call site in this file passes
 * constants that already fit; the masks below exist so that an
 * out-of-range field (count > 0x1FFF, subch > 7, method >= 0x8000) is
 * truncated into its own bit range instead of silently bleeding into and
 * corrupting an adjacent one. */
static inline uint32_t horizon_cmd_hdr_incr(uint32_t subch, uint32_t method,
                                            uint32_t count)
{
    return (UINT32_C(1) << 29) | ((count & 0x1FFFu) << 16) |
           ((subch & 0x7u) << 13) | ((method >> 2) & 0x1FFFu);
}

/* The fence block: wait-for-idle (SCOPE_ALL), dirty-L2 writeback, then
 * SYNCPOINTA payload + SYNCPOINTB incr.
 *
 * The shape is nvgpu's gk20a/gm20b job end plus the one thing this
 * hardware needs and a desktop part does not. A GPU write lands in the
 * GPU's L2 first and nothing about a syncpoint increment obliges that L2
 * to reach the memory a CPU reads — measured on hardware 2026-08-04
 * (t_gpuwrite: no flush never showed the write, a MEMBAR did not help,
 * L2_FLUSH_DIRTY did). So the fence means "done *and* visible", which is
 * the only thing a fence can usefully mean here.
 *
 * The order is load-bearing. Flushing before the wait would leave
 * anything that completed during the wait sitting in L2, which is exactly
 * how asynchronous engine work escapes a flush that host methods do not.
 *
 * Returns the dword count, or 0 when syncpt_id exceeds the 12-bit index
 * field. */
uint32_t horizon_cmds_fence_incr(uint32_t buf[HORIZON_CMDS_FENCE_INCR_DWORDS],
                                 uint32_t syncpt_id);

/* The same increment WITHOUT the wait-for-idle and the L2 writeback:
 * SYNCPOINTA payload + SYNCPOINTB incr, and nothing else.
 *
 * WHEN THIS IS THE RIGHT BLOCK, AND IT IS A NARROW WHEN. The two methods
 * horizon_cmds_fence_incr puts in front of the increment answer two
 * questions, and a submit that touches no memory asks neither:
 *
 *   WFI SCOPE_ALL   makes the increment mean "the engines are done".
 *                   A command list built only from HOST methods has no
 *                   engine work to be done with: the host executes
 *                   SYNCPOINTA/B in pushbuffer order, so an increment
 *                   emitted after them is already after them.
 *   L2_FLUSH_DIRTY  makes the increment mean "the writes are visible".
 *                   A submit that wrote nothing has nothing to flush,
 *                   and the writes it may be *waiting on* were flushed
 *                   by the fence block of the channel that made them
 *                   before it signalled the fence being waited for.
 *
 * So this is for a submit whose whole command list is host methods with
 * no memory effect — horizon_gpu_submit_waits is the one caller, and it
 * builds its list itself out of horizon_cmds_syncpt_wait. It is NOT for
 * anything the caller supplied: this layer cannot know what a caller's
 * command list touched, and a wrong answer here is a fence that reports
 * work as complete and visible when it is neither.
 *
 * Returns the dword count, or 0 when syncpt_id exceeds the 12-bit index
 * field. */
uint32_t
horizon_cmds_fence_incr_bare(uint32_t buf[HORIZON_CMDS_FENCE_INCR_BARE_DWORDS],
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

/* Host semaphore release: writes `payload` to `gpu_va` from the GPU.
 *
 * This is the cheapest GPU write to memory there is on this channel — a
 * host method, so no engine object has to be bound to any subchannel
 * first — which is what makes it the right instrument for asking whether
 * a GPU write becomes visible to the CPU at all.
 *
 * RELEASE_WFI is enabled so the write is ordered after preceding work in
 * the channel rather than at method-fetch time, and RELEASE_SIZE is 4BYTE
 * so exactly the payload dword lands and nothing around it is disturbed —
 * the 16-byte form would also write a timestamp.
 *
 * Returns the dword count, or 0 without writing anything if `gpu_va` is
 * not 4-byte aligned (OFFSET_LOWER is bits 31:2, so a misaligned address
 * would be silently truncated into a different, valid-looking one) or
 * does not fit HORIZON_CMDS_GPU_VA_BITS. */
uint32_t
horizon_cmds_semaphore_release(uint32_t buf[HORIZON_CMDS_SEM_RELEASE_DWORDS],
                               uint64_t gpu_va, uint32_t payload);

/* A MEM_OP with no operand: MEMBAR, L2_SYSMEM_INVALIDATE or
 * L2_FLUSH_DIRTY. A GPU write lands in the GPU's L2 first, and nothing
 * about a syncpoint increment obliges that L2 to reach memory the CPU
 * reads — which is a candidate explanation for a write the GPU made and
 * the CPU cannot see. Returns the dword count, or 0 for an operation
 * that does not fit the 5-bit OPERATION field. */
uint32_t horizon_cmds_mem_op(uint32_t buf[HORIZON_CMDS_MEM_OP_DWORDS],
                             uint32_t operation);

/* `pairs` NOP methods (2 dwords each) into buf, which holds `buf_dwords`
 * dwords of capacity. Returns the dword count, or 0 without writing
 * anything if `pairs` would not fit in that capacity. */
uint32_t horizon_cmds_nop(uint32_t *buf, uint32_t buf_dwords, uint32_t pairs);

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_SUBMIT_CMDS_H */
