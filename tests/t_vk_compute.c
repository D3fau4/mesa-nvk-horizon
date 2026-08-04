/*
 * Phase 5 item 2 — compute dispatch. The first shader this project runs.
 *
 * WHAT IS ACTUALLY NEW HERE. Everything that has run on this GPU so far
 * is host methods and one fill by the NV90B5 copy engine. NAK and NIL
 * have been compiled and linked since Phase 4 and have never produced
 * machine code that was executed. So this test has two failure points
 * that nothing before it could have: vkCreateComputePipelines, where
 * NAK compiles, and vkCmdDispatch, where the result runs.
 *
 * They are separated on purpose. A pipeline that fails to create is a
 * compiler problem and says so before anything is submitted; a pipeline
 * that creates and then produces the wrong words is the shader or the
 * dispatch state. The check lines distinguish them.
 *
 * WHAT THE SHADER COMPUTES, and why it is not a constant:
 *
 *     out[id] = (id * 2654435769) ^ 0xa5c3f00d      (mod 2^32)
 *
 * A dispatch that ran but computed nothing would still fill the buffer
 * if the value were a constant; a dispatch that never ran leaves the
 * poison. Every word differs from every other, so a shader that wrote
 * the right value at the wrong index fails on the index.
 *
 * THE TAIL IS THE BOUNDS CHECK. The buffer is longer than the dispatch,
 * and the words past the last invocation must still hold their poison.
 * The shader has no bounds test of its own — it writes at
 * gl_GlobalInvocationID.x unconditionally — so this is what catches a
 * dispatch of the wrong size or a local size that is not what the
 * shader declared.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/vkfw.h"

/* Generated from tests/shaders/comp_write_id.spvasm by
 * scripts/spv-embed.py; assembled by spirv-as and validated by
 * spirv-val at build time. */
#include "comp_write_id.spv.h"

const char *const test_name = "t_vk_compute";

/* Must match OpExecutionMode LocalSize in the shader. Stated here
 * because the group count below is derived from it, and a disagreement
 * between the two would show up as a wrong tail rather than as an
 * error. */
#define LOCAL_SIZE_X   64u

#define DISPATCH_WORDS 4096u                       /* 64 groups of 64 */
#define GROUPS         (DISPATCH_WORDS / LOCAL_SIZE_X)
#define TAIL_WORDS     64u                         /* never written */
#define BUF_WORDS      (DISPATCH_WORDS + TAIL_WORDS)
#define BUF_BYTES      (BUF_WORDS * 4u)

#define POISON         0xdeadbeefu

/* The same arithmetic the shader does, in C. uint32_t wraps, which is
 * what OpIMul on a 32-bit unsigned does. */
static uint32_t expect_word(uint32_t id)
{
   return (id * 2654435769u) ^ 2781138957u;
}

int run_test(test_ctx *t)
{
   vkfw fw;
   if (!vkfw_init(&fw, t, NULL))
      return 1;

   VkShaderModule module = VK_NULL_HANDLE;
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkDescriptorPool pool = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   vkfw_buffer ssbo = { 0 };

   if (!vkfw_buffer_create(&fw, BUF_BYTES,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &ssbo))
      goto out;

   /* --- the compiler ------------------------------------------------ */
   const VkShaderModuleCreateInfo smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(comp_write_id_spv),
      .pCode = comp_write_id_spv,
   };
   VkResult r = fw.vk.vkCreateShaderModule(fw.dev, &smci, NULL, &module);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateShaderModule(%zu bytes) -> %s",
                sizeof(comp_write_id_spv), vkfw_result_str(r)))
      goto out;

   const VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };
   const VkDescriptorSetLayoutCreateInfo dslci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };
   r = fw.vk.vkCreateDescriptorSetLayout(fw.dev, &dslci, NULL, &set_layout);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorSetLayout -> %s",
                vkfw_result_str(r)))
      goto out;

   const VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
   };
   r = fw.vk.vkCreatePipelineLayout(fw.dev, &plci, NULL, &layout);
   if (!t_check(t, r == VK_SUCCESS, "vkCreatePipelineLayout -> %s",
                vkfw_result_str(r)))
      goto out;

   /* This is the line where NAK compiles a shader for the first time in
    * this project. Anything it has to say arrives through the debug
    * messenger vkfw registered. */
   const VkComputePipelineCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = module,
         .pName = "main",
      },
      .layout = layout,
   };
   r = fw.vk.vkCreateComputePipelines(fw.dev, VK_NULL_HANDLE, 1, &cpci,
                                      NULL, &pipeline);
   if (!t_check(t, r == VK_SUCCESS,
                "vkCreateComputePipelines -> %s  (NAK compiled a shader)",
                vkfw_result_str(r)))
      goto out;

   /* --- the descriptor ---------------------------------------------- */
   const VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
   };
   const VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   r = fw.vk.vkCreateDescriptorPool(fw.dev, &dpci, NULL, &pool);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorPool -> %s",
                vkfw_result_str(r)))
      goto out;

   const VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkDescriptorSet set = VK_NULL_HANDLE;
   r = fw.vk.vkAllocateDescriptorSets(fw.dev, &dsai, &set);
   if (!t_check(t, r == VK_SUCCESS, "vkAllocateDescriptorSets -> %s",
                vkfw_result_str(r)))
      goto out;

   const VkDescriptorBufferInfo dbi = {
      .buffer = ssbo.buf,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
   };
   const VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &dbi,
   };
   fw.vk.vkUpdateDescriptorSets(fw.dev, 1, &write, 0, NULL);

   /* --- the dispatch ------------------------------------------------ */
   if (!vkfw_buffer_poison(&fw, &ssbo, POISON))
      goto out;

   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(&fw, &cb))
      goto out;

   fw.vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   fw.vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout,
                                 0, 1, &set, 0, NULL);
   fw.vk.vkCmdDispatch(cb, GROUPS, 1, 1);

   /* The shader's writes have to be made available to the host domain.
    * horizon_gpu's fence increment flushes the GPU's dirty L2 on every
    * submit, so this would very likely read correctly without it — which
    * is exactly why it is here: the test should be measuring the
    * dispatch, not that one unconditional writeback happens to cover
    * for a missing dependency.
    */
   const VkMemoryBarrier mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   fw.vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_PIPELINE_STAGE_HOST_BIT, 0,
                              1, &mb, 0, NULL, 0, NULL);

   if (!vkfw_submit_and_wait(&fw, cb, "dispatch"))
      goto out;
   if (!vkfw_buffer_invalidate(&fw, &ssbo))
      goto out;

   /* --- what the GPU computed --------------------------------------- */
   {
      uint32_t *want = (uint32_t *)malloc(DISPATCH_WORDS * sizeof(uint32_t));
      if (!t_check(t, want != NULL, "host scratch for the expected words"))
         goto out;
      for (uint32_t i = 0; i < DISPATCH_WORDS; i++)
         want[i] = expect_word(i);

      vkfw_expect_words_array(&fw, ssbo.map, want, DISPATCH_WORDS,
                              "the dispatch computed every word");
      free(want);
   }

   vkfw_expect_words(&fw, (const uint32_t *)ssbo.map + DISPATCH_WORDS,
                     POISON, TAIL_WORDS,
                     "nothing was written past the last invocation");

out:
   if (pipeline != VK_NULL_HANDLE)
      fw.vk.vkDestroyPipeline(fw.dev, pipeline, NULL);
   if (pool != VK_NULL_HANDLE)
      fw.vk.vkDestroyDescriptorPool(fw.dev, pool, NULL);
   if (layout != VK_NULL_HANDLE)
      fw.vk.vkDestroyPipelineLayout(fw.dev, layout, NULL);
   if (set_layout != VK_NULL_HANDLE)
      fw.vk.vkDestroyDescriptorSetLayout(fw.dev, set_layout, NULL);
   if (module != VK_NULL_HANDLE)
      fw.vk.vkDestroyShaderModule(fw.dev, module, NULL);
   vkfw_buffer_destroy(&fw, &ssbo);
   vkfw_finish(&fw);
   return 0;
}
