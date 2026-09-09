/*
 * gpu_fault — the two cases that make the GPU fault on purpose.
 *
 * RUN THIS SUITE LAST, AND ON ITS OWN. Both cases point the channel's
 * semaphore release at an address the page tables may not resolve. That
 * is the instrument: "did the channel survive" is the answer to "is this
 * address defined", and it needs no readback. It also means a channel is
 * lost every time the answer is no, and whether the console is entirely
 * well afterwards has never been confirmed — which is why these two are
 * a suite of their own rather than cases in `gpu_memory` and
 * `gpu_submit`, where they would take every case behind them down with
 * them.
 *
 * THE ORDER INSIDE IT IS THE SAME ARGUMENT. `sparse` faults in a
 * contained way — each of its three probes gets a channel of its own,
 * precisely so a fault costs one probe — and runs first. `mmu_fault`
 * provokes one deliberately and then tears the faulted channel down,
 * which is the only place teardown-after-a-fault is exercised at all;
 * it runs last, here and in the suite order as a whole.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(gpu_fault, sparse);
TEST_CASE_DECL(gpu_fault, mmu_fault);

/* No display: main() starts a console and reports through it. */
TEST_SUITE("gpu_fault", false,
           TEST_CASE(gpu_fault, sparse),
           TEST_CASE(gpu_fault, mmu_fault));
