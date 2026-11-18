#ifndef LATX_BARE_QEMU_ATOMIC128_H
#define LATX_BARE_QEMU_ATOMIC128_H
/* Single-core bare-metal 128-bit atomics: plain accesses are already atomic
 * with respect to the only execution context this build supports. */
#include "qemu/osdep.h"
#include "qemu/int128.h"

static inline Int128 atomic16_read(Int128 *ptr) { return *ptr; }
static inline void atomic16_set(Int128 *ptr, Int128 val) { *ptr = val; }
static inline Int128 atomic16_cmpxchg(Int128 *ptr, Int128 oldv, Int128 newv)
{
    Int128 cur = *ptr;

    if (cur == oldv) {
        *ptr = newv;
    }
    return cur;
}
#endif
