/*
 * RVV baseline support: vtype/vl computation. The algorithm mirrors QEMU's
 * HELPER(vsetvl) (target/riscv/vector_helper.c) so the golden reference and
 * LATR agree, including the fractional-LMUL legality rule and the reserved
 * vtype bits.
 *
 * Layout (RVV 1.0, RV64):
 *   vtype[2:0]   vlmul   0..3 -> LMUL 1,2,4,8; 5..7 -> 1/8,1/4,1/2
 *   vtype[5:3]   vsew    SEW = 8 << vsew
 *   vtype[6]     vta
 *   vtype[7]     vma
 *   vtype[9:8]   vtype reserved (must be zero)
 *   vtype[63]    vill input (read back set when the type is illegal)
 */
#include "latr.h"
#include "translate.h"
#include "riscv-env.h"
#include "mmu.h"
#include "fpu/softfloat.h"

#define CSR_MSTATUS_ADDR 0x300
#define MSTATUS_VS_BITS (UINT64_C(3) << 9)

uint64_t latr_rvv_vsetvl(CPUArchState *env, uint64_t s1, uint64_t vtype)
{
    unsigned vlmul = (unsigned)(vtype & 7u);
    unsigned vsew = (unsigned)((vtype >> 3) & 7u);
    unsigned sew = 8u << vsew;
    unsigned ediv = (unsigned)((vtype >> 8) & 3u);
    uint64_t reserved = vtype & (UINT64_MAX << 10);
    bool vill = ((vtype >> 63) & 1u) != 0;
    unsigned vlen = LATR_RVV_VLEN;
    int lmul;
    int shift;
    uint64_t vlmax;
    uint64_t vl;

    if (vlmul & 4u) {
        /*
         * Fractional LMUL: VLEN * LMUL must hold at least SEW bits, and the
         * reserved encoding vlmul == 4 is illegal.
         */
        if (vlmul == 4u || (vlen >> (8u - vlmul)) < sew) {
            vill = true;
        }
    }
    /* vsetvl marks the vector state dirty even when it sets vill. */
    env->csr[CSR_MSTATUS_ADDR] |= MSTATUS_VS_BITS;

    if (sew > LATR_RVV_ELEN || vill || ediv != 0 || reserved != 0) {
        env->vill = 1;
        env->vtype = 0;
        env->vl = 0;
        env->vstart = 0;
        return 0;
    }

    lmul = (vlmul & 4u) ? (int)vlmul - 8 : (int)vlmul;
    shift = (int)vsew + 3 - lmul;
    vlmax = (uint64_t)vlen >> shift;
    vl = s1 <= vlmax ? s1 : vlmax;

    env->vl = vl;
    env->vtype = vtype & ~(UINT64_C(1) << 63);
    env->vstart = 0;
    env->vill = 0;
    return vl;
}

/*
 * Vector execution helpers.
 *
 * All helpers run in guest context and receive their parameters through
 * env->rvv_args so the emitted call never needs more than one argument.
 * Lanes outside [vstart, vl) are left undisturbed unless the relevant
 * agnostic bit (vta/vma) asks for all-ones, matching QEMU's default
 * rvv_ta_all_1s=false configuration for mask tails.
 */

#define RVV_VLEN            LATR_RVV_VLEN
#define RVV_MASK_WORDS      ((RVV_VLEN + 63) / 64)

static unsigned rvv_sew(const CPUArchState *env)
{
    return 8u << RVV_VTYPE_VSEW(env->vtype);
}

static int rvv_lmul(const CPUArchState *env)
{
    unsigned vlmul = RVV_VTYPE_VLMUL(env->vtype);

    return (vlmul & 4u) ? (int)vlmul - 8 : (int)vlmul;
}

static unsigned rvv_vlmax(const CPUArchState *env)
{
    return RVV_VLEN >> (RVV_VTYPE_VSEW(env->vtype) + 3 - rvv_lmul(env));
}

static unsigned rvv_elems_per_reg(unsigned sew)
{
    return RVV_VLEN / sew;
}

uint64_t latr_rvv_read_elem(const CPUArchState *env, unsigned reg,
                            unsigned lane, unsigned sew)
{
    unsigned per;
    unsigned idx;
    unsigned off;
    unsigned r;
    unsigned bytes;
    unsigned base;
    uint64_t value = 0;
    unsigned b;

    if (sew == 1u) {
        idx = lane / RVV_VLEN;
        off = lane % RVV_VLEN;
        r = reg + idx;
        if (r >= 32u) {
            return 0;
        }
        return (env->vreg[r][off >> 3] >> (off & 7u)) & 1u;
    }
    per = rvv_elems_per_reg(sew);
    idx = lane / per;
    off = lane % per;
    r = reg + idx;
    /* Group legality is enforced when the instruction is translated, so this
     * bound is a backstop: never wrap into another architectural register. */
    if (r >= 32u) {
        return 0;
    }
    bytes = sew / 8u;
    base = off * bytes;

    for (b = 0; b < bytes; b++) {
        value |= (uint64_t)env->vreg[r][base + b] << (8 * b);
    }
    return value;
}

void latr_rvv_write_elem(CPUArchState *env, unsigned reg, unsigned lane,
                         unsigned sew, uint64_t value)
{
    unsigned per;
    unsigned idx;
    unsigned off;
    unsigned r;
    unsigned bytes;
    unsigned base;
    unsigned b;

    if (sew == 1u) {
        uint8_t *cell;
        idx = lane / RVV_VLEN;
        off = lane % RVV_VLEN;
        r = reg + idx;
        if (r >= 32u) {
            return;
        }
        cell = (uint8_t *)&env->vreg[r][off >> 3];
        if (value & 1u) {
            *cell |= (uint8_t)(1u << (off & 7u));
        } else {
            *cell &= (uint8_t)~(1u << (off & 7u));
        }
        return;
    }
    per = rvv_elems_per_reg(sew);
    idx = lane / per;
    off = lane % per;
    r = reg + idx;
    if (r >= 32u) {
        return;
    }
    bytes = sew / 8u;
    base = off * bytes;

    for (b = 0; b < bytes; b++) {
        env->vreg[r][base + b] = (uint8_t)(value >> (8 * b));
    }
}

static uint64_t rvv_sew_mask(unsigned sew)
{
    return sew >= 64 ? UINT64_MAX : ((UINT64_C(1) << sew) - 1);
}

static int64_t rvv_sext(uint64_t value, unsigned sew)
{
    unsigned shift = 64 - sew;

    return (int64_t)(value << shift) >> shift;
}

static unsigned rvv_v0_bit(const CPUArchState *env, unsigned lane)
{
    return (env->vreg[0][lane >> 3] >> (lane & 7u)) & 1u;
}

static void rvv_build_mask(CPUArchState *env, int masked)
{
    unsigned i;

    for (i = 0; i < RVV_MASK_WORDS; i++) {
        env->v_mask_words[i] = 0;
    }
    /*
     * The predicate covers the whole [0, vl) range on purpose: lanes below
     * vstart are *already done*, not inactive, so they must not be mistaken
     * for vma-inactive lanes that need filling with ones.  The restart itself
     * is honoured further down -- pvir-exec.c runs its element loops from
     * env->vstart, and the LSX fast path re-checks vstart at run time in
     * latr_rvv_direct_prepare() because vstart is not part of the TB key.
     */
    for (i = 0; i < (unsigned)env->vl; i++) {
        if (!masked || rvv_v0_bit(env, i)) {
            env->v_mask_words[i / 64] |= UINT64_C(1) << (i % 64);
        }
    }
    env->v_mask_version++;
}

static unsigned rvv_mask_bit(const CPUArchState *env, unsigned lane)
{
    return (unsigned)((env->v_mask_words[lane / 64] >> (lane % 64)) & 1u);
}

static void rvv_tail_ones(CPUArchState *env, unsigned vd, unsigned sew)
{
    unsigned vlmax = rvv_vlmax(env);
    unsigned i;

    if (!RVV_VTYPE_VTA(env->vtype)) {
        return;
    }
    for (i = (unsigned)env->vl; i < vlmax; i++) {
        latr_rvv_write_elem(env, vd, i, sew, rvv_sew_mask(sew));
    }
}

static uint64_t rvv_scalar_value(const CPUArchState *env, unsigned kind,
                                 uint64_t value, unsigned sew)
{
    if (kind == LATR_RVV_SRC_SCALAR) {
        return env->gpr[value & 31u];
    }
    if (kind == LATR_RVV_SRC_IMM) {
        return value;
    }
    return value;
}

static void rvv_pvir_init(PvirInstruction *insn)
{
    memset(insn, 0, sizeof(*insn));
    insn->effects.fault.unknown_from_lane = PVIR_NO_VALUE;
}

static PvirPredicate rvv_pvir_mask(CPUArchState *env)
{
    PvirPredicateSource source;

    source.kind = PVIR_PREDICATE_SOURCE_CONSTANT;
    source.id = 0;
    source.version = 0;
    return pvir_predicate_bitset(source, (size_t)env->vl,
                                 env->v_mask_words, 2);
}

static PvirVectorType rvv_pvir_type(unsigned kind, unsigned signedness,
                                    unsigned bits, unsigned lanes)
{
    PvirVectorType type;

    type.kind = (PvirElementKind)kind;
    type.signedness = (PvirSignedness)signedness;
    type.element_bits = (uint16_t)bits;
    type.vector_bits = (size_t)bits * lanes;
    return type;
}

static void rvv_pvir_scalar(CPUArchState *env, unsigned kind, uint64_t imm)
{
    if (kind == LATR_RVV_SRC_SCALAR) {
        env->rvv_scalar = env->gpr[imm & 31u];
    } else if (kind == LATR_RVV_SRC_IMM) {
        env->rvv_imm = imm;
    }
}

static PvirCompareCondition rvv_pvir_cmp(unsigned cond)
{
    switch (cond) {
    case LATR_RVV_CMP_EQ:  return PVIR_COMPARE_EQ;
    case LATR_RVV_CMP_NE:  return PVIR_COMPARE_NE;
    case LATR_RVV_CMP_LT:  return PVIR_COMPARE_LT;
    case LATR_RVV_CMP_LTU: return PVIR_COMPARE_LTU;
    case LATR_RVV_CMP_LE:  return PVIR_COMPARE_LE;
    case LATR_RVV_CMP_LEU: return PVIR_COMPARE_LEU;
    case LATR_RVV_CMP_GT:  return PVIR_COMPARE_GT;
    case LATR_RVV_CMP_GTU: return PVIR_COMPARE_GTU;
    case LATR_RVV_CMP_GE:  return PVIR_COMPARE_GE;
    default:               return PVIR_COMPARE_GEU;
    }
}

static void rvv_pvir_fill_inactive(CPUArchState *env, unsigned vd,
                                   unsigned bits)
{
    unsigned i;

    if (!RVV_VTYPE_VMA(env->vtype)) {
        return;
    }
    for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
        if (!rvv_mask_bit(env, i)) {
            latr_rvv_write_elem(env, vd, i, bits, rvv_sew_mask(bits));
        }
    }
}

void latr_rvv_alu(CPUArchState *env)
{
    unsigned op = (unsigned)env->rvv_args[0];
    unsigned vd = (unsigned)env->rvv_args[1];
    unsigned vs1 = (unsigned)env->rvv_args[2];
    unsigned vs2 = (unsigned)env->rvv_args[3];
    unsigned kind = (unsigned)env->rvv_args[4];
    uint64_t imm = env->rvv_args[5];
    int vm = (int)env->rvv_args[6];
    unsigned sew = rvv_sew(env);
    unsigned signedness = (op == LATR_RVV_OP_DIVU || op == LATR_RVV_OP_REMU ||
                           op == LATR_RVV_OP_MINU || op == LATR_RVV_OP_MAXU)
                              ? PVIR_UNSIGNED
                              : PVIR_SIGNED;
    PvirInstruction insn;
    PvirVectorOperation *o = &insn.operation.vector;

    if (env->vstart >= env->vl) {
        env->vstart = 0;
        return;
    }
    rvv_build_mask(env, !vm);
    rvv_pvir_init(&insn);
    switch (op) {
    case LATR_RVV_OP_ADD:  insn.opcode = PVIR_OP_ADD; break;
    case LATR_RVV_OP_SUB:  insn.opcode = PVIR_OP_SUB; break;
    case LATR_RVV_OP_AND:  insn.opcode = PVIR_OP_AND; break;
    case LATR_RVV_OP_OR:   insn.opcode = PVIR_OP_OR; break;
    case LATR_RVV_OP_XOR:  insn.opcode = PVIR_OP_XOR; break;
    case LATR_RVV_OP_SLL:  insn.opcode = PVIR_OP_SHL; break;
    case LATR_RVV_OP_SRL:  insn.opcode = PVIR_OP_SHR; break;
    case LATR_RVV_OP_SRA:  insn.opcode = PVIR_OP_SAR; break;
    case LATR_RVV_OP_MUL:  insn.opcode = PVIR_OP_MUL; break;
    case LATR_RVV_OP_DIVU:
    case LATR_RVV_OP_DIV:  insn.opcode = PVIR_OP_DIV; break;
    case LATR_RVV_OP_REMU:
    case LATR_RVV_OP_REM:  insn.opcode = PVIR_OP_REM; break;
    case LATR_RVV_OP_MINU:
    case LATR_RVV_OP_MIN:  insn.opcode = PVIR_OP_MIN; break;
    case LATR_RVV_OP_MAXU:
    case LATR_RVV_OP_MAX:  insn.opcode = PVIR_OP_MAX; break;
    default: env->vstart = 0; return;
    }
    o->type = rvv_pvir_type(PVIR_ELEMENT_INTEGER, signedness,
                            sew, (unsigned)env->vl);
    o->predicate = rvv_pvir_mask(env);
    o->inactive_policy = PVIR_INACTIVE_UNKNOWN;
    o->merge_value = PVIR_NO_VALUE;
    o->destination = vd;
    o->operand_count = 2;
    o->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    o->operands[0] = vs1;
    rvv_pvir_scalar(env, kind, imm);
    if (kind == LATR_RVV_SRC_VECTOR) {
        o->operand_kinds[1] = PVIR_OPERAND_VECTOR;
        o->operands[1] = vs2;
    } else {
        o->operand_kinds[1] = PVIR_OPERAND_SCALAR;
        o->operands[1] = kind == LATR_RVV_SRC_SCALAR ? LATR_PVIR_SCALAR_VALUE
                                                     : LATR_PVIR_IMM_VALUE;
    }
    latr_pvir_execute_cached(env, UINT64_C(0x100) ^ op, &insn);
    if (env->pvir_status == PVIR_STATUS_OK) {
        rvv_pvir_fill_inactive(env, vd, sew);
    }
    env->vstart = 0;
    rvv_tail_ones(env, vd, sew);
}

void latr_rvv_cmp(CPUArchState *env)
{
    unsigned cond = (unsigned)env->rvv_args[0];
    unsigned vd = (unsigned)env->rvv_args[1];
    unsigned vs1 = (unsigned)env->rvv_args[2];
    unsigned vs2 = (unsigned)env->rvv_args[3];
    unsigned kind = (unsigned)env->rvv_args[4];
    uint64_t imm = env->rvv_args[5];
    int vm = (int)env->rvv_args[6];
    unsigned sew = rvv_sew(env);
    unsigned signedness = (cond == LATR_RVV_CMP_LTU ||
                           cond == LATR_RVV_CMP_LEU ||
                           cond == LATR_RVV_CMP_GTU ||
                           cond == LATR_RVV_CMP_GEU)
                              ? PVIR_UNSIGNED
                              : PVIR_SIGNED;
    PvirInstruction insn;
    PvirVectorOperation *o = &insn.operation.vector;

    if (env->vstart >= env->vl) {
        env->vstart = 0;
        return;
    }
    rvv_build_mask(env, !vm);
    rvv_pvir_init(&insn);
    insn.opcode = PVIR_OP_COMPARE;
    o->type = rvv_pvir_type(PVIR_ELEMENT_MASK, PVIR_SIGNLESS, 1,
                            (unsigned)env->vl);
    o->predicate = rvv_pvir_mask(env);
    o->inactive_policy = PVIR_INACTIVE_UNKNOWN;
    o->merge_value = PVIR_NO_VALUE;
    o->compare_condition = rvv_pvir_cmp(cond);
    o->destination = vd;
    o->operand_count = 2;
    o->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    o->operands[0] = vs1;
    o->operand_types[0] = rvv_pvir_type(PVIR_ELEMENT_INTEGER, signedness,
                                        sew, (unsigned)env->vl);
    o->operand_kinds[1] = PVIR_OPERAND_VECTOR;
    rvv_pvir_scalar(env, kind, imm);
    o->operands[1] = kind == LATR_RVV_SRC_VECTOR
                         ? vs2
                         : (kind == LATR_RVV_SRC_SCALAR
                                ? LATR_PVIR_SCALAR_VALUE
                                : LATR_PVIR_IMM_VALUE);
    o->operand_types[1] = o->operand_types[0];
    latr_pvir_execute_cached(env, UINT64_C(0x200) ^ cond, &insn);
    if (env->pvir_status == PVIR_STATUS_OK && RVV_VTYPE_VMA(env->vtype)) {
        unsigned i;
        for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
            if (!rvv_mask_bit(env, i)) {
                env->vreg[vd & 31u][i >> 3] |= (uint8_t)(1u << (i & 7u));
            }
        }
    }
    env->vstart = 0;
}

void latr_rvv_mv(CPUArchState *env)
{
    unsigned vd = (unsigned)env->rvv_args[0];
    unsigned vs1 = (unsigned)env->rvv_args[1];
    unsigned kind = (unsigned)env->rvv_args[2];
    uint64_t imm = env->rvv_args[3];
    unsigned sew = rvv_sew(env);
    PvirInstruction insn;
    PvirVectorOperation *o = &insn.operation.vector;

    if (env->vstart >= env->vl) {
        env->vstart = 0;
        return;
    }
    rvv_pvir_init(&insn);
    o->type = rvv_pvir_type(PVIR_ELEMENT_INTEGER, PVIR_SIGNED, sew,
                            (unsigned)env->vl);
    o->predicate = pvir_predicate_all_true((size_t)env->vl);
    o->inactive_policy = PVIR_INACTIVE_UNKNOWN;
    o->merge_value = PVIR_NO_VALUE;
    o->destination = vd;
    o->operand_count = 1;
    if (kind == LATR_RVV_SRC_VECTOR) {
        insn.opcode = PVIR_OP_MOVE;
        o->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        o->operands[0] = vs1;
    } else {
        insn.opcode = PVIR_OP_BROADCAST;
        o->operand_kinds[0] = PVIR_OPERAND_SCALAR;
        rvv_pvir_scalar(env, kind, imm);
        o->operands[0] = kind == LATR_RVV_SRC_SCALAR
                             ? LATR_PVIR_SCALAR_VALUE
                             : LATR_PVIR_IMM_VALUE;
    }
    latr_pvir_execute_cached(env, UINT64_C(0x280) ^ kind, &insn);
    env->vstart = 0;
    rvv_tail_ones(env, vd, sew);
}

void latr_rvv_merge(CPUArchState *env)
{
    unsigned vd = (unsigned)env->rvv_args[0];
    unsigned vs1 = (unsigned)env->rvv_args[1];
    unsigned vs2 = (unsigned)env->rvv_args[2];
    unsigned kind = (unsigned)env->rvv_args[3];
    uint64_t imm = env->rvv_args[4];
    unsigned sew = rvv_sew(env);
    PvirInstruction insn;
    PvirVectorOperation *o = &insn.operation.vector;

    if (env->vstart >= env->vl) {
        env->vstart = 0;
        return;
    }
    rvv_pvir_init(&insn);
    insn.opcode = PVIR_OP_SELECT;
    o->type = rvv_pvir_type(PVIR_ELEMENT_INTEGER, PVIR_SIGNED, sew,
                            (unsigned)env->vl);
    o->predicate = pvir_predicate_all_true((size_t)env->vl);
    o->inactive_policy = PVIR_INACTIVE_UNKNOWN;
    o->merge_value = PVIR_NO_VALUE;
    o->destination = vd;
    o->operand_count = 3;
    o->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    o->operands[0] = LATR_PVIR_V0_VALUE;
    o->operand_kinds[1] = PVIR_OPERAND_VECTOR;
    o->operands[1] = vs2;
    o->operand_kinds[2] = PVIR_OPERAND_VECTOR;
    rvv_pvir_scalar(env, kind, imm);
    o->operands[2] = kind == LATR_RVV_SRC_VECTOR
                         ? vs1
                         : (kind == LATR_RVV_SRC_SCALAR
                                ? LATR_PVIR_SCALAR_VALUE
                                : LATR_PVIR_IMM_VALUE);
    o->operand_types[0] = o->operand_types[1] = o->operand_types[2] =
        o->type;
    latr_pvir_execute_cached(env, UINT64_C(0x2c0) ^ kind, &insn);
    env->vstart = 0;
    rvv_tail_ones(env, vd, sew);
}

/* Load/store one lane through the unified page/MMIO path. */
static uint64_t rvv_load_lane(CPUArchState *env, uint64_t addr, unsigned bytes,
                              uint64_t *value)
{
    return (uint64_t)latr_guest_load(env, addr, bytes, value);
}

static uint64_t rvv_store_lane(CPUArchState *env, uint64_t addr,
                               unsigned bytes, uint64_t value)
{
    return (uint64_t)latr_guest_store(env, addr, bytes, value);
}

/* ---- byte view of the vector register file ------------------------------ */

/*
 * Segment loads and stores address the register file in bytes: field k of a
 * segment occupies the VLEN*EMUL bits that follow field k-1, and EMUL may be
 * fractional. QEMU's vext_ldst_us() computes the same offset as
 * `vd + k * max_elems + i` elements, which is byte offset
 * `(k * VLENB * EMUL) + i * esz` from register vd. Working in bytes keeps the
 * fractional-EMUL cases (e.g. e8 with SEW=64) exact.
 */
static unsigned rvv_emul_bytes(const CPUArchState *env, unsigned eew)
{
    int emul = (int)eew - (int)RVV_VTYPE_VSEW(env->vtype) + rvv_lmul(env);

    if (emul >= 0) {
        return LATR_RVV_VLENB << (unsigned)emul;
    }
    return LATR_RVV_VLENB >> (unsigned)(-emul);
}

static uint64_t rvv_get_bytes(const CPUArchState *env, unsigned off,
                              unsigned bytes)
{
    uint64_t value = 0;
    unsigned b;

    if (off + bytes > 32u * LATR_RVV_VLENB) {
        return 0;
    }
    for (b = 0; b < bytes; b++) {
        unsigned pos = off + b;

        value |= (uint64_t)env->vreg[pos / LATR_RVV_VLENB][pos % LATR_RVV_VLENB]
                 << (8 * b);
    }
    return value;
}

static void rvv_put_bytes(CPUArchState *env, unsigned off, unsigned bytes,
                          uint64_t value)
{
    unsigned b;

    if (off + bytes > 32u * LATR_RVV_VLENB) {
        return;
    }
    for (b = 0; b < bytes; b++) {
        unsigned pos = off + b;

        env->vreg[pos / LATR_RVV_VLENB][pos % LATR_RVV_VLENB] =
            (uint8_t)(value >> (8 * b));
    }
}

/*
 * Segment (nf > 1) unit-stride, strided and indexed load/store.
 *
 * `mode` selects the address formula: 0 unit-stride (fields are interleaved
 * lane by lane, stride nf*EEW), 1 strided (base + i*stride + k*EEW) and
 * 2 indexed (base + index[i] + k*EEW). vstart is a *lane* index here: a fault
 * names the lane and the fields already transferred for that lane stay
 * written, exactly like QEMU's per-(i, k) emission order.
 */
static uint64_t rvv_ldst_segment(CPUArchState *env, uint64_t base, unsigned vd,
                                 unsigned eew, unsigned nf, int is_load,
                                 int vm, unsigned mode, uint64_t stride,
                                 unsigned index_reg)
{
    unsigned esz = 1u << eew;
    unsigned bits = esz * 8u;
    unsigned span = rvv_emul_bytes(env, eew);
    unsigned vd_off = (vd & 31u) * LATR_RVV_VLENB;
    unsigned start = (unsigned)env->vstart;
    unsigned i;
    unsigned k;

    env->mem_fault_cause = 0;
    if (start >= (unsigned)env->vl) {
        env->vstart = 0;
        return 0;
    }
    rvv_build_mask(env, !vm);
    for (i = start; i < (unsigned)env->vl; i++) {
        if (!rvv_mask_bit(env, i)) {
            if (RVV_VTYPE_VMA(env->vtype)) {
                for (k = 0; k < nf; k++) {
                    rvv_put_bytes(env, vd_off + k * span + i * esz, esz,
                                  rvv_sew_mask(bits));
                }
            }
            continue;
        }
        for (k = 0; k < nf; k++) {
            unsigned off = vd_off + k * span + i * esz;
            uint64_t addr;
            uint64_t value = 0;
            uint64_t cause;

            if (mode == 0) {
                addr = base + (uint64_t)(i * nf + k) * esz;
            } else if (mode == 1) {
                addr = base + stride * (uint64_t)i + (uint64_t)k * esz;
            } else {
                addr = base + stride + (uint64_t)k * esz;
            }
            if (is_load) {
                if (mode == 2 && k == 0) {
                    /* The index vector is read once per lane, before any field
                     * of that lane touches memory. */
                    stride = latr_rvv_read_elem(env, index_reg, i, esz * 8u);
                    addr = base + stride;
                }
                cause = rvv_load_lane(env, addr, esz, &value);
                if (cause) {
                    env->vstart = i;
                    return cause;
                }
                rvv_put_bytes(env, off, esz, value);
            } else {
                if (mode == 2 && k == 0) {
                    stride = latr_rvv_read_elem(env, index_reg, i, esz * 8u);
                    addr = base + stride;
                }
                value = rvv_get_bytes(env, off, esz);
                cause = rvv_store_lane(env, addr, esz, value);
                if (cause) {
                    env->vstart = i;
                    return cause;
                }
            }
        }
    }
    env->vstart = 0;
    if (is_load && RVV_VTYPE_VTA(env->vtype)) {
        for (k = 0; k < nf; k++) {
            unsigned off = vd_off + k * span + (unsigned)env->vl * esz;

            if (off + span > 32u * LATR_RVV_VLENB) {
                continue;
            }
            memset(&env->vreg[off / LATR_RVV_VLENB][off % LATR_RVV_VLENB], 0xff,
                   span - (unsigned)env->vl * esz);
        }
    }
    return 0;
}

/*
 * Unit-stride vector load/store (vle/vse, nf=1) and the mask forms.
 * Returns the exception cause; on a fault env->vstart is the faulting lane
 * and env->mem_va holds the element address, ready for latr_raise_mem_fault.
 *
 * rvv_args[5] selects the flavour: 0 unit-stride elements, 1 mask register
 * (vlm.v/vsm.v, byte granular) and 2 fault-only-first (vle<eew>ff.v).
 * rvv_args[6] is the segment field count nf.
 */
uint64_t latr_rvv_mem(CPUArchState *env)
{
    uint64_t base = env->rvv_args[0];
    unsigned vd = (unsigned)env->rvv_args[1];
    unsigned eew = (unsigned)env->rvv_args[2];
    int is_load = (int)env->rvv_args[3];
    int vm = (int)env->rvv_args[4];
    unsigned flavour = (unsigned)env->rvv_args[5];
    unsigned nf = (unsigned)env->rvv_args[6];
    unsigned bytes = 1u << eew;
    unsigned start = (unsigned)env->vstart;
    PvirInstruction insn;
    PvirVectorOperation *o = &insn.operation.vector;

    if (nf == 0) {
        nf = 1;
    }
    env->mem_fault_cause = 0;

    if (flavour == 1) {
        /*
         * vlm.v / vsm.v: one bit per lane, but the transfer is byte granular.
         * QEMU's vext_ldst_mask() moves ceil(vl/8) whole bytes and leaves the
         * remaining bits of the last byte exactly as memory had them.
         *
         * The tail is *not* filled even when vta=1: QEMU defaults to
         * cfg_vta_all_1s=false, so a tail-agnostic destination keeps its old
         * contents (same rule DESIGN.md §5.4 uses for the vector families, and
         * the one `DESIGN.md` "掩码" note already relies on).  Probe #8 read
         * v8[1]=v8[15]=0x03 with vta=ma *and* with vta=ta, confirming it.
         *
         * vstart counts bytes here, not lanes.
         */
        unsigned nbytes = ((unsigned)env->vl + 7u) / 8u;
        unsigned i;

        if (nbytes > LATR_RVV_VLENB) {
            nbytes = LATR_RVV_VLENB;
        }
        for (i = start; i < nbytes; i++) {
            uint64_t value = 0;
            uint64_t cause;

            if (is_load) {
                cause = rvv_load_lane(env, base + i, 1, &value);
                if (cause) {
                    env->vstart = i;
                    return cause;
                }
                env->vreg[vd & 31u][i] = (uint8_t)value;
            } else {
                cause = rvv_store_lane(env, base + i, 1,
                                       env->vreg[vd & 31u][i]);
                if (cause) {
                    env->vstart = i;
                    return cause;
                }
            }
        }
        env->vstart = 0;
        return 0;
    }

    if (nf > 1) {
        return rvv_ldst_segment(env, base, vd, eew, nf, is_load, vm, 0, 0, 0);
    }

    if (start >= (unsigned)env->vl) {
        env->vstart = 0;
        return 0;
    }
    rvv_build_mask(env, !vm);
    rvv_pvir_init(&insn);
    insn.opcode = is_load ? PVIR_OP_LOAD : PVIR_OP_STORE;
    o->type = rvv_pvir_type(PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED,
                            bytes * 8u, (unsigned)env->vl);
    o->predicate = rvv_pvir_mask(env);
    o->inactive_policy = is_load ? PVIR_INACTIVE_UNKNOWN
                                 : PVIR_INACTIVE_NOT_APPLICABLE;
    o->merge_value = PVIR_NO_VALUE;
    o->destination = is_load ? vd : PVIR_NO_VALUE;
    o->operand_count = is_load ? 0 : 1;
    if (!is_load) {
        o->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        o->operands[0] = vd;
        o->operand_types[0] = o->type;
    }
    o->memory_address.present = true;
    o->memory_address.base_kind = PVIR_MEMORY_BASE_VALUE;
    o->memory_address.base_value = LATR_PVIR_SCALAR_VALUE;
    o->memory_element_bits = bytes * 8u;
    insn.effects.memory.access =
        is_load ? PVIR_MEMORY_READ : PVIR_MEMORY_WRITE;
    insn.effects.memory.ordering = PVIR_MEMORY_ORDER_RELAXED;
    insn.effects.memory.inactive_lanes = PVIR_INACTIVE_MEMORY_NO_TRANSACTION;
    env->rvv_scalar = base;
    latr_pvir_execute_cached(env,
                             UINT64_C(0x380) ^ ((unsigned)is_load << 1),
                             &insn);
    if (env->pvir_status == PVIR_STATUS_OK && RVV_VTYPE_VMA(env->vtype)) {
        unsigned stop =
            env->mem_fault_cause ? (unsigned)env->vstart : (unsigned)env->vl;
        unsigned k;
        for (k = start; k < stop; k++) {
            if (!rvv_mask_bit(env, k)) {
                latr_rvv_write_elem(env, vd, k, bytes * 8u,
                                    rvv_sew_mask(bytes * 8u));
            }
        }
    }
    if (env->mem_fault_cause) {
        return env->mem_fault_cause;
    }
    env->vstart = 0;
    if (is_load) {
        rvv_tail_ones(env, vd, bytes * 8u);
    }
    return 0;
}

/*
 * Fault-only-first unit-stride load (vle<eew>ff.v).
 *
 * A fault is not reported: vl is narrowed to the first lane that could not be
 * loaded and the instruction retires normally, so the translator has to leave
 * the translation block (vl is part of its specialization key). Returns 1 when
 * vl changed and the TB must exit, 0 otherwise.
 *
 * QEMU's vext_ldff() leaves masked-off and tail lanes undisturbed, so neither
 * vma nor vta applies here.
 */
uint64_t latr_rvv_ldff(CPUArchState *env)
{
    uint64_t base = env->rvv_args[0];
    unsigned vd = (unsigned)env->rvv_args[1];
    unsigned eew = (unsigned)env->rvv_args[2];
    int vm = (int)env->rvv_args[4];
    unsigned bytes = 1u << eew;
    unsigned start = (unsigned)env->vstart;
    unsigned i;

    env->mem_fault_cause = 0;
    for (i = start; i < (unsigned)env->vl; i++) {
        uint64_t value = 0;
        uint64_t cause;

        if (!vm && !rvv_v0_bit(env, i)) {
            continue;
        }
        cause = rvv_load_lane(env, base + (uint64_t)i * bytes, bytes, &value);
        if (cause) {
            env->vl = i;
            env->vstart = 0;
            env->mem_fault_cause = 0;
            return 1;
        }
        latr_rvv_write_elem(env, vd, i, bytes * 8u, value);
    }
    env->vstart = 0;
    if (RVV_VTYPE_VTA(env->vtype)) {
        rvv_tail_ones(env, vd, bytes * 8u);
    }
    return 0;
}

/*
 * Whole-register transfers that ignore vl and vtype.
 *
 * vl<n>re<eew>.v loads (and vs<n>r.v stores) nf complete registers as one
 * contiguous VLEN*nf-bit block starting at rs1.  The block size comes from nf
 * alone: SEW, LMUL and the instruction's EEW only decide how the block is cut
 * into elements (and therefore what vstart counts).  Probe #10 pinned this
 * down -- with SEW=8 and vtype untouched, `vl2re32.v v4` filled v4/v5 and left
 * v6..v11 alone, and `vs2r.v` stopped at byte 31.
 *
 * vstart is an element index inside that block, so the restart offset is
 * vstart*esz rather than a lane of the current vtype.
 */
uint64_t latr_rvv_whole_mem(CPUArchState *env)
{
    uint64_t base = env->rvv_args[0];
    unsigned vd = (unsigned)env->rvv_args[1];
    unsigned eew = (unsigned)env->rvv_args[2];
    int is_load = (int)env->rvv_args[3];
    unsigned nf = (unsigned)env->rvv_args[4];
    unsigned esz = 1u << eew;
    unsigned per_reg = LATR_RVV_VLENB >> eew; /* elements in one register */
    unsigned evl = nf * per_reg;
    unsigned off = (vd & 31u) * LATR_RVV_VLENB;
    unsigned i;

    env->mem_fault_cause = 0;
    if ((unsigned)env->vstart >= evl) { /* vstart past the end: nothing to do */
        env->vstart = 0;
        return 0;
    }
    for (i = (unsigned)env->vstart; i < evl; i++) {
        unsigned pos = off + i * esz;
        uint64_t value = 0;
        uint64_t cause;

        if (is_load) {
            cause = rvv_load_lane(env, base + (uint64_t)i * esz, esz, &value);
            if (cause) {
                env->vstart = i;
                return cause;
            }
            rvv_put_bytes(env, pos, esz, value);
        } else {
            value = rvv_get_bytes(env, pos, esz);
            cause = rvv_store_lane(env, base + (uint64_t)i * esz, esz, value);
            if (cause) {
                env->vstart = i;
                return cause;
            }
        }
    }
    env->vstart = 0;
    return 0;
}

/*
 * Group copy for vmv<n>r.v.  Only alignment is constrained by the ISA, so the
 * two groups may overlap and the transfer has to pass through a scratch buffer.
 * memmove is not in the freestanding symbol set (latr_externals.allowlist), so
 * the copy is spelled out here.
 */
static void rvv_copy_whole(uint8_t *dst, const uint8_t *src, unsigned bytes)
{
    uint8_t scratch[8u * LATR_RVV_VLENB];
    unsigned i;

    if (bytes > sizeof(scratch)) {
        bytes = (unsigned)sizeof(scratch);
    }
    for (i = 0; i < bytes; i++) {
        scratch[i] = src[i];
    }
    for (i = 0; i < bytes; i++) {
        dst[i] = scratch[i];
    }
}

/* vmv<n>r.v: copy nf complete registers, vd and vs2 group aligned. */
void latr_rvv_whole_move(CPUArchState *env)
{
    unsigned vd = (unsigned)env->rvv_args[0];
    unsigned vs2 = (unsigned)env->rvv_args[1];
    unsigned nf = (unsigned)env->rvv_args[2];
    unsigned bytes = nf * LATR_RVV_VLENB;

    /*
     * There is no partial whole-register move: with vstart != 0 the
     * instruction transfers nothing but still consumes vstart.  QEMU's
     * trans_vmv_nr() guards the body with `s->vstart == 0` and clears vstart
     * through the generic gen_helper path; probe #8 case C confirmed it
     * (v4=v5 untouched, vstart read back as 0 afterwards).
     */
    if (env->vstart != 0) {
        env->vstart = 0;
        return;
    }
    if (vd + nf > 32u || vs2 + nf > 32u) {
        env->vstart = 0;
        return;
    }
    rvv_copy_whole(&env->vreg[vd][0], &env->vreg[vs2][0], bytes);
    env->vstart = 0;
}

/*
 * Strided (mode 1) and indexed (mode 2) vector load/store, nf=1.
 * Strided uses a sign-extended GPR stride; indexed uses zero-extended
 * elements of vs2 whose width is the instruction's EEW (8/16/32/64),
 * matching QEMU's GEN_VEXT_GET_INDEX_ADDR.
 */
uint64_t latr_rvv_stride_mem(CPUArchState *env)
{
    uint64_t base = env->rvv_args[0];
    unsigned vd = (unsigned)env->rvv_args[1];
    unsigned eew = (unsigned)env->rvv_args[2];
    int is_load = (int)env->rvv_args[3];
    int vm = (int)env->rvv_args[4];
    uint64_t stride_or_index = env->rvv_args[5];
    unsigned mode = (unsigned)env->rvv_args[6];
    unsigned nf = (unsigned)env->rvv_args[7];
    unsigned index_bytes = 1u << eew;
    unsigned data_bytes = mode == 1 ? index_bytes : rvv_sew(env) / 8u;
    unsigned data_bits = data_bytes * 8u;
    unsigned i;

    if (nf == 0) {
        nf = 1;
    }
    if (nf > 1) {
        /* vlsseg / vlsxseg: nf fields per lane, contiguous from the lane
         * address. `stride_or_index` is the byte stride (mode 1) or the index
         * vector register number (mode 2). */
        return rvv_ldst_segment(env, base, vd, eew, nf, is_load, vm, mode,
                                mode == 1 ? stride_or_index : 0,
                                (unsigned)stride_or_index);
    }

    env->mem_fault_cause = 0;
    if (env->vstart >= env->vl) {
        env->vstart = 0;
        return 0;
    }
    rvv_build_mask(env, !vm);
    for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
        uint64_t delta;
        uint64_t addr;
        uint64_t value = 0;
        uint64_t cause;

        if (!rvv_mask_bit(env, i)) {
            if (RVV_VTYPE_VMA(env->vtype)) {
                latr_rvv_write_elem(env, vd, i, data_bits,
                                    rvv_sew_mask(data_bits));
            }
            continue;
        }
        if (mode == 1) {
            delta = stride_or_index * (uint64_t)i;
        } else {
            delta = latr_rvv_read_elem(env, (unsigned)stride_or_index, i,
                                       index_bytes * 8u);
        }
        addr = base + delta;
        if (is_load) {
            cause = rvv_load_lane(env, addr, data_bytes, &value);
            if (cause) {
                env->vstart = i;
                return cause;
            }
            latr_rvv_write_elem(env, vd, i, data_bits, value);
        } else {
            value = latr_rvv_read_elem(env, vd, i, data_bits);
            cause = rvv_store_lane(env, addr, data_bytes, value);
            if (cause) {
                env->vstart = i;
                return cause;
            }
        }
    }
    env->vstart = 0;
    if (is_load) {
        rvv_tail_ones(env, vd, data_bits);
    }
    return 0;
}

/*
 * Mask-register logical instructions (vmand.mm family). They are never
 * masked by v0; QEMU computes OP(vs2_bit, vs1_bit) over [vstart, vl).
 */
static void rvv_mask_byte_op(CPUArchState *env, PvirOpcode opcode,
                             unsigned bytes, unsigned dst, unsigned a,
                             unsigned b, uint64_t tag)
{
    PvirInstruction insn;
    PvirVectorOperation *o = &insn.operation.vector;

    rvv_pvir_init(&insn);
    insn.opcode = opcode;
    o->type = rvv_pvir_type(PVIR_ELEMENT_BITS, PVIR_SIGNLESS, 8, bytes);
    o->predicate = pvir_predicate_all_true(bytes);
    o->inactive_policy = PVIR_INACTIVE_UNKNOWN;
    o->merge_value = PVIR_NO_VALUE;
    o->destination = dst;
    o->operand_count = opcode == PVIR_OP_NOT ? 1 : 2;
    o->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    o->operands[0] = a;
    if (opcode != PVIR_OP_NOT) {
        o->operand_kinds[1] = PVIR_OPERAND_VECTOR;
        o->operands[1] = b;
    }
    latr_pvir_execute_cached(env, tag, &insn);
}

void latr_rvv_mask_logic(CPUArchState *env)
{
    unsigned op = (unsigned)env->rvv_args[0];
    unsigned vd = (unsigned)env->rvv_args[1];
    unsigned vs1 = (unsigned)env->rvv_args[2];
    unsigned vs2 = (unsigned)env->rvv_args[3];
    unsigned bytes = ((unsigned)env->vl + 7u) / 8u;
    unsigned start = (unsigned)env->vstart;
    uint8_t orig_vd[LATR_RVV_VLENB];
    uint8_t saved_tmp[LATR_RVV_VLENB];
    unsigned i;

    if (start >= env->vl || bytes == 0 || bytes > LATR_RVV_VLENB) {
        env->vstart = 0;
        return;
    }
    memcpy(orig_vd, env->vreg[vd & 31u], bytes);
    env->vstart = 0;

    switch (op) {
    case LATR_RVV_MASK_AND:
        rvv_mask_byte_op(env, PVIR_OP_AND, bytes, vd, vs2, vs1, 0x410u);
        break;
    case LATR_RVV_MASK_OR:
        rvv_mask_byte_op(env, PVIR_OP_OR, bytes, vd, vs2, vs1, 0x411u);
        break;
    case LATR_RVV_MASK_XOR:
        rvv_mask_byte_op(env, PVIR_OP_XOR, bytes, vd, vs2, vs1, 0x412u);
        break;
    case LATR_RVV_MASK_NAND:
        rvv_mask_byte_op(env, PVIR_OP_AND, bytes, vd, vs2, vs1, 0x413u);
        rvv_mask_byte_op(env, PVIR_OP_NOT, bytes, vd, vd, 0, 0x414u);
        break;
    case LATR_RVV_MASK_NOR:
        rvv_mask_byte_op(env, PVIR_OP_OR, bytes, vd, vs2, vs1, 0x415u);
        rvv_mask_byte_op(env, PVIR_OP_NOT, bytes, vd, vd, 0, 0x416u);
        break;
    case LATR_RVV_MASK_XNOR:
        rvv_mask_byte_op(env, PVIR_OP_XOR, bytes, vd, vs2, vs1, 0x417u);
        rvv_mask_byte_op(env, PVIR_OP_NOT, bytes, vd, vd, 0, 0x418u);
        break;
    case LATR_RVV_MASK_ANDN:
    case LATR_RVV_MASK_ORN: {
        unsigned tmp = 0;
        while (tmp == (vd & 31u) || tmp == (vs1 & 31u) || tmp == (vs2 & 31u)) {
            tmp++;
        }
        memcpy(saved_tmp, env->vreg[tmp], bytes);
        rvv_mask_byte_op(env, PVIR_OP_NOT, bytes, tmp, vs1, 0, 0x419u);
        rvv_mask_byte_op(env,
                         op == LATR_RVV_MASK_ANDN ? PVIR_OP_AND : PVIR_OP_OR,
                         bytes, vd, vs2, tmp,
                         op == LATR_RVV_MASK_ANDN ? 0x41au : 0x41bu);
        memcpy(env->vreg[tmp], saved_tmp, bytes);
        break;
    }
    default:
        break;
    }

    for (i = 0; i < start; i++) {
        uint8_t bit = (uint8_t)(1u << (i & 7u));
        if (orig_vd[i >> 3] & bit) {
            env->vreg[vd & 31u][i >> 3] |= bit;
        } else {
            env->vreg[vd & 31u][i >> 3] &= (uint8_t)~bit;
        }
    }
    for (i = (unsigned)env->vl; i < bytes * 8u; i++) {
        uint8_t bit = (uint8_t)(1u << (i & 7u));
        if (orig_vd[i >> 3] & bit) {
            env->vreg[vd & 31u][i >> 3] |= bit;
        } else {
            env->vreg[vd & 31u][i >> 3] &= (uint8_t)~bit;
        }
    }
    if (RVV_VTYPE_VTA(env->vtype)) {
        for (i = (unsigned)env->vl; i < rvv_vlmax(env); i++) {
            env->vreg[vd & 31u][i >> 3] |= (uint8_t)(1u << (i & 7u));
        }
    }
}

/*
 * Mask scans: vcpop/vfirst write a GPR; vmsbf/vmsif/vmsof/viota/vid write a
 * vector destination. Masked-off lanes follow QEMU's vma handling.
 */
void latr_rvv_mask_scan(CPUArchState *env)
{
    unsigned op = (unsigned)env->rvv_args[0];
    unsigned rd = (unsigned)env->rvv_args[1];
    unsigned vs2 = (unsigned)env->rvv_args[2];
    int vm = (int)env->rvv_args[3];
    unsigned sew = rvv_sew(env);
    unsigned i;

    env->mem_fault_cause = 0;
    if (env->vstart >= env->vl) {
        env->vstart = 0;
        if (op == LATR_RVV_SCAN_VCPOP || op == LATR_RVV_SCAN_VFIRST) {
            env->gpr[rd & 31u] = op == LATR_RVV_SCAN_VFIRST ? UINT64_MAX : 0;
        }
        return;
    }
    rvv_build_mask(env, !vm);

    if (op == LATR_RVV_SCAN_VCPOP || op == LATR_RVV_SCAN_VFIRST) {
        uint64_t count = 0;
        uint64_t first = UINT64_MAX;

        for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
            if (!rvv_mask_bit(env, i)) {
                continue;
            }
            if ((env->vreg[vs2 & 31u][i >> 3] >> (i & 7u)) & 1u) {
                if (first == UINT64_MAX) {
                    first = i;
                }
                count++;
            }
        }
        env->gpr[rd & 31u] = op == LATR_RVV_SCAN_VFIRST ? first : count;
        env->vstart = 0;
        return;
    }

    if (op == LATR_RVV_SCAN_VID) {
        PvirInstruction insn;
        PvirIndexOperation *ix = &insn.operation.index;

        rvv_pvir_init(&insn);
        insn.opcode = PVIR_OP_LANE_INDEX;
        ix->type = rvv_pvir_type(PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED, sew,
                                 (unsigned)env->vl);
        ix->predicate = rvv_pvir_mask(env);
        ix->inactive_policy = PVIR_INACTIVE_UNKNOWN;
        ix->merge_value = PVIR_NO_VALUE;
        ix->destination = rd;
        ix->start_value = LATR_PVIR_IMM_VALUE;
        ix->step_value = PVIR_NO_VALUE;
        env->rvv_imm = 0;
        latr_pvir_execute_cached(env, UINT64_C(0x450), &insn);
        if (env->pvir_status == PVIR_STATUS_OK) {
            rvv_pvir_fill_inactive(env, rd, sew);
        }
        env->vstart = 0;
        rvv_tail_ones(env, rd, sew);
        return;
    }

    if (op == LATR_RVV_SCAN_VIOTA) {
        uint64_t sum = 0;

        for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
            if (!rvv_mask_bit(env, i)) {
                if (RVV_VTYPE_VMA(env->vtype)) {
                    latr_rvv_write_elem(env, rd, i, sew, rvv_sew_mask(sew));
                }
                continue;
            }
            latr_rvv_write_elem(env, rd, i, sew, sum);
            if ((env->vreg[vs2 & 31u][i >> 3] >> (i & 7u)) & 1u) {
                sum++;
            }
        }
        env->vstart = 0;
        rvv_tail_ones(env, rd, sew);
        return;
    }

    {
        int first_mask_bit = 0;

        for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
            unsigned result;
            unsigned src;

            if (!rvv_mask_bit(env, i)) {
                if (RVV_VTYPE_VMA(env->vtype)) {
                    env->vreg[rd & 31u][i >> 3] |= (uint8_t)(1u << (i & 7u));
                }
                continue;
            }
            src = (env->vreg[vs2 & 31u][i >> 3] >> (i & 7u)) & 1u;
            if (first_mask_bit) {
                result = 0;
            } else if (src) {
                first_mask_bit = 1;
                result = op == LATR_RVV_SCAN_VMSBF ? 0u : 1u;
            } else {
                result = op == LATR_RVV_SCAN_VMSOF ? 0u : 1u;
            }
            if (result) {
                env->vreg[rd & 31u][i >> 3] |= (uint8_t)(1u << (i & 7u));
            } else {
                env->vreg[rd & 31u][i >> 3] &= (uint8_t)~(1u << (i & 7u));
            }
        }
        env->vstart = 0;
        if (RVV_VTYPE_VTA(env->vtype)) {
            for (i = (unsigned)env->vl; i < rvv_vlmax(env); i++) {
                env->vreg[rd & 31u][i >> 3] |= (uint8_t)(1u << (i & 7u));
            }
        }
    }
}


static int64_t rvv_signed_value(uint64_t value, unsigned bits)
{
    return rvv_sext(value, bits);
}

/*
 * Integer and FP reductions (vred, vfred, vwred). The accumulator starts at
 * element 0 of vs1; only active lanes of vs2 are folded. The destination
 * element 0 holds the result and the remaining elements are tail-agnostic.
 */
void latr_rvv_reduce(CPUArchState *env)
{
    unsigned op = (unsigned)env->rvv_args[0];
    unsigned vd = (unsigned)env->rvv_args[1];
    unsigned vs2 = (unsigned)env->rvv_args[2];
    unsigned vs1 = (unsigned)env->rvv_args[3];
    int vm = (int)env->rvv_args[4];
    unsigned sew = rvv_sew(env);
    unsigned i;

    if (env->vstart >= env->vl) {
        env->vstart = 0;
        return;
    }
    rvv_build_mask(env, !vm);

    if (op == LATR_RVV_RED_FUSUM || op == LATR_RVV_RED_FOSUM ||
        op == LATR_RVV_RED_FMIN || op == LATR_RVV_RED_FMAX ||
        op == LATR_RVV_RED_FWUSUM || op == LATR_RVV_RED_FWOSUM) {
        int wide = op == LATR_RVV_RED_FWUSUM || op == LATR_RVV_RED_FWOSUM;
        unsigned acc_bits = wide ? sew * 2 : sew;
        float_status status;
        uint64_t acc;

        if ((!wide && sew != 32 && sew != 64) || (wide && sew != 32)) {
            env->vstart = 0;
            return;
        }
        latr_fp_status_begin(env, &status, 0);
        acc = latr_rvv_read_elem(env, vs1, 0, acc_bits);
        for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
            uint64_t src;
            if (!rvv_mask_bit(env, i)) {
                continue;
            }
            src = latr_rvv_read_elem(env, vs2, i, sew);
            if (!wide && sew == 32) {
                float32 a = (float32)acc;
                float32 b = (float32)src;
                if (op == LATR_RVV_RED_FMIN) {
                    a = float32_minimum_number(a, b, &status);
                } else if (op == LATR_RVV_RED_FMAX) {
                    a = float32_maximum_number(a, b, &status);
                } else {
                    a = float32_add(a, b, &status);
                }
                acc = (uint64_t)a;
            } else if (!wide) {
                float64 a = (float64)acc;
                float64 b = (float64)src;
                if (op == LATR_RVV_RED_FMIN) {
                    a = float64_minimum_number(a, b, &status);
                } else if (op == LATR_RVV_RED_FMAX) {
                    a = float64_maximum_number(a, b, &status);
                } else {
                    a = float64_add(a, b, &status);
                }
                acc = (uint64_t)a;
            } else {
                float64 a = (float64)acc;
                float64 b = (float64)(float32)src;
                a = float64_add(a, b, &status);
                acc = (uint64_t)a;
            }
        }
        latr_fp_status_end(env, &status);
        latr_rvv_write_elem(env, vd, 0, acc_bits, acc);
    } else {
        int wide = op == LATR_RVV_RED_WSUM || op == LATR_RVV_RED_WSUMU;
        unsigned acc_bits = wide ? sew * 2 : sew;
        uint64_t acc = latr_rvv_read_elem(env, vs1, 0, acc_bits);
        uint64_t mask = rvv_sew_mask(acc_bits);

        for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
            uint64_t src;
            int64_t sa;
            int64_t sb;
            if (!rvv_mask_bit(env, i)) {
                continue;
            }
            src = latr_rvv_read_elem(env, vs2, i, sew);
            sa = rvv_signed_value(acc, acc_bits);
            sb = rvv_signed_value(src, sew);
            switch (op) {
            case LATR_RVV_RED_SUM:
                acc = (acc + src) & mask;
                break;
            case LATR_RVV_RED_AND:
                acc &= src;
                break;
            case LATR_RVV_RED_OR:
                acc |= src;
                break;
            case LATR_RVV_RED_XOR:
                acc ^= src;
                break;
            case LATR_RVV_RED_MIN:
                if (sb < sa) {
                    acc = src & mask;
                }
                break;
            case LATR_RVV_RED_MAX:
                if (sb > sa) {
                    acc = src & mask;
                }
                break;
            case LATR_RVV_RED_MINU:
                if ((src & mask) < (acc & mask)) {
                    acc = src & mask;
                }
                break;
            case LATR_RVV_RED_MAXU:
                if ((src & mask) > (acc & mask)) {
                    acc = src & mask;
                }
                break;
            case LATR_RVV_RED_WSUM:
                acc = (acc + (uint64_t)sb) & mask;
                break;
            case LATR_RVV_RED_WSUMU:
                acc = (acc + src) & mask;
                break;
            default:
                break;
            }
        }
        latr_rvv_write_elem(env, vd, 0, acc_bits, acc);
    }
    env->vstart = 0;
    if (RVV_VTYPE_VTA(env->vtype)) {
        unsigned acc_bits = (op == LATR_RVV_RED_WSUM ||
                             op == LATR_RVV_RED_WSUMU ||
                             op == LATR_RVV_RED_FWUSUM ||
                             op == LATR_RVV_RED_FWOSUM)
                                ? sew * 2
                                : sew;
        unsigned per = RVV_VLEN / acc_bits;
        for (i = 1; i < per; i++) {
            latr_rvv_write_elem(env, vd, i, acc_bits, rvv_sew_mask(acc_bits));
        }
    }
}

/*
 * Permute family: slides, gathers, and vcompress. Masked-off lanes follow
 * vma (ones when set), and element 0/1 slides read their scalar operand from
 * rvv_args[4] (already materialized by the translator).
 */
void latr_rvv_permute(CPUArchState *env)
{
    unsigned op = (unsigned)env->rvv_args[0];
    unsigned vd = (unsigned)env->rvv_args[1];
    unsigned vs2 = (unsigned)env->rvv_args[2];
    unsigned kind = (unsigned)env->rvv_args[3];
    uint64_t value = env->rvv_args[4];
    unsigned index_bits = (unsigned)env->rvv_args[5];
    int vm = (int)env->rvv_args[6];
    unsigned sew = rvv_sew(env);
    unsigned vlmax = rvv_vlmax(env);
    unsigned i;

    env->mem_fault_cause = 0;
    if (env->vstart >= env->vl) {
        env->vstart = 0;
        return;
    }
    if (op != LATR_RVV_PERM_COMPRESS) {
        rvv_build_mask(env, !vm);
    }

    if (op == LATR_RVV_PERM_COMPRESS) {
        unsigned num = 0;
        unsigned mask_reg = (unsigned)value;
        for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
            if (!((env->vreg[mask_reg & 31u][i >> 3] >> (i & 7u)) & 1u)) {
                continue;
            }
            latr_rvv_write_elem(env, vd, num, sew,
                                latr_rvv_read_elem(env, vs2, i, sew));
            num++;
        }
        env->vstart = 0;
        if (RVV_VTYPE_VTA(env->vtype)) {
            for (i = (unsigned)env->vl; i < vlmax; i++) {
                latr_rvv_write_elem(env, vd, i, sew, rvv_sew_mask(sew));
            }
        }
        return;
    }

    for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
        uint64_t out;
        int has_value = 1;

        if (!rvv_mask_bit(env, i)) {
            if (RVV_VTYPE_VMA(env->vtype)) {
                latr_rvv_write_elem(env, vd, i, sew, rvv_sew_mask(sew));
            }
            continue;
        }
        switch (op) {
        case LATR_RVV_PERM_SLIDEUP:
            if (i < value) {
                has_value = 0;
                out = 0;
            } else {
                out = latr_rvv_read_elem(env, vs2, i - (unsigned)value, sew);
            }
            break;
        case LATR_RVV_PERM_SLIDEDOWN:
            if (i + value < env->vl) {
                out = latr_rvv_read_elem(env, vs2, i + (unsigned)value, sew);
            } else {
                out = 0;
            }
            break;
        case LATR_RVV_PERM_SLIDE1UP:
            if (i == 0) {
                out = kind == LATR_RVV_SRC_VECTOR
                          ? latr_rvv_read_elem(env, (unsigned)value, 0, sew)
                          : value;
            } else {
                out = latr_rvv_read_elem(env, vs2, i - 1, sew);
            }
            break;
        case LATR_RVV_PERM_SLIDE1DOWN:
            if (i + 1 < env->vl) {
                out = latr_rvv_read_elem(env, vs2, i + 1, sew);
            } else {
                out = kind == LATR_RVV_SRC_VECTOR
                          ? latr_rvv_read_elem(env, (unsigned)value, 0, sew)
                          : value;
            }
            break;
        case LATR_RVV_PERM_GATHER:
        case LATR_RVV_PERM_GATHER16: {
            unsigned ib = index_bits ? index_bits : sew;
            uint64_t index = kind == LATR_RVV_SRC_VECTOR
                                 ? latr_rvv_read_elem(env, (unsigned)value, i,
                                                      ib)
                                 : value;
            if (index >= vlmax) {
                out = 0;
            } else {
                out = latr_rvv_read_elem(env, vs2, (unsigned)index, sew);
            }
            break;
        }
        default:
            has_value = 0;
            out = 0;
            break;
        }
        if (has_value) {
            latr_rvv_write_elem(env, vd, i, sew, out);
        }
    }
    env->vstart = 0;
    rvv_tail_ones(env, vd, sew);
}

/* Rounding helpers for the RVV fixed-point families. */
static uint64_t rvv_extra_scalar(CPUArchState *env, unsigned kind,
                                 uint64_t value, unsigned bits, int wide)
{
    if (kind == LATR_RVV_SRC_SCALAR) {
        return env->gpr[value & 31u];
    }
    return value;
}

static PvirRounding rvv_fixed_rounding(uint64_t vxrm)
{
    switch (vxrm & 3u) {
    case 0:  return PVIR_ROUND_RNU;
    case 1:  return PVIR_ROUND_NE;
    case 2:  return PVIR_ROUND_RDN;
    default: return PVIR_ROUND_ROD;
    }
}

static void rvv_arith_extend(CPUArchState *env, unsigned op, unsigned vd,
                             unsigned vs2, unsigned sew)
{
    unsigned factor = (op == LATR_RVV_X_ZEXT2 || op == LATR_RVV_X_SEXT2) ? 2u
                    : (op == LATR_RVV_X_ZEXT4 || op == LATR_RVV_X_SEXT4) ? 4u
                                                                         : 8u;
    int is_signed = op == LATR_RVV_X_SEXT2 || op == LATR_RVV_X_SEXT4 ||
                    op == LATR_RVV_X_SEXT8;
    unsigned src_bits = sew / factor;
    PvirInstruction insn;
    PvirVectorOperation *o = &insn.operation.vector;

    if (src_bits < 8u || (src_bits & (src_bits - 1u)) != 0) {
        env->vstart = 0;
        return;
    }
    rvv_pvir_init(&insn);
    insn.opcode = PVIR_OP_EXTEND;
    o->type = rvv_pvir_type(PVIR_ELEMENT_INTEGER,
                            is_signed ? PVIR_SIGNED : PVIR_UNSIGNED, sew,
                            (unsigned)env->vl);
    o->predicate = rvv_pvir_mask(env);
    o->inactive_policy = PVIR_INACTIVE_UNKNOWN;
    o->merge_value = PVIR_NO_VALUE;
    o->destination = vd;
    o->operand_count = 1;
    o->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    o->operands[0] = vs2;
    o->operand_types[0] = rvv_pvir_type(PVIR_ELEMENT_INTEGER,
                                        is_signed ? PVIR_SIGNED : PVIR_UNSIGNED,
                                        src_bits, (unsigned)env->vl);
    latr_pvir_execute_cached(env, UINT64_C(0x540) ^ op, &insn);
    if (env->pvir_status == PVIR_STATUS_OK) {
        rvv_pvir_fill_inactive(env, vd, sew);
    }
    env->vstart = 0;
    rvv_tail_ones(env, vd, sew);
}

static bool rvv_arith_fixed(CPUArchState *env, unsigned op, unsigned vd,
                            unsigned vs2, unsigned vs1, unsigned kind,
                            uint64_t imm, unsigned sew)
{
    PvirInstruction insn;
    PvirFixedPointOperation *fp = &insn.operation.fixed;
    PvirFixedPointKind fkind;
    bool is_signed = true;
    bool uses_round = false;
    bool saturating = false;

    switch (op) {
    case LATR_RVV_X_AADD:   fkind = PVIR_FIXED_AVG_ADD; is_signed = true; uses_round = true; break;
    case LATR_RVV_X_AADDU:  fkind = PVIR_FIXED_AVG_ADD; is_signed = false; uses_round = true; break;
    case LATR_RVV_X_ASUB:   fkind = PVIR_FIXED_AVG_SUB; is_signed = true; uses_round = true; break;
    case LATR_RVV_X_ASUBU:  fkind = PVIR_FIXED_AVG_SUB; is_signed = false; uses_round = true; saturating = true; break;
    case LATR_RVV_X_SADD:   fkind = PVIR_FIXED_SAT_ADD; is_signed = true; saturating = true; break;
    case LATR_RVV_X_SADDU:  fkind = PVIR_FIXED_SAT_ADD; is_signed = false; saturating = true; break;
    case LATR_RVV_X_SSUB:   fkind = PVIR_FIXED_SAT_SUB; is_signed = true; saturating = true; break;
    case LATR_RVV_X_SSUBU:  fkind = PVIR_FIXED_SAT_SUB; is_signed = false; saturating = true; break;
    case LATR_RVV_X_SSRL:   fkind = PVIR_FIXED_SCALE_SHIFT; is_signed = false; uses_round = true; break;
    case LATR_RVV_X_SSRA:   fkind = PVIR_FIXED_SCALE_SHIFT; is_signed = true; uses_round = true; break;
    case LATR_RVV_X_SMUL:   fkind = PVIR_FIXED_SAT_MUL; is_signed = true; uses_round = true; saturating = true; break;
    case LATR_RVV_X_NCLIP:  fkind = PVIR_FIXED_CLIP; is_signed = true; uses_round = true; saturating = true; break;
    case LATR_RVV_X_NCLIPU: fkind = PVIR_FIXED_CLIP; is_signed = false; uses_round = true; saturating = true; break;
    default:
        return false;
    }

    rvv_pvir_init(&insn);
    insn.opcode = PVIR_OP_FIXED;
    fp->type = rvv_pvir_type(PVIR_ELEMENT_INTEGER,
                             is_signed ? PVIR_SIGNED : PVIR_UNSIGNED, sew,
                             (unsigned)env->vl);
    fp->predicate = rvv_pvir_mask(env);
    fp->inactive_policy = PVIR_INACTIVE_UNKNOWN;
    fp->merge_value = PVIR_NO_VALUE;
    fp->destination = vd;
    fp->operands[0] = vs2;
    fp->operand_kinds[0] = PVIR_OPERAND_VECTOR;
    if (kind == LATR_RVV_SRC_VECTOR) {
        fp->operands[1] = vs1;
        fp->operand_kinds[1] = PVIR_OPERAND_VECTOR;
    } else if (kind == LATR_RVV_SRC_SCALAR) {
        env->rvv_scalar = env->gpr[imm & 31u];
        fp->operands[1] = LATR_PVIR_SCALAR_VALUE;
        fp->operand_kinds[1] = PVIR_OPERAND_SCALAR;
    } else {
        env->rvv_imm = imm;
        fp->operands[1] = LATR_PVIR_IMM_VALUE;
        fp->operand_kinds[1] = PVIR_OPERAND_SCALAR;
    }
    fp->kind = fkind;
    fp->rounding = uses_round ? rvv_fixed_rounding(env->vxrm)
                              : PVIR_ROUND_DEFAULT;
    fp->saturating = saturating;
    if (saturating) {
        fp->sat_state.output_value = 12u;
        insn.effects.arch_state.present = true;
        insn.effects.arch_state.domain = PVIR_ARCH_STATE_DOMAIN_OTHER;
        insn.effects.arch_state.action = PVIR_ARCH_STATE_UPDATE;
        insn.effects.arch_state.state_id = 3u;
    }
    latr_pvir_execute_cached(env, UINT64_C(0x500) ^ op, &insn);
    if (env->pvir_status == PVIR_STATUS_OK) {
        rvv_pvir_fill_inactive(env, vd, sew);
    }
    env->vstart = 0;
    rvv_tail_ones(env, vd, sew);
    return true;
}

void latr_rvv_arith_extra(CPUArchState *env)
{
    unsigned op = (unsigned)env->rvv_args[0];
    unsigned vd = (unsigned)env->rvv_args[1];
    unsigned vs2 = (unsigned)env->rvv_args[2];
    unsigned vs1 = (unsigned)env->rvv_args[3];
    unsigned kind = (unsigned)env->rvv_args[4];
    uint64_t imm = env->rvv_args[5];
    int vm = (int)env->rvv_args[6];
    int wide_src = (int)env->rvv_args[7];
    unsigned sew = rvv_sew(env);
    unsigned dw = sew * 2u;
    unsigned i;

    if (env->vstart >= env->vl) {
        env->vstart = 0;
        return;
    }
    rvv_build_mask(env, !vm);
    if (op >= LATR_RVV_X_ZEXT2 && op <= LATR_RVV_X_SEXT8) {
        rvv_arith_extend(env, op, vd, vs2, sew);
        return;
    }
    if (rvv_arith_fixed(env, op, vd, vs2, vs1, kind, imm, sew)) {
        return;
    }

    for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
        uint64_t out;
        int is_wide = 0;
        int is_narrow = 0;
        int is_macc = 0;

        switch (op) {
        case LATR_RVV_X_WADD: case LATR_RVV_X_WADDU:
        case LATR_RVV_X_WSUB: case LATR_RVV_X_WSUBU:
        case LATR_RVV_X_WMUL: case LATR_RVV_X_WMULU:
        case LATR_RVV_X_WMULSU:
            is_wide = 1;
            break;
        case LATR_RVV_X_WMACC: case LATR_RVV_X_WMACCU:
        case LATR_RVV_X_WMACCSU: case LATR_RVV_X_WMACCUS:
            is_wide = 1;
            is_macc = 1;
            break;
        case LATR_RVV_X_NSRL: case LATR_RVV_X_NSRA:
            is_narrow = 1;
            break;
        default:
            break;
        }

        if (!rvv_mask_bit(env, i)) {
            unsigned bits = is_wide ? dw : sew;
            if (RVV_VTYPE_VMA(env->vtype)) {
                latr_rvv_write_elem(env, vd, i, bits, rvv_sew_mask(bits));
            }
            continue;
        }

        /*
         * vrsub: the scalar is the minuend (scalar - element), which is the one
         * place where operand roles are reversed relative to the general ALU
         * path. The subtraction is modulo 2^SEW for both the .vx and .vi forms,
         * so the raw scalar bits are used as-is.
         */
        if (op == LATR_RVV_X_RSUB) {
            uint64_t elem = latr_rvv_read_elem(env, vs2, i, sew);
            uint64_t scalar = rvv_extra_scalar(env, kind, imm, sew, 0);

            out = (scalar - elem) & rvv_sew_mask(sew);
            latr_rvv_write_elem(env, vd, i, sew, out);
            continue;
        }
        /*
         * High-half multiply. The product is formed at 2*SEW and shifted down:
         * taking the host's own high word would be wrong for SEW < 64, and
         * mulhsu mixes signedness per operand (vs2 signed, vs1/x unsigned).
         */
        if (op == LATR_RVV_X_MULH || op == LATR_RVV_X_MULHU ||
            op == LATR_RVV_X_MULHSU) {
            int a_signed = op != LATR_RVV_X_MULHU;
            int b_signed = op == LATR_RVV_X_MULH;
            uint64_t a_bits = latr_rvv_read_elem(env, vs2, i, sew);
            __int128 a = a_signed
                             ? (__int128)rvv_sext(a_bits, sew)
                             : (__int128)(unsigned __int128)
                                   (a_bits & rvv_sew_mask(sew));
            __int128 b;
            __int128 prod;

            if (kind == LATR_RVV_SRC_VECTOR) {
                uint64_t b_bits = latr_rvv_read_elem(env, vs1, i, sew);
                b = b_signed
                        ? (__int128)rvv_sext(b_bits, sew)
                        : (__int128)(unsigned __int128)(b_bits & rvv_sew_mask(sew));
            } else {
                uint64_t sv = rvv_extra_scalar(env, kind, imm, sew, 0);
                b = b_signed ? (__int128)rvv_sext(sv, sew)
                             : (__int128)(unsigned __int128)
                                   (sv & rvv_sew_mask(sew));
            }
            prod = a * b;
            out = (uint64_t)((unsigned __int128)prod >> sew) & rvv_sew_mask(sew);
            latr_rvv_write_elem(env, vd, i, sew, out);
            continue;
        }

        /*
         * Integer multiply-accumulate, SEW-width throughout. The product wraps
         * modulo 2^SEW before it is added, so the high half is dropped rather
         * than carried into anything. Operand roles differ between the two
         * halves: MACC/NMSAC accumulate into vd, MADD/NMSUB multiply vd.
         */
        if (op == LATR_RVV_X_MACC || op == LATR_RVV_X_NMSAC ||
            op == LATR_RVV_X_MADD || op == LATR_RVV_X_NMSUB) {
            uint64_t mask = rvv_sew_mask(sew);
            uint64_t a = kind == LATR_RVV_SRC_VECTOR
                             ? latr_rvv_read_elem(env, vs1, i, sew)
                             : rvv_extra_scalar(env, kind, imm, sew, 0);
            uint64_t b = latr_rvv_read_elem(env, vs2, i, sew);
            uint64_t acc = latr_rvv_read_elem(env, vd, i, sew);
            uint64_t prod;

            a &= mask;
            b &= mask;
            acc &= mask;
            if (op == LATR_RVV_X_MACC) {
                out = (acc + (a * b)) & mask;
            } else if (op == LATR_RVV_X_NMSAC) {
                out = (acc - (a * b)) & mask;
            } else if (op == LATR_RVV_X_MADD) {
                out = (((a * acc) & mask) + b) & mask;
            } else {
                out = (b - ((a * acc) & mask)) & mask;
            }
            latr_rvv_write_elem(env, vd, i, sew, out);
            continue;
        }

        if (is_narrow) {
            uint64_t wide = latr_rvv_read_elem(env, vs2, i, dw);
            uint64_t sh;
            if (kind == LATR_RVV_SRC_VECTOR) {
                sh = latr_rvv_read_elem(env, vs1, i, sew);
            } else {
                sh = imm;
            }
            sh &= (dw - 1u);
            switch (op) {
            case LATR_RVV_X_NSRL:
                out = (uint64_t)((unsigned __int128)wide >> sh);
                break;
            case LATR_RVV_X_NSRA:
                out = (uint64_t)(((__int128)rvv_sext(wide, dw)) >> sh);
                break;
            default:
                out = 0;
                break;
            }
            latr_rvv_write_elem(env, vd, i, sew, out);
            continue;
        }

        if (is_wide) {
            int signed_a = op == LATR_RVV_X_WADD || op == LATR_RVV_X_WSUB ||
                           op == LATR_RVV_X_WMUL || op == LATR_RVV_X_WMULSU ||
                           op == LATR_RVV_X_WMACC ||
                           op == LATR_RVV_X_WMACCSU ||
                           op == LATR_RVV_X_WMACCUS;
            int signed_b = op == LATR_RVV_X_WADD || op == LATR_RVV_X_WSUB ||
                           op == LATR_RVV_X_WMUL || op == LATR_RVV_X_WMACC ||
                           op == LATR_RVV_X_WMACCU;
            uint64_t a_bits = latr_rvv_read_elem(env, vs2, i, sew);
            __int128 a = signed_a ? (__int128)rvv_sext(a_bits, sew)
                                  : (__int128)(unsigned __int128)a_bits;
            __int128 b;
            if (kind == LATR_RVV_SRC_VECTOR) {
                unsigned b_bits = wide_src ? dw : sew;
                uint64_t bv = latr_rvv_read_elem(env, vs1, i, b_bits);
                b = signed_b ? (__int128)rvv_sext(bv, b_bits)
                             : (__int128)(unsigned __int128)bv;
            } else {
                uint64_t sv = rvv_extra_scalar(env, kind, imm, sew, wide_src);
                if (wide_src) {
                    b = signed_b ? (__int128)sv
                                 : (__int128)(unsigned __int128)sv;
                } else {
                    b = signed_b ? (__int128)rvv_sext(sv, sew)
                                 : (__int128)(unsigned __int128)
                                       (sv & rvv_sew_mask(sew));
                }
            }
            if (is_macc) {
                __int128 acc = (__int128)rvv_sext(
                    latr_rvv_read_elem(env, vd, i, dw), dw);
                out = (uint64_t)(acc + a * b) & rvv_sew_mask(dw);
            } else {
                switch (op) {
                case LATR_RVV_X_WADD:  out = (uint64_t)(a + b); break;
                case LATR_RVV_X_WADDU: out = (uint64_t)(a + b); break;
                case LATR_RVV_X_WSUB:  out = (uint64_t)(a - b); break;
                case LATR_RVV_X_WSUBU: out = (uint64_t)(a - b); break;
                case LATR_RVV_X_WMUL:
                case LATR_RVV_X_WMULU:
                case LATR_RVV_X_WMULSU:
                    out = (uint64_t)(a * b);
                    break;
                default: out = 0; break;
                }
                out &= rvv_sew_mask(dw);
            }
            latr_rvv_write_elem(env, vd, i, dw, out);
            continue;
        }
    }
    env->vstart = 0;
    if (RVV_VTYPE_VTA(env->vtype)) {
        int wide = 0;
        switch (op) {
        case LATR_RVV_X_WADD: case LATR_RVV_X_WADDU:
        case LATR_RVV_X_WSUB: case LATR_RVV_X_WSUBU:
        case LATR_RVV_X_WMUL: case LATR_RVV_X_WMULU:
        case LATR_RVV_X_WMULSU: case LATR_RVV_X_WMACC:
        case LATR_RVV_X_WMACCU: case LATR_RVV_X_WMACCSU:
        case LATR_RVV_X_WMACCUS:
            wide = 1;
            break;
        default:
            break;
        }
        for (i = (unsigned)env->vl; i < rvv_vlmax(env); i++) {
            latr_rvv_write_elem(env, vd, i, wide ? dw : sew,
                                rvv_sew_mask(wide ? dw : sew));
        }
    }
}

/* ---- Vector floating point -------------------------------------------- */

static uint32_t rvv_fp_scalar32(const CPUArchState *env, unsigned reg)
{
    uint64_t value = env->fpr[reg & 31u];

    return (value >> 32) != UINT32_MAX ? UINT32_C(0x7fc00000)
                                       : (uint32_t)value;
}

static uint64_t rvv_fp_scalar64(const CPUArchState *env, unsigned reg)
{
    return env->fpr[reg & 31u];
}

static uint64_t rvv_fp_class(uint64_t bits, unsigned width)
{
    unsigned sign = (unsigned)(bits >> (width - 1)) & 1u;
    unsigned mant_bits = width == 32 ? 23 : 52;
    uint64_t mant_mask = (((uint64_t)1) << mant_bits) - 1;
    uint64_t exp_mask = (((uint64_t)1) << (width - 1 - mant_bits)) - 1;
    uint64_t exp = (bits >> mant_bits) & exp_mask;
    uint64_t mant = bits & mant_mask;
    unsigned quiet_bit = mant_bits - 1;

    if (exp == exp_mask) {
        if (mant == 0) {
            return sign ? (UINT64_C(1) << 0) : (UINT64_C(1) << 7);
        }
        if (mant & (UINT64_C(1) << quiet_bit)) {
            return UINT64_C(1) << 9;
        }
        return UINT64_C(1) << 8;
    }
    if (exp == 0) {
        if (mant == 0) {
            return sign ? (UINT64_C(1) << 3) : (UINT64_C(1) << 4);
        }
        return sign ? (UINT64_C(1) << 2) : (UINT64_C(1) << 5);
    }
    return sign ? (UINT64_C(1) << 1) : (UINT64_C(1) << 6);
}

/*
 * RVV fused multiply-add (vfmacc/vfmsac/vfnmacc/vfnmsac and the madd/msub
 * family, .vv and .vf).
 *
 * One rounding: the product and the addend are combined before rounding. A
 * mul-then-add sequence differs whenever the product needs more precision than
 * the destination format carries - e.g. (1+2^-13)(1-2^-13) rounds to 1.0 on its
 * own, so only the fused add of -1 exposes the -2^-26 tail.
 *
 * Sign rules come from the variant index: bit0 negates the product, and the
 * addend is negated by `product ^ addend` over bits 0/1 (macc: none, nmacc:
 * both, msac: addend only, nmsac: product only); variants 0..3 accumulate with
 * vd as the multiplicand, 4..7 use vd as the addend.
 */
/*
 * RVV carry/borrow (vadc/vsbc/vmadc/vmsbc).
 *
 * v0 holds the carry-in, one bit per element, and is not a predicate: vadc and
 * vsbc write every body element back regardless of v0, and vmadc/vmsbc are
 * unmaskable. The vm bit therefore only selects whether a carry-in is added,
 * which is why this cannot reuse the ALU path's rvv_build_mask() preamble
 * (vm=0 there would prune lanes instead of feeding the carry).
 *
 * vadc/vsbc are reserved when vd is v0; the shared legality form rejects that
 * through the mask rule (vm=0 requires vd != 0).
 */
void latr_rvv_carry(CPUArchState *env)
{
    unsigned op = (unsigned)env->rvv_args[0];
    unsigned vd = (unsigned)env->rvv_args[1];
    unsigned vs2 = (unsigned)env->rvv_args[2];
    unsigned vs1 = (unsigned)env->rvv_args[3];
    unsigned kind = (unsigned)env->rvv_args[4];
    uint64_t imm = env->rvv_args[5];
    int vm = (int)env->rvv_args[6];
    unsigned sew = rvv_sew(env);
    uint64_t mask = rvv_sew_mask(sew);
    int mask_dest = op == LATR_RVV_X_VMADC || op == LATR_RVV_X_VMSBC;
    /*
     * For vmadc/vmsbc the raw vm bit selects the carry-in form (vm=0 adds it).
     * vadc/vsbc are decoded through @r_vm_1, which forces arg.vm to 1 so that
     * the generic mask rule does not apply; their encoding is always vm=0 and
     * they always add the carry-in.
     */
    int with_carry = (op == LATR_RVV_X_VADC || op == LATR_RVV_X_VSBC) ? 1 : !vm;
    unsigned i;

    if (env->vstart >= env->vl) {
        env->vstart = 0;
        return;
    }
    for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
        uint64_t a = latr_rvv_read_elem(env, vs2, i, sew) & mask;
        uint64_t b;
        unsigned cin = with_carry ? rvv_v0_bit(env, i) : 0u;

        if (kind == LATR_RVV_SRC_VECTOR) {
            b = latr_rvv_read_elem(env, vs1, i, sew) & mask;
        } else if (kind == LATR_RVV_SRC_SCALAR) {
            b = env->gpr[vs1 & 31u] & mask;
        } else {
            b = imm & mask;
        }
        if (!mask_dest) {
            uint64_t out = op == LATR_RVV_X_VADC
                               ? (a + b + cin) & mask
                               : (a - b - cin) & mask;

            latr_rvv_write_elem(env, vd, i, sew, out);
        } else {
            unsigned bit;

            if (op == LATR_RVV_X_VMADC) {
                bit = (unsigned)((((unsigned __int128)a + b + cin) >> sew) & 1u);
            } else {
                /* Borrow iff the difference before truncation is negative. */
                bit = ((unsigned __int128)a < (unsigned __int128)b + cin) ? 1u
                                                                          : 0u;
            }
            if (bit) {
                env->vreg[vd & 31u][i >> 3] |= (uint8_t)(1u << (i & 7u));
            } else {
                env->vreg[vd & 31u][i >> 3] &= (uint8_t)~(1u << (i & 7u));
            }
        }
    }
    if (!mask_dest) {
        /* vma is ignored: there is no masking, only the tail policy applies. */
        rvv_tail_ones(env, vd, sew);
    } else {
        /* Mask destinations are always tail-agnostic, regardless of vta. */
        unsigned vlmax = rvv_vlmax(env);

        for (i = (unsigned)env->vl; i < vlmax; i++) {
            env->vreg[vd & 31u][i >> 3] |= (uint8_t)(1u << (i & 7u));
        }
    }
    env->vstart = 0;
}

void latr_rvv_fma(CPUArchState *env)
{
    unsigned variant = (unsigned)env->rvv_args[0];
    unsigned vd = (unsigned)env->rvv_args[1];
    unsigned op2 = (unsigned)env->rvv_args[2];
    unsigned vs2 = (unsigned)env->rvv_args[3];
    unsigned kind = (unsigned)env->rvv_args[4];
    int vm = (int)env->rvv_args[6];
    unsigned sew = rvv_sew(env);
    int widening = variant >= LATR_RVV_FWMA_MACC;
    int accumulate_into_vd;
    int flags = 0;
    unsigned i;
    float_status status;

    if (widening) {
        variant -= LATR_RVV_FWMA_MACC;
        /* Only the four accumulate variants exist; the product is exact in
         * 2*SEW, so the single rounding at 2*SEW is the whole point. */
        accumulate_into_vd = 1;
    } else {
        accumulate_into_vd = variant < LATR_RVV_FMA_MADD;
    }

    if (env->vstart >= env->vl) {
        env->vstart = 0;
        return;
    }
    if (widening ? (sew != 32) : (sew != 32 && sew != 64)) {
        /* Widening needs 2*SEW = 64 for an f32 product; base RVV FLEN is 64. */
        env->vstart = 0;
        return;
    }
    /* Expand the mask (or all-ones when unmasked) before rvv_mask_bit is used;
     * without this every lane reads as inactive. */
    rvv_build_mask(env, !vm);
    if (variant & 1u) {
        flags |= float_muladd_negate_product;
    }
    if ((variant ^ (variant >> 1)) & 1u) {
        flags |= float_muladd_negate_c;
    }
    latr_fp_status_begin(env, &status, 0);
    if (widening) {
        unsigned dw = sew * 2u;

        for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
            uint64_t acc;
            float64 a;
            float64 b;
            uint64_t out;

            if (!rvv_mask_bit(env, i)) {
                if (RVV_VTYPE_VMA(env->vtype)) {
                    latr_rvv_write_elem(env, vd, i, dw, rvv_sew_mask(dw));
                }
                continue;
            }
            a = float32_to_float64(
                (float32)(kind == LATR_RVV_SRC_VECTOR
                              ? latr_rvv_read_elem(env, op2, i, sew)
                              : rvv_fp_scalar32(env, op2)),
                &status);
            b = float32_to_float64(
                (float32)latr_rvv_read_elem(env, vs2, i, sew), &status);
            acc = latr_rvv_read_elem(env, vd, i, dw);
            out = (uint64_t)float64_muladd(a, b, (float64)acc, flags, &status);
            latr_rvv_write_elem(env, vd, i, dw, out);
        }
        latr_fp_status_end(env, &status);
        env->vstart = 0;
        if (RVV_VTYPE_VTA(env->vtype)) {
            for (i = (unsigned)env->vl; i < rvv_vlmax(env); i++) {
                latr_rvv_write_elem(env, vd, i, dw, rvv_sew_mask(dw));
            }
        }
        return;
    }
    for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
        uint64_t vd_old;
        uint64_t a_bits, b_bits, c_bits;
        uint64_t out;

        if (!rvv_mask_bit(env, i)) {
            if (RVV_VTYPE_VMA(env->vtype)) {
                latr_rvv_write_elem(env, vd, i, sew, rvv_sew_mask(sew));
            }
            continue;
        }
        vd_old = latr_rvv_read_elem(env, vd, i, sew);
        if (kind == LATR_RVV_SRC_VECTOR) {
            a_bits = accumulate_into_vd
                         ? vd_old
                         : latr_rvv_read_elem(env, op2, i, sew);
            b_bits = accumulate_into_vd
                         ? latr_rvv_read_elem(env, op2, i, sew)
                         : latr_rvv_read_elem(env, vs2, i, sew);
            c_bits = accumulate_into_vd
                         ? latr_rvv_read_elem(env, vs2, i, sew)
                         : vd_old;
        } else {
            uint64_t scalar = sew == 32
                                  ? (uint64_t)rvv_fp_scalar32(env, op2)
                                  : rvv_fp_scalar64(env, op2);

            a_bits = accumulate_into_vd ? vd_old : scalar;
            b_bits = accumulate_into_vd ? scalar
                                        : latr_rvv_read_elem(env, vs2, i, sew);
            c_bits = accumulate_into_vd
                         ? latr_rvv_read_elem(env, vs2, i, sew)
                         : vd_old;
        }
        if (sew == 32) {
            out = (uint64_t)float32_muladd((float32)a_bits, (float32)b_bits,
                                           (float32)c_bits, flags, &status);
        } else {
            out = (uint64_t)float64_muladd((float64)a_bits, (float64)b_bits,
                                           (float64)c_bits, flags, &status);
        }
        latr_rvv_write_elem(env, vd, i, sew, out);
    }
    latr_fp_status_end(env, &status);
    env->vstart = 0;
    if (RVV_VTYPE_VTA(env->vtype)) {
        unsigned i2;

        for (i2 = (unsigned)env->vl; i2 < rvv_vlmax(env); i2++) {
            latr_rvv_write_elem(env, vd, i2, sew, rvv_sew_mask(sew));
        }
    }
}

void latr_rvv_fp(CPUArchState *env)
{
    unsigned op = (unsigned)env->rvv_args[0];
    unsigned vd = (unsigned)env->rvv_args[1];
    unsigned vs2 = (unsigned)env->rvv_args[2];
    unsigned vs1 = (unsigned)env->rvv_args[3];
    unsigned kind = (unsigned)env->rvv_args[4];
    int vm = (int)env->rvv_args[5];
    unsigned sew = rvv_sew(env);
    float_status status;
    int uses_fp = 0;
    int is_cmp = op >= LATR_RVV_FP_CMP_EQ && op <= LATR_RVV_FP_CMP_GE;
    unsigned i;

    if (sew != 32 && sew != 64) {
        return;
    }
    if (env->vstart >= env->vl) {
        env->vstart = 0;
        return;
    }

    switch (op) {
    case LATR_RVV_FP_ADD: case LATR_RVV_FP_SUB:
    case LATR_RVV_FP_MUL: case LATR_RVV_FP_DIV:
    case LATR_RVV_FP_MIN: case LATR_RVV_FP_MAX:
    case LATR_RVV_FP_SQRT:
        uses_fp = 1;
        break;
    case LATR_RVV_FP_CMP_EQ: case LATR_RVV_FP_CMP_NE:
    case LATR_RVV_FP_CMP_LT: case LATR_RVV_FP_CMP_LE:
    case LATR_RVV_FP_CMP_GT: case LATR_RVV_FP_CMP_GE:
        uses_fp = 1;
        break;
    default:
        break;
    }
    if (uses_fp) {
        latr_fp_status_begin(env, &status, 0);
    }
    if (!is_cmp) {
        rvv_build_mask(env, !vm);
    }

    if (op == LATR_RVV_FP_ADD || op == LATR_RVV_FP_SUB ||
        op == LATR_RVV_FP_MUL || op == LATR_RVV_FP_DIV ||
        op == LATR_RVV_FP_SQRT) {
        PvirInstruction insn;
        PvirFPOpOperation *o = &insn.operation.fp;

        rvv_pvir_init(&insn);
        insn.opcode = PVIR_OP_FP_ARITH;
        insn.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
        o->type = rvv_pvir_type(PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, sew,
                                (unsigned)env->vl);
        o->predicate = rvv_pvir_mask(env);
        o->inactive_policy = PVIR_INACTIVE_UNKNOWN;
        o->merge_value = PVIR_NO_VALUE;
        o->destination = vd;
        o->kind = op == LATR_RVV_FP_ADD ? PVIR_FP_ADD
                : op == LATR_RVV_FP_SUB ? PVIR_FP_SUB
                : op == LATR_RVV_FP_MUL ? PVIR_FP_MUL
                : op == LATR_RVV_FP_DIV ? PVIR_FP_DIV
                                        : PVIR_FP_SQRT;
        o->rounding = PVIR_ROUND_DEFAULT;
        o->operands[0] = vs2;
        o->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        if (o->kind != PVIR_FP_SQRT) {
            o->operands[1] = kind == LATR_RVV_SRC_VECTOR
                                 ? vs1 : LATR_PVIR_SCALAR_VALUE;
            o->operand_kinds[1] = kind == LATR_RVV_SRC_VECTOR
                                      ? PVIR_OPERAND_VECTOR
                                      : PVIR_OPERAND_SCALAR;
            if (kind != LATR_RVV_SRC_VECTOR) {
                env->rvv_scalar = sew == 32 ? rvv_fp_scalar32(env, vs1)
                                            : rvv_fp_scalar64(env, vs1);
            }
        } else {
            o->operands[1] = PVIR_NO_VALUE;
        }
        latr_pvir_execute_cached(env, UINT64_C(0x600) ^ op, &insn);
        if (env->pvir_status == PVIR_STATUS_OK) {
            rvv_pvir_fill_inactive(env, vd, sew);
        }
        env->vstart = 0;
        rvv_tail_ones(env, vd, sew);
        return;
    }
    if (is_cmp) {
        PvirInstruction insn;
        PvirVectorOperation *o = &insn.operation.vector;
        PvirCompareCondition condition;

        rvv_build_mask(env, !vm);
        switch (op) {
        case LATR_RVV_FP_CMP_EQ: condition = PVIR_COMPARE_EQ; break;
        case LATR_RVV_FP_CMP_NE: condition = PVIR_COMPARE_NE; break;
        case LATR_RVV_FP_CMP_LT: condition = PVIR_COMPARE_LT; break;
        case LATR_RVV_FP_CMP_LE: condition = PVIR_COMPARE_LE; break;
        case LATR_RVV_FP_CMP_GT: condition = PVIR_COMPARE_GT; break;
        default: condition = PVIR_COMPARE_GE; break;
        }
        rvv_pvir_init(&insn);
        insn.opcode = PVIR_OP_COMPARE;
        o->type = rvv_pvir_type(PVIR_ELEMENT_MASK, PVIR_SIGNLESS, 1,
                                (unsigned)env->vl);
        o->predicate = rvv_pvir_mask(env);
        o->inactive_policy = PVIR_INACTIVE_UNKNOWN;
        o->merge_value = PVIR_NO_VALUE;
        o->compare_condition = condition;
        o->destination = vd;
        o->operand_count = 2;
        o->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        o->operand_kinds[1] = PVIR_OPERAND_VECTOR;
        o->operands[0] = vs2;
        o->operand_types[0] = rvv_pvir_type(PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS,
                                            sew, (unsigned)env->vl);
        o->operand_types[1] = o->operand_types[0];
        if (kind == LATR_RVV_SRC_VECTOR) {
            o->operands[1] = vs1;
        } else {
            env->rvv_scalar = sew == 32 ? rvv_fp_scalar32(env, vs1)
                                        : rvv_fp_scalar64(env, vs1);
            o->operands[1] = LATR_PVIR_SCALAR_VALUE;
        }
        latr_pvir_execute_cached(env, UINT64_C(0x640) ^ op, &insn);
        if (env->pvir_status == PVIR_STATUS_OK && RVV_VTYPE_VMA(env->vtype)) {
            unsigned j;
            for (j = (unsigned)env->vstart; j < (unsigned)env->vl; j++) {
                if (!rvv_mask_bit(env, j)) {
                    env->vreg[vd & 31u][j >> 3] |= (uint8_t)(1u << (j & 7u));
                }
            }
        }
        env->vstart = 0;
        return;
    }
    if (op == LATR_RVV_FP_MV_V_F) {
        PvirInstruction insn;
        PvirVectorOperation *o = &insn.operation.vector;

        rvv_pvir_init(&insn);
        insn.opcode = PVIR_OP_BROADCAST;
        o->type = rvv_pvir_type(PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, sew,
                                (unsigned)env->vl);
        o->predicate = pvir_predicate_all_true((size_t)env->vl);
        o->inactive_policy = PVIR_INACTIVE_UNKNOWN;
        o->merge_value = PVIR_NO_VALUE;
        o->destination = vd;
        o->operand_count = 1;
        o->operand_kinds[0] = PVIR_OPERAND_SCALAR;
        o->operands[0] = LATR_PVIR_SCALAR_VALUE;
        env->rvv_scalar = sew == 32 ? rvv_fp_scalar32(env, vs1)
                                    : rvv_fp_scalar64(env, vs1);
        latr_pvir_execute_cached(env, UINT64_C(0x680), &insn);
        env->vstart = 0;
        rvv_tail_ones(env, vd, sew);
        return;
    }
    if (op == LATR_RVV_FP_MERGE) {
        PvirInstruction insn;
        PvirVectorOperation *o = &insn.operation.vector;

        rvv_pvir_init(&insn);
        insn.opcode = PVIR_OP_SELECT;
        o->type = rvv_pvir_type(PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, sew,
                                (unsigned)env->vl);
        o->predicate = pvir_predicate_all_true((size_t)env->vl);
        o->inactive_policy = PVIR_INACTIVE_UNKNOWN;
        o->merge_value = PVIR_NO_VALUE;
        o->destination = vd;
        o->operand_count = 3;
        o->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        o->operand_kinds[1] = PVIR_OPERAND_VECTOR;
        o->operand_kinds[2] = PVIR_OPERAND_VECTOR;
        o->operands[0] = LATR_PVIR_V0_VALUE;
        o->operands[1] = vs2;
        o->operands[2] = LATR_PVIR_SCALAR_VALUE;
        o->operand_types[0] = o->operand_types[1] = o->operand_types[2] =
            o->type;
        env->rvv_scalar = sew == 32 ? rvv_fp_scalar32(env, vs1)
                                    : rvv_fp_scalar64(env, vs1);
        latr_pvir_execute_cached(env, UINT64_C(0x690), &insn);
        env->vstart = 0;
        rvv_tail_ones(env, vd, sew);
        return;
    }

    if (op == LATR_RVV_FP_FWADD || op == LATR_RVV_FP_FWSUB ||
        op == LATR_RVV_FP_FWMUL) {
        float_status wide_status;
        unsigned dw = sew * 2u;
        int wide_src = (int)env->rvv_args[6];
        unsigned j;

        if (sew != 32) {
            env->vstart = 0;
            return;
        }
        latr_fp_status_begin(env, &wide_status, 0);
        rvv_build_mask(env, !vm);
        for (j = (unsigned)env->vstart; j < (unsigned)env->vl; j++) {
            uint64_t a_bits;
            uint64_t b_bits;
            float64 a;
            float64 b;
            float64 out;
            if (!rvv_mask_bit(env, j)) {
                if (RVV_VTYPE_VMA(env->vtype)) {
                    latr_rvv_write_elem(env, vd, j, dw, rvv_sew_mask(dw));
                }
                continue;
            }
            a_bits = latr_rvv_read_elem(env, vs2, j, sew);
            a = float32_to_float64((float32)a_bits, &wide_status);
            if (wide_src && kind == LATR_RVV_SRC_VECTOR) {
                b = (float64)latr_rvv_read_elem(env, vs1, j, dw);
            } else {
                b_bits = kind == LATR_RVV_SRC_VECTOR
                             ? latr_rvv_read_elem(env, vs1, j, sew)
                             : rvv_fp_scalar32(env, vs1);
                b = float32_to_float64((float32)b_bits, &wide_status);
            }
            if (op == LATR_RVV_FP_FWADD) {
                out = float64_add(a, b, &wide_status);
            } else if (op == LATR_RVV_FP_FWSUB) {
                out = float64_sub(a, b, &wide_status);
            } else {
                out = float64_mul(a, b, &wide_status);
            }
            latr_rvv_write_elem(env, vd, j, dw, (uint64_t)out);
        }
        latr_fp_status_end(env, &wide_status);
        env->vstart = 0;
        if (RVV_VTYPE_VTA(env->vtype)) {
            for (j = (unsigned)env->vl; j < rvv_vlmax(env); j++) {
                latr_rvv_write_elem(env, vd, j, dw, rvv_sew_mask(dw));
            }
        }
        return;
    }

    if (op == LATR_RVV_FP_MV_F_S) {
        if (vs2 < 32) {
            uint64_t elem = latr_rvv_read_elem(env, vs2, 0, sew);
            env->fpr[vd & 31u] = sew == 32
                                     ? (UINT64_C(0xffffffff00000000) |
                                        (uint32_t)elem)
                                     : elem;
        }
        env->vstart = 0;
        return;
    }
    if (op == LATR_RVV_FP_MV_S_F) {
        PvirInstruction insn;
        PvirLaneOperation *o = &insn.operation.lane;

        if (env->vl == 0) {
            env->vstart = 0;
            return;
        }
        rvv_pvir_init(&insn);
        insn.opcode = PVIR_OP_INSERT;
        o->source = LATR_PVIR_SCALAR_VALUE;
        o->destination = vd;
        o->merge_value = vd;
        o->vector_type = rvv_pvir_type(PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, sew,
                                       (unsigned)env->vl);
        o->lane = 0;
        env->rvv_scalar = sew == 32 ? rvv_fp_scalar32(env, vs1)
                                    : rvv_fp_scalar64(env, vs1);
        latr_pvir_execute_cached(env, UINT64_C(0x6a0), &insn);
        env->vstart = 0;
        if (RVV_VTYPE_VTA(env->vtype)) {
            unsigned t;
            unsigned vlmax = rvv_vlmax(env);
            for (t = 1; t < vlmax; t++) {
                latr_rvv_write_elem(env, vd, t, sew, rvv_sew_mask(sew));
            }
        }
        return;
    }
    if (op == LATR_RVV_FP_MV_V_F) {
        uint64_t elem = sew == 32 ? rvv_fp_scalar32(env, vs1)
                                  : rvv_fp_scalar64(env, vs1);
        rvv_build_mask(env, !vm);
        for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
            if (!rvv_mask_bit(env, i)) {
                if (RVV_VTYPE_VMA(env->vtype)) {
                    latr_rvv_write_elem(env, vd, i, sew,
                                        rvv_sew_mask(sew));
                }
                continue;
            }
            latr_rvv_write_elem(env, vd, i, sew, elem);
        }
        env->vstart = 0;
        rvv_tail_ones(env, vd, sew);
        return;
    }

    if (op == LATR_RVV_FP_MERGE) {
        for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
            uint64_t out;
            if (rvv_v0_bit(env, i)) {
                out = latr_rvv_read_elem(env, vs2, i, sew);
            } else {
                out = sew == 32 ? rvv_fp_scalar32(env, vs1)
                                : rvv_fp_scalar64(env, vs1);
            }
            latr_rvv_write_elem(env, vd, i, sew, out);
        }
        env->vstart = 0;
        rvv_tail_ones(env, vd, sew);
        return;
    }

    for (i = (unsigned)env->vstart; i < (unsigned)env->vl; i++) {
        uint64_t a_bits = latr_rvv_read_elem(env, vs2, i, sew);
        uint64_t b_bits = kind == LATR_RVV_SRC_VECTOR
                              ? latr_rvv_read_elem(env, vs1, i, sew)
                              : (sew == 32 ? rvv_fp_scalar32(env, vs1)
                                           : rvv_fp_scalar64(env, vs1));
        uint64_t out = 0;
        int write_out = 1;
        int active;

        if (is_cmp) {
            int taken;
            if (!vm) {
                int mask = (int)latr_rvv_read_elem(env, 0, i, 8);
                (void)mask;
            }
        }
        if (!is_cmp) {
            active = (int)rvv_mask_bit(env, i);
        } else {
            active = vm ? 1 : (int)((env->vreg[0][i >> 3] >> (i & 7u)) & 1u);
        }
        if (!active) {
            if (RVV_VTYPE_VMA(env->vtype)) {
                if (is_cmp) {
                    env->vreg[vd & 31u][i >> 3] |= (uint8_t)(1u << (i & 7u));
                } else {
                    latr_rvv_write_elem(env, vd, i, sew,
                                        rvv_sew_mask(sew));
                }
            }
            continue;
        }

        if (sew == 32) {
            float32 a = (float32)a_bits;
            float32 b = (float32)b_bits;
            switch (op) {
            case LATR_RVV_FP_ADD: out = (uint64_t)float32_add(a, b, &status); break;
            case LATR_RVV_FP_SUB: out = (uint64_t)float32_sub(a, b, &status); break;
            case LATR_RVV_FP_MUL: out = (uint64_t)float32_mul(a, b, &status); break;
            case LATR_RVV_FP_DIV: out = (uint64_t)float32_div(a, b, &status); break;
            case LATR_RVV_FP_MIN:
                out = (uint64_t)float32_minimum_number(a, b, &status); break;
            case LATR_RVV_FP_MAX:
                out = (uint64_t)float32_maximum_number(a, b, &status); break;
            case LATR_RVV_FP_SQRT:
                out = (uint64_t)float32_sqrt(a, &status); break;
            case LATR_RVV_FP_SGNJ:
                out = (a_bits & ~(UINT64_C(1) << 31)) |
                      (b_bits & (UINT64_C(1) << 31)); break;
            case LATR_RVV_FP_SGNJN:
                out = (a_bits & ~(UINT64_C(1) << 31)) |
                      (~b_bits & (UINT64_C(1) << 31)); break;
            case LATR_RVV_FP_SGNJX:
                out = (a_bits ^ (b_bits & (UINT64_C(1) << 31))); break;
            case LATR_RVV_FP_CLASS:
                out = rvv_fp_class(a_bits, 32); break;
            case LATR_RVV_FP_CMP_EQ:
                out = float32_eq_quiet(a, b, &status); write_out = 0; break;
            case LATR_RVV_FP_CMP_NE:
                out = float32_compare_quiet(a, b, &status) !=
                      float_relation_equal; write_out = 0; break;
            case LATR_RVV_FP_CMP_LT:
                out = float32_lt(a, b, &status); write_out = 0; break;
            case LATR_RVV_FP_CMP_LE:
                out = float32_le(a, b, &status); write_out = 0; break;
            case LATR_RVV_FP_CMP_GT:
                out = float32_compare(a, b, &status) ==
                      float_relation_greater; write_out = 0; break;
            case LATR_RVV_FP_CMP_GE:
                out = float32_le(b, a, &status); write_out = 0; break;
            default: write_out = 0; break;
            }
        } else {
            float64 a = (float64)a_bits;
            float64 b = (float64)b_bits;
            switch (op) {
            case LATR_RVV_FP_ADD: out = (uint64_t)float64_add(a, b, &status); break;
            case LATR_RVV_FP_SUB: out = (uint64_t)float64_sub(a, b, &status); break;
            case LATR_RVV_FP_MUL: out = (uint64_t)float64_mul(a, b, &status); break;
            case LATR_RVV_FP_DIV: out = (uint64_t)float64_div(a, b, &status); break;
            case LATR_RVV_FP_MIN:
                out = (uint64_t)float64_minimum_number(a, b, &status); break;
            case LATR_RVV_FP_MAX:
                out = (uint64_t)float64_maximum_number(a, b, &status); break;
            case LATR_RVV_FP_SQRT:
                out = (uint64_t)float64_sqrt(a, &status); break;
            case LATR_RVV_FP_SGNJ:
                out = (a_bits & ~(UINT64_C(1) << 63)) |
                      (b_bits & (UINT64_C(1) << 63)); break;
            case LATR_RVV_FP_SGNJN:
                out = (a_bits & ~(UINT64_C(1) << 63)) |
                      (~b_bits & (UINT64_C(1) << 63)); break;
            case LATR_RVV_FP_SGNJX:
                out = (a_bits ^ (b_bits & (UINT64_C(1) << 63))); break;
            case LATR_RVV_FP_CLASS:
                out = rvv_fp_class(a_bits, 64); break;
            case LATR_RVV_FP_CMP_EQ:
                out = float64_eq_quiet(a, b, &status); write_out = 0; break;
            case LATR_RVV_FP_CMP_NE:
                out = float64_compare_quiet(a, b, &status) !=
                      float_relation_equal; write_out = 0; break;
            case LATR_RVV_FP_CMP_LT:
                out = float64_lt(a, b, &status); write_out = 0; break;
            case LATR_RVV_FP_CMP_LE:
                out = float64_le(a, b, &status); write_out = 0; break;
            case LATR_RVV_FP_CMP_GT:
                out = float64_compare(a, b, &status) ==
                      float_relation_greater; write_out = 0; break;
            case LATR_RVV_FP_CMP_GE:
                out = float64_le(b, a, &status); write_out = 0; break;
            default: write_out = 0; break;
            }
        }
        if (write_out) {
            latr_rvv_write_elem(env, vd, i, sew, out);
        } else if (is_cmp) {
            if (out) {
                env->vreg[vd & 31u][i >> 3] |= (uint8_t)(1u << (i & 7u));
            } else {
                env->vreg[vd & 31u][i >> 3] &= (uint8_t)~(1u << (i & 7u));
            }
        }
    }
    if (uses_fp) {
        latr_fp_status_end(env, &status);
    }
    env->vstart = 0;
    if (!is_cmp) {
        rvv_tail_ones(env, vd, sew);
    }
}

void latr_rvv_fp_cvt(CPUArchState *env)
{
    unsigned op = (unsigned)env->rvv_args[0];
    unsigned vd = (unsigned)env->rvv_args[1];
    unsigned vs2 = (unsigned)env->rvv_args[2];
    int vm = (int)env->rvv_args[3];
    unsigned sew = rvv_sew(env);
    int wide = op == LATR_RVV_CVT_FW_F_F || op == LATR_RVV_CVT_FW_F_X ||
               op == LATR_RVV_CVT_FW_F_XU || op == LATR_RVV_CVT_FW_X_F ||
               op == LATR_RVV_CVT_FW_XU_F;
    int narrow = op == LATR_RVV_CVT_FN_F_F || op == LATR_RVV_CVT_FN_ROD_F_F ||
                 op == LATR_RVV_CVT_FN_X_F || op == LATR_RVV_CVT_FN_XU_F ||
                 op == LATR_RVV_CVT_FN_F_X || op == LATR_RVV_CVT_FN_F_XU;
    int rtz = op == LATR_RVV_CVT_RTZ_XU_F || op == LATR_RVV_CVT_RTZ_X_F ||
              op == LATR_RVV_CVT_FW_X_F || op == LATR_RVV_CVT_FW_XU_F ||
              op == LATR_RVV_CVT_FN_X_F || op == LATR_RVV_CVT_FN_XU_F;
    int src_float = op != LATR_RVV_CVT_F_XU && op != LATR_RVV_CVT_F_X &&
                    op != LATR_RVV_CVT_FW_F_X && op != LATR_RVV_CVT_FW_F_XU &&
                    op != LATR_RVV_CVT_FN_F_X && op != LATR_RVV_CVT_FN_F_XU;
    int dst_float = op == LATR_RVV_CVT_F_XU || op == LATR_RVV_CVT_F_X ||
                    op == LATR_RVV_CVT_FW_F_F || op == LATR_RVV_CVT_FW_F_X ||
                    op == LATR_RVV_CVT_FW_F_XU || op == LATR_RVV_CVT_FN_F_F ||
                    op == LATR_RVV_CVT_FN_ROD_F_F || op == LATR_RVV_CVT_FN_F_X ||
                    op == LATR_RVV_CVT_FN_F_XU;
    int is_signed = op == LATR_RVV_CVT_X_F || op == LATR_RVV_CVT_F_X ||
                    op == LATR_RVV_CVT_RTZ_X_F || op == LATR_RVV_CVT_FW_F_X ||
                    op == LATR_RVV_CVT_FW_X_F || op == LATR_RVV_CVT_FN_X_F ||
                    op == LATR_RVV_CVT_FN_F_X;
    unsigned src_bits = wide ? sew : (narrow ? sew * 2u : sew);
    unsigned dst_bits = wide ? sew * 2u : sew;
    PvirInstruction insn;
    PvirConvertOperation *o = &insn.operation.convert;

    if ((sew != 32u && sew != 64u) || ((narrow || wide) && sew != 32u)) {
        return;
    }
    if (env->vstart >= env->vl) {
        env->vstart = 0;
        return;
    }
    rvv_build_mask(env, !vm);
    rvv_pvir_init(&insn);
    insn.opcode = PVIR_OP_CONVERT;
    insn.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
    o->source = vs2;
    o->destination = vd;
    o->source_type = rvv_pvir_type(
        src_float ? PVIR_ELEMENT_FLOAT : PVIR_ELEMENT_INTEGER,
        src_float ? PVIR_SIGNLESS : (is_signed ? PVIR_SIGNED : PVIR_UNSIGNED),
        src_bits, (unsigned)env->vl);
    o->destination_type = rvv_pvir_type(
        dst_float ? PVIR_ELEMENT_FLOAT : PVIR_ELEMENT_INTEGER,
        dst_float ? PVIR_SIGNLESS : (is_signed ? PVIR_SIGNED : PVIR_UNSIGNED),
        dst_bits, (unsigned)env->vl);
    o->rounding = rtz ? PVIR_ROUND_RTZ : PVIR_ROUND_DEFAULT;
    env->rvv_imm = op == LATR_RVV_CVT_FN_ROD_F_F ? 1u : 0u;
    latr_pvir_execute_cached(env, UINT64_C(0x700) ^ op, &insn);
    env->rvv_imm = 0;
    if (env->pvir_status == PVIR_STATUS_OK) {
        rvv_pvir_fill_inactive(env, vd, dst_bits);
    }
    env->vstart = 0;
    rvv_tail_ones(env, vd, dst_bits);
}

void latr_rvv_mv_x_s(CPUArchState *env)
{
    unsigned rd = (unsigned)env->rvv_args[0];
    unsigned vs2 = (unsigned)env->rvv_args[1];
    unsigned sew = rvv_sew(env);
    unsigned lanes = env->vl ? (unsigned)env->vl : 1u;
    PvirInstruction insn;
    PvirLaneOperation *o = &insn.operation.lane;

    rvv_pvir_init(&insn);
    insn.opcode = PVIR_OP_EXTRACT;
    o->source = vs2;
    o->destination = LATR_PVIR_SCALAR_VALUE;
    o->merge_value = PVIR_NO_VALUE;
    o->vector_type = rvv_pvir_type(PVIR_ELEMENT_INTEGER, PVIR_SIGNED, sew,
                                   lanes);
    o->lane = 0;
    latr_pvir_execute_cached(env, UINT64_C(0x780), &insn);
    if (rd != 0) {
        env->gpr[rd & 31u] = (uint64_t)rvv_sext(env->rvv_scalar, sew);
    }
    env->vstart = 0;
}

void latr_rvv_mv_s_x(CPUArchState *env)
{
    unsigned vd = (unsigned)env->rvv_args[0];
    unsigned rs1 = (unsigned)env->rvv_args[1];
    unsigned sew = rvv_sew(env);

    if (env->vstart < env->vl && env->vl > 0) {
        PvirInstruction insn;
        PvirLaneOperation *o = &insn.operation.lane;

        rvv_pvir_init(&insn);
        insn.opcode = PVIR_OP_INSERT;
        o->source = LATR_PVIR_SCALAR_VALUE;
        o->destination = vd;
        o->merge_value = vd;
        o->vector_type = rvv_pvir_type(PVIR_ELEMENT_INTEGER, PVIR_SIGNED, sew,
                                       (unsigned)env->vl);
        o->lane = 0;
        env->rvv_scalar = env->gpr[rs1 & 31u];
        latr_pvir_execute_cached(env, UINT64_C(0x790), &insn);
        if (RVV_VTYPE_VTA(env->vtype)) {
            unsigned t;
            unsigned vlmax = rvv_vlmax(env);
            for (t = 1; t < vlmax; t++) {
                latr_rvv_write_elem(env, vd, t, sew, rvv_sew_mask(sew));
            }
        }
    }
    env->vstart = 0;
}
