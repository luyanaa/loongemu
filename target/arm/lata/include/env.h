#ifndef _ENV_H_
#define _ENV_H_

#include "target/arm/cpu.h"

static inline bool arm_sve_vl_is_valid(uint32_t vl_bits)
{
    return vl_bits >= ARM_SVE_MIN_VL_BITS &&
           vl_bits <= ARM_SVE_MAX_VL_BITS &&
           vl_bits % ARM_SVE_MIN_VL_BITS == 0;
}

static inline bool arm_sve_set_vl(CPUARMState *env, uint32_t vl_bits)
{
    if (env == NULL || !arm_sve_vl_is_valid(vl_bits)) {
        return false;
    }
    env->sve.active_vl_bits = (uint16_t)vl_bits;
    return true;
}

static inline size_t arm_sve_vl_bytes_for_bits(uint32_t vl_bits)
{
    return arm_sve_vl_is_valid(vl_bits) ? vl_bits / 8u : 0;
}

static inline size_t arm_sve_predicate_bytes_for_bits(uint32_t vl_bits)
{
    return arm_sve_vl_is_valid(vl_bits) ? vl_bits / 64u : 0;
}

static inline size_t arm_sve_active_vl_bytes(const CPUARMState *env)
{
    return env == NULL ? 0 :
           arm_sve_vl_bytes_for_bits(env->sve.active_vl_bits);
}

/* Storage views only; V-write upper-half semantics are not modeled here. */
static inline uint8_t *arm_sve_zreg(CPUARMState *env, unsigned reg)
{
    if (env == NULL || reg >= ARM_SVE_NUM_ZREGS) {
        return NULL;
    }
    return env->sve.zregs[reg];
}

static inline uint8_t *arm_sve_vreg(CPUARMState *env, unsigned reg)
{
    return arm_sve_zreg(env, reg);
}

static inline uint8_t *arm_sve_preg(CPUARMState *env, unsigned reg)
{
    if (env == NULL || reg >= ARM_SVE_NUM_PREGS) {
        return NULL;
    }
    return env->sve.pregs[reg];
}

static inline uint8_t *arm_sve_ffr(CPUARMState *env)
{
    return env == NULL ? NULL : env->sve.ffr;
}

static inline int env_offset_zreg(int num)
{
    if (num < 0 || num >= (int)ARM_SVE_NUM_ZREGS) {
        return -1;
    }
    return (int)offsetof(CPUARMState, sve.zregs) +
           num * (int)ARM_SVE_MAX_VL_BYTES;
}

static inline int env_offset_vreg(int num)
{
    return env_offset_zreg(num);
}

/* LISA VLD/VST use signed SI12 displacements; larger offsets need a base GPR. */
#define LATA_VMEM_SI12_MIN (-2048)
#define LATA_VMEM_SI12_MAX 2047

typedef struct {
    bool materialize_address;
    int displacement;
} LataVectorAddressPlan;

static inline LataVectorAddressPlan env_vector_address_plan(int offset)
{
    LataVectorAddressPlan plan;

    plan.materialize_address =
        offset < LATA_VMEM_SI12_MIN || offset > LATA_VMEM_SI12_MAX;
    plan.displacement = plan.materialize_address ? 0 : offset;
    return plan;
}

static inline int env_offset_gpr(int num)
{
    return offsetof(CPUARMState,xregs[num]);//cpu->xregs == gpr
}

static inline int env_offset_pc(void)
{
    return offsetof(CPUARMState,pc);
}

static inline int env_offset_fpr(int num)
{
    return env_offset_vreg(num);
}

static inline int env_offset_sve_preg(int num)
{
    if (num < 0 || num >= (int)ARM_SVE_NUM_PREGS) {
        return -1;
    }
    return (int)offsetof(CPUARMState, sve.pregs) +
           num * (int)ARM_SVE_MAX_PRED_BYTES;
}

static inline int env_offset_sve_ffr(void)
{
    return offsetof(CPUARMState, sve.ffr);
}

static inline int env_offset_sve_active_vl_bits(void)
{
    return offsetof(CPUARMState, sve.active_vl_bits);
}
static inline int env_offset_sve_expanded_mask_scratch(void)
{
    return (int)offsetof(CPUARMState, sve_expanded_mask_scratch);
}

static inline int env_offset_CF(void)
{
    return offsetof(CPUARMState,CF);
}

static inline int env_offset_VF(void)
{
    return offsetof(CPUARMState,VF);
}

static inline int env_offset_ZF(void)
{
    return offsetof(CPUARMState,ZF);
}

static inline int env_offset_NF(void)
{
    return offsetof(CPUARMState,NF);
}

static inline int env_offset_PSTATE(void)
{
    return offsetof(CPUARMState,pstate);
}

static inline int env_offset_FPCR(void)
{
    return offsetof(CPUARMState, vfp.xregs[ARM_VFP_FPCR]);
}

static inline int env_offset_fpsr(void)
{
    return offsetof(CPUARMState, vfp.xregs[ARM_VFP_FPSR]);
}

static inline int env_offset_FPSCR(void)
{
    return offsetof(CPUARMState, vfp.xregs[ARM_VFP_FPSCR]);
}

static inline int env_offset_fpstatus(void)
{
    return offsetof(CPUARMState, vfp.fp_status);
}

static inline int env_offset_RMODE(void)
{
    return offsetof(CPUARMState, vfp.fp_status.float_rounding_mode);
}

static inline int env_offset_QC(void)
{
    return offsetof(CPUARMState, vfp.qc[0]);
}

static inline int env_offset_Exclusive(void)
{
    return offsetof(CPUARMState,exclusive_addr);
}

#define env_offset(item) offsetof(CPUARMState, item)

#endif
