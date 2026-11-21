#include "sve-pvir.h"
#include "qemu/cpu-features.h"

#include <stdio.h>

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static A64SVEIR1Instruction decode(uint32_t word)
{
    A64SVEIR1Instruction instruction = {0};

    CHECK(a64_sve_decode(word, &instruction) == A64_SVE_DECODE_MATCH);
    return instruction;
}

static void test_ptrue_add_chain(void)
{
    A64SVEPvirBuilder builder;
    A64SVEIR1Instruction source = decode(0x2518e3e0u);
    PvirInstruction producer;
    PvirInstruction consumer;
    PvirPredicateGeneratorOperation *generator;
    PvirVectorOperation *add;
    PvirValueId predicate_value;
    size_t lanes = 0;
    uint64_t packed_mask = UINT64_C(0x5);
    uint64_t expanded[4] = {0};
    PvirPredicate runtime_predicate;

    a64_sve_pvir_builder_init(&builder);
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &producer) ==
          PVIR_STATUS_OK);
    CHECK(producer.opcode == PVIR_OP_PREDICATE_GENERATE);
    generator = &producer.operation.predicate_generator;
    CHECK(generator->kind == PVIR_PREDICATE_GENERATOR_CONSTANT);
    CHECK(generator->form == PVIR_PREDICATE_ALL_TRUE);
    CHECK(generator->lane_type.element_bits == 8);
    CHECK(generator->lane_type.vector_bits == 256);
    CHECK(generator->active_lanes == 32);
    CHECK(producer.effects.arch_state.present);
    CHECK(producer.effects.arch_state.version_before == 0);
    CHECK(producer.effects.arch_state.version_after == 1);
    CHECK(pvir_vector_type_lanes(generator->lane_type, &lanes) ==
          PVIR_STATUS_OK);
    CHECK(lanes == 32);
    predicate_value = generator->destination;

    source = decode(0x04000020u);
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &consumer) ==
          PVIR_STATUS_OK);
    CHECK(consumer.opcode == PVIR_OP_ADD);
    add = &consumer.operation.vector;
    CHECK(add->type.kind == PVIR_ELEMENT_BITS);
    CHECK(add->type.element_bits == 8);
    CHECK(add->type.vector_bits == 256);
    CHECK(add->predicate.form == PVIR_PREDICATE_VECTOR_MASK);
    CHECK(add->predicate.source.kind == PVIR_PREDICATE_SOURCE_PREDICATE);
    CHECK(add->predicate.source.id == predicate_value);
    CHECK(add->predicate.source.version == 1);
    CHECK(add->predicate.lanes == 32);
    CHECK(add->predicate.first_lane == 0);
    CHECK(add->inactive_policy == PVIR_INACTIVE_MERGE);
    CHECK(add->merge_value == add->operands[0]);
    CHECK(add->operands[0] == A64_SVE_PVIR_Z_VALUE_BASE);
    CHECK(add->operands[1] == A64_SVE_PVIR_Z_VALUE_BASE + 1);

    runtime_predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = predicate_value,
            .version = 1,
        },
        4);
    CHECK(pvir_predicate_expand(&runtime_predicate, 8, &packed_mask, 1,
                                expanded, 4) == PVIR_STATUS_OK);
    CHECK(expanded[0] == UINT64_C(0xff));
    CHECK(expanded[1] == 0);
    CHECK(expanded[2] == UINT64_C(0xff));
    CHECK(expanded[3] == 0);
    runtime_predicate.source.id = PVIR_NO_VALUE;
    CHECK(pvir_predicate_validate(&runtime_predicate) ==
          PVIR_STATUS_INVALID_PREDICATE);
}
static void test_sub_builder(void)
{
    A64SVEPvirBuilder builder;
    A64SVEIR1Instruction source = decode(0x04810c82u);
    PvirInstruction instruction;
    PvirVectorOperation *operation;

    a64_sve_pvir_builder_init(&builder);
    CHECK(a64_sve_build_pvir(&builder, &source, 512, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_SUB);
    operation = &instruction.operation.vector;
    CHECK(operation->type.kind == PVIR_ELEMENT_BITS);
    CHECK(operation->type.element_bits == 32);
    CHECK(operation->type.vector_bits == 512);
    CHECK(operation->predicate.source.kind == PVIR_PREDICATE_SOURCE_PREDICATE);
    CHECK(operation->predicate.source.id == A64_SVE_PVIR_P_VALUE_BASE + 3);
    CHECK(operation->predicate.source.version == 0);
    CHECK(operation->predicate.lanes == 16);
    CHECK(operation->inactive_policy == PVIR_INACTIVE_MERGE);
    CHECK(operation->merge_value == operation->operands[0]);
    CHECK(operation->operands[0] == A64_SVE_PVIR_Z_VALUE_BASE + 2);
    CHECK(operation->operands[1] == A64_SVE_PVIR_Z_VALUE_BASE + 4);
    CHECK(operation->operand_count == 2);
    CHECK(instruction.effects.memory.access == PVIR_MEMORY_NONE);
    CHECK(instruction.effects.fault.behavior == PVIR_FAULT_NONE);
    CHECK(builder.z_values[2] == operation->destination);
}

static void test_mul_builder(void)
{
    A64SVEPvirBuilder builder;
    A64SVEIR1Instruction source = decode(0x04901fe5u);
    PvirInstruction instruction;
    PvirVectorOperation *operation;

    a64_sve_pvir_builder_init(&builder);
    CHECK(a64_sve_build_pvir(&builder, &source, 1024, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_MUL);
    operation = &instruction.operation.vector;
    CHECK(operation->type.kind == PVIR_ELEMENT_BITS);
    CHECK(operation->type.element_bits == 32);
    CHECK(operation->type.vector_bits == 1024);
    CHECK(operation->predicate.source.kind == PVIR_PREDICATE_SOURCE_PREDICATE);
    CHECK(operation->predicate.source.id == A64_SVE_PVIR_P_VALUE_BASE + 7);
    CHECK(operation->predicate.lanes == 32);
    CHECK(operation->inactive_policy == PVIR_INACTIVE_MERGE);
    CHECK(operation->merge_value == operation->operands[0]);
    CHECK(operation->operands[0] == A64_SVE_PVIR_Z_VALUE_BASE + 5);
    CHECK(operation->operands[1] == A64_SVE_PVIR_Z_VALUE_BASE + 31);
    CHECK(instruction.effects.memory.access == PVIR_MEMORY_NONE);
    CHECK(instruction.effects.fault.behavior == PVIR_FAULT_NONE);
    CHECK(builder.z_values[5] == operation->destination);

    source.arg.f_sve_mul.element_bits = 24;
    a64_sve_pvir_builder_init(&builder);
    CHECK(a64_sve_build_pvir(&builder, &source, 512, &instruction) ==
          PVIR_STATUS_INVALID_ARGUMENT);
}



static void test_prefix_and_lane_compare_producers(void)
{
    A64SVEPvirBuilder builder;
    A64SVEIR1Instruction source = decode(0x2558e083u);
    A64SVEIR1Instruction add_source;
    PvirInstruction instruction;
    PvirInstruction consumer;
    PvirValueId generated_predicate;
    PvirPredicateGeneratorOperation *generator;

    a64_sve_pvir_builder_init(&builder);
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    generator = &instruction.operation.predicate_generator;
    CHECK(generator->kind == PVIR_PREDICATE_GENERATOR_CONSTANT);
    CHECK(generator->lane_type.element_bits == 16);
    CHECK(generator->lane_type.vector_bits == 256);
    CHECK(generator->form == PVIR_PREDICATE_PREFIX);
    CHECK(generator->active_lanes == 4);

    a64_sve_pvir_builder_init(&builder);
    source = decode(0x25211c01u);
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    generator = &instruction.operation.predicate_generator;
    CHECK(generator->kind == PVIR_PREDICATE_GENERATOR_LANE_COMPARE);
    CHECK(generator->form == PVIR_PREDICATE_VECTOR_MASK);
    CHECK(generator->compare_condition == PVIR_COMPARE_LTU);
    CHECK(generator->scalar_bits == 64);
    CHECK(generator->scalar_start == A64_SVE_PVIR_GPR_VALUE_BASE);
    CHECK(generator->scalar_limit == A64_SVE_PVIR_GPR_VALUE_BASE + 1);
    CHECK(generator->status_state.present);
    CHECK(generator->status_state.version_before == 0);
    CHECK(generator->status_state.version_after == 1);
    CHECK(instruction.effects.arch_state.state_id ==
          A64_SVE_PVIR_P_STATE_BASE + 1);
    generated_predicate = generator->destination;
    add_source = decode(0x04000020u);
    add_source.arg.f_sve_add.pg = 1;
    CHECK(a64_sve_build_pvir(&builder, &add_source, 256, &consumer) ==
          PVIR_STATUS_OK);
    CHECK(consumer.operation.vector.predicate.source.kind ==
          PVIR_PREDICATE_SOURCE_PREDICATE);
    CHECK(consumer.operation.vector.predicate.source.id ==
          generated_predicate);
    CHECK(consumer.operation.vector.predicate.source.version == 1);
    CHECK(consumer.operation.vector.inactive_policy == PVIR_INACTIVE_MERGE);
    CHECK(consumer.operation.vector.merge_value ==
          consumer.operation.vector.operands[0]);

    source = decode(0xa400a000u);
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_LOAD);
    CHECK(instruction.effects.memory.access == PVIR_MEMORY_READ);
    CHECK(instruction.effects.memory.inactive_lanes ==
          PVIR_INACTIVE_MEMORY_NO_TRANSACTION);
    CHECK(instruction.effects.fault.behavior == PVIR_FAULT_PRECISE);
    CHECK(instruction.effects.fault.lane_order ==
          PVIR_FAULT_ORDER_LANE_ASCENDING);
    CHECK(instruction.effects.side_effects ==
          (PVIR_SIDE_EFFECT_IO | PVIR_SIDE_EFFECT_VOLATILE |
           PVIR_SIDE_EFFECT_ORDERED));
    CHECK(instruction.operation.vector.inactive_policy == PVIR_INACTIVE_ZERO);
    CHECK(instruction.operation.vector.memory_element_bits == 8);
    CHECK(instruction.operation.vector.memory_address.base_value ==
          A64_SVE_PVIR_GPR_VALUE_BASE);
    CHECK(instruction.operation.vector.destination != PVIR_NO_VALUE);

    source = decode(0xe400e000u);
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_STORE);
    CHECK(instruction.effects.memory.access == PVIR_MEMORY_WRITE);
    CHECK(instruction.effects.fault.behavior == PVIR_FAULT_PRECISE);
    CHECK(instruction.effects.fault.lane_order ==
          PVIR_FAULT_ORDER_LANE_ASCENDING);
    CHECK((instruction.effects.side_effects & PVIR_SIDE_EFFECT_ORDERED) != 0);
    CHECK(instruction.operation.vector.inactive_policy ==
          PVIR_INACTIVE_NOT_APPLICABLE);
    CHECK(instruction.operation.vector.operand_count == 1);
    CHECK(instruction.operation.vector.operand_kinds[0] ==
          PVIR_OPERAND_VECTOR);
}

static void test_ffr_builder_and_fault_loads(void)
{
    A64SVEPvirBuilder builder;
    A64SVEIR1Instruction source = decode(0x252c9000u);
    PvirInstruction instruction;
    PvirInstruction malformed;
    PvirPredicateGeneratorOperation *generator;
    PvirVectorOperation *load;
    PvirValueId ffr_before_load;
    PvirValueId ffr_after_first;

    a64_sve_pvir_builder_init(&builder);
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_PREDICATE_GENERATE);
    generator = &instruction.operation.predicate_generator;
    CHECK(generator->kind == PVIR_PREDICATE_GENERATOR_CONSTANT);
    CHECK(generator->state_id == A64_SVE_PVIR_FFR_STATE);
    CHECK(generator->form == PVIR_PREDICATE_ALL_TRUE);
    CHECK(instruction.effects.arch_state.domain ==
          PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT);
    CHECK(instruction.effects.arch_state.action == PVIR_ARCH_STATE_WRITE);
    CHECK(builder.ffr_version == 1);
    ffr_before_load = builder.ffr_value;

    source = decode(0x2519f007u);
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    generator = &instruction.operation.predicate_generator;
    CHECK(generator->kind == PVIR_PREDICATE_GENERATOR_STATE_READ);
    CHECK(generator->source_state.present);
    CHECK(generator->source_state.domain ==
          PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT);
    CHECK(generator->source_state.action == PVIR_ARCH_STATE_READ);
    CHECK(generator->source_state_value == ffr_before_load);
    CHECK(generator->state_id == A64_SVE_PVIR_P_STATE_BASE + 7);

    source = decode(0xa41f6000u);
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_LOAD);
    CHECK(instruction.effects.fault.behavior == PVIR_FAULT_FIRST_FAULT);
    CHECK(instruction.effects.fault.fault_result ==
          PVIR_FAULT_RESULT_UNKNOWN);
    CHECK(instruction.effects.fault.lane_order ==
          PVIR_FAULT_ORDER_LANE_ASCENDING);
    load = &instruction.operation.vector;
    CHECK(load->arch_state.input_value == ffr_before_load);
    CHECK(load->arch_state.output_value != PVIR_NO_VALUE);
    CHECK(load->arch_state.clear_from_lane != PVIR_NO_VALUE);
    CHECK(instruction.effects.fault.unknown_from_lane != PVIR_NO_VALUE);
    CHECK(load->arch_state.clear_from_lane ==
          instruction.effects.fault.unknown_from_lane);
    CHECK(instruction.effects.arch_state.domain ==
          PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT);
    CHECK(instruction.effects.arch_state.action == PVIR_ARCH_STATE_UPDATE);
    CHECK(instruction.effects.arch_state.update_kind ==
          PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS);
    ffr_after_first = load->arch_state.output_value;
    CHECK(builder.ffr_value == ffr_after_first);
    CHECK(builder.ffr_version == 2);

    malformed = instruction;
    malformed.effects.fault.unknown_from_lane = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&malformed) ==
          PVIR_STATUS_INVALID_ARGUMENT);
    malformed = instruction;
    malformed.operation.vector.arch_state.clear_from_lane = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&malformed) ==
          PVIR_STATUS_INVALID_ARGUMENT);

    source = decode(0xa410a000u);
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.effects.fault.behavior == PVIR_FAULT_NONFAULTING);
    CHECK(instruction.effects.fault.lane_order ==
          PVIR_FAULT_ORDER_UNSPECIFIED);
    CHECK(instruction.effects.fault.fault_result ==
          PVIR_FAULT_RESULT_UNKNOWN);
    load = &instruction.operation.vector;
    CHECK(load->arch_state.input_value == ffr_after_first);
    CHECK(load->arch_state.output_value != PVIR_NO_VALUE);
    CHECK(load->arch_state.clear_from_lane != PVIR_NO_VALUE);
    CHECK(instruction.effects.fault.unknown_from_lane != PVIR_NO_VALUE);
    CHECK(instruction.effects.arch_state.action == PVIR_ARCH_STATE_UPDATE);
    CHECK(instruction.effects.arch_state.update_kind ==
          PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS);
    CHECK(builder.ffr_version == 3);
}

static void test_dynamic_predicate_cache_versions(void)
{
    PvirExpandedMaskCacheEntry entries[2] = {0};
    uint64_t storage[2][4] = {{0}};
    uint64_t version_one_words = UINT64_C(0x5);
    uint64_t version_two_words = UINT64_C(0xa);
    const uint64_t *expanded_one = NULL;
    const uint64_t *expanded_two = NULL;
    PvirExpandedMaskCache cache;
    PvirPredicate predicate;

    entries[0].expanded_lanes = storage[0];
    entries[0].capacity = 4;
    entries[1].expanded_lanes = storage[1];
    entries[1].capacity = 4;
    CHECK(pvir_expanded_mask_cache_init(&cache, entries, 2) ==
          PVIR_STATUS_OK);

    predicate = pvir_predicate_vector_mask(
        (PvirPredicateSource){
            .kind = PVIR_PREDICATE_SOURCE_PREDICATE,
            .id = 123,
            .version = 1,
        },
        4);
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8,
                                       &version_one_words, 1,
                                       &expanded_one) == PVIR_STATUS_OK);
    predicate.source.version = 2;
    CHECK(pvir_expanded_mask_cache_get(&cache, &predicate, 8,
                                       &version_two_words, 1,
                                       &expanded_two) == PVIR_STATUS_OK);
    CHECK(expanded_one != expanded_two);
    CHECK(expanded_one[0] == UINT64_C(0xff));
    CHECK(expanded_one[1] == 0);
    CHECK(expanded_two[0] == 0);
    CHECK(expanded_two[1] == UINT64_C(0xff));
}

static void test_lasx_dispatch_gate(void)
{
    A64SVEAccessDisposition unmodeled_access;

    CHECK(a64_sve_ir2_dispatch(A64_SVE_ACCESS_FEATURE_ABSENT) ==
          A64_SVE_IR2_FEATURE_ABSENT);
    latx_cpu_set_vector_features(1, 1);
    unmodeled_access = a64_sve_access_disposition(
        true, A64_SVE_CONTROL_UNMODELED);
    CHECK(unmodeled_access == A64_SVE_ACCESS_CONTROL_UNSUPPORTED);
    CHECK(a64_sve_ir2_dispatch(unmodeled_access) ==
          A64_SVE_IR2_ACCESS_UNSUPPORTED);
    CHECK(a64_sve_ir2_dispatch(A64_SVE_ACCESS_TRAP_REQUIRED) ==
          A64_SVE_IR2_ACCESS_UNSUPPORTED);

    latx_cpu_set_vector_features(1, 0);
    CHECK(a64_sve_ir2_dispatch(A64_SVE_ACCESS_ALLOWED) ==
          A64_SVE_IR2_LASX_UNAVAILABLE);
    latx_cpu_set_vector_features(1, 1);
    CHECK(a64_sve_ir2_dispatch(A64_SVE_ACCESS_ALLOWED) ==
          A64_SVE_IR2_READY);
}

int main(void)
{
    test_ptrue_add_chain();
    test_sub_builder();
    test_mul_builder();
    test_prefix_and_lane_compare_producers();
    test_ffr_builder_and_fault_loads();
    test_dynamic_predicate_cache_versions();
    test_lasx_dispatch_gate();

    if (failures != 0) {
        fprintf(stderr, "SVE PVIR: %u/%u checks failed\n", failures,
                checks);
        return 1;
    }
    printf("SVE PVIR: %u checks passed\n", checks);
    return 0;
}
