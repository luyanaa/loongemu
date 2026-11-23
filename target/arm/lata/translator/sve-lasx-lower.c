#include "sve-lasx-lower.h"
#include "target/arm/lata/include/translate.h"

#include "target/arm/lata/ir1/sve-pvir.h"
#include "target/arm/lata/include/env.h"
#include "target/arm/lata/include/lata.h"
#include "target/arm/lata/include/lata-mem.h"

#include <limits.h>
#include <string.h>

extern uint32_t lata_vreg_published;

void a64_sve_trace_opt_reset_stats(void)
{
    lata_context_current()->sve_const_pred_folds = 0;
}

size_t a64_sve_trace_opt_const_pred_folds(void)
{
    return lata_context_current()->sve_const_pred_folds;
}

void a64_sve_trace_opt_set_const_pred_enabled(bool enabled)
{
    lata_context_current()->sve_const_pred_opt_enabled = enabled;
}

bool a64_sve_trace_opt_const_pred_enabled(void)
{
    return lata_context_current()->sve_const_pred_opt_enabled;
}

static bool sve_context_valid(const DisasContext *s)
{
    return s != NULL && s->sve_pvir_builder != NULL && s->vl > 0 &&
           arm_sve_vl_is_valid((uint32_t)s->vl * 8u);
}

static uint64_t mask_for_bits(uint16_t bits)
{
    return bits >= 64 ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - UINT64_C(1));
}

static int find_zreg_for_value(const A64SVEPvirBuilder *b, PvirValueId id)
{
    if (id == PVIR_NO_VALUE) {
        return -1;
    }
    if (b != NULL) {
        for (int i = 0; i < (int)A64_SVE_PVIR_NUM_ZREGS; ++i) {
            if (b->z_values[i] == id) {
                return i;
            }
        }
    }
    if (id < A64_SVE_PVIR_NUM_ZREGS) {
        return (int)id;
    }
    return -1;
}

static int find_preg_for_value(const A64SVEPvirBuilder *b, PvirValueId id)
{
    if (id == PVIR_NO_VALUE) {
        return -1;
    }
    if (b != NULL) {
        for (int i = 0; i < (int)A64_SVE_PVIR_NUM_PREGS; ++i) {
            if (b->p_values[i] == id) {
                return i;
            }
        }
    }
    if (id >= A64_SVE_PVIR_P_VALUE_BASE &&
        id < A64_SVE_PVIR_P_VALUE_BASE + A64_SVE_PVIR_NUM_PREGS) {
        return (int)(id - A64_SVE_PVIR_P_VALUE_BASE);
    }
    return -1;
}

static int find_gpr_for_value(PvirValueId id)
{
    if (id >= A64_SVE_PVIR_GPR_VALUE_BASE &&
        id < A64_SVE_PVIR_GPR_VALUE_BASE + 32u) {
        return (int)(id - A64_SVE_PVIR_GPR_VALUE_BASE);
    }
    return -1;
}

static void load_z_chunk(IR2_OPND v, int offset, size_t remaining)
{
    if (remaining >= 32u) {
        lata_env_xvld(v, offset);
    } else {
        lata_env_vld(v, offset);
    }
}

static void store_z_chunk(IR2_OPND v, int offset, size_t remaining)
{
    if (remaining >= 32u) {
        lata_env_xvst(v, offset);
    } else {
        lata_env_vst(v, offset);
    }
}

static size_t sve_pred_count_val(size_t elements, uint8_t pattern)
{
    size_t bound;

    switch (pattern) {
    case 0x00:
        bound = 1;
        while (bound <= elements / 2) {
            bound *= 2;
        }
        return elements == 0 ? 0 : bound;
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x08:
        bound = pattern;
        break;
    case 0x09:
    case 0x0a:
    case 0x0b:
    case 0x0c:
    case 0x0d:
        bound = (size_t)16 << (pattern - 0x09);
        break;
    case 0x1d:
        return elements - elements % 4;
    case 0x1e:
        return elements - elements % 3;
    case 0x1f:
        return elements;
    default:
        return 0;
    }
    return elements >= bound ? bound : 0;
}

/* Compute PSTATE.NZCV = PredTest(pg, pn, element_bits) from env predicate bytes. */
static void emit_predtest_nzcv(size_t vl_bytes, uint8_t element_bits,
                               int pg_off, int pn_off)
{
    size_t elem_bytes = element_bits / 8u;
    size_t lanes = vl_bytes / elem_bytes;
    IR2_OPND seen_first = ra_alloc_itemp();
    IR2_OPND any_active = ra_alloc_itemp();
    IR2_OPND flags = ra_alloc_itemp();
    IR2_OPND word = ra_alloc_itemp();

    li_d(seen_first, 0);
    li_d(any_active, 0);
    li_d(flags, 1LL << 29); /* N=0, Z=0, C=1, V=0 initially */

    for (size_t lane = 0; lane < lanes; ++lane) {
        size_t pbit = lane * elem_bytes;
        size_t word_byte = (pbit / 32u) * 4u;
        unsigned shift = (unsigned)(pbit % 32u);
        IR2_OPND skip_mask = ir2_opnd_new_type(IR2_OPND_LABEL);
        IR2_OPND not_first = ir2_opnd_new_type(IR2_OPND_LABEL);

        lata_env_ld_wu(word, pg_off + (int)word_byte);
        la_bstrpick_d(word, word, shift, shift);
        la_beqz(word, skip_mask);
        lata_env_ld_wu(word, pn_off + (int)word_byte);
        la_bstrpick_d(word, word, shift, shift);
        la_or(any_active, any_active, word);
        la_bnez(seen_first, not_first);
        la_bstrins_d(flags, word, 31, 31);
        li_d(seen_first, 1);
        la_label(not_first);
        la_xori(word, word, 1);
        la_bstrins_d(flags, word, 29, 29);
        la_label(skip_mask);
    }

    la_xori(any_active, any_active, 1); /* Z = !any_active */
    la_bstrins_d(flags, any_active, 30, 30);
    la_armmtflag(flags, 0x39);
    li_d(seen_first, env_offset_PSTATE());
    la_ldx_wu(word, env_ir2_opnd, seen_first);
    la_bstrpick_d(flags, flags, 31, 28);
    la_bstrins_d(word, flags, 31, 28);
    la_stx_w(word, env_ir2_opnd, seen_first);

    ra_free_temp(word);
    ra_free_temp(flags);
    ra_free_temp(any_active);
    ra_free_temp(seen_first);
}

static bool build_instruction(DisasContext *s,
                              A64SVEIR1Instruction *source,
                              PvirInstruction *instruction)
{
    if (!sve_context_valid(s)) {
        return false;
    }
    source->insn_type = (AARCH64_INSN_TYPE_A64)s->insn_type;
    source->arg = s->arg;
    return a64_sve_build_pvir(s->sve_pvir_builder, source,
                              (uint32_t)s->vl * 8u, instruction) ==
           PVIR_STATUS_OK;
}

static bool lower_predicate_constant(DisasContext *s,
                                     const PvirInstruction *instruction)
{
    const PvirPredicateGeneratorOperation *generator =
        &instruction->operation.predicate_generator;
    size_t vl_bits = (size_t)s->vl * 8u;
    size_t vl_bytes = (size_t)s->vl;
    size_t element_bits = generator->lane_type.element_bits;
    size_t element_bytes;
    size_t lanes;
    size_t active_lanes;
    size_t predicate_bytes;
    unsigned predicate_reg;

    if (instruction->opcode != PVIR_OP_PREDICATE_GENERATE ||
        generator->kind != PVIR_PREDICATE_GENERATOR_CONSTANT ||
        generator->lane_type.kind != PVIR_ELEMENT_BITS ||
        generator->lane_type.vector_bits != vl_bits ||
        (element_bits != 8 && element_bits != 16 &&
         element_bits != 32 && element_bits != 64) ||
        generator->state_id < A64_SVE_PVIR_P_STATE_BASE ||
        generator->state_id >= A64_SVE_PVIR_P_STATE_BASE +
                                   A64_SVE_PVIR_NUM_PREGS) {
        return false;
    }

    predicate_reg = generator->state_id - A64_SVE_PVIR_P_STATE_BASE;
    element_bytes = element_bits / 8;
    lanes = vl_bits / element_bits;
    switch (generator->form) {
    case PVIR_PREDICATE_ALL_TRUE:
        active_lanes = lanes;
        break;
    case PVIR_PREDICATE_ALL_FALSE:
        active_lanes = 0;
        break;
    case PVIR_PREDICATE_PREFIX:
        if (generator->active_lanes > lanes) {
            return false;
        }
        active_lanes = generator->active_lanes;
        break;
    case PVIR_PREDICATE_SUFFIX:
        if (generator->active_lanes > lanes) {
            return false;
        }
        active_lanes = lanes - generator->active_lanes;
        break;
    default:
        return false;
    }

    if (generator->status_state.present) {
        /*
         * PTRUES: PSTATE.NZCV = PredTest (DDI 0487 J1.2.530): N = FirstActive,
         * Z = NoneActive, C = !LastActive, V = 0. Every input is compile-time
         * constant here, so materialise the flags directly instead of
         * re-deriving them from the predicate bytes.
         */
        uint64_t flags_value = 0;
        IR2_OPND flags;

        if (generator->status_state.action != PVIR_ARCH_STATE_WRITE ||
            generator->status_state.state_id != A64_SVE_PVIR_STATUS_STATE) {
            return false;
        }
        if (active_lanes != 0) {
            flags_value |= UINT64_C(1) << 31;
        } else {
            flags_value |= UINT64_C(1) << 30;
        }
        if (active_lanes != lanes) {
            flags_value |= UINT64_C(1) << 29;
        }
        flags = ra_alloc_itemp();
        li_d(flags, (int64_t)flags_value);
        la_armmtflag(flags, 0x39);
        ra_free_temp(flags);
    }

    predicate_bytes = vl_bytes / 8;
    for (size_t byte = 0; byte < predicate_bytes; ++byte) {
        uint32_t packed = 0;

        for (unsigned bit = 0; bit < 8; ++bit) {
            size_t vector_byte = byte * 8 + bit;
            size_t lane;

            if (vector_byte >= vl_bytes || vector_byte % element_bytes != 0) {
                continue;
            }
            lane = vector_byte / element_bytes;
            if (generator->form == PVIR_PREDICATE_SUFFIX) {
                if (lane >= active_lanes) {
                    packed |= UINT32_C(1) << bit;
                }
            } else if (lane < active_lanes) {
                packed |= UINT32_C(1) << bit;
            }
        }

        IR2_OPND value = ra_alloc_itemp();
        li_d(value, packed);
        lata_env_st_b(value, env_offset_sve_preg(predicate_reg) + (int)byte);
        ra_free_temp(value);
    }
    return true;
}


static bool lower_ffr_set(DisasContext *s,
                          const PvirInstruction *instruction)
{
    const PvirPredicateGeneratorOperation *generator =
        &instruction->operation.predicate_generator;
    size_t predicate_bytes = (size_t)s->vl / 8u;
    int ffr_offset = env_offset_sve_ffr();
    IR2_OPND all_true;

    if (instruction->opcode != PVIR_OP_PREDICATE_GENERATE ||
        generator->kind != PVIR_PREDICATE_GENERATOR_CONSTANT ||
        generator->form != PVIR_PREDICATE_ALL_TRUE ||
        generator->active_lanes != (size_t)s->vl ||
        generator->lane_type.kind != PVIR_ELEMENT_BITS ||
        generator->lane_type.element_bits != 8 ||
        generator->lane_type.vector_bits != (size_t)s->vl * 8u ||
        generator->state_id != A64_SVE_PVIR_FFR_STATE ||
        generator->destination != s->sve_pvir_builder->ffr_value ||
        instruction->effects.arch_state.domain !=
            PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT ||
        instruction->effects.arch_state.action != PVIR_ARCH_STATE_WRITE ||
        instruction->effects.arch_state.update_kind !=
            PVIR_ARCH_STATE_UPDATE_NONE ||
        instruction->effects.arch_state.state_id !=
            A64_SVE_PVIR_FFR_STATE ||
        instruction->effects.arch_state.version_before == UINT64_MAX ||
        instruction->effects.arch_state.version_before + 1 !=
            instruction->effects.arch_state.version_after ||
        instruction->effects.arch_state.version_after !=
            s->sve_pvir_builder->ffr_version ||
        ffr_offset < 0) {
        return false;
    }

    all_true = ra_alloc_itemp();
    li_d(all_true, UINT8_MAX);
    for (size_t byte = 0; byte < predicate_bytes; ++byte) {
        lata_env_st_b(all_true, ffr_offset + (int)byte);
    }
    ra_free_temp(all_true);
    return true;
}

static bool lower_ffr_write(DisasContext *s,
                            const A64SVEIR1Instruction *source,
                            const PvirInstruction *instruction)
{
    size_t predicate_bytes = (size_t)s->vl / 8u;
    int ffr_offset = env_offset_sve_ffr();
    int pn = -1;
    int pn_offset;
    IR2_OPND packed;

    if (source != NULL && source->insn_type == AARCH64_A64_SVE_WRFFR) {
        pn = source->arg.f_sve_ffr.pn;
    } else if (instruction->opcode == PVIR_OP_ARCH_STATE &&
               instruction->effects.arch_state.domain ==
                   PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT) {
        pn = find_preg_for_value(s->sve_pvir_builder,
                                 instruction->operation.arch_state.input_value);
    }
    if (pn < 0 || pn >= (int)A64_SVE_PVIR_NUM_PREGS || ffr_offset < 0 ||
        (pn_offset = env_offset_sve_preg(pn)) < 0) {
        return false;
    }
    packed = ra_alloc_itemp();
    for (size_t byte = 0; byte < predicate_bytes; ++byte) {
        size_t word_byte = byte & ~(sizeof(uint32_t) - 1u);
        unsigned shift = (unsigned)(byte - word_byte) * 8u;

        lata_env_ld_wu(packed, pn_offset + (int)word_byte);
        la_bstrpick_d(packed, packed, shift + 7u, shift);
        lata_env_st_b(packed, ffr_offset + (int)byte);
    }
    ra_free_temp(packed);
    return true;
}

static bool lower_ffr_read(DisasContext *s,
                           const A64SVEIR1Instruction *source,
                           const PvirInstruction *instruction)
{
    const arg_sve_ffr *ffr = source != NULL ? &source->arg.f_sve_ffr : NULL;
    const PvirPredicateGeneratorOperation *generator =
        &instruction->operation.predicate_generator;
    size_t predicate_bytes = (size_t)s->vl / 8u;
    int ffr_offset = env_offset_sve_ffr();
    int pd;
    int predicate_offset;
    bool predicated = ffr != NULL ? ffr->predicated : false;
    bool setflags = ffr != NULL ? ffr->setflags : generator->status_state.present;
    int pg_offset = -1;
    IR2_OPND packed;

    if (ffr != NULL &&
        (source->insn_type != AARCH64_A64_SVE_RDFFR || !ffr->has_pd ||
         ffr->pd >= A64_SVE_PVIR_NUM_PREGS)) {
        return false;
    }
    pd = ffr != NULL ? ffr->pd
                     : (int)(generator->state_id - A64_SVE_PVIR_P_STATE_BASE);
    if (pd < 0 || pd >= (int)A64_SVE_PVIR_NUM_PREGS ||
        instruction->opcode != PVIR_OP_PREDICATE_GENERATE ||
        generator->kind != PVIR_PREDICATE_GENERATOR_STATE_READ ||
        generator->state_id != A64_SVE_PVIR_P_STATE_BASE + (uint32_t)pd ||
        generator->lane_type.kind != PVIR_ELEMENT_BITS ||
        generator->lane_type.element_bits != 8 ||
        generator->lane_type.vector_bits != (size_t)s->vl * 8u ||
        generator->source_state_value != s->sve_pvir_builder->ffr_value ||
        generator->source_state.domain !=
            PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT ||
        generator->source_state.action != PVIR_ARCH_STATE_READ ||
        ffr_offset < 0 ||
        (predicate_offset = env_offset_sve_preg(pd)) < 0) {
        return false;
    }
    if (predicated) {
        if (ffr->pg >= A64_SVE_PVIR_NUM_PREGS ||
            (pg_offset = env_offset_sve_preg(ffr->pg)) < 0) {
            return false;
        }
    }

    packed = ra_alloc_itemp();
    for (size_t byte = 0; byte < predicate_bytes; ++byte) {
        size_t word_byte = byte & ~(sizeof(uint32_t) - 1u);
        unsigned shift = (unsigned)(byte - word_byte) * 8u;

        lata_env_ld_wu(packed, ffr_offset + (int)word_byte);
        la_bstrpick_d(packed, packed, shift + 7u, shift);
        if (predicated) {
            IR2_OPND pmask = ra_alloc_itemp();
            lata_env_ld_wu(pmask, pg_offset + (int)word_byte);
            la_bstrpick_d(pmask, pmask, shift + 7u, shift);
            la_and(packed, packed, pmask);
            ra_free_temp(pmask);
        }
        lata_env_st_b(packed, predicate_offset + (int)byte);
    }
    ra_free_temp(packed);
    if (setflags && predicated) {
        emit_predtest_nzcv((size_t)s->vl, 8, pg_offset, predicate_offset);
    }
    return true;
}

static bool lower_predicate_lane_compare(
    DisasContext *s, const A64SVEIR1Instruction *source,
    const PvirInstruction *instruction)
{
    const PvirPredicateGeneratorOperation *generator =
        &instruction->operation.predicate_generator;
    const arg_sve_whilelo *whilelo =
        source != NULL ? &source->arg.f_sve_whilelo : NULL;
    size_t vl_bits = (size_t)s->vl * 8u;
    size_t vl_bytes = (size_t)s->vl;
    size_t element_bits = generator->lane_type.element_bits;
    size_t element_bytes;
    size_t predicate_bytes;
    unsigned predicate_reg;
    int predicate_offset;
    uint8_t rn;
    uint8_t rm;
    PvirCompareCondition cond = generator->compare_condition;
    bool is_conflict = generator->conflict_check;
    bool conflict_wr = generator->conflict_write_first;
    IR2_OPND source_start;
    IR2_OPND source_limit;
    IR2_OPND start;
    IR2_OPND limit;
    IR2_OPND candidate;
    IR2_OPND packed;
    IR2_OPND any_active;
    IR2_OPND predicate_base;
    IR2_OPND still_true;
    IR2_OPND first_active;
    IR2_OPND last_active;
    IR2_OPND flags;
    IR2_OPND shift_value;

    if (source != NULL &&
        source->insn_type != AARCH64_A64_SVE_WHILELO &&
        source->insn_type != AARCH64_A64_SVE_WHILE) {
        return false;
    }
    if (instruction->opcode != PVIR_OP_PREDICATE_GENERATE ||
        generator->kind != PVIR_PREDICATE_GENERATOR_LANE_COMPARE ||
        generator->lane_type.kind != PVIR_ELEMENT_BITS ||
        generator->lane_type.vector_bits != vl_bits ||
        (element_bits != 8 && element_bits != 16 &&
         element_bits != 32 && element_bits != 64) ||
        (generator->scalar_bits != 32 && generator->scalar_bits != 64) ||
        generator->form != PVIR_PREDICATE_VECTOR_MASK ||
        generator->state_id < A64_SVE_PVIR_P_STATE_BASE ||
        generator->state_id >= A64_SVE_PVIR_P_STATE_BASE +
                                   A64_SVE_PVIR_NUM_PREGS ||
        !instruction->effects.arch_state.present ||
        instruction->effects.arch_state.state_id != generator->state_id) {
        return false;
    }
    {
        int r0 = whilelo != NULL ? (int)whilelo->rn
                                 : find_gpr_for_value(generator->scalar_start);
        int r1 = whilelo != NULL ? (int)whilelo->rm
                                 : find_gpr_for_value(generator->scalar_limit);
        if (r0 < 0 || r0 >= 32 || r1 < 0 || r1 >= 32) {
            return false;
        }
        rn = (uint8_t)r0;
        rm = (uint8_t)r1;
    }

    predicate_reg = generator->state_id - A64_SVE_PVIR_P_STATE_BASE;
    predicate_offset = env_offset_sve_preg(predicate_reg);
    if (predicate_offset < 0) {
        return false;
    }
    element_bytes = element_bits / 8u;

    source_start = alloc_gpr_src(rn);
    source_limit = alloc_gpr_src(rm);
    start = ra_alloc_itemp();
    limit = ra_alloc_itemp();
    if (generator->scalar_bits == 32 && !is_conflict) {
        if (cond == PVIR_COMPARE_LT || cond == PVIR_COMPARE_LE ||
            cond == PVIR_COMPARE_GT || cond == PVIR_COMPARE_GE) {
            la_slli_w(start, source_start, 0);
            la_slli_w(limit, source_limit, 0);
        } else {
            la_mov32_zx(start, source_start);
            la_mov32_zx(limit, source_limit);
        }
    } else {
        la_mov64(start, source_start);
        la_mov64(limit, source_limit);
    }
    free_alloc_gpr(source_limit);
    free_alloc_gpr(source_start);

    if (is_conflict) {
        /*
         * WHILERW/WHILEWR: compute max conflict-free element count diff_elems.
         * WHILERW: if op0 == op1 -> all active; else |op1 - op0| / elem_bytes.
         * WHILEWR: if op1 <= op0 -> all active; else (op1 - op0) / elem_bytes.
         */
        IR2_OPND diff = ra_alloc_itemp();
        IR2_OPND alt = ra_alloc_itemp();
        IR2_OPND all_done = ir2_opnd_new_type(IR2_OPND_LABEL);
        IR2_OPND do_sub = ir2_opnd_new_type(IR2_OPND_LABEL);
        size_t lanes = vl_bytes / element_bytes;
        unsigned shift_eb = (element_bits == 8) ? 0u
                            : (element_bits == 16 ? 1u
                                                  : (element_bits == 32 ? 2u : 3u));

        li_d(diff, (int64_t)lanes);
        if (!conflict_wr) {
            la_beq(start, limit, all_done);
            la_sub_d(alt, limit, start);
            la_sub_d(diff, start, limit);
            la_sltu(candidate = ra_alloc_itemp(), start, limit);
            la_masknez(diff, diff, candidate);
            la_maskeqz(alt, alt, candidate);
            la_or(diff, diff, alt);
            ra_free_temp(candidate);
        } else {
            la_bltu(start, limit, do_sub);
            la_b(all_done);
            la_label(do_sub);
            la_sub_d(diff, limit, start);
        }
        if (shift_eb != 0) {
            la_srli_d(diff, diff, shift_eb);
        }
        la_label(all_done);
        ra_free_temp(alt);
        /* From here: limit holds diff, start = 0, cond = PVIR_COMPARE_LTU. */
        li_d(start, 0);
        la_mov64(limit, diff);
        ra_free_temp(diff);
        cond = PVIR_COMPARE_LTU;
    }

    packed = ra_alloc_itemp();
    still_true = ra_alloc_itemp();
    li_d(still_true, 1);

    predicate_bytes = vl_bytes / 8u;
    for (size_t byte = 0; byte < predicate_bytes; ++byte) {
        li_d(packed, 0);
        candidate = ra_alloc_itemp();
        for (unsigned bit = 0; bit < 8; ++bit) {
            size_t vector_byte = byte * 8u + bit;
            size_t lane;

            if (vector_byte % element_bytes != 0) {
                continue;
            }
            lane = vector_byte / element_bytes;
            int lane_delta = (cond == PVIR_COMPARE_GT ||
                              cond == PVIR_COMPARE_GE ||
                              cond == PVIR_COMPARE_GTU ||
                              cond == PVIR_COMPARE_GEU)
                                 ? -(int)lane
                                 : (int)lane;
            if (!is_conflict && generator->scalar_bits == 32) {
                la_addi_d(candidate, start, lane_delta);
                if (cond == PVIR_COMPARE_LT || cond == PVIR_COMPARE_LE ||
                    cond == PVIR_COMPARE_GT || cond == PVIR_COMPARE_GE) {
                    la_slli_w(candidate, candidate, 0);
                } else {
                    la_mov32_zx(candidate, candidate);
                }
            } else {
                la_addi_d(candidate, start, lane_delta);
            }
            switch (cond) {
            case PVIR_COMPARE_LT:
                la_slt(candidate, candidate, limit);
                break;
            case PVIR_COMPARE_LE:
                la_slt(candidate, limit, candidate);
                la_xori(candidate, candidate, 1);
                break;
            case PVIR_COMPARE_GT:
                la_slt(candidate, limit, candidate);
                break;
            case PVIR_COMPARE_GE:
                la_slt(candidate, candidate, limit);
                la_xori(candidate, candidate, 1);
                break;
            case PVIR_COMPARE_LEU:
                la_sltu(candidate, limit, candidate);
                la_xori(candidate, candidate, 1);
                break;
            case PVIR_COMPARE_GTU:
                la_sltu(candidate, limit, candidate);
                break;
            case PVIR_COMPARE_GEU:
                la_sltu(candidate, candidate, limit);
                la_xori(candidate, candidate, 1);
                break;
            case PVIR_COMPARE_LTU:
            default:
                la_sltu(candidate, candidate, limit);
                break;
            }
            la_and(still_true, still_true, candidate);
            if (bit != 0) {
                la_slli_d(candidate, still_true, bit);
            } else {
                la_mov64(candidate, still_true);
            }
            la_or(packed, packed, candidate);
        }
        ra_free_temp(candidate);
        lata_env_st_b(packed, predicate_offset + (int)byte);
    }

    ra_free_temp(still_true);
    ra_free_temp(packed);
    ra_free_temp(limit);
    ra_free_temp(start);

    first_active = ra_alloc_itemp();
    last_active = ra_alloc_itemp();
    flags = ra_alloc_itemp();
    shift_value = ra_alloc_itemp();
    lata_env_ld_wu(first_active, predicate_offset);
    la_bstrpick_d(first_active, first_active, 0, 0);
    {
        size_t last_vector_byte = vl_bytes - element_bytes;
        unsigned last_bit = (unsigned)(last_vector_byte % 8u);
        size_t last_byte = last_vector_byte / 8u;
        size_t last_word = last_byte & ~(size_t)3u;
        unsigned last_wsh = (unsigned)((last_byte - last_word) * 8u) + last_bit;

        lata_env_ld_wu(last_active, predicate_offset + (int)last_word);
        la_bstrpick_d(last_active, last_active, last_wsh, last_wsh);
    }

    any_active = flags; /* dead until assembled below */
    (void)predicate_base;
    la_xori(last_active, last_active, 1);
    li_d(flags, 0);
    la_slli_d(shift_value, first_active, 31);
    la_or(flags, flags, shift_value);
    la_xori(first_active, first_active, 1); /* prefix-monotonic => Z = !first_active */
    la_slli_d(shift_value, first_active, 30);
    la_or(flags, flags, shift_value);
    la_slli_d(shift_value, last_active, 29);
    la_or(flags, flags, shift_value);
    if (generator->status_state.present) {
        la_armmtflag(flags, 0x39);
    }
    (void)any_active;

    ra_free_temp(shift_value);
    ra_free_temp(flags);
    ra_free_temp(last_active);
    ra_free_temp(first_active);
    return true;
}

static bool lower_packed_predicate_mask(IR2_OPND mask, int predicate_offset,
                                        uint16_t element_bits)
{
    IR2_OPND packed;

    if (element_bits != 8 && element_bits != 16 &&
        element_bits != 32 && element_bits != 64) {
        return false;
    }

    la_xvxor_v(mask, mask, mask);
    packed = ra_alloc_itemp();
    lata_env_ld_wu(packed, predicate_offset);

    if (element_bits == 64) {
        for (int lane = 0; lane < 4; ++lane) {
            IR2_OPND bit = ra_alloc_itemp();
            IR2_OPND lane_mask = ra_alloc_itemp();

            la_bstrpick_d(bit, packed, lane * 8, lane * 8);
            la_sub_d(lane_mask, zero_ir2_opnd, bit);
            la_xvinsgr2vr_d(mask, lane_mask, lane);
            ra_free_temp(lane_mask);
            ra_free_temp(bit);
        }
    } else {
        unsigned element_bytes = element_bits / 8;

        for (int word_lane = 0; word_lane < 8; ++word_lane) {
            IR2_OPND word = ra_alloc_itemp();
            li_d(word, 0);

            for (unsigned byte_lane = 0; byte_lane < 4;
                 byte_lane += element_bytes) {
                int bit_index = word_lane * 4 + (int)byte_lane;
                IR2_OPND bit = ra_alloc_itemp();
                IR2_OPND lane_mask = ra_alloc_itemp();

                la_bstrpick_d(bit, packed, bit_index, bit_index);
                la_sub_d(lane_mask, zero_ir2_opnd, bit);
                la_bstrpick_d(lane_mask, lane_mask,
                              (int)element_bits - 1, 0);
                if (byte_lane != 0) {
                    la_slli_d(lane_mask, lane_mask, byte_lane * 8);
                }
                la_or(word, word, lane_mask);
                ra_free_temp(lane_mask);
                ra_free_temp(bit);
            }

            la_xvinsgr2vr_w(mask, word, word_lane);
            ra_free_temp(word);
        }
    }

    ra_free_temp(packed);
    return true;
}
static bool expanded_mask_cache_key_from_operation(
    const PvirVectorOperation *operation,
    A64SVEPredicateMaskCacheKey *key)
{
    const PvirPredicate *predicate = &operation->predicate;

    *key = (A64SVEPredicateMaskCacheKey){0};
    key->source = predicate->source;
    key->form = predicate->form;
    key->first_lane = predicate->first_lane;
    key->lanes = predicate->lanes;
    key->element_bits = operation->type.element_bits;
    key->vector_bits = operation->type.vector_bits;
    switch (predicate->form) {
    case PVIR_PREDICATE_PREFIX:
    case PVIR_PREDICATE_SUFFIX:
        key->active_lanes = predicate->value.active_lanes;
        break;
    case PVIR_PREDICATE_BITSET:
        if (predicate->value.bitset.word_count >
                A64_SVE_PVIR_MAX_MASK_KEY_WORDS ||
            (predicate->value.bitset.word_count != 0 &&
             predicate->value.bitset.words == NULL)) {
            return false;
        }
        key->bitset_word_count = predicate->value.bitset.word_count;
        for (size_t i = 0; i < key->bitset_word_count; ++i) {
            key->bitset_words[i] = predicate->value.bitset.words[i];
        }
        break;
    case PVIR_PREDICATE_ALL_TRUE:
    case PVIR_PREDICATE_ALL_FALSE:
    case PVIR_PREDICATE_VECTOR_MASK:
        break;
    default:
        return false;
    }
    key->valid = true;
    return true;
}

static bool expanded_mask_cache_keys_equal(
    const A64SVEPredicateMaskCacheKey *left,
    const A64SVEPredicateMaskCacheKey *right)
{
    if (!left->valid || !right->valid ||
        left->source.kind != right->source.kind ||
        left->source.id != right->source.id ||
        left->source.version != right->source.version ||
        left->form != right->form ||
        left->active_lanes != right->active_lanes ||
        left->bitset_word_count != right->bitset_word_count ||
        left->first_lane != right->first_lane ||
        left->lanes != right->lanes ||
        left->element_bits != right->element_bits ||
        left->vector_bits != right->vector_bits) {
        return false;
    }
    for (size_t i = 0; i < A64_SVE_PVIR_MAX_MASK_KEY_WORDS; ++i) {
        if (left->bitset_words[i] != right->bitset_words[i]) {
            return false;
        }
    }
    return true;
}


static bool is_const_fold_predicate(const DisasContext *s,
                                    const PvirPredicate *pred, uint8_t pg)
{
    if (pred == NULL) {
        return false;
    }
    if (pred->form == PVIR_PREDICATE_ALL_TRUE ||
        pred->form == PVIR_PREDICATE_ALL_FALSE ||
        pred->form == PVIR_PREDICATE_PREFIX) {
        return true;
    }
    if (!lata_context_current()->sve_const_pred_opt_enabled || s == NULL ||
        s->sve_pvir_builder == NULL || pg >= A64_SVE_PVIR_NUM_PREGS) {
        return false;
    }
    return pred->source.kind == PVIR_PREDICATE_SOURCE_PREDICATE &&
           (s->sve_pvir_builder->known_preg_valid_mask & (1u << pg)) != 0 &&
           s->sve_pvir_builder->known_preg_version[pg] == pred->source.version;
}

static bool emit_materialize_const_chunk_mask(
    DisasContext *s, const PvirPredicate *pred, uint8_t pg,
    uint16_t element_bits, size_t byte, IR2_OPND mask)
{
    size_t vl_bytes = (size_t)s->vl;
    size_t elem_bytes = element_bits / 8u;
    uint64_t words[4] = {0, 0, 0, 0};
    bool all_ones = true;
    bool all_zeros = true;

    for (unsigned q = 0; q < 4u; ++q) {
        uint64_t w = 0;
        for (unsigned b = 0; b < 8u; ++b) {
            size_t vec_byte = byte + (size_t)q * 8u + b;
            bool active = false;

            if (vec_byte < vl_bytes) {
                size_t lane = vec_byte / elem_bytes;
                if (pred->form == PVIR_PREDICATE_ALL_TRUE) {
                    active = true;
                } else if (pred->form == PVIR_PREDICATE_ALL_FALSE) {
                    active = false;
                } else if (pred->form == PVIR_PREDICATE_PREFIX) {
                    active = lane < pred->value.active_lanes;
                } else {
                    size_t elem_bit_idx = lane * elem_bytes;
                    active = ((s->sve_pvir_builder->known_preg_bytes[pg]
                                   [elem_bit_idx / 8u] >>
                               (elem_bit_idx % 8u)) & 1u) != 0;
                }
            }
            if (active) {
                w |= UINT64_C(0xff) << (b * 8u);
                all_zeros = false;
            } else {
                all_ones = false;
            }
        }
        words[q] = w;
    }
    if (all_ones) {
        la_xvseq_b(mask, mask, mask);
    } else if (all_zeros) {
        la_xvxor_v(mask, mask, mask);
    } else {
        IR2_OPND tmp = ra_alloc_itemp();
        for (unsigned q = 0; q < 4u; ++q) {
            li_d(tmp, (int64_t)words[q]);
            la_xvinsgr2vr_d(mask, tmp, (int)q);
        }
        ra_free_temp(tmp);
    }
    if (byte == 0 && pred->form == PVIR_PREDICATE_VECTOR_MASK &&
        s != NULL && s->sve_pvir_builder != NULL) {
        s->sve_pvir_builder->trace_opt_const_pred_folds++;
        lata_context_current()->sve_const_pred_folds++;
    }
    return true;
}

static bool emit_load_chunk_mask(DisasContext *s, const PvirPredicate *pred,
                                 uint8_t pg, uint16_t element_bits,
                                 size_t byte, size_t remaining,
                                 int predicate_offset,
                                 int expanded_mask_offset,
                                 bool const_fold, bool cache_hit,
                                 IR2_OPND mask)
{
    if (const_fold) {
        return emit_materialize_const_chunk_mask(s, pred, pg, element_bits,
                                                 byte, mask);
    }
    if (predicate_offset < 0 || expanded_mask_offset < 0) {
        return false;
    }
    if (cache_hit) {
        load_z_chunk(mask, expanded_mask_offset + (int)byte, remaining);
        s->sve_pvir_builder->expanded_mask_chunks_reused++;
        return true;
    }
    if (!lower_packed_predicate_mask(
            mask, predicate_offset + (int)(byte / 8u), element_bits)) {
        return false;
    }
    store_z_chunk(mask, expanded_mask_offset + (int)byte, remaining);
    s->sve_pvir_builder->expanded_mask_chunks_materialized++;
    return true;
}

static bool lower_integer_binary(
    DisasContext *s, const A64SVEIR1Instruction *source,
    const PvirInstruction *instruction)
{
    const PvirVectorOperation *operation = &instruction->operation.vector;
    const arg_sve_integer_binary *binary;
    bool subtract;
    bool multiply;
    size_t vl_bytes = (size_t)s->vl;
    int destination_offset;
    int source_offset;
    int predicate_offset;
    int expanded_mask_offset;
    bool expanded_mask_cache_hit;
    A64SVEPredicateMaskCacheKey expanded_mask_key;

    int bitwise = 0;   /* 1=OR 2=AND 3=XOR 4=SHL 5=SHR 6=SAR 7=SMAX 8=UMAX 9=SMIN 10=UMIN */
    if (source->insn_type == AARCH64_A64_SVE_ADD) {
        binary = &source->arg.f_sve_add;
        subtract = false;
        multiply = false;
    } else if (source->insn_type == AARCH64_A64_SVE_ORR) {
        binary = &source->arg.f_sve_orr;
        subtract = false;
        multiply = false;
        bitwise = 1;
    } else if (source->insn_type == AARCH64_A64_SVE_AND) {
        binary = &source->arg.f_sve_and;
        subtract = false;
        multiply = false;
        bitwise = 2;
    } else if (source->insn_type == AARCH64_A64_SVE_EOR) {
        binary = &source->arg.f_sve_eor;
        subtract = false;
        multiply = false;
        bitwise = 3;
    } else if (source->insn_type == AARCH64_A64_SVE_SMAX) {
        binary = &source->arg.f_sve_smax;
        subtract = false;
        multiply = false;
        bitwise = 7;
    } else if (source->insn_type == AARCH64_A64_SVE_UMAX) {
        binary = &source->arg.f_sve_umax;
        subtract = false;
        multiply = false;
        bitwise = 8;
    } else if (source->insn_type == AARCH64_A64_SVE_SMIN) {
        binary = &source->arg.f_sve_smin;
        subtract = false;
        multiply = false;
        bitwise = 9;
    } else if (source->insn_type == AARCH64_A64_SVE_UMIN) {
        binary = &source->arg.f_sve_umin;
        subtract = false;
        multiply = false;
        bitwise = 10;
    } else if (source->insn_type == AARCH64_A64_SVE_LSL) {
        binary = &source->arg.f_sve_lsl;
        subtract = false;
        multiply = false;
        bitwise = 4;
    } else if (source->insn_type == AARCH64_A64_SVE_LSR) {
        binary = &source->arg.f_sve_lsr;
        subtract = false;
        multiply = false;
        bitwise = 5;
    } else if (source->insn_type == AARCH64_A64_SVE_ASR) {
        binary = &source->arg.f_sve_asr;
        subtract = false;
        multiply = false;
        bitwise = 6;
    } else if (source->insn_type == AARCH64_A64_SVE_SUB) {
        binary = &source->arg.f_sve_sub;
        subtract = true;
        multiply = false;
    } else if (source->insn_type == AARCH64_A64_SVE_MUL) {
        binary = &source->arg.f_sve_mul;
        subtract = false;
        multiply = true;
    } else {
        return false;
    }

    if (instruction->opcode != (bitwise == 1 ? PVIR_OP_OR
                                         : bitwise == 2 ? PVIR_OP_AND
                                         : bitwise == 3 ? PVIR_OP_XOR
                                         : bitwise == 4 ? PVIR_OP_SHL
                                         : bitwise == 5 ? PVIR_OP_SHR
                                         : bitwise == 6 ? PVIR_OP_SAR
                                         : bitwise == 7 || bitwise == 8 ? PVIR_OP_MAX
                                         : bitwise == 9 || bitwise == 10 ? PVIR_OP_MIN
                                         : multiply ? PVIR_OP_MUL
                                         : subtract ? PVIR_OP_SUB
                                                    : PVIR_OP_ADD) ||
        (operation->type.kind != PVIR_ELEMENT_BITS &&
         operation->type.kind != PVIR_ELEMENT_INTEGER) ||
        operation->type.vector_bits != vl_bytes * 8u ||
        operation->inactive_policy != PVIR_INACTIVE_MERGE ||
        operation->merge_value == PVIR_NO_VALUE ||
        operation->merge_value != operation->operands[0] ||
        operation->predicate.form != PVIR_PREDICATE_VECTOR_MASK ||
        operation->predicate.source.kind != PVIR_PREDICATE_SOURCE_PREDICATE ||
        operation->predicate.source.id == PVIR_NO_VALUE ||
        operation->predicate.first_lane != 0 ||
        operation->type.element_bits == 0 ||
        operation->type.element_bits != binary->element_bits ||
        operation->predicate.lanes !=
            operation->type.vector_bits / operation->type.element_bits ||
        binary->zd >= A64_SVE_PVIR_NUM_ZREGS ||
        binary->zm >= A64_SVE_PVIR_NUM_ZREGS ||
        binary->pg >= A64_SVE_PVIR_NUM_PREGS) {
        return false;
    }
    expanded_mask_offset = env_offset_sve_expanded_mask_scratch();
    if (expanded_mask_offset < 0 ||
        !expanded_mask_cache_key_from_operation(operation,
                                                &expanded_mask_key)) {
        return false;
    }
    expanded_mask_cache_hit = expanded_mask_cache_keys_equal(
        &s->sve_pvir_builder->expanded_mask_key, &expanded_mask_key);
    bool const_pred_fold =
        is_const_fold_predicate(s, &operation->predicate, binary->pg);

    destination_offset = env_offset_zreg(binary->zd);
    source_offset = env_offset_zreg(binary->zm);
    predicate_offset = env_offset_sve_preg(binary->pg);
    if (destination_offset < 0 || source_offset < 0 || predicate_offset < 0) {
        return false;
    }

    /* LASX uses 32-byte chunks; each advances four packed predicate bytes. */
    for (size_t byte = 0; byte < vl_bytes; byte += 32) {
        IR2_OPND old_value = ra_alloc_ftemp();
        IR2_OPND source_value = ra_alloc_ftemp();
        IR2_OPND result_value = ra_alloc_ftemp();
        IR2_OPND mask = ra_alloc_ftemp();
        IR2_OPND result = ra_alloc_ftemp();
        uint16_t element_bits = operation->type.element_bits;
        size_t remaining = vl_bytes - byte;

        lata_env_xvld(old_value, destination_offset + (int)byte);
        lata_env_xvld(source_value, source_offset + (int)byte);
        switch (element_bits) {
        case 8:
            if (bitwise) {
                if (bitwise == 1) {
                    la_xvor_v(result_value, old_value, source_value);
                } else if (bitwise == 2) {
                    la_xvand_v(result_value, old_value, source_value);
                } else if (bitwise == 3) {
                    la_xvxor_v(result_value, old_value, source_value);
                } else if (bitwise == 4) {
                    la_xvsll_b(result_value, old_value, source_value);
                } else if (bitwise == 5) {
                    la_xvsrl_b(result_value, old_value, source_value);
                } else if (bitwise == 6) {
                    la_xvsra_b(result_value, old_value, source_value);
                } else if (bitwise == 7) {
                    la_xvmax_b(result_value, old_value, source_value);
                } else if (bitwise == 8) {
                    la_xvmax_bu(result_value, old_value, source_value);
                } else if (bitwise == 9) {
                    la_xvmin_b(result_value, old_value, source_value);
                } else {
                    la_xvmin_bu(result_value, old_value, source_value);
                }
            } else if (multiply) {
                la_xvmul_b(result_value, old_value, source_value);
            } else if (subtract) {
                la_xvsub_b(result_value, old_value, source_value);
            } else {
                la_xvadd_b(result_value, old_value, source_value);
            }
            break;
        case 16:
            if (bitwise) {
                if (bitwise == 1) {
                    la_xvor_v(result_value, old_value, source_value);
                } else if (bitwise == 2) {
                    la_xvand_v(result_value, old_value, source_value);
                } else if (bitwise == 3) {
                    la_xvxor_v(result_value, old_value, source_value);
                } else if (bitwise == 4) {
                    la_xvsll_h(result_value, old_value, source_value);
                } else if (bitwise == 5) {
                    la_xvsrl_h(result_value, old_value, source_value);
                } else if (bitwise == 6) {
                    la_xvsra_h(result_value, old_value, source_value);
                } else if (bitwise == 7) {
                    la_xvmax_h(result_value, old_value, source_value);
                } else if (bitwise == 8) {
                    la_xvmax_hu(result_value, old_value, source_value);
                } else if (bitwise == 9) {
                    la_xvmin_h(result_value, old_value, source_value);
                } else {
                    la_xvmin_hu(result_value, old_value, source_value);
                }
            } else if (multiply) {
                la_xvmul_h(result_value, old_value, source_value);
            } else if (subtract) {
                la_xvsub_h(result_value, old_value, source_value);
            } else {
                la_xvadd_h(result_value, old_value, source_value);
            }
            break;
        case 32:
            if (bitwise) {
                if (bitwise == 1) {
                    la_xvor_v(result_value, old_value, source_value);
                } else if (bitwise == 2) {
                    la_xvand_v(result_value, old_value, source_value);
                } else if (bitwise == 3) {
                    la_xvxor_v(result_value, old_value, source_value);
                } else if (bitwise == 4) {
                    la_xvsll_w(result_value, old_value, source_value);
                } else if (bitwise == 5) {
                    la_xvsrl_w(result_value, old_value, source_value);
                } else if (bitwise == 6) {
                    la_xvsra_w(result_value, old_value, source_value);
                } else if (bitwise == 7) {
                    la_xvmax_w(result_value, old_value, source_value);
                } else if (bitwise == 8) {
                    la_xvmax_wu(result_value, old_value, source_value);
                } else if (bitwise == 9) {
                    la_xvmin_w(result_value, old_value, source_value);
                } else {
                    la_xvmin_wu(result_value, old_value, source_value);
                }
            } else if (multiply) {
                la_xvmul_w(result_value, old_value, source_value);
            } else if (subtract) {
                la_xvsub_w(result_value, old_value, source_value);
            } else {
                la_xvadd_w(result_value, old_value, source_value);
            }
            break;
        case 64:
            if (bitwise) {
                if (bitwise == 1) {
                    la_xvor_v(result_value, old_value, source_value);
                } else if (bitwise == 2) {
                    la_xvand_v(result_value, old_value, source_value);
                } else if (bitwise == 3) {
                    la_xvxor_v(result_value, old_value, source_value);
                } else if (bitwise == 4) {
                    la_xvsll_d(result_value, old_value, source_value);
                } else if (bitwise == 5) {
                    la_xvsrl_d(result_value, old_value, source_value);
                } else if (bitwise == 6) {
                    la_xvsra_d(result_value, old_value, source_value);
                } else if (bitwise == 7) {
                    la_xvmax_d(result_value, old_value, source_value);
                } else if (bitwise == 8) {
                    la_xvmax_du(result_value, old_value, source_value);
                } else if (bitwise == 9) {
                    la_xvmin_d(result_value, old_value, source_value);
                } else {
                    la_xvmin_du(result_value, old_value, source_value);
                }
            } else if (multiply) {
                la_xvmul_d(result_value, old_value, source_value);
            } else if (subtract) {
                la_xvsub_d(result_value, old_value, source_value);
            } else {
                la_xvadd_d(result_value, old_value, source_value);
            }
            break;
        default:
            ra_free_temp(result);
            ra_free_temp(mask);
            ra_free_temp(result_value);
            ra_free_temp(source_value);
            ra_free_temp(old_value);
            return false;
        }
        if (!emit_load_chunk_mask(
                s, &operation->predicate, binary->pg, element_bits, byte,
                remaining, predicate_offset, expanded_mask_offset,
                const_pred_fold, expanded_mask_cache_hit, mask)) {
            ra_free_temp(result);
            ra_free_temp(mask);
            ra_free_temp(result_value);
            ra_free_temp(source_value);
            ra_free_temp(old_value);
            return false;
        }
        la_xvbitsel_v(result, old_value, result_value, mask);
        store_z_chunk(result, destination_offset + (int)byte, remaining);
        ra_free_temp(result);
        ra_free_temp(mask);
        ra_free_temp(result_value);
        ra_free_temp(source_value);
        ra_free_temp(old_value);
    }
    if (!const_pred_fold && !expanded_mask_cache_hit) {
        s->sve_pvir_builder->expanded_mask_key = expanded_mask_key;
    }
    return true;
}

static bool lower_fp_binary(DisasContext *s,
                            const A64SVEIR1Instruction *source,
                            const PvirInstruction *instruction)
{
    const PvirVectorOperation *operation = &instruction->operation.vector;
    const arg_sve_fp_binary *binary;
    PvirOpcode expected_op;
    bool divide;
    size_t vl_bytes = (size_t)s->vl;
    int destination_offset;
    int source_offset;
    int predicate_offset;
    int expanded_mask_offset;
    bool expanded_mask_cache_hit;
    A64SVEPredicateMaskCacheKey expanded_mask_key;

    switch (source->insn_type) {
    case AARCH64_A64_SVE_FADD:
        binary = &source->arg.f_sve_fadd;
        expected_op = PVIR_OP_ADD;
        divide = false;
        break;
    case AARCH64_A64_SVE_FSUB:
        binary = &source->arg.f_sve_fsub;
        expected_op = PVIR_OP_SUB;
        divide = false;
        break;
    case AARCH64_A64_SVE_FMUL:
        binary = &source->arg.f_sve_fmul;
        expected_op = PVIR_OP_MUL;
        divide = false;
        break;
    case AARCH64_A64_SVE_FDIV:
        binary = &source->arg.f_sve_fdiv;
        expected_op = PVIR_OP_DIV;
        divide = true;
        break;
    default:
        return false;
    }

    if (instruction->opcode != expected_op ||
        operation->type.kind != PVIR_ELEMENT_FLOAT ||
        operation->type.signedness != PVIR_SIGNLESS ||
        (operation->type.element_bits != 32 &&
         operation->type.element_bits != 64) ||
        operation->type.vector_bits != vl_bytes * 8u ||
        instruction->effects.fp_exceptions !=
            PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY ||
        operation->inactive_policy != PVIR_INACTIVE_MERGE ||
        operation->merge_value == PVIR_NO_VALUE ||
        operation->merge_value != operation->operands[0] ||
        operation->operand_count != 2 ||
        operation->predicate.form != PVIR_PREDICATE_VECTOR_MASK ||
        operation->predicate.source.kind != PVIR_PREDICATE_SOURCE_PREDICATE ||
        operation->predicate.source.id == PVIR_NO_VALUE ||
        operation->predicate.first_lane != 0 ||
        operation->type.element_bits != binary->element_bits ||
        operation->predicate.lanes !=
            operation->type.vector_bits / operation->type.element_bits ||
        binary->zd >= A64_SVE_PVIR_NUM_ZREGS ||
        binary->zm >= A64_SVE_PVIR_NUM_ZREGS ||
        binary->pg >= A64_SVE_PVIR_NUM_PREGS) {
        return false;
    }
    expanded_mask_offset = env_offset_sve_expanded_mask_scratch();
    if (expanded_mask_offset < 0 ||
        !expanded_mask_cache_key_from_operation(operation,
                                                &expanded_mask_key)) {
        return false;
    }
    expanded_mask_cache_hit = expanded_mask_cache_keys_equal(
        &s->sve_pvir_builder->expanded_mask_key, &expanded_mask_key);
    bool const_pred_fold =
        is_const_fold_predicate(s, &operation->predicate, binary->pg);

    destination_offset = env_offset_zreg(binary->zd);
    source_offset = env_offset_zreg(binary->zm);
    predicate_offset = env_offset_sve_preg(binary->pg);
    if (destination_offset < 0 || source_offset < 0 || predicate_offset < 0) {
        return false;
    }

    /* LASX uses 32-byte chunks; each advances four packed predicate bytes. */
    for (size_t byte = 0; byte < vl_bytes; byte += 32) {
        IR2_OPND old_value = ra_alloc_ftemp();
        IR2_OPND source_value = ra_alloc_ftemp();
        IR2_OPND result_value = ra_alloc_ftemp();
        IR2_OPND mask = ra_alloc_ftemp();
        IR2_OPND result = ra_alloc_ftemp();
        uint16_t element_bits = operation->type.element_bits;
        size_t remaining = vl_bytes - byte;
        bool ok = true;

        lata_env_xvld(old_value, destination_offset + (int)byte);
        lata_env_xvld(source_value, source_offset + (int)byte);
        ok = emit_load_chunk_mask(
            s, &operation->predicate, binary->pg, element_bits, byte, remaining,
            predicate_offset, expanded_mask_offset, const_pred_fold,
            expanded_mask_cache_hit, mask);
        /*
         * Inactive lanes must raise no FP exception. Replace both operands of
         * those lanes with exact neutral elements: 0/0 for ADD/SUB/MUL and
         * 1/1 for DIV. Every inactive lane then computes an exact result, so
         * neither IOC/DZC nor a stray IXC can appear, and the result is
         * discarded by the merge. `result` holds the neutral constant until it
         * is reused as the merge output, keeping the live vector temporaries
         * at five (the pool is six); old_value is reloaded from env for the
         * merge because the destination is not written until the end.
         */
        if (ok) {
            if (divide) {
                IR2_OPND constant = ra_alloc_itemp();

                if (element_bits == 64) {
                    li_d(constant, (int64_t)UINT64_C(0x3ff0000000000000));
                } else {
                    /* Two 1.0f lanes per 64-bit chunk. */
                    li_d(constant, (int64_t)UINT64_C(0x3f8000003f800000));
                }
                /* Seed element 0 of each 128-bit half so the broadcast
                 * leaves no uninitialised (and possibly inexact) lanes. */
                la_xvinsgr2vr_d(result, constant, 0);
                la_xvinsgr2vr_d(result, constant, 2);
                la_xvreplve0_d(result, result);
                ra_free_temp(constant);
            } else {
                la_xvxor_v(result, result, result);
            }
            /* xvbitsel(vd, vj, vk, va) = va ? vk : vj: give the constant to
             * the inactive lanes and keep the real operand when active. */
            la_xvbitsel_v(old_value, result, old_value, mask);
            la_xvbitsel_v(source_value, result, source_value, mask);
            switch (element_bits) {
            case 32:
                if (divide) {
                    la_xvfdiv_s(result_value, old_value, source_value);
                } else if (expected_op == PVIR_OP_MUL) {
                    la_xvfmul_s(result_value, old_value, source_value);
                } else if (expected_op == PVIR_OP_SUB) {
                    la_xvfsub_s(result_value, old_value, source_value);
                } else {
                    la_xvfadd_s(result_value, old_value, source_value);
                }
                break;
            case 64:
                if (divide) {
                    la_xvfdiv_d(result_value, old_value, source_value);
                } else if (expected_op == PVIR_OP_MUL) {
                    la_xvfmul_d(result_value, old_value, source_value);
                } else if (expected_op == PVIR_OP_SUB) {
                    la_xvfsub_d(result_value, old_value, source_value);
                } else {
                    la_xvfadd_d(result_value, old_value, source_value);
                }
                break;
            default:
                ok = false;
                break;
            }
        }
        if (ok) {
            /* The chunk has not been stored yet, so env still holds old Zdn. */
            lata_env_xvld(old_value, destination_offset + (int)byte);
            la_xvbitsel_v(result, old_value, result_value, mask);
            if (remaining >= 32) {
                lata_env_xvst(result, destination_offset + (int)byte);
            } else {
                /* VL is a multiple of 16 bytes; store only the active half. */
                lata_env_vst(result, destination_offset + (int)byte);
            }
        }
        ra_free_temp(result);
        ra_free_temp(mask);
        ra_free_temp(result_value);
        ra_free_temp(source_value);
        ra_free_temp(old_value);
        if (!ok) {
            return false;
        }
    }
    if (!const_pred_fold && !expanded_mask_cache_hit) {
        s->sve_pvir_builder->expanded_mask_key = expanded_mask_key;
    }
    return true;
}

static bool lower_fp_unary(DisasContext *s,
                           const A64SVEIR1Instruction *source,
                           const PvirInstruction *instruction)
{
    const PvirVectorOperation *operation = &instruction->operation.vector;
    const arg_sve_fp_unary *unary;
    PvirOpcode expected_op;
    uint64_t immediate;
    bool negate;
    size_t vl_bytes = (size_t)s->vl;
    int destination_offset;
    int source_offset;
    int predicate_offset;
    int expanded_mask_offset;
    bool expanded_mask_cache_hit;
    A64SVEPredicateMaskCacheKey expanded_mask_key;

    switch (source->insn_type) {
    case AARCH64_A64_SVE_FABS:
        unary = &source->arg.f_sve_fabs;
        expected_op = PVIR_OP_AND;
        negate = false;
        break;
    case AARCH64_A64_SVE_FNEG:
        unary = &source->arg.f_sve_fneg;
        expected_op = PVIR_OP_XOR;
        negate = true;
        break;
    default:
        return false;
    }
    immediate = negate ? (unary->element_bits == 64
                              ? UINT64_C(0x8000000000000000)
                              : UINT64_C(0x80000000))
                       : (unary->element_bits == 64
                              ? UINT64_C(0x7fffffffffffffff)
                              : UINT64_C(0x7fffffff));

    if (instruction->opcode != expected_op ||
        operation->type.kind != PVIR_ELEMENT_BITS ||
        operation->type.signedness != PVIR_SIGNLESS ||
        (operation->type.element_bits != 32 &&
         operation->type.element_bits != 64) ||
        operation->type.vector_bits != vl_bytes * 8u ||
        instruction->effects.fp_exceptions != PVIR_FP_EXCEPTIONS_NONE ||
        instruction->effects.memory.access != PVIR_MEMORY_NONE ||
        operation->inactive_policy != PVIR_INACTIVE_MERGE ||
        operation->merge_value == PVIR_NO_VALUE ||
        operation->operand_count != 2 ||
        operation->operand_kinds[0] != PVIR_OPERAND_VECTOR ||
        operation->operand_kinds[1] != PVIR_OPERAND_IMMEDIATE ||
        operation->operands[0] !=
            (unary->zd == unary->zm
                 ? operation->merge_value
                 : s->sve_pvir_builder->z_values[unary->zm]) ||
        operation->operand_immediates[1] != immediate ||
        operation->predicate.form != PVIR_PREDICATE_VECTOR_MASK ||
        operation->predicate.source.kind != PVIR_PREDICATE_SOURCE_PREDICATE ||
        operation->predicate.source.id == PVIR_NO_VALUE ||
        operation->predicate.first_lane != 0 ||
        operation->type.element_bits != unary->element_bits ||
        operation->predicate.lanes !=
            operation->type.vector_bits / operation->type.element_bits ||
        unary->zd >= A64_SVE_PVIR_NUM_ZREGS ||
        unary->zm >= A64_SVE_PVIR_NUM_ZREGS ||
        unary->pg >= A64_SVE_PVIR_NUM_PREGS) {
        return false;
    }
    expanded_mask_offset = env_offset_sve_expanded_mask_scratch();
    if (expanded_mask_offset < 0 ||
        !expanded_mask_cache_key_from_operation(operation,
                                                &expanded_mask_key)) {
        return false;
    }
    expanded_mask_cache_hit = expanded_mask_cache_keys_equal(
        &s->sve_pvir_builder->expanded_mask_key, &expanded_mask_key);
    bool const_pred_fold =
        is_const_fold_predicate(s, &operation->predicate, unary->pg);

    destination_offset = env_offset_zreg(unary->zd);
    source_offset = env_offset_zreg(unary->zm);
    predicate_offset = env_offset_sve_preg(unary->pg);
    if (destination_offset < 0 || source_offset < 0 || predicate_offset < 0) {
        return false;
    }

    /* LASX uses 32-byte chunks; each advances four packed predicate bytes. */
    for (size_t byte = 0; byte < vl_bytes; byte += 32) {
        IR2_OPND old_value = ra_alloc_ftemp();
        IR2_OPND source_value = ra_alloc_ftemp();
        IR2_OPND result_value = ra_alloc_ftemp();
        IR2_OPND mask = ra_alloc_ftemp();
        IR2_OPND result = ra_alloc_ftemp();
        uint16_t element_bits = operation->type.element_bits;
        size_t remaining = vl_bytes - byte;
        bool ok = true;

        lata_env_xvld(old_value, destination_offset + (int)byte);
        lata_env_xvld(source_value, source_offset + (int)byte);
        ok = emit_load_chunk_mask(
            s, &operation->predicate, unary->pg, element_bits, byte, remaining,
            predicate_offset, expanded_mask_offset, const_pred_fold,
            expanded_mask_cache_hit, mask);
        if (ok) {
            IR2_OPND constant = ra_alloc_itemp();
            uint64_t pattern = element_bits == 64
                                   ? immediate
                                   : (immediate | (immediate << 32));

            /* Seed element 0 of each 128-bit half before the broadcast. */
            li_d(constant, (int64_t)pattern);
            la_xvinsgr2vr_d(result, constant, 0);
            la_xvinsgr2vr_d(result, constant, 2);
            la_xvreplve0_d(result, result);
            ra_free_temp(constant);
            if (negate) {
                la_xvxor_v(result_value, source_value, result);
            } else {
                la_xvand_v(result_value, source_value, result);
            }
        }
        if (ok) {
            /* Merge output overwrites the now-dead constant register. */
            la_xvbitsel_v(result, old_value, result_value, mask);
            if (remaining >= 32) {
                lata_env_xvst(result, destination_offset + (int)byte);
            } else {
                /* VL is a multiple of 16 bytes; store only the active half. */
                lata_env_vst(result, destination_offset + (int)byte);
            }
        }
        ra_free_temp(result);
        ra_free_temp(mask);
        ra_free_temp(result_value);
        ra_free_temp(source_value);
        ra_free_temp(old_value);
        if (!ok) {
            return false;
        }
    }
    if (!const_pred_fold && !expanded_mask_cache_hit) {
        s->sve_pvir_builder->expanded_mask_key = expanded_mask_key;
    }
    return true;
}

/* SVE NOT (predicated): vd = ~vj per element, inactive lanes keep old Zd. */
static bool lower_integer_unary(DisasContext *s,
                                const A64SVEIR1Instruction *source,
                                const PvirInstruction *instruction)
{
    const PvirVectorOperation *operation = &instruction->operation.vector;
    const arg_sve_int_unary *unary = &source->arg.f_sve_not;
    size_t vl_bytes = (size_t)s->vl;
    int destination_offset;
    int source_offset;
    int predicate_offset;
    int expanded_mask_offset;
    bool expanded_mask_cache_hit;
    A64SVEPredicateMaskCacheKey expanded_mask_key;

    if (source->insn_type != AARCH64_A64_SVE_NOT ||
        instruction->opcode != PVIR_OP_NOT ||
        operation->type.kind != PVIR_ELEMENT_BITS ||
        operation->type.signedness != PVIR_SIGNLESS ||
        (operation->type.element_bits != 8 &&
         operation->type.element_bits != 16 &&
         operation->type.element_bits != 32 &&
         operation->type.element_bits != 64) ||
        operation->type.element_bits != unary->element_bits ||
        operation->type.vector_bits != vl_bytes * 8u ||
        instruction->effects.fp_exceptions != PVIR_FP_EXCEPTIONS_NONE ||
        instruction->effects.memory.access != PVIR_MEMORY_NONE ||
        operation->inactive_policy != PVIR_INACTIVE_MERGE ||
        operation->merge_value == PVIR_NO_VALUE ||
        operation->operand_count != 1 ||
        operation->operand_kinds[0] != PVIR_OPERAND_VECTOR ||
        operation->predicate.form != PVIR_PREDICATE_VECTOR_MASK ||
        operation->predicate.source.kind != PVIR_PREDICATE_SOURCE_PREDICATE ||
        operation->predicate.source.id == PVIR_NO_VALUE ||
        operation->predicate.first_lane != 0 ||
        operation->predicate.lanes !=
            operation->type.vector_bits / operation->type.element_bits ||
        unary->zd >= A64_SVE_PVIR_NUM_ZREGS ||
        unary->zm >= A64_SVE_PVIR_NUM_ZREGS ||
        unary->pg >= A64_SVE_PVIR_NUM_PREGS) {
        return false;
    }
    expanded_mask_offset = env_offset_sve_expanded_mask_scratch();
    if (expanded_mask_offset < 0 ||
        !expanded_mask_cache_key_from_operation(operation,
                                                &expanded_mask_key)) {
        return false;
    }
    expanded_mask_cache_hit = expanded_mask_cache_keys_equal(
        &s->sve_pvir_builder->expanded_mask_key, &expanded_mask_key);
    bool const_pred_fold =
        is_const_fold_predicate(s, &operation->predicate, unary->pg);

    destination_offset = env_offset_zreg(unary->zd);
    source_offset = env_offset_zreg(unary->zm);
    predicate_offset = env_offset_sve_preg(unary->pg);
    if (destination_offset < 0 || source_offset < 0 || predicate_offset < 0) {
        return false;
    }

    for (size_t byte = 0; byte < vl_bytes; byte += 32) {
        IR2_OPND old_value = ra_alloc_ftemp();
        IR2_OPND source_value = ra_alloc_ftemp();
        IR2_OPND result_value = ra_alloc_ftemp();
        IR2_OPND mask = ra_alloc_ftemp();
        IR2_OPND result = ra_alloc_ftemp();
        size_t remaining = vl_bytes - byte;
        bool ok = true;

        lata_env_xvld(old_value, destination_offset + (int)byte);
        lata_env_xvld(source_value, source_offset + (int)byte);
        ok = emit_load_chunk_mask(
            s, &operation->predicate, unary->pg, operation->type.element_bits,
            byte, remaining, predicate_offset, expanded_mask_offset,
            const_pred_fold, expanded_mask_cache_hit, mask);
        if (ok) {
            /* ~v = nor(v, v): no constant vector needed. */
            la_xvnor_v(result_value, source_value, source_value);
            la_xvbitsel_v(result, old_value, result_value, mask);
            store_z_chunk(result, destination_offset + (int)byte, remaining);
        }
        ra_free_temp(result);
        ra_free_temp(mask);
        ra_free_temp(result_value);
        ra_free_temp(source_value);
        ra_free_temp(old_value);
        if (!ok) {
            return false;
        }
    }
    if (!const_pred_fold && !expanded_mask_cache_hit) {
        s->sve_pvir_builder->expanded_mask_key = expanded_mask_key;
    }
    return true;
}

/* DUP/MOV (scalar to vector): broadcast the guest GPR's low element. */
static bool lower_broadcast(DisasContext *s,
                            const A64SVEIR1Instruction *source,
                            const PvirInstruction *instruction)
{
    const PvirVectorOperation *operation = &instruction->operation.vector;
    const arg_sve_dup *dup = &source->arg.f_sve_dup;
    size_t vl_bytes = (size_t)s->vl;
    IR2_OPND guest_gpr;
    int destination_offset;

    if (source->insn_type != AARCH64_A64_SVE_DUP ||
        instruction->opcode != PVIR_OP_BROADCAST ||
        operation->type.kind != PVIR_ELEMENT_BITS ||
        operation->type.element_bits != dup->element_bits ||
        operation->type.vector_bits != vl_bytes * 8u ||
        operation->operand_count != 1 ||
        operation->operand_kinds[0] != PVIR_OPERAND_SCALAR ||
        operation->operands[0] != A64_SVE_PVIR_GPR_VALUE_BASE + dup->rn ||
        operation->predicate.form != PVIR_PREDICATE_ALL_TRUE ||
        operation->inactive_policy != PVIR_INACTIVE_MERGE ||
        instruction->effects.memory.access != PVIR_MEMORY_NONE ||
        instruction->effects.fp_exceptions != PVIR_FP_EXCEPTIONS_NONE ||
        dup->zd >= A64_SVE_PVIR_NUM_ZREGS || dup->rn >= 32) {
        return false;
    }
    destination_offset = env_offset_zreg(dup->zd);
    if (destination_offset < 0) {
        return false;
    }
    guest_gpr = alloc_gpr_src(dup->rn);

    for (size_t byte = 0; byte < vl_bytes; byte += 32) {
        IR2_OPND value = ra_alloc_ftemp();
        size_t remaining = vl_bytes - byte;

        switch (operation->type.element_bits) {
        case 8:
            la_xvinsgr2vr_d(value, guest_gpr, 0);
            la_xvinsgr2vr_d(value, guest_gpr, 2);
            la_xvreplve0_b(value, value);
            break;
        case 16:
            la_xvinsgr2vr_d(value, guest_gpr, 0);
            la_xvinsgr2vr_d(value, guest_gpr, 2);
            la_xvreplve0_h(value, value);
            break;
        case 32:
            la_xvinsgr2vr_w(value, guest_gpr, 0);
            la_xvinsgr2vr_w(value, guest_gpr, 4);
            la_xvreplve0_w(value, value);
            break;
        default:
            la_xvinsgr2vr_d(value, guest_gpr, 0);
            la_xvinsgr2vr_d(value, guest_gpr, 2);
            la_xvreplve0_d(value, value);
            break;
        }
        if (remaining >= 32) {
            lata_env_xvst(value, destination_offset + (int)byte);
        } else {
            lata_env_vst(value, destination_offset + (int)byte);
        }
        ra_free_temp(value);
    }
    free_alloc_gpr(guest_gpr);
    return true;
}

static bool instruction_writes_nzcv_static(const PvirInstruction *ins,
                                           const A64SVEIR1Instruction *src)
{
    if (src != NULL) {
        switch (src->insn_type) {
        case AARCH64_A64_SVE_PTRUE:
            return src->arg.f_sve_ptrue.setflags;
        case AARCH64_A64_SVE_RDFFR:
            return src->arg.f_sve_ffr.setflags;
        case AARCH64_A64_SVE_PTEST:
            return true;
        case AARCH64_A64_SVE_BRK:
        case AARCH64_A64_SVE_PFIRST:
        case AARCH64_A64_SVE_PNEXT:
        case AARCH64_A64_SVE_PRED_LOGICAL:
            return src->arg.f_sve_pred_op.setflags;
        case AARCH64_A64_SVE_WHILELO:
        case AARCH64_A64_SVE_WHILE:
        case AARCH64_A64_SVE_FCMP:
        case AARCH64_A64_SVE_ICMP:
            return true;
        default:
            break;
        }
    }
    if (ins != NULL && ins->opcode == PVIR_OP_PREDICATE_GENERATE) {
        return ins->operation.predicate_generator.status_state.present;
    }
    return false;
}

/*
 * Translation-time gate for the SVE inline memory fast path.
 *
 * There is no fast path unless the active provider publishes a
 * LataGuestMemDirectPolicy, i.e. unless somebody can prove the mapping, the
 * permission and the invalidation rule for the spans this instruction touches
 * (see lata-mem.h). This function only decides whether it is worth emitting
 * code at all; the actual entry condition is the per-span runtime admission in
 * emit_direct_span_admission(), which re-checks the proof for the address the
 * instruction really uses.
 */
static bool sve_mem_direct_fast_path_declined(void)
{
    CPUARMState *env;

    if (!lata_guest_mem_direct_available()) {
        return true;
    }
    env = (lsenv != NULL) ? (CPUARMState *)lsenv->cpu_state : NULL;
    if (env != NULL && (env->sve_ctrl.fault_region_count > 0 ||
                        env->sve_ctrl.fault_mem_limit != 0)) {
        return true;
    }
    return false;
}

static void emit_save_guest_gprs_to_env(void)
{
    for (int i = 0; i <= 31; ++i) {
        if (arm_la_map[i] > 0) {
            la_st_d(ir2_opnd_new(IR2_OPND_GPR, arm_la_map[i]),
                    env_ir2_opnd, env_offset_gpr(i));
        }
    }
}

static void emit_restore_guest_gprs_from_env(void)
{
    for (int i = 0; i <= 31; ++i) {
        if (arm_la_map[i] > 0) {
            la_ld_d(ir2_opnd_new(IR2_OPND_GPR, arm_la_map[i]),
                    env_ir2_opnd, env_offset_gpr(i));
        }
    }
}

/*
 * Emit the runtime admission for one contiguous span: the guest GPRs are
 * published to env (the call owns the ABI argument registers, several of which
 * are guest GPR homes), the proof is queried for [span_start, +span_bytes) at
 * the address computed by *this* execution, and control branches to `admitted`
 * only when the provider still stands behind that span. Anything else falls
 * through to the checked helper path the caller emits next.
 */
static void emit_direct_span_admission(IR2_OPND span_start, uint32_t span_bytes,
                                       bool is_write, IR2_OPND admitted)
{
    emit_save_guest_gprs_to_env();
    la_mov64(a1_ir2_opnd, span_start);
    li_d(a2_ir2_opnd, (int64_t)span_bytes);
    li_d(a3_ir2_opnd, is_write ? 1 : 0);
    li_d(a4_ir2_opnd, (int64_t)lata_guest_mem_direct_epoch());
    la_mov64(a0_ir2_opnd, env_ir2_opnd);
    li_d(t0_ir2_opnd, (int64_t)(uintptr_t)lata_guest_mem_direct_admit);
    la_jirl(ra_ir2_opnd, t0_ir2_opnd, 0);
    la_mov64(t2_ir2_opnd, a0_ir2_opnd);
    emit_restore_guest_gprs_from_env();
    la_bnez(t2_ir2_opnd, admitted);
}

static void emit_inline_mem_fault_abort(const DisasContext *s,
                                        IR2_OPND fault_addr_itemp,
                                        bool is_write, bool pop_stack)
{
    if (pop_stack) {
        la_addi_d(sp_ir2_opnd, sp_ir2_opnd, ARM_SVE_MAX_VL_BYTES);
    }
    emit_save_guest_gprs_to_env();
    la_mov64(a2_ir2_opnd, fault_addr_itemp);
    la_mov64(a0_ir2_opnd, env_ir2_opnd);
    li_d(a1_ir2_opnd, (int64_t)s->pc_curr);
    li_d(a3_ir2_opnd, is_write ? 1 : 0);
    li_d(a4_ir2_opnd, (int64_t)LATA_MEM_FAULT_TRANSLATION);
    li_d(t0_ir2_opnd, (int64_t)(uintptr_t)lata_guest_mem_raise_fault);
    la_jirl(ra_ir2_opnd, t0_ir2_opnd, 0);
    emit_restore_guest_gprs_from_env();
    li_d(t0_ir2_opnd, (int64_t)(uintptr_t)&lata_vreg_published);
    li_d(t1_ir2_opnd, 1);
    la_st_w(t1_ir2_opnd, t0_ir2_opnd, 0);
    if (context_switch_native_to_bt_ret_0 != 0) {
        li_d(t0_ir2_opnd, (int64_t)context_switch_native_to_bt_ret_0);
        la_jirl(zero_ir2_opnd, t0_ir2_opnd, 0);
    }
}

static bool emit_sve_helper_call_body(DisasContext *s,
                                      const PvirInstruction *instruction,
                                      const A64SVEIR1Instruction *source,
                                      const LataSVEHelperDesc *desc)
{
    IR2_OPND no_fault = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND fault = ir2_opnd_new_type(IR2_OPND_LABEL);
    uint64_t next_pc = (s->base != NULL) ? s->base->pc_next : (s->pc_curr + 4u);

    emit_save_guest_gprs_to_env();
    li_d(t0_ir2_opnd, (int64_t)next_pc);
    la_st_d(t0_ir2_opnd, env_ir2_opnd, env_offset_pc());

    la_mov64(a0_ir2_opnd, env_ir2_opnd);
    li_d(a1_ir2_opnd, (int64_t)(uintptr_t)desc);
    li_d(t0_ir2_opnd, (int64_t)(uintptr_t)lata_sve_exec_helper);
    la_jirl(ra_ir2_opnd, t0_ir2_opnd, 0);

    la_mov64(s8_ir2_opnd, env_ir2_opnd); /* ensure env reg intact */
    la_mov64(t2_ir2_opnd, a0_ir2_opnd);
    emit_restore_guest_gprs_from_env();
    if (instruction_writes_nzcv_static(instruction, source)) {
        li_d(t0_ir2_opnd, env_offset_PSTATE());
        la_ldx_wu(t1_ir2_opnd, env_ir2_opnd, t0_ir2_opnd);
        la_armmtflag(t1_ir2_opnd, 0x39);
    }
    la_beqz(t2_ir2_opnd, no_fault);

    /* 1 is an architectural exception the helper already latched; anything
     * larger means it declined the instruction. That must not fall through to
     * "resume at pc_next", which is exactly the silent no-op the status
     * encoding exists to remove. */
    li_d(t1_ir2_opnd, LATA_SVE_HELPER_FAULT);
    la_beq(t2_ir2_opnd, t1_ir2_opnd, fault);
    la_mov64(a0_ir2_opnd, s8_ir2_opnd);
    li_d(a1_ir2_opnd, (int64_t)(uintptr_t)desc);
    la_mov64(a2_ir2_opnd, t2_ir2_opnd);
    li_d(t0_ir2_opnd, (int64_t)(uintptr_t)lata_sve_helper_unexpected);
    la_jirl(ra_ir2_opnd, t0_ir2_opnd, 0);

    la_label(fault);
    li_d(t0_ir2_opnd, (int64_t)(uintptr_t)&lata_vreg_published);
    li_d(t1_ir2_opnd, 1);
    la_st_w(t1_ir2_opnd, t0_ir2_opnd, 0);
    if (context_switch_native_to_bt_ret_0 != 0) {
        li_d(t0_ir2_opnd, (int64_t)context_switch_native_to_bt_ret_0);
        la_jirl(zero_ir2_opnd, t0_ir2_opnd, 0);
    }
    la_label(no_fault);
    return true;
}

static bool emit_sve_helper_call(DisasContext *s,
                                 const PvirInstruction *instruction,
                                 const A64SVEIR1Instruction *source)
{
    const LataSVEHelperDesc *desc =
        lata_sve_alloc_helper_desc(s, instruction, source);

    if (desc == NULL) {
        return false;
    }
    return emit_sve_helper_call_body(s, instruction, source, desc);
}

/*
 * Fault-tolerant loads are the one class whose helper must observe and update
 * architectural vector state through env.  Flush the resident registers before
 * the call and reload them afterwards; other helper-based lowerings keep the
 * inline convention (values live in host registers until the TB exits).
 */
static void lata_emit_flush_resident_vectors(void)
{
    if (!latx_cpu_has_lsx()) {
        return;
    }
    for (int i = 0; i <= 31; ++i) {
        if (arm_la_fmap[i] >= 0 && lata_vreg_resident(i)) {
            lata_env_vst(ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[i]),
                         env_offset_vreg(i));
        }
    }
}

static void lata_emit_reload_resident_vectors(void)
{
    if (!latx_cpu_has_lsx()) {
        return;
    }
    for (int i = 0; i <= 31; ++i) {
        if (arm_la_fmap[i] >= 0 && lata_vreg_resident(i)) {
            lata_env_vld(ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[i]),
                         env_offset_vreg(i));
        }
    }
}

static bool lower_contiguous_memory(
    DisasContext *s, const A64SVEIR1Instruction *source,
    const PvirInstruction *instruction)
{
    const arg_sve_contiguous *memory = &source->arg.f_sve_contiguous;
    const PvirVectorOperation *operation = &instruction->operation.vector;
    bool is_load = source->insn_type == AARCH64_A64_SVE_LD1B ||
                   source->insn_type == AARCH64_A64_SVE_LD1H ||
                   source->insn_type == AARCH64_A64_SVE_LD1W ||
                   source->insn_type == AARCH64_A64_SVE_LD1D ||
                   source->insn_type == AARCH64_A64_SVE_LD1SB ||
                   source->insn_type == AARCH64_A64_SVE_LD1SH ||
                   source->insn_type == AARCH64_A64_SVE_LD1SW;
    size_t vl_bytes = (size_t)s->vl;
    size_t element_bytes = operation->type.element_bits / 8u;
    size_t lanes;
    int predicate_offset;
    int vector_offset;
    int64_t displacement;
    IR2_OPND guest_base;
    IR2_OPND adjusted_base;
    IR2_OPND address;
    IR2_OPND predicate_word;
    IR2_OPND active;
    IR2_OPND value;
    IR2_OPND admitted;
    IR2_OPND direct_done;
    const LataSVEHelperDesc *fallback;

    if ((!is_load && source->insn_type != AARCH64_A64_SVE_ST1B &&
         source->insn_type != AARCH64_A64_SVE_ST1H &&
         source->insn_type != AARCH64_A64_SVE_ST1W &&
         source->insn_type != AARCH64_A64_SVE_ST1D) ||
        (!is_load && memory->sign_extend) ||
        !sve_context_valid(s) || memory->zt >= A64_SVE_PVIR_NUM_ZREGS ||
        memory->pg >= 8 || memory->rn > 31 ||
        (memory->memory_bits != 8 && memory->memory_bits != 16 &&
         memory->memory_bits != 32 && memory->memory_bits != 64) ||
        !memory->effects.memory.access ||
        operation->type.kind != PVIR_ELEMENT_INTEGER ||
        (operation->type.signedness != PVIR_UNSIGNED &&
         operation->type.signedness != PVIR_SIGNED) ||
        operation->type.vector_bits != vl_bytes * 8u ||
        (operation->type.element_bits != 8 &&
         operation->type.element_bits != 16 &&
         operation->type.element_bits != 32 &&
         operation->type.element_bits != 64) ||
        operation->memory_element_bits != memory->memory_bits ||
        instruction->effects.fault.behavior != PVIR_FAULT_PRECISE ||
        instruction->effects.fault.lane_order !=
            PVIR_FAULT_ORDER_LANE_ASCENDING ||
        instruction->effects.memory.inactive_lanes !=
            PVIR_INACTIVE_MEMORY_NO_TRANSACTION ||
        operation->predicate.source.kind != PVIR_PREDICATE_SOURCE_PREDICATE ||
        operation->predicate.source.id !=
            s->sve_pvir_builder->p_values[memory->pg] ||
        operation->predicate.source.version !=
            s->sve_pvir_builder->p_versions[memory->pg] ||
        operation->predicate.first_lane != 0 ||
        memory->base_is_sp != (memory->rn == 31) ||
        operation->memory_address.present != true ||
        operation->memory_address.base_kind !=
            (memory->base_is_sp ? PVIR_MEMORY_BASE_STACK_POINTER
                                : PVIR_MEMORY_BASE_VALUE) ||
        operation->memory_address.base_value !=
            (memory->base_is_sp
                 ? PVIR_NO_VALUE
                 : A64_SVE_PVIR_GPR_VALUE_BASE + memory->rn) ||
        instruction->opcode != (is_load ? PVIR_OP_LOAD : PVIR_OP_STORE) ||
        operation->inactive_policy !=
            (is_load ? PVIR_INACTIVE_ZERO
                     : PVIR_INACTIVE_NOT_APPLICABLE) ||
        (is_load && instruction->effects.memory.access != PVIR_MEMORY_READ) ||
        (!is_load &&
         instruction->effects.memory.access != PVIR_MEMORY_WRITE)) {
        return false;
    }

    lanes = vl_bytes / element_bytes;
    displacement =
        (int64_t)memory->immediate_vl * (int64_t)vl_bytes;
    if (operation->memory_address.displacement != displacement ||
        operation->operand_count != (is_load ? 0 : 1) ||
        (is_load &&
         s->sve_pvir_builder->z_values[memory->zt] !=
             operation->destination) ||
        (!is_load &&
         s->sve_pvir_builder->z_values[memory->zt] !=
             operation->operands[0])) {
        return false;
    }

    predicate_offset = env_offset_sve_preg(memory->pg);
    vector_offset = env_offset_zreg(memory->zt);
    if (predicate_offset < 0 || vector_offset < 0) {
        return false;
    }

    if (sve_mem_direct_fast_path_declined()) {
        return emit_sve_helper_call(s, instruction, source);
    }
    /*
     * Allocate the checked-path descriptor before emitting anything: a
     * descriptor failure has to reject the instruction as a whole instead of
     * leaving half an admission sequence in the IR2 stream.
     */
    fallback = lata_sve_alloc_helper_desc(s, instruction, source);
    if (fallback == NULL) {
        return false;
    }

    guest_base = alloc_gpr_src_sp(memory->rn);
    /* Mirror lata_clean_data_tbi() in translate-a64.c. */
    if (s->tbid != 0) {
        /* TBI ignores bits 63:56; keep 56 bits so this matches mem.c and
         * the helper path (0x00ffffffffffffff). */
        la_bstrpick_d(guest_base, guest_base, 55, 0);
    }

    adjusted_base = ra_alloc_itemp();
    li_d(adjusted_base, displacement);
    la_add_d(adjusted_base, guest_base, adjusted_base);
    free_alloc_gpr(guest_base);

    /*
     * Fast-path entry condition. The span is one vector worth of bytes from the
     * adjusted base; a lane whose memory element is narrower than its vector
     * element touches a subset of it, so admitting the enclosing window is the
     * conservative choice. If the provider no longer proves this address the
     * instruction takes the checked path below with the same descriptor.
     */
    admitted = ir2_opnd_new_type(IR2_OPND_LABEL);
    direct_done = ir2_opnd_new_type(IR2_OPND_LABEL);
    emit_direct_span_admission(adjusted_base, (uint32_t)vl_bytes, !is_load,
                               admitted);
    if (!emit_sve_helper_call_body(s, instruction, source, fallback)) {
        return false;
    }
    la_b(direct_done);
    la_label(admitted);

    address = ra_alloc_itemp();
    predicate_word = ra_alloc_itemp();
    active = ra_alloc_itemp();
    value = ra_alloc_itemp();

    /*
     * C8.2.298 assembles the normal-load result before committing Z. Keep
     * lane bytes in bounded host-stack scratch until every active access
     * succeeds; the final vector writes are the architectural commit.
     */
    if (is_load) {
        IR2_OPND zero = ra_alloc_itemp();

        la_addi_d(sp_ir2_opnd, sp_ir2_opnd, -ARM_SVE_MAX_VL_BYTES);
        li_d(zero, 0);
        for (size_t byte = 0; byte < vl_bytes; byte += sizeof(uint64_t)) {
            la_st_d(zero, sp_ir2_opnd, (int)byte);
        }
        ra_free_temp(zero);
    }

    IR2_OPND abort_fault = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND done_loop = ir2_opnd_new_type(IR2_OPND_LABEL);

    /* C8.2.298/C8.2.704 visit lanes in order; inactive lanes branch around
     * the guest access entirely. Stores therefore retain prior lane effects
     * if a later active lane faults.
     */
    for (size_t lane = 0; lane < lanes; ++lane) {
        size_t vector_byte = lane * element_bytes;
        size_t memory_byte = lane * (memory->memory_bits / 8u);
        size_t predicate_bit = vector_byte;
        size_t predicate_word_byte = (predicate_bit / 32u) * 4u;
        unsigned predicate_shift = predicate_bit % 32u;
        IR2_OPND skip_lane = ir2_opnd_new_type(IR2_OPND_LABEL);

        lata_env_ld_wu(predicate_word,
                       predicate_offset + (int)predicate_word_byte);
        la_bstrpick_d(active, predicate_word, predicate_shift,
                      predicate_shift);
        la_beqz(active, skip_lane);
        if (memory_byte == 0) {
            la_mov64(address, adjusted_base);
        } else {
            la_addi_d(address, adjusted_base, (int)memory_byte);
        }
        li_d(active, 0x1000);
        la_bltu(address, active, abort_fault);
        if (is_load) {
            /*
             * The staged slot is pre-zeroed, so a wider store would leave zeros
             * above an unsigned load. Store with the *element* width instead:
             * ldx.b/h/w sign-extends to 64 bits when requested and ldx.bu/hu/wu
             * zero-extends, matching LD1SB/LD1SH/LD1SW versus LD1B/H/W.
             */
            bool sign = operation->type.signedness == PVIR_SIGNED;

            switch (memory->memory_bits) {
            case 8:
                if (sign) {
                    la_ldx_b(value, address, zero_ir2_opnd);
                } else {
                    la_ldx_bu(value, address, zero_ir2_opnd);
                }
                break;
            case 16:
                if (sign) {
                    la_ldx_h(value, address, zero_ir2_opnd);
                } else {
                    la_ldx_hu(value, address, zero_ir2_opnd);
                }
                break;
            case 32:
                if (sign) {
                    la_ldx_w(value, address, zero_ir2_opnd);
                } else {
                    la_ldx_wu(value, address, zero_ir2_opnd);
                }
                break;
            default:
                la_ldx_d(value, address, zero_ir2_opnd);
                break;
            }
            switch (element_bytes) {
            case 1:
                la_st_b(value, sp_ir2_opnd, (int)vector_byte);
                break;
            case 2:
                la_st_h(value, sp_ir2_opnd, (int)vector_byte);
                break;
            case 4:
                la_st_w(value, sp_ir2_opnd, (int)vector_byte);
                break;
            default:
                la_st_d(value, sp_ir2_opnd, (int)vector_byte);
                break;
            }
        } else {
            size_t aligned_byte = vector_byte & ~(sizeof(uint64_t) - 1u);
            unsigned byte_shift =
                (unsigned)(vector_byte - aligned_byte) * 8u;

            lata_env_ld_du(value, vector_offset + (int)aligned_byte);
            la_bstrpick_d(value, value,
                          byte_shift + memory->memory_bits - 1u, byte_shift);
            switch (memory->memory_bits) {
            case 8:
                la_stx_b(value, address, zero_ir2_opnd);
                break;
            case 16:
                la_stx_h(value, address, zero_ir2_opnd);
                break;
            case 32:
                la_stx_w(value, address, zero_ir2_opnd);
                break;
            default:
                la_stx_d(value, address, zero_ir2_opnd);
                break;
            }
        }
        la_label(skip_lane);
    }

    la_b(done_loop);
    la_label(abort_fault);
    emit_inline_mem_fault_abort(s, address, !is_load, is_load);
    la_label(done_loop);

    ra_free_temp(value);
    ra_free_temp(active);
    ra_free_temp(predicate_word);
    ra_free_temp(address);
    ra_free_temp(adjusted_base);

    if (is_load) {
        IR2_OPND staged = ra_alloc_ftemp();
        size_t destination_offset = env_offset_zreg(memory->zt);

        for (size_t byte = 0; byte < vl_bytes; byte += 32u) {
            size_t remaining = vl_bytes - byte;

            if (remaining >= 32u) {
                la_xvld(staged, sp_ir2_opnd, (int)byte);
                lata_env_xvst(staged, destination_offset + (int)byte);
            } else {
                la_vld(staged, sp_ir2_opnd, (int)byte);
                lata_env_vst(staged, destination_offset + (int)byte);
            }
        }
        ra_free_temp(staged);
        la_addi_d(sp_ir2_opnd, sp_ir2_opnd, ARM_SVE_MAX_VL_BYTES);
    }
    la_label(direct_done);
    return true;
}

/* Shared helper: store a 64-bit scalar from src_gpr into zreg lane byte_off (element width bytes). */
static void emit_store_elem_mem(IR2_OPND val, IR2_OPND base, int disp,
                                uint8_t elem_bytes)
{
    switch (elem_bytes) {
    case 1: la_st_b(val, base, disp); break;
    case 2: la_st_h(val, base, disp); break;
    case 4: la_st_w(val, base, disp); break;
    default: la_st_d(val, base, disp); break;
    }
}

static void emit_load_elem_mem(IR2_OPND dst, IR2_OPND base, int disp,
                               uint8_t mem_bytes, bool is_signed)
{
    switch (mem_bytes) {
    case 1:
        if (is_signed) la_ld_b(dst, base, disp);
        else la_ld_bu(dst, base, disp);
        break;
    case 2:
        if (is_signed) la_ld_h(dst, base, disp);
        else la_ld_hu(dst, base, disp);
        break;
    case 4:
        if (is_signed) la_ld_w(dst, base, disp);
        else la_ld_wu(dst, base, disp);
        break;
    default:
        la_ld_d(dst, base, disp);
        break;
    }
}

static void emit_ldx_elem(IR2_OPND dst, IR2_OPND addr, uint8_t mem_bytes,
                          bool is_signed)
{
    switch (mem_bytes) {
    case 1:
        if (is_signed) la_ldx_b(dst, addr, zero_ir2_opnd);
        else la_ldx_bu(dst, addr, zero_ir2_opnd);
        break;
    case 2:
        if (is_signed) la_ldx_h(dst, addr, zero_ir2_opnd);
        else la_ldx_hu(dst, addr, zero_ir2_opnd);
        break;
    case 4:
        if (is_signed) la_ldx_w(dst, addr, zero_ir2_opnd);
        else la_ldx_wu(dst, addr, zero_ir2_opnd);
        break;
    default:
        la_ldx_d(dst, addr, zero_ir2_opnd);
        break;
    }
}

static void emit_commit_stack_to_zreg(int z_offset, size_t vl_bytes)
{
    IR2_OPND staged = ra_alloc_ftemp();
    for (size_t byte = 0; byte < vl_bytes; byte += 32u) {
        size_t rem = vl_bytes - byte;
        if (rem >= 32u) {
            la_xvld(staged, sp_ir2_opnd, (int)byte);
            lata_env_xvst(staged, z_offset + (int)byte);
        } else {
            la_vld(staged, sp_ir2_opnd, (int)byte);
            lata_env_vst(staged, z_offset + (int)byte);
        }
    }
    ra_free_temp(staged);
}

static void emit_zero_stack_stage(size_t vl_bytes)
{
    IR2_OPND zero = ra_alloc_itemp();
    li_d(zero, 0);
    for (size_t b = 0; b < vl_bytes; b += sizeof(uint64_t)) {
        la_st_d(zero, sp_ir2_opnd, (int)b);
    }
    ra_free_temp(zero);
}

/* Broadcast a 64-bit constant pattern across all 32-byte LASX chunks via 1 itemp. */
static void emit_xv_broadcast_u64(IR2_OPND dst, uint64_t pat)
{
    if (pat == 0) {
        la_xvxor_v(dst, dst, dst);
    } else if (pat == ~UINT64_C(0)) {
        la_xvseq_b(dst, dst, dst);
    } else {
        IR2_OPND t = ra_alloc_itemp();
        li_d(t, (int64_t)pat);
        la_xvinsgr2vr_d(dst, t, 0);
        la_xvinsgr2vr_d(dst, t, 2);
        la_xvreplve0_d(dst, dst);
        ra_free_temp(t);
    }
}

static uint64_t repeat_elem_to_u64(uint64_t imm, uint16_t element_bits)
{
    uint64_t masked = imm & mask_for_bits(element_bits);
    uint64_t res = masked;
    for (unsigned s = element_bits; s < 64u; s <<= 1) {
        res |= (res << s);
    }
    return res;
}

/*
 * First-Fault / Non-Fault load (LDFF1* / LDNF1*):
 * Processes active lanes in ascending order, respecting incoming FFR and
 * recording cumulative-success writeback (clearing FFR and zeroing Zt from
 * the first suppressed fault lane onward).
 */
static bool lower_fault_load(DisasContext *s,
                             const A64SVEIR1Instruction *source,
                             const PvirInstruction *instruction)
{
    const PvirVectorOperation *op = &instruction->operation.vector;
    size_t vl_bytes = (size_t)s->vl;
    uint8_t element_bits = op->type.element_bits;
    uint8_t memory_bits = (uint8_t)op->memory_element_bits;
    uint8_t elem_bytes = (uint8_t)(element_bits / 8u);
    uint8_t mem_bytes = (uint8_t)(memory_bits / 8u);
    size_t lanes = vl_bytes / elem_bytes;
    bool is_first =
        instruction->effects.fault.behavior == PVIR_FAULT_FIRST_FAULT;
    bool is_signed = op->type.signedness == PVIR_SIGNED;
    bool base_is_sp =
        op->memory_address.base_kind == PVIR_MEMORY_BASE_STACK_POINTER;
    int rn = base_is_sp ? 31 : find_gpr_for_value(op->memory_address.base_value);
    int zt = find_zreg_for_value(s->sve_pvir_builder, op->destination);
    int pg = find_preg_for_value(s->sve_pvir_builder, op->predicate.source.id);
    bool has_index = false;
    int64_t disp = op->memory_address.displacement;
    int pg_off, zt_off, ffr_off;
    IR2_OPND base, addr, state, lane_addr, tmp;
    IR2_OPND admitted, direct_done;
    const LataSVEHelperDesc *fallback;

    if (source != NULL) {
        if (is_first) {
            const arg_sve_fault_load *f = &source->arg.f_sve_fault_load;
            zt = f->zt;
            pg = f->pg;
            rn = f->rn;
            has_index = f->has_index && f->rm < 31;
            base_is_sp = f->base_is_sp;
        } else {
            const arg_sve_nonfault_load *nf = &source->arg.f_sve_nonfault_load;
            zt = nf->zt;
            pg = nf->pg;
            rn = nf->rn;
            base_is_sp = nf->base_is_sp;
        }
    }
    if (zt < 0 || zt >= 32 || pg < 0 || pg >= 16 || rn < 0 || rn > 31 ||
        instruction->opcode != PVIR_OP_LOAD ||
        (pg_off = env_offset_sve_preg(pg)) < 0 ||
        (zt_off = env_offset_zreg(zt)) < 0 ||
        (ffr_off = env_offset_sve_ffr()) < 0) {
        return false;
    }

    /*
     * An indexed first-fault/non-fault load derives its addresses from a vector
     * of indices, so no statically bounded span exists to prove; and without a
     * mapping policy there is nothing to prove at all. Both cases belong to the
     * checked path, which is the only one that can implement MemNF/FFR
     * suppression exactly.
     */
    if (has_index || sve_mem_direct_fast_path_declined()) {
        return emit_sve_helper_call(s, instruction, source);
    }
    fallback = lata_sve_alloc_helper_desc(s, instruction, source);
    if (fallback == NULL) {
        return false;
    }

    base = base_is_sp ? alloc_gpr_src_sp(31) : alloc_gpr_src(rn);
    addr = ra_alloc_itemp();
    la_mov64(addr, base);
    free_alloc_gpr(base);
    if (disp != 0) {
        IR2_OPND d = ra_alloc_itemp();
        li_d(d, disp);
        la_add_d(addr, addr, d);
        ra_free_temp(d);
    }
    if (s->tbid != 0) {
        la_bstrpick_d(addr, addr, 55, 0);
    }

    admitted = ir2_opnd_new_type(IR2_OPND_LABEL);
    direct_done = ir2_opnd_new_type(IR2_OPND_LABEL);
    emit_direct_span_admission(addr, (uint32_t)vl_bytes, false, admitted);
    if (!emit_sve_helper_call_body(s, instruction, source, fallback)) {
        return false;
    }
    la_b(direct_done);
    la_label(admitted);

    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, -ARM_SVE_MAX_VL_BYTES);
    emit_zero_stack_stage(vl_bytes);

    state = ra_alloc_itemp();
    lane_addr = ra_alloc_itemp();
    tmp = ra_alloc_itemp();

    li_d(state, 0);
    IR2_OPND first_fault_abort = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND fault_loop_done = ir2_opnd_new_type(IR2_OPND_LABEL);

    for (size_t lane = 0; lane < lanes; ++lane) {
        size_t vec_byte = lane * elem_bytes;
        size_t mem_off = lane * mem_bytes;
        size_t pbit = vec_byte;
        size_t pbyte = pbit / 8u;
        unsigned bit_in_byte = (unsigned)(pbit % 8u);
        size_t word_off = pbyte & ~(size_t)3u;
        unsigned shift_w = (unsigned)((pbyte - word_off) * 8u) + bit_in_byte;
        IR2_OPND skip_lane = ir2_opnd_new_type(IR2_OPND_LABEL);
        IR2_OPND fail_lane = ir2_opnd_new_type(IR2_OPND_LABEL);
        IR2_OPND fault_check = ir2_opnd_new_type(IR2_OPND_LABEL);
        IR2_OPND do_real_load = ir2_opnd_new_type(IR2_OPND_LABEL);
        IR2_OPND check_done = ir2_opnd_new_type(IR2_OPND_LABEL);

        lata_env_ld_wu(tmp, pg_off + (int)word_off);
        la_bstrpick_d(tmp, tmp, shift_w, shift_w);
        la_beqz(tmp, skip_lane);

        /* Active lane: fail if input FFR is 0 or an earlier lane already failed. */
        lata_env_ld_wu(tmp, ffr_off + (int)word_off);
        la_bstrpick_d(tmp, tmp, shift_w, shift_w);
        la_beqz(tmp, fail_lane);
        la_andi(tmp, state, 1);
        la_bnez(tmp, fail_lane);

        if (mem_off == 0) {
            la_mov64(lane_addr, addr);
        } else {
            la_addi_d(lane_addr, addr, (int)mem_off);
        }

        li_d(tmp, 0x1000);
        la_bltu(lane_addr, tmp, fault_check);
        lata_env_ld_du(tmp, (int)env_offset(sve_ctrl.fault_mem_limit));
        la_beqz(tmp, check_done);
        la_addi_d(lane_addr, lane_addr, mem_bytes);
        la_bltu(tmp, lane_addr, fault_check);
        la_addi_d(lane_addr, lane_addr, -mem_bytes);
        la_b(do_real_load);

        la_label(check_done);
        li_d(tmp, UINT64_C(0x20000000));
        la_bgeu(lane_addr, tmp, do_real_load);
        li_d(tmp, UINT64_C(0x10000000) - mem_bytes);
        la_bgeu(tmp, lane_addr, do_real_load);

        la_label(fault_check);
        if (is_first) {
            /* First active element (state bit 1 == 0) raises synchronous Data Abort. */
            la_andi(tmp, state, 2);
            la_beqz(tmp, first_fault_abort);
        }
        la_b(fail_lane);

        la_label(do_real_load);
        emit_ldx_elem(tmp, lane_addr, mem_bytes, is_signed);
        emit_store_elem_mem(tmp, sp_ir2_opnd, (int)vec_byte, elem_bytes);
        la_ori(state, state, 2);
        la_b(skip_lane);

        la_label(fail_lane);
        la_ori(state, state, 1);

        la_label(skip_lane);
        {
            IR2_OPND keep_ffr = ir2_opnd_new_type(IR2_OPND_LABEL);
            la_andi(tmp, state, 1);
            la_beqz(tmp, keep_ffr);
            lata_env_ld_wu(lane_addr, ffr_off + (int)word_off);
            la_bstrpick_d(lane_addr, lane_addr,
                          (unsigned)(pbyte - word_off) * 8u + 7u,
                          (unsigned)(pbyte - word_off) * 8u);
            li_d(tmp, ~(1LL << bit_in_byte));
            la_and(lane_addr, lane_addr, tmp);
            lata_env_st_b(lane_addr, ffr_off + (int)pbyte);
            la_label(keep_ffr);
        }
    }

    la_b(fault_loop_done);
    la_label(first_fault_abort);
    emit_inline_mem_fault_abort(s, lane_addr, false, true);
    la_label(fault_loop_done);

    ra_free_temp(tmp);
    ra_free_temp(lane_addr);
    ra_free_temp(state);
    ra_free_temp(addr);

    emit_commit_stack_to_zreg(zt_off, vl_bytes);
    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, ARM_SVE_MAX_VL_BYTES);
    la_label(direct_done);
    return true;
}

static bool lower_compact(DisasContext *s, const A64SVEIR1Instruction *source,
                          const PvirInstruction *instruction)
{
    const PvirPermuteOperation *perm = &instruction->operation.permute;
    size_t vl_bytes = (size_t)s->vl;
    uint8_t element_bits = (uint8_t)perm->type.element_bits;
    uint8_t elem_bytes = (uint8_t)(element_bits / 8u);
    size_t lanes = vl_bytes / elem_bytes;
    int zd = source != NULL ? (int)source->arg.f_sve_general.zd
                            : find_zreg_for_value(s->sve_pvir_builder, perm->destination);
    int zn = source != NULL ? (int)source->arg.f_sve_general.zn
                            : find_zreg_for_value(s->sve_pvir_builder, perm->operands[0]);
    int pg = source != NULL ? (int)source->arg.f_sve_general.pg
                            : find_preg_for_value(s->sve_pvir_builder, perm->predicate.source.id);
    int pg_off, zn_off, zd_off;
    IR2_OPND wptr, src_base, pword, active, val;

    if (zd < 0 || zn < 0 || pg < 0 || (elem_bytes != 4 && elem_bytes != 8) ||
        (pg_off = env_offset_sve_preg(pg)) < 0 ||
        (zn_off = env_offset_zreg(zn)) < 0 ||
        (zd_off = env_offset_zreg(zd)) < 0) {
        return false;
    }

    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, -ARM_SVE_MAX_VL_BYTES);
    emit_zero_stack_stage(vl_bytes);

    wptr = ra_alloc_itemp();
    src_base = ra_alloc_itemp();
    pword = ra_alloc_itemp();
    active = ra_alloc_itemp();
    val = ra_alloc_itemp();

    la_mov64(wptr, sp_ir2_opnd);
    li_d(src_base, zn_off);
    la_add_d(src_base, env_ir2_opnd, src_base);

    for (size_t lane = 0; lane < lanes; ++lane) {
        size_t vec_byte = lane * elem_bytes;
        size_t word_off = (vec_byte / 32u) * 4u;
        unsigned shift_w = (unsigned)(vec_byte % 32u);
        IR2_OPND skip = ir2_opnd_new_type(IR2_OPND_LABEL);

        lata_env_ld_wu(pword, pg_off + (int)word_off);
        la_bstrpick_d(active, pword, shift_w, shift_w);
        la_beqz(active, skip);
        emit_load_elem_mem(val, src_base, (int)vec_byte, elem_bytes, false);
        emit_store_elem_mem(val, wptr, 0, elem_bytes);
        la_addi_d(wptr, wptr, elem_bytes);
        la_label(skip);
    }

    ra_free_temp(val);
    ra_free_temp(active);
    ra_free_temp(pword);
    ra_free_temp(src_base);
    ra_free_temp(wptr);

    emit_commit_stack_to_zreg(zd_off, vl_bytes);
    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, ARM_SVE_MAX_VL_BYTES);
    return true;
}

static bool lower_ptest(DisasContext *s, const A64SVEIR1Instruction *source,
                        const PvirInstruction *instruction)
{
    size_t vl_bytes = (size_t)s->vl;
    int pg = source != NULL ? (int)source->arg.f_sve_pred_op.pg : 0;
    int pn = source != NULL ? (int)source->arg.f_sve_pred_op.pn
                            : find_preg_for_value(s->sve_pvir_builder,
                                                  instruction->operation.arch_state.input_value);
    int pg_off, pn_off;

    if (pg < 0 || pn < 0 || (pg_off = env_offset_sve_preg(pg)) < 0 ||
        (pn_off = env_offset_sve_preg(pn)) < 0) {
        return false;
    }
    emit_predtest_nzcv(vl_bytes, 8, pg_off, pn_off);
    return true;
}

static bool lower_pred_op(DisasContext *s, const A64SVEIR1Instruction *source,
                          const PvirInstruction *instruction)
{
    size_t vl_bytes = (size_t)s->vl;
    size_t pred_bytes = vl_bytes / 8u;
    const arg_sve_pred_op *p =
        source != NULL ? &source->arg.f_sve_pred_op : NULL;
    uint8_t pd, pg, pn, pm, element_bits;
    uint8_t subop;
    bool merge = false, setflags = false;
    int pd_off, pg_off, pn_off, pm_off;

    if (p != NULL) {
        pd = p->pd;
        pg = p->pg;
        pn = p->pn;
        pm = p->pm;
        element_bits = p->element_bits;
        subop = p->subop;
        merge = p->merge;
        setflags = p->setflags;
    } else {
        /* Direct PVIR fallback (PVIR_OP_AND/OR/XOR/SELECT/MOVE on predicate regs). */
        const PvirVectorOperation *vop = &instruction->operation.vector;
        int d = find_preg_for_value(s->sve_pvir_builder, vop->destination);
        int g = find_preg_for_value(s->sve_pvir_builder,
                                    vop->predicate.source.id);
        int n = find_preg_for_value(s->sve_pvir_builder, vop->operands[0]);
        int m = (vop->operand_count >= 2)
                    ? find_preg_for_value(s->sve_pvir_builder, vop->operands[1])
                    : n;
        if (d < 0) return false;
        pd = (uint8_t)d;
        pg = (g >= 0) ? (uint8_t)g : pd;
        pn = (n >= 0) ? (uint8_t)n : pd;
        pm = (m >= 0) ? (uint8_t)m : pn;
        element_bits = (uint8_t)vop->type.element_bits;
        if (instruction->opcode == PVIR_OP_OR) subop = SVE_PRED_OP_ORR;
        else if (instruction->opcode == PVIR_OP_XOR) subop = SVE_PRED_OP_EOR;
        else subop = SVE_PRED_OP_AND;
    }

    if ((pd_off = env_offset_sve_preg(pd)) < 0 ||
        (pg_off = env_offset_sve_preg(pg)) < 0 ||
        (pn_off = env_offset_sve_preg(pn)) < 0 ||
        (pm_off = env_offset_sve_preg(pm)) < 0) {
        return false;
    }

    if (subop == SVE_PRED_OP_BRKA || subop == SVE_PRED_OP_BRKB) {
        bool after = (subop == SVE_PRED_OP_BRKB);
        IR2_OPND seen = ra_alloc_itemp();
        IR2_OPND outb = ra_alloc_itemp();
        IR2_OPND pgb = ra_alloc_itemp();
        IR2_OPND pnb = ra_alloc_itemp();
        IR2_OPND cur = ra_alloc_itemp();

        li_d(seen, 0);
        for (size_t byte = 0; byte < pred_bytes; ++byte) {
            size_t wb = byte & ~(size_t)3u;
            unsigned sh = (unsigned)(byte - wb) * 8u;
            lata_env_ld_wu(pgb, pg_off + (int)wb);
            lata_env_ld_wu(pnb, pn_off + (int)wb);
            la_bstrpick_d(pgb, pgb, sh + 7u, sh);
            la_bstrpick_d(pnb, pnb, sh + 7u, sh);
            li_d(outb, 0);
            for (unsigned b = 0; b < 8u; ++b) {
                IR2_OPND l_inactive = ir2_opnd_new_type(IR2_OPND_LABEL);
                IR2_OPND l_next = ir2_opnd_new_type(IR2_OPND_LABEL);
                la_bstrpick_d(cur, pgb, b, b);
                la_beqz(cur, l_inactive);
                la_bstrpick_d(cur, pnb, b, b);
                if (after) {
                    /* BRKB: result = (!seen && !sbit); seen |= sbit */
                    la_or(seen, seen, cur);
                    la_xori(cur, seen, 1);
                } else {
                    /* BRKA: result = !old_seen; seen = old_seen | sbit */
                    IR2_OPND brk_set = ir2_opnd_new_type(IR2_OPND_LABEL);
                    IR2_OPND brk_done = ir2_opnd_new_type(IR2_OPND_LABEL);
                    la_bnez(seen, brk_set);
                    la_mov64(seen, cur);
                    li_d(cur, 1);
                    la_b(brk_done);
                    la_label(brk_set);
                    li_d(cur, 0);
                    la_label(brk_done);
                }
                la_b(l_next);
                la_label(l_inactive);
                if (merge) {
                    lata_env_ld_wu(cur, pd_off + (int)wb);
                    la_bstrpick_d(cur, cur, sh + b, sh + b);
                } else {
                    li_d(cur, 0);
                }
                la_label(l_next);
                if (b != 0) la_slli_d(cur, cur, b);
                la_or(outb, outb, cur);
            }
            lata_env_st_b(outb, pd_off + (int)byte);
        }
        ra_free_temp(cur);
        ra_free_temp(pnb);
        ra_free_temp(pgb);
        ra_free_temp(outb);
        ra_free_temp(seen);
    } else if (subop == SVE_PRED_OP_PFIRST || subop == SVE_PRED_OP_PNEXT) {
        size_t step = (subop == SVE_PRED_OP_PFIRST) ? 1u : (element_bits / 8u);
        size_t total_lanes = vl_bytes / step;
        IR2_OPND done = ra_alloc_itemp();
        IR2_OPND pgb = ra_alloc_itemp();
        IR2_OPND pdb = ra_alloc_itemp();
        IR2_OPND abit = ra_alloc_itemp();

        if (subop == SVE_PRED_OP_PFIRST) {
            li_d(done, 0);
            for (size_t l = 0; l < total_lanes; ++l) {
                size_t b = l / 8u;
                unsigned bit = (unsigned)(l % 8u);
                size_t wb = b & ~(size_t)3u;
                unsigned wsh = (unsigned)((b - wb) * 8u);
                IR2_OPND skip = ir2_opnd_new_type(IR2_OPND_LABEL);

                lata_env_ld_wu(pgb, pg_off + (int)wb);
                la_bstrpick_d(abit, pgb, wsh + bit, wsh + bit);
                la_beqz(abit, skip);
                la_bnez(done, skip);
                lata_env_ld_wu(pdb, pd_off + (int)wb);
                la_bstrpick_d(pdb, pdb, wsh + 7u, wsh);
                li_d(abit, 1 << bit);
                la_or(pdb, pdb, abit);
                lata_env_st_b(pdb, pd_off + (int)b);
                li_d(done, 1);
                la_label(skip);
            }
        } else {
            /* PNEXT: reuse done = found_last (then done), pdb = after_last. */
            IR2_OPND found_last = done;
            IR2_OPND after_last = pdb;
            li_d(found_last, 0);
            li_d(after_last, 0);
            for (size_t l = total_lanes; l > 0; --l) {
                size_t pbit = (l - 1u) * step;
                size_t b = pbit / 8u;
                unsigned bit = (unsigned)(pbit % 8u);
                size_t wb = b & ~(size_t)3u;
                unsigned wsh = (unsigned)((b - wb) * 8u);
                IR2_OPND skip = ir2_opnd_new_type(IR2_OPND_LABEL);

                la_bnez(found_last, skip);
                lata_env_ld_wu(pgb, pd_off + (int)wb);
                la_bstrpick_d(abit, pgb, wsh + bit, wsh + bit);
                la_beqz(abit, skip);
                li_d(after_last, (int64_t)l);
                li_d(found_last, 1);
                la_label(skip);
            }
            li_d(abit, 0);
            for (size_t b = 0; b < pred_bytes; ++b) {
                lata_env_st_b(abit, pd_off + (int)b);
            }
            li_d(done, 0);
            for (size_t l = 0; l < total_lanes; ++l) {
                size_t pbit = l * step;
                size_t b = pbit / 8u;
                unsigned bit = (unsigned)(pbit % 8u);
                size_t wb = b & ~(size_t)3u;
                unsigned wsh = (unsigned)((b - wb) * 8u);
                IR2_OPND skip = ir2_opnd_new_type(IR2_OPND_LABEL);

                la_bnez(done, skip);
                li_d(abit, (int64_t)l);
                la_bltu(abit, after_last, skip);
                lata_env_ld_wu(pgb, pg_off + (int)wb);
                la_bstrpick_d(abit, pgb, wsh + bit, wsh + bit);
                la_beqz(abit, skip);
                li_d(abit, 1 << bit);
                lata_env_st_b(abit, pd_off + (int)b);
                li_d(done, 1);
                la_label(skip);
            }
        }
        ra_free_temp(abit);
        ra_free_temp(pdb);
        ra_free_temp(pgb);
        ra_free_temp(done);
    } else {
        /* Predicate logical ops: AND/ORR/EOR/BIC/ORN/NOR/NAND/SEL */
        IR2_OPND g = ra_alloc_itemp();
        IR2_OPND n = ra_alloc_itemp();
        IR2_OPND m = ra_alloc_itemp();
        IR2_OPND outb = ra_alloc_itemp();

        for (size_t b = 0; b < pred_bytes; ++b) {
            size_t wb = b & ~(size_t)3u;
            unsigned wsh = (unsigned)((b - wb) * 8u);
            lata_env_ld_wu(g, pg_off + (int)wb);
            lata_env_ld_wu(n, pn_off + (int)wb);
            lata_env_ld_wu(m, pm_off + (int)wb);
            la_bstrpick_d(g, g, wsh + 7u, wsh);
            la_bstrpick_d(n, n, wsh + 7u, wsh);
            la_bstrpick_d(m, m, wsh + 7u, wsh);
            switch (subop) {
            case SVE_PRED_OP_AND:
                la_and(outb, n, m);
                la_and(outb, outb, g);
                break;
            case SVE_PRED_OP_BIC:
                la_xori(m, m, 0xff);
                la_and(outb, n, m);
                la_and(outb, outb, g);
                break;
            case SVE_PRED_OP_ORR:
                la_or(outb, n, m);
                la_and(outb, outb, g);
                break;
            case SVE_PRED_OP_ORN:
                la_xori(m, m, 0xff);
                la_or(outb, n, m);
                la_and(outb, outb, g);
                break;
            case SVE_PRED_OP_EOR:
                la_xor(outb, n, m);
                la_and(outb, outb, g);
                break;
            case SVE_PRED_OP_NOR:
                la_or(outb, n, m);
                la_xori(outb, outb, 0xff);
                la_and(outb, outb, g);
                break;
            case SVE_PRED_OP_NAND:
                la_and(outb, n, m);
                la_xori(outb, outb, 0xff);
                la_and(outb, outb, g);
                break;
            case SVE_PRED_OP_SEL:
            default:
                la_and(n, n, g);
                la_xori(g, g, 0xff);
                la_and(m, m, g);
                la_or(outb, n, m);
                break;
            }
            lata_env_st_b(outb, pd_off + (int)b);
        }
        ra_free_temp(outb);
        ra_free_temp(m);
        ra_free_temp(n);
        ra_free_temp(g);
    }

    if (setflags) {
        emit_predtest_nzcv(vl_bytes, (subop == SVE_PRED_OP_PNEXT) ? element_bits : 8u,
                           pg_off, pd_off);
    }
    return true;
}

static bool lower_cnt(DisasContext *s, const A64SVEIR1Instruction *source,
                      const PvirInstruction *instruction)
{
    size_t vl_bytes = (size_t)s->vl;
    const arg_sve_cnt *cnt = source != NULL ? &source->arg.f_sve_cnt : NULL;
    if (cnt == NULL) {
        /* Fallback for standalone PVIR_OP_REDUCE if called directly -> handled in lower_reduce_op */
        (void)instruction;
        return false;
    }
    if (cnt->rd == 31u) {
        return true;
    }
    size_t elem_bytes = cnt->element_bits / 8u;
    size_t lanes = vl_bytes / elem_bytes;

    if (cnt->subop == SVE_CNT_SUBOP_CNTP) {
        int pg_off = env_offset_sve_preg(cnt->pg);
        int pn_off = env_offset_sve_preg(cnt->pn);
        IR2_OPND acc = ra_alloc_itemp();
        IR2_OPND pw = ra_alloc_itemp();
        IR2_OPND nw = ra_alloc_itemp();
        IR2_OPND b = ra_alloc_itemp();
        size_t loaded_w = (size_t)-1;

        if (pg_off < 0 || pn_off < 0) {
            ra_free_temp(b);
            ra_free_temp(nw);
            ra_free_temp(pw);
            ra_free_temp(acc);
            return false;
        }
        li_d(acc, 0);
        for (size_t l = 0; l < lanes; ++l) {
            size_t pbit = l * elem_bytes;
            size_t wb = (pbit / 32u) * 4u;
            unsigned wsh = (unsigned)(pbit % 32u);
            if (wb != loaded_w) {
                lata_env_ld_wu(pw, pg_off + (int)wb);
                lata_env_ld_wu(nw, pn_off + (int)wb);
                loaded_w = wb;
            }
            la_and(b, pw, nw);
            la_bstrpick_d(b, b, wsh, wsh);
            la_add_d(acc, acc, b);
        }
        store_gpr_dst(cnt->rd, acc);
        ra_free_temp(b);
        ra_free_temp(nw);
        ra_free_temp(pw);
        ra_free_temp(acc);
        return true;
    }

    uint64_t delta =
        (uint64_t)sve_pred_count_val(lanes, cnt->pattern) * cnt->imm_mul;
    if (cnt->subop == SVE_CNT_SUBOP_CNT) {
        IR2_OPND d = ra_alloc_itemp();
        li_d(d, (int64_t)delta);
        store_gpr_dst(cnt->rd, d);
        ra_free_temp(d);
        return true;
    }
    IR2_OPND src = alloc_gpr_src(cnt->rd);
    IR2_OPND dst = ra_alloc_itemp();
    IR2_OPND d = ra_alloc_itemp();
    li_d(d, (int64_t)delta);
    if (cnt->subop == SVE_CNT_SUBOP_INC || cnt->subop == SVE_CNT_SUBOP_DEC) {
        if (cnt->subop == SVE_CNT_SUBOP_INC) la_add_d(dst, src, d);
        else la_sub_d(dst, src, d);
    } else if (!cnt->is_64) {
        /* 32-bit saturating SQINC/UQINC/SQDEC/UQDEC -> extend to 64, clamp to 32-bit range! */
        IR2_OPND lim = ra_alloc_itemp();
        IR2_OPND cond = ra_alloc_itemp();
        bool dec = (cnt->subop == SVE_CNT_SUBOP_SQDEC ||
                    cnt->subop == SVE_CNT_SUBOP_UQDEC);
        if (cnt->is_signed) {
            la_slli_w(dst, src, 0);
            if (dec) la_sub_d(dst, dst, d);
            else la_add_d(dst, dst, d);
            li_d(lim, dec ? INT32_MIN : INT32_MAX);
            if (dec) la_slt(cond, dst, lim);
            else la_slt(cond, lim, dst);
        } else {
            la_mov32_zx(dst, src);
            if (dec) la_sub_d(dst, dst, d);
            else la_add_d(dst, dst, d);
            li_d(lim, dec ? 0LL : (int64_t)UINT32_MAX);
            if (dec) la_slt(cond, dst, lim);
            else la_slt(cond, lim, dst);
        }
        la_masknez(dst, dst, cond);
        la_maskeqz(lim, lim, cond);
        la_or(dst, dst, lim);
        la_mov32_zx(dst, dst);
        ra_free_temp(cond);
        ra_free_temp(lim);
    } else {
        /* 64-bit saturating */
        IR2_OPND ov = ra_alloc_itemp();
        IR2_OPND sat = ra_alloc_itemp();
        bool dec = (cnt->subop == SVE_CNT_SUBOP_SQDEC ||
                    cnt->subop == SVE_CNT_SUBOP_UQDEC);
        if (dec) la_sub_d(dst, src, d);
        else la_add_d(dst, src, d);
        if (cnt->is_signed) {
            /* Signed overflow check for small positive delta <= 0x1000:
             * inc: src >= 0 && dst < src (or wrap negative across INT64_MAX) -> dst < src;
             * dec: dst > src (signed)! */
            if (dec) la_slt(ov, src, dst);
            else la_slt(ov, dst, src);
            li_d(sat, dec ? INT64_MIN : INT64_MAX);
        } else {
            if (dec) la_sltu(ov, src, dst);
            else la_sltu(ov, dst, src);
            li_d(sat, dec ? 0LL : -1LL);
        }
        la_masknez(dst, dst, ov);
        la_maskeqz(sat, sat, ov);
        la_or(dst, dst, sat);
        ra_free_temp(sat);
        ra_free_temp(ov);
    }
    store_gpr_dst(cnt->rd, dst);
    free_alloc_gpr(src);
    ra_free_temp(d);
    ra_free_temp(dst);
    return true;
}

static bool lower_last(DisasContext *s, const A64SVEIR1Instruction *source,
                       const PvirInstruction *instruction)
{
    size_t vl_bytes = (size_t)s->vl;
    const arg_sve_last *l = source != NULL ? &source->arg.f_sve_last : NULL;
    if (l == NULL) {
        (void)instruction;
        return false;
    }
    size_t elem_bytes = l->element_bits / 8u;
    size_t lanes = vl_bytes / elem_bytes;
    int pg_off = env_offset_sve_preg(l->pg);
    int zm_off = env_offset_zreg(l->zm);
    int zd_off = l->dst_is_vector ? env_offset_zreg(l->rd_zd) : -1;

    if (pg_off < 0 || zm_off < 0 || (l->dst_is_vector && zd_off < 0)) {
        return false;
    }
    IR2_OPND idx = ra_alloc_itemp();
    IR2_OPND any = ra_alloc_itemp();
    IR2_OPND pw = ra_alloc_itemp();
    IR2_OPND bit = ra_alloc_itemp();
    size_t loaded_w = (size_t)-1;

    li_d(idx, 0);
    li_d(any, 0);
    for (size_t lane = 0; lane < lanes; ++lane) {
        size_t pbit = lane * elem_bytes;
        size_t wb = (pbit / 32u) * 4u;
        unsigned sh = (unsigned)(pbit % 32u);
        IR2_OPND skip = ir2_opnd_new_type(IR2_OPND_LABEL);
        if (wb != loaded_w) {
            lata_env_ld_wu(pw, pg_off + (int)wb);
            loaded_w = wb;
        }
        la_bstrpick_d(bit, pw, sh, sh);
        la_beqz(bit, skip);
        li_d(any, 1);
        li_d(idx, (int64_t)(l->is_after ? ((lane + 1u) % lanes) : lane));
        la_label(skip);
    }

    IR2_OPND none_lbl = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND have_lbl = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND done_lbl = ir2_opnd_new_type(IR2_OPND_LABEL);
    la_beqz(any, none_lbl);
    la_label(have_lbl);
    {
        IR2_OPND val = pw;
        IR2_OPND base = bit;
        unsigned eb_shift = (elem_bytes == 1) ? 0u
                            : (elem_bytes == 2 ? 1u
                                               : (elem_bytes == 4 ? 2u : 3u));
        if (eb_shift != 0) la_slli_d(idx, idx, (int)eb_shift);
        li_d(base, zm_off);
        la_add_d(base, env_ir2_opnd, base);
        la_add_d(base, base, idx);
        emit_load_elem_mem(val, base, 0, (uint8_t)elem_bytes, false);
        if (!l->dst_is_vector) {
            if (l->rd_zd < 31u) {
                store_gpr_dst(l->rd_zd, val);
            }
        } else if (!l->is_cond) {
            IR2_OPND z0 = any;
            li_d(base, zd_off);
            la_add_d(base, env_ir2_opnd, base);
            li_d(z0, 0);
            for (size_t off = 0; off < vl_bytes; off += 8u) {
                la_st_d(z0, base, (int)off);
            }
            emit_store_elem_mem(val, base, 0, (uint8_t)elem_bytes);
        } else {
            li_d(base, zd_off);
            la_add_d(base, env_ir2_opnd, base);
            for (size_t lane = 0; lane < lanes; ++lane) {
                emit_store_elem_mem(val, base, (int)(lane * elem_bytes),
                                    (uint8_t)elem_bytes);
            }
        }
        la_b(done_lbl);
    }

    la_label(none_lbl);
    if (!l->is_cond) {
        li_d(idx, (int64_t)(l->is_after ? 0u : (lanes - 1u)));
        la_b(have_lbl);
    }
    la_label(done_lbl);

    ra_free_temp(bit);
    ra_free_temp(pw);
    ra_free_temp(any);
    ra_free_temp(idx);
    return true;
}

/*
 * Extended SVE memory: scalar+scalar, LD2/3/4 & ST2/3/4, LD1R, LD1RQ,
 * gather/scatter, LDNT1/STNT1.
 *
 * Every form in this family derives its addresses from a multi-register
 * structure, a replicating load, or an index vector, so there is no statically
 * bounded span a provider could prove stable for the whole access. The inline
 * lowering that used to live here could not satisfy the direct-access contract
 * (there was no per-address admission to make), so the checked helper path is
 * the only lowering that does: exec_mem_ext() drives every lane through
 * lata_guest_mem_load/store, and a guarded or unmapped lane raises the
 * architectural Data Abort with the status that probe reported.
 */
static bool lower_mem_ext(DisasContext *s, const A64SVEIR1Instruction *source,
                          const PvirInstruction *instruction)
{
    if (source == NULL) {
        return false;
    }
    if (env_offset_sve_preg(source->arg.f_sve_mem_ext.pg) < 0) {
        return false;
    }
    return emit_sve_helper_call(s, instruction, source);
}

static bool lower_fma_op(DisasContext *s, const A64SVEIR1Instruction *source,
                         const PvirInstruction *instruction)
{
    const PvirVectorOperation *op = &instruction->operation.vector;
    size_t vl_bytes = (size_t)s->vl;
    uint16_t ebits = op->type.element_bits;
    bool is_fp = op->type.kind == PVIR_ELEMENT_FLOAT;
    bool is_sub_prod = false;
    bool neg_acc = false;
    int zd, pg, zn, zm, za;
    int zd_off, zn_off, zm_off, za_off, pg_off, exp_off;
    bool cache_hit, const_fold;
    A64SVEPredicateMaskCacheKey mkey;

    if (source != NULL && source->insn_type == AARCH64_A64_SVE_FMLA) {
        const arg_sve_fp_ext *f = &source->arg.f_sve_fp_ext;
        zd = f->zd;
        pg = f->pg;
        zn = f->destructive_madd ? f->zd : f->zn;
        zm = f->zm;
        za = f->destructive_madd ? f->zn : f->zd;
        is_sub_prod = (f->subop == SVE_FP_EXT_FMLS || f->subop == SVE_FP_EXT_FNMLA);
        neg_acc = (f->subop == SVE_FP_EXT_FNMLA || f->subop == SVE_FP_EXT_FNMLS);
    } else if (source != NULL && source->insn_type == AARCH64_A64_SVE_WMUL) {
        const arg_sve_general *g = &source->arg.f_sve_general;
        zd = g->zd;
        pg = g->pg;
        zn = g->zn;
        zm = g->zm;
        za = g->zd;
        is_sub_prod = (g->subop == SVE_WMUL_MLS);
    } else {
        zd = find_zreg_for_value(s->sve_pvir_builder, op->destination);
        if (zd < 0) zd = find_zreg_for_value(s->sve_pvir_builder, op->merge_value);
        zn = find_zreg_for_value(s->sve_pvir_builder, op->operands[0]);
        zm = find_zreg_for_value(s->sve_pvir_builder, op->operands[1]);
        za = find_zreg_for_value(s->sve_pvir_builder, op->operands[2]);
        pg = find_preg_for_value(s->sve_pvir_builder, op->predicate.source.id);
    }

    if (zd < 0 || zn < 0 || zm < 0 || za < 0 ||
        (zd_off = env_offset_zreg(zd)) < 0 ||
        (zn_off = env_offset_zreg(zn)) < 0 ||
        (zm_off = env_offset_zreg(zm)) < 0 ||
        (za_off = env_offset_zreg(za)) < 0) {
        return false;
    }
    pg_off = (pg >= 0) ? env_offset_sve_preg(pg) : -1;
    exp_off = env_offset_sve_expanded_mask_scratch();
    if (!expanded_mask_cache_key_from_operation(op, &mkey)) {
        return false;
    }
    cache_hit = expanded_mask_cache_keys_equal(
        &s->sve_pvir_builder->expanded_mask_key, &mkey);
    const_fold = is_const_fold_predicate(s, &op->predicate,
                                         pg >= 0 ? (uint8_t)pg : 0);

    for (size_t byte = 0; byte < vl_bytes; byte += 32u) {
        IR2_OPND a = ra_alloc_ftemp();
        IR2_OPND b = ra_alloc_ftemp();
        IR2_OPND c = ra_alloc_ftemp();
        IR2_OPND m = ra_alloc_ftemp();
        IR2_OPND r = ra_alloc_ftemp();
        size_t rem = vl_bytes - byte;

        load_z_chunk(b, zn_off + (int)byte, rem);
        load_z_chunk(c, zm_off + (int)byte, rem);
        load_z_chunk(a, za_off + (int)byte, rem);
        if (!emit_load_chunk_mask(s, &op->predicate,
                                  pg >= 0 ? (uint8_t)pg : 0, ebits, byte, rem,
                                  pg_off, exp_off, const_fold, cache_hit, m)) {
            ra_free_temp(r); ra_free_temp(m); ra_free_temp(c); ra_free_temp(b); ra_free_temp(a);
            return false;
        }
        if (is_fp) {
            /* Inactive lanes get 0 so 0 * 0 +/- a never raises FP traps. */
            la_xvand_v(b, b, m);
            la_xvand_v(c, c, m);
            la_xvand_v(a, a, m);
            if (!is_sub_prod && !neg_acc) {
                if (ebits == 32) la_xvfmadd_s(r, b, c, a);
                else la_xvfmadd_d(r, b, c, a);
            } else if (is_sub_prod && !neg_acc) {
                /* a - b*c */
                if (ebits == 32) la_xvfnmsub_s(r, b, c, a);
                else la_xvfnmsub_d(r, b, c, a);
            } else if (is_sub_prod && neg_acc) {
                /* -(b*c + a) = -a - b*c */
                if (ebits == 32) la_xvfnmadd_s(r, b, c, a);
                else la_xvfnmadd_d(r, b, c, a);
            } else {
                /* b*c - a = -a + b*c */
                if (ebits == 32) la_xvfmsub_s(r, b, c, a);
                else la_xvfmsub_d(r, b, c, a);
            }
        } else {
            /* Integer MLA / MLS: r = a +/- (b * c) */
            switch (ebits) {
            case 8:
                la_xvmul_b(b, b, c);
                if (is_sub_prod) la_xvsub_b(r, a, b); else la_xvadd_b(r, a, b);
                break;
            case 16:
                la_xvmul_h(b, b, c);
                if (is_sub_prod) la_xvsub_h(r, a, b); else la_xvadd_h(r, a, b);
                break;
            case 32:
                la_xvmul_w(b, b, c);
                if (is_sub_prod) la_xvsub_w(r, a, b); else la_xvadd_w(r, a, b);
                break;
            default:
                la_xvmul_d(b, b, c);
                if (is_sub_prod) la_xvsub_d(r, a, b); else la_xvadd_d(r, a, b);
                break;
            }
        }
        load_z_chunk(a, zd_off + (int)byte, rem);
        la_xvbitsel_v(r, a, r, m);
        store_z_chunk(r, zd_off + (int)byte, rem);
        ra_free_temp(r);
        ra_free_temp(m);
        ra_free_temp(c);
        ra_free_temp(b);
        ra_free_temp(a);
    }
    if (!const_fold && !cache_hit) {
        s->sve_pvir_builder->expanded_mask_key = mkey;
    }
    return true;
}

static bool lower_fp_arith_op(DisasContext *s,
                              const A64SVEIR1Instruction *source,
                              const PvirInstruction *instruction)
{
    const PvirFPOpOperation *fop = &instruction->operation.fp;
    size_t vl_bytes = (size_t)s->vl;
    uint16_t ebits = fop->type.element_bits;
    int zd = find_zreg_for_value(s->sve_pvir_builder, fop->destination);
    int zn = find_zreg_for_value(s->sve_pvir_builder, fop->operands[0]);
    int zm = (fop->operands[1] != PVIR_NO_VALUE)
                 ? find_zreg_for_value(s->sve_pvir_builder, fop->operands[1])
                 : zn;
    int pg = (fop->predicate.source.kind == PVIR_PREDICATE_SOURCE_PREDICATE)
                 ? find_preg_for_value(s->sve_pvir_builder, fop->predicate.source.id)
                 : 0;
    if (source != NULL) {
        zd = source->arg.f_sve_fp_ext.zd;
        zn = source->arg.f_sve_fp_ext.zn;
        zm = source->arg.f_sve_fp_ext.zm;
        pg = source->arg.f_sve_fp_ext.pg;
    } else if (zd < 0 && fop->merge_value != PVIR_NO_VALUE) {
        zd = find_zreg_for_value(s->sve_pvir_builder, fop->merge_value);
    }
    int zd_off, zn_off, zm_off, pg_off, exp_off;
    bool cache_hit, const_fold;
    A64SVEPredicateMaskCacheKey mkey = {0};

    if (zd < 0 || zn < 0 || (ebits != 32 && ebits != 64) ||
        (zd_off = env_offset_zreg(zd)) < 0 ||
        (zn_off = env_offset_zreg(zn)) < 0 ||
        (zm_off = env_offset_zreg(zm >= 0 ? zm : zn)) < 0) {
        return false;
    }
    pg_off = (pg >= 0) ? env_offset_sve_preg(pg) : -1;
    exp_off = env_offset_sve_expanded_mask_scratch();
    mkey.valid = (fop->predicate.form == PVIR_PREDICATE_VECTOR_MASK);
    mkey.source = fop->predicate.source;
    mkey.form = fop->predicate.form;
    mkey.lanes = fop->predicate.lanes;
    mkey.element_bits = ebits;
    mkey.vector_bits = (uint32_t)vl_bytes * 8u;
    cache_hit = mkey.valid && expanded_mask_cache_keys_equal(
                                  &s->sve_pvir_builder->expanded_mask_key, &mkey);
    const_fold = is_const_fold_predicate(s, &fop->predicate,
                                         pg >= 0 ? (uint8_t)pg : 0);

    for (size_t byte = 0; byte < vl_bytes; byte += 32u) {
        IR2_OPND oldv = ra_alloc_ftemp();
        IR2_OPND a = ra_alloc_ftemp();
        IR2_OPND b = ra_alloc_ftemp();
        IR2_OPND mask = ra_alloc_ftemp();
        IR2_OPND res = ra_alloc_ftemp();
        size_t rem = vl_bytes - byte;

        load_z_chunk(oldv, zd_off + (int)byte, rem);
        load_z_chunk(a, zn_off + (int)byte, rem);
        load_z_chunk(b, zm_off + (int)byte, rem);
        if (!emit_load_chunk_mask(s, &fop->predicate,
                                  pg >= 0 ? (uint8_t)pg : 0, ebits, byte, rem,
                                  pg_off, exp_off, const_fold, cache_hit, mask)) {
            ra_free_temp(res); ra_free_temp(mask); ra_free_temp(b); ra_free_temp(a); ra_free_temp(oldv);
            return false;
        }

        if (fop->kind == PVIR_FP_RECPS || fop->kind == PVIR_FP_RSQRTS) {
            /* FRECPS: 2.0 - a*b (if inf*0 or 0*inf -> 2.0). FRSQRTS: (3.0 - a*b) / 2.0 (if inf*0 -> 1.5). */
            uint64_t two = (ebits == 64) ? UINT64_C(0x4000000000000000) : UINT64_C(0x4000000040000000);
            uint64_t three = (ebits == 64) ? UINT64_C(0x4008000000000000) : UINT64_C(0x4040000040400000);
            uint64_t half = (ebits == 64) ? UINT64_C(0x3fe0000000000000) : UINT64_C(0x3f0000003f000000);
            /* Multiply active a*b? Unpredicated! Use oldv as qNaN check, res as 2.0/3.0 */
            if (ebits == 32) la_xvfmul_s(oldv, a, b);
            else la_xvfmul_d(oldv, a, b);
            emit_xv_broadcast_u64(res, (fop->kind == PVIR_FP_RECPS) ? two : three);
            /* Zero invalid (inf * 0 -> NaN? Wait check if either is inf/0 vs normal NaN: if neither operand is NaN and product IS NaN -> inf*0 -> step gives 2.0 or 1.5!) */
            /* Using a^a check for input NaN: keep oldv if either input was NaN, else if prod is NaN return constant! */
            if (ebits == 32) {
                la_xvfcmp_cond_s(mask, a, b, FCMP_COND_CUN); /* input was NaN */
                la_xvfsub_s(res, res, oldv);
                if (fop->kind == PVIR_FP_RSQRTS) {
                    emit_xv_broadcast_u64(oldv, half);
                    la_xvfmul_s(res, res, oldv);
                }
                /* Detect 0*inf -> res became NaN when inputs were NOT NaN! */
                la_xvfcmp_cond_s(oldv, res, res, FCMP_COND_CUN);
                la_xvandn_v(oldv, mask, oldv); /* oldv = !input_nan && result_nan */
                emit_xv_broadcast_u64(mask, (fop->kind == PVIR_FP_RECPS) ? two :
                                                ((ebits == 64) ? UINT64_C(0x3ff8000000000000)
                                                               : UINT64_C(0x3fc000003fc00000)));
                la_xvbitsel_v(res, res, mask, oldv);
            } else {
                la_xvfcmp_cond_d(mask, a, b, FCMP_COND_CUN);
                la_xvfsub_d(res, res, oldv);
                if (fop->kind == PVIR_FP_RSQRTS) {
                    emit_xv_broadcast_u64(oldv, half);
                    la_xvfmul_d(res, res, oldv);
                }
                la_xvfcmp_cond_d(oldv, res, res, FCMP_COND_CUN);
                la_xvandn_v(oldv, mask, oldv);
                emit_xv_broadcast_u64(mask, (fop->kind == PVIR_FP_RECPS) ? two
                                                                         : UINT64_C(0x3ff8000000000000));
                la_xvbitsel_v(res, res, mask, oldv);
            }
            store_z_chunk(res, zd_off + (int)byte, rem);
            ra_free_temp(res); ra_free_temp(mask); ra_free_temp(b); ra_free_temp(a); ra_free_temp(oldv);
            continue;
        }

        /* Replace inactive inputs with 0.0 (or 1.0 for DIV/SQRT/RECPE/RSQRTE) before computing. */
        if (fop->kind == PVIR_FP_DIV || fop->kind == PVIR_FP_SQRT ||
            fop->kind == PVIR_FP_RECPE || fop->kind == PVIR_FP_RSQRTE) {
            emit_xv_broadcast_u64(res, (ebits == 64) ? UINT64_C(0x3ff0000000000000)
                                                     : UINT64_C(0x3f8000003f800000));
        } else {
            la_xvxor_v(res, res, res);
        }
        la_xvbitsel_v(a, res, a, mask);
        la_xvbitsel_v(b, res, b, mask);

        if (ebits == 32) {
            switch (fop->kind) {
            case PVIR_FP_ADD: la_xvfadd_s(res, a, b); break;
            case PVIR_FP_SUB: la_xvfsub_s(res, a, b); break;
            case PVIR_FP_MUL: la_xvfmul_s(res, a, b); break;
            case PVIR_FP_DIV: la_xvfdiv_s(res, a, b); break;
            case PVIR_FP_MINNM: la_xvfmin_s(res, a, b); break;
            case PVIR_FP_MAXNM: la_xvfmax_s(res, a, b); break;
            case PVIR_FP_MIN:
            case PVIR_FP_MAX: {
                if (fop->kind == PVIR_FP_MIN) la_xvfmin_s(res, a, b);
                else la_xvfmax_s(res, a, b);
                la_xvfcmp_cond_s(a, a, b, FCMP_COND_CUN);
                emit_xv_broadcast_u64(b, UINT64_C(0x7fc000007fc00000));
                la_xvbitsel_v(res, res, b, a);
                break;
            }
            case PVIR_FP_SQRT: la_xvfsqrt_s(res, a); break;
            case PVIR_FP_RECPE: la_xvfrecip_s(res, a); break;
            default: la_xvfrsqrt_s(res, a); break;
            }
        } else {
            switch (fop->kind) {
            case PVIR_FP_ADD: la_xvfadd_d(res, a, b); break;
            case PVIR_FP_SUB: la_xvfsub_d(res, a, b); break;
            case PVIR_FP_MUL: la_xvfmul_d(res, a, b); break;
            case PVIR_FP_DIV: la_xvfdiv_d(res, a, b); break;
            case PVIR_FP_MINNM: la_xvfmin_d(res, a, b); break;
            case PVIR_FP_MAXNM: la_xvfmax_d(res, a, b); break;
            case PVIR_FP_MIN:
            case PVIR_FP_MAX: {
                if (fop->kind == PVIR_FP_MIN) la_xvfmin_d(res, a, b);
                else la_xvfmax_d(res, a, b);
                la_xvfcmp_cond_d(a, a, b, FCMP_COND_CUN);
                emit_xv_broadcast_u64(b, UINT64_C(0x7ff8000000000000));
                la_xvbitsel_v(res, res, b, a);
                break;
            }
            case PVIR_FP_SQRT: la_xvfsqrt_d(res, a); break;
            case PVIR_FP_RECPE: la_xvfrecip_d(res, a); break;
            default: la_xvfrsqrt_d(res, a); break;
            }
        }
        if (fop->inactive_policy == PVIR_INACTIVE_MERGE) {
            la_xvbitsel_v(res, oldv, res, mask);
        } else {
            la_xvand_v(res, res, mask);
        }
        store_z_chunk(res, zd_off + (int)byte, rem);
        ra_free_temp(res);
        ra_free_temp(mask);
        ra_free_temp(b);
        ra_free_temp(a);
        ra_free_temp(oldv);
    }
    if (!const_fold && mkey.valid && !cache_hit) {
        s->sve_pvir_builder->expanded_mask_key = mkey;
    }
    return true;
}

static bool lower_compare_op(DisasContext *s,
                             const A64SVEIR1Instruction *source,
                             const PvirInstruction *instruction)
{
    const PvirVectorOperation *op = &instruction->operation.vector;
    size_t vl_bytes = (size_t)s->vl;
    bool is_float = (op->operand_types[0].kind == PVIR_ELEMENT_FLOAT);
    uint16_t ebits = op->operand_types[0].element_bits;
    uint8_t elem_bytes = (uint8_t)(ebits / 8u);
    PvirCompareCondition cond = op->compare_condition;
    bool is_uo = (source != NULL && source->insn_type == AARCH64_A64_SVE_FCMP &&
                  source->arg.f_sve_ext_op.subop == PVIR_COMPARE_NONE);
    int pd = (source != NULL)
                 ? (int)source->arg.f_sve_ext_op.zd_pd
                 : find_preg_for_value(s->sve_pvir_builder, op->destination);
    int zn = find_zreg_for_value(s->sve_pvir_builder, op->operands[0]);
    int zm = (op->operand_kinds[1] == PVIR_OPERAND_VECTOR)
                 ? find_zreg_for_value(s->sve_pvir_builder, op->operands[1])
                 : -1;
    int pg = find_preg_for_value(s->sve_pvir_builder, op->predicate.source.id);
    int pd_off, zn_off, pg_off, zm_off = -1, exp_off;
    bool cache_hit, const_fold;
    A64SVEPredicateMaskCacheKey mkey;

    if (source != NULL) {
        pd = source->arg.f_sve_ext_op.zd_pd;
        zn = source->arg.f_sve_ext_op.zn;
        pg = source->arg.f_sve_ext_op.pg;
        zm = source->arg.f_sve_ext_op.cmp_with_imm ? -1 : source->arg.f_sve_ext_op.zm;
    }
    if (pd < 0) pd = 0;
    if (zn < 0 || (pd_off = env_offset_sve_preg(pd)) < 0 ||
        (zn_off = env_offset_zreg(zn)) < 0) {
        return false;
    }
    if (op->operand_kinds[1] == PVIR_OPERAND_VECTOR &&
        (zm < 0 || (zm_off = env_offset_zreg(zm)) < 0)) {
        return false;
    }
    pg_off = (pg >= 0) ? env_offset_sve_preg(pg) : -1;
    exp_off = env_offset_sve_expanded_mask_scratch();
    if (!expanded_mask_cache_key_from_operation(op, &mkey)) {
        return false;
    }
    cache_hit = expanded_mask_cache_keys_equal(
        &s->sve_pvir_builder->expanded_mask_key, &mkey);
    const_fold = is_const_fold_predicate(s, &op->predicate,
                                         pg >= 0 ? (uint8_t)pg : 0);

    for (size_t byte = 0; byte < vl_bytes; byte += 32u) {
        IR2_OPND lhs = ra_alloc_ftemp();
        IR2_OPND rhs = ra_alloc_ftemp();
        IR2_OPND cmp = ra_alloc_ftemp();
        IR2_OPND mask = ra_alloc_ftemp();
        size_t rem = vl_bytes - byte;
        size_t chunk_bytes = rem < 32u ? rem : 32u;

        load_z_chunk(lhs, zn_off + (int)byte, rem);
        if (op->operand_kinds[1] == PVIR_OPERAND_VECTOR) {
            load_z_chunk(rhs, zm_off + (int)byte, rem);
        } else if (op->operand_kinds[1] == PVIR_OPERAND_SCALAR) {
            int r = find_gpr_for_value(op->operands[1]);
            IR2_OPND gr = (r >= 0) ? alloc_gpr_src(r) : zero_ir2_opnd;
            la_xvinsgr2vr_d(rhs, gr, 0);
            la_xvinsgr2vr_d(rhs, gr, 2);
            switch (ebits) {
            case 8: la_xvreplve0_b(rhs, rhs); break;
            case 16: la_xvreplve0_h(rhs, rhs); break;
            case 32: la_xvreplve0_w(rhs, rhs); break;
            default: la_xvreplve0_d(rhs, rhs); break;
            }
            if (r >= 0) free_alloc_gpr(gr);
        } else {
            emit_xv_broadcast_u64(rhs,
                                  repeat_elem_to_u64(op->operand_immediates[1], ebits));
        }

        if (!emit_load_chunk_mask(s, &op->predicate,
                                  pg >= 0 ? (uint8_t)pg : 0, ebits, byte, rem,
                                  pg_off, exp_off, const_fold, cache_hit, mask)) {
            ra_free_temp(mask); ra_free_temp(cmp); ra_free_temp(rhs); ra_free_temp(lhs);
            return false;
        }
        if (is_float) {
            /* Mask inactive inputs to 0.0 so NaNs in inactive lanes don't signal quiet/signaling trap if unordered. */
            int fcond;
            IR2_OPND j = lhs, k = rhs;
            if (is_uo) {
                fcond = FCMP_COND_CUN;
            } else {
                switch (cond) {
                case PVIR_COMPARE_EQ: fcond = FCMP_COND_CEQ; break;
                case PVIR_COMPARE_NE: fcond = FCMP_COND_CUNE; break;
                case PVIR_COMPARE_LT: fcond = FCMP_COND_CLT; break;
                case PVIR_COMPARE_LE: fcond = FCMP_COND_CLE; break;
                case PVIR_COMPARE_GT: fcond = FCMP_COND_CLT; j = rhs; k = lhs; break;
                case PVIR_COMPARE_GE: fcond = FCMP_COND_CLE; j = rhs; k = lhs; break;
                default: fcond = FCMP_COND_CEQ; break;
                }
            }
            if (ebits == 32) la_xvfcmp_cond_s(cmp, j, k, fcond);
            else la_xvfcmp_cond_d(cmp, j, k, fcond);
        } else {
            IR2_OPND j = lhs, k = rhs;
            bool inv = (cond == PVIR_COMPARE_NE);
            if (cond == PVIR_COMPARE_GT || cond == PVIR_COMPARE_GE ||
                cond == PVIR_COMPARE_GTU || cond == PVIR_COMPARE_GEU) {
                j = rhs;
                k = lhs;
            }
            bool use_eq = (cond == PVIR_COMPARE_EQ || cond == PVIR_COMPARE_NE);
            bool use_le = (cond == PVIR_COMPARE_LE || cond == PVIR_COMPARE_GE ||
                           cond == PVIR_COMPARE_LEU || cond == PVIR_COMPARE_GEU);
            bool uns = (cond >= PVIR_COMPARE_LTU && cond <= PVIR_COMPARE_GEU);
            switch (ebits) {
            case 8:
                if (use_eq) la_xvseq_b(cmp, j, k);
                else if (use_le) uns ? la_xvsle_bu(cmp, j, k) : la_xvsle_b(cmp, j, k);
                else uns ? la_xvslt_bu(cmp, j, k) : la_xvslt_b(cmp, j, k);
                break;
            case 16:
                if (use_eq) la_xvseq_h(cmp, j, k);
                else if (use_le) uns ? la_xvsle_hu(cmp, j, k) : la_xvsle_h(cmp, j, k);
                else uns ? la_xvslt_hu(cmp, j, k) : la_xvslt_h(cmp, j, k);
                break;
            case 32:
                if (use_eq) la_xvseq_w(cmp, j, k);
                else if (use_le) uns ? la_xvsle_wu(cmp, j, k) : la_xvsle_w(cmp, j, k);
                else uns ? la_xvslt_wu(cmp, j, k) : la_xvslt_w(cmp, j, k);
                break;
            default:
                if (use_eq) la_xvseq_d(cmp, j, k);
                else if (use_le) uns ? la_xvsle_du(cmp, j, k) : la_xvsle_d(cmp, j, k);
                else uns ? la_xvslt_du(cmp, j, k) : la_xvslt_d(cmp, j, k);
                break;
            }
            if (inv) {
                la_xvnori_b(cmp, cmp, 0);
            }
        }
        la_xvand_v(cmp, cmp, mask);

        /* Pack element bit 0 from cmp into predicate bytes pd_off + byte/8 + 0..3. */
        {
            IR2_OPND qw = ra_alloc_itemp();
            IR2_OPND pack = ra_alloc_itemp();
            IR2_OPND bit = ra_alloc_itemp();
            for (unsigned q = 0; q < 4u && (size_t)q * 8u < chunk_bytes; ++q) {
                la_xvpickve2gr_d(qw, cmp, (int)q);
                li_d(pack, 0);
                for (unsigned b = 0; b < 8u && (size_t)q * 8u + b < chunk_bytes; b += elem_bytes) {
                    la_bstrpick_d(bit, qw, b * 8u, b * 8u);
                    if (b != 0) la_slli_d(bit, bit, b);
                    la_or(pack, pack, bit);
                }
                lata_env_st_b(pack, pd_off + (int)(byte / 8u) + (int)q);
            }
            ra_free_temp(bit);
            ra_free_temp(pack);
            ra_free_temp(qw);
        }

        ra_free_temp(mask);
        ra_free_temp(cmp);
        ra_free_temp(rhs);
        ra_free_temp(lhs);
    }
    if (!const_fold && !cache_hit) {
        s->sve_pvir_builder->expanded_mask_key = mkey;
    }
    if (pg_off >= 0) {
        emit_predtest_nzcv(vl_bytes, (uint8_t)ebits, pg_off, pd_off);
    }
    return true;
}

static bool lower_convert_op(DisasContext *s,
                             const A64SVEIR1Instruction *source,
                             const PvirInstruction *instruction)
{
    const PvirConvertOperation *cvt = &instruction->operation.convert;
    size_t vl_bytes = (size_t)s->vl;
    uint16_t sbits = cvt->source_type.element_bits;
    uint16_t dbits = cvt->destination_type.element_bits;
    int zd = source != NULL ? (int)source->arg.f_sve_ext_op.zd_pd
                            : find_zreg_for_value(s->sve_pvir_builder, cvt->destination);
    int zn = find_zreg_for_value(s->sve_pvir_builder, cvt->source);
    int pg = source != NULL ? (int)source->arg.f_sve_ext_op.pg : -1;
    bool src_is_flt = (cvt->source_type.kind == PVIR_ELEMENT_FLOAT);
    bool dst_is_flt = (cvt->destination_type.kind == PVIR_ELEMENT_FLOAT);
    bool src_signed = (cvt->source_type.signedness == PVIR_SIGNED);
    int dst_signed = (cvt->destination_type.signedness == PVIR_SIGNED);
    int zd_off, zn_off, pg_off;

    if (source != NULL) zn = source->arg.f_sve_ext_op.zn;
    if (zd < 0) zd = zn >= 0 ? zn : 0;
    if (zn < 0 || (zd_off = env_offset_zreg(zd)) < 0 ||
        (zn_off = env_offset_zreg(zn)) < 0) {
        return false;
    }
    pg_off = (pg >= 0) ? env_offset_sve_preg(pg) : -1;

    if (sbits == dbits) {
        /* Same-width 32<->32 or 64<->64 vectorized LASX conversion! */
        for (size_t byte = 0; byte < vl_bytes; byte += 32u) {
            IR2_OPND oldv = ra_alloc_ftemp();
            IR2_OPND srcv = ra_alloc_ftemp();
            IR2_OPND res = ra_alloc_ftemp();
            IR2_OPND mask = ra_alloc_ftemp();
            size_t rem = vl_bytes - byte;

            load_z_chunk(oldv, zd_off + (int)byte, rem);
            load_z_chunk(srcv, zn_off + (int)byte, rem);
            if (pg_off >= 0) {
                if (!lower_packed_predicate_mask(mask, pg_off + (int)(byte / 8u), sbits)) {
                    ra_free_temp(mask); ra_free_temp(res); ra_free_temp(srcv); ra_free_temp(oldv);
                    return false;
                }
                la_xvand_v(srcv, srcv, mask);
            } else {
                la_xvseq_b(mask, mask, mask);
            }
            if (src_is_flt && !dst_is_flt) {
                if (sbits == 32) {
                    if (dst_signed) la_xvftintrz_w_s(res, srcv);
                    else la_xvftintrz_wu_s(res, srcv);
                } else {
                    if (dst_signed) la_xvftintrz_l_d(res, srcv);
                    else la_xvftintrz_lu_d(res, srcv);
                }
            } else if (!src_is_flt && dst_is_flt) {
                if (sbits == 32) {
                    if (src_signed) la_xvffint_s_w(res, srcv);
                    else la_xvffint_s_wu(res, srcv);
                } else {
                    if (src_signed) la_xvffint_d_l(res, srcv);
                    else la_xvffint_d_lu(res, srcv);
                }
            } else {
                la_xvor_v(res, srcv, srcv);
            }
            la_xvbitsel_v(res, oldv, res, mask);
            store_z_chunk(res, zd_off + (int)byte, rem);
            ra_free_temp(mask);
            ra_free_temp(res);
            ra_free_temp(srcv);
            ra_free_temp(oldv);
        }
        return true;
    }

    /* Mixed 64<->32 container conversion: iterate 64-bit containers (uses only 1 ftemp!). */
    size_t lanes = vl_bytes / 8u;
    IR2_OPND fval = ra_alloc_ftemp();
    IR2_OPND zn_base = ra_alloc_itemp();
    IR2_OPND zd_base = ra_alloc_itemp();
    IR2_OPND pw = ra_alloc_itemp();
    IR2_OPND act = ra_alloc_itemp();
    IR2_OPND g = ra_alloc_itemp();

    li_d(zn_base, zn_off);
    la_add_d(zn_base, env_ir2_opnd, zn_base);
    li_d(zd_base, zd_off);
    la_add_d(zd_base, env_ir2_opnd, zd_base);

    for (size_t lane = 0; lane < lanes; ++lane) {
        size_t off = lane * 8u;
        size_t wb = (off / 32u) * 4u;
        unsigned sh = (unsigned)(off % 32u);
        IR2_OPND skip = ir2_opnd_new_type(IR2_OPND_LABEL);

        if (pg_off >= 0) {
            lata_env_ld_wu(pw, pg_off + (int)wb);
            la_bstrpick_d(act, pw, sh, sh);
            la_beqz(act, skip);
        }
        if (sbits == 32 && !src_is_flt) {
            emit_load_elem_mem(g, zn_base, (int)off, 4, src_signed);
            la_movgr2fr_d(fval, g);
            la_vffint_d_l(fval, fval); /* 64-bit int -> f64 (since w/wu already extended to 64-bit!) */
        } else if (sbits == 32 && src_is_flt) {
            la_ld_wu(g, zn_base, (int)off);
            la_movgr2fr_w(fval, g);
            la_fcvt_d_s(fval, fval);
            if (!dst_is_flt) {
                if (dst_signed) la_vftintrz_l_d(fval, fval);
                else la_vftintrz_lu_d(fval, fval);
            }
        } else if (sbits == 64 && !src_is_flt) {
            la_ld_d(g, zn_base, (int)off);
            la_movgr2fr_d(fval, g);
            if (src_signed) la_vffint_d_l(fval, fval);
            else la_vffint_d_lu(fval, fval);
            if (dbits == 32) {
                la_fcvt_s_d(fval, fval);
            }
        } else {
            /* 64-bit float -> 32-bit float or 32-bit int */
            la_ld_d(g, zn_base, (int)off);
            la_movgr2fr_d(fval, g);
            if (dst_is_flt) {
                la_fcvt_s_d(fval, fval);
            } else {
                if (dst_signed) {
                    la_vftintrz_l_d(fval, fval);
                    la_movfr2gr_d(g, fval);
                    IR2_OPND lmin = ra_alloc_itemp();
                    IR2_OPND lmax = ra_alloc_itemp();
                    IR2_OPND c = ra_alloc_itemp();
                    li_d(lmin, INT32_MIN);
                    li_d(lmax, INT32_MAX);
                    la_slt(c, g, lmin); la_masknez(g, g, c); la_maskeqz(lmin, lmin, c); la_or(g, g, lmin);
                    la_slt(c, lmax, g); la_masknez(g, g, c); la_maskeqz(lmax, lmax, c); la_or(g, g, lmax);
                    ra_free_temp(c); ra_free_temp(lmax); ra_free_temp(lmin);
                } else {
                    /* Unsigned 32-bit saturating from double via f64->i64 clamp [0..0xffffffff] */
                    IR2_OPND lmax = ra_alloc_itemp();
                    IR2_OPND c = ra_alloc_itemp();
                    la_ftintrz_l_d(fval, fval);
                    la_movfr2gr_d(g, fval);
                    la_slt(c, g, zero_ir2_opnd);
                    la_masknez(g, g, c);
                    li_d(lmax, (int64_t)UINT32_MAX);
                    la_slt(c, lmax, g);
                    la_masknez(g, g, c);
                    la_maskeqz(lmax, lmax, c);
                    la_or(g, g, lmax);
                    ra_free_temp(c); ra_free_temp(lmax);
                }
                la_mov32_zx(g, g);
                la_st_d(g, zd_base, (int)off);
                la_b(skip);
            }
        }
        if (dbits == 32 && dst_is_flt) {
            la_movfr2gr_s(g, fval);
            la_mov32_zx(g, g);
            la_st_d(g, zd_base, (int)off);
        } else {
            la_movfr2gr_d(g, fval);
            la_st_d(g, zd_base, (int)off);
        }
        la_label(skip);
    }

    ra_free_temp(g);
    ra_free_temp(act);
    ra_free_temp(pw);
    ra_free_temp(zd_base);
    ra_free_temp(zn_base);
    ra_free_temp(fval);
    return true;
}

static bool lower_reduce_op(DisasContext *s, const A64SVEIR1Instruction *source,
                            const PvirInstruction *instruction)
{
    const PvirReduceOperation *red = &instruction->operation.reduce;
    size_t vl_bytes = (size_t)s->vl;
    uint16_t ebits = red->type.element_bits;
    uint8_t eb = (uint8_t)(ebits / 8u);
    size_t lanes = vl_bytes / eb;
    bool is_float = red->type.kind == PVIR_ELEMENT_FLOAT;
    bool is_signed = red->type.signedness == PVIR_SIGNED;
    int zd = find_zreg_for_value(s->sve_pvir_builder, red->destination);
    int zn = find_zreg_for_value(s->sve_pvir_builder, red->source);
    int pg = (red->predicate.source.kind == PVIR_PREDICATE_SOURCE_PREDICATE)
                 ? find_preg_for_value(s->sve_pvir_builder, red->predicate.source.id)
                 : -1;
    if (source != NULL && (source->insn_type == AARCH64_A64_SVE_FREDUCE ||
                           source->insn_type == AARCH64_A64_SVE_IREDUCE)) {
        zd = source->arg.f_sve_general.zd;
        zn = source->arg.f_sve_general.zn;
        pg = source->arg.f_sve_general.pg;
    } else if (zd < 0) {
        zd = 0;
    }
    int zd_off, zn_off, pg_off;
    if (zn < 0 || (zd_off = env_offset_zreg(zd)) < 0 ||
        (zn_off = env_offset_zreg(zn)) < 0) {
        return false;
    }
    pg_off = (pg >= 0) ? env_offset_sve_preg(pg) : -1;

    if (is_float) {
        IR2_OPND acc = ra_alloc_ftemp();
        IR2_OPND val = ra_alloc_ftemp();
        IR2_OPND zb = ra_alloc_itemp();
        IR2_OPND pw = ra_alloc_itemp();
        IR2_OPND act = ra_alloc_itemp();
        IR2_OPND g = ra_alloc_itemp();

        if (red->kind == PVIR_REDUCE_ADD_ORDERED) {
            li_d(zb, zd_off); la_add_d(zb, env_ir2_opnd, zb);
            la_ld_d(g, zb, 0);
            if (ebits == 32) la_movgr2fr_w(acc, g); else la_movgr2fr_d(acc, g);
        } else if (red->kind == PVIR_REDUCE_ADD) {
            li_d(g, 0);
            if (ebits == 32) la_movgr2fr_w(acc, g); else la_movgr2fr_d(acc, g);
        } else if (red->kind == PVIR_REDUCE_MAX || red->kind == PVIR_REDUCE_MAXNM) {
            li_d(g, (int64_t)(ebits == 32 ? UINT64_C(0xff800000) : UINT64_C(0xfff0000000000000)));
            if (ebits == 32) la_movgr2fr_w(acc, g); else la_movgr2fr_d(acc, g);
        } else {
            li_d(g, (int64_t)(ebits == 32 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000)));
            if (ebits == 32) la_movgr2fr_w(acc, g); else la_movgr2fr_d(acc, g);
        }
        li_d(zb, zn_off);
        la_add_d(zb, env_ir2_opnd, zb);

        for (size_t lane = 0; lane < lanes; ++lane) {
            size_t vb = lane * eb;
            size_t wb = (vb / 32u) * 4u;
            unsigned sh = (unsigned)(vb % 32u);
            IR2_OPND sk = ir2_opnd_new_type(IR2_OPND_LABEL);
            if (pg_off >= 0) {
                lata_env_ld_wu(pw, pg_off + (int)wb);
                la_bstrpick_d(act, pw, sh, sh);
                la_beqz(act, sk);
            }
            if (ebits == 32) {
                la_ld_wu(g, zb, (int)vb);
                la_movgr2fr_w(val, g);
            } else {
                la_ld_d(g, zb, (int)vb);
                la_movgr2fr_d(val, g);
            }
            if (red->kind == PVIR_REDUCE_ADD || red->kind == PVIR_REDUCE_ADD_ORDERED) {
                if (ebits == 32) la_fadd_s(acc, acc, val);
                else la_fadd_d(acc, acc, val);
            } else if (red->kind == PVIR_REDUCE_MAXNM) {
                if (ebits == 32) la_fmax_s(acc, acc, val);
                else la_fmax_d(acc, acc, val);
            } else if (red->kind == PVIR_REDUCE_MINNM) {
                if (ebits == 32) la_fmin_s(acc, acc, val);
                else la_fmin_d(acc, acc, val);
            } else {
                /* IEEE FMAX/FMIN: any NaN produces canonical quiet NaN */
                IR2_OPND cond = ra_alloc_ftemp();
                IR2_OPND qnan = ra_alloc_ftemp();
                if (ebits == 32) {
                    la_xvfcmp_cond_s(cond, acc, val, FCMP_COND_CUN);
                    if (red->kind == PVIR_REDUCE_MAX) la_fmax_s(acc, acc, val);
                    else la_fmin_s(acc, acc, val);
                    li_d(g, 0x7fc00000);
                    la_movgr2fr_w(qnan, g);
                } else {
                    la_xvfcmp_cond_d(cond, acc, val, FCMP_COND_CUN);
                    if (red->kind == PVIR_REDUCE_MAX) la_fmax_d(acc, acc, val);
                    else la_fmin_d(acc, acc, val);
                    li_d(g, (int64_t)UINT64_C(0x7ff8000000000000));
                    la_movgr2fr_d(qnan, g);
                }
                la_xvbitsel_v(acc, acc, qnan, cond);
                ra_free_temp(qnan);
                ra_free_temp(cond);
            }
            la_label(sk);
        }
        li_d(zb, zd_off);
        la_add_d(zb, env_ir2_opnd, zb);
        li_d(g, 0);
        for (size_t off = 0; off < vl_bytes; off += 8u) {
            la_st_d(g, zb, (int)off);
        }
        if (ebits == 32) {
            la_movfr2gr_s(g, acc);
            la_st_w(g, zb, 0);
        } else {
            la_movfr2gr_d(g, acc);
            la_st_d(g, zb, 0);
        }
        ra_free_temp(g); ra_free_temp(act); ra_free_temp(pw); ra_free_temp(zb);
        ra_free_temp(val); ra_free_temp(acc);
        return true;
    }

    /* Integer reduction (UADDV/SADDV/SMAXV/UMAXV/SMINV/UMINV/ORV/EORV/ANDV) */
    IR2_OPND acc = ra_alloc_itemp();
    IR2_OPND pw = ra_alloc_itemp();
    IR2_OPND v = ra_alloc_itemp();
    IR2_OPND any = ra_alloc_itemp();

    if (red->kind == PVIR_REDUCE_AND) li_d(acc, (int64_t)mask_for_bits(ebits));
    else if (red->kind == PVIR_REDUCE_MAX) li_d(acc, is_signed ? INT64_MIN : 0LL);
    else if (red->kind == PVIR_REDUCE_MIN) li_d(acc, is_signed ? INT64_MAX : -1LL);
    else li_d(acc, 0);

    li_d(any, 0);

    for (size_t lane = 0; lane < lanes; ++lane) {
        size_t vb = lane * eb;
        size_t wb = (vb / 32u) * 4u;
        unsigned sh = (unsigned)(vb % 32u);
        IR2_OPND sk = ir2_opnd_new_type(IR2_OPND_LABEL);
        if (pg_off >= 0) {
            lata_env_ld_wu(pw, pg_off + (int)wb);
            la_bstrpick_d(pw, pw, sh, sh);
            la_beqz(pw, sk);
        }
        li_d(v, zn_off + (int)vb);
        la_add_d(v, env_ir2_opnd, v);
        emit_load_elem_mem(v, v, 0, eb, is_signed);
        li_d(any, 1);
        switch (red->kind) {
        case PVIR_REDUCE_ADD:
        case PVIR_REDUCE_ADD_ORDERED:
            la_add_d(acc, acc, v);
            break;
        case PVIR_REDUCE_OR:
            la_or(acc, acc, v);
            break;
        case PVIR_REDUCE_XOR:
            la_xor(acc, acc, v);
            break;
        case PVIR_REDUCE_AND:
            la_and(acc, acc, v);
            break;
        case PVIR_REDUCE_MAX:
            if (is_signed) la_slt(pw, acc, v);
            else la_sltu(pw, acc, v);
            la_masknez(acc, acc, pw);
            la_maskeqz(v, v, pw);
            la_or(acc, acc, v);
            break;
        case PVIR_REDUCE_MIN:
        default:
            if (is_signed) la_slt(pw, v, acc);
            else la_sltu(pw, v, acc);
            la_masknez(acc, acc, pw);
            la_maskeqz(v, v, pw);
            la_or(acc, acc, v);
            break;
        }
        la_label(sk);
    }
    if (red->kind == PVIR_REDUCE_MAX || red->kind == PVIR_REDUCE_MIN) {
        IR2_OPND has = ir2_opnd_new_type(IR2_OPND_LABEL);
        la_bnez(any, has);
        li_d(acc, 0);
        la_label(has);
        if (eb < 8u) {
            la_bstrpick_d(acc, acc, ebits - 1u, 0);
        }
    }
    li_d(pw, zd_off);
    la_add_d(pw, env_ir2_opnd, pw);
    li_d(v, 0);
    for (size_t off = 0; off < vl_bytes; off += 8u) {
        la_st_d(v, pw, (int)off);
    }
    la_st_d(acc, pw, 0);
    ra_free_temp(any); ra_free_temp(v); ra_free_temp(pw); ra_free_temp(acc);
    return true;
}

static bool lower_fixed_op(DisasContext *s, const A64SVEIR1Instruction *source,
                           const PvirInstruction *instruction)
{
    const PvirFixedPointOperation *fx = &instruction->operation.fixed;
    size_t vl_bytes = (size_t)s->vl;
    uint16_t ebits = fx->type.element_bits;
    bool is_signed = fx->type.signedness == PVIR_SIGNED;
    int zd = find_zreg_for_value(s->sve_pvir_builder, fx->destination);
    int zn = find_zreg_for_value(s->sve_pvir_builder, fx->operands[0]);
    int zm = (fx->operand_kinds[1] == PVIR_OPERAND_VECTOR)
                 ? find_zreg_for_value(s->sve_pvir_builder, fx->operands[1])
                 : -1;
    int pg = (fx->predicate.source.kind == PVIR_PREDICATE_SOURCE_PREDICATE)
                 ? find_preg_for_value(s->sve_pvir_builder, fx->predicate.source.id)
                 : -1;
    if (source != NULL) {
        zd = source->arg.f_sve_general.zd;
        zn = source->arg.f_sve_general.zn;
        zm = source->arg.f_sve_general.src2_is_imm ? -1 : source->arg.f_sve_general.zm;
        pg = source->arg.f_sve_general.predicated ? source->arg.f_sve_general.pg : -1;
    } else if (zd < 0 && fx->merge_value != PVIR_NO_VALUE) {
        zd = find_zreg_for_value(s->sve_pvir_builder, fx->merge_value);
    }
    int zd_off, zn_off, zm_off = -1, pg_off, exp_off;
    bool cache_hit, const_fold;
    A64SVEPredicateMaskCacheKey mkey = {0};

    if (zd < 0 || zn < 0 || (zd_off = env_offset_zreg(zd)) < 0 ||
        (zn_off = env_offset_zreg(zn)) < 0) {
        return false;
    }
    if (fx->operand_kinds[1] == PVIR_OPERAND_VECTOR &&
        (zm < 0 || (zm_off = env_offset_zreg(zm)) < 0)) {
        return false;
    }
    pg_off = (pg >= 0) ? env_offset_sve_preg(pg) : -1;
    exp_off = env_offset_sve_expanded_mask_scratch();
    mkey.valid = (fx->predicate.form == PVIR_PREDICATE_VECTOR_MASK);
    mkey.source = fx->predicate.source;
    mkey.form = fx->predicate.form;
    mkey.lanes = fx->predicate.lanes;
    mkey.element_bits = ebits;
    mkey.vector_bits = (uint32_t)vl_bytes * 8u;
    cache_hit = mkey.valid && expanded_mask_cache_keys_equal(
                                  &s->sve_pvir_builder->expanded_mask_key, &mkey);
    const_fold = is_const_fold_predicate(s, &fx->predicate,
                                         pg >= 0 ? (uint8_t)pg : 0);

    for (size_t byte = 0; byte < vl_bytes; byte += 32u) {
        IR2_OPND oldv = ra_alloc_ftemp();
        IR2_OPND a = ra_alloc_ftemp();
        IR2_OPND b = ra_alloc_ftemp();
        IR2_OPND mask = ra_alloc_ftemp();
        IR2_OPND res = ra_alloc_ftemp();
        size_t rem = vl_bytes - byte;

        load_z_chunk(oldv, zd_off + (int)byte, rem);
        load_z_chunk(a, zn_off + (int)byte, rem);
        if (fx->operand_kinds[1] == PVIR_OPERAND_VECTOR) {
            load_z_chunk(b, zm_off + (int)byte, rem);
        } else {
            emit_xv_broadcast_u64(b, repeat_elem_to_u64(fx->operand_immediates[1], ebits));
        }
        if (!emit_load_chunk_mask(s, &fx->predicate,
                                  pg >= 0 ? (uint8_t)pg : 0, ebits, byte, rem,
                                  pg_off, exp_off, const_fold, cache_hit, mask)) {
            ra_free_temp(res); ra_free_temp(mask); ra_free_temp(b); ra_free_temp(a); ra_free_temp(oldv);
            return false;
        }

        if (fx->kind == PVIR_FIXED_AVG_SUB) {
            /* (a >> 1) - (b >> 1) - ((~a & b) & 1) */
            IR2_OPND borrow = res;
            emit_xv_broadcast_u64(borrow, repeat_elem_to_u64(1u, ebits));
            la_xvandn_v(oldv, a, b); /* oldv = ~a & b (reloaded below before merge) */
            la_xvand_v(borrow, oldv, borrow);
            switch (ebits) {
            case 8:
                is_signed ? la_xvsrai_b(a, a, 1) : la_xvsrli_b(a, a, 1);
                is_signed ? la_xvsrai_b(b, b, 1) : la_xvsrli_b(b, b, 1);
                la_xvsub_b(res, a, b);
                la_xvsub_b(res, res, borrow);
                break;
            case 16:
                is_signed ? la_xvsrai_h(a, a, 1) : la_xvsrli_h(a, a, 1);
                is_signed ? la_xvsrai_h(b, b, 1) : la_xvsrli_h(b, b, 1);
                la_xvsub_h(res, a, b);
                la_xvsub_h(res, res, borrow);
                break;
            case 32:
                is_signed ? la_xvsrai_w(a, a, 1) : la_xvsrli_w(a, a, 1);
                is_signed ? la_xvsrai_w(b, b, 1) : la_xvsrli_w(b, b, 1);
                la_xvsub_w(res, a, b);
                la_xvsub_w(res, res, borrow);
                break;
            default:
                is_signed ? la_xvsrai_d(a, a, 1) : la_xvsrli_d(a, a, 1);
                is_signed ? la_xvsrai_d(b, b, 1) : la_xvsrli_d(b, b, 1);
                la_xvsub_d(res, a, b);
                la_xvsub_d(res, res, borrow);
                break;
            }
            load_z_chunk(oldv, zd_off + (int)byte, rem);
        } else if (fx->kind == PVIR_FIXED_AVG_ADD) {
            bool rnu = (fx->rounding == PVIR_ROUND_RNU);
            switch (ebits) {
            case 8:
                if (rnu) is_signed ? la_xvavgr_b(res, a, b) : la_xvavgr_bu(res, a, b);
                else is_signed ? la_xvavg_b(res, a, b) : la_xvavg_bu(res, a, b);
                break;
            case 16:
                if (rnu) is_signed ? la_xvavgr_h(res, a, b) : la_xvavgr_hu(res, a, b);
                else is_signed ? la_xvavg_h(res, a, b) : la_xvavg_hu(res, a, b);
                break;
            case 32:
                if (rnu) is_signed ? la_xvavgr_w(res, a, b) : la_xvavgr_wu(res, a, b);
                else is_signed ? la_xvavg_w(res, a, b) : la_xvavg_wu(res, a, b);
                break;
            default:
                if (rnu) is_signed ? la_xvavgr_d(res, a, b) : la_xvavgr_du(res, a, b);
                else is_signed ? la_xvavg_d(res, a, b) : la_xvavg_du(res, a, b);
                break;
            }
        } else if (fx->kind == PVIR_FIXED_SAT_ADD) {
            switch (ebits) {
            case 8: is_signed ? la_xvsadd_b(res, a, b) : la_xvsadd_bu(res, a, b); break;
            case 16: is_signed ? la_xvsadd_h(res, a, b) : la_xvsadd_hu(res, a, b); break;
            case 32: is_signed ? la_xvsadd_w(res, a, b) : la_xvsadd_wu(res, a, b); break;
            default: is_signed ? la_xvsadd_d(res, a, b) : la_xvsadd_du(res, a, b); break;
            }
        } else if (fx->kind == PVIR_FIXED_SAT_SUB) {
            switch (ebits) {
            case 8: is_signed ? la_xvssub_b(res, a, b) : la_xvssub_bu(res, a, b); break;
            case 16: is_signed ? la_xvssub_h(res, a, b) : la_xvssub_hu(res, a, b); break;
            case 32: is_signed ? la_xvssub_w(res, a, b) : la_xvssub_wu(res, a, b); break;
            default: is_signed ? la_xvssub_d(res, a, b) : la_xvssub_du(res, a, b); break;
            }
        } else {
            ra_free_temp(res); ra_free_temp(mask); ra_free_temp(b); ra_free_temp(a); ra_free_temp(oldv);
            return false;
        }
        la_xvbitsel_v(res, oldv, res, mask);
        store_z_chunk(res, zd_off + (int)byte, rem);
        ra_free_temp(res);
        ra_free_temp(mask);
        ra_free_temp(b);
        ra_free_temp(a);
        ra_free_temp(oldv);
    }
    if (!const_fold && mkey.valid && !cache_hit) {
        s->sve_pvir_builder->expanded_mask_key = mkey;
    }
    return true;
}

static bool lower_shift_imm_op(DisasContext *s,
                               const A64SVEIR1Instruction *source,
                               const PvirInstruction *instruction)
{
    const PvirVectorOperation *op = &instruction->operation.vector;
    size_t vl_bytes = (size_t)s->vl;
    uint16_t ebits = op->type.element_bits;
    bool is_asrd = (source != NULL && source->insn_type == AARCH64_A64_SVE_SHIFT_IMM &&
                    source->arg.f_sve_ext_op.subop == SVE_SHIFT_IMM_ASRD);
    uint8_t sh = (uint8_t)op->operand_immediates[1];
    int zd = find_zreg_for_value(s->sve_pvir_builder, op->destination);
    int zn = find_zreg_for_value(s->sve_pvir_builder, op->operands[0]);
    int pg = (op->predicate.source.kind == PVIR_PREDICATE_SOURCE_PREDICATE)
                 ? find_preg_for_value(s->sve_pvir_builder, op->predicate.source.id)
                 : -1;
    if (source != NULL) {
        zd = source->arg.f_sve_ext_op.zd_pd;
        zn = source->arg.f_sve_ext_op.zn;
        pg = source->arg.f_sve_ext_op.predicated ? source->arg.f_sve_ext_op.pg : -1;
    }
    int zd_off, zn_off, pg_off, exp_off;
    bool cache_hit, const_fold;
    A64SVEPredicateMaskCacheKey mkey = {0};

    if (zd < 0 || zn < 0 || (zd_off = env_offset_zreg(zd)) < 0 ||
        (zn_off = env_offset_zreg(zn)) < 0) {
        return false;
    }
    pg_off = (pg >= 0) ? env_offset_sve_preg(pg) : -1;
    exp_off = env_offset_sve_expanded_mask_scratch();
    mkey.valid = (op->predicate.form == PVIR_PREDICATE_VECTOR_MASK);
    mkey.source = op->predicate.source;
    mkey.form = op->predicate.form;
    mkey.lanes = op->predicate.lanes;
    mkey.element_bits = ebits;
    mkey.vector_bits = (uint32_t)vl_bytes * 8u;
    cache_hit = mkey.valid && expanded_mask_cache_keys_equal(
                                  &s->sve_pvir_builder->expanded_mask_key, &mkey);
    const_fold = is_const_fold_predicate(s, &op->predicate,
                                         pg >= 0 ? (uint8_t)pg : 0);

    for (size_t byte = 0; byte < vl_bytes; byte += 32u) {
        IR2_OPND oldv = ra_alloc_ftemp();
        IR2_OPND srcv = ra_alloc_ftemp();
        IR2_OPND res = ra_alloc_ftemp();
        IR2_OPND mask = ra_alloc_ftemp();
        size_t rem = vl_bytes - byte;

        load_z_chunk(oldv, zd_off + (int)byte, rem);
        load_z_chunk(srcv, zn_off + (int)byte, rem);
        if (!emit_load_chunk_mask(s, &op->predicate,
                                  pg >= 0 ? (uint8_t)pg : 0, ebits, byte, rem,
                                  pg_off, exp_off, const_fold, cache_hit, mask)) {
            ra_free_temp(mask); ra_free_temp(res); ra_free_temp(srcv); ra_free_temp(oldv);
            return false;
        }
        if (sh >= ebits) {
            if (instruction->opcode == PVIR_OP_SAR && !is_asrd) {
                int maxs = ebits - 1;
                switch (ebits) {
                case 8: la_xvsrai_b(res, srcv, maxs); break;
                case 16: la_xvsrai_h(res, srcv, maxs); break;
                case 32: la_xvsrai_w(res, srcv, maxs); break;
                default: la_xvsrai_d(res, srcv, maxs); break;
                }
            } else {
                la_xvxor_v(res, res, res);
            }
        } else {
            if (is_asrd) {
                /* ASRD: divide by 2^sh rounding toward 0 -> if x < 0, add (1<<sh)-1 before ASR. */
                IR2_OPND bias = ra_alloc_ftemp();
                emit_xv_broadcast_u64(bias,
                                      repeat_elem_to_u64((UINT64_C(1) << sh) - 1u, ebits));
                switch (ebits) {
                case 8:
                    la_xvsrai_b(res, srcv, 7);
                    la_xvand_v(res, res, bias);
                    la_xvadd_b(res, srcv, res);
                    la_xvsrai_b(res, res, sh);
                    break;
                case 16:
                    la_xvsrai_h(res, srcv, 15);
                    la_xvand_v(res, res, bias);
                    la_xvadd_h(res, srcv, res);
                    la_xvsrai_h(res, res, sh);
                    break;
                case 32:
                    la_xvsrai_w(res, srcv, 31);
                    la_xvand_v(res, res, bias);
                    la_xvadd_w(res, srcv, res);
                    la_xvsrai_w(res, res, sh);
                    break;
                default:
                    la_xvsrai_d(res, srcv, 63);
                    la_xvand_v(res, res, bias);
                    la_xvadd_d(res, srcv, res);
                    la_xvsrai_d(res, res, sh);
                    break;
                }
                ra_free_temp(bias);
            } else if (instruction->opcode == PVIR_OP_SHL) {
                switch (ebits) {
                case 8: la_xvslli_b(res, srcv, sh); break;
                case 16: la_xvslli_h(res, srcv, sh); break;
                case 32: la_xvslli_w(res, srcv, sh); break;
                default: la_xvslli_d(res, srcv, sh); break;
                }
            } else if (instruction->opcode == PVIR_OP_SHR) {
                switch (ebits) {
                case 8: la_xvsrli_b(res, srcv, sh); break;
                case 16: la_xvsrli_h(res, srcv, sh); break;
                case 32: la_xvsrli_w(res, srcv, sh); break;
                default: la_xvsrli_d(res, srcv, sh); break;
                }
            } else {
                switch (ebits) {
                case 8: la_xvsrai_b(res, srcv, sh); break;
                case 16: la_xvsrai_h(res, srcv, sh); break;
                case 32: la_xvsrai_w(res, srcv, sh); break;
                default: la_xvsrai_d(res, srcv, sh); break;
                }
            }
        }
        la_xvbitsel_v(res, oldv, res, mask);
        store_z_chunk(res, zd_off + (int)byte, rem);
        ra_free_temp(mask);
        ra_free_temp(res);
        ra_free_temp(srcv);
        ra_free_temp(oldv);
    }
    if (!const_fold && mkey.valid && !cache_hit) {
        s->sve_pvir_builder->expanded_mask_key = mkey;
    }
    return true;
}

static bool lower_int_unary_ext(DisasContext *s,
                                const A64SVEIR1Instruction *source,
                                const PvirInstruction *instruction)
{
    const PvirVectorOperation *op = &instruction->operation.vector;
    size_t vl_bytes = (size_t)s->vl;
    uint16_t ebits = op->type.element_bits;
    int zd, zn, pg;
    uint8_t subop;
    bool is_ext = (instruction->opcode == PVIR_OP_EXTEND);
    uint16_t sbits = is_ext ? op->operand_types[0].element_bits : ebits;
    bool ext_signed = op->operand_types[0].signedness == PVIR_SIGNED;
    int zd_off, zn_off, pg_off, exp_off;
    bool cache_hit, const_fold;
    A64SVEPredicateMaskCacheKey mkey;

    if (source != NULL) {
        const arg_sve_general *g = &source->arg.f_sve_general;
        zd = g->zd; zn = g->zn; pg = g->pg; subop = g->subop;
    } else {
        zd = find_zreg_for_value(s->sve_pvir_builder, op->destination);
        if (zd < 0 && op->merge_value != PVIR_NO_VALUE) {
            zd = find_zreg_for_value(s->sve_pvir_builder, op->merge_value);
        }
        zn = find_zreg_for_value(s->sve_pvir_builder, op->operands[0]);
        pg = find_preg_for_value(s->sve_pvir_builder, op->predicate.source.id);
        subop = SVE_UNARY_ABS; /* only used when non-EXTEND/non-MOVE below */
    }
    if (zd < 0 || zn < 0 || (zd_off = env_offset_zreg(zd)) < 0 ||
        (zn_off = env_offset_zreg(zn)) < 0) {
        return false;
    }
    pg_off = (pg >= 0) ? env_offset_sve_preg(pg) : -1;
    exp_off = env_offset_sve_expanded_mask_scratch();
    if (!expanded_mask_cache_key_from_operation(op, &mkey)) {
        return false;
    }
    cache_hit = expanded_mask_cache_keys_equal(
        &s->sve_pvir_builder->expanded_mask_key, &mkey);
    const_fold = is_const_fold_predicate(s, &op->predicate,
                                         pg >= 0 ? (uint8_t)pg : 0);

    if (source != NULL && subop == SVE_UNARY_RBIT) {
        size_t eb = ebits / 8u;
        size_t lanes = vl_bytes / eb;
        IR2_OPND pw = ra_alloc_itemp();
        IR2_OPND val = ra_alloc_itemp();
        IR2_OPND srcb = ra_alloc_itemp();
        IR2_OPND dstb = ra_alloc_itemp();
        li_d(srcb, zn_off); la_add_d(srcb, env_ir2_opnd, srcb);
        li_d(dstb, zd_off); la_add_d(dstb, env_ir2_opnd, dstb);
        for (size_t l = 0; l < lanes; ++l) {
            size_t vb = l * eb;
            size_t wb = (vb / 32u) * 4u;
            unsigned sh = (unsigned)(vb % 32u);
            IR2_OPND sk = ir2_opnd_new_type(IR2_OPND_LABEL);
            lata_env_ld_wu(pw, pg_off + (int)wb);
            la_bstrpick_d(pw, pw, sh, sh);
            la_beqz(pw, sk);
            emit_load_elem_mem(val, srcb, (int)vb, (uint8_t)eb, false);
            if (eb == 1) { la_bitrev_4b(val, val); }
            else if (eb == 2) { la_bitrev_w(val, val); la_srli_w(val, val, 16); }
            else if (eb == 4) { la_bitrev_w(val, val); }
            else { la_bitrev_d(val, val); }
            emit_store_elem_mem(val, dstb, (int)vb, (uint8_t)eb);
            la_label(sk);
        }
        ra_free_temp(dstb); ra_free_temp(srcb); ra_free_temp(val); ra_free_temp(pw);
        return true;
    }

    for (size_t byte = 0; byte < vl_bytes; byte += 32u) {
        IR2_OPND oldv = ra_alloc_ftemp();
        IR2_OPND srcv = ra_alloc_ftemp();
        IR2_OPND res = ra_alloc_ftemp();
        IR2_OPND tmp = ra_alloc_ftemp();
        IR2_OPND mask = ra_alloc_ftemp();
        size_t rem = vl_bytes - byte;

        load_z_chunk(oldv, zd_off + (int)byte, rem);
        load_z_chunk(srcv, zn_off + (int)byte, rem);
        if (!emit_load_chunk_mask(s, &op->predicate,
                                  pg >= 0 ? (uint8_t)pg : 0, ebits, byte, rem,
                                  pg_off, exp_off, const_fold, cache_hit, mask)) {
            ra_free_temp(mask); ra_free_temp(tmp); ra_free_temp(res); ra_free_temp(srcv); ra_free_temp(oldv);
            return false;
        }
        if (is_ext) {
            if (ext_signed) {
                int sh = (int)(ebits - sbits);
                switch (ebits) {
                case 16: la_xvslli_h(res, srcv, sh); la_xvsrai_h(res, res, sh); break;
                case 32: la_xvslli_w(res, srcv, sh); la_xvsrai_w(res, res, sh); break;
                default: la_xvslli_d(res, srcv, sh); la_xvsrai_d(res, res, sh); break;
                }
            } else {
                emit_xv_broadcast_u64(tmp, repeat_elem_to_u64(mask_for_bits(sbits), ebits));
                la_xvand_v(res, srcv, tmp);
            }
        } else if (source == NULL) {
            /* Generic PVIR_OP_MOVE */
            la_xvor_v(res, srcv, srcv);
        } else {
            switch (subop) {
            case SVE_UNARY_CLZ:
                switch (ebits) {
                case 8: la_xvclz_b(res, srcv); break;
                case 16: la_xvclz_h(res, srcv); break;
                case 32: la_xvclz_w(res, srcv); break;
                default: la_xvclz_d(res, srcv); break;
                }
                break;
            case SVE_UNARY_CLS:
                emit_xv_broadcast_u64(res, repeat_elem_to_u64(1u, ebits));
                switch (ebits) {
                case 8: la_xvsrai_b(tmp, srcv, 7); la_xvxor_v(tmp, srcv, tmp); la_xvclz_b(tmp, tmp); la_xvsub_b(res, tmp, res); break;
                case 16: la_xvsrai_h(tmp, srcv, 15); la_xvxor_v(tmp, srcv, tmp); la_xvclz_h(tmp, tmp); la_xvsub_h(res, tmp, res); break;
                case 32: la_xvsrai_w(tmp, srcv, 31); la_xvxor_v(tmp, srcv, tmp); la_xvclz_w(tmp, tmp); la_xvsub_w(res, tmp, res); break;
                default: la_xvsrai_d(tmp, srcv, 63); la_xvxor_v(tmp, srcv, tmp); la_xvclz_d(tmp, tmp); la_xvsub_d(res, tmp, res); break;
                }
                break;
            case SVE_UNARY_CNT:
                switch (ebits) {
                case 8: la_xvpcnt_b(res, srcv); break;
                case 16: la_xvpcnt_h(res, srcv); break;
                case 32: la_xvpcnt_w(res, srcv); break;
                default: la_xvpcnt_d(res, srcv); break;
                }
                break;
            case SVE_UNARY_NEG:
                switch (ebits) {
                case 8: la_xvneg_b(res, srcv); break;
                case 16: la_xvneg_h(res, srcv); break;
                case 32: la_xvneg_w(res, srcv); break;
                default: la_xvneg_d(res, srcv); break;
                }
                break;
            case SVE_UNARY_ABS:
                switch (ebits) {
                case 8: la_xvneg_b(tmp, srcv); la_xvmax_b(res, srcv, tmp); break;
                case 16: la_xvneg_h(tmp, srcv); la_xvmax_h(res, srcv, tmp); break;
                case 32: la_xvneg_w(tmp, srcv); la_xvmax_w(res, srcv, tmp); break;
                default: la_xvneg_d(tmp, srcv); la_xvmax_d(res, srcv, tmp); break;
                }
                break;
            case SVE_UNARY_CNOT:
            default:
                la_xvxor_v(tmp, tmp, tmp);
                switch (ebits) {
                case 8: la_xvseq_b(res, srcv, tmp); la_xvsrli_b(res, res, 7); break;
                case 16: la_xvseq_h(res, srcv, tmp); la_xvsrli_h(res, res, 15); break;
                case 32: la_xvseq_w(res, srcv, tmp); la_xvsrli_w(res, res, 31); break;
                default: la_xvseq_d(res, srcv, tmp); la_xvsrli_d(res, res, 63); break;
                }
                break;
            }
        }
        if (op->inactive_policy == PVIR_INACTIVE_MERGE) {
            la_xvbitsel_v(res, oldv, res, mask);
        } else if (op->inactive_policy == PVIR_INACTIVE_ZERO) {
            la_xvand_v(res, res, mask);
        }
        store_z_chunk(res, zd_off + (int)byte, rem);
        ra_free_temp(mask); ra_free_temp(tmp); ra_free_temp(res); ra_free_temp(srcv); ra_free_temp(oldv);
    }
    if (!const_fold && !cache_hit) {
        s->sve_pvir_builder->expanded_mask_key = mkey;
    }
    return true;
}

static bool lower_permute_op(DisasContext *s, const A64SVEIR1Instruction *source,
                             const PvirInstruction *instruction)
{
    const PvirPermuteOperation *p = &instruction->operation.permute;
    size_t vl_bytes = (size_t)s->vl;
    uint16_t ebits = p->type.element_bits;
    uint8_t eb = (uint8_t)(ebits / 8u);
    size_t lanes = vl_bytes / eb;
    int zd = source != NULL ? (int)source->arg.f_sve_general.zd
                            : find_zreg_for_value(s->sve_pvir_builder, p->destination);
    int zn = source != NULL ? (int)source->arg.f_sve_general.zn
                            : find_zreg_for_value(s->sve_pvir_builder, p->operands[0]);
    int zm = (p->operand_count >= 2)
                 ? (source != NULL ? (int)source->arg.f_sve_general.zm
                                   : find_zreg_for_value(s->sve_pvir_builder, p->operands[1]))
                 : zn;
    int pg = p->has_predicate
                 ? (source != NULL ? (int)source->arg.f_sve_general.pg
                                   : find_preg_for_value(s->sve_pvir_builder, p->predicate.source.id))
                 : -1;
    if (zd < 0 || zn < 0 || zm < 0) return false;
    int zd_off = env_offset_zreg(zd), zn_off = env_offset_zreg(zn), zm_off = env_offset_zreg(zm);
    int pg_off = (pg >= 0) ? env_offset_sve_preg(pg) : -1;
    if (zd_off < 0 || zn_off < 0 || zm_off < 0) return false;

    if (p->kind == PVIR_PERMUTE_COMPACT) {
        return lower_compact(s, source, instruction);
    }

    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, -ARM_SVE_MAX_VL_BYTES);
    emit_zero_stack_stage(vl_bytes);

    if (p->kind == PVIR_PERMUTE_REVB || p->kind == PVIR_PERMUTE_REVH ||
        p->kind == PVIR_PERMUTE_REVW) {
        size_t sub_bytes = (p->kind == PVIR_PERMUTE_REVB) ? 1u
                           : (p->kind == PVIR_PERMUTE_REVH ? 2u : 4u);
        IR2_OPND ptr = ra_alloc_itemp(), val = ra_alloc_itemp(), pw = ra_alloc_itemp();
        li_d(ptr, zd_off); la_add_d(ptr, env_ir2_opnd, ptr);
        for (size_t o = 0; o < vl_bytes; o += 8u) {
            la_ld_d(val, ptr, (int)o);
            la_st_d(val, sp_ir2_opnd, (int)o);
        }
        li_d(ptr, zn_off); la_add_d(ptr, env_ir2_opnd, ptr);
        if (sub_bytes < eb) {
            size_t sub_per_elem = eb / sub_bytes;
            for (size_t l = 0; l < lanes; ++l) {
                size_t vb = l * eb;
                size_t wb = (vb / 32u) * 4u;
                unsigned sh = (unsigned)(vb % 32u);
                IR2_OPND sk = ir2_opnd_new_type(IR2_OPND_LABEL);
                lata_env_ld_wu(pw, pg_off + (int)wb);
                la_bstrpick_d(pw, pw, sh, sh);
                la_beqz(pw, sk);
                for (size_t k = 0; k < sub_per_elem; ++k) {
                    emit_load_elem_mem(val, ptr, (int)(vb + (sub_per_elem - 1u - k) * sub_bytes),
                                       (uint8_t)sub_bytes, false);
                    emit_store_elem_mem(val, sp_ir2_opnd, (int)(vb + k * sub_bytes),
                                        (uint8_t)sub_bytes);
                }
                la_label(sk);
            }
        } else {
            for (size_t l = 0; l < lanes; ++l) {
                emit_load_elem_mem(val, ptr, (int)(l * eb), eb, false);
                emit_store_elem_mem(val, sp_ir2_opnd, (int)(l * eb), eb);
            }
        }
        ra_free_temp(pw); ra_free_temp(val); ra_free_temp(ptr);
    } else if (p->kind == PVIR_PERMUTE_TBL || p->kind == PVIR_PERMUTE_TBX) {
        bool two_regs = (p->kind == PVIR_PERMUTE_TBL && p->imm == 1u);
        int zn1_off = env_offset_zreg((zn + 1) & 31);
        IR2_OPND val = ra_alloc_itemp(), idx = ra_alloc_itemp();
        IR2_OPND lmax = ra_alloc_itemp(), ptr = ra_alloc_itemp();
        li_d(lmax, (int64_t)(two_regs ? lanes * 2u : lanes));
        unsigned eb_sh = (eb == 1) ? 0u : (eb == 2 ? 1u : (eb == 4 ? 2u : 3u));

        for (size_t l = 0; l < lanes; ++l) {
            IR2_OPND oob = ir2_opnd_new_type(IR2_OPND_LABEL);
            IR2_OPND st = ir2_opnd_new_type(IR2_OPND_LABEL);
            IR2_OPND sec = ir2_opnd_new_type(IR2_OPND_LABEL);
            li_d(ptr, zm_off + (int)(l * eb));
            la_add_d(ptr, env_ir2_opnd, ptr);
            emit_load_elem_mem(idx, ptr, 0, eb, false);
            la_bgeu(idx, lmax, oob);
            if (two_regs) {
                IR2_OPND ll = val;
                li_d(ll, (int64_t)lanes);
                la_bgeu(idx, ll, sec);
            }
            if (eb_sh != 0) la_slli_d(idx, idx, (int)eb_sh);
            li_d(ptr, zn_off); la_add_d(ptr, env_ir2_opnd, ptr);
            la_add_d(ptr, ptr, idx);
            emit_load_elem_mem(val, ptr, 0, eb, false);
            la_b(st);
            if (two_regs) {
                la_label(sec);
                la_addi_d(idx, idx, -(int)lanes);
                if (eb_sh != 0) la_slli_d(idx, idx, (int)eb_sh);
                li_d(ptr, zn1_off); la_add_d(ptr, env_ir2_opnd, ptr);
                la_add_d(ptr, ptr, idx);
                emit_load_elem_mem(val, ptr, 0, eb, false);
                la_b(st);
            }
            la_label(oob);
            if (p->kind == PVIR_PERMUTE_TBX) {
                li_d(ptr, zd_off + (int)(l * eb));
                la_add_d(ptr, env_ir2_opnd, ptr);
                emit_load_elem_mem(val, ptr, 0, eb, false);
            } else {
                li_d(val, 0);
            }
            la_label(st);
            emit_store_elem_mem(val, sp_ir2_opnd, (int)(l * eb), eb);
        }
        ra_free_temp(ptr); ra_free_temp(lmax); ra_free_temp(idx); ra_free_temp(val);
    } else if (p->kind == PVIR_PERMUTE_SPLICE) {
        IR2_OPND first = ra_alloc_itemp(), last = ra_alloc_itemp();
        IR2_OPND rem_cnt = ra_alloc_itemp(), wptr = ra_alloc_itemp();

        li_d(first, (int64_t)lanes);
        li_d(last, 0);
        for (size_t l = 0; l < lanes; ++l) {
            size_t vb = l * eb;
            size_t wb = (vb / 32u) * 4u;
            unsigned sh = (unsigned)(vb % 32u);
            IR2_OPND sk = ir2_opnd_new_type(IR2_OPND_LABEL);
            IR2_OPND not_f = ir2_opnd_new_type(IR2_OPND_LABEL);
            lata_env_ld_wu(wptr, pg_off + (int)wb);
            la_bstrpick_d(wptr, wptr, sh, sh);
            la_beqz(wptr, sk);
            li_d(rem_cnt, (int64_t)lanes);
            la_bne(first, rem_cnt, not_f);
            li_d(first, (int64_t)l);
            la_label(not_f);
            li_d(last, (int64_t)l);
            la_label(sk);
        }
        la_mov64(wptr, sp_ir2_opnd);
        li_d(rem_cnt, (int64_t)lanes);
        IR2_OPND fill_zm = ir2_opnd_new_type(IR2_OPND_LABEL);
        IR2_OPND sp_done = ir2_opnd_new_type(IR2_OPND_LABEL);
        la_beq(first, rem_cnt, fill_zm);
        {
            IR2_OPND val = ra_alloc_itemp();
            for (size_t l = 0; l < lanes; ++l) {
                IR2_OPND nxt = ir2_opnd_new_type(IR2_OPND_LABEL);
                li_d(val, (int64_t)l);
                la_bltu(val, first, nxt);
                la_bltu(last, val, nxt);
                li_d(val, zn_off + (int)(l * eb));
                la_add_d(val, env_ir2_opnd, val);
                emit_load_elem_mem(val, val, 0, eb, false);
                emit_store_elem_mem(val, wptr, 0, eb);
                la_addi_d(wptr, wptr, eb);
                la_addi_d(rem_cnt, rem_cnt, -1);
                la_label(nxt);
            }
            ra_free_temp(val);
        }
        la_label(fill_zm);
        {
            IR2_OPND val = first, lidx = last;
            for (size_t l = 0; l < lanes; ++l) {
                li_d(lidx, (int64_t)l);
                la_bgeu(lidx, rem_cnt, sp_done);
                li_d(val, zm_off + (int)(l * eb));
                la_add_d(val, env_ir2_opnd, val);
                emit_load_elem_mem(val, val, 0, eb, false);
                emit_store_elem_mem(val, wptr, 0, eb);
                la_addi_d(wptr, wptr, eb);
            }
        }
        la_label(sp_done);
        ra_free_temp(wptr); ra_free_temp(rem_cnt); ra_free_temp(last); ra_free_temp(first);
    } else if (p->kind == PVIR_PERMUTE_EXT) {
        IR2_OPND ptr = ra_alloc_itemp(), val = ra_alloc_itemp();
        size_t idx = (p->imm < vl_bytes) ? (size_t)p->imm : vl_bytes;
        for (size_t i = 0; i < vl_bytes; ++i) {
            size_t sidx = i + idx;
            int off = (sidx < vl_bytes) ? (zn_off + (int)sidx)
                                        : (zm_off + (int)(sidx - vl_bytes));
            li_d(ptr, off); la_add_d(ptr, env_ir2_opnd, ptr);
            la_ld_bu(val, ptr, 0);
            la_st_b(val, sp_ir2_opnd, (int)i);
        }
        ra_free_temp(val); ra_free_temp(ptr);
    } else {
        IR2_OPND ptr = ra_alloc_itemp(), val = ra_alloc_itemp();
        for (size_t l = 0; l < lanes; ++l) {
            bool from_m = false;
            size_t sl = l;
            switch (p->kind) {
            case PVIR_PERMUTE_REV: sl = lanes - 1u - l; break;
            case PVIR_PERMUTE_ZIP1: from_m = (l & 1u) != 0; sl = l / 2u; break;
            case PVIR_PERMUTE_ZIP2: from_m = (l & 1u) != 0; sl = lanes / 2u + l / 2u; break;
            case PVIR_PERMUTE_UZP1: sl = 2u * l; if (sl >= lanes) { from_m = true; sl -= lanes; } break;
            case PVIR_PERMUTE_UZP2: sl = 2u * l + 1u; if (sl >= lanes) { from_m = true; sl -= lanes; } break;
            case PVIR_PERMUTE_TRN1: from_m = (l & 1u) != 0; sl = l & ~1u; break;
            case PVIR_PERMUTE_TRN2:
            default: from_m = (l & 1u) != 0; sl = (l & ~1u) + 1u; break;
            }
            li_d(ptr, (from_m ? zm_off : zn_off) + (int)(sl * eb));
            la_add_d(ptr, env_ir2_opnd, ptr);
            emit_load_elem_mem(val, ptr, 0, eb, false);
            emit_store_elem_mem(val, sp_ir2_opnd, (int)(l * eb), eb);
        }
        ra_free_temp(val); ra_free_temp(ptr);
    }

    emit_commit_stack_to_zreg(zd_off, vl_bytes);
    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, ARM_SVE_MAX_VL_BYTES);
    return true;
}

static bool lower_insert_op(DisasContext *s, const A64SVEIR1Instruction *source,
                            const PvirInstruction *instruction)
{
    const PvirLaneOperation *op = &instruction->operation.lane;
    size_t vl_bytes = (size_t)s->vl;
    uint16_t ebits = op->vector_type.element_bits;
    uint8_t eb = (uint8_t)(ebits / 8u);
    int zd = find_zreg_for_value(s->sve_pvir_builder, op->destination);
    int zsrc = find_zreg_for_value(s->sve_pvir_builder, op->merge_value);
    if (source != NULL && source->insn_type == AARCH64_A64_SVE_PERMUTE) {
        /* SVE INSR Zdn, Rm/Vm: shifts elements UP by one element and inserts scalar into lane 0! */
        const arg_sve_general *g = &source->arg.f_sve_general;
        int z_off = env_offset_zreg(g->zd);
        if (z_off < 0) return false;
        size_t lanes = vl_bytes / eb;
        IR2_OPND zb = ra_alloc_itemp(), v = ra_alloc_itemp();
        li_d(zb, z_off); la_add_d(zb, env_ir2_opnd, zb);
        for (size_t l = lanes; l > 1; --l) {
            emit_load_elem_mem(v, zb, (int)((l - 2u) * eb), eb, false);
            emit_store_elem_mem(v, zb, (int)((l - 1u) * eb), eb);
        }
        if (g->src1_is_imm) {
            int vo = env_offset_zreg(g->zm);
            IR2_OPND vb = ra_alloc_itemp();
            li_d(vb, vo); la_add_d(vb, env_ir2_opnd, vb);
            emit_load_elem_mem(v, vb, 0, eb, false);
            ra_free_temp(vb);
        } else {
            IR2_OPND gs = alloc_gpr_src(g->zm);
            la_mov64(v, gs);
            free_alloc_gpr(gs);
        }
        emit_store_elem_mem(v, zb, 0, eb);
        ra_free_temp(v); ra_free_temp(zb);
        return true;
    }

    if (zd < 0) zd = zsrc >= 0 ? zsrc : 0;
    if (zsrc < 0) zsrc = zd;
    int zd_off = env_offset_zreg(zd), zsrc_off = env_offset_zreg(zsrc);
    if (zd_off < 0 || zsrc_off < 0) return false;

    if (zd != zsrc) {
        for (size_t b = 0; b < vl_bytes; b += 32u) {
            IR2_OPND t = ra_alloc_ftemp();
            size_t rem = vl_bytes - b;
            load_z_chunk(t, zsrc_off + (int)b, rem);
            store_z_chunk(t, zd_off + (int)b, rem);
            ra_free_temp(t);
        }
    }
    int gpr = find_gpr_for_value(op->source);
    IR2_OPND gs = (gpr >= 0) ? alloc_gpr_src(gpr) : zero_ir2_opnd;
    IR2_OPND zb = ra_alloc_itemp();
    li_d(zb, zd_off); la_add_d(zb, env_ir2_opnd, zb);
    emit_store_elem_mem(gs, zb, (int)(op->lane * eb), eb);
    ra_free_temp(zb);
    if (gpr >= 0) free_alloc_gpr(gs);
    return true;
}

static bool lower_extract_op(DisasContext *s, const A64SVEIR1Instruction *source,
                             const PvirInstruction *instruction)
{
    if (source != NULL && source->insn_type == AARCH64_A64_SVE_LAST) {
        return lower_last(s, source, instruction);
    }
    const PvirLaneOperation *op = &instruction->operation.lane;
    uint8_t eb = (uint8_t)(op->vector_type.element_bits / 8u);
    int zn = find_zreg_for_value(s->sve_pvir_builder, op->source);
    int zn_off;
    if (zn < 0 || (zn_off = env_offset_zreg(zn)) < 0) return false;
    int dst_gpr = find_gpr_for_value(op->destination);
    int dst_z = find_zreg_for_value(s->sve_pvir_builder, op->destination);
    IR2_OPND zb = ra_alloc_itemp(), v = ra_alloc_itemp();
    li_d(zb, zn_off); la_add_d(zb, env_ir2_opnd, zb);
    emit_load_elem_mem(v, zb, (int)(op->lane * eb), eb, false);
    if (dst_gpr >= 0 && dst_gpr < 31) {
        store_gpr_dst(dst_gpr, v);
    } else if (dst_z >= 0) {
        int dst_off = env_offset_zreg(dst_z);
        li_d(zb, dst_off); la_add_d(zb, env_ir2_opnd, zb);
        emit_store_elem_mem(v, zb, 0, eb);
    } else {
        /* Standalone PVIR test extracting SSA value: publish to X0 so host harness reads env->xregs[0]! */
        store_gpr_dst(0, v);
    }
    ra_free_temp(v); ra_free_temp(zb);
    return true;
}

static bool lower_index_op(DisasContext *s, const A64SVEIR1Instruction *source,
                           const PvirInstruction *instruction)
{
    const PvirIndexOperation *idx = &instruction->operation.index;
    size_t vl_bytes = (size_t)s->vl;
    uint16_t ebits = idx->type.element_bits;
    uint8_t eb = (uint8_t)(ebits / 8u);
    size_t lanes = vl_bytes / eb;
    int zd = source != NULL ? (int)source->arg.f_sve_general.zd
                            : find_zreg_for_value(s->sve_pvir_builder, idx->destination);
    if (zd < 0) zd = 0;
    int zd_off = env_offset_zreg(zd);
    if (zd_off < 0) return false;

    IR2_OPND start = ra_alloc_itemp();
    IR2_OPND step = ra_alloc_itemp();
    IR2_OPND cur = ra_alloc_itemp();
    IR2_OPND zb = ra_alloc_itemp();
    if (source != NULL && source->insn_type == AARCH64_A64_SVE_INDEX) {
        const arg_sve_general *g = &source->arg.f_sve_general;
        if (g->src1_is_imm) li_d(start, g->imm1);
        else {
            IR2_OPND gs = alloc_gpr_src(g->zn);
            la_mov64(start, gs);
            free_alloc_gpr(gs);
        }
        if (g->src2_is_imm) li_d(step, g->imm2);
        else {
            IR2_OPND gs = alloc_gpr_src(g->zm);
            la_mov64(step, gs);
            free_alloc_gpr(gs);
        }
    } else {
        int gs1 = find_gpr_for_value(idx->start_value);
        int gs2 = find_gpr_for_value(idx->step_value);
        if (gs1 >= 0) {
            IR2_OPND g = alloc_gpr_src(gs1);
            la_mov64(start, g);
            free_alloc_gpr(g);
        } else {
            li_d(start, 0);
        }
        if (idx->step_value == PVIR_NO_VALUE) li_d(step, 1);
        else if (gs2 >= 0) {
            IR2_OPND g = alloc_gpr_src(gs2);
            la_mov64(step, g);
            free_alloc_gpr(g);
        } else {
            li_d(step, 1);
        }
    }
    li_d(zb, zd_off); la_add_d(zb, env_ir2_opnd, zb);
    la_mov64(cur, start);
    for (size_t l = 0; l < lanes; ++l) {
        emit_store_elem_mem(cur, zb, (int)(l * eb), eb);
        la_add_d(cur, cur, step);
    }
    ra_free_temp(zb); ra_free_temp(cur); ra_free_temp(step); ra_free_temp(start);
    return true;
}

static bool lower_wmul_op(DisasContext *s, const A64SVEIR1Instruction *source,
                          const PvirInstruction *instruction)
{
    if (source == NULL) return false;
    const arg_sve_general *g = &source->arg.f_sve_general;
    if (g->subop != SVE_WMUL_MULL) {
        return lower_fma_op(s, source, instruction);
    }
    size_t vl_bytes = (size_t)s->vl;
    uint8_t dst_eb = g->element_bits / 8u;
    uint8_t src_eb = dst_eb / 2u;
    size_t lanes = vl_bytes / dst_eb;
    int zd_off = env_offset_zreg(g->zd);
    int zn_off = env_offset_zreg(g->zn);
    int zm_off = env_offset_zreg(g->zm);
    if (zd_off < 0 || zn_off < 0 || zm_off < 0) return false;

    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, -ARM_SVE_MAX_VL_BYTES);
    IR2_OPND sn = ra_alloc_itemp(), sm = ra_alloc_itemp();
    IR2_OPND vn = ra_alloc_itemp(), vm = ra_alloc_itemp();
    li_d(sn, zn_off); la_add_d(sn, env_ir2_opnd, sn);
    li_d(sm, zm_off); la_add_d(sm, env_ir2_opnd, sm);
    for (size_t l = 0; l < lanes; ++l) {
        int src_disp = (int)((2u * l + (g->top_half ? 1u : 0u)) * src_eb);
        emit_load_elem_mem(vn, sn, src_disp, src_eb, g->is_signed);
        emit_load_elem_mem(vm, sm, src_disp, src_eb, g->is_signed);
        la_mul_d(vn, vn, vm);
        emit_store_elem_mem(vn, sp_ir2_opnd, (int)(l * dst_eb), dst_eb);
    }
    ra_free_temp(vm); ra_free_temp(vn); ra_free_temp(sm); ra_free_temp(sn);
    emit_commit_stack_to_zreg(zd_off, vl_bytes);
    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, ARM_SVE_MAX_VL_BYTES);
    return true;
}

static bool lower_imm_bin_op(DisasContext *s, const A64SVEIR1Instruction *source,
                             const PvirInstruction *instruction)
{
    const PvirVectorOperation *op = &instruction->operation.vector;
    size_t vl_bytes = (size_t)s->vl;
    uint16_t ebits = op->type.element_bits;
    int zd = source != NULL ? (int)source->arg.f_sve_general.zd
                            : find_zreg_for_value(s->sve_pvir_builder, op->destination);
    int zn = source != NULL ? (int)source->arg.f_sve_general.zn
                            : find_zreg_for_value(s->sve_pvir_builder, op->operands[0]);
    bool is_subr = (source != NULL && source->insn_type == AARCH64_A64_SVE_IMM_BIN &&
                    source->arg.f_sve_general.subop == SVE_IMM_BIN_SUBR);
    bool is_dup = (source != NULL && source->insn_type == AARCH64_A64_SVE_IMM_BIN &&
                   source->arg.f_sve_general.subop == SVE_IMM_BIN_DUP);
    if (zd < 0 || (!is_dup && zn < 0)) return false;
    int zd_off = env_offset_zreg(zd), zn_off = is_dup ? zd_off : env_offset_zreg(zn);
    if (zd_off < 0 || zn_off < 0) return false;

    uint64_t pat = is_dup ? repeat_elem_to_u64((uint64_t)source->arg.f_sve_general.imm1, ebits)
                          : repeat_elem_to_u64(op->operand_immediates[1], ebits);
    for (size_t b = 0; b < vl_bytes; b += 32u) {
        IR2_OPND srcv = ra_alloc_ftemp(), immv = ra_alloc_ftemp(), res = ra_alloc_ftemp();
        size_t rem = vl_bytes - b;
        emit_xv_broadcast_u64(immv, pat);
        if (is_dup) {
            store_z_chunk(immv, zd_off + (int)b, rem);
        } else {
            load_z_chunk(srcv, zn_off + (int)b, rem);
            if (instruction->opcode == PVIR_OP_AND) la_xvand_v(res, srcv, immv);
            else if (instruction->opcode == PVIR_OP_OR) la_xvor_v(res, srcv, immv);
            else if (instruction->opcode == PVIR_OP_XOR) la_xvxor_v(res, srcv, immv);
            else if (instruction->opcode == PVIR_OP_ADD) {
                switch (ebits) {
                case 8: la_xvadd_b(res, srcv, immv); break;
                case 16: la_xvadd_h(res, srcv, immv); break;
                case 32: la_xvadd_w(res, srcv, immv); break;
                default: la_xvadd_d(res, srcv, immv); break;
                }
            } else if (instruction->opcode == PVIR_OP_MUL) {
                switch (ebits) {
                case 8: la_xvmul_b(res, srcv, immv); break;
                case 16: la_xvmul_h(res, srcv, immv); break;
                case 32: la_xvmul_w(res, srcv, immv); break;
                default: la_xvmul_d(res, srcv, immv); break;
                }
            } else {
                IR2_OPND j = is_subr ? immv : srcv, k = is_subr ? srcv : immv;
                switch (ebits) {
                case 8: la_xvsub_b(res, j, k); break;
                case 16: la_xvsub_h(res, j, k); break;
                case 32: la_xvsub_w(res, j, k); break;
                default: la_xvsub_d(res, j, k); break;
                }
            }
            store_z_chunk(res, zd_off + (int)b, rem);
        }
        ra_free_temp(res); ra_free_temp(immv); ra_free_temp(srcv);
    }
    return true;
}

static bool lower_select_op(DisasContext *s, const A64SVEIR1Instruction *source,
                            const PvirInstruction *instruction)
{
    const PvirVectorOperation *op = &instruction->operation.vector;
    size_t vl_bytes = (size_t)s->vl;
    uint16_t ebits = op->type.element_bits;
    int zd = find_zreg_for_value(s->sve_pvir_builder, op->destination);
    int pg = find_preg_for_value(s->sve_pvir_builder, op->operands[0]);
    int zn = find_zreg_for_value(s->sve_pvir_builder, op->operands[1]);
    int zm = find_zreg_for_value(s->sve_pvir_builder, op->operands[2]);
    if (source != NULL && source->insn_type == AARCH64_A64_SVE_SEL) {
        const arg_sve_general *g = &source->arg.f_sve_general;
        zd = g->zd; pg = g->pg; zn = g->zn; zm = g->zm;
    }
    if (zd < 0) zd = zn >= 0 ? zn : 0;
    int zd_off, zn_off, zm_off, pg_off;
    if (pg < 0 || zn < 0 || zm < 0 ||
        (zd_off = env_offset_zreg(zd)) < 0 ||
        (zn_off = env_offset_zreg(zn)) < 0 ||
        (zm_off = env_offset_zreg(zm)) < 0 ||
        (pg_off = env_offset_sve_preg(pg)) < 0) {
        return false;
    }
    for (size_t b = 0; b < vl_bytes; b += 32u) {
        IR2_OPND vn = ra_alloc_ftemp(), vm = ra_alloc_ftemp();
        IR2_OPND m = ra_alloc_ftemp(), r = ra_alloc_ftemp();
        size_t rem = vl_bytes - b;
        load_z_chunk(vn, zn_off + (int)b, rem);
        load_z_chunk(vm, zm_off + (int)b, rem);
        /* Note: SEL picks Zn where Pg==1 else Zm (xvbitsel(r, vm, vn, m)!) */
        if (is_const_fold_predicate(s, &op->predicate, (uint8_t)pg)) {
            emit_materialize_const_chunk_mask(s, &op->predicate, (uint8_t)pg,
                                              ebits, b, m);
        } else if (!lower_packed_predicate_mask(m, pg_off + (int)(b / 8u), ebits)) {
            ra_free_temp(r); ra_free_temp(m); ra_free_temp(vm); ra_free_temp(vn);
            return false;
        }
        la_xvbitsel_v(r, vm, vn, m);
        store_z_chunk(r, zd_off + (int)b, rem);
        ra_free_temp(r); ra_free_temp(m); ra_free_temp(vm); ra_free_temp(vn);
    }
    return true;
}

static bool lower_div_rem_op(DisasContext *s,
                             const A64SVEIR1Instruction *source,
                             const PvirInstruction *instruction)
{
    const PvirVectorOperation *op = &instruction->operation.vector;
    size_t vl_bytes = (size_t)s->vl;
    uint16_t ebits = op->type.element_bits;
    bool is_rem = (instruction->opcode == PVIR_OP_REM);
    bool is_signed = (op->type.signedness == PVIR_SIGNED);
    int zd = find_zreg_for_value(s->sve_pvir_builder, op->destination);
    int zn = find_zreg_for_value(s->sve_pvir_builder, op->operands[0]);
    int zm = (op->operand_kinds[1] == PVIR_OPERAND_VECTOR)
                 ? find_zreg_for_value(s->sve_pvir_builder, op->operands[1])
                 : -1;
    int pg = (op->predicate.source.kind == PVIR_PREDICATE_SOURCE_PREDICATE)
                 ? find_preg_for_value(s->sve_pvir_builder, op->predicate.source.id)
                 : -1;
    if (source != NULL && source->insn_type == AARCH64_A64_SVE_IDIV) {
        const arg_sve_general *g = &source->arg.f_sve_general;
        bool rev = (g->subop == SVE_IDIV_SDIVR || g->subop == SVE_IDIV_UDIVR);
        zd = g->zd;
        zn = rev ? g->zm : g->zd;
        zm = rev ? g->zd : g->zm;
        pg = g->pg;
    } else if (zd < 0 && op->merge_value != PVIR_NO_VALUE) {
        zd = find_zreg_for_value(s->sve_pvir_builder, op->merge_value);
    }
    if (zd < 0) zd = zn >= 0 ? zn : 0;
    int zd_off, zn_off, zm_off = -1, pg_off, exp_off;
    bool cache_hit, const_fold;
    A64SVEPredicateMaskCacheKey mkey = {0};

    if (zn < 0 || (zd_off = env_offset_zreg(zd)) < 0 ||
        (zn_off = env_offset_zreg(zn)) < 0) {
        return false;
    }
    if (op->operand_kinds[1] == PVIR_OPERAND_VECTOR &&
        (zm < 0 || (zm_off = env_offset_zreg(zm)) < 0)) {
        return false;
    }
    pg_off = (pg >= 0) ? env_offset_sve_preg(pg) : -1;
    exp_off = env_offset_sve_expanded_mask_scratch();
    mkey.valid = (op->predicate.form == PVIR_PREDICATE_VECTOR_MASK);
    mkey.source = op->predicate.source;
    mkey.form = op->predicate.form;
    mkey.lanes = op->predicate.lanes;
    mkey.element_bits = ebits;
    mkey.vector_bits = (uint32_t)vl_bytes * 8u;
    cache_hit = mkey.valid && expanded_mask_cache_keys_equal(
                                  &s->sve_pvir_builder->expanded_mask_key, &mkey);
    const_fold = is_const_fold_predicate(s, &op->predicate,
                                         pg >= 0 ? (uint8_t)pg : 0);

    for (size_t b = 0; b < vl_bytes; b += 32u) {
        IR2_OPND oldv = ra_alloc_ftemp(), lhs = ra_alloc_ftemp(), rhs = ra_alloc_ftemp();
        IR2_OPND res = ra_alloc_ftemp(), mask = ra_alloc_ftemp();
        size_t rem = vl_bytes - b;

        load_z_chunk(oldv, zd_off + (int)b, rem);
        load_z_chunk(lhs, zn_off + (int)b, rem);
        if (op->operand_kinds[1] == PVIR_OPERAND_VECTOR) {
            load_z_chunk(rhs, zm_off + (int)b, rem);
        } else {
            emit_xv_broadcast_u64(rhs, repeat_elem_to_u64(op->operand_immediates[1], ebits));
        }
        if (!emit_load_chunk_mask(s, &op->predicate, pg >= 0 ? (uint8_t)pg : 0,
                                  ebits, b, rem, pg_off, exp_off, const_fold,
                                  cache_hit, mask)) {
            ra_free_temp(mask); ra_free_temp(res); ra_free_temp(rhs); ra_free_temp(lhs); ra_free_temp(oldv);
            return false;
        }
        /* Guard divide-by-0 -> ARM returns 0; replace zero divisor with 1 and zero quotient where div==0. */
        la_xvxor_v(res, res, res);
        switch (ebits) {
        case 8: la_xvseq_b(oldv, rhs, res); break;
        case 16: la_xvseq_h(oldv, rhs, res); break;
        case 32: la_xvseq_w(oldv, rhs, res); break;
        default: la_xvseq_d(oldv, rhs, res); break;
        }
        emit_xv_broadcast_u64(res, repeat_elem_to_u64(1u, ebits));
        la_xvbitsel_v(rhs, rhs, res, oldv);
        if (is_rem) {
            switch (ebits) {
            case 8: is_signed ? la_xvmod_b(res, lhs, rhs) : la_xvmod_bu(res, lhs, rhs); break;
            case 16: is_signed ? la_xvmod_h(res, lhs, rhs) : la_xvmod_hu(res, lhs, rhs); break;
            case 32: is_signed ? la_xvmod_w(res, lhs, rhs) : la_xvmod_wu(res, lhs, rhs); break;
            default: is_signed ? la_xvmod_d(res, lhs, rhs) : la_xvmod_du(res, lhs, rhs); break;
            }
        } else {
            switch (ebits) {
            case 8: is_signed ? la_xvdiv_b(res, lhs, rhs) : la_xvdiv_bu(res, lhs, rhs); break;
            case 16: is_signed ? la_xvdiv_h(res, lhs, rhs) : la_xvdiv_hu(res, lhs, rhs); break;
            case 32: is_signed ? la_xvdiv_w(res, lhs, rhs) : la_xvdiv_wu(res, lhs, rhs); break;
            default: is_signed ? la_xvdiv_d(res, lhs, rhs) : la_xvdiv_du(res, lhs, rhs); break;
            }
        }
        la_xvandn_v(res, oldv, res); /* divisor == 0 -> result 0 */
        load_z_chunk(oldv, zd_off + (int)b, rem);
        if (op->inactive_policy == PVIR_INACTIVE_MERGE) la_xvbitsel_v(res, oldv, res, mask);
        else if (op->inactive_policy == PVIR_INACTIVE_ZERO) la_xvand_v(res, res, mask);
        store_z_chunk(res, zd_off + (int)b, rem);
        ra_free_temp(mask); ra_free_temp(res); ra_free_temp(rhs); ra_free_temp(lhs); ra_free_temp(oldv);
    }
    if (!const_fold && mkey.valid && !cache_hit) {
        s->sve_pvir_builder->expanded_mask_key = mkey;
    }
    return true;
}

static bool lower_truncate_op(DisasContext *s,
                              const PvirInstruction *instruction)
{
    const PvirVectorOperation *op = &instruction->operation.vector;
    uint16_t dbits = op->type.element_bits;
    uint16_t sbits = op->operand_types[0].element_bits;
    uint8_t deb = (uint8_t)(dbits / 8u);
    uint8_t seb = (uint8_t)(sbits / 8u);
    size_t lanes = op->predicate.lanes;
    int zd = find_zreg_for_value(s->sve_pvir_builder, op->destination);
    int zn = find_zreg_for_value(s->sve_pvir_builder, op->operands[0]);
    if (zd < 0 && op->merge_value != PVIR_NO_VALUE) {
        zd = find_zreg_for_value(s->sve_pvir_builder, op->merge_value);
    }
    if (zd < 0) zd = 0;
    if (zn < 0 || seb == 0 || deb == 0) return false;
    int zd_off = env_offset_zreg(zd), zn_off = env_offset_zreg(zn);
    if (zd_off < 0 || zn_off < 0) return false;

    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, -ARM_SVE_MAX_VL_BYTES);
    emit_zero_stack_stage((size_t)s->vl);
    IR2_OPND srcb = ra_alloc_itemp(), v = ra_alloc_itemp();
    li_d(srcb, zn_off); la_add_d(srcb, env_ir2_opnd, srcb);
    for (size_t l = 0; l < lanes && l * deb < (size_t)s->vl; ++l) {
        emit_load_elem_mem(v, srcb, (int)(l * seb), seb, false);
        emit_store_elem_mem(v, sp_ir2_opnd, (int)(l * deb), deb);
    }
    ra_free_temp(v); ra_free_temp(srcb);
    emit_commit_stack_to_zreg(zd_off, (size_t)s->vl);
    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, ARM_SVE_MAX_VL_BYTES);
    return true;
}

static bool lower_generic_binary_pvir(DisasContext *s,
                                      const PvirInstruction *instruction)
{
    const PvirVectorOperation *op = &instruction->operation.vector;
    size_t vl_bytes = (size_t)s->vl;
    uint16_t ebits = op->type.element_bits;
    bool is_signed = (op->type.signedness == PVIR_SIGNED);
    int zd = find_zreg_for_value(s->sve_pvir_builder, op->destination);
    int zn = find_zreg_for_value(s->sve_pvir_builder, op->operands[0]);
    int zm = (op->operand_kinds[1] == PVIR_OPERAND_VECTOR)
                 ? find_zreg_for_value(s->sve_pvir_builder, op->operands[1])
                 : -1;
    int pg = (op->predicate.source.kind == PVIR_PREDICATE_SOURCE_PREDICATE)
                 ? find_preg_for_value(s->sve_pvir_builder, op->predicate.source.id)
                 : -1;
    if (zd < 0 && op->merge_value != PVIR_NO_VALUE) {
        zd = find_zreg_for_value(s->sve_pvir_builder, op->merge_value);
    }
    if (zd < 0) zd = zn >= 0 ? zn : 0;
    int zd_off, zn_off, zm_off = -1, pg_off, exp_off;
    bool cache_hit, const_fold;
    A64SVEPredicateMaskCacheKey mkey = {0};

    if (zn < 0 || (zd_off = env_offset_zreg(zd)) < 0 ||
        (zn_off = env_offset_zreg(zn)) < 0) {
        return false;
    }
    if (op->operand_kinds[1] == PVIR_OPERAND_VECTOR &&
        (zm < 0 || (zm_off = env_offset_zreg(zm)) < 0)) {
        return false;
    }
    pg_off = (pg >= 0) ? env_offset_sve_preg(pg) : -1;
    exp_off = env_offset_sve_expanded_mask_scratch();
    mkey.valid = (op->predicate.form == PVIR_PREDICATE_VECTOR_MASK);
    mkey.source = op->predicate.source;
    mkey.form = op->predicate.form;
    mkey.lanes = op->predicate.lanes;
    mkey.element_bits = ebits;
    mkey.vector_bits = (uint32_t)vl_bytes * 8u;
    cache_hit = mkey.valid && expanded_mask_cache_keys_equal(
                                  &s->sve_pvir_builder->expanded_mask_key, &mkey);
    const_fold = is_const_fold_predicate(s, &op->predicate,
                                         pg >= 0 ? (uint8_t)pg : 0);

    for (size_t b = 0; b < vl_bytes; b += 32u) {
        IR2_OPND oldv = ra_alloc_ftemp(), lhs = ra_alloc_ftemp(), rhs = ra_alloc_ftemp();
        IR2_OPND res = ra_alloc_ftemp(), mask = ra_alloc_ftemp();
        size_t rem = vl_bytes - b;

        load_z_chunk(oldv, zd_off + (int)b, rem);
        load_z_chunk(lhs, zn_off + (int)b, rem);
        if (op->operand_kinds[1] == PVIR_OPERAND_VECTOR) {
            load_z_chunk(rhs, zm_off + (int)b, rem);
        } else if (op->operand_kinds[1] == PVIR_OPERAND_SCALAR) {
            int g = find_gpr_for_value(op->operands[1]);
            IR2_OPND gs = (g >= 0) ? alloc_gpr_src(g) : zero_ir2_opnd;
            la_xvinsgr2vr_d(rhs, gs, 0); la_xvinsgr2vr_d(rhs, gs, 2);
            switch (ebits) {
            case 8: la_xvreplve0_b(rhs, rhs); break;
            case 16: la_xvreplve0_h(rhs, rhs); break;
            case 32: la_xvreplve0_w(rhs, rhs); break;
            default: la_xvreplve0_d(rhs, rhs); break;
            }
            if (g >= 0) free_alloc_gpr(gs);
        } else {
            emit_xv_broadcast_u64(rhs, repeat_elem_to_u64(op->operand_immediates[1], ebits));
        }
        if (!emit_load_chunk_mask(s, &op->predicate, pg >= 0 ? (uint8_t)pg : 0,
                                  ebits, b, rem, pg_off, exp_off, const_fold,
                                  cache_hit, mask)) {
            ra_free_temp(mask); ra_free_temp(res); ra_free_temp(rhs); ra_free_temp(lhs); ra_free_temp(oldv);
            return false;
        }
        switch (instruction->opcode) {
        case PVIR_OP_AND: la_xvand_v(res, lhs, rhs); break;
        case PVIR_OP_OR: la_xvor_v(res, lhs, rhs); break;
        case PVIR_OP_XOR: la_xvxor_v(res, lhs, rhs); break;
        case PVIR_OP_ADD:
            if (op->type.kind == PVIR_ELEMENT_FLOAT) {
                ebits == 32 ? la_xvfadd_s(res, lhs, rhs) : la_xvfadd_d(res, lhs, rhs);
            } else {
                ebits == 8 ? la_xvadd_b(res, lhs, rhs)
                           : (ebits == 16 ? la_xvadd_h(res, lhs, rhs)
                                          : (ebits == 32 ? la_xvadd_w(res, lhs, rhs) : la_xvadd_d(res, lhs, rhs)));
            }
            break;
        case PVIR_OP_SUB:
            if (op->type.kind == PVIR_ELEMENT_FLOAT) {
                ebits == 32 ? la_xvfsub_s(res, lhs, rhs) : la_xvfsub_d(res, lhs, rhs);
            } else {
                ebits == 8 ? la_xvsub_b(res, lhs, rhs)
                           : (ebits == 16 ? la_xvsub_h(res, lhs, rhs)
                                          : (ebits == 32 ? la_xvsub_w(res, lhs, rhs) : la_xvsub_d(res, lhs, rhs)));
            }
            break;
        case PVIR_OP_MUL:
            if (op->type.kind == PVIR_ELEMENT_FLOAT) {
                ebits == 32 ? la_xvfmul_s(res, lhs, rhs) : la_xvfmul_d(res, lhs, rhs);
            } else {
                ebits == 8 ? la_xvmul_b(res, lhs, rhs)
                           : (ebits == 16 ? la_xvmul_h(res, lhs, rhs)
                                          : (ebits == 32 ? la_xvmul_w(res, lhs, rhs) : la_xvmul_d(res, lhs, rhs)));
            }
            break;
        case PVIR_OP_SHL:
            ebits == 8 ? la_xvsll_b(res, lhs, rhs)
                       : (ebits == 16 ? la_xvsll_h(res, lhs, rhs)
                                      : (ebits == 32 ? la_xvsll_w(res, lhs, rhs) : la_xvsll_d(res, lhs, rhs)));
            break;
        case PVIR_OP_SHR:
            ebits == 8 ? la_xvsrl_b(res, lhs, rhs)
                       : (ebits == 16 ? la_xvsrl_h(res, lhs, rhs)
                                      : (ebits == 32 ? la_xvsrl_w(res, lhs, rhs) : la_xvsrl_d(res, lhs, rhs)));
            break;
        case PVIR_OP_SAR:
            ebits == 8 ? la_xvsra_b(res, lhs, rhs)
                       : (ebits == 16 ? la_xvsra_h(res, lhs, rhs)
                                      : (ebits == 32 ? la_xvsra_w(res, lhs, rhs) : la_xvsra_d(res, lhs, rhs)));
            break;
        case PVIR_OP_MAX:
            if (is_signed) {
                ebits == 8 ? la_xvmax_b(res, lhs, rhs)
                           : (ebits == 16 ? la_xvmax_h(res, lhs, rhs)
                                          : (ebits == 32 ? la_xvmax_w(res, lhs, rhs) : la_xvmax_d(res, lhs, rhs)));
            } else {
                ebits == 8 ? la_xvmax_bu(res, lhs, rhs)
                           : (ebits == 16 ? la_xvmax_hu(res, lhs, rhs)
                                          : (ebits == 32 ? la_xvmax_wu(res, lhs, rhs) : la_xvmax_du(res, lhs, rhs)));
            }
            break;
        case PVIR_OP_MIN:
        default:
            if (is_signed) {
                ebits == 8 ? la_xvmin_b(res, lhs, rhs)
                           : (ebits == 16 ? la_xvmin_h(res, lhs, rhs)
                                          : (ebits == 32 ? la_xvmin_w(res, lhs, rhs) : la_xvmin_d(res, lhs, rhs)));
            } else {
                ebits == 8 ? la_xvmin_bu(res, lhs, rhs)
                           : (ebits == 16 ? la_xvmin_hu(res, lhs, rhs)
                                          : (ebits == 32 ? la_xvmin_wu(res, lhs, rhs) : la_xvmin_du(res, lhs, rhs)));
            }
            break;
        }
        if (op->inactive_policy == PVIR_INACTIVE_MERGE) la_xvbitsel_v(res, oldv, res, mask);
        else if (op->inactive_policy == PVIR_INACTIVE_ZERO) la_xvand_v(res, res, mask);
        store_z_chunk(res, zd_off + (int)b, rem);
        ra_free_temp(mask); ra_free_temp(res); ra_free_temp(rhs); ra_free_temp(lhs); ra_free_temp(oldv);
    }
    if (!const_fold && mkey.valid && !cache_hit) {
        s->sve_pvir_builder->expanded_mask_key = mkey;
    }
    return true;
}

static bool emit_lsx_chunk_mask(int pred_off, size_t byte_off, uint16_t ebits,
                                IR2_OPND mask)
{
    IR2_OPND packed = ra_alloc_itemp();
    unsigned elem_bytes = (unsigned)(ebits / 8u);

    la_vxor_v(mask, mask, mask);
    lata_env_ld_wu(packed, pred_off + (int)(byte_off / 8u));
    if (ebits == 64) {
        for (int lane = 0; lane < 2; ++lane) {
            IR2_OPND bit = ra_alloc_itemp();
            IR2_OPND lm = ra_alloc_itemp();
            la_bstrpick_d(bit, packed, lane * 8, lane * 8);
            la_sub_d(lm, zero_ir2_opnd, bit);
            la_vinsgr2vr_d(mask, lm, lane);
            ra_free_temp(lm);
            ra_free_temp(bit);
        }
    } else {
        for (int word_lane = 0; word_lane < 4; ++word_lane) {
            IR2_OPND word = ra_alloc_itemp();
            li_d(word, 0);
            for (unsigned bl = 0; bl < 4; bl += elem_bytes) {
                int bit_index = word_lane * 4 + (int)bl;
                IR2_OPND bit = ra_alloc_itemp();
                IR2_OPND lm = ra_alloc_itemp();
                la_bstrpick_d(bit, packed, bit_index, bit_index);
                la_sub_d(lm, zero_ir2_opnd, bit);
                la_bstrpick_d(lm, lm, (int)ebits - 1, 0);
                if (bl != 0) {
                    la_slli_d(lm, lm, (int)(bl * 8u));
                }
                la_or(word, word, lm);
                ra_free_temp(lm);
                ra_free_temp(bit);
            }
            la_vinsgr2vr_w(mask, word, word_lane);
            ra_free_temp(word);
        }
    }
    ra_free_temp(packed);
    return true;
}

static bool lower_sve_lsx_core(DisasContext *s,
                               const PvirInstruction *instruction,
                               const A64SVEIR1Instruction *source)
{
    size_t vl_bytes = (size_t)s->vl;

    if (instruction->opcode == PVIR_OP_PREDICATE_GENERATE) {
        const PvirPredicateGeneratorOperation *g =
            &instruction->operation.predicate_generator;
        if (g->kind == PVIR_PREDICATE_GENERATOR_CONSTANT) {
            if (instruction->effects.arch_state.domain ==
                PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT) {
                return lower_ffr_set(s, instruction);
            }
            return lower_predicate_constant(s, instruction);
        }
        return false;
    }

    if (instruction->opcode == PVIR_OP_BROADCAST) {
        const PvirVectorOperation *op = &instruction->operation.vector;
        uint16_t ebits = op->type.element_bits;
        int zd = source != NULL ? (int)source->arg.f_sve_dup.zd
                                : find_zreg_for_value(s->sve_pvir_builder, op->destination);
        int rn = source != NULL ? (int)source->arg.f_sve_dup.rn
                                : find_gpr_for_value(op->operands[0]);
        int zd_off = env_offset_zreg(zd);
        if (zd_off < 0) return false;
        IR2_OPND gpr = (rn >= 0 && rn < 31) ? alloc_gpr_src(rn) : zero_ir2_opnd;
        if (op->operand_kinds[0] == PVIR_OPERAND_IMMEDIATE) {
            gpr = ra_alloc_itemp();
            li_d(gpr, (int64_t)op->operand_immediates[0]);
        }
        for (size_t b = 0; b < vl_bytes; b += 16u) {
            IR2_OPND v = ra_alloc_ftemp();
            if (ebits == 8) la_vreplgr2vr_b(v, gpr);
            else if (ebits == 16) la_vreplgr2vr_h(v, gpr);
            else if (ebits == 32) la_vreplgr2vr_w(v, gpr);
            else la_vreplgr2vr_d(v, gpr);
            lata_env_vst(v, zd_off + (int)b);
            ra_free_temp(v);
        }
        if (op->operand_kinds[0] == PVIR_OPERAND_IMMEDIATE) {
            ra_free_temp(gpr);
        } else if (rn >= 0 && rn < 31) {
            free_alloc_gpr(gpr);
        }
        return true;
    }

    /* Binary vector ops lowered in 128-bit LSX chunks (ZERO xv* emitted). */
    switch (instruction->opcode) {
    case PVIR_OP_ADD:
    case PVIR_OP_SUB:
    case PVIR_OP_MUL:
    case PVIR_OP_AND:
    case PVIR_OP_OR:
    case PVIR_OP_XOR:
    case PVIR_OP_SHL:
    case PVIR_OP_SHR:
    case PVIR_OP_SAR:
    case PVIR_OP_MIN:
    case PVIR_OP_MAX:
        break;
    default:
        return false;
    }

    const PvirVectorOperation *op = &instruction->operation.vector;
    uint16_t ebits = op->type.element_bits;
    bool is_flt = (op->type.kind == PVIR_ELEMENT_FLOAT);
    bool is_signed = (op->type.signedness == PVIR_SIGNED);
    if (op->operand_kinds[1] != PVIR_OPERAND_VECTOR) {
        return false;
    }
    if (is_flt && ebits != 32 && ebits != 64) {
        return false;
    }
    int zd = find_zreg_for_value(s->sve_pvir_builder, op->destination);
    int zn = find_zreg_for_value(s->sve_pvir_builder, op->operands[0]);
    int zm = find_zreg_for_value(s->sve_pvir_builder, op->operands[1]);
    int pg = (op->predicate.source.kind == PVIR_PREDICATE_SOURCE_PREDICATE)
                 ? find_preg_for_value(s->sve_pvir_builder, op->predicate.source.id)
                 : -1;
    if (source != NULL && source->insn_type >= AARCH64_A64_SVE_ADD &&
        source->insn_type <= AARCH64_A64_SVE_FDIV) {
        zd = source->arg.f_sve_add.zd;
        zn = zd;
        zm = source->arg.f_sve_add.zm;
        pg = source->arg.f_sve_add.pg;
    }
    if (zd < 0 || zn < 0 || zm < 0) return false;
    int zd_off = env_offset_zreg(zd), zn_off = env_offset_zreg(zn);
    int zm_off = env_offset_zreg(zm);
    int pg_off = (pg >= 0) ? env_offset_sve_preg(pg) : -1;
    if (zd_off < 0 || zn_off < 0 || zm_off < 0) return false;
    if (op->predicate.form == PVIR_PREDICATE_VECTOR_MASK && pg_off < 0) return false;
    if (op->predicate.form != PVIR_PREDICATE_VECTOR_MASK &&
        op->predicate.form != PVIR_PREDICATE_ALL_TRUE) {
        return false;
    }

    for (size_t b = 0; b < vl_bytes; b += 16u) {
        IR2_OPND oldv = ra_alloc_ftemp();
        IR2_OPND lhs = ra_alloc_ftemp();
        IR2_OPND rhs = ra_alloc_ftemp();
        IR2_OPND res = ra_alloc_ftemp();
        IR2_OPND mask = ra_alloc_ftemp();

        lata_env_vld(oldv, zd_off + (int)b);
        lata_env_vld(lhs, zn_off + (int)b);
        lata_env_vld(rhs, zm_off + (int)b);
        if (op->predicate.form == PVIR_PREDICATE_ALL_TRUE) {
            la_vseq_b(mask, mask, mask);
        } else {
            emit_lsx_chunk_mask(pg_off, b, ebits, mask);
        }
        if (is_flt) {
            if (instruction->opcode == PVIR_OP_ADD) {
                ebits == 32 ? la_vfadd_s(res, lhs, rhs) : la_vfadd_d(res, lhs, rhs);
            } else if (instruction->opcode == PVIR_OP_SUB) {
                ebits == 32 ? la_vfsub_s(res, lhs, rhs) : la_vfsub_d(res, lhs, rhs);
            } else {
                ebits == 32 ? la_vfmul_s(res, lhs, rhs) : la_vfmul_d(res, lhs, rhs);
            }
        } else {
            switch (instruction->opcode) {
            case PVIR_OP_AND: la_vand_v(res, lhs, rhs); break;
            case PVIR_OP_OR: la_vor_v(res, lhs, rhs); break;
            case PVIR_OP_XOR: la_vxor_v(res, lhs, rhs); break;
            case PVIR_OP_ADD:
                ebits == 8 ? la_vadd_b(res, lhs, rhs)
                           : (ebits == 16 ? la_vadd_h(res, lhs, rhs)
                                          : (ebits == 32 ? la_vadd_w(res, lhs, rhs) : la_vadd_d(res, lhs, rhs)));
                break;
            case PVIR_OP_SUB:
                ebits == 8 ? la_vsub_b(res, lhs, rhs)
                           : (ebits == 16 ? la_vsub_h(res, lhs, rhs)
                                          : (ebits == 32 ? la_vsub_w(res, lhs, rhs) : la_vsub_d(res, lhs, rhs)));
                break;
            case PVIR_OP_MUL:
                ebits == 8 ? la_vmul_b(res, lhs, rhs)
                           : (ebits == 16 ? la_vmul_h(res, lhs, rhs)
                                          : (ebits == 32 ? la_vmul_w(res, lhs, rhs) : la_vmul_d(res, lhs, rhs)));
                break;
            case PVIR_OP_SHL:
                ebits == 8 ? la_vsll_b(res, lhs, rhs)
                           : (ebits == 16 ? la_vsll_h(res, lhs, rhs)
                                          : (ebits == 32 ? la_vsll_w(res, lhs, rhs) : la_vsll_d(res, lhs, rhs)));
                break;
            case PVIR_OP_SHR:
                ebits == 8 ? la_vsrl_b(res, lhs, rhs)
                           : (ebits == 16 ? la_vsrl_h(res, lhs, rhs)
                                          : (ebits == 32 ? la_vsrl_w(res, lhs, rhs) : la_vsrl_d(res, lhs, rhs)));
                break;
            case PVIR_OP_SAR:
                ebits == 8 ? la_vsra_b(res, lhs, rhs)
                           : (ebits == 16 ? la_vsra_h(res, lhs, rhs)
                                          : (ebits == 32 ? la_vsra_w(res, lhs, rhs) : la_vsra_d(res, lhs, rhs)));
                break;
            case PVIR_OP_MAX:
                if (is_signed) {
                    ebits == 8 ? la_vmax_b(res, lhs, rhs)
                               : (ebits == 16 ? la_vmax_h(res, lhs, rhs)
                                              : (ebits == 32 ? la_vmax_w(res, lhs, rhs) : la_vmax_d(res, lhs, rhs)));
                } else {
                    ebits == 8 ? la_vmax_bu(res, lhs, rhs)
                               : (ebits == 16 ? la_vmax_hu(res, lhs, rhs)
                                              : (ebits == 32 ? la_vmax_wu(res, lhs, rhs) : la_vmax_du(res, lhs, rhs)));
                }
                break;
            default:
                if (is_signed) {
                    ebits == 8 ? la_vmin_b(res, lhs, rhs)
                               : (ebits == 16 ? la_vmin_h(res, lhs, rhs)
                                              : (ebits == 32 ? la_vmin_w(res, lhs, rhs) : la_vmin_d(res, lhs, rhs)));
                } else {
                    ebits == 8 ? la_vmin_bu(res, lhs, rhs)
                               : (ebits == 16 ? la_vmin_hu(res, lhs, rhs)
                                              : (ebits == 32 ? la_vmin_wu(res, lhs, rhs) : la_vmin_du(res, lhs, rhs)));
                }
                break;
            }
        }
        if (op->inactive_policy == PVIR_INACTIVE_MERGE) {
            la_vbitsel_v(res, oldv, res, mask);
        } else if (op->inactive_policy == PVIR_INACTIVE_ZERO) {
            la_vand_v(res, res, mask);
        }
        lata_env_vst(res, zd_off + (int)b);
        ra_free_temp(mask);
        ra_free_temp(res);
        ra_free_temp(rhs);
        ra_free_temp(lhs);
        ra_free_temp(oldv);
    }
    return true;
}

static bool lower_sve_lasx_dispatch(DisasContext *s,
                                    const PvirInstruction *instruction,
                                    const A64SVEIR1Instruction *source)
{
    if (!sve_context_valid(s) || instruction == NULL) {
        return false;
    }

    if (source != NULL) {
        switch (source->insn_type) {
        case AARCH64_A64_SVE_PTRUE:
        case AARCH64_A64_SVE_PFALSE:
            return lower_predicate_constant(s, instruction);
        case AARCH64_A64_SVE_WHILELO:
        case AARCH64_A64_SVE_WHILE:
            return lower_predicate_lane_compare(s, source, instruction);
        case AARCH64_A64_SVE_ADD:
        case AARCH64_A64_SVE_SUB:
        case AARCH64_A64_SVE_MUL:
        case AARCH64_A64_SVE_ORR:
        case AARCH64_A64_SVE_AND:
        case AARCH64_A64_SVE_EOR:
        case AARCH64_A64_SVE_LSL:
        case AARCH64_A64_SVE_LSR:
        case AARCH64_A64_SVE_ASR:
        case AARCH64_A64_SVE_SMAX:
        case AARCH64_A64_SVE_UMAX:
        case AARCH64_A64_SVE_SMIN:
        case AARCH64_A64_SVE_UMIN:
            return lower_integer_binary(s, source, instruction);
        case AARCH64_A64_SVE_FADD:
        case AARCH64_A64_SVE_FSUB:
        case AARCH64_A64_SVE_FMUL:
        case AARCH64_A64_SVE_FDIV:
            return lower_fp_binary(s, source, instruction);
        case AARCH64_A64_SVE_FABS:
        case AARCH64_A64_SVE_FNEG:
            return lower_fp_unary(s, source, instruction);
        case AARCH64_A64_SVE_NOT:
            return lower_integer_unary(s, source, instruction);
        case AARCH64_A64_SVE_DUP:
            return lower_broadcast(s, source, instruction);
        case AARCH64_A64_SVE_SETFFR:
            return lower_ffr_set(s, instruction);
        case AARCH64_A64_SVE_RDFFR:
            return lower_ffr_read(s, source, instruction);
        case AARCH64_A64_SVE_WRFFR:
            return lower_ffr_write(s, source, instruction);
        case AARCH64_A64_SVE_LD1B:
        case AARCH64_A64_SVE_LD1H:
        case AARCH64_A64_SVE_LD1W:
        case AARCH64_A64_SVE_LD1D:
        case AARCH64_A64_SVE_ST1B:
        case AARCH64_A64_SVE_ST1H:
        case AARCH64_A64_SVE_ST1W:
        case AARCH64_A64_SVE_ST1D:
        case AARCH64_A64_SVE_LD1SB:
        case AARCH64_A64_SVE_LD1SH:
        case AARCH64_A64_SVE_LD1SW:
            return lower_contiguous_memory(s, source, instruction);
        case AARCH64_A64_SVE_LDFF1B:
        case AARCH64_A64_SVE_LDFF1H:
        case AARCH64_A64_SVE_LDFF1W:
        case AARCH64_A64_SVE_LDFF1D:
        case AARCH64_A64_SVE_LDFF1SB:
        case AARCH64_A64_SVE_LDFF1SH:
        case AARCH64_A64_SVE_LDFF1SW:
        case AARCH64_A64_SVE_LDNF1B:
        case AARCH64_A64_SVE_LDNF1H:
        case AARCH64_A64_SVE_LDNF1W:
        case AARCH64_A64_SVE_LDNF1D:
        case AARCH64_A64_SVE_LDNF1SB:
        case AARCH64_A64_SVE_LDNF1SH:
        case AARCH64_A64_SVE_LDNF1SW:
            return lower_fault_load(s, source, instruction);
        case AARCH64_A64_SVE_COMPACT:
            return lower_compact(s, source, instruction);
        case AARCH64_A64_SVE_PTEST:
            return lower_ptest(s, source, instruction);
        case AARCH64_A64_SVE_BRK:
        case AARCH64_A64_SVE_PFIRST:
        case AARCH64_A64_SVE_PNEXT:
        case AARCH64_A64_SVE_PRED_LOGICAL:
            return lower_pred_op(s, source, instruction);
        case AARCH64_A64_SVE_CNTP:
        case AARCH64_A64_SVE_CNT_SCALAR:
            return lower_cnt(s, source, instruction);
        case AARCH64_A64_SVE_LAST:
            return lower_last(s, source, instruction);
        case AARCH64_A64_SVE_LD1_SCALAR:
        case AARCH64_A64_SVE_ST1_SCALAR:
        case AARCH64_A64_SVE_LDN:
        case AARCH64_A64_SVE_STN:
        case AARCH64_A64_SVE_LD1R:
        case AARCH64_A64_SVE_LD1RQ:
        case AARCH64_A64_SVE_GATHER_LD:
        case AARCH64_A64_SVE_SCATTER_ST:
        case AARCH64_A64_SVE_LDNT1:
        case AARCH64_A64_SVE_STNT1:
            return lower_mem_ext(s, source, instruction);
        case AARCH64_A64_SVE_FMLA:
            return lower_fma_op(s, source, instruction);
        case AARCH64_A64_SVE_FMINMAX:
        case AARCH64_A64_SVE_FP_MATH:
            return lower_fp_arith_op(s, source, instruction);
        case AARCH64_A64_SVE_FCMP:
        case AARCH64_A64_SVE_ICMP:
            return lower_compare_op(s, source, instruction);
        case AARCH64_A64_SVE_FCVT:
            return lower_convert_op(s, source, instruction);
        case AARCH64_A64_SVE_FREDUCE:
        case AARCH64_A64_SVE_IREDUCE:
            return lower_reduce_op(s, source, instruction);
        case AARCH64_A64_SVE_FIXED_INT:
            return lower_fixed_op(s, source, instruction);
        case AARCH64_A64_SVE_SHIFT_IMM:
            return lower_shift_imm_op(s, source, instruction);
        case AARCH64_A64_SVE_INT_UNARY:
            return lower_int_unary_ext(s, source, instruction);
        case AARCH64_A64_SVE_REV:
        case AARCH64_A64_SVE_PERMUTE:
            return (instruction->opcode == PVIR_OP_INSERT)
                       ? lower_insert_op(s, source, instruction)
                       : lower_permute_op(s, source, instruction);
        case AARCH64_A64_SVE_INDEX:
            return lower_index_op(s, source, instruction);
        case AARCH64_A64_SVE_WMUL:
            return lower_wmul_op(s, source, instruction);
        case AARCH64_A64_SVE_IMM_BIN:
            return lower_imm_bin_op(s, source, instruction);
        case AARCH64_A64_SVE_SEL:
            return lower_select_op(s, source, instruction);
        case AARCH64_A64_SVE_IDIV:
            return lower_div_rem_op(s, source, instruction);
        default:
            break;
        }
    }

    /* Generic PVIR-level dispatch (handles direct PVIR_OP_* lowering tests too). */
    switch (instruction->opcode) {
    case PVIR_OP_PREDICATE_GENERATE:
        if (instruction->operation.predicate_generator.kind ==
            PVIR_PREDICATE_GENERATOR_CONSTANT) {
            return (instruction->effects.arch_state.domain ==
                    PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT)
                       ? lower_ffr_set(s, instruction)
                       : lower_predicate_constant(s, instruction);
        }
        if (instruction->operation.predicate_generator.kind ==
            PVIR_PREDICATE_GENERATOR_LANE_COMPARE) {
            return lower_predicate_lane_compare(s, NULL, instruction);
        }
        return lower_ffr_read(s, NULL, instruction);
    case PVIR_OP_ARCH_STATE:
        if (instruction->effects.arch_state.domain ==
            PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT) {
            if (instruction->effects.arch_state.action == PVIR_ARCH_STATE_CLEAR) {
                return true;
            }
            return lower_ffr_write(s, NULL, instruction);
        }
        return lower_ptest(s, NULL, instruction);
    case PVIR_OP_COMPARE:
        return lower_compare_op(s, NULL, instruction);
    case PVIR_OP_SELECT:
        return lower_select_op(s, NULL, instruction);
    case PVIR_OP_FMA:
        return lower_fma_op(s, NULL, instruction);
    case PVIR_OP_TRUNCATE:
        return lower_truncate_op(s, instruction);
    case PVIR_OP_EXTEND:
    case PVIR_OP_MOVE:
        return lower_int_unary_ext(s, NULL, instruction);
    case PVIR_OP_EXTRACT:
        return lower_extract_op(s, NULL, instruction);
    case PVIR_OP_INSERT:
        return lower_insert_op(s, NULL, instruction);
    case PVIR_OP_CONVERT:
        return lower_convert_op(s, NULL, instruction);
    case PVIR_OP_FIXED:
        return lower_fixed_op(s, NULL, instruction);
    case PVIR_OP_FP_ARITH:
        return lower_fp_arith_op(s, NULL, instruction);
    case PVIR_OP_LANE_INDEX:
        return lower_index_op(s, NULL, instruction);
    case PVIR_OP_REDUCE:
        return lower_reduce_op(s, NULL, instruction);
    case PVIR_OP_PERMUTE:
        return lower_permute_op(s, NULL, instruction);
    case PVIR_OP_DIV:
    case PVIR_OP_REM:
        return lower_div_rem_op(s, NULL, instruction);
    case PVIR_OP_ADD:
    case PVIR_OP_SUB:
    case PVIR_OP_MUL:
    case PVIR_OP_AND:
    case PVIR_OP_OR:
    case PVIR_OP_XOR:
    case PVIR_OP_SHL:
    case PVIR_OP_SHR:
    case PVIR_OP_SAR:
    case PVIR_OP_MIN:
    case PVIR_OP_MAX:
        return lower_generic_binary_pvir(s, instruction);
    default:
        return false;
    }
}

/*
 * Fall back to the scalar helper, or refuse to translate.
 *
 * Refusing is the half that used to be missing. A validated PVIR instruction is
 * not automatically executable by the helper: the shape may be one the helper
 * does not implement, and generated code for such a shape runs and does
 * nothing. Returning false makes the caller raise an undefined instruction,
 * which is the architectural answer for an instruction we do not implement --
 * and it is visible, unlike a no-op.
 *
 * The same path serves both the LSX and LASX backends so that "lowering failed"
 * always means the same thing regardless of host SIMD width.
 */
static bool sve_helper_fallback(DisasContext *s, const PvirInstruction *insn,
                                const A64SVEIR1Instruction *source,
                                bool ftemp_exhausted)
{
    if (ftemp_exhausted) {
        ra_ftemp_note_fallback();
    }
    if (pvir_instruction_validate(insn) != PVIR_STATUS_OK) {
        return false;
    }
    if (lata_sve_helper_support(insn, source) != LATA_SVE_HELPER_SUPPORTED) {
        return false;
    }
    return emit_sve_helper_call(s, insn, source);
}

bool a64_sve_lower_pvir_instruction(DisasContext *s,
                                    const PvirInstruction *instruction,
                                    const A64SVEIR1Instruction *source)
{
    A64SVEPvirBuilder bsnap;
    LataIr2Snapshot ir2_snap;
    A64SVEBackendMode backend;
    bool lowered;
    bool ftemp_exhausted;

    if (!sve_context_valid(s) || instruction == NULL) {
        return false;
    }

    lata_flush_vregs_for_sve();
    backend = a64_sve_select_backend();

    if (backend == A64_SVE_BACKEND_SCALAR_HELPER) {
        return sve_helper_fallback(s, instruction, source, false);
    }

    bsnap = *s->sve_pvir_builder;
    lata_ir2_snapshot_take(&ir2_snap);
    ra_ftemp_clear_failed();

    if (backend == A64_SVE_BACKEND_LSX) {
        lowered = lower_sve_lsx_core(s, instruction, source);
    } else {
        lowered = lower_sve_lasx_dispatch(s, instruction, source);
    }
    ftemp_exhausted = ra_ftemp_alloc_failed();
    if (lowered && !ftemp_exhausted) {
        return true;
    }

    /* Roll the half-emitted block back before re-lowering it another way;
     * leaving it in place would run both versions. */
    *s->sve_pvir_builder = bsnap;
    lata_ir2_snapshot_rollback(&ir2_snap);
    ra_ftemp_clear_failed();
    return sve_helper_fallback(s, instruction, source, ftemp_exhausted);
}

static bool translate_sve_pvir(DisasContext *s)
{
    A64SVEIR1Instruction source;
    PvirInstruction instruction;
    A64SVEPvirBuilder builder_snap;
    LataIr2Snapshot ir2_snap;

    if (!sve_context_valid(s)) {
        return false;
    }
    builder_snap = *s->sve_pvir_builder;
    lata_ir2_snapshot_take(&ir2_snap);

    if (!build_instruction(s, &source, &instruction) ||
        !a64_sve_lower_pvir_instruction(s, &instruction, &source)) {
        *s->sve_pvir_builder = builder_snap;
        lata_ir2_snapshot_rollback(&ir2_snap);
        return false;
    }
    return true;
}

bool trans_sve_ptrue(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_pfalse(DisasContext *s)
{
    return translate_sve_pvir(s);
}

bool trans_sve_orr(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_and(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_eor(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_lsl(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_lsr(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_asr(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_smax(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_umax(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_smin(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_umin(DisasContext *s)
{
    return translate_sve_pvir(s);
}

bool trans_sve_add(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_sub(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_mul(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_fadd(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_fsub(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_fmul(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_fdiv(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_fabs(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_fneg(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_not(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_dup(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_whilelo(DisasContext *s)
{
    return translate_sve_pvir(s);
}

bool trans_sve_ld1b(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_setffr(DisasContext *s)
{
    return translate_sve_pvir(s);
}

bool trans_sve_rdffr(DisasContext *s)
{
    return translate_sve_pvir(s);
}

bool trans_sve_st1b(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_ld1h(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_ld1w(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_ld1d(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_st1h(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_st1w(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_st1d(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_ld1sb(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_ld1sh(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_ld1sw(DisasContext *s)
{
    return translate_sve_pvir(s);
}
bool trans_sve_wrffr(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldff1b(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldff1h(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldff1w(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldff1d(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldff1sb(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldff1sh(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldff1sw(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldnf1b(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldnf1h(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldnf1w(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldnf1d(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldnf1sb(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldnf1sh(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldnf1sw(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_compact(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ptest(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_while(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_cntp(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_cnt_scalar(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_brk(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_pfirst(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_pnext(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_last(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_pred_logical(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ld1_scalar(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_st1_scalar(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldn(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_stn(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ld1r(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ld1rq(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_gather_ld(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_scatter_st(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ldnt1(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_stnt1(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_fmla(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_fminmax(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_fcmp(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_fcvt(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_fp_math(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_freduce(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_ireduce(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_fixed_int(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_shift_imm(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_int_unary(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_rev(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_permute(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_index(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_wmul(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_imm_bin(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_icmp(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_sel(DisasContext *s) { return translate_sve_pvir(s); }
bool trans_sve_idiv(DisasContext *s) { return translate_sve_pvir(s); }
