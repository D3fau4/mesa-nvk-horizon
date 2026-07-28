/*
 * Test 12 — Mesa's C11 threads shim on Horizon, and the processor count.
 *
 * WHICH IMPLEMENTATION THIS TESTS, AND WHY IT MATTERS.
 *
 * There are two C11 threads implementations on this platform and they
 * are not interchangeable:
 *
 *   newlib's        <threads.h>, libc.a(libc_a-threads.o)
 *   Mesa's          "c11/threads.h" + src/c11/impl/threads_posix.c
 *
 * Mesa selects its own: src/c11/impl/meson.build compiles
 * threads_posix.c unless with_c11_threads, and mesa/meson.build:1619
 * sets that only for Android. Measured on this tree: build.ninja
 * contains zero occurrences of HAVE_THRD_CREATE.
 *
 * That choice is deliberate and is the answer to milestone item 4.
 * newlib's native C11 threads are not usable for the timed operations:
 *
 *   $ aarch64-none-elf-objdump -d --disassemble=mtx_timedlock libc_a-threads.o
 *   0000000000000000 <mtx_timedlock>:
 *      0:  52800040   mov w0, #0x2     // thrd_error
 *      4:  d65f03c0   ret
 *
 * — an unconditional failure, never attempting the lock — and
 * mtx_init(&m, mtx_timed) returns thrd_error too (it tests bit 2 of the
 * type and branches straight to #2). Mesa's version really implements
 * the timeout, by polling mtx_trylock, which is the path
 * mesa-patches/0003 turns on where pthread_mutex_timedlock is absent.
 *
 * The two headers also disagree on the enumeration itself — newlib has
 * thrd_success = 4, thrd_timedout = 5; Mesa has thrd_success = 0,
 * thrd_timedout = 1 — so the choice cannot be hedged. This file includes
 * Mesa's header and links Mesa's archive, and every constant below is
 * Mesa's.
 *
 * WHAT IS ACTUALLY UNDER TEST. Nothing in Mesa's C11 shim has ever run
 * on a console. The polling mtx_timedlock is the sharpest case: a
 * timeout that returns immediately, or one that never returns, both look
 * like "thrd_timedout" to a test that does not measure the time. So
 * every check on a call that must EXPIRE — mtx_timedlock and
 * cnd_timedwait against their deadlines — is bounded from BOTH sides,
 * and the clock used to bound it is armGetSystemTick() — the ARM system
 * counter, which is not the clock the implementation uses (clock_gettime
 * through c23_timespec_get). Measuring a clock with itself proves
 * nothing.
 *
 * A call that must NOT wait is a different question and is bounded above
 * only: an uncontended mtx_timedlock, a signalled condvar and a
 * broadcast one are correct when they return at once, so there is no
 * lower bound to assert. Those are the PROMPT_MAX_MS checks, and the
 * claim they make is that much weaker deliberately.
 *
 * NEVER WAIT WITHOUT A BOUND. The upper bound above is worth nothing if
 * reaching it means blocking first: a mtx_timedlock or cnd_timedwait
 * that never returns would leave thrd_join or the wait itself blocked
 * for as long as the console is on, and the verdict would never be
 * written. So no wait on a timed call here is unbounded. The worker
 * doing mtx_timedlock is watched by the main thread through an atomic
 * flag; the main thread's own cnd_timedwait is watched by a thread that
 * signals the condvar if the deadline is ignored; and the condvar
 * wakeups are awaited on atomic counters rather than in thrd_join, with
 * a broadcast as recovery. In every case the failing check is recorded
 * BEFORE the call that could still block — testfw fflushes every line to
 * sdmc, so a run that hangs anyway leaves a log that says where and why.
 *
 * One wait is left unbounded, and it is named rather than hidden: the
 * main thread's mtx_lock inside cnd_set_predicate. It blocks until a
 * worker releases the mutex, which cnd_wait does atomically as it
 * suspends — so a cnd_wait that suspends WITHOUT releasing the mutex
 * would hang there. mtx_lock has no bounded form other than
 * mtx_timedlock, and building this file's recovery path out of the very
 * call the section above it is measuring would be circular. The line
 * before it is a t_check, so the log still says how far the run got.
 *
 * A SECTION THAT CANNOT SET ITSELF UP DOES NOT RUN. Each section is its
 * own function and returns as soon as its mutex, condvar or key fails to
 * initialise. Carrying on would pass an invalid object to a lock or a
 * worker, and turn one honest failure into a hang or a crash that says
 * nothing about what broke.
 *
 * The processor-count section is the hardware evidence for
 * mesa-patches/0012 and for compat/sysconf.c's _SC_NPROCESSORS_*.
 *
 * THREAD SAFETY OF THE TEST ITSELF. t_check() and t_note() write the
 * shared test_ctx and a FILE*, so they are called from the main thread
 * only. Worker threads set plain counters and flags, which are read
 * after the join — or the atomic flag — that orders them. A field a
 * worker may write while NOT holding the section's mutex (because the
 * lock is what failed) is an atomic_int instead, or several workers
 * failing at once would lose counts.
 *
 * Every mtx_* and cnd_* return is checked, in the workers as well as on
 * the main thread (CLAUDE.md). A worker that carried on after a failed
 * mtx_lock would hand cnd_wait an unlocked mutex and race the counters
 * it shares, turning one honest failure into several that point
 * elsewhere.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <switch.h>

/* Mesa's, not newlib's. See the note above. */
#include "c11/threads.h"
#include "util/os_misc.h"
#include "util/u_cpu_detect.h"
#include "util/u_thread.h"

#include "common/testfw.h"

const char *const test_name = "t_threads";

/* How many worker threads the shared-counter and broadcast sections
 * use. Four is one per Cortex-A57 on a Tegra X1, so the counter section
 * really can run on more than one core at once; the test does not
 * require that many to be available. */
#define WORKERS 4

/* Increments per worker in the shared-counter section. Large enough that
 * a lost update is overwhelmingly likely if the mutex does not exclude,
 * small enough to stay well inside a frame's worth of time. */
#define INCREMENTS 20000

/* The timeout every expiry check asks for, in milliseconds. Long enough
 * that scheduling noise cannot account for it, short enough to keep the
 * test quick. */
#define TIMEOUT_MS 200

/* Both-sided bounds on a TIMEOUT_MS wait, in milliseconds. The lower
 * bound is what catches a timed call that gives up immediately — the
 * failure a polling implementation makes when its clock comparison is
 * inverted or its clock does not advance. The upper bound catches one
 * that overshoots, e.g. by sleeping a whole scheduling quantum per poll.
 */
#define TIMEOUT_MIN_MS (TIMEOUT_MS - (TIMEOUT_MS / 4)) /* 150 */
#define TIMEOUT_MAX_MS (TIMEOUT_MS * 4)                /* 800  */

/* A call that must NOT wait (an uncontended lock, a signalled condvar)
 * still has to be given room for scheduling. */
#define PROMPT_MAX_MS 100

/* How long any watchdog waits before deciding a timed call is not going
 * to return by itself. Comfortably above TIMEOUT_MAX_MS, so an
 * implementation that is merely slow is still reported as slow — a FAIL
 * on the upper bound — instead of being cut off and reported as hung. */
#define WATCHDOG_MS (TIMEOUT_MAX_MS + 1200) /* 2000 */

/* How often a watchdog looks. svcSleepThread rather than a spin: the
 * thread being watched may want this core. */
#define WATCHDOG_POLL_NS 1000000LL /* 1 ms */

static uint64_t
ms_since(uint64_t start_tick)
{
    return armTicksToNs(armGetSystemTick() - start_tick) / 1000000u;
}

/* Absolute deadline in the base Mesa's shim compares against.
 *
 * mtx_timedlock() and cnd_timedwait() both take an absolute time.
 * threads_posix.c reads "now" with timespec_get(&now, TIME_UTC) in the
 * polling path, and pthread_cond_timedwait uses CLOCK_REALTIME, so
 * TIME_UTC is the base a caller must express the deadline in. Using
 * anything else is a caller bug that would look exactly like an
 * implementation bug, which is why it is spelled out here.
 *
 * Returns false when timespec_get fails. C11 leaves *ts untouched in
 * that case, so the arithmetic below would run on uninitialised stack
 * and hand a meaningless absolute time to a timed call — which would
 * expire at once, or not for years, and either way would be reported as
 * the implementation's fault. Whether TIME_UTC works here at run time is
 * one of the things this project is still measuring (t_ostime), so it is
 * checked rather than assumed. */
static bool
deadline_in_ms(struct timespec *ts, long ms)
{
    memset(ts, 0, sizeof(*ts));
    if (timespec_get(ts, TIME_UTC) != TIME_UTC)
        return false;

    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec += 1;
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* thrd_create / thrd_join                                           */
/* ---------------------------------------------------------------- */

#define JOIN_MAGIC 0x5eed

static int
returning_worker(void *arg)
{
    return (int)(intptr_t)arg;
}

/* Returns false when threads do not work at all: every other section
 * needs them, so there is nothing left to measure. */
static bool
section_threads(test_ctx *t)
{
    thrd_t thread;
    int rc, res;

    rc = thrd_create(&thread, returning_worker, (void *)(intptr_t)JOIN_MAGIC);
    if (!t_check(t, rc == thrd_success, "thrd_create returned thrd_success (%d)",
                 rc))
        return false;

    res = 0;
    rc = thrd_join(thread, &res);
    t_check(t, rc == thrd_success, "thrd_join returned thrd_success (%d)", rc);
    t_check(t, res == JOIN_MAGIC,
            "thrd_join reported the thread's return value (0x%x)", res);

    /* u_thread_create is the wrapper every Mesa consumer actually calls
     * (util/u_queue.c does), and mesa-patches/0011 changed which of its
     * two branches this platform compiles. Exercising it here is what
     * makes that patch a measurement rather than a compile. */
    rc = u_thread_create(&thread, returning_worker, (void *)(intptr_t)JOIN_MAGIC);
    t_check(t, rc == thrd_success, "u_thread_create returned thrd_success (%d)",
            rc);
    if (rc == thrd_success) {
        res = 0;
        thrd_join(thread, &res);
        t_check(t, res == JOIN_MAGIC,
                "u_thread_create's thread ran and returned 0x%x", res);
    }

    return true;
}

/* ---------------------------------------------------------------- */
/* call_once                                                         */
/* ---------------------------------------------------------------- */

static once_flag once = ONCE_FLAG_INIT;

/* Atomic, and that is the whole point of the check. The defect this
 * section hunts is a call_once that runs the body in more than one
 * thread; with a plain int, two concurrent increments are a data race
 * that can lose one, leave the total at 1, and report the broken
 * implementation as correct. The counter that detects concurrent
 * execution must itself be defined under concurrent execution. */
static atomic_int once_calls;

static void
once_body(void)
{
    atomic_fetch_add(&once_calls, 1);
}

static int
once_worker(void *arg)
{
    (void)arg;
    call_once(&once, once_body);
    return 0;
}

static void
section_call_once(test_ctx *t)
{
    thrd_t threads[WORKERS];
    int created = 0;

    for (int i = 0; i < WORKERS; i++) {
        if (thrd_create(&threads[i], once_worker, NULL) != thrd_success)
            break;
        created++;
    }
    for (int i = 0; i < created; i++)
        thrd_join(threads[i], NULL);

    if (!t_check(t, created == WORKERS, "created %d call_once workers (%d)",
                 WORKERS, created))
        return;

    t_check(t, atomic_load(&once_calls) == 1,
            "call_once ran the body exactly once across %d threads (%d)",
            WORKERS, atomic_load(&once_calls));
}

/* ---------------------------------------------------------------- */
/* shared counter under a mutex                                      */
/* ---------------------------------------------------------------- */

typedef struct counter_ctx {
    mtx_t *mtx;
    uint64_t *counter;
    /* How many increments this worker actually performed. Not assumed to
     * be INCREMENTS: see the expected total in the section below. */
    uint64_t increments;
    int lock_failures;
    int unlock_failures;
} counter_ctx;

static int
counter_worker(void *arg)
{
    counter_ctx *c = arg;

    for (int i = 0; i < INCREMENTS; i++) {
        if (mtx_lock(c->mtx) != thrd_success) {
            c->lock_failures++;
            continue;
        }
        /* Deliberately a read-modify-write through memory, not an
         * atomic: the mutex is what is being tested, so the increment
         * must be one the mutex has to protect. */
        *c->counter = *c->counter + 1;
        c->increments++;
        if (mtx_unlock(c->mtx) != thrd_success)
            c->unlock_failures++;
    }
    return 0;
}

static void
section_mutex_counter(test_ctx *t)
{
    thrd_t threads[WORKERS];
    counter_ctx ctxs[WORKERS];
    uint64_t counter = 0, performed = 0;
    mtx_t mtx;
    int created = 0;
    int lock_failures = 0, unlock_failures = 0;

    if (!t_check(t, mtx_init(&mtx, mtx_plain) == thrd_success,
                 "mtx_init(mtx_plain) succeeded")) {
        t_note(t, "without a mutex there is nothing to exclude with; the "
                  "shared-counter checks are NOT attempted");
        return;
    }

    memset(ctxs, 0, sizeof(ctxs));
    for (int i = 0; i < WORKERS; i++) {
        ctxs[i].mtx = &mtx;
        ctxs[i].counter = &counter;
        if (thrd_create(&threads[i], counter_worker, &ctxs[i]) != thrd_success)
            break;
        created++;
    }
    for (int i = 0; i < created; i++)
        thrd_join(threads[i], NULL);

    if (!t_check(t, created == WORKERS, "created %d counter workers (%d)",
                 WORKERS, created)) {
        mtx_destroy(&mtx);
        return;
    }

    for (int i = 0; i < created; i++) {
        lock_failures += ctxs[i].lock_failures;
        unlock_failures += ctxs[i].unlock_failures;
        performed += ctxs[i].increments;
    }
    t_check(t, lock_failures == 0, "no mtx_lock failed (%d)", lock_failures);
    t_check(t, unlock_failures == 0, "no mtx_unlock failed (%d)",
            unlock_failures);
    t_note(t, "shared counter: %d threads x %d increments, %llu performed",
           created, INCREMENTS, (unsigned long long)performed);
    /* Against what the workers actually performed, not against
     * created * INCREMENTS. A failed mtx_lock skips its increment, and
     * it is already reported by the check above; measuring the counter
     * against the nominal total would report that same defect a second
     * time, as a lost update it is not. With no lock failure the two
     * numbers are identical, so nothing is weakened — this stays a check
     * that every increment the mutex admitted survived. */
    t_check(t, counter == performed,
            "counter is exactly %llu, no update lost (got %llu)",
            (unsigned long long)performed, (unsigned long long)counter);

    mtx_destroy(&mtx);
}

/* ---------------------------------------------------------------- */
/* mtx_timedlock — the check the polling path can lie about          */
/* ---------------------------------------------------------------- */

typedef struct timedlock_ctx {
    mtx_t *mtx;
    bool deadline_ok;    /* could a TIME_UTC deadline be built at all */
    int result;          /* what mtx_timedlock returned */
    uint64_t elapsed_ms; /* measured by the ARM system counter */
    bool unlock_failed;  /* the recovery unlock below, if it had to run */

    /* Set last, after every field above. The main thread waits on this
     * with a bound instead of joining, so that a mtx_timedlock which
     * never returns is reported rather than waited on. */
    atomic_int done;
} timedlock_ctx;

static int
timedlock_worker(void *arg)
{
    timedlock_ctx *c = arg;
    struct timespec deadline;
    uint64_t start;

    c->deadline_ok = deadline_in_ms(&deadline, TIMEOUT_MS);
    if (c->deadline_ok) {
        start = armGetSystemTick();
        c->result = mtx_timedlock(c->mtx, &deadline);
        c->elapsed_ms = ms_since(start);

        /* If it unexpectedly succeeded, do not leave the mutex held: the
         * main thread unlocks its own hold and would then be unlocking a
         * mutex owned by nobody. Checked, because the section's claim
         * that the mutex is not left held either way rests on it. */
        if (c->result == thrd_success && mtx_unlock(c->mtx) != thrd_success)
            c->unlock_failed = true;
    }

    atomic_store(&c->done, 1);
    return 0;
}

static void
section_mutex_timed(test_ctx *t)
{
    mtx_t mtx;
    thrd_t worker;
    timedlock_ctx tl;
    bool held = false, finished = false;
    int rc;

    /* mtx_timed is what mtx_timedlock is documented to require, and it
     * is the type newlib's own mtx_init rejects outright. Mesa's shim
     * maps every type onto one pthread mutex, so this must succeed —
     * and a failure here would explain a later timedlock failure. */
    if (!t_check(t, mtx_init(&mtx, mtx_timed) == thrd_success,
                 "mtx_init(mtx_timed) succeeded")) {
        t_note(t, "mtx_trylock and mtx_timedlock need that mutex; NOT "
                  "attempted");
        return;
    }

    /* ---- mtx_trylock: what the polling timedlock is built on ----- */

    if (!t_check(t, mtx_lock(&mtx) == thrd_success,
                 "main thread took the timed mutex")) {
        t_note(t, "the contended checks below need it held; NOT attempted");
        mtx_destroy(&mtx);
        return;
    }
    held = true;

    t_check(t, mtx_trylock(&mtx) == thrd_busy,
            "mtx_trylock on a held mutex reports thrd_busy");

    /* ---- mtx_timedlock, timeout that must EXPIRE ---------------- */

    memset(&tl, 0, sizeof(tl));
    tl.mtx = &mtx;
    tl.result = -1;

    if (t_check(t, thrd_create(&worker, timedlock_worker, &tl) == thrd_success,
                "created the mtx_timedlock worker")) {
        uint64_t wd_start = armGetSystemTick();

        /* Not thrd_join, and not yet. The failure this section exists to
         * catch is a mtx_timedlock that never gives up; joining such a
         * worker would block here until the console is switched off,
         * with the FAIL never written. */
        finished = atomic_load(&tl.done) != 0;
        while (!finished && ms_since(wd_start) < WATCHDOG_MS) {
            svcSleepThread(WATCHDOG_POLL_NS);
            finished = atomic_load(&tl.done) != 0;
        }

        /* Recorded before anything that could still block. */
        t_check(t, finished,
                "mtx_timedlock returned on its own within %d ms", WATCHDOG_MS);

        /* Released before the join, not after: if the worker is still
         * inside a mtx_timedlock that is ignoring its deadline, this is
         * what lets it acquire, finish, and be joined. It unlocks again
         * on that path, so the mutex is not left held either way. */
        t_check(t, mtx_unlock(&mtx) == thrd_success,
                "main thread released the timed mutex");
        held = false;

        thrd_join(worker, NULL);

        if (!t_check(t, tl.deadline_ok,
                     "the worker built a TIME_UTC deadline")) {
            t_note(t, "timespec_get(TIME_UTC) failed, so mtx_timedlock was "
                      "never called; see t_ostime for the clock itself");
        } else {
            t_note(t, "mtx_timedlock(%d ms) returned %d after %llu ms "
                      "(thrd_success=%d thrd_timedout=%d thrd_error=%d)",
                   TIMEOUT_MS, tl.result, (unsigned long long)tl.elapsed_ms,
                   thrd_success, thrd_timedout, thrd_error);

            if (finished) {
                t_check(t, tl.result == thrd_timedout,
                        "mtx_timedlock on a held mutex returned thrd_timedout "
                        "(%d)",
                        tl.result);
                /* Only on the path that ran it: on the expected one the
                 * worker never took the mutex, so there is nothing to
                 * assert about releasing it. */
                if (tl.result == thrd_success)
                    t_check(t, !tl.unlock_failed,
                            "the worker released the mutex it unexpectedly "
                            "acquired");
                /* The two bounds are the point of this section. Either
                 * one alone passes for an implementation that is wrong
                 * in the other direction. */
                t_check(t, tl.elapsed_ms >= TIMEOUT_MIN_MS,
                        "it waited at least %d ms before giving up (%llu ms)",
                        TIMEOUT_MIN_MS, (unsigned long long)tl.elapsed_ms);
                t_check(t, tl.elapsed_ms <= TIMEOUT_MAX_MS,
                        "it gave up within %d ms (%llu ms)", TIMEOUT_MAX_MS,
                        (unsigned long long)tl.elapsed_ms);
            } else {
                /* It only returned once the mutex was released, so the
                 * figures describe the release, not the deadline.
                 * Checking them against TIMEOUT_MS would report a second
                 * failure for the same defect. */
                t_note(t, "it returned only after the mutex was released, so "
                          "the deadline was ignored; the %d ms bounds are not "
                          "applied to that measurement",
                       TIMEOUT_MS);
            }
        }
    }

    if (held)
        t_check(t, mtx_unlock(&mtx) == thrd_success,
                "main thread released the timed mutex");

    /* ---- mtx_timedlock on a FREE mutex: must not wait ------------ */

    {
        struct timespec deadline;
        uint64_t start, elapsed;

        if (!t_check(t, deadline_in_ms(&deadline, TIMEOUT_MS),
                     "built a TIME_UTC deadline for the free-mutex case")) {
            mtx_destroy(&mtx);
            return;
        }

        start = armGetSystemTick();
        rc = mtx_timedlock(&mtx, &deadline);
        elapsed = ms_since(start);

        t_check(t, rc == thrd_success,
                "mtx_timedlock on a free mutex returned thrd_success (%d)", rc);
        t_check(t, elapsed <= PROMPT_MAX_MS,
                "and took %llu ms, under the %d ms bound",
                (unsigned long long)elapsed, PROMPT_MAX_MS);
        if (rc == thrd_success)
            mtx_unlock(&mtx);
    }

    mtx_destroy(&mtx);
}

/* ---------------------------------------------------------------- */
/* condition variables                                               */
/* ---------------------------------------------------------------- */

typedef struct cnd_ctx {
    mtx_t mtx;
    cnd_t cnd;
    int predicate;   /* guarded by mtx */
    int wait_failed; /* guarded by mtx */

    /* Read by the main thread while the workers are running, so atomic
     * rather than guarded: `ready` is how it knows a worker is inside
     * cnd_wait and not merely on its way there, and `woken` is how it
     * bounds its own wait instead of blocking in thrd_join. */
    atomic_int ready;
    atomic_int woken;

    /* mtx_lock/mtx_unlock failures inside a worker. Atomic and not
     * guarded by mtx, because the case it records is the one where the
     * worker does not hold it — several workers can be failing at
     * once. */
    atomic_int mtx_failures;
} cnd_ctx;

static int
cnd_worker(void *arg)
{
    cnd_ctx *c = arg;

    /* Returning instead of carrying on: without the mutex the loop below
     * would read the predicate unguarded, hand cnd_wait a mutex it does
     * not own, and race c->woken — one failure reported as three. */
    if (mtx_lock(&c->mtx) != thrd_success) {
        atomic_fetch_add(&c->mtx_failures, 1);
        return 0;
    }
    /* Published while holding the mutex, immediately before the wait.
     * cnd_wait releases the mutex atomically, so a main thread that
     * observes this count and then acquires the mutex knows this worker
     * is inside the wait rather than about to enter it. That ordering is
     * what makes the signal a test of cnd_signal instead of a test of
     * the predicate. */
    atomic_fetch_add(&c->ready, 1);
    while (!c->predicate) {
        if (cnd_wait(&c->cnd, &c->mtx) != thrd_success) {
            c->wait_failed++;
            break;
        }
    }
    atomic_fetch_add(&c->woken, 1);
    if (mtx_unlock(&c->mtx) != thrd_success)
        atomic_fetch_add(&c->mtx_failures, 1);
    return 0;
}

/* Waits for *v to reach n, or for WATCHDOG_MS to pass. Returns what it
 * last observed, so the caller reports a number rather than a boolean. */
static int
await_count(atomic_int *v, int n)
{
    uint64_t start = armGetSystemTick();
    int seen = atomic_load(v);

    while (seen < n && ms_since(start) < WATCHDOG_MS) {
        svcSleepThread(WATCHDOG_POLL_NS);
        seen = atomic_load(v);
    }
    return seen;
}

/* Sets the predicate the workers loop on, and reports whether the mutex
 * calls around it succeeded.
 *
 * The store happens either way, and that is deliberate. Waiters are
 * released by predicate-then-broadcast; if the main thread could not
 * take the mutex, storing it anyway is what still lets the joins below
 * terminate, and leaving it clear would produce exactly the unbounded
 * wait this file exists not to have. On that path the store races the
 * workers' reads — on a platform whose plain mutex has already failed,
 * which the caller reports. */
static bool
cnd_set_predicate(cnd_ctx *c)
{
    bool ok = mtx_lock(&c->mtx) == thrd_success;

    c->predicate = 1;

    if (ok && mtx_unlock(&c->mtx) != thrd_success)
        ok = false;
    return ok;
}

/* The main thread is the one that waits in the cnd_timedwait check, so
 * it cannot watch itself. This thread does: if the deadline passes and
 * the waiter is still inside the call, it sets the predicate and
 * broadcasts, which frees the waiter so the test can report an ignored
 * deadline instead of never ending.
 *
 * Exactly one of the two ends the wait, decided by a compare-exchange on
 * `outcome` rather than by two separate flags. With two flags the
 * watchdog could observe "not finished", be preempted, and broadcast
 * after the waiter had already returned on its own — reporting a failure
 * that did not happen. Here the loser of the exchange does nothing. */
#define CND_OUTCOME_PENDING 0
#define CND_OUTCOME_WAITER  1 /* cnd_timedwait returned by itself */
#define CND_OUTCOME_FIRED   2 /* the watchdog had to signal it */

typedef struct cnd_watchdog_ctx {
    cnd_ctx *cc;
    atomic_int outcome;
} cnd_watchdog_ctx;

/* Returns true if this caller is the one that claimed the outcome. */
static bool
cnd_claim_outcome(cnd_watchdog_ctx *w, int outcome)
{
    int expected = CND_OUTCOME_PENDING;

    return atomic_compare_exchange_strong(&w->outcome, &expected, outcome);
}

static int
cnd_watchdog_worker(void *arg)
{
    cnd_watchdog_ctx *w = arg;
    uint64_t start = armGetSystemTick();

    while (atomic_load(&w->outcome) == CND_OUTCOME_PENDING &&
           ms_since(start) < WATCHDOG_MS)
        svcSleepThread(WATCHDOG_POLL_NS);

    if (!cnd_claim_outcome(w, CND_OUTCOME_FIRED))
        return 0; /* the waiter got there first; nothing to rescue */

    /* Under the mutex, so this is ordered against the waiter
     * reacquiring it on the way out of cnd_timedwait. A failure here is
     * recorded like any other, and the broadcast still goes out: it is
     * what releases the waiter. */
    if (!cnd_set_predicate(w->cc))
        atomic_fetch_add(&w->cc->mtx_failures, 1);
    cnd_broadcast(&w->cc->cnd);
    return 0;
}

static void
section_condvar(test_ctx *t)
{
    thrd_t threads[WORKERS];
    cnd_ctx cc;
    int created = 0;
    int rc;

    memset(&cc, 0, sizeof(cc));

    if (!t_check(t, mtx_init(&cc.mtx, mtx_plain) == thrd_success,
                 "mtx_init for the condvar section succeeded")) {
        t_note(t, "a condvar needs a mutex; the condvar checks are NOT "
                  "attempted");
        return;
    }
    if (!t_check(t, cnd_init(&cc.cnd) == thrd_success, "cnd_init succeeded")) {
        t_note(t, "the condvar checks are NOT attempted");
        mtx_destroy(&cc.mtx);
        return;
    }

    /* ---- cnd_wait / cnd_signal ---------------------------------- */

    if (t_check(t, thrd_create(&threads[0], cnd_worker, &cc) == thrd_success,
                "created the cnd_wait worker")) {
        uint64_t start, elapsed;
        int seen;

        /* Wait until the worker is inside cnd_wait before signalling.
         * Setting the predicate first is still right — it is what stops
         * a lost wakeup — but on its own it also lets the check pass
         * without cnd_signal doing anything: a worker that has not
         * reached the wait yet reads predicate == 1, leaves, and "the
         * waiter woke" is true against a cnd_signal that is a no-op
         * returning thrd_success. */
        seen = await_count(&cc.ready, 1);
        t_check(t, seen == 1,
                "the cnd_wait worker reached the wait within %d ms (%d of 1)",
                WATCHDOG_MS, seen);

        start = armGetSystemTick();
        t_check(t, cnd_set_predicate(&cc),
                "main thread set the predicate under the mutex");
        t_check(t, cnd_signal(&cc.cnd) == thrd_success, "cnd_signal succeeded");

        /* Bounded, and not thrd_join: a cnd_signal that wakes nobody
         * leaves the worker in cnd_wait for as long as the console is
         * on, and joining it here would take the verdict with it. */
        seen = await_count(&cc.woken, 1);
        elapsed = ms_since(start);

        /* Recorded before anything that could still block. Only this
         * check is made about the wakeup — a second one after the join
         * would report the same defect twice. */
        if (!t_check(t, seen == 1,
                     "cnd_signal woke the waiter within %d ms (%d of 1)",
                     WATCHDOG_MS, seen)) {
            t_note(t, "broadcasting as recovery so the join below can "
                      "return; if the log stops here, cnd_broadcast did not "
                      "wake it either and nothing else can");
            cnd_broadcast(&cc.cnd);
        }

        thrd_join(threads[0], NULL);

        t_check(t, cc.wait_failed == 0, "cnd_wait did not fail (%d)",
                cc.wait_failed);
        t_check(t, atomic_load(&cc.mtx_failures) == 0,
                "no mutex call failed in the cnd_wait worker (%d)",
                atomic_load(&cc.mtx_failures));
        t_check(t, elapsed <= PROMPT_MAX_MS,
                "signal to wake took %llu ms, under the %d ms bound",
                (unsigned long long)elapsed, PROMPT_MAX_MS);
    }

    /* ---- cnd_broadcast: every waiter wakes ---------------------- */

    cc.predicate = 0;
    cc.wait_failed = 0;
    atomic_store(&cc.ready, 0);
    atomic_store(&cc.woken, 0);
    atomic_store(&cc.mtx_failures, 0);

    for (int i = 0; i < WORKERS; i++) {
        if (thrd_create(&threads[i], cnd_worker, &cc) != thrd_success)
            break;
        created++;
    }
    t_check(t, created == WORKERS, "created %d cnd_broadcast workers (%d)",
            WORKERS, created);

    if (created > 0) {
        uint64_t start, elapsed;
        int seen;

        /* Same handshake as above, and it matters more here: with some
         * waiters not yet in cnd_wait, a broadcast that reaches only one
         * of them still lets every worker leave on the predicate, and
         * "every broadcast waiter woke" passes for an implementation
         * that woke one. */
        seen = await_count(&cc.ready, created);
        t_check(t, seen == created,
                "every broadcast worker reached the wait within %d ms "
                "(%d of %d)",
                WATCHDOG_MS, seen, created);

        start = armGetSystemTick();
        t_check(t, cnd_set_predicate(&cc),
                "main thread set the predicate under the mutex");
        t_check(t, cnd_broadcast(&cc.cnd) == thrd_success,
                "cnd_broadcast succeeded");

        seen = await_count(&cc.woken, created);
        elapsed = ms_since(start);

        if (!t_check(t, seen == created,
                     "cnd_broadcast woke every waiter within %d ms "
                     "(%d of %d)",
                     WATCHDOG_MS, seen, created)) {
            t_note(t, "broadcasting again as recovery so the joins below "
                      "can return; if the log stops here, they cannot");
            cnd_broadcast(&cc.cnd);
        }

        for (int i = 0; i < created; i++)
            thrd_join(threads[i], NULL);

        t_check(t, cc.wait_failed == 0, "no cnd_wait failed (%d)",
                cc.wait_failed);
        t_check(t, atomic_load(&cc.mtx_failures) == 0,
                "no mutex call failed in a cnd_broadcast worker (%d)",
                atomic_load(&cc.mtx_failures));
        t_check(t, elapsed <= PROMPT_MAX_MS,
                "broadcast to all woken took %llu ms, under the %d ms bound",
                (unsigned long long)elapsed, PROMPT_MAX_MS);
    }

    /* ---- cnd_timedwait, timeout that must EXPIRE ---------------- */

    /* This one is not academic: src/vulkan/runtime/vk_sync_timeline.c
     * waits on a timeline semaphore with cnd_timedwait, so Phase 4
     * depends on the timeout actually expiring rather than hanging or
     * returning at once. Unlike mtx_timedlock this goes straight to
     * pthread_cond_timedwait, so it measures newlib/libnx rather than
     * Mesa's polling loop — a different question with the same shape. */
    {
        struct timespec deadline;
        cnd_watchdog_ctx wd;
        thrd_t wd_thread;
        uint64_t start, elapsed;

        memset(&wd, 0, sizeof(wd));
        wd.cc = &cc;
        atomic_store(&cc.mtx_failures, 0);

        /* The wait below is the only place in this file where the main
         * thread itself can block indefinitely, so it does not happen
         * without something able to end it. */
        if (!t_check(t, thrd_create(&wd_thread, cnd_watchdog_worker, &wd) ==
                            thrd_success,
                     "created the cnd_timedwait watchdog")) {
            t_note(t, "without it an ignored deadline would hang the test "
                      "rather than fail it; cnd_timedwait NOT called");
            goto out;
        }

        /* cnd_timedwait requires this thread to hold the mutex; calling
         * it without is undefined, so a failure here ends the check
         * instead of being reported and stepped over. Nothing is
         * unlocked on this path — the lock is what failed. */
        if (!t_check(t, mtx_lock(&cc.mtx) == thrd_success,
                     "main thread took the mutex for cnd_timedwait")) {
            t_note(t, "cnd_timedwait needs it held; NOT called");
            cnd_claim_outcome(&wd, CND_OUTCOME_WAITER); /* stand the watchdog down */
            thrd_join(wd_thread, NULL);
            goto out;
        }

        if (!deadline_in_ms(&deadline, TIMEOUT_MS)) {
            t_check(t, false, "built a TIME_UTC deadline for cnd_timedwait");
            t_note(t, "cnd_timedwait was NOT called");
            cnd_claim_outcome(&wd, CND_OUTCOME_WAITER);
            /* Before the join: a watchdog that won the exchange first is
             * blocked on this mutex. */
            t_check(t, mtx_unlock(&cc.mtx) == thrd_success,
                    "main thread released the mutex");
            thrd_join(wd_thread, NULL);
            goto out;
        }

        start = armGetSystemTick();
        rc = cnd_timedwait(&cc.cnd, &cc.mtx, &deadline);
        elapsed = ms_since(start);
        cnd_claim_outcome(&wd, CND_OUTCOME_WAITER);
        t_check(t, mtx_unlock(&cc.mtx) == thrd_success,
                "main thread released the mutex cnd_timedwait reacquired");

        thrd_join(wd_thread, NULL);

        t_note(t, "cnd_timedwait(%d ms) returned %d after %llu ms", TIMEOUT_MS,
               rc, (unsigned long long)elapsed);

        if (t_check(t, atomic_load(&wd.outcome) == CND_OUTCOME_WAITER,
                    "cnd_timedwait ended on its own, without the %d ms "
                    "watchdog having to signal it",
                    WATCHDOG_MS)) {
            t_check(t, rc == thrd_timedout,
                    "cnd_timedwait with nobody signalling returned "
                    "thrd_timedout (%d)",
                    rc);
            t_check(t, elapsed >= TIMEOUT_MIN_MS,
                    "cnd_timedwait waited at least %d ms (%llu ms)",
                    TIMEOUT_MIN_MS, (unsigned long long)elapsed);
            t_check(t, elapsed <= TIMEOUT_MAX_MS,
                    "cnd_timedwait returned within %d ms (%llu ms)",
                    TIMEOUT_MAX_MS, (unsigned long long)elapsed);
        } else {
            /* The figures describe the watchdog's broadcast, not the
             * deadline, so the bounds are not applied to them. */
            t_note(t, "the %d ms deadline did not fire; the figures above "
                      "measure the watchdog, not cnd_timedwait",
                   TIMEOUT_MS);
        }

        t_check(t, atomic_load(&cc.mtx_failures) == 0,
                "no mutex call failed in the cnd_timedwait watchdog (%d)",
                atomic_load(&cc.mtx_failures));
    }

out:
    cnd_destroy(&cc.cnd);
    mtx_destroy(&cc.mtx);
}

/* ---------------------------------------------------------------- */
/* thread-specific storage                                           */
/* ---------------------------------------------------------------- */

static tss_t tss_key;

/* Incremented from several exiting threads at once, so a plain int could
 * lose a count and fail the lower bound below for a reason that has
 * nothing to do with the destructors. */
static atomic_int tss_dtor_calls;

static void
tss_dtor(void *value)
{
    (void)value;
    atomic_fetch_add(&tss_dtor_calls, 1);
}

typedef struct tss_ctx {
    int id;
    int set_result;
    int readback_matched; /* did tss_get return what tss_set stored */
    int foreign_visible;  /* did this thread see another thread's value */
} tss_ctx;

static int
tss_worker(void *arg)
{
    tss_ctx *c = arg;
    void *before;

    /* A fresh thread must start with a null value for the key. Seeing
     * anything else means the storage is process-wide, not per-thread —
     * which is the whole point of tss_*. */
    before = tss_get(tss_key);
    c->foreign_visible = (before != NULL);

    c->set_result = tss_set(tss_key, (void *)(intptr_t)c->id);
    c->readback_matched = (tss_get(tss_key) == (void *)(intptr_t)c->id);
    return 0;
}

static void
section_tss(test_ctx *t)
{
    thrd_t threads[WORKERS];
    tss_ctx tctxs[WORKERS];
    int created = 0;
    int set_failures = 0, readback_failures = 0, leaked = 0;
    int dtor_calls;

    if (!t_check(t, tss_create(&tss_key, tss_dtor) == thrd_success,
                 "tss_create succeeded")) {
        t_note(t, "there is no key to set or read; the tss checks are NOT "
                  "attempted");
        return;
    }

    /* The main thread's own slot, before any worker touches the key. */
    t_check(t, tss_get(tss_key) == NULL,
            "a fresh key reads back NULL on this thread");
    t_check(t, tss_set(tss_key, (void *)(intptr_t)0x11) == thrd_success,
            "tss_set on the main thread succeeded");
    t_check(t, tss_get(tss_key) == (void *)(intptr_t)0x11,
            "tss_get returned what the main thread stored");

    memset(tctxs, 0, sizeof(tctxs));
    for (int i = 0; i < WORKERS; i++) {
        tctxs[i].id = 0x100 + i;
        if (thrd_create(&threads[i], tss_worker, &tctxs[i]) != thrd_success)
            break;
        created++;
    }
    for (int i = 0; i < created; i++)
        thrd_join(threads[i], NULL);

    t_check(t, created == WORKERS, "created %d tss workers (%d)", WORKERS,
            created);

    for (int i = 0; i < created; i++) {
        if (tctxs[i].set_result != thrd_success)
            set_failures++;
        if (!tctxs[i].readback_matched)
            readback_failures++;
        if (tctxs[i].foreign_visible)
            leaked++;
    }
    t_check(t, set_failures == 0, "tss_set succeeded on every thread (%d failed)",
            set_failures);
    t_check(t, readback_failures == 0,
            "tss_get returned each thread's own value (%d mismatched)",
            readback_failures);
    t_check(t, leaked == 0,
            "no thread saw another thread's value (%d did)", leaked);

    /* The main thread's value must be untouched by all of that. */
    t_check(t, tss_get(tss_key) == (void *)(intptr_t)0x11,
            "the main thread's value survived the workers");

    /* The destructor runs when a thread with a non-null value exits.
     * TSS_DTOR_ITERATIONS makes the exact count implementation-defined
     * above one, so this is a lower bound, not an equality. */
    dtor_calls = atomic_load(&tss_dtor_calls);
    t_note(t, "tss destructor calls after %d workers: %d", created, dtor_calls);
    t_check(t, dtor_calls >= created,
            "the tss destructor ran for every exited thread (%d >= %d)",
            dtor_calls, created);

    tss_delete(tss_key);
}

/* ---------------------------------------------------------------- */
/* processor count                                                   */
/* ---------------------------------------------------------------- */

/* Evidence for mesa-patches/0012 and compat/sysconf.c. The raw core mask
 * is printed, not just its population count, because the mask is the
 * measurement and the count is derived from it. Depends on none of the
 * synchronisation above, so it runs whatever those sections found.
 *
 * WHAT EACH CHECK HERE IS WORTH — stated, because two of them look
 * stronger than they are. compat/sysconf.c answers _SC_NPROCESSORS_ONLN
 * with the population count of this same svcGetInfo(InfoType_CoreMask),
 * and answers _SC_NPROCESSORS_CONF from the same case label. So
 * "onln == mask_cpus" and "conf == onln" cannot disagree with that
 * file's arithmetic: they are wiring checks. What they would catch is
 * the wiring breaking — the svcGetInfo inside sysconf failing and
 * returning -1, a _SC_ name falling through to the EINVAL default, or a
 * later edit that reports something other than the mask's popcount. They
 * are not an independent confirmation of the core count, and nothing
 * here should be read as offering one.
 *
 * The independent measurements are the mask itself, its bounds, and
 * caps->nr_cpus: util_cpu_detect arrives at its number through Mesa's
 * own code, and reported 1 regardless of the mask before
 * mesa-patches/0012. */
static void
section_cpu_count(test_ctx *t)
{
    u64 core_mask = 0;
    Result res = svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    int mask_cpus;
    long onln, conf;
    const struct util_cpu_caps_t *caps;

    if (!t_check(t, R_SUCCEEDED(res),
                 "svcGetInfo(InfoType_CoreMask) succeeded (0x%08x)",
                 (unsigned)res))
        return;

    mask_cpus = __builtin_popcountll(core_mask);
    onln = sysconf(_SC_NPROCESSORS_ONLN);
    conf = sysconf(_SC_NPROCESSORS_CONF);

    t_note(t, "InfoType_CoreMask = 0x%llx, %d core(s) allowed",
           (unsigned long long)core_mask, mask_cpus);

    t_check(t, mask_cpus >= 1, "the process may run on at least one core");
    /* The Tegra X1 in the Switch has four Cortex-A57s. More than four
     * allowed cores would mean this reasoning, or the mask, is not what
     * it is believed to be — worth failing on rather than reporting a
     * larger number.
     *
     * That the four is spelled here and not in compat/sysconf.c is the
     * intended split, not an oversight: a test may know which console it
     * was launched on, while a C library function must not hand a caller
     * a number nobody queried. sysconf reports what this process is
     * allowed; this line checks that against the hardware it is running
     * on. */
    t_check(t, mask_cpus <= 4,
            "allowed cores (%d) do not exceed the SoC's four A57s", mask_cpus);

    t_check(t, onln == mask_cpus,
            "sysconf(_SC_NPROCESSORS_ONLN) = %ld is the popcount of the mask "
            "it is computed from (%d)",
            onln, mask_cpus);
    t_check(t, conf == onln,
            "sysconf(_SC_NPROCESSORS_CONF) = %ld answers as _ONLN (%ld), the "
            "case label it shares",
            conf, onln);

    /* And the number Mesa will actually act on. Before
     * mesa-patches/0012 this was 1 regardless of the mask, because
     * u_cpu_detect's counting block was under DETECT_OS_POSIX and
     * Horizon is POSIX-lite.
     *
     * util_get_cpu_caps() is the public entry point; it runs
     * _util_cpu_detect_once through call_once on first use, so this also
     * exercises Mesa's call_once a second time, from inside Mesa's own
     * code.
     *
     * REACHED IN STAGES, AND WHY. On the first run of this test (an
     * emulator, 2026-07-28) the process stopped inside this call: the
     * log ends at the _SC_NPROCESSORS_CONF check above with no verdict
     * line, so nothing between here and the note below was written.
     * util_get_cpu_caps() is not one operation — it is sysconf twice
     * (already exercised two checks above), then Mesa's option lookup,
     * which on first use builds a hash table under a statically
     * initialised simple_mtx whose lock goes through a thread_local in
     * u_call_once.c, allocates with ralloc and registers an atexit
     * handler. None of that had ever run here. Naming each stage before
     * entering it turns "it died somewhere in there" into a line number,
     * and testfw flushes every line, so the next log says which.
     *
     * The tally is printed first for the same reason: a call that never
     * returns takes the RESULT line with it, and a run that produced 70
     * results should not be unreadable because of the 71st. */
    t_note(t, "provisional tally before the staged probe below: %d passed, "
              "%d failed",
           t->pass, t->fail);

    t_note(t, "stage 1/4: getenv(\"GALLIUM_OVERRIDE_CPU_CAPS\") — plain newlib");
    {
        const char *raw = getenv("GALLIUM_OVERRIDE_CPU_CAPS");
        t_note(t, "stage 1/4 returned %s", raw ? raw : "(null)");
    }

    t_note(t, "stage 2/4: os_get_option — Mesa's wrapper over getenv");
    {
        const char *opt = os_get_option("GALLIUM_OVERRIDE_CPU_CAPS");
        t_note(t, "stage 2/4 returned %s", opt ? opt : "(null)");
    }

    /* The first thing in this file to use ralloc, a hash table, atexit
     * and a statically initialised simple_mtx — the machinery
     * _util_cpu_detect_once reaches through debug_get_option_dump_cpu().
     */
    t_note(t, "stage 3/4: os_get_option_cached — hash table, ralloc, "
              "simple_mtx, atexit");
    {
        const char *opt = os_get_option_cached("GALLIUM_DUMP_CPU");
        t_note(t, "stage 3/4 returned %s", opt ? opt : "(null)");
    }

    t_note(t, "stage 4/4: util_get_cpu_caps");
    caps = util_get_cpu_caps();
    t_note(t, "util_cpu_caps: nr_cpus=%u max_cpus=%u num_cpu_mask_bits=%u",
           caps->nr_cpus, caps->max_cpus, caps->num_cpu_mask_bits);
    t_check(t, (int)caps->nr_cpus == mask_cpus,
            "util_cpu_detect reports %u CPUs, matching the core mask (%d)",
            caps->nr_cpus, mask_cpus);
}

int
run_test(test_ctx *t)
{
    /* Ordered so that each section only depends on what the ones before
     * it established. A section that cannot set itself up reports that
     * and returns; the ones after it still run, because they measure
     * different things. The exception is the first: with no threads at
     * all there is nothing below to measure. */
    if (!section_threads(t))
        return 1;

    section_call_once(t);
    section_mutex_counter(t);
    section_mutex_timed(t);
    section_condvar(t);
    section_tss(t);
    section_cpu_count(t);

    return 0;
}
