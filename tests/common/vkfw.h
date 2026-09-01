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
   X(vkGetPhysicalDeviceFeatures)                                        \
   X(vkGetPhysicalDeviceFormatProperties)                                \
   X(vkGetPhysicalDeviceFormatProperties2)                               \
   X(vkGetPhysicalDeviceImageFormatProperties)                           \
   X(vkGetPhysicalDeviceImageFormatProperties2)                          \
   X(vkEnumerateDeviceExtensionProperties)                               \
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
   X(vkQueueBindSparse)                                                  \
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
   X(vkCmdSetScissor)                                                    \
   X(vkCreateQueryPool)                                                  \
   X(vkDestroyQueryPool)                                                 \
   X(vkCmdResetQueryPool)                                                \
   X(vkCmdWriteTimestamp)                                                \
   X(vkGetQueryPoolResults)

/* The window-system entry points, kept apart from the list above and
 * loaded on request.
 *
 * They cannot live in VKFW_PROCS. vk_icdGetInstanceProcAddr answers
 * NULL for an entry point whose extension the instance did not enable,
 * so a test that wants no swapchain would fail "every entry point
 * resolved" for entry points it never asked for. vkfw_wsi_load() is the
 * opt-in, and it is the swapchain tests that call it.
 */
#define VKFW_WSI_PROCS(X)                                                \
   X(vkCreateViSurfaceNN)                                                \
   X(vkDestroySurfaceKHR)                                                \
   X(vkGetPhysicalDeviceSurfaceSupportKHR)                               \
   X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                          \
   X(vkGetPhysicalDeviceSurfaceFormatsKHR)                               \
   X(vkGetPhysicalDeviceSurfacePresentModesKHR)                          \
   X(vkCreateSwapchainKHR)                                               \
   X(vkDestroySwapchainKHR)                                              \
   X(vkGetSwapchainImagesKHR)                                            \
   X(vkAcquireNextImageKHR)                                              \
   X(vkQueuePresentKHR)                                                  \
   X(vkCreateSemaphore)                                                  \
   X(vkDestroySemaphore)

struct vkfw_dispatch {
#define VKFW_DECL(name) PFN_##name name;
   VKFW_PROCS(VKFW_DECL)
#undef VKFW_DECL
};

struct vkfw_wsi_dispatch {
#define VKFW_DECL(name) PFN_##name name;
   VKFW_WSI_PROCS(VKFW_DECL)
#undef VKFW_DECL
};

/* How many debug-utils messages vkfw remembers, and how much of each.
 * Enough for a test to ask whether the driver said a particular thing
 * without keeping a transcript. */
#define VKFW_MESSAGE_SLOTS 16
/* Long enough for the longest thing the driver says, with room. The
 * decision line — "…wsi_horizon.c:1851]: wsi_horizon: zero-copy: the
 * swapchain images are the scanout buffers (3 images, 1280x720, swap
 * interval 1)" — was within about 35 characters of the old 192, and
 * exit criterion 4 asserts on that string. A limit a checked message
 * nearly reaches is a limit that will be crossed. */
#define VKFW_MESSAGE_CHARS 384

typedef struct vkfw {
   test_ctx *t;
   struct vkfw_dispatch vk;
   struct vkfw_wsi_dispatch wsi;
   bool wsi_loaded;

   /* The last VKFW_MESSAGE_SLOTS debug-utils messages, oldest first
    * once it wraps. WHY A TEST WOULD WANT THEM: a driver decision that
    * is only written to a log is not observable by the application that
    * it was made for. The Horizon WSI reports its zero-copy decision as
    * an INFO message precisely so a test can assert on it, and this is
    * where the test finds it.
    *
    * WRITTEN FROM WHICHEVER THREAD PRODUCED THE MESSAGE. Mesa calls a
    * debug-utils messenger on the calling thread, so a worker that does
    * nothing but vkCreateImage or vkDestroySwapchainKHR appends here
    * without the test ever mentioning it, while the main thread reads
    * the same array through vkfw_saw_message. `message_count` was a
    * plain non-atomic increment shared across those threads, and the
    * ring write raced the read. msg_lock covers both. */
   char messages[VKFW_MESSAGE_SLOTS][VKFW_MESSAGE_CHARS];
   uint32_t message_count;
   pthread_mutex_t msg_lock;
   bool msg_lock_ready;
   /* Where vkfw_saw_message copies a match to, so what it hands back
    * outlives the lock it found it under. */
   char message_match[VKFW_MESSAGE_CHARS];

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

/* The same, with device extensions enabled.
 *
 * An extension being *advertised* does not make it usable: its tiling
 * modes, pNext structures and entry points are invalid usage on a
 * device that did not enable it, and with no validation layers here
 * that is a wrong answer rather than an error. t_vk_caps created a
 * VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT image on a device with no
 * extensions enabled and passed, which is the failure mode — found in
 * review of PR #7.
 *
 * Every name passed must be one the physical device advertises; the
 * caller checks that, because "the driver does not offer this" and
 * "the driver offers it and it does not work" are different findings
 * and only the second one is a bug. */
bool vkfw_init_ext(vkfw *fw, test_ctx *t, const void *features2,
                   const char *const *device_exts,
                   uint32_t device_ext_count);

/* The same again, with instance extensions too.
 *
 * VK_EXT_debug_utils is always enabled and must not be repeated here.
 * Every name passed must be one the *instance* advertises; a swapchain
 * test checks that itself, because "this build has no VI surface" and
 * "it has one and it does not work" are different findings. */
bool vkfw_init_full(vkfw *fw, test_ctx *t, const void *features2,
                    const char *const *instance_exts,
                    uint32_t instance_ext_count,
                    const char *const *device_exts,
                    uint32_t device_ext_count);

/* Resolves VKFW_WSI_PROCS into fw->wsi, passing one check for the lot
 * and naming any entry point that is missing. Returns false without
 * touching anything else, so a caller can report and stop. */
bool vkfw_wsi_load(vkfw *fw);

/* True when some debug-utils message since init contained `needle`.
 * The match is a plain substring search over the remembered messages
 * (VKFW_MESSAGE_SLOTS of them), and `out` — when non-NULL — receives
 * the first matching message.
 *
 * Takes the fixture's message lock, so it is not const: the ring it
 * searches is appended to by whatever thread the driver logged from.
 * What `out` receives is a copy owned by the fixture, valid until the
 * next call that matches — not a pointer into the ring, which a worker
 * could overwrite between this returning and the caller printing it. */
bool vkfw_saw_message(vkfw *fw, const char *needle, const char **out);

/* Forgets every remembered message. A test that measures two
 * configurations in a row calls this between them so the second
 * measurement cannot be satisfied by the first one's evidence. */
void vkfw_forget_messages(vkfw *fw);

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

/* A graphics pipeline, and the twelve structures Vulkan needs to build
 * one.
 *
 * WHY THIS IS IN THE FIXTURE. Items 5 to 8 all render, and the part
 * they share is not the interesting part: two shader modules, a layout,
 * and the same rasterisation, multisample and blend state every time.
 * Written out per test it is four copies of a hundred lines in which
 * the two or three fields that actually differ between the tests are
 * invisible. Here, a test states only what it changes.
 *
 * Everything this sets and does not expose is fixed for a reason:
 *
 *   triangle list       the only topology these items draw
 *   cull mode NONE      winding in framebuffer space depends on
 *                       Vulkan's y-down NDC; a culled triangle comes
 *                       back as the clear colour, which says nothing
 *                       about what is being measured. Culling is not
 *                       part of any item here
 *   1 sample            multisampling is not in Phase 5
 *   blending off        every item compares written values; a blend
 *                       would make the destination part of the answer.
 *                       The DEFAULT, not a rule: a test that is
 *                       measuring blending sets `blend` below
 *   one colour att.     items 5 to 8 each render to exactly one
 *
 * Dynamic rendering only: there is no VkRenderPass anywhere in this
 * suite, so the formats come in through VkPipelineRenderingCreateInfo.
 */
typedef struct vkfw_gfx_desc {
   const uint32_t *vs_spv;
   size_t vs_B;
   const uint32_t *fs_spv;
   size_t fs_B;

   VkFormat colour_format;
   /* VK_FORMAT_UNDEFINED when the pass has no depth attachment. */
   VkFormat depth_format;

   /* VK_NULL_HANDLE when the shaders declare no descriptors. */
   VkDescriptorSetLayout set_layout;

   /* One push constant range at offset 0, or zero bytes for none. The
    * stages must be exactly the ones whose SPIR-V declares the block:
    * a range that names a stage the shader does not declare, or omits
    * one it does, is invalid usage that nothing here would report. */
   uint32_t push_constant_B;
   VkShaderStageFlags push_constant_stages;

   /* Vertex input; leave the counts at zero for a shader that builds
    * its own positions from gl_VertexIndex. */
   uint32_t binding_count;
   const VkVertexInputBindingDescription *bindings;
   uint32_t attr_count;
   const VkVertexInputAttributeDescription *attrs;

   /* The static viewport and scissor, both the full target. Ignored
    * when dynamic_viewport is set, in which case the caller issues
    * vkCmdSetViewport and vkCmdSetScissor itself. */
   uint32_t width, height;
   bool dynamic_viewport;

   bool depth_test;
   bool depth_write;
   VkCompareOp depth_compare;    /* only read when depth_test */

   /* The colour blend state for the single attachment, or NULL for the
    * "blending off, all four components written" default the list above
    * describes.
    *
    * WHY THE WHOLE STRUCT AND NOT A FLAG. There is no one blend a test
    * would mean by "on": the factors, the op and the write mask are the
    * thing being measured whenever blending is being measured at all.
    * Passing the state through keeps the fixture from having an opinion
    * about which blend is the interesting one.
    *
    * Copied by value into the pipeline, so it need not outlive the
    * vkfw_gfx_create call. */
   const VkPipelineColorBlendAttachmentState *blend;
} vkfw_gfx_desc;

typedef struct vkfw_gfx {
   VkShaderModule vs, fs;
   VkPipelineLayout layout;
   VkPipeline pipeline;
} vkfw_gfx;

/* Builds modules, layout and pipeline, checking each step and naming it
 * with `what`. Returns false with `out` safe to pass to
 * vkfw_gfx_destroy either way. */
bool vkfw_gfx_create(vkfw *fw, const char *what, const vkfw_gfx_desc *desc,
                     vkfw_gfx *out);

/* Reverse order, safe on a partially-built or zeroed vkfw_gfx. */
void vkfw_gfx_destroy(vkfw *fw, vkfw_gfx *g);

/* Compares `n` 32-bit WORDS — not bytes — against `expect`, reporting
 * the first mismatch by index with both values, and passes exactly one
 * check. Returns the number of words that differ.
 *
 * THE UNIT IS THE TRAP. Four of the six findings in the PR #7 review
 * were readback checks measuring the wrong extent, and every one of
 * them funnels through here; a caller passing a byte count reads four
 * times past its buffer and the helper cannot tell. It has no size to
 * validate against, so the macros the tests use are spelled
 * `.../ 4u` at every call site and the reader can see the division.
 * Passing a byte count is a defect this signature cannot catch —
 * stated here rather than left to be discovered. */
uint32_t vkfw_expect_words(vkfw *fw, const void *got, uint32_t expect,
                           uint32_t n, const char *what);

/* Same, against an array, and the same warning about the unit. */
uint32_t vkfw_expect_words_array(vkfw *fw, const void *got,
                                 const uint32_t *expect, uint32_t n,
                                 const char *what);

/* VkResult as a short name, for check lines. A result outside the list
 * it knows comes back as "VkResult <n>" with the number, which is what
 * the promise here always said and what the code did not do until the
 * PR #7 review. The returned pointer is one of a small rotation of
 * static buffers, so a printf may name up to four results at once and
 * no more. */
const char *vkfw_result_str(VkResult r);

#endif /* HORIZON_VKFW_H */
