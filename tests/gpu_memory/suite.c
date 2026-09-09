/*
 * gpu_memory — allocation, the address space, and what the GPU can see.
 *
 * NvMap objects, the VA allocator, fixed-VA mapping and the cache
 * policies, plus the one region of the address space the hardware
 * reserves for shader local and shared memory. Every case creates its
 * own device and destroys it, so a leak in one is visible in its own
 * counters rather than in the next case's.
 *
 * ORDER MATTERS FOR THE LAST TWO. `borrowed_pages` reports what this
 * launch's heap holds and how much the allocator has had to set aside
 * so far, which is 0 on a healthy launch; `borrowed_retry` then lends
 * the allocator a block on purpose and makes that number grow. Running
 * them the other way round would leave the diagnostic reporting this
 * suite's own doing.
 *
 * WHAT IS NOT HERE. Sparse reservations live in `gpu_fault`, not
 * because they are not memory but because measuring them means writing
 * to addresses the page tables may not resolve, and that loses a
 * channel on purpose. Deliberate faults are kept in one suite that is
 * run last.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(gpu_memory, alloc);
TEST_CASE_DECL(gpu_memory, nvmap);
TEST_CASE_DECL(gpu_memory, va_reserve);
TEST_CASE_DECL(gpu_memory, va_map);
TEST_CASE_DECL(gpu_memory, uncached);
TEST_CASE_DECL(gpu_memory, shader_window);
TEST_CASE_DECL(gpu_memory, borrowed_pages);
TEST_CASE_DECL(gpu_memory, borrowed_retry);

/* No display: main() starts a console and reports through it. */
TEST_SUITE("gpu_memory", false,
           TEST_CASE(gpu_memory, alloc),
           TEST_CASE(gpu_memory, nvmap),
           TEST_CASE(gpu_memory, va_reserve),
           TEST_CASE(gpu_memory, va_map),
           TEST_CASE(gpu_memory, uncached),
           TEST_CASE(gpu_memory, shader_window),
           TEST_CASE(gpu_memory, borrowed_pages),
           TEST_CASE(gpu_memory, borrowed_retry));
