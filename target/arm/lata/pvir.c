#include "la-pvir.h"

#include <limits.h>

static bool valid_element_bits(uint16_t bits)
{
    return bits == 8 || bits == 16 || bits == 32 || bits == 64;
}

static bool valid_predicate_element_bits(uint16_t bits)
{
    return bits == 1 || valid_element_bits(bits);
}

static size_t predicate_word_count(size_t lanes)
{
    return lanes / 64 + (lanes % 64 != 0);
}

static bool predicate_slice_end(const PvirPredicate *predicate, size_t *end)
{
    if (predicate->lanes > SIZE_MAX - predicate->first_lane) {
        return false;
    }
    if (end != NULL) {
        *end = predicate->first_lane + predicate->lanes;
    }
    return true;
}

static bool valid_predicate_source(PvirPredicateSource source)
{
    switch (source.kind) {
    case PVIR_PREDICATE_SOURCE_CONSTANT:
        return true;
    case PVIR_PREDICATE_SOURCE_PREDICATE:
    case PVIR_PREDICATE_SOURCE_VECTOR:
        return source.id != PVIR_NO_VALUE;
    default:
        return false;
    }
}

static bool valid_arch_state_effect(const PvirArchStateEffect *effect)
{
    if (!effect->present) {
        return true;
    }
    if (effect->domain < PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT ||
        effect->domain > PVIR_ARCH_STATE_DOMAIN_OTHER ||
        effect->action < PVIR_ARCH_STATE_READ ||
        effect->action > PVIR_ARCH_STATE_CLEAR ||
        effect->update_kind < PVIR_ARCH_STATE_UPDATE_NONE ||
        effect->update_kind > PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS) {
        return false;
    }
    return effect->update_kind == PVIR_ARCH_STATE_UPDATE_NONE ||
           (effect->update_kind ==
                PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS &&
            effect->action == PVIR_ARCH_STATE_UPDATE);
}

PvirStatus pvir_vector_type_lanes(PvirVectorType type, size_t *lanes)
{
    if (lanes == NULL || type.vector_bits == 0) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }

    switch (type.kind) {
    case PVIR_ELEMENT_INTEGER:
        if (!valid_element_bits(type.element_bits) ||
            (type.signedness != PVIR_SIGNED &&
             type.signedness != PVIR_UNSIGNED)) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        break;
    case PVIR_ELEMENT_FLOAT:
    case PVIR_ELEMENT_BITS:
        if (!valid_element_bits(type.element_bits) ||
            type.signedness != PVIR_SIGNLESS) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        break;
    case PVIR_ELEMENT_MASK:
        if (type.element_bits != 1 || type.signedness != PVIR_SIGNLESS) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        break;
    default:
        return PVIR_STATUS_INVALID_ARGUMENT;
    }

    if (type.vector_bits % type.element_bits != 0) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    *lanes = type.vector_bits / type.element_bits;
    return *lanes == 0 ? PVIR_STATUS_INVALID_ARGUMENT : PVIR_STATUS_OK;
}

static PvirPredicate make_predicate(PvirPredicateForm form, size_t first_lane,
                                    size_t lanes,
                                    PvirPredicateSource source)
{
    PvirPredicate predicate = {0};
    predicate.form = form;
    predicate.first_lane = first_lane;
    predicate.lanes = lanes;
    predicate.source = source;
    return predicate;
}

PvirPredicate pvir_predicate_all_true(size_t lanes)
{
    PvirPredicateSource source = {
        PVIR_PREDICATE_SOURCE_CONSTANT, 0, 0
    };
    return make_predicate(PVIR_PREDICATE_ALL_TRUE, 0, lanes, source);
}

PvirPredicate pvir_predicate_all_false(size_t lanes)
{
    PvirPredicateSource source = {
        PVIR_PREDICATE_SOURCE_CONSTANT, 0, 0
    };
    return make_predicate(PVIR_PREDICATE_ALL_FALSE, 0, lanes, source);
}

PvirPredicate pvir_predicate_prefix(PvirPredicateSource source, size_t lanes,
                                    size_t active_lanes)
{
    PvirPredicate predicate = make_predicate(PVIR_PREDICATE_PREFIX, 0, lanes,
                                             source);
    predicate.value.active_lanes = active_lanes;
    return predicate;
}

PvirPredicate pvir_predicate_suffix(PvirPredicateSource source, size_t lanes,
                                    size_t active_lanes)
{
    PvirPredicate predicate = make_predicate(PVIR_PREDICATE_SUFFIX, 0, lanes,
                                             source);
    predicate.value.active_lanes = active_lanes;
    return predicate;
}

PvirPredicate pvir_predicate_bitset(PvirPredicateSource source, size_t lanes,
                                    const uint64_t *words, size_t word_count)
{
    PvirPredicate predicate = make_predicate(PVIR_PREDICATE_BITSET, 0, lanes,
                                             source);
    predicate.value.bitset.words = words;
    predicate.value.bitset.word_count = word_count;
    return predicate;
}

PvirPredicate pvir_predicate_vector_mask(PvirPredicateSource source,
                                         size_t lanes)
{
    return make_predicate(PVIR_PREDICATE_VECTOR_MASK, 0, lanes, source);
}

PvirStatus pvir_predicate_validate(const PvirPredicate *predicate)
{
    if (predicate == NULL || predicate->lanes == 0 ||
        !predicate_slice_end(predicate, NULL) ||
        !valid_predicate_source(predicate->source)) {
        return PVIR_STATUS_INVALID_PREDICATE;
    }

    switch (predicate->form) {
    case PVIR_PREDICATE_ALL_TRUE:
    case PVIR_PREDICATE_ALL_FALSE:
        return PVIR_STATUS_OK;
    case PVIR_PREDICATE_PREFIX:
    case PVIR_PREDICATE_SUFFIX:
        return predicate->value.active_lanes <= predicate->lanes
                   ? PVIR_STATUS_OK
                   : PVIR_STATUS_INVALID_PREDICATE;
    case PVIR_PREDICATE_BITSET:
        if (predicate->value.bitset.words == NULL ||
            predicate->value.bitset.word_count <
                predicate_word_count(predicate->lanes)) {
            return PVIR_STATUS_INVALID_PREDICATE;
        }
        return PVIR_STATUS_OK;
    case PVIR_PREDICATE_VECTOR_MASK:
        return predicate->source.kind == PVIR_PREDICATE_SOURCE_VECTOR ||
                       predicate->source.kind ==
                           PVIR_PREDICATE_SOURCE_PREDICATE
                   ? PVIR_STATUS_OK
                   : PVIR_STATUS_INVALID_PREDICATE;
    default:
        return PVIR_STATUS_INVALID_PREDICATE;
    }
}

static bool predicate_lane_active(const PvirPredicate *predicate,
                                  size_t lane,
                                  const uint64_t *source_mask_words)
{
    switch (predicate->form) {
    case PVIR_PREDICATE_ALL_TRUE:
        return true;
    case PVIR_PREDICATE_ALL_FALSE:
        return false;
    case PVIR_PREDICATE_PREFIX:
        return lane < predicate->value.active_lanes;
    case PVIR_PREDICATE_SUFFIX:
        return lane >= predicate->lanes - predicate->value.active_lanes;
    case PVIR_PREDICATE_BITSET:
        return (predicate->value.bitset.words[lane / 64] &
                (UINT64_C(1) << (lane % 64))) != 0;
    case PVIR_PREDICATE_VECTOR_MASK: {
        size_t source_lane = predicate->first_lane + lane;
        return (source_mask_words[source_lane / 64] &
                (UINT64_C(1) << (source_lane % 64))) != 0;
    }
    default:
        return false;
    }
}

PvirStatus pvir_predicate_canonicalize(PvirPredicate *predicate)
{
    PvirStatus status = pvir_predicate_validate(predicate);
    size_t active_lanes = 0;
    bool prefix = true;
    bool suffix = true;
    size_t lane;

    if (status != PVIR_STATUS_OK) {
        return status;
    }

    if (predicate->form == PVIR_PREDICATE_PREFIX) {
        if (predicate->value.active_lanes == 0) {
            predicate->form = PVIR_PREDICATE_ALL_FALSE;
        } else if (predicate->value.active_lanes == predicate->lanes) {
            predicate->form = PVIR_PREDICATE_ALL_TRUE;
        }
        return PVIR_STATUS_OK;
    }
    if (predicate->form == PVIR_PREDICATE_SUFFIX) {
        if (predicate->value.active_lanes == 0) {
            predicate->form = PVIR_PREDICATE_ALL_FALSE;
        } else if (predicate->value.active_lanes == predicate->lanes) {
            predicate->form = PVIR_PREDICATE_ALL_TRUE;
        }
        return PVIR_STATUS_OK;
    }
    if (predicate->form != PVIR_PREDICATE_BITSET) {
        return PVIR_STATUS_OK;
    }

    for (lane = 0; lane < predicate->lanes; lane++) {
        if (predicate_lane_active(predicate, lane, NULL)) {
            active_lanes++;
        }
    }
    if (active_lanes == 0) {
        predicate->form = PVIR_PREDICATE_ALL_FALSE;
        return PVIR_STATUS_OK;
    }
    if (active_lanes == predicate->lanes) {
        predicate->form = PVIR_PREDICATE_ALL_TRUE;
        return PVIR_STATUS_OK;
    }

    for (lane = 0; lane < active_lanes; lane++) {
        if (!predicate_lane_active(predicate, lane, NULL)) {
            prefix = false;
            break;
        }
    }
    if (prefix) {
        for (lane = active_lanes; lane < predicate->lanes; lane++) {
            if (predicate_lane_active(predicate, lane, NULL)) {
                prefix = false;
                break;
            }
        }
    }
    if (prefix) {
        predicate->form = PVIR_PREDICATE_PREFIX;
        predicate->value.active_lanes = active_lanes;
        return PVIR_STATUS_OK;
    }

    for (lane = 0; lane < predicate->lanes - active_lanes; lane++) {
        if (predicate_lane_active(predicate, lane, NULL)) {
            suffix = false;
            break;
        }
    }
    if (suffix) {
        for (lane = predicate->lanes - active_lanes;
             lane < predicate->lanes; lane++) {
            if (!predicate_lane_active(predicate, lane, NULL)) {
                suffix = false;
                break;
            }
        }
    }
    if (suffix) {
        predicate->form = PVIR_PREDICATE_SUFFIX;
        predicate->value.active_lanes = active_lanes;
    }
    return PVIR_STATUS_OK;
}

PvirStatus pvir_predicate_expand(const PvirPredicate *predicate,
                                 uint16_t element_bits,
                                 const uint64_t *source_mask_words,
                                 size_t source_mask_word_count,
                                 uint64_t *expanded,
                                 size_t expanded_capacity)
{
    size_t lane;
    size_t slice_end;
    size_t needed_words;
    uint64_t lane_mask;

    if (expanded == NULL || !valid_predicate_element_bits(element_bits)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (pvir_predicate_validate(predicate) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_PREDICATE;
    }
    if (expanded_capacity < predicate->lanes) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }
    if (predicate->form == PVIR_PREDICATE_VECTOR_MASK) {
        if (!predicate_slice_end(predicate, &slice_end)) {
            return PVIR_STATUS_INVALID_PREDICATE;
        }
        needed_words = predicate_word_count(slice_end);
        if (source_mask_words == NULL ||
            source_mask_word_count < needed_words) {
            return PVIR_STATUS_NEEDS_VECTOR_MASK;
        }
    }

    lane_mask = element_bits == 64
                    ? UINT64_MAX
                    : (UINT64_C(1) << element_bits) - 1;
    for (lane = 0; lane < predicate->lanes; lane++) {
        expanded[lane] = predicate_lane_active(predicate, lane,
                                                source_mask_words)
                             ? lane_mask
                             : 0;
    }
    return PVIR_STATUS_OK;
}

static bool valid_memory_effect(const PvirMemoryEffect *effect)
{
    if (effect->access < PVIR_MEMORY_NONE ||
        effect->access > PVIR_MEMORY_READ_WRITE) {
        return false;
    }
    if (effect->ordering < PVIR_MEMORY_ORDER_UNSPECIFIED ||
        effect->ordering > PVIR_MEMORY_ORDER_SEQUENTIAL) {
        return false;
    }
    if (effect->access == PVIR_MEMORY_NONE) {
        return effect->ordering == PVIR_MEMORY_ORDER_UNSPECIFIED &&
               effect->inactive_lanes ==
                   PVIR_INACTIVE_MEMORY_NOT_APPLICABLE;
    }
    return effect->ordering != PVIR_MEMORY_ORDER_UNSPECIFIED &&
           (effect->inactive_lanes == PVIR_INACTIVE_MEMORY_SUPPRESSED ||
            effect->inactive_lanes ==
                PVIR_INACTIVE_MEMORY_NO_TRANSACTION);
}

static bool valid_fault_effect(const PvirFaultEffect *effect)
{
    if (effect->behavior < PVIR_FAULT_NONE ||
        effect->behavior > PVIR_FAULT_NONFAULTING ||
        effect->lane_order < PVIR_FAULT_ORDER_UNSPECIFIED ||
        effect->lane_order > PVIR_FAULT_ORDER_LANE_ASCENDING ||
        effect->fault_result < PVIR_FAULT_RESULT_UNSPECIFIED ||
        effect->fault_result > PVIR_FAULT_RESULT_VALUE) {
        return false;
    }

    switch (effect->behavior) {
    case PVIR_FAULT_NONE:
        return effect->lane_order == PVIR_FAULT_ORDER_UNSPECIFIED &&
               effect->fault_result == PVIR_FAULT_RESULT_UNSPECIFIED &&
               effect->unknown_from_lane == PVIR_NO_VALUE;
    case PVIR_FAULT_PRECISE:
        return (effect->lane_order == PVIR_FAULT_ORDER_UNSPECIFIED ||
                effect->lane_order == PVIR_FAULT_ORDER_LANE_ASCENDING) &&
               effect->fault_result == PVIR_FAULT_RESULT_UNSPECIFIED &&
               effect->unknown_from_lane == PVIR_NO_VALUE;
    case PVIR_FAULT_FIRST_FAULT:
        return effect->lane_order == PVIR_FAULT_ORDER_LANE_ASCENDING &&
               effect->fault_result == PVIR_FAULT_RESULT_UNKNOWN &&
               effect->unknown_from_lane != PVIR_NO_VALUE;
    case PVIR_FAULT_NONFAULTING:
        return effect->lane_order == PVIR_FAULT_ORDER_UNSPECIFIED &&
               effect->fault_result == PVIR_FAULT_RESULT_UNKNOWN &&
               effect->unknown_from_lane != PVIR_NO_VALUE;
    default:
        return false;
    }
}

static bool valid_effect_metadata(const PvirEffectMetadata *effects)
{
    const PvirSideEffectFlags known_side_effects =
        PVIR_SIDE_EFFECT_IO | PVIR_SIDE_EFFECT_VOLATILE |
        PVIR_SIDE_EFFECT_ATOMIC | PVIR_SIDE_EFFECT_ORDERED |
        PVIR_SIDE_EFFECT_BARRIER;
    const PvirSideEffectFlags physical_memory_side_effects =
        PVIR_SIDE_EFFECT_IO | PVIR_SIDE_EFFECT_VOLATILE |
        PVIR_SIDE_EFFECT_ATOMIC;

    if (!valid_memory_effect(&effects->memory) ||
        !valid_fault_effect(&effects->fault) ||
        (effects->side_effects & ~known_side_effects) != 0 ||
        ((effects->side_effects & PVIR_SIDE_EFFECT_ORDERED) != 0 &&
         effects->memory.access == PVIR_MEMORY_NONE) ||
        ((effects->side_effects & physical_memory_side_effects) != 0 &&
         (effects->memory.access == PVIR_MEMORY_NONE ||
          effects->memory.inactive_lanes !=
              PVIR_INACTIVE_MEMORY_NO_TRANSACTION)) ||
        (effects->fp_exceptions != PVIR_FP_EXCEPTIONS_NONE &&
         effects->fp_exceptions !=
             PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY) ||
        !valid_arch_state_effect(&effects->arch_state)) {
        return false;
    }
    if (effects->fault.behavior == PVIR_FAULT_FIRST_FAULT &&
        (!effects->arch_state.present ||
         effects->arch_state.domain != PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT ||
         effects->arch_state.action != PVIR_ARCH_STATE_UPDATE ||
         effects->arch_state.update_kind !=
             PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS)) {
        return false;
    }
    if (effects->fault.behavior == PVIR_FAULT_NONFAULTING &&
        effects->arch_state.present &&
        effects->arch_state.domain == PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT &&
        (effects->arch_state.action != PVIR_ARCH_STATE_UPDATE ||
         effects->arch_state.update_kind !=
             PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS)) {
        return false;
    }
    return true;
}


static bool vector_opcode_arity(PvirOpcode opcode, uint8_t *arity)
{
    switch (opcode) {
    case PVIR_OP_ADD:
    case PVIR_OP_SUB:
    case PVIR_OP_MUL:
    case PVIR_OP_DIV:
    case PVIR_OP_AND:
    case PVIR_OP_OR:
    case PVIR_OP_XOR:
    case PVIR_OP_SHL:
    case PVIR_OP_SHR:
    case PVIR_OP_SAR:
    case PVIR_OP_COMPARE:
        *arity = 2;
        return true;
    case PVIR_OP_FMA:
    case PVIR_OP_SELECT:
        *arity = 3;
        return true;
    case PVIR_OP_TRUNCATE:
    case PVIR_OP_EXTEND:
    case PVIR_OP_MOVE:
        *arity = 1;
        return true;
    case PVIR_OP_REM:
    case PVIR_OP_MIN:
    case PVIR_OP_MAX:
        *arity = 2;
        return true;
    case PVIR_OP_NOT:
    case PVIR_OP_BROADCAST:
        *arity = 1;
        return true;
    case PVIR_OP_LOAD:
        *arity = 0;
        return true;
    case PVIR_OP_STORE:
        *arity = 1;
        return true;
    default:
        return false;
    }
}

static bool same_vector_type(PvirVectorType left, PvirVectorType right)
{
    return left.kind == right.kind && left.signedness == right.signedness &&
           left.element_bits == right.element_bits &&
           left.vector_bits == right.vector_bits;
}

static bool valid_memory_address(const PvirMemoryAddress *address)
{
    if (!address->present) {
        return false;
    }
    switch (address->base_kind) {
    case PVIR_MEMORY_BASE_VALUE:
        return address->base_value != PVIR_NO_VALUE;
    case PVIR_MEMORY_BASE_STACK_POINTER:
        return address->base_value == PVIR_NO_VALUE;
    default:
        return false;
    }
}

static bool zero_vector_type(PvirVectorType type)
{
    return type.kind == PVIR_ELEMENT_INTEGER &&
           type.signedness == PVIR_SIGNLESS && type.element_bits == 0 &&
           type.vector_bits == 0;
}

static bool resolve_operand_type(const PvirVectorOperation *operation,
                                size_t lanes, uint8_t index,
                                PvirVectorType *type)
{
    PvirVectorType source_type = operation->operand_types[index];
    size_t source_lanes;

    if (source_type.element_bits == 0) {
        *type = operation->type;
        return true;
    }

    if (pvir_vector_type_lanes(source_type, &source_lanes) !=
            PVIR_STATUS_OK ||
        source_lanes != lanes ||
        source_lanes > SIZE_MAX / source_type.element_bits ||
        source_type.vector_bits != source_lanes * source_type.element_bits) {
        return false;
    }
    *type = source_type;
    return true;
}

static bool immediate_fits(uint64_t value, uint16_t bits)
{
    return bits != 0 && bits <= 64 &&
           (bits == 64 || value < (UINT64_C(1) << bits));
}

static bool valid_compare_condition(PvirCompareCondition condition)
{
    return condition >= PVIR_COMPARE_EQ &&
           condition <= PVIR_COMPARE_GEU;
}

static bool compare_condition_matches(PvirCompareCondition condition,
                                     PvirVectorType type)
{
    if (condition == PVIR_COMPARE_EQ || condition == PVIR_COMPARE_NE) {
        return true;
    }
    if (type.kind == PVIR_ELEMENT_FLOAT) {
        return condition == PVIR_COMPARE_LT ||
               condition == PVIR_COMPARE_LE ||
               condition == PVIR_COMPARE_GT ||
               condition == PVIR_COMPARE_GE;
    }
    if (type.kind == PVIR_ELEMENT_INTEGER &&
        type.signedness == PVIR_SIGNED) {
        return condition == PVIR_COMPARE_LT ||
               condition == PVIR_COMPARE_LE ||
               condition == PVIR_COMPARE_GT ||
               condition == PVIR_COMPARE_GE;
    }
    if (type.kind == PVIR_ELEMENT_INTEGER &&
        type.signedness == PVIR_UNSIGNED) {
        return condition == PVIR_COMPARE_LTU ||
               condition == PVIR_COMPARE_LEU ||
               condition == PVIR_COMPARE_GTU ||
               condition == PVIR_COMPARE_GEU;
    }
    return false;
}

static bool valid_operand_fields(const PvirVectorOperation *operation,
                                size_t lanes, uint8_t index,
                                PvirVectorType *resolved_type)
{
    PvirOperandKind kind = operation->operand_kinds[index];

    switch (kind) {
    case PVIR_OPERAND_VECTOR:
        return operation->operands[index] != PVIR_NO_VALUE &&
               operation->operand_immediates[index] == 0 &&
               resolve_operand_type(operation, lanes, index,
                                    resolved_type);
    case PVIR_OPERAND_SCALAR:
        return operation->operands[index] != PVIR_NO_VALUE &&
               operation->operand_immediates[index] == 0 &&
               zero_vector_type(operation->operand_types[index]);
    case PVIR_OPERAND_IMMEDIATE:
        return operation->operands[index] == PVIR_NO_VALUE &&
               zero_vector_type(operation->operand_types[index]);
    default:
        return false;
    }
}

static bool valid_operation_operands(PvirOpcode opcode,
                                     const PvirVectorOperation *operation,
                                     size_t lanes)
{
    PvirVectorType operand_types[PVIR_MAX_OPERANDS] = {0};
    uint8_t i;

    for (i = 0; i < operation->operand_count; i++) {
        if (!valid_operand_fields(operation, lanes, i, &operand_types[i])) {
            return false;
        }
    }

    if (opcode == PVIR_OP_COMPARE) {
        if (operation->type.kind != PVIR_ELEMENT_MASK ||
            !valid_compare_condition(operation->compare_condition) ||
            operation->operand_kinds[0] != PVIR_OPERAND_VECTOR ||
            operation->operand_kinds[1] != PVIR_OPERAND_VECTOR ||
            !same_vector_type(operand_types[0], operand_types[1])) {
            return false;
        }
        return compare_condition_matches(operation->compare_condition,
                                         operand_types[0]);
    }

    if (operation->compare_condition != PVIR_COMPARE_NONE ||
        (operation->type.kind == PVIR_ELEMENT_MASK &&
         opcode != PVIR_OP_LOAD && opcode != PVIR_OP_STORE)) {
        return false;
    }
    switch (opcode) {

    case PVIR_OP_LOAD:
        return operation->operand_count == 0;
    case PVIR_OP_STORE:
        return operation->operand_kinds[0] == PVIR_OPERAND_VECTOR &&
               same_vector_type(operand_types[0], operation->type);
    case PVIR_OP_ADD:
    case PVIR_OP_SUB:
    case PVIR_OP_MUL:
    case PVIR_OP_DIV:
    case PVIR_OP_AND:
    case PVIR_OP_OR:
    case PVIR_OP_XOR:
    case PVIR_OP_SHL:
    case PVIR_OP_SHR:
    case PVIR_OP_SAR:
        if (operation->operand_kinds[0] != PVIR_OPERAND_VECTOR) {
            return false;
        }
        if (operation->operand_kinds[1] == PVIR_OPERAND_IMMEDIATE) {
            return immediate_fits(operation->operand_immediates[1],
                                  operand_types[0].element_bits);
        }
        return operation->operand_kinds[1] == PVIR_OPERAND_VECTOR ||
               operation->operand_kinds[1] == PVIR_OPERAND_SCALAR;
    case PVIR_OP_FMA:
    case PVIR_OP_SELECT:
        return operation->operand_kinds[0] == PVIR_OPERAND_VECTOR &&
               operation->operand_kinds[1] == PVIR_OPERAND_VECTOR &&
               operation->operand_kinds[2] == PVIR_OPERAND_VECTOR;
    case PVIR_OP_TRUNCATE:
        return operation->operand_kinds[0] == PVIR_OPERAND_VECTOR &&
               operation->type.kind == PVIR_ELEMENT_INTEGER &&
               operand_types[0].kind == PVIR_ELEMENT_INTEGER &&
               operand_types[0].element_bits > operation->type.element_bits;
    case PVIR_OP_EXTEND:
        return operation->operand_kinds[0] == PVIR_OPERAND_VECTOR &&
               operation->type.kind == PVIR_ELEMENT_INTEGER &&
               operand_types[0].kind == PVIR_ELEMENT_INTEGER &&
               operand_types[0].element_bits < operation->type.element_bits;
    case PVIR_OP_MOVE:
        return operation->operand_kinds[0] == PVIR_OPERAND_VECTOR &&
               same_vector_type(operand_types[0], operation->type);
    case PVIR_OP_REM:
        if (operation->type.kind != PVIR_ELEMENT_INTEGER ||
            operation->operand_kinds[0] != PVIR_OPERAND_VECTOR) {
            return false;
        }
        if (operation->operand_kinds[1] == PVIR_OPERAND_IMMEDIATE) {
            return immediate_fits(operation->operand_immediates[1],
                                  operand_types[0].element_bits);
        }
        return operation->operand_kinds[1] == PVIR_OPERAND_VECTOR ||
               operation->operand_kinds[1] == PVIR_OPERAND_SCALAR;
    case PVIR_OP_MIN:
    case PVIR_OP_MAX:
        /* Integer only: FP min/max carries NaN-propagation and sNaN rules
         * that need their own op/payload rather than an implicit quiet form. */
        if (operation->type.kind != PVIR_ELEMENT_INTEGER) {
            return false;
        }
        if (operation->operand_kinds[0] != PVIR_OPERAND_VECTOR) {
            return false;
        }
        if (operation->operand_kinds[1] == PVIR_OPERAND_IMMEDIATE) {
            return immediate_fits(operation->operand_immediates[1],
                                  operand_types[0].element_bits);
        }
        return operation->operand_kinds[1] == PVIR_OPERAND_VECTOR ||
               operation->operand_kinds[1] == PVIR_OPERAND_SCALAR;
    case PVIR_OP_NOT:
        return operation->operand_kinds[0] == PVIR_OPERAND_VECTOR;
    case PVIR_OP_BROADCAST:
        return operation->operand_kinds[0] == PVIR_OPERAND_SCALAR;
    default:
        return false;
    }
}

static bool valid_state_operation_values(
    PvirArchStateAction action, PvirArchStateUpdateKind update_kind,
    const PvirArchStateOperation *operation)
{
    bool values_valid;

    switch (action) {
    case PVIR_ARCH_STATE_READ:
        values_valid = operation->input_value == PVIR_NO_VALUE &&
                       operation->output_value != PVIR_NO_VALUE;
        break;
    case PVIR_ARCH_STATE_WRITE:
        values_valid = operation->input_value != PVIR_NO_VALUE &&
                       operation->output_value == PVIR_NO_VALUE;
        break;
    case PVIR_ARCH_STATE_UPDATE:
        values_valid = operation->input_value != PVIR_NO_VALUE &&
                       operation->output_value != PVIR_NO_VALUE;
        break;
    case PVIR_ARCH_STATE_CLEAR:
        values_valid = operation->input_value == PVIR_NO_VALUE &&
                       operation->output_value == PVIR_NO_VALUE;
        break;
    default:
        return false;
    }

    if (!values_valid) {
        return false;
    }
    if (update_kind == PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS) {
        return action == PVIR_ARCH_STATE_UPDATE &&
               operation->clear_from_lane != PVIR_NO_VALUE;
    }
    return operation->clear_from_lane == PVIR_NO_VALUE;
}

static bool valid_arch_state_operation(const PvirInstruction *instruction)
{
    const PvirArchStateEffect *effect = &instruction->effects.arch_state;
    const PvirArchStateOperation *operation =
        &instruction->operation.arch_state;

    return effect->present && valid_arch_state_effect(effect) &&
           valid_state_operation_values(effect->action, effect->update_kind,
                                        operation);
}

static bool valid_arch_state_write(const PvirArchStateEffect *effect)
{
    return effect->present && valid_arch_state_effect(effect) &&
           effect->action == PVIR_ARCH_STATE_WRITE &&
           effect->update_kind == PVIR_ARCH_STATE_UPDATE_NONE &&
           effect->version_before != UINT64_MAX &&
           effect->version_after == effect->version_before + 1;
}

static bool valid_predicate_state_write(const PvirArchStateEffect *effect)
{
    return valid_arch_state_write(effect) &&
           effect->domain == PVIR_ARCH_STATE_DOMAIN_OTHER;
}

static bool valid_predicate_generator(const PvirInstruction *instruction)
{
    const PvirPredicateGeneratorOperation *operation =
        &instruction->operation.predicate_generator;
    const PvirArchStateEffect *state_effect =
        &instruction->effects.arch_state;
    size_t lanes;

    if (instruction->effects.memory.access != PVIR_MEMORY_NONE ||
        instruction->effects.fault.behavior != PVIR_FAULT_NONE ||
        instruction->effects.side_effects != 0 ||
        instruction->effects.fp_exceptions != PVIR_FP_EXCEPTIONS_NONE ||
        !valid_arch_state_write(state_effect) ||
        state_effect->state_id != operation->state_id ||
        operation->destination == PVIR_NO_VALUE ||
        operation->lane_type.kind != PVIR_ELEMENT_BITS ||
        operation->lane_type.signedness != PVIR_SIGNLESS ||
        pvir_vector_type_lanes(operation->lane_type, &lanes) !=
            PVIR_STATUS_OK) {
        return false;
    }
    if (operation->status_state.present &&
        !valid_predicate_state_write(&operation->status_state)) {
        return false;
    }

    switch (operation->kind) {
    case PVIR_PREDICATE_GENERATOR_CONSTANT:
        /*
         * A constant generator may also write PSTATE flags (PTRUES): the
         * PredTest result follows from form/active_lanes, so no extra payload
         * is needed. The generic status_state check above already validated
         * the shape of that write.
         */
        if (operation->compare_condition != PVIR_COMPARE_NONE ||
            operation->scalar_bits != 0 ||
            operation->scalar_start != PVIR_NO_VALUE ||
            operation->scalar_limit != PVIR_NO_VALUE ||
            operation->source_state.present) {
            return false;
        }
        if (state_effect->domain == PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT) {
            return operation->lane_type.element_bits == 8 &&
                   operation->form == PVIR_PREDICATE_ALL_TRUE &&
                   operation->active_lanes == lanes;
        }
        if (state_effect->domain != PVIR_ARCH_STATE_DOMAIN_OTHER) {
            return false;
        }
        if (operation->form == PVIR_PREDICATE_ALL_TRUE) {
            return operation->active_lanes == lanes;
        }
        if (operation->form == PVIR_PREDICATE_ALL_FALSE) {
            return operation->active_lanes == 0;
        }
        return operation->form == PVIR_PREDICATE_PREFIX &&
               operation->active_lanes <= lanes;
    case PVIR_PREDICATE_GENERATOR_LANE_COMPARE:
        return state_effect->domain == PVIR_ARCH_STATE_DOMAIN_OTHER &&
               !operation->source_state.present &&
               operation->form == PVIR_PREDICATE_VECTOR_MASK &&
               operation->active_lanes == 0 &&
               operation->compare_condition == PVIR_COMPARE_LTU &&
               (operation->scalar_bits == 32 ||
                operation->scalar_bits == 64) &&
               operation->scalar_start != PVIR_NO_VALUE &&
               operation->scalar_limit != PVIR_NO_VALUE;
    case PVIR_PREDICATE_GENERATOR_STATE_READ:
        return state_effect->domain == PVIR_ARCH_STATE_DOMAIN_OTHER &&
               operation->form == PVIR_PREDICATE_VECTOR_MASK &&
               operation->active_lanes == 0 &&
               operation->compare_condition == PVIR_COMPARE_NONE &&
               operation->scalar_bits == 0 &&
               operation->scalar_start == PVIR_NO_VALUE &&
               operation->scalar_limit == PVIR_NO_VALUE &&
               !operation->status_state.present &&
               operation->source_state.present &&
               valid_arch_state_effect(&operation->source_state) &&
               operation->source_state.domain ==
                   PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT &&
               operation->source_state.action == PVIR_ARCH_STATE_READ &&
               operation->source_state.update_kind ==
                   PVIR_ARCH_STATE_UPDATE_NONE &&
               operation->source_state.version_before ==
                   operation->source_state.version_after &&
               operation->source_state_value != PVIR_NO_VALUE;
    default:
        return false;
    }
}

static bool opcode_can_raise_fp(PvirOpcode opcode)
{
    return opcode == PVIR_OP_ADD || opcode == PVIR_OP_SUB ||
           opcode == PVIR_OP_MUL || opcode == PVIR_OP_DIV ||
           opcode == PVIR_OP_FMA;
}

static bool instruction_has_float_elements(
    const PvirVectorOperation *operation)
{
    uint8_t i;

    if (operation->type.kind == PVIR_ELEMENT_FLOAT) {
        return true;
    }
    for (i = 0; i < operation->operand_count; i++) {
        PvirVectorType operand_type;

        if (operation->operand_kinds[i] != PVIR_OPERAND_VECTOR) {
            continue;
        }
        operand_type = operation->operand_types[i];
        if (operand_type.element_bits == 0) {
            operand_type = operation->type;
        }
        if (operand_type.kind == PVIR_ELEMENT_FLOAT) {
            return true;
        }
    }
    return false;
}

static bool valid_lane_operation(const PvirInstruction *instruction)
{
    const PvirLaneOperation *operation = &instruction->operation.lane;
    size_t lanes;

    if (instruction->effects.memory.access != PVIR_MEMORY_NONE ||
        instruction->effects.fault.behavior != PVIR_FAULT_NONE ||
        instruction->effects.side_effects != 0 ||
        instruction->effects.fp_exceptions != PVIR_FP_EXCEPTIONS_NONE ||
        instruction->effects.arch_state.present) {
        return false;
    }
    if (operation->source == PVIR_NO_VALUE ||
        operation->destination == PVIR_NO_VALUE ||
        operation->vector_type.kind == PVIR_ELEMENT_MASK ||
        pvir_vector_type_lanes(operation->vector_type, &lanes) !=
            PVIR_STATUS_OK ||
        operation->lane >= lanes) {
        return false;
    }
    if (instruction->opcode == PVIR_OP_EXTRACT) {
        return operation->merge_value == PVIR_NO_VALUE;
    }
    return operation->merge_value != PVIR_NO_VALUE;
}

static bool valid_convert_operation(const PvirInstruction *instruction)
{
    const PvirConvertOperation *operation = &instruction->operation.convert;
    size_t source_lanes;
    size_t destination_lanes;
    bool has_float;

    if (instruction->effects.memory.access != PVIR_MEMORY_NONE ||
        instruction->effects.fault.behavior != PVIR_FAULT_NONE ||
        instruction->effects.side_effects != 0 ||
        instruction->effects.arch_state.present ||
        operation->source == PVIR_NO_VALUE ||
        operation->destination == PVIR_NO_VALUE ||
        operation->rounding < PVIR_ROUND_DEFAULT ||
        operation->rounding > PVIR_ROUND_RNV ||
        operation->source_type.kind == PVIR_ELEMENT_MASK ||
        operation->destination_type.kind == PVIR_ELEMENT_MASK ||
        pvir_vector_type_lanes(operation->source_type, &source_lanes) !=
            PVIR_STATUS_OK ||
        pvir_vector_type_lanes(operation->destination_type,
                               &destination_lanes) != PVIR_STATUS_OK ||
        source_lanes != destination_lanes) {
        return false;
    }
    has_float = operation->source_type.kind == PVIR_ELEMENT_FLOAT ||
                operation->destination_type.kind == PVIR_ELEMENT_FLOAT;
    if (!has_float) {
        /* Integer width changes belong to TRUNCATE/EXTEND. */
        return false;
    }
    return instruction->effects.fp_exceptions ==
           PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
}

static bool valid_fixed_rounding(PvirRounding rounding)
{
    return rounding == PVIR_ROUND_RNU || rounding == PVIR_ROUND_NE ||
           rounding == PVIR_ROUND_RDN || rounding == PVIR_ROUND_ROD;
}

/* Shared checks for the new vector-shaped payloads (no memory/effects). */
static bool valid_plain_vector_facts(const PvirInstruction *instruction,
                                     const PvirVectorType *type,
                                     const PvirPredicate *predicate,
                                     size_t *lanes)
{
    if (instruction->effects.memory.access != PVIR_MEMORY_NONE ||
        instruction->effects.fault.behavior != PVIR_FAULT_NONE ||
        instruction->effects.side_effects != 0) {
        return false;
    }
    if (pvir_vector_type_lanes(*type, lanes) != PVIR_STATUS_OK ||
        predicate->lanes != *lanes ||
        pvir_predicate_validate(predicate) != PVIR_STATUS_OK) {
        return false;
    }
    return true;
}

static bool valid_inactive_policy(PvirInactivePolicy policy,
                                  PvirValueId merge_value, bool is_store)
{
    switch (policy) {
    case PVIR_INACTIVE_NOT_APPLICABLE:
        return is_store && merge_value == PVIR_NO_VALUE;
    case PVIR_INACTIVE_MERGE:
        return !is_store && merge_value != PVIR_NO_VALUE;
    case PVIR_INACTIVE_ZERO:
    case PVIR_INACTIVE_UNKNOWN:
        return !is_store && merge_value == PVIR_NO_VALUE;
    default:
        return false;
    }
}

static bool valid_fixed_operation(const PvirInstruction *instruction)
{
    const PvirFixedPointOperation *operation =
        &instruction->operation.fixed;
    size_t lanes;
    size_t i;

    if (!valid_plain_vector_facts(instruction, &operation->type,
                                  &operation->predicate, &lanes) ||
        operation->type.kind != PVIR_ELEMENT_INTEGER ||
        !valid_inactive_policy(operation->inactive_policy,
                               operation->merge_value, false) ||
        operation->destination == PVIR_NO_VALUE ||
        instruction->effects.fp_exceptions != PVIR_FP_EXCEPTIONS_NONE ||
        operation->kind < PVIR_FIXED_AVG_ADD ||
        operation->kind > PVIR_FIXED_CLIP ||
        operation->operand_kinds[0] != PVIR_OPERAND_VECTOR ||
        operation->operands[0] == PVIR_NO_VALUE) {
        return false;
    }
    if (operation->operand_kinds[1] == PVIR_OPERAND_IMMEDIATE) {
        if (!immediate_fits(operation->operand_immediates[1],
                            operation->type.element_bits)) {
            return false;
        }
    } else if (operation->operand_kinds[1] != PVIR_OPERAND_VECTOR &&
               operation->operand_kinds[1] != PVIR_OPERAND_SCALAR) {
        return false;
    }
    if (operation->operand_kinds[1] != PVIR_OPERAND_IMMEDIATE &&
        operation->operands[1] == PVIR_NO_VALUE) {
        return false;
    }
    for (i = 0; i < 2; i++) {
        if (operation->operand_immediates[i] != 0 &&
            operation->operand_kinds[i] != PVIR_OPERAND_IMMEDIATE) {
            return false;
        }
    }
    /* AVG_* and SCALE_SHIFT/CLIP round; SAT_* do not. */
    switch (operation->kind) {
    case PVIR_FIXED_AVG_ADD:
    case PVIR_FIXED_AVG_SUB:
    case PVIR_FIXED_SCALE_SHIFT:
    case PVIR_FIXED_CLIP:
        if (!valid_fixed_rounding(operation->rounding)) {
            return false;
        }
        break;
    default:
        if (operation->rounding != PVIR_ROUND_DEFAULT) {
            return false;
        }
        break;
    }
    if (operation->saturating) {
        if (!instruction->effects.arch_state.present ||
            instruction->effects.arch_state.domain !=
                PVIR_ARCH_STATE_DOMAIN_OTHER ||
            instruction->effects.arch_state.action !=
                PVIR_ARCH_STATE_UPDATE ||
            operation->sat_state.output_value == PVIR_NO_VALUE) {
            return false;
        }
    } else if (instruction->effects.arch_state.present) {
        return false;
    }
    return true;
}

static bool valid_fp_operation(const PvirInstruction *instruction)
{
    const PvirFPOpOperation *operation = &instruction->operation.fp;
    size_t lanes;

    if (!valid_plain_vector_facts(instruction, &operation->type,
                                  &operation->predicate, &lanes) ||
        operation->type.kind != PVIR_ELEMENT_FLOAT ||
        !valid_inactive_policy(operation->inactive_policy,
                               operation->merge_value, false) ||
        operation->destination == PVIR_NO_VALUE ||
        instruction->effects.arch_state.present ||
        instruction->effects.fp_exceptions !=
            PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY ||
        operation->kind < PVIR_FP_ADD || operation->kind > PVIR_FP_SQRT ||
        operation->rounding < PVIR_ROUND_DEFAULT ||
        operation->rounding > PVIR_ROUND_ROD) {
        return false;
    }
    if (operation->operand_kinds[0] != PVIR_OPERAND_VECTOR ||
        operation->operands[0] == PVIR_NO_VALUE) {
        return false;
    }
    if (operation->kind == PVIR_FP_SQRT) {
        /* SQRT is unary: the second operand slot must be unused. */
        return operation->operands[1] == PVIR_NO_VALUE;
    }
    if (operation->operand_kinds[1] != PVIR_OPERAND_VECTOR &&
        operation->operand_kinds[1] != PVIR_OPERAND_SCALAR) {
        return false;
    }
    return operation->operands[1] != PVIR_NO_VALUE;
}

static bool valid_index_operation(const PvirInstruction *instruction)
{
    const PvirIndexOperation *operation = &instruction->operation.index;
    size_t lanes;

    if (!valid_plain_vector_facts(instruction, &operation->type,
                                  &operation->predicate, &lanes) ||
        operation->type.kind != PVIR_ELEMENT_INTEGER ||
        !valid_inactive_policy(operation->inactive_policy,
                               operation->merge_value, false) ||
        operation->destination == PVIR_NO_VALUE ||
        operation->start_value == PVIR_NO_VALUE ||
        instruction->effects.arch_state.present ||
        instruction->effects.fp_exceptions != PVIR_FP_EXCEPTIONS_NONE) {
        return false;
    }
    /* step_value == PVIR_NO_VALUE means the constant 1. */
    return true;
}

PvirStatus pvir_instruction_validate(const PvirInstruction *instruction)
{
    size_t lanes;
    uint8_t expected_arity;
    const PvirVectorOperation *operation;
    const PvirArchStateEffect *state_effect;
    bool has_float_elements;
    PvirStatus status;

    if (instruction == NULL ||
        !valid_effect_metadata(&instruction->effects)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (instruction->opcode == PVIR_OP_ARCH_STATE) {
        if (instruction->effects.memory.access != PVIR_MEMORY_NONE ||
            instruction->effects.fault.behavior != PVIR_FAULT_NONE ||
            instruction->effects.side_effects != 0 ||
            instruction->effects.fp_exceptions !=
                PVIR_FP_EXCEPTIONS_NONE ||
            !valid_arch_state_operation(instruction)) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        return PVIR_STATUS_OK;
    }

    if (instruction->opcode == PVIR_OP_PREDICATE_GENERATE) {
        return valid_predicate_generator(instruction)
                   ? PVIR_STATUS_OK
                   : PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (instruction->opcode == PVIR_OP_EXTRACT ||
        instruction->opcode == PVIR_OP_INSERT) {
        return valid_lane_operation(instruction)
                   ? PVIR_STATUS_OK
                   : PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (instruction->opcode == PVIR_OP_CONVERT) {
        return valid_convert_operation(instruction)
                   ? PVIR_STATUS_OK
                   : PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (instruction->opcode == PVIR_OP_FIXED) {
        return valid_fixed_operation(instruction)
                   ? PVIR_STATUS_OK
                   : PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (instruction->opcode == PVIR_OP_FP_ARITH) {
        return valid_fp_operation(instruction)
                   ? PVIR_STATUS_OK
                   : PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (instruction->opcode == PVIR_OP_LANE_INDEX) {
        return valid_index_operation(instruction)
                   ? PVIR_STATUS_OK
                   : PVIR_STATUS_INVALID_ARGUMENT;
    }

    if (!vector_opcode_arity(instruction->opcode, &expected_arity)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    operation = &instruction->operation.vector;
    status = pvir_vector_type_lanes(operation->type, &lanes);
    if (status != PVIR_STATUS_OK || operation->predicate.lanes != lanes ||
        pvir_predicate_validate(&operation->predicate) != PVIR_STATUS_OK ||
        operation->operand_count != expected_arity ||
        !valid_operation_operands(instruction->opcode, operation, lanes)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (instruction->opcode == PVIR_OP_LOAD ||
        instruction->opcode == PVIR_OP_STORE) {
        if (!valid_memory_address(&operation->memory_address) ||
            !valid_element_bits(operation->memory_element_bits)) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        if (operation->type.kind == PVIR_ELEMENT_MASK) {
            /* Packed mask bytes (RVV vlm.v/vsm.v): one bit per lane, eight
             * lanes per byte, lane i maps to bit i%8 of byte i/8. */
            if (operation->memory_element_bits != 8) {
                return PVIR_STATUS_INVALID_ARGUMENT;
            }
        } else if (operation->memory_element_bits >
                       operation->type.element_bits ||
                   operation->type.element_bits %
                           operation->memory_element_bits != 0) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
    } else if (operation->memory_address.present ||
               operation->memory_element_bits != 0) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    has_float_elements = instruction_has_float_elements(operation);

    if (instruction->opcode == PVIR_OP_STORE) {
        if (operation->destination != PVIR_NO_VALUE ||
            instruction->effects.memory.access != PVIR_MEMORY_WRITE) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
    } else if (operation->destination == PVIR_NO_VALUE) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }

    if (instruction->opcode == PVIR_OP_LOAD) {
        if (instruction->effects.memory.access != PVIR_MEMORY_READ) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
    } else if (instruction->opcode != PVIR_OP_STORE &&
               instruction->effects.memory.access != PVIR_MEMORY_NONE) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }

    switch (operation->inactive_policy) {
    case PVIR_INACTIVE_NOT_APPLICABLE:
        if (instruction->opcode != PVIR_OP_STORE ||
            operation->merge_value != PVIR_NO_VALUE) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        break;
    case PVIR_INACTIVE_MERGE:
        if (instruction->opcode == PVIR_OP_STORE ||
            operation->merge_value == PVIR_NO_VALUE) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        break;
    case PVIR_INACTIVE_ZERO:
    case PVIR_INACTIVE_UNKNOWN:
        if (instruction->opcode == PVIR_OP_STORE ||
            operation->merge_value != PVIR_NO_VALUE) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        break;
    default:
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    /*
     * Vector loads may use:
     *   ZERO    - SVE predicated-load zeroing semantics;
     *   MERGE   - RVV vma=0 (undisturbed) masked loads;
     *   UNKNOWN - RVV vma=1 (agnostic) masked loads.
     * NOT_APPLICABLE remains rejected above because only STORE accepts it.
     */

    if (instruction->effects.fp_exceptions ==
            PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY &&
        (!has_float_elements ||
         !opcode_can_raise_fp(instruction->opcode))) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (has_float_elements && opcode_can_raise_fp(instruction->opcode) &&
        instruction->effects.fp_exceptions !=
            PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }

    state_effect = &instruction->effects.arch_state;
    if (state_effect->present &&
        !valid_state_operation_values(state_effect->action,
                                      state_effect->update_kind,
                                      &operation->arch_state)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    if (instruction->effects.fault.behavior == PVIR_FAULT_FIRST_FAULT ||
        instruction->effects.fault.behavior == PVIR_FAULT_NONFAULTING) {
        if (instruction->opcode != PVIR_OP_LOAD || !state_effect->present ||
            state_effect->domain != PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT ||
            state_effect->action != PVIR_ARCH_STATE_UPDATE ||
            state_effect->update_kind !=
                PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS ||
            operation->arch_state.output_value == PVIR_NO_VALUE) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
    }
    return PVIR_STATUS_OK;
}

static bool same_source(PvirPredicateSource left, PvirPredicateSource right)
{
    return left.kind == right.kind && left.id == right.id &&
           left.version == right.version;
}

static bool expanded_mask_buffer_range(
    const PvirExpandedMaskCacheEntry *entry, uintptr_t *start,
    uintptr_t *end)
{
    uintmax_t byte_count;
    uintptr_t base;
    uintptr_t bytes;

    if (entry->capacity == 0) {
        *start = 0;
        *end = 0;
        return true;
    }
    if (entry->expanded_lanes == NULL ||
        (uintmax_t)entry->capacity >
            (uintmax_t)UINTPTR_MAX / sizeof(*entry->expanded_lanes)) {
        return false;
    }

    byte_count = (uintmax_t)entry->capacity *
                 sizeof(*entry->expanded_lanes);
    bytes = (uintptr_t)byte_count;
    base = (uintptr_t)entry->expanded_lanes;
    if (bytes > UINTPTR_MAX - base) {
        return false;
    }
    *start = base;
    *end = base + bytes;
    return true;
}

PvirStatus pvir_expanded_mask_cache_init(PvirExpandedMaskCache *cache,
                                         PvirExpandedMaskCacheEntry *entries,
                                         size_t entry_count)
{
    size_t i;
    size_t j;
    uintptr_t start;
    uintptr_t end;
    uintptr_t other_start;
    uintptr_t other_end;

    if (cache == NULL || (entry_count != 0 && entries == NULL)) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    for (i = 0; i < entry_count; i++) {
        if (!expanded_mask_buffer_range(&entries[i], &start, &end)) {
            return PVIR_STATUS_INVALID_ARGUMENT;
        }
        if (entries[i].capacity == 0) {
            continue;
        }
        for (j = 0; j < i; j++) {
            if (!expanded_mask_buffer_range(&entries[j], &other_start,
                                            &other_end) ||
                (start < other_end && other_start < end)) {
                return PVIR_STATUS_INVALID_ARGUMENT;
            }
        }
    }

    for (i = 0; i < entry_count; i++) {
        entries[i].valid = false;
        entries[i].source = (PvirPredicateSource){0};
        entries[i].predicate_form = PVIR_PREDICATE_ALL_TRUE;
        entries[i].active_lanes = 0;
        entries[i].element_bits = 0;
        entries[i].lanes = 0;
        entries[i].first_lane = 0;
    }
    cache->entries = entries;
    cache->count = entry_count;
    cache->next_victim = 0;
    return PVIR_STATUS_OK;
}

void pvir_expanded_mask_cache_invalidate_source(PvirExpandedMaskCache *cache,
                                                PvirPredicateSourceKind kind,
                                                uint32_t id)
{
    size_t i;

    if (cache == NULL || cache->entries == NULL ||
        !valid_predicate_source((PvirPredicateSource){kind, id, 0})) {
        return;
    }
    for (i = 0; i < cache->count; i++) {
        if (cache->entries[i].valid && cache->entries[i].source.kind == kind &&
            cache->entries[i].source.id == id) {
            cache->entries[i].valid = false;
        }
    }
}

void pvir_expanded_mask_cache_invalidate_all(PvirExpandedMaskCache *cache)
{
    size_t i;

    if (cache == NULL) {
        return;
    }
    for (i = 0; i < cache->count; i++) {
        cache->entries[i].valid = false;
    }
    cache->next_victim = 0;
}

PvirStatus pvir_expanded_mask_cache_get(PvirExpandedMaskCache *cache,
                                        const PvirPredicate *predicate,
                                        uint16_t element_bits,
                                        const uint64_t *source_mask_words,
                                        size_t source_mask_word_count,
                                        const uint64_t **expanded_lanes)
{
    size_t i;
    size_t candidate = SIZE_MAX;
    size_t cursor;
    PvirStatus status;
    PvirExpandedMaskCacheEntry *entry;

    if (cache == NULL || expanded_lanes == NULL ||
        !valid_predicate_element_bits(element_bits) ||
        pvir_predicate_validate(predicate) != PVIR_STATUS_OK) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }
    *expanded_lanes = NULL;
    for (i = 0; i < cache->count; i++) {
        entry = &cache->entries[i];
        if (entry->valid && same_source(entry->source, predicate->source) &&
            entry->predicate_form == predicate->form &&
            entry->active_lanes ==
                ((predicate->form == PVIR_PREDICATE_PREFIX ||
                  predicate->form == PVIR_PREDICATE_SUFFIX)
                     ? predicate->value.active_lanes
                     : 0) &&
            entry->element_bits == element_bits &&
            entry->lanes == predicate->lanes &&
            entry->first_lane == predicate->first_lane) {
            *expanded_lanes = entry->expanded_lanes;
            return PVIR_STATUS_OK;
        }
    }

    for (i = 0; i < cache->count; i++) {
        entry = &cache->entries[i];
        if (!entry->valid && entry->capacity >= predicate->lanes) {
            candidate = i;
            break;
        }
    }
    if (candidate == SIZE_MAX && cache->count != 0) {
        cursor = cache->next_victim < cache->count ? cache->next_victim : 0;
        for (i = 0; i < cache->count; i++) {
            entry = &cache->entries[cursor];
            if (entry->capacity >= predicate->lanes) {
                candidate = cursor;
                break;
            }
            cursor++;
            if (cursor == cache->count) {
                cursor = 0;
            }
        }
    }
    if (candidate == SIZE_MAX) {
        return PVIR_STATUS_INSUFFICIENT_CAPACITY;
    }

    entry = &cache->entries[candidate];
    status = pvir_predicate_expand(predicate, element_bits,
                                   source_mask_words,
                                   source_mask_word_count,
                                   entry->expanded_lanes, entry->capacity);
    if (status != PVIR_STATUS_OK) {
        return status;
    }
    entry->source = predicate->source;
    entry->predicate_form = predicate->form;
    entry->active_lanes =
        (predicate->form == PVIR_PREDICATE_PREFIX ||
         predicate->form == PVIR_PREDICATE_SUFFIX)
            ? predicate->value.active_lanes
            : 0;
    entry->element_bits = element_bits;
    entry->lanes = predicate->lanes;
    entry->first_lane = predicate->first_lane;
    entry->valid = true;
    cache->next_victim = candidate + 1;
    if (cache->next_victim == cache->count) {
        cache->next_victim = 0;
    }
    *expanded_lanes = entry->expanded_lanes;
    return PVIR_STATUS_OK;
}
