#include "la-pvir.h"
#include "pvir-plan.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(condition)                                                   \
    do {                                                                    \
        checks++;                                                           \
        if (!(condition)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static PvirPredicateSource source(PvirPredicateSourceKind kind, uint32_t id,
                                  uint64_t version)
{
    PvirPredicateSource result = {kind, id, version};
    return result;
}


static void test_shape_and_canonical_forms(void)
{
    const size_t lanes = 70;
    uint64_t prefix_words[2] = {UINT64_MAX, (UINT64_MAX << 6) | UINT64_C(1)};
    uint64_t suffix_words[2] = {0, UINT64_C(0x38)};
    uint64_t all_words[2] = {UINT64_MAX, UINT64_C(0x3f)};
    uint64_t zero_words[2] = {0, 0};
    uint64_t arbitrary_words[2] = {UINT64_C(1), UINT64_C(1) << 1};
    PvirPredicateSource predicate_source =
        source(PVIR_PREDICATE_SOURCE_PREDICATE, 3, 9);
    PvirPredicate predicate;
    PvirVectorType type = {
        PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED, 8, lanes * 8
    };
    size_t actual_lanes = 0;

    CHECK(pvir_vector_type_lanes(type, &actual_lanes) == PVIR_STATUS_OK);
    CHECK(actual_lanes == lanes);
    type.vector_bits++;
    CHECK(pvir_vector_type_lanes(type, &actual_lanes) ==
          PVIR_STATUS_INVALID_ARGUMENT);

    type = (PvirVectorType){
        PVIR_ELEMENT_MASK, PVIR_SIGNLESS, 1, lanes
    };
    CHECK(pvir_vector_type_lanes(type, &actual_lanes) == PVIR_STATUS_OK);
    CHECK(actual_lanes == lanes);
    type.element_bits = 8;
    CHECK(pvir_vector_type_lanes(type, &actual_lanes) ==
          PVIR_STATUS_INVALID_ARGUMENT);

    /* Padding bits above lane 69 are ignored during canonicalization. */
    predicate = pvir_predicate_bitset(predicate_source, lanes,
                                      prefix_words, 2);
    CHECK(pvir_predicate_canonicalize(&predicate) == PVIR_STATUS_OK);
    CHECK(predicate.form == PVIR_PREDICATE_PREFIX);
    CHECK(predicate.value.active_lanes == 65);
    CHECK(predicate.source.version == 9);

    predicate = pvir_predicate_bitset(predicate_source, lanes,
                                      suffix_words, 2);
    CHECK(pvir_predicate_canonicalize(&predicate) == PVIR_STATUS_OK);
    CHECK(predicate.form == PVIR_PREDICATE_SUFFIX);
    CHECK(predicate.value.active_lanes == 3);

    predicate = pvir_predicate_bitset(predicate_source, lanes, all_words, 2);
    CHECK(pvir_predicate_canonicalize(&predicate) == PVIR_STATUS_OK);
    CHECK(predicate.form == PVIR_PREDICATE_ALL_TRUE);

    predicate = pvir_predicate_bitset(predicate_source, lanes, zero_words, 2);
    CHECK(pvir_predicate_canonicalize(&predicate) == PVIR_STATUS_OK);
    CHECK(predicate.form == PVIR_PREDICATE_ALL_FALSE);

    predicate = pvir_predicate_bitset(predicate_source, lanes,
                                      arbitrary_words, 2);
    CHECK(pvir_predicate_canonicalize(&predicate) == PVIR_STATUS_OK);
    CHECK(predicate.form == PVIR_PREDICATE_BITSET);

    predicate = pvir_predicate_prefix(predicate_source, lanes, 0);
    CHECK(pvir_predicate_canonicalize(&predicate) == PVIR_STATUS_OK);
    CHECK(predicate.form == PVIR_PREDICATE_ALL_FALSE);
    predicate = pvir_predicate_suffix(predicate_source, lanes, lanes);
    CHECK(pvir_predicate_canonicalize(&predicate) == PVIR_STATUS_OK);
    CHECK(predicate.form == PVIR_PREDICATE_ALL_TRUE);

    predicate = pvir_predicate_vector_mask(
        source(PVIR_PREDICATE_SOURCE_VECTOR, 4, 1), lanes);
    CHECK(pvir_predicate_canonicalize(&predicate) == PVIR_STATUS_OK);
    CHECK(predicate.form == PVIR_PREDICATE_VECTOR_MASK);
}

static void test_expanded_mask_cache(void)
{
    const size_t lanes = 70;
    uint64_t first_mask[2] = {UINT64_C(1), UINT64_C(1) << 5};
    uint64_t second_mask[2] = {UINT64_C(1) << 1, 0};
    uint64_t third_mask[2] = {UINT64_C(1) << 2, 0};
    uint64_t storage0[lanes];
    uint64_t storage1[lanes];
    uint64_t storage2[lanes];
    PvirExpandedMaskCacheEntry entries[3] = {
        {.expanded_lanes = storage0, .capacity = lanes},
        {.expanded_lanes = storage1, .capacity = lanes},
        {.expanded_lanes = storage2, .capacity = lanes},
    };
    PvirExpandedMaskCache cache;
    PvirPredicate predicate;
    PvirPredicateSource mask_source =
        source(PVIR_PREDICATE_SOURCE_VECTOR, 12, 5);
    const uint64_t *expanded;
    const uint64_t *first_entry;
    size_t lane;

    CHECK(pvir_expanded_mask_cache_init(&cache, entries, 3) ==
          PVIR_STATUS_OK);
    predicate = pvir_predicate_vector_mask(mask_source, lanes);
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8, first_mask, 2,
                                       &expanded) == PVIR_STATUS_OK);
    first_entry = expanded;
    CHECK(expanded[0] == UINT8_MAX);
    CHECK(expanded[69] == UINT8_MAX);
    CHECK(expanded[1] == 0);

    /* A matching source/version/SEW is served without resolving the source. */
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8, NULL, 0,
                                       &expanded) == PVIR_STATUS_OK);
    CHECK(expanded == first_entry);
    CHECK(expanded[69] == UINT8_MAX);

    /* A new source version must not reuse the old expanded value. */
    predicate.source.version++;
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8, second_mask, 2,
                                       &expanded) == PVIR_STATUS_OK);
    CHECK(expanded != first_entry);
    CHECK(expanded[1] == UINT8_MAX);
    CHECK(expanded[0] == 0);

    /* SEW is a distinct cache key and changes the expanded lane value. */
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 16, second_mask, 2,
                                       &expanded) == PVIR_STATUS_OK);
    CHECK(expanded[1] == UINT16_MAX);

    pvir_expanded_mask_cache_invalidate_source(
        &cache, PVIR_PREDICATE_SOURCE_VECTOR, 12);
    for (lane = 0; lane < 3; lane++) {
        CHECK(!entries[lane].valid);
    }
    predicate.source.version = 5;
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8, third_mask, 2,
                                       &expanded) == PVIR_STATUS_OK);
    CHECK(expanded[2] == UINT8_MAX);
    CHECK(expanded[0] == 0);

    pvir_expanded_mask_cache_invalidate_all(&cache);
    CHECK(!entries[0].valid && !entries[1].valid && !entries[2].valid);
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8, NULL, 0,
                                       &expanded) ==
          PVIR_STATUS_NEEDS_VECTOR_MASK);
}

static void test_cache_keys_preserve_canonical_values(void)
{
    const size_t lanes = 8;
    uint64_t buffers[6][lanes];
    PvirExpandedMaskCacheEntry entries[6] = {
        {.expanded_lanes = buffers[0], .capacity = lanes},
        {.expanded_lanes = buffers[1], .capacity = lanes},
        {.expanded_lanes = buffers[2], .capacity = lanes},
        {.expanded_lanes = buffers[3], .capacity = lanes},
        {.expanded_lanes = buffers[4], .capacity = lanes},
        {.expanded_lanes = buffers[5], .capacity = lanes},
    };
    PvirExpandedMaskCache cache;
    PvirPredicateSource shared_source =
        source(PVIR_PREDICATE_SOURCE_PREDICATE, 19, 7);
    PvirPredicate predicate;
    const uint64_t *all_true;
    const uint64_t *all_false;
    const uint64_t *prefix_two;
    const uint64_t *prefix_three;
    const uint64_t *suffix_two;
    const uint64_t *suffix_three;
    size_t lane;

    CHECK(pvir_expanded_mask_cache_init(&cache, entries, 6) ==
          PVIR_STATUS_OK);
    predicate = pvir_predicate_all_true(lanes);
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8, NULL, 0,
                                       &all_true) == PVIR_STATUS_OK);
    predicate = pvir_predicate_all_false(lanes);
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8, NULL, 0,
                                       &all_false) == PVIR_STATUS_OK);
    CHECK(all_true != all_false);
    CHECK(all_true[0] == UINT8_MAX && all_true[7] == UINT8_MAX);
    CHECK(all_false[0] == 0 && all_false[7] == 0);

    predicate = pvir_predicate_prefix(shared_source, lanes, 2);
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8, NULL, 0,
                                       &prefix_two) == PVIR_STATUS_OK);
    predicate = pvir_predicate_prefix(shared_source, lanes, 3);
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8, NULL, 0,
                                       &prefix_three) == PVIR_STATUS_OK);
    CHECK(prefix_two != prefix_three);
    CHECK(prefix_two[1] == UINT8_MAX && prefix_two[2] == 0);
    CHECK(prefix_three[2] == UINT8_MAX && prefix_three[3] == 0);

    predicate = pvir_predicate_suffix(shared_source, lanes, 2);
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8, NULL, 0,
                                       &suffix_two) == PVIR_STATUS_OK);
    predicate = pvir_predicate_suffix(shared_source, lanes, 3);
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8, NULL, 0,
                                       &suffix_three) == PVIR_STATUS_OK);
    CHECK(suffix_two != suffix_three);
    CHECK(suffix_two[5] == 0 && suffix_two[6] == UINT8_MAX);
    CHECK(suffix_three[4] == 0 && suffix_three[5] == UINT8_MAX);
    for (lane = 0; lane < 6; lane++) {
        CHECK(entries[lane].valid);
    }
}

static void test_cache_init_rejects_bad_storage_ranges(void)
{
    uint64_t storage[8];
    PvirExpandedMaskCacheEntry entries[2] = {
        {.expanded_lanes = storage, .capacity = 4},
        {.expanded_lanes = storage + 4, .capacity = 4},
    };
    PvirExpandedMaskCache cache;

    CHECK(pvir_expanded_mask_cache_init(&cache, entries, 2) ==
          PVIR_STATUS_OK);
    entries[1].expanded_lanes = storage + 3;
    CHECK(pvir_expanded_mask_cache_init(&cache, entries, 2) ==
          PVIR_STATUS_INVALID_ARGUMENT);
    CHECK(cache.entries == entries && cache.count == 2);

#if defined(UINTPTR_MAX) && defined(SIZE_MAX)
    {
        uintptr_t base = (uintptr_t)storage;
        uintmax_t capacity =
            ((uintmax_t)UINTPTR_MAX - (uintmax_t)base) /
                sizeof(*storage) +
            1;

        if (base != 0 &&
            capacity <= (uintmax_t)UINTPTR_MAX / sizeof(*storage) &&
            capacity <= (uintmax_t)SIZE_MAX) {
            PvirExpandedMaskCacheEntry overflow_entry = {
                .expanded_lanes = storage,
                .capacity = (size_t)capacity,
            };
            CHECK(pvir_expanded_mask_cache_init(&cache, &overflow_entry, 1) ==
                  PVIR_STATUS_INVALID_ARGUMENT);
        }
    }
#endif
}

static PvirInstruction vector_division(PvirInactivePolicy policy)
{
    PvirInstruction instruction = {0};
    PvirVectorOperation *operation = &instruction.operation.vector;
    PvirPredicateSource predicate_source =
        source(PVIR_PREDICATE_SOURCE_PREDICATE, 2, 4);

    instruction.opcode = PVIR_OP_DIV;
    instruction.effects.fault.behavior = PVIR_FAULT_PRECISE;
    instruction.effects.fault.lane_order = PVIR_FAULT_ORDER_UNSPECIFIED;
    instruction.effects.fault.unknown_from_lane = PVIR_NO_VALUE;
    operation->type = (PvirVectorType){
        PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED, 8, 256
    };
    operation->predicate = pvir_predicate_prefix(predicate_source, 32, 5);
    operation->inactive_policy = policy;
    operation->destination = 10;
    operation->operands[0] = 11;
    operation->operands[1] = 12;
    operation->operand_count = 2;
    operation->merge_value =
        policy == PVIR_INACTIVE_MERGE ? 13 : PVIR_NO_VALUE;
    return instruction;
}

static PvirInstruction memory_load(void)
{
    PvirInstruction instruction = {0};
    PvirVectorOperation *operation = &instruction.operation.vector;
    PvirPredicateSource predicate_source =
        source(PVIR_PREDICATE_SOURCE_PREDICATE, 2, 4);

    instruction.opcode = PVIR_OP_LOAD;
    instruction.effects.memory.access = PVIR_MEMORY_READ;
    instruction.effects.memory.ordering = PVIR_MEMORY_ORDER_RELAXED;
    instruction.effects.memory.inactive_lanes =
        PVIR_INACTIVE_MEMORY_NO_TRANSACTION;
    instruction.effects.fault.behavior = PVIR_FAULT_PRECISE;
    instruction.effects.fault.lane_order = PVIR_FAULT_ORDER_UNSPECIFIED;
    instruction.effects.fault.unknown_from_lane = PVIR_NO_VALUE;
    instruction.effects.side_effects =
        PVIR_SIDE_EFFECT_IO | PVIR_SIDE_EFFECT_VOLATILE;
    operation->type = (PvirVectorType){
        PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED, 8, 256
    };
    operation->predicate = pvir_predicate_prefix(predicate_source, 32, 5);
    operation->inactive_policy = PVIR_INACTIVE_ZERO;
    operation->destination = 10;
    operation->operand_count = 0;
    operation->merge_value = PVIR_NO_VALUE;
    operation->memory_address = (PvirMemoryAddress){
        .present = true,
        .base_kind = PVIR_MEMORY_BASE_VALUE,
        .base_value = 11,
        .displacement = 0,
    };
    operation->memory_element_bits = 8;
    return instruction;
}

static void test_instruction_effects_and_inactive_policy(void)
{
    PvirInstruction merge = vector_division(PVIR_INACTIVE_MERGE);
    PvirInstruction zero = vector_division(PVIR_INACTIVE_ZERO);
    PvirInstruction unknown = vector_division(PVIR_INACTIVE_UNKNOWN);
    PvirInstruction load = memory_load();
    PvirInstruction first_fault = load;
    PvirInstruction nonfaulting = load;
    PvirInstruction store = {0};
    PvirInstruction fp = {0};
    PvirInstruction state = {0};
    PvirPredicateSource predicate_source =
        source(PVIR_PREDICATE_SOURCE_PREDICATE, 6, 2);
    store.effects.fault.unknown_from_lane = PVIR_NO_VALUE;
    fp.effects.fault.unknown_from_lane = PVIR_NO_VALUE;
    state.effects.fault.unknown_from_lane = PVIR_NO_VALUE;

    CHECK(pvir_instruction_validate(&merge) == PVIR_STATUS_OK);
    CHECK(merge.operation.vector.inactive_policy == PVIR_INACTIVE_MERGE);
    CHECK(merge.operation.vector.merge_value == 13);
    CHECK(pvir_instruction_validate(&zero) == PVIR_STATUS_OK);
    CHECK(zero.operation.vector.inactive_policy == PVIR_INACTIVE_ZERO);
    CHECK(zero.operation.vector.merge_value == PVIR_NO_VALUE);
    CHECK(merge.effects.fault.behavior == PVIR_FAULT_PRECISE);
    CHECK(merge.effects.fault.lane_order ==
          PVIR_FAULT_ORDER_UNSPECIFIED);
    CHECK(zero.effects.fault.behavior == merge.effects.fault.behavior);
    CHECK(zero.effects.fault.lane_order == merge.effects.fault.lane_order);
    CHECK(pvir_instruction_validate(&unknown) == PVIR_STATUS_OK);
    CHECK(unknown.operation.vector.inactive_policy == PVIR_INACTIVE_UNKNOWN);

    /* Precise faults need not promise ascending lane order. */
    CHECK(pvir_instruction_validate(&load) == PVIR_STATUS_OK);

    {
        PvirInstruction missing_address = load;
        PvirInstruction invalid_memory_width = load;

        missing_address.operation.vector.memory_address.present = false;
        CHECK(pvir_instruction_validate(&missing_address) ==
              PVIR_STATUS_INVALID_ARGUMENT);
        invalid_memory_width.operation.vector.memory_element_bits = 4;
        CHECK(pvir_instruction_validate(&invalid_memory_width) ==
              PVIR_STATUS_INVALID_ARGUMENT);
    }
    CHECK(load.operation.vector.inactive_policy == PVIR_INACTIVE_ZERO);
    CHECK(load.effects.memory.inactive_lanes ==
          PVIR_INACTIVE_MEMORY_NO_TRANSACTION);
    CHECK(load.effects.fault.behavior == PVIR_FAULT_PRECISE);
    CHECK(load.effects.fault.lane_order == PVIR_FAULT_ORDER_UNSPECIFIED);
    CHECK(load.effects.side_effects ==
          (PVIR_SIDE_EFFECT_IO | PVIR_SIDE_EFFECT_VOLATILE));
    load.effects.memory.inactive_lanes = PVIR_INACTIVE_MEMORY_SUPPRESSED;
    CHECK(pvir_instruction_validate(&load) == PVIR_STATUS_INVALID_ARGUMENT);

    /*
     * RVV masked loads: vma=0 is undisturbed (MERGE the old destination),
     * vma=1 is agnostic (UNKNOWN).  ZERO stays valid for SVE.
     */
    {
        PvirInstruction load_merge = memory_load();
        PvirInstruction load_unknown = memory_load();
        PvirInstruction load_not_applicable = memory_load();

        load_merge.operation.vector.inactive_policy = PVIR_INACTIVE_MERGE;
        load_merge.operation.vector.merge_value = 13;
        CHECK(pvir_instruction_validate(&load_merge) == PVIR_STATUS_OK);

        load_merge.operation.vector.merge_value = PVIR_NO_VALUE;
        CHECK(pvir_instruction_validate(&load_merge) ==
              PVIR_STATUS_INVALID_ARGUMENT);

        load_unknown.operation.vector.inactive_policy = PVIR_INACTIVE_UNKNOWN;
        load_unknown.operation.vector.merge_value = PVIR_NO_VALUE;
        CHECK(pvir_instruction_validate(&load_unknown) == PVIR_STATUS_OK);

        load_not_applicable.operation.vector.inactive_policy =
            PVIR_INACTIVE_NOT_APPLICABLE;
        load_not_applicable.operation.vector.merge_value = PVIR_NO_VALUE;
        CHECK(pvir_instruction_validate(&load_not_applicable) ==
              PVIR_STATUS_INVALID_ARGUMENT);
    }

    first_fault.effects.fault.behavior = PVIR_FAULT_FIRST_FAULT;
    first_fault.effects.fault.lane_order =
        PVIR_FAULT_ORDER_LANE_ASCENDING;
    first_fault.effects.fault.fault_result = PVIR_FAULT_RESULT_UNKNOWN;
    first_fault.effects.fault.fault_value = PVIR_NO_VALUE;
    first_fault.effects.fault.unknown_from_lane = PVIR_NO_VALUE;
    first_fault.effects.arch_state = (PvirArchStateEffect){
        .present = true,
        .domain = PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT,
        .action = PVIR_ARCH_STATE_UPDATE,
        .update_kind = PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS,
        .state_id = 7,
        .version_before = 14,
        .version_after = 15,
    };
    first_fault.operation.vector.arch_state = (PvirArchStateOperation){
        .input_value = 20,
        .output_value = 21,
        .clear_from_lane = 22,
    };
    CHECK(pvir_instruction_validate(&first_fault) ==
          PVIR_STATUS_INVALID_ARGUMENT);
    first_fault.effects.fault.unknown_from_lane = 30;
    CHECK(pvir_instruction_validate(&first_fault) == PVIR_STATUS_OK);
    CHECK(first_fault.effects.fault.fault_result ==
          PVIR_FAULT_RESULT_UNKNOWN);
    CHECK(first_fault.effects.fault.unknown_from_lane == 30);
    CHECK(first_fault.operation.vector.arch_state.input_value == 20);
    CHECK(first_fault.operation.vector.arch_state.output_value == 21);
    CHECK(first_fault.operation.vector.arch_state.clear_from_lane == 22);
    CHECK(first_fault.effects.fault.unknown_from_lane !=
          first_fault.operation.vector.arch_state.clear_from_lane);
    first_fault.operation.vector.arch_state.input_value = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&first_fault) ==
          PVIR_STATUS_INVALID_ARGUMENT);
    first_fault.operation.vector.arch_state.input_value = 20;
    first_fault.operation.vector.arch_state.clear_from_lane = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&first_fault) ==
          PVIR_STATUS_INVALID_ARGUMENT);

    nonfaulting.effects.fault.behavior = PVIR_FAULT_NONFAULTING;
    nonfaulting.effects.fault.fault_result = PVIR_FAULT_RESULT_UNKNOWN;
    nonfaulting.effects.fault.fault_value = PVIR_NO_VALUE;
    nonfaulting.effects.fault.unknown_from_lane = 31;
    nonfaulting.effects.arch_state = (PvirArchStateEffect){
        .present = true,
        .domain = PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT,
        .action = PVIR_ARCH_STATE_UPDATE,
        .update_kind = PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS,
        .state_id = 7,
        .version_before = 15,
        .version_after = 16,
    };
    nonfaulting.operation.vector.arch_state = (PvirArchStateOperation){
        .input_value = 21,
        .output_value = 23,
        .clear_from_lane = 24,
    };
    CHECK(pvir_instruction_validate(&nonfaulting) == PVIR_STATUS_OK);
    CHECK(nonfaulting.effects.fault.behavior == PVIR_FAULT_NONFAULTING);
    CHECK(nonfaulting.effects.fault.fault_result ==
          PVIR_FAULT_RESULT_UNKNOWN);
    CHECK(nonfaulting.effects.fault.unknown_from_lane == 31);
    CHECK(nonfaulting.effects.arch_state.update_kind ==
          PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS);
    CHECK(nonfaulting.operation.vector.arch_state.clear_from_lane == 24);
    nonfaulting.effects.fault.unknown_from_lane = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&nonfaulting) ==
          PVIR_STATUS_INVALID_ARGUMENT);
    nonfaulting.effects.fault.unknown_from_lane = 31;
    nonfaulting.effects.fault.fault_result =
        PVIR_FAULT_RESULT_UNSPECIFIED;
    CHECK(pvir_instruction_validate(&nonfaulting) ==
          PVIR_STATUS_INVALID_ARGUMENT);


    store.opcode = PVIR_OP_STORE;
    store.effects.memory.access = PVIR_MEMORY_WRITE;
    store.effects.memory.ordering = PVIR_MEMORY_ORDER_RELAXED;
    store.effects.memory.inactive_lanes =
        PVIR_INACTIVE_MEMORY_NO_TRANSACTION;
    store.operation.vector.type = (PvirVectorType){
        PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED, 8, 256
    };
    store.operation.vector.predicate =
        pvir_predicate_prefix(predicate_source, 32, 5);
    store.operation.vector.inactive_policy = PVIR_INACTIVE_NOT_APPLICABLE;
    store.operation.vector.destination = PVIR_NO_VALUE;
    store.operation.vector.operands[0] = 30;
    store.operation.vector.operand_kinds[0] = PVIR_OPERAND_VECTOR;
    store.operation.vector.operand_count = 1;
    store.operation.vector.memory_address = (PvirMemoryAddress){
        .present = true,
        .base_kind = PVIR_MEMORY_BASE_VALUE,
        .base_value = 31,
        .displacement = 0,
    };
    store.operation.vector.memory_element_bits = 8;
    store.operation.vector.merge_value = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&store) == PVIR_STATUS_OK);
    store.operation.vector.inactive_policy = PVIR_INACTIVE_ZERO;
    CHECK(pvir_instruction_validate(&store) == PVIR_STATUS_INVALID_ARGUMENT);

    fp.opcode = PVIR_OP_ADD;
    fp.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
    fp.operation.vector.type = (PvirVectorType){
        PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, 32, 128
    };
    fp.operation.vector.predicate =
        pvir_predicate_prefix(predicate_source, 4, 2);
    fp.operation.vector.inactive_policy = PVIR_INACTIVE_UNKNOWN;
    fp.operation.vector.destination = 40;
    fp.operation.vector.operands[0] = 41;
    fp.operation.vector.operands[1] = 42;
    fp.operation.vector.operand_count = 2;
    fp.operation.vector.merge_value = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&fp) == PVIR_STATUS_OK);
    fp.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_NONE;
    CHECK(pvir_instruction_validate(&fp) == PVIR_STATUS_INVALID_ARGUMENT);

    state.opcode = PVIR_OP_ARCH_STATE;
    state.effects.arch_state = (PvirArchStateEffect){
        .present = true,
        .domain = PVIR_ARCH_STATE_DOMAIN_RESTART,
        .action = PVIR_ARCH_STATE_UPDATE,
        .update_kind = PVIR_ARCH_STATE_UPDATE_NONE,
        .state_id = 22,
        .version_before = 3,
        .version_after = 4,
    };
    state.operation.arch_state.input_value = 30;
    state.operation.arch_state.output_value = 31;
    state.operation.arch_state.clear_from_lane = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&state) == PVIR_STATUS_OK);
    CHECK(state.effects.arch_state.domain == PVIR_ARCH_STATE_DOMAIN_RESTART);
    state.operation.arch_state.output_value = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&state) == PVIR_STATUS_INVALID_ARGUMENT);
}

static void test_predicate_slices(void)
{
    const size_t lanes = 70;
    const size_t first_lane = 65;
    uint64_t mask_words[3] = {
        0, (UINT64_C(1) << 1) | (UINT64_C(1) << 6),
        UINT64_C(1) << 6
    };
    uint64_t bitset_words[2] = {UINT64_C(1), UINT64_C(1) << 5};
    uint64_t expanded[lanes];
    uint64_t storage0[lanes];
    uint64_t storage1[lanes];
    PvirExpandedMaskCacheEntry entries[2] = {
        {.expanded_lanes = storage0, .capacity = lanes},
        {.expanded_lanes = storage1, .capacity = lanes},
    };
    PvirExpandedMaskCache cache;
    PvirPredicateSource vector_source =
        source(PVIR_PREDICATE_SOURCE_VECTOR, 31, 8);
    PvirPredicate predicate;
    const uint64_t *slice65;
    const uint64_t *slice64;
    const uint64_t *cached;

    predicate = pvir_predicate_vector_mask(vector_source, lanes);
    predicate.first_lane = first_lane;
    CHECK(pvir_predicate_validate(&predicate) == PVIR_STATUS_OK);
    CHECK(pvir_predicate_expand(&predicate, 8, mask_words, 2, expanded,
                                lanes) == PVIR_STATUS_NEEDS_VECTOR_MASK);
    CHECK(pvir_predicate_expand(&predicate, 8, mask_words, 3, expanded,
                                lanes - 1) ==
          PVIR_STATUS_INSUFFICIENT_CAPACITY);
    CHECK(pvir_predicate_expand(&predicate, 8, mask_words, 3, expanded,
                                lanes) == PVIR_STATUS_OK);
    CHECK(expanded[0] == UINT8_MAX);
    CHECK(expanded[5] == UINT8_MAX);
    CHECK(expanded[69] == UINT8_MAX);
    CHECK(expanded[1] == 0);

    predicate = pvir_predicate_bitset(
        source(PVIR_PREDICATE_SOURCE_PREDICATE, 32, 1),
        lanes, bitset_words, 2);
    predicate.first_lane = first_lane;
    CHECK(pvir_predicate_expand(&predicate, 8, NULL, 0, expanded, lanes) ==
          PVIR_STATUS_OK);
    CHECK(expanded[0] == UINT8_MAX);
    CHECK(expanded[5] == 0);
    CHECK(expanded[69] == UINT8_MAX);

    predicate = pvir_predicate_prefix(
        source(PVIR_PREDICATE_SOURCE_PREDICATE, 33, 1), lanes, 3);
    predicate.first_lane = first_lane;
    CHECK(pvir_predicate_expand(&predicate, 8, NULL, 0, expanded, lanes) ==
          PVIR_STATUS_OK);
    CHECK(expanded[0] == UINT8_MAX && expanded[2] == UINT8_MAX);
    CHECK(expanded[3] == 0);

    predicate = pvir_predicate_suffix(
        source(PVIR_PREDICATE_SOURCE_PREDICATE, 34, 1), lanes, 3);
    predicate.first_lane = first_lane;
    CHECK(pvir_predicate_expand(&predicate, 8, NULL, 0, expanded, lanes) ==
          PVIR_STATUS_OK);
    CHECK(expanded[66] == 0);
    CHECK(expanded[67] == UINT8_MAX && expanded[69] == UINT8_MAX);

    predicate = pvir_predicate_prefix(vector_source, 4, 2);
    predicate.first_lane = SIZE_MAX - 1;
    CHECK(pvir_predicate_validate(&predicate) ==
          PVIR_STATUS_INVALID_PREDICATE);

    CHECK(pvir_expanded_mask_cache_init(&cache, entries, 2) ==
          PVIR_STATUS_OK);
    predicate = pvir_predicate_vector_mask(vector_source, lanes);
    predicate.first_lane = first_lane;
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8, mask_words, 3,
                                       &slice65) == PVIR_STATUS_OK);
    predicate.first_lane = first_lane - 1;
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8, mask_words, 3,
                                       &slice64) == PVIR_STATUS_OK);
    CHECK(slice65 != slice64);
    CHECK(slice65[0] == UINT8_MAX && slice65[1] == 0);
    CHECK(slice64[0] == 0 && slice64[1] == UINT8_MAX);
    CHECK(entries[0].first_lane == first_lane);
    CHECK(entries[1].first_lane == first_lane - 1);
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8, NULL, 0,
                                       &cached) == PVIR_STATUS_OK);
    CHECK(cached == slice64);
}

static void test_one_bit_predicate_expansion(void)
{
    const size_t lanes = 8;
    uint64_t expanded[lanes];
    uint64_t cached_storage[lanes];
    PvirExpandedMaskCacheEntry entry = {
        .expanded_lanes = cached_storage,
        .capacity = lanes,
    };
    PvirExpandedMaskCache cache;
    PvirPredicate predicate = pvir_predicate_prefix(
        source(PVIR_PREDICATE_SOURCE_PREDICATE, 71, 3), lanes, 3);
    const uint64_t *cached;
    size_t lane;
    size_t type_lanes;

    CHECK(pvir_vector_type_lanes(
              (PvirVectorType){PVIR_ELEMENT_BITS, PVIR_SIGNLESS, 1, 8},
              &type_lanes) == PVIR_STATUS_INVALID_ARGUMENT);
    CHECK(pvir_predicate_expand(&predicate, 1, NULL, 0, expanded, lanes) ==
          PVIR_STATUS_OK);
    for (lane = 0; lane < lanes; lane++) {
        CHECK(expanded[lane] == (lane < 3 ? 1 : 0));
    }

    CHECK(pvir_expanded_mask_cache_init(&cache, &entry, 1) ==
          PVIR_STATUS_OK);
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 1, NULL, 0,
                                       &cached) == PVIR_STATUS_OK);
    CHECK(cached == cached_storage);
    for (lane = 0; lane < lanes; lane++) {
        CHECK(cached[lane] == (lane < 3 ? 1 : 0));
    }
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 1, NULL, 0,
                                       &cached) == PVIR_STATUS_OK);
    CHECK(cached == cached_storage);
}

static void test_typed_operands_and_compare(void)
{
    static const PvirCompareCondition signed_conditions[] = {
        PVIR_COMPARE_LT, PVIR_COMPARE_LE,
        PVIR_COMPARE_GT, PVIR_COMPARE_GE
    };
    static const PvirCompareCondition unsigned_conditions[] = {
        PVIR_COMPARE_LTU, PVIR_COMPARE_LEU,
        PVIR_COMPARE_GTU, PVIR_COMPARE_GEU
    };
    static const PvirCompareCondition float_conditions[] = {
        PVIR_COMPARE_EQ, PVIR_COMPARE_NE, PVIR_COMPARE_LT,
        PVIR_COMPARE_LE, PVIR_COMPARE_GT, PVIR_COMPARE_GE
    };
    PvirInstruction vector = vector_division(PVIR_INACTIVE_ZERO);
    PvirVectorOperation *operation = &vector.operation.vector;
    PvirInstruction compare = {0};
    PvirVectorOperation *compare_operation =
        &compare.operation.vector;
    PvirVectorType compare_inputs = {
        PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED, 8, 256
    };
    size_t i;

    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_OK);
    CHECK(operation->operand_types[0].element_bits == 0);
    operation->operand_types[0].kind = PVIR_ELEMENT_FLOAT;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_OK);
    operation->operand_types[0] = (PvirVectorType){0};
    operation->operand_types[0] =
        (PvirVectorType){PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED, 16, 512};
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_OK);
    operation->operand_types[0].vector_bits = 513;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_INVALID_ARGUMENT);
    operation->operand_types[0].vector_bits = 256;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_INVALID_ARGUMENT);
    operation->operand_types[0] = (PvirVectorType){0};

    operation->operands[0] = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_INVALID_ARGUMENT);
    operation->operands[0] = 11;
    operation->operand_immediates[0] = 1;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_INVALID_ARGUMENT);
    operation->operand_immediates[0] = 0;

    operation->operand_kinds[1] = PVIR_OPERAND_SCALAR;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_OK);
    operation->operand_types[1] =
        (PvirVectorType){PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED, 8, 256};
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_INVALID_ARGUMENT);
    operation->operand_types[1] = (PvirVectorType){0};
    operation->operands[1] = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_INVALID_ARGUMENT);
    operation->operands[1] = 12;
    operation->operand_immediates[1] = 1;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_INVALID_ARGUMENT);
    operation->operand_immediates[1] = 0;

    vector.opcode = PVIR_OP_ADD;
    operation->operand_kinds[1] = PVIR_OPERAND_IMMEDIATE;
    operation->operands[1] = PVIR_NO_VALUE;
    operation->operand_immediates[1] = UINT8_MAX;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_OK);
    operation->operands[1] = 12;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_INVALID_ARGUMENT);
    operation->operands[1] = PVIR_NO_VALUE;
    operation->operand_types[1] =
        (PvirVectorType){PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED, 8, 256};
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_INVALID_ARGUMENT);
    operation->operand_types[1] = (PvirVectorType){0};
    operation->operand_types[0] =
        (PvirVectorType){PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED, 16, 512};
    operation->operand_immediates[1] = UINT16_MAX;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_OK);
    operation->operand_immediates[1] = (uint64_t)UINT16_MAX + 1;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_INVALID_ARGUMENT);
    operation->operand_types[0] = (PvirVectorType){0};
    operation->operand_immediates[1] = (uint64_t)UINT8_MAX + 1;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_INVALID_ARGUMENT);
    operation->operand_immediates[1] = 0;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_OK);

    compare.opcode = PVIR_OP_COMPARE;
    compare.effects.fault.behavior = PVIR_FAULT_PRECISE;
    compare.effects.fault.lane_order = PVIR_FAULT_ORDER_UNSPECIFIED;
    compare.effects.fault.unknown_from_lane = PVIR_NO_VALUE;
    compare_operation->type =
        (PvirVectorType){PVIR_ELEMENT_MASK, PVIR_SIGNLESS, 1, 32};
    compare_operation->predicate = pvir_predicate_all_true(32);
    compare_operation->inactive_policy = PVIR_INACTIVE_UNKNOWN;
    compare_operation->destination = 50;
    compare_operation->operands[0] = 51;
    compare_operation->operands[1] = 52;
    compare_operation->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    compare_operation->operand_kinds[1] = PVIR_OPERAND_VECTOR;
    compare_operation->operand_types[0] = compare_inputs;
    compare_operation->operand_types[1] = compare_inputs;
    compare_operation->operand_count = 2;
    compare_operation->compare_condition = PVIR_COMPARE_EQ;
    compare_operation->merge_value = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_OK);

    compare_operation->compare_condition = PVIR_COMPARE_NONE;
    CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_INVALID_ARGUMENT);
    compare_operation->compare_condition =
        (PvirCompareCondition)(PVIR_COMPARE_GEU + 1);
    CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_INVALID_ARGUMENT);
    compare_operation->compare_condition = PVIR_COMPARE_EQ;

    compare_operation->type =
        (PvirVectorType){PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED, 8, 256};
    CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_INVALID_ARGUMENT);
    compare_operation->type =
        (PvirVectorType){PVIR_ELEMENT_MASK, PVIR_SIGNLESS, 1, 32};
    compare_operation->operand_types[1] =
        (PvirVectorType){PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED, 16, 512};
    CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_INVALID_ARGUMENT);
    compare_operation->operand_types[1] = compare_inputs;

    for (i = 0; i < sizeof(unsigned_conditions) /
                        sizeof(unsigned_conditions[0]); i++) {
        compare_operation->compare_condition = unsigned_conditions[i];
        CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_OK);
    }
    compare_operation->compare_condition = PVIR_COMPARE_LT;
    CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_INVALID_ARGUMENT);

    compare_inputs.signedness = PVIR_SIGNED;
    compare_operation->operand_types[0] = compare_inputs;
    compare_operation->operand_types[1] = compare_inputs;
    for (i = 0; i < sizeof(signed_conditions) /
                        sizeof(signed_conditions[0]); i++) {
        compare_operation->compare_condition = signed_conditions[i];
        CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_OK);
    }
    compare_operation->compare_condition = PVIR_COMPARE_LTU;
    CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_INVALID_ARGUMENT);

    compare_inputs = (PvirVectorType){
        PVIR_ELEMENT_BITS, PVIR_SIGNLESS, 8, 256
    };
    compare_operation->operand_types[0] = compare_inputs;
    compare_operation->operand_types[1] = compare_inputs;
    compare_operation->compare_condition = PVIR_COMPARE_LT;
    CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_INVALID_ARGUMENT);
    compare_operation->compare_condition = PVIR_COMPARE_EQ;
    CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_OK);
    compare_operation->compare_condition = PVIR_COMPARE_NE;
    CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_OK);

    vector = vector_division(PVIR_INACTIVE_ZERO);
    vector.operation.vector.compare_condition = PVIR_COMPARE_EQ;
    CHECK(pvir_instruction_validate(&vector) == PVIR_STATUS_INVALID_ARGUMENT);

    compare_inputs = (PvirVectorType){
        PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, 32, 128
    };
    compare_operation->type =
        (PvirVectorType){PVIR_ELEMENT_MASK, PVIR_SIGNLESS, 1, 4};
    compare_operation->predicate = pvir_predicate_all_true(4);
    compare_operation->operand_types[0] = compare_inputs;
    compare_operation->operand_types[1] = compare_inputs;
    compare.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_NONE;
    for (i = 0; i < sizeof(float_conditions) /
                        sizeof(float_conditions[0]); i++) {
        compare_operation->compare_condition = float_conditions[i];
        CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_OK);
    }
    compare_operation->compare_condition = PVIR_COMPARE_LTU;
    CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_INVALID_ARGUMENT);
    compare_operation->compare_condition = PVIR_COMPARE_NE;
    CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_OK);
    compare.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
    CHECK(pvir_instruction_validate(&compare) == PVIR_STATUS_INVALID_ARGUMENT);
}

/* --- shared PvirPlan chunk scheduler --- */

typedef struct {
    unsigned vd;
    unsigned a;
    unsigned b;
    bool vv;
    bool move;
} EvenPairCtx;

/* Mirrors LATR can_lasx_chunk register-pair legality. */
static bool even_pair_legal(void *opaque, unsigned offset, unsigned width)
{
    EvenPairCtx *c = opaque;
    unsigned reg_idx;
    if (width < 32) {
        return true; /* 16B steps always legal */
    }
    reg_idx = offset / 16;
    if (((c->vd + reg_idx) & 1u) != 0) {
        return false;
    }
    if (!c->move && ((c->a + reg_idx) & 1u) != 0) {
        return false;
    }
    if (c->vv && ((c->b + reg_idx) & 1u) != 0) {
        return false;
    }
    return true;
}

static void test_plan_chunks(void)
{
    PvirPlan plan;
    EvenPairCtx ctx;

    memset(&plan, 0, sizeof(plan));
    CHECK(pvir_plan_chunks(&plan, 96, false, NULL, NULL) == PVIR_STATUS_OK);
    CHECK(plan.chunk_count == 6);
    CHECK(plan.lasx_chunks == 0);
    CHECK(plan.lsx_chunks == 6);

    memset(&plan, 0, sizeof(plan));
    ctx = (EvenPairCtx){0, 0, 0, true, false};
    CHECK(pvir_plan_chunks(&plan, 64, true, even_pair_legal, &ctx) ==
          PVIR_STATUS_OK);
    CHECK(plan.chunk_count == 2);
    CHECK(plan.steps[0].size == 32 && plan.steps[1].size == 32);
    CHECK(plan.lasx_chunks == 2);

    memset(&plan, 0, sizeof(plan));
    ctx = (EvenPairCtx){1, 0, 0, false, false};
    CHECK(pvir_plan_chunks(&plan, 64, true, even_pair_legal, &ctx) ==
          PVIR_STATUS_OK);
    CHECK(plan.lasx_chunks == 0);
    CHECK(plan.lsx_chunks == 4);

    CHECK(pvir_plan_chunks(&plan, 20, true, NULL, NULL) ==
          PVIR_STATUS_INVALID_ARGUMENT);
}

static void test_plan_mask_keys(void)
{
    PvirVectorOperation op;
    PvirPredicateMaskKey ka, kb;
    PvirPredicateSource src =
        source(PVIR_PREDICATE_SOURCE_PREDICATE, 5, 7);

    memset(&op, 0, sizeof(op));
    op.type.element_bits = 32;
    op.type.vector_bits = 256;
    op.predicate = pvir_predicate_prefix(src, 64, 40);

    CHECK(pvir_plan_mask_key_from_operation(&op, &ka));
    CHECK(pvir_plan_mask_key_from_operation(&op, &kb));
    CHECK(pvir_plan_mask_keys_equal(&ka, &kb));

    op.predicate = pvir_predicate_prefix(src, 64, 32);
    CHECK(pvir_plan_mask_key_from_operation(&op, &kb));
    CHECK(!pvir_plan_mask_keys_equal(&ka, &kb));

    memset(&kb, 0, sizeof(kb));
    CHECK(!pvir_plan_mask_keys_equal(&ka, &kb));
    CHECK(!pvir_plan_mask_keys_equal(&kb, &kb));
}


int main(void)
{
    test_shape_and_canonical_forms();
    test_predicate_slices();
    test_one_bit_predicate_expansion();
    test_typed_operands_and_compare();
    test_cache_keys_preserve_canonical_values();
    test_expanded_mask_cache();
    test_cache_init_rejects_bad_storage_ranges();
    test_instruction_effects_and_inactive_policy();
    test_plan_chunks();
    test_plan_mask_keys();

    if (failures != 0) {
        printf("PVIR: %d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("PVIR: %d checks passed\n", checks);
    return 0;
}
