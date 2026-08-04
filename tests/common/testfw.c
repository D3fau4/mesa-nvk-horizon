/*
 * Phase 1 test framework — console + sdmc logging main() for the .nro tests.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "testfw.h"

#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

static void t_vemit(test_ctx *t, const char *prefix, const char *fmt,
                    va_list ap)
{
    char line[512];
    vsnprintf(line, sizeof(line), fmt, ap);
    printf("%s%s\n", prefix, line);
    if (t->log) {
        fprintf(t->log, "%s%s\n", prefix, line);
        fflush(t->log);
    }
}

bool t_check(test_ctx *t, bool cond, const char *fmt, ...)
{
    if (cond)
        t->pass++;
    else
        t->fail++;

    va_list ap;
    va_start(ap, fmt);
    t_vemit(t, cond ? "  ok   " : "  FAIL ", fmt, ap);
    va_end(ap);
    return cond;
}

void t_note(test_ctx *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    t_vemit(t, "  note ", fmt, ap);
    va_end(ap);
}

int main(void)
{
    if (!test_uses_display)
        consoleInit(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    test_ctx t = { .pass = 0, .fail = 0, .log = NULL };

    mkdir("sdmc:/horizon_gpu_tests", 0777);
    char path[128];
    snprintf(path, sizeof(path), "sdmc:/horizon_gpu_tests/%s.log", test_name);
    t.log = fopen(path, "w");

    /* Everything Mesa says goes into the same file, in order.
     *
     * Mesa reports its diagnostics with mesa_logw/mesa_loge and
     * vk_errorf, all of which end up on stderr. On a console that is
     * the screen, and the screen is not what gets sent back — so twice
     * now a hardware run has failed with the reason visible for a few
     * seconds and absent from the artefact:
     *
     *   FAIL vkCreateDevice -> -13
     *
     * where -13 is VK_ERROR_UNKNOWN and the driver had already printed
     * exactly which horizon_gpu call failed and with which libnx
     * Result.
     *
     * dup2 onto the log's descriptor rather than a second file: the
     * driver's messages then interleave with the test's own lines in
     * the order they happened, which is most of their value. Unbuffered,
     * because the interesting case is the one that ends in a crash.
     * t_vemit flushes after every line, so the two writers stay in
     * order.
     */
    if (t.log) {
        fflush(stderr);
        if (dup2(fileno(t.log), STDERR_FILENO) < 0)
            printf("  note (could not redirect stderr into the log)\n");
        else
            setvbuf(stderr, NULL, _IONBF, 0);
    }

    printf("== %s ==\n", test_name);
    if (t.log)
        fprintf(t.log, "== %s ==\n", test_name);
    else
        printf("  note (sdmc log unavailable: %s)\n", path);

    /* Stated in the artefact, because a log with no console output in
     * it and a log from a run whose console never started look the
     * same otherwise. */
    if (test_uses_display && t.log) {
        fprintf(t.log, "  note this test owns the display: no console was "
                       "started, and this file is the whole record\n");
        fflush(t.log);
    }

    int aborted = run_test(&t);

    int total = t.pass + t.fail;
    const char *verdict = (t.fail == 0 && !aborted) ? "PASS" : "FAIL";
    printf("RESULT: %s (%d/%d)%s\n", verdict, t.pass, total,
           aborted ? " [aborted early]" : "");
    if (t.log) {
        fprintf(t.log, "RESULT: %s (%d/%d)%s\n", verdict, t.pass, total,
                aborted ? " [aborted early]" : "");
        fclose(t.log);
    }

    printf("\nLog: %s\nPress + to exit.\n", path);
    /* With no console there is no screen to read that on, so the log —
     * which is the whole record for such a run — says it instead.
     * Found in review of PR #7. */
    if (test_uses_display && t.log) {
        fprintf(t.log, "  note the run is finished; press + to exit "
                       "(there is no console to show this)\n");
        fflush(t.log);
    }
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        if (test_uses_display) {
            /* consoleUpdate is what blocked on vsync. Without it this
             * loop was an unthrottled spin on a core until a human
             * pressed +, on the one path t_display exists to validate.
             * A frame's worth of sleep costs nothing and is not hiding
             * a failure — there is nothing here to fail. */
            svcSleepThread(UINT64_C(16000000));
        } else {
            consoleUpdate(NULL);
        }
    }

    if (!test_uses_display)
        consoleExit(NULL);
    return (t.fail == 0 && !aborted) ? 0 : 1;
}
