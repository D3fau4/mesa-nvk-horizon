/*
 * Host unit tests — status code names (horizon/debug/status.c).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <string.h>

#include "../../horizon/include/horizon_gpu/result.h"
#include "hostfw.h"

int main(void)
{
    /* Every declared status has its own non-generic string — a status
     * added to the enum without a case here would silently fall through
     * to "unknown status" and every caller that logs res.status would
     * lose the distinction; horizon_gpu_status_str() is called from every
     * test's failure message, so the table needs to be exhaustive. */
    H_CHECK(strcmp(horizon_gpu_status_str(HORIZON_GPU_OK), "ok") == 0, "OK");
    H_CHECK(strcmp(horizon_gpu_status_str(HORIZON_GPU_ERR_INVALID_ARG),
                   "invalid argument") == 0, "ERR_INVALID_ARG");
    H_CHECK(strcmp(horizon_gpu_status_str(HORIZON_GPU_ERR_OVERFLOW),
                   "arithmetic overflow") == 0, "ERR_OVERFLOW");
    H_CHECK(strcmp(horizon_gpu_status_str(HORIZON_GPU_ERR_OUT_OF_MEMORY),
                   "out of host memory") == 0, "ERR_OUT_OF_MEMORY");
    H_CHECK(strcmp(horizon_gpu_status_str(HORIZON_GPU_ERR_NV),
                   "nv service error") == 0, "ERR_NV");
    H_CHECK(strcmp(horizon_gpu_status_str(HORIZON_GPU_ERR_TIMEOUT),
                   "timeout") == 0, "ERR_TIMEOUT");
    H_CHECK(strcmp(horizon_gpu_status_str(HORIZON_GPU_ERR_VA_EXHAUSTED),
                   "VA interval exhausted") == 0, "ERR_VA_EXHAUSTED");
    H_CHECK(strcmp(horizon_gpu_status_str(HORIZON_GPU_ERR_BUSY),
                   "object busy (live children or in-flight work)") == 0,
            "ERR_BUSY");
    H_CHECK(strcmp(horizon_gpu_status_str(HORIZON_GPU_ERR_CHANNEL_LOST),
                   "channel lost") == 0, "ERR_CHANNEL_LOST");
    H_CHECK(strcmp(horizon_gpu_status_str(HORIZON_GPU_ERR_LEAK),
                   "leak detected at teardown") == 0, "ERR_LEAK");
    H_CHECK(strcmp(horizon_gpu_status_str(HORIZON_GPU_ERR_UNSUPPORTED),
                   "unsupported") == 0, "ERR_UNSUPPORTED");
    H_CHECK(strcmp(horizon_gpu_status_str(HORIZON_GPU_ERR_STATE),
                   "wrong object state") == 0, "ERR_STATE");
    H_CHECK(strcmp(horizon_gpu_status_str(HORIZON_GPU_ERR_IO),
                   "filesystem I/O error") == 0, "ERR_IO");
    H_CHECK(strcmp(horizon_gpu_status_str(HORIZON_GPU_ERR_NOT_FOUND),
                   "entry not found") == 0, "ERR_NOT_FOUND");

    /* Every string above must also be distinct from every other, and from
     * the fallback below — two statuses sharing a message would be just
     * as misleading as an "unknown status" gap. */
    const horizon_gpu_status all[] = {
        HORIZON_GPU_OK, HORIZON_GPU_ERR_INVALID_ARG, HORIZON_GPU_ERR_OVERFLOW,
        HORIZON_GPU_ERR_OUT_OF_MEMORY, HORIZON_GPU_ERR_NV,
        HORIZON_GPU_ERR_TIMEOUT, HORIZON_GPU_ERR_VA_EXHAUSTED,
        HORIZON_GPU_ERR_BUSY, HORIZON_GPU_ERR_CHANNEL_LOST,
        HORIZON_GPU_ERR_LEAK, HORIZON_GPU_ERR_UNSUPPORTED,
        HORIZON_GPU_ERR_STATE, HORIZON_GPU_ERR_IO,
        HORIZON_GPU_ERR_NOT_FOUND,
    };
    int distinct = 1;
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        for (size_t j = i + 1; j < sizeof(all) / sizeof(all[0]); j++)
            if (strcmp(horizon_gpu_status_str(all[i]),
                       horizon_gpu_status_str(all[j])) == 0)
                distinct = 0;
    H_CHECK(distinct, "every status has a distinct string");

    /* An out-of-range value (never emitted by this layer) falls back to
     * the generic string rather than undefined behaviour. */
    H_CHECK(strcmp(horizon_gpu_status_str((horizon_gpu_status)999),
                   "unknown status") == 0, "out-of-range status falls back");

    return h_summary("h_status");
}
