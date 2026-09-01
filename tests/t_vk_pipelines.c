/*
 * Pipeline and shader-heap stress — many distinct compute shaders, one
 * after another, and what it costs to compile each of them.
 *
 * WHAT WAS NOT COVERED BEFORE THIS. t_vk_compute creates one compute
 * pipeline from one shader and dispatches it. Every path this driver has
 * for a *second* distinct shader was therefore unmeasured: NVK's shader
 * heap is contiguous on pre-Volta, it reserves its whole range up front
 * and binds one 64 KiB chunk at device creation (patch 0030), and it
 * grows by binding further chunks into that reservation as shaders are
 * uploaded. Nothing had ever made it grow. Nor had anything freed a
 * shader and allocated another over the same address.
 *
 * WHY THE SHADERS HAVE TO BE DIFFERENT, and how they are made so. NVK
 * keys a compiled shader on the hash of its SPIR-V *and* its
 * specialization, so a hundred pipelines from identical inputs are one
 * compile and one upload — they would measure the pipeline-creation
 * bookkeeping and nothing else. comp_chain.spvasm folds a specialization
 * constant into a 448-instruction dependent chain, so every value of
 * that constant is a different compile, a different cache entry and a
 * different allocation out of the heap.
 *
 * HOW MUCH HEAP THAT IS, as arithmetic rather than a hope. SM50
 * instructions are 8 bytes with one control word per three, so 448
 * arithmetic instructions is at least 448 * 10.67 = 4.7 KiB of machine
 * code before the prologue, the store and the 2 KiB every shader BO is
 * overallocated by. PIPELINE_COUNT of those is far past the 64 KiB the
 * heap starts with — section A cannot pass without the heap having grown
 * several times.
 *
 * WHAT EACH SECTION ASKS:
 *
 *   A  PIPELINE_COUNT distinct pipelines, created back to back, then all
 *      dispatched from ONE command buffer into disjoint regions of one
 *      buffer, and every word checked against the same chain recomputed
 *      in C. A shader placed at the wrong heap address does not fail
 *      quietly here: it computes the wrong words, or it faults.
 *   B  Churn. Rounds of create-N, dispatch, verify, destroy-N, so the
 *      heap's free list is exercised rather than only its growth.
 *   C  What A's compiles cost, reported as a distribution. This is the
 *      stutter an application feels the first time it draws with a
 *      shader it has not seen; the mean hides it and the maximum is the
 *      frame that dropped.
 *   D  The same PIPELINE_COUNT specializations a second time, in the
 *      same process. Cold-versus-warm across *launches* is t_vk_cache's
 *      question; this one is only about what an in-process cache does,
 *      and it is reported as a ratio so it needs no baseline from
 *      another run.
 *   E  One more dispatch after everything else has been destroyed. A
 *      heap whose bookkeeping was corrupted by B usually still answers
 *      allocations; it stops computing the right thing.
 *
 * NO SEPARATE "LARGE SHADER" SECTION, and that is a limit worth stating
 * rather than hiding: a single shader whose machine code exceeds the
 * 64 KiB first chunk would need roughly 6000 SPIR-V instructions, which
 * is a generated file an order of magnitude larger than this one and a
 * different question (arena sizing, not heap growth). comp_chain is
 * large enough to be a real compile and no larger.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "common/vkfw.h"

/* Generated from tests/shaders/comp_chain.spvasm by
 * scripts/spv-embed.py; assembled by spirv-as and validated by
 * spirv-val at build time. */
#include "comp_chain.spv.h"

const char *const test_name = "t_vk_pipelines";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

/* Must match OpExecutionMode LocalSize in comp_chain.spvasm. */
#define LOCAL_SIZE_X    64u
/* Must match the block count in comp_chain.spvasm. The C recomputation
 * below is the shader's loop, so a disagreement is a wrong answer on
 * every word rather than an error. */
#define CHAIN_BLOCKS    64u

/* Per pipeline: one dispatch of this many invocations into its own
 * region of the shared buffer. Small on purpose — what is being measured
 * is that each shader is compiled and placed correctly, not throughput,
 * and PIPELINE_COUNT dispatches have to fit one command buffer. */
#define REGION_WORDS    256u
#define GROUPS          (REGION_WORDS / LOCAL_SIZE_X)

/* Enough distinct shaders to take the heap well past its first chunk;
 * see the arithmetic in the header. */
#define PIPELINE_COUNT  96u

/* Section B: rounds of this many, created and destroyed together. */
#define CHURN_ROUNDS    16u
#define CHURN_BATCH     8u

#define POISON          0xdeadbeefu

/* The specialization values sections A, B and D use. Kept apart so a
 * failure names which section's shader it was, and so section B cannot
 * be satisfied by a compile section A already did. */
#define SPEC_A(i)       (0x1000u + (i))
#define SPEC_B(r, k)    (0x2000u + (r) * CHURN_BATCH + (k))

/* comp_chain.spvasm's chain, in C. uint32_t wraps, which is what the
 * SPIR-V integer ops do on a 32-bit unsigned. */
static uint32_t expect_word(uint32_t id, uint32_t spec)
{
   uint32_t acc = id;
   for (uint32_t b = 0; b < CHAIN_BLOCKS; b++) {
      acc = acc * 2654435769u + spec;
      acc ^= acc >> 13;
      acc += acc << 7;
      acc ^= 2781138957u;
   }
   return acc;
}

/* Everything a pipeline needs that is the same for all of them. */
struct fixture {
   VkShaderModule module;
   VkDescriptorSetLayout set_layout;
   VkPipelineLayout layout;
   VkDescriptorPool pool;
   vkfw_buffer ssbo;
   /* Byte stride between two pipelines' regions: REGION_WORDS * 4
    * rounded up to minStorageBufferOffsetAlignment, which is a device
    * limit and not a number this test may assume. */
   VkDeviceSize region_stride_B;
   /* One descriptor set per region, so a dispatch writes where its
    * index says and nowhere else. */
   VkDescriptorSet sets[PIPELINE_COUNT];
};

static uint64_t align_up_u64(uint64_t v, uint64_t a)
{
   return a <= 1 ? v : ((v + a - 1) / a) * a;
}

/* Creates one pipeline specialized to `spec`, and reports how long
 * vkCreateComputePipelines took in `out_ns` when that is non-NULL.
 *
 * The VkSpecializationInfo and its map entry are per call and live on
 * the stack: vkCreateComputePipelines consumes them before it returns,
 * and a static one shared between calls would be a data race the moment
 * this test grew a thread. */
static bool make_pipeline(vkfw *fw, const struct fixture *fx, uint32_t spec,
                          const char *what, VkPipeline *out,
                          uint64_t *out_ns)
{
   const VkSpecializationMapEntry entry = {
      .constantID = 0,
      .offset = 0,
      .size = sizeof(uint32_t),
   };
   const VkSpecializationInfo si = {
      .mapEntryCount = 1,
      .pMapEntries = &entry,
      .dataSize = sizeof(spec),
      .pData = &spec,
   };
   const VkComputePipelineCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = fx->module,
         .pName = "main",
         .pSpecializationInfo = &si,
      },
      .layout = fx->layout,
   };

   const u64 t0 = armGetSystemTick();
   VkResult r = fw->vk.vkCreateComputePipelines(fw->dev, VK_NULL_HANDLE, 1,
                                                &cpci, NULL, out);
   const uint64_t ns = armTicksToNs(armGetSystemTick() - t0);
   if (out_ns)
      *out_ns = ns;

   if (r != VK_SUCCESS) {
      /* One check line, and only on failure: PIPELINE_COUNT passing
       * check lines would bury everything else in the log. The count is
       * asserted once by the caller instead. */
      t_check(fw->t, false, "%s: vkCreateComputePipelines(spec=0x%x) -> %s",
              what, spec, vkfw_result_str(r));
      *out = VK_NULL_HANDLE;
      return false;
   }
   return true;
}

/* Records `count` dispatches — pipeline i into region i — submits them
 * as one command buffer and waits. Regions are disjoint, so the
 * dispatches have no dependency on each other and need no barrier
 * between them; the one barrier is the shaders' writes to the host.
 *
 * `set_base` is where in fx->sets the regions start, so section B can
 * use the first CHURN_BATCH of them repeatedly. */
static bool dispatch_all(vkfw *fw, const struct fixture *fx,
                         const VkPipeline *pipelines, uint32_t count,
                         uint32_t set_base, const char *what)
{
   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(fw, &cb))
      return false;

   for (uint32_t i = 0; i < count; i++) {
      fw->vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                               pipelines[i]);
      fw->vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     fx->layout, 0, 1,
                                     &fx->sets[set_base + i], 0, NULL);
      fw->vk.vkCmdDispatch(cb, GROUPS, 1, 1);
   }

   const VkMemoryBarrier mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   fw->vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_HOST_BIT, 0,
                               1, &mb, 0, NULL, 0, NULL);

   return vkfw_submit_and_wait(fw, cb, what);
}

/* Checks region `i` against the chain recomputed for `spec`. Returns the
 * number of words that differ and passes NO check of its own — the
 * caller reports once for the whole set, because one line per region is
 * PIPELINE_COUNT lines saying the same thing. */
static uint32_t region_wrong(const struct fixture *fx, uint32_t region,
                             uint32_t spec)
{
   const uint8_t *base = (const uint8_t *)fx->ssbo.map +
                         region * fx->region_stride_B;
   const uint32_t *got = (const uint32_t *)(const void *)base;

   uint32_t wrong = 0;
   for (uint32_t w = 0; w < REGION_WORDS; w++) {
      if (got[w] != expect_word(w, spec))
         wrong++;
   }
   return wrong;
}

static int cmp_u64(const void *a, const void *b)
{
   const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
   return x < y ? -1 : (x > y ? 1 : 0);
}

/* min / mean / max / p99 of `n` durations, in microseconds. Sorts a copy
 * the caller owns; `ns` is modified. */
static void report_times(test_ctx *t, const char *what, uint64_t *ns,
                         uint32_t n)
{
   if (n == 0)
      return;
   qsort(ns, n, sizeof(*ns), cmp_u64);

   uint64_t total = 0;
   for (uint32_t i = 0; i < n; i++)
      total += ns[i];

   /* p99 of a sample this small is the largest element for n < 100; it
    * is reported anyway because the index is what makes the number
    * comparable when PIPELINE_COUNT changes. */
   const uint32_t p99 = (n * 99u) / 100u;
   t_note(t, "%s: %u compiles, min %" PRIu64 " us, mean %" PRIu64
             " us, p99 %" PRIu64 " us, max %" PRIu64 " us, total %"
             PRIu64 " ms",
          what, n, ns[0] / 1000u, total / n / 1000u,
          ns[p99 < n ? p99 : n - 1] / 1000u, ns[n - 1] / 1000u,
          total / 1000000u);
}

static void destroy_pipelines(vkfw *fw, VkPipeline *p, uint32_t n)
{
   for (uint32_t i = 0; i < n; i++) {
      if (p[i] != VK_NULL_HANDLE) {
         fw->vk.vkDestroyPipeline(fw->dev, p[i], NULL);
         p[i] = VK_NULL_HANDLE;
      }
   }
}

static bool fixture_init(vkfw *fw, struct fixture *fx)
{
   test_ctx *t = fw->t;

   VkPhysicalDeviceProperties props;
   fw->vk.vkGetPhysicalDeviceProperties(fw->pdev, &props);
   fx->region_stride_B =
      align_up_u64((uint64_t)REGION_WORDS * 4u,
                   props.limits.minStorageBufferOffsetAlignment);
   t_note(t, "region stride %" PRIu64 " B (minStorageBufferOffsetAlignment "
             "%" PRIu64 "), %u regions",
          (uint64_t)fx->region_stride_B,
          (uint64_t)props.limits.minStorageBufferOffsetAlignment,
          PIPELINE_COUNT);

   const VkDeviceSize buf_B = fx->region_stride_B * PIPELINE_COUNT;
   if (!vkfw_buffer_create(fw, buf_B,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &fx->ssbo))
      return false;

   const VkShaderModuleCreateInfo smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(comp_chain_spv),
      .pCode = comp_chain_spv,
   };
   VkResult r = fw->vk.vkCreateShaderModule(fw->dev, &smci, NULL,
                                            &fx->module);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateShaderModule(%zu bytes) -> %s",
                sizeof(comp_chain_spv), vkfw_result_str(r)))
      return false;

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
   r = fw->vk.vkCreateDescriptorSetLayout(fw->dev, &dslci, NULL,
                                          &fx->set_layout);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorSetLayout -> %s",
                vkfw_result_str(r)))
      return false;

   const VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &fx->set_layout,
   };
   r = fw->vk.vkCreatePipelineLayout(fw->dev, &plci, NULL, &fx->layout);
   if (!t_check(t, r == VK_SUCCESS, "vkCreatePipelineLayout -> %s",
                vkfw_result_str(r)))
      return false;

   const VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = PIPELINE_COUNT,
   };
   const VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = PIPELINE_COUNT,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   r = fw->vk.vkCreateDescriptorPool(fw->dev, &dpci, NULL, &fx->pool);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorPool(%u sets) -> %s",
                PIPELINE_COUNT, vkfw_result_str(r)))
      return false;

   for (uint32_t i = 0; i < PIPELINE_COUNT; i++) {
      const VkDescriptorSetAllocateInfo dsai = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = fx->pool,
         .descriptorSetCount = 1,
         .pSetLayouts = &fx->set_layout,
      };
      r = fw->vk.vkAllocateDescriptorSets(fw->dev, &dsai, &fx->sets[i]);
      if (r != VK_SUCCESS) {
         t_check(t, false, "vkAllocateDescriptorSets(%u of %u) -> %s",
                 i, PIPELINE_COUNT, vkfw_result_str(r));
         return false;
      }

      const VkDescriptorBufferInfo dbi = {
         .buffer = fx->ssbo.buf,
         .offset = i * fx->region_stride_B,
         .range = (VkDeviceSize)REGION_WORDS * 4u,
      };
      const VkWriteDescriptorSet write = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = fx->sets[i],
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &dbi,
      };
      fw->vk.vkUpdateDescriptorSets(fw->dev, 1, &write, 0, NULL);
   }
   t_note(t, "%u descriptor sets, one region each", PIPELINE_COUNT);

   return true;
}

static void fixture_finish(vkfw *fw, struct fixture *fx)
{
   if (fx->pool != VK_NULL_HANDLE)
      fw->vk.vkDestroyDescriptorPool(fw->dev, fx->pool, NULL);
   if (fx->layout != VK_NULL_HANDLE)
      fw->vk.vkDestroyPipelineLayout(fw->dev, fx->layout, NULL);
   if (fx->set_layout != VK_NULL_HANDLE)
      fw->vk.vkDestroyDescriptorSetLayout(fw->dev, fx->set_layout, NULL);
   if (fx->module != VK_NULL_HANDLE)
      fw->vk.vkDestroyShaderModule(fw->dev, fx->module, NULL);
   vkfw_buffer_destroy(fw, &fx->ssbo);
}

int run_test(test_ctx *t)
{
   vkfw fw;
   if (!vkfw_init(&fw, t, NULL))
      return 1;

   struct fixture fx;
   memset(&fx, 0, sizeof(fx));

   VkPipeline pipelines[PIPELINE_COUNT];
   memset(pipelines, 0, sizeof(pipelines));
   uint64_t *cold_ns = NULL, *warm_ns = NULL;

   if (!fixture_init(&fw, &fx))
      goto out;

   cold_ns = (uint64_t *)calloc(PIPELINE_COUNT, sizeof(uint64_t));
   warm_ns = (uint64_t *)calloc(PIPELINE_COUNT, sizeof(uint64_t));
   if (!t_check(t, cold_ns != NULL && warm_ns != NULL,
                "host scratch for the compile times"))
      goto out;

   /* --- A: many distinct pipelines, then all of them at once -------- */
   t_note(t, "A: %u distinct compute pipelines, %u-instruction chain each",
          PIPELINE_COUNT, CHAIN_BLOCKS * 7u);
   {
      uint64_t created = 0;
      const u64 a0 = armGetSystemTick();
      for (uint32_t i = 0; i < PIPELINE_COUNT; i++) {
         if (!make_pipeline(&fw, &fx, SPEC_A(i), "A", &pipelines[i],
                            &cold_ns[i]))
            break;
         created++;
      }
      const uint64_t a_ns = armTicksToNs(armGetSystemTick() - a0);
      if (!t_check(t, created == PIPELINE_COUNT,
                   "A: %" PRIu64 " of %u pipelines created in %" PRIu64
                   " ms", created, PIPELINE_COUNT, a_ns / 1000000u))
         goto out;

      if (!vkfw_buffer_poison(&fw, &fx.ssbo, POISON))
         goto out;
      if (!dispatch_all(&fw, &fx, pipelines, PIPELINE_COUNT, 0,
                        "A: one command buffer, every pipeline once"))
         goto out;
      if (!vkfw_buffer_invalidate(&fw, &fx.ssbo))
         goto out;

      uint32_t bad_regions = 0, first_bad = 0, first_wrong = 0;
      for (uint32_t i = 0; i < PIPELINE_COUNT; i++) {
         const uint32_t wrong = region_wrong(&fx, i, SPEC_A(i));
         if (wrong != 0) {
            if (bad_regions == 0) {
               first_bad = i;
               first_wrong = wrong;
            }
            bad_regions++;
         }
      }
      t_check(t, bad_regions == 0,
              "A: every one of the %u shaders computed its whole region "
              "(%u region(s) wrong; first is %u with %u of %u words)",
              PIPELINE_COUNT, bad_regions, first_bad, first_wrong,
              REGION_WORDS);
   }

   /* --- B: churn ---------------------------------------------------- */
   t_note(t, "B: %u rounds of create-%u / dispatch / destroy-%u",
          CHURN_ROUNDS, CHURN_BATCH, CHURN_BATCH);
   {
      VkPipeline batch[CHURN_BATCH];
      memset(batch, 0, sizeof(batch));
      uint32_t bad_rounds = 0, first_bad_round = 0;

      for (uint32_t round = 0; round < CHURN_ROUNDS; round++) {
         bool ok = true;
         for (uint32_t k = 0; k < CHURN_BATCH && ok; k++)
            ok = make_pipeline(&fw, &fx, SPEC_B(round, k), "B", &batch[k],
                               NULL);
         if (!ok) {
            destroy_pipelines(&fw, batch, CHURN_BATCH);
            goto churn_done;
         }

         if (!vkfw_buffer_poison(&fw, &fx.ssbo, POISON) ||
             !dispatch_all(&fw, &fx, batch, CHURN_BATCH, 0,
                           "B: a round's batch") ||
             !vkfw_buffer_invalidate(&fw, &fx.ssbo)) {
            destroy_pipelines(&fw, batch, CHURN_BATCH);
            goto churn_done;
         }

         uint32_t wrong = 0;
         for (uint32_t k = 0; k < CHURN_BATCH; k++)
            wrong += region_wrong(&fx, k, SPEC_B(round, k));
         if (wrong != 0) {
            if (bad_rounds == 0)
               first_bad_round = round;
            bad_rounds++;
         }

         /* Destroyed before the next round, which is the point: the
          * next round's shaders are allocated over addresses this
          * round's just gave back. */
         destroy_pipelines(&fw, batch, CHURN_BATCH);
      }

      t_check(t, bad_rounds == 0,
              "B: %u rounds x %u shaders over reused heap addresses, all "
              "correct (%u bad round(s), first is %u)",
              CHURN_ROUNDS, CHURN_BATCH, bad_rounds, first_bad_round);
   }
churn_done:

   /* --- C: what the compiles cost ----------------------------------- */
   report_times(t, "C: cold compile", cold_ns, PIPELINE_COUNT);

   /* --- D: the same specializations again, in the same process ------ */
   {
      VkPipeline again[PIPELINE_COUNT];
      memset(again, 0, sizeof(again));
      uint32_t created = 0;
      for (uint32_t i = 0; i < PIPELINE_COUNT; i++) {
         if (!make_pipeline(&fw, &fx, SPEC_A(i), "D", &again[i], &warm_ns[i]))
            break;
         created++;
      }
      if (t_check(t, created == PIPELINE_COUNT,
                  "D: the same %u specializations built a second time",
                  PIPELINE_COUNT)) {
         uint64_t cold_total = 0, warm_total = 0;
         for (uint32_t i = 0; i < PIPELINE_COUNT; i++) {
            cold_total += cold_ns[i];
            warm_total += warm_ns[i];
         }
         /* Reported as a ratio in per-mille so it needs no baseline
          * from another run, and stated in both directions: a second
          * build that is NOT cheaper is a finding, not a failure — it
          * would mean nothing in this process remembers a compiled
          * shader. */
         const uint64_t permille =
            cold_total != 0 ? (warm_total * 1000u) / cold_total : 0;
         t_note(t, "D: second build cost %" PRIu64 " ms against the first's "
                   "%" PRIu64 " ms — %" PRIu64 ".%01" PRIu64 "%% of it",
                warm_total / 1000000u, cold_total / 1000000u,
                permille / 10u, permille % 10u);
         report_times(t, "D: warm compile", warm_ns, PIPELINE_COUNT);
      }
      destroy_pipelines(&fw, again, PIPELINE_COUNT);
   }

   /* --- E: the heap still works ------------------------------------- */
   destroy_pipelines(&fw, pipelines, PIPELINE_COUNT);
   {
      VkPipeline last = VK_NULL_HANDLE;
      const uint32_t spec = 0x3000u;
      if (make_pipeline(&fw, &fx, spec, "E", &last, NULL)) {
         if (vkfw_buffer_poison(&fw, &fx.ssbo, POISON) &&
             dispatch_all(&fw, &fx, &last, 1, 0, "E: one last dispatch") &&
             vkfw_buffer_invalidate(&fw, &fx.ssbo)) {
            t_check(t, region_wrong(&fx, 0, spec) == 0,
                    "E: a shader compiled after every other one was "
                    "destroyed still computes its region");
         }
         fw.vk.vkDestroyPipeline(fw.dev, last, NULL);
      }
   }

out:
   destroy_pipelines(&fw, pipelines, PIPELINE_COUNT);
   free(cold_ns);
   free(warm_ns);
   fixture_finish(&fw, &fx);
   vkfw_finish(&fw);
   return 0;
}
