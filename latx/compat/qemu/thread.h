#ifndef LATX_BARE_QEMU_THREAD_H
#define LATX_BARE_QEMU_THREAD_H
/* Single-core bare build: locking primitives collapse to plain structures. */
#include "qemu/osdep.h"
typedef struct QemuCond { int unused; } QemuCond;
typedef struct QemuThread { int unused; } QemuThread;
typedef struct QemuEvent { int value; } QemuEvent;
static inline void qemu_mutex_lock(QemuMutex *m) { (void)m; }
static inline void qemu_mutex_unlock(QemuMutex *m) { (void)m; }
static inline void qemu_cond_wait(QemuCond *c, QemuMutex *m) { (void)c; (void)m; }
#endif
