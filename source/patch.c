/* patch.c -- game-specific patches for libgodot_android.so (Mega Man X
 * Regenesis, Godot 4.7 arm64-v8a). They are validated against the original
 * instruction bytes before patching (a mismatch is logged and skipped, never
 * applied blind).
 *
 * OFFSETS ARE PER ENGINE BUILD. One binary (md5 5515d98fff588d6c2f8d20e64cd85a3a,
 * 4.7.stable.official) served 1.00.61 through 1.00.92. 1.01.0 ships a different
 * build (md5 904285f049973cf30e1686df25ca9250, 4.7.stable.custom_build, half the
 * size), so both offsets below were re-derived for it. On a new game version, check
 * the md5 first: if it changed, re-derive before shipping -- the words are verified,
 * so a stale offset is skipped rather than miswritten, but then the fix is simply
 * absent (an intro crash, or the GPU freeze described below).
 *
 * How they were re-derived (1.01.0, with the engine's internal symbols stripped):
 * scan .text for the expected instruction word, then confirm the match by the
 * surrounding code and by .eh_frame's FDE bounds, which give each function's exact
 * start and end even without symbols.
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

// The GodotJavaViewWrapper pointer-capture getter:
//   ldp x8, x9, [x0, #0x28]; cmp; ccmp; cset w0; ret
// It reads req=*(self+0x28) and rel=*(self+0x30) and returns (both non-null).
// DisplayServerAndroid calls it on the result of GodotJavaWrapper::get_godot_view()
// WITHOUT a null check (mouse mode / cursor shape setup during setup2), and our view
// is null, so it is a Data Abort at 0x28 -- the first crash seen on hardware, during
// the intro. Replaced with a null-safe version.
//
// 1.00.61-1.00.92 @ 0x122a238 (found from an on-hardware Data Abort there). In that
// binary a second 0xa942a408 site at 0x345b0e0 was ShaderLanguage::ArrayConstructNode
// -- unrelated, must NOT be patched; the word alone does not identify the function.
// 1.01.0 @ 0xa8b370: of the three 0xa942a408 sites (0xa8b370, 0x1ba56b4, 0x1e46728)
// only this one continues cmp/ccmp/cset/ret, and .eh_frame gives it as a whole
// function of exactly five instructions (0xa8b370..0xa8b384), so hooking its first
// word cannot land mid-function.
#define CAN_CAPTURE_POINTER_VADDR 0xa8b370
#define CAN_CAPTURE_POINTER_WORD0 0xa942a408u

static int can_capture_pointer_safe(void *self) {
  if (!self) return 0;
  void *req = *(void **)((char *)self + 0x28);
  void *rel = *(void **)((char *)self + 0x30);
  return (req && rel) ? 1 : 0;
}

// void RendererCanvasCull::canvas_light_set_enabled(RID p_light, bool p_enabled)
// The enabled flag is written inside this function by
//   ldrb w9,[x8]; bfxil w9,w2,#0,#1; strb w9,[x8]   (clight->enabled = p_enabled).
// 1.00.61-1.00.92 @ 0x321f290; 1.01.0 @ 0x1982748. Three sites in 1.01.0 share the
// bfxil word; this is the right one because its ERR_FAIL_NULL path, inside the same
// .eh_frame function (0x1982708..0x19827c0), passes the string
// "canvas_light_set_enabled" as _err_print_error's function-name argument. (The site
// at 0x1983cf0 has the identical shape but names a different function -- the
// occluder version -- so the byte pattern alone is not enough to pick one.)
// Rewriting the bfxil to a bfc clears the enabled bit unconditionally, so any
// canvas light the game enables through this call ends up disabled instead.
// WHY THIS MUST STAY: the game's full-screen DirectionalLight2D (FX/ambient_light,
// present in the levels -- intro_level, fire_level, ...) is enabled when a level
// loads and TDRs the GPU (device lost) on this console -- a hard GPU freeze the
// moment you pick a save slot + difficulty and the first level loads. Forcing it
// off keeps the GPU alive; sprites render fine, just unlit. This was once removed
// as "dead code" (menu point-lights are unaffected and stay visible, so it LOOKED
// inert) and it brought the freeze back -- do not remove it. The word is verified
// before patching, so a stale offset is skipped -- but a skipped patch here means
// the freeze is back, which is why the md5 check at the top matters on every update.
#define CANVAS_LIGHT_ENABLED_VADDR 0x1982748
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
