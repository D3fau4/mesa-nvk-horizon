/*
 * horizon_gpu — device internals shared by the sibling implementation
 * modules (memory/, vm/, channel/, submit/, sync/). Never installed;
 * everything above the layer sees only horizon_gpu/device.h.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_DEVICE_PRIV_H
#define HORIZON_DEVICE_PRIV_H

#include <stdatomic.h>

#include <switch.h>

#include "horizon_gpu/device.h"
#include "../debug/log.h"

struct horizon_gpu_device {
    /* GPU address space every allocation and channel of this device lives
     * in. Owned here; closed last-but-one at destroy (before nvGpuExit). */
    NvAddressSpace as;

    horizon_gpu_device_info info;

    horizon_log log;
    bool debug_synchronous;

    /* Leak accounting (memory-model § 8). Atomics so concurrent child
     * create/destroy on different threads keeps counts exact; individual
     * child objects are externally synchronized like the rest of the API. */
    _Atomic uint32_t live_mem;
    _Atomic uint32_t live_va_ranges;
    _Atomic uint32_t live_mappings;
    _Atomic uint32_t live_channels;
};

#endif /* HORIZON_DEVICE_PRIV_H */
