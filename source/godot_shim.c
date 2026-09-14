/* godot_shim.c -- bionic/NDK shims added for libgodot_android.so (Godot 4.7):
 * bionic dirent conversion, rwlocks, AAssetManager over the assets/ dir,
 * and assorted linux-isms newlib lacks. Camera/media NDK stubs live in
 * imports.c as plain ret0/retm1 entries. MIT license; see LICENSE. */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <math.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "godot_shim.h"
#include "libc_shim.h"
#include "util.h"
#include "asset_pack.h"

// ---------------------------------------------------------------------------
// bionic dirent: { u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[256]; }
// newlib's struct dirent differs, so wrap the directory stream and convert.
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino;
  int64_t  d_off;
  uint16_t d_reclen;
  uint8_t  d_type;
  char     d_name[256];
};

#define BIONIC_DT_UNKNOWN 0
#define BIONIC_DT_DIR     4
#define BIONIC_DT_REG     8

typedef struct {
  uint32_t magic; // 'BDIR'
  DIR *dir;
  char path[512];
  struct bionic_dirent ent;
} FakeDir;

#define FAKEDIR_MAGIC 0x42444952

void *opendir_fake(const char *path) {
  if (!path) return NULL;
  char sb[640];
  path = sandbox_path(path, sb, sizeof(sb));
  // Strip trailing slashes before opendir (fatfs rejects them; DirAccessUnix's
  // fix_path can produce "<dir>/" for user://).
  char nb[640];
  size_t l = strlen(path);
  while (l > 1 && path[l - 1] == '/') l--;
  snprintf(nb, sizeof(nb), "%.*s", (int)l, path);
  DIR *d = opendir(nb);
  if (!d) return NULL;
  FakeDir *fd = calloc(1, sizeof(*fd));
  if (!fd) { closedir(d); return NULL; }
  fd->magic = FAKEDIR_MAGIC;
  fd->dir = d;
  strncpy(fd->path, nb, sizeof(fd->path) - 1);
  return fd;
}

void *fdopendir_fake(int fd) { (void)fd; return NULL; }

void *readdir_fake(void *dirp) {
  FakeDir *fd = dirp;
  if (!fd || fd->magic != FAKEDIR_MAGIC) return NULL;
  struct dirent *e = readdir(fd->dir);
  if (!e) return NULL;
  memset(&fd->ent, 0, sizeof(fd->ent));
  fd->ent.d_ino = 1;
  fd->ent.d_reclen = sizeof(fd->ent);
  strncpy(fd->ent.d_name, e->d_name, sizeof(fd->ent.d_name) - 1);
  // newlib on Switch has no d_type; stat to tell dirs from files (Godot's
  // DirAccessUnix falls back to stat when DT_UNKNOWN, but be explicit).
  char full[768];
  snprintf(full, sizeof(full), "%s/%s", fd->path, e->d_name);
  struct stat st;
  if (stat(full, &st) == 0)
    fd->ent.d_type = S_ISDIR(st.st_mode) ? BIONIC_DT_DIR : BIONIC_DT_REG;
  else
    fd->ent.d_type = BIONIC_DT_UNKNOWN;
  return &fd->ent;
}

int closedir_fake(void *dirp) {
  FakeDir *fd = dirp;
  if (!fd || fd->magic != FAKEDIR_MAGIC) return -1;
  closedir(fd->dir);
  fd->magic = 0;
  free(fd);
  return 0;
}

// ---------------------------------------------------------------------------
// pthread rwlock via libnx RwLock, pointer-indirected like the mutex fakes
// (bionic zero-initializes the storage inline; first use allocates).
// ---------------------------------------------------------------------------

typedef struct { RwLock l; } FakeRwLock;

static FakeRwLock *ensure_rwlock(void **lk) {
  if (!*lk) {
    FakeRwLock *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    rwlockInit(&r->l);
    // benign race at worst leaks one small object; engine inits these early
    *lk = r;
  }
  return (FakeRwLock *)*lk;
}

int pthread_rwlock_rdlock_fake(void **lk) {
  FakeRwLock *r = ensure_rwlock(lk);
  if (!r) return -1;
  rwlockReadLock(&r->l);
  return 0;
}
int pthread_rwlock_wrlock_fake(void **lk) {
  FakeRwLock *r = ensure_rwlock(lk);
  if (!r) return -1;
  rwlockWriteLock(&r->l);
  return 0;
}
int pthread_rwlock_unlock_fake(void **lk) {
  FakeRwLock *r = (FakeRwLock *)*lk;
  if (!r) return -1;
  // libnx needs the matching unlock; the write path holds the writer lock
  if (rwlockIsWriteLockHeldByCurrentThread(&r->l))
    rwlockWriteUnlock(&r->l);
  else
    rwlockReadUnlock(&r->l);
  return 0;
}

// ---------------------------------------------------------------------------
// misc bionic/linux
// ---------------------------------------------------------------------------

int gettid_fake2(void) {
  u64 id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&id, CUR_THREAD_HANDLE)) && id)
    return (int)(id & 0x7fffffff);
  return 1;
}

int pthread_gettid_np_fake(void *thread) { (void)thread; return gettid_fake2(); }

unsigned long getauxval_fake(unsigned long type) { (void)type; return 0; }

int __system_property_get_fake(const char *name, char *value) {
  (void)name;
  if (value) value[0] = 0;
  return 0;
}

struct bionic_rlimit { uint64_t rlim_cur, rlim_max; };
int getrlimit_fake(int res, void *rlim) {
  (void)res;
  struct bionic_rlimit *r = rlim;
  if (r) { r->rlim_cur = r->rlim_max = 8ull * 1024 * 1024; } // plausible stack cap
  return 0;
}

// no symlinks on fatfs: canonicalization is a plain copy
char *realpath_fake(const char *path, char *resolved) {
  if (!path) { errno = EINVAL; return NULL; }
  char *out = resolved ? resolved : malloc(4096);
  if (!out) return NULL;
  strncpy(out, path, 4095);
  out[4095] = 0;
  return out;
}

int mkstemp_fake(char *tmpl) {
  if (!tmpl) { errno = EINVAL; return -1; }
  size_t l = strlen(tmpl);
  if (l < 6) { errno = EINVAL; return -1; }
  static int counter = 0;
  for (int tries = 0; tries < 100; tries++) {
    snprintf(tmpl + l - 6, 7, "%06d", (counter++) % 1000000);
    int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) return fd;
  }
  errno = EEXIST;
  return -1;
}

// sandboxed variants of the direct filesystem mutators
int mkdir_fake(const char *path, int mode) {
  char sb[640];
  return mkdir(sandbox_path(path, sb, sizeof(sb)), (mode_t)mode);
}
int unlink_fake(const char *path) {
  char sb[640];
  return unlink(sandbox_path(path, sb, sizeof(sb)));
}
int rmdir_fake(const char *path) {
  char sb[640];
  return rmdir(sandbox_path(path, sb, sizeof(sb)));
}
int rename_fake(const char *from, const char *to) {
  char s1[640], s2[640];
  return rename(sandbox_path(from, s1, sizeof(s1)), sandbox_path(to, s2, sizeof(s2)));
}
int remove_fake(const char *path) {
  char sb[640];
  return remove(sandbox_path(path, sb, sizeof(sb)));
}

#define BIONIC_AT_FDCWD (-100)

int openat_fake(int dirfd, const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & BIONIC_O_CREAT) {
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
  }
  if (dirfd != BIONIC_AT_FDCWD && path && path[0] != '/') { errno = ENOSYS; return -1; }
  // open_fake, not open: it rebases the path via sandbox_path() and translates
  // the bionic O_* flags. Passing raw sent absolute paths to the SD root and
  // truncated on append -- the bug that keeps a shader cache empty.
  return open_fake(path, flags, (int)mode);
}
int unlinkat_fake(int dirfd, const char *path, int flags) {
  if (dirfd != BIONIC_AT_FDCWD && path && path[0] != '/') { errno = ENOSYS; return -1; }
  if (flags) return rmdir_fake(path);
  return unlink_fake(path);
}
int fchmodat_fake(int dirfd, const char *path, int mode, int flags) {
  (void)dirfd; (void)path; (void)mode; (void)flags; return 0;
}
int utimensat_fake(int dirfd, const char *path, const void *times, int flags) {
  (void)dirfd; (void)path; (void)times; (void)flags; return 0;
}

long pathconf_fake(const char *path, int name) { (void)path; (void)name; return 4096; }

int sched_getaffinity_fake(int pid, size_t setsize, void *mask) {
  (void)pid;
  if (mask && setsize >= 1) { memset(mask, 0, setsize); ((uint8_t *)mask)[0] = 0x7; } // 3 cores
  return 0;
}
int sched_setaffinity_fake(int pid, size_t setsize, const void *mask) {
  (void)pid; (void)setsize; (void)mask; return 0;
}

// thread_local destructors: threads live for the process lifetime here, so
// registering the destructors is safely skippable (leaks only at thread exit).
int __cxa_thread_atexit_impl_fake(void (*dtor)(void *), void *obj, void *dso) {
  (void)dtor; (void)obj; (void)dso; return 0;
}

void __FD_SET_chk_fake(int fd, void *set, size_t setsize) {
  if (set && fd >= 0 && (size_t)(fd / 8) < setsize)
    ((uint8_t *)set)[fd / 8] |= 1u << (fd % 8);
}

struct bionic_statvfs {
  uint64_t f_bsize, f_frsize, f_blocks, f_bfree, f_bavail;
  uint64_t f_files, f_ffree, f_favail;
  uint64_t f_fsid;
  uint64_t f_flag, f_namemax;
  uint64_t __spare[6];
};
int statvfs_fake(const char *path, void *buf) {
  (void)path;
  struct bionic_statvfs *s = buf;
  memset(s, 0, sizeof(*s));
  s->f_bsize = s->f_frsize = 0x1000;
  s->f_blocks = (4ull * 1024 * 1024 * 1024) / 0x1000;
  s->f_bfree = s->f_bavail = (2ull * 1024 * 1024 * 1024) / 0x1000;
  s->f_namemax = 255;
  return 0;
}

int truncate_fake(const char *path, int64_t len) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  int rc = ftruncate(fd, (off_t)len);
  close(fd);
  return rc;
}

// The game spams a few benign GDScript errors on every input event / frame
// (e.g. input.gd's _is_paused access, and lighting/sound _process errors).
// Writing each of those (plus its backtrace) to the SD card per event tanks
// the framerate, so drop that high-frequency noise from the log. Genuine
// one-off engine messages and our own [wrapper] logs are kept.
static int godot_log_drop(const char *s) {
  if (!s) return 0;
  if (strstr(s, "SCRIPT ERROR")) return 1;
  if (strstr(s, "GDScript backtrace")) return 1;
  if (strstr(s, "Unicode parsing error")) return 1;
  if (strstr(s, "at: _input") || strstr(s, "at: _process")) return 1;
  if (strstr(s, "landning and setting dashing")) return 1; // per-frame grounded-state debug spam
  const char *t = s;
  while (*t == ' ' || *t == '\t') t++;
  if (t[0] == '[' && t[1] >= '0' && t[1] <= '9' && t[2] == ']') return 1; // backtrace frame
  return 0;
}

int __android_log_vprint_fake(int prio, const char *tag, const char *fmt, va_list va) {
  (void)prio;
#if DEBUG_LOG
  char buf[0x800];
  vsnprintf(buf, sizeof(buf), fmt, va);
  if (godot_log_drop(buf)) return 0;
  debugPrintf("[%s] %s\n", tag ? tag : "", buf);
#else
  (void)tag; (void)fmt; (void)va;
#endif
  return 0;
}

int android_log_write_fake(int prio, const char *tag, const char *msg) {
  (void)prio;
#if DEBUG_LOG
  if (godot_log_drop(msg)) return 0;
  debugPrintf("[%s] %s\n", tag ? tag : "", msg ? msg : "");
#else
  (void)tag; (void)msg;
#endif
  return 0;
}

void perror_fake(const char *s) {
#if DEBUG_LOG
  debugPrintf("perror: %s: %s\n", s ? s : "", strerror(errno));
#else
  (void)s;
#endif
}

int isatty_fake(int fd) { (void)fd; return 0; }

// On real Android the app process cwd is "/", and Godot's ProjectSettings
// uses the cwd during project discovery to derive resource_path: any real
// directory reported here leaks into every res:// path the engine builds
// (the empty-character-select bug). Mimic Android: cwd is always "/", and the
// sandbox_path() rebase in libc_shim keeps stray absolute writes ("/saves")
// inside the app's save dir instead of the SD root.
char *getcwd_fake(char *buf, size_t size) {
  if (!buf) return strdup("/");
  if (size < 2) { errno = ERANGE; return NULL; }
  strcpy(buf, "/");
  return buf;
}

// The Switch fatfs has no working directory, so the libc chdir fails on every
// /switch path. Godot's DirAccessUnix::change_dir only uses chdir to test that a
// directory exists (it tracks the path itself, and getcwd_fake always returns
// "/"), so validate the directory with opendir and return success. This is what
// makes DirAccess::open("user://") succeed instead of failing before it ever
// reaches opendir -- and in turn lets Godot create and use its shader cache,
// which had been recompiling every run and stuttering on new effects/scene loads.
int chdir_fake(const char *path) {
  if (!path) { errno = EINVAL; return -1; }
  char sb[640];
  const char *rp = sandbox_path(path, sb, sizeof(sb));
  char nb[640];
  size_t l = strlen(rp);
  while (l > 1 && rp[l - 1] == '/') l--; // fatfs opendir rejects trailing '/'
  snprintf(nb, sizeof(nb), "%.*s", (int)l, rp);
  DIR *d = opendir(nb);
  if (!d) { errno = ENOENT; return -1; }
  closedir(d);
  return 0;
}

// ---------------------------------------------------------------------------
// locale _l variants: single-locale system, forward to the C versions
// ---------------------------------------------------------------------------

int strcoll_l_fake(const char *a, const char *b, void *loc) { (void)loc; return strcmp(a, b); }
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) {
  (void)loc; return strftime(s, max, fmt, (const struct tm *)tm);
}
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) { (void)loc; return strxfrm(dst, src, n); }
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscmp(a, b); }
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) { (void)loc; return wcsxfrm(dst, src, n); }
int towlower_l_fake(int c, void *loc) { (void)loc; return towlower(c); }
int towupper_l_fake(int c, void *loc) { (void)loc; return towupper(c); }
int isdigit_l_fake(int c, void *loc) { (void)loc; return isdigit(c); }
int isxdigit_l_fake(int c, void *loc) { (void)loc; return isxdigit(c); }
int islower_l_fake(int c, void *loc) { (void)loc; return islower(c); }
int isupper_l_fake(int c, void *loc) { (void)loc; return isupper(c); }
int tolower_l_fake(int c, void *loc) { (void)loc; return tolower(c); }
int toupper_l_fake(int c, void *loc) { (void)loc; return toupper(c); }
long double log10l_fake(long double x) { return (long double)log10((double)x); }

int iswalpha_l_fake(int c, void *loc) { (void)loc; return iswalpha(c); }
int iswblank_l_fake(int c, void *loc) { (void)loc; return iswblank(c); }
int iswcntrl_l_fake(int c, void *loc) { (void)loc; return iswcntrl(c); }
int iswdigit_l_fake(int c, void *loc) { (void)loc; return iswdigit(c); }
int iswlower_l_fake(int c, void *loc) { (void)loc; return iswlower(c); }
int iswprint_l_fake(int c, void *loc) { (void)loc; return iswprint(c); }
int iswpunct_l_fake(int c, void *loc) { (void)loc; return iswpunct(c); }
int iswspace_l_fake(int c, void *loc) { (void)loc; return iswspace(c); }
int iswupper_l_fake(int c, void *loc) { (void)loc; return iswupper(c); }
int iswxdigit_l_fake(int c, void *loc) { (void)loc; return iswxdigit(c); }

// ---------------------------------------------------------------------------
// AAssetManager over <data_root>/assets/: FileAccessAndroid opens every res://
// file through this. Paths arrive relative ("project.binary", "Instances/...").
// ---------------------------------------------------------------------------

typedef struct {
  uint32_t magic; // 'ASET'
  FILE *f;            // NULL for a wrapper-provided in-memory override
  const char *mem;    // non-NULL -> serve these bytes instead of a file
  int packed;         // 1 -> served from the asset pack via pack_fd
  int pack_fd;        // asset-pack handle (valid only when packed)
  int64_t pos;        // read cursor for the mem case
  int64_t len;
} FakeAsset;

#define FAKEASSET_MAGIC 0x41534554

// ---------------------------------------------------------------------------
// Wrapper-provided shader overrides. The game's text_style / text_selected_fx
// shaders ray-march each glyph through five texture samplers, which Godot's GL
// Compatibility renderer can't bind on the Switch's nouveau/mesa driver
// (GL_INVALID_VALUE in glUniform1i) -> garbled menu text. We can't ship the
// game's assets (copyright) and don't want users hand-editing files, so we
// transparently serve these compatibility shaders whenever the engine opens
// those two res:// paths. They draw each glyph flat but legible and keep each
// label's modulate color. This is our own shader source, not the game's.
//
// Glyph coverage is min(median(rgb), alpha), right for both kinds of font atlas:
// MSDF (distance in rgb) and plain LumAlpha8, which Vulkan samples as (L,L,L,A)
// with L=1 -- there the rgb median alone is 1 over the whole glyph quad and the
// text turns into solid blocks. The 1.00.91/1.00.92 fonts (the main pixel font and the
// CJK/JP/RU/Intl fallbacks) are all LumAlpha8, none is MSDF. If the driver
// ignores the swizzle, alpha reads 1 and this is the previous median-only formula.
// ---------------------------------------------------------------------------
static const char OVR_TEXT_STYLE[] =
  "shader_type canvas_item;\n"
  "render_mode blend_premul_alpha;\n"
  "float msdf_median(vec3 c){return max(min(c.r,c.g),min(max(c.r,c.g),c.b));}\n"
  "void fragment(){\n"
  "  vec4 t=texture(TEXTURE,UV);\n"
  "  float d=min(msdf_median(t.rgb),t.a);\n"
  "  float w=fwidth(d);\n"
  "  float cov=smoothstep(0.5-w,0.5+w,d);\n"
  "  float a=cov*COLOR.a;\n"
  "  COLOR=vec4(COLOR.rgb*a,a);\n"
  "}\n"
  "// nx-pad";  // trailing comment w/o newline: holds the padding spaces (write_shader_overrides)

static const char OVR_TEXT_SELECTED[] =
  "shader_type canvas_item;\n"
  "render_mode blend_premul_alpha;\n"
  "uniform float select_intensity : hint_range(0.0,1.0) = 0.0;\n"
  "uniform float select_shimmer_speed = 2.0;\n"
  "float msdf_median(vec3 c){return max(min(c.r,c.g),min(max(c.r,c.g),c.b));}\n"
  "void fragment(){\n"
  "  vec4 t=texture(TEXTURE,UV);\n"
  "  float d=min(msdf_median(t.rgb),t.a);\n"
  "  float w=fwidth(d);\n"
  "  float cov=smoothstep(0.5-w,0.5+w,d);\n"
  "  vec3 rgb=COLOR.rgb;\n"
  "  float sh=1.0+select_intensity*0.35*(0.5+0.5*sin(TIME*select_shimmer_speed*6.2831853));\n"
  "  rgb*=sh;\n"
  "  float a=cov*COLOR.a;\n"
  "  COLOR=vec4(rgb*a,a);\n"
  "}\n"
  "// nx-pad";  // trailing comment w/o newline: holds the padding spaces (write_shader_overrides)

// The two text shaders that break on nouveau (5 samplers) are replaced by
// our own 1-sampler compatibility shaders. We WRITE them to real files under
// <save_root>/_ovr at startup and redirect reads there, rather than serving the
// source from memory: Godot reads shader CONTENT through the JNI FileAccessHandler
// (jni_fake.c), not through AAsset, so an in-memory AAsset override alone gets the
// size right but the content still comes from the real file -> truncated -> parse
// error. A real file keeps both read paths consistent (proven to load correctly).
static int is_override_shader(const char *base) {
  // These MUST stay overridden on BOTH renderers. On GL Compatibility the game's
  // shaders hit a 5-sampler glUniform1i limit; on Vulkan/NVK they ray-march each
  // glyph in a loop that idle-times-out the GPU channel (type=8, device lost).
  // Our 1-sampler replacement renders flat-but-legible text with neither.
  if (!strcmp(base, "text_style.gdshader") ||
      !strcmp(base, "text_selected_fx.gdshader"))
    return 1;
  return 0;
}

// Trivial passthrough for canvas_item: output the source texture unchanged.
static const char OVR_PASSTHROUGH[] =
  "shader_type canvas_item;\n"
  "void fragment(){ COLOR = texture(TEXTURE, UV); }\n"
  "// nx-pad";

// The compat source we serve for a given override-shader basename.
static const char *override_src_for(const char *base) {
  if (!strcmp(base, "text_style.gdshader"))       return OVR_TEXT_STYLE;
  if (!strcmp(base, "text_selected_fx.gdshader")) return OVR_TEXT_SELECTED;
  return OVR_PASSTHROUGH; // defensive default; is_override_shader only serves the two above
}

// Is this file one we serve a copy of from <save_root>/_ovr? The compatibility
// text shaders always; a game script only once script_patch.c has written its
// patched copy this boot (a copy left over from an earlier boot is never served).
#define MAX_SCRIPT_OVERRIDES 4
static char s_script_overrides[MAX_SCRIPT_OVERRIDES][64];
static int s_script_override_count;

static int is_override_file(const char *base) {
  if (is_override_shader(base)) return 1;
  for (int i = 0; i < s_script_override_count; i++)
    if (!strcmp(base, s_script_overrides[i])) return 1;
  return 0;
}

// If `filename` (a relative asset path, or an absolute .../assets/... one) is served
// from _ovr, writes "<save_root>/_ovr/<basename>" to `out` and returns 1. `out` may be
// `filename` itself: the basename is copied first.
int asset_override_path(const char *filename, char *out, size_t size) {
  const char *slash = strrchr(filename, '/');
  char base[64];
  if (snprintf(base, sizeof(base), "%s", slash ? slash + 1 : filename) >= (int)sizeof(base))
    return 0; // longer than any file we override
  if (!is_override_file(base)) return 0;
  snprintf(out, size, "%s/_ovr/%s", config.save_root, base);
  return 1;
}

// Writes <save_root>/_ovr/<base>; returns 1 when the file reads back exactly as
// `data`. Some FAT/SD stacks ignore O_TRUNC on "wb" AND ftruncate, so writing a
// shorter file straight over a longer stale one from an earlier build left a
// garbage tail -- for a shader, a stray char on the line after it -> "Expected
// constant" parse error. Write a fresh temp, then remove+rename over the target:
// the target is created new from the temp, so no old tail survives.
static int write_override_file(const char *base, const void *data, size_t n) {
  char dir[600], p[768], tmp[800];
  snprintf(dir, sizeof(dir), "%s/_ovr", config.save_root);
  mkdir(dir, 0777);
  snprintf(p, sizeof(p), "%s/%s", dir, base);
  snprintf(tmp, sizeof(tmp), "%s.tmp", p);
  remove(tmp);
  FILE *f = fopen(tmp, "wb");
  if (f) {
    fwrite(data, 1, n, f);
    fflush(f);
    fclose(f);
    remove(p);
    if (rename(tmp, p) != 0) {          // fallback: direct truncating write
      FILE *g = fopen(p, "wb");
      if (g) { fwrite(data, 1, n, g); fflush(g);
               if (ftruncate(fileno(g), (off_t)n) != 0) { /* best-effort */ } fclose(g); }
      remove(tmp);
    }
  } else debugPrintf("[ovr] WARN could not write override %s\n", tmp);
  // Verify by reading it back: a mismatch means a stale tail slipped through (SD
  // didn't honour the remove/rename) or the write failed -- surface it in the log.
  int matches = 0;
  FILE *v = fopen(p, "rb");
  if (v) {
    unsigned char chunk[4096];
    size_t off = 0, got;
    matches = 1;
    while (matches && (got = fread(chunk, 1, sizeof(chunk), v)) > 0) {
      matches = off + got <= n && !memcmp(chunk, (const unsigned char *)data + off, got);
      off += got;
    }
    fclose(v);
    if (off != n) matches = 0;
    if (!matches)
      debugPrintf("[ovr] WARN %s on disk doesn't match what was written (stale tail?)\n", base);
  }
  return matches;
}

// Write the compatibility shaders to <save_root>/_ovr at startup. Called from
// main() once save_root is known, before the game loads any scene.
//
// Each copy is padded with spaces to the size of the game's own shader: the engine
// reads every file listed in assets.sparsepck with the size recorded there, so a
// shorter copy came back with the rest of the buffer uninitialized. The "// nx-pad"
// comment only hides that garbage up to its first newline; past one, some boots
// failed with "Cannot parse shader" (text_selected_fx.gdshader on 1.00.91, taking
// input_menu.scn and pause_menu.gd down with it). The spaces stay inside that
// trailing comment. With no original to measure, the copy is written unpadded.
void write_shader_overrides(void) {
  const char *names[2] = {
    "text_style.gdshader", "text_selected_fx.gdshader",
  };
  for (int i = 0; i < 2; i++) {
    const char *src = override_src_for(names[i]);
    const size_t len = strlen(src);
    char original[768];
    struct stat st;
    snprintf(original, sizeof(original), "%s/assets/shaders/%s", config.data_root, names[i]);
    size_t size = len;
    if (stat(original, &st) == 0 && st.st_size > (off_t)len) size = (size_t)st.st_size;
    char *padded = size > len ? malloc(size) : NULL;
    size_t served = len;
    if (padded) {
      memcpy(padded, src, len);
      memset(padded + len, ' ', size - len);
      write_override_file(names[i], padded, size);
      free(padded);
      served = size;
    } else {
      write_override_file(names[i], src, len);
    }
    debugPrintf("[shader] %s: %zu bytes of source, served as %zu\n", names[i], len, served);
  }
  debugPrintf("[shader] compat text overrides written to %s/_ovr\n", config.save_root);
}

// Writes a patched game script to _ovr and starts serving it (script_patch.c).
int script_override_write(const char *base, const void *data, size_t len) {
  if (s_script_override_count >= MAX_SCRIPT_OVERRIDES ||
      strlen(base) >= sizeof(s_script_overrides[0]))
    return 0;
  if (!write_override_file(base, data, len)) return 0;
  snprintf(s_script_overrides[s_script_override_count], sizeof(s_script_overrides[0]), "%s", base);
  s_script_override_count++;
  return 1;
}

static void *g_fake_assetmgr = (void *)0xA55E7;

// Perf diagnosis: how many times the engine opens assets, and how many of those
// the pack served. If the total dwarfs the ~4600 real files, the engine is
// re-loading resources (no cache) rather than reading each once.
int g_asset_open_count = 0;
int g_asset_pack_open_count = 0;

void *AAssetManager_fromJava_fake(void *env, void *assetManager) {
  (void)env; (void)assetManager;
  return g_fake_assetmgr;
}

void *AAssetManager_open_fake(void *mgr, const char *filename, int mode) {
  (void)mgr; (void)mode;
  if (!filename) return NULL;
  while (*filename == '/') filename++;

  // transparent substitution: open OUR copy (written to <save_root>/_ovr at startup)
  // instead of the game's file -- the compatibility text shaders and patched scripts.
  char path[768];
  if (asset_override_path(filename, path, sizeof(path))) {
    debugPrintf("[ovr] serving override for %s\n", filename);
  } else {
    snprintf(path, sizeof(path), "%s/assets/%s", config.data_root, filename);
    // Fast path: serve the game's own assets from the on-device asset pack (one
    // indexed file) instead of opening a loose file on the SD card each time.
    // Overrides (_ovr) are NEVER packed, so this only covers real game assets.
    if (asset_pack_active()) {
      int pfd = asset_pack_open_path(path);
      if (pfd >= 0) {
        FakeAsset *a = calloc(1, sizeof(*a));
        if (!a) { asset_pack_close_fd(pfd); return NULL; }
        a->magic = FAKEASSET_MAGIC;
        a->packed = 1;
        a->pack_fd = pfd;
        uint64_t sz = 0, ino = 0; int isdir = 0;
        asset_pack_fstat_fd(pfd, &sz, &ino, &isdir);
        a->len = (int64_t)sz;
        g_asset_open_count++; g_asset_pack_open_count++;
        return a;
      }
    }
  }
  FILE *f = fopen(path, "rb");
#if VERBOSE_IO
  debugPrintf("AAssetManager_open(\"%s\") -> %p\n", filename, (void *)f);
#endif
  if (!f) return NULL;
  setvbuf(f, NULL, _IOFBF, 64 * 1024);
  g_asset_open_count++;
  FakeAsset *a = calloc(1, sizeof(*a));
  if (!a) { fclose(f); return NULL; }
  a->magic = FAKEASSET_MAGIC;
  a->f = f;
  fseek(f, 0, SEEK_END);
  a->len = ftell(f);
  fseek(f, 0, SEEK_SET);
  return a;
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  FakeAsset *a = asset;
  if (!a || a->magic != FAKEASSET_MAGIC || !buf) return -1;
  if (a->mem) {
    int64_t rem = a->len - a->pos;
    if (rem <= 0) return 0;
    size_t n = (count < (size_t)rem) ? count : (size_t)rem;
    memcpy(buf, a->mem + a->pos, n);
    a->pos += (int64_t)n;
    return (int)n;
  }
  if (a->packed) { long n = asset_pack_read_fd(a->pack_fd, buf, count); return (int)n; }
  return (int)fread(buf, 1, count, a->f);
}

int64_t AAsset_seek_fake(void *asset, int64_t offset, int whence) {
  FakeAsset *a = asset;
  if (!a || a->magic != FAKEASSET_MAGIC) return -1;
  if (a->mem) {
    int64_t np;
    if (whence == SEEK_SET) np = offset;
    else if (whence == SEEK_CUR) np = a->pos + offset;
    else if (whence == SEEK_END) np = a->len + offset;
    else return -1;
    if (np < 0) np = 0;
    if (np > a->len) np = a->len;
    a->pos = np;
    return np;
  }
  if (a->packed) return (int64_t)asset_pack_lseek_fd(a->pack_fd, (long)offset, whence);
  if (fseek(a->f, (long)offset, whence) != 0) return -1;
  return ftell(a->f);
}

int64_t AAsset_getLength_fake(void *asset) {
  FakeAsset *a = asset;
  return (a && a->magic == FAKEASSET_MAGIC) ? a->len : 0;
}
int64_t AAsset_getLength64_fake(void *asset) { return AAsset_getLength_fake(asset); }

void AAsset_close_fake(void *asset) {
  FakeAsset *a = asset;
  if (a && a->magic == FAKEASSET_MAGIC) {
    if (a->packed) asset_pack_close_fd(a->pack_fd);
    if (a->f) fclose(a->f);
    a->magic = 0;
    free(a);
  }
}

// ---------------------------------------------------------------------------
// data symbols
// ---------------------------------------------------------------------------

unsigned char in6addr_any_fake[16];

extern uint8_t fake_sF[3][0x100];
FILE *stdin_fake  = (FILE *)fake_sF[0];
FILE *stdout_fake = (FILE *)fake_sF[1];
FILE *stderr_fake = (FILE *)fake_sF[2];
