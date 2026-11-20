#ifndef LATM_ENV_H
#define LATM_ENV_H

#include <stddef.h>
#include "cpu.h"

static inline int env_offset_gpr(int num)
{
    return (int)offsetof(CPUMIPSState, active_tc.gpr[num]);
}

static inline int env_offset_fpr(int num)
{
    return (int)offsetof(CPUMIPSState, active_fpu.fpr[num].d);
}

static inline int env_offset_pc(void)
{
    return (int)offsetof(CPUMIPSState, active_tc.PC);
}

static inline int env_offset_hflags(void)
{
    return (int)offsetof(CPUMIPSState, hflags);
}

static inline int env_offset_btarget(void)
{
    return (int)offsetof(CPUMIPSState, btarget);
}

#endif
