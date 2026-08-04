/*
 * vkfw — implementation. See vkfw.h for what it is and what it refuses
 * to do.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>   /* setenv, getenv */
#include <string.h>

#include "common/vkfw.h"

/* The one symbol the driver exports for a loader to find. Declared
 * rather than included: vk_icd.h is the loader's header and this is not
 * a loader — it is the application standing in for one. Same reasoning
 * as t_vulkan.c, which predates this file. */
extern PFN_vkVoidFunction
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);

const char *vkfw_result_str(VkResult r)
{
   switch (r) {
   case VK_SUCCESS:                        return "VK_SUCCESS";
   case VK_NOT_READY:                      return "VK_NOT_READY";
   case VK_TIMEOUT:                        return "VK_TIMEOUT";
   case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
   case VK_ERROR_OUT_OF_HOST_MEMORY:       return "OUT_OF_HOST_MEMORY";
   case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "OUT_OF_DEVICE_MEMORY";
   case VK_ERROR_INITIALIZATION_FAILED:    return "INITIALIZATION_FAILED";
   case VK_ERROR_DEVICE_LOST:              return "DEVICE_LOST";
   case VK_ERROR_MEMORY_MAP_FAILED:        return "MEMORY_MAP_FAILED";
   case VK_ERROR_LAYER_NOT_PRESENT:        return "LAYER_NOT_PRESENT";
   case VK_ERROR_EXTENSION_NOT_PRESENT:    return "EXTENSION_NOT_PRESENT";
   case VK_ERROR_FEATURE_NOT_PRESENT:      return "FEATURE_NOT_PRESENT";
   case VK_ERROR_INCOMPATIBLE_DRIVER:      return "INCOMPATIBLE_DRIVER";
   case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "FORMAT_NOT_SUPPORTED";
   case VK_ERROR_FRAGMENTED_POOL:          return "FRAGMENTED_POOL";
   case VK_ERROR_OUT_OF_POOL_MEMORY:       return "OUT_OF_POOL_MEMORY";
   case VK_ERROR_UNKNOWN:                  return "VK_ERROR_UNKNOWN";
   default:                                return "VkResult";
   }
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
vkfw_debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
              VkDebugUtilsMessageTypeFlagsEXT types,
              const VkDebugUtilsMessengerCallbackDataEXT *data,
              void *user_data)
{
   (void)types;

   const char *sev = "info";
   if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
      sev = "error";
   else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
      sev = "warning";

   /* Both fields are optional in the spec, and a null here would be a
    * crash inside the driver's error path — the worst possible place to
    * lose the message that says what went wrong. */
   t_note((test_ctx *)user_data, "vk %s [%s]: %s", sev,
          data->pMessageIdName ? data->pMessageIdName : "-",
          data->pMessage ? data->pMessage : "(no message)");

   return VK_FALSE;
}

bool vkfw_init(vkfw *fw, test_ctx *t, const void *features2)
{
   memset(fw, 0, sizeof(*fw));
   fw->t = t;

   /* Opting in to an unconformant driver, before vkCreateInstance
    * because the flag is read while the physical device is created.
    *
    * nvk_is_conformant() refuses anything that is not
    * NV_DEVICE_TYPE_DIS, and GM20B is NV_DEVICE_TYPE_SOC. The check is
    * telling the truth — nobody has run the CTS on this chip — so the
    * application says "I know" rather than the patch series saying
    * something it cannot support. Measured on console: without this,
    * vkEnumeratePhysicalDevices succeeds and returns no devices.
    */
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);

   /* horizon_gpu's default level is WARN, which is right for a driver
    * in use and wrong for a bring-up test. */
   setenv("HORIZON_GPU_LOG", "3", 1);

   PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)
      vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
   if (!t_check(t, create_instance != NULL,
                "GetInstanceProcAddr(vkCreateInstance)"))
      return false;

   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = test_name,
      .apiVersion = VK_API_VERSION_1_3,
   };

   /* Every vk_errorf in the driver is silent in a release build unless
    * a messenger is registered (vk_log.c). Chained into pNext as well
    * as created as an object: the chained one is live during
    * vkCreateInstance itself, the object from then on.
    */
   const VkDebugUtilsMessengerCreateInfoEXT dumci = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
      .pfnUserCallback = vkfw_debug_cb,
      .pUserData = t,
   };
   const char *const instance_exts[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
   const VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = &dumci,
      .pApplicationInfo = &app,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = instance_exts,
   };

   VkResult r = create_instance(&ici, NULL, &fw->instance);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateInstance -> %s",
                vkfw_result_str(r)))
      return false;

   /* Not fatal if it fails: the chained messenger already covers
    * instance creation, and losing later diagnostics is not a reason to
    * fail a test that can still run. */
   PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
      (PFN_vkCreateDebugUtilsMessengerEXT)
      vk_icdGetInstanceProcAddr(fw->instance,
                                "vkCreateDebugUtilsMessengerEXT");
   fw->destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)
      vk_icdGetInstanceProcAddr(fw->instance,
                                "vkDestroyDebugUtilsMessengerEXT");
   if (t_check(t, create_messenger != NULL && fw->destroy_messenger != NULL,
               "VK_EXT_debug_utils entry points resolved")) {
      r = create_messenger(fw->instance, &dumci, NULL, &fw->messenger);
      t_check(t, r == VK_SUCCESS, "vkCreateDebugUtilsMessengerEXT -> %s",
              vkfw_result_str(r));
   }

   /* The whole table, one line each. A missing entry point is named
    * rather than reached through a null pointer later. */
   bool all = true;
#define VKFW_LOAD(name)                                                  \
   fw->vk.name = (PFN_##name)                                            \
      vk_icdGetInstanceProcAddr(fw->instance, #name);                    \
   if (fw->vk.name == NULL) {                                            \
      t_check(t, false, "GetInstanceProcAddr(%s)", #name);               \
      all = false;                                                       \
   }
   VKFW_PROCS(VKFW_LOAD)
#undef VKFW_LOAD
   if (!t_check(t, all, "every entry point resolved"))
      goto fail;

   uint32_t pdev_count = 0;
   r = fw->vk.vkEnumeratePhysicalDevices(fw->instance, &pdev_count, NULL);
   if (!t_check(t, r == VK_SUCCESS && pdev_count >= 1,
                "vkEnumeratePhysicalDevices -> %s, %u device(s)",
                vkfw_result_str(r), pdev_count)) {
      /* Vulkan says an empty list, not an error, so the call succeeded
       * and the result says nothing about why. The driver logs its
       * reason to the screen, which the log file cannot reach. */
      t_note(t, "the driver found no GPU; its reason is on screen as an "
                "\"nvkmd_horizon:\" line");
      goto fail;
   }

   pdev_count = 1;
   r = fw->vk.vkEnumeratePhysicalDevices(fw->instance, &pdev_count,
                                         &fw->pdev);
   if (!t_check(t, r == VK_SUCCESS || r == VK_INCOMPLETE,
                "vkEnumeratePhysicalDevices(list) -> %s", vkfw_result_str(r)))
      goto fail;

   fw->vk.vkGetPhysicalDeviceProperties(fw->pdev, &fw->props);
   fw->vk.vkGetPhysicalDeviceMemoryProperties(fw->pdev, &fw->mem_props);
   t_note(t, "device: %s (api %u.%u.%u), %u memory type(s)",
          fw->props.deviceName,
          VK_VERSION_MAJOR(fw->props.apiVersion),
          VK_VERSION_MINOR(fw->props.apiVersion),
          VK_VERSION_PATCH(fw->props.apiVersion),
          fw->mem_props.memoryTypeCount);

   /* A queue family that can do both. Everything Phase 5 submits is
    * graphics, compute or transfer work, and NVK exposes one universal
    * family — but "the first one" is an assumption and this is not. */
   uint32_t qf_count = 0;
   fw->vk.vkGetPhysicalDeviceQueueFamilyProperties(fw->pdev, &qf_count, NULL);
   if (!t_check(t, qf_count >= 1 && qf_count <= 8,
                "queue family count is %u", qf_count))
      goto fail;

   VkQueueFamilyProperties qfp[8];
   fw->vk.vkGetPhysicalDeviceQueueFamilyProperties(fw->pdev, &qf_count, qfp);

   fw->queue_family = UINT32_MAX;
   for (uint32_t i = 0; i < qf_count; i++) {
      const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
      if ((qfp[i].queueFlags & need) == need && qfp[i].queueCount >= 1) {
         fw->queue_family = i;
         break;
      }
   }
   if (!t_check(t, fw->queue_family != UINT32_MAX,
                "a queue family with graphics and compute"))
      goto fail;

   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = fw->queue_family,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   const VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = features2,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
   };
   r = fw->vk.vkCreateDevice(fw->pdev, &dci, NULL, &fw->dev);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDevice -> %s",
                vkfw_result_str(r)))
      goto fail;

   fw->vk.vkGetDeviceQueue(fw->dev, fw->queue_family, 0, &fw->queue);
   if (!t_check(t, fw->queue != VK_NULL_HANDLE, "vkGetDeviceQueue"))
      goto fail;

   const VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = fw->queue_family,
   };
   r = fw->vk.vkCreateCommandPool(fw->dev, &cpci, NULL, &fw->pool);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateCommandPool -> %s",
                vkfw_result_str(r)))
      goto fail;

   return true;

fail:
   vkfw_finish(fw);
   return false;
}

void vkfw_finish(vkfw *fw)
{
   if (fw->dev != VK_NULL_HANDLE) {
      if (fw->pool != VK_NULL_HANDLE)
         fw->vk.vkDestroyCommandPool(fw->dev, fw->pool, NULL);
      fw->vk.vkDestroyDevice(fw->dev, NULL);
   }
   if (fw->instance != VK_NULL_HANDLE) {
      if (fw->messenger != VK_NULL_HANDLE && fw->destroy_messenger != NULL)
         fw->destroy_messenger(fw->instance, fw->messenger, NULL);
      if (fw->vk.vkDestroyInstance != NULL)
         fw->vk.vkDestroyInstance(fw->instance, NULL);
   }
   memset(fw, 0, sizeof(*fw));
}

uint32_t vkfw_memory_type(const vkfw *fw, uint32_t type_bits,
                          VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < fw->mem_props.memoryTypeCount; i++) {
      if (!(type_bits & (1u << i)))
         continue;
      if ((fw->mem_props.memoryTypes[i].propertyFlags & want) == want)
         return i;
   }
   return UINT32_MAX;
}

bool vkfw_buffer_create(vkfw *fw, VkDeviceSize size_B,
                        VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags mem_want,
                        vkfw_buffer *out)
{
   test_ctx *t = fw->t;
   memset(out, 0, sizeof(*out));
   out->size_B = size_B;

   const VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size_B,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkResult r = fw->vk.vkCreateBuffer(fw->dev, &bci, NULL, &out->buf);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateBuffer(0x%llx) -> %s",
                (unsigned long long)size_B, vkfw_result_str(r)))
      return false;

   VkMemoryRequirements mreq;
   fw->vk.vkGetBufferMemoryRequirements(fw->dev, out->buf, &mreq);
   out->alloc_B = mreq.size;

   const uint32_t type = vkfw_memory_type(fw, mreq.memoryTypeBits, mem_want);
   if (!t_check(t, type != UINT32_MAX,
                "a memory type with 0x%x that the buffer accepts",
                (unsigned)mem_want))
      goto fail_buf;

   out->coherent = (fw->mem_props.memoryTypes[type].propertyFlags &
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

   const VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mreq.size,
      .memoryTypeIndex = type,
   };
   r = fw->vk.vkAllocateMemory(fw->dev, &mai, NULL, &out->mem);
   if (!t_check(t, r == VK_SUCCESS, "vkAllocateMemory(0x%llx, type %u) -> %s",
                (unsigned long long)mreq.size, type, vkfw_result_str(r)))
      goto fail_buf;

   r = fw->vk.vkBindBufferMemory(fw->dev, out->buf, out->mem, 0);
   if (!t_check(t, r == VK_SUCCESS, "vkBindBufferMemory -> %s",
                vkfw_result_str(r)))
      goto fail_mem;

   if (mem_want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      r = fw->vk.vkMapMemory(fw->dev, out->mem, 0, VK_WHOLE_SIZE, 0,
                             &out->map);
      if (!t_check(t, r == VK_SUCCESS && out->map != NULL,
                   "vkMapMemory -> %s", vkfw_result_str(r)))
         goto fail_mem;
   }

   return true;

fail_mem:
   fw->vk.vkFreeMemory(fw->dev, out->mem, NULL);
   out->mem = VK_NULL_HANDLE;
fail_buf:
   fw->vk.vkDestroyBuffer(fw->dev, out->buf, NULL);
   out->buf = VK_NULL_HANDLE;
   return false;
}

void vkfw_buffer_destroy(vkfw *fw, vkfw_buffer *b)
{
   if (b->map != NULL)
      fw->vk.vkUnmapMemory(fw->dev, b->mem);
   if (b->mem != VK_NULL_HANDLE)
      fw->vk.vkFreeMemory(fw->dev, b->mem, NULL);
   if (b->buf != VK_NULL_HANDLE)
      fw->vk.vkDestroyBuffer(fw->dev, b->buf, NULL);
   memset(b, 0, sizeof(*b));
}

/* VK_WHOLE_SIZE rather than the requested size: the spec requires a
 * non-coherent range to be atom-aligned at both ends, and the whole
 * allocation is the one range that always is. */
static bool vkfw_maintain(vkfw *fw, const vkfw_buffer *b, bool to_gpu)
{
   if (b->coherent || b->map == NULL)
      return true;

   const VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = b->mem,
      .offset = 0,
      .size = VK_WHOLE_SIZE,
   };
   VkResult r = to_gpu
      ? fw->vk.vkFlushMappedMemoryRanges(fw->dev, 1, &range)
      : fw->vk.vkInvalidateMappedMemoryRanges(fw->dev, 1, &range);
   return t_check(fw->t, r == VK_SUCCESS, "%s -> %s",
                  to_gpu ? "vkFlushMappedMemoryRanges"
                         : "vkInvalidateMappedMemoryRanges",
                  vkfw_result_str(r));
}

bool vkfw_buffer_flush(vkfw *fw, const vkfw_buffer *b)
{
   return vkfw_maintain(fw, b, true);
}

bool vkfw_buffer_invalidate(vkfw *fw, const vkfw_buffer *b)
{
   return vkfw_maintain(fw, b, false);
}

bool vkfw_buffer_poison(vkfw *fw, vkfw_buffer *b, uint32_t pattern)
{
   if (!t_check(fw->t, b->map != NULL, "poison target is mapped"))
      return false;

   uint32_t *w = (uint32_t *)b->map;
   const uint64_t n = (uint64_t)b->alloc_B / sizeof(uint32_t);
   for (uint64_t i = 0; i < n; i++)
      w[i] = pattern;

   /* The flush is the half that matters and the half that is easy to
    * forget: poison left in the CPU's cache is not poison in memory,
    * and a GPU write that never happened would then be read back as the
    * *expected* value out of a dirty cache line. */
   return vkfw_buffer_flush(fw, b);
}

bool vkfw_image_supported(vkfw *fw, VkFormat format, VkImageType type,
                          VkImageTiling tiling, VkImageUsageFlags usage,
                          const char *what)
{
   VkImageFormatProperties props;
   VkResult r = fw->vk.vkGetPhysicalDeviceImageFormatProperties(
      fw->pdev, format, type, tiling, usage, 0, &props);

   /* The query answering "not supported" is a legitimate answer and the
    * query failing some other way is not, so they are told apart here
    * rather than folded into one boolean. */
   t_check(fw->t, r == VK_SUCCESS || r == VK_ERROR_FORMAT_NOT_SUPPORTED,
           "%s: image format query -> %s", what, vkfw_result_str(r));
   return r == VK_SUCCESS;
}

bool vkfw_image_create(vkfw *fw, VkFormat format, VkExtent3D extent,
                       uint32_t mip_levels, uint32_t array_layers,
                       VkImageUsageFlags usage, VkImageTiling tiling,
                       vkfw_image *out)
{
   test_ctx *t = fw->t;
   memset(out, 0, sizeof(*out));
   out->format = format;
   out->extent = extent;
   out->tiling = tiling;
   out->mip_levels = mip_levels;
   out->array_layers = array_layers;

   const VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = extent.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = extent,
      .mipLevels = mip_levels,
      .arrayLayers = array_layers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = tiling,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkResult r = fw->vk.vkCreateImage(fw->dev, &ici, NULL, &out->img);
   if (!t_check(t, r == VK_SUCCESS,
                "vkCreateImage(%d, %ux%ux%u, %u mip, %u layer, %s) -> %s",
                (int)format, extent.width, extent.height, extent.depth,
                mip_levels, array_layers,
                tiling == VK_IMAGE_TILING_LINEAR ? "linear" : "optimal",
                vkfw_result_str(r)))
      return false;

   VkMemoryRequirements mreq;
   fw->vk.vkGetImageMemoryRequirements(fw->dev, out->img, &mreq);
   out->alloc_B = mreq.size;
   out->align_B = mreq.alignment;

   /* DEVICE_LOCAL, not host visible: an off-screen image is read back
    * through a copy to a buffer, which is what the hardware does well
    * and what keeps the image's tiling out of the CPU's business. */
   const uint32_t type = vkfw_memory_type(fw, mreq.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (!t_check(t, type != UINT32_MAX,
                "a device-local memory type the image accepts"))
      goto fail_img;

   const VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mreq.size,
      .memoryTypeIndex = type,
   };
   r = fw->vk.vkAllocateMemory(fw->dev, &mai, NULL, &out->mem);
   if (!t_check(t, r == VK_SUCCESS,
                "vkAllocateMemory(image, 0x%llx align 0x%llx) -> %s",
                (unsigned long long)mreq.size,
                (unsigned long long)mreq.alignment, vkfw_result_str(r)))
      goto fail_img;

   r = fw->vk.vkBindImageMemory(fw->dev, out->img, out->mem, 0);
   if (!t_check(t, r == VK_SUCCESS, "vkBindImageMemory -> %s",
                vkfw_result_str(r)))
      goto fail_mem;

   return true;

fail_mem:
   fw->vk.vkFreeMemory(fw->dev, out->mem, NULL);
   out->mem = VK_NULL_HANDLE;
fail_img:
   fw->vk.vkDestroyImage(fw->dev, out->img, NULL);
   out->img = VK_NULL_HANDLE;
   return false;
}

void vkfw_image_destroy(vkfw *fw, vkfw_image *i)
{
   if (i->img != VK_NULL_HANDLE)
      fw->vk.vkDestroyImage(fw->dev, i->img, NULL);
   if (i->mem != VK_NULL_HANDLE)
      fw->vk.vkFreeMemory(fw->dev, i->mem, NULL);
   memset(i, 0, sizeof(*i));
}

bool vkfw_cmd_begin(vkfw *fw, VkCommandBuffer *cb_out)
{
   test_ctx *t = fw->t;
   *cb_out = VK_NULL_HANDLE;

   const VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = fw->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkResult r = fw->vk.vkAllocateCommandBuffers(fw->dev, &cbai, cb_out);
   if (!t_check(t, r == VK_SUCCESS, "vkAllocateCommandBuffers -> %s",
                vkfw_result_str(r)))
      return false;

   const VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   r = fw->vk.vkBeginCommandBuffer(*cb_out, &cbbi);
   if (!t_check(t, r == VK_SUCCESS, "vkBeginCommandBuffer -> %s",
                vkfw_result_str(r)))
      return false;

   return true;
}

bool vkfw_cmd_end_submit(vkfw *fw, VkCommandBuffer cb, VkFence fence)
{
   test_ctx *t = fw->t;

   VkResult r = fw->vk.vkEndCommandBuffer(cb);
   if (!t_check(t, r == VK_SUCCESS, "vkEndCommandBuffer -> %s",
                vkfw_result_str(r)))
      return false;

   const VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cb,
   };
   r = fw->vk.vkQueueSubmit(fw->queue, 1, &si, fence);
   return t_check(t, r == VK_SUCCESS, "vkQueueSubmit -> %s",
                  vkfw_result_str(r));
}

/* Five seconds. Long enough that a slow but working GPU is never
 * mistaken for a hang, short enough that a hang is reported as one
 * instead of leaving the console apparently frozen. */
#define VKFW_FENCE_TIMEOUT_NS UINT64_C(5000000000)

bool vkfw_submit_and_wait(vkfw *fw, VkCommandBuffer cb, const char *what)
{
   test_ctx *t = fw->t;

   const VkFenceCreateInfo fci = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   VkFence fence = VK_NULL_HANDLE;
   VkResult r = fw->vk.vkCreateFence(fw->dev, &fci, NULL, &fence);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateFence(%s) -> %s", what,
                vkfw_result_str(r)))
      return false;

   bool ok = vkfw_cmd_end_submit(fw, cb, fence);
   if (ok) {
      r = fw->vk.vkWaitForFences(fw->dev, 1, &fence, VK_TRUE,
                                 VKFW_FENCE_TIMEOUT_NS);
      ok = t_check(t, r == VK_SUCCESS, "vkWaitForFences(%s) -> %s", what,
                   vkfw_result_str(r));
   }

   fw->vk.vkDestroyFence(fw->dev, fence, NULL);
   return ok;
}

uint32_t vkfw_expect_words(vkfw *fw, const void *got, uint32_t expect,
                           uint32_t n, const char *what)
{
   const uint32_t *w = (const uint32_t *)got;
   uint32_t wrong = 0, first = 0;
   for (uint32_t i = 0; i < n; i++) {
      if (w[i] != expect) {
         if (wrong == 0)
            first = i;
         wrong++;
      }
   }
   /* One check, and it names the first mismatch. A per-word check would
    * bury the summary under a thousand lines and a bare count would not
    * say where to look. */
   t_check(fw->t, wrong == 0,
           "%s: %u/%u words are 0x%08x%s", what, n - wrong, n, expect,
           wrong ? "" : " (all of them)");
   if (wrong != 0) {
      t_note(fw->t, "%s: first mismatch at word %u: got 0x%08x, want 0x%08x "
             "(%u wrong)", what, first, w[first], expect, wrong);
   }
   return wrong;
}

uint32_t vkfw_expect_words_array(vkfw *fw, const void *got,
                                 const uint32_t *expect, uint32_t n,
                                 const char *what)
{
   const uint32_t *w = (const uint32_t *)got;
   uint32_t wrong = 0, first = 0;
   for (uint32_t i = 0; i < n; i++) {
      if (w[i] != expect[i]) {
         if (wrong == 0)
            first = i;
         wrong++;
      }
   }
   t_check(fw->t, wrong == 0, "%s: %u/%u words match", what, n - wrong, n);
   if (wrong != 0) {
      t_note(fw->t, "%s: first mismatch at word %u: got 0x%08x, want 0x%08x "
             "(%u wrong)", what, first, w[first], expect[first], wrong);
   }
   return wrong;
}
