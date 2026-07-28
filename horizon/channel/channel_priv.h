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
#include "horizon_gpu/memory.h"
#include "horizon_gpu/vm.h"

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

    /* Zcull context (optional). */
    horizon_gpu_mem *zcull_mem;
    horizon_gpu_mapping *zcull_map;

    /* 64-bit syncpoint shadow (docs/synchronization.md § 1.1):
     * shadow_target is the 64-bit value the counter will hold once every
     * submitted increment has retired. Initialised from the hardware
     * value observed at creation (R5: that value is recorded and
     * reported by t_channel). */
    uint32_t syncpt_value_at_create;
    uint64_t shadow_target;
    /* False when the initial read failed and the device's untrusted-baseline
     * opt-in let the channel come up anyway (docs/synchronization.md § 9).
     * Every fence this channel produces is then arithmetic on a baseline of
     * zero that nobody measured. */
    bool syncpt_baseline_trusted;

    /* Retirement list (docs/synchronization.md § 3). */
    horizon_retire_entry *retire;
    uint32_t retire_count;
    uint32_t retire_capacity;
};

/* Shared with submit/: reads the syncpoint and retires; both live in the
 * channel object. Defined in channel.c. */
horizon_gpu_result horizon_channel_read_syncpt(horizon_gpu_channel *chan,
                                               uint32_t *out_hw);

#endif /* HORIZON_CHANNEL_CHANNEL_PRIV_H */
