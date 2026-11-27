#ifndef LATR_TRANSLATE_H
#define LATR_TRANSLATE_H

#include <stdbool.h>
#include <stdint.h>
#include "ir1.h"
#include "ir2.h"

typedef struct LatrTranslateContext {
    const RiscvIR1 *ir1;
    IR2_OPND target_label;
    bool has_target_label;
} LatrTranslateContext;

bool latr_translate_ir1(LatrTranslateContext *ctx);
bool latr_translate_csr(const RiscvIR1 *ir1);
bool latr_translate_privileged(const RiscvIR1 *ir1);
void latr_emit_instret(void);
/* Materialises the block's pending retired-instruction count into minstret. */
void latr_emit_instret_flush(void);
void latr_emit_store(IR2_OPND address, IR2_OPND value, unsigned size);
/* Walk the guest page table; leaves the host address in a0 (0 on fault). */
void latr_emit_resolve(IR2_OPND address, unsigned access, unsigned size);
/* Branch to `fault` if the last resolve reported a page fault. */
void latr_emit_branch_on_fault(IR2_OPND fault);

/* MMIO dispatch used by emitted loads/stores (bus miss -> access fault). */
uint64_t latr_mmio_load(CPUArchState *env, uint64_t pa, unsigned size);
void latr_mmio_store(CPUArchState *env, uint64_t pa, unsigned size,
                     uint64_t value);
/* Environment accesses preserve correctness for fields outside signed 12-bit offsets. */
void latr_env_load_d(IR2_OPND dst, int offset);
void latr_env_store_d(IR2_OPND value, int offset);
/* Emit cause 2 with instruction bits in the privilege-appropriate trap-value CSR. */
void latr_raise_illegal_instruction(const RiscvIR1 *ir1);
/* Emit cause 4/6 with the faulting address in the trap-value CSR. */
void latr_raise_misaligned(const RiscvIR1 *ir1, IR2_OPND address, bool is_store);
/* Emit the page fault recorded by latr_resolve_guest_address(). */
void latr_raise_mem_fault(const RiscvIR1 *ir1);
uint64_t latr_read_mip(CPUArchState *env);
void latr_emit_mark_fs_dirty(void);
void latr_emit_fs_gate(const RiscvIR1 *ir1);
void latr_emit_mark_vs_dirty(void);
/* Emit cause 12 with the unfetchable guest VA as the trap value. */
void latr_raise_fetch_fault(const RiscvIR1 *ir1);
bool latr_translate_fp(const RiscvIR1 *ir1);
bool latr_translate_rvv(const RiscvIR1 *ir1);

/* RVV helper operand kinds/ops (env->rvv_args layout is documented in rvv.c). */
enum {
    LATR_RVV_SRC_VECTOR = 0,
    LATR_RVV_SRC_SCALAR = 1,
    LATR_RVV_SRC_IMM = 2,
    /* Scalar taken from an f register (the .vf forms); the arg is a regno. */
    LATR_RVV_SRC_FSCALAR = 3,
};
enum {
    LATR_RVV_OP_ADD = 0, LATR_RVV_OP_SUB, LATR_RVV_OP_AND,
    LATR_RVV_OP_OR, LATR_RVV_OP_XOR, LATR_RVV_OP_SLL,
    LATR_RVV_OP_SRL, LATR_RVV_OP_SRA,
    LATR_RVV_OP_MUL, LATR_RVV_OP_DIVU, LATR_RVV_OP_DIV,
    LATR_RVV_OP_REMU, LATR_RVV_OP_REM,
    LATR_RVV_OP_MINU, LATR_RVV_OP_MIN, LATR_RVV_OP_MAXU,
    LATR_RVV_OP_MAX,
};
enum {
    LATR_RVV_CMP_EQ = 0, LATR_RVV_CMP_NE, LATR_RVV_CMP_LT,
    LATR_RVV_CMP_LTU, LATR_RVV_CMP_LE, LATR_RVV_CMP_LEU,
    LATR_RVV_CMP_GT, LATR_RVV_CMP_GTU, LATR_RVV_CMP_GE,
    LATR_RVV_CMP_GEU,
};

void latr_rvv_alu(CPUArchState *env);
void latr_rvv_cmp(CPUArchState *env);
void latr_rvv_merge(CPUArchState *env);
void latr_rvv_mv(CPUArchState *env);
void latr_rvv_mv_x_s(CPUArchState *env);
void latr_rvv_mv_s_x(CPUArchState *env);
uint64_t latr_rvv_mem(CPUArchState *env);
uint64_t latr_rvv_stride_mem(CPUArchState *env);
/* Fault-only-first load: returns 1 when it narrowed vl and the TB must exit. */
uint64_t latr_rvv_ldff(CPUArchState *env);
uint64_t latr_rvv_whole_mem(CPUArchState *env);
void latr_rvv_whole_move(CPUArchState *env);
enum {
    LATR_RVV_MASK_AND = 0, LATR_RVV_MASK_NAND, LATR_RVV_MASK_ANDN,
    LATR_RVV_MASK_XOR, LATR_RVV_MASK_OR, LATR_RVV_MASK_NOR,
    LATR_RVV_MASK_ORN, LATR_RVV_MASK_XNOR,
};
enum {
    LATR_RVV_SCAN_VCPOP = 0, LATR_RVV_SCAN_VFIRST, LATR_RVV_SCAN_VMSBF,
    LATR_RVV_SCAN_VMSIF, LATR_RVV_SCAN_VMSOF, LATR_RVV_SCAN_VIOTA,
    LATR_RVV_SCAN_VID,
};
void latr_rvv_mask_logic(CPUArchState *env);
enum {
    LATR_RVV_RED_SUM = 0, LATR_RVV_RED_AND, LATR_RVV_RED_OR,
    LATR_RVV_RED_XOR, LATR_RVV_RED_MINU, LATR_RVV_RED_MIN,
    LATR_RVV_RED_MAXU, LATR_RVV_RED_MAX, LATR_RVV_RED_WSUM,
    LATR_RVV_RED_WSUMU, LATR_RVV_RED_FUSUM, LATR_RVV_RED_FOSUM,
    LATR_RVV_RED_FMIN, LATR_RVV_RED_FMAX, LATR_RVV_RED_FWUSUM,
    LATR_RVV_RED_FWOSUM,
};
enum {
    LATR_RVV_PERM_SLIDEUP = 0, LATR_RVV_PERM_SLIDEDOWN,
    LATR_RVV_PERM_SLIDE1UP, LATR_RVV_PERM_SLIDE1DOWN,
    LATR_RVV_PERM_GATHER, LATR_RVV_PERM_GATHER16,
    LATR_RVV_PERM_COMPRESS,
};
void latr_rvv_reduce(CPUArchState *env);
void latr_rvv_permute(CPUArchState *env);
enum {
    LATR_RVV_X_WADD = 0, LATR_RVV_X_WADDU, LATR_RVV_X_WSUB,
    LATR_RVV_X_WSUBU, LATR_RVV_X_WMUL, LATR_RVV_X_WMULU,
    LATR_RVV_X_WMULSU, LATR_RVV_X_WMACC, LATR_RVV_X_WMACCU,
    LATR_RVV_X_WMACCSU, LATR_RVV_X_WMACCUS,
    LATR_RVV_X_NSRL, LATR_RVV_X_NSRA, LATR_RVV_X_NCLIP,
    LATR_RVV_X_NCLIPU, LATR_RVV_X_SADD, LATR_RVV_X_SADDU,
    LATR_RVV_X_SSUB, LATR_RVV_X_SSUBU, LATR_RVV_X_AADD,
    LATR_RVV_X_AADDU, LATR_RVV_X_ASUB, LATR_RVV_X_ASUBU,
    LATR_RVV_X_SSRL, LATR_RVV_X_SSRA,
    LATR_RVV_X_ZEXT2, LATR_RVV_X_ZEXT4, LATR_RVV_X_ZEXT8,
    LATR_RVV_X_SEXT2, LATR_RVV_X_SEXT4, LATR_RVV_X_SEXT8,
    /* 2.2 batch: reverse subtract and the three high-half multiplies. */
    LATR_RVV_X_RSUB, LATR_RVV_X_MULH, LATR_RVV_X_MULHU,
    LATR_RVV_X_MULHSU, LATR_RVV_X_SMUL,
    /*
     * Integer multiply-accumulate. vd takes one of two roles: for MACC/NMSAC it
     * is the accumulator (vd +/- a*b), for MADD/NMSUB it is a multiplicand
     * (a*vd +/- b), so the operands cannot be shared with the wide MAC path.
     */
    LATR_RVV_X_MACC, LATR_RVV_X_NMSAC, LATR_RVV_X_MADD, LATR_RVV_X_NMSUB,
    /*
     * Carry/borrow. v0 is a carry-in, never a mask: vadc/vsbc write elements
     * back for every body lane, vmadc/vmsbc write a mask register and are
     * unmaskable. The vm bit selects whether the carry/borrow-in is added for
     * the vmadc/vmsbc forms (vm=0 means with carry-in).
     */
    LATR_RVV_X_VADC, LATR_RVV_X_VSBC, LATR_RVV_X_VMADC, LATR_RVV_X_VMSBC,
};
void latr_rvv_arith_extra(CPUArchState *env);
void latr_rvv_carry(CPUArchState *env);

/*
 * RVV fused multiply-add. Variant index encodes the sign rules: bit0 negates
 * the product and the addend negates as `product_bit ^ addend_bit`; variants
 * 0..3 accumulate into vd (macc/msac), 4..7 take vd as the addend (madd/msub).
 */
enum {
    LATR_RVV_FMA_MACC = 0, LATR_RVV_FMA_NMACC,
    LATR_RVV_FMA_MSAC, LATR_RVV_FMA_NMSAC,
    LATR_RVV_FMA_MADD, LATR_RVV_FMA_NMADD,
    LATR_RVV_FMA_MSUB, LATR_RVV_FMA_NMSUB,
    /*
     * Widening forms: the product is SEW x SEW, the accumulator 2*SEW, and only
     * the four acc/acc-negate variants exist (there is no vfwmadd). Values 8..11
     * carry the same sign rules as 0..3.
     */
    LATR_RVV_FWMA_MACC = 8, LATR_RVV_FWMA_NMACC,
    LATR_RVV_FWMA_MSAC, LATR_RVV_FWMA_NMSAC,
};
void latr_rvv_fma(CPUArchState *env);
enum {
    LATR_RVV_FP_ADD = 0, LATR_RVV_FP_SUB, LATR_RVV_FP_MUL,
    LATR_RVV_FP_DIV, LATR_RVV_FP_MIN, LATR_RVV_FP_MAX,
    LATR_RVV_FP_SQRT, LATR_RVV_FP_SGNJ, LATR_RVV_FP_SGNJN,
    LATR_RVV_FP_SGNJX, LATR_RVV_FP_MERGE, LATR_RVV_FP_MV_V_F,
    LATR_RVV_FP_MV_F_S, LATR_RVV_FP_MV_S_F,
    LATR_RVV_FP_CMP_EQ, LATR_RVV_FP_CMP_NE, LATR_RVV_FP_CMP_LT,
    LATR_RVV_FP_CMP_LE, LATR_RVV_FP_CMP_GT, LATR_RVV_FP_CMP_GE,
    /* Reverse-order scalar ops and the 7-bit estimates (see rvv.c). */
    LATR_RVV_FP_RSUB, LATR_RVV_FP_RDIV, LATR_RVV_FP_REC7, LATR_RVV_FP_RSQRT7,
    LATR_RVV_FP_CLASS, LATR_RVV_FP_FWADD, LATR_RVV_FP_FWSUB,
    LATR_RVV_FP_FWMUL,
};
enum {
    LATR_RVV_CVT_XU_F = 0, LATR_RVV_CVT_X_F, LATR_RVV_CVT_F_XU,
    LATR_RVV_CVT_F_X, LATR_RVV_CVT_RTZ_XU_F, LATR_RVV_CVT_RTZ_X_F,
    LATR_RVV_CVT_FW_F_F, LATR_RVV_CVT_FW_F_X, LATR_RVV_CVT_FW_F_XU,
    LATR_RVV_CVT_FW_X_F, LATR_RVV_CVT_FW_XU_F,
    LATR_RVV_CVT_FN_F_F, LATR_RVV_CVT_FN_ROD_F_F,
    LATR_RVV_CVT_FN_X_F, LATR_RVV_CVT_FN_XU_F,
    LATR_RVV_CVT_FN_F_X, LATR_RVV_CVT_FN_F_XU,
};
void latr_rvv_fp(CPUArchState *env);
/* PVIR value-id conventions for LATR vector registers. */
#define LATR_PVIR_SCALAR_VALUE 32u
#define LATR_PVIR_IMM_VALUE    33u
#define LATR_PVIR_V0_VALUE     34u
#include "la-pvir.h"
void latr_pvir_execute(CPUArchState *env,
                       const PvirInstruction *instruction);
void latr_pvir_execute_cached(CPUArchState *env, uint64_t tag,
                              const PvirInstruction *instruction);
bool latr_pvir_cache_lookup(uint64_t key_hash, uint64_t vtype, uint64_t vl,
                            PvirInstruction *out);
void latr_pvir_cache_store(uint64_t key_hash, uint64_t vtype, uint64_t vl,
                           const PvirInstruction *insn);
void latr_env_vld(IR2_OPND vector, int offset);
void latr_env_vst(IR2_OPND vector, int offset);
void latr_env_xvld(IR2_OPND vector, int offset);
void latr_env_xvst(IR2_OPND vector, int offset);
int latr_rvv_direct_prepare(CPUArchState *env, uint64_t packed_desc);
bool latr_pvir_try_lower_lsx(const RiscvIR1 *ir1, IR2_OPND *fallback_label,
                             IR2_OPND *done_label);
void latr_rvv_fp_cvt(CPUArchState *env);
/* Softfloat status helpers shared by the vector FP path. */
struct float_status;
void latr_fp_status_begin(CPUArchState *env, struct float_status *status,
                          int rtz);
void latr_fp_status_end(CPUArchState *env, struct float_status *status);
void latr_rvv_mask_scan(CPUArchState *env);
uint64_t latr_rvv_read_elem(const CPUArchState *env, unsigned reg,
                            unsigned lane, unsigned sew);
void latr_rvv_write_elem(CPUArchState *env, unsigned reg, unsigned lane,
                         unsigned sew, uint64_t value);

#endif
