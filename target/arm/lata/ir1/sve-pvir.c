#include "sve-pvir.h"

#include "env.h"
#include "latx/compat/qemu/cpu-features.h"

#include <string.h>

static void init_instruction(PvirInstruction *instruction)
{
    memset(instruction, 0, sizeof(*instruction));
    instruction->effects.memory.access = PVIR_MEMORY_NONE;
    instruction->effects.memory.ordering = PVIR_MEMORY_ORDER_UNSPECIFIED;
    instruction->effects.memory.inactive_lanes =
        PVIR_INACTIVE_MEMORY_NOT_APPLICABLE;
    instruction->effects.fault.behavior = PVIR_FAULT_NONE;
    instruction->effects.fault.lane_order = PVIR_FAULT_ORDER_UNSPECIFIED;
    instruction->effects.fault.fault_result =
        PVIR_FAULT_RESULT_UNSPECIFIED;
    instruction->effects.fault.fault_value = PVIR_NO_VALUE;
    instruction->effects.fault.unknown_from_lane = PVIR_NO_VALUE;
    instruction->effects.fp_exceptions = PVIR_FP_EXCEPTIONS_NONE;
}

void a64_sve_pvir_builder_init(A64SVEPvirBuilder *builder)
{
    unsigned i;

    if (builder == NULL) {
        return;
    }
    memset(builder, 0, sizeof(*builder));
    for (i = 0; i < A64_SVE_PVIR_NUM_ZREGS; i++) {
        builder->z_values[i] = A64_SVE_PVIR_Z_VALUE_BASE + i;
    }
    for (i = 0; i < A64_SVE_PVIR_NUM_PREGS; i++) {
        builder->p_values[i] = A64_SVE_PVIR_P_VALUE_BASE + i;
    }
    builder->next_value_id = A64_SVE_PVIR_FIRST_TEMP_VALUE;
    builder->ffr_value = A64_SVE_PVIR_FFR_VALUE_BASE;
    builder->ffr_version = 0;
    builder->initialized = true;
}

static bool allocate_value(A64SVEPvirBuilder *builder, PvirValueId *value)
{
    if (builder->next_value_id == PVIR_NO_VALUE) {
        return false;
    }
    *value = builder->next_value_id++;
    return true;
}

static void set_state_write_domain(PvirArchStateEffect *effect,
                                   PvirArchStateDomain domain,
                                   uint32_t state_id,
                                   uint64_t version_before)
{
    effect->present = true;
    effect->domain = domain;
    effect->action = PVIR_ARCH_STATE_WRITE;
    effect->update_kind = PVIR_ARCH_STATE_UPDATE_NONE;
    effect->state_id = state_id;
    effect->version_before = version_before;
    effect->version_after = version_before + 1;
}

static void set_state_write(PvirArchStateEffect *effect, uint32_t state_id,
                            uint64_t version_before)
{
    set_state_write_domain(effect, PVIR_ARCH_STATE_DOMAIN_OTHER, state_id,
                           version_before);
}

/*
 * ARM pseudocode DecodePredCount: number of active elements for a PTRUE
 * pattern. Ported from the same formula as QEMU's decode_pred_count()
 * (target/arm/tcg/translate-sve.c), which cites the pseudocode directly.
 * MUL3/MUL4 round *down* (PTRUE keeps whole triplets/quads active).
 */
static size_t sve_pred_count(size_t elements, uint8_t pattern)
{
    size_t bound;

    switch (pattern) {
    case 0x00: /* POW2: largest power of two <= elements */
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
    case 0x08: /* VL1 .. VL8 */
        bound = pattern;
        break;
    case 0x09:
    case 0x0a:
    case 0x0b:
    case 0x0c:
    case 0x0d: /* VL16 .. VL256 */
        bound = (size_t)16 << (pattern - 0x09);
        break;
    case 0x1d: /* MUL4 */
        return elements - elements % 4;
    case 0x1e: /* MUL3 */
        return elements - elements % 3;
    case 0x1f: /* ALL */
        return elements;
    default: /* other uimm5 */
        return 0;
    }
    return elements >= bound ? bound : 0;
}

static uint64_t mask_for_bits(uint16_t bits)
{
    return bits >= 64 ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - UINT64_C(1));
}

static void record_known_const_preg(A64SVEPvirBuilder *next, uint8_t pd,
                                    uint8_t element_bits, uint32_t vl_bits,
                                    size_t active_lanes)
{
    size_t pred_bytes = vl_bits / 64u;
    size_t elem_bytes = element_bits / 8u;
    size_t lanes = vl_bits / element_bits;

    if (pd >= A64_SVE_PVIR_NUM_PREGS) {
        return;
    }
    memset(next->known_preg_bytes[pd], 0, A64_SVE_PVIR_MAX_PRED_BYTES);
    for (size_t lane = 0; lane < active_lanes && lane < lanes; ++lane) {
        size_t bit_idx = lane * elem_bytes;
        if (bit_idx / 8u < pred_bytes &&
            bit_idx / 8u < A64_SVE_PVIR_MAX_PRED_BYTES) {
            next->known_preg_bytes[pd][bit_idx / 8u] |=
                (uint8_t)(1u << (bit_idx % 8u));
        }
    }
    next->known_preg_valid_mask |= (uint16_t)(1u << pd);
    next->known_preg_version[pd] = next->p_versions[pd];
}

static void invalidate_known_preg(A64SVEPvirBuilder *next, uint8_t pd)
{
    if (pd < A64_SVE_PVIR_NUM_PREGS) {
        next->known_preg_valid_mask &= (uint16_t)~(1u << pd);
    }
}

static PvirStatus build_predicate_generator(
    A64SVEPvirBuilder *builder, A64SVEPvirBuilder *next,
    const A64SVEIR1Instruction *source, uint32_t vl_bits,
    uint8_t destination, PvirPredicateGeneratorOperation *operation,
    PvirInstruction *instruction)
{
    uint16_t element_bits;
    PvirVectorType lane_type;
    size_t lanes;
    uint64_t p_version;

    if (destination >= A64_SVE_PVIR_NUM_PREGS ||
        builder->p_versions[destination] == UINT64_MAX) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    p_version = builder->p_versions[destination];
    lane_type = (PvirVectorType){
        .kind = PVIR_ELEMENT_BITS,
        .signedness = PVIR_SIGNLESS,
        .element_bits = 0,
        .vector_bits = vl_bits,
    };
    switch (source->insn_type) {
    case AARCH64_A64_SVE_PTRUE:
    case AARCH64_A64_SVE_PFALSE:
        element_bits = source->arg.f_sve_ptrue.element_bits;
        break;
    case AARCH64_A64_SVE_WHILELO:
    case AARCH64_A64_SVE_WHILE:
        element_bits = source->arg.f_sve_whilelo.element_bits;
        break;
    default:
        return PVIR_STATUS_UNSUPPORTED_OPERATION;
    }
    lane_type.element_bits = element_bits;
    if (pvir_vector_type_lanes(lane_type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }

    init_instruction(instruction);
    instruction->opcode = PVIR_OP_PREDICATE_GENERATE;
    operation->destination = PVIR_NO_VALUE;
    operation->scalar_start = PVIR_NO_VALUE;
    operation->scalar_limit = PVIR_NO_VALUE;
    operation->lane_type = lane_type;
    operation->state_id = A64_SVE_PVIR_P_STATE_BASE + destination;
    operation->status_state = (PvirArchStateEffect){0};
    operation->conflict_check = false;
    operation->conflict_write_first = false;
    if (!allocate_value(next, &operation->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }

    instruction->effects.arch_state = (PvirArchStateEffect){0};
    set_state_write(&instruction->effects.arch_state, operation->state_id,
                    p_version);
    switch (source->insn_type) {
    case AARCH64_A64_SVE_PTRUE: {
        uint8_t pattern = source->arg.f_sve_ptrue.pattern;
        size_t active_lanes;

        operation->kind = PVIR_PREDICATE_GENERATOR_CONSTANT;
        operation->compare_condition = PVIR_COMPARE_NONE;
        operation->scalar_bits = 0;
        active_lanes = sve_pred_count(lanes, pattern);
        if (active_lanes > lanes) {
            return PVIR_STATUS_UNSUPPORTED_OPERATION;
        }
        if (active_lanes == lanes) {
            operation->form = PVIR_PREDICATE_ALL_TRUE;
        } else if (active_lanes == 0) {
            operation->form = PVIR_PREDICATE_ALL_FALSE;
        } else {
            operation->form = PVIR_PREDICATE_PREFIX;
        }
        operation->active_lanes =
            operation->form == PVIR_PREDICATE_ALL_TRUE ? lanes : active_lanes;
        if (source->arg.f_sve_ptrue.setflags) {
            if (next->status_version == UINT64_MAX) {
                return PVIR_STATUS_INSUFFICIENT_CAPACITY;
            }
            set_state_write(&operation->status_state,
                            A64_SVE_PVIR_STATUS_STATE,
                            next->status_version);
            next->status_version++;
        }
        next->p_values[destination] = operation->destination;
        next->p_versions[destination] = p_version + 1;
        record_known_const_preg(next, destination, (uint8_t)element_bits,
                                vl_bits, operation->active_lanes);
        return PVIR_STATUS_OK;
    }
    case AARCH64_A64_SVE_PFALSE: {
        operation->kind = PVIR_PREDICATE_GENERATOR_CONSTANT;
        operation->compare_condition = PVIR_COMPARE_NONE;
        operation->scalar_bits = 0;
        operation->form = PVIR_PREDICATE_ALL_FALSE;
        operation->active_lanes = 0;
        next->p_values[destination] = operation->destination;
        next->p_versions[destination] = p_version + 1;
        record_known_const_preg(next, destination, 8, vl_bits, 0);
        return PVIR_STATUS_OK;
    }
    case AARCH64_A64_SVE_WHILELO:
    case AARCH64_A64_SVE_WHILE: {
        const arg_sve_whilelo *whilelo = &source->arg.f_sve_whilelo;
        PvirCompareCondition cond =
            source->insn_type == AARCH64_A64_SVE_WHILELO
                ? PVIR_COMPARE_LTU
                : whilelo->cond;

        if ((whilelo->scalar_bits != 32 && whilelo->scalar_bits != 64) ||
            whilelo->rn >= 32 || whilelo->rm >= 32) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        operation->kind = PVIR_PREDICATE_GENERATOR_LANE_COMPARE;
        operation->form = PVIR_PREDICATE_VECTOR_MASK;
        operation->active_lanes = 0;
        operation->compare_condition = cond;
        operation->scalar_bits = whilelo->scalar_bits;
        operation->scalar_start = A64_SVE_PVIR_GPR_VALUE_BASE + whilelo->rn;
        operation->scalar_limit = A64_SVE_PVIR_GPR_VALUE_BASE + whilelo->rm;
        operation->conflict_check = whilelo->is_conflict;
        operation->conflict_write_first = whilelo->conflict_wr;
        if (whilelo->effects.writes_nzcv) {
            if (next->status_version == UINT64_MAX) {
                return PVIR_STATUS_INSUFFICIENT_CAPACITY;
            }
            set_state_write(&operation->status_state,
                            A64_SVE_PVIR_STATUS_STATE,
                            next->status_version);
            next->status_version++;
        }
        break;
    }
    default:
        return PVIR_STATUS_UNSUPPORTED_OPERATION;
    }

    next->p_values[destination] = operation->destination;
    next->p_versions[destination] = p_version + 1;
    invalidate_known_preg(next, destination);
    return PVIR_STATUS_OK;
}

static PvirStatus build_predicate_instruction(
    A64SVEPvirBuilder *builder, A64SVEPvirBuilder *next,
    const A64SVEIR1Instruction *source, uint32_t vl_bits,
    PvirInstruction *instruction)
{
    PvirPredicateGeneratorOperation *operation;
    uint8_t destination;
    PvirStatus status;

    if (source->insn_type == AARCH64_A64_SVE_PTRUE ||
        source->insn_type == AARCH64_A64_SVE_PFALSE) {
        destination = source->arg.f_sve_ptrue.pd;
    } else {
        destination = source->arg.f_sve_whilelo.pd;
    }
    operation = &instruction->operation.predicate_generator;
    status = build_predicate_generator(builder, next, source, vl_bits,
                                       destination, operation, instruction);
    return status;
}

static void set_state_update(PvirArchStateEffect *effect, uint32_t state_id,
                             uint64_t version_before)
{
    effect->present = true;
    effect->domain = PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT;
    effect->action = PVIR_ARCH_STATE_UPDATE;
    effect->update_kind = PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS;
    effect->state_id = state_id;
    effect->version_before = version_before;
    effect->version_after = version_before + 1;
}

static PvirStatus build_ffr_operation(A64SVEPvirBuilder *next,
                                      const A64SVEIR1Instruction *source,
                                      uint32_t vl_bits,
                                      PvirInstruction *instruction)
{
    const arg_sve_ffr *ffr = &source->arg.f_sve_ffr;
    PvirPredicateGeneratorOperation *operation;
    PvirVectorType lane_type = {
        .kind = PVIR_ELEMENT_BITS,
        .signedness = PVIR_SIGNLESS,
        .element_bits = 8,
        .vector_bits = vl_bits,
    };
    size_t lanes;

    if (pvir_vector_type_lanes(lane_type, &lanes) != PVIR_STATUS_OK ||
        ffr->effects.memory.access != PVIR_MEMORY_NONE ||
        ffr->effects.fault_behavior != PVIR_FAULT_NONE) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }

    init_instruction(instruction);
    if (source->insn_type == AARCH64_A64_SVE_WRFFR || ffr->is_write) {
        if (ffr->pn >= A64_SVE_PVIR_NUM_PREGS ||
            ffr->effects.state_domain != PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT ||
            ffr->effects.state_action != PVIR_ARCH_STATE_WRITE ||
            ffr->effects.writes_nzcv ||
            next->ffr_version == UINT64_MAX) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        instruction->opcode = PVIR_OP_ARCH_STATE;
        instruction->operation.arch_state.input_value = next->p_values[ffr->pn];
        instruction->operation.arch_state.output_value = PVIR_NO_VALUE;
        instruction->operation.arch_state.clear_from_lane = PVIR_NO_VALUE;
        set_state_write_domain(&instruction->effects.arch_state,
                               PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT,
                               A64_SVE_PVIR_FFR_STATE, next->ffr_version);
        next->ffr_value = next->p_values[ffr->pn];
        next->ffr_version++;
        return PVIR_STATUS_OK;
    }

    instruction->opcode = PVIR_OP_PREDICATE_GENERATE;
    operation = &instruction->operation.predicate_generator;
    operation->destination = PVIR_NO_VALUE;
    operation->lane_type = lane_type;
    operation->compare_condition = PVIR_COMPARE_NONE;
    operation->scalar_start = PVIR_NO_VALUE;
    operation->scalar_limit = PVIR_NO_VALUE;
    operation->source_state_value = PVIR_NO_VALUE;

    if (source->insn_type == AARCH64_A64_SVE_SETFFR) {
        if (ffr->has_pd ||
            ffr->effects.inactive_policy != PVIR_INACTIVE_NOT_APPLICABLE ||
            ffr->effects.writes_nzcv ||
            ffr->effects.state_domain !=
                PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT ||
            ffr->effects.state_action != PVIR_ARCH_STATE_WRITE ||
            ffr->effects.state_update != PVIR_ARCH_STATE_UPDATE_NONE ||
            !ffr->effects.ffr_initializes_all_true ||
            next->ffr_version == UINT64_MAX) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        operation->kind = PVIR_PREDICATE_GENERATOR_CONSTANT;
        operation->form = PVIR_PREDICATE_ALL_TRUE;
        operation->active_lanes = lanes;
        operation->state_id = A64_SVE_PVIR_FFR_STATE;
        if (!allocate_value(next, &operation->destination)) {
            return PVIR_STATUS_INSUFFICIENT_CAPACITY;
        }
        set_state_write_domain(&instruction->effects.arch_state,
                               PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT,
                               A64_SVE_PVIR_FFR_STATE, next->ffr_version);
        next->ffr_value = operation->destination;
        next->ffr_version++;
        return PVIR_STATUS_OK;
    }

    if (source->insn_type != AARCH64_A64_SVE_RDFFR || !ffr->has_pd ||
        ffr->pd >= A64_SVE_PVIR_NUM_PREGS ||
        (ffr->predicated && ffr->pg >= A64_SVE_PVIR_NUM_PREGS) ||
        (!ffr->predicated &&
         (ffr->effects.inactive_policy != PVIR_INACTIVE_NOT_APPLICABLE ||
          ffr->effects.writes_nzcv)) ||
        (ffr->predicated &&
         ffr->effects.inactive_policy != PVIR_INACTIVE_ZERO) ||
        ffr->effects.writes_nzcv != ffr->setflags ||
        ffr->effects.state_domain != PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT ||
        ffr->effects.state_action != PVIR_ARCH_STATE_READ ||
        ffr->effects.state_update != PVIR_ARCH_STATE_UPDATE_NONE ||
        ffr->effects.ffr_initializes_all_true ||
        next->p_versions[ffr->pd] == UINT64_MAX ||
        next->ffr_value == PVIR_NO_VALUE) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }

    operation->kind = PVIR_PREDICATE_GENERATOR_STATE_READ;
    operation->form = PVIR_PREDICATE_VECTOR_MASK;
    operation->active_lanes = 0;
    operation->state_id = A64_SVE_PVIR_P_STATE_BASE + ffr->pd;
    operation->source_state.present = true;
    operation->source_state.domain = PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT;
    operation->source_state.action = PVIR_ARCH_STATE_READ;
    operation->source_state.update_kind = PVIR_ARCH_STATE_UPDATE_NONE;
    operation->source_state.state_id = A64_SVE_PVIR_FFR_STATE;
    operation->source_state.version_before = next->ffr_version;
    operation->source_state.version_after = next->ffr_version;
    operation->source_state_value = next->ffr_value;
    if (ffr->setflags) {
        if (next->status_version == UINT64_MAX) {
            return PVIR_STATUS_INSUFFICIENT_CAPACITY;
        }
        set_state_write(&operation->status_state, A64_SVE_PVIR_STATUS_STATE,
                        next->status_version);
        next->status_version++;
    }
    if (!allocate_value(next, &operation->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    set_state_write(&instruction->effects.arch_state, operation->state_id,
                    next->p_versions[ffr->pd]);
    next->p_values[ffr->pd] = operation->destination;
    next->p_versions[ffr->pd]++;
    invalidate_known_preg(next, ffr->pd);
    return PVIR_STATUS_OK;
}

static bool fault_load_effects_valid(const arg_sve_effect_intent *effects,
                                    PvirFaultBehavior behavior)
{
    PvirFaultOrder order = behavior == PVIR_FAULT_FIRST_FAULT
                               ? PVIR_FAULT_ORDER_LANE_ASCENDING
                               : PVIR_FAULT_ORDER_UNSPECIFIED;

    return effects->inactive_policy == PVIR_INACTIVE_ZERO &&
           effects->memory.access == PVIR_MEMORY_READ &&
           effects->memory.ordering == PVIR_MEMORY_ORDER_RELAXED &&
           effects->memory.inactive_lanes ==
               PVIR_INACTIVE_MEMORY_NO_TRANSACTION &&
           effects->fault_behavior == behavior &&
           effects->fault_order == order &&
           effects->fault_result == PVIR_FAULT_RESULT_UNKNOWN &&
           effects->state_domain == PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT &&
           effects->state_action == PVIR_ARCH_STATE_UPDATE &&
           effects->state_update ==
               PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS &&
           effects->result_unknown_from_fault_boundary &&
           effects->result_unknown_where_ffr_input_clear &&
           effects->ffr_clears_from_fault_boundary &&
           !effects->writes_nzcv && !effects->ffr_initializes_all_true;
}

static bool is_first_fault_load_insn(AARCH64_INSN_TYPE_A64 insn_type)
{
    return insn_type == AARCH64_A64_SVE_LDFF1B ||
           insn_type == AARCH64_A64_SVE_LDFF1H ||
           insn_type == AARCH64_A64_SVE_LDFF1W ||
           insn_type == AARCH64_A64_SVE_LDFF1D ||
           insn_type == AARCH64_A64_SVE_LDFF1SB ||
           insn_type == AARCH64_A64_SVE_LDFF1SH ||
           insn_type == AARCH64_A64_SVE_LDFF1SW;
}

static PvirStatus build_fault_load(A64SVEPvirBuilder *next,
                                  const A64SVEIR1Instruction *source,
                                  uint32_t vl_bits,
                                  PvirInstruction *instruction)
{
    const arg_sve_fault_load *first = &source->arg.f_sve_fault_load;
    const arg_sve_nonfault_load *nonfault = &source->arg.f_sve_nonfault_load;
    bool is_first = is_first_fault_load_insn(source->insn_type);
    uint8_t zt = is_first ? first->zt : nonfault->zt;
    uint8_t pg = is_first ? first->pg : nonfault->pg;
    uint8_t rn = is_first ? first->rn : nonfault->rn;
    uint8_t element_bits =
        is_first ? first->element_bits : nonfault->element_bits;
    uint8_t memory_bits = is_first ? first->memory_bits : nonfault->memory_bits;
    bool sign_extend = is_first ? first->sign_extend : nonfault->sign_extend;
    bool base_is_sp = is_first ? first->base_is_sp : nonfault->base_is_sp;
    PvirFaultBehavior behavior = is_first ? PVIR_FAULT_FIRST_FAULT
                                          : PVIR_FAULT_NONFAULTING;
    const arg_sve_effect_intent *effects =
        is_first ? &first->effects : &nonfault->effects;
    PvirVectorOperation *operation;
    PvirVectorType type;
    PvirValueId fault_boundary;
    PvirValueId ffr_output;
    size_t lanes;
    int64_t displacement = 0;

    if (zt >= A64_SVE_PVIR_NUM_ZREGS ||
        pg >= A64_SVE_PVIR_NUM_PREGS || rn > 31 ||
        (is_first && first->rm > 31) ||
        (memory_bits != 8 && memory_bits != 16 &&
         memory_bits != 32 && memory_bits != 64) ||
        (element_bits != 8 && element_bits != 16 && element_bits != 32 &&
         element_bits != 64) ||
        memory_bits > element_bits ||
        base_is_sp != (rn == 31) ||
        !fault_load_effects_valid(effects, behavior) ||
        next->ffr_value == PVIR_NO_VALUE ||
        next->ffr_version == UINT64_MAX) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (!is_first) {
        displacement =
            (int64_t)nonfault->immediate_vl * (int64_t)(vl_bits / 8u);
    }

    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        .signedness = sign_extend ? PVIR_SIGNED : PVIR_UNSIGNED,
        .element_bits = element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }

    init_instruction(instruction);
    instruction->opcode = PVIR_OP_LOAD;
    instruction->effects.memory = effects->memory;
    instruction->effects.fault.behavior = behavior;
    instruction->effects.fault.lane_order = effects->fault_order;
    instruction->effects.fault.fault_result = PVIR_FAULT_RESULT_UNKNOWN;
    instruction->effects.side_effects =
        PVIR_SIDE_EFFECT_IO | PVIR_SIDE_EFFECT_VOLATILE;
    if (is_first) {
        instruction->effects.side_effects |= PVIR_SIDE_EFFECT_ORDERED;
    }
    set_state_update(&instruction->effects.arch_state,
                     A64_SVE_PVIR_FFR_STATE, next->ffr_version);

    operation = &instruction->operation.vector;
    operation->type = type;
    operation->predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = next->p_values[pg],
            .version = next->p_versions[pg],
        },
        lanes);
    operation->inactive_policy = PVIR_INACTIVE_ZERO;
    operation->destination = PVIR_NO_VALUE;
    operation->merge_value = PVIR_NO_VALUE;
    operation->memory_address = (PvirMemoryAddress){
        .present = true,
        .base_kind = base_is_sp ? PVIR_MEMORY_BASE_STACK_POINTER
                                : PVIR_MEMORY_BASE_VALUE,
        .base_value = base_is_sp ? PVIR_NO_VALUE
                                 : A64_SVE_PVIR_GPR_VALUE_BASE + rn,
        .displacement = displacement,
    };
    operation->memory_element_bits = memory_bits;
    operation->arch_state.input_value = next->ffr_value;
    operation->arch_state.output_value = PVIR_NO_VALUE;
    operation->arch_state.clear_from_lane = PVIR_NO_VALUE;
    if (!allocate_value(next, &operation->destination) ||
        !allocate_value(next, &ffr_output) ||
        !allocate_value(next, &fault_boundary)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    operation->arch_state.output_value = ffr_output;
    operation->arch_state.clear_from_lane = fault_boundary;
    instruction->effects.fault.unknown_from_lane = fault_boundary;
    next->z_values[zt] = operation->destination;
    next->ffr_value = ffr_output;
    next->ffr_version++;
    return PVIR_STATUS_OK;
}

static PvirStatus build_integer_binary(
    A64SVEPvirBuilder *builder, A64SVEPvirBuilder *next,
    const arg_sve_integer_binary *binary, uint32_t vl_bits,
    PvirOpcode opcode, PvirInstruction *instruction)
{
    PvirVectorOperation *operation;
    PvirVectorType type;
    size_t lanes;
    PvirValueId old_destination;

    if ((opcode != PVIR_OP_ADD && opcode != PVIR_OP_SUB &&
         opcode != PVIR_OP_MUL && opcode != PVIR_OP_OR &&
         opcode != PVIR_OP_AND && opcode != PVIR_OP_XOR &&
         opcode != PVIR_OP_SHL && opcode != PVIR_OP_SHR &&
         opcode != PVIR_OP_SAR && opcode != PVIR_OP_MAX &&
         opcode != PVIR_OP_MIN) ||
        binary->zd >= A64_SVE_PVIR_NUM_ZREGS ||
        binary->zm >= A64_SVE_PVIR_NUM_ZREGS ||
        binary->pg >= A64_SVE_PVIR_NUM_PREGS ||
        binary->effects.inactive_policy != PVIR_INACTIVE_MERGE ||
        !arm_sve_vl_is_valid(vl_bits)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    /*
     * MAX/MIN are integer-only in PVIR (FP min/max has its own NaN rules),
     * so they carry PVIR_ELEMENT_INTEGER; the signed/unsigned choice itself is
     * taken from the instruction tag by the lowering.
     */
    type = (PvirVectorType){
        .kind = (opcode == PVIR_OP_MAX || opcode == PVIR_OP_MIN)
                    ? PVIR_ELEMENT_INTEGER
                    : PVIR_ELEMENT_BITS,
        .signedness = (opcode == PVIR_OP_MAX || opcode == PVIR_OP_MIN)
                          ? (binary->is_signed ? PVIR_SIGNED : PVIR_UNSIGNED)
                          : PVIR_SIGNLESS,
        .element_bits = binary->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    old_destination = builder->z_values[binary->zd];
    init_instruction(instruction);
    instruction->opcode = opcode;
    operation = &instruction->operation.vector;
    operation->type = type;
    operation->predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = builder->p_values[binary->pg],
            .version = builder->p_versions[binary->pg],
        },
        lanes);
    operation->inactive_policy = PVIR_INACTIVE_MERGE;
    operation->destination = PVIR_NO_VALUE;
    operation->operand_count = 2;
    operation->operands[0] = old_destination;
    operation->operands[1] = builder->z_values[binary->zm];
    operation->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    operation->operand_kinds[1] = PVIR_OPERAND_VECTOR;
    operation->merge_value = old_destination;
    if (!allocate_value(next, &operation->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[binary->zd] = operation->destination;
    return PVIR_STATUS_OK;
}

/*
 * SVE FP predicated arithmetic: Zdn is read as an addend and is also the
 * merge source, /M is the only encoding (llvm-mc rejects a /Z form), and
 * inactive lanes must not raise FP exceptions or update FPSR.
 */
static PvirStatus build_fp_binary(
    A64SVEPvirBuilder *builder, A64SVEPvirBuilder *next,
    const arg_sve_fp_binary *binary, uint32_t vl_bits,
    PvirOpcode opcode, PvirInstruction *instruction)
{
    PvirVectorOperation *operation;
    PvirVectorType type;
    size_t lanes;
    PvirValueId old_destination;

    if ((opcode != PVIR_OP_ADD && opcode != PVIR_OP_SUB &&
         opcode != PVIR_OP_MUL && opcode != PVIR_OP_DIV) ||
        binary->zd >= A64_SVE_PVIR_NUM_ZREGS ||
        binary->zm >= A64_SVE_PVIR_NUM_ZREGS ||
        binary->pg >= A64_SVE_PVIR_NUM_PREGS ||
        (binary->element_bits != 32 && binary->element_bits != 64) ||
        binary->effects.inactive_policy != PVIR_INACTIVE_MERGE ||
        !arm_sve_vl_is_valid(vl_bits)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_FLOAT,
        .signedness = PVIR_SIGNLESS,
        .element_bits = binary->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    old_destination = builder->z_values[binary->zd];
    init_instruction(instruction);
    instruction->opcode = opcode;
    operation = &instruction->operation.vector;
    operation->type = type;
    operation->predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = builder->p_values[binary->pg],
            .version = builder->p_versions[binary->pg],
        },
        lanes);
    operation->inactive_policy = PVIR_INACTIVE_MERGE;
    operation->destination = PVIR_NO_VALUE;
    operation->operand_count = 2;
    operation->operands[0] = old_destination;
    operation->operands[1] = builder->z_values[binary->zm];
    operation->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    operation->operand_kinds[1] = PVIR_OPERAND_VECTOR;
    operation->merge_value = old_destination;
    instruction->effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
    if (!allocate_value(next, &operation->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[binary->zd] = operation->destination;
    return PVIR_STATUS_OK;
}

/*
 * SVE FABS/FNEG are sign-bit operations: AND clears the sign bit, XOR flips
 * it. They never touch FP status, so the PVIR type is BITS (not FLOAT) and
 * fp_exceptions stays NONE; inactive lanes simply keep old Zd.
 */
static PvirStatus build_fp_unary(
    A64SVEPvirBuilder *builder, A64SVEPvirBuilder *next,
    const arg_sve_fp_unary *unary, uint32_t vl_bits,
    PvirOpcode opcode, uint64_t immediate, PvirInstruction *instruction)
{
    PvirVectorOperation *operation;
    PvirVectorType type;
    size_t lanes;
    PvirValueId old_destination;

    if ((opcode != PVIR_OP_AND && opcode != PVIR_OP_XOR) ||
        unary->zd >= A64_SVE_PVIR_NUM_ZREGS ||
        unary->zm >= A64_SVE_PVIR_NUM_ZREGS ||
        unary->pg >= A64_SVE_PVIR_NUM_PREGS ||
        (unary->element_bits != 32 && unary->element_bits != 64) ||
        unary->effects.inactive_policy != PVIR_INACTIVE_MERGE ||
        !arm_sve_vl_is_valid(vl_bits)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_BITS,
        .signedness = PVIR_SIGNLESS,
        .element_bits = unary->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    old_destination = builder->z_values[unary->zd];
    init_instruction(instruction);
    instruction->opcode = opcode;
    operation = &instruction->operation.vector;
    operation->type = type;
    operation->predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = builder->p_values[unary->pg],
            .version = builder->p_versions[unary->pg],
        },
        lanes);
    operation->inactive_policy = PVIR_INACTIVE_MERGE;
    operation->destination = PVIR_NO_VALUE;
    operation->operand_count = 2;
    operation->operands[0] = builder->z_values[unary->zm];
    operation->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    operation->operands[1] = PVIR_NO_VALUE;
    operation->operand_kinds[1] = PVIR_OPERAND_IMMEDIATE;
    operation->operand_immediates[1] = immediate;
    operation->merge_value = old_destination;
    if (!allocate_value(next, &operation->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[unary->zd] = operation->destination;
    return PVIR_STATUS_OK;
}

/* DUP/MOV (scalar to vector): every lane receives the guest GPR's low element. */
static PvirStatus build_broadcast(
    A64SVEPvirBuilder *builder, A64SVEPvirBuilder *next,
    const arg_sve_dup *dup, uint32_t vl_bits, PvirInstruction *instruction)
{
    PvirVectorOperation *operation;
    PvirVectorType type;
    size_t lanes;
    PvirValueId old_destination;

    if (dup->zd >= A64_SVE_PVIR_NUM_ZREGS || dup->rn >= 32 ||
        (dup->element_bits != 8 && dup->element_bits != 16 &&
         dup->element_bits != 32 && dup->element_bits != 64) ||
        !arm_sve_vl_is_valid(vl_bits)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_BITS,
        .signedness = PVIR_SIGNLESS,
        .element_bits = dup->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    old_destination = builder->z_values[dup->zd];
    init_instruction(instruction);
    instruction->opcode = PVIR_OP_BROADCAST;
    operation = &instruction->operation.vector;
    operation->type = type;
    /* Unpredicated: every lane is active, so the merge arm never applies. */
    operation->predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){ .kind = PVIR_PREDICATE_SOURCE_CONSTANT },
        lanes);
    operation->predicate.form = PVIR_PREDICATE_ALL_TRUE;
    operation->inactive_policy = PVIR_INACTIVE_MERGE;
    operation->destination = PVIR_NO_VALUE;
    operation->operand_count = 1;
    operation->operands[0] = A64_SVE_PVIR_GPR_VALUE_BASE + dup->rn;
    operation->operand_kinds[0] = PVIR_OPERAND_SCALAR;
    operation->merge_value = old_destination;
    if (!allocate_value(next, &operation->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[dup->zd] = operation->destination;
    return PVIR_STATUS_OK;
}

/* SVE NOT (predicated): one vector operand, bitwise complement, merge policy. */
static PvirStatus build_integer_unary(
    A64SVEPvirBuilder *builder, A64SVEPvirBuilder *next,
    const arg_sve_int_unary *unary, uint32_t vl_bits,
    PvirInstruction *instruction)
{
    PvirVectorOperation *operation;
    PvirVectorType type;
    size_t lanes;
    PvirValueId old_destination;

    if (unary->zd >= A64_SVE_PVIR_NUM_ZREGS ||
        unary->zm >= A64_SVE_PVIR_NUM_ZREGS ||
        unary->pg >= A64_SVE_PVIR_NUM_PREGS ||
        (unary->element_bits != 8 && unary->element_bits != 16 &&
         unary->element_bits != 32 && unary->element_bits != 64) ||
        unary->effects.inactive_policy != PVIR_INACTIVE_MERGE ||
        !arm_sve_vl_is_valid(vl_bits)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_BITS,
        .signedness = PVIR_SIGNLESS,
        .element_bits = unary->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    old_destination = builder->z_values[unary->zd];
    init_instruction(instruction);
    instruction->opcode = PVIR_OP_NOT;
    operation = &instruction->operation.vector;
    operation->type = type;
    operation->predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = builder->p_values[unary->pg],
            .version = builder->p_versions[unary->pg],
        },
        lanes);
    operation->inactive_policy = PVIR_INACTIVE_MERGE;
    operation->destination = PVIR_NO_VALUE;
    operation->operand_count = 1;
    operation->operands[0] = builder->z_values[unary->zm];
    operation->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    operation->merge_value = old_destination;
    if (!allocate_value(next, &operation->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[unary->zd] = operation->destination;
    return PVIR_STATUS_OK;
}

static PvirStatus build_contiguous_memory(
    A64SVEPvirBuilder *next, const A64SVEIR1Instruction *source,
    uint32_t vl_bits, PvirInstruction *instruction)
{
    const arg_sve_contiguous *memory = &source->arg.f_sve_contiguous;
    PvirVectorOperation *operation;
    PvirVectorType type;
    size_t lanes;
    bool is_load = source->insn_type == AARCH64_A64_SVE_LD1B ||
                   source->insn_type == AARCH64_A64_SVE_LD1H ||
                   source->insn_type == AARCH64_A64_SVE_LD1W ||
                   source->insn_type == AARCH64_A64_SVE_LD1D ||
                   source->insn_type == AARCH64_A64_SVE_LD1SB ||
                   source->insn_type == AARCH64_A64_SVE_LD1SH ||
                   source->insn_type == AARCH64_A64_SVE_LD1SW;

    if (memory->zt >= A64_SVE_PVIR_NUM_ZREGS ||
        memory->pg >= A64_SVE_PVIR_NUM_PREGS ||
        memory->rn > 31 ||
        (memory->memory_bits != 8 && memory->memory_bits != 16 &&
         memory->memory_bits != 32 && memory->memory_bits != 64) ||
        memory->memory_bits > memory->element_bits ||
        (memory->element_bits != 8 && memory->element_bits != 16 &&
         memory->element_bits != 32 && memory->element_bits != 64) ||
        memory->base_is_sp != (memory->rn == 31) ||
        !arm_sve_vl_is_valid(vl_bits)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (!is_load && memory->sign_extend) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        /* SIGNED records the widening performed by LD1SB/LD1SH/LD1SW. */
        .signedness = memory->sign_extend ? PVIR_SIGNED : PVIR_UNSIGNED,
        .element_bits = memory->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK ||
        memory->effects.memory.access !=
            (is_load ? PVIR_MEMORY_READ : PVIR_MEMORY_WRITE) ||
        memory->effects.memory.ordering != PVIR_MEMORY_ORDER_RELAXED ||
        memory->effects.memory.inactive_lanes !=
            PVIR_INACTIVE_MEMORY_NO_TRANSACTION ||
        memory->effects.fault_behavior != PVIR_FAULT_PRECISE ||
        memory->effects.fault_order != PVIR_FAULT_ORDER_LANE_ASCENDING ||
        memory->effects.inactive_policy !=
            (is_load ? PVIR_INACTIVE_ZERO
                     : PVIR_INACTIVE_NOT_APPLICABLE) ||
        memory->effects.state_domain != PVIR_ARCH_STATE_DOMAIN_NONE ||
        memory->effects.state_action != PVIR_ARCH_STATE_ACTION_NONE) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }

    init_instruction(instruction);
    instruction->opcode = is_load ? PVIR_OP_LOAD : PVIR_OP_STORE;
    instruction->effects.memory = memory->effects.memory;
    instruction->effects.fault.behavior = memory->effects.fault_behavior;
    instruction->effects.fault.lane_order = memory->effects.fault_order;
    instruction->effects.side_effects =
        PVIR_SIDE_EFFECT_IO | PVIR_SIDE_EFFECT_VOLATILE |
        PVIR_SIDE_EFFECT_ORDERED;

    operation = &instruction->operation.vector;
    operation->type = type;
    operation->predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = next->p_values[memory->pg],
            .version = next->p_versions[memory->pg],
        },
        lanes);
    operation->inactive_policy =
        is_load ? PVIR_INACTIVE_ZERO : PVIR_INACTIVE_NOT_APPLICABLE;
    operation->destination = PVIR_NO_VALUE;
    operation->operand_count = is_load ? 0 : 1;
    operation->merge_value = PVIR_NO_VALUE;
    operation->memory_address = (PvirMemoryAddress){
        .present = true,
        .base_kind = memory->base_is_sp
                         ? PVIR_MEMORY_BASE_STACK_POINTER
                         : PVIR_MEMORY_BASE_VALUE,
        .base_value = memory->base_is_sp
                          ? PVIR_NO_VALUE
                          : A64_SVE_PVIR_GPR_VALUE_BASE + memory->rn,
        .displacement =
            (int64_t)memory->immediate_vl * (int64_t)(vl_bits / 8u),
    };
    operation->memory_element_bits = memory->memory_bits;
    if (!is_load) {
        operation->operands[0] = next->z_values[memory->zt];
        operation->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    } else if (!allocate_value(next, &operation->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    if (is_load) {
        next->z_values[memory->zt] = operation->destination;
    }
    return PVIR_STATUS_OK;
}


static PvirStatus build_compact(A64SVEPvirBuilder *next,
                                const arg_sve_general *g,
                                uint32_t vl_bits,
                                PvirInstruction *instruction)
{
    PvirPermuteOperation *op;
    PvirVectorType type;
    size_t lanes;

    if (g->zd >= A64_SVE_PVIR_NUM_ZREGS || g->zn >= A64_SVE_PVIR_NUM_ZREGS ||
        g->pg >= A64_SVE_PVIR_NUM_PREGS ||
        (g->element_bits != 32 && g->element_bits != 64)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_BITS,
        .signedness = PVIR_SIGNLESS,
        .element_bits = g->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    instruction->opcode = PVIR_OP_PERMUTE;
    op = &instruction->operation.permute;
    op->type = type;
    op->has_predicate = true;
    op->predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = next->p_values[g->pg],
            .version = next->p_versions[g->pg],
        },
        lanes);
    op->inactive_policy = PVIR_INACTIVE_ZERO;
    op->merge_value = PVIR_NO_VALUE;
    op->operands[0] = next->z_values[g->zn];
    op->operand_count = 1;
    op->kind = PVIR_PERMUTE_COMPACT;
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[g->zd] = op->destination;
    return PVIR_STATUS_OK;
}

static PvirStatus build_ptest(A64SVEPvirBuilder *next,
                              const arg_sve_pred_op *p,
                              PvirInstruction *instruction)
{
    if (p->pg >= A64_SVE_PVIR_NUM_PREGS || p->pn >= A64_SVE_PVIR_NUM_PREGS ||
        next->status_version == UINT64_MAX) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    instruction->opcode = PVIR_OP_ARCH_STATE;
    set_state_write(&instruction->effects.arch_state, A64_SVE_PVIR_STATUS_STATE,
                    next->status_version);
    instruction->operation.arch_state.input_value = next->p_values[p->pn];
    instruction->operation.arch_state.output_value = PVIR_NO_VALUE;
    instruction->operation.arch_state.clear_from_lane = PVIR_NO_VALUE;
    next->status_version++;
    return PVIR_STATUS_OK;
}

static PvirStatus build_pred_op(const A64SVEPvirBuilder *builder,
                                A64SVEPvirBuilder *next,
                                const arg_sve_pred_op *p,
                                uint32_t vl_bits,
                                PvirInstruction *instruction)
{
    PvirVectorOperation *op;
    PvirVectorType type;
    size_t lanes;

    if (p->pd >= A64_SVE_PVIR_NUM_PREGS || p->pg >= A64_SVE_PVIR_NUM_PREGS ||
        p->pn >= A64_SVE_PVIR_NUM_PREGS || p->pm >= A64_SVE_PVIR_NUM_PREGS ||
        (p->element_bits != 8 && p->element_bits != 16 &&
         p->element_bits != 32 && p->element_bits != 64) ||
        next->p_versions[p->pd] == UINT64_MAX) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_BITS,
        .signedness = PVIR_SIGNLESS,
        .element_bits = p->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    op = &instruction->operation.vector;
    op->type = type;

    if (p->subop == SVE_PRED_OP_BRKA || p->subop == SVE_PRED_OP_BRKB ||
        p->subop == SVE_PRED_OP_BRKN || p->subop == SVE_PRED_OP_PFIRST ||
        p->subop == SVE_PRED_OP_PNEXT) {
        bool merge = (p->subop == SVE_PRED_OP_PFIRST ||
                      p->subop == SVE_PRED_OP_PNEXT || p->merge);
        instruction->opcode = PVIR_OP_MOVE;
        op->predicate = pvir_predicate_vector_mask(
            (PvirPredicateSource){
                .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
                .id = builder->p_values[p->pg],
                .version = builder->p_versions[p->pg],
            },
            lanes);
        op->inactive_policy = merge ? PVIR_INACTIVE_MERGE : PVIR_INACTIVE_ZERO;
        op->merge_value = merge ? builder->p_values[p->pd] : PVIR_NO_VALUE;
        op->operand_count = 1;
        op->operands[0] = builder->p_values[p->pn];
        op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    } else if (p->subop == SVE_PRED_OP_SEL) {
        instruction->opcode = PVIR_OP_SELECT;
        op->predicate = pvir_predicate_all_true(lanes);
        op->inactive_policy = PVIR_INACTIVE_ZERO;
        op->merge_value = PVIR_NO_VALUE;
        op->operand_count = 3;
        op->operands[0] = builder->p_values[p->pg];
        op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        op->operands[1] = builder->p_values[p->pn];
        op->operand_kinds[1] = PVIR_OPERAND_VECTOR;
        op->operands[2] = builder->p_values[p->pm];
        op->operand_kinds[2] = PVIR_OPERAND_VECTOR;
    } else {
        if (p->subop == SVE_PRED_OP_ORR || p->subop == SVE_PRED_OP_ORN ||
            p->subop == SVE_PRED_OP_NOR) {
            instruction->opcode = PVIR_OP_OR;
        } else if (p->subop == SVE_PRED_OP_EOR) {
            instruction->opcode = PVIR_OP_XOR;
        } else {
            instruction->opcode = PVIR_OP_AND;
        }
        op->predicate = pvir_predicate_vector_mask(
            (PvirPredicateSource){
                .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
                .id = builder->p_values[p->pg],
                .version = builder->p_versions[p->pg],
            },
            lanes);
        op->inactive_policy = PVIR_INACTIVE_ZERO;
        op->merge_value = PVIR_NO_VALUE;
        op->operand_count = 2;
        op->operands[0] = builder->p_values[p->pn];
        op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        op->operands[1] = builder->p_values[p->pm];
        op->operand_kinds[1] = PVIR_OPERAND_VECTOR;
    }

    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->p_values[p->pd] = op->destination;
    next->p_versions[p->pd]++;
    invalidate_known_preg(next, p->pd);
    if (p->setflags) {
        if (next->status_version == UINT64_MAX) {
            return PVIR_STATUS_INSUFFICIENT_CAPACITY;
        }
        next->status_version++;
    }
    return PVIR_STATUS_OK;
}

static PvirStatus build_cnt(A64SVEPvirBuilder *next, const arg_sve_cnt *cnt,
                            uint32_t vl_bits, PvirInstruction *instruction)
{
    PvirReduceOperation *op;
    PvirVectorType type;
    size_t lanes;

    if (cnt->rd > 31 ||
        (cnt->element_bits != 8 && cnt->element_bits != 16 &&
         cnt->element_bits != 32 && cnt->element_bits != 64)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (cnt->subop == SVE_CNT_SUBOP_CNTP &&
        (cnt->pg >= A64_SVE_PVIR_NUM_PREGS ||
         cnt->pn >= A64_SVE_PVIR_NUM_PREGS)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        .signedness = cnt->is_signed ? PVIR_SIGNED : PVIR_UNSIGNED,
        .element_bits = cnt->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    instruction->opcode = PVIR_OP_REDUCE;
    op = &instruction->operation.reduce;
    op->type = type;
    op->result_type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        .signedness = type.signedness,
        .element_bits = 64,
        .vector_bits = 64,
    };
    op->predicate = (cnt->subop == SVE_CNT_SUBOP_CNTP)
                        ? pvir_predicate_vector_mask(
                              (PvirPredicateSource){
                                  .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
                                  .id = next->p_values[cnt->pg],
                                  .version = next->p_versions[cnt->pg],
                              },
                              lanes)
                        : pvir_predicate_all_true(lanes);
    op->source = (cnt->subop == SVE_CNT_SUBOP_CNTP)
                     ? next->p_values[cnt->pn]
                     : (A64_SVE_PVIR_GPR_VALUE_BASE + cnt->rd);
    op->init_scalar = PVIR_NO_VALUE;
    op->kind = PVIR_REDUCE_ADD;
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    return PVIR_STATUS_OK;
}

static PvirStatus build_last(A64SVEPvirBuilder *next, const arg_sve_last *l,
                             uint32_t vl_bits, PvirInstruction *instruction)
{
    PvirLaneOperation *op;
    PvirVectorType type;
    size_t lanes;

    if (l->zm >= A64_SVE_PVIR_NUM_ZREGS || l->pg >= A64_SVE_PVIR_NUM_PREGS ||
        l->rd_zd >= A64_SVE_PVIR_NUM_ZREGS ||
        (l->element_bits != 8 && l->element_bits != 16 &&
         l->element_bits != 32 && l->element_bits != 64)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        .signedness = PVIR_UNSIGNED,
        .element_bits = l->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    instruction->opcode = PVIR_OP_EXTRACT;
    op = &instruction->operation.lane;
    op->source = next->z_values[l->zm];
    op->merge_value = PVIR_NO_VALUE;
    op->vector_type = type;
    op->lane = 0;
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    if (l->dst_is_vector) {
        next->z_values[l->rd_zd] = op->destination;
    }
    return PVIR_STATUS_OK;
}

static PvirStatus build_mem_ext(A64SVEPvirBuilder *next,
                                const arg_sve_mem_ext *m, uint32_t vl_bits,
                                PvirInstruction *instruction)
{
    PvirVectorOperation *op;
    PvirVectorType type;
    size_t lanes;
    bool is_vec_base = (m->kind == SVE_MEM_GATHER_VEC_BASE);

    if (m->zt >= A64_SVE_PVIR_NUM_ZREGS || m->pg >= A64_SVE_PVIR_NUM_PREGS ||
        m->rn > 31 || m->rm > 31 || m->nregs == 0 || m->nregs > 4 ||
        (m->memory_bits != 8 && m->memory_bits != 16 &&
         m->memory_bits != 32 && m->memory_bits != 64) ||
        (m->element_bits != 8 && m->element_bits != 16 &&
         m->element_bits != 32 && m->element_bits != 64) ||
        m->memory_bits > m->element_bits ||
        (!m->is_load && m->sign_extend)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        .signedness = m->sign_extend ? PVIR_SIGNED : PVIR_UNSIGNED,
        .element_bits = m->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    instruction->opcode = m->is_load ? PVIR_OP_LOAD : PVIR_OP_STORE;
    instruction->effects.memory.access =
        m->is_load ? PVIR_MEMORY_READ : PVIR_MEMORY_WRITE;
    instruction->effects.memory.ordering = PVIR_MEMORY_ORDER_RELAXED;
    instruction->effects.memory.inactive_lanes =
        PVIR_INACTIVE_MEMORY_NO_TRANSACTION;
    instruction->effects.fault.behavior = PVIR_FAULT_PRECISE;
    instruction->effects.fault.lane_order = PVIR_FAULT_ORDER_LANE_ASCENDING;
    instruction->effects.side_effects =
        PVIR_SIDE_EFFECT_IO | PVIR_SIDE_EFFECT_VOLATILE |
        PVIR_SIDE_EFFECT_ORDERED;
    op = &instruction->operation.vector;
    op->type = type;
    op->predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = next->p_values[m->pg],
            .version = next->p_versions[m->pg],
        },
        lanes);
    op->inactive_policy =
        m->is_load ? PVIR_INACTIVE_ZERO : PVIR_INACTIVE_NOT_APPLICABLE;
    op->merge_value = PVIR_NO_VALUE;
    op->operand_count = m->is_load ? 0 : 1;
    op->memory_address = (PvirMemoryAddress){
        .present = true,
        .base_kind = (!is_vec_base && m->base_is_sp)
                         ? PVIR_MEMORY_BASE_STACK_POINTER
                         : PVIR_MEMORY_BASE_VALUE,
        .base_value = is_vec_base
                          ? next->z_values[m->rn]
                          : (m->base_is_sp ? PVIR_NO_VALUE
                                           : (A64_SVE_PVIR_GPR_VALUE_BASE + m->rn)),
        .displacement = (int64_t)m->imm,
    };
    op->memory_element_bits = m->memory_bits;
    if (!m->is_load) {
        op->destination = PVIR_NO_VALUE;
        op->operands[0] = next->z_values[m->zt];
        op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    } else {
        for (uint8_t r = 0; r < m->nregs; ++r) {
            PvirValueId val;
            if (!allocate_value(next, &val)) {
                return PVIR_STATUS_INSUFFICIENT_CAPACITY;
            }
            if (r == 0) {
                op->destination = val;
            }
            next->z_values[(m->zt + r) & 31u] = val;
        }
    }
    return PVIR_STATUS_OK;
}

static PvirStatus build_fp_ext(const A64SVEPvirBuilder *builder,
                               A64SVEPvirBuilder *next,
                               AARCH64_INSN_TYPE_A64 insn_type,
                               const arg_sve_fp_ext *f, uint32_t vl_bits,
                               PvirInstruction *instruction)
{
    PvirVectorType type;
    size_t lanes;

    if (f->zd >= A64_SVE_PVIR_NUM_ZREGS || f->zn >= A64_SVE_PVIR_NUM_ZREGS ||
        f->zm >= A64_SVE_PVIR_NUM_ZREGS || f->pg >= A64_SVE_PVIR_NUM_PREGS ||
        (f->element_bits != 32 && f->element_bits != 64)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_FLOAT,
        .signedness = PVIR_SIGNLESS,
        .element_bits = f->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    instruction->effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;

    if (insn_type == AARCH64_A64_SVE_FMLA) {
        PvirVectorOperation *op = &instruction->operation.vector;
        instruction->opcode = PVIR_OP_FMA;
        op->type = type;
        op->predicate = pvir_predicate_vector_mask(
            (PvirPredicateSource){
                .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
                .id = builder->p_values[f->pg],
                .version = builder->p_versions[f->pg],
            },
            lanes);
        op->inactive_policy = PVIR_INACTIVE_MERGE;
        op->merge_value = builder->z_values[f->zd];
        op->operand_count = 3;
        if (f->destructive_madd) {
            op->operands[0] = builder->z_values[f->zd];
            op->operands[1] = builder->z_values[f->zm];
            op->operands[2] = builder->z_values[f->zn];
        } else {
            op->operands[0] = builder->z_values[f->zn];
            op->operands[1] = builder->z_values[f->zm];
            op->operands[2] = builder->z_values[f->zd];
        }
        op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        op->operand_kinds[1] = PVIR_OPERAND_VECTOR;
        op->operand_kinds[2] = PVIR_OPERAND_VECTOR;
        if (!allocate_value(next, &op->destination)) {
            return PVIR_STATUS_INSUFFICIENT_CAPACITY;
        }
        next->z_values[f->zd] = op->destination;
        return PVIR_STATUS_OK;
    }

    {
        PvirFPOpOperation *op = &instruction->operation.fp;
        bool is_unary = (f->subop == SVE_FP_EXT_FSQRT ||
                         f->subop == SVE_FP_EXT_FRECPE ||
                         f->subop == SVE_FP_EXT_FRSQRTE);
        bool predicated = (insn_type == AARCH64_A64_SVE_FMINMAX ||
                           f->subop == SVE_FP_EXT_FSQRT);
        instruction->opcode = PVIR_OP_FP_ARITH;
        op->type = type;
        op->predicate = predicated
                            ? pvir_predicate_vector_mask(
                                  (PvirPredicateSource){
                                      .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
                                      .id = builder->p_values[f->pg],
                                      .version = builder->p_versions[f->pg],
                                  },
                                  lanes)
                            : pvir_predicate_all_true(lanes);
        op->inactive_policy =
            predicated ? PVIR_INACTIVE_MERGE : PVIR_INACTIVE_ZERO;
        op->merge_value =
            predicated ? builder->z_values[f->zd] : PVIR_NO_VALUE;
        op->rounding = PVIR_ROUND_DEFAULT;
        op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        op->operands[0] = builder->z_values[f->zn];
        if (is_unary) {
            op->operand_kinds[1] = PVIR_OPERAND_VECTOR;
            op->operands[1] = PVIR_NO_VALUE;
        } else {
            op->operand_kinds[1] = PVIR_OPERAND_VECTOR;
            op->operands[1] = builder->z_values[f->zm];
        }
        switch (f->subop) {
        case SVE_FP_EXT_FMINNM: op->kind = PVIR_FP_MINNM; break;
        case SVE_FP_EXT_FMAXNM: op->kind = PVIR_FP_MAXNM; break;
        case SVE_FP_EXT_FMIN: op->kind = PVIR_FP_MIN; break;
        case SVE_FP_EXT_FMAX: op->kind = PVIR_FP_MAX; break;
        case SVE_FP_EXT_FSQRT: op->kind = PVIR_FP_SQRT; break;
        case SVE_FP_EXT_FRECPE: op->kind = PVIR_FP_RECPE; break;
        case SVE_FP_EXT_FRSQRTE: op->kind = PVIR_FP_RSQRTE; break;
        case SVE_FP_EXT_FRECPS: op->kind = PVIR_FP_RECPS; break;
        default: op->kind = PVIR_FP_RSQRTS; break;
        }
        if (!allocate_value(next, &op->destination)) {
            return PVIR_STATUS_INSUFFICIENT_CAPACITY;
        }
        next->z_values[f->zd] = op->destination;
    }
    return PVIR_STATUS_OK;
}

static PvirStatus build_fcmp(const A64SVEPvirBuilder *builder,
                             A64SVEPvirBuilder *next, const arg_sve_ext_op *e,
                             uint32_t vl_bits, PvirInstruction *instruction)
{
    PvirVectorOperation *op;
    PvirVectorType src_type;
    PvirVectorType mask_type;
    size_t lanes;

    if (e->zd_pd >= A64_SVE_PVIR_NUM_PREGS ||
        e->zn >= A64_SVE_PVIR_NUM_ZREGS || e->pg >= A64_SVE_PVIR_NUM_PREGS ||
        (!e->cmp_with_imm && e->zm >= A64_SVE_PVIR_NUM_ZREGS) ||
        (e->src_bits != 32 && e->src_bits != 64) ||
        next->p_versions[e->zd_pd] == UINT64_MAX) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    src_type = (PvirVectorType){
        .kind = PVIR_ELEMENT_FLOAT,
        .signedness = PVIR_SIGNLESS,
        .element_bits = e->src_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(src_type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    mask_type = (PvirVectorType){
        .kind = PVIR_ELEMENT_MASK,
        .signedness = PVIR_SIGNLESS,
        .element_bits = 1,
        .vector_bits = lanes,
    };
    init_instruction(instruction);
    instruction->opcode = PVIR_OP_COMPARE;
    op = &instruction->operation.vector;
    op->type = mask_type;
    op->predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = builder->p_values[e->pg],
            .version = builder->p_versions[e->pg],
        },
        lanes);
    op->inactive_policy = PVIR_INACTIVE_ZERO;
    op->merge_value = PVIR_NO_VALUE;
    op->compare_condition = (e->subop == PVIR_COMPARE_NONE)
                                ? PVIR_COMPARE_NE
                                : (PvirCompareCondition)e->subop;
    op->operand_count = 2;
    op->operands[0] = builder->z_values[e->zn];
    op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    op->operand_types[0] = src_type;
    if (e->cmp_with_imm) {
        op->operand_kinds[1] = PVIR_OPERAND_IMMEDIATE;
        op->operands[1] = PVIR_NO_VALUE;
        op->operand_immediates[1] = 0;
    } else {
        op->operand_kinds[1] = PVIR_OPERAND_VECTOR;
        op->operands[1] = builder->z_values[e->zm];
        op->operand_types[1] = src_type;
    }
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->p_values[e->zd_pd] = op->destination;
    next->p_versions[e->zd_pd]++;
    invalidate_known_preg(next, e->zd_pd);
    if (next->status_version < UINT64_MAX) {
        next->status_version++;
    }
    return PVIR_STATUS_OK;
}

static PvirStatus build_fcvt(const A64SVEPvirBuilder *builder,
                             A64SVEPvirBuilder *next, const arg_sve_ext_op *e,
                             uint32_t vl_bits, PvirInstruction *instruction)
{
    PvirConvertOperation *op;
    size_t lanes;

    if (e->zd_pd >= A64_SVE_PVIR_NUM_ZREGS ||
        e->zn >= A64_SVE_PVIR_NUM_ZREGS || e->pg >= A64_SVE_PVIR_NUM_PREGS ||
        (e->src_bits != 32 && e->src_bits != 64) ||
        (e->dst_bits != 32 && e->dst_bits != 64)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    lanes = (e->src_bits == e->dst_bits) ? (vl_bits / e->src_bits)
                                         : (vl_bits / 64u);
    init_instruction(instruction);
    instruction->opcode = PVIR_OP_CONVERT;
    instruction->effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
    op = &instruction->operation.convert;
    op->source = builder->z_values[e->zn];
    op->source_type = (PvirVectorType){
        .kind = (e->subop == SVE_FCVT_S_TO_F || e->subop == SVE_FCVT_U_TO_F)
                    ? PVIR_ELEMENT_INTEGER
                    : PVIR_ELEMENT_FLOAT,
        .signedness = (e->subop == SVE_FCVT_S_TO_F)
                          ? PVIR_SIGNED
                          : ((e->subop == SVE_FCVT_U_TO_F) ? PVIR_UNSIGNED
                                                           : PVIR_SIGNLESS),
        .element_bits = e->src_bits,
        .vector_bits = (uint32_t)(lanes * e->src_bits),
    };
    op->destination_type = (PvirVectorType){
        .kind = (e->subop == SVE_FCVT_F_TO_S || e->subop == SVE_FCVT_F_TO_U)
                    ? PVIR_ELEMENT_INTEGER
                    : PVIR_ELEMENT_FLOAT,
        .signedness = (e->subop == SVE_FCVT_F_TO_S)
                          ? PVIR_SIGNED
                          : ((e->subop == SVE_FCVT_F_TO_U) ? PVIR_UNSIGNED
                                                           : PVIR_SIGNLESS),
        .element_bits = e->dst_bits,
        .vector_bits = (uint32_t)(lanes * e->dst_bits),
    };
    op->rounding = (e->subop == SVE_FCVT_F_TO_S || e->subop == SVE_FCVT_F_TO_U)
                       ? PVIR_ROUND_RTZ
                       : PVIR_ROUND_DEFAULT;
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[e->zd_pd] = op->destination;
    return PVIR_STATUS_OK;
}

static PvirStatus build_reduce_insn(const A64SVEPvirBuilder *builder,
                                    A64SVEPvirBuilder *next,
                                    const arg_sve_general *g,
                                    uint32_t vl_bits, bool is_float,
                                    PvirInstruction *instruction)
{
    PvirReduceOperation *op;
    PvirVectorType type;
    size_t lanes;

    if (g->zd >= A64_SVE_PVIR_NUM_ZREGS || g->zn >= A64_SVE_PVIR_NUM_ZREGS ||
        g->pg >= A64_SVE_PVIR_NUM_PREGS) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (is_float && g->element_bits != 32 && g->element_bits != 64) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (!is_float && g->element_bits != 8 && g->element_bits != 16 &&
        g->element_bits != 32 && g->element_bits != 64) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = is_float ? PVIR_ELEMENT_FLOAT : PVIR_ELEMENT_INTEGER,
        .signedness = is_float ? PVIR_SIGNLESS
                               : (g->is_signed ? PVIR_SIGNED : PVIR_UNSIGNED),
        .element_bits = g->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    instruction->opcode = PVIR_OP_REDUCE;
    instruction->effects.fp_exceptions =
        is_float ? PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY
                 : PVIR_FP_EXCEPTIONS_NONE;
    op = &instruction->operation.reduce;
    op->type = type;
    op->result_type = (PvirVectorType){
        .kind = type.kind,
        .signedness = type.signedness,
        .element_bits = is_float ? g->element_bits : 64u,
        .vector_bits = is_float ? g->element_bits : 64u,
    };
    op->predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = builder->p_values[g->pg],
            .version = builder->p_versions[g->pg],
        },
        lanes);
    op->source = builder->z_values[g->zn];
    op->kind = (PvirReduceKind)g->subop;
    op->init_scalar = (is_float && op->kind == PVIR_REDUCE_ADD_ORDERED)
                          ? builder->z_values[g->zd]
                          : PVIR_NO_VALUE;
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[g->zd] = op->destination;
    return PVIR_STATUS_OK;
}

static PvirStatus build_fixed_int(const A64SVEPvirBuilder *builder,
                                  A64SVEPvirBuilder *next,
                                  const arg_sve_general *g, uint32_t vl_bits,
                                  PvirInstruction *instruction)
{
    PvirFixedPointOperation *op;
    PvirVectorType type;
    size_t lanes;

    if (g->zd >= A64_SVE_PVIR_NUM_ZREGS || g->zn >= A64_SVE_PVIR_NUM_ZREGS ||
        (!g->src2_is_imm && g->zm >= A64_SVE_PVIR_NUM_ZREGS) ||
        (g->predicated && g->pg >= A64_SVE_PVIR_NUM_PREGS)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        .signedness = g->is_signed ? PVIR_SIGNED : PVIR_UNSIGNED,
        .element_bits = g->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    instruction->opcode = PVIR_OP_FIXED;
    op = &instruction->operation.fixed;
    op->type = type;
    op->predicate = g->predicated
                        ? pvir_predicate_vector_mask(
                              (PvirPredicateSource){
                                  .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
                                  .id = builder->p_values[g->pg],
                                  .version = builder->p_versions[g->pg],
                              },
                              lanes)
                        : pvir_predicate_all_true(lanes);
    op->inactive_policy =
        g->predicated ? PVIR_INACTIVE_MERGE : PVIR_INACTIVE_ZERO;
    op->merge_value =
        g->predicated ? builder->z_values[g->zd] : PVIR_NO_VALUE;
    op->operands[0] = builder->z_values[g->zn];
    op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    if (g->src2_is_imm) {
        op->operands[1] = PVIR_NO_VALUE;
        op->operand_kinds[1] = PVIR_OPERAND_IMMEDIATE;
        op->operand_immediates[1] =
            (uint64_t)g->imm1 & mask_for_bits(g->element_bits);
    } else {
        op->operands[1] = builder->z_values[g->zm];
        op->operand_kinds[1] = PVIR_OPERAND_VECTOR;
    }
    op->kind = (PvirFixedPointKind)g->subop;
    op->rounding = (op->kind == PVIR_FIXED_AVG_ADD ||
                    op->kind == PVIR_FIXED_AVG_SUB)
                       ? (PvirRounding)g->imm1
                       : PVIR_ROUND_DEFAULT;
    op->saturating = false;
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[g->zd] = op->destination;
    return PVIR_STATUS_OK;
}

static PvirStatus build_shift_imm(const A64SVEPvirBuilder *builder,
                                  A64SVEPvirBuilder *next,
                                  const arg_sve_ext_op *e, uint32_t vl_bits,
                                  PvirInstruction *instruction)
{
    PvirVectorOperation *op;
    PvirVectorType type;
    size_t lanes;

    if (e->zd_pd >= A64_SVE_PVIR_NUM_ZREGS ||
        e->zn >= A64_SVE_PVIR_NUM_ZREGS ||
        (e->predicated && e->pg >= A64_SVE_PVIR_NUM_PREGS)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        .signedness = (e->subop == SVE_SHIFT_IMM_ASR ||
                       e->subop == SVE_SHIFT_IMM_ASRD)
                          ? PVIR_SIGNED
                          : PVIR_UNSIGNED,
        .element_bits = e->src_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    instruction->opcode =
        (e->subop == SVE_SHIFT_IMM_LSL)
            ? PVIR_OP_SHL
            : (e->subop == SVE_SHIFT_IMM_LSR ? PVIR_OP_SHR : PVIR_OP_SAR);
    op = &instruction->operation.vector;
    op->type = type;
    op->predicate = e->predicated
                        ? pvir_predicate_vector_mask(
                              (PvirPredicateSource){
                                  .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
                                  .id = builder->p_values[e->pg],
                                  .version = builder->p_versions[e->pg],
                              },
                              lanes)
                        : pvir_predicate_all_true(lanes);
    op->inactive_policy =
        e->predicated ? PVIR_INACTIVE_MERGE : PVIR_INACTIVE_ZERO;
    op->merge_value =
        e->predicated ? builder->z_values[e->zd_pd] : PVIR_NO_VALUE;
    op->operand_count = 2;
    op->operands[0] = builder->z_values[e->zn];
    op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    op->operands[1] = PVIR_NO_VALUE;
    op->operand_kinds[1] = PVIR_OPERAND_IMMEDIATE;
    op->operand_immediates[1] =
        (uint64_t)e->imm & mask_for_bits(e->src_bits);
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[e->zd_pd] = op->destination;
    return PVIR_STATUS_OK;
}

static PvirStatus build_int_unary(const A64SVEPvirBuilder *builder,
                                  A64SVEPvirBuilder *next,
                                  const arg_sve_general *g, uint32_t vl_bits,
                                  PvirInstruction *instruction)
{
    PvirVectorOperation *op;
    PvirVectorType type;
    size_t lanes;

    if (g->zd >= A64_SVE_PVIR_NUM_ZREGS || g->zn >= A64_SVE_PVIR_NUM_ZREGS ||
        g->pg >= A64_SVE_PVIR_NUM_PREGS) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    bool is_ext = (g->subop >= SVE_UNARY_SXTB && g->subop <= SVE_UNARY_UXTW);
    bool is_signed = (g->subop == SVE_UNARY_CLS || g->subop == SVE_UNARY_ABS ||
                      g->subop == SVE_UNARY_NEG ||
                      g->subop == SVE_UNARY_SXTB ||
                      g->subop == SVE_UNARY_SXTH ||
                      g->subop == SVE_UNARY_SXTW);
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        .signedness = is_signed ? PVIR_SIGNED : PVIR_UNSIGNED,
        .element_bits = g->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    instruction->opcode = is_ext ? PVIR_OP_EXTEND : PVIR_OP_MOVE;
    op = &instruction->operation.vector;
    op->type = type;
    op->predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = builder->p_values[g->pg],
            .version = builder->p_versions[g->pg],
        },
        lanes);
    op->inactive_policy = PVIR_INACTIVE_MERGE;
    op->merge_value = builder->z_values[g->zd];
    op->operand_count = 1;
    op->operands[0] = builder->z_values[g->zn];
    op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    if (is_ext) {
        uint16_t sbits = (g->subop == SVE_UNARY_SXTB || g->subop == SVE_UNARY_UXTB)
                             ? 8u
                             : ((g->subop == SVE_UNARY_SXTH ||
                                 g->subop == SVE_UNARY_UXTH)
                                    ? 16u
                                    : 32u);
        if (sbits >= g->element_bits) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        op->operand_types[0] = (PvirVectorType){
            .kind = PVIR_ELEMENT_INTEGER,
            .signedness = is_signed ? PVIR_SIGNED : PVIR_UNSIGNED,
            .element_bits = sbits,
            .vector_bits = (uint32_t)(lanes * sbits),
        };
    }
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[g->zd] = op->destination;
    return PVIR_STATUS_OK;
}

static PvirStatus build_permute_insn(const A64SVEPvirBuilder *builder,
                                     A64SVEPvirBuilder *next,
                                     const arg_sve_general *g,
                                     uint32_t vl_bits,
                                     PvirInstruction *instruction)
{
    PvirVectorType type = {
        .kind = PVIR_ELEMENT_BITS,
        .signedness = PVIR_SIGNLESS,
        .element_bits = g->element_bits,
        .vector_bits = vl_bits,
    };
    size_t lanes;

    if (g->zd >= A64_SVE_PVIR_NUM_ZREGS || g->zn >= A64_SVE_PVIR_NUM_ZREGS ||
        pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    if (g->subop == 0xffu) {
        /* INSR */
        PvirLaneOperation *op = &instruction->operation.lane;
        instruction->opcode = PVIR_OP_INSERT;
        op->vector_type = type;
        op->source = g->src1_is_imm ? builder->z_values[g->zm]
                                    : (A64_SVE_PVIR_GPR_VALUE_BASE + g->zm);
        op->merge_value = builder->z_values[g->zn];
        op->lane = 0;
        if (!allocate_value(next, &op->destination)) {
            return PVIR_STATUS_INSUFFICIENT_CAPACITY;
        }
        next->z_values[g->zd] = op->destination;
        return PVIR_STATUS_OK;
    }

    {
        PvirPermuteOperation *op = &instruction->operation.permute;
        instruction->opcode = PVIR_OP_PERMUTE;
        op->type = type;
        op->has_predicate = g->predicated;
        if (g->predicated) {
            if (g->pg >= A64_SVE_PVIR_NUM_PREGS) {
                return PVIR_STATUS_INVALID_ARGUMENT;
            }
            op->predicate = pvir_predicate_vector_mask(
                (PvirPredicateSource){
                    .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
                    .id = builder->p_values[g->pg],
                    .version = builder->p_versions[g->pg],
                },
                lanes);
            op->inactive_policy =
                (g->subop == PVIR_PERMUTE_SPLICE) ? PVIR_INACTIVE_ZERO
                                                  : PVIR_INACTIVE_MERGE;
            op->merge_value =
                (op->inactive_policy == PVIR_INACTIVE_MERGE)
                    ? builder->z_values[g->zd]
                    : PVIR_NO_VALUE;
        } else {
            op->inactive_policy = PVIR_INACTIVE_ZERO;
            op->merge_value = PVIR_NO_VALUE;
        }
        op->kind = (PvirPermuteKind)g->subop;
        op->imm = (uint32_t)g->imm1;
        op->operands[0] = builder->z_values[g->zn];
        if (op->kind == PVIR_PERMUTE_REV || op->kind == PVIR_PERMUTE_REVB ||
            op->kind == PVIR_PERMUTE_REVH || op->kind == PVIR_PERMUTE_REVW) {
            op->operand_count = 1;
        } else if (op->kind == PVIR_PERMUTE_TBX) {
            op->operands[1] = builder->z_values[g->zm];
            op->operands[2] = builder->z_values[g->zd];
            op->operand_count = 3;
        } else {
            if (g->zm >= A64_SVE_PVIR_NUM_ZREGS) {
                return PVIR_STATUS_INVALID_ARGUMENT;
            }
            op->operands[1] = builder->z_values[g->zm];
            op->operand_count = 2;
        }
        if (!allocate_value(next, &op->destination)) {
            return PVIR_STATUS_INSUFFICIENT_CAPACITY;
        }
        next->z_values[g->zd] = op->destination;
    }
    return PVIR_STATUS_OK;
}

static PvirStatus build_index(const A64SVEPvirBuilder *builder,
                              A64SVEPvirBuilder *next,
                              const arg_sve_general *g, uint32_t vl_bits,
                              PvirInstruction *instruction)
{
    PvirIndexOperation *op;
    PvirVectorType type;
    size_t lanes;
    (void)builder;

    if (g->zd >= A64_SVE_PVIR_NUM_ZREGS) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        .signedness = PVIR_SIGNED,
        .element_bits = g->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    instruction->opcode = PVIR_OP_LANE_INDEX;
    op = &instruction->operation.index;
    op->type = type;
    op->predicate = pvir_predicate_all_true(lanes);
    op->inactive_policy = PVIR_INACTIVE_ZERO;
    op->merge_value = PVIR_NO_VALUE;
    if (g->src1_is_imm) {
        if (!allocate_value(next, &op->start_value)) {
            return PVIR_STATUS_INSUFFICIENT_CAPACITY;
        }
    } else {
        op->start_value = A64_SVE_PVIR_GPR_VALUE_BASE + g->zn;
    }
    if (g->src2_is_imm) {
        if (g->imm2 == 1) {
            op->step_value = PVIR_NO_VALUE;
        } else if (!allocate_value(next, &op->step_value)) {
            return PVIR_STATUS_INSUFFICIENT_CAPACITY;
        }
    } else {
        op->step_value = A64_SVE_PVIR_GPR_VALUE_BASE + g->zm;
    }
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[g->zd] = op->destination;
    return PVIR_STATUS_OK;
}

static PvirStatus build_wmul(const A64SVEPvirBuilder *builder,
                             A64SVEPvirBuilder *next,
                             const arg_sve_general *g, uint32_t vl_bits,
                             PvirInstruction *instruction)
{
    PvirVectorOperation *op;
    PvirVectorType type;
    size_t lanes;

    if (g->zd >= A64_SVE_PVIR_NUM_ZREGS || g->zn >= A64_SVE_PVIR_NUM_ZREGS ||
        g->zm >= A64_SVE_PVIR_NUM_ZREGS ||
        (g->predicated && g->pg >= A64_SVE_PVIR_NUM_PREGS)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        .signedness = g->is_signed ? PVIR_SIGNED : PVIR_UNSIGNED,
        .element_bits = g->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    op = &instruction->operation.vector;
    op->type = type;
    if (g->subop == SVE_WMUL_MULL) {
        PvirVectorType src_type;
        if (g->element_bits < 16) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        src_type = (PvirVectorType){
            .kind = PVIR_ELEMENT_INTEGER,
            .signedness = type.signedness,
            .element_bits = (uint16_t)(g->element_bits / 2u),
            .vector_bits = (uint32_t)(vl_bits / 2u),
        };
        instruction->opcode = PVIR_OP_MUL;
        op->predicate = pvir_predicate_all_true(lanes);
        op->inactive_policy = PVIR_INACTIVE_ZERO;
        op->merge_value = PVIR_NO_VALUE;
        op->operand_count = 2;
        op->operands[0] = builder->z_values[g->zn];
        op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        op->operand_types[0] = src_type;
        op->operands[1] = builder->z_values[g->zm];
        op->operand_kinds[1] = PVIR_OPERAND_VECTOR;
        op->operand_types[1] = src_type;
    } else {
        instruction->opcode = PVIR_OP_FMA;
        op->predicate = pvir_predicate_vector_mask(
            (PvirPredicateSource){
                .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
                .id = builder->p_values[g->pg],
                .version = builder->p_versions[g->pg],
            },
            lanes);
        op->inactive_policy = PVIR_INACTIVE_MERGE;
        op->merge_value = builder->z_values[g->zd];
        op->operand_count = 3;
        op->operands[0] = builder->z_values[g->zn];
        op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        op->operands[1] = builder->z_values[g->zm];
        op->operand_kinds[1] = PVIR_OPERAND_VECTOR;
        op->operands[2] = builder->z_values[g->zd];
        op->operand_kinds[2] = PVIR_OPERAND_VECTOR;
    }
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[g->zd] = op->destination;
    return PVIR_STATUS_OK;
}

static PvirStatus build_imm_bin(const A64SVEPvirBuilder *builder,
                                A64SVEPvirBuilder *next,
                                const arg_sve_general *g, uint32_t vl_bits,
                                PvirInstruction *instruction)
{
    PvirVectorOperation *op;
    PvirVectorType type;
    size_t lanes;

    if (g->zd >= A64_SVE_PVIR_NUM_ZREGS) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    bool is_log = (g->subop == SVE_IMM_BIN_AND ||
                   g->subop == SVE_IMM_BIN_ORR ||
                   g->subop == SVE_IMM_BIN_EOR);
    type = (PvirVectorType){
        .kind = is_log ? PVIR_ELEMENT_BITS : PVIR_ELEMENT_INTEGER,
        .signedness = is_log ? PVIR_SIGNLESS : PVIR_SIGNED,
        .element_bits = g->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    op = &instruction->operation.vector;
    op->type = type;
    op->predicate = pvir_predicate_all_true(lanes);
    op->inactive_policy = PVIR_INACTIVE_ZERO;
    op->merge_value = PVIR_NO_VALUE;
    if (g->subop == SVE_IMM_BIN_DUP) {
        PvirValueId imm_val;
        if (!allocate_value(next, &imm_val)) {
            return PVIR_STATUS_INSUFFICIENT_CAPACITY;
        }
        instruction->opcode = PVIR_OP_BROADCAST;
        op->operand_count = 1;
        op->operands[0] = imm_val;
        op->operand_kinds[0] = PVIR_OPERAND_SCALAR;
    } else {
        uint64_t imm_val = is_log
                               ? (g->mask_imm & mask_for_bits(g->element_bits))
                               : ((uint64_t)g->imm1 &
                                  mask_for_bits(g->element_bits));
        switch (g->subop) {
        case SVE_IMM_BIN_AND: instruction->opcode = PVIR_OP_AND; break;
        case SVE_IMM_BIN_ORR: instruction->opcode = PVIR_OP_OR; break;
        case SVE_IMM_BIN_EOR: instruction->opcode = PVIR_OP_XOR; break;
        case SVE_IMM_BIN_ADD: instruction->opcode = PVIR_OP_ADD; break;
        case SVE_IMM_BIN_MUL: instruction->opcode = PVIR_OP_MUL; break;
        default: instruction->opcode = PVIR_OP_SUB; break;
        }
        op->operand_count = 2;
        op->operands[0] = builder->z_values[g->zn];
        op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        op->operands[1] = PVIR_NO_VALUE;
        op->operand_kinds[1] = PVIR_OPERAND_IMMEDIATE;
        op->operand_immediates[1] = imm_val;
    }
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[g->zd] = op->destination;
    return PVIR_STATUS_OK;
}

static PvirStatus build_icmp(const A64SVEPvirBuilder *builder,
                             A64SVEPvirBuilder *next, const arg_sve_ext_op *e,
                             uint32_t vl_bits, PvirInstruction *instruction)
{
    PvirVectorOperation *op;
    PvirVectorType src_type;
    PvirVectorType mask_type;
    size_t lanes;
    bool uns = (e->subop == PVIR_COMPARE_LTU || e->subop == PVIR_COMPARE_LEU ||
                e->subop == PVIR_COMPARE_GTU || e->subop == PVIR_COMPARE_GEU);

    if (e->zd_pd >= A64_SVE_PVIR_NUM_PREGS ||
        e->zn >= A64_SVE_PVIR_NUM_ZREGS || e->pg >= A64_SVE_PVIR_NUM_PREGS ||
        (!e->cmp_with_imm && e->zm >= A64_SVE_PVIR_NUM_ZREGS) ||
        next->p_versions[e->zd_pd] == UINT64_MAX) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    src_type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        .signedness = uns ? PVIR_UNSIGNED : PVIR_SIGNED,
        .element_bits = e->src_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(src_type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    mask_type = (PvirVectorType){
        .kind = PVIR_ELEMENT_MASK,
        .signedness = PVIR_SIGNLESS,
        .element_bits = 1,
        .vector_bits = lanes,
    };
    init_instruction(instruction);
    instruction->opcode = PVIR_OP_COMPARE;
    op = &instruction->operation.vector;
    op->type = mask_type;
    op->predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = builder->p_values[e->pg],
            .version = builder->p_versions[e->pg],
        },
        lanes);
    op->inactive_policy = PVIR_INACTIVE_ZERO;
    op->merge_value = PVIR_NO_VALUE;
    op->compare_condition = (PvirCompareCondition)e->subop;
    op->operand_count = 2;
    op->operands[0] = builder->z_values[e->zn];
    op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    op->operand_types[0] = src_type;
    if (e->cmp_with_imm) {
        op->operand_kinds[1] = PVIR_OPERAND_IMMEDIATE;
        op->operands[1] = PVIR_NO_VALUE;
        op->operand_immediates[1] =
            (uint64_t)e->imm & mask_for_bits(e->src_bits);
    } else {
        op->operand_kinds[1] = PVIR_OPERAND_VECTOR;
        op->operands[1] = builder->z_values[e->zm];
        op->operand_types[1] = src_type;
    }
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->p_values[e->zd_pd] = op->destination;
    next->p_versions[e->zd_pd]++;
    invalidate_known_preg(next, e->zd_pd);
    if (next->status_version < UINT64_MAX) {
        next->status_version++;
    }
    return PVIR_STATUS_OK;
}

static PvirStatus build_sel(const A64SVEPvirBuilder *builder,
                            A64SVEPvirBuilder *next, const arg_sve_general *g,
                            uint32_t vl_bits, PvirInstruction *instruction)
{
    PvirVectorOperation *op;
    PvirVectorType type;
    size_t lanes;

    if (g->zd >= A64_SVE_PVIR_NUM_ZREGS || g->zn >= A64_SVE_PVIR_NUM_ZREGS ||
        g->zm >= A64_SVE_PVIR_NUM_ZREGS || g->pg >= A64_SVE_PVIR_NUM_PREGS) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_BITS,
        .signedness = PVIR_SIGNLESS,
        .element_bits = g->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    instruction->opcode = PVIR_OP_SELECT;
    op = &instruction->operation.vector;
    op->type = type;
    op->predicate = pvir_predicate_all_true(lanes);
    op->inactive_policy = PVIR_INACTIVE_ZERO;
    op->merge_value = PVIR_NO_VALUE;
    op->operand_count = 3;
    op->operands[0] = builder->p_values[g->pg];
    op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    op->operands[1] = builder->z_values[g->zn];
    op->operand_kinds[1] = PVIR_OPERAND_VECTOR;
    op->operands[2] = builder->z_values[g->zm];
    op->operand_kinds[2] = PVIR_OPERAND_VECTOR;
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[g->zd] = op->destination;
    return PVIR_STATUS_OK;
}

static PvirStatus build_idiv(const A64SVEPvirBuilder *builder,
                             A64SVEPvirBuilder *next, const arg_sve_general *g,
                             uint32_t vl_bits, PvirInstruction *instruction)
{
    PvirVectorOperation *op;
    PvirVectorType type;
    size_t lanes;

    if (g->zd >= A64_SVE_PVIR_NUM_ZREGS || g->zm >= A64_SVE_PVIR_NUM_ZREGS ||
        g->pg >= A64_SVE_PVIR_NUM_PREGS ||
        (g->element_bits != 32 && g->element_bits != 64)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        .signedness = g->is_signed ? PVIR_SIGNED : PVIR_UNSIGNED,
        .element_bits = g->element_bits,
        .vector_bits = vl_bits,
    };
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    init_instruction(instruction);
    instruction->opcode = PVIR_OP_DIV;
    op = &instruction->operation.vector;
    op->type = type;
    op->predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = builder->p_values[g->pg],
            .version = builder->p_versions[g->pg],
        },
        lanes);
    op->inactive_policy = PVIR_INACTIVE_MERGE;
    op->merge_value = builder->z_values[g->zd];
    op->operand_count = 2;
    if (g->subop == SVE_IDIV_SDIVR || g->subop == SVE_IDIV_UDIVR) {
        op->operands[0] = builder->z_values[g->zm];
        op->operands[1] = builder->z_values[g->zd];
    } else {
        op->operands[0] = builder->z_values[g->zd];
        op->operands[1] = builder->z_values[g->zm];
    }
    op->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    op->operand_kinds[1] = PVIR_OPERAND_VECTOR;
    if (!allocate_value(next, &op->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[g->zd] = op->destination;
    return PVIR_STATUS_OK;
}

PvirStatus a64_sve_build_pvir(A64SVEPvirBuilder *builder,
                              const A64SVEIR1Instruction *source,
                              uint32_t vl_bits,
                              PvirInstruction *out)
{
    A64SVEPvirBuilder next;
    PvirInstruction instruction;
    PvirStatus status;

    if (builder == NULL || !builder->initialized || source == NULL ||
        out == NULL || !arm_sve_vl_is_valid(vl_bits)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    next = *builder;
    switch (source->insn_type) {
    case AARCH64_A64_SVE_PTRUE:
    case AARCH64_A64_SVE_PFALSE:
    case AARCH64_A64_SVE_WHILELO:
    case AARCH64_A64_SVE_WHILE:
        status = build_predicate_instruction(builder, &next, source, vl_bits,
                                             &instruction);
        break;
    case AARCH64_A64_SVE_ADD:
        status = build_integer_binary(
            builder, &next, &source->arg.f_sve_add, vl_bits,
            PVIR_OP_ADD, &instruction);
        break;
    case AARCH64_A64_SVE_SUB:
        status = build_integer_binary(
            builder, &next, &source->arg.f_sve_sub, vl_bits,
            PVIR_OP_SUB, &instruction);
        break;
    case AARCH64_A64_SVE_MUL:
        status = build_integer_binary(
            builder, &next, &source->arg.f_sve_mul, vl_bits,
            PVIR_OP_MUL, &instruction);
        break;
    case AARCH64_A64_SVE_ORR:
        status = build_integer_binary(
            builder, &next, &source->arg.f_sve_orr, vl_bits,
            PVIR_OP_OR, &instruction);
        break;
    case AARCH64_A64_SVE_AND:
        status = build_integer_binary(
            builder, &next, &source->arg.f_sve_and, vl_bits,
            PVIR_OP_AND, &instruction);
        break;
    case AARCH64_A64_SVE_EOR:
        status = build_integer_binary(
            builder, &next, &source->arg.f_sve_eor, vl_bits,
            PVIR_OP_XOR, &instruction);
        break;
    case AARCH64_A64_SVE_SMAX:
        status = build_integer_binary(
            builder, &next, &source->arg.f_sve_smax, vl_bits,
            PVIR_OP_MAX, &instruction);
        break;
    case AARCH64_A64_SVE_UMAX:
        status = build_integer_binary(
            builder, &next, &source->arg.f_sve_umax, vl_bits,
            PVIR_OP_MAX, &instruction);
        break;
    case AARCH64_A64_SVE_SMIN:
        status = build_integer_binary(
            builder, &next, &source->arg.f_sve_smin, vl_bits,
            PVIR_OP_MIN, &instruction);
        break;
    case AARCH64_A64_SVE_UMIN:
        status = build_integer_binary(
            builder, &next, &source->arg.f_sve_umin, vl_bits,
            PVIR_OP_MIN, &instruction);
        break;
    case AARCH64_A64_SVE_LSL:
        status = build_integer_binary(
            builder, &next, &source->arg.f_sve_lsl, vl_bits,
            PVIR_OP_SHL, &instruction);
        break;
    case AARCH64_A64_SVE_LSR:
        status = build_integer_binary(
            builder, &next, &source->arg.f_sve_lsr, vl_bits,
            PVIR_OP_SHR, &instruction);
        break;
    case AARCH64_A64_SVE_ASR:
        status = build_integer_binary(
            builder, &next, &source->arg.f_sve_asr, vl_bits,
            PVIR_OP_SAR, &instruction);
        break;
    case AARCH64_A64_SVE_FADD:
        status = build_fp_binary(
            builder, &next, &source->arg.f_sve_fadd, vl_bits,
            PVIR_OP_ADD, &instruction);
        break;
    case AARCH64_A64_SVE_FSUB:
        status = build_fp_binary(
            builder, &next, &source->arg.f_sve_fsub, vl_bits,
            PVIR_OP_SUB, &instruction);
        break;
    case AARCH64_A64_SVE_FMUL:
        status = build_fp_binary(
            builder, &next, &source->arg.f_sve_fmul, vl_bits,
            PVIR_OP_MUL, &instruction);
        break;
    case AARCH64_A64_SVE_FDIV:
        status = build_fp_binary(
            builder, &next, &source->arg.f_sve_fdiv, vl_bits,
            PVIR_OP_DIV, &instruction);
        break;
    case AARCH64_A64_SVE_FABS:
        status = build_fp_unary(
            builder, &next, &source->arg.f_sve_fabs, vl_bits,
            PVIR_OP_AND,
            source->arg.f_sve_fabs.element_bits == 64
                ? UINT64_C(0x7fffffffffffffff)
                : UINT64_C(0x7fffffff),
            &instruction);
        break;
    case AARCH64_A64_SVE_FNEG:
        status = build_fp_unary(
            builder, &next, &source->arg.f_sve_fneg, vl_bits,
            PVIR_OP_XOR,
            source->arg.f_sve_fneg.element_bits == 64
                ? UINT64_C(0x8000000000000000)
                : UINT64_C(0x80000000),
            &instruction);
        break;
    case AARCH64_A64_SVE_DUP:
        status = build_broadcast(builder, &next, &source->arg.f_sve_dup,
                                 vl_bits, &instruction);
        break;
    case AARCH64_A64_SVE_NOT:
        status = build_integer_unary(builder, &next, &source->arg.f_sve_not,
                                     vl_bits, &instruction);
        break;
    case AARCH64_A64_SVE_SETFFR:
    case AARCH64_A64_SVE_RDFFR:
    case AARCH64_A64_SVE_WRFFR:
        status = build_ffr_operation(&next, source, vl_bits, &instruction);
        break;
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
        status = build_contiguous_memory(&next, source, vl_bits,
                                        &instruction);
        break;
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
        status = build_fault_load(&next, source, vl_bits, &instruction);
        break;
    case AARCH64_A64_SVE_COMPACT:
        status = build_compact(&next, &source->arg.f_sve_general, vl_bits,
                               &instruction);
        break;
    case AARCH64_A64_SVE_PTEST:
        status = build_ptest(&next, &source->arg.f_sve_pred_op, &instruction);
        break;
    case AARCH64_A64_SVE_BRK:
    case AARCH64_A64_SVE_PFIRST:
    case AARCH64_A64_SVE_PNEXT:
    case AARCH64_A64_SVE_PRED_LOGICAL:
        status = build_pred_op(builder, &next, &source->arg.f_sve_pred_op,
                               vl_bits, &instruction);
        break;
    case AARCH64_A64_SVE_CNTP:
    case AARCH64_A64_SVE_CNT_SCALAR:
        status = build_cnt(&next, &source->arg.f_sve_cnt, vl_bits, &instruction);
        break;
    case AARCH64_A64_SVE_LAST:
        status = build_last(&next, &source->arg.f_sve_last, vl_bits,
                            &instruction);
        break;
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
        status = build_mem_ext(&next, &source->arg.f_sve_mem_ext, vl_bits,
                               &instruction);
        break;
    case AARCH64_A64_SVE_FMLA:
    case AARCH64_A64_SVE_FMINMAX:
    case AARCH64_A64_SVE_FP_MATH:
        status = build_fp_ext(builder, &next, source->insn_type,
                              &source->arg.f_sve_fp_ext, vl_bits, &instruction);
        break;
    case AARCH64_A64_SVE_FCMP:
        status = build_fcmp(builder, &next, &source->arg.f_sve_ext_op, vl_bits,
                            &instruction);
        break;
    case AARCH64_A64_SVE_FCVT:
        status = build_fcvt(builder, &next, &source->arg.f_sve_ext_op, vl_bits,
                            &instruction);
        break;
    case AARCH64_A64_SVE_FREDUCE:
        status = build_reduce_insn(builder, &next, &source->arg.f_sve_general,
                                   vl_bits, true, &instruction);
        break;
    case AARCH64_A64_SVE_IREDUCE:
        status = build_reduce_insn(builder, &next, &source->arg.f_sve_general,
                                   vl_bits, false, &instruction);
        break;
    case AARCH64_A64_SVE_FIXED_INT:
        status = build_fixed_int(builder, &next, &source->arg.f_sve_general,
                                 vl_bits, &instruction);
        break;
    case AARCH64_A64_SVE_SHIFT_IMM:
        status = build_shift_imm(builder, &next, &source->arg.f_sve_ext_op,
                                 vl_bits, &instruction);
        break;
    case AARCH64_A64_SVE_INT_UNARY:
        status = build_int_unary(builder, &next, &source->arg.f_sve_general,
                                 vl_bits, &instruction);
        break;
    case AARCH64_A64_SVE_REV:
    case AARCH64_A64_SVE_PERMUTE:
        status = build_permute_insn(builder, &next, &source->arg.f_sve_general,
                                    vl_bits, &instruction);
        break;
    case AARCH64_A64_SVE_INDEX:
        status = build_index(builder, &next, &source->arg.f_sve_general,
                             vl_bits, &instruction);
        break;
    case AARCH64_A64_SVE_WMUL:
        status = build_wmul(builder, &next, &source->arg.f_sve_general, vl_bits,
                            &instruction);
        break;
    case AARCH64_A64_SVE_IMM_BIN:
        status = build_imm_bin(builder, &next, &source->arg.f_sve_general,
                               vl_bits, &instruction);
        break;
    case AARCH64_A64_SVE_ICMP:
        status = build_icmp(builder, &next, &source->arg.f_sve_ext_op, vl_bits,
                            &instruction);
        break;
    case AARCH64_A64_SVE_SEL:
        status = build_sel(builder, &next, &source->arg.f_sve_general, vl_bits,
                           &instruction);
        break;
    case AARCH64_A64_SVE_IDIV:
        status = build_idiv(builder, &next, &source->arg.f_sve_general, vl_bits,
                            &instruction);
        break;
    default:
        return PVIR_STATUS_UNSUPPORTED_OPERATION;
    }
    if (status != PVIR_STATUS_OK) {
        return status;
    }
    status = pvir_instruction_validate(&instruction);
    if (status != PVIR_STATUS_OK) {
        return status;
    }
    *builder = next;
    *out = instruction;
    return PVIR_STATUS_OK;
}

A64SVEIR2Dispatch a64_sve_ir2_dispatch(
    A64SVEAccessDisposition access_disposition)
{
    switch (access_disposition) {
    case A64_SVE_ACCESS_FEATURE_ABSENT:
        return A64_SVE_IR2_FEATURE_ABSENT;
    case A64_SVE_ACCESS_CONTROL_UNSUPPORTED:
    case A64_SVE_ACCESS_TRAP_REQUIRED:
        return A64_SVE_IR2_ACCESS_UNSUPPORTED;
    case A64_SVE_ACCESS_ALLOWED:
        /* Guest SVE legality must not depend on host LASX/LSX capability. */
        return A64_SVE_IR2_READY;
    default:
        return A64_SVE_IR2_ACCESS_UNSUPPORTED;
    }
}

A64SVEBackendMode a64_sve_select_backend(void)
{
    if (latx_cpu_has_lasx()) {
        return A64_SVE_BACKEND_LASX;
    }
    if (latx_cpu_has_lsx()) {
        return A64_SVE_BACKEND_LSX;
    }
    return A64_SVE_BACKEND_SCALAR_HELPER;
}
