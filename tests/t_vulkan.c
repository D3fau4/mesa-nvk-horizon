/*
 * Test 14 — the mandatory Vulkan sequence (docs/milestones.md Phase 4).
 *
 *   vkCreateInstance
 *   vkEnumeratePhysicalDevices
 *   vkCreateDevice
 *   vkCreateBuffer
 *   vkAllocateMemory
 *   vkBindBufferMemory
 *   vkCmdFillBuffer
 *   vkQueueSubmit
 *   vkWaitForFences
 *   -> the CPU reads back the filled pattern and validates it
 *
 * WHAT THIS TEST IS FOR. Not "does it run without errors" — the exit
 * criterion is that the CPU reads the pattern the GPU wrote. A fill of a
 * known value into a known range is the smallest piece of work that
 * proves the whole path: memory that the GPU can reach, a command buffer
 * it executed, a fence that told the truth about when, and cache
 * maintenance that made the result visible.
 *
 * NO LOADER. Horizon has no dynamic loader, so there is no libvulkan and
 * no ICD to open (mesa-patches 0015). The driver is linked in and
 * entered through vk_icdGetInstanceProcAddr, which is what a loader
 * would have called. Every other entry point is fetched by name from
 * there, exactly as the loader does — so this test exercises the same
 * dispatch the real thing would.
 *
 * NO WAIT-IDLE. Phase 4's exit criterion forbids inserting
 * vkQueueWaitIdle or vkDeviceWaitIdle to make the result appear. The
 * only wait here is vkWaitForFences, which is where Vulkan says a CPU
 * stall belongs.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>   /* setenv, getenv */
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "common/testfw.h"

const char *const test_name = "t_vulkan";

/* The one symbol the driver exports for a loader to find. Declared
 * rather than included: vk_icd.h is the loader's header and this is not
 * a loader — it is the application standing in for one.
 */
extern PFN_vkVoidFunction
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);

/* The pattern and the buffer. 4 KiB is one small page, which is the
 * granularity everything below works in, and 0xa5c3f00d is a value no
 * uninitialised allocation is likely to hold by accident — a test that
 * passes on zeroed memory proves nothing.
 */
#define FILL_SIZE_B   4096u
#define FILL_PATTERN  0xa5c3f00du

#define GET_INSTANCE_PROC(inst, name)                                    \
   do {                                                                  \
      name = (PFN_##name)vk_icdGetInstanceProcAddr(inst, #name);         \
      if (!t_check(t, name != NULL, "GetInstanceProcAddr(%s)", #name))   \
         return 1;                                                       \
   } while (0)

int
run_test(test_ctx *t)
{
   /* --- entry points, through the ICD hook a loader would use ------- */
   PFN_vkCreateInstance vkCreateInstance;
   PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
   PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
   PFN_vkGetPhysicalDeviceQueueFamilyProperties
      vkGetPhysicalDeviceQueueFamilyProperties;
   PFN_vkGetPhysicalDeviceMemoryProperties
      vkGetPhysicalDeviceMemoryProperties;
   PFN_vkCreateDevice vkCreateDevice;
   PFN_vkGetDeviceQueue vkGetDeviceQueue;
   PFN_vkCreateBuffer vkCreateBuffer;
   PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
   PFN_vkAllocateMemory vkAllocateMemory;
   PFN_vkBindBufferMemory vkBindBufferMemory;
   PFN_vkMapMemory vkMapMemory;
   PFN_vkCreateCommandPool vkCreateCommandPool;
   PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
   PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
   PFN_vkCmdFillBuffer vkCmdFillBuffer;
   PFN_vkEndCommandBuffer vkEndCommandBuffer;
   PFN_vkCreateFence vkCreateFence;
   PFN_vkQueueSubmit vkQueueSubmit;
   PFN_vkWaitForFences vkWaitForFences;
   PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges;
   PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges;
   PFN_vkDestroyFence vkDestroyFence;
   PFN_vkDestroyCommandPool vkDestroyCommandPool;
   PFN_vkFreeMemory vkFreeMemory;
   PFN_vkDestroyBuffer vkDestroyBuffer;
   PFN_vkDestroyDevice vkDestroyDevice;
   PFN_vkDestroyInstance vkDestroyInstance;

   /* --- opting in to an untested driver, on purpose ------------------
    *
    * nvk_is_conformant() opens with
    *
    *     "Tegra is not currently supported"
    *     if (info->type != NV_DEVICE_TYPE_DIS)
    *        return false;
    *
    * and GM20B is NV_DEVICE_TYPE_SOC, which is what it is. NVK then
    * refuses the device with VK_ERROR_INCOMPATIBLE_DRIVER, and under
    * NDEBUG it does so with no message at all — so vkEnumeratePhysical-
    * Devices returns VK_SUCCESS and no devices. Measured on console:
    * the first hardware run of this test failed at exactly that line.
    *
    * NVK supplies the escape hatch and this is what it is for. Setting
    * it here rather than patching nvk_is_conformant() is deliberate:
    * the check is telling the truth. NVK is not conformant on this
    * chip, nobody has run the CTS on it, and saying otherwise in the
    * patch series would be a claim this project cannot support. The
    * application is the right place to say "I know, proceed" —
    * vk_warn_non_conformant_implementation() still fires.
    *
    * Before vkCreateInstance, because the flag is read while the
    * physical device is being created.
    */
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);

   /* And ask horizon_gpu to say what it is doing. Its default level is
    * WARN, which is right for a driver in use and wrong for a bring-up
    * test: the fourth hardware run failed inside channel creation and
    * the one number that would have identified which channel — its
    * syncpoint id — is logged at INFO. This is set here rather than
    * lowered in the library, because the library's default is correct
    * and it is the test that wants more.
    */
   setenv("HORIZON_GPU_LOG", "3", 1);
   t_check(t, getenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER") != NULL,
                  "the non-conformance opt-in is set in the environment");

   GET_INSTANCE_PROC(VK_NULL_HANDLE, vkCreateInstance);

   /* --- vkCreateInstance -------------------------------------------- */
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "t_vulkan",
      .apiVersion = VK_API_VERSION_1_1,
   };
   const VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance instance = VK_NULL_HANDLE;
   VkResult r = vkCreateInstance(&ici, NULL, &instance);
   t_check(t, r == VK_SUCCESS, "vkCreateInstance -> %d", (int)r);
   if (r != VK_SUCCESS)
      return 1;

   GET_INSTANCE_PROC(instance, vkEnumeratePhysicalDevices);
   GET_INSTANCE_PROC(instance, vkGetPhysicalDeviceProperties);
   GET_INSTANCE_PROC(instance, vkGetPhysicalDeviceQueueFamilyProperties);
   GET_INSTANCE_PROC(instance, vkGetPhysicalDeviceMemoryProperties);
   GET_INSTANCE_PROC(instance, vkCreateDevice);
   GET_INSTANCE_PROC(instance, vkGetDeviceQueue);
   GET_INSTANCE_PROC(instance, vkCreateBuffer);
   GET_INSTANCE_PROC(instance, vkGetBufferMemoryRequirements);
   GET_INSTANCE_PROC(instance, vkAllocateMemory);
   GET_INSTANCE_PROC(instance, vkBindBufferMemory);
   GET_INSTANCE_PROC(instance, vkMapMemory);
   GET_INSTANCE_PROC(instance, vkCreateCommandPool);
   GET_INSTANCE_PROC(instance, vkAllocateCommandBuffers);
   GET_INSTANCE_PROC(instance, vkBeginCommandBuffer);
   GET_INSTANCE_PROC(instance, vkCmdFillBuffer);
   GET_INSTANCE_PROC(instance, vkEndCommandBuffer);
   GET_INSTANCE_PROC(instance, vkCreateFence);
   GET_INSTANCE_PROC(instance, vkQueueSubmit);
   GET_INSTANCE_PROC(instance, vkWaitForFences);
   GET_INSTANCE_PROC(instance, vkFlushMappedMemoryRanges);
   GET_INSTANCE_PROC(instance, vkInvalidateMappedMemoryRanges);
   GET_INSTANCE_PROC(instance, vkDestroyFence);
   GET_INSTANCE_PROC(instance, vkDestroyCommandPool);
   GET_INSTANCE_PROC(instance, vkFreeMemory);
   GET_INSTANCE_PROC(instance, vkDestroyBuffer);
   GET_INSTANCE_PROC(instance, vkDestroyDevice);
   GET_INSTANCE_PROC(instance, vkDestroyInstance);

   /* --- vkEnumeratePhysicalDevices ---------------------------------- */
   uint32_t pdev_count = 0;
   r = vkEnumeratePhysicalDevices(instance, &pdev_count, NULL);
   t_check(t, r == VK_SUCCESS, "vkEnumeratePhysicalDevices -> %d",
                  (int)r);
   t_check(t, pdev_count == 1, "one physical device, got %u",
                  pdev_count);
   if (pdev_count == 0) {
      /* Vulkan says an empty list, not an error, so the call above
       * succeeded and there is nothing in the result to say why. The
       * driver logs the reason to the screen — it cannot reach this
       * log file — so point at it rather than leave the reader to
       * guess.
       */
      t_note(t, "the driver found no GPU; its reason is on screen, "
                "above this list, as an \"nvkmd_horizon:\" line");
      return 1;
   }

   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   pdev_count = 1;
   r = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   t_check(t, r == VK_SUCCESS || r == VK_INCOMPLETE,
                  "vkEnumeratePhysicalDevices(list) -> %d", (int)r);

   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   t_note(t, "device: %s (api %u.%u.%u)", props.deviceName,
             VK_VERSION_MAJOR(props.apiVersion),
             VK_VERSION_MINOR(props.apiVersion),
             VK_VERSION_PATCH(props.apiVersion));

   /* --- vkCreateDevice ---------------------------------------------- */
   uint32_t qf_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qf_count, NULL);
   t_check(t, qf_count >= 1, "at least one queue family, got %u",
                  qf_count);
   if (qf_count == 0)
      return 1;

   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   const VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
   };
   VkDevice dev = VK_NULL_HANDLE;
   r = vkCreateDevice(pdev, &dci, NULL, &dev);
   t_check(t, r == VK_SUCCESS, "vkCreateDevice -> %d", (int)r);
   if (r != VK_SUCCESS)
      return 1;

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(dev, 0, 0, &queue);
   t_check(t, queue != VK_NULL_HANDLE, "vkGetDeviceQueue");

   /* --- vkCreateBuffer ---------------------------------------------- */
   const VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = FILL_SIZE_B,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buf = VK_NULL_HANDLE;
   r = vkCreateBuffer(dev, &bci, NULL, &buf);
   t_check(t, r == VK_SUCCESS, "vkCreateBuffer -> %d", (int)r);
   if (r != VK_SUCCESS)
      return 1;

   /* --- vkAllocateMemory -------------------------------------------- */
   VkMemoryRequirements mreq;
   vkGetBufferMemoryRequirements(dev, buf, &mreq);

   VkPhysicalDeviceMemoryProperties mprops;
   vkGetPhysicalDeviceMemoryProperties(pdev, &mprops);

   /* HOST_VISIBLE so the CPU can read the result back, which is the
    * point of the test. HOST_COHERENT is *not* required: this platform's
    * memory is CPU-cached and the invalidate below is what makes the
    * GPU's write visible, which is exactly the mechanism decision D5 is
    * about. Asking only for HOST_VISIBLE keeps the test honest about
    * which of the two is doing the work.
    */
   uint32_t type_index = UINT32_MAX;
   for (uint32_t i = 0; i < mprops.memoryTypeCount; i++) {
      if (!(mreq.memoryTypeBits & (1u << i)))
         continue;
      if (mprops.memoryTypes[i].propertyFlags &
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
         type_index = i;
         break;
      }
   }
   t_check(t, type_index != UINT32_MAX,
                  "a host-visible memory type the buffer accepts");
   if (type_index == UINT32_MAX)
      return 1;

   const VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mreq.size,
      .memoryTypeIndex = type_index,
   };
   VkDeviceMemory mem = VK_NULL_HANDLE;
   r = vkAllocateMemory(dev, &mai, NULL, &mem);
   t_check(t, r == VK_SUCCESS, "vkAllocateMemory -> %d", (int)r);
   if (r != VK_SUCCESS)
      return 1;

   /* --- vkBindBufferMemory ------------------------------------------ */
   r = vkBindBufferMemory(dev, buf, mem, 0);
   t_check(t, r == VK_SUCCESS, "vkBindBufferMemory -> %d", (int)r);
   if (r != VK_SUCCESS)
      return 1;

   /* Map before the submit and poison the buffer with the complement of
    * the pattern. A test that reads back the right value out of memory
    * that already held it proves nothing; this makes the GPU's write
    * the only way the check can pass.
    */
   void *map = NULL;
   r = vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &map);
   t_check(t, r == VK_SUCCESS && map != NULL, "vkMapMemory -> %d",
                  (int)r);
   if (r != VK_SUCCESS || map == NULL)
      return 1;

   uint32_t *words = map;
   for (uint32_t i = 0; i < FILL_SIZE_B / 4; i++)
      words[i] = ~FILL_PATTERN;

   /* And flush it, which is not a formality.
    *
    * The memory type chosen above is HOST_VISIBLE and HOST_CACHED but
    * NOT HOST_COHERENT, so those writes are sitting in the CPU's cache
    * as dirty lines. Vulkan requires vkFlushMappedMemoryRanges for them
    * to become visible to the device — but the reason it matters here
    * is worse than visibility. A dirty line can be evicted at any time,
    * including *after* the GPU's fill has landed in memory, and the
    * eviction would then overwrite the GPU's result with the poison.
    * The test would report that the GPU had not written, and it would
    * be the test that was wrong.
    *
    * So the poison has to be pushed out of the cache before the submit.
    * Without this it is not a poison, it is a landmine.
    */
   const VkMappedMemoryRange poison_range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = mem,
      .offset = 0,
      .size = VK_WHOLE_SIZE,
   };
   r = vkFlushMappedMemoryRanges(dev, 1, &poison_range);
   t_check(t, r == VK_SUCCESS, "vkFlushMappedMemoryRanges (poison) -> %d",
                  (int)r);
   if (r != VK_SUCCESS)
      return 1;

   /* --- vkCmdFillBuffer --------------------------------------------- */
   const VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   r = vkCreateCommandPool(dev, &cpci, NULL, &pool);
   t_check(t, r == VK_SUCCESS, "vkCreateCommandPool -> %d", (int)r);
   if (r != VK_SUCCESS)
      return 1;

   const VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   r = vkAllocateCommandBuffers(dev, &cbai, &cmd);
   t_check(t, r == VK_SUCCESS, "vkAllocateCommandBuffers -> %d",
                  (int)r);
   if (r != VK_SUCCESS)
      return 1;

   const VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   r = vkBeginCommandBuffer(cmd, &cbbi);
   t_check(t, r == VK_SUCCESS, "vkBeginCommandBuffer -> %d", (int)r);

   vkCmdFillBuffer(cmd, buf, 0, FILL_SIZE_B, FILL_PATTERN);

   r = vkEndCommandBuffer(cmd);
   t_check(t, r == VK_SUCCESS, "vkEndCommandBuffer -> %d", (int)r);

   /* --- vkQueueSubmit ----------------------------------------------- */
   const VkFenceCreateInfo fci = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   VkFence fence = VK_NULL_HANDLE;
   r = vkCreateFence(dev, &fci, NULL, &fence);
   t_check(t, r == VK_SUCCESS, "vkCreateFence -> %d", (int)r);

   const VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   r = vkQueueSubmit(queue, 1, &si, fence);
   t_check(t, r == VK_SUCCESS, "vkQueueSubmit -> %d", (int)r);
   if (r != VK_SUCCESS)
      return 1;

   /* --- vkWaitForFences --------------------------------------------- */
   /*
    * The only CPU stall in this test, and the one Vulkan puts here. No
    * vkQueueWaitIdle and no vkDeviceWaitIdle before the readback: the
    * phase's exit criterion says the result must be visible because the
    * fence said the work was done, not because the driver was told to
    * drain.
    *
    * One second, not infinite. A hang is a result to report, and a test
    * that never returns reports nothing.
    */
   r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 1000000000ull);
   t_check(t, r == VK_SUCCESS, "vkWaitForFences -> %d%s", (int)r,
                  r == VK_TIMEOUT ? " (TIMEOUT — the GPU did not finish)"
                                  : "");
   if (r != VK_SUCCESS)
      return 1;

   /* The GPU wrote through its own view of this memory; the CPU's cache
    * may still hold what was there before. This is the invalidate side
    * of decision D5, and it is not optional on a platform whose memory
    * is CPU-cached.
    */
   const VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = mem,
      .offset = 0,
      .size = VK_WHOLE_SIZE,
   };
   r = vkInvalidateMappedMemoryRanges(dev, 1, &range);
   t_check(t, r == VK_SUCCESS, "vkInvalidateMappedMemoryRanges -> %d",
                  (int)r);

   /* --- the readback, which is the actual exit criterion ------------ */
   uint32_t bad = 0;
   uint32_t first_bad_index = 0;
   uint32_t first_bad_value = 0;
   for (uint32_t i = 0; i < FILL_SIZE_B / 4; i++) {
      if (words[i] != FILL_PATTERN) {
         if (bad == 0) {
            first_bad_index = i;
            first_bad_value = words[i];
         }
         bad++;
      }
   }

   t_check(t, bad == 0,
                  "readback: %u/%u words wrong; first at [%u] = 0x%08x, "
                  "expected 0x%08x",
                  bad, FILL_SIZE_B / 4, first_bad_index, first_bad_value,
                  FILL_PATTERN);

   if (bad == 0) {
      t_note(t, "readback: %u/%u words are 0x%08x — the GPU wrote it",
                FILL_SIZE_B / 4, FILL_SIZE_B / 4, FILL_PATTERN);
   }

   /* --- teardown, in reverse order ---------------------------------- */
   vkDestroyFence(dev, fence, NULL);
   vkDestroyCommandPool(dev, pool, NULL);
   vkDestroyBuffer(dev, buf, NULL);
   vkFreeMemory(dev, mem, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(instance, NULL);

   return 0;
}
