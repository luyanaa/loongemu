#ifndef LATA_CPREGS_H
#define LATA_CPREGS_H
#include "qemu/osdep.h"
#include "target/arm/cpu.h"

#define ARM_CP_RAISES_EXC (1U << 0)
#define ARM_CP_IO         (1U << 1)
#define ARM_CP_CONST      (1U << 2)
#define ARM_CP_FPU        (1U << 3)
#define ARM_CP_SVE        (1U << 4)
#define ARM_CP_SME        (1U << 5)
#define ARM_CP_SPECIAL_MASK (0xfU)
#define ARM_CP_NOP         (1U << 6)
#define ARM_CP_NZCV        (1U << 7)
#define ARM_CP_CURRENTEL   (1U << 8)
#define ARM_CP_DC_ZVA      (1U << 9)
#define ARM_CP_DC_GVA      (1U << 10)
#define ARM_CP_DC_GZVA     (1U << 11)

struct ARMCPRegInfo {
    const char *name;
    int type;
    int fieldoffset;
    uint64_t resetvalue;
    void *accessfn;
    bool fgt;
    void (*readfn)(void);
    void (*writefn)(void);
};

const ARMCPRegInfo *get_arm_cp_reginfo(void *regs, uint32_t key);
#endif
