/*
 * vk_render — the graphics pipeline: vertices in, pixels out.
 *
 * The first draw call (`triangle`), sampling (`texture`), the depth test
 * with the buffer read back as a value rather than trusted because the
 * colours looked right (`depth`), twelve colour formats each checked
 * against the bytes its encoding demands (`formats`), a pass that
 * declares three render targets and writes one (`multi_target`), Zcull
 * checked by rendering the same workload twice and comparing
 * (`zcull`), and volume — hundreds of draws with the pipeline changing
 * between them, split across many render passes, plus blending
 * (`draw_volume`).
 *
 * THE ORDER IS THE DEPENDENCY ORDER, as it was when these were seven
 * .nro run in a documented sequence: `triangle` establishes that a draw
 * lands at all, and everything after it reads its result back through
 * machinery an earlier case proved.
 *
 * `zcull` sets NVK_HORIZON_ZCULL for its two halves and clears it again;
 * the framework also restores the environment around every case, so a
 * case that forgot could not reach the next one.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(vk_render, triangle);
TEST_CASE_DECL(vk_render, texture);
TEST_CASE_DECL(vk_render, depth);
TEST_CASE_DECL(vk_render, formats);
TEST_CASE_DECL(vk_render, multi_target);
TEST_CASE_DECL(vk_render, zcull);
TEST_CASE_DECL(vk_render, draw_volume);

/* No display: main() starts a console and reports through it. */
TEST_SUITE("vk_render", false,
           TEST_CASE(vk_render, triangle),
           TEST_CASE(vk_render, texture),
           TEST_CASE(vk_render, depth),
           TEST_CASE(vk_render, formats),
           TEST_CASE(vk_render, multi_target),
           TEST_CASE(vk_render, zcull),
           TEST_CASE(vk_render, draw_volume));
