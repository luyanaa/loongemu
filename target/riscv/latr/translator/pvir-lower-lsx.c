/*
 * Direct adaptive PVIR -> mixed LASX (32B) + LSX (16B) lowerer for LATR RVV.
 *
 * Tier 2 (LASX, 256-bit `la_xv*`): emits 32-byte chunks when latx_cpu_has_lasx()
 *   holds, at least 32 bytes remain in the span, offset is 32-byte aligned,
 *   and every accessed vreg slice is an even-numbered register (32B aligned in
 *   CPUArchState).
 * Tier 1 (LSX, 128-bit `la_v*`): emits 16-byte chunks for LMUL <= 1, odd 16B
 *   tail chunks of LMUL >= 2 (mixed LASX + LSX inside the same instruction),
 *   mask logic (VLEN=128 -> 16B), and LSX-only hosts.
 * Tier 0 (fallback): returns false or fails latr_rvv_direct_prepare() at
 *   runtime -> falls through to the existing PVIR C helper path.
 */
#include <string.h>
#include "latr.h"
#include "translate.h"
#include "riscv-env.h"
#include "qemu/cpu-features.h"
#include "pvir-plan.h"

void latr_env_vld(IR2_OPND vector, int offset)
{
    if (offset >= -2048 && offset <= 2047) {
        la_vld(vector, env_ir2_opnd, offset);
        return;
    }
    li_d(r21_ir2_opnd, offset);
    la_add_d(r21_ir2_opnd, env_ir2_opnd, r21_ir2_opnd);
    la_vld(vector, r21_ir2_opnd, 0);
}

void latr_env_vst(IR2_OPND vector, int offset)
{
    if (offset >= -2048 && offset <= 2047) {
        la_vst(vector, env_ir2_opnd, offset);
        return;
    }
    li_d(r21_ir2_opnd, offset);
    la_add_d(r21_ir2_opnd, env_ir2_opnd, r21_ir2_opnd);
    la_vst(vector, r21_ir2_opnd, 0);
}

void latr_env_xvld(IR2_OPND vector, int offset)
{
    if (offset >= -2048 && offset <= 2047) {
        la_xvld(vector, env_ir2_opnd, offset);
        return;
    }
    li_d(r21_ir2_opnd, offset);
    la_add_d(r21_ir2_opnd, env_ir2_opnd, r21_ir2_opnd);
    la_xvld(vector, r21_ir2_opnd, 0);
}

void latr_env_xvst(IR2_OPND vector, int offset)
{
    if (offset >= -2048 && offset <= 2047) {
        la_xvst(vector, env_ir2_opnd, offset);
        return;
    }
    li_d(r21_ir2_opnd, offset);
    la_add_d(r21_ir2_opnd, env_ir2_opnd, r21_ir2_opnd);
    la_xvst(vector, r21_ir2_opnd, 0);
}

/* Packed runtime specialization descriptor passed in a1 to direct_prepare. */
#define DESC_SEW_SHIFT(d)     ((unsigned)((d) & 0xfu))
#define DESC_VL(d)            ((unsigned)(((d) >> 4) & 0xffu))
#define DESC_VLMAX_BYTES(d)   ((unsigned)(((d) >> 12) & 0xffu))
#define DESC_VM(d)            ((unsigned)(((d) >> 20) & 1u))
#define DESC_IS_FP(d)         ((unsigned)(((d) >> 21) & 1u))
#define DESC_IS_MASK(d)       ((unsigned)(((d) >> 22) & 1u))
#define DESC_IS_MERGE(d)      ((unsigned)(((d) >> 23) & 1u))
#define DESC_VTA(d)           ((unsigned)(((d) >> 24) & 1u))
#define DESC_VMA(d)           ((unsigned)(((d) >> 25) & 1u))
#define DESC_LASX_CHUNKS(d)   ((unsigned)(((d) >> 26) & 0xfu))
#define DESC_LSX_CHUNKS(d)    ((unsigned)(((d) >> 30) & 0xfu))
#define DESC_VD(d)            ((unsigned)(((d) >> 34) & 0x1fu))

static uint64_t pack_desc(unsigned sew_shift, unsigned vl,
                          unsigned vlmax_bytes, unsigned vm,
                          unsigned is_fp, unsigned is_mask,
                          unsigned is_merge, unsigned vta, unsigned vma,
                          unsigned lasx_chunks, unsigned lsx_chunks,
                          unsigned vd, uint64_t tag)
{
    return (uint64_t)(sew_shift & 0xfu) |
           ((uint64_t)(vl & 0xffu) << 4) |
           ((uint64_t)(vlmax_bytes & 0xffu) << 12) |
           ((uint64_t)(vm & 1u) << 20) |
           ((uint64_t)(is_fp & 1u) << 21) |
           ((uint64_t)(is_mask & 1u) << 22) |
           ((uint64_t)(is_merge & 1u) << 23) |
           ((uint64_t)(vta & 1u) << 24) |
           ((uint64_t)(vma & 1u) << 25) |
           ((uint64_t)(lasx_chunks & 0xfu) << 26) |
           ((uint64_t)(lsx_chunks & 0xfu) << 30) |
           ((uint64_t)(vd & 0x1fu) << 34) |
           ((tag & 0x1ffffffULL) << 39);
}

static unsigned compute_vlmax(uint64_t vtype)
{
    unsigned vsew = RVV_VTYPE_VSEW(vtype);
    unsigned vlmul = RVV_VTYPE_VLMUL(vtype);
    int lmul;
    int shift;

    if (vlmul == 4u || vsew > 3u) {
        return 0;
    }
    lmul = (vlmul & 4u) ? (int)vlmul - 8 : (int)vlmul;
    shift = (int)vsew + 3 - lmul;
    if (shift < 0 || shift > 7) {
        return 0;
    }
    return LATR_RVV_VLEN >> (unsigned)shift;
}

int latr_rvv_direct_prepare(CPUArchState *env, uint64_t packed_desc)
{
    unsigned expected_sew = DESC_SEW_SHIFT(packed_desc);
    unsigned expected_vl = DESC_VL(packed_desc);
    unsigned expected_vlmax_val = DESC_VLMAX_BYTES(packed_desc);
    unsigned expected_vm = DESC_VM(packed_desc);
    unsigned is_fp = DESC_IS_FP(packed_desc);
    unsigned is_mask = DESC_IS_MASK(packed_desc);
    unsigned is_merge = DESC_IS_MERGE(packed_desc);
    unsigned expected_vta = DESC_VTA(packed_desc);
    unsigned expected_vma = DESC_VMA(packed_desc);
    unsigned lasx_chunks = DESC_LASX_CHUNKS(packed_desc);
    unsigned lsx_chunks = DESC_LSX_CHUNKS(packed_desc);
    unsigned vd = DESC_VD(packed_desc);
    unsigned runtime_vlmax;
    PvirInstruction cache_entry;

    if (env == NULL || (((uintptr_t)env) & 31u) != 0) {
        return 0;
    }
    if (!latx_cpu_has_lsx() || (lasx_chunks > 0 && !latx_cpu_has_lasx())) {
        return 0;
    }
    if (env->vill != 0 || env->vstart != 0 || env->vl == 0 ||
        env->vl != expected_vl) {
        return 0;
    }
    runtime_vlmax = compute_vlmax(env->vtype);
    if (runtime_vlmax == 0 || env->vl > runtime_vlmax) {
        return 0;
    }
    if (RVV_VTYPE_VTA(env->vtype) != (int)expected_vta) {
        return 0;
    }

    if (is_mask) {
        unsigned i;
        if (runtime_vlmax != expected_vlmax_val) {
            return 0;
        }
        memset(env->v_expanded_mask, 0, LATR_RVV_VLENB);
        memcpy(env->v_inactive_fill, env->vreg[vd & 31u], LATR_RVV_VLENB);
        for (i = 0; i < (unsigned)env->vl; i++) {
            env->v_expanded_mask[i >> 3] |= (uint8_t)(1u << (i & 7u));
        }
        if (expected_vta) {
            for (i = (unsigned)env->vl; i < runtime_vlmax; i++) {
                env->v_inactive_fill[i >> 3] |= (uint8_t)(1u << (i & 7u));
            }
        }
    } else {
        unsigned elem_bytes = 1u << expected_sew;
        unsigned active_bytes = expected_vl * elem_bytes;
        unsigned runtime_vlmax_bytes = runtime_vlmax * elem_bytes;
        unsigned span_bytes;
        unsigned i;

        if ((unsigned)RVV_VTYPE_VSEW(env->vtype) != expected_sew ||
            RVV_VTYPE_VMA(env->vtype) != (int)expected_vma ||
            runtime_vlmax_bytes != expected_vlmax_val) {
            return 0;
        }
        if (is_fp) {
            unsigned sew_bits = 8u << expected_sew;
            unsigned frm = (unsigned)((env->fcsr >> 5) & 7u);
            if ((sew_bits != 32u && sew_bits != 64u) || frm >= 4u) {
                return 0;
            }
        }
        span_bytes = expected_vta ? runtime_vlmax_bytes
                                  : ((active_bytes + 15u) & ~15u);
        span_bytes = (span_bytes + 15u) & ~15u;
        if (is_merge && (expected_vta != 0 || active_bytes != span_bytes)) {
            return 0;
        }
        if (span_bytes == 0 || span_bytes > LATR_RVV_VLENB * 8u ||
            vd + (span_bytes / LATR_RVV_VLENB) > 32u) {
            return 0;
        }
        memset(env->v_expanded_mask, 0, span_bytes);
        memcpy(env->v_inactive_fill, env->vreg[vd], span_bytes);
        for (i = 0; i < expected_vl; i++) {
            unsigned m = (expected_vm || is_merge)
                             ? ((is_merge)
                                    ? ((env->vreg[0][i >> 3] >> (i & 7u)) & 1u)
                                    : 1u)
                             : ((env->vreg[0][i >> 3] >> (i & 7u)) & 1u);
            if (is_merge) {
                memset(&env->v_expanded_mask[i * elem_bytes],
                       m ? 0xff : 0x00, elem_bytes);
            } else {
                memset(&env->v_expanded_mask[i * elem_bytes],
                       m ? 0xff : 0x00, elem_bytes);
                if (!m && expected_vma) {
                    memset(&env->v_inactive_fill[i * elem_bytes], 0xff,
                           elem_bytes);
                }
            }
        }
        if (expected_vta && active_bytes < runtime_vlmax_bytes) {
            memset(&env->v_inactive_fill[active_bytes], 0xff,
                   runtime_vlmax_bytes - active_bytes);
        }
    }

    if (latr_pvir_cache_lookup(packed_desc, env->vtype, env->vl, NULL)) {
        env->pvir_cache_hits++;
    } else {
        memset(&cache_entry, 0, sizeof(cache_entry));
        cache_entry.effects.fault.unknown_from_lane = PVIR_NO_VALUE;
        cache_entry.opcode = PVIR_OP_MOVE;
        cache_entry.operation.vector.type.kind = PVIR_ELEMENT_INTEGER;
        cache_entry.operation.vector.type.signedness = PVIR_SIGNED;
        cache_entry.operation.vector.type.element_bits =
            is_mask ? 8u : (uint16_t)(8u << expected_sew);
        cache_entry.operation.vector.type.vector_bits =
            is_mask ? 128u : expected_vl * (8u << expected_sew);
        cache_entry.operation.vector.predicate = pvir_predicate_all_true(
            is_mask ? 16u : expected_vl);
        cache_entry.operation.vector.inactive_policy = PVIR_INACTIVE_UNKNOWN;
        cache_entry.operation.vector.merge_value = PVIR_NO_VALUE;
        cache_entry.operation.vector.destination = vd;
        cache_entry.operation.vector.operand_count = 1;
        cache_entry.operation.vector.operand_kinds[0] = PVIR_OPERAND_VECTOR;
        cache_entry.operation.vector.operands[0] = vd;
        latr_pvir_cache_store(packed_desc, env->vtype, env->vl, &cache_entry);
    }

    env->pvir_status = PVIR_STATUS_OK;
    env->pvir_count++;
    env->pvir_direct_count++;
    env->pvir_lasx_chunks += lasx_chunks;
    env->pvir_lsx_chunks += lsx_chunks;
    return 1;
}

typedef enum {
    DIRECT_KIND_NONE = 0,
    DIRECT_KIND_INT_ALU,
    DIRECT_KIND_MOVE,
    DIRECT_KIND_MERGE,
    DIRECT_KIND_MASK_LOGIC,
    DIRECT_KIND_FP_ARITH,
} DirectFamily;

typedef enum {
    DIRECT_SRC_VV = 0,
    DIRECT_SRC_VX,
    DIRECT_SRC_VI,
    DIRECT_SRC_VF,
    DIRECT_SRC_UNARY,
} DirectSrcKind;

typedef struct {
    DirectFamily family;
    DirectSrcKind src_kind;
    unsigned subop;
    bool swapped;
    bool imm_signed;
    bool unmasked;
} DirectMatch;

static bool match_direct_op(const RiscvIR1 *ir1, DirectMatch *m)
{
    memset(m, 0, sizeof(*m));
    m->unmasked = ir1->arg.vm != 0;
    switch (ir1->opcode) {
    case LATR_RV_VADD_VV: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_ADD; m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VADD_VX: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_ADD; m->src_kind = DIRECT_SRC_VX; return true;
    case LATR_RV_VADD_VI: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_ADD; m->src_kind = DIRECT_SRC_VI; m->imm_signed = true; return true;
    case LATR_RV_VSUB_VV: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_SUB; m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VSUB_VX: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_SUB; m->src_kind = DIRECT_SRC_VX; return true;
    case LATR_RV_VMUL_VV: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_MUL; m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VMUL_VX: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_MUL; m->src_kind = DIRECT_SRC_VX; return true;
    case LATR_RV_VAND_VV: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_AND; m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VAND_VX: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_AND; m->src_kind = DIRECT_SRC_VX; return true;
    case LATR_RV_VAND_VI: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_AND; m->src_kind = DIRECT_SRC_VI; m->imm_signed = true; return true;
    case LATR_RV_VOR_VV:  m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_OR;  m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VOR_VX:  m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_OR;  m->src_kind = DIRECT_SRC_VX; return true;
    case LATR_RV_VOR_VI:  m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_OR;  m->src_kind = DIRECT_SRC_VI; m->imm_signed = true; return true;
    case LATR_RV_VXOR_VV: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_XOR; m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VXOR_VX: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_XOR; m->src_kind = DIRECT_SRC_VX; return true;
    case LATR_RV_VXOR_VI: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_XOR; m->src_kind = DIRECT_SRC_VI; m->imm_signed = true; return true;
    case LATR_RV_VSLL_VV: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_SLL; m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VSLL_VX: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_SLL; m->src_kind = DIRECT_SRC_VX; return true;
    case LATR_RV_VSLL_VI: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_SLL; m->src_kind = DIRECT_SRC_VI; m->imm_signed = false; return true;
    case LATR_RV_VSRL_VV: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_SRL; m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VSRL_VX: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_SRL; m->src_kind = DIRECT_SRC_VX; return true;
    case LATR_RV_VSRL_VI: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_SRL; m->src_kind = DIRECT_SRC_VI; m->imm_signed = false; return true;
    case LATR_RV_VSRA_VV: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_SRA; m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VSRA_VX: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_SRA; m->src_kind = DIRECT_SRC_VX; return true;
    case LATR_RV_VSRA_VI: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_SRA; m->src_kind = DIRECT_SRC_VI; m->imm_signed = false; return true;
    case LATR_RV_VMIN_VV:  m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_MIN;  m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VMIN_VX:  m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_MIN;  m->src_kind = DIRECT_SRC_VX; return true;
    case LATR_RV_VMINU_VV: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_MINU; m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VMINU_VX: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_MINU; m->src_kind = DIRECT_SRC_VX; return true;
    case LATR_RV_VMAX_VV:  m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_MAX;  m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VMAX_VX:  m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_MAX;  m->src_kind = DIRECT_SRC_VX; return true;
    case LATR_RV_VMAXU_VV: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_MAXU; m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VMAXU_VX: m->family = DIRECT_KIND_INT_ALU; m->subop = LATR_RVV_OP_MAXU; m->src_kind = DIRECT_SRC_VX; return true;

    case LATR_RV_VMV_V_V: m->family = DIRECT_KIND_MOVE; m->src_kind = DIRECT_SRC_VV; m->unmasked = true; return true;
    case LATR_RV_VMV_V_X: m->family = DIRECT_KIND_MOVE; m->src_kind = DIRECT_SRC_VX; m->unmasked = true; return true;
    case LATR_RV_VMV_V_I: m->family = DIRECT_KIND_MOVE; m->src_kind = DIRECT_SRC_VI; m->imm_signed = true; m->unmasked = true; return true;
    case LATR_RV_VFMV_V_F: m->family = DIRECT_KIND_MOVE; m->src_kind = DIRECT_SRC_VF; m->unmasked = true; return true;

    case LATR_RV_VMERGE_VVM: m->family = DIRECT_KIND_MERGE; m->src_kind = DIRECT_SRC_VV; m->unmasked = true; return true;
    case LATR_RV_VMERGE_VXM: m->family = DIRECT_KIND_MERGE; m->src_kind = DIRECT_SRC_VX; m->unmasked = true; return true;
    case LATR_RV_VMERGE_VIM: m->family = DIRECT_KIND_MERGE; m->src_kind = DIRECT_SRC_VI; m->imm_signed = true; m->unmasked = true; return true;
    case LATR_RV_VFMERGE_VFM: m->family = DIRECT_KIND_MERGE; m->src_kind = DIRECT_SRC_VF; m->unmasked = true; return true;

    case LATR_RV_VMAND_MM:  m->family = DIRECT_KIND_MASK_LOGIC; m->subop = LATR_RVV_MASK_AND;  m->src_kind = DIRECT_SRC_VV; m->unmasked = true; return true;
    case LATR_RV_VMNAND_MM: m->family = DIRECT_KIND_MASK_LOGIC; m->subop = LATR_RVV_MASK_NAND; m->src_kind = DIRECT_SRC_VV; m->unmasked = true; return true;
    case LATR_RV_VMANDN_MM: m->family = DIRECT_KIND_MASK_LOGIC; m->subop = LATR_RVV_MASK_ANDN; m->src_kind = DIRECT_SRC_VV; m->unmasked = true; return true;
    case LATR_RV_VMXOR_MM:  m->family = DIRECT_KIND_MASK_LOGIC; m->subop = LATR_RVV_MASK_XOR;  m->src_kind = DIRECT_SRC_VV; m->unmasked = true; return true;
    case LATR_RV_VMOR_MM:   m->family = DIRECT_KIND_MASK_LOGIC; m->subop = LATR_RVV_MASK_OR;   m->src_kind = DIRECT_SRC_VV; m->unmasked = true; return true;
    case LATR_RV_VMNOR_MM:  m->family = DIRECT_KIND_MASK_LOGIC; m->subop = LATR_RVV_MASK_NOR;  m->src_kind = DIRECT_SRC_VV; m->unmasked = true; return true;
    case LATR_RV_VMORN_MM:  m->family = DIRECT_KIND_MASK_LOGIC; m->subop = LATR_RVV_MASK_ORN;  m->src_kind = DIRECT_SRC_VV; m->unmasked = true; return true;
    case LATR_RV_VMXNOR_MM: m->family = DIRECT_KIND_MASK_LOGIC; m->subop = LATR_RVV_MASK_XNOR; m->src_kind = DIRECT_SRC_VV; m->unmasked = true; return true;

    case LATR_RV_VFADD_VV:  m->family = DIRECT_KIND_FP_ARITH; m->subop = PVIR_FP_ADD;  m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VFADD_VF:  m->family = DIRECT_KIND_FP_ARITH; m->subop = PVIR_FP_ADD;  m->src_kind = DIRECT_SRC_VF; return true;
    case LATR_RV_VFSUB_VV:  m->family = DIRECT_KIND_FP_ARITH; m->subop = PVIR_FP_SUB;  m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VFSUB_VF:  m->family = DIRECT_KIND_FP_ARITH; m->subop = PVIR_FP_SUB;  m->src_kind = DIRECT_SRC_VF; return true;
    case LATR_RV_VFMUL_VV:  m->family = DIRECT_KIND_FP_ARITH; m->subop = PVIR_FP_MUL;  m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VFMUL_VF:  m->family = DIRECT_KIND_FP_ARITH; m->subop = PVIR_FP_MUL;  m->src_kind = DIRECT_SRC_VF; return true;
    case LATR_RV_VFDIV_VV:  m->family = DIRECT_KIND_FP_ARITH; m->subop = PVIR_FP_DIV;  m->src_kind = DIRECT_SRC_VV; return true;
    case LATR_RV_VFDIV_VF:  m->family = DIRECT_KIND_FP_ARITH; m->subop = PVIR_FP_DIV;  m->src_kind = DIRECT_SRC_VF; return true;
    case LATR_RV_VFSQRT_V:  m->family = DIRECT_KIND_FP_ARITH; m->subop = PVIR_FP_SQRT; m->src_kind = DIRECT_SRC_UNARY; return true;
    default:
        return false;
    }
}

static bool validate_direct_pvir(const RiscvIR1 *ir1, const DirectMatch *m,
                                 unsigned sew_bits, unsigned vl,
                                 const CPUArchState *env)
{
    PvirInstruction insn;
    PvirPredicateSource src = { PVIR_PREDICATE_SOURCE_CONSTANT, 0, 0 };

    memset(&insn, 0, sizeof(insn));
    insn.effects.fault.unknown_from_lane = PVIR_NO_VALUE;

    if (m->family == DIRECT_KIND_FP_ARITH) {
        PvirFPOpOperation *fp = &insn.operation.fp;
        insn.opcode = PVIR_OP_FP_ARITH;
        insn.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
        fp->type = (PvirVectorType){ PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS,
                                     (uint16_t)sew_bits, sew_bits * vl };
        fp->predicate = m->unmasked
                            ? pvir_predicate_all_true(vl)
                            : pvir_predicate_bitset(src, vl, env->v_mask_words, 2);
        fp->inactive_policy = PVIR_INACTIVE_UNKNOWN;
        fp->merge_value = PVIR_NO_VALUE;
        fp->destination = (unsigned)ir1->arg.rd;
        fp->kind = (PvirFPKind)m->subop;
        fp->rounding = PVIR_ROUND_DEFAULT;
        fp->operands[0] = (unsigned)ir1->arg.rs2;
        fp->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        if (m->src_kind == DIRECT_SRC_UNARY) {
            fp->operands[1] = PVIR_NO_VALUE;
        } else {
            fp->operands[1] = m->src_kind == DIRECT_SRC_VV
                                  ? (unsigned)ir1->arg.rs1
                                  : LATR_PVIR_SCALAR_VALUE;
            fp->operand_kinds[1] = m->src_kind == DIRECT_SRC_VV
                                       ? PVIR_OPERAND_VECTOR
                                       : PVIR_OPERAND_SCALAR;
        }
        return pvir_instruction_validate(&insn) == PVIR_STATUS_OK;
    }

    {
        PvirVectorOperation *o = &insn.operation.vector;
        if (m->family == DIRECT_KIND_MASK_LOGIC) {
            unsigned bytes = (vl + 7u) / 8u;
            insn.opcode = PVIR_OP_AND;
            o->type = (PvirVectorType){ PVIR_ELEMENT_BITS, PVIR_SIGNLESS, 8u,
                                        bytes * 8u };
            o->predicate = pvir_predicate_all_true(bytes);
            o->inactive_policy = PVIR_INACTIVE_UNKNOWN;
            o->merge_value = PVIR_NO_VALUE;
            o->destination = (unsigned)ir1->arg.rd;
            o->operand_count = 2;
            o->operand_kinds[0] = PVIR_OPERAND_VECTOR;
            o->operands[0] = (unsigned)ir1->arg.rs2;
            o->operand_kinds[1] = PVIR_OPERAND_VECTOR;
            o->operands[1] = (unsigned)ir1->arg.rs1;
            return pvir_instruction_validate(&insn) == PVIR_STATUS_OK;
        }

        o->type = (PvirVectorType){
            m->src_kind == DIRECT_SRC_VF ? PVIR_ELEMENT_FLOAT
                                         : PVIR_ELEMENT_INTEGER,
            m->src_kind == DIRECT_SRC_VF
                ? PVIR_SIGNLESS
                : ((m->subop == LATR_RVV_OP_MINU ||
                    m->subop == LATR_RVV_OP_MAXU)
                       ? PVIR_UNSIGNED
                       : PVIR_SIGNED),
            (uint16_t)sew_bits, sew_bits * vl
        };
        o->predicate = m->unmasked
                           ? pvir_predicate_all_true(vl)
                           : pvir_predicate_bitset(src, vl, env->v_mask_words, 2);
        o->inactive_policy = PVIR_INACTIVE_UNKNOWN;
        o->merge_value = PVIR_NO_VALUE;
        o->destination = (unsigned)ir1->arg.rd;

        if (m->family == DIRECT_KIND_MOVE) {
            insn.opcode = m->src_kind == DIRECT_SRC_VV ? PVIR_OP_MOVE
                                                       : PVIR_OP_BROADCAST;
            o->operand_count = 1;
            o->operand_kinds[0] = m->src_kind == DIRECT_SRC_VV
                                      ? PVIR_OPERAND_VECTOR
                                      : PVIR_OPERAND_SCALAR;
            o->operands[0] = m->src_kind == DIRECT_SRC_VV
                                 ? (unsigned)ir1->arg.rs1
                                 : LATR_PVIR_SCALAR_VALUE;
        } else if (m->family == DIRECT_KIND_MERGE) {
            insn.opcode = PVIR_OP_SELECT;
            o->operand_count = 3;
            o->operand_kinds[0] = PVIR_OPERAND_VECTOR;
            o->operands[0] = LATR_PVIR_V0_VALUE;
            o->operand_kinds[1] = PVIR_OPERAND_VECTOR;
            o->operands[1] = (unsigned)ir1->arg.rs2;
            o->operand_kinds[2] = PVIR_OPERAND_VECTOR;
            o->operands[2] = m->src_kind == DIRECT_SRC_VV
                                 ? (unsigned)ir1->arg.rs1
                                 : LATR_PVIR_SCALAR_VALUE;
            o->operand_types[0] = o->operand_types[1] = o->operand_types[2] =
                o->type;
        } else {
            switch (m->subop) {
            case LATR_RVV_OP_ADD:  insn.opcode = PVIR_OP_ADD; break;
            case LATR_RVV_OP_SUB:  insn.opcode = PVIR_OP_SUB; break;
            case LATR_RVV_OP_MUL:  insn.opcode = PVIR_OP_MUL; break;
            case LATR_RVV_OP_AND:  insn.opcode = PVIR_OP_AND; break;
            case LATR_RVV_OP_OR:   insn.opcode = PVIR_OP_OR;  break;
            case LATR_RVV_OP_XOR:  insn.opcode = PVIR_OP_XOR; break;
            case LATR_RVV_OP_SLL:  insn.opcode = PVIR_OP_SHL; break;
            case LATR_RVV_OP_SRL:  insn.opcode = PVIR_OP_SHR; break;
            case LATR_RVV_OP_SRA:  insn.opcode = PVIR_OP_SAR; break;
            case LATR_RVV_OP_MINU:
            case LATR_RVV_OP_MIN:  insn.opcode = PVIR_OP_MIN; break;
            case LATR_RVV_OP_MAXU:
            case LATR_RVV_OP_MAX:  insn.opcode = PVIR_OP_MAX; break;
            default: return false;
            }
            o->operand_count = 2;
            o->operand_kinds[0] = PVIR_OPERAND_VECTOR;
            o->operands[0] = (unsigned)ir1->arg.rs2;
            if (m->src_kind == DIRECT_SRC_VV) {
                o->operand_kinds[1] = PVIR_OPERAND_VECTOR;
                o->operands[1] = (unsigned)ir1->arg.rs1;
            } else {
                o->operand_kinds[1] = PVIR_OPERAND_SCALAR;
                o->operands[1] = LATR_PVIR_SCALAR_VALUE;
            }
        }
        return pvir_instruction_validate(&insn) == PVIR_STATUS_OK;
    }
}

typedef IR2_INST *(*VecBinOp)(IR2_OPND, IR2_OPND, IR2_OPND);
typedef IR2_INST *(*VecUnaryOp)(IR2_OPND, IR2_OPND);

static void emit_vec_fp_begin(void)
{
    IR2_OPND value = ra_alloc_itemp();
    IR2_OPND frm = ra_alloc_itemp();

    la_movgr2fcsr(fcsr1_ir2_opnd, zero_ir2_opnd);
    la_movgr2fcsr(fcsr2_ir2_opnd, zero_ir2_opnd);
    la_ld_d(value, env_ir2_opnd, env_offset_fcsr());
    la_bstrpick_d(frm, value, 7, 5);
    la_srli_d(value, frm, 1);
    la_andi(value, value, 1);
    la_xor(frm, frm, value);
    la_slli_d(frm, frm, 8);
    la_movgr2fcsr(fcsr3_ir2_opnd, frm);
    ra_free_temp(frm);
    ra_free_temp(value);
}

static void emit_vec_fp_end(void)
{
    IR2_OPND flags = ra_alloc_itemp();
    IR2_OPND fcsr = ra_alloc_itemp();

    la_movfcsr2gr(flags, fcsr2_ir2_opnd);
    la_bstrpick_d(flags, flags, 28, 24);
    la_ld_d(fcsr, env_ir2_opnd, env_offset_fcsr());
    la_or(fcsr, fcsr, flags);
    la_st_d(fcsr, env_ir2_opnd, env_offset_fcsr());
    ra_free_temp(fcsr);
    ra_free_temp(flags);
}

static void emit_repl_gpr(IR2_OPND dst, IR2_OPND src, unsigned sew_shift,
                          bool lasx)
{
    static VecUnaryOp const repl_v[4] = {
        la_vreplgr2vr_b, la_vreplgr2vr_h, la_vreplgr2vr_w, la_vreplgr2vr_d,
    };
    static VecUnaryOp const repl_xv[4] = {
        la_xvreplgr2vr_b, la_xvreplgr2vr_h, la_xvreplgr2vr_w, la_xvreplgr2vr_d,
    };
    (lasx ? repl_xv : repl_v)[sew_shift & 3u](dst, src);
}

static void emit_broadcast_scalar(IR2_OPND b_vec, const RiscvIR1 *ir1,
                                  const DirectMatch *m, unsigned sew_shift,
                                  bool lasx)
{
    if (m->src_kind == DIRECT_SRC_VI) {
        IR2_OPND tmp = ra_alloc_itemp();
        uint64_t imm = (uint64_t)ir1->arg.rs1;
        if (m->imm_signed) {
            imm = (uint64_t)((int64_t)(imm << 59) >> 59);
        }
        li_d(tmp, (int64_t)imm);
        emit_repl_gpr(b_vec, tmp, sew_shift, lasx);
        ra_free_temp(tmp);
    } else if (m->src_kind == DIRECT_SRC_VX) {
        IR2_OPND src = alloc_gpr_src(ir1->arg.rs1);
        emit_repl_gpr(b_vec, src, sew_shift, lasx);
        free_alloc_gpr(src);
    } else if (m->src_kind == DIRECT_SRC_VF) {
        latr_regcache_flush();
        IR2_OPND val = ra_alloc_itemp();
        latr_env_load_d(val, env_offset_fpr(ir1->arg.rs1 & 31));
        if (sew_shift == 2u) {
            IR2_OPND hi = ra_alloc_itemp();
            IR2_OPND ref = ra_alloc_itemp();
            la_srli_d(hi, val, 32);
            li_d(ref, UINT32_MAX);
            la_xor(hi, hi, ref);
            li_d(ref, INT64_C(0x7fc00000));
            la_masknez(val, val, hi);
            la_maskeqz(ref, ref, hi);
            la_or(val, val, ref);
            ra_free_temp(ref);
            ra_free_temp(hi);
        }
        emit_repl_gpr(b_vec, val, sew_shift, lasx);
        ra_free_temp(val);
    }
}

static void emit_int_alu_op(unsigned subop, unsigned sew_shift, bool lasx,
                            IR2_OPND res, IR2_OPND a, IR2_OPND b)
{
    static VecBinOp const add_v[4]  = { la_vadd_b,  la_vadd_h,  la_vadd_w,  la_vadd_d };
    static VecBinOp const add_xv[4] = { la_xvadd_b, la_xvadd_h, la_xvadd_w, la_xvadd_d };
    static VecBinOp const sub_v[4]  = { la_vsub_b,  la_vsub_h,  la_vsub_w,  la_vsub_d };
    static VecBinOp const sub_xv[4] = { la_xvsub_b, la_xvsub_h, la_xvsub_w, la_xvsub_d };
    static VecBinOp const mul_v[4]  = { la_vmul_b,  la_vmul_h,  la_vmul_w,  la_vmul_d };
    static VecBinOp const mul_xv[4] = { la_xvmul_b, la_xvmul_h, la_xvmul_w, la_xvmul_d };
    static VecBinOp const sll_v[4]  = { la_vsll_b,  la_vsll_h,  la_vsll_w,  la_vsll_d };
    static VecBinOp const sll_xv[4] = { la_xvsll_b, la_xvsll_h, la_xvsll_w, la_xvsll_d };
    static VecBinOp const srl_v[4]  = { la_vsrl_b,  la_vsrl_h,  la_vsrl_w,  la_vsrl_d };
    static VecBinOp const srl_xv[4] = { la_xvsrl_b, la_xvsrl_h, la_xvsrl_w, la_xvsrl_d };
    static VecBinOp const sra_v[4]  = { la_vsra_b,  la_vsra_h,  la_vsra_w,  la_vsra_d };
    static VecBinOp const sra_xv[4] = { la_xvsra_b, la_xvsra_h, la_xvsra_w, la_xvsra_d };
    static VecBinOp const min_v[4]  = { la_vmin_b,  la_vmin_h,  la_vmin_w,  la_vmin_d };
    static VecBinOp const min_xv[4] = { la_xvmin_b, la_xvmin_h, la_xvmin_w, la_xvmin_d };
    static VecBinOp const minu_v[4] = { la_vmin_bu, la_vmin_hu, la_vmin_wu, la_vmin_du };
    static VecBinOp const minu_xv[4]= { la_xvmin_bu,la_xvmin_hu,la_xvmin_wu,la_xvmin_du };
    static VecBinOp const max_v[4]  = { la_vmax_b,  la_vmax_h,  la_vmax_w,  la_vmax_d };
    static VecBinOp const max_xv[4] = { la_xvmax_b, la_xvmax_h, la_xvmax_w, la_xvmax_d };
    static VecBinOp const maxu_v[4] = { la_vmax_bu, la_vmax_hu, la_vmax_wu, la_vmax_du };
    static VecBinOp const maxu_xv[4]= { la_xvmax_bu,la_xvmax_hu,la_xvmax_wu,la_xvmax_du };
    unsigned idx = sew_shift & 3u;

    switch (subop) {
    case LATR_RVV_OP_ADD:  (lasx ? add_xv  : add_v)[idx](res, a, b); break;
    case LATR_RVV_OP_SUB:  (lasx ? sub_xv  : sub_v)[idx](res, a, b); break;
    case LATR_RVV_OP_MUL:  (lasx ? mul_xv  : mul_v)[idx](res, a, b); break;
    case LATR_RVV_OP_SLL:  (lasx ? sll_xv  : sll_v)[idx](res, a, b); break;
    case LATR_RVV_OP_SRL:  (lasx ? srl_xv  : srl_v)[idx](res, a, b); break;
    case LATR_RVV_OP_SRA:  (lasx ? sra_xv  : sra_v)[idx](res, a, b); break;
    case LATR_RVV_OP_MIN:  (lasx ? min_xv  : min_v)[idx](res, a, b); break;
    case LATR_RVV_OP_MINU: (lasx ? minu_xv : minu_v)[idx](res, a, b); break;
    case LATR_RVV_OP_MAX:  (lasx ? max_xv  : max_v)[idx](res, a, b); break;
    case LATR_RVV_OP_MAXU: (lasx ? maxu_xv : maxu_v)[idx](res, a, b); break;
    case LATR_RVV_OP_AND:  (lasx ? la_xvand_v : la_vand_v)(res, a, b); break;
    case LATR_RVV_OP_OR:   (lasx ? la_xvor_v  : la_vor_v)(res, a, b);  break;
    case LATR_RVV_OP_XOR:  (lasx ? la_xvxor_v : la_vxor_v)(res, a, b); break;
    default: break;
    }
}

static void emit_fp_alu_op(unsigned subop, unsigned sew_shift, bool lasx,
                           IR2_OPND res, IR2_OPND a, IR2_OPND b)
{
    bool f64 = sew_shift == 3u;
    switch (subop) {
    case PVIR_FP_ADD:
        (lasx ? (f64 ? la_xvfadd_d : la_xvfadd_s)
              : (f64 ? la_vfadd_d : la_vfadd_s))(res, a, b);
        break;
    case PVIR_FP_SUB:
        (lasx ? (f64 ? la_xvfsub_d : la_xvfsub_s)
              : (f64 ? la_vfsub_d : la_vfsub_s))(res, a, b);
        break;
    case PVIR_FP_MUL:
        (lasx ? (f64 ? la_xvfmul_d : la_xvfmul_s)
              : (f64 ? la_vfmul_d : la_vfmul_s))(res, a, b);
        break;
    case PVIR_FP_DIV:
        (lasx ? (f64 ? la_xvfdiv_d : la_xvfdiv_s)
              : (f64 ? la_vfdiv_d : la_vfdiv_s))(res, a, b);
        break;
    case PVIR_FP_SQRT:
        (lasx ? (f64 ? la_xvfsqrt_d : la_xvfsqrt_s)
              : (f64 ? la_vfsqrt_d : la_vfsqrt_s))(res, a);
        break;
    default:
        break;
    }
}

static void load_fp_one(IR2_OPND scratch, unsigned sew_shift, bool lasx)
{
    IR2_OPND one = ra_alloc_itemp();
    li_d(one, sew_shift == 3u ? (int64_t)UINT64_C(0x3ff0000000000000)
                              : (int64_t)UINT64_C(0x3f800000));
    emit_repl_gpr(scratch, one, sew_shift, lasx);
    ra_free_temp(one);
}

typedef struct RvvChunkLegalCtx {
    const DirectMatch *m;
    unsigned vd;
    unsigned a_reg;
    unsigned b_reg;
} RvvChunkLegalCtx;

/* Shared-planner legality: a `width`-byte chunk at `offset` may issue only when
 * every register it touches keeps LMUL even-pair alignment. The planner itself
 * checks has_lasx/32B-align/remaining; this supplies the RVV register rule. */
static bool rvv_chunk_legal(void *opaque, unsigned offset, unsigned width)
{
    const RvvChunkLegalCtx *c = opaque;
    unsigned reg_idx = offset / LATR_RVV_VLENB;

    if (width < 32u) {
        return true;
    }
    if (((c->vd + reg_idx) & 1u) != 0) {
        return false;
    }
    if (c->m->family != DIRECT_KIND_MOVE && ((c->a_reg + reg_idx) & 1u) != 0) {
        return false;
    }
    if (c->m->src_kind == DIRECT_SRC_VV && ((c->b_reg + reg_idx) & 1u) != 0) {
        return false;
    }
    return true;
}

bool latr_pvir_try_lower_lsx(const RiscvIR1 *ir1, IR2_OPND *fallback_label,
                             IR2_OPND *done_label)
{
    const CPUArchState *env = lsenv != NULL ? lsenv->cpu_state : NULL;
    DirectMatch m;
    unsigned sew_shift;
    unsigned sew_bits;
    unsigned vlmax;
    unsigned vl;
    unsigned vta;
    unsigned vma;
    unsigned vd;
    unsigned a_reg;
    unsigned b_reg;
    unsigned span_bytes;
    PvirPlan plan;
    RvvChunkLegalCtx legal_ctx;
    bool has_lasx = latx_cpu_has_lasx() != 0;
    bool is_fp_arith;
    uint64_t packed;

    if (ir1 == NULL || env == NULL || !latx_cpu_has_lsx()) {
        return false;
    }
    if (env->vill != 0 || env->vstart != 0 || env->vl == 0 ||
        env->vl > LATR_RVV_VLEN) {
        return false;
    }
    if (!match_direct_op(ir1, &m)) {
        return false;
    }

    sew_shift = (unsigned)RVV_VTYPE_VSEW(env->vtype);
    sew_bits = 8u << sew_shift;
    vlmax = compute_vlmax(env->vtype);
    vl = (unsigned)env->vl;
    vta = (unsigned)RVV_VTYPE_VTA(env->vtype);
    vma = (unsigned)RVV_VTYPE_VMA(env->vtype);
    if (sew_shift > 3u || vlmax == 0 || vl > vlmax) {
        return false;
    }
    if (m.family == DIRECT_KIND_FP_ARITH ||
        (m.family != DIRECT_KIND_MASK_LOGIC && m.src_kind == DIRECT_SRC_VF)) {
        if (sew_bits != 32u && sew_bits != 64u) {
            return false;
        }
    }

    vd = (unsigned)ir1->arg.rd & 31u;
    a_reg = (m.family == DIRECT_KIND_MOVE && m.src_kind == DIRECT_SRC_VV)
                ? ((unsigned)ir1->arg.rs1 & 31u)
                : ((unsigned)ir1->arg.rs2 & 31u);
    b_reg = (unsigned)ir1->arg.rs1 & 31u;

    legal_ctx = (RvvChunkLegalCtx){&m, vd, a_reg, b_reg};
    memset(&plan, 0, sizeof(plan));

    if (m.family == DIRECT_KIND_MASK_LOGIC) {
        /* One 16B chunk regardless of LASX. */
        span_bytes = LATR_RVV_VLENB;
        if (pvir_plan_chunks(&plan, span_bytes, false, NULL, NULL) !=
            PVIR_STATUS_OK) {
            return false;
        }
    } else {
        unsigned elem_bytes = 1u << sew_shift;
        unsigned active_bytes = vl * elem_bytes;
        unsigned vlmax_bytes = vlmax * elem_bytes;
        unsigned nregs;

        span_bytes = vta ? vlmax_bytes : ((active_bytes + 15u) & ~15u);
        span_bytes = (span_bytes + 15u) & ~15u;
        if (m.family == DIRECT_KIND_MERGE &&
            (vta != 0 || active_bytes != span_bytes)) {
            return false;
        }
        nregs = span_bytes / LATR_RVV_VLENB;
        if (span_bytes == 0 || span_bytes > LATR_RVV_VLENB * 8u ||
            vd + nregs > 32u ||
            (m.family != DIRECT_KIND_MOVE && a_reg + nregs > 32u) ||
            (m.src_kind == DIRECT_SRC_VV &&
             m.family != DIRECT_KIND_MOVE && b_reg + nregs > 32u) ||
            (m.family == DIRECT_KIND_MOVE && m.src_kind == DIRECT_SRC_VV &&
             a_reg + nregs > 32u)) {
            return false;
        }

        if (pvir_plan_chunks(&plan, span_bytes, has_lasx, rvv_chunk_legal,
                             &legal_ctx) != PVIR_STATUS_OK) {
            return false;
        }
    }

    if (!validate_direct_pvir(ir1, &m, sew_bits, vl, env)) {
        return false;
    }

    is_fp_arith = m.family == DIRECT_KIND_FP_ARITH;
    packed = pack_desc(
        sew_shift, vl,
        m.family == DIRECT_KIND_MASK_LOGIC ? vlmax : vlmax * (1u << sew_shift),
        m.unmasked ? 1u : 0u,
        (is_fp_arith || m.src_kind == DIRECT_SRC_VF) ? 1u : 0u,
        m.family == DIRECT_KIND_MASK_LOGIC ? 1u : 0u,
        m.family == DIRECT_KIND_MERGE ? 1u : 0u, vta, vma, plan.lasx_chunks,
        plan.lsx_chunks, vd,
        ((ir1->pc & 0xffffu) ^ ((uint64_t)(unsigned)ir1->opcode << 16)));

    *fallback_label = ir2_opnd_new_type(IR2_OPND_LABEL);
    *done_label = ir2_opnd_new_type(IR2_OPND_LABEL);

    {
        IR2_OPND target = ra_alloc_itemp();
        la_mov64(a0_ir2_opnd, env_ir2_opnd);
        li_d(a1_ir2_opnd, (int64_t)packed);
        latr_env_store_d(ra_ir2_opnd, env_offset_host_return_address());
        li_d(target, (int64_t)(uintptr_t)&latr_rvv_direct_prepare);
        la_jirl(ra_ir2_opnd, target, 0);
        ra_free_temp(target);
        latr_env_load_d(ra_ir2_opnd, env_offset_host_return_address());
        la_beq(a0_ir2_opnd, zero_ir2_opnd, *fallback_label);
    }

    {
        IR2_OPND a_vec = ra_alloc_ftemp();
        IR2_OPND b_vec = ra_alloc_ftemp();
        IR2_OPND res = ra_alloc_ftemp();
        IR2_OPND mask = ra_alloc_ftemp();
        IR2_OPND scratch = ra_alloc_ftemp();
        bool scalar_b = m.src_kind == DIRECT_SRC_VX ||
                        m.src_kind == DIRECT_SRC_VI ||
                        m.src_kind == DIRECT_SRC_VF;
        unsigned byte = 0;
        unsigned idx;

        if (is_fp_arith) {
            emit_vec_fp_begin();
        }
        if (scalar_b) {
            emit_broadcast_scalar(b_vec, ir1, &m, sew_shift,
                                  plan.lasx_chunks > 0);
        }

        for (idx = 0; idx < plan.chunk_count; idx++) {
            bool lasx = plan.steps[idx].size == 32u;
            int mask_off = env_offset_v_expanded_mask((int)byte);
            int fill_off = env_offset_v_inactive_fill((int)byte);
            int out_off = span_bytes > LATR_RVV_VLENB
                              ? env_offset_v_staging((int)byte)
                              : env_offset_vreg((int)vd) + (int)byte;

            (lasx ? latr_env_xvld : latr_env_vld)(mask, mask_off);

            if (m.family == DIRECT_KIND_MOVE) {
                if (m.src_kind == DIRECT_SRC_VV) {
                    (lasx ? latr_env_xvld : latr_env_vld)(
                        res, env_offset_vreg((int)a_reg) + (int)byte);
                } else {
                    (lasx ? la_xvor_v : la_vor_v)(res, b_vec, b_vec);
                }
            } else {
                (lasx ? latr_env_xvld : latr_env_vld)(
                    a_vec, env_offset_vreg((int)a_reg) + (int)byte);
                if (m.src_kind == DIRECT_SRC_VV) {
                    (lasx ? latr_env_xvld : latr_env_vld)(
                        b_vec, env_offset_vreg((int)b_reg) + (int)byte);
                }

                if (m.family == DIRECT_KIND_INT_ALU) {
                    emit_int_alu_op(m.subop, sew_shift, lasx, res, a_vec,
                                    b_vec);
                } else if (m.family == DIRECT_KIND_MERGE) {
                    (lasx ? la_xvbitsel_v : la_vbitsel_v)(res, b_vec, a_vec,
                                                           mask);
                } else if (m.family == DIRECT_KIND_MASK_LOGIC) {
                    switch (m.subop) {
                    case LATR_RVV_MASK_AND:
                        la_vand_v(res, a_vec, b_vec);
                        break;
                    case LATR_RVV_MASK_OR:
                        la_vor_v(res, a_vec, b_vec);
                        break;
                    case LATR_RVV_MASK_XOR:
                        la_vxor_v(res, a_vec, b_vec);
                        break;
                    case LATR_RVV_MASK_NOR:
                        la_vnor_v(res, a_vec, b_vec);
                        break;
                    case LATR_RVV_MASK_NAND:
                        la_vand_v(res, a_vec, b_vec);
                        la_vnor_v(res, res, res);
                        break;
                    case LATR_RVV_MASK_XNOR:
                        la_vxor_v(res, a_vec, b_vec);
                        la_vnor_v(res, res, res);
                        break;
                    case LATR_RVV_MASK_ANDN:
                        la_vandn_v(res, b_vec, a_vec);
                        break;
                    case LATR_RVV_MASK_ORN:
                        la_vorn_v(res, a_vec, b_vec);
                        break;
                    default:
                        break;
                    }
                } else if (m.family == DIRECT_KIND_FP_ARITH) {
                    (lasx ? la_xvxor_v : la_vxor_v)(scratch, scratch, scratch);
                    if (m.src_kind == DIRECT_SRC_UNARY) {
                        (lasx ? la_xvbitsel_v : la_vbitsel_v)(
                            a_vec, scratch, a_vec, mask);
                        emit_fp_alu_op(m.subop, sew_shift, lasx, res, a_vec,
                                       a_vec);
                    } else if (m.swapped) {
                        if (m.subop == PVIR_FP_DIV) {
                            load_fp_one(scratch, sew_shift, lasx);
                        }
                        (lasx ? la_xvbitsel_v : la_vbitsel_v)(
                            a_vec, scratch, a_vec, mask);
                        (lasx ? la_xvxor_v : la_vxor_v)(
                            scratch, scratch, scratch);
                        (lasx ? la_xvbitsel_v : la_vbitsel_v)(
                            res, scratch, b_vec, mask);
                        emit_fp_alu_op(m.subop, sew_shift, lasx, res, res,
                                       a_vec);
                    } else {
                        (lasx ? la_xvbitsel_v : la_vbitsel_v)(
                            a_vec, scratch, a_vec, mask);
                        if (m.subop == PVIR_FP_DIV) {
                            load_fp_one(scratch, sew_shift, lasx);
                        }
                        (lasx ? la_xvbitsel_v : la_vbitsel_v)(
                            res, scratch, b_vec, mask);
                        emit_fp_alu_op(m.subop, sew_shift, lasx, res, a_vec,
                                       res);
                    }
                }
            }

            if (m.family != DIRECT_KIND_MERGE) {
                (lasx ? latr_env_xvld : latr_env_vld)(scratch, fill_off);
                (lasx ? la_xvbitsel_v : la_vbitsel_v)(res, scratch, res, mask);
            }
            (lasx ? latr_env_xvst : latr_env_vst)(res, out_off);
            byte += plan.steps[idx].size;
        }

        if (span_bytes > LATR_RVV_VLENB) {
            byte = 0;
            for (idx = 0; idx < plan.chunk_count; idx++) {
                bool lasx = plan.steps[idx].size == 32u;
                (lasx ? latr_env_xvld : latr_env_vld)(
                    res, env_offset_v_staging((int)byte));
                (lasx ? latr_env_xvst : latr_env_vst)(
                    res, env_offset_vreg((int)vd) + (int)byte);
                byte += plan.steps[idx].size;
            }
        }

        if (is_fp_arith) {
            emit_vec_fp_end();
        }
        ra_free_temp(scratch);
        ra_free_temp(mask);
        ra_free_temp(res);
        ra_free_temp(b_vec);
        ra_free_temp(a_vec);
    }

    la_b(*done_label);
    return true;
}
