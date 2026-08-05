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
    snprintf(t.log_path, sizeof(t.log_path), "%s", path);
    /* "w+", not "w": t_log_scan reads the log back through this same
     * handle. A second fopen() of a file already open for writing is
     * not something the SD card's device layer promises, and the first
     * version of that check silently answered "nothing found" every
     * time because of it. */
    t.log = fopen(path, "w+");

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

/* Overlap between chunks: the longest needle this can find spans two
 * reads by at most this much, so a needle up to that length is never
 * split across a boundary and missed. */
#define T_LOG_SCAN_OVERLAP 128
#define T_LOG_SCAN_CHUNK   4096

bool t_log_scan(test_ctx *t, const char *needle, bool *found_out)
{
    *found_out = false;

    if (t->log == NULL || needle == NULL)
        return false;

    const size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > T_LOG_SCAN_OVERLAP)
        return false;

    /* Both writers, because stderr was dup2'd onto this file and the
     * lines that matter most are usually the driver's. */
    fflush(t->log);
    fflush(stderr);

    const long resume = ftell(t->log);
    if (resume < 0)
        return false;
    if (fseek(t->log, 0, SEEK_SET) != 0)
        return false;

    char buf[T_LOG_SCAN_OVERLAP + T_LOG_SCAN_CHUNK + 1];
    size_t carry = 0;
    bool found = false;

    for (;;) {
        const size_t n = fread(buf + carry, 1, T_LOG_SCAN_CHUNK, t->log);
        if (n == 0)
            break;

        const size_t total = carry + n;
        buf[total] = '\0';
        if (strstr(buf, needle) != NULL) {
            found = true;
            break;
        }

        /* Keep the tail, so a needle straddling this boundary is seen
         * with the next chunk. */
        const size_t keep = total < T_LOG_SCAN_OVERLAP ? total
                                                       : T_LOG_SCAN_OVERLAP;
        memmove(buf, buf + total - keep, keep);
        carry = keep;
    }

    /* Back where the writers left it, before anything else writes. A
     * failure here would put every later line in the wrong place, so it
     * is reported as a scan that did not happen. */
    if (fseek(t->log, resume, SEEK_SET) != 0)
        return false;

    *found_out = found;
    return true;
}
