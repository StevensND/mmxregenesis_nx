/* script_patch.h -- on-device patches to defaults in the game's compiled GDScript.
 * MIT license; see LICENSE. */

#ifndef __SCRIPT_PATCH_H__
#define __SCRIPT_PATCH_H__

// Rewrites the targeted .gdc scripts into <save_root>/_ovr and has godot_shim.c
// serve them. Needs the engine's zstd, so call it after the modules are loaded and
// their init arrays ran, before the game thread starts.
void script_patches_apply(void);

#endif
