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

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct test_ctx {
    int pass;
    int fail;
    FILE *log; /* sdmc log file; may be NULL */
    char log_path[128];

    /* REPORTING IS NOT SINGLE-THREADED, AND CANNOT BE.
     *
     * It was documented as if it were: a test would state that only the
     * main thread calls t_check and t_note, and reason from there that
     * the shared FILE and the shared counters need no protection. The
     * reasoning was sound and the premise was false. vkfw installs a
     * VK_EXT_debug_utils messenger, and Mesa calls it on whichever
     * thread produced the message — so a worker doing nothing but
     * vkCreateImage, vkDestroySwapchainKHR or vkQueuePresentKHR reaches
     * t_note through the driver, without the test ever naming it. Two
     * such calls interleaved on one FILE, and `pass`/`fail` are plain
     * non-atomic increments.
     *
     * That is undefined behaviour in the reporting path of a suite whose
     * entire value is that undefined behaviour it observes belongs to
     * the driver. So the framework serialises its own output instead of
     * asking every test to promise something it cannot keep.
     *
     * Guarded by `out_lock_ready` because t_note is called before main()
     * has finished setting the context up, and a test that fails early
     * must still be able to say so. */
    pthread_mutex_t out_lock;
    bool out_lock_ready;
} test_ctx;

/* Record one check. Returns `cond` so callers can bail out on failure. */
__attribute__((format(printf, 3, 4)))
bool t_check(test_ctx *t, bool cond, const char *fmt, ...);

/* Free-form annotation (measurements, decoded errors). */
__attribute__((format(printf, 2, 3)))
void t_note(test_ctx *t, const char *fmt, ...);

/* Searches the log written so far for `needle`.
 *
 * WHY A TEST WOULD READ ITS OWN LOG. main() dup2s stderr onto this
 * file, so everything the driver says with mesa_loge, mesa_logw or
 * vk_errorf is in it, interleaved with the test's own lines. Some of
 * what a driver reports has no Vulkan representation at all — a memory
 * object it could not destroy, a teardown it refused — and a test that
 * cannot see those can only report success beside them. This is how a
 * check is made out of one.
 *
 * TWO OUTCOMES, NOT ONE, AND THAT IS THE POINT. The return value says
 * whether the scan happened; `*found_out` says what it found. An
 * earlier version returned only the second, and when the scan could not
 * be performed at all it answered "not found" — so a run that leaked 33
 * memory objects and said so in this very file reported
 * "ok the driver tore down every object it created". A check that
 * cannot look must not answer.
 *
 * Flushes both writers first, so the scan sees everything up to the
 * call, and reads through the log's own handle rather than opening the
 * file a second time — the SD card's device layer does not promise a
 * second handle to a file that is already open for writing, and that
 * is what the earlier version tripped over.
 *
 * `needle` must be shorter than 128 bytes. It rewinds the shared handle
 * and puts it back, so it holds test_ctx::out_lock for the whole scan —
 * a line written in between would land at the rewound offset and
 * overwrite the log instead of extending it. This used to be documented
 * as "not thread-safe against anything else writing to stderr", which
 * stopped being something a caller could act on once the debug-utils
 * messenger made any thread a possible writer.
 */
bool t_log_scan(test_ctx *t, const char *needle, bool *found_out);

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
