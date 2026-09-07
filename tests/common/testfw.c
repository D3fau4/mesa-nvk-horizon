/*
 * Test framework — console + sdmc logging main() for the .nro suites.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "testfw.h"

/* Generated on every build by scripts/gen-build-id.sh. */
#include "horizon_build_id.h"

#include <stdarg.h>
#include <stdlib.h>
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
 * suite that owns the display) and the log file. Call with the lock. */
static void t_sink(test_ctx *t, const char *line, size_t len)
{
    fwrite(line, 1, len, stdout);
    if (t->log) {
        fwrite(line, 1, len, t->log);
        fflush(t->log);
    }
}

/* Formats and emits one line. Call with the lock. */
__attribute__((format(printf, 3, 0)))
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

__attribute__((format(printf, 2, 3)))
static void t_emit(test_ctx *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    t_lock(t);
    t_vemit(t, "", fmt, ap);
    t_unlock(t);
    va_end(ap);
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
    if (cond) {
        t->pass++;
        t->case_pass++;
    } else {
        t->fail++;
        t->case_fail++;
    }
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

const char *t_case_id(const test_ctx *t)
{
    return t->case_id;
}

/* --- the environment, saved and put back around every case -----------
 *
 * THIS IS THE ISOLATION THAT GROUPING ACTUALLY NEEDED. Options in this
 * project are environment variables — HORIZON_GPU_LOG,
 * HORIZON_GPU_UNTRUSTED_SYNCPT_BASELINE, NVK_HORIZON_ZCULL,
 * MESA_VK_WSI_HORIZON_FORCE_COPY, MESA_SHADER_CACHE_DIR and the two
 * stats switches among them — and a test sets them with setenv() on
 * itself. As one .nro per test that was airtight: the process ended and
 * took the variable with it.
 *
 * Inside a suite it is not. vk_core/bringup raises horizon_gpu's log
 * level to INFO and never lowers it, so every later case in that .nro
 * would run verbose; worse, the same case turns on untrusted syncpoint
 * baselines when the platform cannot read a syncpoint, and a case that
 * inherited *that* would report fences it had not waited for. Neither
 * shows up as a failure. Both change what the suite measures.
 *
 * So the framework snapshots the environment before each case and puts
 * it back afterwards, and says which variables it had to undo — a case
 * that leaks one is worth knowing about even though the leak no longer
 * escapes it.
 *
 * WHAT IT DOES NOT UNDO, and the reason it is stated rather than
 * implied: an option a library reads once and caches in a static is not
 * restored by restoring the variable. Mesa's os_get_option_cached and
 * NVK's NVK_DEBUG parse are both of that shape. Every case here builds
 * its own VkInstance and VkDevice, so options read per-create do come
 * back; options latched on first use in the process do not, and a case
 * that needs one of those needs its own .nro. That is one of the
 * reasons vk_cache is not a case in vk_core.
 */
typedef struct env_snapshot {
    char **entries; /* "NAME=VALUE" copies */
    size_t count;
    bool valid;     /* false: the snapshot could not be taken */
} env_snapshot;

extern char **environ;

static void env_snapshot_free(env_snapshot *s)
{
    for (size_t i = 0; i < s->count; i++)
        free(s->entries[i]);
    free(s->entries);
    s->entries = NULL;
    s->count = 0;
    s->valid = false;
}

/* A snapshot that could not be taken is marked invalid rather than
 * silently treated as "the environment was empty" — restoring from that
 * would unset every variable the case inherited. Same rule as
 * t_log_scan: something that cannot look must not answer. */
static void env_snapshot_take(env_snapshot *s)
{
    s->entries = NULL;
    s->count = 0;
    s->valid = false;

    size_t n = 0;
    for (char **e = environ; e != NULL && *e != NULL; e++)
        n++;

    s->entries = calloc(n + 1, sizeof(*s->entries));
    if (s->entries == NULL)
        return;

    for (size_t i = 0; i < n; i++) {
        s->entries[i] = strdup(environ[i]);
        if (s->entries[i] == NULL) {
            s->count = i;
            env_snapshot_free(s);
            return;
        }
    }
    s->count = n;
    s->valid = true;
}

/* Copies the NAME half of "NAME=VALUE" into `out`. Returns false when
 * there is no '=' (which environ entries always have) or the name does
 * not fit, in which case the entry is left alone. */
static bool env_name_of(const char *entry, char *out, size_t out_size)
{
    const char *eq = strchr(entry, '=');
    if (eq == NULL)
        return false;
    const size_t len = (size_t)(eq - entry);
    if (len == 0 || len >= out_size)
        return false;
    memcpy(out, entry, len);
    out[len] = '\0';
    return true;
}

static bool env_snapshot_has(const env_snapshot *s, const char *name)
{
    const size_t len = strlen(name);
    for (size_t i = 0; i < s->count; i++) {
        if (strncmp(s->entries[i], name, len) == 0 &&
            s->entries[i][len] == '=')
            return true;
    }
    return false;
}

/* Appends to the report while it fits, and counts every entry either
 * way. THE REPORT IS ALLOWED TO TRUNCATE; THE WORK IS NOT — the first
 * version of this let the buffer's length decide the loop, so a case
 * that leaked enough variables to fill 256 bytes had the rest of them
 * left set, with a note that named only the ones that fitted. */
static void env_report(char *buf, size_t size, size_t *len,
                       unsigned *count, const char *mark, const char *name)
{
    (*count)++;
    if (*len >= size)
        return;
    const int n = snprintf(buf + *len, size - *len, "%s%s%s",
                           *len ? " " : "", mark, name);
    if (n > 0)
        *len += (size_t)n;
}

/* One pass collects at most this many names before unsetting them:
 * unsetenv() rewrites environ under the loop that is walking it, so the
 * collection and the removal cannot be interleaved. More than a pass's
 * worth is handled by running another pass, not by giving up. */
#define ENV_BATCH 16

/* Enough passes for any plausible leak, and a bound rather than `for
 * (;;)`: if unsetenv ever failed to remove a name, an unbounded loop
 * would hang the suite instead of reporting it. */
#define ENV_MAX_PASSES 64

/* Puts the environment back and reports what had to be undone. */
static void env_snapshot_restore(test_ctx *t, env_snapshot *s)
{
    if (!s->valid) {
        t_note(t, "the environment could not be snapshotted for this case, "
                  "so a variable it leaked is still set");
        env_snapshot_free(s);
        return;
    }

    char undone[256];
    size_t undone_len = 0;
    unsigned undone_count = 0;
    unsigned unnamed = 0;
    undone[0] = '\0';

    /* Names the case added. */
    unsigned pass = 0;
    for (; pass < ENV_MAX_PASSES; pass++) {
        char added[ENV_BATCH][64];
        size_t n = 0;
        unnamed = 0;
        for (char **e = environ; e != NULL && *e != NULL && n < ENV_BATCH;
             e++) {
            char name[64];
            if (!env_name_of(*e, name, sizeof(name))) {
                unnamed++;
                continue;
            }
            if (env_snapshot_has(s, name)) {
                /* Present before; a changed value is put back below. */
                continue;
            }
            snprintf(added[n++], sizeof(added[0]), "%s", name);
        }
        if (n == 0)
            break;
        for (size_t i = 0; i < n; i++) {
            unsetenv(added[i]);
            env_report(undone, sizeof(undone), &undone_len, &undone_count,
                       "-", added[i]);
        }
    }
    if (pass == ENV_MAX_PASSES)
        t_note(t, "the environment still has variables this case added "
                  "after %d removal passes; unsetenv is not removing them",
               ENV_MAX_PASSES);
    if (unnamed > 0)
        t_note(t, "%u environment entr%s could not be parsed as NAME=VALUE "
                  "and %s left alone", unnamed, unnamed == 1 ? "y" : "ies",
               unnamed == 1 ? "was" : "were");

    /* And the ones that were there before: put the old value back
     * wherever it moved, and restore one the case unset. */
    for (size_t i = 0; i < s->count; i++) {
        char name[64];
        if (!env_name_of(s->entries[i], name, sizeof(name)))
            continue;
        const char *want = s->entries[i] + strlen(name) + 1;
        const char *have = getenv(name);
        if (have != NULL && strcmp(have, want) == 0)
            continue;
        setenv(name, want, 1);
        env_report(undone, sizeof(undone), &undone_len, &undone_count,
                   have ? "=" : "+", name);
    }

    if (undone_count > 0)
        t_note(t, "the environment was put back after this case: %s%s "
                  "(%u in all; -added +restored =changed)",
               undone, undone_len >= sizeof(undone) ? " ..." : "",
               undone_count);

    env_snapshot_free(s);
}

/* --- the run ---------------------------------------------------------
 *
 * One case: banner, snapshot, run, verdict, restore. Everything a case
 * shares with the next one that this framework can see is reset here,
 * and what it cannot see is the case's own to tear down — a case owns
 * every device, channel, thread and file it creates, and leaves the
 * process as it found it. That is the contract grouping rests on, and
 * it is the same one every one of these tests already kept when it was
 * a .nro of its own: each of them creates its device at the top and
 * destroys it at the bottom.
 */
static bool run_one_case(test_ctx *t, const test_case *tc)
{
    snprintf(t->case_id, sizeof(t->case_id), "%s/%s", suite_name, tc->name);
    t->case_pass = 0;
    t->case_fail = 0;

    /* Where this case's own output starts, so t_log_scan does not find
     * an earlier case's driver message and call it evidence. */
    t_lock(t);
    if (t->log != NULL) {
        fflush(t->log);
        fflush(stderr);
        const long here = ftell(t->log);
        t->case_log_start = here < 0 ? 0 : here;
    }
    t_unlock(t);

    t_emit(t, "-- %s --", t->case_id);

    env_snapshot env;
    env_snapshot_take(&env);

    const uint64_t start_tick = armGetSystemTick();
    const int aborted = tc->run(t);
    const uint64_t elapsed_tick = armGetSystemTick() - start_tick;
    const uint64_t freq = armGetSystemTickFreq();
    const uint64_t elapsed_ms = freq ? (elapsed_tick * UINT64_C(1000)) / freq
                                     : 0;

    env_snapshot_restore(t, &env);

    const int total = t->case_pass + t->case_fail;
    const bool ok = (t->case_fail == 0 && aborted == 0);
    t_emit(t, "CASE: %s %s (%d/%d) in %llu ms%s", ok ? "PASS" : "FAIL",
           t->case_id, t->case_pass, total, (unsigned long long)elapsed_ms,
           aborted ? " [abandoned early]" : "");

    /* Back to naming the suite: anything emitted between cases belongs
     * to no case, and a scan started there searches the whole file. */
    snprintf(t->case_id, sizeof(t->case_id), "%s", suite_name);
    t->case_log_start = 0;
    return ok;
}

int main(void)
{
    if (!test_uses_display)
        consoleInit(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    test_ctx t = { .pass = 0, .fail = 0, .log = NULL };
    snprintf(t.case_id, sizeof(t.case_id), "%s", suite_name);

    /* Before the log is opened and long before any test starts a thread,
     * so every line this file can emit is already covered. A failure
     * here leaves out_lock_ready false, which costs the serialisation
     * and keeps the reporting: a test that cannot lock must still be
     * able to say what went wrong. */
    if (pthread_mutex_init(&t.out_lock, NULL) == 0)
        t.out_lock_ready = true;

    mkdir("sdmc:/horizon_gpu_tests", 0777);
    char path[128];
    snprintf(path, sizeof(path), "sdmc:/horizon_gpu_tests/%s.log", suite_name);
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

    t_emit(&t, "== %s (%u case%s) ==", suite_name, suite_case_count,
           suite_case_count == 1 ? "" : "s");
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
        fprintf(t.log, "  note this suite owns the display: no console was "
                       "started, and this file is the whole record\n");
        fflush(t.log);
    }

    unsigned cases_failed = 0;
    char failed_names[256];
    size_t failed_len = 0;
    failed_names[0] = '\0';

    for (unsigned i = 0; i < suite_case_count; i++) {
        if (run_one_case(&t, &suite_cases[i]))
            continue;
        cases_failed++;
        if (failed_len < sizeof(failed_names))
            failed_len += (size_t)snprintf(failed_names + failed_len,
                                           sizeof(failed_names) - failed_len,
                                           "%s%s", failed_len ? " " : "",
                                           suite_cases[i].name);
    }

    const int total = t.pass + t.fail;
    const char *verdict = cases_failed == 0 ? "PASS" : "FAIL";
    if (cases_failed == 0)
        t_emit(&t, "RESULT: %s (%d/%d) [%u/%u cases]", verdict, t.pass, total,
               suite_case_count, suite_case_count);
    else
        t_emit(&t, "RESULT: %s (%d/%d) [%u of %u cases failed: %s]", verdict,
               t.pass, total, cases_failed, suite_case_count, failed_names);

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
             * pressed +, on the one path display/console_handoff exists
             * to validate. A frame's worth of sleep costs nothing and is
             * not hiding a failure — there is nothing here to fail. */
            svcSleepThread(UINT64_C(16000000));
        } else {
            consoleUpdate(NULL);
        }
    }

    if (!test_uses_display)
        consoleExit(NULL);
    return cases_failed == 0 ? 0 : 1;
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
    /* From where this case's output began, not from the start of the
     * file: see test_ctx::case_log_start. */
    if (fseek(t->log, t->case_log_start, SEEK_SET) != 0) {
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

const char *t_build_id(void)
{
    return HORIZON_BUILD_ID;
}
