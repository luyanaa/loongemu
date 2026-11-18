#ifndef LATA_TARGET_ARM_CPU_H
#define LATA_TARGET_ARM_CPU_H
#include "qemu/osdep.h"
#include "cpu.h"

typedef struct ARMCPRegInfo ARMCPRegInfo;

typedef struct ARMCPU {
    void *cp_regs;
    uint32_t dcz_blocksize;
    struct { uint64_t mvfr0; } isar;
} ARMCPU;

typedef struct CPUARMState {
    uint64_t xregs[32];
    uint64_t pc;
    struct {
        uint64_t zregs[32];
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
} CPUARMState;

#define ARM_VFP_FPSCR 1
#undef env_archcpu
#define env_archcpu(env) (&(env)->arm_cpu)
#endif
