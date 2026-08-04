/*
 * vkfw — the Vulkan fixture the Phase 5 tests are built on.
 *
 * WHAT THIS IS FOR. Phase 5 has nine items and seven test binaries, and
 * every one of them needs the same forty lines before it can ask its
 * question: no loader, so entry points come through
 * vk_icdGetInstanceProcAddr; an instance with the debug messenger
 * attached, because in a release build Mesa drops every vk_errorf unless
 * somebody is listening; a physical device NVK will only hand over if
 * the application says it knows the driver is unconformant on this chip;
 * a queue; a command pool. Written once here, the tests stay short
 * enough to read as the measurement they are.
 *
 * WHAT IT DELIBERATELY DOES NOT DO. It does not wrap submission. Phase 5
 * item 9 has to put two submits in flight with no CPU wait between them,
 * so a helper that submits and waits would be the one thing the exit
 * criterion forbids. The pieces are exposed instead — record, submit,
 * fence — and the tests that only need the simple shape call
 * vkfw_submit_and_wait(), which is that shape spelled out and nothing
 * more.
 *
 * NO GLOBALS. Every entry point takes its `vkfw *` (CLAUDE.md rejected
 * design 5). The dispatch table lives inside it, not at file scope.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_VKFW_H
#define HORIZON_VKFW_H

#include <stdbool.h>
#include <stdint.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "common/testfw.h"

/* The entry points the Phase 5 tests use, as one list.
 *
 * All of them are fetched from vk_icdGetInstanceProcAddr with the
 * instance, including the device-level ones: that is what a loader does
 * before it has a device, and NVK's ICD hook answers for them. There is
 * no second, device-level table because there is no second dispatch to
 * skip — this driver is linked in, not loaded.
 *
 * A name added here appears in the struct and is fetched and checked
 * automatically, so a test that needs a new call adds exactly one line.
 */
#define VKFW_PROCS(X)                                                    \
   /* instance and physical device */                                    \
   X(vkDestroyInstance)                                                  \
   X(vkEnumeratePhysicalDevices)                                         \
   X(vkGetPhysicalDeviceProperties)                                      \
   X(vkGetPhysicalDeviceMemoryProperties)                                \
   X(vkGetPhysicalDeviceQueueFamilyProperties)                           \
   X(vkGetPhysicalDeviceFormatProperties)                                \
   X(vkGetPhysicalDeviceImageFormatProperties)                           \
   X(vkCreateDevice)                                                     \
   X(vkDestroyDevice)                                                    \
   X(vkGetDeviceQueue)                                                   \
   X(vkDeviceWaitIdle)                                                   \
   X(vkQueueWaitIdle)                                                    \
   /* memory */                                                          \
   X(vkAllocateMemory)                                                   \
   X(vkFreeMemory)                                                       \
   X(vkMapMemory)                                                        \
   X(vkUnmapMemory)                                                      \
   X(vkFlushMappedMemoryRanges)                                          \
   X(vkInvalidateMappedMemoryRanges)                                     \
   /* buffers */                                                         \
   X(vkCreateBuffer)                                                     \
   X(vkDestroyBuffer)                                                    \
   X(vkGetBufferMemoryRequirements)                                      \
   X(vkBindBufferMemory)                                                 \
   /* images */                                                          \
   X(vkCreateImage)                                                      \
   X(vkDestroyImage)                                                     \
   X(vkGetImageMemoryRequirements)                                       \
   X(vkBindImageMemory)                                                  \
   X(vkGetImageSubresourceLayout)                                        \
   X(vkCreateImageView)                                                  \
   X(vkDestroyImageView)                                                 \
   /* command recording */                                               \
   X(vkCreateCommandPool)                                                \
   X(vkDestroyCommandPool)                                               \
   X(vkResetCommandPool)                                                 \
   X(vkAllocateCommandBuffers)                                           \
   X(vkFreeCommandBuffers)                                               \
   X(vkBeginCommandBuffer)                                               \
   X(vkEndCommandBuffer)                                                 \
   /* synchronisation */                                                 \
   X(vkCreateFence)                                                      \
   X(vkDestroyFence)                                                     \
   X(vkResetFences)                                                      \
   X(vkGetFenceStatus)                                                   \
   X(vkWaitForFences)                                                    \
   X(vkQueueSubmit)                                                      \
   /* transfer and clear commands */                                     \
   X(vkCmdFillBuffer)                                                    \
   X(vkCmdUpdateBuffer)                                                  \
   X(vkCmdCopyBuffer)                                                    \
   X(vkCmdCopyImage)                                                     \
   X(vkCmdCopyBufferToImage)                                             \
   X(vkCmdCopyImageToBuffer)                                             \
   X(vkCmdClearColorImage)                                               \
   X(vkCmdClearDepthStencilImage)                                        \
   X(vkCmdPipelineBarrier)                                               \
   /* pipelines, descriptors and the commands that use them */           \
   X(vkCreateShaderModule)                                               \
   X(vkDestroyShaderModule)                                              \
   X(vkCreateDescriptorSetLayout)                                        \
   X(vkDestroyDescriptorSetLayout)                                       \
   X(vkCreateDescriptorPool)                                             \
   X(vkDestroyDescriptorPool)                                            \
   X(vkAllocateDescriptorSets)                                           \
   X(vkUpdateDescriptorSets)                                             \
   X(vkCreatePipelineLayout)                                             \
   X(vkDestroyPipelineLayout)                                            \
   X(vkCreateComputePipelines)                                           \
   X(vkCreateGraphicsPipelines)                                          \
   X(vkDestroyPipeline)                                                  \
   X(vkCreateSampler)                                                    \
   X(vkDestroySampler)                                                   \
   X(vkCmdBindPipeline)                                                  \
   X(vkCmdBindVertexBuffers)                                             \
   X(vkCmdBindDescriptorSets)                                            \
   X(vkCmdPushConstants)                                                 \
   X(vkCmdDispatch)                                                      \
   X(vkCmdDraw)                                                          \
   X(vkCmdBeginRendering)                                                \
   X(vkCmdEndRendering)                                                  \
   X(vkCmdSetViewport)                                                   \
   X(vkCmdSetScissor)

struct vkfw_dispatch {
#define VKFW_DECL(name) PFN_##name name;
   VKFW_PROCS(VKFW_DECL)
#undef VKFW_DECL
};

typedef struct vkfw {
   test_ctx *t;
   struct vkfw_dispatch vk;

   VkInstance instance;
   VkDebugUtilsMessengerEXT messenger;
   PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger;

   VkPhysicalDevice pdev;
   VkPhysicalDeviceProperties props;
   VkPhysicalDeviceMemoryProperties mem_props;
   uint32_t queue_family;

   VkDevice dev;
   VkQueue queue;
   VkCommandPool pool;

   /* The last result from a submit or a fence wait. A test with several
    * independent cases uses this to stop after the first VK_ERROR_DEVICE_LOST
    * rather than reporting the same failure once per remaining case: once
    * the channel is gone nothing else can be measured, and N identical
    * failures hide which one was first.
    */
   VkResult last_submit_result;
} vkfw;

/* True once a submit or wait has reported VK_ERROR_DEVICE_LOST. */
static inline bool vkfw_device_lost(const vkfw *fw)
{
   return fw->last_submit_result == VK_ERROR_DEVICE_LOST;
}

/* Brings up instance → physical device → device → queue → command pool,
 * checking each step. Returns false with everything it did create torn
 * down again, so a caller may return immediately.
 *
 * `features2` is chained into VkDeviceCreateInfo::pNext when non-NULL,
 * which is how a test asks for dynamic rendering or anything else it
 * needs; the fixture itself enables nothing.
 */
bool vkfw_init(vkfw *fw, test_ctx *t, const void *features2);

/* Reverse order of vkfw_init. Safe on a partially-initialised fixture. */
void vkfw_finish(vkfw *fw);

/* Index of a memory type satisfying `type_bits` (from a memory
 * requirement) with all of `want` set. Returns UINT32_MAX if there is
 * none — checked by the caller, never assumed. */
uint32_t vkfw_memory_type(const vkfw *fw, uint32_t type_bits,
                          VkMemoryPropertyFlags want);

/* A buffer with memory bound, and mapped when the memory is host
 * visible. `map` is NULL otherwise. */
typedef struct vkfw_buffer {
   VkBuffer buf;
   VkDeviceMemory mem;
   void *map;
   VkDeviceSize size_B;      /* as requested                          */
   VkDeviceSize alloc_B;     /* what the requirement made it          */
   bool coherent;            /* HOST_COHERENT: no explicit maintenance */
} vkfw_buffer;

bool vkfw_buffer_create(vkfw *fw, VkDeviceSize size_B,
                        VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags mem_want,
                        vkfw_buffer *out);
void vkfw_buffer_destroy(vkfw *fw, vkfw_buffer *b);

/* Cache maintenance around a mapped buffer, skipped when the memory is
 * coherent. Both check their VkResult. */
bool vkfw_buffer_flush(vkfw *fw, const vkfw_buffer *b);
bool vkfw_buffer_invalidate(vkfw *fw, const vkfw_buffer *b);

/* POISON BEFORE READBACK, and the reason it is in the fixture rather
 * than in each test.
 *
 * Phase 4's exit criterion turned on this: a readback buffer that
 * already holds the expected value proves nothing, and zeroed memory
 * looks like a successful clear to black. Every destination is filled
 * with a value the GPU is not going to write — and flushed, so the
 * poison is in memory and not only in the CPU's cache — before the
 * submit that is supposed to overwrite it. Then a match can only have
 * come from the GPU.
 */
bool vkfw_buffer_poison(vkfw *fw, vkfw_buffer *b, uint32_t pattern);

/* An image with memory bound. No view: the tests that need one make it
 * themselves, because the view's format and aspect are usually part of
 * what is being measured. */
typedef struct vkfw_image {
   VkImage img;
   VkDeviceMemory mem;
   VkFormat format;
   VkExtent3D extent;
   VkImageTiling tiling;
   uint32_t mip_levels;
   uint32_t array_layers;
   VkDeviceSize alloc_B;
   VkDeviceSize align_B;     /* what the requirement asked for */
} vkfw_image;

bool vkfw_image_create(vkfw *fw, VkFormat format, VkExtent3D extent,
                       uint32_t mip_levels, uint32_t array_layers,
                       VkImageUsageFlags usage, VkImageTiling tiling,
                       vkfw_image *out);
void vkfw_image_destroy(vkfw *fw, vkfw_image *i);

/* True when the driver supports this image configuration at all. An
 * unsupported combination is a fact about the device, not a failure, so
 * a caller uses this to say "skipped, and why" instead of reporting a
 * bug it did not find. Passes one check: that the query itself
 * succeeded, since VK_ERROR_FORMAT_NOT_SUPPORTED and a broken query are
 * different answers. */
bool vkfw_image_supported(vkfw *fw, VkFormat format, VkImageType type,
                          VkImageTiling tiling, VkImageUsageFlags usage,
                          const char *what);

/* Allocates and begins a primary command buffer with ONE_TIME_SUBMIT. */
bool vkfw_cmd_begin(vkfw *fw, VkCommandBuffer *cb_out);

/* Ends `cb` and submits it, signalling `fence`. Does NOT wait: item 9
 * needs submits in flight, and a helper that waited would make that
 * measurement impossible to write. */
bool vkfw_cmd_end_submit(vkfw *fw, VkCommandBuffer cb, VkFence fence);

/* The simple shape, spelled out: end, submit, wait on the fence with a
 * finite timeout. The only CPU stall is vkWaitForFences, which is where
 * Vulkan puts one. `what` names the step in the check line. */
bool vkfw_submit_and_wait(vkfw *fw, VkCommandBuffer cb, const char *what);

/* Compares `n` 32-bit words against `expect`, reporting the first
 * mismatch by index with both values, and passes exactly one check.
 * Returns the number of words that differ. */
uint32_t vkfw_expect_words(vkfw *fw, const void *got, uint32_t expect,
                           uint32_t n, const char *what);

/* Same, against an array. */
uint32_t vkfw_expect_words_array(vkfw *fw, const void *got,
                                 const uint32_t *expect, uint32_t n,
                                 const char *what);

/* VkResult as a short name, for check lines. Falls back to the number. */
const char *vkfw_result_str(VkResult r);

#endif /* HORIZON_VKFW_H */
