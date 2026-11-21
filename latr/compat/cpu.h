#ifndef LATR_COMPAT_CPU_H
#define LATR_COMPAT_CPU_H

#include "qemu/osdep.h"
#include "riscv-env.h"

typedef uint64_t target_ulong;
typedef int64_t target_long;
/* The stripped shared exec header still names this legacy type in a few
 * declarations; LATR never consumes the x86-specific fields. */
typedef CPUArchState CPUX86State;

typedef struct CPUState CPUState;
struct CPUState {
    CPUArchState *env_ptr;
};

#endif
