/*
 * SMC + shootdown stress (design 9.3b, last item): rewrite, FENCE.I, execute,
 * repeat - with the other hart polling.
 *
 * hart 0 rewrites the target's `li a0, N` constant and fences, once per
 * iteration; hart 1 must observe value N+1 on iteration N. Neither side uses a
 * timer or an interrupt: the handshake is two words in the data page, so the
 * interleaving is driven by the guest, not by luck.
 *
 * Data page 0x80001000:
 *   +0x00 ready    hart 1 has executed the target once (old code)
 *   +0x08 ok       observations that matched the published value
 *   +0x10 bad      observations that did not
 *   +0x18 published  iteration hart 0 has rewritten and fenced
 *   +0x28 seen     last iteration hart 1 observed
 *
 * a0 carries the hart id on entry (LATR_Machine boot contract).
 */
    .option norvc
    .section .text
    .globl _start
_start:
    li    t0, 0x80001000
    bnez  a0, hart1

/* ------------------------------- hart 0 ------------------------------- */
    /* Wait until hart 1 has executed the old code, so every iteration below is
     * a genuine stale-translation case. */
wait_ready:
    ld    t1, 0(t0)
    beqz  t1, wait_ready
    li    t3, 0
    li    t6, 32
loop0:
    addi  t3, t3, 1                /* value = iteration + 1 */
    li    t1, 0x80002000
    addi  t2, t3, 1
    slli  t2, t2, 20
    li    t4, 0x00000513           /* addi a0, zero, imm */
    or    t2, t2, t4
    sw    t2, 0(t1)
    li    t4, 0x00008067           /* ret */
    sw    t4, 4(t1)
    fence.i
    sd    t3, 24(t0)
wait_seen:
    ld    t4, 40(t0)
    bne   t4, t3, wait_seen
    blt   t3, t6, loop0
    li    t1, 0x100000
    li    t2, 0x5555
    sw    t2, 0(t1)
1:  j     1b

/* ------------------------------- hart 1 ------------------------------- */
hart1:
    li    t1, 0x80002000
    jalr  ra, t1, 0                /* translate the old code first */
    li    t2, 1
    sd    t2, 0(t0)
    li    t3, 0
    li    t6, 32
loop1:
    ld    t4, 24(t0)
    beq   t4, t3, loop1            /* wait for a new published iteration */
    li    t1, 0x80002000
    jalr  ra, t1, 0
    addi  t5, t4, 1
    bne   a0, t5, bad
    ld    t5, 8(t0)
    addi  t5, t5, 1
    sd    t5, 8(t0)
    j     ack
bad:
    ld    t5, 16(t0)
    addi  t5, t5, 1
    sd    t5, 16(t0)
ack:
    mv    t3, t4
    sd    t3, 40(t0)
    blt   t4, t6, loop1
2:  j     2b

/* ------------------------- rewritten code ----------------------------- */
    .org 0x2000
target:
    li    a0, 1
    jalr  zero, ra, 0
