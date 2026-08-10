/*
 * Phase 1 test framework — console + sdmc logging main() for the .nro tests.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "testfw.h"

/* Generated on every build by scripts/gen-build-id.sh. */
#include "horizon_build_id.h"

#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

/* Serialises everything that reaches the log: both writes and the
 * offset games t_log_scan plays with the shared handle. See the comment
 * on test_ctx::out_lock for why a test framework needs a lock at all.
 *
 * The public entry points take it; t_sink and t_vemit below assume it is
 * held. Formatting happens into stack buffers, so only the emit itself
 * is inside the lock and a slow SD-card flush never serialises anything
 * but other writers. */
static void t_lock(test_ctx *t)
{
    if (t->out_lock_ready)
        pthread_mutex_lock(&t->out_lock);
}

static void t_unlock(test_ctx *t)
{
    if (t->out_lock_ready)
        pthread_mutex_unlock(&t->out_lock);
}

/* One place that knows where a line goes: the console (or nowhere, on a
 * test that owns the display) and the log file. Call with the lock. */
static void t_sink(test_ctx *t, const char *line, size_t len)
{
    fwrite(line, 1, len, stdout);
    if (t->log) {
        fwrite(line, 1, len, t->log);
        fflush(t->log);
    }
}

static void t_vemit(test_ctx *t, const char *prefix, const char *fmt,
                    va_list ap)
{
    char body[512];
    vsnprintf(body, sizeof(body), fmt, ap);

    char line[640];
    const int n = snprintf(line, sizeof(line), "%s%s\n", prefix, body);
    if (n <= 0)
        return;
    const size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
    t_sink(t, line, len);
}

bool t_check(test_ctx *t, bool cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    /* The counters are inside the lock with the line they belong to, so
     * a count can never disagree with the number of lines that produced
     * it. Tests still keep t_check on the main thread — a worker has no
     * way to report a verdict in order — but "the counter is safe" is
     * now a property of this function rather than of every caller. */
    t_lock(t);
    if (cond)
        t->pass++;
    else
        t->fail++;
    t_vemit(t, cond ? "  ok   " : "  FAIL ", fmt, ap);
    t_unlock(t);

    va_end(ap);
    return cond;
}

void t_note(test_ctx *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    t_lock(t);
    t_vemit(t, "  note ", fmt, ap);
    t_unlock(t);
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

    /* Before the log is opened and long before any test starts a thread,
     * so every line this file can emit is already covered. A failure
     * here leaves out_lock_ready false, which costs the serialisation
     * and keeps the reporting: a test that cannot lock must still be
     * able to say what went wrong. */
    if (pthread_mutex_init(&t.out_lock, NULL) == 0)
        t.out_lock_ready = true;

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

    char head[160];
    int head_len = snprintf(head, sizeof(head), "== %s ==\n", test_name);
    if (head_len > 0) {
        t_lock(&t);
        t_sink(&t, head, (size_t)head_len < sizeof(head) ? (size_t)head_len
                                                         : sizeof(head) - 1);
        t_unlock(&t);
    }
    if (!t.log)
        printf("  note (sdmc log unavailable: %s)\n", path);

    /* WHICH BUILD THIS IS, in the second line of every log.
     *
     * A .nro on an SD card looks exactly like the .nro it replaced, and
     * a run that reports the previous build's behaviour is
     * indistinguishable from a fix that did not work — which cost this
     * project a hardware run on 2026-08-05. The stamp is regenerated on
     * every build; the run instructions say which one to expect.
     *
     * One string literal, marker included, and printed with "%s" rather
     * than composed by the format: the same bytes then appear in the
     * .nro and in the log, so scripts/package-horizon.sh can read a
     * binary's identity out of the binary and the operator can match it
     * against the log by eye. Composing it ("build %s") would leave the
     * marker and the stamp as two unrelated literals in the image, and
     * whatever the manifest then grepped for would not be the thing the
     * log prints.
     */
    static const char build_id_line[] = "horizon-build-id " HORIZON_BUILD_ID;
    t_note(&t, "%s", build_id_line);

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
    char tail[160];
    int tail_len = snprintf(tail, sizeof(tail), "RESULT: %s (%d/%d)%s\n",
                            verdict, t.pass, total,
                            aborted ? " [aborted early]" : "");
    if (tail_len > 0) {
        t_lock(&t);
        t_sink(&t, tail, (size_t)tail_len < sizeof(tail) ? (size_t)tail_len
                                                         : sizeof(tail) - 1);
        t_unlock(&t);
    }

    printf("\nLog: %s\nPress + to exit.\n", path);
    /* With no console there is no screen to read that on, so the log —
     * which is the whole record for such a run — says it instead.
     * Found in review of PR #7.
     *
     * AND IT NEVER APPEARED. This wrote to `t.log` after fclose() had
     * already closed it, three lines above: the handle was closed but
     * not cleared, so the guard `t.log != NULL` was still true. The
     * line is absent from every log collected up to run 14 — check any
     * of them, they end at RESULT — and writing to a closed stream is
     * undefined behaviour, not merely a lost line. The log is now
     * closed once, at the end, after everything that writes to it, and
     * runs 15 and 16 are the first logs in this project that carry the
     * line. */
    if (test_uses_display)
        t_note(&t, "the run is finished; press + to exit (there is no "
                   "console to show this)");

    if (t.log) {
        fclose(t.log);
        t.log = NULL;
    }

    /* After the last writer, and after the log is closed. Clearing the
     * flag first means anything that still tries to report — a stray
     * driver message on a thread a test failed to join, which is exactly
     * the situation worth surviving — falls back to the unlocked path
     * instead of touching a destroyed mutex. */
    if (t.out_lock_ready) {
        t.out_lock_ready = false;
        pthread_mutex_destroy(&t.out_lock);
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

    /* HELD ACROSS THE WHOLE SCAN, not just the reads. This rewinds the
     * shared handle and puts it back, so a line written by another
     * thread in between would land at the wrong offset and overwrite the
     * log rather than extend it. The header used to say this was "not
     * thread-safe against anything else writing to stderr" and leave it
     * there; with the debug messenger able to emit from any thread, that
     * is not a caveat a caller can act on. */
    t_lock(t);

    /* Both writers, because stderr was dup2'd onto this file and the
     * lines that matter most are usually the driver's. */
    fflush(t->log);
    fflush(stderr);

    const long resume = ftell(t->log);
    if (resume < 0) {
        t_unlock(t);
        return false;
    }
    if (fseek(t->log, 0, SEEK_SET) != 0) {
        t_unlock(t);
        return false;
    }

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
    if (fseek(t->log, resume, SEEK_SET) != 0) {
        t_unlock(t);
        return false;
    }

    t_unlock(t);

    *found_out = found;
    return true;
}
