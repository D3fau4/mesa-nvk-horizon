/*
 * Host unit tests — wrap-safe syncpoint arithmetic
 * (horizon/include/horizon_gpu/sync.h, horizon/sync/syncpt_math.h).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../horizon/include/horizon_gpu/sync.h"
#include "../../horizon/sync/syncpt_math.h"
#include "hostfw.h"

/* An oracle standing in for NVHOST_IOCTL_CTRL_SYNCPT_WAIT with a zero
 * timeout: it knows the counter and answers only the yes/no question, the
 * way the kernel does. `advance` moves the counter after every answer, to
 * model a syncpoint incrementing underneath the search; `fail_at` makes
 * the n-th probe unanswerable, to model the ioctl being absent too. */
typedef struct probe_oracle {
    uint32_t value;
    uint32_t advance;
    uint32_t fail_at; /* 1-based probe index; 0 = never fail */
    uint32_t calls;
} probe_oracle;

static bool oracle_probe(void *ctx, uint32_t threshold, bool *out_reached)
{
    probe_oracle *o = (probe_oracle *)ctx;
    o->calls++;
    if (o->fail_at != 0 && o->calls >= o->fail_at)
        return false;
    *out_reached = horizon_gpu_syncpt_reached(o->value, threshold);
    o->value += o->advance;
    return true;
}

/* Recovers `hidden` through the predicate alone and checks the value, the
 * status and the probe budget. Returns true when all three hold. */
static bool search_finds(uint32_t hidden)
{
    probe_oracle o = { .value = hidden };
    uint32_t got = ~hidden, probes = 0;
    horizon_syncpt_search_status st =
        horizon_syncpt_search_value(oracle_probe, &o, &got, &probes);
    /* 1 anchor + 31 halvings + 1 verification. */
    return st == HORIZON_SYNCPT_SEARCH_OK && got == hidden && probes == 33;
}

int main(void)
{
    /* Wrap-safe reached predicate. */
    H_CHECK(horizon_gpu_syncpt_reached(5, 5), "equal reached");
    H_CHECK(horizon_gpu_syncpt_reached(6, 5), "past reached");
    H_CHECK(!horizon_gpu_syncpt_reached(4, 5), "behind not reached");
    /* Across the 32-bit wrap: current wrapped to 2, threshold near max. */
    H_CHECK(horizon_gpu_syncpt_reached(2, 0xFFFFFFF0),
            "reached across wrap");
    H_CHECK(!horizon_gpu_syncpt_reached(0xFFFFFFF0, 2),
            "not reached across wrap");
    /* The naive >= comparison gives the wrong answer for both cases
     * above; guard the exact boundary too. */
    H_CHECK(horizon_gpu_syncpt_reached(0, 0x80000001u),
            "just inside the 2^31 window");
    H_CHECK(!horizon_gpu_syncpt_reached(0, 1), "0 vs 1");

    /* 64-bit shadow extension. */
    H_CHECK(horizon_syncpt_extend(100, 100) == 100, "extend identity");
    H_CHECK(horizon_syncpt_extend(100, 90) == 90, "extend behind");
    H_CHECK(horizon_syncpt_extend(0x100000005ull, 0xFFFFFFF0u) ==
            0xFFFFFFF0ull, "extend borrows across wrap boundary");
    H_CHECK(horizon_syncpt_extend(0xFFFFFFF0ull, 2) == 0x100000002ull,
            "extend carries across wrap boundary");
    H_CHECK(horizon_syncpt_extend(0x2FFFFFFF5ull, 0xFFFFFFF6u) ==
            0x2FFFFFFF6ull, "extend same epoch high");
    /* Degenerate low references never underflow. */
    H_CHECK(horizon_syncpt_extend(5, 0xFFFFFFF0u) == 0xFFFFFFF0ull,
            "no underflow for low reference");

    /* ns -> µs conversion: round up, saturate (the reference's 1000x
     * unit bug). */
    H_CHECK(horizon_timeout_ns_to_us_clamped(0) == 0, "0 ns");
    H_CHECK(horizon_timeout_ns_to_us_clamped(1) == 1, "1 ns rounds up");
    H_CHECK(horizon_timeout_ns_to_us_clamped(1000) == 1, "1 us exact");
    H_CHECK(horizon_timeout_ns_to_us_clamped(1001) == 2, "1001 ns");
    H_CHECK(horizon_timeout_ns_to_us_clamped(UINT64_C(2000000000)) ==
            2000000, "2 s = 2,000,000 us (not 2,000,000,000)");
    H_CHECK(horizon_timeout_ns_to_us_clamped(UINT64_MAX) == INT32_MAX,
            "saturates at INT32_MAX");

    /* --- Recovering the value from the wait predicate alone. The
     * counter is never read; the oracle only ever answers "has T been
     * reached?". */

    /* The values a binary search over a circular order gets wrong when it
     * is written as if the order were linear. */
    static const uint32_t corners[] = {
        0, 1, 2, 5,
        0x7ffffffeu, 0x7fffffffu,          /* just below the half-way point */
        0x80000000u, 0x80000001u,          /* the anchor flip */
        0xfffffff0u, 0xfffffffeu, 0xffffffffu, /* the wrap itself */
    };
    bool all_corners = true;
    for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) {
        if (!search_finds(corners[i]))
            all_corners = false;
    }
    H_CHECK(all_corners, "search recovers every corner value");

    /* A deterministic sweep, so the corners above are not the only
     * evidence. 4096 values from an LCG, spread over the whole 32-bit
     * range rather than clustered near zero. */
    bool all_sweep = true;
    uint32_t rng = 0x12345677u;
    for (uint32_t i = 0; i < 4096; i++) {
        rng = rng * 1664525u + 1013904223u;
        if (!search_finds(rng))
            all_sweep = false;
    }
    H_CHECK(all_sweep, "search recovers 4096 pseudo-random values");

    /* A counter moving underneath the search can only produce an
     * *under*estimate — a "reached" answer never becomes false — and the
     * verification probe turns that into an exact test rather than a
     * heuristic: it is reported precisely when the result is stale. */
    {
        probe_oracle o = { .value = 1000, .advance = 1 };
        uint32_t got = 0, probes = 0;
        horizon_syncpt_search_status st =
            horizon_syncpt_search_value(oracle_probe, &o, &got, &probes);
        H_CHECK(st == HORIZON_SYNCPT_SEARCH_MOVED,
                "a counter advancing during the search is reported");
        H_CHECK(got == 0, "a moved search does not write a value out");
        H_CHECK(probes == 33, "detection costs no extra probes");
    }
    /* advance = 0 is the same code path with a still counter, and must
     * not report movement. */
    H_CHECK(search_finds(0x0badc0deu), "a still counter is not 'moved'");

    /* The predicate being unavailable is a distinct answer from either of
     * the above, at whichever probe it happens. */
    {
        probe_oracle o = { .value = 42, .fail_at = 1 };
        uint32_t got = 0, probes = 99;
        H_CHECK(horizon_syncpt_search_value(oracle_probe, &o, &got, &probes) ==
                HORIZON_SYNCPT_SEARCH_PROBE_FAILED, "anchor probe failure");
        H_CHECK(probes == 0, "probe count excludes the failed probe");
    }
    {
        probe_oracle o = { .value = 42, .fail_at = 10 };
        uint32_t got = 0, probes = 99;
        H_CHECK(horizon_syncpt_search_value(oracle_probe, &o, &got, &probes) ==
                HORIZON_SYNCPT_SEARCH_PROBE_FAILED, "mid-search failure");
        H_CHECK(probes == 9, "probe count reports where it stopped");
    }
    /* out_probes is optional. */
    {
        probe_oracle o = { .value = 7 };
        uint32_t got = 0;
        H_CHECK(horizon_syncpt_search_value(oracle_probe, &o, &got, NULL) ==
                HORIZON_SYNCPT_SEARCH_OK && got == 7,
                "out_probes may be NULL");
    }

    return h_summary("h_syncpt_math");
}
