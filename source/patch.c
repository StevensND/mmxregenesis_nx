/* patch.c -- game-specific patches for libgodot_android.so (Mega Man X
 * Regenesis, Godot 4.7 arm64-v8a). Offsets were derived from the v1.00.61 APK;
 * the engine binary is byte-identical in 1.00.7, 1.00.8 and 1.00.91 (md5
 * 5515d98fff588d6c2f8d20e64cd85a3a). They are validated against the
 * original instruction bytes before patching (a mismatch is logged and
 * skipped, never applied blind).
 *
 * Retargeted from delson's smwr_nx patch (Godot 4.6.dev4). The can_capture_pointer
 * offset below was re-derived for the 1.00.61 binary from an on-hardware crash
 * (see the per-define note). Offsets shift between game versions, so if a boot log
 * shows a Data Abort inside DisplayServerAndroid at some OTHER godot+0xADDR, that's
 * a further null-view getter whose offset needs re-deriving and adding here.
 *
 * Two kinds of patch are supported (see GamePatch):
 *   - whole-function hook  (repl != NULL): redirect the function to a C replacement.
 *   - single-word rewrite  (repl == NULL): overwrite one instruction word in place.
 * Both verify the original word first and patch by ADDRESS, never by byte pattern.
 * MIT license; see LICENSE. */

#include <stdint.h>
#include <string.h>

#include "patch.h"
#include "so_util.h"
#include "util.h"

// GodotJavaViewWrapper method-id getters, called on the result of
// GodotJavaWrapper::get_godot_view() WITHOUT a null check by
// DisplayServerAndroid (mouse mode / cursor shape setup during setup2).
// If the view is null this is a Data Abort at 0x28 (the first crash seen on
// hardware). Replace with null-safe versions.

// bool GodotJavaViewWrapper::can_capture_pointer() const
// MMX 1.00.61 @ 0x122a238: ldp x8, x9, [x0, #0x28]; cmp; ccmp; cset; ret
// (reads req=*(self+0x28), rel=*(self+0x30); faults on a null self at 0x28).
// Re-derived for 1.00.61 from an on-hardware Data Abort at godot+0x122a238: the
// two 0xa942a408 sites are now 0x122a238 (this -- the real can_capture_pointer,
// called by DisplayServerAndroid with a null GodotJavaViewWrapper on Switch) and
// 0x345b0e0 (ShaderLanguage::ArrayConstructNode -- unrelated, do NOT patch). The
// previous 0x13181e8 offset is stale for this binary and was being skipped
// ("unexpected bytes 540040e1"), leaving the getter unpatched -> intro crash.
#define CAN_CAPTURE_POINTER_VADDR 0x122a238
#define CAN_CAPTURE_POINTER_WORD0 0xa942a408u

static int can_capture_pointer_safe(void *self) {
  if (!self) return 0;
  void *req = *(void **)((char *)self + 0x28);
  void *rel = *(void **)((char *)self + 0x30);
  return (req && rel) ? 1 : 0;
}

// void RendererCanvasCull::canvas_light_set_enabled(RID p_light, bool p_enabled)
// The enabled flag is written inside this function at 0x321f290 by
//   ldrb w9,[x8]; bfxil w9,w2,#0,#1; strb w9,[x8]   (clight->enabled = p_enabled).
// Rewriting the bfxil to a bfc clears the enabled bit unconditionally, so any
// canvas light the game enables through this call ends up disabled instead.
// WHY THIS MUST STAY: the game's full-screen DirectionalLight2D (FX/ambient_light,
// present in the levels -- intro_level, fire_level, ...) is enabled when a level
// loads and TDRs the GPU (device lost) on this console -- a hard GPU freeze the
// moment you pick a save slot + difficulty and the first level loads. Forcing it
// off keeps the GPU alive; sprites render fine, just unlit. This was once removed
// as "dead code" (menu point-lights are unaffected and stay visible, so it LOOKED
// inert) and it brought the freeze back -- do not remove it. Word is verified
// before patching, and the engine binary is the same in every version checked (see top).
#define CANVAS_LIGHT_ENABLED_VADDR 0x321f290
#define CANVAS_LIGHT_ENABLED_ORIG  0x33000049u  /* bfxil w9, w2, #0, #1 */
#define CANVAS_LIGHT_ENABLED_REPL  0x330003e9u  /* bfc   w9, #0, #1      */

// repl_word: replacement instruction word, used only for single-word rewrites
//            (repl == NULL). Ignored for whole-function hooks.
// expect:    original instruction word at vaddr, verified before patching.
// repl:      C replacement for a whole-function hook, or NULL for a word rewrite.
typedef struct { uint32_t repl_word; uint32_t expect; uintptr_t vaddr; void *repl; const char *name; } GamePatch;

void so_patch(so_module *mod) {
  static const GamePatch patches[] = {
    { 0, CAN_CAPTURE_POINTER_WORD0, CAN_CAPTURE_POINTER_VADDR,
      (void *)&can_capture_pointer_safe, "can_capture_pointer" },
    { CANVAS_LIGHT_ENABLED_REPL, CANVAS_LIGHT_ENABLED_ORIG, CANVAS_LIGHT_ENABLED_VADDR,
      NULL, "canvas_light_set_enabled(force off)" },
  };

  for (unsigned i = 0; i < sizeof(patches) / sizeof(*patches); i++) {
    const GamePatch *p = &patches[i];
    // patches are written into the RW backing (load_base) before so_finalize,
    // which flushes the icache when it maps the segment executable.
    uint32_t *insn = (uint32_t *)((uintptr_t)mod->load_base + p->vaddr);
    if (*insn != p->expect) {
      debugPrintf("[patch] %s: unexpected bytes %08x at 0x%lx, skipping\n",
                  p->name, *insn, (unsigned long)p->vaddr);
      continue;
    }
    if (p->repl) {
      hook_arm64((uintptr_t)insn, (uintptr_t)p->repl);
      debugPrintf("[patch] %s hooked at vaddr 0x%lx\n", p->name, (unsigned long)p->vaddr);
    } else {
      *insn = p->repl_word;
      debugPrintf("[patch] %s applied at vaddr 0x%lx (%08x -> %08x)\n",
                  p->name, (unsigned long)p->vaddr, p->expect, p->repl_word);
    }
  }
}
