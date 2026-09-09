/*
 * vk_cache — is a shader compiled on one launch still compiled on the
 * next?
 *
 * ONE CASE, AND ITS OWN .nro, FOR A REASON THAT IS ABOUT THE CACHE AND
 * NOT ABOUT TIDINESS.
 *
 * It needs two launches of the same build, and it must be able to say
 * which of them a given run was. Within one process vk_pipeline_cache
 * answers the second identical vkCreateComputePipelines out of memory,
 * so the disk is only observable across processes: THE FIRST RUN ON A
 * GIVEN BUILD IS COLD AND THAT IS A PASS. Run it twice and read the
 * cold/warm note; do not rebuild in between, because a rebuild changes
 * the driver identity and empties the cache by design.
 *
 * And a cold run DELETES every .hzc file under sdmc:/mesa_shader_cache
 * first, so that a run calling itself cold is one. (Spelled without the
 * glob on purpose: the glob after a directory separator opens a comment
 * inside a comment, which -Werror=comment rejects.) That is destructive
 * to state the
 * whole driver shares: as a case inside `vk_core` or `vk_pipelines` it
 * would silently reset what those measure, on a schedule decided by how
 * many times somebody had happened to launch the suite.
 *
 * Delete sdmc:/horizon_gpu_tests/t_vk_cache_launch.txt to make the next
 * run cold.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(vk_cache, shader_reuse);

/* No display: main() starts a console and reports through it. */
TEST_SUITE("vk_cache", false,
           TEST_CASE(vk_cache, shader_reuse));
