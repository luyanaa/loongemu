#ifndef LATX_BARE_QEMU_MAIN_LOOP_H
#define LATX_BARE_QEMU_MAIN_LOOP_H
/* Single-core bare build: the iothread lock is a no-op. */
#include "qemu/osdep.h"
static inline void qemu_mutex_lock_iothread(void) { }
static inline void qemu_mutex_unlock_iothread(void) { }
#endif
