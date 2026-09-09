/*
 * vk_wsi — VK_KHR_swapchain on the compositor: what it does, and what
 * it returns when it cannot.
 *
 * `swapchain` is the swapchain itself — pacing, buffering, recreation,
 * error paths. `suboptimal` is VK_SUBOPTIMAL_KHR against
 * VK_ERROR_OUT_OF_DATE_KHR and the line between them. `concurrency` is
 * the same swapchain while something else legitimately happens at the
 * same time, and over a length a slot leaked once per generation would
 * show up in.
 *
 * IT OWNS THE DISPLAY: no console, and
 * sdmc:/horizon_gpu_tests/vk_wsi.log is the whole record.
 *
 * `suboptimal`'s section D NEEDS SOMEBODY TO DOCK OR UNDOCK THE CONSOLE
 * WHILE IT RUNS. Nothing in the process can resize a VI layer, so that
 * is the one part of this suite no run has ever executed.
 *
 * WHY THE PACING TESTS ARE NOT HERE. `vk_present` holds the two cases
 * whose verdict is a number rather than a return code — a drawn frame's
 * cost, and FIFO against IMMEDIATE. Those two compare their own runs
 * against each other within one launch, and they are the ones a hang
 * anywhere in this suite would prevent from ever starting. Three long
 * display cases in one .nro is already the most this project is willing
 * to lose to a single hang.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(vk_wsi, swapchain);
TEST_CASE_DECL(vk_wsi, suboptimal);
TEST_CASE_DECL(vk_wsi, concurrency);

/* This suite owns the display: no console, and the SD-card log is the
 * whole record (testfw.h). */
TEST_SUITE("vk_wsi", true,
           TEST_CASE(vk_wsi, swapchain),
           TEST_CASE(vk_wsi, suboptimal),
           TEST_CASE(vk_wsi, concurrency));
