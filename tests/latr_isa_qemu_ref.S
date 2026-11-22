#include "latr_isa_diff_vectors.h"

    .section .text
    .option norvc
    .globl _start
    .type _start, @function
_start:
    li      x2, 0
    .word   LATR_DIFF_ADDI_X1_5
    .word   LATR_DIFF_DIV_X3_X1_X2
    .word   LATR_DIFF_REM_X4_X1_X2
    li      t0, -1
    bne     x3, t0, .Lfail
    li      t0, 5
    bne     x4, t0, .Lfail
    /* The compressed vectors below use x8/x9 (= s0/s1), so park the
     * div/rem results in memory and reload them at print time. */
    la      t0, .Lsave_div
    sd      x3, 0(t0)
    sd      x4, 8(t0)

    li      x1, 0x7fffffff
    li      x2, 0x7fffffff
    .word   LATR_DIFF_MULW_X3_X1_X2
    li      t0, 1
    bne     x3, t0, .Lfail
    mv      s2, x3

    li      x10, 9
    .2byte  LATR_DIFF_C_ADDI_X10_1
    li      t0, 10
    bne     x10, t0, .Lfail
    mv      s3, x10
    slli    s3, s3, 24
    li      x8, 0
    li      x9, 0
    .2byte  LATR_DIFF_C_BEQZ_X8_4
    .2byte  LATR_DIFF_C_LI_X9_9
    .2byte  LATR_DIFF_C_ADDI_X9_1
    add     x10, x9, x8
    li      t0, 1
    bne     x10, t0, .Lfail
    slli    t0, x10, 16
    or      s3, s3, t0

    li      x8, 1
    li      x9, 0
    .2byte  LATR_DIFF_C_BEQZ_X8_4
    .2byte  LATR_DIFF_C_LI_X9_9
    .2byte  LATR_DIFF_C_ADDI_X9_1
    add     x10, x9, x8
    li      t0, 11
    bne     x10, t0, .Lfail
    slli    t0, x10, 8
    or      s3, s3, t0

    li      x10, 0
    .2byte  LATR_DIFF_C_J_6
    addi    x10, x0, 9
    addi    x10, x10, 1
    li      t0, 1
    bne     x10, t0, .Lfail
    or      s3, s3, x10


    lui     x2, 0x40000
    slli    x2, x2, 2
    .word   LATR_DIFF_ADDI_X1_5
    .word   LATR_DIFF_DIVW_X3_X1_X2
    .word   LATR_DIFF_REMW_X4_X1_X2
    .word   LATR_DIFF_DIVUW_X5_X1_X2
    .word   LATR_DIFF_REMUW_X6_X1_X2
    li      t2, -1
    bne     x3, t2, .Lfail
    li      t2, 5
    bne     x4, t2, .Lfail
    li      t2, -1
    bne     x5, t2, .Lfail
    li      t2, 5
    bne     x6, t2, .Lfail
    mv      s4, x3
    mv      s5, x4
    mv      s6, x5
    mv      s7, x6
    li      t0, 0x6000
    csrs    mstatus, t0
    lui     x1, 0x3f800
    lui     x2, 0x33800
    .word   LATR_DIFF_FMV_W_X_F1_X1
    .word   LATR_DIFF_FMV_W_X_F2_X2
    .word   LATR_DIFF_FADD_S_RMM
    .word   LATR_DIFF_FMV_X_W_X3_F3
    .word   LATR_DIFF_CSRRS_X5_FFLAGS
    li      t2, 1
    bne     x5, t2, .Lfail
    mv      s8, x3
    .word   LATR_DIFF_CSRRW_X0_FFLAGS_X0
    .word   LATR_DIFF_CSRRWI_X0_FRM_4
    .word   LATR_DIFF_FADD_S_DYN_F4
    .word   LATR_DIFF_FMV_X_W_X6_F4
    .word   LATR_DIFF_CSRRS_X7_FFLAGS
    li      t2, 0x3f800001
    bne     x3, t2, .Lfail
    bne     x6, t2, .Lfail
    li      t2, 1
    bne     x7, t2, .Lfail
    mv      s9, x6
    .word   LATR_DIFF_CSRRW_X0_FFLAGS_X0
    .word   LATR_DIFF_FADD_S_DYN_F5
    .word   LATR_DIFF_CSRRS_X11_FFLAGS
    bnez    x11, .Lfail
    lui     x1, 0x3ff00
    slli    x1, x1, 32
    lui     x2, 0x40000
    slli    x2, x2, 32
    .word   LATR_DIFF_FMV_D_X_F1_X1
    .word   LATR_DIFF_FMV_D_X_F2_X2
    .word   LATR_DIFF_FADD_D_RNE
    .word   LATR_DIFF_FMV_X_D_X3_F3
    lui     t2, 0x40080
    slli    t2, t2, 32
    bne     x3, t2, .Lfail
    mv      s10, x3
    .word   LATR_DIFF_CSRRS_X10_FFLAGS
    bnez    x10, .Lfail
    .word   LATR_DIFF_FMADD_D_F4_F1_F2_F3
    .word   LATR_DIFF_FMV_X_D_X3_F4
    lui     t2, 0x40140
    slli    t2, t2, 32
    bne     x3, t2, .Lfail
    mv      s11, x3
    lui     x2, 0x40080
    slli    x2, x2, 32
    .word   LATR_DIFF_FMV_D_X_F2_X2
    .word   LATR_DIFF_FDIV_D_RMM
    .word   LATR_DIFF_FMV_X_D_X3_F3
    li      t2, 0x3fd5555555555555
    bne     x3, t2, .Lfail
    .word   LATR_DIFF_CSRRS_X5_FFLAGS
    li      t2, 1
    bne     x5, t2, .Lfail
    mv      s10, x3
    .word   LATR_DIFF_CSRRW_X0_FFLAGS_X0
    .word   LATR_DIFF_LUI_X1_40200
    .word   LATR_DIFF_FMV_W_X_F1_X1
    .word   LATR_DIFF_FCVT_W_S_RMM
    .word   LATR_DIFF_CSRRS_X5_FFLAGS
    li      t2, 3
    bne     x3, t2, .Lfail
    li      t2, 1
    bne     x5, t2, .Lfail
    .word   LATR_DIFF_CSRRW_X0_FFLAGS_X0
    .word   LATR_DIFF_FCVT_W_S_RMM_X0
    .word   LATR_DIFF_CSRRS_X5_FFLAGS
    li      t2, 1
    bne     x5, t2, .Lfail
    bnez    x0, .Lfail
    .word   LATR_DIFF_CSRRW_X0_FFLAGS_X0
    lui     x1, 0x7fc00
    lui     x2, 0x3f800
    .word   LATR_DIFF_FMV_W_X_F1_X1
    .word   LATR_DIFF_FMV_W_X_F2_X2
    .word   LATR_DIFF_FMIN_S_F3_F1_F2
    .word   LATR_DIFF_FMV_X_W_X3_F3
    .word   LATR_DIFF_CSRRS_X5_FFLAGS
    .word   LATR_DIFF_FCLASS_S_X4_F1
    li      t2, 0x3f800000
    bne     x3, t2, .Lfail
    bnez    x5, .Lfail
    li      t2, 0x200
    bne     x4, t2, .Lfail
    .word   LATR_DIFF_CSRRW_X0_FFLAGS_X0
    lui     x1, 0x7f800
    addi    x1, x1, 1
    .word   LATR_DIFF_FMV_W_X_F1_X1
    .word   LATR_DIFF_FMV_W_X_F2_X2
    .word   LATR_DIFF_FMIN_S_F3_F1_F2
    .word   LATR_DIFF_FMV_X_W_X3_F3
    .word   LATR_DIFF_CSRRS_X5_FFLAGS
    .word   LATR_DIFF_FCLASS_S_X4_F1
    li      t2, 0x3f800000
    bne     x3, t2, .Lfail
    li      t2, 0x10
    bne     x5, t2, .Lfail
    li      t2, 0x100
    bne     x4, t2, .Lfail
    lui     x1, 0x80000
    li      x2, 0
    .word   LATR_DIFF_FMV_W_X_F1_X1
    .word   LATR_DIFF_FMV_W_X_F2_X2
    .word   LATR_DIFF_FMIN_S_F3_F1_F2
    .word   LATR_DIFF_FMV_X_W_X3_F3
    lui     t2, 0x80000
    bne     x3, t2, .Lfail
    .word   LATR_DIFF_FMAX_S_F3_F1_F2
    .word   LATR_DIFF_FMV_X_W_X6_F3
    bnez    x6, .Lfail
    .word   LATR_DIFF_FCLASS_S_X4_F1
    li      t2, 8
    bne     x4, t2, .Lfail

    /* satp/PMP WARL: cross-check the LATR constants against this engine.
     * Enabling paging in the golden run would translate the next fetch, so
     * only Bare and an unsupported MODE (rejected) are exercised here. */
    li      x1, 0x999
    csrw    satp, x1
    csrr    x5, satp
    li      t0, 0x999
    bne     x5, t0, .Lfail
    li      x1, 0xb000000000000abc    /* MODE=11 is not supported */
    csrw    satp, x1
    csrr    x5, satp
    li      t0, 0x999
    bne     x5, t0, .Lfail
    csrw    satp, x0

    li      x1, 0x123456789
    csrw    pmpaddr0, x1
    csrr    x5, pmpaddr0
    bne     x5, x1, .Lfail
    li      x1, 0x0018020f            /* entry1 = 0x02: W without R */
    csrw    pmpcfg0, x1
    csrr    x5, pmpcfg0
    li      t0, 0x0018000f
    bne     x5, t0, .Lfail
    csrw    pmpcfg0, x0
    csrw    pmpaddr0, x0

    /* Zba/Zbb/Zbs: the same vectors and expected values are asserted by
     * the LATR harness, so this run cross-checks those constants. */
    li      x1, 0x010100ff
    .word   0x2870d293          /* orc.b x5, x1 */
    li      t0, 0xffff00ff
    bne     x5, t0, .Lfail

    li      x1, 1
    .word   0x60009293          /* clz x5, x1 */
    li      t0, 63
    bne     x5, t0, .Lfail

    li      x1, 0x100
    .word   0x60109293          /* ctz x5, x1 */
    li      t0, 8
    bne     x5, t0, .Lfail

    li      x1, 0xf0f0
    .word   0x60209293          /* cpop x5, x1 */
    li      t0, 8
    bne     x5, t0, .Lfail

    li      x1, 0x80000000
    li      x2, 1
    .word   0x602092bb          /* rolw x5, x1, x2 */
    li      t0, 1
    bne     x5, t0, .Lfail

    li      x1, 0x80
    .word   0x60409293          /* sext.b x5, x1 */
    li      t0, -128
    bne     x5, t0, .Lfail

    li      x1, 0xffff0000ffff
    .word   0x0800c2bb          /* zext.h x5, x1 */
    li      t0, 0xffff
    bne     x5, t0, .Lfail

    li      x1, 0x1122334455667788
    .word   0x6b80d293          /* rev8 x5, x1 */
    li      t0, 0x8877665544332211
    bne     x5, t0, .Lfail

    li      x1, -1
    li      x2, 1
    .word   0x0a20c2b3          /* min x5, x1, x2 */
    bne     x5, x1, .Lfail
    .word   0x0a20f2b3          /* maxu x5, x1, x2 */
    bne     x5, x1, .Lfail

    li      x1, 0x20
    .word   0x4850d293          /* bexti x5, x1, 5 */
    li      t0, 1
    bne     x5, t0, .Lfail

    li      x1, 0
    .word   0x28309293          /* bseti x5, x1, 3 */
    li      t0, 8
    bne     x5, t0, .Lfail

    li      x1, 1
    .word   0x68009293          /* binvi x5, x1, 0 */
    bnez    x5, .Lfail

    li      x1, 3
    li      x2, 5
    .word   0x2020a2b3          /* sh1add x5, x1, x2 */
    li      t0, 11
    bne     x5, t0, .Lfail

    li      x1, 0xffffffff
    li      x2, 1
    .word   0x082082bb          /* add.uw x5, x1, x2 */
    li      t0, 0x100000000
    bne     x5, t0, .Lfail

    li      x1, 0xffffffff
    .word   0x0840929b          /* slli.uw x5, x1, 4 */
    li      t0, 0xffffffff0
    bne     x5, t0, .Lfail

    li      x1, 0xf
    .word   0x6040d29b          /* roriw x5, x1, 4 */
    li      t0, 0xfffffffff0000000
    bne     x5, t0, .Lfail

    /* RVV golden differential sequence (-march=rv64gcv). */
    li      t0, 0x600
    csrs    mstatus, t0
    .word   LATR_DIFF_RVV_VSETIVLI_4
    li      t0, 3
    li      t1, -5
    .word   LATR_DIFF_RVV_VMV_V_I_V1_7
    .word   LATR_DIFF_RVV_VMV_V_X_V2_T0
    .word   LATR_DIFF_RVV_VMUL_VV_V3
    .word   LATR_DIFF_RVV_VADD_VX_V3_T1
    .word   LATR_DIFF_RVV_VMAX_VV_V3
    .word   LATR_DIFF_RVV_VSLL_VI_V4_2
    .word   LATR_DIFF_RVV_VDIV_VV_V5
    .word   LATR_DIFF_RVV_VREM_VV_V6
    .word   LATR_DIFF_RVV_VXOR_VV_V3
    .word   LATR_DIFF_RVV_VMV_V_I_V7_0
    .word   LATR_DIFF_RVV_VREDSUM_V7
    .word   LATR_DIFF_RVV_VMV_X_S_T0_V7
    la      t1, .Lsave_div
    sd      t0, 16(t1)


    li      t0, 0x10000000
    la      t1, .Lprefix
.Lwrite_prefix:
    lbu     t2, 0(t1)
    beqz    t2, .Lprint_diff
    sb      t2, 0(t0)
    addi    t1, t1, 1
    j       .Lwrite_prefix
.Lprint_diff:
    li      a1, 0x10000000
    la      a2, .Lhex_digits
    la      t0, .Lsave_div
    ld      a0, 0(t0)
    jal     ra, .Lprint_hex64
    ld      a0, 8(t0)
    jal     ra, .Lprint_hex64
    mv      a0, s2
    jal     ra, .Lprint_hex64
    mv      a0, s3
    jal     ra, .Lprint_hex64
    mv      a0, s4
    jal     ra, .Lprint_hex64
    mv      a0, s5
    jal     ra, .Lprint_hex64
    mv      a0, s6
    jal     ra, .Lprint_hex64
    mv      a0, s7
    jal     ra, .Lprint_hex64
    mv      a0, s8
    jal     ra, .Lprint_hex64
    mv      a0, s9
    jal     ra, .Lprint_hex64
    mv      a0, s10
    jal     ra, .Lprint_hex64
    mv      a0, s11
    jal     ra, .Lprint_hex64
    la      t0, .Lsave_div
    ld      a0, 16(t0)
    jal     ra, .Lprint_hex64
    li      t0, 10
    sb      t0, 0(a1)

    li      t0, 0x10000000
    la      t1, .Lpass
.Lwrite_pass:
    lbu     t2, 0(t1)
    beqz    t2, .Lpass_exit
    sb      t2, 0(t0)
    addi    t1, t1, 1
    j       .Lwrite_pass
.Lpass_exit:
    li      t0, 0x100000
    li      t1, 0x5555
    sw      t1, 0(t0)
1:  j       1b

.Lfail:
    li      t0, 0x10000000
    la      t1, .Lfailure
.Lwrite_failure:
    lbu     t2, 0(t1)
    beqz    t2, .Lfail_exit
    sb      t2, 0(t0)
    addi    t1, t1, 1
    j       .Lwrite_failure
.Lfail_exit:
    li      t0, 0x100000
    li      t1, 0x3333
    sw      t1, 0(t0)
2:  j       2b

.Lprint_hex64:
    li      t3, 60
    li      t4, 16
.Lhex_loop:
    srl     t5, a0, t3
    andi    t5, t5, 15
    add     t6, a2, t5
    lbu     t5, 0(t6)
    sb      t5, 0(a1)
    addi    t3, t3, -4
    addi    t4, t4, -1
    bnez    t4, .Lhex_loop
    ret

    .section .rodata
.Lprefix:
    .asciz "latr-diff="
.Lhex_digits:
    .ascii "0123456789abcdef"
.Lpass:
    .asciz "latr-riscv-ref: PASS\n"
.Lfailure:
    .asciz "latr-riscv-ref: FAIL\n"

    .section .data
    .align 3
.Lsave_div:
    .dword 0
    .dword 0
    .dword 0
