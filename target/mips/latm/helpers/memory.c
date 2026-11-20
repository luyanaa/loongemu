/*
 * LATM guest memory access.
 *
 * The direct mode used by the bare-metal tests is identity mapped.  A
 * system-mode embedder can install a translate callback; the R4000 TLB walk
 * lives in tlb.c and calls it through cpu_mips_translate_address().
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "internal.h"
#include "exec/cpu_ldst.h"

static uint64_t latm_translate(CPUMIPSState *env, uint64_t addr,
                               MMUAccessType access_type, uintptr_t ra)
{
    if (env->tlb && env->tlb->map_address) {
        hwaddr phys = 0;
        int prot = 0;
        int ret;

        ret = env->tlb->map_address(env, &phys, &prot, addr, access_type);
        if (ret != TLBRET_MATCH) {
            env->CP0_BadVAddr = addr;
            env->CP0_Cause = (env->CP0_Cause & ~(0x1f << 2)) |
                             ((access_type == MMU_INST_FETCH ? EXCP_TLBL :
                               access_type == MMU_DATA_STORE ? EXCP_TLBS :
                               EXCP_TLBL) << 2);
            env->CP0_EntryHi = addr;
            if (env->hflags & MIPS_HFLAG_BMASK) {
                env->CP0_Cause |= 1u << 31;
            }
            env->CP0_EPC = env->active_tc.PC;
            env->active_tc.PC = env->exception_base + 0x180;
            env->CP0_Status |= 1u << CP0St_EXL;
            cpu_loop_exit(NULL);
        }
        (void)prot;
        return phys;
    }
    (void)ra;
    return addr;
}

#define LD_FUNC(NAME, TYPE)                                                 \
TYPE NAME(CPUMIPSState *env, uint64_t addr, int mmu_idx, uintptr_t ra)      \
{                                                                            \
    uint64_t p = latm_translate(env, addr, MMU_DATA_LOAD, ra);               \
    (void)mmu_idx;                                                            \
    return *(const TYPE *)(uintptr_t)p;                                       \
}

LD_FUNC(cpu_ldub_mmuidx_ra, uint8_t)
LD_FUNC(cpu_lduw_mmuidx_ra, uint16_t)
LD_FUNC(cpu_ldl_mmuidx_ra, uint32_t)
LD_FUNC(cpu_ldq_mmuidx_ra, uint64_t)

#define ST_FUNC(NAME, TYPE)                                                 \
void NAME(CPUMIPSState *env, uint64_t addr, TYPE v, int mmu_idx,            \
          uintptr_t ra)                                                     \
{                                                                            \
    uint64_t p = latm_translate(env, addr, MMU_DATA_STORE, ra);              \
    (void)mmu_idx;                                                            \
    *(TYPE *)(uintptr_t)p = v;                                                \
}

ST_FUNC(cpu_stb_mmuidx_ra, uint8_t)
ST_FUNC(cpu_stw_mmuidx_ra, uint16_t)
ST_FUNC(cpu_stl_mmuidx_ra, uint32_t)
ST_FUNC(cpu_stq_mmuidx_ra, uint64_t)

uint32_t cpu_ldl_code(CPUMIPSState *env, uint64_t addr)
{
    return cpu_ldl_mmuidx_ra(env, addr, 0, 0);
}

uint64_t cpu_ldq_code(CPUMIPSState *env, uint64_t addr)
{
    return cpu_ldq_mmuidx_ra(env, addr, 0, 0);
}

uint16_t cpu_lduw_code(CPUMIPSState *env, uint64_t addr)
{
    return cpu_lduw_mmuidx_ra(env, addr, 0, 0);
}

uint8_t cpu_ldub_code(CPUMIPSState *env, uint64_t addr)
{
    return cpu_ldub_mmuidx_ra(env, addr, 0, 0);
}

uint64_t cpu_mips_translate_address(CPUMIPSState *env, uint64_t address,
                                    MMUAccessType access_type, uintptr_t ra)
{
    return latm_translate(env, address, access_type, ra);
}
