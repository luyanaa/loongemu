/*
 * LATM IR2 register allocation.
 *
 * The interface is LATA's; the maps are MIPS-specific.  FPRs live in
 * CPUMIPSState, so only the six ftemp registers are used for short-lived
 * IR2 FP values.
 */
#include "reg-alloc.h"
#include "reg-map.h"
#include "latm.h"

IR2_OPND env_ir2_opnd, zero_ir2_opnd, ra_ir2_opnd, tp_ir2_opnd, sp_ir2_opnd;
IR2_OPND a0_ir2_opnd, a1_ir2_opnd, a2_ir2_opnd, a3_ir2_opnd;
IR2_OPND a4_ir2_opnd, a5_ir2_opnd, a6_ir2_opnd, a7_ir2_opnd;
IR2_OPND t0_ir2_opnd, t1_ir2_opnd, t2_ir2_opnd, t3_ir2_opnd;
IR2_OPND t4_ir2_opnd, t5_ir2_opnd, t6_ir2_opnd, t7_ir2_opnd, t8_ir2_opnd;
IR2_OPND r21_ir2_opnd, fp_ir2_opnd;
IR2_OPND s0_ir2_opnd, s1_ir2_opnd, s2_ir2_opnd, s3_ir2_opnd;
IR2_OPND s4_ir2_opnd, s5_ir2_opnd, s6_ir2_opnd, s7_ir2_opnd, s8_ir2_opnd;
IR2_OPND fsmask_ir2_opnd, fcsr_ir2_opnd, fcsr1_ir2_opnd, fcsr2_ir2_opnd, fcsr3_ir2_opnd;
IR2_OPND fcc0_ir2_opnd, fcc1_ir2_opnd, fcc2_ir2_opnd, fcc3_ir2_opnd;
IR2_OPND fcc4_ir2_opnd, fcc5_ir2_opnd, fcc6_ir2_opnd, fcc7_ir2_opnd;
IR2_OPND scr0_ir2_opnd, scr1_ir2_opnd, scr2_ir2_opnd, scr3_ir2_opnd;

const uint64 la_ir2_opnd_tab[] = {
    [la_zero] = (uint64)&zero_ir2_opnd,
    [la_ra]   = (uint64)&ra_ir2_opnd,
    [la_tp]   = (uint64)&tp_ir2_opnd,
    [la_sp]   = (uint64)&sp_ir2_opnd,
    [la_a0]   = (uint64)&a0_ir2_opnd,
    [la_a1]   = (uint64)&a1_ir2_opnd,
    [la_a2]   = (uint64)&a2_ir2_opnd,
    [la_a3]   = (uint64)&a3_ir2_opnd,
    [la_a4]   = (uint64)&a4_ir2_opnd,
    [la_a5]   = (uint64)&a5_ir2_opnd,
    [la_a6]   = (uint64)&a6_ir2_opnd,
    [la_a7]   = (uint64)&a7_ir2_opnd,
    [la_t0]   = (uint64)&t0_ir2_opnd,
    [la_t1]   = (uint64)&t1_ir2_opnd,
    [la_t2]   = (uint64)&t2_ir2_opnd,
    [la_t3]   = (uint64)&t3_ir2_opnd,
    [la_t4]   = (uint64)&t4_ir2_opnd,
    [la_t5]   = (uint64)&t5_ir2_opnd,
    [la_t6]   = (uint64)&t6_ir2_opnd,
    [la_t7]   = (uint64)&t7_ir2_opnd,
    [la_t8]   = (uint64)&t8_ir2_opnd,
    [la_r21]  = (uint64)&r21_ir2_opnd,
    [la_fp]   = (uint64)&fp_ir2_opnd,
    [la_s0]   = (uint64)&s0_ir2_opnd,
    [la_s1]   = (uint64)&s1_ir2_opnd,
    [la_s2]   = (uint64)&s2_ir2_opnd,
    [la_s3]   = (uint64)&s3_ir2_opnd,
    [la_s4]   = (uint64)&s4_ir2_opnd,
    [la_s5]   = (uint64)&s5_ir2_opnd,
    [la_s6]   = (uint64)&s6_ir2_opnd,
    [la_s7]   = (uint64)&s7_ir2_opnd,
    [la_s8]   = (uint64)&s8_ir2_opnd,
};

const int reg_gpr_map[] = {
    [0]  = NOT_DEF,
    [1]  = la_a1,  [2]  = la_a2,  [3]  = la_a3,  [4]  = la_a4,
    [5]  = la_a5,  [6]  = la_a6,  [7]  = la_a7,
    [8]  = la_t3,  [9]  = la_t4,  [10] = la_t5,  [11] = la_t6,
    [12] = la_t7,  [13] = la_t8,  [14] = la_fp,
    [15] = la_s0,  [16] = la_s1,  [17] = la_s2,  [18] = la_s3,
    [19] = la_s4,  [20] = la_s5,  [21] = la_s6,  [22] = la_s7,
    [23] = NOT_DEF, [24] = NOT_DEF, [25] = NOT_DEF,
    [26] = NOT_DEF, [27] = NOT_DEF, [28] = NOT_DEF,
    [29] = la_s8,  [30] = la_r21, [31] = la_ra,
};

const int reg_fpr_map[] = {
    [0 ... 31] = NOT_DEF,
};

const int reg_xmm_map[] = {
    [0 ... 31] = NOT_DEF,
};
const int reg_mmx_map[] = {
    [0 ... 31] = NOT_DEF,
};

const int reg_statics_map[] = {
    [S_ENV] = la_tp,
    [S_EFLAGS] = la_a0,
};

const int reg_itemp_map[] = {
    [ITEMP0] = la_t0,
    [ITEMP1] = la_t1,
    [ITEMP2] = la_t2,
};

const int reg_itemp_reverse_map[32] = {
    [0 ... 31] = NOT_DEF,
    [la_t0] = ITEMP0,
    [la_t1] = ITEMP1,
    [la_t2] = ITEMP2,
};

const int reg_ftemp_map[] = {
    [FTEMP0] = 9,
    [FTEMP1] = 10,
    [FTEMP2] = 11,
    [FTEMP3] = 12,
    [FTEMP4] = 13,
    [FTEMP5] = 14,
};

const int reg_ftemp_reverse_map[32] = {
    [0 ... 31] = NOT_DEF,
    [9]  = FTEMP0,
    [10] = FTEMP1,
    [11] = FTEMP2,
    [12] = FTEMP3,
    [13] = FTEMP4,
    [14] = FTEMP5,
};

const int reg_scr_map[] = { 0, 1, 2, 3 };
const int reg_trace_map[] = { NOT_DEF };

IR2_OPND get_la_ir2_opnd(int la_reg_num)
{
    return *(IR2_OPND *)la_ir2_opnd_tab[la_reg_num];
}

#undef RA_ALLOC_FUNC
#define RA_ALLOC_FUNC(name, type)                                           \
IR2_OPND ra_alloc_##name(int index)                                         \
{                                                                           \
    IR2_OPND opnd;                                                          \
    opnd._type = type;                                                      \
    opnd._reg_num = reg_##name##_map[index];                                \
    opnd._assist = 0;                                                       \
    return opnd;                                                            \
}

RA_ALLOC_FUNC(gpr, IR2_OPND_GPR)
RA_ALLOC_FUNC(fpr, IR2_OPND_FPR)
RA_ALLOC_FUNC(mmx, IR2_OPND_FPR)
RA_ALLOC_FUNC(xmm, IR2_OPND_FPR)
RA_ALLOC_FUNC(statics, IR2_OPND_GPR)
RA_ALLOC_FUNC(trace, IR2_OPND_GPR)
RA_ALLOC_FUNC(scr, IR2_OPND_SCR)

static int ra_alloc_temp_idx(uint16_t status, int max)
{
    uint32_t free_bits = (~status) & 0xffffu;
    if (free_bits == 0) {
        return max;
    }
    return __builtin_ctz(free_bits);
}

IR2_OPND ra_alloc_itemp(void)
{
    uint16_t full = (uint16_t)((1u << itemp_status_num) - 1);
    if ((lsenv->tr_data->itemp_status & full) == full) {
        /* Retire one cached guest register.  A full-cache flush is deliberately
         * not used here: it emits env writebacks in the middle of the block, so
         * a later alloc_gpr_src() would reload from env before those stores run
         * and pick up the pre-writeback value. */
        latm_regcache_evict_itemp();
    }
    int idx = ra_alloc_temp_idx(lsenv->tr_data->itemp_status, 16);
    lsassertm(idx < (int)(sizeof(reg_itemp_map) / sizeof(int)),
              "out of IR2 itemps");
    lsenv->tr_data->itemp_status |= (uint16_t)(1u << idx);
    return ir2_opnd_new(IR2_OPND_GPR, reg_itemp_map[idx]);
}

IR2_OPND ra_alloc_ftemp(void)
{
    int idx = ra_alloc_temp_idx(lsenv->tr_data->ftemp_status, 16);
    lsassertm(idx < (int)(sizeof(reg_ftemp_map) / sizeof(int)),
              "out of IR2 ftemps");
    lsenv->tr_data->ftemp_status |= (uint16_t)(1u << idx);
    return ir2_opnd_new(IR2_OPND_FPR, reg_ftemp_map[idx]);
}

void ra_free_all(void)
{
    lsenv->tr_data->all_temp_status = 0;
}

IR2_OPND ra_alloc_num_4095(void)
{
    IR2_OPND t = ra_alloc_itemp();
    la_ori(t, zero_ir2_opnd, 0xfff);
    return t;
}

void ra_free_num_4095(IR2_OPND opnd)
{
    ra_free_itemp(opnd._reg_num);
}

IR2_OPND ra_alloc_st(int st_num)
{
    (void)st_num;
    return ra_alloc_fpr(0);
}

IR2_OPND ra_alloc_mda(void)
{
    return ra_ir2_opnd;
}
