/*
 * vkfw — implementation. See vkfw.h for what it is and what it refuses
 * to do.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>    /* snprintf */
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
   default:                                break;
   }

   /* THE FALLBACK THE HEADER PROMISED AND THIS DID NOT HAVE. It
    * returned the literal "VkResult", so anything outside the list
    * above — VK_ERROR_VALIDATION_FAILED_EXT, INVALID_EXTERNAL_HANDLE,
    * PIPELINE_COMPILE_REQUIRED, anything a newer header adds — logged
    * as `vkCreateDevice -> VkResult` with the code thrown away. This
    * is the string every failure line in the suite is built on. Found
    * in review of PR #7.
    *
    * A small rotation of buffers, because a check line can name two
    * results in one printf. */
   static char buf[4][24];
   static uint32_t next;
   char *out = buf[next++ % 4];
   snprintf(out, sizeof(buf[0]), "VkResult %d", (int)r);
   return out;
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

   vkfw *fw = (vkfw *)user_data;

   /* Both fields are optional in the spec, and a null here would be a
    * crash inside the driver's error path — the worst possible place to
    * lose the message that says what went wrong. */
   const char *text = data->pMessage ? data->pMessage : "(no message)";

   t_note(fw->t, "vk %s [%s]: %s", sev,
          data->pMessageIdName ? data->pMessageIdName : "-", text);

   /* Remembered as well as logged, so a test can assert on what the
    * driver said rather than on a log a human has to read. Oldest
    * messages are kept and later ones dropped: the decisions worth
    * asserting on are made at creation. */
   if (fw->message_count < VKFW_MESSAGE_SLOTS) {
      snprintf(fw->messages[fw->message_count], VKFW_MESSAGE_CHARS,
               "%s", text);
   }
   fw->message_count++;

   return VK_FALSE;
}

bool vkfw_init(vkfw *fw, test_ctx *t, const void *features2)
{
   return vkfw_init_ext(fw, t, features2, NULL, 0);
}

bool vkfw_init_ext(vkfw *fw, test_ctx *t, const void *features2,
                   const char *const *device_exts, uint32_t device_ext_count)
{
   return vkfw_init_full(fw, t, features2, NULL, 0,
                         device_exts, device_ext_count);
}

void vkfw_forget_messages(vkfw *fw)
{
   fw->message_count = 0;
   memset(fw->messages, 0, sizeof(fw->messages));
}

bool vkfw_saw_message(const vkfw *fw, const char *needle, const char **out)
{
   const uint32_t n = fw->message_count < VKFW_MESSAGE_SLOTS ?
      fw->message_count : VKFW_MESSAGE_SLOTS;

   for (uint32_t i = 0; i < n; i++) {
      if (strstr(fw->messages[i], needle) != NULL) {
         if (out != NULL)
            *out = fw->messages[i];
         return true;
      }
   }
   return false;
}

bool vkfw_wsi_load(vkfw *fw)
{
   bool all = true;
#define VKFW_WSI_LOAD(name)                                              \
   fw->wsi.name = (PFN_##name)                                           \
      vk_icdGetInstanceProcAddr(fw->instance, #name);                    \
   if (fw->wsi.name == NULL) {                                           \
      t_check(fw->t, false, "GetInstanceProcAddr(%s)", #name);           \
      all = false;                                                       \
   }
   VKFW_WSI_PROCS(VKFW_WSI_LOAD)
#undef VKFW_WSI_LOAD

   fw->wsi_loaded = all;
   return t_check(fw->t, all, "every window-system entry point resolved");
}

bool vkfw_init_full(vkfw *fw, test_ctx *t, const void *features2,
                    const char *const *instance_exts,
                    uint32_t instance_ext_count,
                    const char *const *device_exts, uint32_t device_ext_count)
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
    * in use and wrong for a bring-up test. Overwriting, not
    * defaulting: this one is set from outside often enough while
    * debugging that a silent override would waste somebody's
    * afternoon, so it defers to an existing value and says which it
    * used. Found in review of PR #7. */
   if (getenv("HORIZON_GPU_LOG") == NULL)
      setenv("HORIZON_GPU_LOG", "3", 1);
   else
      t_note(t, "HORIZON_GPU_LOG was already %s in the environment; "
                "leaving it", getenv("HORIZON_GPU_LOG"));

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
      /* INFO as well, since Phase 6: the Horizon WSI reports which
       * present path a swapchain got as an INFO message, and a
       * decision an application cannot hear about is not observable at
       * run time whatever the log says. */
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
      .pfnUserCallback = vkfw_debug_cb,
      /* The fixture, not the test context: the callback records what it
       * is told as well as logging it, and fw->t is already set. */
      .pUserData = fw,
   };

   /* VK_EXT_debug_utils plus whatever the caller asked for. Bounded by
    * the array rather than by a promise: a caller asking for more than
    * this many is a mistake that stops here instead of writing past it.
    */
   const char *all_instance_exts[8] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
   uint32_t all_instance_ext_count = 1;
   for (uint32_t i = 0; i < instance_ext_count; i++) {
      if (!t_check(t, all_instance_ext_count <
                      (uint32_t)(sizeof(all_instance_exts) /
                                 sizeof(all_instance_exts[0])),
                   "instance extension list fits (%u asked for)",
                   instance_ext_count + 1))
         return false;
      all_instance_exts[all_instance_ext_count++] = instance_exts[i];
   }

   const VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = &dumci,
      .pApplicationInfo = &app,
      .enabledExtensionCount = all_instance_ext_count,
      .ppEnabledExtensionNames = all_instance_exts,
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
      .enabledExtensionCount = device_ext_count,
      .ppEnabledExtensionNames = device_exts,
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
      /* This fixture deliberately exposes a submit that does not wait,
       * so item 9 can keep eight in flight — which means any test
       * taking a failure path mid-flight arrives here with work
       * outstanding, and destroying a command pool or a device with
       * outstanding work is invalid usage. Found in review of PR #7.
       *
       * Unchecked on purpose: this runs on the teardown path, often
       * after a failure, and a device that is already lost will report
       * it again here. The point is to drain when draining is possible,
       * not to add a check to the end of every test. */
      if (fw->vk.vkDeviceWaitIdle != NULL)
         (void)fw->vk.vkDeviceWaitIdle(fw->dev);
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
   if (!vkfw_buffer_flush(fw, b))
      return false;

   /* AND THEN IT IS CHECKED, because every readback in this suite rests
    * on it and nothing was measuring it.
    *
    * Hardware run 2, 2026-08-04: t_vk_transfer case B and probe F issued
    * the *same* copy of [1028, 3480) in the same process and disagreed
    * about which bytes changed — B saw four bytes before the region and
    * eight after holding the source's pattern, F saw neither. A copy
    * cannot be non-deterministic in that way, but a destination whose
    * poison never reached memory can look exactly like one: the scan
    * then finds whatever the *previous* case left there and reports it
    * as this case's overrun.
    *
    * So the poison is invalidated and read back before it is trusted. A
    * failure here says the measurement is invalid, which is a different
    * and much more useful statement than the wrong conclusion it would
    * otherwise support — and this is the exact shape of the mistake this
    * project keeps finding: a step that reports success without having
    * verified anything.
    */
   if (!vkfw_buffer_invalidate(fw, b))
      return false;

   uint64_t wrong = 0, first = 0;
   for (uint64_t i = 0; i < n; i++) {
      if (w[i] != pattern) {
         if (wrong == 0)
            first = i;
         wrong++;
      }
   }
   if (!t_check(fw->t, wrong == 0,
                "the poison reached memory (%llu/%llu words)",
                (unsigned long long)(n - wrong), (unsigned long long)n)) {
      t_note(fw->t, "poison: first surviving word %llu is 0x%08x, not "
             "0x%08x — every readback after this measures something else",
             (unsigned long long)first, w[first], pattern);
      return false;
   }

   return true;
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
   fw->last_submit_result = r;
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
   bool safe_to_destroy = !ok;   /* nothing was submitted */
   if (ok) {
      r = fw->vk.vkWaitForFences(fw->dev, 1, &fence, VK_TRUE,
                                 VKFW_FENCE_TIMEOUT_NS);
      /* Not overwritten with a success: a submit that already reported
       * DEVICE_LOST has lost the device whatever the wait says. */
      if (r != VK_SUCCESS)
         fw->last_submit_result = r;
      ok = t_check(t, r == VK_SUCCESS, "vkWaitForFences(%s) -> %s", what,
                   vkfw_result_str(r));
      /* VK_SUCCESS means the submit retired. DEVICE_LOST means it never
       * will and the fence is not in use by anything that can still
       * run. VK_TIMEOUT means it is STILL PENDING, and destroying a
       * fence a pending submit owns is invalid usage — triggered
       * exactly on a hang, which is the case where the report has to
       * be trustworthy. Found in review of PR #7. */
      safe_to_destroy = (r == VK_SUCCESS || r == VK_ERROR_DEVICE_LOST);
      if (!safe_to_destroy) {
         t_note(t, "%s: the fence is still pending after the wait, so it "
                   "is leaked rather than destroyed — a fence a submit "
                   "still owns must not be destroyed", what);
      }
   }

   if (safe_to_destroy)
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

static bool gfx_module(vkfw *fw, const uint32_t *code, size_t size_B,
                       const char *what, const char *stage,
                       VkShaderModule *out)
{
   const VkShaderModuleCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = size_B,
      .pCode = code,
   };
   VkResult r = fw->vk.vkCreateShaderModule(fw->dev, &ci, NULL, out);
   return t_check(fw->t, r == VK_SUCCESS,
                  "%s: vkCreateShaderModule(%s) -> %s", what, stage,
                  vkfw_result_str(r));
}

bool vkfw_gfx_create(vkfw *fw, const char *what, const vkfw_gfx_desc *desc,
                     vkfw_gfx *out)
{
   test_ctx *t = fw->t;
   memset(out, 0, sizeof(*out));

   if (!gfx_module(fw, desc->vs_spv, desc->vs_B, what, "vertex", &out->vs))
      return false;
   if (!gfx_module(fw, desc->fs_spv, desc->fs_B, what, "fragment", &out->fs))
      return false;

   /* The layout has to match what the shaders declare, and an empty one
    * against a shader that declares a binding is what took a console
    * down in t_vk_image run 2 — no validation layers here to say so.
    * The caller passes the set layout its shaders were written for, or
    * VK_NULL_HANDLE when they declare nothing. */
   const VkPushConstantRange pcr = {
      .stageFlags = desc->push_constant_stages,
      .offset = 0,
      .size = desc->push_constant_B,
   };
   const VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = desc->set_layout != VK_NULL_HANDLE ? 1u : 0u,
      .pSetLayouts = desc->set_layout != VK_NULL_HANDLE ? &desc->set_layout
                                                        : NULL,
      .pushConstantRangeCount = desc->push_constant_B != 0 ? 1u : 0u,
      .pPushConstantRanges = desc->push_constant_B != 0 ? &pcr : NULL,
   };
   VkResult r = fw->vk.vkCreatePipelineLayout(fw->dev, &plci, NULL,
                                              &out->layout);
   if (!t_check(t, r == VK_SUCCESS, "%s: vkCreatePipelineLayout -> %s",
                what, vkfw_result_str(r)))
      return false;

   const VkPipelineShaderStageCreateInfo stages[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = out->vs,
         .pName = "main",
      },
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = out->fs,
         .pName = "main",
      },
   };

   const VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = desc->binding_count,
      .pVertexBindingDescriptions = desc->bindings,
      .vertexAttributeDescriptionCount = desc->attr_count,
      .pVertexAttributeDescriptions = desc->attrs,
   };

   const VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };

   const VkViewport viewport = {
      .x = 0.0f, .y = 0.0f,
      .width = (float)desc->width, .height = (float)desc->height,
      .minDepth = 0.0f, .maxDepth = 1.0f,
   };
   const VkRect2D scissor = {
      .offset = { 0, 0 },
      .extent = { desc->width, desc->height },
   };
   const VkPipelineViewportStateCreateInfo vp = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = desc->dynamic_viewport ? NULL : &viewport,
      .scissorCount = 1,
      .pScissors = desc->dynamic_viewport ? NULL : &scissor,
   };
   const VkDynamicState dyn_states[2] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
   };
   const VkPipelineDynamicStateCreateInfo dyn = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dyn_states,
   };

   const VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
   };

   const VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };

   /* Supplied even with no depth attachment rather than left NULL: a
    * NULL pDepthStencilState is legal only when the pass provably has
    * none, and "all off" spelled out cannot be misread. */
   const VkPipelineDepthStencilStateCreateInfo ds = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = desc->depth_test ? VK_TRUE : VK_FALSE,
      .depthWriteEnable = desc->depth_write ? VK_TRUE : VK_FALSE,
      .depthCompareOp = desc->depth_test ? desc->depth_compare
                                         : VK_COMPARE_OP_ALWAYS,
   };

   const VkPipelineColorBlendAttachmentState cba = {
      .blendEnable = VK_FALSE,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   const VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &cba,
   };

   const VkPipelineRenderingCreateInfo prci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &desc->colour_format,
      .depthAttachmentFormat = desc->depth_format,
      .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
   };

   const VkGraphicsPipelineCreateInfo gpci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &prci,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vi,
      .pInputAssemblyState = &ia,
      .pViewportState = &vp,
      .pRasterizationState = &rs,
      .pMultisampleState = &ms,
      .pDepthStencilState = &ds,
      .pColorBlendState = &cb,
      .pDynamicState = desc->dynamic_viewport ? &dyn : NULL,
      .layout = out->layout,
      .renderPass = VK_NULL_HANDLE,   /* dynamic rendering */
   };

   r = fw->vk.vkCreateGraphicsPipelines(fw->dev, VK_NULL_HANDLE, 1, &gpci,
                                        NULL, &out->pipeline);
   return t_check(t, r == VK_SUCCESS, "%s: vkCreateGraphicsPipelines -> %s",
                  what, vkfw_result_str(r));
}

void vkfw_gfx_destroy(vkfw *fw, vkfw_gfx *g)
{
   if (g->pipeline != VK_NULL_HANDLE)
      fw->vk.vkDestroyPipeline(fw->dev, g->pipeline, NULL);
   if (g->layout != VK_NULL_HANDLE)
      fw->vk.vkDestroyPipelineLayout(fw->dev, g->layout, NULL);
   if (g->fs != VK_NULL_HANDLE)
      fw->vk.vkDestroyShaderModule(fw->dev, g->fs, NULL);
   if (g->vs != VK_NULL_HANDLE)
      fw->vk.vkDestroyShaderModule(fw->dev, g->vs, NULL);
   memset(g, 0, sizeof(*g));
}
