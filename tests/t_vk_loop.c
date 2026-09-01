/*
 * A loop whose trip count comes from memory, in the shape the Godot 4.1
 * Forward+ scene shader gives its cluster loops. Measured on GM20B
 * (2026-08-28): with an upper bound of 0 the body still ran for every
 * fragment, ~250 ms per draw, and the first 3D frame died with the
 * host's "fifo idle timeout". This is that loop with nothing else around
 * it, so the compiler and the hardware can be asked directly.
 *
 * Four warps of 32 invocations, each lane with its own bound and its own
 * "am I in the loop at all" flag (act), which is how a fragment warp
 * with lanes outside the triangle looks to the SM. The CPU recomputes
 * every output word; the check lines say which pattern went wrong.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/vkfw.h"

/* Generated from tests/shaders/comp_loop.spvasm by scripts/spv-embed.py. */
#include "comp_loop.spv.h"

const char *const test_name = "t_vk_loop";
const bool test_uses_display = false;

#define LOCAL_SIZE_X 32u
#define GROUPS       4u
#define LANES        (LOCAL_SIZE_X * GROUPS)   /* 128 */
#define DATA_WORDS   64u

#define OFF_TO       0u
#define OFF_ACT      128u
#define OFF_N        256u
#define OFF_I        384u
#define OFF_DATA     512u
#define BUF_WORDS    (OFF_DATA + DATA_WORDS)
#define BUF_BYTES    (BUF_WORDS * 4u)

#define POISON       0xdeadbeefu

/* Exactly what the shader does, in C. */
static void expect_lane(const uint32_t *buf, uint32_t id,
                        uint32_t *n_out, uint32_t *i_out)
{
   uint32_t n = 0, i = 0;
   if (buf[OFF_ACT + id] != 0) {
      for (i = 0; i < buf[OFF_TO + id]; i++) {
         uint32_t mask = buf[OFF_DATA + ((i + id) & 63u)];
         while (mask != 0) {
            uint32_t bit = 31u - (uint32_t)__builtin_clz(mask);
            mask &= ~(1u << bit);
            if ((bit & 1u) == 0)
               continue;
            n += bit;
         }
         n += 1;
      }
   }
   *n_out = n;
   *i_out = i;
}

/* Group g (warp g) gets pattern g:
 *   0: every lane active, every bound 0        -- the Godot case
 *   1: lanes 0..7 active, every bound 0        -- partial warp, bound 0
 *   2: every lane active, bound = lane % 4     -- real, small trip counts
 *   3: lanes 0..11 active, bound = lane % 5    -- partial warp, real counts
 */
static void fill_inputs(uint32_t *buf)
{
   for (uint32_t id = 0; id < LANES; id++) {
      const uint32_t g = id / LOCAL_SIZE_X, lane = id % LOCAL_SIZE_X;
      uint32_t to = 0, act = 1;
      switch (g) {
      case 0: to = 0; act = 1; break;
      case 1: to = 0; act = lane < 8; break;
      case 2: to = lane % 4; act = 1; break;
      default: to = lane % 5; act = lane < 12; break;
      }
      buf[OFF_TO + id] = to;
      buf[OFF_ACT + id] = act;
      buf[OFF_N + id] = POISON;
      buf[OFF_I + id] = POISON;
   }
   /* Mask words with a few bits each, different per word, so a body that
    * ran at the wrong i shows up in n. */
   for (uint32_t k = 0; k < DATA_WORDS; k++)
      buf[OFF_DATA + k] = ((k * 2654435769u) >> 20) | 1u;
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
   uint32_t *want = NULL;

   if (!vkfw_buffer_create(&fw, BUF_BYTES,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &ssbo))
      goto out;

   const VkShaderModuleCreateInfo smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(comp_loop_spv),
      .pCode = comp_loop_spv,
   };
   VkResult r = fw.vk.vkCreateShaderModule(fw.dev, &smci, NULL, &module);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateShaderModule(%zu bytes) -> %s",
                sizeof(comp_loop_spv), vkfw_result_str(r)))
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
   if (!t_check(t, r == VK_SUCCESS, "vkCreateComputePipelines -> %s",
                vkfw_result_str(r)))
      goto out;

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

   /* --- inputs ------------------------------------------------------ */
   fill_inputs((uint32_t *)ssbo.map);
   if (!vkfw_buffer_flush(&fw, &ssbo))
      goto out;

   want = (uint32_t *)malloc(BUF_WORDS * sizeof(uint32_t));
   if (!t_check(t, want != NULL, "host scratch for the expected words"))
      goto out;
   memcpy(want, ssbo.map, BUF_BYTES);
   for (uint32_t id = 0; id < LANES; id++)
      expect_lane((const uint32_t *)ssbo.map, id,
                  &want[OFF_N + id], &want[OFF_I + id]);

   /* --- the dispatch ------------------------------------------------ */
   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(&fw, &cb))
      goto out;

   fw.vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   fw.vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout,
                                 0, 1, &set, 0, NULL);
   fw.vk.vkCmdDispatch(cb, GROUPS, 1, 1);

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

   /* --- what the GPU computed, one check line per pattern ---------- */
   {
      const uint32_t *got = (const uint32_t *)ssbo.map;
      static const char *const pattern_name[GROUPS] = {
         "all lanes active, every bound 0 (the Godot case)",
         "lanes 0..7 active, every bound 0",
         "all lanes active, bound = lane % 4",
         "lanes 0..11 active, bound = lane % 5",
      };
      for (uint32_t g = 0; g < GROUPS; g++) {
         const uint32_t base = g * LOCAL_SIZE_X;
         uint32_t bad_n = 0, bad_i = 0, first_bad = LANES;
         for (uint32_t l = 0; l < LOCAL_SIZE_X; l++) {
            const uint32_t id = base + l;
            if (got[OFF_N + id] != want[OFF_N + id]) {
               bad_n++;
               if (first_bad == LANES)
                  first_bad = id;
            }
            if (got[OFF_I + id] != want[OFF_I + id])
               bad_i++;
         }
         if (first_bad == LANES) {
            t_check(t, true, "warp %u: %s -> n and i right on all 32 lanes",
                    g, pattern_name[g]);
         } else {
            t_check(t, false,
                    "warp %u: %s -> %u lanes with wrong n, %u with wrong i; "
                    "lane %u: n=%u want %u, i=%u want %u",
                    g, pattern_name[g], bad_n, bad_i, first_bad - base,
                    got[OFF_N + first_bad], want[OFF_N + first_bad],
                    got[OFF_I + first_bad], want[OFF_I + first_bad]);
         }
      }
      /* A body that ran at the wrong index would have written nothing
       * else, but say so anyway. */
      vkfw_expect_words_array(&fw, got, want, OFF_N,
                              "the inputs were left alone");
      vkfw_expect_words_array(&fw, got + OFF_DATA, want + OFF_DATA,
                              DATA_WORDS, "the mask words were left alone");
   }

out:
   free(want);
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
