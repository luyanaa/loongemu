#include "latr.h"

#define GPR_OPND(reg) { ._type = IR2_OPND_GPR, ._reg_num = (reg), ._assist = 0 }
#define FPR_OPND(reg) { ._type = IR2_OPND_FPR, ._reg_num = (reg), ._assist = 0 }
#define CCR_OPND(reg) { ._type = IR2_OPND_CC, ._reg_num = (reg), ._assist = 0 }
#define FCSR_OPND(reg) { ._type = IR2_OPND_FCSR, ._reg_num = (reg), ._assist = 0 }
#define SCR_OPND(reg) { ._type = IR2_OPND_SCR, ._reg_num = (reg), ._assist = 0 }

IR2_OPND env_ir2_opnd = GPR_OPND(la_s8);
IR2_OPND zero_ir2_opnd = GPR_OPND(la_zero);
IR2_OPND ra_ir2_opnd = GPR_OPND(la_ra);
IR2_OPND tp_ir2_opnd = GPR_OPND(la_tp);
IR2_OPND sp_ir2_opnd = GPR_OPND(la_sp);
IR2_OPND a0_ir2_opnd = GPR_OPND(la_a0);
IR2_OPND a1_ir2_opnd = GPR_OPND(la_a1);
IR2_OPND a2_ir2_opnd = GPR_OPND(la_a2);
IR2_OPND a3_ir2_opnd = GPR_OPND(la_a3);
IR2_OPND a4_ir2_opnd = GPR_OPND(la_a4);
IR2_OPND a5_ir2_opnd = GPR_OPND(la_a5);
IR2_OPND a6_ir2_opnd = GPR_OPND(la_a6);
IR2_OPND a7_ir2_opnd = GPR_OPND(la_a7);
IR2_OPND t0_ir2_opnd = GPR_OPND(la_t0);
IR2_OPND t1_ir2_opnd = GPR_OPND(la_t1);
IR2_OPND t2_ir2_opnd = GPR_OPND(la_t2);
IR2_OPND t3_ir2_opnd = GPR_OPND(la_t3);
IR2_OPND t4_ir2_opnd = GPR_OPND(la_t4);
IR2_OPND t5_ir2_opnd = GPR_OPND(la_t5);
IR2_OPND t6_ir2_opnd = GPR_OPND(la_t6);
IR2_OPND t7_ir2_opnd = GPR_OPND(la_t7);
IR2_OPND t8_ir2_opnd = GPR_OPND(la_t8);
IR2_OPND r21_ir2_opnd = GPR_OPND(la_r21);
IR2_OPND fp_ir2_opnd = GPR_OPND(la_fp);
IR2_OPND s0_ir2_opnd = GPR_OPND(la_s0);
IR2_OPND s1_ir2_opnd = GPR_OPND(la_s1);
IR2_OPND s2_ir2_opnd = GPR_OPND(la_s2);
IR2_OPND s3_ir2_opnd = GPR_OPND(la_s3);
IR2_OPND s4_ir2_opnd = GPR_OPND(la_s4);
IR2_OPND s5_ir2_opnd = GPR_OPND(la_s5);
IR2_OPND s6_ir2_opnd = GPR_OPND(la_s6);
IR2_OPND s7_ir2_opnd = GPR_OPND(la_s7);
IR2_OPND s8_ir2_opnd = GPR_OPND(la_s8);
IR2_OPND fsmask_ir2_opnd = FPR_OPND(15);
IR2_OPND fcsr_ir2_opnd = FCSR_OPND(0);
IR2_OPND fcsr1_ir2_opnd = FCSR_OPND(1);
IR2_OPND fcsr2_ir2_opnd = FCSR_OPND(2);
IR2_OPND fcsr3_ir2_opnd = FCSR_OPND(3);
IR2_OPND fcc0_ir2_opnd = CCR_OPND(0);
IR2_OPND fcc1_ir2_opnd = CCR_OPND(1);
IR2_OPND fcc2_ir2_opnd = CCR_OPND(2);
IR2_OPND fcc3_ir2_opnd = CCR_OPND(3);
IR2_OPND fcc4_ir2_opnd = CCR_OPND(4);
IR2_OPND fcc5_ir2_opnd = CCR_OPND(5);
IR2_OPND fcc6_ir2_opnd = CCR_OPND(6);
IR2_OPND fcc7_ir2_opnd = CCR_OPND(7);
IR2_OPND scr0_ir2_opnd = SCR_OPND(0);
IR2_OPND scr1_ir2_opnd = SCR_OPND(1);
IR2_OPND scr2_ir2_opnd = SCR_OPND(2);
IR2_OPND scr3_ir2_opnd = SCR_OPND(3);

const int reg_itemp_map[6] = {la_t0, la_t1, la_t2, la_t3, la_t4, la_t5};
const int reg_itemp_reverse_map[32] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    ITEMP0, ITEMP1, ITEMP2, ITEMP3, ITEMP4, ITEMP5,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};
const int reg_ftemp_map[6] = {24, 25, 26, 27, 28, 29};
const int reg_ftemp_reverse_map[32] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    FTEMP0, FTEMP1, FTEMP2, FTEMP3, FTEMP4, FTEMP5, -1, -1
};
const int reg_statics_map[1] = {la_s8};

/* LATR has no vector cache window, so every f-temp stays available. */
int ra_ftemp_pool_size(void)
{
    return (int)(sizeof(reg_ftemp_map) / sizeof(int));
}

static int allocate_temp(uint16_t *status, const int *map, int count)
{
    for (int i = 0; i < count; i++) {
        uint16_t bit = (uint16_t)(1u << i);
        if ((*status & bit) == 0) {
            *status |= bit;
            return map[i];
        }
    }
    lsassertm(0, "LATR temporary register exhaustion");
    return map[0];
}

static void free_temp(uint16_t *status, const int *reverse, int reg)
{
    lsassertm(reg >= 0 && reg < 32, "invalid temporary register %d", reg);
    int index = reverse[reg];
    lsassertm(index >= 0 && index < 6 && (*status & (1u << index)),
              "free of unallocated temporary register %d", reg);
    *status &= (uint16_t)~(1u << index);
}

IR2_OPND ra_alloc_itemp(void)
{
    uint16_t full = (uint16_t)((1u << itemp_status_num) - 1);
    if ((lsenv->tr_data->itemp_status & full) == full) {
        latr_regcache_evict_itemp();
    }
    int reg = allocate_temp(&lsenv->tr_data->itemp_status,
                            reg_itemp_map, itemp_status_num);
    return ir2_opnd_new(IR2_OPND_GPR, reg);
}

IR2_OPND ra_alloc_ftemp(void)
{
    int count = ftemp_status_num;
    uint16_t full = (uint16_t)((1u << count) - 1);
    if ((lsenv->tr_data->ftemp_status & full) == full) {
        latr_regcache_evict_ftemp();
    }
    int reg = allocate_temp(&lsenv->tr_data->ftemp_status,
                            reg_ftemp_map, count);
    return ir2_opnd_new(IR2_OPND_FPR, reg);
}

void ra_free_itemp(int reg)
{
    free_temp(&lsenv->tr_data->itemp_status, reg_itemp_reverse_map, reg);
}

void ra_free_ftemp(int reg)
{
    free_temp(&lsenv->tr_data->ftemp_status, reg_ftemp_reverse_map, reg);
}

void ra_free_all(void)
{
    lsenv->tr_data->itemp_status = 0;
    lsenv->tr_data->ftemp_status = 0;
}

IR2_OPND get_la_ir2_opnd(int reg)
{
    lsassertm(reg >= 0 && reg < 32, "invalid LoongArch register %d", reg);
    return ir2_opnd_new(IR2_OPND_GPR, reg);
}

IR2_OPND ra_alloc_gpr(int reg) { return ir2_opnd_new(IR2_OPND_GPR, reg); }
IR2_OPND ra_alloc_fpr(int reg) { return ir2_opnd_new(IR2_OPND_FPR, reg); }
IR2_OPND ra_alloc_mmx(int reg) { return ir2_opnd_new(IR2_OPND_FPR, reg); }
IR2_OPND ra_alloc_xmm(int reg) { return ir2_opnd_new(IR2_OPND_FPR, 16 + reg); }
IR2_OPND ra_alloc_statics(int reg)
{
    lsassertm(reg >= 0 && reg < 1, "invalid LATR static register %d", reg);
    return ir2_opnd_new(IR2_OPND_GPR, reg_statics_map[reg]);
}
IR2_OPND ra_alloc_trace(int reg) { return ir2_opnd_new(IR2_OPND_GPR, la_zero + reg); }
IR2_OPND ra_alloc_scr(int reg) { return ir2_opnd_new(IR2_OPND_SCR, reg); }
IR2_OPND ra_alloc_st(int reg) { return ra_alloc_fpr(reg); }
IR2_OPND ra_alloc_mda(void) { return ra_ir2_opnd; }
IR2_OPND ra_alloc_num_4095(void) { return ra_alloc_itemp(); }
void ra_free_num_4095(IR2_OPND opnd) { ra_free_itemp(opnd._reg_num); }
