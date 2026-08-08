/*
 * Phase 1 test framework — console + sdmc logging main() for the .nro tests.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "testfw.h"

/* Generated on every build by scripts/gen-build-id.sh. */
#include "horizon_build_id.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <switch.h>

/* ------------------------------------------------------------------
 * nxlink: the log on the developer's machine, while it happens
 *
 * WHY THIS IS NOT JUST CONVENIENCE. Two of these tests own the display
 * (t_nwindow, t_vk_swapchain): they start no console, so their stdout
 * has nowhere to go and the SD card file is the entire record — which
 * can only be read after the run, by taking the card out. Everything
 * this project learns from hardware has been going through that loop.
 *
 * And one of them can now hang rather than fail: the acquire session at
 * timeout = UINT64_MAX has no deadline left to expire. A hang leaves an
 * SD log that stops mid-run with no indication of where; a live stream
 * shows the last line that made it out, which is the whole diagnosis.
 *
 * WHAT IS SENT, AND WHY IT IS SENT TWICE. Every line this framework
 * writes goes to the socket as it is written. Then, at the end, the
 * whole log file is replayed down the same socket — because the file
 * holds something the live stream cannot: Mesa's own diagnostics.
 * Those arrive on stderr, which is dup2'd onto the log below, and they
 * are historically the lines that say why a run failed. The replay is
 * how an operator gets the complete artefact without touching the card.
 *
 * WHAT IS NOT DONE. stdout and stderr are deliberately NOT redirected
 * to the socket (nxlinkConnectToHost is asked for neither). Redirecting
 * stderr would take Mesa's messages out of the log file, which is the
 * artefact; redirecting stdout would blank the console on the tests
 * that have one. Nothing that exists today loses anything.
 * ------------------------------------------------------------------ */

static int  t_nx_sock = -1;
static bool t_nx_driver_up = false;
static bool t_nx_said_dropped = false;
static char t_nx_status[192] =
    "nxlink: no host, so nothing is streamed (this run was not launched "
    "by nxlink)";

/* Writes the whole buffer or gives up and says so. A short write on a
 * blocking socket is legal and a loop is the only correct answer;
 * losing the stream is reported once, into the log, because a stream
 * that silently stops looks exactly like a test that silently hung —
 * and telling those two apart is why this exists. */
static void t_nx_raw(const char *buf, size_t len)
{
    while (t_nx_sock >= 0 && len > 0) {
        const ssize_t n = write(t_nx_sock, buf, len);
        if (n > 0) {
            buf += (size_t)n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;

        const int why = errno;
        close(t_nx_sock);
        t_nx_sock = -1;
        if (!t_nx_said_dropped) {
            t_nx_said_dropped = true;
            /* stderr, not this function: stderr is the log file, and
             * this is the one line that has to survive in the artefact
             * when the network does not. */
            fprintf(stderr, "  note nxlink: the stream stopped (errno %d); "
                            "the rest of this run is in this file only\n",
                    why);
        }
    }
}

/* One place that knows where a line goes: the console (or nowhere, on a
 * test that owns the display), the log file, and the host. */
static void t_sink(test_ctx *t, const char *line, size_t len)
{
    fwrite(line, 1, len, stdout);
    if (t->log) {
        fwrite(line, 1, len, t->log);
        fflush(t->log);
    }
    t_nx_raw(line, len);
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

/* Connects if — and only if — this run was launched by nxlink.
 *
 * The socket driver is not started otherwise: it costs transfer memory,
 * and the Vulkan tests are the ones that have run out of it. A run
 * launched from the homebrew menu therefore behaves exactly as it did
 * before this existed, which is what keeps every measurement in
 * docs/hw-logs/ comparable with the ones taken after it.
 *
 * The default socket configuration is used unchanged. Shrinking the
 * buffers is the obvious tuning and it is not done here, because the
 * right values cannot be measured from this container and a guess
 * dressed as a constant is worse than the library's own default.
 *
 * The outcome is recorded rather than acted on: whichever way it went
 * is printed into the log a few lines later, so a run's timings can be
 * read knowing whether a socket was in the picture.
 */
static void t_nx_start(void)
{
    const in_addr_t host = __nxlink_host.s_addr;
    if (host == 0 || host == INADDR_NONE)
        return;

    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        snprintf(t_nx_status, sizeof(t_nx_status),
                 "nxlink: host %s, but the socket driver would not start "
                 "(0x%08x); nothing is streamed",
                 inet_ntoa(__nxlink_host), rc);
        return;
    }
    t_nx_driver_up = true;

    /* Neither stream redirected — see the note at the top of this
     * file. The socket is written to explicitly instead. */
    const int sock = nxlinkConnectToHost(false, false);
    if (sock < 0) {
        snprintf(t_nx_status, sizeof(t_nx_status),
                 "nxlink: host %s knows about this run but the connection "
                 "failed (errno %d) — nxlink has to be run with -s for "
                 "anything to be listening",
                 inet_ntoa(__nxlink_host), errno);
        socketExit();
        t_nx_driver_up = false;
        return;
    }

    /* A DEADLINE ON EVERY WRITE, because without one a log line can
     * block forever.
     *
     * nxlinkConnectToHost clears O_NONBLOCK before it returns — read
     * out of libnx's own nxlink_stdio.o, which sets the flag only for
     * the connect and then masks it off again — so the socket handed
     * back is blocking. A host that stops reading (nxlink killed, the
     * laptop asleep, the wifi gone) then stalls the console inside
     * write(), and a test that is stuck writing a log line looks
     * exactly like a test that is stuck doing the thing it measures.
     *
     * One second: far longer than any write on a working link, short
     * enough that a dead one costs a single stall. The timeout surfaces
     * as EAGAIN, which t_nx_raw treats as the stream being gone — it
     * closes it, says so in the file, and the run carries on without a
     * network. That happens at most once. */
    const struct timeval snd = { .tv_sec = 1, .tv_usec = 0 };
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd)) < 0) {
        /* Not fatal, and not hidden: the stream still works, it just
         * has no deadline, and the operator should know which of the
         * two they have. */
        snprintf(t_nx_status, sizeof(t_nx_status),
                 "nxlink: streaming to %s, but the send deadline could not "
                 "be set (errno %d) — a host that stops reading will stall "
                 "this run",
                 inet_ntoa(__nxlink_host), errno);
        t_nx_sock = sock;
        return;
    }

    t_nx_sock = sock;
    snprintf(t_nx_status, sizeof(t_nx_status),
             "nxlink: streaming this log live to %s, and the whole file "
             "again at the end",
             inet_ntoa(__nxlink_host));
}

/* The file, in full, down the same socket. This is the part the live
 * stream cannot produce: Mesa writes to stderr, stderr is the log, and
 * the driver's messages interleaved with the test's own lines are what
 * the last several hardware failures were diagnosed from. */
static void t_nx_replay(test_ctx *t)
{
    if (t_nx_sock < 0 || t->log == NULL)
        return;

    fflush(t->log);
    fflush(stderr);

    const long resume = ftell(t->log);
    if (resume < 0 || fseek(t->log, 0, SEEK_SET) != 0) {
        static const char failed[] =
            "---- the log could not be re-read, so this stream is the live "
            "lines only; the driver's own messages are on the SD card ----\n";
        t_nx_raw(failed, sizeof(failed) - 1);
        return;
    }

    static const char banner[] =
        "\n---- the complete log follows, driver messages included ----\n";
    t_nx_raw(banner, sizeof(banner) - 1);

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), t->log)) > 0)
        t_nx_raw(buf, n);

    static const char end[] = "---- end of log ----\n";
    t_nx_raw(end, sizeof(end) - 1);

    (void)fseek(t->log, resume, SEEK_SET);
}

static void t_nx_stop(void)
{
    if (t_nx_sock >= 0) {
        close(t_nx_sock);
        t_nx_sock = -1;
    }
    if (t_nx_driver_up) {
        socketExit();
        t_nx_driver_up = false;
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

    /* Before the first line is written, so the host sees the header and
     * the build id rather than joining halfway through. Whether it
     * worked is printed a few lines below, once there is a log to print
     * it into. */
    t_nx_start();

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

    char head[160];
    int head_len = snprintf(head, sizeof(head), "== %s ==\n", test_name);
    if (head_len > 0)
        t_sink(&t, head, (size_t)head_len < sizeof(head) ? (size_t)head_len
                                                         : sizeof(head) - 1);
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

    /* AFTER the build id, never before: package-horizon.sh and the
     * operator both read the stamp as the log's second line, and a run
     * that streams is otherwise indistinguishable from one that does
     * not — which matters, because a socket in the picture is a
     * difference in what the timings below were measured against. */
    t_note(&t, "%s", t_nx_status);

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
    if (tail_len > 0)
        t_sink(&t, tail, (size_t)tail_len < sizeof(tail) ? (size_t)tail_len
                                                         : sizeof(tail) - 1);

    printf("\nLog: %s\nPress + to exit.\n", path);
    /* With no console there is no screen to read that on, so the log —
     * which is the whole record for such a run — says it instead.
     * Found in review of PR #7.
     *
     * AND IT NEVER APPEARED. This wrote to `t.log` after fclose() had
     * already closed it, three lines above: the handle was closed but
     * not cleared, so the guard `t.log != NULL` was still true. The
     * line is absent from every log this project has collected — check
     * any of them, they end at RESULT — and writing to a closed stream
     * is undefined behaviour, not merely a lost line. The log is now
     * closed once, at the end, after everything that writes to it. */
    if (test_uses_display)
        t_note(&t, "the run is finished; press + to exit (there is no "
                   "console to show this)");

    /* The whole file to the host while it is still open, so an operator
     * watching over nxlink has the complete artefact — driver messages
     * included — without going near the SD card. */
    t_nx_replay(&t);

    if (t.log) {
        fclose(t.log);
        t.log = NULL;
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

    t_nx_stop();

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
