/*
 * horizon_gpu — memory object internals, shared with vm/ (mapping
 * bookkeeping) and channel/ (internal buffers). Not installed.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_MEMORY_MEM_PRIV_H
#define HORIZON_MEMORY_MEM_PRIV_H

#include <stdatomic.h>

#include <switch.h>

#include "horizon_gpu/memory.h"

struct horizon_gpu_mem {
    horizon_gpu_device *dev;   /* not owned */
    NvMap nvmap;               /* owned; closed at destroy */
    void *cpu;                 /* owned; aligned_alloc'd backing */
    uint64_t size;             /* rounded-up size in bytes */
    uint64_t align;
    horizon_gpu_cache_policy policy;

    /* Mapping bookkeeping, maintained by vm/ (memory-model § 2). */
    _Atomic uint32_t live_mappings;
    uint64_t mapped_va;        /* most recent live mapping's VA; 0 = none */
};

#endif /* HORIZON_MEMORY_MEM_PRIV_H */
