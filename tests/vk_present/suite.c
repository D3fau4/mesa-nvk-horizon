/*
 * vk_present — what a presented frame costs, measured two ways.
 *
 * `drawn_frame` presents a frame the graphics pipeline drew, which is
 * the shape every other presenting test leaves out: the four cases in
 * `vk_wsi` present a vkCmdClearColorImage on purpose, so nothing there
 * puts the 3D pipeline's ROP into a scanout surface. Its section F is
 * also the only thing in the tree that acquires with a binary semaphore
 * and waits for it on the GPU rather than on the CPU.
 *
 * `present_modes` compares FIFO, IMMEDIATE and FIFO again, 240 frames
 * each, so the reference is shown to be stable rather than assumed.
 *
 * THESE TWO ARE TOGETHER AND APART FROM `vk_wsi` FOR THE SAME REASON.
 * Both report a verdict that is a number — a frame interval, a ratio —
 * and both get that number by comparing runs against each other inside
 * one launch, on one console, at one clock. Keeping them in one .nro
 * keeps those comparisons in one process; keeping them out of `vk_wsi`
 * keeps them from being the thing that never runs because a functional
 * case ahead of them hung.
 *
 * IT OWNS THE DISPLAY: no console, and
 * sdmc:/horizon_gpu_tests/vk_present.log is the whole record.
 *
 * FOR A BENCHMARK, leave the debug-synchronous mode, the breadcrumbs
 * and NVK_HORIZON_PUSH_SPLIT off. Both cases set the stats switches
 * they need themselves, and the framework puts the environment back
 * between them.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(vk_present, drawn_frame);
TEST_CASE_DECL(vk_present, present_modes);

/* This suite owns the display: no console, and the SD-card log is the
 * whole record (testfw.h). */
TEST_SUITE("vk_present", true,
           TEST_CASE(vk_present, drawn_frame),
           TEST_CASE(vk_present, present_modes));
