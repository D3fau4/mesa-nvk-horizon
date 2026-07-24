/*
 * Minimal host-side harness for the pure-logic unit tests (compiled with
 * the host compiler + sanitizers by scripts/run-host-tests.sh; no libnx).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_HOSTFW_H
#define HORIZON_HOSTFW_H

#include <stdio.h>

static int h_pass, h_fail;

#define H_CHECK(cond, name)                                                  \
    do {                                                                     \
        if (cond) {                                                          \
            h_pass++;                                                        \
        } else {                                                             \
            h_fail++;                                                        \
            printf("  FAIL %s (%s:%d)\n", name, __FILE__, __LINE__);         \
        }                                                                    \
    } while (0)

static inline int h_summary(const char *suite)
{
    printf("RESULT: %s (%d/%d) %s\n", h_fail == 0 ? "PASS" : "FAIL", h_pass,
           h_pass + h_fail, suite);
    return h_fail == 0 ? 0 : 1;
}

#endif /* HORIZON_HOSTFW_H */
