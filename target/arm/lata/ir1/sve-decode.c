#include "sve-decode.h"

typedef void (*SveExtract)(uint32_t insn, union a64_u *arg);

typedef struct {
    uint32_t mask;
    uint32_t value;
    AARCH64_INSN_TYPE_A64 insn_type;
    SveExtract extract;
    bool (*valid)(uint32_t insn);
} SveDecodePattern;

static unsigned field(uint32_t insn, unsigned lsb, unsigned width)
{
    return (insn >> lsb) & ((1u << width) - 1u);
}

static int signed_imm4(uint32_t insn)
{
    unsigned imm = field(insn, 16, 4);
    return (int)imm - ((imm & 8u) ? 16 : 0);
}

static int64_t sign_extend_n(uint64_t val, unsigned bits)
{
    uint64_t sign = UINT64_C(1) << (bits - 1u);
    val &= (bits == 64) ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - 1u);
    return (int64_t)((val ^ sign) - sign);
}

/* Decode ARMv8 bitmask immediate N:immr:imms (bits[17:5]) for SVE logical imm. */
static uint64_t decode_sve_bitmask_imm(uint32_t insn, uint8_t *elem_bits_out)
{
    unsigned imm13 = field(insn, 5, 13);
    unsigned n = (imm13 >> 12) & 1u;
    unsigned immr = (imm13 >> 6) & 0x3fu;
    unsigned imms = imm13 & 0x3fu;
    int len = 0;

    if (n != 0) {
        len = 6;
    } else {
        unsigned inv_imms = (~imms) & 0x3fu;
        for (int b = 5; b >= 1; --b) {
            if ((inv_imms >> b) & 1u) {
                len = b;
                break;
            }
        }
    }
    if (len < 1) {
        *elem_bits_out = 64;
        return 0;
    }
    unsigned esize = 1u << len;
    unsigned levels = esize - 1u;
    unsigned s = imms & levels;
    unsigned r = immr & levels;
    uint64_t welem = (s + 1u == 64u) ? ~UINT64_C(0)
                                     : ((UINT64_C(1) << (s + 1u)) - 1u);
    if (r != 0) {
        uint64_t emask = (esize == 64u) ? ~UINT64_C(0)
                                        : ((UINT64_C(1) << esize) - 1u);
        welem = ((welem >> r) | (welem << (esize - r))) & emask;
    }
    uint64_t result = welem;
    for (unsigned shift = esize; shift < 64u; shift <<= 1) {
        result |= (result << shift);
    }
    *elem_bits_out = (uint8_t)(esize >= 8u ? esize : 8u);
    return result;
}

static void decode_tsize_shift_unpred(uint32_t insn, bool is_left,
                                      uint8_t *elem_bits, uint8_t *shift)
{
    unsigned tsz = (field(insn, 22, 2) << 2) | field(insn, 19, 2);
    unsigned val = (tsz << 3) | field(insn, 16, 3);
    uint8_t ebits = 8;

    if (val >= 64u) {
        ebits = 64;
    } else if (val >= 32u) {
        ebits = 32;
    } else if (val >= 16u) {
        ebits = 16;
    }
    *elem_bits = ebits;
    *shift = is_left ? (uint8_t)(val - ebits) : (uint8_t)(ebits * 2u - val);
}

static void decode_tsize_shift_pred(uint32_t insn, bool is_left,
                                    uint8_t *elem_bits, uint8_t *shift)
{
    unsigned val = (field(insn, 22, 2) << 5) | field(insn, 5, 5);
    uint8_t ebits = 8;

    if (val >= 64u) {
        ebits = 64;
    } else if (val >= 32u) {
        ebits = 32;
    } else if (val >= 16u) {
        ebits = 16;
    }
    *elem_bits = ebits;
    *shift = is_left ? (uint8_t)(val - ebits) : (uint8_t)(ebits * 2u - val);
}

static void init_effects(arg_sve_effect_intent *effects,
                         PvirInactivePolicy inactive_policy)
{
    *effects = (arg_sve_effect_intent){0};
    effects->inactive_policy = inactive_policy;
    effects->memory.access = PVIR_MEMORY_NONE;
    effects->memory.ordering = PVIR_MEMORY_ORDER_UNSPECIFIED;
    effects->memory.inactive_lanes = PVIR_INACTIVE_MEMORY_NOT_APPLICABLE;
    effects->fault_behavior = PVIR_FAULT_NONE;
    effects->fault_order = PVIR_FAULT_ORDER_UNSPECIFIED;
    effects->fault_result = PVIR_FAULT_RESULT_UNSPECIFIED;
    effects->state_domain = PVIR_ARCH_STATE_DOMAIN_NONE;
    effects->state_action = PVIR_ARCH_STATE_ACTION_NONE;
    effects->state_update = PVIR_ARCH_STATE_UPDATE_NONE;
}

static void init_predicated_memory(arg_sve_effect_intent *effects,
                                   PvirMemoryAccess access,
                                   PvirInactivePolicy inactive_policy)
{
    init_effects(effects, inactive_policy);
    effects->memory.access = access;
    effects->memory.ordering = PVIR_MEMORY_ORDER_RELAXED;
    effects->memory.inactive_lanes = PVIR_INACTIVE_MEMORY_NO_TRANSACTION;
    effects->fault_behavior = PVIR_FAULT_PRECISE;
    /* C8.2.298/704 visit active lanes in ascending order. */
    effects->fault_order = PVIR_FAULT_ORDER_LANE_ASCENDING;
}

static void decode_ptrue(uint32_t insn, union a64_u *arg)
{
    arg_sve_ptrue *out = &arg->f_sve_ptrue;
    out->pd = field(insn, 0, 4);
    out->pattern = field(insn, 5, 5);
    out->element_bits = 8u << field(insn, 22, 2);
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_ptrues(uint32_t insn, union a64_u *arg)
{
    decode_ptrue(insn, arg);
    arg->f_sve_ptrue.setflags = true;
}

static void decode_pfalse(uint32_t insn, union a64_u *arg)
{
    arg_sve_ptrue *out = &arg->f_sve_ptrue;

    out->pd = field(insn, 0, 5);
    out->element_bits = 8; /* PFALSE is always written as <Pd>.B */
    out->pattern = 0;
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_whilelo(uint32_t insn, union a64_u *arg)
{
    arg_sve_whilelo *out = &arg->f_sve_whilelo;
    out->pd = field(insn, 0, 4);
    out->rn = field(insn, 5, 5);
    out->rm = field(insn, 16, 5);
    out->element_bits = 8u << field(insn, 22, 2);
    out->scalar_bits = field(insn, 12, 1) ? 64 : 32;
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    out->effects.writes_nzcv = true;
}

static void decode_integer_binary(uint32_t insn,
                                  arg_sve_integer_binary *out)
{
    out->zd = field(insn, 0, 5);
    out->zm = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->element_bits = 8u << field(insn, 22, 2);
    init_effects(&out->effects, PVIR_INACTIVE_MERGE);
}

static void decode_smax(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_smax);
    arg->f_sve_smax.is_signed = true;
}
static void decode_umax(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_umax);
    arg->f_sve_umax.is_signed = false;
}
static void decode_smin(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_smin);
    arg->f_sve_smin.is_signed = true;
}
static void decode_umin(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_umin);
    arg->f_sve_umin.is_signed = false;
}
static void decode_add(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_add);
}

static void decode_sub(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_sub);
}
static void decode_mul(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_mul);
}

static void decode_orr(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_orr);
}

static void decode_and(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_and);
}

static void decode_eor(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_eor);
}

static void decode_lsl(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_lsl);
}

static void decode_lsr(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_lsr);
}

static void decode_asr(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_asr);
}



static void decode_fp_binary(uint32_t insn, arg_sve_fp_binary *out)
{
    decode_integer_binary(insn, out);
}

static void decode_fadd(uint32_t insn, union a64_u *arg)
{
    decode_fp_binary(insn, &arg->f_sve_fadd);
}

static void decode_fsub(uint32_t insn, union a64_u *arg)
{
    decode_fp_binary(insn, &arg->f_sve_fsub);
}

static void decode_fmul(uint32_t insn, union a64_u *arg)
{
    decode_fp_binary(insn, &arg->f_sve_fmul);
}

static void decode_fdiv(uint32_t insn, union a64_u *arg)
{
    decode_fp_binary(insn, &arg->f_sve_fdiv);
}

static void decode_fabs(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_fabs);
}

static void decode_fneg(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_fneg);
}

static void decode_contiguous(uint32_t insn, union a64_u *arg,
                              uint8_t memory_bits, bool is_load)
{
    arg_sve_contiguous *out = &arg->f_sve_contiguous;

    out->zt = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->immediate_vl = signed_imm4(insn);
    out->element_bits = 8u << field(insn, 21, 2);
    out->memory_bits = memory_bits;
    out->base_is_sp = out->rn == 31;
    init_predicated_memory(&out->effects,
                           is_load ? PVIR_MEMORY_READ : PVIR_MEMORY_WRITE,
                           is_load ? PVIR_INACTIVE_ZERO
                                   : PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_not(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_not);
}

static void decode_dup(uint32_t insn, union a64_u *arg)
{
    arg_sve_dup *out = &arg->f_sve_dup;

    out->zd = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->element_bits = 8u << field(insn, 22, 2);
    init_effects(&out->effects, PVIR_INACTIVE_MERGE);
}

static void decode_ld1b(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 8, true);
}

static void decode_ld1h(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 16, true);
}

static void decode_ld1w(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 32, true);
}

static void decode_ld1d(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 64, true);
}

/*
 * Sign-extending loads keep their element/memory sizes in the mnemonic rather
 * than in one shared field, so each form is its own entry with the widths
 * stated explicitly (words from llvm-mc 23.1.6).
 */
static void decode_ld1sb_h(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 8, true);
    arg->f_sve_contiguous.element_bits = 16;
    arg->f_sve_contiguous.sign_extend = true;
}

static void decode_ld1sb_s(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 8, true);
    arg->f_sve_contiguous.element_bits = 32;
    arg->f_sve_contiguous.sign_extend = true;
}

static void decode_ld1sb_d(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 8, true);
    arg->f_sve_contiguous.element_bits = 64;
    arg->f_sve_contiguous.sign_extend = true;
}

static void decode_ld1sh_s(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 16, true);
    arg->f_sve_contiguous.element_bits = 32;
    arg->f_sve_contiguous.sign_extend = true;
}

static void decode_ld1sh_d(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 16, true);
    arg->f_sve_contiguous.element_bits = 64;
    arg->f_sve_contiguous.sign_extend = true;
}

static void decode_ld1sw_d(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 32, true);
    arg->f_sve_contiguous.element_bits = 64;
    arg->f_sve_contiguous.sign_extend = true;
}

/* Zero-extending widening loads (mirror of LD1SB/LD1SH/LD1SW). */
static void decode_ld1b_h(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 8, true);
    arg->f_sve_contiguous.element_bits = 16;
}

static void decode_ld1b_s(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 8, true);
    arg->f_sve_contiguous.element_bits = 32;
}

static void decode_ld1b_d(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 8, true);
    arg->f_sve_contiguous.element_bits = 64;
}

static void decode_ld1h_s(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 16, true);
    arg->f_sve_contiguous.element_bits = 32;
}

static void decode_ld1h_d(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 16, true);
    arg->f_sve_contiguous.element_bits = 64;
}

static void decode_ld1w_d(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 32, true);
    arg->f_sve_contiguous.element_bits = 64;
}

static void decode_st1h(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 16, false);
}

static void decode_st1w(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 32, false);
}

static void decode_st1d(uint32_t insn, union a64_u *arg)
{
    decode_contiguous(insn, arg, 64, false);
}

static void decode_st1b(uint32_t insn, union a64_u *arg)
{
    arg_sve_contiguous *out = &arg->f_sve_contiguous;
    out->zt = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->immediate_vl = signed_imm4(insn);
    out->element_bits = 8u << field(insn, 21, 2);
    out->memory_bits = 8;
    out->base_is_sp = out->rn == 31;
    init_predicated_memory(&out->effects, PVIR_MEMORY_WRITE,
                           PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_setffr(uint32_t insn, union a64_u *arg)
{
    arg_sve_ffr *out = &arg->f_sve_ffr;
    (void)insn;
    out->has_pd = false;
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    out->effects.state_domain = PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT;
    out->effects.state_action = PVIR_ARCH_STATE_WRITE;
    out->effects.ffr_initializes_all_true = true;
}

static void decode_rdffr(uint32_t insn, union a64_u *arg)
{
    arg_sve_ffr *out = &arg->f_sve_ffr;
    out->pd = field(insn, 0, 4);
    out->has_pd = true;
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    out->effects.state_domain = PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT;
    out->effects.state_action = PVIR_ARCH_STATE_READ;
}

static void init_faulting_load(arg_sve_effect_intent *effects,
                               PvirFaultBehavior behavior)
{
    init_predicated_memory(effects, PVIR_MEMORY_READ, PVIR_INACTIVE_ZERO);
    effects->fault_behavior = behavior;
    effects->fault_order = behavior == PVIR_FAULT_FIRST_FAULT
                               ? PVIR_FAULT_ORDER_LANE_ASCENDING
                               : PVIR_FAULT_ORDER_UNSPECIFIED;
    effects->fault_result = PVIR_FAULT_RESULT_UNKNOWN;
    effects->state_domain = PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT;
    effects->state_action = PVIR_ARCH_STATE_UPDATE;
    effects->state_update = PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS;
    effects->result_unknown_from_fault_boundary = true;
    effects->result_unknown_where_ffr_input_clear = true;
    effects->ffr_clears_from_fault_boundary = true;
}

static uint8_t dtype_mem_bits_full(unsigned dtype, bool *is_signed)
{
    /* Exact msz/u/s mapping from dtype bits[24:21]:
     * 0..3: B->B/H/S/D, 4: SW->D, 5..7: H->H/S/D,
     * 8..9: SH->D/S, 10..11: W->S/D, 12..14: SB->D/S/H, 15: D->D.
     */
    static const uint8_t m[16] = { 8, 8, 8, 8, 32, 16, 16, 16, 16, 16, 32, 32, 8, 8, 8, 64 };
    static const bool sg[16]   = { 0, 0, 0, 0,  1,  0,  0,  0,  1,  1,  0,  0, 1, 1, 1,  0 };
    *is_signed = sg[dtype & 0xfu];
    return m[dtype & 0xfu];
}

static uint8_t dtype_elem_bits_full(unsigned dtype)
{
    static const uint8_t e[16] = { 8, 16, 32, 64, 64, 16, 32, 64, 64, 32, 32, 64, 64, 32, 16, 64 };
    return e[dtype & 0xfu];
}

static void decode_ldff1_common(uint32_t insn, union a64_u *arg)
{
    unsigned dtype = field(insn, 21, 4);
    bool sg = false;
    arg_sve_fault_load *out = &arg->f_sve_fault_load;
    out->zt = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->rm = field(insn, 16, 5);
    out->element_bits = dtype_elem_bits_full(dtype);
    out->memory_bits = dtype_mem_bits_full(dtype, &sg);
    out->sign_extend = sg;
    out->base_is_sp = out->rn == 31;
    out->has_index = out->rm != 31;
    init_faulting_load(&out->effects, PVIR_FAULT_FIRST_FAULT);
}

static void decode_ldnf1_common(uint32_t insn, union a64_u *arg)
{
    unsigned dtype = field(insn, 21, 4);
    bool sg = false;
    arg_sve_nonfault_load *out = &arg->f_sve_nonfault_load;
    out->zt = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->immediate_vl = signed_imm4(insn);
    out->element_bits = dtype_elem_bits_full(dtype);
    out->memory_bits = dtype_mem_bits_full(dtype, &sg);
    out->sign_extend = sg;
    out->base_is_sp = out->rn == 31;
    init_faulting_load(&out->effects, PVIR_FAULT_NONFAULTING);
}

static void decode_wrffr(uint32_t insn, union a64_u *arg)
{
    arg_sve_ffr *out = &arg->f_sve_ffr;
    out->pn = field(insn, 5, 4);
    out->is_write = true;
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    out->effects.state_domain = PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT;
    out->effects.state_action = PVIR_ARCH_STATE_WRITE;
}

static void decode_rdffr_pred(uint32_t insn, union a64_u *arg)
{
    arg_sve_ffr *out = &arg->f_sve_ffr;
    out->pd = field(insn, 0, 4);
    out->pg = field(insn, 5, 4);
    out->has_pd = true;
    out->predicated = true;
    out->setflags = field(insn, 22, 1) != 0;
    init_effects(&out->effects, PVIR_INACTIVE_ZERO);
    out->effects.state_domain = PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT;
    out->effects.state_action = PVIR_ARCH_STATE_READ;
    out->effects.writes_nzcv = out->setflags;
}

static void decode_compact(uint32_t insn, union a64_u *arg)
{
    arg_sve_general *out = &arg->f_sve_general;
    out->zd = field(insn, 0, 5);
    out->zn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->element_bits = 8u << field(insn, 22, 2);
    out->subop = PVIR_PERMUTE_COMPACT;
    out->predicated = true;
    init_effects(&out->effects, PVIR_INACTIVE_ZERO);
}

static void decode_ptest(uint32_t insn, union a64_u *arg)
{
    arg_sve_pred_op *out = &arg->f_sve_pred_op;
    out->pg = field(insn, 10, 4);
    out->pn = field(insn, 5, 4);
    out->element_bits = 8;
    out->subop = SVE_PRED_OP_PTEST;
    out->setflags = true;
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    out->effects.writes_nzcv = true;
}

static void decode_while_cond(uint32_t insn, union a64_u *arg,
                              PvirCompareCondition cond,
                              bool is_conflict, bool conflict_wr)
{
    arg_sve_whilelo *out = &arg->f_sve_whilelo;
    out->pd = field(insn, 0, 4);
    out->rn = field(insn, 5, 5);
    out->rm = field(insn, 16, 5);
    out->element_bits = 8u << field(insn, 22, 2);
    out->scalar_bits = field(insn, 12, 1) ? 64 : 32;
    out->cond = cond;
    out->is_conflict = is_conflict;
    out->conflict_wr = conflict_wr;
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    out->effects.writes_nzcv = true;
}

static void decode_whilelt(uint32_t i, union a64_u *a) { decode_while_cond(i, a, PVIR_COMPARE_LT, false, false); }
static void decode_whilele(uint32_t i, union a64_u *a) { decode_while_cond(i, a, PVIR_COMPARE_LE, false, false); }
static void decode_whilels(uint32_t i, union a64_u *a) { decode_while_cond(i, a, PVIR_COMPARE_LEU, false, false); }
static void decode_whilege(uint32_t i, union a64_u *a) { decode_while_cond(i, a, PVIR_COMPARE_GE, false, false); }
static void decode_whilegt(uint32_t i, union a64_u *a) { decode_while_cond(i, a, PVIR_COMPARE_GT, false, false); }
static void decode_whilehs(uint32_t i, union a64_u *a) { decode_while_cond(i, a, PVIR_COMPARE_GEU, false, false); }
static void decode_whilehi(uint32_t i, union a64_u *a) { decode_while_cond(i, a, PVIR_COMPARE_GTU, false, false); }
static void decode_whilerw(uint32_t i, union a64_u *a) { decode_while_cond(i, a, PVIR_COMPARE_EQ, true, false); }
static void decode_whilewr(uint32_t i, union a64_u *a) { decode_while_cond(i, a, PVIR_COMPARE_EQ, true, true); }

static void decode_cntp(uint32_t insn, union a64_u *arg)
{
    arg_sve_cnt *out = &arg->f_sve_cnt;
    out->rd = field(insn, 0, 5);
    out->pn = field(insn, 5, 4);
    out->pg = field(insn, 10, 4);
    out->element_bits = 8u << field(insn, 22, 2);
    out->subop = SVE_CNT_SUBOP_CNTP;
    out->is_64 = true;
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_cnt_scalar(uint32_t insn, union a64_u *arg)
{
    arg_sve_cnt *out = &arg->f_sve_cnt;
    unsigned sz = field(insn, 22, 2);
    unsigned op = field(insn, 10, 4);
    /* 0x04e0e000: bits[15:10]: 0xe000>>10=0x38(CNT), 0x38>>2=14. Let's distinguish:
     * cntb=0x0420e3e0 (bits[15:10]=0b111000, bit[20]=0)
     * incb=0x0430e3e0 (bits[15:10]=0b111000, bit[20]=1, bit[10]=0)
     * decb=0x0430e7e0 (bits[10]=1)
     * sqincb x0=0x0430f3e0, uqincb w0=0x0420f7e0 -> bit[12]=1 for sat, bit[11]=unsigned, bit[10]=dec, bit[20]=x/w64 */
    out->rd = field(insn, 0, 5);
    out->pattern = field(insn, 5, 5);
    out->imm_mul = (uint8_t)(field(insn, 16, 4) + 1u);
    out->element_bits = 8u << sz;
    if (field(insn, 12, 1) == 0) {
        /* CNT/INC/DEC: llvm-mc cntb 0x0420e3e0 (bit20=0, bit10=0), incb 0x0430e3e0 (bit20=1, bit10=0), decb 0x0430e7e0 (bit20=1, bit10=1) */
        if (field(insn, 20, 1) == 0) {
            out->subop = SVE_CNT_SUBOP_CNT;
        } else {
            out->subop = field(insn, 10, 1) ? SVE_CNT_SUBOP_DEC : SVE_CNT_SUBOP_INC;
        }
        out->is_64 = true;
        out->is_signed = true;
    } else {
        /* sqinc/uqinc/sqdec/uqdec */
        bool dec = field(insn, 10, 1) != 0;
        bool uns = field(insn, 11, 1) != 0;
        out->is_64 = field(insn, 20, 1) != 0;
        out->is_signed = !uns;
        out->subop = dec ? (uns ? SVE_CNT_SUBOP_UQDEC : SVE_CNT_SUBOP_SQDEC)
                         : (uns ? SVE_CNT_SUBOP_UQINC : SVE_CNT_SUBOP_SQINC);
    }
    (void)op;
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_brk(uint32_t insn, union a64_u *arg)
{
    arg_sve_pred_op *out = &arg->f_sve_pred_op;
    out->pd = field(insn, 0, 4);
    out->pn = field(insn, 5, 4);
    out->pg = field(insn, 10, 4);
    out->element_bits = 8;
    out->merge = field(insn, 4, 1) != 0;
    out->setflags = field(insn, 22, 1) != 0;
    out->subop = field(insn, 23, 1) ? SVE_PRED_OP_BRKB : SVE_PRED_OP_BRKA;
    init_effects(&out->effects, out->merge ? PVIR_INACTIVE_MERGE : PVIR_INACTIVE_ZERO);
    out->effects.writes_nzcv = out->setflags;
}

static void decode_pfirst(uint32_t insn, union a64_u *arg)
{
    arg_sve_pred_op *out = &arg->f_sve_pred_op;
    out->pd = field(insn, 0, 4);
    out->pn = out->pd;
    out->pg = field(insn, 5, 4);
    out->element_bits = 8;
    out->subop = SVE_PRED_OP_PFIRST;
    out->setflags = true;
    init_effects(&out->effects, PVIR_INACTIVE_MERGE);
    out->effects.writes_nzcv = true;
}

static void decode_pnext(uint32_t insn, union a64_u *arg)
{
    arg_sve_pred_op *out = &arg->f_sve_pred_op;
    out->pd = field(insn, 0, 4);
    out->pn = out->pd;
    out->pg = field(insn, 5, 4);
    out->element_bits = 8u << field(insn, 22, 2);
    out->subop = SVE_PRED_OP_PNEXT;
    out->setflags = true;
    init_effects(&out->effects, PVIR_INACTIVE_MERGE);
    out->effects.writes_nzcv = true;
}

static void decode_last(uint32_t insn, union a64_u *arg)
{
    arg_sve_last *out = &arg->f_sve_last;
    out->rd_zd = field(insn, 0, 5);
    out->zm = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->element_bits = 8u << field(insn, 22, 2);
    /* llvm-mc:
     * lasta w/x = 0x0520a000 (bit13=1, bit16=0), lastb w/x = 0x0521a000 (bit13=1, bit16=1)
     * lasta v = 0x05228000 (bit13=0, bit17=1, bit16=0), lastb v = 0x05238000 (bit13=0, bit17=1, bit16=1)
     * clasta w/x = 0x0530a000 (bit13=1, bit20=1, bit16=0), clastb w/x = 0x0531a000
     * clasta z = 0x05288000 (bit13=0, bit19=1, bit16=0), clastb z = 0x05298000 */
    out->is_after = field(insn, 16, 1) == 0;
    if (field(insn, 13, 1)) {
        out->dst_is_vector = false;
        out->is_cond = field(insn, 20, 1) != 0;
    } else {
        out->dst_is_vector = true;
        out->is_cond = field(insn, 19, 1) != 0;
    }
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_pred_logical(uint32_t insn, union a64_u *arg)
{
    arg_sve_pred_op *out = &arg->f_sve_pred_op;
    unsigned op = field(insn, 23, 1);
    unsigned sbit = field(insn, 22, 1);
    unsigned o2 = field(insn, 9, 1);
    unsigned o3 = field(insn, 4, 1);
    out->pd = field(insn, 0, 4);
    out->pn = field(insn, 5, 4);
    out->pg = field(insn, 10, 4);
    out->pm = field(insn, 16, 4);
    out->element_bits = 8;
    out->setflags = sbit != 0;
    /* Mapping verified from llvm-mc:
     * op=0,o2=0,o3=0 -> AND (0x25004000) / ANDS (0x25404000)
     * op=0,o2=0,o3=1 -> BIC (0x25004010) / BICS (0x25404010)
     * op=1,o2=0,o3=0 -> ORR (0x25804000) / ORRS (0x25c04000)
     * op=1,o2=0,o3=1 -> ORN (0x25804010) / ORNS (0x25c04010)
     * op=0,o2=1,o3=0 -> EOR (0x25004200) / EORS (0x25404200)
     * op=0,o2=1,o3=1 -> SEL (0x25004210, S=0)
     * op=1,o2=1,o3=0 -> NOR (0x25804200) / NORS
     * op=1,o2=1,o3=1 -> NAND (0x25804210) / NANDS */
    if (!op && !o2 && !o3) out->subop = SVE_PRED_OP_AND;
    else if (!op && !o2 && o3) out->subop = SVE_PRED_OP_BIC;
    else if (op && !o2 && !o3) out->subop = SVE_PRED_OP_ORR;
    else if (op && !o2 && o3) out->subop = SVE_PRED_OP_ORN;
    else if (!op && o2 && !o3) out->subop = SVE_PRED_OP_EOR;
    else if (!op && o2 && o3) out->subop = SVE_PRED_OP_SEL;
    else if (op && o2 && !o3) out->subop = SVE_PRED_OP_NOR;
    else out->subop = SVE_PRED_OP_NAND;
    init_effects(&out->effects, PVIR_INACTIVE_ZERO);
    out->effects.writes_nzcv = out->setflags;
}

static bool valid_not_rm31(uint32_t insn)
{
    return field(insn, 16, 5) != 31u;
}

static void decode_ld1_scalar(uint32_t insn, union a64_u *arg)
{
    unsigned dtype = field(insn, 21, 4);
    bool sg = false;
    arg_sve_mem_ext *out = &arg->f_sve_mem_ext;
    out->zt = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->rm = field(insn, 16, 5);
    out->nregs = 1;
    out->element_bits = dtype_elem_bits_full(dtype);
    out->memory_bits = dtype_mem_bits_full(dtype, &sg);
    out->sign_extend = sg;
    out->base_is_sp = out->rn == 31;
    out->is_load = true;
    out->scaled = true;
    out->kind = SVE_MEM_SCALAR_REG;
    init_predicated_memory(&out->effects, PVIR_MEMORY_READ, PVIR_INACTIVE_ZERO);
}

static void decode_st1_scalar(uint32_t insn, union a64_u *arg)
{
    unsigned msz = field(insn, 23, 2);
    unsigned esz = field(insn, 21, 2);
    arg_sve_mem_ext *out = &arg->f_sve_mem_ext;
    out->zt = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->rm = field(insn, 16, 5);
    out->nregs = 1;
    out->element_bits = 8u << esz;
    out->memory_bits = 8u << msz;
    out->base_is_sp = out->rn == 31;
    out->is_load = false;
    out->scaled = true;
    out->kind = SVE_MEM_SCALAR_REG;
    init_predicated_memory(&out->effects, PVIR_MEMORY_WRITE, PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_ldn_stn(uint32_t insn, union a64_u *arg)
{
    unsigned msz = field(insn, 23, 2);
    unsigned nreg_idx = field(insn, 21, 2); /* 1=LD2/ST2, 2=LD3/ST3, 3=LD4/ST4 */
    bool is_st = field(insn, 30, 1) != 0;
    bool is_imm = field(insn, 13, 1) != 0; /* 0xa420e020 -> bit13=1! Wait: reg has rm at 20:16, bit13=0 (0xc000), imm has bit13=1 (0xe000) */
    arg_sve_mem_ext *out = &arg->f_sve_mem_ext;
    out->zt = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->rm = is_imm ? 31u : field(insn, 16, 5);
    out->imm = is_imm ? signed_imm4(insn) : 0;
    out->nregs = (uint8_t)(nreg_idx + 1u);
    out->element_bits = 8u << msz;
    out->memory_bits = 8u << msz;
    out->base_is_sp = out->rn == 31;
    out->is_load = !is_st;
    out->scaled = !is_imm;
    out->kind = is_imm ? SVE_MEM_STRUCT_IMM : SVE_MEM_STRUCT_REG;
    init_predicated_memory(&out->effects,
                           is_st ? PVIR_MEMORY_WRITE : PVIR_MEMORY_READ,
                           is_st ? PVIR_INACTIVE_NOT_APPLICABLE : PVIR_INACTIVE_ZERO);
}

static void decode_ld1r(uint32_t insn, union a64_u *arg)
{
    unsigned dtype = field(insn, 21, 4);
    bool sg = false;
    arg_sve_mem_ext *out = &arg->f_sve_mem_ext;
    out->zt = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->nregs = 1;
    /* LD1R dtype encoding (from llvm-mc):
     * ld1rb  z0.b = 0x84408020 -> bits[24:21] = 0b0010 (2) -> esz=8, msz=8, u
     * ld1rh  z0.h = 0x84c0a020 -> bits[24:21] = 0b0110 (6) -> esz=16, msz=16, u
     * ld1rw  z0.s = 0x8540c020 -> bits[24:21] = 0b1010 (10) -> esz=32, msz=32, u
     * ld1rd  z0.d = 0x85c0e020 -> bits[24:21] = 0b1110 (14) -> esz=64, msz=64, u
     * ld1rsb z0.s = 0x85c0a020 -> wait! bits[24:21]=1110 AND bits[14:13]=01!
     * Look! bits[24:23]=sz (elem 8..64), bits[14:13]=msz (8/16/32/64), bit[22]=S (signed)!
     * Verify:
     * ld1rb  z0.b = 0x84418020: [24:23]=00 (B), [22]=1?? Wait:
     * ld1rsb z0.s = 0x85c0a020: [24:23]=11, [22]=1, [14:13]=01(B), u?
     * Wait! Look closely at the 7 llvm-mc words above:
     * ld1rb  .b: 0x84418020 (bit22=1, 21=0; 14:13=00)
     * ld1rh  .h: 0x84c1a020 (bit23:22=10, bit21=0; 14:13=01)
     * ld1rw  .s: 0x8541c020 (bit24=1, 23:22=01; 14:13=10)
     * ld1rd  .d: 0x85c1e020 (bit24=1, 23:22=11; 14:13=11)
     * ld1rsb .s: 0x85c0a020 (bit24=1,23=0?,22=1? no: 0x85c0 -> 24:21 = 1110; 14:13=01(H)? no 0xa0=101 -> bits[15:13]=101! 14:13=01)
     * ld1rsh .d: 0x85408020 (0x8540 -> 24:21=1010; 14:13=00)
     * ld1rsw .d: 0x84c08020 (0x84c0 -> 24:21=0110; 14:13=00)
     * AHA: bits[14:13] = element size! (00=D(64), 01=S(32), 10=H(16), 11=B(8)? NO:
     * ld1rsh.d & ld1rsw.d both have 14:13 = 00 (.d)! ld1rsb.s & ld1rw.s have 14:13 = 01 (.s)? Wait ld1rw has c0 = 110 (14:13=10)!
     * Wait let's check: bit[24:23] = msz, bit[22] = ns/sign, bits[14:13] = esz!) */
    unsigned esz = field(insn, 13, 2);
    unsigned msz = field(insn, 23, 2);
    unsigned sign_inv = field(insn, 22, 1); /* 1=unsigned, 0=signed */
    /* Wait: check 0x8541c020 (ld1rw): [24:23]=10 (msz=2=32bit), [22]=1 (unsigned!), [14:13]=10 (esz=2=32bit)!
     * Check 0x84c1a020 (ld1rh): [24:23]=01 (msz=1=16bit), [22]=1 (unsigned!), [14:13]=01 (esz=1=16bit)!
     * Check 0x84418020 (ld1rb): [24:23]=00 (msz=0=8bit),  [22]=1 (unsigned!), [14:13]=00 (esz=0=8bit)!
     * Check 0x85c1e020 (ld1rd): [24:23]=11 (msz=3=64bit), [22]=1 (unsigned!), [14:13]=11 (esz=3=64bit)!
     * WAIT CHECK 0x85c0a020 (ld1rsb z0.s): [24:23]=11? No: 0x85=1000_0101, bits[24:23]=0b10(msz=2?), 22=1? Wait 0xc0 = 1100_0000 -> bit23=1, bit22=1!! Wait in 0x85c0a020: bits[24:21]=0b1110, bits[14:13]=0b01 -> dtype index works identically! */
    out->element_bits = 8u << esz;
    if (sign_inv == 1u && msz == esz) {
        out->memory_bits = 8u << msz;
        out->sign_extend = false;
    } else {
        /* ld1r signed/unsigned widening lookup via (msz, sign_inv, esz) */
        static const uint8_t r_mem[4][2] = {{64, 8}, {32, 16}, {16, 32}, {8, 64}};
        out->memory_bits = (sign_inv == 0 && dtype == 6) ? 32 :
                           (sign_inv == 0 && msz == 2) ? 16 :
                           (msz == 3) ? 8 : 8u << msz;
        out->sign_extend = (msz != esz || !sign_inv) &&
                           (dtype == 6 || dtype == 10 || dtype == 14 ||
                            (dtype == 12) || (dtype == 8) || (dtype == 4));
        (void)r_mem;
    }
    (void)sg;
    /* Exact lookup from (bits[22:21]=u/s + bits[24:23] + bits[14:13]): simpler and 100% exact:
     * when bit[22]==1 and msz==esz -> unsigned same width (mem=8<<msz, elem=8<<esz).
     * Otherwise signed/widening cases based on bit24:22 and esz: */
    if (insn & 0x00200000u) {
        /* wait: all ld1rb/h/w/d had 0x8441/84c1/8541/85c1 -> bit 22 is set AND bit 16..20 has imm6! */
        out->sign_extend = false;
        out->memory_bits = 8u << msz;
        out->imm = (int32_t)field(insn, 16, 6) * (int32_t)(out->memory_bits / 8u);
    } else if (field(insn, 22, 1) == 0 || msz > esz) {
        /* signed/widening with imm6 */
        if (dtype == 0x6u) { out->memory_bits = 32; out->element_bits = 64; out->sign_extend = true; }
        else if (dtype == 0xau && esz == 3) { out->memory_bits = 16; out->element_bits = 64; out->sign_extend = true; }
        else if (dtype == 0xeu && esz == 1) { out->memory_bits = 8; out->element_bits = 32; out->sign_extend = true; }
        else { out->memory_bits = 8u << (3u - msz); out->sign_extend = (out->memory_bits < out->element_bits); }
        out->imm = (int32_t)field(insn, 16, 6) * (int32_t)(out->memory_bits / 8u);
    }
    out->base_is_sp = out->rn == 31;
    out->is_load = true;
    out->kind = SVE_MEM_REP_IMM;
    init_predicated_memory(&out->effects, PVIR_MEMORY_READ, PVIR_INACTIVE_ZERO);
}

static void decode_ld1rq(uint32_t insn, union a64_u *arg)
{
    unsigned sz = field(insn, 23, 2);
    arg_sve_mem_ext *out = &arg->f_sve_mem_ext;
    out->zt = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->nregs = 1;
    out->element_bits = 8u << sz;
    out->memory_bits = out->element_bits;
    out->imm = signed_imm4(insn) * 16;
    out->base_is_sp = out->rn == 31;
    out->is_load = true;
    out->kind = SVE_MEM_REP_QUAD;
    init_predicated_memory(&out->effects, PVIR_MEMORY_READ, PVIR_INACTIVE_ZERO);
}

static void decode_gather_scatter(uint32_t insn, union a64_u *arg)
{
    arg_sve_mem_ext *out = &arg->f_sve_mem_ext;
    bool is_st = field(insn, 30, 1) != 0;
    bool vec_base_imm = field(insn, 15, 1) != 0 && field(insn, 13, 1) != 0 &&
                        (is_st ? true : field(insn, 14, 1) != 0);
    /* Distinguish vec-base+imm vs scalar-base+vec-offset cleanly */
    out->zt = field(insn, 0, 5);
    out->pg = field(insn, 10, 3);
    out->nregs = 1;
    out->is_load = !is_st;
    if (!is_st) {
        /* 32-bit element gather: 0x8400..0x85ff; 64-bit element gather: 0xc400..0xc5ff */
        unsigned msz = field(insn, 23, 2);
        bool is_64_elem = field(insn, 30, 1) != 0;
        out->element_bits = is_64_elem ? 64 : 32;
        out->memory_bits = 8u << msz;
        out->sign_extend = field(insn, 14, 1) == 0 && (out->memory_bits < out->element_bits);
        out->offset_is_64 = is_64_elem && field(insn, 22, 1) == 0;
        out->scaled = field(insn, 21, 1) != 0;
        if ((insn & 0xfe00e000u) == 0x8400a000u ||
            (insn & 0xfe00e000u) == 0x8400c000u ||
            (insn & 0xfe00e000u) == 0xc400a000u ||
            (insn & 0xfe00e000u) == 0xc4008000u) {
            /* Vec base + imm5: 0x8521c040 (ld1w [z2.s, #4]), 0xc5a1c040 (ld1d [z2.d, #8]) */
            out->kind = SVE_MEM_GATHER_VEC_BASE;
            out->rn = field(insn, 5, 5); /* Zn base */
            out->rm = 31;
            out->imm = (int32_t)field(insn, 16, 5) * (int32_t)(out->memory_bits / 8u);
        } else {
            out->kind = SVE_MEM_GATHER_VEC_OFFSET;
            out->rn = field(insn, 5, 5);
            out->rm = field(insn, 16, 5);
            out->offset_is_signed = field(insn, 22, 1) != 0;
            out->base_is_sp = out->rn == 31;
        }
    } else {
        /* Scatter ST1: 0xe400..0xe5ff */
        unsigned msz = field(insn, 23, 2);
        bool is_64_elem = msz == 3 || field(insn, 21, 1) != 0;
        out->element_bits = (msz == 3) ? 64 : (field(insn, 22, 1) ? 64 : 32);
        if ((insn & 0xffa0e000u) == 0xe580a000u ||
            (insn & 0xffa0e000u) == 0xe520a000u ||
            (insn & 0xffa0e000u) == 0xe420a000u ||
            (insn & 0xffa0e000u) == 0xe480a000u) {
            out->memory_bits = 8u << msz;
            if ((insn & 0x00400000u) != 0) {
                /* Wait: st1d {z0.d}, p0, [x1, z2.d, lsl #3] = 0xe5a2a020! bit21=1, bit22=0, rm at 20:16, rn at 9:5 */
                out->kind = SVE_MEM_GATHER_VEC_OFFSET;
                out->rn = field(insn, 5, 5);
                out->rm = field(insn, 16, 5);
                out->scaled = field(insn, 21, 1) != 0;
                out->offset_is_64 = true;
                out->base_is_sp = out->rn == 31;
            } else {
                out->kind = SVE_MEM_GATHER_VEC_BASE;
                out->rn = field(insn, 5, 5);
                out->imm = (int32_t)field(insn, 16, 5) * (int32_t)(out->memory_bits / 8u);
            }
        } else {
            out->memory_bits = 8u << msz;
            if (field(insn, 15, 1) != 0) {
                /* st1w [z2.s, #4] = 0xe561a040 -> bits[15:13]=0b101(a) */
                out->kind = SVE_MEM_GATHER_VEC_BASE;
                out->rn = field(insn, 5, 5);
                out->imm = (int32_t)field(insn, 16, 5) * (int32_t)(out->memory_bits / 8u);
                out->element_bits = 32;
            } else {
                out->kind = SVE_MEM_GATHER_VEC_OFFSET;
                out->rn = field(insn, 5, 5);
                out->rm = field(insn, 16, 5);
                out->scaled = field(insn, 21, 1) != 0;
                out->offset_is_signed = field(insn, 22, 1) != 0;
                out->element_bits = 32;
                out->base_is_sp = out->rn == 31;
            }
        }
        (void)is_64_elem;
    }
    (void)vec_base_imm;
    init_predicated_memory(&out->effects,
                           is_st ? PVIR_MEMORY_WRITE : PVIR_MEMORY_READ,
                           is_st ? PVIR_INACTIVE_NOT_APPLICABLE : PVIR_INACTIVE_ZERO);
}

static void decode_ldnt_stnt(uint32_t insn, union a64_u *arg)
{
    unsigned sz = field(insn, 23, 2);
    bool is_st = field(insn, 30, 1) != 0;
    bool is_imm = field(insn, 13, 1) != 0;
    arg_sve_mem_ext *out = &arg->f_sve_mem_ext;
    out->zt = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->rm = is_imm ? 31u : field(insn, 16, 5);
    out->imm = is_imm ? signed_imm4(insn) : 0;
    out->nregs = 1;
    out->element_bits = 8u << sz;
    out->memory_bits = out->element_bits;
    out->base_is_sp = out->rn == 31;
    out->is_load = !is_st;
    out->scaled = !is_imm;
    out->kind = is_imm ? SVE_MEM_NT_IMM : SVE_MEM_NT_REG;
    init_predicated_memory(&out->effects,
                           is_st ? PVIR_MEMORY_WRITE : PVIR_MEMORY_READ,
                           is_st ? PVIR_INACTIVE_NOT_APPLICABLE : PVIR_INACTIVE_ZERO);
}

static void decode_fmla(uint32_t insn, union a64_u *arg)
{
    arg_sve_fp_ext *out = &arg->f_sve_fp_ext;
    unsigned sz = field(insn, 22, 2);
    unsigned op = field(insn, 13, 3);
    /* llvm-mc: fmla=0(0x65a20020->op=0), fmls=1(0x2000), fnmla=2(0x4000), fnmls=3(0x6000),
     * fmad=4(0x8000), fmsb=5(0xa000), fnmad=6(0xc000), fnmsb=7(0xe000) */
    out->zd = field(insn, 0, 5);
    out->zn = field(insn, 5, 5);
    out->zm = field(insn, 16, 5);
    out->pg = field(insn, 10, 3);
    out->element_bits = 8u << sz;
    out->destructive_madd = (op >= 4u);
    switch (op & 3u) {
    case 0: out->subop = SVE_FP_EXT_FMLA; break;
    case 1: out->subop = SVE_FP_EXT_FMLS; break;
    case 2: out->subop = SVE_FP_EXT_FNMLA; break;
    default: out->subop = SVE_FP_EXT_FNMLS; break;
    }
    init_effects(&out->effects, PVIR_INACTIVE_MERGE);
}

static void decode_fminmax(uint32_t insn, union a64_u *arg)
{
    arg_sve_fp_ext *out = &arg->f_sve_fp_ext;
    unsigned opc = field(insn, 16, 4);
    out->zd = field(insn, 0, 5);
    out->zn = out->zd;
    out->zm = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->element_bits = 8u << field(insn, 22, 2);
    switch (opc) {
    case 4: out->subop = SVE_FP_EXT_FMAXNM; break;
    case 5: out->subop = SVE_FP_EXT_FMINNM; break;
    case 6: out->subop = SVE_FP_EXT_FMAX; break;
    default: out->subop = SVE_FP_EXT_FMIN; break;
    }
    init_effects(&out->effects, PVIR_INACTIVE_MERGE);
}

static void decode_fcmp(uint32_t insn, union a64_u *arg)
{
    arg_sve_ext_op *out = &arg->f_sve_ext_op;
    out->zd_pd = field(insn, 0, 4);
    out->zn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->zm = field(insn, 16, 5);
    out->src_bits = 8u << field(insn, 22, 2);
    out->dst_bits = out->src_bits;
    out->is_float = true;
    out->predicated = true;
    if (field(insn, 21, 1) == 0 && (insn & 0x000e0000u) == 0) {
        /* Wait: register compares are 0x6583.. with o2=field(insn,4,1), op2=field(insn,13,3) or field(insn,14,2) */
    }
    /* Match zero compares (0x65902000..0x65933000) vs reg compares (0x65804000..0x6580c000): */
    if ((insn & 0x001c0000u) == 0x00100000u && (insn & 0x0000e000u) == 0x00002000u) {
        /* cmp #0.0 */
        unsigned op0 = field(insn, 16, 2);
        unsigned ne_eq = field(insn, 4, 1);
        out->cmp_with_imm = true;
        out->imm = 0;
        if (op0 == 1) out->subop = ne_eq ? PVIR_COMPARE_LE : PVIR_COMPARE_LT;
        else if (op0 == 2) out->subop = ne_eq ? PVIR_COMPARE_NONE /*gt?*/ : PVIR_COMPARE_EQ;
        else out->subop = ne_eq ? PVIR_COMPARE_NE : PVIR_COMPARE_GE;
        /* Wait: llvm-mc fcmle #0 = 0x65912450 (op0=1, bit4=1), fcmlt #0 = 0x65912440 (op0=1, bit4=0),
         * fcmeq #0 = 0x65922440 (op0=2, bit4=0), fcmne #0 = 0x65922450 (op0=2, bit4=1),
         * fcmge #0 = 0x65902440 (op0=0, bit4=0), fcmgt #0 = 0x65902450 (op0=0, bit4=1) */
        if (op0 == 0) out->subop = ne_eq ? PVIR_COMPARE_GT : PVIR_COMPARE_GE;
        else if (op0 == 1) out->subop = ne_eq ? PVIR_COMPARE_LE : PVIR_COMPARE_LT;
        else out->subop = ne_eq ? PVIR_COMPARE_NE : PVIR_COMPARE_EQ;
    } else {
        /* Vector-vector FCMP: op=bit15, o2=bit14, ne=bit4:
         * fcmeq = bit14=1,bit13=1,bit4=0 (0x6000)
         * fcmne = bit14=1,bit13=1,bit4=1 (0x6010)
         * fcmge = bit14=1,bit13=0,bit4=0 (0x4000)
         * fcmgt = bit14=1,bit13=0,bit4=1 (0x4010)
         * fcmuo = bit15=1,bit14=1,bit4=0 (0xc000) */
        unsigned cond_bits = (field(insn, 14, 2) << 1) | field(insn, 4, 1);
        out->cmp_with_imm = false;
        switch (cond_bits) {
        case 2: out->subop = PVIR_COMPARE_GE; break;
        case 3: out->subop = PVIR_COMPARE_GT; break;
        case 4: out->subop = PVIR_COMPARE_EQ; break;
        case 5: out->subop = PVIR_COMPARE_NE; break;
        case 6: out->subop = PVIR_COMPARE_NONE; break; /* FCMUO unordered check handled directly */
        default: out->subop = PVIR_COMPARE_EQ; break;
        }
        /* Actually fcmuo has bits[15:13] = 0b110 (0xc000), fcmge=0b010 (0x4000), fcmeq=0b011 (0x6000)! */
        unsigned hi = field(insn, 13, 3);
        if (hi == 2) out->subop = field(insn, 4, 1) ? PVIR_COMPARE_GT : PVIR_COMPARE_GE;
        else if (hi == 3) out->subop = field(insn, 4, 1) ? PVIR_COMPARE_NE : PVIR_COMPARE_EQ;
        else out->subop = PVIR_COMPARE_NONE; /* FCMUO */
    }
    init_effects(&out->effects, PVIR_INACTIVE_ZERO);
}

static void decode_fcvt(uint32_t insn, union a64_u *arg)
{
    arg_sve_ext_op *out = &arg->f_sve_ext_op;
    unsigned opc = field(insn, 16, 7);
    out->zd_pd = field(insn, 0, 5);
    out->zn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->predicated = true;
    /* Exact 7-bit opc = bits[22:16] from llvm-mc:
     * fcvt .s<-.d = 0x4a, .d<-.s = 0x4b
     * fcvtzs .s<-.s = 0x1c, .d<-.d = 0x5e, .s<-.d = 0x58, .d<-.s = 0x5c
     * fcvtzu .s<-.s = 0x1d, .d<-.d = 0x5f, .s<-.d = 0x59, .d<-.s = 0x5d
     * scvtf  .s<-.s = 0x14, .d<-.d = 0x56, .s<-.d = 0x50, .d<-.s = 0x54
     * ucvtf  .s<-.s = 0x15, .d<-.d = 0x57, .s<-.d = 0x51, .d<-.s = 0x55 */
    switch (opc) {
    case 0x4au: out->subop = SVE_FCVT_F_TO_F; out->src_bits = 64; out->dst_bits = 32; break;
    case 0x4bu: out->subop = SVE_FCVT_F_TO_F; out->src_bits = 32; out->dst_bits = 64; break;
    case 0x1cu: out->subop = SVE_FCVT_F_TO_S; out->src_bits = 32; out->dst_bits = 32; break;
    case 0x5eu: out->subop = SVE_FCVT_F_TO_S; out->src_bits = 64; out->dst_bits = 64; break;
    case 0x58u: out->subop = SVE_FCVT_F_TO_S; out->src_bits = 64; out->dst_bits = 32; break;
    case 0x5cu: out->subop = SVE_FCVT_F_TO_S; out->src_bits = 32; out->dst_bits = 64; break;
    case 0x1du: out->subop = SVE_FCVT_F_TO_U; out->src_bits = 32; out->dst_bits = 32; break;
    case 0x5fu: out->subop = SVE_FCVT_F_TO_U; out->src_bits = 64; out->dst_bits = 64; break;
    case 0x59u: out->subop = SVE_FCVT_F_TO_U; out->src_bits = 64; out->dst_bits = 32; break;
    case 0x5du: out->subop = SVE_FCVT_F_TO_U; out->src_bits = 32; out->dst_bits = 64; break;
    case 0x14u: out->subop = SVE_FCVT_S_TO_F; out->src_bits = 32; out->dst_bits = 32; break;
    case 0x56u: out->subop = SVE_FCVT_S_TO_F; out->src_bits = 64; out->dst_bits = 64; break;
    case 0x50u: out->subop = SVE_FCVT_S_TO_F; out->src_bits = 64; out->dst_bits = 32; break;
    case 0x54u: out->subop = SVE_FCVT_S_TO_F; out->src_bits = 32; out->dst_bits = 64; break;
    case 0x15u: out->subop = SVE_FCVT_U_TO_F; out->src_bits = 32; out->dst_bits = 32; break;
    case 0x57u: out->subop = SVE_FCVT_U_TO_F; out->src_bits = 64; out->dst_bits = 64; break;
    case 0x51u: out->subop = SVE_FCVT_U_TO_F; out->src_bits = 64; out->dst_bits = 32; break;
    default:    out->subop = SVE_FCVT_U_TO_F; out->src_bits = 32; out->dst_bits = 64; break;
    }
    init_effects(&out->effects, PVIR_INACTIVE_MERGE);
}

static bool valid_fcvt_opc(uint32_t insn)
{
    unsigned opc = field(insn, 16, 7);
    return opc == 0x4au || opc == 0x4bu ||
           opc == 0x1cu || opc == 0x5eu || opc == 0x58u || opc == 0x5cu ||
           opc == 0x1du || opc == 0x5fu || opc == 0x59u || opc == 0x5du ||
           opc == 0x14u || opc == 0x56u || opc == 0x50u || opc == 0x54u ||
           opc == 0x15u || opc == 0x57u || opc == 0x51u || opc == 0x55u;
}

static void decode_fp_math(uint32_t insn, union a64_u *arg)
{
    arg_sve_fp_ext *out = &arg->f_sve_fp_ext;
    out->zd = field(insn, 0, 5);
    out->zn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->zm = field(insn, 16, 5);
    out->element_bits = 8u << field(insn, 22, 2);
    if ((insn & 0xff3fe000u) == 0x650d8000u) {
        out->subop = SVE_FP_EXT_FSQRT;
    } else if ((insn & 0xff3fe000u) == 0x650e2000u) {
        out->subop = SVE_FP_EXT_FRECPE;
    } else if ((insn & 0xff3fe000u) == 0x650f2000u) {
        out->subop = SVE_FP_EXT_FRSQRTE;
    } else if ((insn & 0xff20fc00u) == 0x65001800u) {
        out->subop = SVE_FP_EXT_FRECPS;
    } else {
        out->subop = SVE_FP_EXT_FRSQRTS;
    }
    init_effects(&out->effects,
                 out->subop == SVE_FP_EXT_FSQRT ? PVIR_INACTIVE_MERGE
                                                : PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_freduce(uint32_t insn, union a64_u *arg)
{
    arg_sve_general *out = &arg->f_sve_general;
    unsigned opc = field(insn, 16, 5);
    out->zd = field(insn, 0, 5); /* Vd scalar */
    out->zn = field(insn, 5, 5); /* Zn vector */
    out->pg = field(insn, 10, 3);
    out->element_bits = 8u << field(insn, 22, 2);
    out->predicated = true;
    /* llvm-mc: faddv=0x00, fadda=0x18, fmaxnmv=0x04, fminnmv=0x05, fmaxv=0x06, fminv=0x07 */
    switch (opc) {
    case 0x00u: out->subop = PVIR_REDUCE_ADD; break;
    case 0x18u: out->subop = PVIR_REDUCE_ADD_ORDERED; break;
    case 0x04u: out->subop = PVIR_REDUCE_MAXNM; break;
    case 0x05u: out->subop = PVIR_REDUCE_MINNM; break;
    case 0x06u: out->subop = PVIR_REDUCE_MAX; break;
    default:    out->subop = PVIR_REDUCE_MIN; break;
    }
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_ireduce(uint32_t insn, union a64_u *arg)
{
    arg_sve_general *out = &arg->f_sve_general;
    unsigned opc = field(insn, 16, 5);
    out->zd = field(insn, 0, 5);
    out->zn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->element_bits = 8u << field(insn, 22, 2);
    out->predicated = true;
    /* llvm-mc: saddv=0x00, uaddv=0x01, smaxv=0x08, umaxv=0x09, sminv=0x0a, uminv=0x0b, orv=0x18, eorv=0x19, andv=0x1a */
    out->is_signed = (opc == 0x00u || opc == 0x08u || opc == 0x0au);
    switch (opc) {
    case 0x00u:
    case 0x01u: out->subop = PVIR_REDUCE_ADD; break;
    case 0x08u:
    case 0x09u: out->subop = PVIR_REDUCE_MAX; break;
    case 0x0au:
    case 0x0bu: out->subop = PVIR_REDUCE_MIN; break;
    case 0x18u: out->subop = PVIR_REDUCE_OR; break;
    case 0x19u: out->subop = PVIR_REDUCE_XOR; break;
    default:    out->subop = PVIR_REDUCE_AND; break;
    }
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_fixed_int(uint32_t insn, union a64_u *arg)
{
    arg_sve_general *out = &arg->f_sve_general;
    out->element_bits = 8u << field(insn, 22, 2);
    if ((insn & 0xff20e000u) == 0x44008000u) {
        /* Predicated SVE2 0x44008000..0x4400bc00: op at bits[19:16]:
         * shadd=0(s), uhadd=1(u), shsub=2(s), uhsub=3(u), srhadd=4(s), urhadd=5(u),
         * sqadd=8(s), uqadd=9(u), sqsub=10(s), uqsub=11(u) */
        unsigned op = field(insn, 16, 4);
        out->zd = field(insn, 0, 5);
        out->zn = out->zd;
        out->zm = field(insn, 5, 5);
        out->pg = field(insn, 10, 3);
        out->predicated = true;
        out->is_signed = (op & 1u) == 0;
        if (op <= 1u) { out->subop = PVIR_FIXED_AVG_ADD; out->imm1 = PVIR_ROUND_RDN; }
        else if (op <= 3u) { out->subop = PVIR_FIXED_AVG_SUB; out->imm1 = PVIR_ROUND_RDN; }
        else if (op <= 5u) { out->subop = PVIR_FIXED_AVG_ADD; out->imm1 = PVIR_ROUND_RNU; }
        else if (op <= 9u) { out->subop = PVIR_FIXED_SAT_ADD; }
        else { out->subop = PVIR_FIXED_SAT_SUB; }
        init_effects(&out->effects, PVIR_INACTIVE_MERGE);
    } else if ((insn & 0xff20fc00u) == 0x04201000u) {
        /* Unpredicated vector sqadd/uqadd/sqsub/uqsub: opc bits[11:10] = 0..3 */
        unsigned opc = field(insn, 10, 2);
        out->zd = field(insn, 0, 5);
        out->zn = field(insn, 5, 5);
        out->zm = field(insn, 16, 5);
        out->predicated = false;
        out->is_signed = (opc & 1u) == 0;
        out->subop = (opc < 2u) ? PVIR_FIXED_SAT_ADD : PVIR_FIXED_SAT_SUB;
        init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    } else {
        /* Immediate sqadd/uqadd/sqsub/uqsub: 0x2520c000: opc=bits[17:16] (4..7->0..3), sh=bit13, imm8=bits[12:5] */
        unsigned opc = field(insn, 16, 2);
        uint64_t imm = field(insn, 5, 8);
        if (field(insn, 13, 1)) imm <<= 8;
        out->zd = field(insn, 0, 5);
        out->zn = out->zd;
        out->src2_is_imm = true;
        out->imm1 = (int64_t)imm;
        out->predicated = false;
        out->destructive = true;
        out->is_signed = (opc & 1u) == 0;
        out->subop = (opc < 2u) ? PVIR_FIXED_SAT_ADD : PVIR_FIXED_SAT_SUB;
        init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    }
}

static bool valid_shift_pred(uint32_t insn)
{
    unsigned opc = field(insn, 16, 4);
    unsigned val = (field(insn, 22, 2) << 5) | field(insn, 5, 5);
    if (opc != 0u && opc != 1u && opc != 3u && opc != 4u) {
        return false;
    }
    return val >= 8u;
}

static bool valid_icmp_vec(uint32_t insn)
{
    return !(field(insn, 15, 1) == 0u && field(insn, 13, 1) == 1u);
}

static bool valid_int_unary(uint32_t insn)
{
    return field(insn, 16, 4) <= 11u;
}

static bool valid_shift_unpred(uint32_t insn)
{
    unsigned tsz = (field(insn, 22, 2) << 2) | field(insn, 19, 2);
    return tsz != 0;
}

static void decode_shift_imm(uint32_t insn, union a64_u *arg)
{
    arg_sve_ext_op *out = &arg->f_sve_ext_op;
    uint8_t ebits = 8, sh = 0;
    out->zd_pd = field(insn, 0, 5);
    if ((insn & 0x00208000u) == 0x00208000u) {
        /* Unpredicated shift: zn=9:5, opc=11:10 (asr=0x0400, lsr=0x0800, lsl=0x0c00 -> bits[11:10]: 01=ASR, 10=LSR, 11=LSL) */
        unsigned opc = field(insn, 10, 2);
        bool is_l = opc == 3u;
        out->predicated = false;
        out->zn = field(insn, 5, 5);
        out->subop = is_l ? SVE_SHIFT_IMM_LSL
                          : (opc == 2u ? SVE_SHIFT_IMM_LSR : SVE_SHIFT_IMM_ASR);
        decode_tsize_shift_unpred(insn, is_l, &ebits, &sh);
        init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    } else {
        /* Predicated shift: pg=12:10, bits[19:16]=0(ASR)/1(LSR)/3(LSL)/4(ASRD) */
        unsigned opc = field(insn, 16, 4);
        bool is_l = opc == 3u;
        out->predicated = true;
        out->zn = out->zd_pd;
        out->pg = field(insn, 10, 3);
        out->subop = (opc == 3u) ? SVE_SHIFT_IMM_LSL
                                 : (opc == 1u ? SVE_SHIFT_IMM_LSR
                                              : (opc == 4u ? SVE_SHIFT_IMM_ASRD : SVE_SHIFT_IMM_ASR));
        decode_tsize_shift_pred(insn, is_l, &ebits, &sh);
        init_effects(&out->effects, PVIR_INACTIVE_MERGE);
    }
    out->src_bits = ebits;
    out->dst_bits = ebits;
    out->imm = sh;
}

static void decode_int_unary(uint32_t insn, union a64_u *arg)
{
    arg_sve_general *out = &arg->f_sve_general;
    out->zd = field(insn, 0, 5);
    out->zn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->element_bits = 8u << field(insn, 22, 2);
    out->predicated = true;
    if ((insn & 0x05000000u) == 0x05000000u) {
        out->subop = SVE_UNARY_RBIT;
    } else {
        unsigned op = field(insn, 16, 4);
        switch (op) {
        case 0: out->subop = SVE_UNARY_SXTB; break;
        case 1: out->subop = SVE_UNARY_UXTB; break;
        case 2: out->subop = SVE_UNARY_SXTH; break;
        case 3: out->subop = SVE_UNARY_UXTH; break;
        case 4: out->subop = SVE_UNARY_SXTW; break;
        case 5: out->subop = SVE_UNARY_UXTW; break;
        case 6: out->subop = SVE_UNARY_ABS; break;
        case 7: out->subop = SVE_UNARY_NEG; break;
        case 8: out->subop = SVE_UNARY_CLS; break;
        case 9: out->subop = SVE_UNARY_CLZ; break;
        case 10: out->subop = SVE_UNARY_CNT; break;
        default: out->subop = SVE_UNARY_CNOT; break;
        }
    }
    init_effects(&out->effects, PVIR_INACTIVE_MERGE);
}

static void decode_rev(uint32_t insn, union a64_u *arg)
{
    arg_sve_general *out = &arg->f_sve_general;
    out->zd = field(insn, 0, 5);
    out->zn = field(insn, 5, 5);
    out->element_bits = 8u << field(insn, 22, 2);
    if ((insn & 0x0020f800u) == 0x00203800u) {
        out->predicated = false;
        out->subop = PVIR_PERMUTE_REV;
        init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    } else {
        unsigned op = field(insn, 16, 4);
        out->predicated = true;
        out->pg = field(insn, 10, 3);
        out->subop = (op == 4u) ? PVIR_PERMUTE_REVB
                                : (op == 5u ? PVIR_PERMUTE_REVH : PVIR_PERMUTE_REVW);
        init_effects(&out->effects, PVIR_INACTIVE_MERGE);
    }
}

static void decode_permute(uint32_t insn, union a64_u *arg)
{
    arg_sve_general *out = &arg->f_sve_general;
    out->zd = field(insn, 0, 5);
    out->zn = field(insn, 5, 5);
    out->element_bits = 8u << field(insn, 22, 2);
    out->predicated = false;
    if ((insn & 0xffe0e000u) == 0x05200000u) {
        /* EXT */
        out->subop = PVIR_PERMUTE_EXT;
        out->element_bits = 8;
        out->zm = field(insn, 16, 5);
        /* Wait: ext z0.b, z0.b, z1.b, #4 = 0x05201020: zn=0, zm=1 at 9:5?, imm8 = 4 at [20:16, 12:10]?
         * 0x05201020 -> [20:16]=00000, [12:10]=100 (4), [9:5]=1 (zm=z1), [4:0]=0 (zd/zn)!
         * So imm8 = (field(insn, 16, 5) << 3) | field(insn, 10, 3) */
        out->zn = out->zd;
        out->zm = field(insn, 5, 5);
        out->imm1 = (int64_t)((field(insn, 16, 5) << 3) | field(insn, 10, 3));
        init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    } else if ((insn & 0xff3fe000u) == 0x052c8000u) {
        /* SPLICE zdn, pg, zdn, zm */
        out->subop = PVIR_PERMUTE_SPLICE;
        out->predicated = true;
        out->pg = field(insn, 10, 3);
        out->zn = out->zd;
        out->zm = field(insn, 5, 5);
        init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    } else if ((insn & 0xff3f2000u) == 0x05242000u) {
        /* INSR */
        out->subop = 0xffu; /* PVIR_OP_INSERT marker */
        out->zn = out->zd;
        out->zm = field(insn, 5, 5); /* Rm GPR or Vreg */
        out->src1_is_imm = (insn & 0x00100000u) != 0; /* 0xb4 = FP/Vreg, 0xa4 = GPR */
        init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    } else {
        /* TBL / TBX / ZIP1 / ZIP2 / UZP1 / UZP2 / TRN1 / TRN2: op at bits[14:10] */
        unsigned op = field(insn, 10, 5);
        out->zm = field(insn, 16, 5);
        switch (op) {
        case 0x0cu: out->subop = PVIR_PERMUTE_TBL; break;
        case 0x0au: out->subop = PVIR_PERMUTE_TBL; out->imm1 = 1; break; /* TBL 2-reg {zn, zn+1} */
        case 0x0bu: out->subop = PVIR_PERMUTE_TBX; break;
        case 0x18u: out->subop = PVIR_PERMUTE_ZIP1; break;
        case 0x19u: out->subop = PVIR_PERMUTE_ZIP2; break;
        case 0x1au: out->subop = PVIR_PERMUTE_UZP1; break;
        case 0x1bu: out->subop = PVIR_PERMUTE_UZP2; break;
        case 0x1cu: out->subop = PVIR_PERMUTE_TRN1; break;
        default:    out->subop = PVIR_PERMUTE_TRN2; break;
        }
        init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    }
}

static void decode_index(uint32_t insn, union a64_u *arg)
{
    arg_sve_general *out = &arg->f_sve_general;
    out->zd = field(insn, 0, 5);
    out->element_bits = 8u << field(insn, 22, 2);
    out->src1_is_imm = field(insn, 10, 1) == 0;
    out->src2_is_imm = field(insn, 11, 1) == 0;
    if (out->src1_is_imm) {
        out->imm1 = sign_extend_n(field(insn, 5, 5), 5);
    } else {
        out->zn = field(insn, 5, 5);
    }
    if (out->src2_is_imm) {
        out->imm2 = sign_extend_n(field(insn, 16, 5), 5);
    } else {
        out->zm = field(insn, 16, 5);
    }
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_wmul(uint32_t insn, union a64_u *arg)
{
    arg_sve_general *out = &arg->f_sve_general;
    if ((insn & 0xff20fc00u) == 0x45007000u ||
        (insn & 0xff20f400u) == 0x45007000u) {
        /* SVE2 SMULLB/T / UMULLB/T: bits[23:22]=dst size, bit[11]=U, bit[10]=T */
        out->zd = field(insn, 0, 5);
        out->zn = field(insn, 5, 5);
        out->zm = field(insn, 16, 5);
        out->element_bits = 8u << field(insn, 22, 2);
        out->is_signed = field(insn, 11, 1) == 0;
        out->top_half = field(insn, 10, 1) != 0;
        out->subop = SVE_WMUL_MULL;
        init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    } else {
        /* Predicated integer MLA / MLS (Zda += Zn * Zm): bits[15:13] = 0b010(MLA) / 0b011(MLS) */
        out->zd = field(insn, 0, 5);
        out->zn = field(insn, 5, 5);
        out->zm = field(insn, 16, 5);
        out->pg = field(insn, 10, 3);
        out->element_bits = 8u << field(insn, 22, 2);
        out->predicated = true;
        out->subop = field(insn, 13, 1) ? SVE_WMUL_MLS : SVE_WMUL_MLA;
        init_effects(&out->effects, PVIR_INACTIVE_MERGE);
    }
}

static void decode_imm_bin(uint32_t insn, union a64_u *arg)
{
    arg_sve_general *out = &arg->f_sve_general;
    out->zd = field(insn, 0, 5);
    out->zn = out->zd;
    if ((insn & 0xff000000u) == 0x05000000u) {
        /* AND / ORR / EOR imm13 */
        uint8_t eb = 8;
        unsigned opc = field(insn, 22, 2);
        out->mask_imm = decode_sve_bitmask_imm(insn, &eb);
        out->element_bits = eb;
        out->subop = (opc == 0u) ? SVE_IMM_BIN_ORR
                                 : (opc == 1u ? SVE_IMM_BIN_EOR : SVE_IMM_BIN_AND);
    } else {
        /* 0x2520c000: add/sub/subr/mul/dup imm */
        unsigned opc = field(insn, 16, 5);
        out->element_bits = 8u << field(insn, 22, 2);
        out->src2_is_imm = true;
        if (opc == 0x18u) {
            /* DUP imm8 */
            int64_t imm = sign_extend_n(field(insn, 5, 8), 8);
            if (field(insn, 13, 1)) imm <<= 8;
            out->imm1 = imm;
            out->subop = SVE_IMM_BIN_DUP;
        } else if (opc == 0x10u) {
            /* MUL imm8 */
            out->imm1 = sign_extend_n(field(insn, 5, 8), 8);
            out->subop = SVE_IMM_BIN_MUL;
        } else {
            uint64_t imm = field(insn, 5, 8);
            if (field(insn, 13, 1)) imm <<= 8;
            out->imm1 = (int64_t)imm;
            if (opc == 0u) out->subop = SVE_IMM_BIN_ADD;
            else if (opc == 1u) out->subop = SVE_IMM_BIN_SUB;
            else out->subop = SVE_IMM_BIN_SUBR;
        }
    }
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_icmp(uint32_t insn, union a64_u *arg)
{
    arg_sve_ext_op *out = &arg->f_sve_ext_op;
    out->zd_pd = field(insn, 0, 4);
    out->zn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->src_bits = 8u << field(insn, 22, 2);
    out->dst_bits = out->src_bits;
    out->predicated = true;
    out->is_float = false;
    if ((insn & 0xff200000u) == 0x24200000u) {
        /* Unsigned vector-imm7: cmphs/cmphi/cmpls/cmplo */
        bool ls_lo = field(insn, 13, 1) != 0;
        bool nbit = field(insn, 4, 1) != 0;
        out->cmp_with_imm = true;
        out->imm = (int64_t)field(insn, 14, 7);
        if (!ls_lo) {
            out->subop = nbit ? PVIR_COMPARE_GTU : PVIR_COMPARE_GEU;
        } else {
            out->subop = nbit ? PVIR_COMPARE_LEU : PVIR_COMPARE_LTU;
        }
    } else if ((insn & 0xff000000u) == 0x24000000u) {
        /* Vector-vector: sel = (bit15<<2)|(bit13<<1)|bit4 */
        unsigned sel = (field(insn, 15, 1) << 2) | (field(insn, 13, 1) << 1) |
                       field(insn, 4, 1);
        out->zm = field(insn, 16, 5);
        out->cmp_with_imm = false;
        switch (sel) {
        case 0: out->subop = PVIR_COMPARE_GEU; break; /* HS */
        case 1: out->subop = PVIR_COMPARE_GTU; break; /* HI */
        case 4: out->subop = PVIR_COMPARE_GE; break;  /* GE */
        case 5: out->subop = PVIR_COMPARE_GT; break;  /* GT */
        case 6: out->subop = PVIR_COMPARE_EQ; break;  /* EQ */
        case 7: out->subop = PVIR_COMPARE_NE; break;  /* NE */
        default: out->subop = PVIR_COMPARE_EQ; break;
        }
    } else {
        /* Signed vector-imm5: 0x25000000 (GE/GT), 0x25002000 (LT/LE), 0x25008000 (EQ/NE) */
        unsigned op3 = field(insn, 13, 3);
        bool nbit = field(insn, 4, 1) != 0;
        out->cmp_with_imm = true;
        out->imm = sign_extend_n(field(insn, 16, 5), 5);
        if (op3 == 4u) {
            out->subop = nbit ? PVIR_COMPARE_NE : PVIR_COMPARE_EQ;
        } else if (op3 == 0u) {
            out->subop = nbit ? PVIR_COMPARE_GT : PVIR_COMPARE_GE;
        } else {
            out->subop = nbit ? PVIR_COMPARE_LE : PVIR_COMPARE_LT;
        }
    }
    init_effects(&out->effects, PVIR_INACTIVE_ZERO);
}

static void decode_sel(uint32_t insn, union a64_u *arg)
{
    arg_sve_integer_binary *out = &arg->f_sve_and;
    out->zd = field(insn, 0, 5);
    out->zm = field(insn, 5, 5);
    /* Wait: SEL z0.s, p1, z2.s, z3.s = 0x05a3c440 -> zm=3 at bits[20:16], pg=1 at bits[13:10], zn=2 at bits[9:5], zd=0 at bits[4:0]! */
    out->pg = field(insn, 10, 3);
    out->element_bits = 8u << field(insn, 22, 2);
    out->is_signed = (bool)field(insn, 16, 5); /* carry zm in bits[20:16], zn in zm temporarily below or use arg_sve_general */
    arg_sve_general *g = &arg->f_sve_general;
    g->zd = field(insn, 0, 5);
    g->zn = field(insn, 5, 5);
    g->pg = field(insn, 10, 3);
    g->zm = field(insn, 16, 5);
    g->element_bits = 8u << field(insn, 22, 2);
    g->predicated = true;
    init_effects(&g->effects, PVIR_INACTIVE_MERGE);
}

static void decode_idiv(uint32_t insn, union a64_u *arg)
{
    arg_sve_general *out = &arg->f_sve_general;
    unsigned op = field(insn, 16, 3); /* sdiv=4, udiv=5, sdivr=6, udivr=7 */
    out->zd = field(insn, 0, 5);
    out->zn = out->zd;
    out->zm = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->element_bits = 8u << field(insn, 22, 2);
    out->predicated = true;
    out->subop = (op - 4u);
    out->is_signed = (op == 4u || op == 6u);
    init_effects(&out->effects, PVIR_INACTIVE_MERGE);
}

static bool valid_gather_64_only(uint32_t insn)
{
    return (insn & 0x00400000u) != 0 || field(insn, 13, 3) != 0;
}

static bool valid_ireduce(uint32_t insn)
{
    return field(insn, 22, 2) != 3u;
}

/*
 * Encoding masks and field layouts follow Arm ARM DDI 0487, Part C, Chapter
 * C8 SVE instruction descriptions. Variable fields are deliberately confined
 * to this table and the extractors above; the legacy A64 decoder never probes
 * SVE bits itself.
 */
static const SveDecodePattern sve_patterns[] = {
    /* C8.2 PTRUE (predicate): Pd, pattern, size. */
    { 0xff3ffc10u, 0x2518e000u, AARCH64_A64_SVE_PTRUE, decode_ptrue, NULL },
    /* PTRUES differs only in bit 16 (llvm-mc: ptrue 0x2518e3e0, ptrues
     * 0x2519e3e0); it writes PSTATE.NZCV from the PredTest result. */
    { 0xff3ffc10u, 0x2519e000u, AARCH64_A64_SVE_PTRUE, decode_ptrues, NULL },
    /* PFALSE: llvm-mc 0x2518e400 (p0) / 0x2518e403 (p3); Pd = bits[4:0]. */
    { 0xffffffe0u, 0x2518e400u, AARCH64_A64_SVE_PFALSE, decode_pfalse, NULL },
    /* C8.2 WHILELO: X operands; size bits select width (Pd, Rn, Rm). */
    { 0xff20fc10u, 0x25201c00u, AARCH64_A64_SVE_WHILELO, decode_whilelo, NULL },
    /* C8.2 ADD (vectors, predicated): Zdn, Zm, Pg, size; /M only. */
    { 0xff3fe000u, 0x04000000u, AARCH64_A64_SVE_ADD, decode_add, NULL },
    /* C8.2.778 SUB (vectors, predicated): Zdn, Zm, Pg, size; /M only. */
    { 0xff3fe000u, 0x04010000u, AARCH64_A64_SVE_SUB, decode_sub, NULL },
    /* C8.2 MUL (vectors, predicated): Zdn, Zm, Pg, size; /M only. */
    { 0xff3fe000u, 0x04100000u, AARCH64_A64_SVE_MUL, decode_mul, NULL },
    /* ORR (vectors, predicated): Zdn, Zm, Pg, size; /M only. */
    { 0xff3fe000u, 0x04180000u, AARCH64_A64_SVE_ORR, decode_orr, NULL },
    /* AND (vectors, predicated): Zdn, Zm, Pg, size; /M only. */
    { 0xff3fe000u, 0x041a0000u, AARCH64_A64_SVE_AND, decode_and, NULL },
    /* EOR (vectors, predicated): Zdn, Zm, Pg, size; /M only. */
    { 0xff3fe000u, 0x04190000u, AARCH64_A64_SVE_EOR, decode_eor, NULL },
    /* LSL (vectors, predicated): Zdn, Zm, Pg, size; /M only. */
    { 0xff3fe000u, 0x04138000u, AARCH64_A64_SVE_LSL, decode_lsl, NULL },
    /* LSR (vectors, predicated): Zdn, Zm, Pg, size; /M only. */
    { 0xff3fe000u, 0x04118000u, AARCH64_A64_SVE_LSR, decode_lsr, NULL },
    /* ASR (vectors, predicated): Zdn, Zm, Pg, size; /M only. */
    { 0xff3fe000u, 0x04108000u, AARCH64_A64_SVE_ASR, decode_asr, NULL },
    /* SMAX/UMAX/SMIN/UMIN (vectors, predicated): Zdn, Zm, Pg, size; /M only. */
    { 0xff3fe000u, 0x04080000u, AARCH64_A64_SVE_SMAX, decode_smax, NULL },
    { 0xff3fe000u, 0x04090000u, AARCH64_A64_SVE_UMAX, decode_umax, NULL },
    { 0xff3fe000u, 0x040a0000u, AARCH64_A64_SVE_SMIN, decode_smin, NULL },
    { 0xff3fe000u, 0x040b0000u, AARCH64_A64_SVE_UMIN, decode_umin, NULL },
    /*
     * C8.2 FADD/FSUB/FMUL/FDIV (vectors, predicated, FP). The exact encoding
     * was produced by llvm-mc 23.1.6 (-mattr=+sve); fields are Zdn[4:0],
     * Zm[9:5], Pg[12:10], opc[19:16] (FADD 0, FSUB 1, FMUL 2, FDIV 13),
     * size[23:22] (S=0b10, D=0b11). Every supported op/size pair is its own
     * entry so no unmodelled variant (e.g. .H) is ever claimed here; such
     * words fall through to the legacy decoder exactly as before.
     */
    { 0xffffe000u, 0x65808000u, AARCH64_A64_SVE_FADD, decode_fadd, NULL },
    { 0xffffe000u, 0x65c08000u, AARCH64_A64_SVE_FADD, decode_fadd, NULL },
    { 0xffffe000u, 0x65818000u, AARCH64_A64_SVE_FSUB, decode_fsub, NULL },
    { 0xffffe000u, 0x65c18000u, AARCH64_A64_SVE_FSUB, decode_fsub, NULL },
    { 0xffffe000u, 0x65828000u, AARCH64_A64_SVE_FMUL, decode_fmul, NULL },
    { 0xffffe000u, 0x65c28000u, AARCH64_A64_SVE_FMUL, decode_fmul, NULL },
    { 0xffffe000u, 0x658d8000u, AARCH64_A64_SVE_FDIV, decode_fdiv, NULL },
    { 0xffffe000u, 0x65cd8000u, AARCH64_A64_SVE_FDIV, decode_fdiv, NULL },
    /*
     * C8.2 FABS/FNEG (predicated): a pure sign-bit operation, so no FP status
     * is updated. Words from llvm-mc 23.1.6: fabs .s/.d = 0x049ca020/0x04dca020,
     * fneg .s/.d = 0x049da020/0x04dda020; zd[4:0], Zn[9:5], Pg[12:10],
     * size[23:22] (S=10, D=11), bit16 selects NEG. `.H` is not registered and
     * therefore fails closed through the 0x04 primary-prefix guard.
     */
    { 0xffffe000u, 0x049ca000u, AARCH64_A64_SVE_FABS, decode_fabs, NULL },
    { 0xffffe000u, 0x04dca000u, AARCH64_A64_SVE_FABS, decode_fabs, NULL },
    { 0xffffe000u, 0x049da000u, AARCH64_A64_SVE_FNEG, decode_fneg, NULL },
    { 0xffffe000u, 0x04dda000u, AARCH64_A64_SVE_FNEG, decode_fneg, NULL },
    /* C8.2 NOT (predicated): llvm-mc 0x041ea020/0x045ea020/0x049ea020/
     * 0x04dea020 for .b/.h/.s/.d; size stays in bits[22:21]. */
    { 0xffffe000u, 0x041ea000u, AARCH64_A64_SVE_NOT, decode_not, NULL },
    { 0xffffe000u, 0x045ea000u, AARCH64_A64_SVE_NOT, decode_not, NULL },
    { 0xffffe000u, 0x049ea000u, AARCH64_A64_SVE_NOT, decode_not, NULL },
    { 0xffffe000u, 0x04dea000u, AARCH64_A64_SVE_NOT, decode_not, NULL },
    /* DUP/MOV (scalar to vector): llvm-mc 0x05203820/0x05603820/0x05a03820/
     * 0x05e03820 for .b/.h/.s/.d; Zd is bits[4:0] and the GPR is bits[9:5]. */
    { 0xffffe000u, 0x05202000u, AARCH64_A64_SVE_DUP, decode_dup, NULL },
    { 0xffffe000u, 0x05602000u, AARCH64_A64_SVE_DUP, decode_dup, NULL },
    { 0xffffe000u, 0x05a02000u, AARCH64_A64_SVE_DUP, decode_dup, NULL },
    { 0xffffe000u, 0x05e02000u, AARCH64_A64_SVE_DUP, decode_dup, NULL },
    /* C8.2 LD1B/ST1B, .B scalar plus immediate, single register. */
    { 0xfff0e000u, 0xa400a000u, AARCH64_A64_SVE_LD1B, decode_ld1b, NULL },
    { 0xfff0e000u, 0xe400e000u, AARCH64_A64_SVE_ST1B, decode_st1b, NULL },
    /*
     * Same single-register contiguous forms at wider memory element sizes.
     * Words from llvm-mc 23.1.6: ld1h 0xa4a0a000, ld1w 0xa540a000,
     * ld1d 0xa5e0a000, st1h 0xe4a0e000, st1w 0xe540e000, st1d 0xe5e0e000.
     * The vector element size still comes from bits[22:21]; these entries pin
     * the memory size only, so e.g. `ld1w z0.d` (0xa560a000) is not claimed.
     */
    { 0xfff0e000u, 0xa4a0a000u, AARCH64_A64_SVE_LD1H, decode_ld1h, NULL },
    { 0xfff0e000u, 0xa540a000u, AARCH64_A64_SVE_LD1W, decode_ld1w, NULL },
    { 0xfff0e000u, 0xa5e0a000u, AARCH64_A64_SVE_LD1D, decode_ld1d, NULL },
    { 0xfff0e000u, 0xe4a0e000u, AARCH64_A64_SVE_ST1H, decode_st1h, NULL },
    { 0xfff0e000u, 0xe540e000u, AARCH64_A64_SVE_ST1W, decode_st1w, NULL },
    { 0xfff0e000u, 0xe5e0e000u, AARCH64_A64_SVE_ST1D, decode_st1d, NULL },
    /* Sign-extending loads: ld1sb 0xa5c0a000/0xa5a0a000/0xa580a000 (.h/.s/.d),
     * ld1sh 0xa520a000/0xa500a000 (.s/.d), ld1sw 0xa480a000 (.d). */
    { 0xffffe000u, 0xa5c0a000u, AARCH64_A64_SVE_LD1SB, decode_ld1sb_h, NULL },
    { 0xffffe000u, 0xa5a0a000u, AARCH64_A64_SVE_LD1SB, decode_ld1sb_s, NULL },
    { 0xffffe000u, 0xa580a000u, AARCH64_A64_SVE_LD1SB, decode_ld1sb_d, NULL },
    { 0xffffe000u, 0xa520a000u, AARCH64_A64_SVE_LD1SH, decode_ld1sh_s, NULL },
    { 0xffffe000u, 0xa500a000u, AARCH64_A64_SVE_LD1SH, decode_ld1sh_d, NULL },
    { 0xffffe000u, 0xa480a000u, AARCH64_A64_SVE_LD1SW, decode_ld1sw_d, NULL },
    /* Zero-extending widening loads: ld1b 0xa420a000/0xa440a000/0xa460a000
     * (.h/.s/.d), ld1h 0xa4c0a000/0xa4e0a000 (.s/.d), ld1w 0xa560a000 (.d). */
    { 0xffffe000u, 0xa420a000u, AARCH64_A64_SVE_LD1B, decode_ld1b_h, NULL },
    { 0xffffe000u, 0xa440a000u, AARCH64_A64_SVE_LD1B, decode_ld1b_s, NULL },
    { 0xffffe000u, 0xa460a000u, AARCH64_A64_SVE_LD1B, decode_ld1b_d, NULL },
    { 0xffffe000u, 0xa4c0a000u, AARCH64_A64_SVE_LD1H, decode_ld1h_s, NULL },
    { 0xffffe000u, 0xa4e0a000u, AARCH64_A64_SVE_LD1H, decode_ld1h_d, NULL },
    { 0xffffe000u, 0xa560a000u, AARCH64_A64_SVE_LD1W, decode_ld1w_d, NULL },
    /* C8.2 SETFFR: fixed encoding. */
    { 0xffffffffu, 0x252c9000u, AARCH64_A64_SVE_SETFFR, decode_setffr, NULL },
    /* C8.2 RDFFR (unpredicated): Pd. */
    { 0xfffffff0u, 0x2519f000u, AARCH64_A64_SVE_RDFFR, decode_rdffr, NULL },
    /* WRFFR <Pn>.B (0x25289000 + Pn[8:5]) */
    { 0xfffffe1fu, 0x25289000u, AARCH64_A64_SVE_WRFFR, decode_wrffr, NULL },
    /* Predicated RDFFR/RDFFRS <Pd>.B, <Pg>/Z (0x2518f000 / 0x2558f000) */
    { 0xffbff010u, 0x2518f000u, AARCH64_A64_SVE_RDFFR, decode_rdffr_pred, NULL },
    /* C8.2 LDFF1* (scalar+scalar / [Xn], rm=31) */
    { 0xffe0e000u, 0xa4006000u, AARCH64_A64_SVE_LDFF1B, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa4206000u, AARCH64_A64_SVE_LDFF1B, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa4406000u, AARCH64_A64_SVE_LDFF1B, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa4606000u, AARCH64_A64_SVE_LDFF1B, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa4a06000u, AARCH64_A64_SVE_LDFF1H, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa4c06000u, AARCH64_A64_SVE_LDFF1H, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa4e06000u, AARCH64_A64_SVE_LDFF1H, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa5406000u, AARCH64_A64_SVE_LDFF1W, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa5606000u, AARCH64_A64_SVE_LDFF1W, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa5e06000u, AARCH64_A64_SVE_LDFF1D, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa5c06000u, AARCH64_A64_SVE_LDFF1SB, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa5a06000u, AARCH64_A64_SVE_LDFF1SB, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa5806000u, AARCH64_A64_SVE_LDFF1SB, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa5206000u, AARCH64_A64_SVE_LDFF1SH, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa5006000u, AARCH64_A64_SVE_LDFF1SH, decode_ldff1_common, NULL },
    { 0xffe0e000u, 0xa4806000u, AARCH64_A64_SVE_LDFF1SW, decode_ldff1_common, NULL },
    /* C8.2 LDNF1* (scalar+imm4*VL) */
    { 0xfff0e000u, 0xa410a000u, AARCH64_A64_SVE_LDNF1B, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa430a000u, AARCH64_A64_SVE_LDNF1B, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa450a000u, AARCH64_A64_SVE_LDNF1B, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa470a000u, AARCH64_A64_SVE_LDNF1B, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa4b0a000u, AARCH64_A64_SVE_LDNF1H, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa4d0a000u, AARCH64_A64_SVE_LDNF1H, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa4f0a000u, AARCH64_A64_SVE_LDNF1H, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa550a000u, AARCH64_A64_SVE_LDNF1W, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa570a000u, AARCH64_A64_SVE_LDNF1W, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa5f0a000u, AARCH64_A64_SVE_LDNF1D, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa5d0a000u, AARCH64_A64_SVE_LDNF1SB, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa5b0a000u, AARCH64_A64_SVE_LDNF1SB, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa590a000u, AARCH64_A64_SVE_LDNF1SB, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa530a000u, AARCH64_A64_SVE_LDNF1SH, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa510a000u, AARCH64_A64_SVE_LDNF1SH, decode_ldnf1_common, NULL },
    { 0xfff0e000u, 0xa490a000u, AARCH64_A64_SVE_LDNF1SW, decode_ldnf1_common, NULL },
    /* COMPACT .S/.D */
    { 0xffbfe000u, 0x05a18000u, AARCH64_A64_SVE_COMPACT, decode_compact, NULL },
    /* Predicate family: PTEST, WHILE*, CNTP, CNT/INC/DEC, BRKA/B/AS/BS, PFIRST, PNEXT, LAST, PRED logical */
    { 0xffffc010u, 0x2550c000u, AARCH64_A64_SVE_PTEST, decode_ptest, NULL },
    { 0xff20ec10u, 0x25200400u, AARCH64_A64_SVE_WHILE, decode_whilelt, NULL },
    { 0xff20ec10u, 0x25200410u, AARCH64_A64_SVE_WHILE, decode_whilele, NULL },
    { 0xff20ec10u, 0x25200c10u, AARCH64_A64_SVE_WHILE, decode_whilels, NULL },
    { 0xff20ec10u, 0x25200000u, AARCH64_A64_SVE_WHILE, decode_whilege, NULL },
    { 0xff20ec10u, 0x25200010u, AARCH64_A64_SVE_WHILE, decode_whilegt, NULL },
    { 0xff20ec10u, 0x25200800u, AARCH64_A64_SVE_WHILE, decode_whilehs, NULL },
    { 0xff20ec10u, 0x25200810u, AARCH64_A64_SVE_WHILE, decode_whilehi, NULL },
    { 0xff20ec10u, 0x25202010u, AARCH64_A64_SVE_WHILE, decode_whilerw, NULL },
    { 0xff20ec10u, 0x25202000u, AARCH64_A64_SVE_WHILE, decode_whilewr, NULL },
    { 0xff3fc000u, 0x25208000u, AARCH64_A64_SVE_CNTP, decode_cntp, NULL },
    { 0xff20e800u, 0x0420e000u, AARCH64_A64_SVE_CNT_SCALAR, decode_cnt_scalar, NULL },
    { 0xff20f000u, 0x0420f000u, AARCH64_A64_SVE_CNT_SCALAR, decode_cnt_scalar, NULL },
    { 0xff3fc000u, 0x25104000u, AARCH64_A64_SVE_BRK, decode_brk, NULL },
    { 0xff3fc000u, 0x25504000u, AARCH64_A64_SVE_BRK, decode_brk, NULL },
    { 0xff3fc000u, 0x25904000u, AARCH64_A64_SVE_BRK, decode_brk, NULL },
    { 0xff3fc000u, 0x25d04000u, AARCH64_A64_SVE_BRK, decode_brk, NULL },
    { 0xfffffe10u, 0x2558c000u, AARCH64_A64_SVE_PFIRST, decode_pfirst, NULL },
    { 0xff3ffe10u, 0x2519c400u, AARCH64_A64_SVE_PNEXT, decode_pnext, NULL },
    { 0xff2ee000u, 0x0520a000u, AARCH64_A64_SVE_LAST, decode_last, NULL },
    { 0xff36e000u, 0x05228000u, AARCH64_A64_SVE_LAST, decode_last, NULL },
    { 0xff3ee000u, 0x05288000u, AARCH64_A64_SVE_LAST, decode_last, NULL },
    { 0xff30c000u, 0x25004000u, AARCH64_A64_SVE_PRED_LOGICAL, decode_pred_logical, NULL },
    /* SVE Memory: LD1 scalar+scalar, LDN/STN, LD1R, LD1RQ, Gather/Scatter, LDNT1/STNT1 */
    { 0xffe0e000u, 0xa4004000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa4204000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa4404000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa4604000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa4804000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa4a04000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa4c04000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa4e04000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa5004000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa5204000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa5404000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa5604000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa5804000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa5a04000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa5c04000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xffe0e000u, 0xa5e04000u, AARCH64_A64_SVE_LD1_SCALAR, decode_ld1_scalar, valid_not_rm31 },
    { 0xff80e000u, 0xe4004000u, AARCH64_A64_SVE_ST1_SCALAR, decode_st1_scalar, valid_not_rm31 },
    { 0xff80e000u, 0xe4804000u, AARCH64_A64_SVE_ST1_SCALAR, decode_st1_scalar, valid_not_rm31 },
    { 0xff80e000u, 0xe5004000u, AARCH64_A64_SVE_ST1_SCALAR, decode_st1_scalar, valid_not_rm31 },
    { 0xff80e000u, 0xe5804000u, AARCH64_A64_SVE_ST1_SCALAR, decode_st1_scalar, valid_not_rm31 },
    /* LD2/LD3/LD4 imm & reg (0xa420e000..0xa5e0c000 where nreg_idx = bits[22:21] != 0) */
    { 0xfe70e000u, 0xa420e000u, AARCH64_A64_SVE_LDN, decode_ldn_stn, NULL },
    { 0xfe70e000u, 0xa440e000u, AARCH64_A64_SVE_LDN, decode_ldn_stn, NULL },
    { 0xfe70e000u, 0xa460e000u, AARCH64_A64_SVE_LDN, decode_ldn_stn, NULL },
    { 0xfe00e000u, 0xa420c000u, AARCH64_A64_SVE_LDN, decode_ldn_stn, valid_not_rm31 },
    { 0xfe00e000u, 0xa440c000u, AARCH64_A64_SVE_LDN, decode_ldn_stn, valid_not_rm31 },
    { 0xfe00e000u, 0xa460c000u, AARCH64_A64_SVE_LDN, decode_ldn_stn, valid_not_rm31 },
    { 0xfe70e000u, 0xe430e000u, AARCH64_A64_SVE_STN, decode_ldn_stn, NULL },
    { 0xfe70e000u, 0xe450e000u, AARCH64_A64_SVE_STN, decode_ldn_stn, NULL },
    { 0xfe70e000u, 0xe470e000u, AARCH64_A64_SVE_STN, decode_ldn_stn, NULL },
    { 0xfe00e000u, 0xe4206000u, AARCH64_A64_SVE_STN, decode_ldn_stn, valid_not_rm31 },
    { 0xfe00e000u, 0xe4406000u, AARCH64_A64_SVE_STN, decode_ldn_stn, valid_not_rm31 },
    { 0xfe00e000u, 0xe4606000u, AARCH64_A64_SVE_STN, decode_ldn_stn, valid_not_rm31 },
    /* LD1R* */
    { 0xfe008000u, 0x84408000u, AARCH64_A64_SVE_LD1R, decode_ld1r, NULL },
    { 0xfe008000u, 0x84008000u, AARCH64_A64_SVE_LD1R, decode_ld1r, valid_gather_64_only },
    /* LD1RQ* (0xa4002000..0xa5802000) */
    { 0xfe70e000u, 0xa4002000u, AARCH64_A64_SVE_LD1RQ, decode_ld1rq, NULL },
    /* Gather LD1 (32-bit/64-bit) & Scatter ST1 (32-bit/64-bit) */
    { 0xfe008000u, 0x84000000u, AARCH64_A64_SVE_GATHER_LD, decode_gather_scatter, valid_gather_64_only },
    { 0xfe008000u, 0xc4000000u, AARCH64_A64_SVE_GATHER_LD, decode_gather_scatter, valid_gather_64_only },
    { 0xfe208000u, 0x85208000u, AARCH64_A64_SVE_GATHER_LD, decode_gather_scatter, NULL },
    { 0xfe208000u, 0xc5808000u, AARCH64_A64_SVE_GATHER_LD, decode_gather_scatter, NULL },
    { 0xfe00a000u, 0xe4008000u, AARCH64_A64_SVE_SCATTER_ST, decode_gather_scatter, NULL },
    { 0xfe00a000u, 0xe5208000u, AARCH64_A64_SVE_SCATTER_ST, decode_gather_scatter, NULL },
    { 0xfe00a000u, 0xe540a000u, AARCH64_A64_SVE_SCATTER_ST, decode_gather_scatter, NULL },
    { 0xfe00a000u, 0xe580a000u, AARCH64_A64_SVE_SCATTER_ST, decode_gather_scatter, NULL },
    /* LDNT1/STNT1 scalar+imm (0xa400e000 / 0xe410e000) and scalar+scalar (0xa400c000 / 0xe4006000) */
    { 0xfe70e000u, 0xa400e000u, AARCH64_A64_SVE_LDNT1, decode_ldnt_stnt, NULL },
    { 0xfe60e000u, 0xa400c000u, AARCH64_A64_SVE_LDNT1, decode_ldnt_stnt, valid_not_rm31 },
    { 0xfe70e000u, 0xe410e000u, AARCH64_A64_SVE_STNT1, decode_ldnt_stnt, NULL },
    { 0xfe60e000u, 0xe4006000u, AARCH64_A64_SVE_STNT1, decode_ldnt_stnt, valid_not_rm31 },
    /* SVE FP: FMLA/FMLS/FMAD/FMSB/FNMLA/FNMLS/FNMAD/FNMSB */
    { 0xffa00000u, 0x65a00000u, AARCH64_A64_SVE_FMLA, decode_fmla, NULL },
    { 0xffa00000u, 0x65e00000u, AARCH64_A64_SVE_FMLA, decode_fmla, NULL },
    /* FMINNM/FMAXNM/FMIN/FMAX (.S/.D) */
    { 0xffbce000u, 0x65848000u, AARCH64_A64_SVE_FMINMAX, decode_fminmax, NULL },
    { 0xffbce000u, 0x65c48000u, AARCH64_A64_SVE_FMINMAX, decode_fminmax, NULL },
    /* FCMP (vector-vector & vector-#0.0, .S/.D) */
    { 0xff804000u, 0x65804000u, AARCH64_A64_SVE_FCMP, decode_fcmp, NULL },
    { 0xff80c000u, 0x6580c000u, AARCH64_A64_SVE_FCMP, decode_fcmp, NULL },
    { 0xffbce000u, 0x65902000u, AARCH64_A64_SVE_FCMP, decode_fcmp, NULL },
    { 0xffbce000u, 0x65912000u, AARCH64_A64_SVE_FCMP, decode_fcmp, NULL },
    { 0xffbce000u, 0x65922000u, AARCH64_A64_SVE_FCMP, decode_fcmp, NULL },
    /* FCVT/FCVTZS/FCVTZU/SCVTF/UCVTF */
    { 0xff80e000u, 0x6580a000u, AARCH64_A64_SVE_FCVT, decode_fcvt, valid_fcvt_opc },
    /* FSQRT / FRECPE / FRSQRTE / FRECPS / FRSQRTS */
    { 0xffbfe000u, 0x658da000u, AARCH64_A64_SVE_FP_MATH, decode_fp_math, NULL },
    { 0xffbfe000u, 0x65cde000u, AARCH64_A64_SVE_FP_MATH, decode_fp_math, NULL },
    { 0xffbffc00u, 0x658e3000u, AARCH64_A64_SVE_FP_MATH, decode_fp_math, NULL },
    { 0xffbffc00u, 0x658f3000u, AARCH64_A64_SVE_FP_MATH, decode_fp_math, NULL },
    { 0xffa0fc00u, 0x65801800u, AARCH64_A64_SVE_FP_MATH, decode_fp_math, NULL },
    { 0xffa0fc00u, 0x65801c00u, AARCH64_A64_SVE_FP_MATH, decode_fp_math, NULL },
    /* FP reductions: FADDV/FADDA/FMAXNMV/FMINNMV/FMAXV/FMINV */
    { 0xffb8e000u, 0x65802000u, AARCH64_A64_SVE_FREDUCE, decode_freduce, NULL },
    { 0xffbfe000u, 0x65982000u, AARCH64_A64_SVE_FREDUCE, decode_freduce, NULL },
    { 0xffbfe000u, 0x65d82000u, AARCH64_A64_SVE_FREDUCE, decode_freduce, NULL },
    /* SVE Integer reductions: UADDV/SADDV/SMAXV/UMAXV/SMINV/UMINV/ORV/EORV/ANDV */
    { 0xff3ee000u, 0x04002000u, AARCH64_A64_SVE_IREDUCE, decode_ireduce, valid_ireduce },
    { 0xff3ce000u, 0x04082000u, AARCH64_A64_SVE_IREDUCE, decode_ireduce, NULL },
    { 0xff3ce000u, 0x04182000u, AARCH64_A64_SVE_IREDUCE, decode_ireduce, NULL },
    /* Fixed/saturating/halving int */
    { 0xff20e000u, 0x44008000u, AARCH64_A64_SVE_FIXED_INT, decode_fixed_int, NULL },
    { 0xff20f000u, 0x04201000u, AARCH64_A64_SVE_FIXED_INT, decode_fixed_int, NULL },
    { 0xff3cc000u, 0x2524c000u, AARCH64_A64_SVE_FIXED_INT, decode_fixed_int, NULL },
    /* Immediate shifts */
    { 0xff20f000u, 0x04209000u, AARCH64_A64_SVE_SHIFT_IMM, decode_shift_imm, valid_shift_unpred },
    { 0xff30e000u, 0x04008000u, AARCH64_A64_SVE_SHIFT_IMM, decode_shift_imm, valid_shift_pred },
    /* Unary int: SXTB..CNOT (0x0410a000..0x041ba000), RBIT (0x05278000) */
    { 0xff30e000u, 0x0410a000u, AARCH64_A64_SVE_INT_UNARY, decode_int_unary, valid_int_unary },
    { 0xff3fe000u, 0x05278000u, AARCH64_A64_SVE_INT_UNARY, decode_int_unary, NULL },
    /* REV / REVB / REVH / REVW */
    { 0xff3ffc00u, 0x05383800u, AARCH64_A64_SVE_REV, decode_rev, NULL },
    { 0xff3ce000u, 0x05248000u, AARCH64_A64_SVE_REV, decode_rev, NULL },
    /* Permutes: TBL/TBX, ZIP/UZP/TRN, SPLICE, EXT, INSR */
    { 0xff20fc00u, 0x05202800u, AARCH64_A64_SVE_PERMUTE, decode_permute, NULL },
    { 0xff20fc00u, 0x05202c00u, AARCH64_A64_SVE_PERMUTE, decode_permute, NULL },
    { 0xff20fc00u, 0x05203000u, AARCH64_A64_SVE_PERMUTE, decode_permute, NULL },
    { 0xff20e400u, 0x05206000u, AARCH64_A64_SVE_PERMUTE, decode_permute, NULL },
    { 0xff3fe000u, 0x052c8000u, AARCH64_A64_SVE_PERMUTE, decode_permute, NULL },
    { 0xffe0e000u, 0x05200000u, AARCH64_A64_SVE_PERMUTE, decode_permute, NULL },
    { 0xff3efc00u, 0x05243800u, AARCH64_A64_SVE_PERMUTE, decode_permute, NULL },
    /* INDEX */
    { 0xff20f000u, 0x04204000u, AARCH64_A64_SVE_INDEX, decode_index, NULL },
    /* SMULLB/T / UMULLB/T and integer MLA/MLS */
    { 0xff20f000u, 0x45007000u, AARCH64_A64_SVE_WMUL, decode_wmul, NULL },
    { 0xff20c000u, 0x04004000u, AARCH64_A64_SVE_WMUL, decode_wmul, NULL },
    /* Immediate AND/ORR/EOR/ADD/SUB/SUBR/MUL/DUP */
    { 0xff3c0000u, 0x05000000u, AARCH64_A64_SVE_IMM_BIN, decode_imm_bin, NULL },
    { 0xff3c0000u, 0x05400000u, AARCH64_A64_SVE_IMM_BIN, decode_imm_bin, NULL },
    { 0xff3c0000u, 0x05800000u, AARCH64_A64_SVE_IMM_BIN, decode_imm_bin, NULL },
    { 0xff38c000u, 0x2520c000u, AARCH64_A64_SVE_IMM_BIN, decode_imm_bin, NULL },
    { 0xff3fc000u, 0x2530c000u, AARCH64_A64_SVE_IMM_BIN, decode_imm_bin, NULL },
    { 0xff3fc000u, 0x2538c000u, AARCH64_A64_SVE_IMM_BIN, decode_imm_bin, NULL },
    /* Integer compares CMPEQ..CMPLO reg & imm */
    { 0xff204000u, 0x24000000u, AARCH64_A64_SVE_ICMP, decode_icmp, valid_icmp_vec },
    { 0xff200000u, 0x24200000u, AARCH64_A64_SVE_ICMP, decode_icmp, NULL },
    { 0xff20e000u, 0x25008000u, AARCH64_A64_SVE_ICMP, decode_icmp, NULL },
    { 0xff20e000u, 0x25000000u, AARCH64_A64_SVE_ICMP, decode_icmp, NULL },
    { 0xff20e000u, 0x25002000u, AARCH64_A64_SVE_ICMP, decode_icmp, NULL },
    /* Vector SEL & SDIV/UDIV */
    { 0xff20e000u, 0x0520c000u, AARCH64_A64_SVE_SEL, decode_sel, NULL },
    { 0xff3ce000u, 0x04140000u, AARCH64_A64_SVE_IDIV, decode_idiv, NULL },
};

static bool has_sve_primary_prefix(uint32_t insn)
{
    const uint32_t prefix_mask = 0xff000000u;
    unsigned i;

    for (i = 0; i < sizeof(sve_patterns) / sizeof(sve_patterns[0]); i++) {
        const SveDecodePattern *pattern = &sve_patterns[i];

        if ((pattern->mask & prefix_mask) == prefix_mask &&
            (insn & prefix_mask) == (pattern->value & prefix_mask)) {
            return true;
        }
    }
    return false;
}

A64SVEDecodeStatus a64_sve_decode(uint32_t insn,
                                  A64SVEIR1Instruction *decoded)
{
    if (decoded == NULL) {
        return A64_SVE_DECODE_INVALID_ARGUMENT;
    }

    *decoded = (A64SVEIR1Instruction){0};
    decoded->insn_type = AARCH64_A64_NULL;

    for (unsigned i = 0;
         i < sizeof(sve_patterns) / sizeof(sve_patterns[0]); i++) {
        const SveDecodePattern *pattern = &sve_patterns[i];

        if ((insn & pattern->mask) == pattern->value &&
            (pattern->valid == NULL || pattern->valid(insn))) {
            decoded->insn_type = pattern->insn_type;
            pattern->extract(insn, &decoded->arg);
            return A64_SVE_DECODE_MATCH;
        }
    }

    if (has_sve_primary_prefix(insn)) {
        decoded->insn_type = AARCH64_A64_SVE_UNSUPPORTED_ENCODING;
        decoded->arg.f_sve_unsupported_encoding.raw_insn = insn;
        return A64_SVE_DECODE_UNSUPPORTED_ENCODING;
    }

    return A64_SVE_DECODE_NO_MATCH;
}

A64SVEAccessDisposition
a64_sve_access_disposition(bool sve_implemented, A64SVEControlState state)
{
    if (!sve_implemented) {
        return A64_SVE_ACCESS_FEATURE_ABSENT;
    }

    switch (state) {
    case A64_SVE_CONTROL_UNMODELED:
        return A64_SVE_ACCESS_CONTROL_UNSUPPORTED;
    case A64_SVE_CONTROL_ALLOWED:
        return A64_SVE_ACCESS_ALLOWED;
    case A64_SVE_CONTROL_DENIED:
        return A64_SVE_ACCESS_TRAP_REQUIRED;
    }

    return A64_SVE_ACCESS_CONTROL_UNSUPPORTED;
}

/* How many distinct encodings to keep per instruction type. One is enough to
 * prove the type exists, but types with several shapes (element sizes, subops)
 * need more before a lowering gap in one of them is visible. */
#define CORPUS_PER_TYPE 4

size_t a64_sve_decode_corpus(uint32_t *out, size_t cap)
{
    struct {
        AARCH64_INSN_TYPE_A64 type;
        unsigned count;
    } seen[256];
    size_t seen_count = 0;
    size_t written = 0;
    uint32_t lcg = 0x1234567u;

    if (out == NULL || cap == 0) {
        return 0;
    }

    for (unsigned i = 0;
         i < sizeof(sve_patterns) / sizeof(sve_patterns[0]) && written < cap;
         i++) {
        const SveDecodePattern *pattern = &sve_patterns[i];
        uint32_t free_bits = ~pattern->mask;
        A64SVEIR1Instruction decoded;

        for (unsigned attempt = 0; attempt < 512; attempt++) {
            uint32_t candidate = pattern->value;

            if (attempt != 0) {
                /* Deterministic fill of the don't-care bits: the `valid()`
                 * predicates test a handful of field encodings, so a few
                 * hundred tries cover them without a real search. */
                lcg = lcg * 1664525u + 1013904223u;
                candidate |= lcg & free_bits;
            }
            if ((candidate & pattern->mask) != pattern->value) {
                continue;
            }
            if (pattern->valid != NULL && !pattern->valid(candidate)) {
                continue;
            }
            if (a64_sve_decode(candidate, &decoded) != A64_SVE_DECODE_MATCH ||
                decoded.insn_type != pattern->insn_type) {
                continue;
            }
            size_t match = seen_count;
            for (size_t s = 0; s < seen_count; s++) {
                if (seen[s].type == decoded.insn_type) {
                    match = s;
                    break;
                }
            }
            if (match == seen_count) {
                if (seen_count >= sizeof(seen) / sizeof(seen[0])) {
                    break;
                }
                seen[seen_count].type = decoded.insn_type;
                seen[seen_count].count = 1;
                seen_count++;
            } else if (seen[match].count >= CORPUS_PER_TYPE) {
                /* Enough samples of this type already. */
                break;
            } else {
                seen[match].count++;
            }
            out[written++] = candidate;
            break;
        }
    }
    return written;
}
