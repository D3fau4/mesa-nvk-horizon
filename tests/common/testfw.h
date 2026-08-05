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
    char log_path[128];
} test_ctx;

/* Record one check. Returns `cond` so callers can bail out on failure. */
__attribute__((format(printf, 3, 4)))
bool t_check(test_ctx *t, bool cond, const char *fmt, ...);

/* Free-form annotation (measurements, decoded errors). */
__attribute__((format(printf, 2, 3)))
void t_note(test_ctx *t, const char *fmt, ...);

/* True when the log written so far contains `needle`.
 *
 * WHY A TEST WOULD READ ITS OWN LOG. main() dup2s stderr onto this
 * file, so everything the driver says with mesa_loge, mesa_logw or
 * vk_errorf is in it, interleaved with the test's own lines. Some of
 * what a driver reports has no Vulkan representation at all — a memory
 * object it could not destroy, a teardown it refused — and a test that
 * cannot see those can only report success beside them. This is how a
 * check is made out of one.
 *
 * Flushes both writers first, so the scan sees everything up to the
 * call. Returns false when there is no log file. Chunked with an
 * overlap, so a needle that straddles a read boundary is still found;
 * `needle` must be shorter than 128 bytes.
 */
bool t_log_contains(test_ctx *t, const char *needle);

extern const char *const test_name;
int run_test(test_ctx *t);

/* Set to true by a test that owns the display.
 *
 * WHY IT EXISTS. main() calls consoleInit(NULL), which configures the
 * default nwindow with the console's own framebuffers and keeps them.
 * A swapchain on Horizon configures that same nwindow — Phase 6's whole
 * subject — so the two cannot both have it, and a presenting test that
 * kept the console would be fighting it for the buffers rather than
 * measuring anything.
 *
 * A test that sets this gets no console: no consoleInit, no
 * consoleUpdate, no consoleExit, and stdout goes nowhere. It loses
 * nothing that matters, because the artefact a run is reported through
 * has never been the screen — it is
 * sdmc:/horizon_gpu_tests/<name>.log, which already receives every
 * check line, every note, and Mesa's own stderr, flushed after each
 * one so a run that ends in a hang still leaves a complete record up to
 * the hang.
 *
 * EVERY TEST DEFINES IT, like test_name, and the linker enforces that:
 * a test that forgets fails to link with an undefined reference. The
 * first version of this made it a weak `const bool` defaulting to
 * false, which compiled, linked, and did not work — gcc folded the
 * weak definition into main() and consoleInit was called
 * unconditionally, with the symbol garbage-collected out of the binary
 * entirely. A default nobody can see is worse than a line in every
 * file.
 */
extern const bool test_uses_display;

#endif /* HORIZON_TESTFW_H */
