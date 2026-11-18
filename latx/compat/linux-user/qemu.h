#ifndef LATX_BARE_LINUX_USER_QEMU_H
#define LATX_BARE_LINUX_USER_QEMU_H
/* The user-mode host layer is not part of the bare build; runtime-trace.c only
 * needs the type vocabulary, not the linux-user implementation. */
#include "qemu/osdep.h"
#include "exec/exec-all.h"
typedef uint64_t abi_ulong;
typedef int64_t abi_long;
typedef uint64_t abi_ptr;
int page_get_flags(uint64_t address);
#endif
