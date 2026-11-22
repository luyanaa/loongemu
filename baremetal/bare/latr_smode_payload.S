/*
 * Minimal S-mode payload for the LATR OpenSBI smoke test.
 *
 * OpenSBI (fw_dynamic) hands off to the next stage in S-mode at
 * LATR_MACHINE_KERNEL_BASE (0x80200000) with a0 = hartid and a1 = DTB address,
 * exactly like a Linux kernel's boot protocol. Putting a real payload there
 * turns the smoke test from "OpenSBI printed a banner" into a proof of the
 * whole M-mode -> S-mode transition:
 *
 *   1. S-mode runs the payload.
 *   2. The payload prints through the SBI legacy console (a7=1, a6=0, ecall),
 *      which traps to M-mode, lands in OpenSBI's ecall handler, and only then
 *      reaches the 16550 model.  A byte coming out the other end proves the
 *      S -> M ecall path, OpenSBI's trap save/restore, and the UART all work.
 *   3. The payload shuts the machine down through SBI SRST (a7=0x53525354) so
 *      the harness observes a clean exit instead of a livelock.
 *
 * Everything lives in one .text section so `objcopy -O binary` emits a flat
 * image with no gaps, and all addressing is PC-relative (`la`), so the image
 * only works when linked at the address it is loaded at (0x80200000).
 */

    .section .text
    .globl _start
_start:
    mv      s0, a0                  /* s0 = hartid                            */
    mv      s1, a1                  /* s1 = DTB physical address              */

    la      s2, msg_up
1:  lbu     a0, 0(s2)
    beqz    a0, 2f
    addi    s2, s2, 1
    jal     ra, putc
    j       1b

2:  srli    a0, s0, 4               /* print hartid as two hex digits         */
    andi    a0, a0, 15
    jal     ra, puthex
    andi    a0, s0, 15
    jal     ra, puthex

    la      s2, msg_ok
3:  lbu     a0, 0(s2)
    beqz    a0, 4f
    addi    s2, s2, 1
    jal     ra, putc
    j       3b

4:  li      a7, 0x53525354          /* SBI SRST: system_reset(0, 0) = shutdown */
    li      a6, 0
    li      a0, 0
    li      a1, 0
    ecall

    /* SRST may be unimplemented on this platform; drive the test device
     * directly so a run still terminates deterministically. */
    li      t0, 0x00100000
    li      t1, 0x5555
    sw      t1, 0(t0)
5:  j       5b

/* puthex: emit the low nibble of a0 as one hex digit (tail-calls putc). */
puthex:
    la      t0, hexdig
    add     t0, t0, a0
    lbu     a0, 0(t0)
    j       putc

/* putc: legacy SBI console putchar. Clobbers a0/a6/a7. */
putc:
    li      a7, 1
    li      a6, 0
    ecall
    ret

hexdig: .asciz  "0123456789abcdef"
msg_up: .asciz  "\r\n[latr] smode up hart="
msg_ok: .asciz  "\r\n[latr] smode ok\r\n"
