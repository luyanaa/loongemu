#include <string.h>
#include "reg-alloc.h"
#include "lata-vcache.h"
#include "ir2.h"
// #include "translate.h"
// #include "common.h"

/*
 * Init in global_register_init()
 */
IR2_OPND env_ir2_opnd;
IR2_OPND zero_ir2_opnd;
IR2_OPND ra_ir2_opnd;
IR2_OPND tp_ir2_opnd;
IR2_OPND sp_ir2_opnd;
IR2_OPND a0_ir2_opnd;
IR2_OPND a1_ir2_opnd;
IR2_OPND a2_ir2_opnd;
IR2_OPND a3_ir2_opnd;
IR2_OPND a4_ir2_opnd;
IR2_OPND a5_ir2_opnd;
IR2_OPND a6_ir2_opnd;
IR2_OPND a7_ir2_opnd;
IR2_OPND t0_ir2_opnd;
IR2_OPND t1_ir2_opnd;
IR2_OPND t2_ir2_opnd;
IR2_OPND t3_ir2_opnd;
IR2_OPND t4_ir2_opnd;
IR2_OPND t5_ir2_opnd;
IR2_OPND t6_ir2_opnd;
IR2_OPND t7_ir2_opnd;
IR2_OPND t8_ir2_opnd;
IR2_OPND r21_ir2_opnd;
IR2_OPND fp_ir2_opnd;
IR2_OPND s0_ir2_opnd;
IR2_OPND s1_ir2_opnd;
IR2_OPND s2_ir2_opnd;
IR2_OPND s3_ir2_opnd;
IR2_OPND s4_ir2_opnd;
IR2_OPND s5_ir2_opnd;
IR2_OPND s6_ir2_opnd;
IR2_OPND s7_ir2_opnd;
IR2_OPND s8_ir2_opnd;
IR2_OPND fsmask_ir2_opnd;
IR2_OPND fcsr_ir2_opnd;
IR2_OPND fcsr1_ir2_opnd;
IR2_OPND fcsr2_ir2_opnd;
IR2_OPND fcsr3_ir2_opnd;
IR2_OPND fcc0_ir2_opnd;
IR2_OPND fcc1_ir2_opnd;
IR2_OPND fcc2_ir2_opnd;
IR2_OPND fcc3_ir2_opnd;
IR2_OPND fcc4_ir2_opnd;
IR2_OPND fcc5_ir2_opnd;
IR2_OPND fcc6_ir2_opnd;
IR2_OPND fcc7_ir2_opnd;

IR2_OPND scr0_ir2_opnd;
IR2_OPND scr1_ir2_opnd;
IR2_OPND scr2_ir2_opnd;
IR2_OPND scr3_ir2_opnd;

/*
 * LA opnd mapping table
 */
const uint64 la_ir2_opnd_tab[] = {
    /* {reg_index, opnd addr} */
    REG_MAP_DEF(la_zero, (uint64)&zero_ir2_opnd),
    REG_MAP_DEF(la_ra, (uint64)&ra_ir2_opnd),
    REG_MAP_DEF(la_tp, (uint64)&tp_ir2_opnd),
    REG_MAP_DEF(la_sp, (uint64)&sp_ir2_opnd),
    REG_MAP_DEF(la_a0, (uint64)&a0_ir2_opnd),
    REG_MAP_DEF(la_a1, (uint64)&a1_ir2_opnd),
    REG_MAP_DEF(la_a2, (uint64)&a2_ir2_opnd),
    REG_MAP_DEF(la_a3, (uint64)&a3_ir2_opnd),
    REG_MAP_DEF(la_a4, (uint64)&a4_ir2_opnd),
    REG_MAP_DEF(la_a5, (uint64)&a5_ir2_opnd),
    REG_MAP_DEF(la_a6, (uint64)&a6_ir2_opnd),
    REG_MAP_DEF(la_a7, (uint64)&a7_ir2_opnd),
    REG_MAP_DEF(la_t0, (uint64)&t0_ir2_opnd),
    REG_MAP_DEF(la_t1, (uint64)&t1_ir2_opnd),
    REG_MAP_DEF(la_t2, (uint64)&t2_ir2_opnd),
    REG_MAP_DEF(la_t3, (uint64)&t3_ir2_opnd),
    REG_MAP_DEF(la_t4, (uint64)&t4_ir2_opnd),
    REG_MAP_DEF(la_t5, (uint64)&t5_ir2_opnd),
    REG_MAP_DEF(la_t6, (uint64)&t6_ir2_opnd),
    REG_MAP_DEF(la_t7, (uint64)&t7_ir2_opnd),
    REG_MAP_DEF(la_t8, (uint64)&t8_ir2_opnd),
    REG_MAP_DEF(la_r21, (uint64)&r21_ir2_opnd),
    REG_MAP_DEF(la_fp, (uint64)&fp_ir2_opnd),
    REG_MAP_DEF(la_s0, (uint64)&s0_ir2_opnd),
    REG_MAP_DEF(la_s1, (uint64)&s1_ir2_opnd),
    REG_MAP_DEF(la_s2, (uint64)&s2_ir2_opnd),
    REG_MAP_DEF(la_s3, (uint64)&s3_ir2_opnd),
    REG_MAP_DEF(la_s4, (uint64)&s4_ir2_opnd),
    REG_MAP_DEF(la_s5, (uint64)&s5_ir2_opnd),
    REG_MAP_DEF(la_s6, (uint64)&s6_ir2_opnd),
    REG_MAP_DEF(la_s7, (uint64)&s7_ir2_opnd),
    REG_MAP_DEF(la_s7, (uint64)&s7_ir2_opnd),
};

/*
 * Register mapping table
 */
const int reg_gpr_map[] = {
    REG_MAP_DEF(eax_index, la_s0),
    REG_MAP_DEF(ecx_index, la_s1),
    REG_MAP_DEF(edx_index, la_s2),
    REG_MAP_DEF(ebx_index, la_s3),
    REG_MAP_DEF(esp_index, la_s4),
    REG_MAP_DEF(ebp_index, la_s5),
    REG_MAP_DEF(esi_index, la_s6),
    REG_MAP_DEF(edi_index, la_s7),
#ifdef TARGET_X86_64
    REG_MAP_DEF(r8_index, la_a2),
    REG_MAP_DEF(r9_index, la_a3),
    REG_MAP_DEF(r10_index, la_a4),
    REG_MAP_DEF(r11_index, la_a5),
    REG_MAP_DEF(r12_index, la_t5),
    REG_MAP_DEF(r13_index, la_t6),
    REG_MAP_DEF(r14_index, la_t7),
    REG_MAP_DEF(r15_index, la_t8),
#endif
};

const int reg_fpr_map[] = {
    REG_MAP_DEF(0, 0),
    REG_MAP_DEF(1, 1),
    REG_MAP_DEF(2, 2),
    REG_MAP_DEF(3, 3),
    REG_MAP_DEF(4, 4),
    REG_MAP_DEF(5, 5),
    REG_MAP_DEF(6, 6),
    REG_MAP_DEF(7, 7),
};
#define reg_mmx_map reg_fpr_map

const int reg_xmm_map[] = {
    REG_MAP_DEF(0, 16),
    REG_MAP_DEF(1, 17),
    REG_MAP_DEF(2, 18),
    REG_MAP_DEF(3, 19),
    REG_MAP_DEF(4, 20),
    REG_MAP_DEF(5, 21),
    REG_MAP_DEF(6, 22),
    REG_MAP_DEF(7, 23),
#ifdef TARGET_X86_64
    REG_MAP_DEF(8, 24),
    REG_MAP_DEF(9, 25),
    REG_MAP_DEF(10, 26),
    REG_MAP_DEF(11, 27),
    REG_MAP_DEF(12, 28),
    REG_MAP_DEF(13, 29),
    REG_MAP_DEF(14, 30),
    REG_MAP_DEF(15, 31),
#endif
};

const int reg_statics_map[] = {
    /* last exec tb */
    REG_MAP_DEF(S_UD0, la_fp),
    /* next x86 addr */
    REG_MAP_DEF(S_UD1, la_r21),
#ifndef TARGET_X86_64
    /* we don't use shadow stack */
    REG_MAP_DEF(S_UD2, NOT_DEF),
#endif
    REG_MAP_DEF(S_ENV, la_s8),
    REG_MAP_DEF(S_EFLAGS, la_a6),
};

const int reg_trace_map[] = {
    REG_MAP_DEF(TRACE, NOT_DEF),
};

const int reg_itemp_map[] = {
    REG_MAP_DEF(ITEMP0, la_t0),
    REG_MAP_DEF(ITEMP1, la_t1),
    REG_MAP_DEF(ITEMP2, la_t2),
    REG_MAP_DEF(ITEMP3, la_t3),
    REG_MAP_DEF(ITEMP4, la_t4),
    REG_MAP_DEF(ITEMP5, la_t5),
    // REG_MAP_DEF(ITEMP6, la_t6),
    // REG_MAP_DEF(ITEMP7, la_t7),
    // REG_MAP_DEF(ITEMP8, la_t8),
    // REG_MAP_DEF(ITEMP9, la_ra),
    // REG_MAP_DEF(ITEMP10, la_a7),
};

const int reg_itemp_reverse_map[32] = {
    REG_MAP_DEF(0 ... 31, INVALID_TEMP),
    REG_MAP_DEF(la_t0, ITEMP0),
    REG_MAP_DEF(la_t1, ITEMP1),
    REG_MAP_DEF(la_t2, ITEMP2),
    REG_MAP_DEF(la_t3, ITEMP3),
    REG_MAP_DEF(la_t4, ITEMP4),
    REG_MAP_DEF(la_t5, ITEMP5),
    // REG_MAP_DEF(la_t6, ITEMP6),
    // REG_MAP_DEF(la_t7, ITEMP7),
    // REG_MAP_DEF(la_t8, ITEMP8),
    // REG_MAP_DEF(la_ra, ITEMP9),
    // REG_MAP_DEF(la_a7, ITEMP10),
};

/*
 * Temporary pool, initially f9..f14.  When the pool runs dry the allocator
 * spills a cold resident guest register and appends it here, so the pool is a
 * prefix of this array whose length is ftemp_pool_entries.
 */
int reg_ftemp_map[32] = {
    REG_MAP_DEF(FTEMP0, 9),
    REG_MAP_DEF(FTEMP1, 10),
    REG_MAP_DEF(FTEMP2, 11),
    REG_MAP_DEF(FTEMP3, 12),
    REG_MAP_DEF(FTEMP4, 13),
    REG_MAP_DEF(FTEMP5, 14),
};

#define FTEMP_BASE_ENTRIES 6

int reg_ftemp_reverse_map[32] = {
    REG_MAP_DEF(0 ... 31, INVALID_TEMP),
    REG_MAP_DEF(9, FTEMP0),
    REG_MAP_DEF(10, FTEMP1),
    REG_MAP_DEF(11, FTEMP2),
    REG_MAP_DEF(12, FTEMP3),
    REG_MAP_DEF(13, FTEMP4),
    REG_MAP_DEF(14, FTEMP5),
};

void lata_ftemp_init_context(LataContext *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ctx->ftemp_base = FTEMP_BASE_ENTRIES;
    ctx->ftemp_pool_entries = FTEMP_BASE_ENTRIES;
    memcpy(ctx->reg_ftemp_map, reg_ftemp_map, sizeof(ctx->reg_ftemp_map));
    memcpy(ctx->reg_ftemp_reverse_map, reg_ftemp_reverse_map,
           sizeof(ctx->reg_ftemp_reverse_map));
    ctx->ftemp_alloc_failures = 0;
    ctx->ftemp_fallbacks = 0;
}

int lata_ftemp_slot_for_phys(int phys)
{
    LataContext *ctx;
    if (phys < 0 || phys >= 32) {
        return INVALID_TEMP;
    }
    ctx = lata_context_current();
    if (ctx != NULL && ctx->initialized) {
        return ctx->reg_ftemp_reverse_map[phys];
    }
    return reg_ftemp_reverse_map[phys];
}

bool ra_ftemp_phys_in_pool(int phys)
{
    LataContext *ctx = lata_context_current();
    const int *map = (ctx != NULL && ctx->initialized) ? ctx->reg_ftemp_map
                                                       : reg_ftemp_map;
    for (int slot = 0; slot < ra_ftemp_pool_size(); ++slot) {
        if (map[slot] == phys) {
            return true;
        }
    }
    return false;
}

void lata_ftemp_set_base_pool(int pool)
{
    LataContext *ctx = lata_context_current();
    if (pool < 1) {
        pool = 1;
    }
    if (pool > FTEMP_BASE_ENTRIES) {
        pool = FTEMP_BASE_ENTRIES;
    }
    if (ctx == NULL || !ctx->initialized) {
        return;
    }
    /* Drop appended spill slots so their registers are not handed out twice. */
    for (int slot = pool; slot < ctx->ftemp_pool_entries; ++slot) {
        int phys = ctx->reg_ftemp_map[slot];
        if (phys >= 0 && phys < 32) {
            ctx->reg_ftemp_reverse_map[phys] = INVALID_TEMP;
        }
    }
    ctx->ftemp_base = pool;
    ctx->ftemp_pool_entries = pool;
}

const int reg_scr_map[] = {
    REG_MAP_DEF(0, 0),
    REG_MAP_DEF(1, 1),
    REG_MAP_DEF(2, 2),
    REG_MAP_DEF(3, 3),
};

/*
 * Base interface
 */
IR2_OPND get_la_ir2_opnd(int la_reg_num)
{
    return *(IR2_OPND *)la_ir2_opnd_tab[la_reg_num];
}

/* ra_alloc_gpr(int index) */
RA_ALLOC_FUNC(gpr, IR2_OPND_GPR)
/* ra_alloc_fpr(int index) */
RA_ALLOC_FUNC(fpr, IR2_OPND_FPR)
/* ra_alloc_mmx(int index) */
RA_ALLOC_FUNC(mmx, IR2_OPND_FPR)
/* ra_alloc_xmm(int index) */
RA_ALLOC_FUNC(xmm, IR2_OPND_FPR)
/* ra_alloc_statics */
RA_ALLOC_FUNC(statics, IR2_OPND_GPR)
/* ra_alloc_trace */
RA_ALLOC_FUNC(trace, IR2_OPND_GPR)
/* ra_alloc_src */
RA_ALLOC_FUNC(scr, IR2_OPND_SCR);
/* ra_alloc_itemp_num() */
RA_ALLOC_TEMP_NUM_FUNC(itemp)
/* ra_alloc_ftemp_num(): spill a cold guest register when the pool is dry. */
/*
 * Allocation failures must be deterministic: the old code fell through with an
 * out-of-range slot after the assert, which produced a bogus register and made
 * the generated TB loop forever instead of reporting the problem.
 */
uint32_t ra_ftemp_alloc_failures(void)
{
    LataContext *ctx = lata_context_current();
    return (ctx != NULL) ? ctx->ftemp_alloc_failures : 0;
}

bool ra_ftemp_alloc_failed(void)
{
    return lsenv != NULL && lsenv->tr_data != NULL &&
           lsenv->tr_data->ftemp_alloc_failed;
}

void ra_ftemp_clear_failed(void)
{
    if (lsenv != NULL && lsenv->tr_data != NULL) {
        lsenv->tr_data->ftemp_alloc_failed = false;
    }
}

void ra_ftemp_note_fallback(void)
{
    LataContext *ctx = lata_context_current();
    if (ctx != NULL) {
        ctx->ftemp_fallbacks++;
    }
}

uint32_t ra_ftemp_fallbacks(void)
{
    LataContext *ctx = lata_context_current();
    return (ctx != NULL) ? ctx->ftemp_fallbacks : 0;
}

static int ra_alloc_ftemp_num(void)
{
    LataContext *ctx = lata_context_current();
    int cto_num, ftemp_num;
    int spill_attempts = 0;

    for (;;) {
        __asm("cto.w %0, %1\n\t"
              : "=r"(cto_num)
              : "r"(lsenv->tr_data->ftemp_status));
        if (cto_num < ra_ftemp_pool_size()) {
            break;
        }
        {
            int spilled = lata_vreg_spill_cold();
            int phys;

            if (spilled < 0 || spill_attempts >= 8) {
                ctx->ftemp_alloc_failures++;
                /* Explicit failure: callers roll back snapshot and fall back. */
                lsenv->tr_data->ftemp_alloc_failed = true;
                return ctx->reg_ftemp_map[0];
            }
            spill_attempts++;
            phys = arm_la_fmap[spilled];
            if (!ra_ftemp_phys_in_pool(phys)) {
                if (ctx->ftemp_pool_entries >= 32) {
                    ctx->ftemp_alloc_failures++;
                    lsenv->tr_data->ftemp_alloc_failed = true;
                    return ctx->reg_ftemp_map[0];
                }
                ctx->reg_ftemp_map[ctx->ftemp_pool_entries] = phys;
                ctx->reg_ftemp_reverse_map[phys] = ctx->ftemp_pool_entries;
                ctx->ftemp_pool_entries++;
            }
        }
    }
    if (cto_num >= ra_ftemp_pool_size()) {
        ctx->ftemp_alloc_failures++;
        lsenv->tr_data->ftemp_alloc_failed = true;
        return ctx->reg_ftemp_map[0];
    }
    ftemp_num = ctx->reg_ftemp_map[cto_num];
    BITS_SET(lsenv->tr_data->ftemp_status, 1 << cto_num);
    return ftemp_num;
}

int ra_ftemp_pool_size(void)
{
    LataContext *ctx = lata_context_current();
    int entries = (ctx != NULL && ctx->initialized) ? ctx->ftemp_pool_entries
                                                    : FTEMP_BASE_ENTRIES;
    int base = (ctx != NULL && ctx->initialized) ? ctx->ftemp_base
                                                 : FTEMP_BASE_ENTRIES;

    /* f14/f15 are the cache homes while the window is enabled; a deliberately
     * shrunken pool (test knob) keeps its own size. */
    if (lata_vcache_enabled() && base == FTEMP_BASE_ENTRIES &&
        entries > 1) {
        entries -= 1;
    }
    return entries;
}

/*
 * Some encapsulation of base interface
 */
IR2_OPND ra_alloc_st(int st_num)
{
    return ra_alloc_fpr(st_num);
}

IR2_OPND ra_alloc_itemp(void)
{
    IR2_OPND ir2_opnd;
    int itemp_num;

    itemp_num = ra_alloc_itemp_num();

    ir2_opnd_build(&ir2_opnd, IR2_OPND_GPR, itemp_num);

    return ir2_opnd;
}

IR2_OPND ra_alloc_ftemp(void)
{
    IR2_OPND ir2_opnd;
    ir2_opnd_build(&ir2_opnd, IR2_OPND_FPR, ra_alloc_ftemp_num());
    return ir2_opnd;
}

IR2_OPND ra_alloc_num_4095(void)
{
    IR2_OPND num = ra_alloc_itemp();
    lsassertm(0, "ori do not generate");
    // la_ori(num, zero_ir2_opnd, 0xfff);
    return num;
}

inline __attribute__((__always_inline__)) void ra_free_num_4095(IR2_OPND opnd)
{
    ra_free_itemp(opnd._reg_num);
}

inline __attribute__((__always_inline__)) void ra_free_all(void)
{
    lsenv->tr_data->all_temp_status = 0;
}

/*
 * Future patches may remove it.
 */
IR2_OPND ra_alloc_mda(void)
{
    return ra_ir2_opnd;
}
