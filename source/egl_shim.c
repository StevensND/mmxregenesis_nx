/* egl_shim.c -- dlopen/dlsym bridge to mesa EGL/GLES. SDL's Android backend
 * dlopen()s libEGL.so and resolves egl* by name; we answer with mesa.
 * MIT license; see LICENSE. */

#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "egl_shim.h"
#include "imports.h"
#include "config.h"
#include "vulkan_shim.h"

#define FAKE_DL_HANDLE ((void *)0xE61D1B)
#define VULKAN_DL_HANDLE ((void *)0x76554B)  // dlopen("libvulkan.so") sentinel

extern void debugPrintf(const char *fmt, ...);

static int str_ends(const char *s, const char *suf) {
  size_t ls = strlen(s), n = strlen(suf);
  return ls >= n && !strcmp(s + ls - n, suf);
}

typedef void (*generic_func)(void);
typedef struct { const char *name; generic_func fn; } EglEntry;

#define E(sym) { #sym, (generic_func)sym }

// core EGL entry points (eglGetProcAddress only returns extension/GL functions)
static const EglEntry egl_table[] = {
  E(eglGetError),
  E(eglGetDisplay),
  E(eglInitialize),
  E(eglTerminate),
  E(eglQueryString),
  E(eglGetConfigs),
  E(eglChooseConfig),
  E(eglGetConfigAttrib),
  E(eglCreateWindowSurface),
  E(eglCreatePbufferSurface),
  E(eglCreatePixmapSurface),
  E(eglDestroySurface),
  E(eglQuerySurface),
  E(eglBindAPI),
  E(eglQueryAPI),
  E(eglWaitClient),
  E(eglReleaseThread),
  E(eglCreatePbufferFromClientBuffer),
  E(eglSurfaceAttrib),
  E(eglBindTexImage),
  E(eglReleaseTexImage),
  E(eglSwapInterval),
  E(eglCreateContext),
  E(eglDestroyContext),
  E(eglMakeCurrent),
  E(eglGetCurrentContext),
  E(eglGetCurrentSurface),
  E(eglGetCurrentDisplay),
  E(eglQueryContext),
  E(eglWaitGL),
  E(eglWaitNative),
  E(eglSwapBuffers),
  E(eglCopyBuffers),
  E(eglGetProcAddress),
};

void *dlopen_fake(const char *filename, int flag) {
  (void)flag;
  // libvulkan.so: the driver is linked statically into the .nro, so there is
  // nothing to load -- hand back a sentinel and let dlsym resolve through
  // vulkan_shim. Gated on enable_vulkan: refusing when off makes the engine
  // fall back to GLES3 instead of half-initialising Vulkan.
  if (filename && config.enable_vulkan &&
      (str_ends(filename, "libvulkan.so") || str_ends(filename, "libvulkan.so.1"))) {
    debugPrintf("[dl] %s -> static Vulkan\n", filename);
    return VULKAN_DL_HANDLE;
  }
  return FAKE_DL_HANDLE; // any GL/EGL library (or NULL) maps to the bridge
}

void *dlsym_fake(void *handle, const char *symbol) {
  if (!symbol)
    return NULL;
  if (handle == VULKAN_DL_HANDLE)
    return vulkan_shim_find(symbol);
  for (unsigned i = 0; i < sizeof(egl_table) / sizeof(*egl_table); i++) {
    if (strcmp(symbol, egl_table[i].name) == 0)
      return (void *)egl_table[i].fn;
  }
  // anything the wrapper already provides via the import table (Godot dlopens
  // itself/other libs and dlsyms plain libc/GL names)
  uintptr_t imp = mmx_find_import(symbol);
  if (imp)
    return (void *)imp;
  return (void *)eglGetProcAddress(symbol); // GL entry points + EGL extensions
}

int dlclose_fake(void *handle) { (void)handle; return 0; }

char *dlerror_fake(void) { return NULL; }
