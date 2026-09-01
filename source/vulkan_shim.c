/* vulkan_shim.c -- present the static Switch Vulkan (NVK, over the VI surface)
 * as the Android Vulkan the engine expects.
 *
 * libgodot_android.so is an Android build: its Vulkan path wants
 * VK_KHR_android_surface + vkCreateAndroidSurfaceKHR(ANativeWindow*). The static
 * driver exports VK_NN_vi_surface + vkCreateViSurfaceNN(NWindow*) instead --
 * Nintendo's own surface extension, backed by a real WSI over libnx NWindow.
 * The two create-structs are the same shape, so the whole gap is one function
 * and one extension name, translated in both directions:
 *
 *   enumerate  VI -> Android  so the engine's extension check passes
 *   create     Android -> VI  so the driver accepts what the engine asked for
 *   surface    Android -> VI  substituting nwindowGetDefault()
 *
 * The driver is linked statically, so "libvulkan.so" is a dlopen sentinel and
 * dlsym resolves here (see egl_shim.c). */

#include <string.h>
#include <stdlib.h>
#include <switch.h>

#define VK_USE_PLATFORM_VI_NN
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include "vulkan_shim.h"
#include "config.h"

extern void debugPrintf(const char *fmt, ...);

// Mesa's GLSL type system allocates from a global context that must be set up
// before any type is created. On the Vulkan path nothing here does it, so the
// first shader with a uniform/storage block reads a NULL allocator and dies
// (spirv_to_nir -> vtn_handle_type -> glsl_interface_type). Call it ourselves,
// exactly as the GL drivers in this same Mesa do. Refcounted, so an extra ref
// is harmless -- it just keeps the cache alive for the whole session.
extern void glsl_type_singleton_init_or_ref(void);

// --- surface: vkCreateAndroidSurfaceKHR implemented over VK_NN_vi_surface -----
// The ANativeWindow the engine passes is the wrapper's fake one; the real target
// is libnx's default NWindow, which is what the Switch WSI drives.
static VKAPI_ATTR VkResult VKAPI_CALL
shim_vkCreateAndroidSurfaceKHR(VkInstance inst,
                               const VkAndroidSurfaceCreateInfoKHR *info,
                               const VkAllocationCallbacks *alloc,
                               VkSurfaceKHR *out) {
  NWindow *win = nwindowGetDefault();
  if (!win) {
    debugPrintf("[vk] nwindowGetDefault() returned NULL -- no surface\n");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkViSurfaceCreateInfoNN vi = {
    .sType  = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN,
    .pNext  = info ? info->pNext : NULL,
    .flags  = 0,
    .window = win,
  };
  VkResult r = vkCreateViSurfaceNN(inst, &vi, alloc, out);
  debugPrintf("[vk] CreateAndroidSurface -> CreateViSurfaceNN(nwindow=%p) = %d\n",
              (void *)win, (int)r);
  return r;
}

// --- extension name: report VI as Android so the engine's check passes -------
static VKAPI_ATTR VkResult VKAPI_CALL
shim_vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                            VkExtensionProperties *props) {
  VkResult r = vkEnumerateInstanceExtensionProperties(layer, count, props);
  if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && props && count)
    for (uint32_t i = 0; i < *count; i++)
      if (!strcmp(props[i].extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME)) {
        strncpy(props[i].extensionName, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
                sizeof(props[i].extensionName) - 1);
        props[i].extensionName[sizeof(props[i].extensionName) - 1] = '\0';
      }
  return r;
}

// --- create instance: translate Android back to VI (driver only knows VI) ----
static VKAPI_ATTR VkResult VKAPI_CALL
shim_vkCreateInstance(const VkInstanceCreateInfo *info,
                      const VkAllocationCallbacks *alloc, VkInstance *out) {
  if (!info) return vkCreateInstance(info, alloc, out);
  VkInstanceCreateInfo patched = *info;
  const char **names = NULL;
  if (info->enabledExtensionCount && info->ppEnabledExtensionNames) {
    names = calloc(info->enabledExtensionCount, sizeof(*names));
    if (!names) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
      const char *n = info->ppEnabledExtensionNames[i];
      names[i] = (n && !strcmp(n, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME))
                   ? VK_NN_VI_SURFACE_EXTENSION_NAME : n;
    }
    patched.ppEnabledExtensionNames = names;
  }
  VkResult r = vkCreateInstance(&patched, alloc, out);
  free(names);
  debugPrintf("[vk] vkCreateInstance -> %d (%u extensions)\n",
              (int)r, (unsigned)info->enabledExtensionCount);
  return r;
}

// --- proc address ------------------------------------------------------------
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
shim_vkGetDeviceProcAddr(VkDevice dev, const char *name) {
  if (name && !strcmp(name, "vkGetDeviceProcAddr"))
    return (PFN_vkVoidFunction)shim_vkGetDeviceProcAddr;
  return vkGetDeviceProcAddr(dev, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
shim_vkGetInstanceProcAddr(VkInstance inst, const char *name) {
  if (!name) return NULL;
  if (!strcmp(name, "vkCreateAndroidSurfaceKHR"))               return (PFN_vkVoidFunction)shim_vkCreateAndroidSurfaceKHR;
  if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))  return (PFN_vkVoidFunction)shim_vkEnumerateInstanceExtensionProperties;
  if (!strcmp(name, "vkCreateInstance"))                        return (PFN_vkVoidFunction)shim_vkCreateInstance;
  if (!strcmp(name, "vkGetInstanceProcAddr"))                   return (PFN_vkVoidFunction)shim_vkGetInstanceProcAddr;
  if (!strcmp(name, "vkGetDeviceProcAddr"))                     return (PFN_vkVoidFunction)shim_vkGetDeviceProcAddr;
  return vkGetInstanceProcAddr(inst, name);
}

// --- dlsym() on the fake libvulkan.so handle (see egl_shim.c) -----------------
// Godot resolves most entry points through vkGetInstanceProcAddr, so the four
// the loader fetches by name go here and everything else defers to the driver.
void *vulkan_shim_find(const char *symbol) {
  if (!symbol) return NULL;
  if (!strcmp(symbol, "vkGetInstanceProcAddr"))                  return (void *)&shim_vkGetInstanceProcAddr;
  if (!strcmp(symbol, "vkCreateInstance"))                       return (void *)&shim_vkCreateInstance;
  if (!strcmp(symbol, "vkEnumerateInstanceExtensionProperties")) return (void *)&shim_vkEnumerateInstanceExtensionProperties;
  if (!strcmp(symbol, "vkCreateAndroidSurfaceKHR"))              return (void *)&shim_vkCreateAndroidSurfaceKHR;
  if (!strncmp(symbol, "vk", 2)) {
    void *p = (void *)vkGetInstanceProcAddr(VK_NULL_HANDLE, symbol);
    if (p) return p;
  }
  return NULL;
}

// --- probe: verify the driver actually comes up (advertised != works) --------
// Checking the extension list is not the same as a driver starting: on hardware
// the list can be perfect and vkCreateInstance still fail. So actually create an
// instance and throw it away; if that fails we return 0 and main() sets up the
// GLES3 context instead.
int vulkan_shim_probe(void) {
  uint32_t n = 0;
  VkResult r = vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
    debugPrintf("[vk] probe: EnumerateInstanceExtensionProperties -> %d\n", (int)r);
    return 0;
  }
  VkExtensionProperties *props = calloc(n ? n : 1, sizeof(*props));
  if (!props) return 0;
  int have_vi = 0, have_surface = 0;
  if (vkEnumerateInstanceExtensionProperties(NULL, &n, props) == VK_SUCCESS)
    for (uint32_t i = 0; i < n; i++) {
      if (!strcmp(props[i].extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME)) have_vi = 1;
      if (!strcmp(props[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME))   have_surface = 1;
    }
  free(props);
  debugPrintf("[vk] probe: VK_KHR_surface=%d VK_NN_vi_surface=%d\n",
              have_surface, have_vi);
  if (!have_surface || !have_vi) return 0;

  // We already know a real instance creates and drives NVK fine (earlier builds
  // did). DON'T spin up a throwaway instance here: tearing it down hangs under
  // NVK -- instance destruction waits on the C11 threading and never returns.
  // Godot creates the one real instance and never tears it down until exit.
  debugPrintf("[vk] probe: extensions OK -- using Vulkan\n");
  glsl_type_singleton_init_or_ref();
  debugPrintf("[vk] glsl_type_singleton_init_or_ref() called\n");
  return 1;
}
