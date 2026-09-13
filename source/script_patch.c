/* script_patch.c -- on-device patches to the game's compiled GDScript, for
 * defaults that only exist in script code (the touch-control settings).
 *
 * Each target .gdc is read from the user's own assets, decompressed with the
 * engine's zstd (the ZSTD_decompress forwarder in main.c), patched by rewriting
 * literal constants in its token buffer, and written to <save_root>/_ovr as an
 * uncompressed .gdc that godot_shim.c serves instead of the original. Nothing is
 * patched blind: the whole buffer has to parse, and each constant has to hold the
 * expected value with the expected number of uses -- otherwise that script is left
 * alone and the game runs its original code. A game version that changes these
 * scripts may need the tables below re-derived. No game code is shipped here, only
 * the literals to look for.
 *
 * .gdc layout (Godot 4.5+, tokenizer version 101): "GDSC", u32 version, u32
 * decompressed size (0 = stored uncompressed), then the token buffer: u32 counts of
 * identifiers, constants, lines and tokens; identifiers (u32 length + UTF-32 chars);
 * constants (Variant encoding); line and column tables (line count x 8 bytes each);
 * tokens (5 bytes, or 8 when bit 7 of the first byte is set: type in the low 7 bits,
 * identifier/constant index from bit 8, then 4 bytes of line).
 * MIT license; see LICENSE. */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "util.h"
#include "asset_pack.h"
#include "godot_shim.h"
#include "script_patch.h"

// the engine's zstd, forwarded by main.c
size_t ZSTD_decompress(void *dst, size_t dstCap, const void *src, size_t srcSize);

#define GDSC_HEADER_SIZE 12
#define TOKEN_LITERAL 3 // GDScriptTokenizer::Token::LITERAL

// Variant::Type ids of GDScript literals, and the 64-bit payload flag
#define VARIANT_NIL 0
#define VARIANT_BOOL 1
#define VARIANT_INT 2
#define VARIANT_FLOAT 3
#define VARIANT_STRING 4
#define VARIANT_STRING_NAME 21
#define VARIANT_FLAG_64 (1u << 16)

#define MAX_CONST_PATCHES 4

typedef struct {
  const char *text;     // String literal to find, or NULL for a float literal
  const char *new_text; // same-length replacement
  double value;         // float literal to find
  double new_value;
  uint32_t uses;        // LITERAL tokens that must reference it
} ConstPatch;

typedef struct {
  const char *asset;    // path under assets/
  const char *name;     // override file name (the .gdc basename)
  const ConstPatch *patches;
  unsigned count;
} ScriptPatch;

// Touch controls are meant for phones; the Switch has a controller. touch_controls.gd
// asks OS.has_feature('mobile') twice -- `enabled` defaults to it, and the "TC" on/off
// button uses toggle_anchor.visible = is_mobile or enabled -- and this is the Android
// build, so both were true. 'nx_off' is no feature tag: touch controls start OFF and
// the TC button only shows while they are ON. The overlay opacity default (18%) -> 0%.
static const ConstPatch k_touch_controls[] = {
  { "mobile", "nx_off", 0.0, 0.0, 2 },
  { NULL, NULL, 0.1764706, 0.0, 1 },
};

// save_load.gd _load_touch_settings(): overlay_opacity = clamp(saved, 0.1, 1.0), while
// the Touch Controls menu goes down to 0% -- so a saved 0% came back as 10% on the next
// boot. Lower bound 0.1 -> 0.0.
static const ConstPatch k_save_load[] = {
  { NULL, NULL, 0.1, 0.0, 1 },
};

static const ScriptPatch k_scripts[] = {
  { "scripts/touch_controls.gdc", "touch_controls.gdc", k_touch_controls, 2 },
  { "scripts/Functions/save_load.gdc", "save_load.gdc", k_save_load, 1 },
};

static uint32_t read_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Encoded size of one constant, for the Variant types GDScript literals use; 0 for
// anything else or truncated data.
static size_t constant_size(const uint8_t *p, size_t avail) {
  if (avail < 4) return 0;
  const uint32_t header = read_u32(p);
  size_t size;
  switch (header & 0xFF) {
    case VARIANT_NIL: size = 4; break;
    case VARIANT_BOOL: size = 8; break;
    case VARIANT_INT:
    case VARIANT_FLOAT: size = (header & VARIANT_FLAG_64) ? 12 : 8; break;
    case VARIANT_STRING:
    case VARIANT_STRING_NAME:
      if (avail < 8) return 0;
      size = 8 + (((size_t)read_u32(p + 4) + 3) & ~(size_t)3);
      break;
    default: return 0;
  }
  return size <= avail ? size : 0;
}

static int constant_matches(const uint8_t *c, const ConstPatch *patch) {
  const uint32_t header = read_u32(c);
  if (patch->text) {
    const size_t n = strlen(patch->text);
    return header == VARIANT_STRING && read_u32(c + 4) == n && !memcmp(c + 8, patch->text, n);
  }
  double v;
  if (header == (VARIANT_FLOAT | VARIANT_FLAG_64)) {
    memcpy(&v, c + 4, sizeof(v));
  } else if (header == VARIANT_FLOAT) {
    float f;
    memcpy(&f, c + 4, sizeof(f));
    v = f;
  } else {
    return 0;
  }
  return fabs(v - patch->value) < 1e-6;
}

static void constant_rewrite(uint8_t *c, const ConstPatch *patch) {
  if (patch->text) {
    memcpy(c + 8, patch->new_text, strlen(patch->new_text));
  } else if (read_u32(c) & VARIANT_FLAG_64) {
    memcpy(c + 4, &patch->new_value, sizeof(patch->new_value));
  } else {
    const float f = (float)patch->new_value;
    memcpy(c + 4, &f, sizeof(f));
  }
}

// Applies `patches` to a decompressed token buffer in place. Returns 1 only if the
// buffer parsed exactly to its end and every patch found its constant.
static int patch_token_buffer(uint8_t *buf, size_t len, const ConstPatch *patches, unsigned count) {
  if (len < 16 || count > MAX_CONST_PATCHES) return 0;
  const uint32_t identifiers = read_u32(buf), constants = read_u32(buf + 4);
  const uint32_t lines = read_u32(buf + 8), tokens = read_u32(buf + 12);
  size_t p = 16;
  for (uint32_t i = 0; i < identifiers; i++) {
    if (len - p < 4) return 0;
    const size_t chars = read_u32(buf + p);
    if (chars > (len - p - 4) / 4) return 0;
    p += 4 + chars * 4;
  }

  int ok = 0;
  uint32_t target[MAX_CONST_PATCHES];
  size_t *offsets = malloc((constants ? constants : 1) * sizeof(*offsets));
  uint32_t *uses = calloc(constants ? constants : 1, sizeof(*uses));
  if (!offsets || !uses) goto done;
  for (uint32_t i = 0; i < constants; i++) {
    const size_t size = constant_size(buf + p, len - p);
    if (!size) goto done;
    offsets[i] = p;
    p += size;
  }
  if (lines > (len - p) / 16) goto done;
  p += (size_t)lines * 16;
  for (uint32_t i = 0; i < tokens; i++) {
    if (p >= len) goto done;
    const int wide = buf[p] & 0x80;
    const size_t token_len = wide ? 8 : 5;
    if (len - p < token_len) goto done;
    const uint32_t word = wide ? read_u32(buf + p) : buf[p];
    if ((word & 0x7F) == TOKEN_LITERAL) {
      if ((word >> 8) >= constants) goto done;
      uses[word >> 8]++;
    }
    p += token_len;
  }
  if (p != len) goto done;

  for (unsigned k = 0; k < count; k++) {
    unsigned found = 0;
    for (uint32_t i = 0; i < constants; i++) {
      if (constant_matches(buf + offsets[i], &patches[k])) {
        target[k] = i;
        found++;
      }
    }
    if (found != 1) {
      debugPrintf("[script]   constant %u: %u matches, expected 1\n", k, found);
      goto done;
    }
    if (uses[target[k]] != patches[k].uses) {
      debugPrintf("[script]   constant %u: %u uses, expected %u\n", k, uses[target[k]], patches[k].uses);
      goto done;
    }
  }
  for (unsigned k = 0; k < count; k++)
    constant_rewrite(buf + offsets[target[k]], &patches[k]);
  ok = 1;

done:
  free(offsets);
  free(uses);
  return ok;
}

// Returns the patched script as an uncompressed .gdc (caller frees), or NULL.
static uint8_t *patch_script(const uint8_t *file, size_t file_len, const ScriptPatch *script,
                             size_t *out_len) {
  if (file_len < GDSC_HEADER_SIZE || memcmp(file, "GDSC", 4)) return NULL;
  const size_t stored = file_len - GDSC_HEADER_SIZE;
  const size_t raw = read_u32(file + 8);
  const size_t tokens_len = raw ? raw : stored;
  uint8_t *out = malloc(GDSC_HEADER_SIZE + tokens_len);
  if (!out) return NULL;
  uint8_t *tokens = out + GDSC_HEADER_SIZE;
  if (raw) {
    if (ZSTD_decompress(tokens, raw, file + GDSC_HEADER_SIZE, stored) != raw) {
      free(out);
      return NULL;
    }
  } else {
    memcpy(tokens, file + GDSC_HEADER_SIZE, stored);
  }
  if (!patch_token_buffer(tokens, tokens_len, script->patches, script->count)) {
    free(out);
    return NULL;
  }
  memcpy(out, file, 8);  // "GDSC" + tokenizer version
  memset(out + 8, 0, 4); // decompressed size 0: stored uncompressed
  *out_len = GDSC_HEADER_SIZE + tokens_len;
  return out;
}

static uint8_t *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  uint8_t *data = NULL;
  long size = -1;
  if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
  if (size > 0 && fseek(f, 0, SEEK_SET) == 0) {
    data = malloc((size_t)size);
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
      free(data);
      data = NULL;
    }
  }
  fclose(f);
  if (data) *len = (size_t)size;
  return data;
}

void script_patches_apply(void) {
  for (unsigned i = 0; i < sizeof(k_scripts) / sizeof(*k_scripts); i++) {
    const ScriptPatch *script = &k_scripts[i];
    char path[512];
    snprintf(path, sizeof(path), "%s/assets/%s", config.data_root, script->asset);
    size_t file_len = 0, out_len = 0;
    uint8_t *file = read_file(path, &file_len);
    if (!file && asset_pack_active()) {
      void *packed = NULL;
      if (asset_pack_read_all_path(path, &packed, &file_len)) file = packed;
    }
    uint8_t *out = file ? patch_script(file, file_len, script, &out_len) : NULL;
    if (out && script_override_write(script->name, out, out_len))
      debugPrintf("[script] %s patched\n", script->asset);
    else
      debugPrintf("[script] %s left unpatched (%s)\n", script->asset,
                  !file ? "not found" : !out ? "unexpected contents" : "write failed");
    free(out);
    free(file);
  }
}
