/* hotfix.c -- self-healing fixes for game data known to be missing/broken in
 * the shipped APK assets. Runs once at boot, before the engine starts.
 *
 * For Mega Man X Regenesis (Godot 4.7) no missing/broken asset is known yet,
 * so apply_asset_hotfixes() is a no-op. The write_if_missing() helper is kept
 * ready: if a first-boot log shows an unguarded FileAccess.open on a file the
 * APK export dropped (the pattern that bit the SMWR credits.txt), embed the
 * file's bytes and restore it here. MIT license; see LICENSE. */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "hotfix.h"
#include "util.h"

// writes `content` (length `len`, excluding the NUL) to <data_root>/assets/<rel_path>,
// but only if that file doesn't already exist. Returns 1 if written, else 0.
__attribute__((unused))
static int write_if_missing(const char *rel_path, const char *content, size_t len) {
  char path[512];
  snprintf(path, sizeof(path), "%s/assets/%s", config.data_root, rel_path);
  struct stat st;
  if (stat(path, &st) == 0) return 0; // already there -- never overwrite
  FILE *f = fopen(path, "wb");
  if (!f) { debugPrintf("!! hotfix: could not create %s\n", path); return 0; }
  size_t written = fwrite(content, 1, len, f);
  fclose(f);
  if (written != len) { debugPrintf("!! hotfix: short write on %s\n", path); return 0; }
  debugPrintf(">> hotfix: wrote missing %s (%zu bytes)\n", rel_path, len);
  return 1;
}

void apply_asset_hotfixes(void) {
  // No known-missing assets for Mega Man X Regenesis yet. Add write_if_missing()
  // calls here if a boot log reveals one.
}
