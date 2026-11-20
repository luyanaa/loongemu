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
#include "exec/exec-all.h"
#include "exec/page-protection.h"
#include "exec/helper-proto.h"
#include "latm-helpers.h"
#include "latm-platform.h"
int latm_last_tlb_error;

/* Segment translation for the LATM system-mode memory path.  This is the
 * MIPS64R2 subset of QEMU's target/mips/sysemu/physaddr.c. */
static int is_seg_am_mapped(unsigned int am, bool eu, int mmu_idx)
{
    int32_t adetlb_mask;

    switch (mmu_idx) {
    case 3: /* ERL */
        if (eu) {
            return 0;
        }
        /* fall through */
    case MIPS_HFLAG_KM:
        adetlb_mask = 0x70000000;
        goto check_tlb;
    case MIPS_HFLAG_SM:
        adetlb_mask = 0xc0380000;
        goto check_ade;
    case MIPS_HFLAG_UM:
        adetlb_mask = 0xe4180000;
        /* fall through */
    check_ade:
        if ((adetlb_mask << am) < 0) {
            return TLBRET_BADADDR;
        }
        adetlb_mask <<= 8;
        /* fall through */
    check_tlb:
        return ((adetlb_mask << am) < 0);
    default:
        return TLBRET_BADADDR;
    }
}

static int get_seg_physical_address(CPUMIPSState *env, hwaddr *physical,
                                    int *prot, target_ulong real_address,
                                    MMUAccessType access_type, int mmu_idx,
                                    unsigned int am, bool eu,
                                    target_ulong segmask,
                                    hwaddr physical_base)
{
    int mapped = is_seg_am_mapped(am, eu, mmu_idx);

    if (mapped < 0) {
        return mapped;
    } else if (mapped) {
        if (!env->tlb || !env->tlb->map_address) {
            return TLBRET_NOMATCH;
        }
        return env->tlb->map_address(env, physical, prot, real_address,
                                     access_type);
    } else {
        *physical = physical_base | (real_address & segmask);
        *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        return TLBRET_MATCH;
    }
}

static int get_segctl_physical_address(CPUMIPSState *env, hwaddr *physical,
                                       int *prot, target_ulong real_address,
                                       MMUAccessType access_type, int mmu_idx,
                                       uint16_t segctl, target_ulong segmask)
{
    unsigned int am = (segctl & CP0SC_AM_MASK) >> CP0SC_AM;
    bool eu = (segctl >> CP0SC_EU) & 1;
    hwaddr pa = ((hwaddr)segctl & CP0SC_PA_MASK) << 20;

    return get_seg_physical_address(env, physical, prot, real_address,
                                    access_type, mmu_idx, am, eu, segmask,
                                    pa & ~(hwaddr)segmask);
}

int get_physical_address(CPUMIPSState *env, hwaddr *physical,
                         int *prot, target_ulong real_address,
                         MMUAccessType access_type, int mmu_idx)
{
#if defined(TARGET_MIPS64)
    int user_mode = mmu_idx == MIPS_HFLAG_UM;
    int supervisor_mode = mmu_idx == MIPS_HFLAG_SM;
    int kernel_mode = !user_mode && !supervisor_mode;
    int UX = (env->CP0_Status & (1 << CP0St_UX)) != 0;
    int SX = (env->CP0_Status & (1 << CP0St_SX)) != 0;
    int KX = (env->CP0_Status & (1 << CP0St_KX)) != 0;
#endif
    int ret = TLBRET_MATCH;
    target_ulong address = real_address;

    if (address <= USEG_LIMIT) {
        uint16_t segctl = address >= 0x40000000UL ?
                          env->CP0_SegCtl2 : env->CP0_SegCtl2 >> 16;
        ret = get_segctl_physical_address(env, physical, prot, real_address,
                                          access_type, mmu_idx, segctl,
                                          0x3FFFFFFF);
#if defined(TARGET_MIPS64)
    } else if (address < 0x4000000000000000ULL) {
        if (UX && address <= (0x3FFFFFFFFFFFFFFFULL & env->SEGMask)) {
            ret = get_seg_physical_address(env, physical, prot, real_address,
                                           access_type, mmu_idx, CP0SC_AM_UK,
                                           false, env->SEGMask, 0);
        } else {
            ret = TLBRET_BADADDR;
        }
    } else if (address < 0x8000000000000000ULL) {
        if ((supervisor_mode || kernel_mode) &&
            SX && address <= (0x7FFFFFFFFFFFFFFFULL & env->SEGMask)) {
            ret = get_seg_physical_address(env, physical, prot, real_address,
                                           access_type, mmu_idx, CP0SC_AM_UK,
                                           false, env->SEGMask, 0);
        } else {
            ret = TLBRET_BADADDR;
        }
    } else if (address < 0xC000000000000000ULL) {
        if ((address & 0x07FFFFFFFFFFFFFFULL) <= env->PAMask) {
            unsigned int am = CP0SC_AM_UK;
            unsigned int xr = (env->CP0_SegCtl2 & CP0SC2_XR_MASK) >> CP0SC2_XR;
            static const uint8_t am_ksux[8] = {
                [CP0SC_AM_UK] = (1u << CP0St_KX),
                [CP0SC_AM_MK] = (1u << CP0St_KX),
                [CP0SC_AM_MSK] = (1u << CP0St_SX),
                [CP0SC_AM_MUSK] = (1u << CP0St_UX),
                [CP0SC_AM_MUSUK] = (1u << CP0St_UX),
                [CP0SC_AM_USK] = (1u << CP0St_SX),
                [6] = (1u << CP0St_KX),
                [CP0SC_AM_UUSK] = (1u << CP0St_UX),
            };
            if (xr & (1 << ((address >> 59) & 0x7))) {
                am = (env->CP0_SegCtl1 & CP0SC1_XAM_MASK) >> CP0SC1_XAM;
            }
            if (env->CP0_Status & am_ksux[am]) {
                ret = get_seg_physical_address(env, physical, prot,
                                               real_address, access_type,
                                               mmu_idx, am, false,
                                               env->PAMask, 0);
            } else {
                ret = TLBRET_BADADDR;
            }
        } else {
            ret = TLBRET_BADADDR;
        }
    } else if (address < 0xFFFFFFFF80000000ULL) {
        if (kernel_mode && KX &&
            address <= (0xFFFFFFFF7FFFFFFFULL & env->SEGMask)) {
            ret = get_seg_physical_address(env, physical, prot, real_address,
                                           access_type, mmu_idx, CP0SC_AM_UK,
                                           false, env->SEGMask, 0);
        } else {
            ret = TLBRET_BADADDR;
        }
#endif
    } else if (address < KSEG1_BASE) {
        ret = get_segctl_physical_address(env, physical, prot, real_address,
                                          access_type, mmu_idx,
                                          env->CP0_SegCtl1 >> 16, 0x1FFFFFFF);
    } else if (address < KSEG2_BASE) {
        ret = get_segctl_physical_address(env, physical, prot, real_address,
                                          access_type, mmu_idx,
                                          env->CP0_SegCtl1, 0x1FFFFFFF);
    } else if (address < KSEG3_BASE) {
        ret = get_segctl_physical_address(env, physical, prot, real_address,
                                          access_type, mmu_idx,
                                          env->CP0_SegCtl0 >> 16, 0x1FFFFFFF);
    } else {
        ret = get_segctl_physical_address(env, physical, prot, real_address,
                                          access_type, mmu_idx,
                                          env->CP0_SegCtl0, 0x1FFFFFFF);
    }
    return ret;
}

static void latm_raise_mmu_exception(CPUMIPSState *env, uint64_t address,
                                     MMUAccessType access_type, int tlb_error,
                                     uintptr_t ra)
{
    int exception;
    int error_code = access_type == MMU_INST_FETCH ? EXCP_INST_NOTAVAIL : 0;

    latm_last_tlb_error = tlb_error;
    switch (tlb_error) {
    default:
    case TLBRET_BADADDR:
        exception = access_type == MMU_DATA_STORE ? EXCP_AdES : EXCP_AdEL;
        break;
    case TLBRET_NOMATCH:
        exception = access_type == MMU_DATA_STORE ? EXCP_TLBS : EXCP_TLBL;
        error_code |= EXCP_TLB_NOMATCH;
        break;
    case TLBRET_INVALID:
        exception = access_type == MMU_DATA_STORE ? EXCP_TLBS : EXCP_TLBL;
        break;
    case TLBRET_DIRTY:
        exception = EXCP_LTLBL;
        break;
    case TLBRET_XI:
        exception = (env->CP0_PageGrain & (1 << CP0PG_IEC)) ?
                    EXCP_TLBXI : EXCP_TLBL;
        break;
    case TLBRET_RI:
        exception = (env->CP0_PageGrain & (1 << CP0PG_IEC)) ?
                    EXCP_TLBRI : EXCP_TLBL;
        break;
    }
    if (!(env->hflags & MIPS_HFLAG_DM)) {
        env->CP0_BadVAddr = address;
    }
    env->CP0_Context = (env->CP0_Context & ~0x007fffff) |
                       ((address >> 9) & 0x007ffff0);
    env->CP0_EntryHi = (env->CP0_EntryHi & env->CP0_EntryHi_ASID_mask) |
                       (env->CP0_EntryHi & (1U << CP0EnHi_EHINV)) |
                       (address & (TARGET_PAGE_MASK << 1));
    env->CP0_EntryHi &= env->SEGMask;
    env->CP0_XContext = (env->CP0_XContext & ((~0ULL) << (env->SEGBITS - 7))) |
                        (((address >> 62) & 3) << (env->SEGBITS - 9)) |
                        (((address >> 13) & ((1ULL << (env->SEGBITS - 13)) - 1))
                         << 4);
    do_raise_exception_err(env, exception, error_code, ra);
}

static uint64_t latm_translate(CPUMIPSState *env, uint64_t addr,
                               MMUAccessType access_type, uintptr_t ra)
{
    hwaddr phys = 0;
    int prot = 0;
    int ret;
    int mmu_idx;

    if (!env->tlb || !env->tlb->map_address) {
        (void)ra;
        return addr;
    }
    mmu_idx = (env->hflags & MIPS_HFLAG_UM) ? MIPS_HFLAG_UM :
              (env->hflags & MIPS_HFLAG_SM) ? MIPS_HFLAG_SM : MIPS_HFLAG_KM;
    ret = get_physical_address(env, &phys, &prot, addr, access_type, mmu_idx);
    if (ret == TLBRET_MATCH) {
        if (access_type == MMU_DATA_STORE && !(prot & PAGE_WRITE)) {
            ret = TLBRET_DIRTY;
        } else if (access_type == MMU_DATA_LOAD && !(prot & PAGE_READ)) {
            ret = TLBRET_RI;
        } else if (access_type == MMU_INST_FETCH && !(prot & PAGE_EXEC)) {
            ret = TLBRET_XI;
        } else {
            return phys;
        }
    }
    latm_raise_mmu_exception(env, addr, access_type, ret, ra);
    return 0;
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
    const LatmPlatform *plat = latm_platform_get();
    uint64_t p = latm_translate(env, addr, MMU_INST_FETCH, 0);
    uint32_t v;

    if (plat && plat->fetch32) {
        if (plat->fetch32(env, p, &v) != LATM_MEM_OK) {
            do_raise_exception(env, EXCP_AdEL, GETPC());
        }
        return v;
    }
    return *(const uint32_t *)(uintptr_t)p;
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

uint64_t latm_helper_load(CPUMIPSState *env, uint32_t op, uint64_t addr)
{
    const LatmPlatform *plat = latm_platform_get();
    uint64_t p;
    uint64_t v = 0;
    unsigned size;

    if (plat && plat->mem_read) {
        switch (op) {
        case MIPS_MEM_LB: case MIPS_MEM_LBU: size = 1; break;
        case MIPS_MEM_LH: case MIPS_MEM_LHU: size = 2; break;
        case MIPS_MEM_LW: case MIPS_MEM_LWU: size = 4; break;
        case MIPS_MEM_LD: size = 8; break;
        default: return 0;
        }
        p = latm_translate(env, addr, MMU_DATA_LOAD, GETPC());
        if (plat->mem_read(env, p, &v, size) != LATM_MEM_OK) {
            do_raise_exception(env, EXCP_AdEL, GETPC());
            return 0;
        }
        switch (op) {
        case MIPS_MEM_LB: return (int8_t)v;
        case MIPS_MEM_LBU: return (uint8_t)v;
        case MIPS_MEM_LH: return (int16_t)v;
        case MIPS_MEM_LHU: return (uint16_t)v;
        case MIPS_MEM_LW: return (int32_t)v;
        case MIPS_MEM_LWU: return (uint32_t)v;
        default: return v;
        }
    }
    switch (op) {
    case MIPS_MEM_LB:  return (int8_t)cpu_ldub_mmuidx_ra(env, addr, 0, 0);
    case MIPS_MEM_LBU: return cpu_ldub_mmuidx_ra(env, addr, 0, 0);
    case MIPS_MEM_LH:  return (int16_t)cpu_lduw_mmuidx_ra(env, addr, 0, 0);
    case MIPS_MEM_LHU: return cpu_lduw_mmuidx_ra(env, addr, 0, 0);
    case MIPS_MEM_LW:  return (int32_t)cpu_ldl_mmuidx_ra(env, addr, 0, 0);
    case MIPS_MEM_LWU: return cpu_ldl_mmuidx_ra(env, addr, 0, 0);
    case MIPS_MEM_LD:  return cpu_ldq_mmuidx_ra(env, addr, 0, 0);
    default: return 0;
    }
}

void latm_helper_store(CPUMIPSState *env, uint32_t op, uint64_t addr,
                       uint64_t val)
{
    const LatmPlatform *plat = latm_platform_get();
    uint64_t p;
    unsigned size;

    switch (op) {
    case MIPS_MEM_SB: size = 1; break;
    case MIPS_MEM_SH: size = 2; break;
    case MIPS_MEM_SW: size = 4; break;
    case MIPS_MEM_SD: size = 8; break;
    default: return;
    }
    p = latm_translate(env, addr, MMU_DATA_STORE, GETPC());
    if (plat && plat->mem_write) {
        if (plat->mem_write(env, p, val, size) != LATM_MEM_OK) {
            do_raise_exception(env, EXCP_AdES, GETPC());
        }
        return;
    }
    switch (op) {
    case MIPS_MEM_SB: *(uint8_t *)(uintptr_t)p = (uint8_t)val; break;
    case MIPS_MEM_SH: *(uint16_t *)(uintptr_t)p = (uint16_t)val; break;
    case MIPS_MEM_SW: *(uint32_t *)(uintptr_t)p = (uint32_t)val; break;
    case MIPS_MEM_SD: *(uint64_t *)(uintptr_t)p = val; break;
    default: break;
    }
}
