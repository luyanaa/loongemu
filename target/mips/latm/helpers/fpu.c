/*
 * LATM MIPS64r2 FPU dispatch.
 *
 * The arithmetic bodies are the QEMU MIPS helpers in qemu_fpu_helper.c
 * (LGPL-2.1-or-later); this file only maps decoded MIPS fields onto them and
 * performs MIPS's 32-bit FPR read/write merging.
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "internal.h"
#include "fpu_helper.h"
#include "exec/helper-proto.h"
#include "exec/cpu_ldst.h"
#include "latm-helpers.h"
#include "latm.h"

#define FPR64(i) (env->active_fpu.fpr[(i)].d)
#define FPR32(i) ((uint32_t)env->active_fpu.fpr[(i)].d)
#define FPR32H(i) ((uint32_t)(env->active_fpu.fpr[(i)].d >> 32))

static void set_fpr32(CPUMIPSState *env, int i, uint32_t v)
{
    env->active_fpu.fpr[i].d =
        (env->active_fpu.fpr[i].d & UINT64_C(0xffffffff00000000)) | v;
}

static void set_fpr32h(CPUMIPSState *env, int i, uint32_t v)
{
    env->active_fpu.fpr[i].d =
        (env->active_fpu.fpr[i].d & UINT64_C(0x00000000ffffffff)) |
        ((uint64_t)v << 32);
}

static int fcc_get(CPUMIPSState *env, int cc)
{
    return (env->active_fpu.fcr31 >> latm_fcc_bit(cc)) & 1;
}

void latm_fpu_arith(CPUMIPSState *env, uint32_t fmt, uint32_t funct,
                    uint32_t fd, uint32_t fs, uint32_t ft)
{
    uint32_t r32;
    uint64_t r64;

    if (fmt == 16) {
        switch (funct) {
        case 0: r32 = helper_float_add_s(env, FPR32(fs), FPR32(ft)); break;
        case 1: r32 = helper_float_sub_s(env, FPR32(fs), FPR32(ft)); break;
        case 2: r32 = helper_float_mul_s(env, FPR32(fs), FPR32(ft)); break;
        case 3: r32 = helper_float_div_s(env, FPR32(fs), FPR32(ft)); break;
        case 4: r32 = helper_float_sqrt_s(env, FPR32(fs)); break;
        case 5:
            if (env->active_fpu.fcr31 & (1u << FCR31_ABS2008)) {
                r32 = FPR32(fs) & 0x7fffffffu;
            } else {
                r32 = helper_float_abs_s(FPR32(fs));
            }
            break;
        case 6: r32 = FPR32(fs); break;
        case 7:
            if (env->active_fpu.fcr31 & (1u << FCR31_ABS2008)) {
                r32 = FPR32(fs) ^ 0x80000000u;
            } else {
                r32 = helper_float_chs_s(FPR32(fs));
            }
            break;
        case 8: r64 = helper_float_round_l_s(env, FPR32(fs)); set_fpr32(env, fd, (uint32_t)r64); set_fpr32h(env, fd, (uint32_t)(r64 >> 32)); return;
        case 9: r64 = helper_float_trunc_l_s(env, FPR32(fs)); set_fpr32(env, fd, (uint32_t)r64); set_fpr32h(env, fd, (uint32_t)(r64 >> 32)); return;
        case 10: r64 = helper_float_ceil_l_s(env, FPR32(fs)); set_fpr32(env, fd, (uint32_t)r64); set_fpr32h(env, fd, (uint32_t)(r64 >> 32)); return;
        case 11: r64 = helper_float_floor_l_s(env, FPR32(fs)); set_fpr32(env, fd, (uint32_t)r64); set_fpr32h(env, fd, (uint32_t)(r64 >> 32)); return;
        case 12: r32 = helper_float_round_w_s(env, FPR32(fs)); break;
        case 13: r32 = helper_float_trunc_w_s(env, FPR32(fs)); break;
        case 14: r32 = helper_float_ceil_w_s(env, FPR32(fs)); break;
        case 15: r32 = helper_float_floor_w_s(env, FPR32(fs)); break;
        case 21: r32 = helper_float_recip_s(env, FPR32(fs)); break;
        case 22: r32 = helper_float_rsqrt_s(env, FPR32(fs)); break;
        case 28: r32 = helper_float_recip2_s(env, FPR32(fs), FPR32(ft)); break;
        case 29: r32 = helper_float_recip1_s(env, FPR32(fs)); break;
        case 30: r32 = helper_float_rsqrt1_s(env, FPR32(fs)); break;
        case 31: r32 = helper_float_rsqrt2_s(env, FPR32(fs), FPR32(ft)); break;
        default: return;
        }
        set_fpr32(env, fd, r32);
    } else if (fmt == 17) {
        switch (funct) {
        case 0: r64 = helper_float_add_d(env, FPR64(fs), FPR64(ft)); break;
        case 1: r64 = helper_float_sub_d(env, FPR64(fs), FPR64(ft)); break;
        case 2: r64 = helper_float_mul_d(env, FPR64(fs), FPR64(ft)); break;
        case 3: r64 = helper_float_div_d(env, FPR64(fs), FPR64(ft)); break;
        case 4: r64 = helper_float_sqrt_d(env, FPR64(fs)); break;
        case 5:
            if (env->active_fpu.fcr31 & (1u << FCR31_ABS2008)) {
                r64 = FPR64(fs) & UINT64_C(0x7fffffffffffffff);
            } else {
                r64 = helper_float_abs_d(FPR64(fs));
            }
            break;
        case 6: r64 = FPR64(fs); break;
        case 7:
            if (env->active_fpu.fcr31 & (1u << FCR31_ABS2008)) {
                r64 = FPR64(fs) ^ UINT64_C(0x8000000000000000);
            } else {
                r64 = helper_float_chs_d(FPR64(fs));
            }
            break;
        case 8: r64 = helper_float_round_l_d(env, FPR64(fs)); break;
        case 9: r64 = helper_float_trunc_l_d(env, FPR64(fs)); break;
        case 10: r64 = helper_float_ceil_l_d(env, FPR64(fs)); break;
        case 11: r64 = helper_float_floor_l_d(env, FPR64(fs)); break;
        case 12: r32 = helper_float_round_w_d(env, FPR64(fs)); set_fpr32(env, fd, r32); return;
        case 13: r32 = helper_float_trunc_w_d(env, FPR64(fs)); set_fpr32(env, fd, r32); return;
        case 14: r32 = helper_float_ceil_w_d(env, FPR64(fs)); set_fpr32(env, fd, r32); return;
        case 15: r32 = helper_float_floor_w_d(env, FPR64(fs)); set_fpr32(env, fd, r32); return;
        case 21: r64 = helper_float_recip_d(env, FPR64(fs)); break;
        case 22: r64 = helper_float_rsqrt_d(env, FPR64(fs)); break;
        case 28: r64 = helper_float_recip2_d(env, FPR64(fs), FPR64(ft)); break;
        case 29: r64 = helper_float_recip1_d(env, FPR64(fs)); break;
        case 30: r64 = helper_float_rsqrt1_d(env, FPR64(fs)); break;
        case 31: r64 = helper_float_rsqrt2_d(env, FPR64(fs), FPR64(ft)); break;
        default: return;
        }
        set_fpr32(env, fd, (uint32_t)r64);
        set_fpr32h(env, fd, (uint32_t)(r64 >> 32));
    }
}

void latm_fpu_cvt(CPUMIPSState *env, uint32_t fmt, uint32_t funct,
                  uint32_t fd, uint32_t fs)
{
    uint32_t r32;
    uint64_t r64;

    if (fmt == 16) { /* source single */
        switch (funct) {
        case 33: r64 = helper_float_cvtd_s(env, FPR32(fs)); set_fpr32(env, fd, (uint32_t)r64); set_fpr32h(env, fd, (uint32_t)(r64 >> 32)); return;
        case 36: r32 = helper_float_cvt_w_s(env, FPR32(fs)); break;
        case 37: r64 = helper_float_cvt_l_s(env, FPR32(fs)); set_fpr32(env, fd, (uint32_t)r64); set_fpr32h(env, fd, (uint32_t)(r64 >> 32)); return;
        default: return;
        }
        set_fpr32(env, fd, r32);
    } else if (fmt == 17) { /* source double */
        switch (funct) {
        case 32: r32 = helper_float_cvts_d(env, FPR64(fs)); break;
        case 36: r32 = helper_float_cvt_w_d(env, FPR64(fs)); break;
        case 37: r64 = helper_float_cvt_l_d(env, FPR64(fs)); set_fpr32(env, fd, (uint32_t)r64); set_fpr32h(env, fd, (uint32_t)(r64 >> 32)); return;
        default: return;
        }
        set_fpr32(env, fd, r32);
    } else if (fmt == 20) { /* source word */
        switch (funct) {
        case 32: r32 = helper_float_cvts_w(env, FPR32(fs)); set_fpr32(env, fd, r32); return;
        case 33: r64 = helper_float_cvtd_w(env, FPR32(fs)); set_fpr32(env, fd, (uint32_t)r64); set_fpr32h(env, fd, (uint32_t)(r64 >> 32)); return;
        default: return;
        }
    } else if (fmt == 21) { /* source long */
        switch (funct) {
        case 32: r32 = helper_float_cvts_l(env, FPR64(fs)); set_fpr32(env, fd, r32); return;
        case 33: r64 = helper_float_cvtd_l(env, FPR64(fs)); set_fpr32(env, fd, (uint32_t)r64); set_fpr32h(env, fd, (uint32_t)(r64 >> 32)); return;
        default: return;
        }
    }
}

typedef void (*cmp_s_fn)(CPUMIPSState *, uint32_t, uint32_t, int);
typedef void (*cmp_d_fn)(CPUMIPSState *, uint64_t, uint64_t, int);

void latm_fpu_cmp(CPUMIPSState *env, uint32_t fmt, uint32_t cond,
                  uint32_t fs, uint32_t ft, uint32_t cc)
{
    static const cmp_s_fn sfn[16] = {
        helper_cmp_s_f, helper_cmp_s_un, helper_cmp_s_eq, helper_cmp_s_ueq,
        helper_cmp_s_olt, helper_cmp_s_ult, helper_cmp_s_ole, helper_cmp_s_ule,
        helper_cmp_s_sf, helper_cmp_s_ngle, helper_cmp_s_seq, helper_cmp_s_ngl,
        helper_cmp_s_lt, helper_cmp_s_nge, helper_cmp_s_le, helper_cmp_s_ngt,
    };
    static const cmp_d_fn dfn[16] = {
        helper_cmp_d_f, helper_cmp_d_un, helper_cmp_d_eq, helper_cmp_d_ueq,
        helper_cmp_d_olt, helper_cmp_d_ult, helper_cmp_d_ole, helper_cmp_d_ule,
        helper_cmp_d_sf, helper_cmp_d_ngle, helper_cmp_d_seq, helper_cmp_d_ngl,
        helper_cmp_d_lt, helper_cmp_d_nge, helper_cmp_d_le, helper_cmp_d_ngt,
    };

    if (cond >= 16) {
        return;
    }
    if (fmt == 16) {
        sfn[cond](env, FPR32(fs), FPR32(ft), cc);
    } else if (fmt == 17) {
        dfn[cond](env, FPR64(fs), FPR64(ft), cc);
    }
}

void latm_fpu_movc(CPUMIPSState *env, uint32_t fmt, uint32_t fd, uint32_t fs,
                   uint32_t cc, uint32_t tf)
{
    if (fcc_get(env, cc) != (int)tf) {
        return;
    }
    if (fmt == 16) {
        set_fpr32(env, fd, FPR32(fs));
    } else if (fmt == 17) {
        set_fpr32(env, fd, (uint32_t)FPR64(fs));
        set_fpr32h(env, fd, (uint32_t)(FPR64(fs) >> 32));
    }
}

void latm_fpu_move(CPUMIPSState *env, uint32_t fmt, uint32_t op,
                   uint32_t fd, uint32_t fs, uint32_t rt)
{
    uint64_t g = env->active_tc.gpr[rt];
    bool take = (op == 2) ? (g == 0) : (g != 0); /* MOVZ=2, MOVN=3 */

    if (!take) {
        return;
    }
    if (fmt == 16) {
        set_fpr32(env, fd, FPR32(fs));
    } else if (fmt == 17) {
        set_fpr32(env, fd, (uint32_t)FPR64(fs));
        set_fpr32h(env, fd, (uint32_t)(FPR64(fs) >> 32));
    }
}

void latm_fpu_madd(CPUMIPSState *env, uint32_t fmt, uint32_t op,
                   uint32_t fd, uint32_t fs, uint32_t ft, uint32_t fr)
{
    /* The freestanding softfloat path miscompiles float64_mul; use the host
     * scalar unit for the MADD family.  This matches QEMU's two-step
     * multiply/add semantics and keeps the COP1X path usable until the
     * softfloat build issue is fixed. */
    if (fmt == 16) {
        union { uint32_t u; float f; } a = { FPR32(fs) }, b = { FPR32(ft) },
                                      c = { FPR32(fr) }, r;
        switch (op) {
        case 0: r.f = a.f * b.f + c.f; break;
        case 1: r.f = a.f * b.f - c.f; break;
        case 2: r.f = -(a.f * b.f + c.f); break;
        case 3: r.f = -(a.f * b.f - c.f); break;
        default: return;
        }
        set_fpr32(env, fd, r.u);
    } else if (fmt == 17) {
        union { uint64_t u; double d; } a = { FPR64(fs) }, b = { FPR64(ft) },
                                      c = { FPR64(fr) }, r;
        switch (op) {
        case 0: r.d = a.d * b.d + c.d; break;
        case 1: r.d = a.d * b.d - c.d; break;
        case 2: r.d = -(a.d * b.d + c.d); break;
        case 3: r.d = -(a.d * b.d - c.d); break;
        default: return;
        }
        set_fpr32(env, fd, (uint32_t)r.u);
        set_fpr32h(env, fd, (uint32_t)(r.u >> 32));
    }
}

void latm_fpu_gpr(CPUMIPSState *env, uint32_t op, uint32_t rt, uint32_t fs)
{
    uint64_t v;

    switch (op) {
    case 0: /* MFC1 */
        env->active_tc.gpr[rt] = (int64_t)(int32_t)FPR32(fs);
        break;
    case 1: /* DMFC1 */
        env->active_tc.gpr[rt] = FPR64(fs);
        break;
    case 2: /* CFC1 */
        env->active_tc.gpr[rt] = helper_cfc1(env, fs);
        break;
    case 3: /* MFHC1 */
        env->active_tc.gpr[rt] = (int64_t)(int32_t)FPR32H(fs);
        break;
    case 4: /* MTC1 */
        set_fpr32(env, fs, (uint32_t)env->active_tc.gpr[rt]);
        break;
    case 5: /* DMTC1 */
        env->active_fpu.fpr[fs].d = env->active_tc.gpr[rt];
        break;
    case 6: /* CTC1 */
        helper_ctc1(env, env->active_tc.gpr[rt], fs, rt);
        break;
    case 7: /* MTHC1 */
        set_fpr32h(env, fs, (uint32_t)env->active_tc.gpr[rt]);
        break;
    }
}

void latm_fpu_ldst(CPUMIPSState *env, uint32_t op, uint32_t ft,
                   uint32_t fs, uint64_t addr)
{
    uint32_t v32;
    uint64_t v64;


    if (latm_system_memory) {
        switch (op) {
        case 0: /* LWC1 */
        case 4: /* LWXC1 */
            v32 = (uint32_t)latm_helper_load(env, MIPS_MEM_LW, addr);
            set_fpr32(env, ft, v32);
            return;
        case 1: /* LDC1 */
        case 5: /* LDXC1 */
        case 6: /* LUXC1 */
            v64 = latm_helper_load(env, MIPS_MEM_LD,
                                   op == 6 ? addr & ~UINT64_C(7) : addr);
            env->active_fpu.fpr[ft].d = v64;
            return;
        case 2: /* SWC1 */
        case 7: /* SWXC1 */
            latm_helper_store(env, MIPS_MEM_SW, addr, FPR32(fs));
            return;
        case 3: /* SDC1 */
        case 8: /* SDXC1 */
        case 9: /* SUXC1 */
            latm_helper_store(env, MIPS_MEM_SD,
                              op == 9 ? addr & ~UINT64_C(7) : addr,
                              FPR64(fs));
            return;
        default:
            return;
        }
    }
    switch (op) {
    case 0: /* LWC1 */
        v32 = cpu_ldl_mmuidx_ra(env, addr, 0, 0);
        set_fpr32(env, ft, v32);
        break;
    case 1: /* LDC1 */
        v64 = cpu_ldq_mmuidx_ra(env, addr, 0, 0);
        env->active_fpu.fpr[ft].d = v64;
        break;
    case 2: /* SWC1 */
        cpu_stl_mmuidx_ra(env, addr, FPR32(fs), 0, 0);
        break;
    case 3: /* SDC1 */
        cpu_stq_mmuidx_ra(env, addr, FPR64(fs), 0, 0);
        break;
    case 4: /* LWXC1 */
        v32 = cpu_ldl_mmuidx_ra(env, addr, 0, 0);
        set_fpr32(env, ft, v32);
        break;
    case 5: /* LDXC1 */
        v64 = cpu_ldq_mmuidx_ra(env, addr, 0, 0);
        env->active_fpu.fpr[ft].d = v64;
        break;
    case 6: /* LUXC1 */
        v64 = cpu_ldq_mmuidx_ra(env, addr & ~UINT64_C(7), 0, 0);
        env->active_fpu.fpr[ft].d = v64;
        break;
    case 7: /* SWXC1 */
        cpu_stl_mmuidx_ra(env, addr, FPR32(fs), 0, 0);
        break;
    case 8: /* SDXC1 */
        cpu_stq_mmuidx_ra(env, addr, FPR64(fs), 0, 0);
        break;
    case 9: /* SUXC1 */
        cpu_stq_mmuidx_ra(env, addr & ~UINT64_C(7), FPR64(fs), 0, 0);
        break;
    }
}
