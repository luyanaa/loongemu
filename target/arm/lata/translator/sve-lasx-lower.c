#include "sve-lasx-lower.h"
#include "target/arm/lata/include/translate.h"

#include "target/arm/lata/ir1/sve-pvir.h"
#include "target/arm/lata/include/env.h"

static bool sve_context_valid(const DisasContext *s)
{
    return s != NULL && s->sve_pvir_builder != NULL && s->vl > 0 &&
           arm_sve_vl_is_valid((uint32_t)s->vl * 8u);
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

static bool lower_ffr_read(DisasContext *s,
                           const A64SVEIR1Instruction *source,
                           const PvirInstruction *instruction)
{
    const arg_sve_ffr *ffr = &source->arg.f_sve_ffr;
    const PvirPredicateGeneratorOperation *generator =
        &instruction->operation.predicate_generator;
    size_t predicate_bytes = (size_t)s->vl / 8u;
    int ffr_offset = env_offset_sve_ffr();
    int predicate_offset;
    IR2_OPND packed;

    if (source->insn_type != AARCH64_A64_SVE_RDFFR || !ffr->has_pd ||
        ffr->pd >= A64_SVE_PVIR_NUM_PREGS ||
        instruction->opcode != PVIR_OP_PREDICATE_GENERATE ||
        generator->kind != PVIR_PREDICATE_GENERATOR_STATE_READ ||
        generator->state_id != A64_SVE_PVIR_P_STATE_BASE + ffr->pd ||
        generator->lane_type.kind != PVIR_ELEMENT_BITS ||
        generator->lane_type.element_bits != 8 ||
        generator->lane_type.vector_bits != (size_t)s->vl * 8u ||
        generator->source_state_value != s->sve_pvir_builder->ffr_value ||
        generator->source_state.domain !=
            PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT ||
        generator->source_state.action != PVIR_ARCH_STATE_READ ||
        generator->source_state.version_before !=
            s->sve_pvir_builder->ffr_version ||
        generator->source_state.version_after !=
            s->sve_pvir_builder->ffr_version ||
        generator->destination != s->sve_pvir_builder->p_values[ffr->pd] ||
        instruction->effects.arch_state.domain !=
            PVIR_ARCH_STATE_DOMAIN_OTHER ||
        instruction->effects.arch_state.action != PVIR_ARCH_STATE_WRITE ||
        instruction->effects.arch_state.state_id != generator->state_id ||
        instruction->effects.arch_state.version_before == UINT64_MAX ||
        instruction->effects.arch_state.version_before + 1 !=
            instruction->effects.arch_state.version_after ||
        instruction->effects.arch_state.version_after !=
            s->sve_pvir_builder->p_versions[ffr->pd] ||
        ffr_offset < 0 ||
        (predicate_offset = env_offset_sve_preg(ffr->pd)) < 0) {
        return false;
    }

    packed = ra_alloc_itemp();
    for (size_t byte = 0; byte < predicate_bytes; ++byte) {
        size_t word_byte = byte & ~(sizeof(uint32_t) - 1u);
        unsigned shift = (unsigned)(byte - word_byte) * 8u;

        lata_env_ld_wu(packed, ffr_offset + (int)word_byte);
        la_bstrpick_d(packed, packed, shift + 7u, shift);
        lata_env_st_b(packed, predicate_offset + (int)byte);
    }
    ra_free_temp(packed);
    return true;
}

static bool lower_predicate_lane_compare(
    DisasContext *s, const A64SVEIR1Instruction *source,
    const PvirInstruction *instruction)
{
    const PvirPredicateGeneratorOperation *generator =
        &instruction->operation.predicate_generator;
    const arg_sve_whilelo *whilelo = &source->arg.f_sve_whilelo;
    size_t vl_bits = (size_t)s->vl * 8u;
    size_t vl_bytes = (size_t)s->vl;
    size_t element_bits = generator->lane_type.element_bits;
    size_t element_bytes;
    size_t predicate_bytes;
    unsigned predicate_reg;
    int predicate_offset;
    IR2_OPND source_start;
    IR2_OPND source_limit;
    IR2_OPND start;
    IR2_OPND limit;
    IR2_OPND candidate;
    IR2_OPND packed;
    IR2_OPND any_active;
    IR2_OPND predicate_base;
    IR2_OPND first_active;
    IR2_OPND last_active;
    IR2_OPND flags;
    IR2_OPND shift_value;

    if (source->insn_type != AARCH64_A64_SVE_WHILELO ||
        !whilelo->effects.writes_nzcv ||
        instruction->opcode != PVIR_OP_PREDICATE_GENERATE ||
        generator->kind != PVIR_PREDICATE_GENERATOR_LANE_COMPARE ||
        generator->lane_type.kind != PVIR_ELEMENT_BITS ||
        generator->lane_type.vector_bits != vl_bits ||
        (element_bits != 8 && element_bits != 16 &&
         element_bits != 32 && element_bits != 64) ||
        (generator->scalar_bits != 32 && generator->scalar_bits != 64) ||
        generator->scalar_bits != whilelo->scalar_bits ||
        generator->compare_condition != PVIR_COMPARE_LTU ||
        generator->form != PVIR_PREDICATE_VECTOR_MASK ||
        generator->scalar_start !=
            A64_SVE_PVIR_GPR_VALUE_BASE + whilelo->rn ||
        generator->scalar_limit !=
            A64_SVE_PVIR_GPR_VALUE_BASE + whilelo->rm ||
        whilelo->rn >= 32 || whilelo->rm >= 32 ||
        generator->state_id < A64_SVE_PVIR_P_STATE_BASE ||
        generator->state_id >= A64_SVE_PVIR_P_STATE_BASE +
                                   A64_SVE_PVIR_NUM_PREGS ||
        !instruction->effects.arch_state.present ||
        instruction->effects.arch_state.state_id != generator->state_id ||
        !generator->status_state.present ||
        generator->status_state.action != PVIR_ARCH_STATE_WRITE ||
        generator->status_state.state_id != A64_SVE_PVIR_STATUS_STATE) {
        return false;
    }

    predicate_reg = generator->state_id - A64_SVE_PVIR_P_STATE_BASE;
    predicate_offset = env_offset_sve_preg(predicate_reg);
    if (predicate_offset < 0) {
        return false;
    }
    element_bytes = element_bits / 8;

    source_start = alloc_gpr_src(whilelo->rn);
    source_limit = alloc_gpr_src(whilelo->rm);
    start = ra_alloc_itemp();
    limit = ra_alloc_itemp();
    if (generator->scalar_bits == 32) {
        la_mov32_zx(start, source_start);
        la_mov32_zx(limit, source_limit);
    } else {
        la_mov64(start, source_start);
        la_mov64(limit, source_limit);
    }
    free_alloc_gpr(source_limit);
    free_alloc_gpr(source_start);

    candidate = ra_alloc_itemp();
    packed = ra_alloc_itemp();
    any_active = ra_alloc_itemp();
    predicate_base = ra_alloc_itemp();
    li_d(any_active, 0);
    li_d(predicate_base, predicate_offset);
    la_add_d(predicate_base, env_ir2_opnd, predicate_base);

    predicate_bytes = vl_bytes / 8;
    for (size_t byte = 0; byte < predicate_bytes; ++byte) {
        li_d(packed, 0);
        for (unsigned bit = 0; bit < 8; ++bit) {
            size_t vector_byte = byte * 8 + bit;
            size_t lane;

            if (vector_byte % element_bytes != 0) {
                continue;
            }
            lane = vector_byte / element_bytes;
            if (generator->scalar_bits == 32) {
                /* PVIR lane-compare addition is modulo 2^scalar_bits. */
                la_addi_d(candidate, start, (int)lane);
                la_mov32_zx(candidate, candidate);
            } else {
                la_addi_d(candidate, start, (int)lane);
            }
            la_sltu(candidate, candidate, limit);
            la_or(any_active, any_active, candidate);
            if (bit != 0) {
                la_slli_d(candidate, candidate, bit);
            }
            la_or(packed, packed, candidate);
        }
        la_st_b(packed, predicate_base, (int)byte);
    }

    ra_free_temp(packed);
    ra_free_temp(candidate);
    ra_free_temp(limit);
    ra_free_temp(start);

    first_active = ra_alloc_itemp();
    last_active = ra_alloc_itemp();
    la_ld_bu(first_active, predicate_base, 0);
    la_bstrpick_d(first_active, first_active, 0, 0);
    {
        size_t last_vector_byte = vl_bytes - element_bytes;
        unsigned last_bit = last_vector_byte % 8;
        int last_byte = (int)(last_vector_byte / 8);

        la_ld_bu(last_active, predicate_base, last_byte);
        la_bstrpick_d(last_active, last_active, last_bit, last_bit);
    }
    ra_free_temp(predicate_base);

    /* DDI 0487 C8.2.922/J1.2.530: N=FirstActive, Z=NoneActive,
     * C=!LastActive, V=0. PSTATE NZCV occupies bits 31:28. */
    la_xori(any_active, any_active, 1);
    la_xori(last_active, last_active, 1);
    flags = ra_alloc_itemp();
    shift_value = ra_alloc_itemp();
    li_d(flags, 0);
    la_slli_d(shift_value, first_active, 31);
    la_or(flags, flags, shift_value);
    la_slli_d(shift_value, any_active, 30);
    la_or(flags, flags, shift_value);
    la_slli_d(shift_value, last_active, 29);
    la_or(flags, flags, shift_value);
    la_armmtflag(flags, 0x39);

    ra_free_temp(shift_value);
    ra_free_temp(flags);
    ra_free_temp(last_active);
    ra_free_temp(first_active);
    ra_free_temp(any_active);
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

    if (source->insn_type == AARCH64_A64_SVE_ADD) {
        binary = &source->arg.f_sve_add;
        subtract = false;
        multiply = false;
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

    if (instruction->opcode != (multiply ? PVIR_OP_MUL
                                         : subtract ? PVIR_OP_SUB
                                                    : PVIR_OP_ADD) ||
        operation->type.kind != PVIR_ELEMENT_BITS ||
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
            if (multiply) {
                la_xvmul_b(result_value, old_value, source_value);
            } else if (subtract) {
                la_xvsub_b(result_value, old_value, source_value);
            } else {
                la_xvadd_b(result_value, old_value, source_value);
            }
            break;
        case 16:
            if (multiply) {
                la_xvmul_h(result_value, old_value, source_value);
            } else if (subtract) {
                la_xvsub_h(result_value, old_value, source_value);
            } else {
                la_xvadd_h(result_value, old_value, source_value);
            }
            break;
        case 32:
            if (multiply) {
                la_xvmul_w(result_value, old_value, source_value);
            } else if (subtract) {
                la_xvsub_w(result_value, old_value, source_value);
            } else {
                la_xvadd_w(result_value, old_value, source_value);
            }
            break;
        case 64:
            if (multiply) {
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
        if (expanded_mask_cache_hit) {
            if (remaining >= 32) {
                lata_env_xvld(mask, expanded_mask_offset + (int)byte);
            } else {
                lata_env_vld(mask, expanded_mask_offset + (int)byte);
            }
            s->sve_pvir_builder->expanded_mask_chunks_reused++;
        } else {
            if (!lower_packed_predicate_mask(
                    mask, predicate_offset + (int)(byte / 8),
                    element_bits)) {
                ra_free_temp(result);
                ra_free_temp(mask);
                ra_free_temp(result_value);
                ra_free_temp(source_value);
                ra_free_temp(old_value);
                return false;
            }
            if (remaining >= 32) {
                lata_env_xvst(mask, expanded_mask_offset + (int)byte);
            } else {
                lata_env_vst(mask, expanded_mask_offset + (int)byte);
            }
            s->sve_pvir_builder->expanded_mask_chunks_materialized++;
        }
        la_xvbitsel_v(result, old_value, result_value, mask);
        if (remaining >= 32) {
            lata_env_xvst(result, destination_offset + (int)byte);
        } else {
            /* VL is a multiple of 16 bytes; store only the active low half. */
            lata_env_vst(result, destination_offset + (int)byte);
        }
        ra_free_temp(result);
        ra_free_temp(mask);
        ra_free_temp(result_value);
        ra_free_temp(source_value);
        ra_free_temp(old_value);
    }
    if (!expanded_mask_cache_hit) {
        s->sve_pvir_builder->expanded_mask_key = expanded_mask_key;
    }
    return true;
}

static bool lower_contiguous_memory(
    DisasContext *s, const A64SVEIR1Instruction *source,
    const PvirInstruction *instruction)
{
    const arg_sve_contiguous *memory = &source->arg.f_sve_contiguous;
    const PvirVectorOperation *operation = &instruction->operation.vector;
    bool is_load = source->insn_type == AARCH64_A64_SVE_LD1B;
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

    if ((source->insn_type != AARCH64_A64_SVE_LD1B &&
         source->insn_type != AARCH64_A64_SVE_ST1B) ||
        !sve_context_valid(s) || memory->zt >= A64_SVE_PVIR_NUM_ZREGS ||
        memory->pg >= 8 || memory->rn > 31 || memory->memory_bits != 8 ||
        !memory->effects.memory.access ||
        operation->type.kind != PVIR_ELEMENT_INTEGER ||
        operation->type.signedness != PVIR_UNSIGNED ||
        operation->type.vector_bits != vl_bytes * 8u ||
        (operation->type.element_bits != 8 &&
         operation->type.element_bits != 16 &&
         operation->type.element_bits != 32 &&
         operation->type.element_bits != 64) ||
        operation->memory_element_bits != 8 ||
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

    guest_base = alloc_gpr_src_sp(memory->rn);
    /* Mirror lata_clean_data_tbi() in translate-a64.c. */
    if (s->tbid != 0) {
        la_bstrpick_d(guest_base, guest_base, 56, 0);
    }

    adjusted_base = ra_alloc_itemp();
    li_d(adjusted_base, displacement);
    la_add_d(adjusted_base, guest_base, adjusted_base);
    free_alloc_gpr(guest_base);

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
        if (is_load) {
            la_ldx_bu(value, address, zero_ir2_opnd);
            la_st_b(value, sp_ir2_opnd, (int)vector_byte);
        } else {
            size_t aligned_byte = vector_byte & ~(sizeof(uint32_t) - 1u);
            unsigned byte_shift =
                (unsigned)(vector_byte - aligned_byte) * 8u;

            lata_env_ld_wu(value, vector_offset + (int)aligned_byte);
            la_bstrpick_d(value, value, byte_shift + 7u, byte_shift);
            la_stx_b(value, address, zero_ir2_opnd);
        }
        la_label(skip_lane);
    }

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
    return true;
}

static bool translate_sve_pvir(DisasContext *s)
{
    A64SVEIR1Instruction source;
    PvirInstruction instruction;

    if (!sve_context_valid(s) || !build_instruction(s, &source, &instruction)) {
        return false;
    }
    switch (source.insn_type) {
    case AARCH64_A64_SVE_PTRUE:
        return lower_predicate_constant(s, &instruction);
    case AARCH64_A64_SVE_WHILELO:
        return lower_predicate_lane_compare(s, &source, &instruction);
    case AARCH64_A64_SVE_ADD:
    case AARCH64_A64_SVE_SUB:
    case AARCH64_A64_SVE_MUL:
        return lower_integer_binary(s, &source, &instruction);
    case AARCH64_A64_SVE_SETFFR:
        return lower_ffr_set(s, &instruction);
    case AARCH64_A64_SVE_RDFFR:
        return lower_ffr_read(s, &source, &instruction);
    case AARCH64_A64_SVE_LD1B:
    case AARCH64_A64_SVE_ST1B:
        return lower_contiguous_memory(s, &source, &instruction);
    default:
        return false;
    }
}

bool trans_sve_ptrue(DisasContext *s)
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
