/*
 * vk_shaders — machine code NAK produced, and whether it does what the
 * SPIR-V said.
 *
 * Each case is a thing a real shader does that nothing else in the tree
 * asks about: a dispatch at all (`compute_dispatch`), a loop whose trip
 * count comes from memory (`dynamic_loop`), the same loop with the
 * packed min/max bound idiom Godot's cluster loops use
 * (`packed_bound_loop`), twelve levels of nested control flow in compute
 * and again in fragment (`nested_control_flow`,
 * `nested_control_flow_frag` — the two pipes carry the convergence stack
 * through different state), the convergence stack and the register count
 * varied one at a time and at two extents (`crs_matrix`), a loop behind
 * a kill (`fragment_kill`), and reading descriptor set 1
 * (`descriptor_set1`).
 *
 * `priv_reg` IS LAST ON PURPOSE, and it is the only case here that is
 * not about NAK. It asks the queue context for the two Maxwell-B
 * privileged register writes this backend has refused since Phase 4,
 * and known-risk R18 says a console answers that by resetting the
 * channel. Every case builds its own device, so the blast radius should
 * be its own — but "should be" is why nothing is listed after it.
 *
 * GROUPING THEM COSTS NO DIAGNOSTIC VALUE, and that was the thing to
 * check. Each case still reports under its own name — a failure is
 * "vk_shaders/nested_control_flow_frag", which is as specific as the
 * .nro name was — and each builds and destroys its own device, so
 * neither the shader heap nor the pipeline cache carries from one to
 * the next. What they share is only the process, and none of them
 * measures anything about the process.
 *
 * WHAT IS DELIBERATELY NOT HERE. `vk_pipelines` is a volume and
 * compile-time measurement, not a correctness one, and it is a .nro of
 * its own for that reason.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(vk_shaders, compute_dispatch);
TEST_CASE_DECL(vk_shaders, dynamic_loop);
TEST_CASE_DECL(vk_shaders, packed_bound_loop);
TEST_CASE_DECL(vk_shaders, nested_control_flow);
TEST_CASE_DECL(vk_shaders, nested_control_flow_frag);
TEST_CASE_DECL(vk_shaders, crs_matrix);
TEST_CASE_DECL(vk_shaders, fragment_kill);
TEST_CASE_DECL(vk_shaders, descriptor_set1);
TEST_CASE_DECL(vk_shaders, priv_reg);

/* No display: main() starts a console and reports through it. */
TEST_SUITE("vk_shaders", false,
           TEST_CASE(vk_shaders, compute_dispatch),
           TEST_CASE(vk_shaders, dynamic_loop),
           TEST_CASE(vk_shaders, packed_bound_loop),
           TEST_CASE(vk_shaders, nested_control_flow),
           TEST_CASE(vk_shaders, nested_control_flow_frag),
           TEST_CASE(vk_shaders, crs_matrix),
           TEST_CASE(vk_shaders, fragment_kill),
           TEST_CASE(vk_shaders, descriptor_set1),
           TEST_CASE(vk_shaders, priv_reg));
