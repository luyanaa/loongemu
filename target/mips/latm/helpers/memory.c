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
    uint32_t v = 0;

    if (plat && plat->fetch32) {
        if (plat->fetch32(env, p, &v) != LATM_MEM_OK) {
            env->latm_fetch_fault = 1;
            do_raise_exception(env, EXCP_AdEL, GETPC());
            return 0;
        }
        env->latm_fetch_fault = 0;
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

/*
 * Per-helper counters for the boot harness: how much of a translated block's
 * work is helper calls. They are statics read through a getter, so the cost is a
 * couple of increments and nothing in the translation path changes.
 */
uint64_t latm_helper_stats_loads;
uint64_t latm_helper_stats_stores;
uint64_t latm_helper_stats_unaligned;

void latm_helper_stats_get(uint64_t *loads, uint64_t *stores, uint64_t *unaligned)
{
    if (loads) *loads = latm_helper_stats_loads;
    if (stores) *stores = latm_helper_stats_stores;
    if (unaligned) *unaligned = latm_helper_stats_unaligned;
}

uint64_t latm_helper_load(CPUMIPSState *env, uint32_t op, uint64_t addr)
{
    latm_helper_stats_loads++;
    const LatmPlatform *plat = latm_platform_get();
    uint64_t p;
    uint64_t v = 0;
    unsigned size;

    switch (op) {
    case MIPS_MEM_LB: case MIPS_MEM_LBU: size = 1; break;
    case MIPS_MEM_LH: case MIPS_MEM_LHU: size = 2; break;
    case MIPS_MEM_LW: case MIPS_MEM_LWU: size = 4; break;
    case MIPS_MEM_LD: size = 8; break;
    default: return 0;
    }
    if (addr & (size - 1)) {
        do_raise_exception(env, EXCP_AdEL, GETPC());
        return 0;
    }
    if (plat && plat->mem_read) {
        p = latm_translate(env, addr, MMU_DATA_LOAD, GETPC());
        if (plat->mem_read(env, p, &v, size) != LATM_MEM_OK) {
            do_raise_exception(env, EXCP_AdEL, GETPC());
            return 0;
        }
        /* Sign-extend explicitly and keep the cast visible: converting
         * (int32_t)v straight to the uint64_t return type zero-extends, which
         * silently turned every MIPS LW into LWU. */
        {
            int64_t s;
            switch (op) {
            case MIPS_MEM_LB:  s = (int8_t)v; break;
            case MIPS_MEM_LH:  s = (int16_t)v; break;
            case MIPS_MEM_LW:  s = (int32_t)v; break;
            default:           return v;
            }
            return (uint64_t)s;
        }
    }
    {
        int64_t s;
        switch (op) {
        case MIPS_MEM_LB:  s = (int8_t)cpu_ldub_mmuidx_ra(env, addr, 0, 0); break;
        case MIPS_MEM_LH:  s = (int16_t)cpu_lduw_mmuidx_ra(env, addr, 0, 0); break;
        case MIPS_MEM_LW:  s = (int32_t)cpu_ldl_mmuidx_ra(env, addr, 0, 0); break;
        case MIPS_MEM_LBU: return cpu_ldub_mmuidx_ra(env, addr, 0, 0);
        case MIPS_MEM_LHU: return cpu_lduw_mmuidx_ra(env, addr, 0, 0);
        case MIPS_MEM_LWU: return cpu_ldl_mmuidx_ra(env, addr, 0, 0);
        case MIPS_MEM_LD:  return cpu_ldq_mmuidx_ra(env, addr, 0, 0);
        default: return 0;
        }
        return (uint64_t)s;
    }
}

void latm_helper_store(CPUMIPSState *env, uint32_t op, uint64_t addr,
                       uint64_t val)
{
    const LatmPlatform *plat = latm_platform_get();

    latm_helper_stats_stores++;
    uint64_t p;
    unsigned size;

    switch (op) {
    case MIPS_MEM_SB: size = 1; break;
    case MIPS_MEM_SH: size = 2; break;
    case MIPS_MEM_SW: size = 4; break;
    case MIPS_MEM_SD: size = 8; break;
    default: return;
    }
    if (addr & (size - 1)) {
        do_raise_exception(env, EXCP_AdES, GETPC());
        return;
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


static int latm_big_endian(CPUMIPSState *env)
{
    return (env->CP0_Config0 & (1u << CP0C0_BE)) != 0;
}

static int latm_probe_load(CPUMIPSState *env, uint64_t addr, unsigned size)
{
    const LatmPlatform *plat = latm_platform_get();
    if (!plat || !plat->mem_probe) {
        return -1;
    }
    uint64_t p = latm_translate(env, addr, MMU_DATA_LOAD, GETPC());
    return plat->mem_probe(env, p, size);
}

static unsigned latm_lmask(CPUMIPSState *env, uint64_t addr, unsigned bits)
{
    unsigned mask = bits / 8u - 1u;
    unsigned v = (unsigned)addr & mask;
    if (!latm_big_endian(env)) v ^= mask;
    return v;
}

uint64_t latm_helper_unaligned_load(CPUMIPSState *env, uint32_t op,
                                    uint64_t addr, uint64_t old)
{
    unsigned size = (op == MIPS_MEM_LWL || op == MIPS_MEM_LWR) ? 4u : 8u;
    unsigned mask = size - 1u;
    unsigned pos = (unsigned)addr & mask;
    unsigned shift;
    uint64_t word;

    /* Trigger the page/alignment exception at the guest address.  A probing
     * platform lets us validate without a side-effecting MMIO byte read. */
    {
        int probe = latm_probe_load(env, addr, 1);
        if (probe < 0) {
            (void)latm_helper_load(env, MIPS_MEM_LBU, addr);
        } else if (probe != LATM_MEM_OK) {
            (void)latm_helper_load(env, MIPS_MEM_LBU, addr);
            return 0;
        }
    }
    /* LWL/LWR operate on the zero-extended 32-bit word; MIPS32 results are
     * sign-extended only after the byte merge (QEMU gen_lxl/gen_lxr +
     * ext32s). */
    word = latm_helper_load(env, size == 4 ? MIPS_MEM_LWU : MIPS_MEM_LD,
                            addr & ~(uint64_t)mask);
    if (op == MIPS_MEM_LWL || op == MIPS_MEM_LDL) {
        shift = (latm_big_endian(env) ? pos : (mask - pos)) * 8u;
        uint64_t result = (old & ~((size == 4 ? UINT64_C(0xffffffff) : UINT64_MAX)
                                   << shift)) | (word << shift);
        return size == 4 ? (uint64_t)(int64_t)(int32_t)result : result;
    } else {
        shift = (latm_big_endian(env) ? (mask - pos) : pos) * 8u;
        word >>= shift;
        {
            uint64_t keep = ~UINT64_C(1) << (size * 8u - 1u - shift);
            uint64_t result = (old & keep) | word;
            return size == 4 ? (uint64_t)(int64_t)(int32_t)result : result;
        }
    }
}

void latm_helper_unaligned_store(CPUMIPSState *env, uint32_t op,
                                 uint64_t addr, uint64_t value)
{
    unsigned size = (op == MIPS_MEM_SWL || op == MIPS_MEM_SWR) ? 4u : 8u;
    unsigned bits = size * 8u;
    unsigned lmask = latm_lmask(env, addr, bits);
    int dir = latm_big_endian(env) ? 1 : -1;

    if (op == MIPS_MEM_SWL || op == MIPS_MEM_SDL) {
        latm_helper_store(env, MIPS_MEM_SB, addr, value >> (bits - 8u));
        if (lmask <= size - 2u)
            latm_helper_store(env, MIPS_MEM_SB, addr + dir,
                              value >> (bits - 16u));
        if (lmask <= size - 3u)
            latm_helper_store(env, MIPS_MEM_SB, addr + 2 * dir,
                              value >> (bits - 24u));
        if (lmask <= size - 4u)
            latm_helper_store(env, MIPS_MEM_SB, addr + 3 * dir,
                              value >> (bits - 32u));
        if (size == 8) {
            if (lmask <= 3u)
                latm_helper_store(env, MIPS_MEM_SB, addr + 4 * dir,
                                  value >> 24u);
            if (lmask <= 2u)
                latm_helper_store(env, MIPS_MEM_SB, addr + 5 * dir,
                                  value >> 16u);
            if (lmask <= 1u)
                latm_helper_store(env, MIPS_MEM_SB, addr + 6 * dir,
                                  value >> 8u);
            if (lmask == 0u)
                latm_helper_store(env, MIPS_MEM_SB, addr + 7 * dir, value);
        }
    } else {
        latm_helper_store(env, MIPS_MEM_SB, addr, value);
        if (lmask >= 1u)
            latm_helper_store(env, MIPS_MEM_SB, addr - dir, value >> 8u);
        if (lmask >= 2u)
            latm_helper_store(env, MIPS_MEM_SB, addr - 2 * dir, value >> 16u);
        if (lmask >= 3u)
            latm_helper_store(env, MIPS_MEM_SB, addr - 3 * dir, value >> 24u);
        if (size == 8) {
            if (lmask >= 4u)
                latm_helper_store(env, MIPS_MEM_SB, addr - 4 * dir, value >> 32u);
            if (lmask >= 5u)
                latm_helper_store(env, MIPS_MEM_SB, addr - 5 * dir, value >> 40u);
            if (lmask >= 6u)
                latm_helper_store(env, MIPS_MEM_SB, addr - 6 * dir, value >> 48u);
            if (lmask == 7u)
                latm_helper_store(env, MIPS_MEM_SB, addr - 7 * dir, value >> 56u);
        }
    }
}
