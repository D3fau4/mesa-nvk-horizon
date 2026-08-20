/*
 * horizon_gpu — status code names. Pure C11, libnx-free (host-testable).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "horizon_gpu/result.h"

const char *horizon_gpu_status_str(horizon_gpu_status status)
{
    switch (status) {
    case HORIZON_GPU_OK:                 return "ok";
    case HORIZON_GPU_ERR_INVALID_ARG:    return "invalid argument";
    case HORIZON_GPU_ERR_OVERFLOW:       return "arithmetic overflow";
    case HORIZON_GPU_ERR_OUT_OF_MEMORY:  return "out of host memory";
    case HORIZON_GPU_ERR_NV:             return "nv service error";
    case HORIZON_GPU_ERR_TIMEOUT:        return "timeout";
    case HORIZON_GPU_ERR_VA_EXHAUSTED:   return "VA interval exhausted";
    case HORIZON_GPU_ERR_BUSY:           return "object busy (live children or in-flight work)";
    case HORIZON_GPU_ERR_CHANNEL_LOST:   return "channel lost";
    case HORIZON_GPU_ERR_LEAK:           return "leak detected at teardown";
    case HORIZON_GPU_ERR_UNSUPPORTED:    return "unsupported";
    case HORIZON_GPU_ERR_STATE:          return "wrong object state";
    case HORIZON_GPU_ERR_IO:             return "filesystem I/O error";
    case HORIZON_GPU_ERR_NOT_FOUND:      return "entry not found";
    }
    return "unknown status";
}
