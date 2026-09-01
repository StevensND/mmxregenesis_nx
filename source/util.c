/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <reent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

#if DEBUG_LOG

static int s_nxlinkSock = -1;
static FILE *s_log = NULL; // persistent log handle (fast; fflush per line)

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

// sdmc is mounted by the time userAppInit runs, so open the log once here
// instead of reopening it per line (the engine logs thousands of lines).
void userAppInit(void) {
  initNxLink();
  s_log = fopen(LOG_PATH, "w");
  if (!s_log) s_log = fopen(LOG_NAME, "w"); // fall back to the launch CWD
  if (s_log) {
    fputs("== mmx log open ==\n", s_log);
    fflush(s_log);
  }
}

void userAppExit(void) {
  if (s_log) { fclose(s_log); s_log = NULL; }
  deinitNxLink();
}

#endif

// Shared TLS block for the engine stack-protector guard at tpidr_el0 + 0x28.
static uint8_t s_tls_block[0x1000] __attribute__((aligned(16)));

// Point tpidr_el0 straight at the guard block with msr, WITHOUT armSetTlsRw's
// getThreadVars path, so it works even on a thread that has no libnx TLS at all
// (tpidr_el0 == 0 -- the engine's Vulkan render/worker threads). no_stack_protector
// so this function never reads the canary it is about to install.
__attribute__((no_stack_protector))
static void heal_stack_guard(void) {
  uintptr_t tp;
  __asm__ volatile("mrs %0, s3_3_c13_c0_2" : "=r"(tp)); // read tpidr_el0 (safe if 0)
  if (tp) return;
  *(volatile uint64_t *)(s_tls_block + 0x28) = 0x0123456789ABCDEFull;
  __asm__ volatile("msr s3_3_c13_c0_2, %0" : : "r"(s_tls_block)); // set tpidr_el0
}

__attribute__((no_stack_protector))
int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  va_list list;
  heal_stack_guard(); // engine render/worker threads never ran tls_setup_guard()

  if (s_log) {
    // Use the GLOBAL reent explicitly, not the per-thread _REENT. Threads the
    // engine/driver spawn outside libnx have no per-thread reent, so plain
    // vfprintf/fflush (which resolve _REENT via __getreent) fault. The _r forms
    // with _GLOBAL_REENT touch only the always-valid global reent.
    va_start(list, text);
    _vfprintf_r(_GLOBAL_REENT, s_log, text, list);
    va_end(list);
    _fflush_r(_GLOBAL_REENT, s_log); // flush each line so a crash leaves a full log
  }
  // NOTE: no vprintf() to stdout here. On engine render/worker threads newlib's
  // per-thread _REENT is not set up (no libnx TLS), so _REENT->_stdout is NULL
  // and vprintf faults (NULL FILE* deref). The on-screen console is invisible on
  // Switch anyway; the log file (vfprintf above) is the only sink that matters.
#endif
  return 0;
}

void tls_setup_guard(void) {
  *(uint64_t *)(s_tls_block + 0x28) = 0x0123456789ABCDEFull;
  armSetTlsRw(s_tls_block);
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
