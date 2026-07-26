/*
 * Host unit tests — structured logging (horizon/debug/log.c).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L /* setenv/unsetenv/fmemopen */

#include <stdlib.h>
#include <string.h>

#include "../../horizon/debug/log.h"
#include "hostfw.h"

int main(void)
{
    /* HORIZON_GPU_LOG parsing. */
    unsetenv("HORIZON_GPU_LOG");
    horizon_log log;
    horizon_log_init_from_env(&log, NULL);
    H_CHECK(log.level == HORIZON_LOG_WARN, "unset env defaults to WARN");
    H_CHECK(log.stream == stderr, "NULL stream argument defaults to stderr");

    setenv("HORIZON_GPU_LOG", "4", 1);
    horizon_log_init_from_env(&log, NULL);
    H_CHECK(log.level == HORIZON_LOG_DEBUG, "env \"4\" selects DEBUG");

    setenv("HORIZON_GPU_LOG", "0", 1);
    horizon_log_init_from_env(&log, NULL);
    H_CHECK(log.level == HORIZON_LOG_NONE, "env \"0\" selects NONE");

    setenv("HORIZON_GPU_LOG", "9", 1);
    horizon_log_init_from_env(&log, NULL);
    H_CHECK(log.level == HORIZON_LOG_WARN,
            "out-of-range digit falls back to WARN");

    setenv("HORIZON_GPU_LOG", "1x", 1);
    horizon_log_init_from_env(&log, NULL);
    H_CHECK(log.level == HORIZON_LOG_WARN,
            "multi-character value falls back to WARN");

    unsetenv("HORIZON_GPU_LOG");

    /* Level filtering and message formatting, captured through a memory
     * stream so the actual bytes horizon_logf() writes are checked, not
     * just that it does not crash. */
    char buf[256];
    memset(buf, 0, sizeof(buf));
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    horizon_log capture = { .level = HORIZON_LOG_WARN, .stream = f };

    horizon_logf(&capture, HORIZON_LOG_DEBUG, "should be dropped");
    fflush(f);
    H_CHECK(buf[0] == '\0', "message above the configured level is dropped");

    horizon_logf(&capture, HORIZON_LOG_WARN, "value=%d", 42);
    fflush(f);
    H_CHECK(strstr(buf, "[horizon_gpu:W] value=42\n") != NULL,
            "message at the configured level is tagged and formatted");
    fclose(f);

    /* Never crashes on a NULL log or a NONE level, even at the highest
     * severity — HORIZON_LOG_NONE must suppress everything, not just
     * anything below it. */
    horizon_logf(NULL, HORIZON_LOG_ERROR, "no log pointer");
    horizon_log off = { .level = HORIZON_LOG_NONE, .stream = stderr };
    horizon_logf(&off, HORIZON_LOG_ERROR, "level NONE suppresses everything");

    return h_summary("h_log");
}
