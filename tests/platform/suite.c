/*
 * platform — the console underneath everything else.
 *
 * The nv service session and what it says about this GPU, the full
 * reverse teardown of a device, and the C library's own answers about
 * the machine — plus the one piece of horizon_gpu arithmetic whose
 * shipped implementation a host build cannot compile at all (`crc32`).
 * Nothing here draws anything or submits any work, so a failure in this
 * suite means the rest of the suites are measuring a console that is
 * not in the state they assume.
 *
 * RUN IT FIRST when triaging. `sysconf` and `crc32` need no nv services
 * at all and are the cheapest things in the tree that can fail for a
 * real reason.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(platform, nv_bringup);
TEST_CASE_DECL(platform, teardown);
TEST_CASE_DECL(platform, sysconf);
TEST_CASE_DECL(platform, crc32);

/* No display: main() starts a console and reports through it. */
TEST_SUITE("platform", false,
           TEST_CASE(platform, nv_bringup),
           TEST_CASE(platform, teardown),
           TEST_CASE(platform, sysconf),
           TEST_CASE(platform, crc32));
