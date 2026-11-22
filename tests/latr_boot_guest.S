/*
 * LATR firmware smoke guest.
 *
 * Uses the QEMU fw_cfg DMA interface to select/read the SIGNATURE item, then
 * prints "FW" through the 16550 UART and requests test exit.  The same raw
 * image runs from RAM and from the pflash code slot (execute-in-place through
 * the MMIO bus); all addresses are absolute so no relocation is required.
 */
    .section .text
    .globl _start
_start:
    lui     t0, 0x10100              /* fw_cfg base */
    li      t1, 0x80001000           /* DMA descriptor (zero-extended) */
    li      t2, 8                    /* SELECT */
    sw      t2, 0(t1)
    sw      zero, 4(t1)
    sd      zero, 8(t1)
    sd      t1, 16(t0)               /* trigger DMA select */
1:
    lw      t2, 0(t1)
    bnez    t2, 1b

    li      t2, 2                    /* READ */
    sw      t2, 0(t1)
    li      t2, 4
    sw      t2, 4(t1)
    li      t2, 0x80002000
    sd      t2, 8(t1)
    sd      t1, 16(t0)               /* trigger DMA read */
2:
    lw      t2, 0(t1)
    bnez    t2, 2b

    li      t2, 0x80002000
    lw      t3, 0(t2)
    li      t4, 0x554d4551           /* "QEMU" raw bytes, little-endian load */
    bne     t3, t4, fail

    li      t5, 0x10000000           /* UART THR */
    li      t6, 'F'
    sb      t6, 0(t5)
    li      t6, 'W'
    sb      t6, 0(t5)

    li      t5, 0x100000
    li      t6, 0x5555
    sw      t6, 0(t5)
3:
    j       3b

fail:
    li      t5, 0x10000000
    li      t6, 'E'
    sb      t6, 0(t5)
    li      t5, 0x100000
    li      t6, 0xdead
    sw      t6, 0(t5)
4:
    j       4b
