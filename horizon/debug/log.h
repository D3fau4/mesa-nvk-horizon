/*
 * horizon_gpu — structured logging with no global mutable state.
 *
 * The log configuration lives inside each horizon_gpu_device (CLAUDE.md bans
 * ambient state); every module logs through the config pointer it was handed.
 * Pure C11, libnx-free, so it compiles for both host and Horizon.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_DEBUG_LOG_H
#define HORIZON_DEBUG_LOG_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum horizon_log_level {
    HORIZON_LOG_NONE  = 0,
    HORIZON_LOG_ERROR = 1,
    HORIZON_LOG_WARN  = 2,
    HORIZON_LOG_INFO  = 3,
    HORIZON_LOG_DEBUG = 4,
} horizon_log_level;

typedef struct horizon_log {
    horizon_log_level level;
    FILE *stream; /* not owned; defaults to stderr */
} horizon_log;

/* Initialise from the HORIZON_GPU_LOG environment variable ("0".."4";
 * unset/invalid -> HORIZON_LOG_WARN) and the given stream (NULL -> stderr). */
void horizon_log_init_from_env(horizon_log *log, FILE *stream);

/* printf-style; drops the message when `log` is NULL or below `level`. */
__attribute__((format(printf, 3, 4)))
void horizon_logf(const horizon_log *log, horizon_log_level level,
                  const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_DEBUG_LOG_H */
