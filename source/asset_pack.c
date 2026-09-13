#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <switch.h>

/* Progress callback during the one-time pack build; defined in main.c. */
void startup_status_update(const char *message);

#include "asset_pack.h"
#include "error.h"

#define PACK_VERSION 2u
#define PACK_HANDLES 512
#define PACK_CACHE_SIZE (64u * 1024u)

typedef struct {
  char magic[8];
  uint32_t version;
  uint32_t reserved;
  uint64_t pack_id;
  uint64_t file_size;
  uint64_t data_checksum;
} PackHeader;

typedef struct {
  char magic[8];
  uint32_t version;
  uint32_t count;
  uint64_t pack_id;
  uint64_t pack_size;
  uint64_t paths_size;
  uint64_t checksum;
  uint64_t data_checksum;
} IndexHeader;

typedef struct {
  uint64_t offset;
  uint64_t size;
  uint32_t path_offset;
  uint32_t path_length;
} DiskEntry;

typedef struct {
  char *path;
  uint64_t size;
  uint64_t offset;
} BuildEntry;

typedef struct {
  int used;
  int fd;
  uint32_t entry;
  uint64_t directory_ino;
  uint64_t position;
  uint64_t cache_offset;
  size_t cache_size;
  unsigned char *cache;
  Mutex lock;
} PackHandle;

typedef struct {
  uint64_t magic;
  size_t cursor;
  unsigned dots;
  char prefix[768];
  char name[256];
  char last[256];
} PackDir;

#define PACK_DIR_MAGIC 0x53534e5844495232ULL
#define PACK_DIRECTORY_ENTRY UINT32_MAX

static int g_pack_fd = -1;
static DiskEntry *g_entries;
static char *g_paths;
static size_t g_entry_count;
static char g_pack_path[768];
static PackHandle g_handles[PACK_HANDLES];
static Mutex g_handle_lock;
static Mutex g_pack_io_lock;
static char g_error[192];

static void set_error(const char *message) {
  snprintf(g_error, sizeof g_error, "%s", message ? message : "Unknown error");
}

const char *asset_pack_error(void) {
  return g_error[0] ? g_error : "Asset pack is unavailable";
}

static uint64_t fnv_bytes(uint64_t hash, const void *data, size_t size) {
  const unsigned char *bytes = data;
  for (size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

static int read_at(int fd, void *buffer, size_t size, uint64_t offset) {
  if (lseek(fd, (off_t)offset, SEEK_SET) < 0) return 0;
  size_t done = 0;
  while (done < size) {
    ssize_t got = read(fd, (char *)buffer + done, size - done);
    if (got <= 0) return 0;
    done += (size_t)got;
  }
  return 1;
}

static int write_all(int fd, const void *buffer, size_t size) {
  size_t done = 0;
  while (done < size) {
    ssize_t put = write(fd, (const char *)buffer + done, size - done);
    if (put <= 0) return 0;
    done += (size_t)put;
  }
  return 1;
}

static int valid_relative(const char *path, size_t length) {
  if (!path || !length || path[0] == '/' || path[length - 1] == '/') return 0;
  size_t part = 0;
  for (size_t i = 0; i <= length; i++) {
    if (i == length || path[i] == '/') {
      if (!part || (part == 1 && path[i - 1] == '.') ||
          (part == 2 && path[i - 2] == '.' && path[i - 1] == '.')) return 0;
      part = 0;
    } else {
      if (path[i] == '\\' || path[i] == ':') return 0;
      part++;
    }
  }
  return 1;
}

static int normalize_asset_path(const char *input, char *output, size_t capacity,
                                int allow_empty) {
  if (!input || !output || capacity == 0) return 0;
  char temp[768];
  size_t length = strlen(input);
  if (length >= sizeof temp) return 0;
  for (size_t i = 0; i <= length; i++) temp[i] = input[i] == '\\' ? '/' : input[i];

  const char *relative = NULL;
  if (!strcmp(temp, "assets")) relative = temp + 6;
  else if (!strncmp(temp, "assets/", 7)) relative = temp + 7;
  for (const char *p = temp; (p = strstr(p, "/assets")) != NULL; p++) {
    if (p[7] == 0) relative = p + 7;
    else if (p[7] == '/') relative = p + 8;
  }
  if (!relative) return 0;

  size_t used = 0;
  const char *p = relative;
  while (*p) {
    while (*p == '/') p++;
    const char *start = p;
    while (*p && *p != '/') p++;
    size_t part = (size_t)(p - start);
    if (!part || (part == 1 && start[0] == '.')) continue;
    if (part == 2 && start[0] == '.' && start[1] == '.') return 0;
    if (used && used + 1 >= capacity) return 0;
    if (used) output[used++] = '/';
    if (used + part >= capacity) return 0;
    memcpy(output + used, start, part);
    used += part;
  }
  output[used] = 0;
  return allow_empty || used != 0;
}

static int normalize_relative(const char *input, char *output, size_t capacity) {
  return normalize_asset_path(input, output, capacity, 0);
}

static int entry_compare_path(const char *path, size_t *result) {
  size_t low = 0, high = g_entry_count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    const char *candidate = g_paths + g_entries[mid].path_offset;
    int cmp = strcmp(path, candidate);
    if (cmp == 0) { *result = mid; return 1; }
    if (cmp < 0) high = mid;
    else low = mid + 1;
  }
  *result = low;
  return 0;
}

static int find_relative(const char *path, size_t *result) {
  if (!g_entries || !path) return 0;
  size_t length = strlen(path);
  if (!valid_relative(path, length)) return 0;
  return entry_compare_path(path, result);
}

static int find_directory_relative(const char *path, uint64_t *ino) {
  if (!g_entries || !path) return 0;
  size_t length = strlen(path);
  if (length && !valid_relative(path, length)) return 0;
  if (!length) {
    if (!g_entry_count) return 0;
  } else {
    char prefix[768];
    if (length + 2 > sizeof prefix) return 0;
    memcpy(prefix, path, length);
    prefix[length++] = '/';
    prefix[length] = 0;
    size_t index;
    entry_compare_path(prefix, &index);
    if (index >= g_entry_count ||
        strncmp(g_paths + g_entries[index].path_offset, prefix, length)) return 0;
  }
  uint64_t hash = fnv_bytes(1469598103934665603ULL, path, strlen(path));
  if (ino) *ino = hash ? hash : 1;
  return 1;
}

static void free_loaded(DiskEntry *entries, char *paths, int fd) {
  if (fd >= 0) close(fd);
  free(entries);
  free(paths);
}

static int load_pair(const char *pack_path, const char *index_path,
                     DiskEntry **entries_out, char **paths_out,
                     size_t *count_out, int *fd_out) {
  int pack_fd = -1, index_fd = -1;
  DiskEntry *entries = NULL;
  char *paths = NULL;
  struct stat pack_stat, index_stat;
  PackHeader pack_header;
  IndexHeader index_header;

  pack_fd = open(pack_path, O_RDONLY);
  index_fd = open(index_path, O_RDONLY);
  if (pack_fd < 0 || index_fd < 0 || fstat(pack_fd, &pack_stat) != 0 ||
      fstat(index_fd, &index_stat) != 0) {
    set_error("No complete asset pack was found");
    goto failed;
  }
  if (!read_at(pack_fd, &pack_header, sizeof pack_header, 0) ||
      !read_at(index_fd, &index_header, sizeof index_header, 0) ||
      memcmp(pack_header.magic, "SSNXPAK1", 8) ||
      memcmp(index_header.magic, "SSNXIDX1", 8) ||
      pack_header.version != PACK_VERSION || index_header.version != PACK_VERSION ||
      pack_header.pack_id != index_header.pack_id ||
      pack_header.file_size != (uint64_t)pack_stat.st_size ||
      pack_header.data_checksum != index_header.data_checksum ||
      index_header.pack_size != (uint64_t)pack_stat.st_size ||
      index_header.count == 0 || index_header.count > 100000 ||
      index_header.paths_size == 0 || index_header.paths_size > (64u << 20)) {
    set_error("The asset pack header is invalid");
    goto failed;
  }

  /* NOTE kept from round 138: the validation above compares
   * pack_header.data_checksum to index_header.data_checksum -- the two headers
   * to EACH OTHER. That proves both files came from one run of the packer; it
   * never hashes a payload byte, so a pack damaged after writing still mounts
   * clean. Checked once on device and the payload was intact
   * (FNV b3e645de7a86ae28); the check now lives in tools/verify_pack.py. */
  uint64_t entries_size = (uint64_t)index_header.count * sizeof(DiskEntry);
  uint64_t expected_index = sizeof(IndexHeader) + entries_size + index_header.paths_size;
  if (expected_index != (uint64_t)index_stat.st_size || expected_index > SIZE_MAX) {
    set_error("The asset index size is invalid");
    goto failed;
  }
  entries = malloc((size_t)entries_size);
  paths = malloc((size_t)index_header.paths_size);
  if (!entries || !paths ||
      !read_at(index_fd, entries, (size_t)entries_size, sizeof(IndexHeader)) ||
      !read_at(index_fd, paths, (size_t)index_header.paths_size,
               sizeof(IndexHeader) + entries_size)) {
    set_error("The asset index could not be read");
    goto failed;
  }
  uint64_t checksum = fnv_bytes(1469598103934665603ULL, entries, (size_t)entries_size);
  checksum = fnv_bytes(checksum, paths, (size_t)index_header.paths_size);
  if (checksum != index_header.checksum) {
    set_error("The asset index checksum is invalid");
    goto failed;
  }
  for (uint32_t i = 0; i < index_header.count; i++) {
    DiskEntry *entry = &entries[i];
    uint64_t path_end = (uint64_t)entry->path_offset + entry->path_length + 1;
    if (entry->path_length == 0 || path_end > index_header.paths_size ||
        paths[entry->path_offset + entry->path_length] != 0 ||
        !valid_relative(paths + entry->path_offset, entry->path_length) ||
        entry->offset < sizeof(PackHeader) || entry->offset > index_header.pack_size ||
        entry->size > index_header.pack_size - entry->offset ||
        (i && strcmp(paths + entries[i - 1].path_offset,
                     paths + entry->path_offset) >= 0)) {
      set_error("The asset index contains an invalid entry");
      goto failed;
    }
  }

  close(index_fd);
  *entries_out = entries;
  *paths_out = paths;
  *count_out = index_header.count;
  *fd_out = pack_fd;
  return 1;

failed:
  if (index_fd >= 0) close(index_fd);
  free_loaded(entries, paths, pack_fd);
  return 0;
}

int asset_pack_open_existing(const char *root) {
  if (g_pack_fd >= 0) return 1;
  char pack_path[768], index_path[768];
  snprintf(pack_path, sizeof pack_path, "%s/assets.nxpack", root);
  snprintf(index_path, sizeof index_path, "%s/assets.nxidx", root);
  DiskEntry *entries = NULL;
  char *paths = NULL;
  size_t count = 0;
  int fd = -1;
  if (!load_pair(pack_path, index_path, &entries, &paths, &count, &fd)) return 0;
  g_entries = entries;
  g_paths = paths;
  g_entry_count = count;
  g_pack_fd = fd;
  snprintf(g_pack_path, sizeof g_pack_path, "%s", pack_path);
  g_error[0] = 0;
  return 1;
}

// Returns 1 when <assets_root>/<relative> no longer matches the copy stored in
// the pack (different size or bytes, or not packed at all). A loose file that
// doesn't exist has nothing to compare against and counts as unchanged.
static int packed_copy_differs(int pack_fd, const DiskEntry *entries, const char *paths,
                               size_t count, const char *assets_root, const char *relative) {
  char loose_path[1024];
  snprintf(loose_path, sizeof loose_path, "%s/%s", assets_root, relative);
  struct stat st;
  if (stat(loose_path, &st) != 0) return 0;

  const DiskEntry *entry = NULL;
  size_t low = 0, high = count;
  while (low < high && !entry) {
    size_t mid = low + (high - low) / 2;
    int cmp = strcmp(relative, paths + entries[mid].path_offset);
    if (cmp == 0) entry = &entries[mid];
    else if (cmp < 0) high = mid;
    else low = mid + 1;
  }
  if (!entry || entry->size != (uint64_t)st.st_size) return 1;

  int differs = 1;
  int loose_fd = open(loose_path, O_RDONLY);
  unsigned char *packed = malloc(PACK_CACHE_SIZE), *loose = malloc(PACK_CACHE_SIZE);
  if (loose_fd >= 0 && packed && loose) {
    differs = 0;
    for (uint64_t done = 0; done < entry->size && !differs;) {
      size_t chunk = entry->size - done > PACK_CACHE_SIZE ? PACK_CACHE_SIZE
                                                          : (size_t)(entry->size - done);
      differs = !read_at(pack_fd, packed, chunk, entry->offset + done) ||
                !read_at(loose_fd, loose, chunk, done) || memcmp(packed, loose, chunk) != 0;
      done += chunk;
    }
  }
  free(packed);
  free(loose);
  if (loose_fd >= 0) close(loose_fd);
  return differs;
}

// Returns 1 if the pack should be (re)built: it's missing or unreadable, or the
// assets changed since it was built. project.binary is rewritten on every game
// update and freshly-copied assets usually carry a newer mtime than the pack --
// but not always: 7-Zip keeps the APK's own timestamps, so an update copied off a
// PC can look OLDER than a pack built after that APK was published, and the old
// pack would keep serving the previous version. So when the mtimes don't flag it,
// also compare the packed copies of the files every Godot export rewrites
// (project.binary, and assets.sparsepck, which lists every asset with its md5)
// against the loose ones. Returns 0 when the pack is present and matches.
int asset_pack_stale(const char *assets_root, const char *root) {
  char pack_path[768], index_path[768], src_path[768];
  snprintf(pack_path, sizeof pack_path, "%s/assets.nxpack", root);
  snprintf(index_path, sizeof index_path, "%s/assets.nxidx", root);
  snprintf(src_path, sizeof src_path, "%s/project.binary", assets_root);
  struct stat ps, ss;
  if (stat(pack_path, &ps) != 0) return 1;   // no pack yet -> build
  if (stat(src_path, &ss) != 0) return 0;    // no source to compare -> keep the pack
  if (ss.st_mtime > ps.st_mtime) return 1;   // assets updated after the pack was built

  DiskEntry *entries = NULL;
  char *paths = NULL;
  size_t count = 0;
  int pack_fd = -1;
  if (!load_pair(pack_path, index_path, &entries, &paths, &count, &pack_fd)) return 1;
  int stale = packed_copy_differs(pack_fd, entries, paths, count, assets_root, "project.binary") ||
              packed_copy_differs(pack_fd, entries, paths, count, assets_root, "assets.sparsepck");
  free_loaded(entries, paths, pack_fd);
  return stale;
}

int asset_pack_active(void) {
  return g_pack_fd >= 0;
}

static int build_compare(const void *left, const void *right) {
  const BuildEntry *a = left, *b = right;
  return strcmp(a->path, b->path);
}

static int collect_files(const char *root, const char *relative,
                         BuildEntry **items, size_t *count, size_t *capacity) {
  char directory[1024];
  if (relative[0]) snprintf(directory, sizeof directory, "%s/%s", root, relative);
  else snprintf(directory, sizeof directory, "%s", root);
  DIR *dir = opendir(directory);
  if (!dir) return 0;
  int ok = 1;
  struct dirent *entry;
  while (ok && (entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char child_relative[768], child_path[1024];
    if (snprintf(child_relative, sizeof child_relative, "%s%s%s", relative,
                 relative[0] ? "/" : "", entry->d_name) >= (int)sizeof child_relative ||
        snprintf(child_path, sizeof child_path, "%s/%s", root, child_relative) >=
          (int)sizeof child_path) {
      ok = 0;
      break;
    }
    struct stat st;
    if (stat(child_path, &st) != 0) { ok = 0; break; }
    if (S_ISDIR(st.st_mode)) {
      ok = collect_files(root, child_relative, items, count, capacity);
    } else if (S_ISREG(st.st_mode)) {
      if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 256;
        BuildEntry *grown = realloc(*items, next * sizeof(**items));
        if (!grown) { ok = 0; break; }
        *items = grown;
        *capacity = next;
      }
      (*items)[*count].path = strdup(child_relative);
      if (!(*items)[*count].path) { ok = 0; break; }
      (*items)[*count].size = (uint64_t)st.st_size;
      (*items)[*count].offset = 0;
      (*count)++;
    }
  }
  closedir(dir);
  return ok;
}

static void free_build_entries(BuildEntry *items, size_t count) {
  for (size_t i = 0; i < count; i++) free(items[i].path);
  free(items);
}

static uint64_t align16(uint64_t value) {
  return (value + 15u) & ~15ULL;
}

static int verify_pack_data(int fd, const DiskEntry *entries, size_t count,
                            uint64_t expected) {
  void *buffer = malloc(1u << 20);
  if (!buffer) return 0;
  uint64_t total = 0, checked = 0;
  for (size_t i = 0; i < count; i++) total += entries[i].size;
  uint64_t checksum = 1469598103934665603ULL;
  unsigned last_percent = 101;
  for (size_t i = 0; i < count; i++) {
    uint64_t offset = 0;
    while (offset < entries[i].size) {
      size_t chunk = entries[i].size - offset > (1u << 20)
                       ? (1u << 20) : (size_t)(entries[i].size - offset);
      if (!read_at(fd, buffer, chunk, entries[i].offset + offset)) {
        free(buffer);
        return 0;
      }
      checksum = fnv_bytes(checksum, buffer, chunk);
      offset += chunk;
      checked += chunk;
      unsigned percent = total ? (unsigned)((checked * 100) / total) : 100;
      if (percent != last_percent) {
        char status[96];
        snprintf(status, sizeof status, "Verifying optimized assets\n\n  %u%%", percent);
        startup_status_update(status);
        last_percent = percent;
      }
    }
  }
  free(buffer);
  return checksum == expected;
}

int asset_pack_build(const char *assets_root, const char *root) {
  BuildEntry *items = NULL;
  size_t count = 0, capacity = 0;
  int pack_fd = -1, index_fd = -1, source_fd = -1;
  void *buffer = NULL;
  DiskEntry *disk_entries = NULL;
  char *paths = NULL;
  char pack_path[768], index_path[768], temp_pack[768], temp_index[768];
  snprintf(pack_path, sizeof pack_path, "%s/assets.nxpack", root);
  snprintf(index_path, sizeof index_path, "%s/assets.nxidx", root);
  snprintf(temp_pack, sizeof temp_pack, "%s/assets.nxpack.tmp", root);
  snprintf(temp_index, sizeof temp_index, "%s/assets.nxidx.tmp", root);
  unlink(temp_pack);
  unlink(temp_index);

  if (!collect_files(assets_root, "", &items, &count, &capacity) ||
      count == 0 || count > 100000) {
    set_error("The extracted assets could not be enumerated");
    goto failed;
  }
  qsort(items, count, sizeof(*items), build_compare);
  uint64_t paths_size = 0, total_data = 0, pack_size = sizeof(PackHeader);
  for (size_t i = 0; i < count; i++) {
    size_t path_length = strlen(items[i].path);
    if (path_length > UINT32_MAX || paths_size + path_length + 1 > UINT32_MAX) {
      set_error("The extracted asset paths are too large");
      goto failed;
    }
    pack_size = align16(pack_size);
    items[i].offset = pack_size;
    if (UINT64_MAX - pack_size < items[i].size) {
      set_error("The extracted assets are too large");
      goto failed;
    }
    pack_size += items[i].size;
    paths_size += path_length + 1;
    total_data += items[i].size;
  }

  uint64_t pack_id = 1469598103934665603ULL;
  for (size_t i = 0; i < count; i++) {
    pack_id = fnv_bytes(pack_id, items[i].path, strlen(items[i].path));
    pack_id = fnv_bytes(pack_id, &items[i].size, sizeof items[i].size);
  }
  uint64_t nonce = ((uint64_t)time(NULL) << 32) ^ armGetSystemTick();
  pack_id = fnv_bytes(pack_id, &nonce, sizeof nonce);
  if (!pack_id) pack_id = 1;

  PackHeader pack_header = {0};
  memcpy(pack_header.magic, "SSNXPAK1", 8);
  pack_header.version = PACK_VERSION;
  pack_header.pack_id = pack_id;
  pack_header.file_size = pack_size;
  pack_fd = open(temp_pack, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (pack_fd < 0 || !write_all(pack_fd, &pack_header, sizeof pack_header)) {
    set_error("The temporary asset pack could not be created");
    goto failed;
  }
  buffer = malloc(1u << 20);
  if (!buffer) { set_error("Not enough memory to build the asset pack"); goto failed; }
  uint64_t written_data = 0, output_position = sizeof(PackHeader);
  uint64_t data_checksum = 1469598103934665603ULL;
  unsigned last_percent = 101;
  static const unsigned char padding[16] = {0};
  for (size_t i = 0; i < count; i++) {
    if (items[i].offset > output_position &&
        !write_all(pack_fd, padding, (size_t)(items[i].offset - output_position))) {
      set_error("The asset pack could not be written");
      goto failed;
    }
    output_position = items[i].offset;
    char source_path[1024];
    snprintf(source_path, sizeof source_path, "%s/%s", assets_root, items[i].path);
    source_fd = open(source_path, O_RDONLY);
    if (source_fd < 0) { set_error("An extracted asset disappeared while packing"); goto failed; }
    uint64_t remaining = items[i].size;
    while (remaining) {
      size_t chunk = remaining > (1u << 20) ? (1u << 20) : (size_t)remaining;
      ssize_t got = read(source_fd, buffer, chunk);
      if (got <= 0 || !write_all(pack_fd, buffer, (size_t)got)) {
        set_error("An extracted asset could not be packed");
        goto failed;
      }
      remaining -= (size_t)got;
      output_position += (size_t)got;
      written_data += (size_t)got;
      data_checksum = fnv_bytes(data_checksum, buffer, (size_t)got);
      unsigned percent = total_data ? (unsigned)((written_data * 100) / total_data) : 100;
      if (percent != last_percent) {
        char status[96];
        snprintf(status, sizeof status, "Optimizing game assets (first boot)\n\n  %u%%", percent);
        startup_status_update(status);
        last_percent = percent;
      }
    }
    close(source_fd);
    source_fd = -1;
  }
  pack_header.data_checksum = data_checksum;
  if (lseek(pack_fd, 0, SEEK_SET) < 0 ||
      !write_all(pack_fd, &pack_header, sizeof pack_header)) {
    set_error("The asset pack checksum could not be written");
    goto failed;
  }
  int pack_ok = fsync(pack_fd) == 0;
  if (close(pack_fd) != 0) pack_ok = 0;
  pack_fd = -1;
  if (!pack_ok) {
    set_error("The asset pack could not be finalized");
    goto failed;
  }

  disk_entries = malloc(count * sizeof(*disk_entries));
  paths = malloc((size_t)paths_size);
  if (!disk_entries || !paths) { set_error("Not enough memory to create the asset index"); goto failed; }
  uint32_t path_offset = 0;
  for (size_t i = 0; i < count; i++) {
    size_t path_length = strlen(items[i].path);
    disk_entries[i].offset = items[i].offset;
    disk_entries[i].size = items[i].size;
    disk_entries[i].path_offset = path_offset;
    disk_entries[i].path_length = (uint32_t)path_length;
    memcpy(paths + path_offset, items[i].path, path_length + 1);
    path_offset += (uint32_t)path_length + 1;
  }
  IndexHeader index_header = {0};
  memcpy(index_header.magic, "SSNXIDX1", 8);
  index_header.version = PACK_VERSION;
  index_header.count = (uint32_t)count;
  index_header.pack_id = pack_id;
  index_header.pack_size = pack_size;
  index_header.paths_size = paths_size;
  index_header.checksum = fnv_bytes(1469598103934665603ULL, disk_entries,
                                    count * sizeof(*disk_entries));
  index_header.checksum = fnv_bytes(index_header.checksum, paths, (size_t)paths_size);
  index_header.data_checksum = data_checksum;
  index_fd = open(temp_index, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  int index_ok = index_fd >= 0 &&
                 write_all(index_fd, &index_header, sizeof index_header) &&
                 write_all(index_fd, disk_entries, count * sizeof(*disk_entries)) &&
                 write_all(index_fd, paths, (size_t)paths_size);
  if (index_fd >= 0 && index_ok && fsync(index_fd) != 0) index_ok = 0;
  if (index_fd >= 0 && close(index_fd) != 0) index_ok = 0;
  index_fd = -1;
  if (!index_ok) {
    set_error("The asset index could not be finalized");
    goto failed;
  }

  DiskEntry *test_entries = NULL;
  char *test_paths = NULL;
  size_t test_count = 0;
  int test_fd = -1;
  if (!load_pair(temp_pack, temp_index, &test_entries, &test_paths, &test_count, &test_fd))
    goto failed;
  if (!verify_pack_data(test_fd, test_entries, test_count, data_checksum)) {
    set_error("The written asset data did not pass verification");
    free_loaded(test_entries, test_paths, test_fd);
    goto failed;
  }
  free_loaded(test_entries, test_paths, test_fd);

  unlink(pack_path);
  unlink(index_path);
  if (rename(temp_pack, pack_path) != 0 || rename(temp_index, index_path) != 0 ||
      !asset_pack_open_existing(root)) {
    set_error("The validated asset pack could not be installed");
    goto failed;
  }
  free(buffer);
  free(disk_entries);
  free(paths);
  free_build_entries(items, count);
  return 1;

failed:
  if (source_fd >= 0) close(source_fd);
  if (pack_fd >= 0) close(pack_fd);
  if (index_fd >= 0) close(index_fd);
  unlink(temp_pack);
  unlink(temp_index);
  free(buffer);
  free(disk_entries);
  free(paths);
  free_build_entries(items, count);
  return 0;
}

int asset_pack_stat_relative(const char *path, uint64_t *size, uint64_t *ino) {
  size_t index;
  if (!find_relative(path, &index)) {
    return 0;
  }
  if (size) *size = g_entries[index].size;
  if (ino) *ino = 0x5353000000000000ULL | (uint64_t)(index + 1);
  return 1;
}

int asset_pack_stat_path(const char *path, uint64_t *size, uint64_t *ino) {
  char relative[768];
  if (!normalize_relative(path, relative, sizeof relative)) return 0;
  return asset_pack_stat_relative(relative, size, ino);
}

int asset_pack_stat_path_info(const char *path, uint64_t *size, uint64_t *ino,
                              int *directory) {
  char relative[768];
  size_t index;
  if (!normalize_asset_path(path, relative, sizeof relative, 1)) return 0;
  if (relative[0] && find_relative(relative, &index)) {
    if (size) *size = g_entries[index].size;
    if (ino) *ino = 0x5353000000000000ULL | (uint64_t)(index + 1);
    if (directory) *directory = 0;
    return 1;
  }
  if (find_directory_relative(relative, ino)) {
    if (size) *size = 0;
    if (directory) *directory = 1;
    return 1;
  }
  return 0;
}

static int add_handle(int fd, uint32_t entry, uint64_t position, uint64_t directory_ino) {
  mutexLock(&g_handle_lock);
  for (int i = 0; i < PACK_HANDLES; i++) {
    if (!g_handles[i].used) {
      g_handles[i].used = 1;
      g_handles[i].fd = fd;
      g_handles[i].entry = entry;
      g_handles[i].directory_ino = directory_ino;
      g_handles[i].position = position;
      g_handles[i].cache_offset = 0;
      g_handles[i].cache_size = 0;
      mutexUnlock(&g_handle_lock);
      return fd;
    }
  }
  mutexUnlock(&g_handle_lock);
  close(fd);
  errno = EMFILE;
  return -1;
}

int asset_pack_open_path(const char *path) {
  char relative[768];
  size_t index;
  uint64_t directory_ino = 0;
  if (!normalize_asset_path(path, relative, sizeof relative, 1)) return -1;
  int directory = !relative[0] || !find_relative(relative, &index);
  if (directory && !find_directory_relative(relative, &directory_ino)) {
    return -1;
  }
  int fd = open(g_pack_path, O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  uint32_t entry = directory ? PACK_DIRECTORY_ENTRY : (uint32_t)index;
  int result = add_handle(fd, entry, 0, directory_ino);
  return result;
}

static PackHandle *get_handle(int fd) {
  if (fd < 0) return NULL;
  for (int i = 0; i < PACK_HANDLES; i++)
    if (g_handles[i].used && g_handles[i].fd == fd) return &g_handles[i];
  return NULL;
}

int asset_pack_fd_is(int fd) {
  return get_handle(fd) != NULL;
}

int asset_pack_dup_fd(int fd) {
  PackHandle *source = get_handle(fd);
  if (!source) { errno = EBADF; return -1; }
  mutexLock(&source->lock);
  uint32_t entry = source->entry;
  uint64_t directory_ino = source->directory_ino;
  uint64_t position = source->position;
  mutexUnlock(&source->lock);
  int duplicate = open(g_pack_path, O_RDONLY);
  if (duplicate < 0) return -1;
  int result = add_handle(duplicate, entry, position, directory_ino);
  return result;
}

int asset_pack_dup2_fd(int fd, int target) {
  if (fd == target) return asset_pack_fd_is(fd) ? fd : -1;
  if (target == g_pack_fd) return asset_pack_dup_fd(fd);
  PackHandle *source = get_handle(fd);
  if (!source) { errno = EBADF; return -1; }
  mutexLock(&source->lock);
  uint32_t entry = source->entry;
  uint64_t directory_ino = source->directory_ino;
  uint64_t position = source->position;
  mutexUnlock(&source->lock);
  if (asset_pack_fd_is(target)) asset_pack_close_fd(target);
  int temporary = open(g_pack_path, O_RDONLY);
  if (temporary < 0) return -1;
  int duplicate = temporary == target ? temporary : dup2(temporary, target);
  if (temporary != target) close(temporary);
  if (duplicate < 0) return -1;
  int result = add_handle(duplicate, entry, position, directory_ino);
  return result;
}

static long pread_entry(uint32_t entry_index, void *buffer, size_t count, uint64_t offset) {
  if (entry_index >= g_entry_count) { errno = EBADF; return -1; }
  DiskEntry *entry = &g_entries[entry_index];
  if (offset >= entry->size) return 0;
  uint64_t available = entry->size - offset;
  if ((uint64_t)count > available) count = (size_t)available;
  mutexLock(&g_pack_io_lock);
  int ok = read_at(g_pack_fd, buffer, count, entry->offset + offset);
  mutexUnlock(&g_pack_io_lock);
  return ok ? (long)count : -1;
}

static long read_handle_at(PackHandle *handle, void *buffer, size_t count, uint64_t offset) {
  if (handle->entry == PACK_DIRECTORY_ENTRY) { errno = EISDIR; return -1; }
  DiskEntry *entry = &g_entries[handle->entry];
  if (offset >= entry->size) return 0;
  uint64_t available = entry->size - offset;
  if ((uint64_t)count > available) count = (size_t)available;
  return read_at(handle->fd, buffer, count, entry->offset + offset) ? (long)count : -1;
}

long asset_pack_read_fd(int fd, void *buffer, size_t count) {
  PackHandle *handle = get_handle(fd);
  if (!handle) { errno = EBADF; return -1; }
  mutexLock(&handle->lock);
  uint32_t entry = handle->entry;
  long result = 0;
  if (entry == PACK_DIRECTORY_ENTRY) {
    errno = EISDIR;
    result = -1;
  } else if (handle->position >= g_entries[entry].size) {
    result = 0;
  } else if (count > PACK_CACHE_SIZE / 2) {
    result = read_handle_at(handle, buffer, count, handle->position);
    if (result > 0) handle->position += (uint64_t)result;
  } else {
    size_t done = 0;
    while (done < count) {
      if (!handle->cache || handle->position < handle->cache_offset ||
          handle->position >= handle->cache_offset + handle->cache_size) {
        if (!handle->cache) handle->cache = malloc(PACK_CACHE_SIZE);
        if (!handle->cache) { result = done ? (long)done : -1; break; }
        handle->cache_offset = handle->position;
        handle->cache_size = 0;
        long got = read_handle_at(handle, handle->cache, PACK_CACHE_SIZE,
                                  handle->cache_offset);
        if (got <= 0) { result = done ? (long)done : got; break; }
        handle->cache_size = (size_t)got;
      }
      size_t inside = (size_t)(handle->position - handle->cache_offset);
      size_t available = handle->cache_size - inside;
      size_t take = count - done < available ? count - done : available;
      memcpy((char *)buffer + done, handle->cache + inside, take);
      handle->position += take;
      done += take;
      result = (long)done;
    }
  }
  mutexUnlock(&handle->lock);
  return result;
}

long asset_pack_pread_fd(int fd, void *buffer, size_t count, long offset) {
  PackHandle *handle = get_handle(fd);
  if (!handle || offset < 0) { errno = EINVAL; return -1; }
  mutexLock(&handle->lock);
  uint32_t entry = handle->entry;
  long result = entry == PACK_DIRECTORY_ENTRY ? (errno = EISDIR, -1) :
                read_handle_at(handle, buffer, count, (uint64_t)offset);
  mutexUnlock(&handle->lock);
  return result;
}

long asset_pack_lseek_fd(int fd, long offset, int whence) {
  PackHandle *handle = get_handle(fd);
  if (!handle) { errno = EBADF; return -1; }
  mutexLock(&handle->lock);
  if (handle->entry == PACK_DIRECTORY_ENTRY) {
    mutexUnlock(&handle->lock);
    errno = EISDIR;
    return -1;
  }
  int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ?
                 (int64_t)handle->position : whence == SEEK_END ?
                 (int64_t)g_entries[handle->entry].size : -1;
  if (base < 0 || (offset < 0 && base < -offset)) {
    mutexUnlock(&handle->lock);
    errno = EINVAL;
    return -1;
  }
  handle->position = (uint64_t)(base + offset);
  long result = (long)handle->position;
  mutexUnlock(&handle->lock);
  return result;
}

int asset_pack_fstat_fd(int fd, uint64_t *size, uint64_t *ino, int *directory) {
  PackHandle *handle = get_handle(fd);
  if (!handle) return 0;
  if (handle->entry == PACK_DIRECTORY_ENTRY) {
    if (size) *size = 0;
    if (ino) *ino = handle->directory_ino;
    if (directory) *directory = 1;
    return 1;
  }
  if (directory) *directory = 0;
  int result = asset_pack_stat_relative(g_paths + g_entries[handle->entry].path_offset,
                                        size, ino);
  return result;
}

int asset_pack_close_fd(int fd) {
  mutexLock(&g_handle_lock);
  PackHandle *handle = get_handle(fd);
  if (!handle) {
    mutexUnlock(&g_handle_lock);
    errno = EBADF;
    return -1;
  }
  mutexLock(&handle->lock);
  handle->used = 0;
  free(handle->cache);
  handle->cache = NULL;
  handle->cache_size = 0;
  handle->fd = -1;
  mutexUnlock(&handle->lock);
  mutexUnlock(&g_handle_lock);
  int result = close(fd);
  return result;
}

int asset_pack_read_all_relative(const char *path, void **data, size_t *size) {
  size_t index;
  if (!data || !size) return 0;
  if (!find_relative(path, &index)) {
    return 0;
  }
  if (g_entries[index].size > SIZE_MAX)
    return 0;
  size_t length = (size_t)g_entries[index].size;
  void *buffer = malloc(length ? length : 1);
  if (!buffer || (length && pread_entry((uint32_t)index, buffer, length, 0) != (long)length)) {
    free(buffer);
    return 0;
  }
  *data = buffer;
  *size = length;
  return 1;
}

int asset_pack_read_all_path(const char *path, void **data, size_t *size) {
  char relative[768];
  return normalize_relative(path, relative, sizeof relative) &&
         asset_pack_read_all_relative(relative, data, size);
}

size_t asset_pack_entry_count(void) {
  return g_entry_count;
}

const char *asset_pack_entry_path(size_t index) {
  return index < g_entry_count ? g_paths + g_entries[index].path_offset : NULL;
}

void *asset_pack_opendir_path(const char *path) {
  char relative[768];
  if (!normalize_asset_path(path, relative, sizeof relative, 1)) return NULL;
  size_t prefix = strlen(relative);
  int found = 0;
  for (size_t i = 0; i < g_entry_count; i++) {
    const char *entry = g_paths + g_entries[i].path_offset;
    if ((!prefix && entry[0]) ||
        (prefix && !strncmp(entry, relative, prefix) && entry[prefix] == '/')) {
      found = 1;
      break;
    }
  }
  if (!found) {
    return NULL;
  }
  PackDir *dir = calloc(1, sizeof(*dir));
  if (!dir) return NULL;
  dir->magic = PACK_DIR_MAGIC;
  snprintf(dir->prefix, sizeof dir->prefix, "%s", relative);
  return dir;
}

int asset_pack_dir_is(const void *dir) {
  return dir && ((const PackDir *)dir)->magic == PACK_DIR_MAGIC;
}

const char *asset_pack_readdir_path(void *opaque, uint8_t *type, uint64_t *ino) {
  PackDir *dir = opaque;
  if (!asset_pack_dir_is(dir)) return NULL;
  if (dir->dots < 2) {
    snprintf(dir->name, sizeof dir->name, "%s", dir->dots++ ? ".." : ".");
    if (type) *type = DT_DIR;
    if (ino) *ino = dir->dots;
    return dir->name;
  }
  size_t prefix = strlen(dir->prefix);
  while (dir->cursor < g_entry_count) {
    size_t index = dir->cursor++;
    const char *entry = g_paths + g_entries[index].path_offset;
    const char *tail = entry;
    if (prefix) {
      if (strncmp(entry, dir->prefix, prefix) || entry[prefix] != '/') continue;
      tail = entry + prefix + 1;
    }
    const char *slash = strchr(tail, '/');
    size_t length = slash ? (size_t)(slash - tail) : strlen(tail);
    if (!length || length >= sizeof dir->name) continue;
    if (strlen(dir->last) == length && !memcmp(dir->last, tail, length)) continue;
    memcpy(dir->name, tail, length);
    dir->name[length] = 0;
    snprintf(dir->last, sizeof dir->last, "%s", dir->name);
    if (type) *type = slash ? DT_DIR : DT_REG;
    if (ino) {
      uint64_t hash = fnv_bytes(1469598103934665603ULL, dir->name, length);
      *ino = hash ? hash : 1;
    }
    return dir->name;
  }
  return NULL;
}

int asset_pack_closedir_path(void *opaque) {
  PackDir *dir = opaque;
  if (!asset_pack_dir_is(dir)) return -1;
  dir->magic = 0;
  free(dir);
  return 0;
}
