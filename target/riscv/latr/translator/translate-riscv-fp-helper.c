#include <stdbool.h>
#include <stdint.h>

#include "ir1.h"
#include "riscv-env.h"
#include "fpu/softfloat.h"

static uint32_t latr_fp_read_s(const CPUArchState *env, uint64_t reg)
{
    uint64_t value = env->fpr[reg];

    if ((value >> 32) != UINT32_MAX) {
        return UINT32_C(0x7fc00000);
    }
    return (uint32_t)value;
}

static uint64_t latr_fp_sign_extend_w(uint32_t value)
{
    return (value & UINT32_C(0x80000000)) ?
        (UINT64_C(0xffffffff00000000) | value) : value;
}

static int32_t latr_fp_s32_from_bits(uint32_t value)
{
    if (value & UINT32_C(0x80000000)) {
        return -1 - (int32_t)~value;
    }
    return (int32_t)value;
}

static int64_t latr_fp_s64_from_bits(uint64_t value)
{
    if (value & UINT64_C(0x8000000000000000)) {
        return -1 - (int64_t)~value;
    }
    return (int64_t)value;
}

static bool latr_fp_set_rounding(float_status *status,
                                 const CPUArchState *env, uint64_t rm)
{
    FloatRoundMode mode;

    if (rm == 7) {
        rm = (env->fcsr >> 5) & 7;
    }
    switch (rm) {
    case 0:
        mode = float_round_nearest_even;
        break;
    case 1:
        mode = float_round_to_zero;
        break;
    case 2:
        mode = float_round_down;
        break;
    case 3:
        mode = float_round_up;
        break;
    case 4:
        mode = float_round_ties_away;
        break;
    default:
        return false;
    }

    set_float_rounding_mode(mode, status);
    return true;
}

static uint64_t latr_fp_riscv_flags(const float_status *status)
{
    const uint16_t flags = status->float_exception_flags;
    const uint16_t invalid_flags = float_flag_invalid |
        float_flag_invalid_isi | float_flag_invalid_imz |
        float_flag_invalid_idi | float_flag_invalid_zdz |
        float_flag_invalid_sqrt | float_flag_invalid_cvti |
        float_flag_invalid_snan;
    uint64_t riscv_flags = 0;

    if (flags & float_flag_inexact) {
        riscv_flags |= UINT64_C(1) << 0;
    }
    if (flags & float_flag_underflow) {
        riscv_flags |= UINT64_C(1) << 1;
    }
    if (flags & float_flag_overflow) {
        riscv_flags |= UINT64_C(1) << 2;
    }
    if (flags & float_flag_divbyzero) {
        riscv_flags |= UINT64_C(1) << 3;
    }
    if (flags & invalid_flags) {
        riscv_flags |= UINT64_C(1) << 4;
    }
    return riscv_flags;
}

static void latr_fp_store_s(CPUArchState *env, uint64_t rd, uint32_t value)
{
    env->fpr[rd] = UINT64_C(0xffffffff00000000) | value;
}

/* Shared softfloat status setup for scalar and vector FP helpers. */
void latr_fp_status_begin(CPUArchState *env, float_status *status, int rtz)
{
    set_float_exception_flags(0, status);
    set_float_detect_tininess(float_tininess_after_rounding, status);
    set_flush_to_zero(false, status);
    set_flush_inputs_to_zero(false, status);
    set_default_nan_mode(true, status);
    set_snan_bit_is_one(false, status);
    set_no_signaling_nans(false, status);
    (void)latr_fp_set_rounding(status, env, rtz ? 1 : 7);
}

void latr_fp_status_end(CPUArchState *env, float_status *status)
{
    env->fcsr |= latr_fp_riscv_flags(status);
}

uint64_t latr_riscv_fp_rmm(CPUArchState *env, uint64_t opcode, uint64_t rd,
                           uint64_t rs1, uint64_t rs2, uint64_t rs3,
                           uint64_t rm, uint64_t pc)
{
    float_status status = { 0 };
    uint64_t result = 0;
    bool fp_destination = true;
    bool single_destination = false;

    (void)pc;
    set_float_exception_flags(0, &status);
    set_float_detect_tininess(float_tininess_after_rounding, &status);
    set_flush_to_zero(false, &status);
    set_flush_inputs_to_zero(false, &status);
    set_default_nan_mode(true, &status);
    set_snan_bit_is_one(false, &status);
    set_no_signaling_nans(false, &status);
    if (!latr_fp_set_rounding(&status, env, rm)) {
        return 1;
    }

    switch (opcode) {
    case LATR_RV_FADD_S:
        result = float32_add(latr_fp_read_s(env, rs1),
                             latr_fp_read_s(env, rs2), &status);
        single_destination = true;
        break;
    case LATR_RV_FSUB_S:
        result = float32_sub(latr_fp_read_s(env, rs1),
                             latr_fp_read_s(env, rs2), &status);
        single_destination = true;
        break;
    case LATR_RV_FMUL_S:
        result = float32_mul(latr_fp_read_s(env, rs1),
                             latr_fp_read_s(env, rs2), &status);
        single_destination = true;
        break;
    case LATR_RV_FDIV_S:
        result = float32_div(latr_fp_read_s(env, rs1),
                             latr_fp_read_s(env, rs2), &status);
        single_destination = true;
        break;
    case LATR_RV_FSQRT_S:
        result = float32_sqrt(latr_fp_read_s(env, rs1), &status);
        single_destination = true;
        break;
    case LATR_RV_FMADD_S:
        result = float32_muladd(latr_fp_read_s(env, rs1),
                                 latr_fp_read_s(env, rs2),
                                 latr_fp_read_s(env, rs3), 0, &status);
        single_destination = true;
        break;
    case LATR_RV_FMSUB_S:
        result = float32_muladd(latr_fp_read_s(env, rs1),
                                 latr_fp_read_s(env, rs2),
                                 latr_fp_read_s(env, rs3),
                                 float_muladd_negate_c, &status);
        single_destination = true;
        break;
    case LATR_RV_FNMSUB_S:
        result = float32_muladd(latr_fp_read_s(env, rs1),
                                 latr_fp_read_s(env, rs2),
                                 latr_fp_read_s(env, rs3),
                                 float_muladd_negate_product, &status);
        single_destination = true;
        break;
    case LATR_RV_FNMADD_S:
        result = float32_muladd(latr_fp_read_s(env, rs1),
                                 latr_fp_read_s(env, rs2),
                                 latr_fp_read_s(env, rs3),
                                 float_muladd_negate_product |
                                 float_muladd_negate_c, &status);
        single_destination = true;
        break;

    case LATR_RV_FADD_D:
        result = float64_add(env->fpr[rs1], env->fpr[rs2], &status);
        break;
    case LATR_RV_FSUB_D:
        result = float64_sub(env->fpr[rs1], env->fpr[rs2], &status);
        break;
    case LATR_RV_FMUL_D:
        result = float64_mul(env->fpr[rs1], env->fpr[rs2], &status);
        break;
    case LATR_RV_FDIV_D:
        result = float64_div(env->fpr[rs1], env->fpr[rs2], &status);
        break;
    case LATR_RV_FSQRT_D:
        result = float64_sqrt(env->fpr[rs1], &status);
        break;
    case LATR_RV_FMADD_D:
        result = float64_muladd(env->fpr[rs1], env->fpr[rs2],
                                env->fpr[rs3], 0, &status);
        break;
    case LATR_RV_FMSUB_D:
        result = float64_muladd(env->fpr[rs1], env->fpr[rs2],
                                env->fpr[rs3], float_muladd_negate_c,
                                &status);
        break;
    case LATR_RV_FNMSUB_D:
        result = float64_muladd(env->fpr[rs1], env->fpr[rs2],
                                env->fpr[rs3],
                                float_muladd_negate_product, &status);
        break;
    case LATR_RV_FNMADD_D:
        result = float64_muladd(env->fpr[rs1], env->fpr[rs2],
                                env->fpr[rs3],
                                float_muladd_negate_product |
                                float_muladd_negate_c, &status);
        break;

    case LATR_RV_FCVT_S_D:
        result = float64_to_float32(env->fpr[rs1], &status);
        single_destination = true;
        break;
    case LATR_RV_FCVT_D_S:
        result = float32_to_float64(latr_fp_read_s(env, rs1), &status);
        break;

    case LATR_RV_FCVT_W_S:
        result = latr_fp_sign_extend_w((uint32_t)
            float32_to_int32(latr_fp_read_s(env, rs1), &status));
        fp_destination = false;
        break;
    case LATR_RV_FCVT_W_D:
        result = latr_fp_sign_extend_w((uint32_t)
            float64_to_int32(env->fpr[rs1], &status));
        fp_destination = false;
        break;
    case LATR_RV_FCVT_L_S:
        result = (uint64_t)float32_to_int64(latr_fp_read_s(env, rs1),
                                             &status);
        fp_destination = false;
        break;
    case LATR_RV_FCVT_L_D:
        result = (uint64_t)float64_to_int64(env->fpr[rs1], &status);
        fp_destination = false;
        break;
    case LATR_RV_FCVT_WU_S:
        result = latr_fp_sign_extend_w(float32_to_uint32(
            latr_fp_read_s(env, rs1), &status));
        fp_destination = false;
        break;
    case LATR_RV_FCVT_WU_D:
        result = latr_fp_sign_extend_w(float64_to_uint32(
            env->fpr[rs1], &status));
        fp_destination = false;
        break;
    case LATR_RV_FCVT_LU_S:
        result = float32_to_uint64(latr_fp_read_s(env, rs1), &status);
        fp_destination = false;
        break;
    case LATR_RV_FCVT_LU_D:
        result = float64_to_uint64(env->fpr[rs1], &status);
        fp_destination = false;
        break;

    case LATR_RV_FCVT_S_W:
        result = int32_to_float32(latr_fp_s32_from_bits(
            (uint32_t)env->gpr[rs1]), &status);
        single_destination = true;
        break;
    case LATR_RV_FCVT_S_WU:
        result = uint32_to_float32((uint32_t)env->gpr[rs1], &status);
        single_destination = true;
        break;
    case LATR_RV_FCVT_S_L:
        result = int64_to_float32(latr_fp_s64_from_bits(env->gpr[rs1]),
                                  &status);
        single_destination = true;
        break;
    case LATR_RV_FCVT_S_LU:
        result = uint64_to_float32(env->gpr[rs1], &status);
        single_destination = true;
        break;
    case LATR_RV_FCVT_D_W:
        result = int32_to_float64(latr_fp_s32_from_bits(
            (uint32_t)env->gpr[rs1]), &status);
        break;
    case LATR_RV_FCVT_D_WU:
        result = uint32_to_float64((uint32_t)env->gpr[rs1], &status);
        break;
    case LATR_RV_FCVT_D_L:
        result = int64_to_float64(latr_fp_s64_from_bits(env->gpr[rs1]),
                                  &status);
        break;
    case LATR_RV_FCVT_D_LU:
        result = uint64_to_float64(env->gpr[rs1], &status);
        break;
    default:
        return 1;
    }

    if (fp_destination) {
        if (single_destination) {
            latr_fp_store_s(env, rd, (uint32_t)result);
        } else {
            env->fpr[rd] = result;
        }
    } else if (rd != 0) {
        env->gpr[rd] = result;
    }
    env->fcsr |= latr_fp_riscv_flags(&status);
    return 0;
}
