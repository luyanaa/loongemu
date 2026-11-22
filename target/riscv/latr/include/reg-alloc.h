#ifndef LATR_REG_ALLOC_H
#define LATR_REG_ALLOC_H

#include "qemu/osdep.h"
#include "ir2.h"
#include "reg-map.h"

extern IR2_OPND env_ir2_opnd;
extern IR2_OPND zero_ir2_opnd;
extern IR2_OPND ra_ir2_opnd, tp_ir2_opnd, sp_ir2_opnd;
extern IR2_OPND a0_ir2_opnd, a1_ir2_opnd, a2_ir2_opnd, a3_ir2_opnd;
extern IR2_OPND a4_ir2_opnd, a5_ir2_opnd, a6_ir2_opnd, a7_ir2_opnd;
extern IR2_OPND t0_ir2_opnd, t1_ir2_opnd, t2_ir2_opnd, t3_ir2_opnd;
extern IR2_OPND t4_ir2_opnd, t5_ir2_opnd, t6_ir2_opnd, t7_ir2_opnd;
extern IR2_OPND t8_ir2_opnd, r21_ir2_opnd, fp_ir2_opnd;
extern IR2_OPND s0_ir2_opnd, s1_ir2_opnd, s2_ir2_opnd, s3_ir2_opnd;
extern IR2_OPND s4_ir2_opnd, s5_ir2_opnd, s6_ir2_opnd, s7_ir2_opnd;
extern IR2_OPND s8_ir2_opnd;
extern IR2_OPND fsmask_ir2_opnd;
extern IR2_OPND fcsr_ir2_opnd, fcsr1_ir2_opnd, fcsr2_ir2_opnd, fcsr3_ir2_opnd;
extern IR2_OPND fcc0_ir2_opnd, fcc1_ir2_opnd, fcc2_ir2_opnd, fcc3_ir2_opnd;
extern IR2_OPND fcc4_ir2_opnd, fcc5_ir2_opnd, fcc6_ir2_opnd, fcc7_ir2_opnd;
extern IR2_OPND scr0_ir2_opnd, scr1_ir2_opnd, scr2_ir2_opnd, scr3_ir2_opnd;

extern const int reg_itemp_map[];
extern const int reg_itemp_reverse_map[];
extern const int reg_ftemp_map[];
extern const int reg_ftemp_reverse_map[];
extern const int reg_statics_map[];

#define INIT_RA(type, reg) ((IR2_OPND){._type = type, ._reg_num = reg, ._assist = 0})
#define ra_free_temp(opnd) do { \
    if (ir2_opnd_is_itemp(&(opnd))) ra_free_itemp((opnd)._reg_num); \
    else if (ir2_opnd_is_ftemp(&(opnd))) ra_free_ftemp((opnd)._reg_num); \
    else lsassertm(0, "attempt to free non-temp register"); \
} while (0)
#define ra_free_temp_auto(opnd) do { \
    if (ir2_opnd_is_itemp(&(opnd))) ra_free_itemp((opnd)._reg_num); \
    else if (ir2_opnd_is_ftemp(&(opnd))) ra_free_ftemp((opnd)._reg_num); \
} while (0)

/*
 * The shared `target/arm/lata/ir2/ir2.c` classifies FPR operands through this
 * helper. LATA resolves it against its per-context reverse map; LATR only has the
 * static map below, and this header shadows the ARM `reg-alloc.h`, so the same
 * contract is provided here. `-1` is INVALID_TEMP on both sides.
 */
static inline int lata_ftemp_slot_for_phys(int phys)
{
    if (phys < 0 || phys >= 32) {
        return -1;
    }
    return reg_ftemp_reverse_map[phys];
}

IR2_OPND get_la_ir2_opnd(int la_reg_num);
IR2_OPND ra_alloc_gpr(int index);
IR2_OPND ra_alloc_fpr(int index);
IR2_OPND ra_alloc_mmx(int index);
IR2_OPND ra_alloc_xmm(int index);
IR2_OPND ra_alloc_statics(int index);
IR2_OPND ra_alloc_trace(int index);
IR2_OPND ra_alloc_scr(int index);
IR2_OPND ra_alloc_itemp(void);
IR2_OPND ra_alloc_ftemp(void);
IR2_OPND ra_alloc_num_4095(void);
void ra_free_num_4095(IR2_OPND opnd);
void ra_free_all(void);
void ra_free_itemp(int phy_id);
void ra_free_ftemp(int phy_id);
IR2_OPND ra_alloc_st(int index);
IR2_OPND ra_alloc_mda(void);

#endif
