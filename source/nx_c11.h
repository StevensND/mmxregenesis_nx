/* nx_c11.h -- see nx_c11.c.
 *
 * Working C11 mtx_ and cnd_ primitives backed by libnx, because c11_probe.c reports
 * `mtx_init FAILED` on this platform and Mesa's nvkmd-switch builds its
 * channel management on them.
 *
 * MIT license; see LICENSE. */

#ifndef __NX_C11_H__
#define __NX_C11_H__

/* One line: how many C11 mutexes and condvars are live. Zero after Mesa has
 * initialised means the shim is NOT in effect and the platform's broken
 * versions are still being used. */
void sts2_c11_report(void);

#endif
