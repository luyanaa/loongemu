/*
 * Software emulation of the LBT x86 subset.
 *
 * QEMU does not execute the LoongArch binary-translation extension, so the
 * translated code's x86* instructions and flag moves trap with #INE.  This
 * module installs an #INE handler that decodes those few encodings in place
 * and advances ERA, which is enough for the translator's output to run.
 *
 * Shared by the bare-metal harnesses; install it once before running any
 * translated block.
 */
#ifndef LOONGEMU_BARE_LBT_EMU_H
#define LOONGEMU_BARE_LBT_EMU_H

#include <stdint.h>

typedef struct {
    uint64_t scr[4];
    uint32_t eflags;
    uint32_t top;
    int tm;
} SwLbt;

extern SwLbt sw_lbt;

/* Point CSR.EENTRY at the emulation stub. */
void lbt_emu_install(void);

/* Fast-path LBT emulator called directly from tb_sys_handle_trap on EXC_IPE. */
void lbt_handle_fast(uint64_t *regs, uint64_t era);

#endif
