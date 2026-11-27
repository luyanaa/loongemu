/* S-mode guest for the boot smoke's SBI phase (no firmware, no OpenSBI).
 *
 * A kernel booted without firmware asks its supervisor for SBI services
 * through `ecall` from S-mode. This guest exercises the two that matter first
 * for a console bring-up - a BASE probe (does the SBI layer answer at all) and
 * a DBCN byte write (does the answer reach the UART) - then writes the UART THR
 * directly so the two paths are distinguishable in the log, and shuts the
 * machine down through the SiFive test device.
 *
 * Loaded as the machine kernel at +2 MiB, so it runs at 0x80200000 in S-mode
 * with a0=hartid / a1=DTB from latr_machine_boot(); it only uses t0/t1 and the
 * ABI argument registers, so the handoff registers stay inspectable.
 */
    .section .text
    .globl _start
_start:
    /* BASE.get_spec_version: a0 must be SUCCESS, a1 the 2.0 version. */
    li      a7, 0x10
    li      a6, 0
    ecall
    bnez    a0, fail
    li      t0, 0x02000000
    bne     a1, t0, fail

    /* DBCN.write_byte('S'): a0 = byte, a1 = 1 (length). */
    li      a7, 0x4442434e
    li      a6, 2
    li      a0, 'S'
    li      a1, 1
    ecall
    bnez    a0, fail

    /* UART0 THR directly, so 'S' (SBI) and 'T' (MMIO) are both in the log. */
    li      t0, 0x10000000
    li      t1, 'T'
    sb      t1, 0(t0)

    li      t0, 0x00100000
    li      t1, 0x5555
    sw      t1, 0(t0)
1:  j       1b

fail:
    li      t0, 0x10000000
    li      t1, 'F'
    sb      t1, 0(t0)
    li      t0, 0x00100000
    li      t1, 0x3333
    sw      t1, 0(t0)
2:  j       2b
