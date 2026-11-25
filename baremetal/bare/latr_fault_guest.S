/*
 * Guest half of the LATR fault differential (roadmap 7).
 *
 * The same flat RISC-V image runs on two engines: LATR (the machine translates
 * it and executes the emitted LoongArch) and qemu-system-riscv64 (native). Each
 * case takes a trap, the handler records (mcause, mepc, mtval), resumes after
 * the faulting instruction, and at the end the guest hashes the records and
 * prints `latr-fault-diff=<hex>`.
 *
 * What is hashed, and why not more:
 *   - cause and mepc for every case: the spec pins both;
 *   - mtval only for the three access faults, where the spec requires the
 *     faulting address. For illegal-instruction it is explicitly allowed to be
 *     the instruction or zero, so comparing it would flag a permitted
 *     difference - that is the kind of gate that gets disabled instead of read.
 *
 * M-mode only, no paging, `.option norvc` so mepc arithmetic stays 4-byte based.
 */
    .option norvc
    .section .text
    .globl _start
_start:
    la      t0, trap_entry
    csrw    mtvec, t0
    li      s1, 0              /* record index */
    la      s2, recs

    /* 1. load access fault above RAM, where neither engine maps anything.
     * (0x40000000 is *not* a safe choice: on QEMU virt it is a mapped window,
     * so the access succeeded and the fault case silently vanished - found by
     * this differential on its first run.) */
    la      s0, 1f
    li      t0, 0x90000000
    ld      t1, 0(t0)
1:
    /* 2. store access fault at the same address. */
    la      s0, 2f
    li      t0, 0x90000000
    sd      t1, 0(t0)
2:
    /* 3. instruction access fault: fetch from the unmapped address. */
    la      s0, 3f
    li      t0, 0x90000000
    jr      t0
3:
    /* 4. environment call from M-mode. */
    la      s0, 4f
    ecall
4:
    /* 5. illegal instruction. 0x7f is a reserved 32-bit opcode: all-zero would
     * be two RVC halfwords, and 0x0000 is a reserved C.ADDI4SPN, so the two
     * engines could legitimately disagree on where the instruction ends. */
    la      s0, 5f
    .word   0x0000007f
5:

    /* Hash the records: cause and mepc always, mtval for the access faults. */
    li      s3, 0
    li      s4, 5
    la      s2, recs
    li      s5, 0x9e3779b97f4a7c15
    li      s6, 0x100000001b3
hash_loop:
    ld      t1, 0(s2)          /* mcause */
    xor     s5, s5, t1
    mul     s5, s5, s6
    ld      t1, 8(s2)          /* mepc */
    xor     s5, s5, t1
    mul     s5, s5, s6
    li      t2, 3
    bgeu    s3, t2, hash_skip_mtval
    ld      t1, 16(s2)         /* mtval, access faults only */
    xor     s5, s5, t1
    mul     s5, s5, s6
hash_skip_mtval:
    addi    s2, s2, 24
    addi    s3, s3, 1
    bltu    s3, s4, hash_loop

    /* Print "latr-fault-diff=" + 16 hex digits to the 16550 at 0x10000000. */
    li      a0, 0x10000000
    la      t0, prefix
print_prefix:
    lbu     t1, 0(t0)
    beqz    t1, print_hex
    sb      t1, 0(a0)
    addi    t0, t0, 1
    j       print_prefix
print_hex:
    li      t3, 60
    la      t4, hexdigits
hex_loop:
    srl     t5, s5, t3
    andi    t5, t5, 15
    add     t6, t4, t5
    lbu     t6, 0(t6)
    sb      t6, 0(a0)
    addi    t3, t3, -4
    bgez    t3, hex_loop
    li      t1, 10
    sb      t1, 0(a0)

    /* Also print the raw records: a hash mismatch must be diagnosable from the
     * log of either engine, not by bisecting the guest. */
    li      s3, 0
    la      s2, recs
rec_loop:
    la      t0, rec_prefix
rec_prefix_loop:
    lbu     t1, 0(t0)
    beqz    t1, rec_body
    sb      t1, 0(a0)
    addi    t0, t0, 1
    j       rec_prefix_loop
rec_body:
    li      t1, '0'
    add     t1, t1, s3
    sb      t1, 0(a0)
    li      t1, '='
    sb      t1, 0(a0)
    ld      a2, 0(s2)
    jal     ra, puthex
    li      t1, ','
    sb      t1, 0(a0)
    ld      a2, 8(s2)
    jal     ra, puthex
    li      t1, ','
    sb      t1, 0(a0)
    ld      a2, 16(s2)
    jal     ra, puthex
    li      t1, 10
    sb      t1, 0(a0)
    addi    s2, s2, 24
    addi    s3, s3, 1
    li      t2, 5
    bltu    s3, t2, rec_loop

    /* Power off through the SiFive test device. */
    li      t0, 0x100000
    li      t1, 0x5555
    sw      t1, 0(t0)
1:  j       1b

/* a0 = UART base, a2 = value; clobbers t3..t6. */
puthex:
    li      t3, 60
    la      t4, hexdigits
1:  srl     t5, a2, t3
    andi    t5, t5, 15
    add     t6, t4, t5
    lbu     t6, 0(t6)
    sb      t6, 0(a0)
    addi    t3, t3, -4
    bgez    t3, 1b
    ret

/* Trap entry: record (mcause, mepc, mtval), advance past the instruction the
 * mainline parked in s0, return with mret. s1 indexes a 24-byte slot. */
    .balign 4
trap_entry:
    csrr    t0, mcause
    csrr    t1, mepc
    csrr    t2, mtval
    slli    t3, s1, 1          /* 2*s1 */
    add     t3, t3, s1         /* 3*s1 */
    slli    t3, t3, 3          /* 24*s1 */
    la      t4, recs
    add     t4, t4, t3
    sd      t0, 0(t4)
    sd      t1, 8(t4)
    sd      t2, 16(t4)
    addi    s1, s1, 1
    csrw    mepc, s0
    mret

    .section .rodata
prefix:
    .asciz  "latr-fault-diff="
rec_prefix:
    .asciz  "\nlatr-fault-rec"
hexdigits:
    .ascii  "0123456789abcdef"

    .section .data
    .balign 8
recs:
    .space  24 * 5
