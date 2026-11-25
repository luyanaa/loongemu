/*
 * Firmware for the scripted interleaving scenarios (design 9.2/9.3): the
 * driver, not a guest program, decides which hart runs and when, so both harts
 * only need a state that is easy to place at a chosen point.
 *
 *   hart 0: a three-instruction spin, so one TB is one instruction and the
 *           driver always stops it at a boundary.
 *   hart 1: wfi, then the same spin. The first TB parks it in WFI, which is the
 *           state the wake scenario needs; every later TB proves it resumed.
 *
 * a0 carries the hart id on entry (LATR_Machine boot contract).
 */
    .option norvc
    .section .text
    .globl _start
_start:
    bnez  a0, hart1
    nop
    nop
    j     _start
hart1:
    wfi
    nop
    j     hart1
