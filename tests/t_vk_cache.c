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

/* The shader's geometry, and it is NOT a free choice — run 28 got this
 * wrong in both directions at once.
 *
 * comp_write_id declares `OpExecutionMode %main LocalSize 64 1 1`, so a
 * dispatch of N groups runs 64*N invocations, each storing at its own
 * global id. Run 28 dispatched 64 groups into a 64-word buffer: 4096
 * invocations, 4032 of them writing past the end of a runtime array.
 * These are t_vk_compute's numbers, which are the shader's.
 *
 * TAIL_WORDS is the detector for exactly that mistake: 64 words past
 * everything the dispatch may touch, which must still hold the poison
 * afterwards. */
#define LOCAL_SIZE_X   64u
#define DISPATCH_WORDS 4096u
#define GROUPS         (DISPATCH_WORDS / LOCAL_SIZE_X)
#define TAIL_WORDS     64u
#define BUF_WORDS      (DISPATCH_WORDS + TAIL_WORDS)
#define BUF_BYTES      (BUF_WORDS * 4u)
#define POISON         0xdeadbeefu

/* What comp_write_id stores at index `id`.
 *
 * From the shader itself, which documents it in its own header:
 *
 *     out[id] = (id * 2654435769) ^ 2781138957      (mod 2^32)
 *
 * and which t_vk_compute computes the same way. Run 28 assumed
 * `out[id] = id` without reading either, got 0xa5c4d00d back from the
 * console, and reported it as a cache that had returned a wrong shader.
 * 0xa5c4d00d is expect_word(0) — the multiply vanishes at id 0 and the
 * XOR constant is all that is left. The console was right and the test
 * was wrong, which is the most expensive kind of test failure there is,
 * so there is now a static assertion on that exact value below. */
static uint32_t expect_word(uint32_t id)
{
   return (id * 2654435769u) ^ 2781138957u;
}

/* Records that a previous launch of THIS test has run. It carries the
 * build id, because a different build has a different driver identity
 * and therefore an empty cache by design — without that, a rebuild
 * between runs would look like a cache that lost its entries. */
#define MARKER_PATH "sdmc:/horizon_gpu_tests/t_vk_cache_launch.txt"

static uint64_t now_us(void)
{
   return armTicksToNs(armGetSystemTick()) / 1000u;
}

/* The marker carries the build stamp AND the cold pipeline time, so a
 * warm run can print the comparison instead of asking whoever is holding
 * the console to keep two logs side by side. One line:
 *
 *     <cold vkCreateComputePipelines us> <build id>
 *
 * Returns true when this build has run here before, and fills
 * *cold_us_out with what it cost that time. */
static bool marker_read(const char *build_id, uint64_t *cold_us_out)
{
   char seen[320] = { 0 };
   FILE *f = fopen(MARKER_PATH, "rb");
   size_t n;

   *cold_us_out = 0;

   if (f == NULL)
      return false;
   n = fread(seen, 1, sizeof(seen) - 1, f);
   fclose(f);
   seen[n] = '\0';

   /* "<us> <build id>". A marker written by an older revision has no
    * number and no space, and reads as a different build — cold, which
    * is the safe answer. */
   char *space = strchr(seen, ' ');
   if (space == NULL)
      return false;
   *space = '\0';
   *cold_us_out = strtoull(seen, NULL, 10);

   return strcmp(space + 1, build_id) == 0;
}

static void marker_write(const char *build_id, uint64_t cold_us)
{
   FILE *f = fopen(MARKER_PATH, "wb");

   if (f == NULL)
      return;
   fprintf(f, "%llu %s", (unsigned long long)cold_us, build_id);
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
   uint64_t cold_us = 0;
   bool warm = marker_read(build_id, &cold_us);

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

   /* WAS THE DRIVER'S CACHE ACTUALLY EMPTY? The marker file only says
    * whether *this test* has run before, and those are different
    * questions: anything else that ran this driver — another test, a
    * homebrew — leaves the same shader in sdmc:/mesa_shader_cache. A
    * "cold" baseline measured against a cache that already held the
    * entry is a warm number wearing a cold label, and every ratio
    * derived from it afterwards is wrong.
    *
    * So the question is put to the driver, which announces what it
    * opened: "disk cache: <path>, N entries". Its own words, in this
    * log, before anything was asked of it. */
   bool cache_empty = false;
   bool scanned_open = t_log_scan(t, ", 0 entries", &cache_empty);
   t_check(t, scanned_open,
           "the log could be scanned for what the driver opened");
   t_note(t, "the driver's own cache was %s when this launch started",
          cache_empty ? "EMPTY" : "already populated");

   if (!vkfw_buffer_create(&fw, BUF_BYTES,
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

   if (warm && cold_us > 0) {
      /* THE NUMBER THE WHOLE FEATURE IS FOR, in one line of one log. */
      t_note(t, "against %llu us when this build's cache was cold: "
                "%llu us saved, %llu%% of the compile",
             (unsigned long long)cold_us,
             (unsigned long long)(cold_us > pipeline_us
                                     ? cold_us - pipeline_us : 0),
             (unsigned long long)(cold_us > 0
                                     ? (cold_us - pipeline_us) * 100 / cold_us
                                     : 0));
      t_check(t, pipeline_us < cold_us,
              "a warm cache creates the pipeline faster than a cold one "
              "(%llu us against %llu us)",
              (unsigned long long)pipeline_us, (unsigned long long)cold_us);
   }

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

   if (!vkfw_buffer_poison(&fw, &ssbo, POISON))
      goto out;

   VkCommandBuffer cb = VK_NULL_HANDLE;
   if (!vkfw_cmd_begin(&fw, &cb))
      goto out;
   fw.vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   fw.vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout,
                                 0, 1, &set, 0, NULL);
   fw.vk.vkCmdDispatch(cb, GROUPS, 1, 1);
   if (!vkfw_submit_and_wait(&fw, cb, "the cached shader's dispatch"))
      goto out;

   if (!vkfw_buffer_invalidate(&fw, &ssbo))
      goto out;

   /* THE CHECK THAT MATTERS. A cache that handed back a corrupt or
    * stale binary would have created a pipeline just as happily, and
    * faster. What it cannot do is compute the right answer.
    *
    * The self-check first: a wrong expectation here would look exactly
    * like a wrong shader, and in run 28 it did. */
   t_check(t, expect_word(0) == 0xa5c4d00du,
           "the expected-value function matches the shader "
           "(expect_word(0) = 0x%08x, must be 0xa5c4d00d)", expect_word(0));

   uint32_t *want = malloc(DISPATCH_WORDS * sizeof(uint32_t));
   if (t_check(t, want != NULL, "host scratch for the expected words")) {
      for (uint32_t i = 0; i < DISPATCH_WORDS; i++)
         want[i] = expect_word(i);
      uint32_t bad = vkfw_expect_words_array(&fw, ssbo.map, want,
                                             DISPATCH_WORDS,
                                             "the cached shader's output");
      t_check(t, bad == 0,
              "the shader computes the right answer on a %s cache "
              "(%u of %u words wrong)", warm ? "warm" : "cold", bad,
              DISPATCH_WORDS);
      free(want);
   }

   /* And nothing beyond what it was asked to write. This is the check
    * run 28 did not have, and it is the one that would have named the
    * dispatch size rather than blaming the cache. */
   uint32_t tail_bad =
      vkfw_expect_words(&fw, (const uint32_t *)ssbo.map + DISPATCH_WORDS,
                        POISON, TAIL_WORDS,
                        "the words past the dispatch are untouched");
   t_check(t, tail_bad == 0,
           "the shader wrote nothing past its last invocation "
           "(%u of %u tail words changed)", tail_bad, TAIL_WORDS);

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

   if (warm && cold_us == 0)
      t_note(t, "no cold baseline on record, so the saving is not "
                "reported. Delete %s AND sdmc:/mesa_shader_cache, then run "
                "twice.", MARKER_PATH);

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

   /* What the next run compares against.
    *
    * A cold time is only recorded when the driver's cache was genuinely
    * empty — otherwise the number is a warm create with a cold label,
    * and it would make every later ratio a lie. Recording 0 instead
    * means the next run simply has nothing to compare with and says so,
    * which is the honest failure. */
   uint64_t record_us = cold_us;
   if (!warm) {
      record_us = cache_empty ? pipeline_us : 0;
      if (!cache_empty)
         t_note(t, "NOT recording a cold baseline: the driver's cache "
                   "already held entries, so %llu us is not the cost of a "
                   "compile. Delete sdmc:/mesa_shader_cache and run again.",
                (unsigned long long)pipeline_us);
   }
   marker_write(build_id, record_us);
   t_note(t, "%s records this build. To measure cold again, delete it AND "
             "sdmc:/mesa_shader_cache — the driver's cache is what makes a "
             "create cheap, and the marker only remembers that this test "
             "ran.", MARKER_PATH);

   return 0;
}
