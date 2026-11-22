/*
 * PVIR execution for LATR vector instructions.
 *
 * The frontend emits one PvirInstruction per vector operation (built in the
 * family helpers), validates it with pvir_instruction_validate(), and runs it
 * here. PVIR owns no memory: predicate bitsets point at env->v_mask_words and
 * all value ids below 32 name architectural vector registers.
 */
#include "latr.h"
#include "translate.h"
#include "riscv-env.h"
#include "mmu.h"
#include "la-pvir.h"
#include "fpu/softfloat.h"

#define PVIR_MAX_LANES 128
#define LATR_PVIR_CACHE_SLOTS 64u

typedef struct {
    uint64_t key_hash;
    uint64_t vtype;
    uint64_t vl;
    bool valid;
    PvirInstruction insn;
} LatrPvirCacheEntry;

static LatrPvirCacheEntry latr_pvir_cache[LATR_PVIR_CACHE_SLOTS];

static uint32_t pvir_cache_slot(uint64_t key_hash, uint64_t vtype,
                                uint64_t vl)
{
    uint64_t h = key_hash ^ (vtype * UINT64_C(0x9e3779b97f4a7c15)) ^
                 (vl * UINT64_C(0xbf58476d1ce4e5b9));
    return (uint32_t)((h ^ (h >> 32)) & (LATR_PVIR_CACHE_SLOTS - 1u));
}

void latr_pvir_cache_clear(void)
{
    memset(latr_pvir_cache, 0, sizeof(latr_pvir_cache));
}

bool latr_pvir_cache_lookup(uint64_t key_hash, uint64_t vtype, uint64_t vl,
                            PvirInstruction *out)
{
    const LatrPvirCacheEntry *entry =
        &latr_pvir_cache[pvir_cache_slot(key_hash, vtype, vl)];

    if (!entry->valid || entry->key_hash != key_hash ||
        entry->vtype != vtype || entry->vl != vl) {
        return false;
    }
    if (out != NULL) {
        *out = entry->insn;
    }
    return true;
}

void latr_pvir_cache_store(uint64_t key_hash, uint64_t vtype, uint64_t vl,
                           const PvirInstruction *insn)
{
    LatrPvirCacheEntry *entry;

    if (insn == NULL) {
        return;
    }
    entry = &latr_pvir_cache[pvir_cache_slot(key_hash, vtype, vl)];
    entry->key_hash = key_hash;
    entry->vtype = vtype;
    entry->vl = vl;
    entry->insn = *insn;
    entry->valid = true;
}

static uint64_t pvir_cache_tag_hash(const CPUArchState *env, uint64_t tag)
{
    uint64_t h = tag ^ (env->pc * UINT64_C(0x9e3779b97f4a7c15));
    unsigned i;

    for (i = 0; i < 7; i++) {
        h ^= env->rvv_args[i] + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) +
             (h >> 2);
    }
    h ^= (env->vxrm & 3u) << 8;
    return h;
}

static uint64_t pvir_operand(const CPUArchState *env, PvirValueId id,
                             unsigned lane, unsigned bits)
{
    if (id < 32u) {
        return latr_rvv_read_elem(env, id, lane, bits);
    }
    if (id == LATR_PVIR_SCALAR_VALUE) {
        return env->rvv_scalar & (bits >= 64 ? UINT64_MAX
                                            : (((uint64_t)1 << bits) - 1));
    }
    if (id == LATR_PVIR_IMM_VALUE) {
        return env->rvv_imm & (bits >= 64 ? UINT64_MAX
                                         : (((uint64_t)1 << bits) - 1));
    }
    if (id == LATR_PVIR_V0_VALUE) {
        unsigned on = (env->vreg[0][lane >> 3] >> (lane & 7u)) & 1u;
        return on ? (bits >= 64 ? UINT64_MAX
                                : (((uint64_t)1 << bits) - 1))
                  : 0;
    }
    return 0;
}

static int64_t pvir_sign(uint64_t value, unsigned bits)
{
    unsigned shift = 64 - bits;

    return (int64_t)(value << shift) >> shift;
}

static FloatRoundMode pvir_round_mode(PvirRounding rounding,
                                      const CPUArchState *env)
{
    switch (rounding) {
    case PVIR_ROUND_NE:
        return float_round_nearest_even;
    case PVIR_ROUND_RTZ:
        return float_round_to_zero;
    case PVIR_ROUND_RDN:
        return float_round_down;
    case PVIR_ROUND_RUP:
        return float_round_up;
    case PVIR_ROUND_RMM:
        return float_round_ties_away;
    case PVIR_ROUND_RNV:
        return float_round_nearest_even;
    case PVIR_ROUND_DEFAULT:
    default:
        switch ((env->fcsr >> 5) & 7u) {
        case 1: return float_round_to_zero;
        case 2: return float_round_down;
        case 3: return float_round_up;
        case 4: return float_round_ties_away;
        default: return float_round_nearest_even;
        }
    }
}

static uint64_t pvir_int_binop(PvirOpcode opcode,
                               const PvirVectorOperation *op, uint64_t a,
                               uint64_t b, unsigned bits)
{
    uint64_t width_mask =
        bits >= 64 ? UINT64_MAX : (((uint64_t)1 << bits) - 1);
    unsigned shift = bits - 1u;
    int64_t sa;
    int64_t sb;

    a &= width_mask;
    b &= width_mask;
    sa = pvir_sign(a, bits);
    sb = pvir_sign(b, bits);

    switch (opcode) {
    case PVIR_OP_ADD: return a + b;
    case PVIR_OP_SUB: return a - b;
    case PVIR_OP_MUL: return a * b;
    case PVIR_OP_AND: return a & b;
    case PVIR_OP_OR:  return a | b;
    case PVIR_OP_XOR: return a ^ b;
    case PVIR_OP_SHL: return a << (b & shift);
    case PVIR_OP_SHR: return a >> (b & shift);
    case PVIR_OP_SAR: return (uint64_t)(sa >> (b & shift));
    case PVIR_OP_DIV:
        if (b == 0) {
            return UINT64_MAX;
        }
        if (op->type.signedness == PVIR_SIGNED) {
            int64_t min_val = (int64_t)(UINT64_MAX << (bits - 1u));
            if (sa == min_val && sb == -1) {
                return (uint64_t)min_val;
            }
            return (uint64_t)(sa / sb);
        }
        return (uint64_t)(a / b);
    case PVIR_OP_REM:
        if (b == 0) {
            return a;
        }
        if (op->type.signedness == PVIR_SIGNED) {
            int64_t min_val = (int64_t)(UINT64_MAX << (bits - 1u));
            if (sa == min_val && sb == -1) {
                return 0;
            }
            return (uint64_t)(sa % sb);
        }
        return (uint64_t)(a % b);
    case PVIR_OP_MIN:
        return op->type.signedness == PVIR_SIGNED
                   ? (uint64_t)(sa < sb ? sa : sb)
                   : (a < b ? a : b);
    case PVIR_OP_MAX:
        return op->type.signedness == PVIR_SIGNED
                   ? (uint64_t)(sa > sb ? sa : sb)
                   : (a > b ? a : b);
    default:
        return 0;
    }
}

static uint64_t pvir_fp_compare(PvirCompareCondition condition,
                                uint64_t a_bits, uint64_t b_bits,
                                unsigned bits, float_status *status)
{
    if (bits == 32) {
        float32 a = (float32)a_bits;
        float32 b = (float32)b_bits;
        switch (condition) {
        case PVIR_COMPARE_EQ: return float32_eq_quiet(a, b, status);
        case PVIR_COMPARE_NE:
            return float32_compare_quiet(a, b, status) !=
                   float_relation_equal;
        case PVIR_COMPARE_LT: return float32_lt(a, b, status);
        case PVIR_COMPARE_LE: return float32_le(a, b, status);
        case PVIR_COMPARE_GT:
            return float32_compare(a, b, status) == float_relation_greater;
        case PVIR_COMPARE_GE: return float32_le(b, a, status);
        default: return 0;
        }
    } else {
        float64 a = (float64)a_bits;
        float64 b = (float64)b_bits;
        switch (condition) {
        case PVIR_COMPARE_EQ: return float64_eq_quiet(a, b, status);
        case PVIR_COMPARE_NE:
            return float64_compare_quiet(a, b, status) !=
                   float_relation_equal;
        case PVIR_COMPARE_LT: return float64_lt(a, b, status);
        case PVIR_COMPARE_LE: return float64_le(a, b, status);
        case PVIR_COMPARE_GT:
            return float64_compare(a, b, status) == float_relation_greater;
        case PVIR_COMPARE_GE: return float64_le(b, a, status);
        default: return 0;
        }
    }
}

static uint64_t pvir_fp_arith(const PvirFPOpOperation *op,
                              uint64_t a_bits, uint64_t b_bits,
                              float_status *status)
{
    if (op->type.element_bits == 32) {
        float32 a = (float32)a_bits;
        float32 b = (float32)b_bits;
        switch (op->kind) {
        case PVIR_FP_ADD: return (uint64_t)float32_add(a, b, status);
        case PVIR_FP_SUB: return (uint64_t)float32_sub(a, b, status);
        case PVIR_FP_MUL: return (uint64_t)float32_mul(a, b, status);
        case PVIR_FP_DIV: return (uint64_t)float32_div(a, b, status);
        case PVIR_FP_SQRT: return (uint64_t)float32_sqrt(a, status);
        default: return 0;
        }
    } else {
        float64 a = (float64)a_bits;
        float64 b = (float64)b_bits;
        switch (op->kind) {
        case PVIR_FP_ADD: return (uint64_t)float64_add(a, b, status);
        case PVIR_FP_SUB: return (uint64_t)float64_sub(a, b, status);
        case PVIR_FP_MUL: return (uint64_t)float64_mul(a, b, status);
        case PVIR_FP_DIV: return (uint64_t)float64_div(a, b, status);
        case PVIR_FP_SQRT: return (uint64_t)float64_sqrt(a, status);
        default: return 0;
        }
    }
}

static uint64_t pvir_convert(const PvirConvertOperation *op, uint64_t value,
                             float_status *status)
{
    unsigned src_bits = op->source_type.element_bits;
    unsigned dst_bits = op->destination_type.element_bits;
    int src_float = op->source_type.kind == PVIR_ELEMENT_FLOAT;
    int dst_float = op->destination_type.kind == PVIR_ELEMENT_FLOAT;

    if (src_float && dst_float) {
        if (src_bits == 32 && dst_bits == 64) {
            return (uint64_t)float32_to_float64((float32)value, status);
        }
        if (src_bits == 64 && dst_bits == 32) {
            return (uint64_t)float64_to_float32((float64)value, status);
        }
        return value;
    }
    if (src_float && !dst_float) {
        int is_signed = op->destination_type.signedness == PVIR_SIGNED;
        if (src_bits == 32) {
            if (dst_bits == 32) {
                return is_signed
                           ? (uint64_t)(uint32_t)float32_to_int32(
                                 (float32)value, status)
                           : float32_to_uint32((float32)value, status);
            }
            return is_signed
                       ? (uint64_t)float32_to_int64((float32)value, status)
                       : float32_to_uint64((float32)value, status);
        }
        if (dst_bits == 32) {
            return is_signed
                       ? (uint64_t)(uint32_t)float64_to_int32(
                             (float64)value, status)
                       : float64_to_uint32((float64)value, status);
        }
        return is_signed
                   ? (uint64_t)float64_to_int64((float64)value, status)
                   : float64_to_uint64((float64)value, status);
    }
    if (!src_float && dst_float) {
        int is_signed = op->source_type.signedness == PVIR_SIGNED;
        if (dst_bits == 32) {
            if (src_bits == 64) {
                return is_signed
                           ? (uint64_t)int64_to_float32((int64_t)value, status)
                           : (uint64_t)uint64_to_float32(value, status);
            }
            return is_signed
                       ? (uint64_t)int32_to_float32((int32_t)value, status)
                       : (uint64_t)uint32_to_float32((uint32_t)value, status);
        }
        if (src_bits == 32) {
            return is_signed
                       ? (uint64_t)int32_to_float64((int32_t)value, status)
                       : (uint64_t)uint32_to_float64((uint32_t)value, status);
        }
        return is_signed
                   ? (uint64_t)int64_to_float64((int64_t)value, status)
                   : (uint64_t)uint64_to_float64(value, status);
    }
    return value;
}

static unsigned pvir_fixed_rm(PvirRounding rounding, const CPUArchState *env)
{
    switch (rounding) {
    case PVIR_ROUND_RNU:     return 0;
    case PVIR_ROUND_NE:      return 1;
    case PVIR_ROUND_RDN:     return 2;
    case PVIR_ROUND_ROD:     return 3;
    case PVIR_ROUND_DEFAULT:
    default:                 return (unsigned)env->vxrm & 3u;
    }
}

static unsigned __int128 pvir_round_magnitude(unsigned __int128 value,
                                              unsigned shift, unsigned rm)
{
    unsigned __int128 mask;
    unsigned __int128 half;
    unsigned __int128 rem;
    unsigned __int128 q;

    if (shift == 0) {
        return value;
    }
    mask = (((unsigned __int128)1) << shift) - 1;
    half = ((unsigned __int128)1) << (shift - 1);
    rem = value & mask;
    q = value >> shift;
    if (rm == 0) {
        if (rem >= half) {
            q++;
        }
    } else if (rm == 1) {
        if (rem > half || (rem == half && (q & 1))) {
            q++;
        }
    } else if (rm == 3) {
        if (rem) {
            q |= 1;
        }
    }
    return q;
}

static __int128 pvir_round_signed(__int128 value, unsigned shift, unsigned rm)
{
    unsigned __int128 magnitude;

    if (shift == 0) {
        return value;
    }
    if (rm == 2) {
        return value >> shift;
    }
    if (rm == 3) {
        __int128 q = value >> shift;
        unsigned __int128 mask = (((unsigned __int128)1) << shift) - 1;
        if (((unsigned __int128)value & mask) != 0) {
            q |= 1;
        }
        return q;
    }
    magnitude = value < 0 ? -(unsigned __int128)value
                          : (unsigned __int128)value;
    magnitude = pvir_round_magnitude(magnitude, shift, rm);
    return value < 0 ? -(__int128)magnitude : (__int128)magnitude;
}

static uint64_t pvir_sat_signed(__int128 value, unsigned bits,
                                CPUArchState *env)
{
    __int128 max = (((__int128)1) << (bits - 1u)) - 1;
    __int128 min = -(((__int128)1) << (bits - 1u));
    uint64_t mask = bits >= 64 ? UINT64_MAX : (((uint64_t)1 << bits) - 1);

    if (value < min) {
        env->vxsat = 1;
        value = min;
    } else if (value > max) {
        env->vxsat = 1;
        value = max;
    }
    return (uint64_t)value & mask;
}

static uint64_t pvir_sat_unsigned(unsigned __int128 value, unsigned bits,
                                  CPUArchState *env)
{
    unsigned __int128 max = bits >= 64 ? UINT64_MAX
                                       : (((unsigned __int128)1 << bits) - 1);

    if (value > max) {
        env->vxsat = 1;
        value = max;
    }
    return (uint64_t)value;
}

static void pvir_write_inactive(CPUArchState *env, PvirValueId dst,
                                PvirInactivePolicy policy,
                                PvirValueId merge_value, unsigned lane,
                                unsigned bits)
{
    if (policy == PVIR_INACTIVE_MERGE) {
        if (merge_value != dst) {
            latr_rvv_write_elem(env, dst, lane, bits,
                                pvir_operand(env, merge_value, lane, bits));
        }
    } else if (policy == PVIR_INACTIVE_ZERO) {
        latr_rvv_write_elem(env, dst, lane, bits, 0);
    }
}

static uint64_t pvir_fixed_operand(const CPUArchState *env,
                                   const PvirFixedPointOperation *op,
                                   unsigned index, unsigned lane,
                                   unsigned bits)
{
    uint64_t mask = bits >= 64 ? UINT64_MAX : (((uint64_t)1 << bits) - 1);

    if (op->operand_kinds[index] == PVIR_OPERAND_IMMEDIATE) {
        return op->operand_immediates[index] & mask;
    }
    return pvir_operand(env, op->operands[index], lane, bits);
}

static uint64_t pvir_fixed_eval(CPUArchState *env,
                                const PvirFixedPointOperation *op,
                                unsigned lane, unsigned bits)
{
    unsigned src_bits = op->kind == PVIR_FIXED_CLIP ? bits * 2u : bits;
    uint64_t a_bits = pvir_fixed_operand(env, op, 0, lane, src_bits);
    uint64_t b_bits = pvir_fixed_operand(env, op, 1, lane, bits);
    unsigned rm = pvir_fixed_rm(op->rounding, env);
    int is_signed = op->type.signedness == PVIR_SIGNED;
    uint64_t mask = bits >= 64 ? UINT64_MAX : (((uint64_t)1 << bits) - 1);
    __int128 sa = (__int128)pvir_sign(a_bits, src_bits);
    __int128 sb = (__int128)pvir_sign(b_bits, bits);
    unsigned __int128 ua = (unsigned __int128)a_bits;
    unsigned __int128 ub = (unsigned __int128)b_bits;

    switch (op->kind) {
    case PVIR_FIXED_AVG_ADD:
        if (is_signed) {
            return (uint64_t)pvir_round_signed(sa + sb, 1, rm) & mask;
        }
        return (uint64_t)pvir_round_magnitude(ua + ub, 1, rm) & mask;
    case PVIR_FIXED_AVG_SUB:
        if (is_signed) {
            return (uint64_t)pvir_round_signed(sa - sb, 1, rm) & mask;
        }
        if (ua < ub) {
            env->vxsat = 1;
            return 0;
        }
        return (uint64_t)pvir_round_magnitude(ua - ub, 1, rm) & mask;
    case PVIR_FIXED_SAT_ADD:
        return is_signed ? pvir_sat_signed(sa + sb, bits, env)
                         : pvir_sat_unsigned(ua + ub, bits, env);
    case PVIR_FIXED_SAT_SUB:
        if (is_signed) {
            return pvir_sat_signed(sa - sb, bits, env);
        }
        if (ua < ub) {
            env->vxsat = 1;
            return 0;
        }
        return pvir_sat_unsigned(ua - ub, bits, env);
    case PVIR_FIXED_SAT_MUL: {
        __int128 prod = sa * sb;
        return pvir_sat_signed(pvir_round_signed(prod, bits - 1u, rm), bits,
                               env);
    }
    case PVIR_FIXED_SCALE_SHIFT: {
        unsigned sh = (unsigned)(b_bits & (bits - 1u));
        return is_signed ? ((uint64_t)pvir_round_signed(sa, sh, rm) & mask)
                         : ((uint64_t)pvir_round_magnitude(ua, sh, rm) & mask);
    }
    case PVIR_FIXED_CLIP: {
        unsigned sh = (unsigned)(b_bits & (src_bits - 1u));
        return is_signed
                   ? pvir_sat_signed(pvir_round_signed(sa, sh, rm), bits, env)
                   : pvir_sat_unsigned(pvir_round_magnitude(ua, sh, rm), bits,
                                       env);
    }
    default:
        return 0;
    }
}

static uint64_t pvir_mem_rw_lane(CPUArchState *env, uint64_t addr,
                                 unsigned bytes, int is_write, uint64_t val)
{
    uint64_t host = 0;
    uint64_t phys = 0;
    unsigned b;
    int cause = latr_translate(env, addr,
                               is_write ? LATR_ACCESS_STORE : LATR_ACCESS_LOAD,
                               bytes, &host, &phys);

    if (cause) {
        env->mem_fault_cause = cause;
        env->mem_va = addr;
        return 0;
    }
    if (env->mem_mmio) {
        if (is_write) {
            latr_mmio_store(env, phys, bytes, val);
        } else {
            val = latr_mmio_load(env, phys, bytes);
        }
        if (env->mem_fault_cause) {
            env->mem_va = addr;
            return 0;
        }
        return val;
    }
    if (is_write) {
        for (b = 0; b < bytes; b++) {
            ((uint8_t *)(uintptr_t)host)[b] = (uint8_t)(val >> (8u * b));
        }
        return 0;
    }
    val = 0;
    for (b = 0; b < bytes; b++) {
        val |= (uint64_t)((const uint8_t *)(uintptr_t)host)[b] << (8u * b);
    }
    return val;
}

static uint64_t pvir_resolve_base(const CPUArchState *env,
                                  const PvirMemoryAddress *addr)
{
    uint64_t base = 0;

    if (addr->base_kind == PVIR_MEMORY_BASE_VALUE) {
        if (addr->base_value == LATR_PVIR_SCALAR_VALUE) {
            base = env->rvv_scalar;
        } else if (addr->base_value < 32u) {
            base = env->gpr[addr->base_value & 31u];
        }
    }
    return (uint64_t)((int64_t)base + addr->displacement);
}

static void pvir_insn_type_and_predicate(const PvirInstruction *instruction,
                                         PvirVectorType *type,
                                         const PvirPredicate **predicate)
{
    *predicate = NULL;
    switch (instruction->opcode) {
    case PVIR_OP_FP_ARITH:
        *type = instruction->operation.fp.type;
        *predicate = &instruction->operation.fp.predicate;
        break;
    case PVIR_OP_CONVERT:
        *type = instruction->operation.convert.source_type;
        break;
    case PVIR_OP_FIXED:
        *type = instruction->operation.fixed.type;
        *predicate = &instruction->operation.fixed.predicate;
        break;
    case PVIR_OP_LANE_INDEX:
        *type = instruction->operation.index.type;
        *predicate = &instruction->operation.index.predicate;
        break;
    case PVIR_OP_EXTRACT:
    case PVIR_OP_INSERT:
        *type = instruction->operation.lane.vector_type;
        break;
    default:
        *type = instruction->operation.vector.type;
        *predicate = &instruction->operation.vector.predicate;
        break;
    }
}

static void pvir_execute_impl(CPUArchState *env,
                              const PvirInstruction *instruction,
                              bool already_validated)
{
    uint64_t active[PVIR_MAX_LANES];
    PvirVectorType type;
    const PvirPredicate *predicate = NULL;
    size_t lanes = 0;
    unsigned i;
    float_status status;

    if (instruction == NULL) {
        return;
    }
    env->pvir_status = already_validated
                           ? PVIR_STATUS_OK
                           : pvir_instruction_validate(instruction);
    env->pvir_count++;
    if (env->pvir_status != PVIR_STATUS_OK) {
        return;
    }

    pvir_insn_type_and_predicate(instruction, &type, &predicate);
    if (pvir_vector_type_lanes(type, &lanes) != PVIR_STATUS_OK ||
        lanes > PVIR_MAX_LANES) {
        return;
    }
    if (predicate != NULL) {
        if (pvir_predicate_expand(predicate, type.element_bits,
                                  env->v_mask_words, 2, active,
                                  lanes) != PVIR_STATUS_OK) {
            return;
        }
    } else if (instruction->opcode == PVIR_OP_CONVERT &&
               env->v_mask_version != 0) {
        for (i = 0; i < lanes; i++) {
            unsigned bit =
                (unsigned)((env->v_mask_words[i / 64] >> (i % 64)) & 1u);
            active[i] = bit ? UINT64_MAX : 0;
        }
    } else {
        for (i = 0; i < lanes; i++) {
            active[i] = UINT64_MAX;
        }
    }

    if (instruction->opcode == PVIR_OP_EXTRACT) {
        const PvirLaneOperation *op = &instruction->operation.lane;
        env->rvv_scalar =
            pvir_operand(env, op->source, op->lane, op->vector_type.element_bits);
        return;
    }

    if (instruction->opcode == PVIR_OP_INSERT) {
        const PvirLaneOperation *op = &instruction->operation.lane;
        unsigned ebits = op->vector_type.element_bits;
        uint64_t val = pvir_operand(env, op->source, 0, ebits);
        for (i = 0; i < lanes; i++) {
            if (i == op->lane) {
                latr_rvv_write_elem(env, op->destination, i, ebits, val);
            } else if (op->destination != op->merge_value) {
                latr_rvv_write_elem(env, op->destination, i, ebits,
                                    pvir_operand(env, op->merge_value, i,
                                                 ebits));
            }
        }
        return;
    }

    if (instruction->opcode == PVIR_OP_LANE_INDEX) {
        const PvirIndexOperation *op = &instruction->operation.index;
        unsigned bits = op->type.element_bits;
        uint64_t mask = bits >= 64 ? UINT64_MAX : (((uint64_t)1 << bits) - 1);
        uint64_t start = pvir_operand(env, op->start_value, 0, bits);
        uint64_t step = op->step_value == PVIR_NO_VALUE
                            ? 1
                            : pvir_operand(env, op->step_value, 0, bits);
        for (i = (unsigned)env->vstart; i < lanes; i++) {
            if (!(active[i] & 1u)) {
                pvir_write_inactive(env, op->destination, op->inactive_policy,
                                    op->merge_value, i, bits);
                continue;
            }
            latr_rvv_write_elem(env, op->destination, i, bits,
                                (start + (uint64_t)i * step) & mask);
        }
        return;
    }

    if (instruction->opcode == PVIR_OP_FIXED) {
        const PvirFixedPointOperation *op = &instruction->operation.fixed;
        unsigned bits = op->type.element_bits;
        for (i = (unsigned)env->vstart; i < lanes; i++) {
            if (!(active[i] & 1u)) {
                pvir_write_inactive(env, op->destination, op->inactive_policy,
                                    op->merge_value, i, bits);
                continue;
            }
            latr_rvv_write_elem(env, op->destination, i, bits,
                                pvir_fixed_eval(env, op, i, bits));
        }
        return;
    }

    if (instruction->opcode == PVIR_OP_FP_ARITH) {
        const PvirFPOpOperation *op = &instruction->operation.fp;
        latr_fp_status_begin(env, &status, 0);
        if (op->rounding != PVIR_ROUND_DEFAULT) {
            set_float_rounding_mode(pvir_round_mode(op->rounding, env), &status);
        }
        for (i = (unsigned)env->vstart; i < lanes; i++) {
            uint64_t a = pvir_operand(env, op->operands[0], i,
                                      op->type.element_bits);
            uint64_t b = op->kind == PVIR_FP_SQRT
                             ? 0
                             : pvir_operand(env, op->operands[1], i,
                                            op->type.element_bits);
            uint64_t out;
            if (!(active[i] & 1u)) {
                pvir_write_inactive(env, op->destination, op->inactive_policy,
                                    op->merge_value, i, op->type.element_bits);
                continue;
            }
            out = pvir_fp_arith(op, a, b, &status);
            latr_rvv_write_elem(env, op->destination, i,
                                op->type.element_bits, out);
        }
        latr_fp_status_end(env, &status);
        return;
    }

    if (instruction->opcode == PVIR_OP_CONVERT) {
        const PvirConvertOperation *op = &instruction->operation.convert;
        int rod = op->rounding == PVIR_ROUND_ROD ||
                  (op->rounding == PVIR_ROUND_DEFAULT && env->rvv_imm == 1);
        latr_fp_status_begin(env, &status, op->rounding == PVIR_ROUND_RTZ);
        if (rod) {
            set_float_rounding_mode(float_round_to_odd, &status);
        } else if (op->rounding != PVIR_ROUND_DEFAULT) {
            set_float_rounding_mode(pvir_round_mode(op->rounding, env), &status);
        }
        for (i = (unsigned)env->vstart; i < lanes; i++) {
            if (!(active[i] & 1u)) {
                continue;
            }
            {
                uint64_t src = latr_rvv_read_elem(
                    env, op->source, i, op->source_type.element_bits);
                uint64_t out = pvir_convert(op, src, &status);
                latr_rvv_write_elem(env, op->destination, i,
                                    op->destination_type.element_bits, out);
            }
        }
        latr_fp_status_end(env, &status);
        return;
    }

    if (instruction->opcode == PVIR_OP_LOAD ||
        instruction->opcode == PVIR_OP_STORE) {
        const PvirVectorOperation *op = &instruction->operation.vector;
        int is_write = instruction->opcode == PVIR_OP_STORE;
        uint64_t base = pvir_resolve_base(env, &op->memory_address);
        unsigned mbytes = op->memory_element_bits / 8u;
        unsigned ebits = op->type.element_bits;
        uint64_t emask =
            ebits >= 64 ? UINT64_MAX : (((uint64_t)1 << ebits) - 1);

        env->mem_fault_cause = 0;
        if (op->type.kind == PVIR_ELEMENT_MASK && mbytes == 1u) {
            unsigned bytes = (unsigned)((lanes + 7u) / 8u);
            unsigned b;
            for (b = (unsigned)env->vstart / 8u; b < bytes; b++) {
                uint64_t addr = base + b;
                if (is_write) {
                    uint64_t val = env->vreg[op->operands[0] & 31u][b];
                    pvir_mem_rw_lane(env, addr, 1, 1, val);
                } else {
                    uint64_t val = pvir_mem_rw_lane(env, addr, 1, 0, 0);
                    if (!env->mem_fault_cause) {
                        env->vreg[op->destination & 31u][b] = (uint8_t)val;
                    }
                }
                if (env->mem_fault_cause) {
                    env->vstart = b * 8u;
                    return;
                }
            }
            return;
        }
        for (i = (unsigned)env->vstart; i < lanes; i++) {
            uint64_t addr;
            if (!(active[i] & 1u)) {
                if (!is_write) {
                    pvir_write_inactive(env, op->destination,
                                        op->inactive_policy, op->merge_value,
                                        i, ebits);
                }
                continue;
            }
            addr = base + (uint64_t)i * mbytes;
            if (is_write) {
                uint64_t val = pvir_operand(env, op->operands[0], i, ebits);
                pvir_mem_rw_lane(env, addr, mbytes, 1, val);
                if (env->mem_fault_cause) {
                    env->vstart = i;
                    return;
                }
            } else {
                uint64_t val = pvir_mem_rw_lane(env, addr, mbytes, 0, 0);
                if (env->mem_fault_cause) {
                    env->vstart = i;
                    return;
                }
                if (op->type.signedness == PVIR_SIGNED &&
                    op->memory_element_bits < ebits) {
                    val = (uint64_t)pvir_sign(val, op->memory_element_bits);
                }
                latr_rvv_write_elem(env, op->destination, i, ebits,
                                    val & emask);
            }
        }
        return;
    }

    {
        const PvirVectorOperation *op = &instruction->operation.vector;
        PvirOpcode opcode = instruction->opcode;
        unsigned bits = op->type.element_bits;
        int is_mask_dest = op->type.kind == PVIR_ELEMENT_MASK;

        for (i = (unsigned)env->vstart; i < lanes; i++) {
            uint64_t a;
            uint64_t b;
            uint64_t out;

            if (!(active[i] & 1u)) {
                if (op->inactive_policy == PVIR_INACTIVE_MERGE) {
                    if (is_mask_dest) {
                        uint64_t m = pvir_operand(env, op->merge_value, i, 1);
                        if (m) {
                            env->vreg[op->destination & 31u][i >> 3] |=
                                (uint8_t)(1u << (i & 7u));
                        }
                    } else {
                        latr_rvv_write_elem(env, op->destination, i, bits,
                                            pvir_operand(env, op->merge_value,
                                                         i, bits));
                    }
                } else if (op->inactive_policy == PVIR_INACTIVE_ZERO &&
                           !is_mask_dest) {
                    latr_rvv_write_elem(env, op->destination, i, bits, 0);
                }
                continue;
            }
            if (opcode == PVIR_OP_COMPARE) {
                unsigned ebits = op->operand_types[0].element_bits;
                PvirVectorType stype = op->operand_types[0];
                if (ebits == 0) {
                    ebits = bits;
                }
                a = pvir_operand(env, op->operands[0], i, ebits);
                b = pvir_operand(env, op->operands[1], i, ebits);
                if (stype.kind == PVIR_ELEMENT_FLOAT) {
                    float_status fs;
                    latr_fp_status_begin(env, &fs, 0);
                    out = pvir_fp_compare(op->compare_condition, a, b, ebits,
                                          &fs);
                    latr_fp_status_end(env, &fs);
                } else if (op->compare_condition == PVIR_COMPARE_EQ) {
                    out = a == b;
                } else if (op->compare_condition == PVIR_COMPARE_NE) {
                    out = a != b;
                } else if (op->compare_condition == PVIR_COMPARE_LT) {
                    out = pvir_sign(a, ebits) < pvir_sign(b, ebits);
                } else if (op->compare_condition == PVIR_COMPARE_LE) {
                    out = pvir_sign(a, ebits) <= pvir_sign(b, ebits);
                } else if (op->compare_condition == PVIR_COMPARE_GT) {
                    out = pvir_sign(a, ebits) > pvir_sign(b, ebits);
                } else if (op->compare_condition == PVIR_COMPARE_GE) {
                    out = pvir_sign(a, ebits) >= pvir_sign(b, ebits);
                } else if (op->compare_condition == PVIR_COMPARE_LTU) {
                    out = a < b;
                } else if (op->compare_condition == PVIR_COMPARE_LEU) {
                    out = a <= b;
                } else if (op->compare_condition == PVIR_COMPARE_GTU) {
                    out = a > b;
                } else {
                    out = a >= b;
                }
                if (out) {
                    env->vreg[op->destination & 31u][i >> 3] |=
                        (uint8_t)(1u << (i & 7u));
                } else {
                    env->vreg[op->destination & 31u][i >> 3] &=
                        (uint8_t)~(1u << (i & 7u));
                }
                continue;
            }
            if (opcode == PVIR_OP_SELECT) {
                uint64_t cond = pvir_operand(env, op->operands[0], i, bits);
                out = cond ? pvir_operand(env, op->operands[1], i, bits)
                           : pvir_operand(env, op->operands[2], i, bits);
                latr_rvv_write_elem(env, op->destination, i, bits, out);
                continue;
            }
            if (opcode == PVIR_OP_BROADCAST ||
                opcode == PVIR_OP_MOVE ||
                opcode == PVIR_OP_NOT ||
                opcode == PVIR_OP_EXTEND ||
                opcode == PVIR_OP_TRUNCATE) {
                unsigned src_bits = op->operand_types[0].element_bits ?
                                        op->operand_types[0].element_bits
                                        : bits;
                a = pvir_operand(env, op->operands[0], i, src_bits);
                if (opcode == PVIR_OP_NOT) {
                    out = ~a;
                } else if (opcode == PVIR_OP_EXTEND) {
                    out = (uint64_t)pvir_sign(a, src_bits);
                    if (op->operand_types[0].signedness != PVIR_SIGNED) {
                        out = a;
                    }
                } else {
                    out = a;
                }
                latr_rvv_write_elem(env, op->destination, i, bits, out);
                continue;
            }
            a = pvir_operand(env, op->operands[0], i, bits);
            b = pvir_operand(env, op->operands[1], i, bits);
            if (op->type.kind == PVIR_ELEMENT_FLOAT &&
                (opcode == PVIR_OP_ADD || opcode == PVIR_OP_SUB ||
                 opcode == PVIR_OP_MUL || opcode == PVIR_OP_DIV)) {
                PvirFPOpOperation fp = { 0 };
                float_status fs;
                fp.type = op->type;
                fp.kind = opcode == PVIR_OP_ADD ? PVIR_FP_ADD
                        : opcode == PVIR_OP_SUB ? PVIR_FP_SUB
                        : opcode == PVIR_OP_MUL ? PVIR_FP_MUL
                                                : PVIR_FP_DIV;
                latr_fp_status_begin(env, &fs, 0);
                out = pvir_fp_arith(&fp, a, b, &fs);
                latr_fp_status_end(env, &fs);
            } else {
                out = pvir_int_binop(opcode, op, a, b, bits);
            }
            out &= bits >= 64 ? UINT64_MAX : (((uint64_t)1 << bits) - 1);
            latr_rvv_write_elem(env, op->destination, i, bits, out);
        }
    }
}

void latr_pvir_execute(CPUArchState *env,
                       const PvirInstruction *instruction)
{
    pvir_execute_impl(env, instruction, false);
}

void latr_pvir_execute_cached(CPUArchState *env, uint64_t tag,
                              const PvirInstruction *instruction)
{
    PvirInstruction cached;
    uint64_t key_hash;

    if (env == NULL || instruction == NULL) {
        return;
    }
    key_hash = pvir_cache_tag_hash(env, tag);
    if (latr_pvir_cache_lookup(key_hash, env->vtype, env->vl, &cached)) {
        env->pvir_cache_hits++;
        pvir_execute_impl(env, &cached, true);
        return;
    }
    pvir_execute_impl(env, instruction, false);
    if (env->pvir_status == PVIR_STATUS_OK) {
        latr_pvir_cache_store(key_hash, env->vtype, env->vl, instruction);
    }
}
