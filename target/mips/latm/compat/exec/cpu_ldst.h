#ifndef LATM_EXEC_CPU_LDST_H
#define LATM_EXEC_CPU_LDST_H
#include "qemu/osdep.h"
#include "cpu.h"

/* MIPS guest memory access.  The direct-mode implementation is identity
 * mapped; the system-mode implementation performs the R4000 TLB walk. */
uint8_t  cpu_ldub_mmuidx_ra(CPUMIPSState *env, uint64_t addr, int mmu_idx,
                            uintptr_t ra);
uint16_t cpu_lduw_mmuidx_ra(CPUMIPSState *env, uint64_t addr, int mmu_idx,
                            uintptr_t ra);
uint32_t cpu_ldl_mmuidx_ra(CPUMIPSState *env, uint64_t addr, int mmu_idx,
                           uintptr_t ra);
uint64_t cpu_ldq_mmuidx_ra(CPUMIPSState *env, uint64_t addr, int mmu_idx,
                           uintptr_t ra);
void cpu_stb_mmuidx_ra(CPUMIPSState *env, uint64_t addr, uint8_t v,
                       int mmu_idx, uintptr_t ra);
void cpu_stw_mmuidx_ra(CPUMIPSState *env, uint64_t addr, uint16_t v,
                       int mmu_idx, uintptr_t ra);
void cpu_stl_mmuidx_ra(CPUMIPSState *env, uint64_t addr, uint32_t v,
                       int mmu_idx, uintptr_t ra);
void cpu_stq_mmuidx_ra(CPUMIPSState *env, uint64_t addr, uint64_t v,
                       int mmu_idx, uintptr_t ra);

uint32_t cpu_ldl_code(CPUMIPSState *env, uint64_t addr);
uint64_t cpu_ldq_code(CPUMIPSState *env, uint64_t addr);
uint16_t cpu_lduw_code(CPUMIPSState *env, uint64_t addr);
uint8_t cpu_ldub_code(CPUMIPSState *env, uint64_t addr);

uint64_t cpu_mips_translate_address(CPUMIPSState *env, uint64_t address,
                                    MMUAccessType access_type, uintptr_t ra);

#endif
