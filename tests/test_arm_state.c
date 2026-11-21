#include "env.h"

#include <stdio.h>

static unsigned checks;
static unsigned failures;
static CPUARMState state;

#define CHECK(condition)                                                     \
    do {                                                                      \
        checks++;                                                             \
        if (!(condition)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);      \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void test_v_register_offsets(void)
{
    int reg;

    for (reg = 0; reg < (int)ARM_SVE_NUM_ZREGS; reg++) {
        int offset = env_offset_vreg(reg);

        CHECK(offset >= 0);
        CHECK(offset == env_offset_zreg(reg));
        CHECK(offset == env_offset_fpr(reg));
        if (reg + 1 < (int)ARM_SVE_NUM_ZREGS) {
            int next_offset = env_offset_vreg(reg + 1);

            CHECK(next_offset - offset == (int)ARM_SVE_MAX_VL_BYTES);
            CHECK(offset + (int)(ARM_SVE_MIN_VL_BITS / 8u) <= next_offset);
        }
    }
    CHECK(env_offset_vreg(-1) == -1);
    CHECK(env_offset_vreg((int)ARM_SVE_NUM_ZREGS) == -1);
    CHECK(sizeof(state.sve.zregs[0]) == ARM_SVE_MAX_VL_BYTES);
    CHECK(_Alignof(CPUARMState) >= 16);
    CHECK(offsetof(CPUARMState, sve.zregs) % 16 == 0);
    CHECK((uintptr_t)state.sve.zregs % 16 == 0);
}

static void test_legacy_scalar_offsets(void)
{
    size_t legacy_state_end =
        offsetof(CPUARMState, arm_cpu) + sizeof(state.arm_cpu);

    CHECK(env_offset_gpr(31) <= LATA_VMEM_SI12_MAX);
    CHECK(env_offset_pc() <= LATA_VMEM_SI12_MAX);
    CHECK(env_offset_FPSCR() <= LATA_VMEM_SI12_MAX);
    CHECK(env_offset_fpstatus() <= LATA_VMEM_SI12_MAX);
    CHECK(env_offset_RMODE() <= LATA_VMEM_SI12_MAX);
    CHECK(env_offset_QC() <= LATA_VMEM_SI12_MAX);
    CHECK(env_offset_CF() <= LATA_VMEM_SI12_MAX);
    CHECK(env_offset_VF() <= LATA_VMEM_SI12_MAX);
    CHECK(env_offset_ZF() <= LATA_VMEM_SI12_MAX);
    CHECK(env_offset_NF() <= LATA_VMEM_SI12_MAX);
    CHECK(env_offset_PSTATE() <= LATA_VMEM_SI12_MAX);
    CHECK(env_offset_Exclusive() <= LATA_VMEM_SI12_MAX);
    CHECK(legacy_state_end <= (size_t)LATA_VMEM_SI12_MAX + 1u);
    CHECK(offsetof(CPUARMState, sve.zregs) >= legacy_state_end);
    CHECK(offsetof(CPUARMState, sve.pregs) >= legacy_state_end);
    CHECK(offsetof(CPUARMState, sve.ffr) >= legacy_state_end);
    CHECK(offsetof(CPUARMState, sve.active_vl_bits) >= legacy_state_end);
}


static void test_vreg_address_plans(void)
{
    static const int regs[] = {8, 16, 31};
    const int v0_offset = env_offset_vreg(0);
    LataVectorAddressPlan plan;
    size_t i;

    plan = env_vector_address_plan(v0_offset);
    CHECK(!plan.materialize_address);
    CHECK(plan.displacement == v0_offset);

    plan = env_vector_address_plan(LATA_VMEM_SI12_MIN);
    CHECK(!plan.materialize_address);
    CHECK(plan.displacement == LATA_VMEM_SI12_MIN);
    plan = env_vector_address_plan(LATA_VMEM_SI12_MAX);
    CHECK(!plan.materialize_address);
    CHECK(plan.displacement == LATA_VMEM_SI12_MAX);
    plan = env_vector_address_plan(LATA_VMEM_SI12_MIN - 1);
    CHECK(plan.materialize_address);
    CHECK(plan.displacement == 0);
    plan = env_vector_address_plan(LATA_VMEM_SI12_MAX + 1);
    CHECK(plan.materialize_address);
    CHECK(plan.displacement == 0);

    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        int offset = env_offset_vreg(regs[i]);

        CHECK(offset == v0_offset + regs[i] * (int)ARM_SVE_MAX_VL_BYTES);
        CHECK(offset > LATA_VMEM_SI12_MAX);
        plan = env_vector_address_plan(offset);
        CHECK(plan.materialize_address);
        CHECK(plan.displacement == 0);
    }
}

static void test_v_z_storage_alias(void)
{
    int reg;
    size_t byte;

    CHECK(arm_sve_set_vl(&state, ARM_SVE_MAX_VL_BITS));
    for (reg = 0; reg < (int)ARM_SVE_NUM_ZREGS; reg++) {
        uint8_t *v = arm_sve_vreg(&state, (unsigned)reg);
        uint8_t *z = arm_sve_zreg(&state, (unsigned)reg);
        uint8_t upper = (uint8_t)(UINT8_C(0x80) + reg);

        CHECK(v != NULL);
        CHECK(v == z);
        for (byte = ARM_SVE_MIN_VL_BITS / 8u;
             byte < ARM_SVE_MAX_VL_BYTES; byte++) {
            z[byte] = upper;
        }
        for (byte = 0; byte < ARM_SVE_MIN_VL_BITS / 8u; byte++) {
            z[byte] = (uint8_t)(reg + byte);
        }
        for (byte = 0; byte < ARM_SVE_MIN_VL_BITS / 8u; byte++) {
            CHECK(v[byte] == (uint8_t)(reg + byte));
        }

        /* A V storage-view write changes the shared low bytes only. */
        for (byte = 0; byte < ARM_SVE_MIN_VL_BITS / 8u; byte++) {
            v[byte] = (uint8_t)(reg + byte + 1u);
        }
        for (byte = 0; byte < ARM_SVE_MIN_VL_BITS / 8u; byte++) {
            CHECK(z[byte] == (uint8_t)(reg + byte + 1u));
        }
        for (byte = ARM_SVE_MIN_VL_BITS / 8u;
             byte < ARM_SVE_MAX_VL_BYTES; byte++) {
            CHECK(z[byte] == upper);
        }
    }
    CHECK(arm_sve_zreg(&state, ARM_SVE_NUM_ZREGS) == NULL);
    CHECK(arm_sve_vreg(NULL, 0) == NULL);
    state.sve.active_vl_bits = 0;
}

static void test_predicate_storage(void)
{
    uint8_t *p15 = arm_sve_preg(&state, ARM_SVE_NUM_PREGS - 1u);
    uint8_t *ffr = arm_sve_ffr(&state);

    CHECK(sizeof(state.sve.pregs[0]) == ARM_SVE_MAX_PRED_BYTES);
    CHECK(sizeof(state.sve.ffr) == ARM_SVE_MAX_PRED_BYTES);
    CHECK(p15 == state.sve.pregs[ARM_SVE_NUM_PREGS - 1u]);
    CHECK(ffr == state.sve.ffr);
    CHECK(arm_sve_preg(&state, ARM_SVE_NUM_PREGS) == NULL);
    CHECK(arm_sve_preg(NULL, 0) == NULL);
    CHECK(env_offset_sve_preg(-1) == -1);
    CHECK(env_offset_sve_preg((int)ARM_SVE_NUM_PREGS) == -1);
    CHECK(env_offset_sve_ffr() ==
          env_offset_sve_preg(0) +
              (int)(ARM_SVE_NUM_PREGS * ARM_SVE_MAX_PRED_BYTES));

    p15[ARM_SVE_MAX_PRED_BYTES - 1u] = UINT8_C(0x80);
    ffr[ARM_SVE_MAX_PRED_BYTES - 1u] = UINT8_C(0x40);
    CHECK(state.sve.pregs[ARM_SVE_NUM_PREGS - 1u]
                           [ARM_SVE_MAX_PRED_BYTES - 1u] == UINT8_C(0x80));
    CHECK(state.sve.ffr[ARM_SVE_MAX_PRED_BYTES - 1u] == UINT8_C(0x40));
    CHECK(arm_sve_predicate_bytes_for_bits(128) == 2);
    CHECK(arm_sve_predicate_bytes_for_bits(256) == 4);
    CHECK(arm_sve_predicate_bytes_for_bits(ARM_SVE_MAX_VL_BITS) ==
          ARM_SVE_MAX_PRED_BYTES);
    CHECK(arm_sve_predicate_bytes_for_bits(192) == 0);
}

static void test_active_vl(void)
{
    CHECK(!arm_sve_vl_is_valid(0));
    CHECK(!arm_sve_vl_is_valid(127));
    CHECK(arm_sve_vl_is_valid(128));
    CHECK(arm_sve_vl_is_valid(256));
    CHECK(!arm_sve_vl_is_valid(192));
    CHECK(arm_sve_vl_is_valid(ARM_SVE_MAX_VL_BITS));
    CHECK(!arm_sve_vl_is_valid(ARM_SVE_MAX_VL_BITS + 128u));
    CHECK(arm_sve_active_vl_bytes(&state) == 0);
    CHECK(!arm_sve_set_vl(&state, 0));
    CHECK(arm_sve_set_vl(&state, 256));
    CHECK(state.sve.active_vl_bits == 256);
    CHECK(arm_sve_active_vl_bytes(&state) == 32);
    CHECK(arm_sve_vl_bytes_for_bits(256) == 32);
    CHECK(arm_sve_set_vl(&state, ARM_SVE_MAX_VL_BITS));
    CHECK(arm_sve_active_vl_bytes(&state) == ARM_SVE_MAX_VL_BYTES);
    CHECK(!arm_sve_set_vl(&state, ARM_SVE_MAX_VL_BITS + 128u));
    CHECK(state.sve.active_vl_bits == ARM_SVE_MAX_VL_BITS);
    CHECK(!arm_sve_set_vl(NULL, 128));
}

static void test_active_vl_storage_window(void)
{
    uint8_t *z = arm_sve_zreg(&state, 0);
    size_t active_bytes;
    size_t byte;

    CHECK(arm_sve_set_vl(&state, 256));
    active_bytes = arm_sve_active_vl_bytes(&state);
    CHECK(active_bytes == 32);
    for (byte = 0; byte < ARM_SVE_MAX_VL_BYTES; byte++) {
        z[byte] = UINT8_C(0xee);
    }
    for (byte = 0; byte < active_bytes; byte++) {
        z[byte] = (uint8_t)byte;
    }
    for (byte = 0; byte < active_bytes; byte++) {
        CHECK(z[byte] == (uint8_t)byte);
    }
    for (byte = active_bytes; byte < ARM_SVE_MAX_VL_BYTES; byte++) {
        CHECK(z[byte] == UINT8_C(0xee));
    }
    state.sve.active_vl_bits = 0;
}
static void test_expanded_mask_scratch(void)
{
    int offset = env_offset_sve_expanded_mask_scratch();

    CHECK(sizeof(state.sve_expanded_mask_scratch) == ARM_SVE_MAX_VL_BYTES);
    CHECK(offset >= env_offset_sve_active_vl_bits() +
                        (int)sizeof(state.sve.active_vl_bits));
    CHECK((offset % 16) == 0);
    CHECK((size_t)offset + sizeof(state.sve_expanded_mask_scratch) <=
          sizeof(state));
}


int main(void)
{
    test_v_register_offsets();
    test_legacy_scalar_offsets();
    test_vreg_address_plans();
    test_v_z_storage_alias();
    test_predicate_storage();
    test_active_vl();
    test_active_vl_storage_window();
    test_expanded_mask_scratch();
    if (failures != 0) {
        printf("ARM state: %u/%u checks failed\n", failures, checks);
        return 1;
    }
    printf("ARM state: %u checks passed\n", checks);
    return 0;
}
