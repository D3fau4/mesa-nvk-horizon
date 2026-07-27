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
 * every timed check here is bounded from BOTH sides, and the clock used
 * to bound it is armGetSystemTick() — the ARM system counter, which is
 * not the clock the implementation uses (clock_gettime through
 * c23_timespec_get). Measuring a clock with itself proves nothing.
 *
 * The processor-count section is the hardware evidence for
 * mesa-patches/0012 and for compat/sysconf.c's _SC_NPROCESSORS_*.
 *
 * THREAD SAFETY OF THE TEST ITSELF. t_check() and t_note() write the
 * shared test_ctx and a FILE*, so they are called from the main thread
 * only. Worker threads set plain counters and flags, which are read
 * after the join that orders them.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <switch.h>

/* Mesa's, not newlib's. See the note above. */
#include "c11/threads.h"
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
 * implementation bug, which is why it is spelled out here. */
static void
deadline_in_ms(struct timespec *ts, long ms)
{
    timespec_get(ts, TIME_UTC);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec += 1;
    }
}

/* ---------------------------------------------------------------- */
/* call_once                                                         */
/* ---------------------------------------------------------------- */

static once_flag once = ONCE_FLAG_INIT;
static int once_calls; /* written only inside call_once's callback */

static void
once_body(void)
{
    once_calls++;
}

static int
once_worker(void *arg)
{
    (void)arg;
    call_once(&once, once_body);
    return 0;
}

/* ---------------------------------------------------------------- */
/* shared counter under a mutex                                      */
/* ---------------------------------------------------------------- */

typedef struct counter_ctx {
    mtx_t *mtx;
    uint64_t *counter;
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
        if (mtx_unlock(c->mtx) != thrd_success)
            c->unlock_failures++;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* mtx_timedlock — the check the polling path can lie about          */
/* ---------------------------------------------------------------- */

typedef struct timedlock_ctx {
    mtx_t *mtx;
    int result;          /* what mtx_timedlock returned */
    uint64_t elapsed_ms; /* measured by the ARM system counter */
} timedlock_ctx;

static int
timedlock_worker(void *arg)
{
    timedlock_ctx *c = arg;
    struct timespec deadline;
    uint64_t start;

    deadline_in_ms(&deadline, TIMEOUT_MS);
    start = armGetSystemTick();
    c->result = mtx_timedlock(c->mtx, &deadline);
    c->elapsed_ms = ms_since(start);

    /* If it unexpectedly succeeded, do not leave the mutex held: the
     * main thread unlocks its own hold and would then be unlocking a
     * mutex owned by nobody. */
    if (c->result == thrd_success)
        mtx_unlock(c->mtx);
    return 0;
}

/* ---------------------------------------------------------------- */
/* condition variables                                               */
/* ---------------------------------------------------------------- */

typedef struct cnd_ctx {
    mtx_t mtx;
    cnd_t cnd;
    int predicate;   /* guarded by mtx */
    int woken;       /* guarded by mtx */
    int wait_failed; /* guarded by mtx */
} cnd_ctx;

static int
cnd_worker(void *arg)
{
    cnd_ctx *c = arg;

    mtx_lock(&c->mtx);
    while (!c->predicate) {
        if (cnd_wait(&c->cnd, &c->mtx) != thrd_success) {
            c->wait_failed++;
            break;
        }
    }
    c->woken++;
    mtx_unlock(&c->mtx);
    return 0;
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

/* ---------------------------------------------------------------- */
/* the thread that returns a value, for thrd_join                    */
/* ---------------------------------------------------------------- */

#define JOIN_MAGIC 0x5eed

static int
returning_worker(void *arg)
{
    return (int)(intptr_t)arg;
}

int
run_test(test_ctx *t)
{
    thrd_t threads[WORKERS];
    int rc, res;

    /* ---- thrd_create / thrd_join, and the returned value --------- */

    rc = thrd_create(&threads[0], returning_worker, (void *)(intptr_t)JOIN_MAGIC);
    if (!t_check(t, rc == thrd_success, "thrd_create returned thrd_success (%d)",
                 rc))
        return 1; /* nothing below can run without threads */

    res = 0;
    rc = thrd_join(threads[0], &res);
    t_check(t, rc == thrd_success, "thrd_join returned thrd_success (%d)", rc);
    t_check(t, res == JOIN_MAGIC,
            "thrd_join reported the thread's return value (0x%x)", res);

    /* u_thread_create is the wrapper every Mesa consumer actually calls
     * (util/u_queue.c does), and mesa-patches/0011 changed which of its
     * two branches this platform compiles. Exercising it here is what
     * makes that patch a measurement rather than a compile. */
    rc = u_thread_create(&threads[0], returning_worker,
                         (void *)(intptr_t)JOIN_MAGIC);
    t_check(t, rc == thrd_success, "u_thread_create returned thrd_success (%d)",
            rc);
    if (rc == thrd_success) {
        res = 0;
        thrd_join(threads[0], &res);
        t_check(t, res == JOIN_MAGIC,
                "u_thread_create's thread ran and returned 0x%x", res);
    }

    /* ---- call_once ---------------------------------------------- */

    for (int i = 0; i < WORKERS; i++) {
        if (thrd_create(&threads[i], once_worker, NULL) != thrd_success) {
            t_check(t, false, "created call_once worker %d", i);
            /* Join what was created, so the flag below is not read while
             * a thread may still be running. */
            for (int j = 0; j < i; j++)
                thrd_join(threads[j], NULL);
            return 1;
        }
    }
    for (int i = 0; i < WORKERS; i++)
        thrd_join(threads[i], NULL);

    t_check(t, once_calls == 1,
            "call_once ran the body exactly once across %d threads (%d)",
            WORKERS, once_calls);

    /* ---- mutex: mutual exclusion over a shared counter ----------- */

    mtx_t counter_mtx;
    t_check(t, mtx_init(&counter_mtx, mtx_plain) == thrd_success,
            "mtx_init(mtx_plain) succeeded");

    /* mtx_timed is what mtx_timedlock is documented to require, and it
     * is the type newlib's own mtx_init rejects outright. Mesa's shim
     * maps every type onto one pthread mutex, so this must succeed —
     * and a failure here would explain a later timedlock failure. */
    mtx_t timed_mtx;
    t_check(t, mtx_init(&timed_mtx, mtx_timed) == thrd_success,
            "mtx_init(mtx_timed) succeeded");

    uint64_t counter = 0;
    counter_ctx ctxs[WORKERS];
    memset(ctxs, 0, sizeof(ctxs));

    for (int i = 0; i < WORKERS; i++) {
        ctxs[i].mtx = &counter_mtx;
        ctxs[i].counter = &counter;
        if (thrd_create(&threads[i], counter_worker, &ctxs[i]) != thrd_success) {
            t_check(t, false, "created counter worker %d", i);
            for (int j = 0; j < i; j++)
                thrd_join(threads[j], NULL);
            return 1;
        }
    }
    for (int i = 0; i < WORKERS; i++)
        thrd_join(threads[i], NULL);

    int lock_failures = 0, unlock_failures = 0;
    for (int i = 0; i < WORKERS; i++) {
        lock_failures += ctxs[i].lock_failures;
        unlock_failures += ctxs[i].unlock_failures;
    }
    t_check(t, lock_failures == 0, "no mtx_lock failed (%d)", lock_failures);
    t_check(t, unlock_failures == 0, "no mtx_unlock failed (%d)",
            unlock_failures);
    t_note(t, "shared counter: %d threads x %d increments", WORKERS,
           INCREMENTS);
    t_check(t, counter == (uint64_t)WORKERS * INCREMENTS,
            "counter is exactly %llu, no update lost (got %llu)",
            (unsigned long long)WORKERS * INCREMENTS,
            (unsigned long long)counter);

    /* ---- mtx_trylock: what the polling timedlock is built on ----- */

    t_check(t, mtx_lock(&timed_mtx) == thrd_success,
            "main thread took the timed mutex");
    t_check(t, mtx_trylock(&timed_mtx) == thrd_busy,
            "mtx_trylock on a held mutex reports thrd_busy");

    /* ---- mtx_timedlock, timeout that must EXPIRE ---------------- */

    timedlock_ctx tl;
    memset(&tl, 0, sizeof(tl));
    tl.mtx = &timed_mtx;
    tl.result = -1;

    if (t_check(t, thrd_create(&threads[0], timedlock_worker, &tl) ==
                       thrd_success,
                "created the mtx_timedlock worker")) {
        thrd_join(threads[0], NULL);

        t_note(t, "mtx_timedlock(%d ms) returned %d after %llu ms "
                  "(thrd_success=%d thrd_timedout=%d thrd_error=%d)",
               TIMEOUT_MS, tl.result, (unsigned long long)tl.elapsed_ms,
               thrd_success, thrd_timedout, thrd_error);

        t_check(t, tl.result == thrd_timedout,
                "mtx_timedlock on a held mutex returned thrd_timedout (%d)",
                tl.result);
        /* The two bounds are the point of this section. Either one alone
         * passes for an implementation that is wrong in the other
         * direction. */
        t_check(t, tl.elapsed_ms >= TIMEOUT_MIN_MS,
                "it waited at least %d ms before giving up (%llu ms)",
                TIMEOUT_MIN_MS, (unsigned long long)tl.elapsed_ms);
        t_check(t, tl.elapsed_ms <= TIMEOUT_MAX_MS,
                "it gave up within %d ms (%llu ms)", TIMEOUT_MAX_MS,
                (unsigned long long)tl.elapsed_ms);
    }

    t_check(t, mtx_unlock(&timed_mtx) == thrd_success,
            "main thread released the timed mutex");

    /* ---- mtx_timedlock on a FREE mutex: must not wait ------------ */

    {
        struct timespec deadline;
        uint64_t start, elapsed;

        deadline_in_ms(&deadline, TIMEOUT_MS);
        start = armGetSystemTick();
        rc = mtx_timedlock(&timed_mtx, &deadline);
        elapsed = ms_since(start);

        t_check(t, rc == thrd_success,
                "mtx_timedlock on a free mutex returned thrd_success (%d)", rc);
        t_check(t, elapsed <= PROMPT_MAX_MS,
                "and took %llu ms, under the %d ms bound",
                (unsigned long long)elapsed, PROMPT_MAX_MS);
        if (rc == thrd_success)
            mtx_unlock(&timed_mtx);
    }

    mtx_destroy(&timed_mtx);
    mtx_destroy(&counter_mtx);

    /* ---- cnd_wait / cnd_signal ---------------------------------- */

    cnd_ctx cc;
    memset(&cc, 0, sizeof(cc));
    t_check(t, mtx_init(&cc.mtx, mtx_plain) == thrd_success,
            "mtx_init for the condvar section succeeded");
    t_check(t, cnd_init(&cc.cnd) == thrd_success, "cnd_init succeeded");

    if (t_check(t, thrd_create(&threads[0], cnd_worker, &cc) == thrd_success,
                "created the cnd_wait worker")) {
        uint64_t start = armGetSystemTick();

        /* Set the predicate under the mutex, then signal: the worker
         * loops on the predicate, so this is correct whether it is
         * already waiting or has not reached the wait yet. */
        mtx_lock(&cc.mtx);
        cc.predicate = 1;
        mtx_unlock(&cc.mtx);
        t_check(t, cnd_signal(&cc.cnd) == thrd_success, "cnd_signal succeeded");

        thrd_join(threads[0], NULL);
        uint64_t elapsed = ms_since(start);

        t_check(t, cc.woken == 1, "the cnd_wait worker woke (%d)", cc.woken);
        t_check(t, cc.wait_failed == 0, "cnd_wait did not fail (%d)",
                cc.wait_failed);
        t_check(t, elapsed <= PROMPT_MAX_MS,
                "signal to wake took %llu ms, under the %d ms bound",
                (unsigned long long)elapsed, PROMPT_MAX_MS);
    }

    /* ---- cnd_broadcast: every waiter wakes ---------------------- */

    cc.predicate = 0;
    cc.woken = 0;
    cc.wait_failed = 0;

    int created = 0;
    for (int i = 0; i < WORKERS; i++) {
        if (thrd_create(&threads[i], cnd_worker, &cc) != thrd_success)
            break;
        created++;
    }
    t_check(t, created == WORKERS, "created %d cnd_broadcast workers (%d)",
            WORKERS, created);

    if (created > 0) {
        uint64_t start = armGetSystemTick();

        mtx_lock(&cc.mtx);
        cc.predicate = 1;
        mtx_unlock(&cc.mtx);
        t_check(t, cnd_broadcast(&cc.cnd) == thrd_success,
                "cnd_broadcast succeeded");

        for (int i = 0; i < created; i++)
            thrd_join(threads[i], NULL);
        uint64_t elapsed = ms_since(start);

        t_check(t, cc.woken == created,
                "every broadcast waiter woke (%d of %d)", cc.woken, created);
        t_check(t, cc.wait_failed == 0, "no cnd_wait failed (%d)",
                cc.wait_failed);
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
        uint64_t start, elapsed;

        mtx_lock(&cc.mtx);
        deadline_in_ms(&deadline, TIMEOUT_MS);
        start = armGetSystemTick();
        rc = cnd_timedwait(&cc.cnd, &cc.mtx, &deadline);
        elapsed = ms_since(start);
        mtx_unlock(&cc.mtx);

        t_note(t, "cnd_timedwait(%d ms) returned %d after %llu ms", TIMEOUT_MS,
               rc, (unsigned long long)elapsed);
        t_check(t, rc == thrd_timedout,
                "cnd_timedwait with nobody signalling returned thrd_timedout "
                "(%d)",
                rc);
        t_check(t, elapsed >= TIMEOUT_MIN_MS,
                "cnd_timedwait waited at least %d ms (%llu ms)", TIMEOUT_MIN_MS,
                (unsigned long long)elapsed);
        t_check(t, elapsed <= TIMEOUT_MAX_MS,
                "cnd_timedwait returned within %d ms (%llu ms)", TIMEOUT_MAX_MS,
                (unsigned long long)elapsed);
    }

    cnd_destroy(&cc.cnd);
    mtx_destroy(&cc.mtx);

    /* ---- tss_create / tss_set / tss_get ------------------------- */

    t_check(t, tss_create(&tss_key, tss_dtor) == thrd_success,
            "tss_create succeeded");

    /* The main thread's own slot, before any worker touches the key. */
    t_check(t, tss_get(tss_key) == NULL,
            "a fresh key reads back NULL on this thread");
    t_check(t, tss_set(tss_key, (void *)(intptr_t)0x11) == thrd_success,
            "tss_set on the main thread succeeded");
    t_check(t, tss_get(tss_key) == (void *)(intptr_t)0x11,
            "tss_get returned what the main thread stored");

    tss_ctx tctxs[WORKERS];
    memset(tctxs, 0, sizeof(tctxs));
    created = 0;
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

    int set_failures = 0, readback_failures = 0, leaked = 0;
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
    int dtor_calls = atomic_load(&tss_dtor_calls);
    t_note(t, "tss destructor calls after %d workers: %d", created, dtor_calls);
    t_check(t, dtor_calls >= created,
            "the tss destructor ran for every exited thread (%d >= %d)",
            dtor_calls, created);

    tss_delete(tss_key);

    /* ---- processor count ---------------------------------------- */

    /* Evidence for mesa-patches/0012 and compat/sysconf.c. The raw core
     * mask is printed, not just its population count, because the mask
     * is the measurement and the count is derived from it. */
    u64 core_mask = 0;
    Result crc_res = svcGetInfo(&core_mask, InfoType_CoreMask,
                                CUR_PROCESS_HANDLE, 0);

    if (t_check(t, R_SUCCEEDED(crc_res),
                "svcGetInfo(InfoType_CoreMask) succeeded (0x%08x)",
                (unsigned)crc_res)) {
        int mask_cpus = __builtin_popcountll(core_mask);
        long onln = sysconf(_SC_NPROCESSORS_ONLN);
        long conf = sysconf(_SC_NPROCESSORS_CONF);

        t_note(t, "InfoType_CoreMask = 0x%llx, %d core(s) allowed",
               (unsigned long long)core_mask, mask_cpus);

        t_check(t, mask_cpus >= 1, "the process may run on at least one core");
        /* The Tegra X1 in the Switch has four Cortex-A57s. More than
         * four allowed cores would mean this reasoning, or the mask, is
         * not what it is believed to be — worth failing on rather than
         * reporting a larger number. */
        t_check(t, mask_cpus <= 4,
                "allowed cores (%d) do not exceed the SoC's four A57s",
                mask_cpus);

        t_check(t, onln == mask_cpus,
                "sysconf(_SC_NPROCESSORS_ONLN) = %ld matches the core mask (%d)",
                onln, mask_cpus);
        t_check(t, conf == onln,
                "sysconf(_SC_NPROCESSORS_CONF) = %ld agrees with _ONLN (%ld)",
                conf, onln);

        /* And the number Mesa will actually act on. Before
         * mesa-patches/0012 this was 1 regardless of the mask, because
         * u_cpu_detect's counting block was under DETECT_OS_POSIX and
         * Horizon is POSIX-lite. */
        /* util_get_cpu_caps() is the public entry point; it runs
         * _util_cpu_detect_once through call_once on first use, so this
         * also exercises Mesa's call_once on hardware a second time,
         * from inside Mesa's own code. */
        const struct util_cpu_caps_t *caps = util_get_cpu_caps();
        t_note(t, "util_cpu_caps: nr_cpus=%u max_cpus=%u num_cpu_mask_bits=%u",
               caps->nr_cpus, caps->max_cpus, caps->num_cpu_mask_bits);
        t_check(t, (int)caps->nr_cpus == mask_cpus,
                "util_cpu_detect reports %u CPUs, matching the core mask (%d)",
                caps->nr_cpus, mask_cpus);
    }

    return 0;
}
