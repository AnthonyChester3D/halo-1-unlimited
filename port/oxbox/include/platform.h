/*
PLATFORM.H (Xbox)

What the port units we reuse (halo_linker_common.c, msvc_wide.c) and our Xbox layer need from the Linux
platform header: the XDK declarations as the game sees them, and the layer's log. No pthreads, SDL or POSIX:
on the Xbox the retail XBE's own libraries are the SDK.
*/

#ifndef __OXBOX_PLATFORM_H
#define __OXBOX_PLATFORM_H

#include <stdlib.h>

#define DEBUG_KEYBOARD
#include "halo_linux_winsock_names.h"
#include <xtl.h>
#include <xbdm.h>
#include <d3d8perf.h>
#define HALO_LINUX_WINSOCK_NAMES_UNDEFINE
#include "halo_linux_winsock_names.h"
#undef HALO_LINUX_WINSOCK_NAMES_UNDEFINE
#include "oxbox_sdk_extra.h"

/* writes "oxbox: <message>" to the log file (oxbox_log.txt beside the XBE) */
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* reports an SDK entry point we do not provide, once per name */
void platform_unimplemented(const char *name);
#define PLATFORM_UNIMPLEMENTED() platform_unimplemented(__func__)

#endif
