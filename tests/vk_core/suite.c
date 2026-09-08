/*
 * vk_core — the driver comes up, and the things everything else reads
 * its answers back through.
 *
 * `bringup` is the mandatory Vulkan sequence, ending in a CPU readback
 * that validates. `transfer` is buffer and image copies, which every
 * later case in every later suite uses to get a result off the GPU;
 * `upload_chunks` is next to it because it is the same readback aimed
 * at one defect underneath it — the command-buffer upload chunk that
 * patch `0073` cleans.
 * `image_clear` is an off-screen image and the render pass NVK
 * implements a clear as. Then what the driver claims about itself
 * (`capabilities`), what a GPU timestamp tick is worth (`timestamps`),
 * several submits in flight at once (`concurrent_submits`), several
 * command buffers inside ONE submit (`submit_batching`), and sparse
 * binding through vkQueueBindSparse (`sparse_binding`).
 *
 * EVERY CASE BUILDS ITS OWN VkInstance AND VkDevice AND DESTROYS THEM.
 * That is what makes grouping safe here: no case inherits a heap, a
 * queue or a channel from the one before it. It is also new coverage —
 * seven create/destroy cycles in one process, where the most any test
 * had ever done was two.
 *
 * `image_clear` MUST STILL REACH ITS FIRST RENDER PASS WITH NOTHING
 * UPLOADED TO THE SHADER HEAP, or it stops being the regression test
 * for patch 0030. It compiles in no shader, and its device is its own,
 * so a case listed before it cannot spend that heap. The shader list in
 * meson.build says the same thing and check-mesa-test-parity.sh fails
 * if it ever stops being true.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(vk_core, bringup);
TEST_CASE_DECL(vk_core, transfer);
TEST_CASE_DECL(vk_core, upload_chunks);
TEST_CASE_DECL(vk_core, device_memory);
TEST_CASE_DECL(vk_core, image_clear);
TEST_CASE_DECL(vk_core, capabilities);
TEST_CASE_DECL(vk_core, timestamps);
TEST_CASE_DECL(vk_core, concurrent_submits);
TEST_CASE_DECL(vk_core, submit_batching);
TEST_CASE_DECL(vk_core, sparse_binding);

/* No display: main() starts a console and reports through it. */
TEST_SUITE("vk_core", false,
           TEST_CASE(vk_core, bringup),
           TEST_CASE(vk_core, transfer),
           TEST_CASE(vk_core, upload_chunks),
           TEST_CASE(vk_core, device_memory),
           TEST_CASE(vk_core, image_clear),
           TEST_CASE(vk_core, capabilities),
           TEST_CASE(vk_core, timestamps),
           TEST_CASE(vk_core, concurrent_submits),
           TEST_CASE(vk_core, submit_batching),
           TEST_CASE(vk_core, sparse_binding));
