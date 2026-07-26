/*
 * horizon_gpu — logging implementation. Pure C11, libnx-free.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "log.h"

#include <stdarg.h>
#include <stdlib.h>

void horizon_log_init_from_env(horizon_log *log, FILE *stream)
{
    log->level = HORIZON_LOG_WARN;
    log->stream = stream ? stream : stderr;

    const char *env = getenv("HORIZON_GPU_LOG");
    if (env && env[0] >= '0' && env[0] <= '4' && env[1] == '\0')
        log->level = (horizon_log_level)(env[0] - '0');
}

void horizon_logf(const horizon_log *log, horizon_log_level level,
                  const char *fmt, ...)
{
    if (!log || !log->stream || level > log->level ||
        level == HORIZON_LOG_NONE)
        return;

    static const char *const tags[] = {
        [HORIZON_LOG_ERROR] = "E",
        [HORIZON_LOG_WARN]  = "W",
        [HORIZON_LOG_INFO]  = "I",
        [HORIZON_LOG_DEBUG] = "D",
    };

    fprintf(log->stream, "[horizon_gpu:%s] ", tags[level]);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(log->stream, fmt, ap);
    va_end(ap);

    fputc('\n', log->stream);
}
