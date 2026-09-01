/* main.c -- Mega Man X Regenesis v1.00.61 (Godot 4.7, Android) Switch
 * wrapper entry point.
 *
 * Loads the arm64-v8a libc++_shared.so + libgodot_android.so pair, provides a
 * minimal Android-like environment (fake JNI, libc/GLES3/EGL import table),
 * brings up the render context (Vulkan/NVK), and drives the GodotLib lifecycle
 * (initialize/setup/newcontext/resize/step) plus joypad/touch input from the
 * Switch pad and touchscreen.
 *
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "patch.h"
#include "libc_shim.h"
#include "godot_shim.h"
#include "hotfix.h"
#include "asset_pack.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module cxx_mod, game_mod;

// reserve a slice for the .so loader; the rest is the newlib heap where the
// engine's malloc lands. libgodot_android.so's LOAD zone is ~73 MB and
// libc++_shared.so's ~1.3 MB. Requires full-RAM mode (title override /
// forwarder) for the engine heap.
#define SO_HEAP_RESERVE (88 * 1024 * 1024)
#define CXX_SO_SLICE    (2 * 1024 * 1024)

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  size_t so_reserve = SO_HEAP_RESERVE;
  if (so_reserve > size / 2)
    so_reserve = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  size_t fake_heap_size = size - so_reserve;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  if (stat(SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nPlace it next to the NRO.", SO_NAME);
  if (stat(CXX_SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nPlace it next to the NRO.", CXX_SO_NAME);
  char assets[300];
  snprintf(assets, sizeof(assets), "%s/assets/project.binary", config.data_root);
  if (stat(assets, &st) < 0)
    fatal_error("Could not find\nassets/project.binary.\nCopy the APK's assets/ folder next to the NRO.");
}

// Resolve the app's data directory from the launch CWD so the port works from
// any folder under /switch (not just /switch/mmxregenesis_nx). Falls back to the
// compile-time default when the CWD doesn't hold libgodot_android.so.
static void resolve_data_root(void) {
  char cwd[256];
  if (!getcwd(cwd, sizeof(cwd)) || !cwd[0]) return;
  // drop any "device:" prefix ("sdmc:/switch/x" -> "/switch/x")
  char *colon = strchr(cwd, ':');
  char *base = colon ? colon + 1 : cwd;
  if (!base[0]) return;
  size_t l = strlen(base);
  while (l > 1 && base[l - 1] == '/') base[--l] = 0; // strip trailing slashes
  // only adopt it if the game binary is actually there
  char so[300];
  snprintf(so, sizeof(so), "%s/%s", base, SO_NAME);
  struct stat st;
  if (stat(so, &st) != 0) return;
  snprintf(config.data_root, sizeof(config.data_root), "%s", base);
  snprintf(config.save_root, sizeof(config.save_root), "%s/save", base);
}

#include "vulkan_shim.h"
#include "nx_c11.h"
#include "call_once_shim.h"
static int s_use_vulkan = 0;  // renderer decided once in main(); read when building e_setup args

// CPU fault handler: log WHERE a crash lands (PC/LR + faulting regs) so a
// segfault in NVK / the engine is a log line instead of a silent cut. libnx
// routes hardware exceptions here when a handler + stack are provided.
int main(void);  // code anchor for the fault offset below
static void log_code_addr(const char *tag, uint64_t a); // fwd: symbolizes an addr to godot+off
__attribute__((aligned(16))) unsigned char __nx_exception_stack[0x2000];
unsigned long __nx_exception_stack_size = sizeof(__nx_exception_stack);
void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  // Everything the map needs, in ONE line, first -- if only this prints we can
  // still place PC and LR: nro_base = &main - <main's elf addr>, then subtract.
  unsigned long tp = 0;
  __asm__ volatile("mrs %0, s3_3_c13_c0_2" : "=r"(tp));
  debugPrintf("\n### FAULT PC=0x%lx LR=0x%lx &main=0x%lx FAR=0x%lx ESR=0x%x tp=0x%lx\n",
              (unsigned long)ctx->pc.x, (unsigned long)ctx->lr.x,
              (unsigned long)(uintptr_t)&main, (unsigned long)ctx->far.x,
              (unsigned)ctx->esr, tp);
  debugPrintf("### desc=0x%x X0=0x%lx X1=0x%lx X19=0x%lx SP=0x%lx\n",
              (unsigned)ctx->error_desc, (unsigned long)ctx->cpu_gprs[0].x,
              (unsigned long)ctx->cpu_gprs[1].x, (unsigned long)ctx->cpu_gprs[19].x,
              (unsigned long)ctx->sp.x);
  // Place PC/LR inside libgodot/libc++/wrapper so the crash can be disassembled.
  log_code_addr("PC", (uint64_t)ctx->pc.x);
  log_code_addr("LR", (uint64_t)ctx->lr.x);
}

static void set_screen_size(int w, int h) {
  if (w > 0 && h > 0 && w <= 1920 && h <= 1080) {
    // Explicit override from config.txt (screen_width/screen_height).
    screen_width = w; screen_height = h;
  } else {
    // 720p in both handheld and docked; the console upscales to the TV for free.
    // The framerate here is bounded by the GPU clock, not this resolution: the
    // FastLoad boost had been pinning the GPU at 76.8 MHz during gameplay (a death
    // spiral -- see the boost logic in the step loop). With the GPU back at its
    // stock 384/768 MHz, this renders 2D at 720p comfortably at 60 fps. To trade
    // sharpness for headroom on the heaviest scenes, set a smaller size in
    // config.txt (e.g. 960x540 or 640x360).
    screen_width = 1280; screen_height = 720;
  }
}

// ---------------------------------------------------------------------------
// EGL / GLES3 context (mesa). Godot's android GL path expects an external
// context that is current on the thread that calls step(), so the wrapper
// owns it, exactly like the Java GLSurfaceView does on Android.
// ---------------------------------------------------------------------------

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLSurface s_surf = EGL_NO_SURFACE;
static EGLContext s_ctx = EGL_NO_CONTEXT;

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif

static int egl_setup(void) {
  s_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (s_dpy == EGL_NO_DISPLAY) return -1;
  if (eglInitialize(s_dpy, NULL, NULL) == EGL_FALSE) return -2;
  if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) return -3;

  const EGLint cfg_attr[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE
  };
  EGLConfig cfg;
  EGLint num = 0;
  if (eglChooseConfig(s_dpy, cfg_attr, &cfg, 1, &num) == EGL_FALSE || num < 1)
    return -4;

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  s_surf = eglCreateWindowSurface(s_dpy, cfg, (EGLNativeWindowType)win, NULL);
  if (s_surf == EGL_NO_SURFACE) return -5;

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
  s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (s_ctx == EGL_NO_CONTEXT) return -6;
  return 0;
}

// ---------------------------------------------------------------------------
// GodotLib native entry points (platform/android/java_godot_lib_jni.h)
// ---------------------------------------------------------------------------

typedef uint8_t jboolean;

static int      (*e_JNI_OnLoad)(void *vm, void *reserved);
// Godot >= 4.5: initialize(godot, asset_mgr, io, net, dirh, fileh, expansion)
static jboolean (*e_initialize)(void *env, void *cls, void *godot, void *asset_mgr,
                                void *io, void *net_utils, void *dir_handler,
                                void *file_handler, jboolean use_apk_expansion);
// Godot <= 4.4: same but with the Activity as the first object argument
static jboolean (*e_initialize44)(void *env, void *cls, void *activity, void *godot,
                                  void *asset_mgr, void *io, void *net_utils,
                                  void *dir_handler, void *file_handler,
                                  jboolean use_apk_expansion);
static int s_glue_44 = 0; // JNI glue generation of the loaded libgodot
static void     (*e_ondestroy)(void *env, void *cls);
static jboolean (*e_setup)(void *env, void *cls, void *cmdline_array, void *tts);
static void     (*e_resize)(void *env, void *cls, void *surface, int w, int h);
static void     (*e_newcontext)(void *env, void *cls, void *surface);
static jboolean (*e_step)(void *env, void *cls);
static void     (*e_key)(void *env, void *cls, int keycode, int unicode, int label, jboolean pressed, jboolean echo);
static void     (*e_joybutton)(void *env, void *cls, int device, int button, jboolean pressed);
static void     (*e_joyaxis)(void *env, void *cls, int device, int axis, float value);
static void     (*e_joyhat)(void *env, void *cls, int device, int hat_x, int hat_y);
static void     (*e_joyconnectionchanged)(void *env, void *cls, int device, jboolean connected, void *name);
static void     (*e_dispatchTouchEvent)(void *env, void *cls, int ev, int pointer, int count, void *positions, jboolean double_tap);
static void     (*e_focusin)(void *env, void *cls);
static void     (*e_focusout)(void *env, void *cls);
static void     (*e_onRendererResumed)(void *env, void *cls);
static void     (*e_onRendererPaused)(void *env, void *cls);
// libgodot exports zstd; we resolve it in resolve_entry_points() (BEFORE
// so_free_temp frees the .so symbol table) and forward the GL/Mesa shader-cache's
// ZSTD_* link imports to it (see the forwarders below). Non-static so the
// forwarders keep external linkage.
size_t          (*e_zstd_decompress)(void *dst, size_t dstCap, const void *src, size_t srcSize) = NULL;
size_t          (*e_zstd_compress)(void *dst, size_t dstCap, const void *src, size_t srcSize, int level) = NULL;
size_t          (*e_zstd_compressbound)(size_t srcSize) = NULL;
unsigned        (*e_zstd_iserror)(size_t code) = NULL;

// The GL libs pull ZSTD_* in at link time (shader-cache compression), but the
// toolchain ships no zstd for this target. The game .so exports zstd and we
// resolve it just above (e_zstd_*, ready before the GL driver comes up), so
// satisfy the link by forwarding these four to it. Null-guarded: if the game
// ever lacks them the cache fails soft instead of crashing.
size_t ZSTD_decompress(void *dst, size_t dstCap, const void *src, size_t srcSize) {
  return e_zstd_decompress ? e_zstd_decompress(dst, dstCap, src, srcSize) : (size_t)-1;
}
size_t ZSTD_compress(void *dst, size_t dstCap, const void *src, size_t srcSize, int level) {
  return e_zstd_compress ? e_zstd_compress(dst, dstCap, src, srcSize, level) : (size_t)-1;
}
size_t ZSTD_compressBound(size_t srcSize) {
  return e_zstd_compressbound ? e_zstd_compressbound(srcSize) : srcSize + (srcSize >> 8) + 512;
}
unsigned ZSTD_isError(size_t code) {
  return e_zstd_iserror ? e_zstd_iserror(code) : 1u;
}

// Perf diagnosis counters (defined in godot_shim.c).
extern int g_asset_open_count, g_asset_pack_open_count;

#define G "Java_org_godotengine_godot_GodotLib_"

// Progress callback used by asset_pack.c during the one-time pack build.
void startup_status_update(const char *message) {
  debugPrintf("[pack] %s\n", message ? message : "");
}

static void resolve_entry_points(void) {
  e_JNI_OnLoad           = (void *)so_try_find_addr_rx(&game_mod, "JNI_OnLoad");
  e_initialize           = (void *)so_find_addr_rx(&game_mod, G "initialize");
  e_initialize44         = (void *)e_initialize;
  // initialize() gained/lost the Activity argument across engine versions;
  // hardwareKeyboardConnected only exists on the new-signature builds (4.5+),
  // so use it to pick the calling convention.
  s_glue_44 = so_try_find_addr_rx(&game_mod, G "hardwareKeyboardConnected") == 0;
  debugPrintf("== godot JNI glue: %s-style initialize ==\n", s_glue_44 ? "4.4" : "4.6");
  e_ondestroy            = (void *)so_try_find_addr_rx(&game_mod, G "ondestroy");
  e_setup                = (void *)so_find_addr_rx(&game_mod, G "setup");
  e_resize               = (void *)so_find_addr_rx(&game_mod, G "resize");
  e_newcontext           = (void *)so_find_addr_rx(&game_mod, G "newcontext");
  e_step                 = (void *)so_find_addr_rx(&game_mod, G "step");
  e_key                  = (void *)so_try_find_addr_rx(&game_mod, G "key");
  e_joybutton            = (void *)so_try_find_addr_rx(&game_mod, G "joybutton");
  e_zstd_decompress      = (void *)so_try_find_addr_rx(&game_mod, "ZSTD_decompress"); // for the GL/Mesa shader-cache ZSTD_* imports; symtab is freed later by so_free_temp
  e_zstd_compress        = (void *)so_try_find_addr_rx(&game_mod, "ZSTD_compress");
  e_zstd_compressbound   = (void *)so_try_find_addr_rx(&game_mod, "ZSTD_compressBound");
  e_zstd_iserror         = (void *)so_try_find_addr_rx(&game_mod, "ZSTD_isError");
  e_joyaxis              = (void *)so_try_find_addr_rx(&game_mod, G "joyaxis");
  e_joyhat               = (void *)so_try_find_addr_rx(&game_mod, G "joyhat");
  e_joyconnectionchanged = (void *)so_try_find_addr_rx(&game_mod, G "joyconnectionchanged");
  e_dispatchTouchEvent   = (void *)so_try_find_addr_rx(&game_mod, G "dispatchTouchEvent");
  e_focusin              = (void *)so_try_find_addr_rx(&game_mod, G "focusin");
  e_focusout             = (void *)so_try_find_addr_rx(&game_mod, G "focusout");
  e_onRendererResumed    = (void *)so_try_find_addr_rx(&game_mod, G "onRendererResumed");
  e_onRendererPaused     = (void *)so_try_find_addr_rx(&game_mod, G "onRendererPaused");
  (void)e_key; (void)e_joyhat;
}

// ---------------------------------------------------------------------------
// input: Switch pad + touchscreen -> Godot JoyButton/JoyAxis/touch events.
// The android Java layer translates keycodes into Godot's own enums before
// crossing into native code, so we emit Godot indices directly.
// ---------------------------------------------------------------------------

#define GD_JOY_A 0
#define GD_JOY_B 1
#define GD_JOY_X 2
#define GD_JOY_Y 3
#define GD_JOY_BACK 4
#define GD_JOY_START 6
#define GD_JOY_LSTICK 7
#define GD_JOY_RSTICK 8
#define GD_JOY_L1 9
#define GD_JOY_R1 10
#define GD_JOY_DPAD_UP 11
#define GD_JOY_DPAD_DOWN 12
#define GD_JOY_DPAD_LEFT 13
#define GD_JOY_DPAD_RIGHT 14

#define GD_AXIS_LX 0
#define GD_AXIS_LY 1
#define GD_AXIS_RX 2
#define GD_AXIS_RY 3
#define GD_AXIS_LT 4
#define GD_AXIS_RT 5

static PadState pad;

// label mapping (Switch A -> Godot A, ...): positional Godot JoyButton indices,
// which is what the Android Java layer feeds native. MMX Regenesis' InputMap
// (project.binary) binds its actions (a/b/x/y, l1/r1/r3, start, menu_*) to these
// same indices, so menus and gameplay work without per-game remapping here.
// Nintendo's physical A/B and X/Y are swapped vs the SDL positions; this table
// maps by physical Switch button to the Godot index at that position.
static struct { u64 sw; int btn; } s_btnmap[] = {
  { HidNpadButton_A,      GD_JOY_A },        // east
  { HidNpadButton_B,      GD_JOY_B },        // south
  { HidNpadButton_X,      GD_JOY_X },        // north
  { HidNpadButton_Y,      GD_JOY_Y },        // west
  { HidNpadButton_L,      GD_JOY_L1 },
  { HidNpadButton_R,      GD_JOY_R1 },
  { HidNpadButton_StickL, GD_JOY_LSTICK },
  { HidNpadButton_StickR, GD_JOY_RSTICK },
  { HidNpadButton_Plus,   GD_JOY_START },
  { HidNpadButton_Minus,  GD_JOY_BACK },
  { HidNpadButton_Up,     GD_JOY_DPAD_UP },
  { HidNpadButton_Down,   GD_JOY_DPAD_DOWN },
  { HidNpadButton_Left,   GD_JOY_DPAD_LEFT },
  { HidNpadButton_Right,  GD_JOY_DPAD_RIGHT },
};

static u64 s_prev_buttons = 0;
static int s_touching = 0;
static float s_prev_axis[6] = { 99, 99, 99, 99, 99, 99 }; // force initial send

// Input is polled on the game thread right before e_step(). When e_step stalls
// for seconds (shader compile / scene load) the poll doesn't run, so a button
// that is pressed AND released during the stall leaves no edge at the next poll
// and is lost -- which is why "press A to continue" screens hang until a restart.
// A tiny dedicated thread keeps sampling the pad at ~160 Hz during those stalls
// and latches any press into s_pending_down; poll_input() then replays it as a
// clean tap so the game's event-based _input still receives it.
static u64        s_pending_down = 0;  // presses caught by the capture thread (atomic)
static u64        s_synth_held = 0;    // synthetic presses to release on the next poll
static PadState   s_ipad;
static Thread     s_ithread;
static volatile int s_ithread_run = 0;

static void input_capture_thread(void *arg) {
  (void)arg;
  while (s_ithread_run) {
    padUpdate(&s_ipad);
    const u64 down = padGetButtonsDown(&s_ipad);
    if (down) __atomic_or_fetch(&s_pending_down, down, __ATOMIC_RELAXED);
    svcSleepThread(6 * 1000000LL); // ~6 ms
  }
}

// Radial-ish deadzone: the Switch sticks never rest at exactly 0, so without
// this they emit a joyaxis event every frame. That made the game's _input
// handler (scripts/input.gd) fire continuously -- and on the input-config
// screen it throws a GDScript error every call, which with logging on floods
// the SD card and tanks the framerate. Below the deadzone we report a hard 0
// (send_axis de-dups, so it's sent once); above it we rescale so control still
// starts smoothly from the edge of the deadzone.
// Radial-ish deadzone: the Switch sticks never rest at exactly 0, so without
// this they emit a joyaxis event every frame. That made the game's _input
// handler (scripts/input.gd) fire continuously -- flooding the log and hurting
// perf. Below the deadzone we report a hard 0 (send_axis de-dups, so it's sent
// once); above it we rescale so control still starts smoothly from the edge.
// Tunable from config.txt ("deadzone 0"..."deadzone 40", percent; 0 disables).
static float stick_norm(s32 v) {
  float dz = config.deadzone / 100.0f;
  if (dz < 0.0f) dz = 0.0f;
  if (dz > 0.9f) dz = 0.9f;
  float f = v / 32767.0f;
  if (f > 1.0f) f = 1.0f;
  if (f < -1.0f) f = -1.0f;
  if (dz <= 0.0f) return f;
  float a = (f < 0.0f) ? -f : f;
  if (a < dz) return 0.0f;
  float s = (a - dz) / (1.0f - dz);
  return (f < 0.0f) ? -s : s;
}

static void send_axis(void *cls, int axis, float v) {
  if (v == s_prev_axis[axis]) return;
  s_prev_axis[axis] = v;
  if (e_joyaxis) e_joyaxis(fake_env, cls, 0, axis, v);
}

static void poll_input(void) {
  void *cls = jni_activity_class();
  padUpdate(&pad);
  const u64 cur = padGetButtons(&pad);
  const unsigned NB = sizeof(s_btnmap) / sizeof(*s_btnmap);

  if (e_joybutton) {
    // Release any synthetic press from the previous poll FIRST, so a real press
    // this frame can't collide with a lingering synthetic hold.
    if (s_synth_held) {
      for (unsigned i = 0; i < NB; i++)
        if (s_synth_held & s_btnmap[i].sw)
          e_joybutton(fake_env, cls, 0, s_btnmap[i].btn, 0);
      s_synth_held = 0;
    }
    // Presses the capture thread saw while this poll was blocked inside e_step.
    const u64 pend = __atomic_exchange_n(&s_pending_down, 0, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < NB; i++) {
      const u64 m = s_btnmap[i].sw;
      if ((cur & m) && !(s_prev_buttons & m))      e_joybutton(fake_env, cls, 0, s_btnmap[i].btn, 1);
      else if (!(cur & m) && (s_prev_buttons & m)) e_joybutton(fake_env, cls, 0, s_btnmap[i].btn, 0);
      else if ((pend & m) && !(cur & m)) { // tapped during a stall, already released
        e_joybutton(fake_env, cls, 0, s_btnmap[i].btn, 1); // press now
        s_synth_held |= m;                                 // release next poll (never coalesced)
      }
    }
  }

  // sticks: godot's android convention is Y-down-positive
  HidAnalogStickState l = padGetStickPos(&pad, 0);
  HidAnalogStickState r = padGetStickPos(&pad, 1);
  send_axis(cls, GD_AXIS_LX, stick_norm(l.x));
  send_axis(cls, GD_AXIS_LY, -stick_norm(l.y));
  send_axis(cls, GD_AXIS_RX, stick_norm(r.x));
  send_axis(cls, GD_AXIS_RY, -stick_norm(r.y));
  // ZL/ZR as digital triggers
  send_axis(cls, GD_AXIS_LT, (cur & HidNpadButton_ZL) ? 1.0f : 0.0f);
  send_axis(cls, GD_AXIS_RT, (cur & HidNpadButton_ZR) ? 1.0f : 0.0f);

  s_prev_buttons = cur;

  // single-finger touch, scaled from the 1280x720 panel to the surface size
  if (e_dispatchTouchEvent) {
    HidTouchScreenState ts = {0};
    const int have = hidGetTouchScreenStates(&ts, 1) && ts.count > 0;
    if (have || s_touching) {
      float x = have ? (float)ts.touches[0].x * screen_width  / 1280.0f : 0.0f;
      float y = have ? (float)ts.touches[0].y * screen_height / 720.0f : 0.0f;
      float pos[3] = { 0.0f, x, y };
      void *arr = jni_new_float_array(3, pos);
      if (have && !s_touching)      { e_dispatchTouchEvent(fake_env, cls, 0 /*DOWN*/, 0, 1, arr, 0); s_touching = 1; }
      else if (have)                { e_dispatchTouchEvent(fake_env, cls, 2 /*MOVE*/, 0, 1, arr, 0); }
      else                          { e_dispatchTouchEvent(fake_env, cls, 1 /*UP*/,   0, 1, arr, 0); s_touching = 0; }
      jni_release_local(arr);
    }
  }
}

// ---------------------------------------------------------------------------
// game thread (owns the EGL context and the whole GodotLib lifecycle)
// ---------------------------------------------------------------------------

static Thread s_game_thread;
static volatile int s_game_running = 1;
static volatile int s_focused = 1;
static volatile int s_frames_done = 0; // step() iterations completed (watchdog)

// ---------------------------------------------------------------------------
// debug telemetry (DEBUG_LOG only): boot phase timings + gameplay stalls,
// written to <data_root>/boot_stats.txt when DEBUG_LOG is on (see stats_open)
// ---------------------------------------------------------------------------

static u64 s_t_boot;
static FILE *s_stats;

static void stats_open(void) {
#if DEBUG_LOG
  char p[300];
  snprintf(p, sizeof(p), "%s/boot_stats.txt", config.data_root);
  s_stats = fopen(p, "w");
  s_t_boot = armGetSystemTick();
  if (s_stats) {
    fprintf(s_stats, "build " __DATE__ " " __TIME__ "\n");
    fflush(s_stats);
  }
#endif
}

static void stats_mark(const char *what) {
  if (!s_stats) return;
  const u64 ms = armTicksToNs(armGetSystemTick() - s_t_boot) / 1000000ull;
  fprintf(s_stats, "%7llu ms  %s\n", (unsigned long long)ms, what);
  fflush(s_stats);
}

static void game_thread_fn(void *arg) {
  (void)arg;
  tls_setup_guard(); // bionic stack canary from tpidr_el0+0x28

  eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx);
  eglSwapInterval(s_dpy, 1); // EGL path only; the game renders through Vulkan,
                             // which presents FIFO (the only mode NVK exposes on
                             // Switch), so the framerate is bounded by the render
                             // resolution, not by any vsync toggle.

  void *cls = jni_activity_class();

  if (e_JNI_OnLoad) {
    debugPrintf(">> JNI_OnLoad...\n");
    e_JNI_OnLoad(fake_vm, NULL);
    debugPrintf(">> JNI_OnLoad ok\n");
  }

  debugPrintf(">> GodotLib.initialize...\n");
  jboolean ok;
  if (s_glue_44)
    ok = e_initialize44(fake_env, cls, jni_activity_object(),
                        jni_godot_object(), jni_assetmgr_object(),
                        jni_godot_io_object(), jni_netutils_object(),
                        jni_dirhandler_object(), jni_filehandler_object(),
                        0 /* use_apk_expansion */);
  else
    ok = e_initialize(fake_env, cls,
                      jni_godot_object(), jni_assetmgr_object(),
                      jni_godot_io_object(), jni_netutils_object(),
                      jni_dirhandler_object(), jni_filehandler_object(),
                      0 /* use_apk_expansion */);
  debugPrintf(">> GodotLib.initialize -> %d\n", (int)ok);
  if (!ok) fatal_error("GodotLib.initialize failed.");
  stats_mark("GodotLib.initialize");

  // force the GL compatibility renderer; redundant with the project settings
  // but immune to project.binary quirks. When <data_root>/game.pck exists
  // (tools/make_pck.py), mount it as the main pack: offset reads from one
  // file instead of per-file SD path walks (much faster boot/level loads).
  static char pck_path[300];
  snprintf(pck_path, sizeof(pck_path), "%s/game.pck", config.data_root);
  struct stat pck_st;
  const int have_pck = (stat(pck_path, &pck_st) == 0);
  debugPrintf(">> main pack: %s (%s)\n", pck_path, have_pck ? "found" : "absent, using assets/");
  stats_mark(have_pck ? "main pack: FOUND (game.pck)" : "main pack: ABSENT (assets/ dir)");

  const char *args[12];
  int nargs = 0;
  if (s_use_vulkan) {
    // "mobile" (SceneForwardMobile), NOT forward_plus: forward_plus compiles the
    // whole desktop 3D pipeline up front (dozens of shader sets a 2D game never
    // uses) -- minutes of compile = black screen. mobile drops all of it and still
    // gets Godot's Vulkan RD pipeline cache. Override via config.txt if needed.
    args[nargs++] = "--rendering-method";
    args[nargs++] = config.rendering_method[0] ? config.rendering_method : "mobile";
    args[nargs++] = "--rendering-driver";
    args[nargs++] = "vulkan";
    // NOTE: no --render-thread pin. We used to force "safe" (render on the main
    // thread) because the engine's separate render thread had no bionic stack
    // guard and faulted. Now EVERY thread gets the guard (pthread trampoline +
    // --wrap=pthread_create for Mesa), so multi-threaded rendering is safe again
    // -- and necessary: with rendering pinned to the main thread, a resource load
    // that needs GPU work (shader compile / texture upload) deadlocks, because
    // the main thread is blocked waiting on that very load.
  } else {
    args[nargs++] = "--rendering-method";
    args[nargs++] = "gl_compatibility";
    args[nargs++] = "--rendering-driver";
    args[nargs++] = "opengl3";
  }
  if (have_pck) {
    args[nargs++] = "--main-pack";
    args[nargs++] = pck_path;
  }
  // NOTE: passing --verbose makes Godot install a *synchronous* GL debug
  // callback. On MMX that fires 54+ times/frame (the text_style.gdshader
  // sampler binding the old nouveau driver rejects), which alone tanks the
  // framerate and floods the SD log. Keep it OFF by default even when
  // DEBUG_LOG is on -- our own [wrapper]/[patch]/[audio] logs and the crash
  // handler still work; we just don't ask the engine for its verbose+GL-debug
  // firehose. Flip GODOT_VERBOSE to 1 only to debug engine-side load issues.
#ifndef GODOT_VERBOSE
#define GODOT_VERBOSE 0
#endif
#if GODOT_VERBOSE
  args[nargs++] = "--verbose";
#endif
  // Disable Android frame pacing (Swappy). Godot's Android Vulkan backend links
  // Google's Swappy frame-pacing library, which spawns ChoreographerFilter threads
  // that wait on an Android Choreographer callback. There is no Choreographer on
  // Switch, so they never wake -- and when a present hiccup (e.g. VkResult -3)
  // makes Godot recreate the swapchain, Swappy's destructor JOINS those threads
  // forever (game thread stuck in pthread_join <- ~ChoreographerFilter <- swapchain
  // teardown -> whole engine hangs, black screen). override.cfg turns it off. res://
  // is the assets dir (read via AAsset with a loose-file fallback), so the file goes
  // there; a copy at data_root covers the game.pck path too.
  if (s_use_vulkan) {
    const char *ovr_body =
      "; Written by the wrapper: this console has no Android Choreographer, so\n"
      "; Godot's Swappy frame pacing would deadlock on swapchain recreation.\n"
      "; vsync_mode=3 (mailbox): with the GPU at full clock, most scenes finish in\n"
      "; ~18 ms -- just over the 16.7 ms vblank -- and plain FIFO would hard-lock\n"
      "; them to 30 fps with the GPU sitting ~50% idle. Mailbox lets the native\n"
      "; ~40-55 fps through with no tearing. (If NVK ignores it and falls back to\n"
      "; FIFO, the only other lever is a smaller render size in config.txt.)\n"
      "; Delete this file to restore Godot's default.\n"
      "\n[display]\n\n"
      "window/frame_pacing/android/enable_frame_pacing=false\n"
      "window/vsync/vsync_mode=3\n";
    char ovrp[400];
    snprintf(ovrp, sizeof(ovrp), "%s/assets/override.cfg", config.data_root);
    FILE *of = fopen(ovrp, "w");
    if (of) { fputs(ovr_body, of); fclose(of);
              debugPrintf(">> wrote %s (Android frame pacing disabled)\n", ovrp); }
    else debugPrintf(">> COULD NOT write %s -- Swappy may deadlock on swapchain recreate\n", ovrp);
    snprintf(ovrp, sizeof(ovrp), "%s/override.cfg", config.data_root);
    of = fopen(ovrp, "w");
    if (of) { fputs(ovr_body, of); fclose(of); }
  }

  void *cmdline = jni_new_string_array(nargs, args);

  debugPrintf(">> GodotLib.setup...\n");
  ok = e_setup(fake_env, cls, cmdline, jni_tts_object());
  debugPrintf(">> GodotLib.setup -> %d\n", (int)ok);
  if (!ok) fatal_error("GodotLib.setup (Main::setup) failed.\nCheck %s.", LOG_NAME);
  stats_mark("GodotLib.setup (project+drivers)");

  debugPrintf(">> newcontext/resize (%dx%d)...\n", screen_width, screen_height);
  e_newcontext(fake_env, cls, jni_surface_object());
  e_resize(fake_env, cls, NULL, screen_width, screen_height);

  debugPrintf(">> entering step loop\n");
  int frames = 0;
  int paused = 0;
  int announced_pad = 0;
  // adaptive CPU boost: shader compilation and level loads are CPU-bound
  // stalls on mesa/nouveau. Any slow step re-arms the boost; it drops only
  // after ~10 s of smooth frames. Boot naturally keeps it armed throughout.
  int boosted = 1; // main() starts boosted for load
  int calm_frames = 0;
  int last_opens = 0; // asset-open count last frame, to detect genuine load stalls
  // Steady-state framerate window: every FPS_WIN gameplay frames, summarize the
  // real frametime (avg/worst) and how many missed 60/30 fps. Frames over 100 ms
  // are the big one-off load stalls (logged separately above) and are excluded so
  // this reflects actual in-game smoothness, not boot/scene loads.
  #define FPS_WIN 120
  u64 fps_sum_ns = 0, fps_work_ns = 0, fps_worst_ns = 0;
  int fps_n = 0, fps_miss60 = 0, fps_miss30 = 0;

  while (s_game_running && !jni_quit_requested) {
    if (!s_focused) {
      if (!paused) {
        if (e_focusout) e_focusout(fake_env, cls);
        if (e_onRendererPaused) e_onRendererPaused(fake_env, cls);
        paused = 1;
      }
      svcSleepThread(16 * 1000 * 1000);
      continue;
    }
    if (paused) {
      if (e_onRendererResumed) e_onRendererResumed(fake_env, cls);
      if (e_focusin) e_focusin(fake_env, cls);
      paused = 0;
    }

    if (announced_pad) poll_input();

    if (frames < 8) debugPrintf(">> step %d begin\n", frames + 1);
    const u64 t0 = armGetSystemTick();
    e_step(fake_env, cls);
    if (frames < 8) debugPrintf(">> step %d done\n", frames + 1);
    const u64 t_work = armGetSystemTick(); // after game logic + GPU command submit
    eglSwapBuffers(s_dpy, s_surf);
    const u64 t_end = armGetSystemTick();  // after present (GPU wait + vblank)
    const u64 work_ns = armTicksToNs(t_work - t0);
    const u64 step_ns = armTicksToNs(t_end - t0);
    const u64 step_ms = step_ns / 1000000ull;

    // Only FastLoad on a genuine big load stall (the boot into a scene, or a
    // level transition): a frame that is very slow AND opens a large batch of
    // assets. A GPU-bound gameplay frame is slow too but opens few/no assets, and
    // FastLoad would drop its GPU to 76.8 MHz -- which on hardware showed the GPU
    // flickering 307<->76.8 mid-play and stuttering. Requiring both a >100 ms
    // frame and >100 new asset opens keeps gameplay on the full GPU clock.
    const int opens_delta = g_asset_open_count - last_opens;
    last_opens = g_asset_open_count;
    if (step_ms > 100 && opens_delta > 100) { // genuine big load stall: burst CPU
      calm_frames = 0;
      if (!boosted) { cpu_boost(1); boosted = 1; }
    } else if (boosted && ++calm_frames > 15) {
      // ~15 frames past the last big load -> back to stock so the GPU runs at its
      // full 307/384/768 MHz for rendering instead of FastLoad's 76.8 MHz.
      cpu_boost(0);
      boosted = 0;
    }

    if (step_ms > 50) { // log notable slow frames (load or GPU-bound), capped
      if (step_ms > 100 && announced_pad && s_stats && frames < 100000) {
        fprintf(s_stats, "stall %4llu ms  frame %d\n", (unsigned long long)step_ms, frames);
        fflush(s_stats); // we already dropped frames; one tiny write is noise
      }
      // Record notable stalls in the debug log (capped) with the running asset-
      // open counts, to see where load time goes and whether resources are being
      // re-opened (opens climbing during a stall = re-loading, not I/O volume).
      static int stall_logs = 0;
      if (step_ms > 80 && stall_logs < 80) {
        debugPrintf("[perf] stall %llu ms @frame %d (asset opens=%d, from pack=%d)\n",
                    (unsigned long long)step_ms, frames, g_asset_open_count, g_asset_pack_open_count);
        stall_logs++;
      }
    }


    frames++;
    s_frames_done = frames;
    if ((frames % 600) == 0)
      debugPrintf("[perf] frame %d: asset opens=%d (from pack=%d)\n",
                  frames, g_asset_open_count, g_asset_pack_open_count);

    // Steady-state framerate: only count real gameplay frames (<=100 ms); the big
    // load stalls are excluded so this shows in-game smoothness, not scene loads.
    if (step_ms <= 100) {
      fps_sum_ns += step_ns;
      fps_work_ns += work_ns;
      if (step_ns > fps_worst_ns) fps_worst_ns = step_ns;
      if (step_ns > 16666667ull) fps_miss60++; // slower than 1/60 s
      if (step_ns > 33333333ull) fps_miss30++; // slower than 1/30 s
      if (++fps_n >= FPS_WIN) {
        const u64 avg_ns = fps_sum_ns / (u64)fps_n;
        const u64 avg_work = fps_work_ns / (u64)fps_n;
        const u64 avg_swap = avg_ns > avg_work ? avg_ns - avg_work : 0;
        debugPrintf("[fps] %d frames: avg %llu.%02llu ms (%llu fps) = work %llu.%02llu + "
                    "swap %llu.%02llu, worst %llu ms, missed60 %d, missed30 %d\n",
                    fps_n,
                    (unsigned long long)(avg_ns / 1000000ull),
                    (unsigned long long)((avg_ns / 10000ull) % 100ull),
                    (unsigned long long)(avg_ns ? 1000000000ull / avg_ns : 0),
                    (unsigned long long)(avg_work / 1000000ull),
                    (unsigned long long)((avg_work / 10000ull) % 100ull),
                    (unsigned long long)(avg_swap / 1000000ull),
                    (unsigned long long)((avg_swap / 10000ull) % 100ull),
                    (unsigned long long)(fps_worst_ns / 1000000ull),
                    fps_miss60, fps_miss30);
        fps_sum_ns = 0; fps_work_ns = 0; fps_worst_ns = 0; fps_n = 0; fps_miss60 = 0; fps_miss30 = 0;
      }
    }
    if (frames == 1) stats_mark("step 1 (engine servers up)");
    if (frames == 4) stats_mark("step 4 (game scene running)");
    if (!announced_pad && frames >= 4) {
      // engine servers are up after the first steps; announce the pad once
      if (e_joyconnectionchanged) {
        void *name = jni_new_string("Nintendo Switch Controller");
        e_joyconnectionchanged(fake_env, cls, 0, 1, name);
        jni_release_local(name);
      }
      announced_pad = 1;
      debugPrintf(">> pad announced after %d frames\n", frames);
    }
  }

  debugPrintf(">> leaving step loop (running=%d quit=%d)\n", s_game_running, jni_quit_requested);
  if (e_ondestroy) e_ondestroy(fake_env, cls);
  s_game_running = 0;
}

// ---------------------------------------------------------------------------
// hang watchdog: when the game thread stops completing steps, pause it and
// dump PC/LR plus an FP-chain backtrace so the stall site lands in the log.
// Offsets are printed relative to both loaded modules and the wrapper.
// ---------------------------------------------------------------------------

int main(void); // forward-declared: the watchdog uses it as a code anchor

static void log_code_addr(const char *tag, uint64_t a) {
  // anchor the wrapper's code region on a known function (module base symbols
  // resolve to 0 under hbl); offsets are then relative to main()
  const uint64_t wrap_base = ((uint64_t)&main) & ~0xFFFFFull;
  const uint64_t gd_base = (uint64_t)game_mod.load_virtbase;
  const uint64_t cxx_base = (uint64_t)cxx_mod.load_virtbase;
  if (a >= gd_base && a < gd_base + game_mod.load_size)
    debugPrintf("[watchdog]   %s %016llx  godot+0x%llx\n", tag, (unsigned long long)a, (unsigned long long)(a - gd_base));
  else if (a >= cxx_base && a < cxx_base + cxx_mod.load_size)
    debugPrintf("[watchdog]   %s %016llx  libc+++0x%llx\n", tag, (unsigned long long)a, (unsigned long long)(a - cxx_base));
  else if (a >= wrap_base && a < wrap_base + 0x800000)
    debugPrintf("[watchdog]   %s %016llx  mmx+0x%llx\n", tag, (unsigned long long)a, (unsigned long long)(a - wrap_base));
  else
    debugPrintf("[watchdog]   %s %016llx\n", tag, (unsigned long long)a);
}

static void watchdog_dump_thread(Thread *t, const char *what) {
  if (R_FAILED(threadPause(t))) {
    debugPrintf("[watchdog] could not pause %s\n", what);
    return;
  }
  ThreadContext ctx;
  Result rc = svcGetThreadContext3(&ctx, t->handle);
  if (R_SUCCEEDED(rc)) {
    debugPrintf("[watchdog] %s:\n", what);
    log_code_addr("PC", ctx.pc.x);
    log_code_addr("LR", ctx.lr);
    // walk the frame-pointer chain: [fp] = next fp, [fp+8] = return address
    uint64_t fp = ctx.fp;
    const uint64_t sp = ctx.sp;
    for (int i = 0; i < 12; i++) {
      if (fp < sp || fp > sp + (16ull << 20) || (fp & 7)) break;
      const uint64_t next = *(const uint64_t *)fp;
      const uint64_t ret = *(const uint64_t *)(fp + 8);
      if (!ret) break;
      char tag[8];
      snprintf(tag, sizeof(tag), "#%d", i);
      log_code_addr(tag, ret);
      if (next <= fp) break;
      fp = next;
    }
  } else {
    debugPrintf("[watchdog] svcGetThreadContext3(%s) failed: %08x\n", what, rc);
  }
  threadResume(t);
}

static void watchdog_dump(void) {
  debugPrintf("[watchdog] bases: main()=%p godot=%p libc++=%p\n",
              (void *)&main, game_mod.load_virtbase, cxx_mod.load_virtbase);
  watchdog_dump_thread(&s_game_thread, "game thread");

  Thread *thr[16];
  void *entry[16];
  int n = mmx_engine_threads(thr, entry, 16);
  for (int i = 0; i < n; i++) {
    char what[64];
    snprintf(what, sizeof(what), "engine thread %d (entry godot+0x%lx)", i,
             (unsigned long)((uintptr_t)entry[i] - (uintptr_t)game_mod.load_virtbase));
    watchdog_dump_thread(thr[i], what);
  }
}

static void load_module(so_module *mod, const char *name, void *base, size_t limit) {
  int res = so_load(mod, name, base, limit);
  if (res < 0)
    fatal_error("Could not load\n%s (%d).", name, res);
  debugPrintf("== so_load %s ok (load_size=%u KB) ==\n", name, (unsigned)(mod->load_size >> 10));
}

int main(void) {
  cpu_boost(1);

  if (read_config(CONFIG_NAME) != 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  resolve_data_root(); // adopt the actual launch folder as the data root
  stats_open();
  check_data();
  apply_asset_hotfixes(); // restore game data files known to be missing from the APK export
  mkdir(config.save_root, 0777);
  {
    char cache[300];
    // Godot's GLES3 rasterizer creates user://shader_cache at boot; if that
    // make_dir goes through a code path our JNI DirAccess mishandles it prints
    // "Can't create shader cache folder ... user://" and disables ITS shader
    // cache. Pre-creating it here means Godot's change_dir("shader_cache")
    // succeeds and it never hits the failing make_dir path. (This is Godot's own
    // cache, separate from the mesa disk cache configured below.)
    snprintf(cache, sizeof(cache), "%s/shader_cache", config.save_root);
    mkdir(cache, 0777);
  }
  write_shader_overrides(); // stage our compat MSDF text shaders under <save_root>/_ovr
  setenv("HOME", config.save_root, 1);

  // GPU driver tuning, set BEFORE the GL driver comes up in egl_setup().
  // Persist the compiled-shader disk cache across launches so shaders don't
  // recompile every boot. MESA_GLTHREAD stays FALSE: a GL worker thread races
  // the driver's small-buffer allocator and corrupts its pool.
  {
    static char scache[300];
    snprintf(scache, sizeof(scache), "%s/shadercache", config.data_root);
    mkdir(scache, 0777);
    // Select the GL driver and point the gallium loader at it. The on-disk
    // shader cache (MESA_SHADER_CACHE_*, old MESA_GLSL_CACHE_* as aliases)
    // persists across launches, so shaders compile once.
    setenv("MESA_SWITCH_GL_DRIVER", "nvc0", 1);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "nouveau", 1);
    setenv("MESA_SHADER_CACHE_DIR", scache, 1);
    setenv("MESA_SHADER_CACHE_DISABLE", "false", 1);
    setenv("MESA_GLSL_CACHE_DIR", scache, 1);
    setenv("MESA_GLSL_CACHE_DISABLE", "false", 1);
    setenv("MESA_GLTHREAD", "false", 1);
    // NVK on the Switch: CPU/GPU cache-coherency mitigations. The GPFIFO is a
    // known hazard -- HOST_CACHED memory lets the GPU read stale data, and the
    // channel idle-times out (nvkmd type=8) -> VK_ERROR_DEVICE_LOST -> crash.
    // Mapping GPU memory CPU-uncached removes the race (costs bandwidth, not
    // correctness). NVK reads these at device/memory init, so set them now.
    setenv("NVK_SWITCH_CMD_MEM_CPU_UNCACHED", "1", 1);
    setenv("NVK_SWITCH_MEM_STREAM_CPU_UNCACHED", "1", 1);
    setenv("NVK_SWITCH_MAPPED_COMPLETION", "1", 1);
    debugPrintf("== mesa shader cache dir: %s ==\n", scache);
  }

  set_screen_size(config.screen_width, config.screen_height);

  // Decide the renderer ONCE, before any context: probe the static Vulkan (NVK).
  // If it comes up, e_setup's args pass --rendering-driver vulkan and we skip the
  // EGL/GLES3 context; egl_setup would have sized the NWindow, so do that here. If
  // the probe fails, fall back to the GLES3 context as before.
  s_use_vulkan = config.enable_vulkan && vulkan_shim_probe();
  debugPrintf(s_use_vulkan ? "== renderer: VULKAN (NVK) ==\n"
                           : "== renderer: GLES3 ==\n");
  sts2_c11_report();  // confirm the C11 mtx/cnd shim is in effect (not newlib's)
  call_once_report(); // confirm the call_once shim is in effect
  if (s_use_vulkan) {
    NWindow *win = nwindowGetDefault();
    if (win) nwindowSetDimensions(win, screen_width, screen_height);
  } else if (egl_setup() != 0) {
    fatal_error("Could not create the EGL/GLES3 context.");
  }

  debugPrintf("== MMX Regenesis Switch wrapper booting; build " __DATE__ " " __TIME__ "; data_root=%s ==\n", config.data_root);
  debugPrintf("== EGL/GLES3 context created (%dx%d) ==\n", screen_width, screen_height);

  // libc++ first so libgodot's C++ ABI imports resolve against it
  load_module(&cxx_mod, CXX_SO_NAME, heap_so_base, CXX_SO_SLICE);
  void *game_base = (char *)heap_so_base + CXX_SO_SLICE;
  load_module(&game_mod, SO_NAME, game_base, heap_so_limit - CXX_SO_SLICE);

  mmx_resolve_imports(&cxx_mod);
  mmx_resolve_imports(&game_mod);
  debugPrintf("== imports resolved ==\n");
  so_patch(&game_mod);

  // resolve exports before so_finalize maps the code and locks load_base out
  resolve_entry_points();

  so_finalize(&cxx_mod);
  so_flush_caches(&cxx_mod);
  so_finalize(&game_mod);
  so_flush_caches(&game_mod);
  debugPrintf("== so_finalize ok; running init arrays ==\n");

  jni_init();
  tls_setup_guard();
  so_execute_init_array(&cxx_mod);
  so_execute_init_array(&game_mod);
  so_free_temp(&cxx_mod);
  so_free_temp(&game_mod);
  debugPrintf("== init arrays done ==\n");
  stats_mark("modules loaded + init arrays");

  // Asset pack: fold the ~4800 loose asset files into ONE indexed pack for fast
  // SD I/O. Opening loose files one-by-one on FAT is what stalls the intro/level
  // loads; a single indexed file replaces per-file directory lookups with an
  // in-memory index + sequential reads. Built ON-DEVICE from the user's own
  // assets the first time (nothing game-owned is shipped). Fully safe: if the
  // pack is missing or bad, asset_pack_active() stays false and every asset read
  // falls back to the loose files exactly as before. Overrides (_ovr) are never
  // packed, so the lighting/shader fixes keep working on top of the pack.
  if (config.assetpack) {
    char adir[512];
    snprintf(adir, sizeof(adir), "%s/assets", config.data_root);
    if (!asset_pack_stale(adir, config.data_root) && asset_pack_open_existing(config.data_root)) {
      debugPrintf("[pack] mounted existing pack (%zu entries)\n", asset_pack_entry_count());
    } else {
      debugPrintf("[pack] building from loose assets (missing or assets updated; one-time, may take a bit)...\n");
      if (!asset_pack_build(adir, config.data_root)) {
        debugPrintf("[pack] build FAILED: %s -- using loose files\n", asset_pack_error());
      } else if (asset_pack_open_existing(config.data_root)) {
        debugPrintf("[pack] built + mounted (%zu entries)\n", asset_pack_entry_count());
      } else {
        debugPrintf("[pack] built but mount failed -- using loose files\n");
      }
    }
  }

  // the game sees cwd="/" (getcwd_fake) and stray absolute writes are rebased
  // into save_root (sandbox_path); move the REAL cwd there too so any genuine
  // relative libc paths agree. The .so files were already loaded above.
  if (chdir(config.save_root) != 0)
    debugPrintf("!! chdir(%s) failed\n", config.save_root);

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  // Dedicated high-rate input sampler so button presses are never lost while the
  // game thread is stalled for seconds inside e_step (see s_pending_down).
  padInitializeAny(&s_ipad);
  s_ithread_run = 1;
  if (R_SUCCEEDED(threadCreate(&s_ithread, input_capture_thread, NULL, NULL, 0x4000, 0x2C, -2)))
    threadStart(&s_ithread);

  if (R_FAILED(threadCreate(&s_game_thread, game_thread_fn, NULL, NULL, 8 * 1024 * 1024, 0x2C, -2)))
    fatal_error("Could not create the game thread.");
  threadStart(&s_game_thread);

  int last_frames = -1;
  int stall_ms = 0;
  while (appletMainLoop() && s_game_running) {
    AppletFocusState fs = appletGetFocusState();
    s_focused = (fs == AppletFocusState_InFocus);

    // hang watchdog: dump the game thread's stack once every 20 s of stall
    if (s_focused) {
      if (s_frames_done != last_frames) {
        last_frames = s_frames_done;
        stall_ms = 0;
      } else if ((stall_ms += 16) >= 45000) {
        debugPrintf("[watchdog] no step completed for 45 s (steps done: %d)\n", s_frames_done);
        watchdog_dump();
        stall_ms = 0;
      }
    }
    svcSleepThread(16 * 1000 * 1000);
  }

  s_game_running = 0;
  threadWaitForExit(&s_game_thread);
  threadClose(&s_game_thread);

  if (s_ctx != EGL_NO_CONTEXT) {
    eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(s_dpy, s_ctx);
    eglDestroySurface(s_dpy, s_surf);
    eglTerminate(s_dpy);
  }

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
