/* call_once_shim.h -- see call_once_shim.c.
 *
 * Provides C11 call_once for Mesa's util_call_once. Defining it in an object
 * file overrides the library's archive member.
 *
 * MIT license; see LICENSE. */

#ifndef __CALL_ONCE_SHIM_H__
#define __CALL_ONCE_SHIM_H__

// How many distinct one-time initialisers actually ran. Logged after the
// Vulkan probe, because "0" would mean the override is not in effect.
void call_once_report(void);

#endif
