/*
 * gpu_submit — channels, the GPFIFO, syncpoints and fences.
 *
 * Everything between "there is a device" and "the GPU did the thing":
 * a channel and its syncpoint, a submit that executes, the increment
 * that submit produces, waiting on it, waiting on many of them at once,
 * a GPU write becoming visible to the CPU, and how large a single GPFIFO
 * entry this hardware will accept.
 *
 * THE ORDER IS THE DEPENDENCY ORDER. `channel` before `submit` before
 * `syncpt_per_submit` before the two waits: each reads its result back
 * through machinery the one before it established, so a failure early
 * in the list explains the failures after it rather than adding to
 * them. Grouping them made that order a property of the file instead of
 * a sentence in a README.
 *
 * `syncpt_cpu_incr` disturbs a channel on purpose — it advances a
 * syncpoint from the CPU past what the channel expects — and creates
 * that channel itself for the purpose, so the disturbance does not
 * outlive the case.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(gpu_submit, channel);
TEST_CASE_DECL(gpu_submit, submit);
TEST_CASE_DECL(gpu_submit, syncpt_per_submit);
TEST_CASE_DECL(gpu_submit, syncpt_cpu_incr);
TEST_CASE_DECL(gpu_submit, fence_wait);
TEST_CASE_DECL(gpu_submit, fence_wait_many);
TEST_CASE_DECL(gpu_submit, gpu_write);
TEST_CASE_DECL(gpu_submit, pushbuf_size);

/* No display: main() starts a console and reports through it. */
TEST_SUITE("gpu_submit", false,
           TEST_CASE(gpu_submit, channel),
           TEST_CASE(gpu_submit, submit),
           TEST_CASE(gpu_submit, syncpt_per_submit),
           TEST_CASE(gpu_submit, syncpt_cpu_incr),
           TEST_CASE(gpu_submit, fence_wait),
           TEST_CASE(gpu_submit, fence_wait_many),
           TEST_CASE(gpu_submit, gpu_write),
           TEST_CASE(gpu_submit, pushbuf_size));
