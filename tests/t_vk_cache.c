/*
 * Test 35 — is a shader compiled on one launch still compiled on the
 * next, or does it come back off the card?
 *
 * WHY THIS EXISTS SEPARATELY FROM t_shader_cache. That test proves the
 * store: blobs go in, blobs come out, damaged files recover. It says
 * nothing about the thing the feature is *for*. This one asks the only
 * question that matters to somebody running a game — whether NVK
 * recompiles — and it asks it of the shipping configuration: the real
 * driver, the real cache directory, no environment overrides.
 *
 * WHY IT NEEDS TWO LAUNCHES, AND WHY THAT CANNOT BE FAKED. Within one
 * process, vk_pipeline_cache answers the second identical
 * vkCreateComputePipelines out of memory, so a warm second call in the
 * same run measures a hash lookup and not the disk. The disk cache is
 * only observable across processes. So: the FIRST RUN OF THIS TEST ON A
 * GIVEN BUILD IS COLD AND THAT IS A PASS; run it twice and read the
 * `cold`/`warm` note.
 *
 * THE CHECK THAT MATTERS ON A WARM RUN IS NOT THE TIMING. A cache that
 * returned a corrupt shader would still create a pipeline and still be
 * fast. So the pipeline is dispatched and its output verified on every
 * run, warm or cold — a shader that came off the card has to compute the
 * same answer as one NAK just built.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

#include "common/testfw.h"
#include "common/vkfw.h"

/* Generated from tests/shaders/comp_write_id.spvasm by
 * scripts/spv-embed.py; the same shader t_vk_compute uses, so that a
 * failure here is about the cache and not about a new shader. */
#include "comp_write_id.spv.h"

const char *const test_name = "t_vk_cache";
const bool test_uses_display = false;

/* How many invocations the shader writes, and therefore how much there
 * is to check. Matches t_vk_compute so the expected output is the same
 * already-verified pattern. */
#define ELEMS 64u

/* Records that a previous launch of THIS test has run. It carries the
 * build id, because a different build has a different driver identity
 * and therefore an empty cache by design — without that, a rebuild
 * between runs would look like a cache that lost its entries. */
#define MARKER_PATH "sdmc:/horizon_gpu_tests/t_vk_cache_launch.txt"

static uint64_t now_us(void)
{
   return armTicksToNs(armGetSystemTick()) / 1000u;
}

/* What the previous launch of this build left behind, if any. Returns
 * true when this build has run here before. */
static bool marker_read(const char *build_id)
{
   char seen[256] = { 0 };
   FILE *f = fopen(MARKER_PATH, "rb");
   size_t n;

   if (f == NULL)
      return false;
   n = fread(seen, 1, sizeof(seen) - 1, f);
   fclose(f);
   seen[n] = '\0';

   return strcmp(seen, build_id) == 0;
}

static void marker_write(const char *build_id)
{
   FILE *f = fopen(MARKER_PATH, "wb");

   if (f == NULL)
      return;
   fwrite(build_id, 1, strlen(build_id), f);
   fclose(f);
}

int run_test(test_ctx *t)
{
   vkfw fw;
   VkShaderModule module = VK_NULL_HANDLE;
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   VkDescriptorPool desc_pool = VK_NULL_HANDLE;
   vkfw_buffer ssbo = { 0 };
   uint64_t init_us = 0, pipeline_us = 0;

   mkdir("sdmc:/horizon_gpu_tests", 0777);

   /* The stamp gen-build-id.sh put in this .nro — the same string the
    * second line of this log carries. */
   const char *build_id = t_build_id();
   bool warm = marker_read(build_id);

   t_note(t, "this launch is %s for build %s",
          warm ? "WARM — this build has run here before"
               : "COLD — first run of this build on this console, "
                 "which is a pass",
          build_id);

   /* No MESA_SHADER_CACHE_DIR: the point is the path the driver uses on
    * its own. MESA_SHADER_CACHE_SHOW_STATS makes disk_cache_destroy
    * print its hit/miss counts at instance teardown, which is the
    * driver's own account of what happened and is read back below. */
   setenv("MESA_SHADER_CACHE_SHOW_STATS", "1", 1);

   uint64_t t0 = now_us();
   if (!vkfw_init(&fw, t, NULL))
      return 1;
   init_us = now_us() - t0;
   t_note(t, "vkfw_init (instance + device + the disk cache being opened) "
             "took %llu us", (unsigned long long)init_us);

   if (!vkfw_buffer_create(&fw, ELEMS * sizeof(uint32_t),
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &ssbo))
      goto out;

   const VkShaderModuleCreateInfo smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(comp_write_id_spv),
      .pCode = comp_write_id_spv,
   };
   VkResult r = fw.vk.vkCreateShaderModule(fw.dev, &smci, NULL, &module);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateShaderModule -> %s",
                vkfw_result_str(r)))
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

   /* THE MEASUREMENT. On a cold cache this is NAK compiling; on a warm
    * one it should be a read from the card and a relocation. No
    * VkPipelineCache is passed, so nothing in this process can answer it
    * except the disk. */
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
   t0 = now_us();
   r = fw.vk.vkCreateComputePipelines(fw.dev, VK_NULL_HANDLE, 1, &cpci,
                                      NULL, &pipeline);
   pipeline_us = now_us() - t0;
   if (!t_check(t, r == VK_SUCCESS, "vkCreateComputePipelines -> %s",
                vkfw_result_str(r)))
      goto out;

   t_note(t, "vkCreateComputePipelines took %llu us on a %s cache",
          (unsigned long long)pipeline_us, warm ? "warm" : "cold");

   /* --- the descriptor and the dispatch ----------------------------- */
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
   r = fw.vk.vkCreateDescriptorPool(fw.dev, &dpci, NULL, &desc_pool);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorPool -> %s",
                vkfw_result_str(r)))
      goto out;

   VkDescriptorSet set = VK_NULL_HANDLE;
   const VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = desc_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout,
   };
   r = fw.vk.vkAllocateDescriptorSets(fw.dev, &dsai, &set);
   if (!t_check(t, r == VK_SUCCESS, "vkAllocateDescriptorSets -> %s",
                vkfw_result_str(r)))
      goto out;

   const VkDescriptorBufferInfo dbi = {
      .buffer = ssbo.buf, .offset = 0, .range = VK_WHOLE_SIZE,
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

   if (!vkfw_buffer_poison(&fw, &ssbo, 0xDEADBEEFu))
      goto out;

   VkCommandBuffer cb = VK_NULL_HANDLE;
   if (!vkfw_cmd_begin(&fw, &cb))
      goto out;
   fw.vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   fw.vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout,
                                 0, 1, &set, 0, NULL);
   fw.vk.vkCmdDispatch(cb, ELEMS, 1, 1);
   if (!vkfw_submit_and_wait(&fw, cb, "the cached shader's dispatch"))
      goto out;

   if (!vkfw_buffer_invalidate(&fw, &ssbo))
      goto out;

   /* THE CHECK THAT MATTERS. A cache that handed back a corrupt or
    * stale binary would have created a pipeline just as happily, and
    * faster. What it cannot do is compute the right answer. */
   uint32_t expect[ELEMS];
   for (uint32_t i = 0; i < ELEMS; i++)
      expect[i] = i;
   uint32_t bad = vkfw_expect_words_array(&fw, ssbo.map, expect, ELEMS,
                                          "the cached shader's output");
   t_check(t, bad == 0,
           "the shader computes the right answer on a %s cache "
           "(%u of %u words wrong)", warm ? "warm" : "cold", bad, ELEMS);

out:
   fw.vk.vkDeviceWaitIdle(fw.dev);
   if (desc_pool != VK_NULL_HANDLE)
      fw.vk.vkDestroyDescriptorPool(fw.dev, desc_pool, NULL);
   if (pipeline != VK_NULL_HANDLE)
      fw.vk.vkDestroyPipeline(fw.dev, pipeline, NULL);
   if (layout != VK_NULL_HANDLE)
      fw.vk.vkDestroyPipelineLayout(fw.dev, layout, NULL);
   if (set_layout != VK_NULL_HANDLE)
      fw.vk.vkDestroyDescriptorSetLayout(fw.dev, set_layout, NULL);
   if (module != VK_NULL_HANDLE)
      fw.vk.vkDestroyShaderModule(fw.dev, module, NULL);
   vkfw_buffer_destroy(&fw, &ssbo);

   /* The driver prints its hit/miss counts from disk_cache_destroy(),
    * which runs inside vkDestroyInstance — so the line does not exist
    * until the fixture is torn down, and it can only be read after. */
   vkfw_finish(&fw);

   bool found = false;
   bool scanned = t_log_scan(t, "disk shader cache:", &found);
   t_check(t, scanned, "the log could be scanned for the driver's own "
                       "account of the cache");
   t_check(t, !scanned || found,
           "the driver reported its disk cache hit/miss counts — i.e. it "
           "had a disk cache at all");

   if (warm) {
      /* On a warm run NVK must have found the shader. "hits = 0" is the
       * exact string a cache that did nothing would print. */
      bool zero_hits = false;
      if (t_log_scan(t, "hits = 0", &zero_hits))
         t_check(t, !zero_hits,
                 "a warm run reports a hit, not 'hits = 0' — the shader "
                 "came off the card instead of being recompiled");
   } else {
      t_note(t, "cold run: nothing to hit yet. Run this test a second "
                "time, with the same build, and the check above becomes "
                "a real one.");
   }

   marker_write(build_id);
   t_note(t, "%s records this build; delete it to make the next run cold "
             "again", MARKER_PATH);

   return 0;
}
