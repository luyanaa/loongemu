/*
 * LATM CP0, TLB and exception-return helpers.
 *
 * Register semantics follow QEMU target/mips/tcg/sysemu/cp0_helper.c and
 * tcg/sysemu/tlb_helper.c; the R4000 TLB subset is copied here so the
 * freestanding core does not depend on QEMU's soft-TLB.
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "internal.h"
#include "exec/helper-proto.h"
#include "exec/exec-all.h"
#include "latm-helpers.h"
#include "latm-platform.h"

static uint64_t get_tlb_pfn(uint64_t entrylo)
{
    return (entrylo >> 6) & UINT64_C(0x000003ffffffffff);
}

static void r4k_fill_tlb(CPUMIPSState *env, int idx)
{
    r4k_tlb_t *tlb;
    uint64_t mask = env->CP0_PageMask >> (TARGET_PAGE_BITS + 1);

    tlb = &env->tlb->mmu.r4k.tlb[idx];
    if (env->CP0_EntryHi & (1 << CP0EnHi_EHINV)) {
        tlb->EHINV = 1;
        return;
    }
    tlb->EHINV = 0;
    tlb->VPN = env->CP0_EntryHi & (TARGET_PAGE_MASK << 1);
    tlb->VPN &= env->SEGMask;
    tlb->ASID = env->CP0_EntryHi & env->CP0_EntryHi_ASID_mask;
    tlb->MMID = env->CP0_MemoryMapID;
    tlb->PageMask = env->CP0_PageMask;
    tlb->G = env->CP0_EntryLo0 & env->CP0_EntryLo1 & 1;
    tlb->V0 = (env->CP0_EntryLo0 & 2) != 0;
    tlb->D0 = (env->CP0_EntryLo0 & 4) != 0;
    tlb->C0 = (env->CP0_EntryLo0 >> 3) & 7;
    tlb->XI0 = (env->CP0_EntryLo0 >> CP0EnLo_XI) & 1;
    tlb->RI0 = (env->CP0_EntryLo0 >> CP0EnLo_RI) & 1;
    tlb->PFN[0] = (get_tlb_pfn(env->CP0_EntryLo0) & ~mask) << 12;
    tlb->V1 = (env->CP0_EntryLo1 & 2) != 0;
    tlb->D1 = (env->CP0_EntryLo1 & 4) != 0;
    tlb->C1 = (env->CP0_EntryLo1 >> 3) & 7;
    tlb->XI1 = (env->CP0_EntryLo1 >> CP0EnLo_XI) & 1;
    tlb->RI1 = (env->CP0_EntryLo1 >> CP0EnLo_RI) & 1;
    tlb->PFN[1] = (get_tlb_pfn(env->CP0_EntryLo1) & ~mask) << 12;
}

static void r4k_tlbwi(CPUMIPSState *env)
{
    int idx = env->CP0_Index & 0xff;
    if (idx >= (int)env->tlb->nb_tlb) {
        idx %= env->tlb->nb_tlb;
    }
    r4k_fill_tlb(env, idx);
}

static void r4k_tlbwr(CPUMIPSState *env)
{
    int idx = env->CP0_Random % env->tlb->nb_tlb;
    r4k_fill_tlb(env, idx);
}

static int tlb_match(CPUMIPSState *env, int idx)
{
    r4k_tlb_t *t = &env->tlb->mmu.r4k.tlb[idx];
    uint64_t mask = t->PageMask | ~(TARGET_PAGE_MASK << 1);
    target_ulong vpn = env->CP0_EntryHi & (TARGET_PAGE_MASK << 1);

    if (t->EHINV) {
        return 0;
    }
    if ((t->VPN & ~mask) != (vpn & ~mask)) {
        return 0;
    }
    if (!(t->G) && t->ASID != (env->CP0_EntryHi & env->CP0_EntryHi_ASID_mask)) {
        return 0;
    }
    if (t->MMID != (uint32_t)env->CP0_MemoryMapID &&
        (env->CP0_Config5 & (1u << CP0C5_MI))) {
        return 0;
    }
    return 1;
}

static int r4k_map_address(CPUMIPSState *env, hwaddr *physical, int *prot,
                           target_ulong address, MMUAccessType access_type)
{
    uint16_t ASID = env->CP0_EntryHi & env->CP0_EntryHi_ASID_mask;
    uint32_t MMID = env->CP0_MemoryMapID;
    bool mi = !!((env->CP0_Config5 >> CP0C5_MI) & 1);
    uint32_t tlb_mmid;

    MMID = mi ? MMID : (uint32_t)ASID;
    for (unsigned i = 0; i < env->tlb->tlb_in_use; i++) {
        r4k_tlb_t *tlb = &env->tlb->mmu.r4k.tlb[i];
        target_ulong mask = tlb->PageMask | ~(TARGET_PAGE_MASK << 1);
        target_ulong tag = address & ~mask;
        target_ulong VPN = tlb->VPN & ~mask;

        tag &= env->SEGMask;
        tlb_mmid = mi ? tlb->MMID : (uint32_t)tlb->ASID;
        if ((tlb->G == 1 || tlb_mmid == MMID) &&
            VPN == tag && !tlb->EHINV) {
            int n = !!(address & mask & ~(mask >> 1));
            if (!(n ? tlb->V1 : tlb->V0)) {
                return TLBRET_INVALID;
            }
            if (access_type == MMU_INST_FETCH && (n ? tlb->XI1 : tlb->XI0)) {
                return TLBRET_XI;
            }
            if (access_type == MMU_DATA_LOAD && (n ? tlb->RI1 : tlb->RI0)) {
                return TLBRET_RI;
            }
            if (access_type != MMU_DATA_STORE || (n ? tlb->D1 : tlb->D0)) {
                *physical = tlb->PFN[n] | (address & (mask >> 1));
                *prot = PAGE_READ;
                if (n ? tlb->D1 : tlb->D0) {
                    *prot |= PAGE_WRITE;
                }
                if (!(n ? tlb->XI1 : tlb->XI0)) {
                    *prot |= PAGE_EXEC;
                }
                return TLBRET_MATCH;
            }
            return TLBRET_DIRTY;
        }
    }
    return TLBRET_NOMATCH;
}

static void r4k_tlbp(CPUMIPSState *env)
{
    for (unsigned i = 0; i < env->tlb->nb_tlb; i++) {
        if (tlb_match(env, i)) {
            env->CP0_Index = i;
            return;
        }
    }
    env->CP0_Index = -1;
}

static void r4k_tlbinv(CPUMIPSState *env)
{
    bool mi = !!((env->CP0_Config5 >> CP0C5_MI) & 1);
    uint16_t asid = env->CP0_EntryHi & env->CP0_EntryHi_ASID_mask;
    uint32_t mmid = mi ? env->CP0_MemoryMapID : (uint32_t)asid;

    if (!env->tlb) {
        return;
    }
    for (unsigned i = 0; i < env->tlb->nb_tlb; i++) {
        r4k_tlb_t *t = &env->tlb->mmu.r4k.tlb[i];
        uint32_t tlb_mmid = mi ? t->MMID : (uint32_t)t->ASID;
        if (!t->G && tlb_mmid == mmid) {
            t->EHINV = 1;
        }
    }
}

static void r4k_tlbinvf(CPUMIPSState *env)
{
    if (!env->tlb) {
        return;
    }
    for (unsigned i = 0; i < env->tlb->nb_tlb; i++) {
        env->tlb->mmu.r4k.tlb[i].EHINV = 1;
    }
}

static void r4k_tlbr(CPUMIPSState *env)
{
    int idx = env->CP0_Index & 0xff;
    r4k_tlb_t *t;
    uint64_t mask;

    if (idx >= (int)env->tlb->nb_tlb) {
        idx = 0;
    }
    t = &env->tlb->mmu.r4k.tlb[idx];
    env->CP0_EntryHi = (env->CP0_EntryHi & ~(TARGET_PAGE_MASK << 1)) |
                       (t->VPN & (TARGET_PAGE_MASK << 1));
    env->CP0_EntryHi |= t->ASID;
    env->CP0_PageMask = t->PageMask;
    mask = t->PageMask >> (TARGET_PAGE_BITS + 1);
    env->CP0_EntryLo0 = (t->PFN[0] >> 12) << 6;
    env->CP0_EntryLo1 = (t->PFN[1] >> 12) << 6;
    env->CP0_EntryLo0 |= (uint64_t)t->C0 << 3;
    env->CP0_EntryLo1 |= (uint64_t)t->C1 << 3;
    env->CP0_EntryLo0 |= t->V0 << 1;
    env->CP0_EntryLo1 |= t->V1 << 1;
    env->CP0_EntryLo0 |= t->D0 << 2;
    env->CP0_EntryLo1 |= t->D1 << 2;
    env->CP0_EntryLo0 |= (uint64_t)t->G;
    env->CP0_EntryLo1 |= (uint64_t)t->G;
    (void)mask;
}

void latm_tlb_init(CPUMIPSState *env, CPUMIPSTLBContext *tlb, int nb_tlb)
{
    memset(tlb, 0, sizeof(*tlb));
    tlb->nb_tlb = nb_tlb;
    tlb->tlb_in_use = nb_tlb;
    tlb->map_address = r4k_map_address;
    tlb->helper_tlbwi = NULL;
    tlb->helper_tlbwr = NULL;
    tlb->helper_tlbp = NULL;
    tlb->helper_tlbr = NULL;
    env->tlb = tlb;
    env->CP0_Random = nb_tlb - 1;
    env->CP0_EntryHi_ASID_mask = 0xff;
    env->SEGMask = (1ULL << env->SEGBITS) - 1;
    env->PAMask = (1ULL << env->PABITS) - 1;
    env->CP0_SegCtl0 = (CP0SC_AM_MK << CP0SC_AM) |
                       ((CP0SC_AM_MSK << CP0SC_AM) << 16);
    env->CP0_SegCtl1 = (CP0SC_AM_UK << CP0SC_AM) |
                       ((CP0SC_AM_UK << CP0SC_AM) << 16) |
                       (CP0SC_AM_UK << CP0SC1_XAM);
    env->CP0_SegCtl2 = (2 << CP0SC_PA) | (CP0SC_AM_MUSK << CP0SC_AM) |
                       (((0 << CP0SC_PA) | (CP0SC_AM_MUSK << CP0SC_AM)) << 16);
}

void latm_helper_tlb(CPUMIPSState *env, uint32_t op)
{
    if (!env->tlb) {
        do_raise_exception(env, EXCP_RI, GETPC());
        return;
    }
    switch (op) {
    case MIPS_TLB_TLBR:  r4k_tlbr(env); break;
    case MIPS_TLB_TLBWI: r4k_tlbwi(env); break;
    case MIPS_TLB_TLBWR: r4k_tlbwr(env); break;
    case MIPS_TLB_TLBP:  r4k_tlbp(env); break;
    case MIPS_TLB_TLBINV: r4k_tlbinv(env); break;
    case MIPS_TLB_TLBINVF: r4k_tlbinvf(env); break;
    }
    env->llvalid = 0;
    env->lladdr = UINT64_MAX;
    latm_jmp_cache_flush(env);
    {
        const LatmPlatform *p = latm_platform_get();
        if (p && p->tlb_flush) {
            p->tlb_flush(env);
        }
    }
}

uint64_t latm_helper_cp0_read(CPUMIPSState *env, uint32_t reg, uint32_t sel)
{
    switch (reg) {
    case 0: return env->CP0_Index;
    case 1: return env->CP0_Random;
    case 2: return env->CP0_EntryLo0;
    case 3: return env->CP0_EntryLo1;
    case 4:
        switch (sel) {
        case 0: return env->CP0_Context;
        case 1: return 0;
        case 2: return env->active_tc.CP0_UserLocal;
        case 3: return 0;
        default: return 0;
        }
    case 5:
        switch (sel) {
        case 0: return env->CP0_PageMask;
        case 1: return env->CP0_PageGrain;
        case 2: return env->CP0_SegCtl0;
        case 3: return env->CP0_SegCtl1;
        case 4: return env->CP0_SegCtl2;
        default: return 0;
        }
    case 6:
        switch (sel) {
        case 0: return env->CP0_Wired;
        case 1: return env->CP0_SRSConf0;
        case 2: return env->CP0_SRSConf1;
        case 3: return env->CP0_SRSConf2;
        case 4: return env->CP0_SRSConf3;
        case 5: return env->CP0_SRSConf4;
        case 6: return env->CP0_PWCtl;
        default: return 0;
        }
    case 7: return env->CP0_HWREna;
    case 8:
        switch (sel) {
        case 0: return env->CP0_BadVAddr;
        case 1: return env->CP0_BadInstr;
        case 2: return env->CP0_BadInstrP;
        case 3: return env->CP0_BadInstrX;
        default: return 0;
        }
    case 9:
        switch (sel) {
        case 0: return cpu_mips_get_count(env);
        default: return 0;
        }
    case 10: return env->CP0_EntryHi;
    case 11: return env->CP0_Compare;
    case 12:
        switch (sel) {
        case 0: return env->CP0_Status;
        case 1: return env->CP0_IntCtl;
        case 2: return env->CP0_SRSCtl;
        case 3: return env->CP0_SRSMap;
        default: return 0;
        }
    case 13: return env->CP0_Cause;
    case 14: return env->CP0_EPC;
    case 15:
        switch (sel) {
        case 0: return env->CP0_PRid;
        case 1: return env->CP0_EBase;
        case 2: return env->CP0_CMGCRBase;
        default: return 0;
        }
    case 16:
        switch (sel) {
        case 0: return env->CP0_Config0;
        case 1: return env->CP0_Config1;
        case 2: return env->CP0_Config2;
        case 3: return env->CP0_Config3;
        case 4: return env->CP0_Config4;
        case 5: return env->CP0_Config5;
        case 6: return env->CP0_Config6;
        case 7: return env->CP0_Config7;
        default: return 0;
        }
    case 17: return env->CP0_LLAddr;
    case 18: return env->CP0_WatchLo[0];
    case 19: return env->CP0_WatchHi[0];
    case 20: return env->CP0_XContext;
    case 23: return env->CP0_Debug;
    case 24: return env->CP0_DEPC;
    case 26: return env->CP0_ErrCtl;
    case 28: return env->CP0_TagLo;
    case 29: return env->CP0_TagHi;
    case 30: return env->CP0_ErrorEPC;
    case 31: return env->active_tc.CP0_UserLocal; /* KScratch approximation */
    default: return 0;
    }
}

void latm_helper_cp0_write(CPUMIPSState *env, uint32_t reg, uint32_t sel,
                           uint64_t v)
{
    switch (reg) {
    case 0: env->CP0_Index = v; break;
    case 2: env->CP0_EntryLo0 = v; break;
    case 3: env->CP0_EntryLo1 = v; break;
    case 4:
        if (sel == 0) env->CP0_Context = v;
        else if (sel == 1) { /* ContextConfig not modeled */ }
        else if (sel == 2) env->active_tc.CP0_UserLocal = v;
        else if (sel == 3) { /* XContextConfig not modeled */ }
        break;
    case 5:
        if (sel == 0) env->CP0_PageMask = v;
        else if (sel == 1) env->CP0_PageGrain = v;
        else if (sel == 2) env->CP0_SegCtl0 = v;
        else if (sel == 3) env->CP0_SegCtl1 = v;
        else if (sel == 4) env->CP0_SegCtl2 = v;
        break;
    case 6:
        if (sel == 0) env->CP0_Wired = v;
        else if (sel == 1) env->CP0_SRSConf0 = v;
        else if (sel == 2) env->CP0_SRSConf1 = v;
        else if (sel == 3) env->CP0_SRSConf2 = v;
        else if (sel == 4) env->CP0_SRSConf3 = v;
        else if (sel == 5) env->CP0_SRSConf4 = v;
        else if (sel == 6) env->CP0_PWCtl = v;
        break;
    case 7: env->CP0_HWREna = v; break;
    case 9:
        if (sel == 0) {
            const LatmPlatform *p = latm_platform_get();
            env->CP0_Count = v;
            if (p && p->store_count) {
                p->store_count(env, (uint32_t)v);
            }
        }
        break;
    case 10: {
        uint16_t old_asid = env->CP0_EntryHi & env->CP0_EntryHi_ASID_mask;
        env->CP0_EntryHi = v;
        if (old_asid != (v & env->CP0_EntryHi_ASID_mask)) {
            latm_jmp_cache_flush(env);
            {
                const LatmPlatform *p = latm_platform_get();
                if (p && p->tlb_flush) {
                    p->tlb_flush(env);
                }
            }
        }
        break;
    }
    case 11:
        env->CP0_Compare = v;
        env->CP0_Cause &= ~((1u << 30) |
                            (1u << (8 + ((env->CP0_IntCtl >> 29) & 7))));
        {
            const LatmPlatform *p = latm_platform_get();
            if (p && p->set_compare) {
                p->set_compare(env, (uint32_t)v);
            }
        }
        break;
    case 12:
        if (sel == 0) {
            uint32_t old_mode = env->hflags & (MIPS_HFLAG_UM | MIPS_HFLAG_SM);
            env->CP0_Status = v & env->CP0_Status_rw_bitmask;
            compute_hflags(env);
            if (old_mode != (env->hflags & (MIPS_HFLAG_UM | MIPS_HFLAG_SM))) {
                latm_jmp_cache_flush(env);
            }
        } else if (sel == 1) env->CP0_IntCtl = v;
        else if (sel == 2) env->CP0_SRSCtl = v;
        else if (sel == 3) env->CP0_SRSMap = v;
        break;
    case 13:
        env->CP0_Cause = (env->CP0_Cause & ~0x00000300u) | (v & 0x00000300u);
        break;
    case 14: env->CP0_EPC = v; break;
    case 15:
        if (sel == 1) env->CP0_EBase = v;
        else if (sel == 2) env->CP0_CMGCRBase = v;
        break;
    case 16:
        if (sel == 0) env->CP0_Config0 = v;
        else if (sel == 1) env->CP0_Config1 = v;
        else if (sel == 2) env->CP0_Config2 = v;
        else if (sel == 3) env->CP0_Config3 = v;
        else if (sel == 4) env->CP0_Config4 = v;
        else if (sel == 5) env->CP0_Config5 = v;
        else if (sel == 6) env->CP0_Config6 = v;
        else if (sel == 7) env->CP0_Config7 = v;
        break;
    case 17: env->CP0_LLAddr = v; break;
    case 18: env->CP0_WatchLo[0] = v; break;
    case 19: env->CP0_WatchHi[0] = v; break;
    case 20: env->CP0_XContext = v; break;
    case 23: env->CP0_Debug = v; break;
    case 24: env->CP0_DEPC = v; break;
    case 26: env->CP0_ErrCtl = v; break;
    case 28: env->CP0_TagLo = v; break;
    case 29: env->CP0_TagHi = v; break;
    case 30: env->CP0_ErrorEPC = v; break;
    case 31: env->active_tc.CP0_UserLocal = v; break;
    }
}

uint64_t latm_helper_shadow_read(CPUMIPSState *env, uint32_t rd)
{
    unsigned pss = (env->CP0_SRSCtl >> CP0SRSCtl_PSS) & 0xf;
    return env->shadow_gpr[pss][rd & 31];
}

void latm_helper_shadow_write(CPUMIPSState *env, uint32_t rd, uint64_t val)
{
    unsigned pss = (env->CP0_SRSCtl >> CP0SRSCtl_PSS) & 0xf;
    env->shadow_gpr[pss][rd & 31] = val;
}

uint32_t cpu_mips_get_count(CPUMIPSState *env)
{
    const LatmPlatform *p = latm_platform_get();
    uint32_t count;

    if (p && p->read_count && !(env->CP0_Cause & (1u << 27))) {
        count = p->read_count(env, UINT64_C(50000000));
    } else {
        count = (uint32_t)env->CP0_Count;
    }

    /* Compare is a 32-bit free-running match.  Keep TI sticky until software
     * writes Compare, and route the timer IRQ through IntCtl.IPTI. */
    if (!(env->CP0_Cause & (1u << 27)) && env->CP0_Compare &&
        !(env->CP0_Cause & (1u << 30)) &&
        (int32_t)(count - env->CP0_Compare) >= 0) {
        unsigned ipti = (env->CP0_IntCtl >> 29) & 7;
        env->CP0_Cause |= (1u << 30) | (1u << (8 + ipti));
    }
    env->CP0_Count = count;
    return count;
}

void latm_helper_eret(CPUMIPSState *env)
{
    env->llvalid = 0;
    env->lladdr = UINT64_MAX;
    env->active_tc.PC = env->CP0_EPC;
    env->CP0_Status &= ~((1u << CP0St_EXL) | (1u << CP0St_ERL));
    env->hflags &= ~MIPS_HFLAG_BMASK;
    compute_hflags(env);
}

void latm_helper_wait(CPUMIPSState *env)
{
    env->active_tc.CP0_TCHalt |= 1;
}

uint64_t latm_helper_di(CPUMIPSState *env)
{
    uint64_t old = (env->CP0_Status >> CP0St_IE) & 1;
    env->CP0_Status &= ~(1u << CP0St_IE);
    compute_hflags(env);
    return old;
}

uint64_t latm_helper_ei(CPUMIPSState *env)
{
    uint64_t old = (env->CP0_Status >> CP0St_IE) & 1;
    env->CP0_Status |= 1u << CP0St_IE;
    compute_hflags(env);
    return old;
}
