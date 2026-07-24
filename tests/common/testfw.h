/*
 * Phase 1 test framework for the standalone .nro tests.
 *
 * Each test prints one line per check and a machine-checkable summary
 * ("RESULT: PASS (n/n)" / "RESULT: FAIL (k/n)") to stdout AND to
 * sdmc:/horizon_gpu_tests/<name>.log, so a hardware run can be reported
 * back as text (docs/known-risks.md R2).
 *
 * Each test translation unit defines:
 *     const char *const test_name;
 *     int run_test(test_ctx *t);   // return != 0 aborts early
 * and links against testfw.c, which provides main().
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_TESTFW_H
#define HORIZON_TESTFW_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct test_ctx {
    int pass;
    int fail;
    FILE *log; /* sdmc log file; may be NULL */
} test_ctx;

/* Record one check. Returns `cond` so callers can bail out on failure. */
__attribute__((format(printf, 3, 4)))
bool t_check(test_ctx *t, bool cond, const char *fmt, ...);

/* Free-form annotation (measurements, decoded errors). */
__attribute__((format(printf, 2, 3)))
void t_note(test_ctx *t, const char *fmt, ...);

extern const char *const test_name;
int run_test(test_ctx *t);

#endif /* HORIZON_TESTFW_H */
