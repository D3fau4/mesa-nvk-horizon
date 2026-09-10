/*
 * vk_pipelines — pipeline volume, and the compile times an application
 * feels as stutter.
 *
 * ITS OWN .nro, BECAUSE WHAT IT REPORTS IS A MEASUREMENT. Ninety-six
 * distinct compute pipelines back to back is the only thing that makes
 * NVK's contiguous shader heap grow past the chunk it binds at device
 * creation, and the numbers this reports — per-pipeline compile time,
 * where the heap grew, what the free list did under create/destroy
 * churn — are what an application feels between frames.
 *
 * A measurement that runs behind other work is a different measurement.
 * Grouping this with `vk_shaders` would put it after seven device
 * lifetimes' worth of allocation in the same process; grouping it with
 * `vk_cache` would let a cold run there delete the shader cache and
 * change these compile times from one launch to the next. Neither
 * failure is visible as a failure — the numbers simply mean something
 * else — which is exactly the case for keeping a .nro.
 *
 * WHICH IS ALSO WHY THE ORDER HERE IS NOT NEGOTIABLE. `pipeline_volume`
 * runs first and must go on running first: its numbers are what a fresh
 * process pays, and they are on record from a console. `compile_identity`
 * runs behind it deliberately — its numbers are a paired comparison
 * between two builds of this same suite, not an absolute, so the work in
 * front of it is the same work on both sides.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(vk_pipelines, pipeline_volume);
TEST_CASE_DECL(vk_pipelines, compile_identity);

/* No display: main() starts a console and reports through it. */
TEST_SUITE("vk_pipelines", false,
           TEST_CASE(vk_pipelines, pipeline_volume),
           TEST_CASE(vk_pipelines, compile_identity));
