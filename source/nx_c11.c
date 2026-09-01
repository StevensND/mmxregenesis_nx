/* nx_c11.c -- C11 mtx_* and cnd_*, backed by libnx.
 *
 * WHY THIS EXISTS
 * ===============
 * c11_probe.c asks, at startup, whether the platform's C11 threading actually
 * works. On hardware it answers:
 *
 *     [c11] mtx_init FAILED -- Mesa checks for this by name
 *     [c11] mtx_init FAILED (cnd test skipped)
 *     [c11] 2 C11 threading problem(s).
 *
 * `mtx_init` does not work. Mesa's `nvkmd-switch` builds its channel
 * management on these primitives, and the run that produced that line ended
 * with:
 *
 *     nvkmd-switch: channel 2 lost: notification={type=8 info=0 status=65535}
 *     nvkmd-switch: channel submit failed: device lost (VK_ERROR_DEVICE_LOST)
 *     ERROR: Vulkan device was lost.
 *     [BRK #1]                       <- Godot's CRASH_NOW
 *
 * A driver serialising GPFIFO submissions with a mutex that failed to
 * initialise is a coherent explanation for a lost channel, and it is the only
 * one currently supported by evidence rather than inference.
 *
 * Defining these here overrides the library's: object files take precedence
 * over archive members, and --allow-multiple-definition is already set.
 *
 * ===========================================================================
 * WHY AN INDEX AND NOT THE OBJECT ITSELF
 * ===========================================================================
 * The natural implementation stores a libnx Mutex inside the caller's mtx_t.
 * That requires sizeof(mtx_t) to be large enough, and nothing here can check
 * that without a build to find out -- a round trip to hardware to learn a
 * number.
 *
 * Storing a 4-byte INDEX needs only sizeof(mtx_t) >= 4, which any conceivable
 * mtx_t satisfies. The payload lives in a static table. The cost is a bounded
 * pool; the benefit is that no assumption about a type this file cannot see
 * has to be right.
 *
 * MIT license; see LICENSE. */

#include <stdint.h>
#include <stdlib.h>   // calloc, for pool growth
#include <string.h>
#include <threads.h>
#include <time.h>
#include <switch.h>
extern void tls_setup_guard(void);  // install bionic stack-guard block

/* clock_gettime(CLOCK_REALTIME), NOT timespec_get.
 *
 * timespec_get is C11 and devkitPro does not provide it -- it failed at LINK
 * time, which is a pointed way to be reminded that this file exists precisely
 * because this platform's C11 support is incomplete. Reaching for another C11
 * function to implement it was the wrong instinct.
 *
 * clock_gettime is used in four other files here, including c11_probe.c, so it
 * is proven on this target rather than assumed.
 *
 * Matching the probe matters beyond availability: c11_probe.c builds its
 * cnd_timedwait deadline with clock_gettime(CLOCK_REALTIME), and a deadline is
 * only meaningful against the clock it was measured on. Reading a different
 * one here would make every timeout wrong by the offset between them. */

extern void debugPrintf(const char *fmt, ...);

/* ===========================================================================
 * RETURN VALUES: Mesa's numbers, not this platform's symbols.
 * ===========================================================================
 *
 * C11 does not fix the values of thrd_success/thrd_error -- they are an
 * implementation's own enum. Which means the caller's <threads.h> and the
 * implementation's must agree, and here they do not.
 *
 * Mesa was compiled against its own bundled c11/threads.h, where success is 0.
 * Disassembling libvulkan.a's vk_instance.c.o shows `cbnz w0` immediately
 * after each of its three mtx_init calls: any non-zero return is failure.
 *
 * The first version of this file returned devkitPro's `thrd_success`. That is
 * evidently not 0, so every mtx_init Mesa made "failed":
 *
 *     ../src/vulkan/runtime/vk_instance.c:187  VK_ERROR_INITIALIZATION_FAILED
 *     [vk] vkCreateInstance -> -3
 *
 * Godot then fell back to GLES3 -- but main() had already skipped egl_setup()
 * on the strength of the Vulkan probe, so GLES3 ran with no context, the
 * texture atlas failed with status 0, and Godot memcpy'd into the null result:
 *
 *     stp x2, x3, [x5, #-0x40]     far = 0x40
 *
 * A boot crash three layers away from the actual mistake.
 *
 * Note what made this hard to see: c11_probe.c compares against devkitPro's
 * `thrd_success` too, so it was self-consistent and reported
 * "mtx/cnd/call_once behave correctly" while Mesa was being told the opposite.
 * Two components agreeing with each other is not the same as either being
 * right.
 * ========================================================================= */
#define NX_THRD_SUCCESS   0
#define NX_THRD_BUSY      1
#define NX_THRD_ERROR     2
#define NX_THRD_NOMEM     3
#define NX_THRD_TIMEDOUT  4

// A GROWABLE pool, in chunks.
//
// The first version was a flat 1024 entries, on the reasoning that Mesa
// creates these "per-instance, per-device and per-channel: tens, not
// thousands". That estimate was wrong, and hardware said so precisely:
//
//     [c11] OUT OF SLOTS (1024) -- returning thrd_error
//     nvkmd-switch: mtx_init failed (VK_ERROR_UNKNOWN)
//     ERROR: swap_chain_acquire_framebuffer -> err != VK_SUCCESS   x904
//
// Exhaustion is not a soft failure here: a driver that cannot make a mutex
// stops acquiring swapchain images, and the screen goes black while audio and
// logic carry on. Which is exactly what it looked like.
//
// Chunks are allocated on demand, so the common case costs the first chunk and
// nothing more, and there is no number to guess. The ceiling is high enough
// that reaching it means something is leaking mutexes rather than using them.
#define NXC11_CHUNK   1024u
#define NXC11_CHUNKS  64u                       // 65536 slots
#define NXC11_SLOTS   (NXC11_CHUNK * NXC11_CHUNKS)

typedef struct {
  Mutex   m;
  CondVar c;
  uint32_t type;        // mtx_plain / mtx_recursive / mtx_timed
  uint32_t owner;       // recursive: owning thread's TLS tag, 0 = free
  uint32_t depth;       // recursive: lock count
  uint32_t in_use;
} nxc11_slot;

static nxc11_slot *g_chunk[NXC11_CHUNKS];   // allocated on demand
static uint32_t    g_next;
static uint32_t    g_high;                  // highest chunk in use
static Mutex       g_pool_lk;

// Index -> slot. NULL when the chunk has not been allocated.
static inline nxc11_slot *slot_at(uint32_t i) {
  if (i >= NXC11_SLOTS) return NULL;
  nxc11_slot *c = g_chunk[i / NXC11_CHUNK];
  return c ? &c[i % NXC11_CHUNK] : NULL;
}

uint64_t sts2_c11_mtx_live, sts2_c11_cnd_live;

// Index+1 is stored, so a zeroed mtx_t (0) is recognisably "not initialised"
// rather than slot 0.
static inline nxc11_slot *slot_of(const void *handle) {
  uint32_t i;
  memcpy(&i, handle, sizeof(i));
  if (i == 0 || i > NXC11_SLOTS) return NULL;
  return slot_at(i - 1);
}

static int slot_new(void *handle, uint32_t type) {
  mutexLock(&g_pool_lk);

  uint32_t found = 0;

  // Reuse a free slot in an already-allocated chunk first.
  const uint32_t live = (g_high + 1) * NXC11_CHUNK;
  for (uint32_t k = 0; k < live; k++) {
    const uint32_t i = (g_next + k) % live;
    nxc11_slot *sl = slot_at(i);
    if (sl && !sl->in_use) {
      memset(sl, 0, sizeof(*sl));
      sl->in_use = 1;
      sl->type = type;
      g_next = (i + 1) % live;
      found = i + 1;
      break;
    }
  }

  // Otherwise grow.
  if (!found) {
    for (uint32_t c = 0; c < NXC11_CHUNKS; c++) {
      if (g_chunk[c]) continue;
      // calloc, not malloc: in_use must read 0 across the whole chunk before
      // any of it is visible to the scan above.
      //
      // Called with g_pool_lk held, which is safe only because nothing in the
      // allocator path calls back into mtx_init -- newlib's malloc locks with
      // __malloc_lock, not C11, and Mesa's vk_alloc goes to the app allocator.
      // If that ever stops being true this deadlocks, so it is worth stating
      // rather than leaving as an unexamined assumption.
      g_chunk[c] = (nxc11_slot *)calloc(NXC11_CHUNK, sizeof(nxc11_slot));
      if (!g_chunk[c]) break;
      if (c > g_high) g_high = c;
      const uint32_t i = c * NXC11_CHUNK;
      g_chunk[c][0].in_use = 1;
      g_chunk[c][0].type = type;
      g_next = i + 1;
      found = i + 1;
      debugPrintf("[c11] pool grew to %u slots\n",
                  (unsigned)((g_high + 1) * NXC11_CHUNK));
      break;
    }
  }
  mutexUnlock(&g_pool_lk);

  if (!found) {
    // Loud, not silent. A C11 mutex that reports success without locking is
    // exactly the failure this file exists to fix, so exhaustion must never
    // look like success -- but note what exhaustion COSTS: Mesa stops being
    // able to acquire swapchain images and the screen goes black.
    debugPrintf("[c11] OUT OF SLOTS (%u max) -- returning error rather than a\n"
                "[c11] mutex that does not lock. Something is leaking mutexes.\n",
                (unsigned)NXC11_SLOTS);
    return NX_THRD_ERROR;
  }

  memcpy(handle, &found, sizeof(found));
  return NX_THRD_SUCCESS;
}

// ---------------------------------------------------------------------------
// mtx_*
// ---------------------------------------------------------------------------

int mtx_init(mtx_t *mtx, int type) {
  if (!mtx) return NX_THRD_ERROR;
  const int r = slot_new(mtx, (uint32_t)type);
  if (r == NX_THRD_SUCCESS) sts2_c11_mtx_live++;
  return r;
}

void mtx_destroy(mtx_t *mtx) {
  nxc11_slot *s = mtx ? slot_of(mtx) : NULL;
  if (!s) return;
  mutexLock(&g_pool_lk);
  s->in_use = 0;
  mutexUnlock(&g_pool_lk);
  if (sts2_c11_mtx_live) sts2_c11_mtx_live--;
  const uint32_t zero = 0;
  memcpy(mtx, &zero, sizeof(zero));
}

int mtx_lock(mtx_t *mtx) {
  nxc11_slot *s = mtx ? slot_of(mtx) : NULL;
  if (!s) return NX_THRD_ERROR;

  if (s->type & mtx_recursive) {
    const uint32_t me = (uint32_t)((uintptr_t)armGetTls() >> 8);
    if (s->owner == me) { s->depth++; return NX_THRD_SUCCESS; }
    mutexLock(&s->m);
    s->owner = me;
    s->depth = 1;
    return NX_THRD_SUCCESS;
  }

  mutexLock(&s->m);
  return NX_THRD_SUCCESS;
}

int mtx_trylock(mtx_t *mtx) {
  nxc11_slot *s = mtx ? slot_of(mtx) : NULL;
  if (!s) return NX_THRD_ERROR;

  if (s->type & mtx_recursive) {
    const uint32_t me = (uint32_t)((uintptr_t)armGetTls() >> 8);
    if (s->owner == me) { s->depth++; return NX_THRD_SUCCESS; }
    if (!mutexTryLock(&s->m)) return NX_THRD_BUSY;
    s->owner = me;
    s->depth = 1;
    return NX_THRD_SUCCESS;
  }

  return mutexTryLock(&s->m) ? NX_THRD_SUCCESS : NX_THRD_BUSY;
}

int mtx_unlock(mtx_t *mtx) {
  nxc11_slot *s = mtx ? slot_of(mtx) : NULL;
  if (!s) return NX_THRD_ERROR;

  if (s->type & mtx_recursive) {
    if (s->depth > 1) { s->depth--; return NX_THRD_SUCCESS; }
    s->depth = 0;
    s->owner = 0;
  }
  mutexUnlock(&s->m);
  return NX_THRD_SUCCESS;
}

int mtx_timedlock(mtx_t *mtx, const struct timespec *ts) {
  // libnx has no timed mutex acquire. Polling with a short sleep is honest
  // about what it is: a real wait that respects the deadline, rather than
  // either blocking forever (ignoring the timeout) or returning immediately
  // (turning every timed lock into a spin at the caller).
  if (!mtx) return NX_THRD_ERROR;
  if (!ts) return mtx_lock(mtx);

  for (;;) {
    const int r = mtx_trylock(mtx);
    if (r != NX_THRD_BUSY) return r;

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return NX_THRD_ERROR;
    if (now.tv_sec > ts->tv_sec ||
        (now.tv_sec == ts->tv_sec && now.tv_nsec >= ts->tv_nsec))
      return NX_THRD_TIMEDOUT;

    svcSleepThread(100000ULL);   // 0.1 ms
  }
}

// ---------------------------------------------------------------------------
// cnd_*
//
// The CondVar lives in the same slot as its own index, so cnd_t needs the same
// four bytes and nothing more.
// ---------------------------------------------------------------------------

int cnd_init(cnd_t *cnd) {
  if (!cnd) return NX_THRD_ERROR;
  const int r = slot_new(cnd, mtx_plain);
  if (r == NX_THRD_SUCCESS) {
    nxc11_slot *s = slot_of(cnd);
    if (s) condvarInit(&s->c);
    sts2_c11_cnd_live++;
  }
  return r;
}

void cnd_destroy(cnd_t *cnd) {
  nxc11_slot *s = cnd ? slot_of(cnd) : NULL;
  if (!s) return;
  mutexLock(&g_pool_lk);
  s->in_use = 0;
  mutexUnlock(&g_pool_lk);
  if (sts2_c11_cnd_live) sts2_c11_cnd_live--;
  const uint32_t zero = 0;
  memcpy(cnd, &zero, sizeof(zero));
}

int cnd_signal(cnd_t *cnd) {
  nxc11_slot *s = cnd ? slot_of(cnd) : NULL;
  if (!s) return NX_THRD_ERROR;
  condvarWakeOne(&s->c);
  return NX_THRD_SUCCESS;
}

int cnd_broadcast(cnd_t *cnd) {
  nxc11_slot *s = cnd ? slot_of(cnd) : NULL;
  if (!s) return NX_THRD_ERROR;
  condvarWakeAll(&s->c);
  return NX_THRD_SUCCESS;
}

int cnd_wait(cnd_t *cnd, mtx_t *mtx) {
  nxc11_slot *c = cnd ? slot_of(cnd) : NULL;
  nxc11_slot *m = mtx ? slot_of(mtx) : NULL;
  if (!c || !m) return NX_THRD_ERROR;
  // condvarWait takes the libnx Mutex directly, which is why the mutex slot
  // stores a plain Mutex rather than an RMutex: an RMutex could not be handed
  // to it, and a condvar waiting on a different object than the caller locked
  // is a lost wakeup.
  // Return value deliberately ignored, matching audio.c. libnx's condvarWait
  // is believed to return Result, but wrapping it in R_SUCCEEDED would fail to
  // compile if it returns void -- and an untimed wait has no failure this
  // caller could act on anyway.
  condvarWait(&c->c, &m->m);
  return NX_THRD_SUCCESS;
}

int cnd_timedwait(cnd_t *cnd, mtx_t *mtx, const struct timespec *ts) {
  nxc11_slot *c = cnd ? slot_of(cnd) : NULL;
  nxc11_slot *m = mtx ? slot_of(mtx) : NULL;
  if (!c || !m) return NX_THRD_ERROR;
  if (!ts) return cnd_wait(cnd, mtx);

  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return NX_THRD_ERROR;

  int64_t ns = (int64_t)(ts->tv_sec - now.tv_sec) * 1000000000LL +
               ((int64_t)ts->tv_nsec - (int64_t)now.tv_nsec);
  if (ns < 0) ns = 0;

  const Result rc = condvarWaitTimeout(&c->c, &m->m, (u64)ns);
  if (R_SUCCEEDED(rc)) return NX_THRD_SUCCESS;

  // A timeout is NOT an error. Mesa uses cnd_timedwait to poll for fence
  // completion, so reporting thrd_error on expiry would turn every ordinary
  // wait into a failure path.
  return NX_THRD_TIMEDOUT;
}

void sts2_c11_report(void) {
  // Say so if the platform header disagrees with the numbers we return.
  //
  // This is the bug that produced a boot crash: nothing anywhere reported that
  // Mesa and devkitPro had different ideas of what "success" is. Now the log
  // does, whether or not anything is currently going wrong with it.
  if ((int)thrd_success != NX_THRD_SUCCESS)
    debugPrintf("[c11] NOTE: platform thrd_success=%d, we return %d (Mesa's).\n"
                "[c11] Anything compiled against <threads.h> here will disagree\n"
                "[c11] with Mesa about success -- c11_probe.c included.\n",
                (int)thrd_success, NX_THRD_SUCCESS);

  debugPrintf("[c11] shim active: %llu mutex(es), %llu condvar(s) live\n",
              (unsigned long long)sts2_c11_mtx_live,
              (unsigned long long)sts2_c11_cnd_live);
}

// ---------------------------------------------------------------------------
// C11 thread creation, with the bionic stack-guard block installed.
//
// Mesa's util_queue spins up worker threads via thrd_create (disk-cache writer,
// etc.). Those workers run engine/bionic code -- the game's zlib deflate through
// util_compress_deflate, for one -- which reads its stack canary from
// tpidr_el0+0x28. devkitPro's own thrd_create sets up no such block, so the
// worker faults (FAR=0x28, tp=0) on the first stack-protected call. Provide our
// own on top of pthread (which gets a real libnx TLS) and run tls_setup_guard in
// the entry trampoline, exactly like the pthread path in imports.c. Overrides
// the library's via object-file precedence + --allow-multiple-definition.
// Returns Mesa's numbers (0 = success), matching the mtx/cnd shims above.

// Mesa ships its OWN thrd_create (threads_posix.c.o) that wins the link over a
// plain override, and it creates the worker with no bionic stack-guard block.
// So intercept the call itself with --wrap=thrd_create: wrap the entry to run
// tls_setup_guard first, then hand it to Mesa's real thrd_create. This catches
// every C11 thread Mesa starts regardless of which thrd_create the linker keeps.
extern int __real_thrd_create(thrd_t *thr, thrd_start_t func, void *arg);

typedef struct { thrd_start_t func; void *arg; } nxc11_thrd_start;

static int nxc11_thrd_tramp(void *p) {
  nxc11_thrd_start ts = *(nxc11_thrd_start *)p;
  free(p);
  tls_setup_guard();
  return ts.func(ts.arg);
}

int __wrap_thrd_create(thrd_t *thr, thrd_start_t func, void *arg) {
  nxc11_thrd_start *ts = malloc(sizeof(*ts));
  if (!ts) return __real_thrd_create(thr, func, arg); // unguarded fallback (rare)
  ts->func = func;
  ts->arg = arg;
  return __real_thrd_create(thr, nxc11_thrd_tramp, ts);
}
