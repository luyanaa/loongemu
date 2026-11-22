#ifndef LATA_SVE_LASX_LOWER_H
#define LATA_SVE_LASX_LOWER_H

#include <stdbool.h>
#include <stddef.h>

#include "target/arm/lata/include/la-pvir.h"
#include "target/arm/lata/ir1/sve-decode.h"
#include "target/arm/lata/ir1/sve-pvir.h"

struct DisasContext;

/*
 * Result of executing one PVIR instruction in the scalar helper.
 *
 * Before this existed the helper returned 0 for both "done" and "I do not
 * implement this opcode", so pvir_instruction_validate() succeeding said
 * nothing about whether the fallback could actually run the instruction: a
 * valid PVIR shape that neither LSX lowering nor the helper covered became a
 * silent no-op, and the guest kept running with a stale destination register.
 *
 * The values double as the ABI with generated code, so keep them stable and
 * non-zero for everything that is not success.
 */
typedef enum LataSVEHelperStatus {
    LATA_SVE_HELPER_OK = 0,
    /* An architectural exception was raised; the generated code must take the
     * guest fault path with the PC the helper stored. */
    LATA_SVE_HELPER_FAULT = 1,
    /* The opcode or the shape was not implemented. Translation is supposed to
     * refuse these, so reaching it at run time is a bug: report loudly instead
     * of pretending the instruction executed. */
    LATA_SVE_HELPER_UNSUPPORTED = 2,
    /* Internal inconsistency (bad register id, missing description data). */
    LATA_SVE_HELPER_INTERNAL = 3,
} LataSVEHelperStatus;

/* Why the helper cannot execute an instruction; the reason only exists to make
 * a translation-time refusal self-explanatory in logs and tests. */
typedef enum LataSVEHelperSupport {
    LATA_SVE_HELPER_SUPPORTED = 0,
    LATA_SVE_HELPER_UNSUPPORTED_OPCODE,
    LATA_SVE_HELPER_UNSUPPORTED_SHAPE,
    LATA_SVE_HELPER_INVALID_PVIR,
} LataSVEHelperSupport;

typedef struct LataSVEHelperDesc {
    PvirInstruction pvir;
    bool has_source;
    A64SVEIR1Instruction source;
    uint16_t vl_bytes;
    uint8_t tbid;
    uint64_t pc_curr;
    uint64_t pc_next;
    PvirValueId z_values[A64_SVE_PVIR_NUM_ZREGS];
    PvirValueId p_values[A64_SVE_PVIR_NUM_PREGS];
    PvirValueId ffr_value;
} LataSVEHelperDesc;

const LataSVEHelperDesc *lata_sve_alloc_helper_desc(
    const struct DisasContext *s, const PvirInstruction *instruction,
    const A64SVEIR1Instruction *source);

/*
 * Translate-time query: can the scalar helper execute this instruction?
 *
 * A caller that is about to fall back to the helper must check this first and
 * refuse to translate when it says no, because the alternative is generating
 * code that runs and does nothing.
 */
LataSVEHelperSupport lata_sve_helper_support(
    const PvirInstruction *instruction, const A64SVEIR1Instruction *source);

/* Status-returning entry point, used by generated code and by tests. */
LataSVEHelperStatus lata_sve_exec_helper_status(void *env_ptr,
                                                const LataSVEHelperDesc *desc);

/* Compatibility wrapper: 0 on success, non-zero (LataSVEHelperStatus) else. */
uint64_t lata_sve_exec_helper(void *env_ptr, const LataSVEHelperDesc *desc);

/* Called from generated code when the helper reported UNSUPPORTED/INTERNAL at
 * run time, which the translate-time query is supposed to make impossible.
 * Raises an undefined instruction so the instruction cannot look executed. */
void lata_sve_helper_unexpected(void *env_ptr, const LataSVEHelperDesc *desc,
                                uint64_t status);

void lata_sve_helper_list_free(void *head);

/* Lower one validated PVIR instruction (source optional for direct PVIR ops). */
bool a64_sve_lower_pvir_instruction(struct DisasContext *s,
                                    const PvirInstruction *instruction,
                                    const A64SVEIR1Instruction *source);

/* P2 trace vector optimization controls and statistics. */
void a64_sve_trace_opt_reset_stats(void);
size_t a64_sve_trace_opt_const_pred_folds(void);
void a64_sve_trace_opt_set_const_pred_enabled(bool enabled);
bool a64_sve_trace_opt_const_pred_enabled(void);

bool trans_sve_ptrue(struct DisasContext *s);
bool trans_sve_pfalse(struct DisasContext *s);
bool trans_sve_add(struct DisasContext *s);
bool trans_sve_orr(struct DisasContext *s);
bool trans_sve_and(struct DisasContext *s);
bool trans_sve_eor(struct DisasContext *s);
bool trans_sve_lsl(struct DisasContext *s);
bool trans_sve_lsr(struct DisasContext *s);
bool trans_sve_asr(struct DisasContext *s);
bool trans_sve_smax(struct DisasContext *s);
bool trans_sve_umax(struct DisasContext *s);
bool trans_sve_smin(struct DisasContext *s);
bool trans_sve_umin(struct DisasContext *s);
bool trans_sve_sub(struct DisasContext *s);
bool trans_sve_mul(struct DisasContext *s);
bool trans_sve_fadd(struct DisasContext *s);
bool trans_sve_fsub(struct DisasContext *s);
bool trans_sve_fmul(struct DisasContext *s);
bool trans_sve_fdiv(struct DisasContext *s);
bool trans_sve_fabs(struct DisasContext *s);
bool trans_sve_fneg(struct DisasContext *s);
bool trans_sve_not(struct DisasContext *s);
bool trans_sve_dup(struct DisasContext *s);
bool trans_sve_whilelo(struct DisasContext *s);

bool trans_sve_ld1b(struct DisasContext *s);
bool trans_sve_st1b(struct DisasContext *s);
bool trans_sve_ld1h(struct DisasContext *s);
bool trans_sve_ld1w(struct DisasContext *s);
bool trans_sve_ld1d(struct DisasContext *s);
bool trans_sve_st1h(struct DisasContext *s);
bool trans_sve_st1w(struct DisasContext *s);
bool trans_sve_st1d(struct DisasContext *s);
bool trans_sve_ld1sb(struct DisasContext *s);
bool trans_sve_ld1sh(struct DisasContext *s);
bool trans_sve_ld1sw(struct DisasContext *s);
bool trans_sve_setffr(struct DisasContext *s);
bool trans_sve_rdffr(struct DisasContext *s);
bool trans_sve_wrffr(struct DisasContext *s);
bool trans_sve_ldff1b(struct DisasContext *s);
bool trans_sve_ldff1h(struct DisasContext *s);
bool trans_sve_ldff1w(struct DisasContext *s);
bool trans_sve_ldff1d(struct DisasContext *s);
bool trans_sve_ldff1sb(struct DisasContext *s);
bool trans_sve_ldff1sh(struct DisasContext *s);
bool trans_sve_ldff1sw(struct DisasContext *s);
bool trans_sve_ldnf1b(struct DisasContext *s);
bool trans_sve_ldnf1h(struct DisasContext *s);
bool trans_sve_ldnf1w(struct DisasContext *s);
bool trans_sve_ldnf1d(struct DisasContext *s);
bool trans_sve_ldnf1sb(struct DisasContext *s);
bool trans_sve_ldnf1sh(struct DisasContext *s);
bool trans_sve_ldnf1sw(struct DisasContext *s);
bool trans_sve_compact(struct DisasContext *s);
bool trans_sve_ptest(struct DisasContext *s);
bool trans_sve_while(struct DisasContext *s);
bool trans_sve_cntp(struct DisasContext *s);
bool trans_sve_cnt_scalar(struct DisasContext *s);
bool trans_sve_brk(struct DisasContext *s);
bool trans_sve_pfirst(struct DisasContext *s);
bool trans_sve_pnext(struct DisasContext *s);
bool trans_sve_last(struct DisasContext *s);
bool trans_sve_pred_logical(struct DisasContext *s);
bool trans_sve_ld1_scalar(struct DisasContext *s);
bool trans_sve_st1_scalar(struct DisasContext *s);
bool trans_sve_ldn(struct DisasContext *s);
bool trans_sve_stn(struct DisasContext *s);
bool trans_sve_ld1r(struct DisasContext *s);
bool trans_sve_ld1rq(struct DisasContext *s);
bool trans_sve_gather_ld(struct DisasContext *s);
bool trans_sve_scatter_st(struct DisasContext *s);
bool trans_sve_ldnt1(struct DisasContext *s);
bool trans_sve_stnt1(struct DisasContext *s);
bool trans_sve_fmla(struct DisasContext *s);
bool trans_sve_fminmax(struct DisasContext *s);
bool trans_sve_fcmp(struct DisasContext *s);
bool trans_sve_fcvt(struct DisasContext *s);
bool trans_sve_fp_math(struct DisasContext *s);
bool trans_sve_freduce(struct DisasContext *s);
bool trans_sve_ireduce(struct DisasContext *s);
bool trans_sve_fixed_int(struct DisasContext *s);
bool trans_sve_shift_imm(struct DisasContext *s);
bool trans_sve_int_unary(struct DisasContext *s);
bool trans_sve_rev(struct DisasContext *s);
bool trans_sve_permute(struct DisasContext *s);
bool trans_sve_index(struct DisasContext *s);
bool trans_sve_wmul(struct DisasContext *s);
bool trans_sve_imm_bin(struct DisasContext *s);
bool trans_sve_icmp(struct DisasContext *s);
bool trans_sve_sel(struct DisasContext *s);
bool trans_sve_idiv(struct DisasContext *s);

#endif
