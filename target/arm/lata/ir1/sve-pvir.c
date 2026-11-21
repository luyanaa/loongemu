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
        element_bits = source->arg.f_sve_ptrue.element_bits;
        break;
    case AARCH64_A64_SVE_WHILELO:
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
        if (pattern == 31) {
            operation->form = PVIR_PREDICATE_ALL_TRUE;
            active_lanes = lanes;
        } else if (pattern == 4) {
            active_lanes = lanes < 4 ? lanes : 4;
            operation->form = active_lanes == lanes
                                  ? PVIR_PREDICATE_ALL_TRUE
                                  : PVIR_PREDICATE_PREFIX;
        } else {
            return PVIR_STATUS_UNSUPPORTED_OPERATION;
        }
        operation->active_lanes =
            operation->form == PVIR_PREDICATE_ALL_TRUE ? lanes : active_lanes;
        break;
    }
    case AARCH64_A64_SVE_WHILELO: {
        const arg_sve_whilelo *whilelo = &source->arg.f_sve_whilelo;

        if ((whilelo->scalar_bits != 32 && whilelo->scalar_bits != 64) ||
            whilelo->rn >= 32 || whilelo->rm >= 32) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        operation->kind = PVIR_PREDICATE_GENERATOR_LANE_COMPARE;
        operation->form = PVIR_PREDICATE_VECTOR_MASK;
        operation->active_lanes = 0;
        operation->compare_condition = PVIR_COMPARE_LTU;
        operation->scalar_bits = whilelo->scalar_bits;
        operation->scalar_start = A64_SVE_PVIR_GPR_VALUE_BASE + whilelo->rn;
        operation->scalar_limit = A64_SVE_PVIR_GPR_VALUE_BASE + whilelo->rm;
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

    if (source->insn_type == AARCH64_A64_SVE_PTRUE) {
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
        ffr->effects.inactive_policy != PVIR_INACTIVE_NOT_APPLICABLE ||
        ffr->effects.memory.access != PVIR_MEMORY_NONE ||
        ffr->effects.fault_behavior != PVIR_FAULT_NONE ||
        ffr->effects.writes_nzcv) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }

    init_instruction(instruction);
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
    if (!allocate_value(next, &operation->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    set_state_write(&instruction->effects.arch_state, operation->state_id,
                    next->p_versions[ffr->pd]);
    next->p_values[ffr->pd] = operation->destination;
    next->p_versions[ffr->pd]++;
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

static PvirStatus build_fault_load(A64SVEPvirBuilder *next,
                                  const A64SVEIR1Instruction *source,
                                  uint32_t vl_bits,
                                  PvirInstruction *instruction)
{
    const arg_sve_fault_load *first = &source->arg.f_sve_fault_load;
    const arg_sve_nonfault_load *nonfault = &source->arg.f_sve_nonfault_load;
    bool is_first = source->insn_type == AARCH64_A64_SVE_LDFF1B;
    uint8_t zt = is_first ? first->zt : nonfault->zt;
    uint8_t pg = is_first ? first->pg : nonfault->pg;
    uint8_t rn = is_first ? first->rn : nonfault->rn;
    uint8_t element_bits =
        is_first ? first->element_bits : nonfault->element_bits;
    uint8_t memory_bits = is_first ? first->memory_bits : nonfault->memory_bits;
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

    if ((source->insn_type != AARCH64_A64_SVE_LDFF1B &&
         source->insn_type != AARCH64_A64_SVE_LDNF1B) ||
        zt >= A64_SVE_PVIR_NUM_ZREGS ||
        pg >= A64_SVE_PVIR_NUM_PREGS || rn > 31 || memory_bits != 8 ||
        (element_bits != 8 && element_bits != 16 && element_bits != 32 &&
         element_bits != 64) ||
        base_is_sp != (rn == 31) ||
        !fault_load_effects_valid(effects, behavior) ||
        next->ffr_value == PVIR_NO_VALUE ||
        next->ffr_version == UINT64_MAX) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (is_first) {
        if (first->has_index || first->rm != 31) {
            return PVIR_STATUS_UNSUPPORTED_OPERATION;
        }
    } else {
        displacement =
            (int64_t)nonfault->immediate_vl * (int64_t)(vl_bits / 8u);
    }

    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        .signedness = PVIR_UNSIGNED,
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
         opcode != PVIR_OP_MUL) ||
        binary->zd >= A64_SVE_PVIR_NUM_ZREGS ||
        binary->zm >= A64_SVE_PVIR_NUM_ZREGS ||
        binary->pg >= A64_SVE_PVIR_NUM_PREGS ||
        binary->effects.inactive_policy != PVIR_INACTIVE_MERGE ||
        !arm_sve_vl_is_valid(vl_bits)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_BITS,
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
    if (!allocate_value(next, &operation->destination)) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    next->z_values[binary->zd] = operation->destination;
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
    bool is_load = source->insn_type == AARCH64_A64_SVE_LD1B;

    if (memory->zt >= A64_SVE_PVIR_NUM_ZREGS ||
        memory->pg >= A64_SVE_PVIR_NUM_PREGS ||
        memory->rn > 31 || memory->memory_bits != 8 ||
        (memory->element_bits != 8 && memory->element_bits != 16 &&
         memory->element_bits != 32 && memory->element_bits != 64) ||
        memory->base_is_sp != (memory->rn == 31) ||
        !arm_sve_vl_is_valid(vl_bits)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    type = (PvirVectorType){
        .kind = PVIR_ELEMENT_INTEGER,
        .signedness = PVIR_UNSIGNED,
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
    case AARCH64_A64_SVE_WHILELO:
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
    case AARCH64_A64_SVE_SETFFR:
    case AARCH64_A64_SVE_RDFFR:
        status = build_ffr_operation(&next, source, vl_bits, &instruction);
        break;
    case AARCH64_A64_SVE_LD1B:
    case AARCH64_A64_SVE_ST1B:
        status = build_contiguous_memory(&next, source, vl_bits,
                                        &instruction);
        break;
    case AARCH64_A64_SVE_LDFF1B:
    case AARCH64_A64_SVE_LDNF1B:
        status = build_fault_load(&next, source, vl_bits, &instruction);
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
        return latx_cpu_has_lasx() ? A64_SVE_IR2_READY
                                   : A64_SVE_IR2_LASX_UNAVAILABLE;
    default:
        return A64_SVE_IR2_ACCESS_UNSUPPORTED;
    }
}
