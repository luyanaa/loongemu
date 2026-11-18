#ifndef LATX_BARE_QEMU_TIMER_H
#define LATX_BARE_QEMU_TIMER_H
/* Bare-metal stand-in: the helpers only carry timer pointers around and query
 * the virtual clock.  No timer wheel exists in this build. */
#include "qemu/osdep.h"
typedef struct QEMUTimerListGroup { int unused; } QEMUTimerListGroup;
typedef struct QEMUTimer { int unused; } QEMUTimer;
#define QEMU_CLOCK_VIRTUAL 0
#define QEMU_CLOCK_HOST    1
int64_t qemu_clock_get_ns(int clock);
#endif
