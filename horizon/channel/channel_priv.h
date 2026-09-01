/*
 * horizon_gpu — channel internals shared with submit/ and sync/.
 * Not installed.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_CHANNEL_CHANNEL_PRIV_H
#define HORIZON_CHANNEL_CHANNEL_PRIV_H

#include <switch.h>

#include "horizon_gpu/channel.h"
#include "horizon_gpu/cmds.h"
#include "horizon_gpu/submit.h"
#include "horizon_gpu/memory.h"
#include "horizon_gpu/vm.h"

/* Slots in the channel's GPU-side wait ring, and the dwords each holds.
 * Both are fixed by what is left of the command page after the three
 * write-once blocks; channel.c asserts the arithmetic against the page.
 */
#define HORIZON_CHANNEL_WAIT_SLOT_DWORDS     (HORIZON_CMDS_SYNCPT_WAIT_DWORDS * HORIZON_GPU_MAX_WAIT_FENCES)
#define HORIZON_CHANNEL_WAIT_SLOTS 24u
/* Byte offset of the ring inside the channel's command page. Here
 * rather than in channel.c because submit/ writes the slots and
 * channel/ places them, and a second copy of the number is how the
 * two would drift. */
#define HORIZON_CHANNEL_WAIT_CMDS_OFFSET UINT64_C(0x400)

typedef struct horizon_retire_entry {
    uint64_t threshold64; /* shadow-extended completion value */
    void (*fn)(void *ctx);
    void *ctx;
} horizon_retire_entry;

struct horizon_gpu_channel {
    horizon_gpu_device *dev; /* not owned */
    NvGpuChannel gc;
    uint32_t syncpt_id;
    bool lost;
    bool engines_bound;

    /* The last error-notification type this channel reported, kept
     * because reading one consumes it.
     *
     * MEASURED ON HARDWARE, 2026-08-04 (t_fault). libnx's
     * nvGpuChannelGetErrorNotification does a non-blocking eventWait on
     * the channel's error *event* before the ioctl, so the first reader
     * takes the notification and every reader after it is told there is
     * none. The first reader is channel_check_fault, on the wait path.
     * A caller that then received HORIZON_GPU_ERR_CHANNEL_LOST and
     * asked why was told "no error recorded" — for a channel that was
     * lost because of one. The type was in a log line and nowhere a
     * program could reach.
     *
     * Zero means none has ever been seen. */
    uint32_t last_error_type;

    /* Internal GPU-visible buffer: the per-submit fence-increment list at
     * offset 0, the SET_OBJECT list right after (written by
     * bind_engines). One page, mapped read-only-for-the-GPU concerns
     * aside, at the base of `internal_range`. */
    horizon_gpu_mem *cmdbuf_mem;
    horizon_gpu_va_range *internal_range;
    horizon_gpu_mapping *cmdbuf_map;
    uint64_t fence_cmds_va;
    uint32_t fence_cmds_dwords;
    uint64_t setobj_cmds_va;
    /* Runs before every submit's work: one L2_SYSMEM_INVALIDATE, so the
     * GPU cannot merge into an L2 line the CPU has since overwritten.
     * See horizon_gpu_channel_create for the hardware measurement. */
    uint64_t prologue_cmds_va;
    uint32_t prologue_cmds_dwords;
    /* The increment-only fence block, used instead of fence_cmds_* by a
     * submit whose command list is host methods with no memory effect.
     * horizon_gpu/cmds.h states when that is; horizon_gpu_submit_waits
     * is the only caller. */
    uint64_t bare_fence_cmds_va;
    uint32_t bare_fence_cmds_dwords;

    /* The GPU-side wait ring (horizon_gpu_submit_waits).
     *
     * A wait list is built at submit time — its thresholds are not known
     * before then — so unlike the three blocks above it cannot be one
     * buffer written once. It is a ring of fixed slots in the same page,
     * and a slot may be rewritten only once the GPU is provably done
     * fetching the list that was in it. `fence` is the fence of the
     * submit that used the slot and `busy` says whether it means
     * anything yet; a slot whose fence the syncpoint has passed is free.
     *
     * No lock: a channel is externally synchronised, like the rest of
     * this struct.
     */
    uint64_t wait_cmds_va;
    uint32_t wait_slot_next;
    struct {
        horizon_gpu_fence fence;
        bool busy;
    } wait_slots[HORIZON_CHANNEL_WAIT_SLOTS];

    /* Submit-path counters (horizon_gpu_channel_get_stats).
     *
     * Plain, not atomic, and that is the same rule as the rest of this
     * struct: a channel is externally synchronised. They exist because
     * two changes to this path — the increment-only fence block for a
     * memory-free submit, and skipping the syncpoint read when nothing
     * can retire — are claims about how much work a submit does, and a
     * claim about work done should be a number a test reads back rather
     * than a sentence in a commit message. */
    horizon_gpu_channel_stats stats;

    /* Zcull context (optional). */
    horizon_gpu_mem *zcull_mem;
    horizon_gpu_mapping *zcull_map;

    /* 64-bit syncpoint shadow:
     * shadow_target is the 64-bit value the counter will hold once every
     * submitted increment has retired. Initialised from the hardware
     * value observed at creation (R5: that value is recorded and
     * reported by t_channel). */
    uint32_t syncpt_value_at_create;
    uint64_t shadow_target;
    /* False when the initial read failed and the device's untrusted-baseline
     * opt-in let the channel come up anyway.
     * Every fence this channel produces is then arithmetic on a baseline of
     * zero that nobody measured. */
    bool syncpt_baseline_trusted;

    /* Retirement list. */
    horizon_retire_entry *retire;
    uint32_t retire_count;
    uint32_t retire_capacity;
};

/* Shared with submit/: reads the syncpoint and retires; both live in the
 * channel object. Defined in channel.c. */
horizon_gpu_result horizon_channel_read_syncpt(horizon_gpu_channel *chan,
                                               uint32_t *out_hw);

#endif /* HORIZON_CHANNEL_CHANNEL_PRIV_H */
