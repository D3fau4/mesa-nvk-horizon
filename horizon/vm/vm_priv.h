/*
 * horizon_gpu — VM internals shared with channel/ (internal buffers need
 * reservations and mappings). Not installed.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_VM_VM_PRIV_H
#define HORIZON_VM_VM_PRIV_H

#include "horizon_gpu/vm.h"
#include "va_space.h"

struct horizon_gpu_va_range {
    horizon_gpu_device *dev;  /* not owned */
    uint64_t base;            /* kernel-chosen base VA */
    uint64_t size;            /* bytes, page-size multiple */
    uint32_t page_size;
    horizon_va_set live;      /* live mapping intervals, offsets rel. base */
};

struct horizon_gpu_mapping {
    horizon_gpu_va_range *range; /* not owned */
    horizon_gpu_mem *mem;        /* not owned */
    uint64_t va;
    uint64_t size;
};

#endif /* HORIZON_VM_VM_PRIV_H */
