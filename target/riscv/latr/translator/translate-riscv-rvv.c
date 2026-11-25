/*
 * vsetvli / vsetivli / vsetvl lowering. The AVL selection rules and the
 * TB-exit behavior mirror QEMU's do_vsetvl()/do_vsetivli(): a vector type
 * change invalidates the specialization of the following TB.
 */
#include "latr.h"
#include "translate.h"
#include "riscv-env.h"
#include "latr-rvv-legality.h"

#define CSR_MSTATUS_ADDR 0x300
#define MSTATUS_VS_BITS (UINT64_C(3) << 9)

/*
 * Vector instructions are illegal while mstatus.VS == Off (QEMU require_rvv)
 * and while vill is set (QEMU vext_check_isa_ill).
 */
static void emit_vs_gate(const RiscvIR1 *ir1)
{
    IR2_OPND illegal = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND allowed = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND status = ra_alloc_itemp();

    latr_env_load_d(status, env_offset_csr(CSR_MSTATUS_ADDR));
    la_andi(status, status, (int)MSTATUS_VS_BITS);
    la_beq(status, zero_ir2_opnd, illegal);
    latr_env_load_d(status, env_offset_vill());
    la_bne(status, zero_ir2_opnd, illegal);
    la_b(allowed);
    la_label(illegal);
    latr_raise_illegal_instruction(ir1);
    la_label(allowed);
    ra_free_temp(status);
}

/*
 * Register-group alignment, overlap and EMUL depend only on vtype, not on the
 * dynamic vl/vstart, so they can be decided while translating. QEMU rejects
 * these encodings outright; executing them against `reg + idx` would otherwise
 * wrap at v31 and silently address a different architectural register.
 */
static bool emit_operand_legality(const RiscvIR1 *ir1)
{
    const CPUArchState *env = lsenv != 0 ? lsenv->cpu_state : 0;
    LatrVectorState state;
    LatrRvvLegality why;

    if (env == 0) {
        return true;
    }
    state.vtype = env->vtype;
    state.vill = (uint8_t)(env->vill != 0);
    state.elen = 64;
    state.xlen = 64;
    why = latr_rvv_check_operands(ir1, &state);
    if (why == LATR_RVV_LEGAL) {
        return true;
    }
    latr_raise_illegal_instruction(ir1);
    return false;
}

/*
 * frm is only sampled by the vector FP instructions that QEMU routes through
 * gen_set_rm()/gen_set_rm_chkfrm(); both raise ILLEGAL_INST for a reserved
 * value, while RMM (4) remains valid. Gating here keeps the SoftFloat status
 * from being initialised with an undefined rounding mode.
 */
static void emit_frm_gate(const RiscvIR1 *ir1)
{
    IR2_OPND illegal = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND allowed = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND value = ra_alloc_itemp();
    IR2_OPND frm = ra_alloc_itemp();

    latr_env_load_d(value, env_offset_fcsr());
    la_bstrpick_d(frm, value, 7, 5);
    la_sltui(value, frm, 5);
    la_beqz(value, illegal);
    la_b(allowed);
    la_label(illegal);
    latr_raise_illegal_instruction(ir1);
    la_label(allowed);
    ra_free_temp(frm);
    ra_free_temp(value);
}

static void emit_vstart_zero_gate(const RiscvIR1 *ir1)
{
    IR2_OPND illegal = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND allowed = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND vstart = ra_alloc_itemp();

    latr_env_load_d(vstart, env_offset_vstart());
    la_bne(vstart, zero_ir2_opnd, illegal);
    la_b(allowed);
    la_label(illegal);
    latr_raise_illegal_instruction(ir1);
    la_label(allowed);
    ra_free_temp(vstart);
}

static void emit_rvv_arg_const(int index, uint64_t value)
{
    IR2_OPND temp = ra_alloc_itemp();

    li_d(temp, (int64_t)value);
    latr_env_store_d(temp, env_offset_rvv_arg(index));
    ra_free_temp(temp);
}

static void emit_rvv_arg_opnd(int index, IR2_OPND value)
{
    latr_env_store_d(value, env_offset_rvv_arg(index));
}

static void emit_rvv_call(uintptr_t function)
{
    IR2_OPND target = ra_alloc_itemp();

    la_mov64(a0_ir2_opnd, env_ir2_opnd);
    latr_env_store_d(ra_ir2_opnd, env_offset_host_return_address());
    latr_emit_code_symbol(target, (const void *)function);
    la_jirl(ra_ir2_opnd, target, 0);
    ra_free_temp(target);
    latr_env_load_d(ra_ir2_opnd, env_offset_host_return_address());
}

static void emit_rvv_fault_check(const RiscvIR1 *ir1)
{
    IR2_OPND fault = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND done = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND cause = ra_alloc_itemp();

    latr_env_load_d(cause, env_offset_mem_fault_cause());
    la_bne(cause, zero_ir2_opnd, fault);
    la_b(done);
    la_label(fault);
    latr_raise_mem_fault(ir1);
    la_label(done);
    ra_free_temp(cause);
}

#define RVV_ALU_ENTRY(tag, op_value, kind_value) \
    case LATR_RV_##tag: *op = op_value; *kind = kind_value; return true
#define RVV_CMP_ENTRY(tag, cond_value, kind_value) \
    case LATR_RV_##tag: *cond = cond_value; *kind = kind_value; return true

/* Map an OPIVV/OPIVX/OPIVI instruction to the ALU helper opcode. */
static bool rvv_alu_op(int opcode, int *op, int *kind, int *imm_signed)
{
    *imm_signed = 1;
    switch (opcode) {
    RVV_ALU_ENTRY(VADD_VV, LATR_RVV_OP_ADD, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VADD_VX, LATR_RVV_OP_ADD, LATR_RVV_SRC_SCALAR);
    RVV_ALU_ENTRY(VADD_VI, LATR_RVV_OP_ADD, LATR_RVV_SRC_IMM);
    RVV_ALU_ENTRY(VSUB_VV, LATR_RVV_OP_SUB, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VSUB_VX, LATR_RVV_OP_SUB, LATR_RVV_SRC_SCALAR);
    RVV_ALU_ENTRY(VAND_VV, LATR_RVV_OP_AND, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VAND_VX, LATR_RVV_OP_AND, LATR_RVV_SRC_SCALAR);
    RVV_ALU_ENTRY(VAND_VI, LATR_RVV_OP_AND, LATR_RVV_SRC_IMM);
    RVV_ALU_ENTRY(VOR_VV, LATR_RVV_OP_OR, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VOR_VX, LATR_RVV_OP_OR, LATR_RVV_SRC_SCALAR);
    RVV_ALU_ENTRY(VOR_VI, LATR_RVV_OP_OR, LATR_RVV_SRC_IMM);
    RVV_ALU_ENTRY(VXOR_VV, LATR_RVV_OP_XOR, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VXOR_VX, LATR_RVV_OP_XOR, LATR_RVV_SRC_SCALAR);
    RVV_ALU_ENTRY(VXOR_VI, LATR_RVV_OP_XOR, LATR_RVV_SRC_IMM);
    RVV_ALU_ENTRY(VSLL_VV, LATR_RVV_OP_SLL, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VSLL_VX, LATR_RVV_OP_SLL, LATR_RVV_SRC_SCALAR);
    case LATR_RV_VSLL_VI:
        *op = LATR_RVV_OP_SLL; *kind = LATR_RVV_SRC_IMM; *imm_signed = 0; return true;
    RVV_ALU_ENTRY(VSRL_VV, LATR_RVV_OP_SRL, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VSRL_VX, LATR_RVV_OP_SRL, LATR_RVV_SRC_SCALAR);
    case LATR_RV_VSRL_VI:
        *op = LATR_RVV_OP_SRL; *kind = LATR_RVV_SRC_IMM; *imm_signed = 0; return true;
    RVV_ALU_ENTRY(VSRA_VV, LATR_RVV_OP_SRA, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VSRA_VX, LATR_RVV_OP_SRA, LATR_RVV_SRC_SCALAR);
    case LATR_RV_VSRA_VI:
        *op = LATR_RVV_OP_SRA; *kind = LATR_RVV_SRC_IMM; *imm_signed = 0; return true;
    RVV_ALU_ENTRY(VMUL_VV, LATR_RVV_OP_MUL, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VMUL_VX, LATR_RVV_OP_MUL, LATR_RVV_SRC_SCALAR);
    RVV_ALU_ENTRY(VDIVU_VV, LATR_RVV_OP_DIVU, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VDIVU_VX, LATR_RVV_OP_DIVU, LATR_RVV_SRC_SCALAR);
    RVV_ALU_ENTRY(VDIV_VV, LATR_RVV_OP_DIV, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VDIV_VX, LATR_RVV_OP_DIV, LATR_RVV_SRC_SCALAR);
    RVV_ALU_ENTRY(VREMU_VV, LATR_RVV_OP_REMU, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VREMU_VX, LATR_RVV_OP_REMU, LATR_RVV_SRC_SCALAR);
    RVV_ALU_ENTRY(VREM_VV, LATR_RVV_OP_REM, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VREM_VX, LATR_RVV_OP_REM, LATR_RVV_SRC_SCALAR);
    RVV_ALU_ENTRY(VMINU_VV, LATR_RVV_OP_MINU, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VMINU_VX, LATR_RVV_OP_MINU, LATR_RVV_SRC_SCALAR);
    RVV_ALU_ENTRY(VMIN_VV, LATR_RVV_OP_MIN, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VMIN_VX, LATR_RVV_OP_MIN, LATR_RVV_SRC_SCALAR);
    RVV_ALU_ENTRY(VMAXU_VV, LATR_RVV_OP_MAXU, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VMAXU_VX, LATR_RVV_OP_MAXU, LATR_RVV_SRC_SCALAR);
    RVV_ALU_ENTRY(VMAX_VV, LATR_RVV_OP_MAX, LATR_RVV_SRC_VECTOR);
    RVV_ALU_ENTRY(VMAX_VX, LATR_RVV_OP_MAX, LATR_RVV_SRC_SCALAR);
    default: return false;
    }
}

static bool rvv_cmp_op(int opcode, int *cond, int *kind)
{
    switch (opcode) {
    RVV_CMP_ENTRY(VMSEQ_VV, LATR_RVV_CMP_EQ, LATR_RVV_SRC_VECTOR);
    RVV_CMP_ENTRY(VMSEQ_VX, LATR_RVV_CMP_EQ, LATR_RVV_SRC_SCALAR);
    RVV_CMP_ENTRY(VMSEQ_VI, LATR_RVV_CMP_EQ, LATR_RVV_SRC_IMM);
    RVV_CMP_ENTRY(VMSNE_VV, LATR_RVV_CMP_NE, LATR_RVV_SRC_VECTOR);
    RVV_CMP_ENTRY(VMSNE_VX, LATR_RVV_CMP_NE, LATR_RVV_SRC_SCALAR);
    RVV_CMP_ENTRY(VMSNE_VI, LATR_RVV_CMP_NE, LATR_RVV_SRC_IMM);
    RVV_CMP_ENTRY(VMSLT_VV, LATR_RVV_CMP_LT, LATR_RVV_SRC_VECTOR);
    RVV_CMP_ENTRY(VMSLT_VX, LATR_RVV_CMP_LT, LATR_RVV_SRC_SCALAR);
    RVV_CMP_ENTRY(VMSLT_VI, LATR_RVV_CMP_LT, LATR_RVV_SRC_IMM);
    RVV_CMP_ENTRY(VMSLTU_VV, LATR_RVV_CMP_LTU, LATR_RVV_SRC_VECTOR);
    RVV_CMP_ENTRY(VMSLTU_VX, LATR_RVV_CMP_LTU, LATR_RVV_SRC_SCALAR);
    RVV_CMP_ENTRY(VMSLTU_VI, LATR_RVV_CMP_LTU, LATR_RVV_SRC_IMM);
    RVV_CMP_ENTRY(VMSLE_VV, LATR_RVV_CMP_LE, LATR_RVV_SRC_VECTOR);
    RVV_CMP_ENTRY(VMSLE_VX, LATR_RVV_CMP_LE, LATR_RVV_SRC_SCALAR);
    RVV_CMP_ENTRY(VMSLE_VI, LATR_RVV_CMP_LE, LATR_RVV_SRC_IMM);
    RVV_CMP_ENTRY(VMSLEU_VV, LATR_RVV_CMP_LEU, LATR_RVV_SRC_VECTOR);
    RVV_CMP_ENTRY(VMSLEU_VX, LATR_RVV_CMP_LEU, LATR_RVV_SRC_SCALAR);
    RVV_CMP_ENTRY(VMSLEU_VI, LATR_RVV_CMP_LEU, LATR_RVV_SRC_IMM);
    RVV_CMP_ENTRY(VMSGTU_VX, LATR_RVV_CMP_GTU, LATR_RVV_SRC_SCALAR);
    RVV_CMP_ENTRY(VMSGTU_VI, LATR_RVV_CMP_GTU, LATR_RVV_SRC_IMM);
    RVV_CMP_ENTRY(VMSGT_VX, LATR_RVV_CMP_GT, LATR_RVV_SRC_SCALAR);
    RVV_CMP_ENTRY(VMSGT_VI, LATR_RVV_CMP_GT, LATR_RVV_SRC_IMM);
    default: return false;
    }
}

static bool rvv_mask_logic_op(int opcode, unsigned *op)
{
    switch (opcode) {
    case LATR_RV_VMAND_MM:  *op = LATR_RVV_MASK_AND; return true;
    case LATR_RV_VMNAND_MM: *op = LATR_RVV_MASK_NAND; return true;
    case LATR_RV_VMANDN_MM: *op = LATR_RVV_MASK_ANDN; return true;
    case LATR_RV_VMXOR_MM:  *op = LATR_RVV_MASK_XOR; return true;
    case LATR_RV_VMOR_MM:   *op = LATR_RVV_MASK_OR; return true;
    case LATR_RV_VMNOR_MM:  *op = LATR_RVV_MASK_NOR; return true;
    case LATR_RV_VMORN_MM:  *op = LATR_RVV_MASK_ORN; return true;
    case LATR_RV_VMXNOR_MM: *op = LATR_RVV_MASK_XNOR; return true;
    default: return false;
    }
}

static bool rvv_mask_scan_op(int opcode, unsigned *op)
{
    switch (opcode) {
    case LATR_RV_VCPOP_M: *op = LATR_RVV_SCAN_VCPOP; return true;
    case LATR_RV_VFIRST_M: *op = LATR_RVV_SCAN_VFIRST; return true;
    case LATR_RV_VMSBF_M: *op = LATR_RVV_SCAN_VMSBF; return true;
    case LATR_RV_VMSIF_M: *op = LATR_RVV_SCAN_VMSIF; return true;
    case LATR_RV_VMSOF_M: *op = LATR_RVV_SCAN_VMSOF; return true;
    case LATR_RV_VIOTA_M: *op = LATR_RVV_SCAN_VIOTA; return true;
    case LATR_RV_VID_V:   *op = LATR_RVV_SCAN_VID; return true;
    default: return false;
    }
}

#define RVV_X_ENTRY(tag, opv, kindv, widev, signedv) \
    case LATR_RV_##tag: *op = opv; *kind = kindv; \
        *wide_src = widev; *imm_signed = signedv; return true
#define RVV_X_VV(tag, opv)  RVV_X_ENTRY(tag##_VV, opv, LATR_RVV_SRC_VECTOR, 0, 1)
#define RVV_X_VX(tag, opv)  RVV_X_ENTRY(tag##_VX, opv, LATR_RVV_SRC_SCALAR, 0, 1)
#define RVV_X_VI(tag, opv, sgn) RVV_X_ENTRY(tag##_VI, opv, LATR_RVV_SRC_IMM, 0, sgn)
#define RVV_X_WV(tag, opv)  RVV_X_ENTRY(tag##_WV, opv, LATR_RVV_SRC_VECTOR, 1, 1)
#define RVV_X_WX(tag, opv, sgn) RVV_X_ENTRY(tag##_WX, opv, LATR_RVV_SRC_SCALAR, 1, sgn)
#define RVV_X_WI(tag, opv, sgn) RVV_X_ENTRY(tag##_WI, opv, LATR_RVV_SRC_IMM, 0, sgn)

#define RVV_FMA_ENTRY(tag, variant, kindv) \
    case LATR_RV_##tag: *fma = variant; *kind = kindv; return true

/* Fused multiply-add: variant index carries the sign rules (see translate.h). */
static bool rvv_fma_op(int opcode, unsigned *fma, unsigned *kind)
{
    switch (opcode) {
    RVV_FMA_ENTRY(VFMACC_VV, LATR_RVV_FMA_MACC, LATR_RVV_SRC_VECTOR);
    RVV_FMA_ENTRY(VFMACC_VF, LATR_RVV_FMA_MACC, LATR_RVV_SRC_SCALAR);
    RVV_FMA_ENTRY(VFNMACC_VV, LATR_RVV_FMA_NMACC, LATR_RVV_SRC_VECTOR);
    RVV_FMA_ENTRY(VFNMACC_VF, LATR_RVV_FMA_NMACC, LATR_RVV_SRC_SCALAR);
    RVV_FMA_ENTRY(VFMSAC_VV, LATR_RVV_FMA_MSAC, LATR_RVV_SRC_VECTOR);
    RVV_FMA_ENTRY(VFMSAC_VF, LATR_RVV_FMA_MSAC, LATR_RVV_SRC_SCALAR);
    RVV_FMA_ENTRY(VFNMSC_VV, LATR_RVV_FMA_NMSAC, LATR_RVV_SRC_VECTOR);
    RVV_FMA_ENTRY(VFNMSC_VF, LATR_RVV_FMA_NMSAC, LATR_RVV_SRC_SCALAR);
    RVV_FMA_ENTRY(VFMADD_VV, LATR_RVV_FMA_MADD, LATR_RVV_SRC_VECTOR);
    RVV_FMA_ENTRY(VFMADD_VF, LATR_RVV_FMA_MADD, LATR_RVV_SRC_SCALAR);
    RVV_FMA_ENTRY(VFNMADD_VV, LATR_RVV_FMA_NMADD, LATR_RVV_SRC_VECTOR);
    RVV_FMA_ENTRY(VFNMADD_VF, LATR_RVV_FMA_NMADD, LATR_RVV_SRC_SCALAR);
    RVV_FMA_ENTRY(VFMSUB_VV, LATR_RVV_FMA_MSUB, LATR_RVV_SRC_VECTOR);
    RVV_FMA_ENTRY(VFMSUB_VF, LATR_RVV_FMA_MSUB, LATR_RVV_SRC_SCALAR);
    RVV_FMA_ENTRY(VFNMSSUB_VV, LATR_RVV_FMA_NMSUB, LATR_RVV_SRC_VECTOR);
    RVV_FMA_ENTRY(VFNMSSUB_VF, LATR_RVV_FMA_NMSUB, LATR_RVV_SRC_SCALAR);
    /* Widening: SEW inputs, 2*SEW accumulator. */
    RVV_FMA_ENTRY(VFWMACC_VV, LATR_RVV_FWMA_MACC, LATR_RVV_SRC_VECTOR);
    RVV_FMA_ENTRY(VFWMACC_VF, LATR_RVV_FWMA_MACC, LATR_RVV_SRC_SCALAR);
    RVV_FMA_ENTRY(VFWNMACC_VV, LATR_RVV_FWMA_NMACC, LATR_RVV_SRC_VECTOR);
    RVV_FMA_ENTRY(VFWNMACC_VF, LATR_RVV_FWMA_NMACC, LATR_RVV_SRC_SCALAR);
    RVV_FMA_ENTRY(VFWMSAC_VV, LATR_RVV_FWMA_MSAC, LATR_RVV_SRC_VECTOR);
    RVV_FMA_ENTRY(VFWMSAC_VF, LATR_RVV_FWMA_MSAC, LATR_RVV_SRC_SCALAR);
    RVV_FMA_ENTRY(VFWNMSAC_VV, LATR_RVV_FWMA_NMSAC, LATR_RVV_SRC_VECTOR);
    RVV_FMA_ENTRY(VFWNMSAC_VF, LATR_RVV_FWMA_NMSAC, LATR_RVV_SRC_SCALAR);
    default: return false;
    }
}

static bool rvv_fp_op(int opcode, unsigned *op, unsigned *kind)
{
    *kind = LATR_RVV_SRC_VECTOR;
    switch (opcode) {
    case LATR_RV_VFADD_VV: *op = LATR_RVV_FP_ADD; return true;
    case LATR_RV_VFADD_VF: *op = LATR_RVV_FP_ADD; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFSUB_VV: *op = LATR_RVV_FP_SUB; return true;
    case LATR_RV_VFSUB_VF: *op = LATR_RVV_FP_SUB; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFMUL_VV: *op = LATR_RVV_FP_MUL; return true;
    case LATR_RV_VFMUL_VF: *op = LATR_RVV_FP_MUL; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFDIV_VV: *op = LATR_RVV_FP_DIV; return true;
    case LATR_RV_VFDIV_VF: *op = LATR_RVV_FP_DIV; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFMIN_VV: *op = LATR_RVV_FP_MIN; return true;
    case LATR_RV_VFMIN_VF: *op = LATR_RVV_FP_MIN; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFMAX_VV: *op = LATR_RVV_FP_MAX; return true;
    case LATR_RV_VFMAX_VF: *op = LATR_RVV_FP_MAX; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFSQRT_V: *op = LATR_RVV_FP_SQRT; return true;
    case LATR_RV_VFSGNJ_VV: *op = LATR_RVV_FP_SGNJ; return true;
    case LATR_RV_VFSGNJ_VF: *op = LATR_RVV_FP_SGNJ; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFSGNJN_VV: *op = LATR_RVV_FP_SGNJN; return true;
    case LATR_RV_VFSGNJN_VF: *op = LATR_RVV_FP_SGNJN; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFSGNJX_VV: *op = LATR_RVV_FP_SGNJX; return true;
    case LATR_RV_VFSGNJX_VF: *op = LATR_RVV_FP_SGNJX; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFMERGE_VFM: *op = LATR_RVV_FP_MERGE; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFMV_V_F: *op = LATR_RVV_FP_MV_V_F; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFMV_F_S: *op = LATR_RVV_FP_MV_F_S; return true;
    case LATR_RV_VFMV_S_F: *op = LATR_RVV_FP_MV_S_F; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VMFEQ_VV: *op = LATR_RVV_FP_CMP_EQ; return true;
    case LATR_RV_VMFEQ_VF: *op = LATR_RVV_FP_CMP_EQ; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VMFNE_VV: *op = LATR_RVV_FP_CMP_NE; return true;
    case LATR_RV_VMFNE_VF: *op = LATR_RVV_FP_CMP_NE; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VMFLT_VV: *op = LATR_RVV_FP_CMP_LT; return true;
    case LATR_RV_VMFLT_VF: *op = LATR_RVV_FP_CMP_LT; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VMFLE_VV: *op = LATR_RVV_FP_CMP_LE; return true;
    case LATR_RV_VMFLE_VF: *op = LATR_RVV_FP_CMP_LE; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VMFGT_VF: *op = LATR_RVV_FP_CMP_GT; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VMFGE_VF: *op = LATR_RVV_FP_CMP_GE; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFCLASS_V: *op = LATR_RVV_FP_CLASS; return true;
    case LATR_RV_VFWADD_VV: *op = LATR_RVV_FP_FWADD; return true;
    case LATR_RV_VFWADD_VF: *op = LATR_RVV_FP_FWADD; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFWSUB_VV: *op = LATR_RVV_FP_FWSUB; return true;
    case LATR_RV_VFWSUB_VF: *op = LATR_RVV_FP_FWSUB; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFWMUL_VV: *op = LATR_RVV_FP_FWMUL; return true;
    case LATR_RV_VFWMUL_VF: *op = LATR_RVV_FP_FWMUL; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFWADD_WV: *op = LATR_RVV_FP_FWADD; return true;
    case LATR_RV_VFWADD_WF: *op = LATR_RVV_FP_FWADD; *kind = LATR_RVV_SRC_SCALAR; return true;
    case LATR_RV_VFWSUB_WV: *op = LATR_RVV_FP_FWSUB; return true;
    case LATR_RV_VFWSUB_WF: *op = LATR_RVV_FP_FWSUB; *kind = LATR_RVV_SRC_SCALAR; return true;
    default: return false;
    }
}

static bool rvv_cvt_op(int opcode, unsigned *op)
{
    switch (opcode) {
    case LATR_RV_VFCVT_XU_F_V: *op = LATR_RVV_CVT_XU_F; return true;
    case LATR_RV_VFCVT_X_F_V: *op = LATR_RVV_CVT_X_F; return true;
    case LATR_RV_VFCVT_F_XU_V: *op = LATR_RVV_CVT_F_XU; return true;
    case LATR_RV_VFCVT_F_X_V: *op = LATR_RVV_CVT_F_X; return true;
    case LATR_RV_VFCVT_RTZ_XU_F_V: *op = LATR_RVV_CVT_RTZ_XU_F; return true;
    case LATR_RV_VFCVT_RTZ_X_F_V: *op = LATR_RVV_CVT_RTZ_X_F; return true;
    case LATR_RV_VFWCVT_F_F_V: *op = LATR_RVV_CVT_FW_F_F; return true;
    case LATR_RV_VFWCVT_F_X_V: *op = LATR_RVV_CVT_FW_F_X; return true;
    case LATR_RV_VFWCVT_F_XU_V: *op = LATR_RVV_CVT_FW_F_XU; return true;
    case LATR_RV_VFWCVT_X_F_V: *op = LATR_RVV_CVT_FW_X_F; return true;
    case LATR_RV_VFWCVT_XU_F_V: *op = LATR_RVV_CVT_FW_XU_F; return true;
    case LATR_RV_VFWCVT_RTZ_X_F_V: *op = LATR_RVV_CVT_FW_X_F; return true;
    case LATR_RV_VFWCVT_RTZ_XU_F_V: *op = LATR_RVV_CVT_FW_XU_F; return true;
    case LATR_RV_VFNCVT_F_F_W: *op = LATR_RVV_CVT_FN_F_F; return true;
    case LATR_RV_VFNCVT_ROD_F_F_W: *op = LATR_RVV_CVT_FN_ROD_F_F; return true;
    case LATR_RV_VFNCVT_X_F_W: *op = LATR_RVV_CVT_FN_X_F; return true;
    case LATR_RV_VFNCVT_XU_F_W: *op = LATR_RVV_CVT_FN_XU_F; return true;
    case LATR_RV_VFNCVT_F_X_W: *op = LATR_RVV_CVT_FN_F_X; return true;
    case LATR_RV_VFNCVT_F_XU_W: *op = LATR_RVV_CVT_FN_F_XU; return true;
    case LATR_RV_VFNCVT_RTZ_X_F_W: *op = LATR_RVV_CVT_FN_X_F; return true;
    case LATR_RV_VFNCVT_RTZ_XU_F_W: *op = LATR_RVV_CVT_FN_XU_F; return true;
    default: return false;
    }
}

static bool rvv_extra_op(int opcode, unsigned *op, unsigned *kind,
                         int *wide_src, int *imm_signed)
{
    *wide_src = 0;
    *imm_signed = 1;
    switch (opcode) {
    RVV_X_VV(VWADD, LATR_RVV_X_WADD);
    RVV_X_VX(VWADD, LATR_RVV_X_WADD);
    RVV_X_VV(VWADDU, LATR_RVV_X_WADDU);
    RVV_X_VX(VWADDU, LATR_RVV_X_WADDU);
    RVV_X_VV(VWSUB, LATR_RVV_X_WSUB);
    RVV_X_VX(VWSUB, LATR_RVV_X_WSUB);
    RVV_X_VV(VWSUBU, LATR_RVV_X_WSUBU);
    RVV_X_VX(VWSUBU, LATR_RVV_X_WSUBU);
    RVV_X_VV(VWMUL, LATR_RVV_X_WMUL);
    RVV_X_VX(VWMUL, LATR_RVV_X_WMUL);
    RVV_X_VV(VWMULU, LATR_RVV_X_WMULU);
    RVV_X_VX(VWMULU, LATR_RVV_X_WMULU);
    RVV_X_VV(VWMULSU, LATR_RVV_X_WMULSU);
    RVV_X_VX(VWMULSU, LATR_RVV_X_WMULSU);
    RVV_X_VV(VWMACC, LATR_RVV_X_WMACC);
    RVV_X_VX(VWMACC, LATR_RVV_X_WMACC);
    RVV_X_VV(VWMACCU, LATR_RVV_X_WMACCU);
    RVV_X_VX(VWMACCU, LATR_RVV_X_WMACCU);
    RVV_X_VV(VWMACCSU, LATR_RVV_X_WMACCSU);
    RVV_X_VX(VWMACCSU, LATR_RVV_X_WMACCSU);
    RVV_X_ENTRY(VWMACCUS_VX, LATR_RVV_X_WMACCUS, LATR_RVV_SRC_SCALAR, 0, 1);
    RVV_X_WV(VNSRL, LATR_RVV_X_NSRL);
    RVV_X_WX(VNSRL, LATR_RVV_X_NSRL, 0);
    RVV_X_WI(VNSRL, LATR_RVV_X_NSRL, 0);
    RVV_X_WV(VNSRA, LATR_RVV_X_NSRA);
    RVV_X_WX(VNSRA, LATR_RVV_X_NSRA, 0);
    RVV_X_WI(VNSRA, LATR_RVV_X_NSRA, 0);
    RVV_X_WV(VNCLIP, LATR_RVV_X_NCLIP);
    RVV_X_WX(VNCLIP, LATR_RVV_X_NCLIP, 0);
    RVV_X_WI(VNCLIP, LATR_RVV_X_NCLIP, 0);
    RVV_X_WV(VNCLIPU, LATR_RVV_X_NCLIPU);
    RVV_X_WX(VNCLIPU, LATR_RVV_X_NCLIPU, 0);
    RVV_X_WI(VNCLIPU, LATR_RVV_X_NCLIPU, 0);
    RVV_X_VV(VSADD, LATR_RVV_X_SADD);
    RVV_X_VX(VSADD, LATR_RVV_X_SADD);
    RVV_X_VI(VSADD, LATR_RVV_X_SADD, 1);
    RVV_X_VV(VSADDU, LATR_RVV_X_SADDU);
    RVV_X_VX(VSADDU, LATR_RVV_X_SADDU);
    RVV_X_VI(VSADDU, LATR_RVV_X_SADDU, 1);
    RVV_X_VV(VSSUB, LATR_RVV_X_SSUB);
    RVV_X_VX(VSSUB, LATR_RVV_X_SSUB);
    RVV_X_VV(VSSUBU, LATR_RVV_X_SSUBU);
    RVV_X_VX(VSSUBU, LATR_RVV_X_SSUBU);
    RVV_X_VV(VAADD, LATR_RVV_X_AADD);
    RVV_X_VX(VAADD, LATR_RVV_X_AADD);
    RVV_X_VV(VAADDU, LATR_RVV_X_AADDU);
    RVV_X_VX(VAADDU, LATR_RVV_X_AADDU);
    RVV_X_VV(VASUB, LATR_RVV_X_ASUB);
    RVV_X_VX(VASUB, LATR_RVV_X_ASUB);
    RVV_X_VV(VASUBU, LATR_RVV_X_ASUBU);
    RVV_X_VX(VASUBU, LATR_RVV_X_ASUBU);
    RVV_X_VV(VSSRL, LATR_RVV_X_SSRL);
    RVV_X_VX(VSSRL, LATR_RVV_X_SSRL);
    RVV_X_VI(VSSRL, LATR_RVV_X_SSRL, 0);
    RVV_X_VV(VSSRA, LATR_RVV_X_SSRA);
    RVV_X_VX(VSSRA, LATR_RVV_X_SSRA);
    RVV_X_VI(VSSRA, LATR_RVV_X_SSRA, 0);
    RVV_X_ENTRY(VZEXT_VF2, LATR_RVV_X_ZEXT2, LATR_RVV_SRC_VECTOR, 0, 0);
    RVV_X_ENTRY(VZEXT_VF4, LATR_RVV_X_ZEXT4, LATR_RVV_SRC_VECTOR, 0, 0);
    RVV_X_ENTRY(VZEXT_VF8, LATR_RVV_X_ZEXT8, LATR_RVV_SRC_VECTOR, 0, 0);
    RVV_X_ENTRY(VSEXT_VF2, LATR_RVV_X_SEXT2, LATR_RVV_SRC_VECTOR, 0, 1);
    RVV_X_ENTRY(VSEXT_VF4, LATR_RVV_X_SEXT4, LATR_RVV_SRC_VECTOR, 0, 1);
    RVV_X_ENTRY(VSEXT_VF8, LATR_RVV_X_SEXT8, LATR_RVV_SRC_VECTOR, 0, 1);
    RVV_X_VX(VRSUB, LATR_RVV_X_RSUB);
    RVV_X_VI(VRSUB, LATR_RVV_X_RSUB, 1);
    RVV_X_VV(VMULH, LATR_RVV_X_MULH);
    RVV_X_VX(VMULH, LATR_RVV_X_MULH);
    RVV_X_VV(VMULHU, LATR_RVV_X_MULHU);
    RVV_X_VX(VMULHU, LATR_RVV_X_MULHU);
    RVV_X_VV(VMULHSU, LATR_RVV_X_MULHSU);
    RVV_X_VX(VMULHSU, LATR_RVV_X_MULHSU);
    /* .wv/.wx: the second operand is 2*SEW wide, so wide_src=1 and the helper
     * reads it at dw instead of SEW. */
    RVV_X_WV(VWADD, LATR_RVV_X_WADD);
    RVV_X_WX(VWADD, LATR_RVV_X_WADD, 1);
    RVV_X_WV(VWADDU, LATR_RVV_X_WADDU);
    RVV_X_WX(VWADDU, LATR_RVV_X_WADDU, 0);
    RVV_X_WV(VWSUB, LATR_RVV_X_WSUB);
    RVV_X_WX(VWSUB, LATR_RVV_X_WSUB, 1);
    RVV_X_WV(VWSUBU, LATR_RVV_X_WSUBU);
    RVV_X_WX(VWSUBU, LATR_RVV_X_WSUBU, 0);
    RVV_X_VV(VSMUL, LATR_RVV_X_SMUL);
    RVV_X_VX(VSMUL, LATR_RVV_X_SMUL);
    default: return false;
    }
}

static bool rvv_perm_op(int opcode, unsigned *op, unsigned *kind,
                        unsigned *index_bits)
{
    *index_bits = 0;
    *kind = LATR_RVV_SRC_SCALAR;
    switch (opcode) {
    case LATR_RV_VSLIDEUP_VX:    *op = LATR_RVV_PERM_SLIDEUP; return true;
    case LATR_RV_VSLIDEUP_VI:    *op = LATR_RVV_PERM_SLIDEUP; *kind = LATR_RVV_SRC_IMM; return true;
    case LATR_RV_VSLIDEDOWN_VX:  *op = LATR_RVV_PERM_SLIDEDOWN; return true;
    case LATR_RV_VSLIDEDOWN_VI:  *op = LATR_RVV_PERM_SLIDEDOWN; *kind = LATR_RVV_SRC_IMM; return true;
    case LATR_RV_VSLIDE1UP_VX:   *op = LATR_RVV_PERM_SLIDE1UP; return true;
    case LATR_RV_VSLIDE1DOWN_VX: *op = LATR_RVV_PERM_SLIDE1DOWN; return true;
    case LATR_RV_VRGATHER_VV:    *op = LATR_RVV_PERM_GATHER; *kind = LATR_RVV_SRC_VECTOR; return true;
    case LATR_RV_VRGATHER_VX:    *op = LATR_RVV_PERM_GATHER; return true;
    case LATR_RV_VRGATHER_VI:    *op = LATR_RVV_PERM_GATHER; *kind = LATR_RVV_SRC_IMM; return true;
    case LATR_RV_VRGATHEREI16_VV:*op = LATR_RVV_PERM_GATHER16; *kind = LATR_RVV_SRC_VECTOR; *index_bits = 16; return true;
    case LATR_RV_VCOMPRESS_VM:   *op = LATR_RVV_PERM_COMPRESS; return true;
    default: return false;
    }
}

static bool rvv_reduce_op(int opcode, unsigned *op)
{
    switch (opcode) {
    case LATR_RV_VREDSUM_VS:   *op = LATR_RVV_RED_SUM; return true;
    case LATR_RV_VREDAND_VS:   *op = LATR_RVV_RED_AND; return true;
    case LATR_RV_VREDOR_VS:    *op = LATR_RVV_RED_OR; return true;
    case LATR_RV_VREDXOR_VS:   *op = LATR_RVV_RED_XOR; return true;
    case LATR_RV_VREDMINU_VS:  *op = LATR_RVV_RED_MINU; return true;
    case LATR_RV_VREDMIN_VS:   *op = LATR_RVV_RED_MIN; return true;
    case LATR_RV_VREDMAXU_VS:  *op = LATR_RVV_RED_MAXU; return true;
    case LATR_RV_VREDMAX_VS:   *op = LATR_RVV_RED_MAX; return true;
    case LATR_RV_VWREDSUM_VS:  *op = LATR_RVV_RED_WSUM; return true;
    case LATR_RV_VWREDSUMU_VS: *op = LATR_RVV_RED_WSUMU; return true;
    case LATR_RV_VFREDUSUM_VS: *op = LATR_RVV_RED_FUSUM; return true;
    case LATR_RV_VFREDOSUM_VS: *op = LATR_RVV_RED_FOSUM; return true;
    case LATR_RV_VFREDMIN_VS:  *op = LATR_RVV_RED_FMIN; return true;
    case LATR_RV_VFREDMAX_VS:  *op = LATR_RVV_RED_FMAX; return true;
    case LATR_RV_VFWREDUSUM_VS:*op = LATR_RVV_RED_FWUSUM; return true;
    case LATR_RV_VFWREDOSUM_VS:*op = LATR_RVV_RED_FWOSUM; return true;
    default: return false;
    }
}

/*
 * QEMU requires vstart == 0 for the mask scans, viota, vcompress and every
 * reduction (vext_check_reduction): those instructions cannot resume, because
 * the lanes below vstart were already folded into state that is not kept.
 */
static bool rvv_requires_vstart_zero(LatrRiscvOpcode opcode)
{
    unsigned unused_op = 0;

    if (rvv_reduce_op((int)opcode, &unused_op)) {
        return true;
    }
    switch (opcode) {
    case LATR_RV_VCPOP_M:
    case LATR_RV_VFIRST_M:
    case LATR_RV_VMSBF_M:
    case LATR_RV_VMSIF_M:
    case LATR_RV_VMSOF_M:
    case LATR_RV_VIOTA_M:
    case LATR_RV_VCOMPRESS_VM:
        return true;
    default:
        return false;
    }
}

static bool rvv_mem_op(int opcode, unsigned *eew, int *is_load,
                       unsigned *mode){
    *mode = 0;
    switch (opcode) {
    case LATR_RV_VLE8_V:  *eew = 0; *is_load = 1; return true;
    case LATR_RV_VLE16_V: *eew = 1; *is_load = 1; return true;
    case LATR_RV_VLE32_V: *eew = 2; *is_load = 1; return true;
    case LATR_RV_VLE64_V: *eew = 3; *is_load = 1; return true;
    case LATR_RV_VSE8_V:  *eew = 0; *is_load = 0; return true;
    case LATR_RV_VSE16_V: *eew = 1; *is_load = 0; return true;
    case LATR_RV_VSE32_V: *eew = 2; *is_load = 0; return true;
    case LATR_RV_VSE64_V: *eew = 3; *is_load = 0; return true;
    case LATR_RV_VLSE8_V:  *eew = 0; *is_load = 1; *mode = 1; return true;
    case LATR_RV_VLSE16_V: *eew = 1; *is_load = 1; *mode = 1; return true;
    case LATR_RV_VLSE32_V: *eew = 2; *is_load = 1; *mode = 1; return true;
    case LATR_RV_VLSE64_V: *eew = 3; *is_load = 1; *mode = 1; return true;
    case LATR_RV_VSSE8_V:  *eew = 0; *is_load = 0; *mode = 1; return true;
    case LATR_RV_VSSE16_V: *eew = 1; *is_load = 0; *mode = 1; return true;
    case LATR_RV_VSSE32_V: *eew = 2; *is_load = 0; *mode = 1; return true;
    case LATR_RV_VSSE64_V: *eew = 3; *is_load = 0; *mode = 1; return true;
    case LATR_RV_VLXEI8_V:  *eew = 0; *is_load = 1; *mode = 2; return true;
    case LATR_RV_VLXEI16_V: *eew = 1; *is_load = 1; *mode = 2; return true;
    case LATR_RV_VLXEI32_V: *eew = 2; *is_load = 1; *mode = 2; return true;
    case LATR_RV_VLXEI64_V: *eew = 3; *is_load = 1; *mode = 2; return true;
    case LATR_RV_VSXEI8_V:  *eew = 0; *is_load = 0; *mode = 2; return true;
    case LATR_RV_VSXEI16_V: *eew = 1; *is_load = 0; *mode = 2; return true;
    case LATR_RV_VSXEI32_V: *eew = 2; *is_load = 0; *mode = 2; return true;
    case LATR_RV_VSXEI64_V: *eew = 3; *is_load = 0; *mode = 2; return true;
    case LATR_RV_VLM_V:     *eew = 0; *is_load = 1; *mode = 3; return true;
    case LATR_RV_VSM_V:     *eew = 0; *is_load = 0; *mode = 3; return true;
    case LATR_RV_VLE8FF_V:  *eew = 0; *is_load = 1; *mode = 4; return true;
    case LATR_RV_VLE16FF_V: *eew = 1; *is_load = 1; *mode = 4; return true;
    case LATR_RV_VLE32FF_V: *eew = 2; *is_load = 1; *mode = 4; return true;
    case LATR_RV_VLE64FF_V: *eew = 3; *is_load = 1; *mode = 4; return true;
    default: return false;
    }
}

static bool translate_vector_op(const RiscvIR1 *ir1)
{
    int op;
    int kind;
    int vm = ir1->arg.vm;
    unsigned eew = 0;
    int is_load = 0;
    unsigned mem_mode = 0;
    IR2_OPND base;
    IR2_OPND extra;
    IR2_OPND direct_fallback = { 0 };
    IR2_OPND direct_done = { 0 };
    bool emitted_direct = false;
    int imm_signed = 1;
    bool is_alu = rvv_alu_op(ir1->opcode, &op, &kind, &imm_signed);
    bool is_cmp = !is_alu && rvv_cmp_op(ir1->opcode, &op, &kind);
    bool is_merge = ir1->opcode == LATR_RV_VMERGE_VVM ||
                    ir1->opcode == LATR_RV_VMERGE_VXM ||
                    ir1->opcode == LATR_RV_VMERGE_VIM;
    bool is_mv = ir1->opcode == LATR_RV_VMV_V_V ||
                 ir1->opcode == LATR_RV_VMV_V_X ||
                 ir1->opcode == LATR_RV_VMV_V_I;
    unsigned mask_op = 0;
    bool is_mask_logic = rvv_mask_logic_op(ir1->opcode, &mask_op);
    bool is_mask_scan = !is_mask_logic && rvv_mask_scan_op(ir1->opcode, &mask_op);
    unsigned reduce_op = 0;
    unsigned perm_op = 0;
    unsigned perm_kind = 0;
    unsigned perm_index_bits = 0;
    bool is_reduce = rvv_reduce_op(ir1->opcode, &reduce_op);
    bool is_perm = rvv_perm_op(ir1->opcode, &perm_op, &perm_kind,
                               &perm_index_bits);
    unsigned extra_op = 0;
    unsigned extra_kind = 0;
    int extra_wide = 0;
    int extra_signed = 1;
    bool is_extra = rvv_extra_op(ir1->opcode, &extra_op, &extra_kind,
                                 &extra_wide, &extra_signed);
    unsigned fma_variant = 0;
    unsigned fma_kind = LATR_RVV_SRC_VECTOR;
    bool is_fma = rvv_fma_op(ir1->opcode, &fma_variant, &fma_kind);
    unsigned fp_op = 0;
    unsigned fp_kind = 0;
    unsigned cvt_op = 0;
    bool is_fp = rvv_fp_op(ir1->opcode, &fp_op, &fp_kind);
    bool is_cvt = !is_fp && rvv_cvt_op(ir1->opcode, &cvt_op);
    bool is_mem = rvv_mem_op(ir1->opcode, &eew, &is_load, &mem_mode);
    LatrRvvWholeKind whole = latr_rvv_whole_kind((unsigned)ir1->opcode);
    unsigned whole_nf = latr_rvv_whole_nf((unsigned)ir1->opcode);
    bool is_ff = is_mem && mem_mode == 4;
    IR2_OPND ff_exit = { 0 };
    IR2_OPND ff_done = { 0 };

    bool is_mv_scalar = ir1->opcode == LATR_RV_VMV_X_S ||
                        ir1->opcode == LATR_RV_VMV_S_X;

    if (!is_alu && !is_cmp && !is_merge && !is_mv && !is_mv_scalar &&
        !is_mask_logic && !is_mask_scan && !is_reduce && !is_perm &&
        !is_extra && !is_fma && !is_fp && !is_cvt && !is_mem &&
        whole == LATR_RVV_WHOLE_NONE) {
        return false;
    }
    emit_vs_gate(ir1);
    if (!emit_operand_legality(ir1)) {
        return true;
    }
    if (latr_rvv_uses_frm((unsigned)ir1->opcode)) {
        emit_frm_gate(ir1);
    }

    /* Segment (nf>1) forms share their decode tags with the unit-stride ones;
     * all three address modes handle nf in the helper now. vlm.v/vsm.v use
     * arg_r2, which has no nf field at all (mem_mode 3). */
    if (rvv_requires_vstart_zero(ir1->opcode)) {
        emit_vstart_zero_gate(ir1);
    }
    if (is_ff) {
        ff_exit = ir2_opnd_new_type(IR2_OPND_LABEL);
        ff_done = ir2_opnd_new_type(IR2_OPND_LABEL);
    }

    emitted_direct = latr_pvir_try_lower_lsx(ir1, &direct_fallback,
                                             &direct_done);
    if (emitted_direct) {
        la_label(direct_fallback);
    }

    if (is_mv_scalar) {
        emit_rvv_arg_const(0, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(1, (uint64_t)(ir1->opcode == LATR_RV_VMV_X_S
                                             ? ir1->arg.rs2
                                             : ir1->arg.rs1));
        emit_rvv_call((uintptr_t)(ir1->opcode == LATR_RV_VMV_X_S
                                      ? &latr_rvv_mv_x_s
                                      : &latr_rvv_mv_s_x));
    } else if (is_alu) {
        emit_rvv_arg_const(0, (uint64_t)op);
        emit_rvv_arg_const(1, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(2, (uint64_t)ir1->arg.rs2);
        emit_rvv_arg_const(3, (uint64_t)ir1->arg.rs1);
        emit_rvv_arg_const(4, (uint64_t)kind);
        emit_rvv_arg_const(5, kind == LATR_RVV_SRC_IMM
                                  ? (imm_signed
                                         ? (uint64_t)((int64_t)((uint64_t)
                                               ir1->arg.rs1 << 59) >> 59)
                                         : (uint64_t)ir1->arg.rs1)
                                  : (uint64_t)ir1->arg.rs1);
        emit_rvv_arg_const(6, (uint64_t)vm);
        emit_rvv_call((uintptr_t)&latr_rvv_alu);
    } else if (is_cmp) {
        emit_rvv_arg_const(0, (uint64_t)op);
        emit_rvv_arg_const(1, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(2, (uint64_t)ir1->arg.rs2);
        emit_rvv_arg_const(3, (uint64_t)ir1->arg.rs1);
        emit_rvv_arg_const(4, (uint64_t)kind);
        emit_rvv_arg_const(5, (uint64_t)((int64_t)((uint64_t)ir1->arg.rs1
                                                   << 59) >> 59));
        emit_rvv_arg_const(6, (uint64_t)vm);
        emit_rvv_call((uintptr_t)&latr_rvv_cmp);
    } else if (is_merge) {
        kind = ir1->opcode == LATR_RV_VMERGE_VVM
                   ? LATR_RVV_SRC_VECTOR
                   : ir1->opcode == LATR_RV_VMERGE_VXM
                         ? LATR_RVV_SRC_SCALAR
                         : LATR_RVV_SRC_IMM;
        emit_rvv_arg_const(0, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(1, (uint64_t)ir1->arg.rs1);
        emit_rvv_arg_const(2, (uint64_t)ir1->arg.rs2);
        emit_rvv_arg_const(3, (uint64_t)kind);
        emit_rvv_arg_const(4, kind == LATR_RVV_SRC_IMM
                                  ? (uint64_t)((int64_t)((uint64_t)
                                        ir1->arg.rs1 << 59) >> 59)
                                  : (uint64_t)ir1->arg.rs1);
        emit_rvv_call((uintptr_t)&latr_rvv_merge);
    } else if (is_mv) {
        kind = ir1->opcode == LATR_RV_VMV_V_V
                   ? LATR_RVV_SRC_VECTOR
                   : ir1->opcode == LATR_RV_VMV_V_X
                         ? LATR_RVV_SRC_SCALAR
                         : LATR_RVV_SRC_IMM;
        {
            uint64_t imm = ir1->opcode == LATR_RV_VMV_V_I
                               ? (uint64_t)((int64_t)((uint64_t)ir1->arg.rs1
                                                      << 59) >> 59)
                               : (uint64_t)ir1->arg.rs1;
            emit_rvv_arg_const(0, (uint64_t)ir1->arg.rd);
            emit_rvv_arg_const(1, (uint64_t)ir1->arg.rs1);
            emit_rvv_arg_const(2, (uint64_t)kind);
            emit_rvv_arg_const(3, imm);
            /* All vmv.v.* encodings are unmasked (the vm pattern bit is 1). */
            emit_rvv_arg_const(4, 1);
        }
        emit_rvv_call((uintptr_t)&latr_rvv_mv);
    } else if (is_mask_logic) {
        emit_rvv_arg_const(0, (uint64_t)mask_op);
        emit_rvv_arg_const(1, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(2, (uint64_t)ir1->arg.rs1);
        emit_rvv_arg_const(3, (uint64_t)ir1->arg.rs2);
        emit_rvv_call((uintptr_t)&latr_rvv_mask_logic);
    } else if (is_mask_scan) {
        emit_rvv_arg_const(0, (uint64_t)mask_op);
        emit_rvv_arg_const(1, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(2, (uint64_t)ir1->arg.rs2);
        emit_rvv_arg_const(3, (uint64_t)vm);
        emit_rvv_call((uintptr_t)&latr_rvv_mask_scan);
    } else if (is_cvt) {
        emit_rvv_arg_const(0, (uint64_t)cvt_op);
        emit_rvv_arg_const(1, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(2, (uint64_t)ir1->arg.rs2);
        emit_rvv_arg_const(3, (uint64_t)vm);
        emit_rvv_call((uintptr_t)&latr_rvv_fp_cvt);
    } else if (is_fp) {
        emit_rvv_arg_const(0, (uint64_t)fp_op);
        emit_rvv_arg_const(1, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(2, (uint64_t)ir1->arg.rs2);
        emit_rvv_arg_const(3, (uint64_t)ir1->arg.rs1);
        emit_rvv_arg_const(4, (uint64_t)fp_kind);
        /* vfmv.v.f has no vm bit; its encoding is always unmasked. */
        emit_rvv_arg_const(5, fp_op == LATR_RVV_FP_MV_V_F ? 1u
                                                         : (uint64_t)vm);
        emit_rvv_arg_const(6, (uint64_t)((ir1->opcode == LATR_RV_VFWADD_WV ||
                                          ir1->opcode == LATR_RV_VFWSUB_WV)
                                             ? 1 : 0));
        emit_rvv_call((uintptr_t)&latr_rvv_fp);
    } else if (is_fma) {
        emit_rvv_arg_const(0, (uint64_t)fma_variant);
        emit_rvv_arg_const(1, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(2, (uint64_t)ir1->arg.rs1);
        emit_rvv_arg_const(3, (uint64_t)ir1->arg.rs2);
        emit_rvv_arg_const(4, (uint64_t)fma_kind);
        emit_rvv_arg_const(5, 0);
        emit_rvv_arg_const(6, (uint64_t)vm);
        emit_rvv_call((uintptr_t)&latr_rvv_fma);
    } else if (is_extra) {
        uint64_t imm = (uint64_t)ir1->arg.rs1;
        if (extra_kind == LATR_RVV_SRC_IMM && extra_signed) {
            imm = (uint64_t)((int64_t)((uint64_t)ir1->arg.rs1 << 59) >> 59);
        }
        emit_rvv_arg_const(0, (uint64_t)extra_op);
        emit_rvv_arg_const(1, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(2, (uint64_t)ir1->arg.rs2);
        emit_rvv_arg_const(3, (uint64_t)ir1->arg.rs1);
        emit_rvv_arg_const(4, (uint64_t)extra_kind);
        emit_rvv_arg_const(5, extra_kind == LATR_RVV_SRC_SCALAR
                                  ? (uint64_t)ir1->arg.rs1
                                  : imm);
        emit_rvv_arg_const(6, (uint64_t)vm);
        emit_rvv_arg_const(7, (uint64_t)extra_wide);
        emit_rvv_call((uintptr_t)&latr_rvv_arith_extra);
    } else if (is_perm) {
        emit_rvv_arg_const(0, (uint64_t)perm_op);
        emit_rvv_arg_const(1, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(2, (uint64_t)ir1->arg.rs2);
        emit_rvv_arg_const(3, (uint64_t)perm_kind);
        if (perm_op == LATR_RVV_PERM_COMPRESS) {
            emit_rvv_arg_const(4, (uint64_t)ir1->arg.rs1);
        } else if (perm_kind == LATR_RVV_SRC_VECTOR) {
            emit_rvv_arg_const(4, (uint64_t)ir1->arg.rs1);
        } else if (perm_kind == LATR_RVV_SRC_SCALAR) {
            extra = alloc_gpr_src(ir1->arg.rs1);
            emit_rvv_arg_opnd(4, extra);
            free_alloc_gpr(extra);
        } else {
            emit_rvv_arg_const(4, (uint64_t)ir1->arg.rs1);
        }
        emit_rvv_arg_const(5, (uint64_t)perm_index_bits);
        emit_rvv_arg_const(6, (uint64_t)vm);
        emit_rvv_call((uintptr_t)&latr_rvv_permute);
    } else if (is_reduce) {
        emit_rvv_arg_const(0, (uint64_t)reduce_op);
        emit_rvv_arg_const(1, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(2, (uint64_t)ir1->arg.rs2);
        emit_rvv_arg_const(3, (uint64_t)ir1->arg.rs1);
        emit_rvv_arg_const(4, (uint64_t)vm);
        emit_rvv_call((uintptr_t)&latr_rvv_reduce);
    } else if (whole == LATR_RVV_WHOLE_MOVE) {
        emit_rvv_arg_const(0, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(1, (uint64_t)ir1->arg.rs2);
        emit_rvv_arg_const(2, (uint64_t)whole_nf);
        emit_rvv_call((uintptr_t)&latr_rvv_whole_move);
    } else if (whole != LATR_RVV_WHOLE_NONE) {
        base = alloc_gpr_src(ir1->arg.rs1);
        emit_rvv_arg_opnd(0, base);
        free_alloc_gpr(base);
        emit_rvv_arg_const(1, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(2, (uint64_t)latr_rvv_whole_eew((unsigned)
                                                               ir1->opcode));
        emit_rvv_arg_const(3, (uint64_t)(whole == LATR_RVV_WHOLE_LOAD));
        emit_rvv_arg_const(4, (uint64_t)whole_nf);
        emit_rvv_call((uintptr_t)&latr_rvv_whole_mem);
        emit_rvv_fault_check(ir1);
    } else if (is_mem) {

        base = alloc_gpr_src(ir1->arg.rs1);
        emit_rvv_arg_opnd(0, base);
        free_alloc_gpr(base);
        emit_rvv_arg_const(1, (uint64_t)ir1->arg.rd);
        emit_rvv_arg_const(2, (uint64_t)eew);
        emit_rvv_arg_const(3, (uint64_t)is_load);
        emit_rvv_arg_const(4, (uint64_t)vm);
        if (mem_mode == 0 || mem_mode == 3 || mem_mode == 4) {
            emit_rvv_arg_const(5, mem_mode == 3 ? 1u
                                             : (mem_mode == 4 ? 2u : 0u));
            emit_rvv_arg_const(6, mem_mode == 3
                                      ? 1u
                                      : (uint64_t)(ir1->arg.nf == 0
                                                       ? 1
                                                       : ir1->arg.nf));
            emit_rvv_call((uintptr_t)(mem_mode == 4 ? &latr_rvv_ldff
                                                    : &latr_rvv_mem));
        } else {
            if (mem_mode == 1) {
                extra = alloc_gpr_src(ir1->arg.rs2);
                emit_rvv_arg_opnd(5, extra);
                free_alloc_gpr(extra);
            } else {
                emit_rvv_arg_const(5, (uint64_t)ir1->arg.rs2);
            }
            emit_rvv_arg_const(6, (uint64_t)mem_mode);
            emit_rvv_arg_const(7, (uint64_t)(ir1->arg.nf == 0
                                                 ? 1
                                                 : ir1->arg.nf));
            emit_rvv_call((uintptr_t)&latr_rvv_stride_mem);
        }
        if (is_ff) {
            /* vl narrowed: the rest of this block was specialised for the old
             * vl, so retire the instruction and restart after it. */
            la_bnez(a0_ir2_opnd, ff_exit);
        } else {
            emit_rvv_fault_check(ir1);
        }
    }

    if (emitted_direct) {
        la_label(direct_done);
    }
    latr_emit_mark_vs_dirty();
    latr_emit_instret();
    if (is_ff) {
        IR2_OPND next_pc = ra_alloc_itemp();

        la_b(ff_done);
        la_label(ff_exit);
        latr_emit_mark_vs_dirty();
        latr_emit_instret();
        li_d(next_pc, (int64_t)(ir1->pc + ir1->raw_len));
        la_st_d(next_pc, env_ir2_opnd, env_offset_pc());
        ra_free_temp(next_pc);
        la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
        la_label(ff_done);
    }
    return true;
}

bool latr_translate_rvv(const RiscvIR1 *ir1)
{
    bool vsetvli = ir1->opcode == LATR_RV_VSETVLI;
    bool vsetivli = ir1->opcode == LATR_RV_VSETIVLI;
    bool vsetvl = ir1->opcode == LATR_RV_VSETVL;
    IR2_OPND s1;
    IR2_OPND vtype;
    bool s1_from_gpr;
    bool vtype_from_gpr;
    IR2_OPND target;

    if (!vsetvli && !vsetivli && !vsetvl) {
        return translate_vector_op(ir1);
    }
    emit_vs_gate(ir1);

    /* AVL: vsetivli uses the uimm field; rd=x0,rs1=x0 keeps the current vl;
     * rs1=x0 otherwise means "maximum" (QEMU passes RV_VLEN_MAX). */
    if (vsetivli) {
        s1 = ra_alloc_itemp();
        li_d(s1, ir1->arg.rs1);
        s1_from_gpr = false;
    } else if (ir1->arg.rd == 0 && ir1->arg.rs1 == 0) {
        s1 = ra_alloc_itemp();
        latr_env_load_d(s1, env_offset_vl());
        s1_from_gpr = false;
    } else if (ir1->arg.rs1 == 0) {
        s1 = ra_alloc_itemp();
        li_d(s1, -1);
        s1_from_gpr = false;
    } else {
        s1 = alloc_gpr_src(ir1->arg.rs1);
        s1_from_gpr = true;
    }

    if (vsetvl) {
        vtype = alloc_gpr_src(ir1->arg.rs2);
        vtype_from_gpr = true;
    } else {
        vtype = ra_alloc_itemp();
        li_d(vtype, ir1->arg.zimm);
        vtype_from_gpr = false;
    }

    target = ra_alloc_itemp();
    la_mov64(a1_ir2_opnd, s1);
    la_mov64(a2_ir2_opnd, vtype);
    la_mov64(a0_ir2_opnd, env_ir2_opnd);
    latr_env_store_d(ra_ir2_opnd, env_offset_host_return_address());
    latr_emit_code_symbol(target, (const void *)&latr_rvv_vsetvl);
    la_jirl(ra_ir2_opnd, target, 0);
    ra_free_temp(target);
    latr_env_load_d(ra_ir2_opnd, env_offset_host_return_address());

    if (ir1->arg.rd != 0) {
        IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);
        la_mov64(dst, a0_ir2_opnd);
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
    }
    if (vtype_from_gpr) {
        free_alloc_gpr(vtype);
    } else {
        ra_free_temp(vtype);
    }
    if (s1_from_gpr) {
        free_alloc_gpr(s1);
    } else {
        ra_free_temp(s1);
    }

    latr_emit_instret();

    /* vtype/vl changed: the next TB needs its own specialization. */
    {
        IR2_OPND next_pc = ra_alloc_itemp();
        li_d(next_pc, (int64_t)(ir1->pc + ir1->raw_len));
        la_st_d(next_pc, env_ir2_opnd, env_offset_pc());
        ra_free_temp(next_pc);
    }
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    return true;
}
