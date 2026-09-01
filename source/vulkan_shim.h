/* vulkan_shim.h -- Android-shaped Vulkan over the Switch VI surface.
 * See vulkan_shim.c. */
#ifndef __VULKAN_SHIM_H__
#define __VULKAN_SHIM_H__

// dlsym() on the fake libvulkan.so handle. Returns the shimmed entry points and
// defers everything else to the static driver's vkGetInstanceProcAddr.
void *vulkan_shim_find(const char *symbol);

// 1 if the driver (NVK) comes up and reports VK_KHR_surface + VK_NN_vi_surface,
// else 0. Called once before the engine starts so a missing/broken driver is a
// log line here rather than a fault deep inside the renderer.
int vulkan_shim_probe(void);

#endif
