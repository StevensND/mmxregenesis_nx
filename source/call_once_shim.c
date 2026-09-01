/* call_once_shim.c -- a C11 call_once that actually runs the function.
 *
 * Mesa's one-time initialisation goes through util_call_once ->
 * util_call_once_data_slow -> C11 call_once(). Whatever devkitPro's newlib
 * provides for that on this target, the observed behaviour is that the
 * initialiser does not run:
 *
 *   vk_common_CreateComputePipelines
 *     vk_pipeline_precompile_shader -> vk_spirv_to_nir -> spirv_to_nir
 *       vtn_foreach_instruction -> vtn_handle_type
 *         glsl_interface_type
 *           linear_zalloc_child      <- ldp w1, w6, [x0, #4], x0 == NULL
 *
 * `glsl_interface_type` allocates from the GLSL type cache's linear context.
 * A NULL context there means the cache was never set up, which is what a
 * call_once that does nothing produces -- and it surfaces far away, on the
 * first shader with a uniform or storage block, rather than at init.
 *
 * Defining call_once here overrides the library's: object files take
 * precedence over archive members at link time.
 *
 * ===========================================================================
 * Why a side table instead of writing through the flag
 * ===========================================================================
 *
 * The obvious implementation stores state in *flag. That requires knowing
 * once_flag's layout and, crucially, the value of ONCE_FLAG_INIT -- and if
 * this platform's ONCE_FLAG_INIT is not what we assume, the result is either
 * "never runs" (the bug we are fixing) or "runs every time" (worse, since
 * these initialisers allocate).
 *
 * Keying on the flag's ADDRESS needs neither. Mesa's once_flags are static
 * objects with stable addresses, which is exactly what this relies on.
 *
 * MIT license; see LICENSE. */

// ===========================================================================
// OFF BY DEFAULT. Build with -DSHIM_CALL_ONCE=1 to enable.
// ===========================================================================
//
// This was written for a NULL glsl_type_cache linear context, on the theory
// that C11 call_once was not running its initialisers. A better explanation
// turned up before it was ever tested: libEGL.a and libvulkan.a each contain a
// complete copy of Mesa's GLSL compiler, and with libEGL linked first the two
// were being MIXED, so vk_instance_init initialised one static
// glsl_type_cache while glsl_interface_type read another. The Makefile now
// links libvulkan first. See the comment there.
//
// Kept rather than deleted because the reasoning is sound and the code is
// correct; but overriding a call_once that already works is its own risk, and
// changing the link order and the one-time-init primitive in the same build
// would leave neither result interpretable.
// ===========================================================================

#if defined(SHIM_CALL_ONCE) && SHIM_CALL_ONCE

#include <stddef.h>
#include <switch.h>

extern void debugPrintf(const char *fmt, ...);

#ifndef CALL_ONCE_SLOTS
// Mesa uses a modest number of these -- glsl type cache, format tables, driver
// config, debug options. 128 is generous; the overflow path below is correct
// rather than merely safe, so an underestimate degrades instead of breaking.
#define CALL_ONCE_SLOTS 128
#endif

static RMutex g_once_lock;  // RECURSIVE: Mesa nests call_once (an init that calls call_once)
static const void *g_done[CALL_ONCE_SLOTS];
static int g_done_n;
static int g_overflowed;

void call_once(void *flag, void (*func)(void)) {
  if (!func) return;

  rmutexLock(&g_once_lock);

  for (int i = 0; i < g_done_n; i++) {
    if (g_done[i] == flag) {          // already run
      rmutexUnlock(&g_once_lock);
      return;
    }
  }

  if (g_done_n < CALL_ONCE_SLOTS) {
    // Recorded BEFORE the call, not after.
    //
    // Mesa initialisers do call other util_call_once-guarded code, and one
    // that re-entered its own flag would otherwise recurse forever. Marking
    // first makes a re-entrant call a no-op, which matches what a correct
    // call_once does for the thread already inside it.
    g_done[g_done_n++] = flag;
  } else if (!g_overflowed) {
    g_overflowed = 1;
    debugPrintf("[once] more than %d distinct once_flags; the rest will run\n"
                "[once] EVERY time. Raise CALL_ONCE_SLOTS.\n",
                CALL_ONCE_SLOTS);
  }

  // Called with the lock held. That is what serialises concurrent callers,
  // and it is safe against re-entry because the flag is already recorded.
  func();

  rmutexUnlock(&g_once_lock);
}

// Some builds route through the _data variant, which passes a context.
void call_once_data(void *flag, void (*func)(const void *), const void *data) {
  if (!func) return;

  rmutexLock(&g_once_lock);
  for (int i = 0; i < g_done_n; i++) {
    if (g_done[i] == flag) { rmutexUnlock(&g_once_lock); return; }
  }
  if (g_done_n < CALL_ONCE_SLOTS) g_done[g_done_n++] = flag;
  func(data);
  rmutexUnlock(&g_once_lock);
}

void call_once_report(void) {
  debugPrintf("[once] %d one-time initialisers ran%s\n", g_done_n,
              g_overflowed ? " (SLOTS OVERFLOWED)" : "");
}

#else  /* !SHIM_CALL_ONCE */

extern void debugPrintf(const char *fmt, ...);
#include "call_once_shim.h"

void call_once_report(void) {
  debugPrintf("[once] call_once shim not built in (-DSHIM_CALL_ONCE=1 to enable);\n"
              "[once] the platform's own call_once is in use\n");
}

#endif
