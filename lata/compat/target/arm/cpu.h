#ifndef LATA_TARGET_ARM_CPU_H
#define LATA_TARGET_ARM_CPU_H
#include "qemu/osdep.h"
#include "cpu.h"

typedef struct ARMCPRegInfo ARMCPRegInfo;

typedef struct ARMISAState {
    uint64_t mvfr0;
    /* Zero-initialized CPU models do not implement FEAT_SVE. */
    bool aa64_sve;
    /* The standalone LSE test opts in explicitly; default guests lack FEAT_LSE. */
    bool aa64_lse;
} ARMISAState;

typedef struct ARMCPU {
    void *cp_regs;
    uint32_t dcz_blocksize;
    ARMISAState isar;
} ARMCPU;

static inline void arm_cpu_set_aa64_sve(ARMCPU *cpu, bool enabled)
{
    if (cpu != NULL) {
        cpu->isar.aa64_sve = enabled;
    }
}

static inline bool arm_isar_has_aa64_sve(const ARMISAState *isar)
{
    return isar != NULL && isar->aa64_sve;
}

static inline void arm_cpu_set_aa64_lse(ARMCPU *cpu, bool enabled)
{
    if (cpu != NULL) {
        cpu->isar.aa64_lse = enabled;
    }
}

static inline bool arm_isar_has_aa64_lse(const ARMISAState *isar)
{
    return isar != NULL && isar->aa64_lse;
}

#define ARM_SVE_MIN_VL_BITS 128u
#define ARM_SVE_MAX_VL_BITS 2048u
#define ARM_SVE_MAX_VL_BYTES (ARM_SVE_MAX_VL_BITS / 8u)
#define ARM_SVE_NUM_ZREGS 32u
#define ARM_SVE_NUM_PREGS 16u
#define ARM_SVE_MAX_PRED_BYTES (ARM_SVE_MAX_VL_BYTES / 8u)

typedef struct CPUARMState {
    uint64_t xregs[32];
    uint64_t pc;
    struct {
        /* Keep legacy scalar/FPSR fields ahead of scalable vector state. */
        uint64_t xregs[16];
        uint64_t qc[4];
        struct { int float_rounding_mode; } fp_status;
    } vfp;
    uint32_t CF, VF, ZF, NF;
    uint32_t pstate;
    uint64_t exclusive_addr;
    uint64_t features[8];
    void *tr_data;
    uint64_t *pc_map_cache;
    uint64_t exclusive_memdata;
    ARMCPU arm_cpu;
    /* Appended to preserve the legacy scalar/FPSR offsets. */
    struct {
        /* Shared Z storage; each V register is its low 128-bit view. */
        _Alignas(16) uint8_t zregs[ARM_SVE_NUM_ZREGS][ARM_SVE_MAX_VL_BYTES];
        /* One packed predicate bit per vector byte. */
        uint8_t pregs[ARM_SVE_NUM_PREGS][ARM_SVE_MAX_PRED_BYTES];
        uint8_t ffr[ARM_SVE_MAX_PRED_BYTES];
        uint16_t active_vl_bits;
    } sve;
    /* Transient lowerer cache storage; never architectural or context-saved. */
    _Alignas(16) uint8_t sve_expanded_mask_scratch[ARM_SVE_MAX_VL_BYTES];
    /* Architectural SVE access control & standalone runtime trap/fault state. */
    struct {
        uint64_t cpacr_el1;
        uint64_t cptr_el2;
        uint64_t cptr_el3;
        uint32_t svcr;
        bool access_control_enforced;
        bool cptr_el3_enforced;
        uint64_t fault_mem_limit; /* 0 = default 0x10000000 bare-metal RAM limit */
        uint32_t last_exception_index;
        uint32_t last_exception_syndrome;
        uint32_t last_exception_target_el;
    } sve_ctrl;
} CPUARMState;

static inline void arm_sve_set_cpacr_el1_zen(CPUARMState *env, unsigned zen)
{
    if (env == NULL) {
        return;
    }
    env->sve_ctrl.cpacr_el1 =
        (env->sve_ctrl.cpacr_el1 & ~(UINT64_C(3) << 16)) |
        ((uint64_t)(zen & 3u) << 16);
    env->sve_ctrl.access_control_enforced = true;
}

static inline void arm_sve_set_cptr_el2_tz(CPUARMState *env, bool trap_sve)
{
    if (env == NULL) {
        return;
    }
    if (trap_sve) {
        env->sve_ctrl.cptr_el2 |= (UINT64_C(1) << 8);
    } else {
        env->sve_ctrl.cptr_el2 &= ~(UINT64_C(1) << 8);
    }
    env->sve_ctrl.access_control_enforced = true;
}

static inline void arm_sve_set_cptr_el3_ez(CPUARMState *env, bool enable_sve)
{
    if (env == NULL) {
        return;
    }
    if (enable_sve) {
        env->sve_ctrl.cptr_el3 |= (UINT64_C(1) << 8);
    } else {
        env->sve_ctrl.cptr_el3 &= ~(UINT64_C(1) << 8);
    }
    env->sve_ctrl.cptr_el3_enforced = true;
    env->sve_ctrl.access_control_enforced = true;
}

/*
 * Evaluate the target exception level (0 = allowed, 1..3 = EL to trap to)
 * from CPACR_EL1.ZEN, CPTR_EL2.TZ, and CPTR_EL3.EZ.
 */
static inline int arm_sve_evaluate_excp_el(const CPUARMState *env,
                                           int current_el)
{
    if (env == NULL || !env->sve_ctrl.access_control_enforced) {
        return 0;
    }
    if (current_el <= 1) {
        unsigned zen = (unsigned)((env->sve_ctrl.cpacr_el1 >> 16) & 3u);
        if (zen == 0u || zen == 2u || (current_el == 0 && zen == 1u)) {
            return 1;
        }
    }
    if (current_el <= 2 && (env->sve_ctrl.cptr_el2 & (UINT64_C(1) << 8))) {
        return 2;
    }
    if (env->sve_ctrl.cptr_el3_enforced &&
        !(env->sve_ctrl.cptr_el3 & (UINT64_C(1) << 8))) {
        return 3;
    }
    return 0;
}

#define ARM_VFP_FPSCR 1
#undef env_archcpu
#define env_archcpu(env) (&(env)->arm_cpu)
#endif
