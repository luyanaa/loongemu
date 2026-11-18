#ifndef LATX_BARE_CPU_LDST_H
#define LATX_BARE_CPU_LDST_H
#include "exec/exec-all.h"
static inline uint8_t cpu_ldub_code(CPUArchState *env, uint64_t addr)
{
    (void)env;
    return latx_bare_ldub_code(addr);
}
uint8_t  cpu_ldub_data_ra(CPUArchState *env, uint64_t addr, uintptr_t ra);
uint16_t cpu_lduw_data_ra(CPUArchState *env, uint64_t addr, uintptr_t ra);
uint32_t cpu_ldl_data_ra(CPUArchState *env, uint64_t addr, uintptr_t ra);
uint64_t cpu_ldq_data_ra(CPUArchState *env, uint64_t addr, uintptr_t ra);
void cpu_stb_data_ra(CPUArchState *env, uint64_t addr, uint8_t v, uintptr_t ra);
void cpu_stw_data_ra(CPUArchState *env, uint64_t addr, uint16_t v, uintptr_t ra);
void cpu_stl_data_ra(CPUArchState *env, uint64_t addr, uint32_t v, uintptr_t ra);
void cpu_stq_data_ra(CPUArchState *env, uint64_t addr, uint64_t v, uintptr_t ra);
int qemu_guest_getrandom(void *buf, size_t len, void **errp);
#endif
