/*
 * Test framework for the standalone .nro suites.
 *
 * ONE .nro IS A SUITE, AND A SUITE IS SEVERAL CASES. It prints one line
 * per check, one verdict per case, and one machine-checkable summary
 * ("RESULT: PASS (n/n)" / "RESULT: FAIL (k/n)") to stdout AND to
 * sdmc:/horizon_gpu_tests/<suite>.log, so a hardware run can be
 * reported back as text — no console access is required to read a
 * result.
 *
 * A case is identified everywhere as "<suite>/<case>":
 *
 *     -- vk_shaders/dynamic_loop --
 *       ok   ...
 *     CASE: PASS vk_shaders/dynamic_loop (13/13) in 412 ms
 *
 * Each case translation unit defines exactly one entry point:
 *
 *     TEST_CASE_DECL(vk_shaders, dynamic_loop) { ... }   // in the case
 *
 * and the suite translation unit names it:
 *
 *     TEST_CASE_DECL(vk_shaders, dynamic_loop);          // in the suite
 *     TEST_SUITE("vk_shaders", false,
 *                TEST_CASE(vk_shaders, dynamic_loop));
 *
 * The two spellings produce the same symbol, so a case listed and not
 * written, or written and not listed, is a link error rather than a
 * test that quietly never runs.
 *
 * testfw.c provides main(), which runs the cases in the order the suite
 * lists them.
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

/* Long enough for "<suite>/<case>" with room to spare; the suite and
 * case names are source identifiers, not input. */
#define TEST_CASE_ID_MAX 96

typedef struct test_ctx {
    /* The suite's totals, across every case it has run so far. The
     * RESULT line is these two. */
    int pass;
    int fail;

    /* The running case's own totals, reset at the top of each case.
     * The CASE line is these two, and a case that wants to say how far
     * it has got before a call that may not return should print these
     * rather than the suite's. */
    int case_pass;
    int case_fail;

    FILE *log; /* sdmc log file; may be NULL */
    char log_path[128];

    /* "<suite>/<case>" while a case is running, "<suite>" otherwise.
     * Read it through t_case_id(); it is here so the reporting path can
     * reach it without a function call while holding the lock. */
    char case_id[TEST_CASE_ID_MAX];

    /* Where in the log the running case's own output begins.
     *
     * t_log_scan searches from here rather than from the start of the
     * file, because in a suite the file also holds every earlier case's
     * driver messages — and a check built on "the driver said X" must
     * mean "said it during this case". Without this, one case's
     * VK_ERROR_DEVICE_LOST would answer every later case's scan for
     * one. Set by the framework at the top of each case; 0 before the
     * first one. */
    long case_log_start;

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

/* One case: a name unique within its suite, and the function that runs
 * it. The function returns non-zero to abandon *this case*; the suite
 * goes on to the next one.
 *
 * THAT IS THE CHANGE THAT MATTERS about grouping tests into suites. As
 * separate .nro, a test that gave up early cost its own run and nothing
 * else. Inside a suite the same early return must not take the other
 * cases with it, or one broken area would hide every area behind it —
 * which is the failure mode of a grouped suite, and the reason this is
 * a per-case verdict rather than one run_test. An abandoned case is a
 * failed case, and the suite's verdict is FAIL. */
typedef struct test_case {
    const char *name;
    int (*run)(test_ctx *t);
} test_case;

/* The symbol a case's entry point has, spelled the same way by the case
 * that defines it and the suite that lists it. */
#define TEST_CASE_FN(suite_, case_) test_case_##suite_##_##case_

/* Declares (in the suite) or opens (in the case) that entry point. */
#define TEST_CASE_DECL(suite_, case_) \
    int TEST_CASE_FN(suite_, case_)(test_ctx *t)

/* One entry of a suite's table. The case's name in the log is the
 * identifier, so it is the identifier that has to be semantic. */
#define TEST_CASE(suite_, case_) { #case_, TEST_CASE_FN(suite_, case_) }

/* The whole of a suite translation unit's contract, in one place, so
 * the table and its length cannot drift apart:
 *
 *     TEST_SUITE("gpu_memory", false,
 *                TEST_CASE(gpu_memory, alloc),
 *                TEST_CASE(gpu_memory, nvmap));
 *
 * The trailing semicolon is the caller's, as it is for any declaration:
 * the macro expands to three definitions and does not swallow one.
 */
#define TEST_SUITE(name_, uses_display_, ...)                       \
    const char *const suite_name = (name_);                         \
    const bool test_uses_display = (uses_display_);                 \
    const test_case suite_cases[] = { __VA_ARGS__ };                \
    const unsigned suite_case_count =                               \
        (unsigned)(sizeof(suite_cases) / sizeof(suite_cases[0]))

extern const char *const suite_name;
extern const test_case suite_cases[];
extern const unsigned suite_case_count;

/* Record one check. Returns `cond` so callers can bail out on failure. */
__attribute__((format(printf, 3, 4)))
bool t_check(test_ctx *t, bool cond, const char *fmt, ...);

/* Free-form annotation (measurements, decoded errors). */
__attribute__((format(printf, 2, 3)))
void t_note(test_ctx *t, const char *fmt, ...);

/* "<suite>/<case>" while a case is running, "<suite>" otherwise. Never
 * NULL, and stable for the case's lifetime — vkfw passes it as
 * VkApplicationInfo::pApplicationName, where the Vulkan runtime
 * vk_strdup()s it (vk_instance.c) rather than keeping the pointer, so
 * the buffer being reused by the next case reaches nothing. */
const char *t_case_id(const test_ctx *t);

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
 * SCOPED TO THE RUNNING CASE. The scan starts at test_ctx::case_log_start,
 * not at the start of the file: see the comment there.
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

/* The build stamp this .nro was built with — the same string the second
 * line of every log carries. Exposed through a function rather than the
 * macro because build/horizon_build_id.h is on testfw.c's include path
 * and on nothing else's, and a test that wants to say "this is the same
 * build as last time" needs the value, not the header. Never NULL. */
const char *t_build_id(void);

/* Set to true by a suite that owns the display.
 *
 * WHY IT EXISTS. main() calls consoleInit(NULL), which configures the
 * default nwindow with the console's own framebuffers and keeps them.
 * A swapchain on Horizon configures that same nwindow — Phase 6's whole
 * subject — so the two cannot both have it, and a presenting test that
 * kept the console would be fighting it for the buffers rather than
 * measuring anything.
 *
 * A suite that sets this gets no console: no consoleInit, no
 * consoleUpdate, no consoleExit, and stdout goes nowhere. It loses
 * nothing that matters, because the artefact a run is reported through
 * has never been the screen — it is
 * sdmc:/horizon_gpu_tests/<suite>.log, which already receives every
 * check line, every note, and Mesa's own stderr, flushed after each
 * one so a run that ends in a hang still leaves a complete record up to
 * the hang.
 *
 * IT IS PER SUITE AND NOT PER CASE, and that is what decides some of
 * the grouping: the console is configured once, before the first case
 * runs, so a case that needs a console and a case that needs the
 * display cannot share a .nro. `display` and `dock` are two suites for
 * this reason and no other.
 *
 * TEST_SUITE defines it, like suite_name, and the linker enforces that:
 * a suite that forgets fails to link with an undefined reference. The
 * first version of this made it a weak `const bool` defaulting to
 * false, which compiled, linked, and did not work — gcc folded the
 * weak definition into main() and consoleInit was called
 * unconditionally, with the symbol garbage-collected out of the binary
 * entirely. A default nobody can see is worse than a line in every
 * file.
 */
extern const bool test_uses_display;

#endif /* HORIZON_TESTFW_H */
