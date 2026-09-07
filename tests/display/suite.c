/*
 * display — the compositor, with no Vulkan in the way.
 *
 * `console_handoff` checks that a suite which claims the display gets
 * it: no console, and the SD-card log as the whole record.
 * `nwindow` then drives the VI compositor through raw bq* calls,
 * one level below NWindow, so the number of buffer slots that can be
 * held at once is measured rather than assumed.
 *
 * IT OWNS THE DISPLAY, so there is no console and nothing of this run
 * reaches a screen. Read sdmc:/horizon_gpu_tests/display.log.
 *
 * WHY `dock` IS NOT HERE. test_uses_display is decided once, before the
 * first case runs, because consoleInit configures the same nwindow a
 * swapchain would. A case that needs a console and a case that needs
 * the display therefore cannot share a .nro, and watching a console
 * being docked is the whole of what `dock` does.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(display, console_handoff);
TEST_CASE_DECL(display, nwindow);

/* This suite owns the display: no console, and the SD-card log is the
 * whole record (testfw.h). */
TEST_SUITE("display", true,
           TEST_CASE(display, console_handoff),
           TEST_CASE(display, nwindow));
