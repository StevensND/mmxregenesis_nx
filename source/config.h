/* config.h -- Mega Man X Regenesis (Godot 4.7) Switch wrapper configuration.
 * Retargeted from delson's smwr_nx (Godot 4.6) template. MIT license; see LICENSE. */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// libgodot_android.so is fopen()'d relative to the NRO's directory (the homebrew CWD),
// so keep it a bare filename: place the NRO next to it in /switch/mmxregenesis_nx/.
#define SO_NAME "libgodot_android.so"
#define CXX_SO_NAME "libc++_shared.so"
#define CONFIG_NAME "config.txt"
#define LOG_NAME "mmx_debug.log"

// '/'-absolute paths resolved against the default sdmc device. DATA_ROOT holds
// the app tree the user prepared under /switch/mmxregenesis_nx/
// (libgodot_android.so, libc++_shared.so, assets/). SAVE_ROOT holds saves/config.
// Overridable from config.txt.
#define DEFAULT_DATA_ROOT "/switch/mmxregenesis_nx"
#define DEFAULT_SAVE_ROOT "/switch/mmxregenesis_nx/save"

// absolute so the log lands in the app dir regardless of the launch CWD
#define LOG_PATH DEFAULT_DATA_ROOT "/mmx_debug.log"

// Master debug switch: log file (<data_root>/mmx_debug.log), boot_stats.txt,
// nxlink stdout, and all debugPrintf/[io]/[audio] output. Set to 1 to diagnose
// on hardware; 0 for release (no log/stats files written, and faster: no
// per-line fflush to the SD card).
#define DEBUG_LOG 0
// Per-file-operation logging (open/stat/access/fopen). Very noisy and slow
// (one fflush per line during asset loading); requires DEBUG_LOG too.
#define VERBOSE_IO 0

extern int screen_width;
extern int screen_height;

// locale reported to the engine via GodotIO.getLocale (the game is English)
#define DEVICE_LOCALE "en_US"

typedef struct {
  int screen_width;   // render width  (config.txt); with the GPU/vsync this sets
  int screen_height;  // render height (config.txt); the framerate. Default 1280x720.
  int deadzone;       // analog stick deadzone, percent (0 disables); default 18
  int assetpack;      // 1 (default) = fold the loose assets into an on-device pack
                      // (assets.nxpack/.nxidx, built on first boot) for fast SD I/O.
                      // 0 = always read the loose files.
  int enable_vulkan;  // 1 (default) = use Godot's Vulkan renderer (NVK) when the driver
                      // comes up; 0 = force GLES3. Falls back to GLES3 if the probe fails.
  char rendering_method[32]; // override the Vulkan render method ("mobile" by default).
  char data_root[256];
  char save_root[256];
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
