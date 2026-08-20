/*
 * horizon_gpu — result type shared by every public entry point.
 *
 * Every horizon_gpu_* function that can fail returns a horizon_gpu_result
 * carrying the underlying libnx Result when one exists (architecture.md § 6).
 *
 * This header is deliberately libnx-free so that the pure-logic modules and
 * their host-side unit tests can include it. `horizon_gpu_nv_result` mirrors
 * libnx's `Result` (a u32, 0 == success); the layer's libnx-facing .c files
 * static_assert the equivalence.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_GPU_RESULT_H
#define HORIZON_GPU_RESULT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirror of libnx `Result` (u32; 0 means success). */
typedef uint32_t horizon_gpu_nv_result;

typedef enum horizon_gpu_status {
    HORIZON_GPU_OK = 0,
    HORIZON_GPU_ERR_INVALID_ARG,   /* bad parameter (alignment, range, null)      */
    HORIZON_GPU_ERR_OVERFLOW,      /* a size/offset/alignment computation wrapped */
    HORIZON_GPU_ERR_OUT_OF_MEMORY, /* host allocation failed                      */
    HORIZON_GPU_ERR_NV,            /* an nv service call failed; see .nv          */
    HORIZON_GPU_ERR_TIMEOUT,       /* a wait hit its caller-supplied deadline     */
    HORIZON_GPU_ERR_VA_EXHAUSTED,  /* no space in the requested VA interval       */
    HORIZON_GPU_ERR_BUSY,          /* live children / in-flight work forbid this  */
    HORIZON_GPU_ERR_CHANNEL_LOST,  /* channel faulted; error notifier has details */
    HORIZON_GPU_ERR_LEAK,          /* teardown found live objects (memory-model §8) */
    HORIZON_GPU_ERR_UNSUPPORTED,   /* valid request this phase does not implement */
    HORIZON_GPU_ERR_STATE,         /* object is in the wrong state for this call  */
    /* Appended, never inserted: the numeric values above are printed in
     * hardware logs that are kept as evidence (docs/hw-logs/), so a run
     * from 2026-08 and a run from later must mean the same thing by the
     * same number. */
    HORIZON_GPU_ERR_IO,            /* a filesystem read/write/seek/flush failed   */
    HORIZON_GPU_ERR_NOT_FOUND,     /* the requested entry is not in the store     */
} horizon_gpu_status;

typedef struct horizon_gpu_result {
    horizon_gpu_status status;
    /* Underlying libnx Result when status == HORIZON_GPU_ERR_NV; 0 otherwise. */
    horizon_gpu_nv_result nv;
} horizon_gpu_result;

static inline horizon_gpu_result horizon_gpu_ok(void)
{
    return (horizon_gpu_result){ .status = HORIZON_GPU_OK, .nv = 0 };
}

static inline horizon_gpu_result horizon_gpu_err(horizon_gpu_status status)
{
    return (horizon_gpu_result){ .status = status, .nv = 0 };
}

static inline horizon_gpu_result horizon_gpu_err_nv(horizon_gpu_nv_result nv)
{
    return (horizon_gpu_result){ .status = HORIZON_GPU_ERR_NV, .nv = nv };
}

static inline bool horizon_gpu_failed(horizon_gpu_result r)
{
    return r.status != HORIZON_GPU_OK;
}

static inline bool horizon_gpu_succeeded(horizon_gpu_result r)
{
    return r.status == HORIZON_GPU_OK;
}

/* Human-readable name for a status code (implemented in horizon/debug/status.c,
 * libnx-free). Never returns NULL. */
const char *horizon_gpu_status_str(horizon_gpu_status status);

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_GPU_RESULT_H */
