/*
 * Test 13 — Mesa's src/util/os_time.c on Horizon (milestone item 5).
 *
 * WHAT THIS EXISTS TO CATCH. os_time.c compiled for Horizon after
 * mesa-patches/0008 turned its DETECT_OS_POSIX guards into POSIX-lite
 * ones, and nothing more was ever established: no line of it had run.
 * Two of its functions are not safe to assume from a successful
 * compile.
 *
 *   os_time_get_nano() reads the clock with
 *
 *       timespec_get(&ts, TIME_MONOTONIC);
 *       return ts.tv_nsec + ts.tv_sec * 1000000000;
 *
 *   and ignores the return value. On this platform timespec_get is
 *   Mesa's own c23_timespec_get (src/c11/impl/time.c), which forwards to
 *   clock_gettime(CLOCK_MONOTONIC) and returns 0 on failure WITHOUT
 *   touching ts. So if libsysbase does not support CLOCK_MONOTONIC at
 *   run time, os_time_get_nano() returns uninitialised stack — a number
 *   that looks plausible, is not monotonic, and would make every Vulkan
 *   timeout in Phase 4 behave at random. Nothing about that is visible
 *   at compile time, and CLOCK_MONOTONIC is defined in newlib's time.h
 *   (as 4) whether or not the implementation honours it.
 *
 *   os_time_sleep() is usleep() here, through the POSIX-lite branch
 *   patch 0008 added. A usleep that returns immediately and one that
 *   sleeps ten times too long both "work".
 *
 * HOW IT IS MEASURED. Every duration is measured with
 * armGetSystemTick() — the ARM system counter, read directly, which is
 * NOT the clock under test. Checking a clock against itself proves
 * nothing, and the rate check below is what would catch os_time_get_nano
 * reporting the wrong unit.
 *
 * Uses no horizon_gpu and needs no nv services.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "c11/time.h"
#include "util/os_time.h"

#include "common/testfw.h"

const char *const test_name = "t_ostime";

/* Samples for the monotonicity and resolution measurements. Enough that
 * a clock ticking at a coarse rate still shows several distinct values,
 * few enough to stay instantaneous. */
#define SAMPLES 2000

/* ...but a sample count alone does not make "the clock advanced" a fair
 * question. This test accepts a resolution as coarse as 1 ms (see
 * min_step below), and 2000 register reads can finish inside one such
 * tick — which would leave one distinct value and look exactly like a
 * stopped clock. So the loop also runs until the ARM system counter,
 * which is not the clock under test, says more than one accepted tick
 * has passed. Five milliseconds is four ticks of headroom.
 */
#define SAMPLE_MIN_MS 5

/* And a ceiling, so a clock that never advances ends the loop instead of
 * spinning until the console is powered off. At roughly 50 ns per read
 * this is a few seconds — far above SAMPLE_MIN_MS on any working clock,
 * so it bounds only the broken case. */
#define SAMPLES_MAX 20000000

/* The sleep the accuracy section asks for, in microseconds. */
#define SLEEP_US 50000 /* 50 ms */

/* Both-sided bounds on that sleep, in microseconds. The lower bound
 * catches a usleep that returns at once; the upper one catches a sleep
 * rounded up to a scheduling quantum many times over. Generous on the
 * high side because this is a preemptive system and the test may share
 * a core with the applet. */
#define SLEEP_MIN_US (SLEEP_US / 2)
#define SLEEP_MAX_US (SLEEP_US * 4)

/* How closely os_time_get_nano's elapsed time must agree with the ARM
 * system counter's over the same interval, in percent. This is a unit
 * and rate check, not a precision one: a factor of 1000 (us reported as
 * ns) or a stopped clock is what it exists to catch. */
#define RATE_TOLERANCE_PCT 10

static uint64_t
ticks_to_ns(uint64_t ticks)
{
    return armTicksToNs(ticks);
}

static uint64_t
ms_since(uint64_t start_tick)
{
    return ticks_to_ns(armGetSystemTick() - start_tick) / 1000000u;
}

int
run_test(test_ctx *t)
{
    /* ---- does the monotonic clock actually answer? --------------- */

    /* This is the check os_time_get_nano() itself omits. If it fails,
     * every number below is stack garbage and the test says so rather
     * than reporting a plausible-looking figure. */
    struct timespec ts;
    int base;

    memset(&ts, 0, sizeof(ts));
    base = timespec_get(&ts, TIME_MONOTONIC);
    t_note(t, "timespec_get(TIME_MONOTONIC) returned %d (TIME_MONOTONIC=%d), "
              "ts = %lld s + %ld ns",
           base, TIME_MONOTONIC, (long long)ts.tv_sec, ts.tv_nsec);
    if (!t_check(t, base == TIME_MONOTONIC,
                 "the monotonic clock is available to timespec_get")) {
        t_note(t, "os_time_get_nano() reads through this call and does not "
                  "check it, so it would return uninitialised stack; the "
                  "measurements below are NOT attempted");
        return 1;
    }

    /* TIME_UTC too: threads_posix.c's polling mtx_timedlock compares
     * against it, so t_threads depends on this one working. Measured
     * here because this is the file that owns clock questions. */
    memset(&ts, 0, sizeof(ts));
    t_check(t, timespec_get(&ts, TIME_UTC) == TIME_UTC,
            "the realtime clock is available to timespec_get");

    /* ---- monotonicity and resolution ----------------------------- */

    int64_t first = os_time_get_nano();
    int64_t prev = first;
    int64_t last = first;
    int backwards = 0;
    int64_t min_step = 0; /* smallest non-zero step seen */
    int distinct = 1;
    int taken = 1;
    uint64_t loop_start = armGetSystemTick();
    uint64_t span_ms;

    /* Both conditions, not either: SAMPLES is what makes a backwards
     * step likely to be caught, SAMPLE_MIN_MS is what makes "it never
     * advanced" mean something. */
    while (taken < SAMPLES || ms_since(loop_start) < SAMPLE_MIN_MS) {
        int64_t now = os_time_get_nano();

        if (now < prev)
            backwards++;
        else if (now > prev) {
            int64_t step = now - prev;
            if (min_step == 0 || step < min_step)
                min_step = step;
            distinct++;
        }
        prev = now;
        last = now;
        taken++;

        if (taken >= SAMPLES_MAX)
            break;
    }
    span_ms = ms_since(loop_start);

    t_note(t, "os_time_get_nano: first=%lld last=%lld over %d samples "
              "spanning %llu ms by the ARM counter, %d distinct values",
           (long long)first, (long long)last, taken,
           (unsigned long long)span_ms, distinct);

    t_check(t, backwards == 0,
            "os_time_get_nano never went backwards in %d samples (%d did)",
            taken, backwards);
    t_check(t, first > 0, "os_time_get_nano returned a positive value (%lld)",
            (long long)first);

    /* A clock that does not change over an interval an independent
     * counter measured as several milliseconds is stopped, which is what
     * an unimplemented CLOCK_MONOTONIC returning a constant would look
     * like. Asserting that only once the interval is established keeps a
     * merely coarse clock from being reported as a stopped one. */
    if (!t_check(t, span_ms >= SAMPLE_MIN_MS,
                 "the sample loop spanned at least %d ms of real time "
                 "(%llu ms over %d reads)",
                 SAMPLE_MIN_MS, (unsigned long long)span_ms, taken)) {
        /* The loop only ends early at SAMPLES_MAX, and that many reads
         * cannot take under 5 ms on this hardware — so this is the
         * reference counter failing, not the clock under test. Saying
         * which one failed is the point of measuring with two. */
        t_note(t, "that is the ARM system counter not advancing, not "
                  "os_time_get_nano; the resolution checks are NOT "
                  "attempted");
    } else if (t_check(t, distinct > 1,
                       "the clock advanced during the sample loop (%d "
                       "distinct values)",
                       distinct)) {
        t_note(t, "smallest observed step: %lld ns", (long long)min_step);
        /* One millisecond is already coarse for a driver that times GPU
         * work; anything worse is worth failing on so it is not
         * discovered later as jitter. */
        t_check(t, min_step > 0 && min_step <= 1000000,
                "clock resolution is at least 1 ms (%lld ns)",
                (long long)min_step);
    }

    /* ---- rate: agree with the ARM system counter ----------------- */

    {
        uint64_t tick0, tick1;
        int64_t nano0, nano1;
        uint64_t ref_ns, measured_ns, diff_ns, tolerance_ns;

        tick0 = armGetSystemTick();
        nano0 = os_time_get_nano();
        /* svcSleepThread, not os_time_sleep: this interval must be
         * produced by something other than the code under test. */
        svcSleepThread(100000000LL); /* 100 ms */
        nano1 = os_time_get_nano();
        tick1 = armGetSystemTick();

        ref_ns = ticks_to_ns(tick1 - tick0);
        measured_ns = (uint64_t)(nano1 - nano0);
        diff_ns = measured_ns > ref_ns ? measured_ns - ref_ns
                                       : ref_ns - measured_ns;
        tolerance_ns = ref_ns / 100 * RATE_TOLERANCE_PCT;

        t_note(t, "over one 100 ms sleep: system counter %llu ns, "
                  "os_time_get_nano %llu ns, difference %llu ns",
               (unsigned long long)ref_ns, (unsigned long long)measured_ns,
               (unsigned long long)diff_ns);

        t_check(t, diff_ns <= tolerance_ns,
                "os_time_get_nano agrees with the ARM counter within %d%% "
                "(%llu ns of %llu ns allowed)",
                RATE_TOLERANCE_PCT, (unsigned long long)diff_ns,
                (unsigned long long)tolerance_ns);
    }

    /* ---- os_time_get: the microsecond wrapper -------------------- */

    {
        int64_t nano = os_time_get_nano();
        int64_t micro = os_time_get();
        int64_t derived = nano / 1000;
        int64_t drift = micro > derived ? micro - derived : derived - micro;

        /* The two calls are not simultaneous, so this is a bound rather
         * than an equality; 10 ms of slack is far more than the two
         * reads can take and far less than a unit error. */
        t_check(t, drift < 10000,
                "os_time_get is os_time_get_nano/1000 (%lld us apart)",
                (long long)drift);
    }

    /* ---- os_time_sleep: it must sleep, and not too long ---------- */

    {
        uint64_t start = armGetSystemTick();
        os_time_sleep(SLEEP_US);
        uint64_t elapsed_us = ticks_to_ns(armGetSystemTick() - start) / 1000u;

        t_note(t, "os_time_sleep(%d us) took %llu us", SLEEP_US,
               (unsigned long long)elapsed_us);
        t_check(t, elapsed_us >= SLEEP_MIN_US,
                "it slept at least %d us (%llu us)", SLEEP_MIN_US,
                (unsigned long long)elapsed_us);
        t_check(t, elapsed_us <= SLEEP_MAX_US,
                "it woke within %d us (%llu us)", SLEEP_MAX_US,
                (unsigned long long)elapsed_us);
    }

    /* A zero-length sleep must return promptly rather than blocking. */
    {
        uint64_t start = armGetSystemTick();
        os_time_sleep(0);
        uint64_t elapsed_us = ticks_to_ns(armGetSystemTick() - start) / 1000u;

        t_note(t, "os_time_sleep(0) took %llu us",
               (unsigned long long)elapsed_us);
        t_check(t, elapsed_us < 50000, "os_time_sleep(0) did not block (%llu us)",
                (unsigned long long)elapsed_us);
    }

    /* ---- os_time_nanosleep_until -------------------------------- */

    /* On POSIX-lite this is implemented as os_time_sleep of the
     * remaining time, so it depends on os_time_get_nano being usable as
     * a deadline base — which is exactly what the monotonicity check
     * above establishes. */
    {
        int64_t deadline = os_time_get_nano() + 50 * 1000000LL; /* +50 ms */
        uint64_t start = armGetSystemTick();

        os_time_nanosleep_until(deadline);
        uint64_t elapsed_us = ticks_to_ns(armGetSystemTick() - start) / 1000u;

        t_note(t, "os_time_nanosleep_until(+50 ms) took %llu us",
               (unsigned long long)elapsed_us);
        t_check(t, elapsed_us >= 25000 && elapsed_us <= 200000,
                "it waited roughly 50 ms (%llu us)",
                (unsigned long long)elapsed_us);
    }

    /* A deadline already in the past must not wait at all. */
    {
        int64_t deadline = os_time_get_nano() - 1000000LL; /* 1 ms ago */
        uint64_t start = armGetSystemTick();

        os_time_nanosleep_until(deadline);
        uint64_t elapsed_us = ticks_to_ns(armGetSystemTick() - start) / 1000u;

        t_check(t, elapsed_us < 50000,
                "a deadline in the past returns at once (%llu us)",
                (unsigned long long)elapsed_us);
    }

    /* ---- os_time_get_absolute_timeout --------------------------- */

    {
        int64_t now = os_time_get_nano();
        int64_t abs_1s = os_time_get_absolute_timeout(1000000000ULL);

        t_check(t, abs_1s > now,
                "a 1 s relative timeout is in the future (%lld > %lld)",
                (long long)abs_1s, (long long)now);
        /* Bounded above as well, or "in the future" would pass for any
         * garbage large number. */
        t_check(t, abs_1s - now >= 900000000LL && abs_1s - now <= 1100000000LL,
                "and is about 1 s away (%lld ns)", (long long)(abs_1s - now));

        t_check(t, os_time_get_absolute_timeout(OS_TIMEOUT_INFINITE) ==
                       (int64_t)OS_TIMEOUT_INFINITE,
                "OS_TIMEOUT_INFINITE passes through unchanged");
        /* The documented overflow behaviour: a timeout so large that
         * now + timeout would wrap must saturate, not wrap. */
        t_check(t, os_time_get_absolute_timeout((uint64_t)INT64_MAX) ==
                       (int64_t)OS_TIMEOUT_INFINITE,
                "an overflowing timeout saturates to OS_TIMEOUT_INFINITE");
    }

    /* ---- os_time_timeout, the pure helper ----------------------- */

    /* Pure arithmetic, no clock: checked here because it is what
     * consumers combine with the values above, and because it is cheap
     * to be sure the wrap-around branch is the right way round. */
    t_check(t, !os_time_timeout(100, 200, 150),
            "os_time_timeout: 150 is inside [100, 200)");
    t_check(t, os_time_timeout(100, 200, 250),
            "os_time_timeout: 250 is past [100, 200)");
    t_check(t, os_time_timeout(100, 200, 50),
            "os_time_timeout: 50 is outside [100, 200)");
    /* start > end is the wrapped interval [start, MAX] + [MIN, end). */
    t_check(t, !os_time_timeout(200, 100, 250),
            "os_time_timeout: 250 is inside the wrapped [200, 100)");
    t_check(t, !os_time_timeout(200, 100, 50),
            "os_time_timeout: 50 is inside the wrapped [200, 100)");
    t_check(t, os_time_timeout(200, 100, 150),
            "os_time_timeout: 150 is outside the wrapped [200, 100)");

    return 0;
}
