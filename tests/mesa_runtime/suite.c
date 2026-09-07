/*
 * mesa_runtime — Mesa's own code, measured on this console.
 *
 * These three link the archives Mesa's build produced rather than
 * sources recompiled with flags of our own: the object under test has
 * to be the object Mesa builds, or the measurement describes a
 * different build. They use no horizon_gpu and no nv services — this is
 * the C library, the clocks and the SD card — so they are cheap to run
 * early when triaging a console.
 *
 * RUN THIS SUITE TWICE. `disk_cache`'s section C leaves entries behind
 * and reports, on the next launch, whether they came back. A cache is
 * only a cache if it survives the process that filled it, so the FIRST
 * RUN ON ANY CONSOLE REPORTS A COLD CACHE AND THAT IS A PASS; the
 * second run is the measurement. Nothing else in the suite cares how
 * many times it has run.
 *
 * `disk_cache` goes first on purpose: it times fsync() and file
 * creation on the SD card, and those are the only numbers here that a
 * busier process would change.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(mesa_runtime, disk_cache);
TEST_CASE_DECL(mesa_runtime, c11_threads);
TEST_CASE_DECL(mesa_runtime, os_time);

/* No display: main() starts a console and reports through it. */
TEST_SUITE("mesa_runtime", false,
           TEST_CASE(mesa_runtime, disk_cache),
           TEST_CASE(mesa_runtime, c11_threads),
           TEST_CASE(mesa_runtime, os_time));
