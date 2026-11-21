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
} CPUARMState;

#define ARM_VFP_FPSCR 1
#undef env_archcpu
#define env_archcpu(env) (&(env)->arm_cpu)
#endif
