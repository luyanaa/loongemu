/*
 * Guest-side half of the bare-metal SMP scenario: a two-hart M-mode program
 * that LATR translates and runs through the real machine model. The driver
 * (baremetal/bare/latr_smp_test.c) loads this flat image at 0x80000000 and
 * checks the result words it leaves in RAM.
 *
 * Both harts enter at _start with a0 = hartid (LATR_Machine boot contract).
 * Shared data page: 0x80001000.
 *   0x08 counter      AMO counter           expect 32 (4 * 8)
 *   0x10 lrsc_value   LR/SC increments      expect 16 (no failed SC)
 *   0x18 early_result hart 1's call before the rewrite   expect 1
 *   0x20 ipi_count    software interrupts taken        expect 1
 *   0x28 h0_result    hart 0's call after fence.i      expect 2
 *   0x30 h1_result    hart 1's call after fence.i      expect 2
 *   0x38 exit_marker  copy of h1_result, published before the exit write
 *
 * The interesting part is the order: hart 1 must translate the OLD `target`
 * before hart 0 rewrites it and executes FENCE.I, and after the IPI its call
 * must land on the NEW code. That can only pass if the cross-core
 * FENCE.I shootdown actually invalidated hart 1's cached block.
 *
 * Instructions are assembled with the RISC-V toolchain and objcopy'd to a
 * flat binary by the Makefile, exactly like latr_smode_payload.S, so the
 * image has no relocations left when it is embedded.
 */
    .section .text
    .globl _start
_start:
    li    t0, 0x80001000
    bnez  a0, hart1

/* ---------------------------- hart 0 ---------------------------------- */
    /* AMO/LR-SC forms take no immediate offset, so the two hot addresses get
     * their own base registers. */
    addi  t5, t0, 8                /* counter */
    addi  t6, t0, 16               /* lrsc_value */

    /* Shared counter through AMOs: 4 * 8 = 32. */
    li    t1, 8
    amoadd.d s0, t1, (t5)
    amoadd.d s0, t1, (t5)
    amoadd.d zero, t1, (t5)
    amoadd.d zero, t1, (t5)

    /* LR/SC increment 16 times; a failed SC retries, it never gives up. */
    li    t2, 0                    /* successes */
    li    t3, 16
lrsc_loop:
    lr.d  t1, (t6)
    addi  t1, t1, 1
    sc.d  t4, t1, (t6)
    bnez  t4, lrsc_loop
    addi  t2, t2, 1
    blt   t2, t3, lrsc_loop
    sd    t2, (t6)

    /* Wait until hart 1 has translated the old target. */
wait_early:
    ld    t1, 24(t0)
    beqz  t1, wait_early

    /* Replace target with `li a0, 2; ret` and publish it to instruction
     * fetch. The old target was `li a0, 1; ret`. */
    li    t1, 0x80002000
    li    t2, 0x00200513           /* addi a0, zero, 2 */
    sw    t2, 0(t1)
    li    t2, 0x00008067           /* jalr zero, ra, 0 */
    sw    t2, 4(t1)
    fence.i

    /* Software interrupt to hart 1 through the CLINT. */
    li    t1, 0x02000004
    li    t2, 1
    sw    t2, 0(t1)

wait_ipi:
    ld    t1, 32(t0)
    beqz  t1, wait_ipi

    /* Hart 0's own call must now see the new code too. */
    li    t1, 0x80002000
    jalr  ra, t1, 0
    sd    a0, 40(t0)

    /* Publish hart 1's post-fence result and power the machine off. */
    ld    t1, 48(t0)
    sd    t1, 56(t0)
    li    t1, 0x100000
    li    t2, 0x5555
    sw    t2, 0(t1)
1:  j     1b

/* ---------------------------- hart 1 ---------------------------------- */
hart1:
    la    t1, h1_handler
    csrw  mtvec, t1
    csrsi mstatus, 8              /* MIE */
    csrsi mie, 8                  /* MSIE */

    /* Translate the old target before hart 0 rewrites it. */
    li    t1, 0x80002000
    jalr  ra, t1, 0
    sd    a0, 24(t0)

h1_idle:
    wfi
    j     h1_idle

/* Trap entry for the software interrupt: count it, acknowledge the MSIP, then
 * call target again. `mret` returns to the interrupted wfi loop. */
h1_handler:
    addi  t5, t0, 32
    li    t1, 1
    amoadd.d zero, t1, (t5)
    li    t1, 0x02000004
    sw    zero, 0(t1)
    li    t1, 0x80002000
    jalr  ra, t1, 0
    sd    a0, 48(t0)
    mret

/* ------------------------- rewritten code ----------------------------- */
    .org 0x2000
target:
    li    a0, 1
    jalr  zero, ra, 0
