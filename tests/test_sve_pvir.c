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
          A64_SVE_IR2_READY);
    CHECK(a64_sve_select_backend() == A64_SVE_BACKEND_LSX);
    latx_cpu_set_vector_features(0, 0);
    CHECK(a64_sve_ir2_dispatch(A64_SVE_ACCESS_ALLOWED) ==
          A64_SVE_IR2_READY);
    CHECK(a64_sve_select_backend() == A64_SVE_BACKEND_SCALAR_HELPER);
    latx_cpu_set_vector_features(1, 1);
    CHECK(a64_sve_ir2_dispatch(A64_SVE_ACCESS_ALLOWED) ==
          A64_SVE_IR2_READY);
    CHECK(a64_sve_select_backend() == A64_SVE_BACKEND_LASX);
}

static void test_fp_builder(void)
{
    A64SVEPvirBuilder builder;
    A64SVEIR1Instruction source;
    PvirInstruction instruction;
    const PvirVectorOperation *op;
    PvirValueId z0_before;

    a64_sve_pvir_builder_init(&builder);

    source = decode(0x65808020u); /* FADD z0.s,p0/m,z0.s,z1.s */
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_ADD);
    op = &instruction.operation.vector;
    CHECK(op->type.kind == PVIR_ELEMENT_FLOAT);
    CHECK(op->type.signedness == PVIR_SIGNLESS);
    CHECK(op->type.element_bits == 32);
    CHECK(op->type.vector_bits == 256);
    CHECK(op->predicate.lanes == 8);
    CHECK(op->predicate.form == PVIR_PREDICATE_VECTOR_MASK);
    CHECK(op->inactive_policy == PVIR_INACTIVE_MERGE);
    CHECK(op->merge_value != PVIR_NO_VALUE);
    CHECK(op->merge_value == op->operands[0]);
    /* Inactive lanes must not raise FP exceptions or touch FPSR. */
    CHECK(instruction.effects.fp_exceptions ==
          PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY);
    CHECK(instruction.effects.memory.access == PVIR_MEMORY_NONE);
    CHECK(instruction.effects.fault.behavior == PVIR_FAULT_NONE);

    source = decode(0x658d88e6u); /* FDIV z6.s,p2/m,z6.s,z7.s */
    CHECK(a64_sve_build_pvir(&builder, &source, 512, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_DIV);
    CHECK(instruction.operation.vector.type.element_bits == 32);
    CHECK(instruction.operation.vector.predicate.lanes == 16);
    CHECK(instruction.effects.fp_exceptions ==
          PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY);

    source = decode(0x65c18462u); /* FSUB z2.d,p1/m,z2.d,z3.d */
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_SUB);
    CHECK(instruction.operation.vector.type.element_bits == 64);
    CHECK(instruction.operation.vector.predicate.lanes == 4);

    /* Element size outside {32,64} is rejected, never silently widened. */
    source = decode(0x65808020u);
    source.arg.f_sve_fadd.element_bits = 16;
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_INVALID_ARGUMENT);

    /* FABS/FNEG are sign-bit ops: BITS + immediate, never FLOAT. */
    source = decode(0x049ca020u); /* FABS z0.s,p0/m,z1.s */
    z0_before = builder.z_values[0];
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_AND);
    op = &instruction.operation.vector;
    CHECK(op->type.kind == PVIR_ELEMENT_BITS);
    CHECK(op->type.element_bits == 32);
    CHECK(op->type.vector_bits == 256);
    CHECK(op->predicate.lanes == 8);
    CHECK(op->inactive_policy == PVIR_INACTIVE_MERGE);
    CHECK(op->merge_value == z0_before);
    CHECK(op->operand_count == 2);
    CHECK(op->operand_kinds[0] == PVIR_OPERAND_VECTOR);
    CHECK(op->operand_kinds[1] == PVIR_OPERAND_IMMEDIATE);
    CHECK(op->operand_immediates[1] == UINT64_C(0x7fffffff));
    CHECK(instruction.effects.fp_exceptions == PVIR_FP_EXCEPTIONS_NONE);

    source = decode(0x04dda020u); /* FNEG z0.d,p0/m,z1.d */
    CHECK(a64_sve_build_pvir(&builder, &source, 512, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_XOR);
    CHECK(instruction.operation.vector.type.element_bits == 64);
    CHECK(instruction.operation.vector.operand_immediates[1] ==
          UINT64_C(0x8000000000000000));
    CHECK(instruction.effects.fp_exceptions == PVIR_FP_EXCEPTIONS_NONE);

    source = decode(0x049ca020u);
    source.arg.f_sve_fabs.element_bits = 16;
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_INVALID_ARGUMENT);
}

static void test_ptrue_patterns(void)
{
    struct {
        uint8_t pattern;
        uint32_t vl_bits;
        uint8_t element_bits;
        PvirPredicateForm form;
        size_t active_lanes;
    } cases[] = {
        /* VL=256: .b 32 lanes, .s 8 lanes, .d 4 lanes. */
        { 0x1f, 256, 8, PVIR_PREDICATE_ALL_TRUE, 32 },
        { 0x00, 256, 8, PVIR_PREDICATE_ALL_TRUE, 32 },
        { 0x00, 256, 32, PVIR_PREDICATE_ALL_TRUE, 8 },
        { 0x01, 256, 8, PVIR_PREDICATE_PREFIX, 1 },
        { 0x00, 256, 64, PVIR_PREDICATE_ALL_TRUE, 4 },
        { 0x04, 256, 32, PVIR_PREDICATE_PREFIX, 4 },
        { 0x04, 128, 64, PVIR_PREDICATE_ALL_FALSE, 0 }, /* 2 lanes < VL4 */
        { 0x02, 128, 64, PVIR_PREDICATE_ALL_TRUE, 2 }, /* VL2 == 2 lanes */
        { 0x09, 1024, 32, PVIR_PREDICATE_PREFIX, 16 }, /* VL16 < 32 lanes */
        { 0x0a, 1024, 32, PVIR_PREDICATE_ALL_TRUE, 32 }, /* VL32 == 32 */
        /* VL256 needs 256 elements; a 128-lane VL cannot satisfy it. */
        { 0x0d, 1024, 8, PVIR_PREDICATE_ALL_FALSE, 0 },
        { 0x1d, 256, 8, PVIR_PREDICATE_ALL_TRUE, 32 }, /* 32 % 4 == 0 */
        { 0x1e, 256, 8, PVIR_PREDICATE_PREFIX, 30 },   /* 32 - 32%3 */
        { 0x10, 256, 8, PVIR_PREDICATE_ALL_FALSE, 0 }, /* reserved uimm5 */
    };
    A64SVEPvirBuilder builder;
    A64SVEIR1Instruction source;
    PvirInstruction instruction;
    const PvirPredicateGeneratorOperation *generator;
    const PvirVectorOperation *op;
    PvirValueId z0_before;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint32_t word = 0x2518e000u; /* PTRUE p0.b, all */
        word |= (uint32_t)cases[i].pattern << 5;
        word |= (uint32_t)(cases[i].element_bits == 8    ? 0x0
                           : cases[i].element_bits == 16 ? 0x400000u
                           : cases[i].element_bits == 32 ? 0x800000u
                                                         : 0xc00000u);

        a64_sve_pvir_builder_init(&builder);
        source = decode(word);
        CHECK(a64_sve_build_pvir(&builder, &source, cases[i].vl_bits,
                                 &instruction) == PVIR_STATUS_OK);
        generator = &instruction.operation.predicate_generator;
        CHECK(generator->form == cases[i].form);
        CHECK(generator->active_lanes == cases[i].active_lanes);
    }

    /* DUP: scalar operand broadcast across an all-true predicate. */
    a64_sve_pvir_builder_init(&builder);
    source = decode(0x05a03820u); /* MOV z0.s, w1 */
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_BROADCAST);
    op = &instruction.operation.vector;
    CHECK(op->type.element_bits == 32);
    CHECK(op->operand_count == 1);
    CHECK(op->operand_kinds[0] == PVIR_OPERAND_SCALAR);
    CHECK(op->operands[0] == A64_SVE_PVIR_GPR_VALUE_BASE + 1);
    CHECK(op->predicate.form == PVIR_PREDICATE_ALL_TRUE);
    CHECK(op->predicate.lanes == 8);

    /* NOT: PVIR_OP_NOT with one vector operand and merge policy. */
    a64_sve_pvir_builder_init(&builder);
    source = decode(0x049ea020u); /* NOT z0.s,p0/m,z1.s */
    z0_before = builder.z_values[0];
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_NOT);
    op = &instruction.operation.vector;
    CHECK(op->type.kind == PVIR_ELEMENT_BITS);
    CHECK(op->type.element_bits == 32);
    CHECK(op->operand_count == 1);
    CHECK(op->operand_kinds[0] == PVIR_OPERAND_VECTOR);
    CHECK(op->merge_value == z0_before);
    CHECK(op->predicate.lanes == 8);
    CHECK(instruction.effects.fp_exceptions == PVIR_FP_EXCEPTIONS_NONE);

    /* PFALSE: constant generator with zero active lanes. */
    a64_sve_pvir_builder_init(&builder);
    source = decode(0x2518e400u);
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.operation.predicate_generator.form ==
          PVIR_PREDICATE_ALL_FALSE);
    CHECK(instruction.operation.predicate_generator.active_lanes == 0);

    /* PTRUES carries a PSTATE write; PTRUE does not. */
    a64_sve_pvir_builder_init(&builder);
    source = decode(0x2519e3e0u); /* PTRUES p0.b, all */
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.operation.predicate_generator.status_state.present);
    CHECK(instruction.operation.predicate_generator.status_state.state_id ==
          A64_SVE_PVIR_STATUS_STATE);

    a64_sve_pvir_builder_init(&builder);
    source = decode(0x2518e3e0u); /* PTRUE p0.b, all */
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(!instruction.operation.predicate_generator.status_state.present);

    /* VL32 at exactly 32 lanes is a prefix, not all-true. */
    a64_sve_pvir_builder_init(&builder);
    source = decode(0x2518e3e0u);
    source.arg.f_sve_ptrue.pattern = 0x0a;
    source.arg.f_sve_ptrue.element_bits = 32;
    CHECK(a64_sve_build_pvir(&builder, &source, 1024, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.operation.predicate_generator.active_lanes == 32);
}


static void test_contiguous_widths(void)
{
    A64SVEPvirBuilder builder;
    A64SVEIR1Instruction source;
    PvirInstruction instruction;
    const PvirVectorOperation *op;

    /* LD1W {z0.s},p0/z,[x0] at VL=256: 8 lanes of 32 bits from 32-bit memory. */
    a64_sve_pvir_builder_init(&builder);
    source = decode(0xa540a000u);
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_LOAD);
    op = &instruction.operation.vector;
    CHECK(op->type.kind == PVIR_ELEMENT_INTEGER);
    CHECK(op->type.signedness == PVIR_UNSIGNED);
    CHECK(op->type.element_bits == 32);
    CHECK(op->memory_element_bits == 32);
    CHECK(op->predicate.lanes == 8);
    CHECK(op->inactive_policy == PVIR_INACTIVE_ZERO);
    CHECK(instruction.effects.memory.access == PVIR_MEMORY_READ);

    /* ST1D {z0.d},p0,[x0]: 4 lanes, 64-bit memory, no merge value. */
    a64_sve_pvir_builder_init(&builder);
    source = decode(0xe5e0e000u);
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_STORE);
    op = &instruction.operation.vector;
    CHECK(op->type.element_bits == 64);
    CHECK(op->memory_element_bits == 64);
    CHECK(op->predicate.lanes == 4);
    CHECK(op->inactive_policy == PVIR_INACTIVE_NOT_APPLICABLE);
    CHECK(instruction.effects.memory.access == PVIR_MEMORY_WRITE);

    /* LD1SB {z0.d}: sign extension is recorded as SIGNED integer elements. */
    a64_sve_pvir_builder_init(&builder);
    source = decode(0xa580a000u);
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_LOAD);
    op = &instruction.operation.vector;
    CHECK(op->type.signedness == PVIR_SIGNED);
    CHECK(op->type.element_bits == 64);
    CHECK(op->memory_element_bits == 8);
    CHECK(op->predicate.lanes == 4);

    /* A store can never carry sign extension. */
    a64_sve_pvir_builder_init(&builder);
    source = decode(0xe5e0e000u); /* ST1D {z0.d} */
    source.arg.f_sve_contiguous.sign_extend = true;
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_INVALID_ARGUMENT);

    /* A memory width wider than the vector element is rejected. */
    a64_sve_pvir_builder_init(&builder);
    source = decode(0xa5e0a000u); /* LD1D {z0.d} */
    source.arg.f_sve_contiguous.element_bits = 32;
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) ==
          PVIR_STATUS_INVALID_ARGUMENT);
}

static void test_expanded_sve_pvir_and_const_pred(void)
{
    A64SVEPvirBuilder builder;
    A64SVEIR1Instruction source;
    PvirInstruction instruction;

    /* PTRUE records constant predicate bytes in known_preg_valid_mask and invalidates on WHILELO. */
    a64_sve_pvir_builder_init(&builder);
    source = decode(0x2518e3e0u); /* PTRUE p0.b, all */
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) == PVIR_STATUS_OK);
    CHECK((builder.known_preg_valid_mask & 1u) != 0);
    CHECK(builder.known_preg_bytes[0][0] == 0xffu);
    CHECK(builder.known_preg_bytes[0][3] == 0xffu);

    /* WRFFR, predicated RDFFR, and COMPACT */
    source = decode(0x25289040u); /* wrffr p2.b */
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) == PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_ARCH_STATE);

    source = decode(0x2558f020u); /* rdffrs p0.b, p1/z */
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) == PVIR_STATUS_OK);
    CHECK(instruction.opcode == PVIR_OP_PREDICATE_GENERATE);
    CHECK((builder.known_preg_valid_mask & 1u) == 0); /* invalidated by dynamic write */

    /* PFALSE records all-zero constant predicate bytes */
    source = decode(0x2518e400u); /* pfalse p0.b */
    CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) == PVIR_STATUS_OK);
    CHECK((builder.known_preg_valid_mask & 1u) != 0);
    CHECK(builder.known_preg_bytes[0][0] == 0x00u);

    /* Expanded instruction builders produce valid PVIR ops. */
    static const uint32_t words[] = {
        0x05a18440u, /* compact z0.s, p1, z2.s */
        0x2550c440u, /* ptest p1, p2.b */
        0x25a21420u, /* whilelt p0.s, x1, x2 */
        0x25a08440u, /* cntp x0, p1, p2.s */
        0x04b2e3e0u, /* incw x0, all, mul #3 */
        0x25104440u, /* brka p0.b, p1/z, p2.b */
        0x2558c020u, /* pfirst p0.b, p1, p0.b */
        0x2599c420u, /* pnext p0.s, p1, p0.s */
        0x05a0a440u, /* lasta w0, p1, z2.s */
        0x25034440u, /* and p0.b, p1/z, p2.b, p3.b */
        0xa5434440u, /* ld1w scalar+scalar */
        0xa520e440u, /* ld2w */
        0x8541c440u, /* ld1rw */
        0xa5012440u, /* ld1rqw */
        0x85234440u, /* gather ld1w */
        0xa501e440u, /* ldnt1w */
        0x65a30440u, /* fmla */
        0x65858440u, /* fminnm */
        0x65836440u, /* fcmeq */
        0x65caa440u, /* fcvt */
        0x658e3040u, /* frecpe */
        0x65802440u, /* faddv */
        0x04812440u, /* uaddv */
        0x04a21020u, /* sqadd */
        0x04639c20u, /* lsl #3 */
        0x0499a440u, /* clz */
        0x05b83820u, /* rev */
        0x05a23020u, /* tbl */
        0x04a24020u, /* index */
        0x45c27020u, /* smullb */
        0x058000e0u, /* and #0xff */
        0x2483a440u, /* cmpeq */
        0x05a3c440u, /* sel */
        0x04940440u, /* sdiv */
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        a64_sve_pvir_builder_init(&builder);
        source = decode(words[i]);
        CHECK(a64_sve_build_pvir(&builder, &source, 256, &instruction) == PVIR_STATUS_OK);
    }
}

int main(void)
{
    test_ptrue_add_chain();
    test_sub_builder();
    test_mul_builder();
    test_fp_builder();
    test_ptrue_patterns();
    test_contiguous_widths();
    test_prefix_and_lane_compare_producers();
    test_ffr_builder_and_fault_loads();
    test_dynamic_predicate_cache_versions();
    test_lasx_dispatch_gate();
    test_expanded_sve_pvir_and_const_pred();

    if (failures != 0) {
        fprintf(stderr, "SVE PVIR: %u/%u checks failed\n", failures,
                checks);
        return 1;
    }
    printf("SVE PVIR: %u checks passed\n", checks);
    return 0;
}
