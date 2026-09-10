/*
 * The same fixed shaders, compiled with the disk cache off, timed, and
 * reported as what NAK made of them — so two builds of this repository
 * can be compared against each other.
 *
 * WHAT IT IS FOR. Meson emits no `-C opt-level` for `buildtype=plain`
 * (`mesonbuild/compilers/rust.py:36-44`, `'plain': []`), and nothing in
 * the cross file or in Mesa supplies one, so every Rust crate in the
 * driver — NAK and NIL among them — is compiled at rustc's default,
 * opt-level 0. Measured on the configured build directory rather than
 * read off the source: none of the thirteen cross-targeted `rustc`
 * invocations carries `-C opt-level`. The variant is one option away,
 *
 *     scripts/configure-mesa-nvk.sh -Drust_args=-Copt-level=2
 *
 * and what it should change is how long NAK takes to compile a shader,
 * not the shader it produces. This case is the measurement that says
 * whether either half of that is true.
 *
 * WHY IT CANNOT BE `pipeline_volume`, WHICH ALREADY TIMES COMPILES.
 * Two reasons, and both of them would make the comparison meaningless
 * rather than noisy.
 *
 *   - THE DISK CACHE. `scripts/gen-driver-id.sh` digests the Mesa
 *     commit, the patch series, `horizon/`, `compat/`, `versions.env`
 *     and the cross files — and nothing else. A Meson option is not in
 *     it, so two builds differing only in `rust_args` produce the SAME
 *     driver id, the same `pipelineCacheUUID` and the same cache file.
 *     The second build would be handed the first build's compiled
 *     shaders and would appear to compile them instantly. This case
 *     sets `MESA_SHADER_CACHE_DISABLE=true` on itself before it creates
 *     a device, which `disk_cache_enabled()` reads per physical device
 *     through `os_get_option` (`util/disk_cache_horizon.c:133`) and
 *     therefore honours mid-process.
 *   - THE OUTPUT IS NOT COMPARED ANYWHERE. A compiler that is faster
 *     and emits something else has not been made faster. `NAK_CRS_INFO`
 *     (patch 0077) prints one line per shader — stage, convergence
 *     reservation and depth, registers, spills, scratch and instruction
 *     count — and it is read per compilation for exactly this reason,
 *     because homebrew launches with no environment. Set here, its
 *     lines land in this case's own log, and two builds' logs compare
 *     line for line.
 *
 * WHY IT IS A CASE IN THIS SUITE AND NOT A `.nro` OF ITS OWN. It runs
 * after `pipeline_volume`, which by then has compiled almost two
 * hundred shaders and grown the shader heap several times, so these
 * absolute numbers are not what a first compile in a fresh process
 * costs. That is acceptable and the comparison is still sound, because
 * this is a PAIRED measurement: both builds run the same suite in the
 * same order, so the work in front of this case is the same work on
 * both sides. Putting it first instead would have made it fresh and
 * changed what `pipeline_volume` measures, and that one has console
 * results on record.
 *
 * WHAT IT CHECKS, as opposed to reports. That every pipeline was
 * created; that every one of them computes its whole region, so a
 * build that compiles faster and wrongly fails here rather than
 * reporting a good number; and that NAK's report actually reached the
 * log, because a run without those lines can time the two builds but
 * cannot compare their output, which is half the question.
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
 * spirv-val at build time. The same shader pipeline_volume uses, on
 * purpose: a compile long enough to be a real compile, and one whose
 * output is already understood. */
#include "comp_chain.spv.h"

/* Must match OpExecutionMode LocalSize in comp_chain.spvasm. */
#define LOCAL_SIZE_X    64u
/* Must match the block count in comp_chain.spvasm. */
#define CHAIN_BLOCKS    64u

#define REGION_WORDS    256u
#define GROUPS          (REGION_WORDS / LOCAL_SIZE_X)

/* Enough compiles for a distribution to mean something and few enough
 * that this stays a measurement rather than a second volume test —
 * pipeline_volume is the one that stresses the heap. */
#define PIPELINE_COUNT  24u

#define POISON          0xfeedfaceu

/* Disjoint from every specialization pipeline_volume uses — 0x1000 for
 * its section A, 0x2000 for B, 0x3000 for E — so nothing here is
 * answered by a compile that already happened in this process. */
#define SPEC_C(i)       (0x4000u + (i))

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

struct fixture {
   VkShaderModule module;
   VkDescriptorSetLayout set_layout;
   VkPipelineLayout layout;
   VkDescriptorPool pool;
   vkfw_buffer ssbo;
   VkDeviceSize region_stride_B;
   VkDescriptorSet sets[PIPELINE_COUNT];
};

static uint64_t align_up_u64(uint64_t v, uint64_t a)
{
   return a <= 1 ? v : ((v + a - 1) / a) * a;
}

static int cmp_u64(const void *a, const void *b)
{
   const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
   return x < y ? -1 : (x > y ? 1 : 0);
}

/* Creates one pipeline specialized to `spec` and reports what
 * vkCreateComputePipelines cost. */
static bool make_pipeline(vkfw *fw, const struct fixture *fx, uint32_t spec,
                          VkPipeline *out, uint64_t *out_ns)
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
   *out_ns = armTicksToNs(armGetSystemTick() - t0);

   if (r != VK_SUCCESS) {
      t_check(fw->t, false, "vkCreateComputePipelines(spec=0x%x) -> %s",
              spec, vkfw_result_str(r));
      *out = VK_NULL_HANDLE;
      return false;
   }
   return true;
}

static bool dispatch_all(vkfw *fw, const struct fixture *fx,
                         const VkPipeline *pipelines, uint32_t count)
{
   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(fw, &cb))
      return false;

   for (uint32_t i = 0; i < count; i++) {
      fw->vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                               pipelines[i]);
      fw->vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     fx->layout, 0, 1, &fx->sets[i],
                                     0, NULL);
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

   return vkfw_submit_and_wait(fw, cb, "every fixed pipeline once");
}

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

TEST_CASE_DECL(vk_pipelines, compile_identity)
{
   /* BEFORE THE DEVICE EXISTS, because both are read while one is being
    * created: disk_cache_enabled() per physical device, and NAK's
    * report per compilation. The framework snapshots the environment
    * around every case and restores it afterwards, so neither of these
    * reaches the case that runs next. */
   setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
   setenv("NAK_CRS_INFO", "1", 1);
   t_note(t, "MESA_SHADER_CACHE_DISABLE=true and NAK_CRS_INFO=1 set by "
             "this case on itself; build %s", t_build_id());

   vkfw fw;
   if (!vkfw_init(&fw, t, NULL))
      return 1;

   struct fixture fx;
   memset(&fx, 0, sizeof(fx));

   VkPipeline pipelines[PIPELINE_COUNT];
   memset(pipelines, 0, sizeof(pipelines));
   uint64_t ns[PIPELINE_COUNT];
   memset(ns, 0, sizeof(ns));

   if (!fixture_init(&fw, &fx))
      goto out;

   /* --- A: the compiles, timed ------------------------------------- */
   uint32_t created = 0;
   uint64_t total_ns = 0;
   for (uint32_t i = 0; i < PIPELINE_COUNT; i++) {
      if (!make_pipeline(&fw, &fx, SPEC_C(i), &pipelines[i], &ns[i]))
         break;
      total_ns += ns[i];
      created++;
   }
   if (!t_check(t, created == PIPELINE_COUNT,
                "A: %u of %u fixed pipelines created", created,
                PIPELINE_COUNT))
      goto out;

   /* --- B: the same shaders still compute the right thing ----------- */
   if (!vkfw_buffer_poison(&fw, &fx.ssbo, POISON))
      goto out;
   if (!dispatch_all(&fw, &fx, pipelines, PIPELINE_COUNT))
      goto out;
   if (!vkfw_buffer_invalidate(&fw, &fx.ssbo))
      goto out;

   uint32_t bad_regions = 0, first_bad = 0, first_wrong = 0;
   for (uint32_t i = 0; i < PIPELINE_COUNT; i++) {
      const uint32_t wrong = region_wrong(&fx, i, SPEC_C(i));
      if (wrong != 0) {
         if (bad_regions == 0) {
            first_bad = i;
            first_wrong = wrong;
         }
         bad_regions++;
      }
   }
   t_check(t, bad_regions == 0,
           "B: every one of the %u shaders computed its whole region "
           "(%u region(s) wrong; first is %u with %u of %u words)",
           PIPELINE_COUNT, bad_regions, first_bad, first_wrong,
           REGION_WORDS);

   /* --- C: the numbers the comparison is made of -------------------- */
   {
      /* Sorted for the distribution, which is why the total is summed
       * above and not from this copy. */
      uint64_t sorted[PIPELINE_COUNT];
      memcpy(sorted, ns, sizeof(sorted));
      qsort(sorted, PIPELINE_COUNT, sizeof(sorted[0]), cmp_u64);

      t_note(t, "MEASURED C: %u fixed compiles, cache off — min %" PRIu64
                " us, median %" PRIu64 " us, mean %" PRIu64 " us, max %"
                PRIu64 " us, total %" PRIu64 " us",
             PIPELINE_COUNT, sorted[0] / 1000u,
             sorted[PIPELINE_COUNT / 2u] / 1000u,
             total_ns / PIPELINE_COUNT / 1000u,
             sorted[PIPELINE_COUNT - 1u] / 1000u, total_ns / 1000u);
   }

   /* --- D: the output NAK reported, so two builds can be compared --- */
   {
      bool found = false;
      if (t_check(t, t_log_scan(t, "NAK crs: stage=compute", &found),
                  "D: this case's log could be searched")) {
         t_check(t, found,
                 "D: NAK reported what it compiled — without these "
                 "lines the two builds' compile times can be compared "
                 "and their output cannot");
      }
   }

out:
   destroy_pipelines(&fw, pipelines, PIPELINE_COUNT);
   fixture_finish(&fw, &fx);
   vkfw_finish(&fw);
   return 0;
}
