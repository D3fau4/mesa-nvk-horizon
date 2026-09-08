/*
 * platform — the console underneath everything else.
 *
 * The nv service session and what it says about this GPU, the full
 * reverse teardown of a device, and the C library's own answers about
 * the machine. Nothing here draws anything or submits any work, so a
 * failure in this suite means the rest of the suites are measuring a
 * console that is not in the state they assume.
 *
 * RUN IT FIRST when triaging. `sysconf` needs no nv services at all and
 * is the cheapest thing in the tree that can fail for a real reason.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(platform, nv_bringup);
TEST_CASE_DECL(platform, teardown);
TEST_CASE_DECL(platform, sysconf);

/* No display: main() starts a console and reports through it. */
TEST_SUITE("platform", false,
           TEST_CASE(platform, nv_bringup),
           TEST_CASE(platform, teardown),
           TEST_CASE(platform, sysconf));
