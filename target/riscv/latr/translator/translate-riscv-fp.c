#include "latr.h"
#include "ir1.h"
#include "translate.h"
#include "la-ir2.h"

typedef IR2_INST *(*LatrFPBinaryOp)(IR2_OPND, IR2_OPND, IR2_OPND);
typedef IR2_INST *(*LatrFPUnaryOp)(IR2_OPND, IR2_OPND);
typedef IR2_INST *(*LatrFPFmaOp)(IR2_OPND, IR2_OPND, IR2_OPND, IR2_OPND);
typedef IR2_INST *(*LatrFPIntToFloatOp)(IR2_OPND, IR2_OPND);
typedef IR2_INST *(*LatrFPFloatToIntOp)(IR2_OPND, IR2_OPND);

/*
 * Returns non-zero when the rounding mode is not one the helper implements.
 * Deliberately 64-bit: every other LATR cross-boundary helper uses uint64_t,
 * and a 32-bit return would leave a0's upper half holding whatever the caller
 * put there (the env pointer), which the generated code cannot test against.
 */
extern uint64_t latr_riscv_fp_rmm(CPUArchState *env, uint64_t opcode,
                                  uint64_t rd, uint64_t rs1, uint64_t rs2,
                                  uint64_t rs3, uint64_t rm, uint64_t pc);

static void emit_fp_rmm_call(const RiscvIR1 *ir1);

static IR2_OPND fp_label(void)
{
    return ir2_opnd_new_type(IR2_OPND_LABEL);
}


static void emit_fp_raise_illegal(const RiscvIR1 *ir1)
{
    latr_raise_illegal_instruction(ir1);
}

static void emit_fp_rmm_call(const RiscvIR1 *ir1)
{
    IR2_OPND sp = ir2_opnd_new(IR2_OPND_GPR, la_sp);
    IR2_OPND a0 = ir2_opnd_new(IR2_OPND_GPR, la_a0);
    IR2_OPND a1 = ir2_opnd_new(IR2_OPND_GPR, la_a1);
    IR2_OPND a2 = ir2_opnd_new(IR2_OPND_GPR, la_a2);
    IR2_OPND a3 = ir2_opnd_new(IR2_OPND_GPR, la_a3);
    IR2_OPND a4 = ir2_opnd_new(IR2_OPND_GPR, la_a4);
    IR2_OPND a5 = ir2_opnd_new(IR2_OPND_GPR, la_a5);
    IR2_OPND a6 = ir2_opnd_new(IR2_OPND_GPR, la_a6);
    IR2_OPND a7 = ir2_opnd_new(IR2_OPND_GPR, la_a7);
    IR2_OPND target = ra_alloc_itemp();
    IR2_OPND done = fp_label();

    la_addi_d(sp, sp, -16);
    la_st_d(ra_ir2_opnd, sp, 0);
    la_or(a0, env_ir2_opnd, zero_ir2_opnd);
    li_d(a1, ir1->opcode);
    li_d(a2, ir1->arg.rd);
    li_d(a3, ir1->arg.rs1);
    li_d(a4, ir1->arg.rs2);
    li_d(a5, ir1->arg.rs3);
    li_d(a6, ir1->arg.rm);
    li_d(a7, ir1->pc);
    li_d(target, (uint64_t)(uintptr_t)&latr_riscv_fp_rmm);
    la_jirl(ra_ir2_opnd, target, 0);
    ra_free_temp(target);
    la_ld_d(ra_ir2_opnd, sp, 0);
    la_addi_d(sp, sp, 16);

    /*
     * Belt and braces: even with the 64-bit return type above, the generated
     * test must not depend on the upper half of a0, so mask it off explicitly.
     */
    la_bstrpick_d(a0, a0, 31, 0);
    la_beqz(a0, done);
    emit_fp_raise_illegal(ir1);
    la_label(done);
}



static void emit_class_to_gpr(IR2_OPND src, bool single, IR2_OPND class_fpr,
                              IR2_OPND class_gpr);
static bool fp_opcode_uses_rm(LatrRiscvOpcode opcode)
{
    switch (opcode) {
    case LATR_RV_FMADD_S: case LATR_RV_FMSUB_S:
    case LATR_RV_FNMSUB_S: case LATR_RV_FNMADD_S:
    case LATR_RV_FADD_S: case LATR_RV_FSUB_S:
    case LATR_RV_FMUL_S: case LATR_RV_FDIV_S: case LATR_RV_FSQRT_S:
    case LATR_RV_FCVT_W_S: case LATR_RV_FCVT_WU_S:
    case LATR_RV_FCVT_S_W: case LATR_RV_FCVT_S_WU:
    case LATR_RV_FCVT_L_S: case LATR_RV_FCVT_LU_S:
    case LATR_RV_FCVT_S_L: case LATR_RV_FCVT_S_LU:
    case LATR_RV_FMADD_D: case LATR_RV_FMSUB_D:
    case LATR_RV_FNMSUB_D: case LATR_RV_FNMADD_D:
    case LATR_RV_FADD_D: case LATR_RV_FSUB_D:
    case LATR_RV_FMUL_D: case LATR_RV_FDIV_D: case LATR_RV_FSQRT_D:
    case LATR_RV_FCVT_S_D: case LATR_RV_FCVT_D_S:
    case LATR_RV_FCVT_W_D: case LATR_RV_FCVT_WU_D:
    case LATR_RV_FCVT_D_W: case LATR_RV_FCVT_D_WU:
    case LATR_RV_FCVT_L_D: case LATR_RV_FCVT_LU_D:
    case LATR_RV_FCVT_D_L: case LATR_RV_FCVT_D_LU:
        return true;
    default:
        return false;
    }
}


static void emit_fp_disable_exceptions(void)
{
    /* RISC-V has sticky flags, not host-style enabled FP traps. */
    la_movgr2fcsr(fcsr1_ir2_opnd, zero_ir2_opnd);
    la_movgr2fcsr(fcsr2_ir2_opnd, zero_ir2_opnd);
}

/* Map RISC-V's frm values to LoongArch's RM encoding in FCSR0[9:8]. */
static void emit_fp_rounding_mode(int rm, uint64_t pc)
{
    static const uint8_t la_rm[4] = { 0, 1, 3, 2 };
    IR2_OPND value = ra_alloc_itemp();
    (void)pc;
    emit_fp_disable_exceptions();

    if (rm != 7) {
        li_d(value, (uint64_t)la_rm[rm] << 8);
        la_movgr2fcsr(fcsr3_ir2_opnd, value);
        ra_free_temp(value);
        return;
    }

    /* Dynamic native execution has already excluded frm=4..7. */
    IR2_OPND frm = ra_alloc_itemp();
    la_ld_d(value, env_ir2_opnd, env_offset_fcsr());
    la_bstrpick_d(frm, value, 7, 5);
    la_srli_d(value, frm, 1);
    la_andi(value, value, 1);
    la_xor(frm, frm, value);
    la_slli_d(frm, frm, 8);
    la_movgr2fcsr(fcsr3_ir2_opnd, frm);

    ra_free_temp(frm);
    ra_free_temp(value);
}

/* LA FCSR cause bits [28:24] match RISC-V fflags bit positions. */
static void emit_fp_commit_flags(void)
{
    IR2_OPND flags = ra_alloc_itemp();
    IR2_OPND fcsr = ra_alloc_itemp();

    la_movfcsr2gr(flags, fcsr2_ir2_opnd);
    la_bstrpick_d(flags, flags, 28, 24);
    la_ld_d(fcsr, env_ir2_opnd, env_offset_fcsr());
    la_or(fcsr, fcsr, flags);
    la_st_d(fcsr, env_ir2_opnd, env_offset_fcsr());

    ra_free_temp(fcsr);
    ra_free_temp(flags);
}

static void emit_fp_set_guest_nv(void)
{
    IR2_OPND fcsr = ra_alloc_itemp();

    la_ld_d(fcsr, env_ir2_opnd, env_offset_fcsr());
    la_ori(fcsr, fcsr, 0x10);
    la_st_d(fcsr, env_ir2_opnd, env_offset_fcsr());
    ra_free_temp(fcsr);
}

/* QEMU's check_nanbox_s turns every malformed S register into canonical qNaN. */
static void emit_check_nanbox_s(IR2_OPND value)
{
    IR2_OPND raw = ra_alloc_itemp();
    IR2_OPND valid = ra_alloc_itemp();
    IR2_OPND mask = ra_alloc_itemp();

    la_movfr2gr_d(raw, value);
    la_srai_d(valid, raw, 32);
    la_nor(valid, valid, zero_ir2_opnd);
    la_sltui(valid, valid, 1);
    la_bstrpick_d(raw, raw, 31, 0);
    la_sub_d(mask, zero_ir2_opnd, valid);
    la_and(raw, raw, mask);
    la_nor(mask, mask, zero_ir2_opnd);
    li_d(valid, 0x7fc00000);
    la_and(valid, valid, mask);
    la_or(raw, raw, valid);
    li_d(mask, UINT64_C(0xffffffff00000000));
    la_or(raw, raw, mask);
    la_movgr2fr_d(value, raw);

    ra_free_temp(mask);
    ra_free_temp(valid);
    ra_free_temp(raw);
}

static void emit_box_s(IR2_OPND value)
{
    IR2_OPND raw = ra_alloc_itemp();
    IR2_OPND box = ra_alloc_itemp();

    la_movfr2gr_s(raw, value);
    la_bstrpick_d(raw, raw, 31, 0);
    li_d(box, UINT64_C(0xffffffff00000000));
    la_or(raw, raw, box);
    la_movgr2fr_d(value, raw);

    ra_free_temp(box);
    ra_free_temp(raw);
}

static void emit_canonicalize_nan(IR2_OPND value, bool single)
{
    IR2_OPND classification = ra_alloc_ftemp();
    IR2_OPND classes = ra_alloc_itemp();
    IR2_OPND canonical = ra_alloc_itemp();
    IR2_OPND not_nan = fp_label();

    if (single) {
        la_fclass_s(classification, value);
    } else {
        la_fclass_d(classification, value);
    }
    la_movfr2gr_d(classes, classification);
    la_andi(classes, classes, 0x3);
    la_beqz(classes, not_nan);
    li_d(canonical, single ? UINT64_C(0xffffffff7fc00000) :
                             UINT64_C(0x7ff8000000000000));
    la_movgr2fr_d(value, canonical);
    la_label(not_nan);

    ra_free_temp(canonical);
    ra_free_temp(classes);
    free_alloc_fpr(classification);
}

static void emit_fp_binary(const RiscvIR1 *ir1, bool single, bool round,
                           bool update_flags, bool canonicalize,
                           LatrFPBinaryOp op)
{
    IR2_OPND lhs = alloc_fpr_src(ir1->arg.rs1);
    IR2_OPND rhs = alloc_fpr_src(ir1->arg.rs2);
    IR2_OPND dst = alloc_fpr_dst(ir1->arg.rd);

    if (single) {
        emit_check_nanbox_s(lhs);
        emit_check_nanbox_s(rhs);
    }
    if (round) {
        emit_fp_rounding_mode(ir1->arg.rm, ir1->pc);
    } else if (update_flags) {
        emit_fp_disable_exceptions();
    }
    op(dst, lhs, rhs);
    if (update_flags) {
        emit_fp_commit_flags();
    }
    if (canonicalize) {
        emit_canonicalize_nan(dst, single);
    }
    if (single) {
        emit_box_s(dst);
    }
    store_fpr_dst(ir1->arg.rd, dst);

    free_alloc_fpr(dst);
    free_alloc_fpr(rhs);
    free_alloc_fpr(lhs);
}
static void emit_fp_minmax(const RiscvIR1 *ir1, bool single, bool is_min,
                           LatrFPBinaryOp op)
{
    IR2_OPND lhs = alloc_fpr_src(ir1->arg.rs1);
    IR2_OPND rhs = alloc_fpr_src(ir1->arg.rs2);
    IR2_OPND dst = alloc_fpr_dst(ir1->arg.rd);
    IR2_OPND class_l_fpr = ra_alloc_ftemp();
    IR2_OPND class_r_fpr = ra_alloc_ftemp();
    IR2_OPND class_l = ra_alloc_itemp();
    IR2_OPND class_r = ra_alloc_itemp();
    IR2_OPND scratch = ra_alloc_itemp();
    IR2_OPND left_nan = fp_label();
    IR2_OPND right_nan = fp_label();
    IR2_OPND both_nan = fp_label();
    IR2_OPND left_value = fp_label();
    IR2_OPND right_value = fp_label();
    IR2_OPND both_value = fp_label();
    IR2_OPND done = fp_label();
    IR2_OPND nonzero = fp_label();

    if (single) {
        emit_check_nanbox_s(lhs);
        emit_check_nanbox_s(rhs);
        la_fclass_s(class_l_fpr, lhs);
        la_fclass_s(class_r_fpr, rhs);
    } else {
        la_fclass_d(class_l_fpr, lhs);
        la_fclass_d(class_r_fpr, rhs);
    }
    la_movfr2gr_d(class_l, class_l_fpr);
    la_movfr2gr_d(class_r, class_r_fpr);
    la_andi(scratch, class_l, 0x3);
    la_bnez(scratch, left_nan);
    la_andi(scratch, class_r, 0x3);
    la_bnez(scratch, right_nan);
    /* FMIN/FMAX must distinguish -0 and +0 even if host MIN/MAX does not. */
    la_andi(scratch, class_l, (1 << 5) | (1 << 9));
    la_beqz(scratch, nonzero);
    la_andi(scratch, class_r, (1 << 5) | (1 << 9));
    la_beqz(scratch, nonzero);
    if (is_min) {
        IR2_OPND zero_bits = ra_alloc_itemp();
        la_movfr2gr_d(zero_bits, lhs);
        la_movfr2gr_d(scratch, rhs);
        la_or(zero_bits, zero_bits, scratch);
        la_movgr2fr_d(dst, zero_bits);
        ra_free_temp(zero_bits);
    } else {
        IR2_OPND zero_bits = ra_alloc_itemp();
        la_movfr2gr_d(zero_bits, lhs);
        la_movfr2gr_d(scratch, rhs);
        la_and(zero_bits, zero_bits, scratch);
        la_movgr2fr_d(dst, zero_bits);
        ra_free_temp(zero_bits);
    }
    la_b(done);

    la_label(nonzero);
    emit_fp_disable_exceptions();
    op(dst, lhs, rhs);
    la_b(done);

    la_label(left_nan);
    la_andi(scratch, class_r, 0x3);
    la_bnez(scratch, both_nan);
    la_andi(scratch, class_l, 0x1);
    la_beqz(scratch, left_value);
    emit_fp_set_guest_nv();
    la_label(left_value);
    la_movfr2gr_d(scratch, rhs);
    la_movgr2fr_d(dst, scratch);
    la_b(done);

    la_label(right_nan);
    la_andi(scratch, class_r, 0x1);
    la_beqz(scratch, right_value);
    emit_fp_set_guest_nv();
    la_label(right_value);
    la_movfr2gr_d(scratch, lhs);
    la_movgr2fr_d(dst, scratch);
    la_b(done);

    la_label(both_nan);
    la_andi(scratch, class_l, 0x1);
    la_andi(class_r, class_r, 0x1);
    la_or(scratch, scratch, class_r);
    la_beqz(scratch, both_value);
    emit_fp_set_guest_nv();
    la_label(both_value);
    li_d(scratch, single ? UINT64_C(0xffffffff7fc00000) :
                           UINT64_C(0x7ff8000000000000));
    la_movgr2fr_d(dst, scratch);

    la_label(done);
    if (single) {
        emit_box_s(dst);
    }
    store_fpr_dst(ir1->arg.rd, dst);

    ra_free_temp(scratch);
    ra_free_temp(class_r);
    ra_free_temp(class_l);
    free_alloc_fpr(class_r_fpr);
    free_alloc_fpr(class_l_fpr);
    free_alloc_fpr(dst);
    free_alloc_fpr(rhs);
    free_alloc_fpr(lhs);
}


static void emit_fp_unary(const RiscvIR1 *ir1, bool single, bool round,
                          bool update_flags, bool canonicalize,
                          LatrFPUnaryOp op)
{
    IR2_OPND src = alloc_fpr_src(ir1->arg.rs1);
    IR2_OPND dst = alloc_fpr_dst(ir1->arg.rd);

    if (single) {
        emit_check_nanbox_s(src);
    }
    if (round) {
        emit_fp_rounding_mode(ir1->arg.rm, ir1->pc);
    } else if (update_flags) {
        emit_fp_disable_exceptions();
    }
    op(dst, src);
    if (update_flags) {
        emit_fp_commit_flags();
    }
    if (canonicalize) {
        emit_canonicalize_nan(dst, single);
    }
    if (single) {
        emit_box_s(dst);
    }
    store_fpr_dst(ir1->arg.rd, dst);

    free_alloc_fpr(dst);
    free_alloc_fpr(src);
}
static void emit_fp_d_to_s(const RiscvIR1 *ir1)
{
    IR2_OPND src = alloc_fpr_src(ir1->arg.rs1);
    IR2_OPND dst = alloc_fpr_dst(ir1->arg.rd);

    emit_fp_rounding_mode(ir1->arg.rm, ir1->pc);
    la_fcvt_s_d(dst, src);
    emit_fp_commit_flags();
    emit_canonicalize_nan(dst, true);
    emit_box_s(dst);
    store_fpr_dst(ir1->arg.rd, dst);

    free_alloc_fpr(dst);
    free_alloc_fpr(src);
}


static void emit_fp_fma(const RiscvIR1 *ir1, bool single,
                        LatrFPFmaOp op)
{
    IR2_OPND lhs = alloc_fpr_src(ir1->arg.rs1);
    IR2_OPND rhs = alloc_fpr_src(ir1->arg.rs2);
    IR2_OPND addend = alloc_fpr_src(ir1->arg.rs3);
    IR2_OPND dst = alloc_fpr_dst(ir1->arg.rd);

    if (single) {
        emit_check_nanbox_s(lhs);
        emit_check_nanbox_s(rhs);
        emit_check_nanbox_s(addend);
    }
    emit_fp_rounding_mode(ir1->arg.rm, ir1->pc);
    op(dst, lhs, rhs, addend);
    emit_fp_commit_flags();
    emit_canonicalize_nan(dst, single);
    if (single) {
        emit_box_s(dst);
    }
    store_fpr_dst(ir1->arg.rd, dst);

    free_alloc_fpr(dst);
    free_alloc_fpr(addend);
    free_alloc_fpr(rhs);
    free_alloc_fpr(lhs);
}

static void emit_fp_sign_inject(const RiscvIR1 *ir1, bool single,
                                bool negate, bool xor_sign)
{
    IR2_OPND lhs = alloc_fpr_src(ir1->arg.rs1);
    IR2_OPND rhs = alloc_fpr_src(ir1->arg.rs2);
    IR2_OPND dst = alloc_fpr_dst(ir1->arg.rd);
    IR2_OPND bits1 = ra_alloc_itemp();
    IR2_OPND bits2 = ra_alloc_itemp();
    IR2_OPND sign_mask = ra_alloc_itemp();
    IR2_OPND sign_value = ir2_opnd_new_none();

    if (single) {
        emit_check_nanbox_s(lhs);
        emit_check_nanbox_s(rhs);
    }
    la_movfr2gr_d(bits1, lhs);
    la_movfr2gr_d(bits2, rhs);
    li_d(sign_mask, single ? UINT64_C(0x80000000) :
                             UINT64_C(0x8000000000000000));

    if (xor_sign) {
        sign_value = ra_alloc_itemp();
        la_and(sign_value, bits1, sign_mask);
        la_and(bits2, bits2, sign_mask);
        la_xor(sign_value, sign_value, bits2);
        la_andn(bits1, bits1, sign_mask);
        la_or(bits1, bits1, sign_value);
        ra_free_temp(sign_value);
    } else {
        la_andn(bits1, bits1, sign_mask);
        la_and(bits2, bits2, sign_mask);
        if (negate) {
            la_xor(bits2, bits2, sign_mask);
        }
        la_or(bits1, bits1, bits2);
    }

    la_movgr2fr_d(dst, bits1);
    store_fpr_dst(ir1->arg.rd, dst);

    ra_free_temp(sign_mask);
    ra_free_temp(bits2);
    ra_free_temp(bits1);
    free_alloc_fpr(dst);
    free_alloc_fpr(rhs);
    free_alloc_fpr(lhs);
}

static void emit_fp_class(const RiscvIR1 *ir1, bool single)
{
    IR2_OPND src = alloc_fpr_src(ir1->arg.rs1);
    IR2_OPND classification = ra_alloc_ftemp();
    IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);

    if (single) {
        emit_check_nanbox_s(src);
    }
    emit_class_to_gpr(src, single, classification, dst);
    store_gpr_dst(ir1->arg.rd, dst);

    free_alloc_gpr(dst);
    free_alloc_fpr(classification);
    free_alloc_fpr(src);
}

static void emit_fp_compare(const RiscvIR1 *ir1, bool single, int predicate)
{
    IR2_OPND lhs = alloc_fpr_src(ir1->arg.rs1);
    IR2_OPND rhs = alloc_fpr_src(ir1->arg.rs2);
    IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);

    emit_fp_disable_exceptions();
    if (single) {
        emit_check_nanbox_s(lhs);
        emit_check_nanbox_s(rhs);
        la_fcmp_cond_s(fcc0_ir2_opnd, lhs, rhs, predicate);
    } else {
        la_fcmp_cond_d(fcc0_ir2_opnd, lhs, rhs, predicate);
    }
    emit_fp_commit_flags();
    la_movcf2gr(dst, fcc0_ir2_opnd);
    store_gpr_dst(ir1->arg.rd, dst);

    free_alloc_gpr(dst);
    free_alloc_fpr(rhs);
    free_alloc_fpr(lhs);
}


static void emit_fp_to_signed_int(const RiscvIR1 *ir1, bool single,
                                  bool word, LatrFPFloatToIntOp op)
{
    IR2_OPND src = alloc_fpr_src(ir1->arg.rs1);
    IR2_OPND classification = ra_alloc_ftemp();
    IR2_OPND classes = ra_alloc_itemp();
    IR2_OPND scratch = ra_alloc_itemp();
    IR2_OPND converted = ra_alloc_ftemp();
    IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);
    IR2_OPND nan = fp_label();
    IR2_OPND invalid = fp_label();
    IR2_OPND negative_invalid = fp_label();
    IR2_OPND done = fp_label();

    if (single) {
        emit_check_nanbox_s(src);
    }
    emit_fp_rounding_mode(ir1->arg.rm, ir1->pc);
    emit_class_to_gpr(src, single, classification, classes);
    la_andi(scratch, classes, 0x300);
    la_bnez(scratch, nan);

    op(converted, src);
    if (word) {
        la_movfr2gr_s(dst, converted);
    } else {
        la_movfr2gr_d(dst, converted);
    }
    la_movfcsr2gr(scratch, fcsr2_ir2_opnd);
    la_bstrpick_d(scratch, scratch, 28, 28);
    emit_fp_commit_flags();
    la_bnez(scratch, invalid);
    store_gpr_dst(ir1->arg.rd, dst);
    la_b(done);

    la_label(invalid);
    la_andi(scratch, classes, 0xf);
    la_bnez(scratch, negative_invalid);
    li_d(dst, word ? INT32_MAX : INT64_MAX);
    store_gpr_dst(ir1->arg.rd, dst);
    la_b(done);

    la_label(negative_invalid);
    li_d(dst, word ? INT32_MIN : INT64_MIN);
    store_gpr_dst(ir1->arg.rd, dst);
    la_b(done);

    la_label(nan);
    emit_fp_set_guest_nv();
    li_d(dst, word ? INT32_MAX : INT64_MAX);
    store_gpr_dst(ir1->arg.rd, dst);
    la_label(done);

    free_alloc_gpr(dst);
    free_alloc_fpr(converted);
    ra_free_temp(scratch);
    ra_free_temp(classes);
    free_alloc_fpr(classification);
    free_alloc_fpr(src);
}


static void emit_class_to_gpr(IR2_OPND src, bool single, IR2_OPND class_fpr,
                              IR2_OPND class_gpr)
{
    IR2_OPND raw = ra_alloc_itemp();
    IR2_OPND scratch = ra_alloc_itemp();

    if (single) {
        la_fclass_s(class_fpr, src);
    } else {
        la_fclass_d(class_fpr, src);
    }
    la_movfr2gr_d(raw, class_fpr);

    /* LoongArch FCLASS ordering differs from RISC-V's ten class bits. */
    la_bstrpick_d(class_gpr, raw, 1, 0);
    la_slli_d(class_gpr, class_gpr, 8);
    la_bstrpick_d(scratch, raw, 5, 2);
    la_or(class_gpr, class_gpr, scratch);
    la_bstrpick_d(scratch, raw, 9, 9);
    la_slli_d(scratch, scratch, 4);
    la_or(class_gpr, class_gpr, scratch);
    la_bstrpick_d(scratch, raw, 8, 8);
    la_slli_d(scratch, scratch, 5);
    la_or(class_gpr, class_gpr, scratch);
    la_bstrpick_d(scratch, raw, 7, 7);
    la_slli_d(scratch, scratch, 6);
    la_or(class_gpr, class_gpr, scratch);
    la_bstrpick_d(scratch, raw, 6, 6);
    la_slli_d(scratch, scratch, 7);
    la_or(class_gpr, class_gpr, scratch);

    ra_free_temp(scratch);
    ra_free_temp(raw);
}

/* FCVT.WU uses the signed-64 conversion as an in-range primitive. */
static void emit_fp_to_u32(const RiscvIR1 *ir1, bool single,
                           LatrFPFloatToIntOp op)
{
    IR2_OPND src = alloc_fpr_src(ir1->arg.rs1);
    if (single) {
        emit_check_nanbox_s(src);
    }
    emit_fp_rounding_mode(ir1->arg.rm, ir1->pc);

    IR2_OPND class_fpr = ra_alloc_ftemp();
    IR2_OPND classes = ra_alloc_itemp();
    IR2_OPND scratch = ra_alloc_itemp();
    IR2_OPND converted = ra_alloc_ftemp();
    IR2_OPND dst = ra_alloc_itemp();
    IR2_OPND negative = fp_label();
    IR2_OPND negative_zero = fp_label();
    IR2_OPND nan = fp_label();
    IR2_OPND too_large = fp_label();
    IR2_OPND done = fp_label();

    emit_class_to_gpr(src, single, class_fpr, classes);
    la_andi(scratch, classes, 0x300);
    la_bnez(scratch, nan);
    la_andi(scratch, classes, 0xf);
    la_bnez(scratch, negative);
    free_alloc_gpr(scratch);
    free_alloc_gpr(classes);
    free_alloc_fpr(class_fpr);

    op(converted, src);
    la_movfr2gr_d(dst, converted);
    scratch = ra_alloc_itemp();
    la_movfcsr2gr(scratch, fcsr2_ir2_opnd);
    la_bstrpick_d(scratch, scratch, 28, 28);
    la_bnez(scratch, too_large);
    la_slt(scratch, dst, zero_ir2_opnd);
    la_bnez(scratch, too_large);
    IR2_OPND max_u32 = ra_alloc_itemp();
    li_d(max_u32, UINT64_C(0xffffffff));
    la_sltu(scratch, max_u32, dst);
    la_bnez(scratch, too_large);
    ra_free_temp(max_u32);
    ra_free_temp(scratch);
    emit_fp_commit_flags();
    la_mov32_sx(dst, dst);               /* RV64 W conversions sign-extend */
    store_gpr_dst(ir1->arg.rd, dst);
    la_b(done);

    la_label(negative);
    op(converted, src);
    la_movfr2gr_d(dst, converted);
    la_beq(dst, zero_ir2_opnd, negative_zero);
    emit_fp_set_guest_nv();
    li_d(dst, 0);
    store_gpr_dst(ir1->arg.rd, dst);
    la_b(done);

    la_label(negative_zero);
    emit_fp_commit_flags();
    li_d(dst, 0);
    store_gpr_dst(ir1->arg.rd, dst);
    la_b(done);

    la_label(nan);
    emit_fp_set_guest_nv();
    li_d(dst, -1);
    store_gpr_dst(ir1->arg.rd, dst);
    la_b(done);

    la_label(too_large);
    emit_fp_set_guest_nv();
    li_d(dst, -1);
    la_label(done);
    store_gpr_dst(ir1->arg.rd, dst);

    free_alloc_gpr(dst);
    free_alloc_fpr(converted);
    free_alloc_fpr(src);
}


/*
 * FCVT.LU splits the unsigned range at 2^63.  In the upper half, subtracting
 * 2^63 is exact for every representable input below 2^64; the signed result
 * is then combined with the high bit.
 */
static void emit_fp_to_u64(const RiscvIR1 *ir1, bool single,
                           LatrFPFloatToIntOp op)
{
    IR2_OPND src = alloc_fpr_src(ir1->arg.rs1);
    if (single) {
        emit_check_nanbox_s(src);
    }
    emit_fp_rounding_mode(ir1->arg.rm, ir1->pc);

    IR2_OPND class_fpr = ra_alloc_ftemp();
    IR2_OPND classes = ra_alloc_itemp();
    IR2_OPND scratch = ra_alloc_itemp();
    IR2_OPND converted = ra_alloc_ftemp();
    IR2_OPND adjusted = ra_alloc_ftemp();
    IR2_OPND offset = ra_alloc_ftemp();
    IR2_OPND dst = ra_alloc_itemp();
    IR2_OPND negative = fp_label();
    IR2_OPND negative_rounds_zero = fp_label();
    IR2_OPND nan = fp_label();
    IR2_OPND negative_zero = fp_label();
    IR2_OPND high = fp_label();
    IR2_OPND overflow = fp_label();
    IR2_OPND done = fp_label();

    emit_class_to_gpr(src, single, class_fpr, classes);
    la_andi(scratch, classes, 0x7);       /* negative nonzero classes */
    la_bnez(scratch, negative);
    la_andi(scratch, classes, 0x300);     /* either NaN class */
    la_bnez(scratch, nan);
    la_andi(scratch, classes, 0x8);       /* -0 is valid and converts to zero */
    la_bnez(scratch, negative_zero);

    free_alloc_gpr(scratch);
    free_alloc_gpr(classes);
    free_alloc_fpr(class_fpr);
    IR2_OPND raw = ra_alloc_itemp();
    IR2_OPND threshold63 = ra_alloc_itemp();
    IR2_OPND threshold64 = ra_alloc_itemp();

    if (single) {
        la_movfr2gr_s(raw, src);
        la_bstrpick_d(raw, raw, 31, 0);
        li_d(threshold63, UINT64_C(0x5f000000));
        li_d(threshold64, UINT64_C(0x5f800000));
    } else {
        la_movfr2gr_d(raw, src);
        li_d(threshold63, UINT64_C(0x43e0000000000000));
        li_d(threshold64, UINT64_C(0x43f0000000000000));
    }
    scratch = ra_alloc_itemp();
    la_sltu(scratch, raw, threshold64);
    la_beqz(scratch, overflow);
    la_sltu(scratch, raw, threshold63);
    la_beqz(scratch, high);
    ra_free_temp(scratch);
    ra_free_temp(threshold64);
    ra_free_temp(threshold63);

    emit_fp_disable_exceptions();
    op(converted, src);
    la_movfr2gr_d(dst, converted);
    emit_fp_commit_flags();
    la_b(done);

    la_label(negative_zero);
    emit_fp_disable_exceptions();
    op(converted, src);
    la_movfr2gr_d(dst, converted);
    emit_fp_commit_flags();
    la_b(done);

    la_label(high);
    if (single) {
        li_d(raw, UINT64_C(0xffffffff5f000000));
    } else {
        li_d(raw, UINT64_C(0x43e0000000000000));
    }
    la_movgr2fr_d(offset, raw);           /* exactly 2^63 */
    emit_fp_disable_exceptions();
    if (single) {
        la_fsub_s(adjusted, src, offset);
    } else {
        la_fsub_d(adjusted, src, offset);
    }
    emit_fp_disable_exceptions();
    op(converted, adjusted);
    la_movfr2gr_d(dst, converted);
    emit_fp_commit_flags();
    scratch = ra_alloc_itemp();
    li_d(scratch, 1);
    la_bstrins_d(dst, scratch, 63, 63);
    ra_free_temp(scratch);
    la_b(done);

    la_label(negative);
    emit_fp_disable_exceptions();
    op(converted, src);
    la_movfr2gr_d(dst, converted);
    la_beq(dst, zero_ir2_opnd, negative_rounds_zero);
    emit_fp_set_guest_nv();
    li_d(dst, 0);
    store_gpr_dst(ir1->arg.rd, dst);
    la_b(done);

    la_label(negative_rounds_zero);
    emit_fp_commit_flags();
    li_d(dst, 0);
    store_gpr_dst(ir1->arg.rd, dst);
    la_b(done);
    la_label(nan);
    emit_fp_set_guest_nv();
    li_d(dst, -1);
    la_b(done);

    la_label(overflow);
    emit_fp_set_guest_nv();
    li_d(dst, -1);
    la_label(done);
    store_gpr_dst(ir1->arg.rd, dst);

    ra_free_temp(raw);
    free_alloc_gpr(dst);
    free_alloc_fpr(offset);
    free_alloc_fpr(adjusted);
    free_alloc_fpr(converted);
    free_alloc_fpr(src);
}


static void emit_int_to_fp(const RiscvIR1 *ir1, bool single,
                           bool word, bool is_unsigned,
                           LatrFPIntToFloatOp op)
{
    IR2_OPND gpr_src = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND src = gpr_src;
    IR2_OPND integer = ra_alloc_ftemp();
    IR2_OPND dst = alloc_fpr_dst(ir1->arg.rd);
    IR2_OPND zero_extended;

    if (is_unsigned && word) {
        zero_extended = ra_alloc_itemp();
        la_mov32_zx(zero_extended, gpr_src);
        src = zero_extended;
    }
    emit_fp_rounding_mode(ir1->arg.rm, ir1->pc);
    if (word && !is_unsigned) {
        la_movgr2fr_w(integer, src);
    } else {
        la_movgr2fr_d(integer, src);
    }
    op(dst, integer);
    emit_fp_commit_flags();
    if (single) {
        emit_box_s(dst);
    }
    store_fpr_dst(ir1->arg.rd, dst);

    free_alloc_fpr(dst);
    free_alloc_fpr(integer);
    if (is_unsigned && word) {
        ra_free_temp(zero_extended);
    }
    free_alloc_gpr(gpr_src);
}


static void emit_int_to_fp_u64(const RiscvIR1 *ir1, bool single,
                               LatrFPIntToFloatOp op)
{
    IR2_OPND src = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND sign = ra_alloc_itemp();
    IR2_OPND half = ra_alloc_itemp();
    IR2_OPND sticky = ra_alloc_itemp();
    IR2_OPND integer = ra_alloc_ftemp();
    IR2_OPND dst = alloc_fpr_dst(ir1->arg.rd);
    IR2_OPND twice = ra_alloc_ftemp();
    IR2_OPND high = fp_label();
    IR2_OPND done = fp_label();

    emit_fp_rounding_mode(ir1->arg.rm, ir1->pc);
    la_slt(sign, src, zero_ir2_opnd);
    la_bnez(sign, high);

    emit_fp_disable_exceptions();
    la_movgr2fr_d(integer, src);
    op(dst, integer);
    emit_fp_commit_flags();
    la_b(done);

    la_label(high);
    la_srli_d(half, src, 1);
    la_andi(sticky, src, 1);
    la_or(half, half, sticky);
    la_movgr2fr_d(integer, half);
    emit_fp_disable_exceptions();
    op(dst, integer);
    emit_fp_commit_flags();
    if (single) {
        li_d(sticky, UINT64_C(0xffffffff40000000));
    } else {
        li_d(sticky, UINT64_C(0x4000000000000000));
    }
    la_movgr2fr_d(twice, sticky);
    emit_fp_disable_exceptions();
    if (single) {
        la_fmul_s(dst, dst, twice);
    } else {
        la_fmul_d(dst, dst, twice);
    }
    la_label(done);

    if (single) {
        emit_box_s(dst);
    }
    store_fpr_dst(ir1->arg.rd, dst);

    free_alloc_fpr(twice);
    free_alloc_fpr(dst);
    free_alloc_fpr(integer);
    ra_free_temp(sticky);
    ra_free_temp(half);
    ra_free_temp(sign);
    free_alloc_gpr(src);
}

static void emit_fp_load_store(const RiscvIR1 *ir1)
{
    IR2_OPND base = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND value;

    switch (ir1->opcode) {
    case LATR_RV_FLW: {
        IR2_OPND bits = ra_alloc_itemp();
        IR2_OPND box = ra_alloc_itemp();
        value = alloc_fpr_dst(ir1->arg.rd);
        la_ld_wu(bits, base, ir1->arg.imm);
        li_d(box, UINT64_C(0xffffffff00000000));
        la_or(bits, bits, box);
        la_movgr2fr_d(value, bits);
        store_fpr_dst(ir1->arg.rd, value);
        ra_free_temp(box);
        ra_free_temp(bits);
        free_alloc_fpr(value);
        break;
    }
    case LATR_RV_FSW: {
        IR2_OPND bits = ra_alloc_itemp();
        value = alloc_fpr_src(ir1->arg.rs2);
        la_movfr2gr_s(bits, value);
        la_st_w(bits, base, ir1->arg.imm);
        ra_free_temp(bits);
        free_alloc_fpr(value);
        break;
    }
    case LATR_RV_FLD:
        value = alloc_fpr_dst(ir1->arg.rd);
        la_fld_d(value, base, ir1->arg.imm);
        store_fpr_dst(ir1->arg.rd, value);
        free_alloc_fpr(value);
        break;
    case LATR_RV_FSD:
        value = alloc_fpr_src(ir1->arg.rs2);
        la_fst_d(value, base, ir1->arg.imm);
        free_alloc_fpr(value);
        break;
    default:
        g_assert_not_reached();
    }

    free_alloc_gpr(base);
}

static bool latr_translate_fp_native(const RiscvIR1 *ir1)
{

    switch (ir1->opcode) {
    case LATR_RV_FLW: case LATR_RV_FSW:
    case LATR_RV_FLD: case LATR_RV_FSD:
        emit_fp_load_store(ir1);
        return true;

    case LATR_RV_FMADD_S: emit_fp_fma(ir1, true, la_fmadd_s); return true;
    case LATR_RV_FMSUB_S: emit_fp_fma(ir1, true, la_fmsub_s); return true;
    case LATR_RV_FNMSUB_S: emit_fp_fma(ir1, true, la_fnmsub_s); return true;
    case LATR_RV_FNMADD_S: emit_fp_fma(ir1, true, la_fnmadd_s); return true;
    case LATR_RV_FMADD_D: emit_fp_fma(ir1, false, la_fmadd_d); return true;
    case LATR_RV_FMSUB_D: emit_fp_fma(ir1, false, la_fmsub_d); return true;
    case LATR_RV_FNMSUB_D: emit_fp_fma(ir1, false, la_fnmsub_d); return true;
    case LATR_RV_FNMADD_D: emit_fp_fma(ir1, false, la_fnmadd_d); return true;

    case LATR_RV_FADD_S:
        emit_fp_binary(ir1, true, true, true, true, la_fadd_s); return true;
    case LATR_RV_FSUB_S:
        emit_fp_binary(ir1, true, true, true, true, la_fsub_s); return true;
    case LATR_RV_FMUL_S:
        emit_fp_binary(ir1, true, true, true, true, la_fmul_s); return true;
    case LATR_RV_FDIV_S:
        emit_fp_binary(ir1, true, true, true, true, la_fdiv_s); return true;
    case LATR_RV_FADD_D:
        emit_fp_binary(ir1, false, true, true, true, la_fadd_d); return true;
    case LATR_RV_FSUB_D:
        emit_fp_binary(ir1, false, true, true, true, la_fsub_d); return true;
    case LATR_RV_FMUL_D:
        emit_fp_binary(ir1, false, true, true, true, la_fmul_d); return true;
    case LATR_RV_FDIV_D:
        emit_fp_binary(ir1, false, true, true, true, la_fdiv_d); return true;

    case LATR_RV_FSQRT_S:
        emit_fp_unary(ir1, true, true, true, true, la_fsqrt_s); return true;
    case LATR_RV_FSQRT_D:
        emit_fp_unary(ir1, false, true, true, true, la_fsqrt_d); return true;

    case LATR_RV_FSGNJ_S: emit_fp_sign_inject(ir1, true, false, false); return true;
    case LATR_RV_FSGNJN_S: emit_fp_sign_inject(ir1, true, true, false); return true;
    case LATR_RV_FSGNJX_S: emit_fp_sign_inject(ir1, true, false, true); return true;
    case LATR_RV_FSGNJ_D: emit_fp_sign_inject(ir1, false, false, false); return true;
    case LATR_RV_FSGNJN_D: emit_fp_sign_inject(ir1, false, true, false); return true;
    case LATR_RV_FSGNJX_D: emit_fp_sign_inject(ir1, false, false, true); return true;

    case LATR_RV_FMIN_S:
        emit_fp_minmax(ir1, true, true, la_fmin_s); return true;
    case LATR_RV_FMAX_S:
        emit_fp_minmax(ir1, true, false, la_fmax_s); return true;
    case LATR_RV_FMIN_D:
        emit_fp_minmax(ir1, false, true, la_fmin_d); return true;
    case LATR_RV_FMAX_D:
        emit_fp_minmax(ir1, false, false, la_fmax_d); return true;

    case LATR_RV_FCVT_S_D:
        emit_fp_d_to_s(ir1);
        return true;
    case LATR_RV_FCVT_D_S: {
        IR2_OPND src = alloc_fpr_src(ir1->arg.rs1);
        IR2_OPND dst = alloc_fpr_dst(ir1->arg.rd);
        emit_check_nanbox_s(src);
        emit_fp_rounding_mode(ir1->arg.rm, ir1->pc);
        la_fcvt_d_s(dst, src);
        emit_fp_commit_flags();
        emit_canonicalize_nan(dst, false);
        store_fpr_dst(ir1->arg.rd, dst);
        free_alloc_fpr(dst);
        free_alloc_fpr(src);
        return true;
    }

    case LATR_RV_FCVT_W_S:
        emit_fp_to_signed_int(ir1, true, true, la_ftint_w_s); return true;
    case LATR_RV_FCVT_W_D:
        emit_fp_to_signed_int(ir1, false, true, la_ftint_w_d); return true;
    case LATR_RV_FCVT_L_S:
        emit_fp_to_signed_int(ir1, true, false, la_ftint_l_s); return true;
    case LATR_RV_FCVT_L_D:
        emit_fp_to_signed_int(ir1, false, false, la_ftint_l_d); return true;
    case LATR_RV_FCVT_WU_S:
        emit_fp_to_u32(ir1, true, la_ftint_l_s); return true;
    case LATR_RV_FCVT_WU_D:
        emit_fp_to_u32(ir1, false, la_ftint_l_d); return true;
    case LATR_RV_FCVT_LU_S:
        emit_fp_to_u64(ir1, true, la_ftint_l_s); return true;
    case LATR_RV_FCVT_LU_D:
        emit_fp_to_u64(ir1, false, la_ftint_l_d); return true;

    case LATR_RV_FMV_X_W: {
        IR2_OPND src = alloc_fpr_src(ir1->arg.rs1);
        IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);
        la_movfr2gr_s(dst, src);
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
        free_alloc_fpr(src);
        return true;
    }
    case LATR_RV_FMV_W_X: {
        IR2_OPND src = alloc_gpr_src(ir1->arg.rs1);
        IR2_OPND bits = ra_alloc_itemp();
        IR2_OPND box = ra_alloc_itemp();
        IR2_OPND dst = alloc_fpr_dst(ir1->arg.rd);
        la_bstrpick_d(bits, src, 31, 0);
        li_d(box, UINT64_C(0xffffffff00000000));
        la_or(bits, bits, box);
        la_movgr2fr_d(dst, bits);
        store_fpr_dst(ir1->arg.rd, dst);
        free_alloc_fpr(dst);
        ra_free_temp(box);
        ra_free_temp(bits);
        free_alloc_gpr(src);
        return true;
    }
    case LATR_RV_FMV_X_D: {
        IR2_OPND src = alloc_fpr_src(ir1->arg.rs1);
        IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);
        la_movfr2gr_d(dst, src);
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
        free_alloc_fpr(src);
        return true;
    }
    case LATR_RV_FMV_D_X: {
        IR2_OPND src = alloc_gpr_src(ir1->arg.rs1);
        IR2_OPND dst = alloc_fpr_dst(ir1->arg.rd);
        la_movgr2fr_d(dst, src);
        store_fpr_dst(ir1->arg.rd, dst);
        free_alloc_fpr(dst);
        free_alloc_gpr(src);
        return true;
    }

    case LATR_RV_FEQ_S:
        emit_fp_compare(ir1, true, FCMP_COND_CEQ); return true;
    case LATR_RV_FLT_S:
        emit_fp_compare(ir1, true, FCMP_COND_SLT); return true;
    case LATR_RV_FLE_S:
        emit_fp_compare(ir1, true, FCMP_COND_SLE); return true;
    case LATR_RV_FEQ_D:
        emit_fp_compare(ir1, false, FCMP_COND_CEQ); return true;
    case LATR_RV_FLT_D:
        emit_fp_compare(ir1, false, FCMP_COND_SLT); return true;
    case LATR_RV_FLE_D:
        emit_fp_compare(ir1, false, FCMP_COND_SLE); return true;
    case LATR_RV_FCLASS_S: emit_fp_class(ir1, true); return true;
    case LATR_RV_FCLASS_D: emit_fp_class(ir1, false); return true;

    case LATR_RV_FCVT_S_W:
        emit_int_to_fp(ir1, true, true, false, la_ffint_s_w); return true;
    case LATR_RV_FCVT_S_WU:
        emit_int_to_fp(ir1, true, true, true, la_ffint_s_l); return true;
    case LATR_RV_FCVT_S_L:
        emit_int_to_fp(ir1, true, false, false, la_ffint_s_l); return true;
    case LATR_RV_FCVT_S_LU:
        emit_int_to_fp_u64(ir1, true, la_ffint_s_l); return true;
    case LATR_RV_FCVT_D_W:
        emit_int_to_fp(ir1, false, true, false, la_ffint_d_w); return true;
    case LATR_RV_FCVT_D_WU:
        emit_int_to_fp(ir1, false, true, true, la_ffint_d_l); return true;
    case LATR_RV_FCVT_D_L:
        emit_int_to_fp(ir1, false, false, false, la_ffint_d_l); return true;
    case LATR_RV_FCVT_D_LU:
        emit_int_to_fp_u64(ir1, false, la_ffint_d_l); return true;

    default:
        return false;
    }
}

void latr_emit_fs_gate(const RiscvIR1 *ir1)
{
    IR2_OPND illegal = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND allowed = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND status = ra_alloc_itemp();

    latr_env_load_d(status, env_offset_csr(0x300));
    la_bstrpick_d(status, status, 14, 13);
    la_beq(status, zero_ir2_opnd, illegal);
    la_b(allowed);
    la_label(illegal);
    latr_raise_illegal_instruction(ir1);
    la_label(allowed);
    ra_free_temp(status);
}

bool latr_translate_fp(const RiscvIR1 *ir1)
{
    if (!fp_opcode_uses_rm(ir1->opcode)) {
        return latr_translate_fp_native(ir1);
    }

    switch (ir1->arg.rm) {
    case 0:
    case 1:
    case 2:
    case 3:
        return latr_translate_fp_native(ir1);
    case 4:
        emit_fp_rmm_call(ir1);
        return true;
    case 7: {
        IR2_OPND frm = ra_alloc_itemp();
        IR2_OPND value = ra_alloc_itemp();
        IR2_OPND native = fp_label();
        IR2_OPND rmm = fp_label();
        IR2_OPND illegal = fp_label();
        IR2_OPND done = fp_label();

        la_ld_d(value, env_ir2_opnd, env_offset_fcsr());
        la_bstrpick_d(frm, value, 7, 5);
        li_d(value, 4);
        la_beq(frm, value, rmm);
        la_sltui(value, frm, 5);
        la_beqz(value, illegal);
        ra_free_temp(value);
        ra_free_temp(frm);

        la_label(native);
        latr_translate_fp_native(ir1);
        la_b(done);

        la_label(rmm);
        emit_fp_rmm_call(ir1);
        la_b(done);

        la_label(illegal);
        emit_fp_raise_illegal(ir1);
        la_label(done);
        return true;
    }
    default:
        emit_fp_raise_illegal(ir1);
        return true;
    }
}
