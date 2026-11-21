#include "sve-decode.h"
#include "target/arm/cpu.h"

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

static A64SVEDecodeStatus decode(uint32_t word,
                                 A64SVEIR1Instruction *out)
{
    return a64_sve_decode(word, out);
}

static void test_sve_feature_state(void)
{
    ARMCPU arm_cpu = {0};

    CHECK(!arm_isar_has_aa64_sve(&arm_cpu.isar));
    CHECK(a64_sve_access_disposition(
              arm_isar_has_aa64_sve(&arm_cpu.isar),
              A64_SVE_CONTROL_UNMODELED) == A64_SVE_ACCESS_FEATURE_ABSENT);

    arm_cpu_set_aa64_sve(&arm_cpu, true);
    CHECK(arm_isar_has_aa64_sve(&arm_cpu.isar));
    CHECK(a64_sve_access_disposition(
              arm_isar_has_aa64_sve(&arm_cpu.isar),
              A64_SVE_CONTROL_UNMODELED) ==
          A64_SVE_ACCESS_CONTROL_UNSUPPORTED);
}

int main(void)
{
    A64SVEIR1Instruction insn;
    A64SVEDecodeStatus status;
    static const uint32_t unsupported_prefix_words[] = {
        0x2518e3f0u, 0x04002000u, 0xa4000000u, 0xe4000000u
    };
    unsigned i;
    test_sve_feature_state();

    /* DDI 0487 M.d C8.2 encodings assembled with llvm-mc -mattr=+sve. */
    status = decode(0x2518e3e0u, &insn); /* PTRUE p0.b, all */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_PTRUE);
    CHECK(insn.arg.f_sve_ptrue.pd == 0);
    CHECK(insn.arg.f_sve_ptrue.element_bits == 8);
    CHECK(insn.arg.f_sve_ptrue.pattern == 31);

    status = decode(0x2558e083u, &insn); /* PTRUE p3.h, vl4 */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_ptrue.pd == 3);
    CHECK(insn.arg.f_sve_ptrue.element_bits == 16);
    CHECK(insn.arg.f_sve_ptrue.pattern == 4);

    status = decode(0x25211c01u, &insn); /* WHILELO p1.b, x0, x1 */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_WHILELO);
    CHECK(insn.arg.f_sve_whilelo.pd == 1);
    CHECK(insn.arg.f_sve_whilelo.rn == 0);
    CHECK(insn.arg.f_sve_whilelo.rm == 1);
    CHECK(insn.arg.f_sve_whilelo.element_bits == 8);
    CHECK(insn.arg.f_sve_whilelo.scalar_bits == 64);
    CHECK(insn.arg.f_sve_whilelo.effects.writes_nzcv);

    status = decode(0x04000020u, &insn); /* ADD z0.b,p0/m,z0.b,z1.b */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_ADD);
    CHECK(insn.arg.f_sve_add.zd == 0);
    CHECK(insn.arg.f_sve_add.zm == 1);
    CHECK(insn.arg.f_sve_add.pg == 0);
    CHECK(insn.arg.f_sve_add.element_bits == 8);
    CHECK(insn.arg.f_sve_add.effects.inactive_policy == PVIR_INACTIVE_MERGE);

    status = decode(0x048014c4u, &insn); /* ADD z4.s,p5/m,z4.s,z6.s */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_add.zd == 4);
    CHECK(insn.arg.f_sve_add.zm == 6);
    CHECK(insn.arg.f_sve_add.pg == 5);
    CHECK(insn.arg.f_sve_add.element_bits == 32);
    CHECK(insn.arg.f_sve_add.effects.inactive_policy == PVIR_INACTIVE_MERGE);

    status = decode(0x04010020u, &insn); /* SUB z0.b,p0/m,z0.b,z1.b */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_SUB);
    CHECK(insn.arg.f_sve_sub.zd == 0);
    CHECK(insn.arg.f_sve_sub.zm == 1);
    CHECK(insn.arg.f_sve_sub.pg == 0);
    CHECK(insn.arg.f_sve_sub.element_bits == 8);
    CHECK(insn.arg.f_sve_sub.effects.inactive_policy == PVIR_INACTIVE_MERGE);

    status = decode(0x04810c82u, &insn); /* SUB z2.s,p3/m,z2.s,z4.s */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_sub.zd == 2);
    CHECK(insn.arg.f_sve_sub.zm == 4);
    CHECK(insn.arg.f_sve_sub.pg == 3);
    CHECK(insn.arg.f_sve_sub.element_bits == 32);
    CHECK(insn.arg.f_sve_sub.effects.inactive_policy == PVIR_INACTIVE_MERGE);

    status = decode(0x04100020u, &insn); /* MUL z0.b,p0/m,z0.b,z1.b */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_MUL);
    CHECK(insn.arg.f_sve_mul.zd == 0);
    CHECK(insn.arg.f_sve_mul.zm == 1);
    CHECK(insn.arg.f_sve_mul.pg == 0);
    CHECK(insn.arg.f_sve_mul.element_bits == 8);
    CHECK(insn.arg.f_sve_mul.effects.inactive_policy == PVIR_INACTIVE_MERGE);

    status = decode(0x04500c82u, &insn); /* MUL z2.h,p3/m,z2.h,z4.h */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_mul.zd == 2);
    CHECK(insn.arg.f_sve_mul.zm == 4);
    CHECK(insn.arg.f_sve_mul.pg == 3);
    CHECK(insn.arg.f_sve_mul.element_bits == 16);

    status = decode(0x04901fe5u, &insn); /* MUL z5.s,p7/m,z5.s,z31.s */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_mul.zd == 5);
    CHECK(insn.arg.f_sve_mul.zm == 31);
    CHECK(insn.arg.f_sve_mul.pg == 7);
    CHECK(insn.arg.f_sve_mul.element_bits == 32);

    status = decode(0x04d01928u, &insn); /* MUL z8.d,p6/m,z8.d,z9.d */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_mul.zd == 8);
    CHECK(insn.arg.f_sve_mul.zm == 9);
    CHECK(insn.arg.f_sve_mul.pg == 6);
    CHECK(insn.arg.f_sve_mul.element_bits == 64);

    status = decode(0x04110020u, &insn);
    CHECK(status == A64_SVE_DECODE_UNSUPPORTED_ENCODING);
    CHECK(insn.insn_type == AARCH64_A64_SVE_UNSUPPORTED_ENCODING);

    status = decode(0x04012020u, &insn);
    CHECK(status == A64_SVE_DECODE_UNSUPPORTED_ENCODING);
    CHECK(insn.insn_type == AARCH64_A64_SVE_UNSUPPORTED_ENCODING);

    status = decode(0xa400a000u, &insn); /* LD1B {z0.b},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_LD1B);
    CHECK(insn.arg.f_sve_contiguous.zt == 0);
    CHECK(insn.arg.f_sve_contiguous.pg == 0);
    CHECK(insn.arg.f_sve_contiguous.rn == 0);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 8);
    CHECK(insn.arg.f_sve_contiguous.effects.inactive_policy == PVIR_INACTIVE_ZERO);
    CHECK(insn.arg.f_sve_contiguous.effects.memory.access == PVIR_MEMORY_READ);
    CHECK(insn.arg.f_sve_contiguous.effects.memory.inactive_lanes ==
          PVIR_INACTIVE_MEMORY_NO_TRANSACTION);
    CHECK(insn.arg.f_sve_contiguous.effects.memory.ordering ==
          PVIR_MEMORY_ORDER_RELAXED);
    CHECK(insn.arg.f_sve_contiguous.effects.fault_behavior == PVIR_FAULT_PRECISE);
    CHECK(insn.arg.f_sve_contiguous.effects.fault_order ==
          PVIR_FAULT_ORDER_LANE_ASCENDING);

    status = decode(0xa400b9abu, &insn); /* LD1B {z11.b},p6/z,[x13] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_contiguous.zt == 11);
    CHECK(insn.arg.f_sve_contiguous.pg == 6);
    CHECK(insn.arg.f_sve_contiguous.rn == 13);

    status = decode(0xe400e000u, &insn); /* ST1B {z0.b},p0,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_ST1B);
    CHECK(insn.arg.f_sve_contiguous.effects.inactive_policy ==
          PVIR_INACTIVE_NOT_APPLICABLE);
    CHECK(insn.arg.f_sve_contiguous.effects.memory.access == PVIR_MEMORY_WRITE);
    CHECK(insn.arg.f_sve_contiguous.effects.memory.inactive_lanes ==
          PVIR_INACTIVE_MEMORY_NO_TRANSACTION);

    status = decode(0xe400fe0eu, &insn); /* ST1B {z14.b},p7,[x16] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_contiguous.zt == 14);
    CHECK(insn.arg.f_sve_contiguous.pg == 7);
    CHECK(insn.arg.f_sve_contiguous.rn == 16);

    status = decode(0x252c9000u, &insn); /* SETFFR */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_SETFFR);
    CHECK(!insn.arg.f_sve_ffr.has_pd);
    CHECK(insn.arg.f_sve_ffr.effects.state_domain ==
          PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT);
    CHECK(insn.arg.f_sve_ffr.effects.state_action == PVIR_ARCH_STATE_WRITE);
    CHECK(insn.arg.f_sve_ffr.effects.ffr_initializes_all_true);

    status = decode(0x2519f000u, &insn); /* RDFFR p0.b */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_RDFFR);
    CHECK(insn.arg.f_sve_ffr.has_pd);
    CHECK(insn.arg.f_sve_ffr.pd == 0);
    CHECK(insn.arg.f_sve_ffr.effects.state_action == PVIR_ARCH_STATE_READ);

    status = decode(0xa41f6000u, &insn); /* LDFF1B {z0.b},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_LDFF1B);
    CHECK(!insn.arg.f_sve_fault_load.has_index);
    CHECK(insn.arg.f_sve_fault_load.effects.fault_behavior ==
          PVIR_FAULT_FIRST_FAULT);
    CHECK(insn.arg.f_sve_fault_load.effects.fault_order ==
          PVIR_FAULT_ORDER_LANE_ASCENDING);
    CHECK(insn.arg.f_sve_fault_load.effects.fault_result ==
          PVIR_FAULT_RESULT_UNKNOWN);
    CHECK(insn.arg.f_sve_fault_load.effects.result_unknown_from_fault_boundary);
    CHECK(insn.arg.f_sve_fault_load.effects.result_unknown_where_ffr_input_clear);
    CHECK(insn.arg.f_sve_fault_load.effects.state_domain ==
          PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT);
    CHECK(insn.arg.f_sve_fault_load.effects.state_action == PVIR_ARCH_STATE_UPDATE);
    CHECK(insn.arg.f_sve_fault_load.effects.state_update ==
          PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS);
    CHECK(insn.arg.f_sve_fault_load.effects.ffr_clears_from_fault_boundary);
    CHECK(insn.arg.f_sve_fault_load.effects.memory.access == PVIR_MEMORY_READ);
    CHECK(insn.arg.f_sve_fault_load.effects.inactive_policy == PVIR_INACTIVE_ZERO);

    status = decode(0xa41f6861u, &insn); /* LDFF1B {z1.b},p2/z,[x3] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_fault_load.zt == 1);
    CHECK(insn.arg.f_sve_fault_load.pg == 2);
    CHECK(insn.arg.f_sve_fault_load.rn == 3);

    status = decode(0xa410a000u, &insn); /* LDNF1B {z0.b},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_LDNF1B);
    CHECK(insn.arg.f_sve_nonfault_load.effects.fault_behavior ==
          PVIR_FAULT_NONFAULTING);
    CHECK(insn.arg.f_sve_nonfault_load.effects.fault_order ==
          PVIR_FAULT_ORDER_UNSPECIFIED);
    CHECK(insn.arg.f_sve_nonfault_load.effects.fault_result ==
          PVIR_FAULT_RESULT_UNKNOWN);
    CHECK(insn.arg.f_sve_nonfault_load.effects.result_unknown_from_fault_boundary);
    CHECK(insn.arg.f_sve_nonfault_load.effects.result_unknown_where_ffr_input_clear);
    CHECK(insn.arg.f_sve_nonfault_load.effects.state_domain ==
          PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT);
    CHECK(insn.arg.f_sve_nonfault_load.effects.state_action == PVIR_ARCH_STATE_UPDATE);
    CHECK(insn.arg.f_sve_nonfault_load.effects.state_update ==
          PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS);
    CHECK(insn.arg.f_sve_nonfault_load.effects.ffr_clears_from_fault_boundary);
    CHECK(insn.arg.f_sve_nonfault_load.effects.memory.access == PVIR_MEMORY_READ);
    CHECK(insn.arg.f_sve_nonfault_load.effects.inactive_policy == PVIR_INACTIVE_ZERO);

    status = decode(0xa410b4c4u, &insn); /* LDNF1B {z4.b},p5/z,[x6] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_nonfault_load.zt == 4);
    CHECK(insn.arg.f_sve_nonfault_load.pg == 5);
    CHECK(insn.arg.f_sve_nonfault_load.rn == 6);

    status = decode(0x2518e3e0u, &insn);
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_PTRUE);
    CHECK(insn.arg.f_sve_ptrue.pattern == 31);
    CHECK(a64_sve_access_disposition(false, A64_SVE_CONTROL_UNMODELED) ==
          A64_SVE_ACCESS_FEATURE_ABSENT);
    CHECK(a64_sve_access_disposition(true, A64_SVE_CONTROL_UNMODELED) ==
          A64_SVE_ACCESS_CONTROL_UNSUPPORTED);
    CHECK(a64_sve_access_disposition(true, A64_SVE_CONTROL_ALLOWED) ==
          A64_SVE_ACCESS_ALLOWED);
    CHECK(a64_sve_access_disposition(true, A64_SVE_CONTROL_DENIED) ==
          A64_SVE_ACCESS_TRAP_REQUIRED);
    status = decode(0xffffffffu, &insn);
    CHECK(status == A64_SVE_DECODE_NO_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_NULL);
    for (i = 0; i < sizeof(unsupported_prefix_words) /
                        sizeof(unsupported_prefix_words[0]); i++) {
        status = decode(unsupported_prefix_words[i], &insn);
        CHECK(status == A64_SVE_DECODE_UNSUPPORTED_ENCODING);
        CHECK(insn.insn_type == AARCH64_A64_SVE_UNSUPPORTED_ENCODING);
        CHECK(insn.arg.f_sve_unsupported_encoding.raw_insn ==
              unsupported_prefix_words[i]);
    }
    status = decode(0x14000000u, &insn); /* Ordinary B-immediate prefix. */
    CHECK(status == A64_SVE_DECODE_NO_MATCH);
    CHECK(a64_sve_decode(0x2518e3e0u, NULL) ==
          A64_SVE_DECODE_INVALID_ARGUMENT);

    if (failures != 0) {
        fprintf(stderr, "SVE decoder: %u/%u checks failed\n", failures, checks);
        return 1;
    }
    printf("SVE decoder: %u checks passed\n", checks);
    return 0;
}
