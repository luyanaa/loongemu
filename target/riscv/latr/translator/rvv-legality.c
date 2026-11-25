/*
 * RVV 1.0 operand legality check.
 *
 * Each rule below is a transcription of the corresponding QEMU helper so that
 * "illegal instruction" agrees with the golden model bit for bit. The QEMU
 * rule is cited next to the LATR implementation of it; see
 * target/riscv/insn_trans/trans_rvv.c.inc in QEMU-LVZ.
 */
#include "latr-rvv-legality.h"

/* Which operand-shape rule an instruction follows. */
typedef enum RvvForm {
    FORM_NONE = 0,     /* no operand rule beyond vext_check_isa_ill */
    FORM_SS,           /* vext_check_ss:   vd, vs2                 */
    FORM_SSS,          /* vext_check_sss:  vd, vs1, vs2            */
    FORM_MS,           /* vext_check_ms:   mask vd, vs2            */
    FORM_MSS,          /* vext_check_mss:  mask vd, vs1, vs2       */
    FORM_DS,           /* vext_check_ds:   2*SEW vd, SEW vs2       */
    FORM_DSS,          /* vext_check_dss:  2*SEW vd, SEW vs1, vs2  */
    FORM_SD,           /* vext_check_sd:   SEW vd, 2*SEW vs2       */
    FORM_SDS,          /* vext_check_sds:  SEW vd, 2*SEW vs2, vs1  */
    FORM_REDUCTION,    /* vext_check_reduction                     */
    FORM_REDUCTION_W,  /* vext_check_reduction + 2*SEW <= ELEN     */
    FORM_SLIDE_UP,     /* vext_check_slide(is_over = true)         */
    FORM_SLIDE_DOWN,   /* vext_check_slide(is_over = false)        */
    FORM_GATHER_VV,    /* vrgather_vv_check                        */
    FORM_GATHER_VX,    /* vrgather_vx_check                        */
    FORM_GATHER_EI16,  /* vrgatherei16_vv_check                    */
    FORM_COMPRESS,     /* vcompress_vm_check                       */
    FORM_EXT,          /* int_ext_check (vzext/vsext)              */
    FORM_LOAD,         /* vext_check_load                          */
    FORM_STORE,        /* vext_check_store                         */
    FORM_LOAD_INDEX,   /* vext_check_ld_index                      */
    FORM_STORE_INDEX,  /* vext_check_st_index                      */
    FORM_ALIGN_ONLY,   /* require_align(vd): vmv.v.x/i, vfmv.v.f   */
    FORM_ALIGN_PAIR,   /* align(vd) && align(vs1): vmv.v.v        */
    FORM_SCAN_SET,     /* vmsbf/vmsif/vmsof                        */
    FORM_VIOTA,        /* viota.m                                  */
    FORM_VID,          /* vid.v                                    */
    FORM_WHOLE_MOVE,   /* vmv<n>r.v: align(vd, nf) && align(vs2, nf)*/
    FORM_WHOLE_MEM,    /* vl<n>re.v / vs<n>r.v: align(vd, nf)      */
    FORM_WIDE_SRC_W,   /* vw*.wv/.wx: 2*SEW vd and vs1, SEW vs2   */
} RvvForm;

typedef struct RvvShape {
    RvvForm form;
    uint8_t eew; /* log2 of the element width for memory/gather forms */
    uint8_t div; /* vzext/vsext width divisor: 1, 2 or 3 */
} RvvShape;

/* ---- vtype view --------------------------------------------------------- */

static int vtype_sew_log(uint64_t vtype)
{
    return (int)((vtype >> 3) & 7u);
}

/* LMUL in log2 units: 0..3 for 1,2,4,8 and -3..-1 for 1/8,1/4,1/2. */
static int vtype_lmul(uint64_t vtype)
{
    unsigned vlmul = (unsigned)(vtype & 7u);

    return (vlmul & 4u) ? (int)vlmul - 8 : (int)vlmul;
}

/* ---- group rules -------------------------------------------------------- */

/* QEMU require_align(): fractional LMUL accepts any register. */
static bool align_ok(int reg, int lmul)
{
    if (lmul <= 0) {
        return true;
    }
    return ((unsigned)reg & ((1u << lmul) - 1u)) == 0;
}

/* QEMU require_nf(): the whole segment must stay inside v0..v31. */
static bool nf_ok(int reg, int nf, int lmul)
{
    int size = nf << (lmul > 0 ? lmul : 0);

    return size <= 8 && reg + size <= 32;
}

static bool overlaps(int astart, int asize, int bstart, int bsize)
{
    return astart < bstart + bsize && bstart < astart + asize;
}

static int group_size(int lmul)
{
    return lmul <= 0 ? 1 : 1 << lmul;
}

/*
 * QEMU require_noover(): a destination may overlap a source only when the
 * destination is wider, the source group is at least one register, and the
 * overlap sits in the high part of the destination.
 */
static bool noover_ok(int dst, int dst_lmul, int src, int src_lmul)
{
    int dst_size = group_size(dst_lmul);
    int src_size = group_size(src_lmul);

    if (dst_size > src_size && dst < src && src_lmul >= 0 &&
        overlaps(dst, dst_size, src, src_size) &&
        !overlaps(dst, dst_size, src + src_size, src_size)) {
        return true;
    }
    return !overlaps(dst, dst_size, src, src_size);
}

/* QEMU require_vm(): a masked destination may not clobber the mask in v0. */
static bool vm_ok(int vm, int vd)
{
    return vm != 0 || vd != 0;
}

/* ---- instruction -> operand shape --------------------------------------- */

#define V(tag) case LATR_RV_##tag:

static void rvv_shape(unsigned opcode, RvvShape *shape)
{
    shape->form = FORM_NONE;
    shape->eew = 0;
    shape->div = 0;

    switch (opcode) {
    /* Single-width integer ALU: SEW = SEW op SEW. */
    V(VADD_VV) V(VSUB_VV) V(VAND_VV) V(VOR_VV) V(VXOR_VV)
    V(VSLL_VV) V(VSRL_VV) V(VSRA_VV) V(VMUL_VV)
    V(VDIVU_VV) V(VDIV_VV) V(VREMU_VV) V(VREM_VV)
    V(VMINU_VV) V(VMIN_VV) V(VMAXU_VV) V(VMAX_VV)
    V(VMULH_VV) V(VMULHU_VV) V(VMULHSU_VV) V(VSMUL_VV)
    V(VSADD_VV) V(VSADDU_VV) V(VSSUB_VV) V(VSSUBU_VV)
    V(VAADD_VV) V(VAADDU_VV) V(VASUB_VV) V(VASUBU_VV)
    V(VSSRL_VV) V(VSSRA_VV)
        shape->form = FORM_SSS;
        break;
    V(VADD_VX) V(VSUB_VX) V(VAND_VX) V(VOR_VX) V(VXOR_VX)
    V(VSLL_VX) V(VSRL_VX) V(VSRA_VX) V(VMUL_VX)
    V(VDIVU_VX) V(VDIV_VX) V(VREMU_VX) V(VREM_VX)
    V(VMINU_VX) V(VMIN_VX) V(VMAXU_VX) V(VMAX_VX)
    V(VMULH_VX) V(VMULHU_VX) V(VMULHSU_VX) V(VRSUB_VX) V(VSMUL_VX)
    V(VSADD_VX) V(VSADDU_VX) V(VSSUB_VX) V(VSSUBU_VX)
    V(VAADD_VX) V(VAADDU_VX) V(VASUB_VX) V(VASUBU_VX)
    V(VSSRL_VX) V(VSSRA_VX)
    V(VADD_VI) V(VRSUB_VI) V(VAND_VI) V(VOR_VI) V(VXOR_VI)
    V(VSLL_VI) V(VSRL_VI) V(VSRA_VI)
    V(VSADD_VI) V(VSADDU_VI) V(VSSRL_VI) V(VSSRA_VI)
        shape->form = FORM_SS;
        break;

    /* Integer compares write a mask register; the three-operand form has to
     * keep the destination clear of both sources. */
    V(VMSEQ_VV) V(VMSNE_VV) V(VMSLT_VV) V(VMSLTU_VV)
    V(VMSLE_VV) V(VMSLEU_VV)
        shape->form = FORM_MSS;
        break;
    V(VMSEQ_VX) V(VMSNE_VX) V(VMSLT_VX) V(VMSLTU_VX)
    V(VMSLE_VX) V(VMSLEU_VX) V(VMSGTU_VX) V(VMSGT_VX)
    V(VMSEQ_VI) V(VMSNE_VI) V(VMSLT_VI) V(VMSLTU_VI)
    V(VMSLE_VI) V(VMSLEU_VI) V(VMSGTU_VI) V(VMSGT_VI)
        shape->form = FORM_MS;
        break;

    /* Fused multiply-add: three SEW operands (.vv) or two plus a scalar (.vf). */
    V(VFMACC_VV) V(VFNMACC_VV) V(VFMSAC_VV) V(VFNMSC_VV)
    V(VFMADD_VV) V(VFNMADD_VV) V(VFMSUB_VV) V(VFNMSSUB_VV)
        shape->form = FORM_SSS;
        break;
    V(VFMACC_VF) V(VFNMACC_VF) V(VFMSAC_VF) V(VFNMSC_VF)
    V(VFMADD_VF) V(VFNMADD_VF) V(VFMSUB_VF) V(VFNMSSUB_VF)
        shape->form = FORM_SS;
        break;
    /* Widening FMA: 2*SEW destination, SEW sources. */
    V(VFWMACC_VV) V(VFWNMACC_VV) V(VFWMSAC_VV) V(VFWNMSAC_VV)
        shape->form = FORM_DSS;
        break;
    V(VFWMACC_VF) V(VFWNMACC_VF) V(VFWMSAC_VF) V(VFWNMSAC_VF)
        shape->form = FORM_DS;
        break;

    /* Merge and move. */
    V(VMERGE_VVM)
        shape->form = FORM_SSS;
        break;
    V(VMERGE_VXM) V(VMERGE_VIM)
        shape->form = FORM_SS;
        break;
    V(VMV_V_V)
        shape->form = FORM_ALIGN_PAIR;
        break;
    V(VMV_V_X) V(VMV_V_I) V(VFMV_V_F)
        shape->form = FORM_ALIGN_ONLY;
        break;
    V(VMV_X_S) V(VMV_S_X) V(VFMV_F_S) V(VFMV_S_F)
        shape->form = FORM_NONE;
        break;

    /* Widening integer: 2*SEW = SEW op SEW, or 2*SEW += SEW * SEW. */
    V(VWADD_VV) V(VWADDU_VV) V(VWSUB_VV) V(VWSUBU_VV)
    V(VWMUL_VV) V(VWMULU_VV) V(VWMULSU_VV)
    V(VWMACC_VV) V(VWMACCU_VV) V(VWMACCSU_VV)
        shape->form = FORM_DSS;
        break;
    V(VWADD_VX) V(VWADDU_VX) V(VWSUB_VX) V(VWSUBU_VX)
    V(VWMUL_VX) V(VWMULU_VX) V(VWMULSU_VX)
    V(VWMACC_VX) V(VWMACCU_VX) V(VWMACCSU_VX) V(VWMACCUS_VX)
        shape->form = FORM_DS;
        break;

    /* Widening with a 2*SEW vector second operand: vd and vs1 both double. */
    V(VWADD_WV) V(VWADDU_WV) V(VWSUB_WV) V(VWSUBU_WV)
        shape->form = FORM_WIDE_SRC_W;
        break;
    /* The .wx forms take a scalar in rs1, so only vd is double-width; checking
     * rs1 as a vector register is what rejected vwadd.wx, t0 (odd register). */
    V(VWADD_WX) V(VWADDU_WX) V(VWSUB_WX) V(VWSUBU_WX)
        shape->form = FORM_DS;
        break;

    /* Narrowing integer: SEW = 2*SEW op SEW. */
    V(VNSRL_WV) V(VNSRA_WV) V(VNCLIP_WV) V(VNCLIPU_WV)
        shape->form = FORM_SDS;
        break;
    V(VNSRL_WX) V(VNSRL_WI) V(VNSRA_WX) V(VNSRA_WI)
    V(VNCLIP_WX) V(VNCLIP_WI) V(VNCLIPU_WX) V(VNCLIPU_WI)
        shape->form = FORM_SD;
        break;

    /* Integer extension. */
    V(VZEXT_VF2) V(VSEXT_VF2)
        shape->form = FORM_EXT;
        shape->div = 1;
        break;
    V(VZEXT_VF4) V(VSEXT_VF4)
        shape->form = FORM_EXT;
        shape->div = 2;
        break;
    V(VZEXT_VF8) V(VSEXT_VF8)
        shape->form = FORM_EXT;
        shape->div = 3;
        break;

    /* Single-width FP arithmetic and conversion. */
    V(VFADD_VV) V(VFSUB_VV) V(VFMUL_VV) V(VFDIV_VV)
    V(VFMIN_VV) V(VFMAX_VV)
    V(VFSGNJ_VV) V(VFSGNJN_VV) V(VFSGNJX_VV)
        shape->form = FORM_SSS;
        break;
    V(VFADD_VF) V(VFSUB_VF) V(VFMUL_VF) V(VFDIV_VF)
    V(VFMIN_VF) V(VFMAX_VF)
    V(VFSGNJ_VF) V(VFSGNJN_VF) V(VFSGNJX_VF)
    V(VFMERGE_VFM)
    V(VFSQRT_V) V(VFCLASS_V)
        shape->form = FORM_SS;
        break;
    V(VFCVT_XU_F_V) V(VFCVT_X_F_V) V(VFCVT_F_XU_V) V(VFCVT_F_X_V)
    V(VFCVT_RTZ_XU_F_V) V(VFCVT_RTZ_X_F_V)
        shape->form = FORM_SS;
        break;

    /* FP compare: like the integer compares, no rounding involved. */
    V(VMFEQ_VV) V(VMFNE_VV) V(VMFLT_VV) V(VMFLE_VV)
        shape->form = FORM_MSS;
        break;
    V(VMFEQ_VF) V(VMFNE_VF) V(VMFLT_VF) V(VMFLE_VF)
    V(VMFGT_VF) V(VMFGE_VF)
        shape->form = FORM_MS;
        break;

    /* Widening FP. */
    V(VFWADD_VV) V(VFWSUB_VV) V(VFWMUL_VV) V(VFWADD_WV) V(VFWSUB_WV)
        shape->form = FORM_DSS;
        break;
    V(VFWADD_VF) V(VFWSUB_VF) V(VFWMUL_VF) V(VFWADD_WF) V(VFWSUB_WF)
        shape->form = FORM_DS;
        break;
    V(VFWCVT_XU_F_V) V(VFWCVT_X_F_V) V(VFWCVT_F_XU_V) V(VFWCVT_F_X_V)
    V(VFWCVT_F_F_V) V(VFWCVT_RTZ_XU_F_V) V(VFWCVT_RTZ_X_F_V)
        shape->form = FORM_DS;
        break;

    /* Narrowing FP. */
    V(VFNCVT_XU_F_W) V(VFNCVT_X_F_W) V(VFNCVT_F_XU_W) V(VFNCVT_F_X_W)
    V(VFNCVT_F_F_W) V(VFNCVT_ROD_F_F_W)
    V(VFNCVT_RTZ_XU_F_W) V(VFNCVT_RTZ_X_F_W)
        shape->form = FORM_SD;
        break;

    /* Reductions. */
    V(VREDSUM_VS) V(VREDAND_VS) V(VREDOR_VS) V(VREDXOR_VS)
    V(VREDMINU_VS) V(VREDMIN_VS) V(VREDMAXU_VS) V(VREDMAX_VS)
    V(VFREDUSUM_VS) V(VFREDOSUM_VS) V(VFREDMIN_VS) V(VFREDMAX_VS)
        shape->form = FORM_REDUCTION;
        break;
    V(VWREDSUM_VS) V(VWREDSUMU_VS) V(VFWREDUSUM_VS) V(VFWREDOSUM_VS)
        shape->form = FORM_REDUCTION_W;
        break;

    /* Permute. */
    V(VSLIDEUP_VX) V(VSLIDEUP_VI) V(VSLIDE1UP_VX)
        shape->form = FORM_SLIDE_UP;
        break;
    V(VSLIDEDOWN_VX) V(VSLIDEDOWN_VI) V(VSLIDE1DOWN_VX)
        shape->form = FORM_SLIDE_DOWN;
        break;
    V(VRGATHER_VV)
        shape->form = FORM_GATHER_VV;
        break;
    V(VRGATHER_VX) V(VRGATHER_VI)
        shape->form = FORM_GATHER_VX;
        break;
    V(VRGATHEREI16_VV)
        shape->form = FORM_GATHER_EI16;
        break;
    V(VCOMPRESS_VM)
        shape->form = FORM_COMPRESS;
        break;

    /* Unit-stride and strided memory share one rule set; the element width
     * comes from the opcode, the register group from EMUL. */
    V(VLE8_V)   shape->form = FORM_LOAD;  shape->eew = 0; break;
    V(VLE16_V)  shape->form = FORM_LOAD;  shape->eew = 1; break;
    V(VLE32_V)  shape->form = FORM_LOAD;  shape->eew = 2; break;
    V(VLE64_V)  shape->form = FORM_LOAD;  shape->eew = 3; break;
    V(VSE8_V)   shape->form = FORM_STORE; shape->eew = 0; break;
    V(VSE16_V)  shape->form = FORM_STORE; shape->eew = 1; break;
    V(VSE32_V)  shape->form = FORM_STORE; shape->eew = 2; break;
    V(VSE64_V)  shape->form = FORM_STORE; shape->eew = 3; break;
    V(VLSE8_V)  shape->form = FORM_LOAD;  shape->eew = 0; break;
    V(VLSE16_V) shape->form = FORM_LOAD;  shape->eew = 1; break;
    V(VLSE32_V) shape->form = FORM_LOAD;  shape->eew = 2; break;
    V(VLSE64_V) shape->form = FORM_LOAD;  shape->eew = 3; break;
    V(VSSE8_V)  shape->form = FORM_STORE; shape->eew = 0; break;
    V(VSSE16_V) shape->form = FORM_STORE; shape->eew = 1; break;
    V(VSSE32_V) shape->form = FORM_STORE; shape->eew = 2; break;
    V(VSSE64_V) shape->form = FORM_STORE; shape->eew = 3; break;
    V(VLXEI8_V)  shape->form = FORM_LOAD_INDEX;  shape->eew = 0; break;
    V(VLXEI16_V) shape->form = FORM_LOAD_INDEX;  shape->eew = 1; break;
    V(VLXEI32_V) shape->form = FORM_LOAD_INDEX;  shape->eew = 2; break;
    V(VLXEI64_V) shape->form = FORM_LOAD_INDEX;  shape->eew = 3; break;
    V(VSXEI8_V)  shape->form = FORM_STORE_INDEX; shape->eew = 0; break;
    V(VSXEI16_V) shape->form = FORM_STORE_INDEX; shape->eew = 1; break;
    V(VSXEI32_V) shape->form = FORM_STORE_INDEX; shape->eew = 2; break;
    V(VSXEI64_V) shape->form = FORM_STORE_INDEX; shape->eew = 3; break;

    /* Fault-only-first loads follow exactly the unit-stride load rules; QEMU
     * reachability is the only difference (a fault truncates instead of
     * trapping). */
    V(VLE8FF_V)  shape->form = FORM_LOAD; shape->eew = 0; break;
    V(VLE16FF_V) shape->form = FORM_LOAD; shape->eew = 1; break;
    V(VLE32FF_V) shape->form = FORM_LOAD; shape->eew = 2; break;
    V(VLE64FF_V) shape->form = FORM_LOAD; shape->eew = 3; break;

    /* Mask register load/store addresses one register; only vill applies. */
    V(VLM_V) V(VSM_V)
        shape->form = FORM_NONE;
        break;

    /* Whole-register transfers. `nf` comes from the opcode (see
     * latr_rvv_whole_nf); vtype plays no part in their operand check. */
    V(VMV1R_V) V(VMV2R_V) V(VMV4R_V) V(VMV8R_V)
        shape->form = FORM_WHOLE_MOVE;
        break;
    V(VL1RE8_V) V(VL1RE16_V) V(VL1RE32_V) V(VL1RE64_V)
    V(VL2RE8_V) V(VL2RE16_V) V(VL2RE32_V) V(VL2RE64_V)
    V(VL4RE8_V) V(VL4RE16_V) V(VL4RE32_V) V(VL4RE64_V)
    V(VL8RE8_V) V(VL8RE16_V) V(VL8RE32_V) V(VL8RE64_V)
    V(VS1R_V) V(VS2R_V) V(VS4R_V) V(VS8R_V)
        shape->form = FORM_WHOLE_MEM;
        break;

    /* Mask register logical ops are single-register: QEMU checks only vill. */
    V(VMAND_MM) V(VMNAND_MM) V(VMANDN_MM) V(VMXOR_MM)
    V(VMOR_MM) V(VMNOR_MM) V(VMORN_MM) V(VMXNOR_MM)
    V(VCPOP_M) V(VFIRST_M)
        shape->form = FORM_NONE;
        break;
    V(VMSBF_M) V(VMSIF_M) V(VMSOF_M)
        shape->form = FORM_SCAN_SET;
        break;
    V(VIOTA_M)
        shape->form = FORM_VIOTA;
        break;
    V(VID_V)
        shape->form = FORM_VID;
        break;

    default:
        shape->form = FORM_NONE;
        break;
    }
}

#undef V

/* ---- whole-register transfers ------------------------------------------- */

static int whole_log2(unsigned nf)
{
    switch (nf) {
    case 2:  return 1;
    case 4:  return 2;
    case 8:  return 3;
    default: return 0;
    }
}

#define V(tag) case LATR_RV_##tag:

LatrRvvWholeKind latr_rvv_whole_kind(unsigned opcode)
{
    switch (opcode) {
    V(VMV1R_V) V(VMV2R_V) V(VMV4R_V) V(VMV8R_V)
        return LATR_RVV_WHOLE_MOVE;
    V(VL1RE8_V) V(VL1RE16_V) V(VL1RE32_V) V(VL1RE64_V)
    V(VL2RE8_V) V(VL2RE16_V) V(VL2RE32_V) V(VL2RE64_V)
    V(VL4RE8_V) V(VL4RE16_V) V(VL4RE32_V) V(VL4RE64_V)
    V(VL8RE8_V) V(VL8RE16_V) V(VL8RE32_V) V(VL8RE64_V)
        return LATR_RVV_WHOLE_LOAD;
    V(VS1R_V) V(VS2R_V) V(VS4R_V) V(VS8R_V)
        return LATR_RVV_WHOLE_STORE;
    default:
        return LATR_RVV_WHOLE_NONE;
    }
}

unsigned latr_rvv_whole_nf(unsigned opcode)
{
    switch (opcode) {
    V(VMV1R_V) V(VL1RE8_V) V(VL1RE16_V) V(VL1RE32_V) V(VL1RE64_V) V(VS1R_V)
        return 1;
    V(VMV2R_V) V(VL2RE8_V) V(VL2RE16_V) V(VL2RE32_V) V(VL2RE64_V) V(VS2R_V)
        return 2;
    V(VMV4R_V) V(VL4RE8_V) V(VL4RE16_V) V(VL4RE32_V) V(VL4RE64_V) V(VS4R_V)
        return 4;
    V(VMV8R_V) V(VL8RE8_V) V(VL8RE16_V) V(VL8RE32_V) V(VL8RE64_V) V(VS8R_V)
        return 8;
    default:
        return 0;
    }
}

unsigned latr_rvv_whole_eew(unsigned opcode)
{
    switch (opcode) {
    V(VL1RE8_V) V(VL2RE8_V) V(VL4RE8_V) V(VL8RE8_V)
        return 0;
    V(VL1RE16_V) V(VL2RE16_V) V(VL4RE16_V) V(VL8RE16_V)
        return 1;
    V(VL1RE32_V) V(VL2RE32_V) V(VL4RE32_V) V(VL8RE32_V)
        return 2;
    V(VL1RE64_V) V(VL2RE64_V) V(VL4RE64_V) V(VL8RE64_V)
        return 3;
    default:
        /* vs<n>r.v has no width field. Byte granularity still covers exactly
         * nf whole registers, and it is the finest index vstart can report. */
        return 0;
    }
}

bool latr_rvv_is_ldff(unsigned opcode)
{
    switch (opcode) {
    V(VLE8FF_V) V(VLE16FF_V) V(VLE32FF_V) V(VLE64FF_V)
        return true;
    default:
        return false;
    }
}

#undef V

/* ---- rule application --------------------------------------------------- */

typedef struct RvvCtx {
    int sew;  /* log2(SEW) */
    int lmul; /* log2(LMUL) */
    int elen; /* bits */
    int xlen;
    const RiscvIR1 *insn;
    LatrRvvLegality why;
} RvvCtx;

static void fail(RvvCtx *c, LatrRvvLegality why)
{
    if (c->why == LATR_RVV_LEGAL) {
        c->why = why;
    }
}

/* vext_check_ss(): masked destination must differ from v0, both groups aligned. */
static void check_ss(RvvCtx *c, int vd, int vs2)
{
    if (!vm_ok(c->insn->arg.vm, vd)) {
        fail(c, LATR_RVV_ILLEGAL_MASK);
    }
    if (!align_ok(vd, c->lmul) || !align_ok(vs2, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
}

static void check_sss(RvvCtx *c, int vd, int vs1, int vs2)
{
    check_ss(c, vd, vs2);
    if (!align_ok(vs1, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
}

/* vext_check_ms(): mask destination, single source group. */
static void check_ms(RvvCtx *c, int vd, int vs)
{
    if (!align_ok(vs, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
    if (vd != vs && !noover_ok(vd, 0, vs, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_OVERLAP);
    }
}

static void check_mss(RvvCtx *c, int vd, int vs1, int vs2)
{
    check_ms(c, vd, vs2);
    if (!align_ok(vs1, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
    if (vd != vs1 && !noover_ok(vd, 0, vs1, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_OVERLAP);
    }
}

/* vext_wide_check_common(): 2*SEW result must fit the largest group. */
static void check_wide_common(RvvCtx *c, int vd)
{
    if (c->lmul > 2 || c->sew >= 3 || c->sew + 1 > c->elen / 16) {
        fail(c, LATR_RVV_ILLEGAL_ELEN);
    }
    if (!align_ok(vd, c->lmul + 1)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
    if (!vm_ok(c->insn->arg.vm, vd)) {
        fail(c, LATR_RVV_ILLEGAL_MASK);
    }
}

static void check_ds(RvvCtx *c, int vd, int vs2)
{
    check_wide_common(c, vd);
    if (!align_ok(vs2, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
    if (!noover_ok(vd, c->lmul + 1, vs2, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_OVERLAP);
    }
}

static void check_dss(RvvCtx *c, int vd, int vs1, int vs2)
{
    check_ds(c, vd, vs2);
    if (!align_ok(vs1, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
    if (!noover_ok(vd, c->lmul + 1, vs1, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_OVERLAP);
    }
}

/*
 * Widening with a double-width source (`vwadd.wv/wx` and the unsigned/sub
 * forms): the destination and vs1 are 2*SEW wide, so both need the wider group
 * alignment; vs2 stays at SEW. Reusing check_ds would only align vd/vs2 and let
 * a misaligned wide source through.
 */
static void check_wide_src_w(RvvCtx *c, int vd, int vs1, int vs2)
{
    check_ds(c, vd, vs2);
    if (!align_ok(vs1, c->lmul + 1)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
    /* Both the destination and vs1 occupy 2*LMUL registers here, so an overlap
     * between them is a write-after-read hazard the helper cannot honour: the
     * wide destination would clobber the source mid-loop. */
    if (!noover_ok(vd, c->lmul + 1, vs1, c->lmul + 1)) {
        fail(c, LATR_RVV_ILLEGAL_OVERLAP);
    }
    if (!noover_ok(vd, c->lmul + 1, vs2, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_OVERLAP);
    }
}

/* vext_narrow_check_common(): SEW result from a 2*SEW source. */
static void check_sd(RvvCtx *c, int vd, int vs2)
{
    if (c->lmul > 2 || c->sew >= 3 || c->sew + 1 > c->elen / 16) {
        fail(c, LATR_RVV_ILLEGAL_ELEN);
    }
    if (!align_ok(vs2, c->lmul + 1)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
    if (!align_ok(vd, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
    if (!vm_ok(c->insn->arg.vm, vd)) {
        fail(c, LATR_RVV_ILLEGAL_MASK);
    }
    if (vd != vs2 && !noover_ok(vd, c->lmul, vs2, c->lmul + 1)) {
        fail(c, LATR_RVV_ILLEGAL_OVERLAP);
    }
}

static void check_sds(RvvCtx *c, int vd, int vs1, int vs2)
{
    check_sd(c, vd, vs2);
    if (!align_ok(vs1, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
}

static void check_reduction(RvvCtx *c, int vs2, int wide)
{
    if (wide && (c->sew >= 3 || c->sew + 1 > c->elen / 16)) {
        fail(c, LATR_RVV_ILLEGAL_ELEN);
    }
    if (!align_ok(vs2, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
}

/* vext_check_slide(). */
static void check_slide(RvvCtx *c, int vd, int vs2, int is_over)
{
    if (!align_ok(vs2, c->lmul) || !align_ok(vd, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
    if (is_over && vd == vs2) {
        fail(c, LATR_RVV_ILLEGAL_OVERLAP);
    }
    if (!vm_ok(c->insn->arg.vm, vd)) {
        fail(c, LATR_RVV_ILLEGAL_MASK);
    }
}

/* int_ext_check() for vzext/vsext. */
static void check_ext(RvvCtx *c, int vd, int vs2, int div)
{
    int from = (c->sew + 3) - div;
    int src_lmul = c->lmul - div;

    if (from < 3 || from > 8) {
        fail(c, LATR_RVV_ILLEGAL_ELEN);
    }
    if (vd == vs2) {
        fail(c, LATR_RVV_ILLEGAL_OVERLAP);
    }
    if (!align_ok(vd, c->lmul) || !align_ok(vs2, src_lmul)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
    if (!vm_ok(c->insn->arg.vm, vd)) {
        fail(c, LATR_RVV_ILLEGAL_MASK);
    }
    if (!noover_ok(vd, c->lmul, vs2, src_lmul)) {
        fail(c, LATR_RVV_ILLEGAL_OVERLAP);
    }
}

/* vext_check_store() plus vext_check_load()'s mask rule. */
static void check_mem(RvvCtx *c, int vd, int eew, int is_load)
{
    int emul = eew - c->sew + c->lmul;

    if (emul < -3 || emul > 3) {
        fail(c, LATR_RVV_ILLEGAL_EMUL);
    }
    if (!align_ok(vd, emul)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
    if (!nf_ok(vd, c->insn->arg.nf, emul)) {
        fail(c, LATR_RVV_ILLEGAL_NF);
    }
    if (is_load && !vm_ok(c->insn->arg.vm, vd)) {
        fail(c, LATR_RVV_ILLEGAL_MASK);
    }
}

/* vext_check_st_index() / vext_check_ld_index(). */
static void check_mem_index(RvvCtx *c, int vd, int vs2, int eew, int is_load)
{
    int emul = eew - c->sew + c->lmul;

    if (emul < -3 || emul > 3) {
        fail(c, LATR_RVV_ILLEGAL_EMUL);
    }
    if (!align_ok(vs2, emul)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
    if (!align_ok(vd, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_ALIGN);
    }
    if (!nf_ok(vd, c->insn->arg.nf, c->lmul)) {
        fail(c, LATR_RVV_ILLEGAL_NF);
    }
    if (c->xlen == 32 && eew == 3) {
        fail(c, LATR_RVV_ILLEGAL_INDEX);
    }
    if (!is_load) {
        return;
    }
    if (!vm_ok(c->insn->arg.vm, vd)) {
        fail(c, LATR_RVV_ILLEGAL_MASK);
    }
    /* Each segment group must also stay clear of the index vector. */
    for (int i = 0; i < c->insn->arg.nf; i++) {
        int seg_vd = vd + group_size(c->lmul) * i;

        if (eew > c->sew) {
            if (seg_vd != vs2 && !noover_ok(seg_vd, c->lmul, vs2, emul)) {
                fail(c, LATR_RVV_ILLEGAL_OVERLAP);
            }
        } else if (eew < c->sew) {
            if (!noover_ok(seg_vd, c->lmul, vs2, emul)) {
                fail(c, LATR_RVV_ILLEGAL_OVERLAP);
            }
        }
        if (c->insn->arg.nf > 1 &&
            overlaps(seg_vd, group_size(c->lmul), vs2, group_size(emul))) {
            fail(c, LATR_RVV_ILLEGAL_OVERLAP);
        }
    }
}

LatrRvvLegality latr_rvv_check_operands(const RiscvIR1 *insn,
                                        const LatrVectorState *state)
{
    RvvCtx c;
    RvvShape shape;
    int vd;
    int vs1;
    int vs2;

    if (insn == 0 || state == 0) {
        return LATR_RVV_LEGAL;
    }
    if (state->vill != 0) {
        return LATR_RVV_ILLEGAL_VILL;
    }
    c.sew = vtype_sew_log(state->vtype);
    c.lmul = vtype_lmul(state->vtype);
    c.elen = state->elen != 0 ? state->elen : 64;
    c.xlen = state->xlen != 0 ? state->xlen : 64;
    c.insn = insn;
    c.why = LATR_RVV_LEGAL;

    if ((8 << c.sew) > c.elen) {
        return LATR_RVV_ILLEGAL_ELEN;
    }

    rvv_shape((unsigned)insn->opcode, &shape);
    vd = insn->arg.rd;
    vs1 = insn->arg.rs1;
    vs2 = insn->arg.rs2;

    switch (shape.form) {
    case FORM_NONE:
        break;
    case FORM_SS:
        check_ss(&c, vd, vs2);
        break;
    case FORM_SSS:
        check_sss(&c, vd, vs1, vs2);
        break;
    case FORM_MS:
        check_ms(&c, vd, vs2);
        break;
    case FORM_MSS:
        check_mss(&c, vd, vs1, vs2);
        break;
    case FORM_DS:
        check_ds(&c, vd, vs2);
        break;
    case FORM_DSS:
        check_dss(&c, vd, vs1, vs2);
        break;
    case FORM_SD:
        check_sd(&c, vd, vs2);
        break;
    case FORM_SDS:
        check_sds(&c, vd, vs1, vs2);
        break;
    case FORM_WIDE_SRC_W:
        check_wide_src_w(&c, vd, vs1, vs2);
        break;
    case FORM_REDUCTION:
        check_reduction(&c, vs2, 0);
        break;
    case FORM_REDUCTION_W:
        check_reduction(&c, vs2, 1);
        break;
    case FORM_SLIDE_UP:
        check_slide(&c, vd, vs2, 1);
        break;
    case FORM_SLIDE_DOWN:
        check_slide(&c, vd, vs2, 0);
        break;
    case FORM_GATHER_VV:
        if (!align_ok(vd, c.lmul) || !align_ok(vs1, c.lmul) ||
            !align_ok(vs2, c.lmul)) {
            fail(&c, LATR_RVV_ILLEGAL_ALIGN);
        }
        if (vd == vs2 || vd == vs1) {
            fail(&c, LATR_RVV_ILLEGAL_OVERLAP);
        }
        if (!vm_ok(insn->arg.vm, vd)) {
            fail(&c, LATR_RVV_ILLEGAL_MASK);
        }
        break;
    case FORM_GATHER_VX:
        if (!align_ok(vd, c.lmul) || !align_ok(vs2, c.lmul)) {
            fail(&c, LATR_RVV_ILLEGAL_ALIGN);
        }
        if (vd == vs2) {
            fail(&c, LATR_RVV_ILLEGAL_OVERLAP);
        }
        if (!vm_ok(insn->arg.vm, vd)) {
            fail(&c, LATR_RVV_ILLEGAL_MASK);
        }
        break;
    case FORM_GATHER_EI16: {
        int emul = 1 - c.sew + c.lmul; /* index EEW is always 16 bits */

        if (emul < -3 || emul > 3) {
            fail(&c, LATR_RVV_ILLEGAL_EMUL);
        }
        if (!align_ok(vd, c.lmul) || !align_ok(vs1, emul) ||
            !align_ok(vs2, c.lmul)) {
            fail(&c, LATR_RVV_ILLEGAL_ALIGN);
        }
        if (vd == vs2 || vd == vs1) {
            fail(&c, LATR_RVV_ILLEGAL_OVERLAP);
        }
        if (overlaps(vd, group_size(c.lmul), vs1, group_size(emul)) ||
            overlaps(vd, group_size(c.lmul), vs2, group_size(c.lmul))) {
            fail(&c, LATR_RVV_ILLEGAL_OVERLAP);
        }
        if (!vm_ok(insn->arg.vm, vd)) {
            fail(&c, LATR_RVV_ILLEGAL_MASK);
        }
        break;
    }
    case FORM_COMPRESS:
        if (!align_ok(vd, c.lmul) || !align_ok(vs2, c.lmul)) {
            fail(&c, LATR_RVV_ILLEGAL_ALIGN);
        }
        if (vd == vs2) {
            fail(&c, LATR_RVV_ILLEGAL_OVERLAP);
        }
        if (overlaps(vd, group_size(c.lmul), vs1, 1)) {
            fail(&c, LATR_RVV_ILLEGAL_OVERLAP);
        }
        break;
    case FORM_EXT:
        check_ext(&c, vd, vs2, shape.div);
        break;
    case FORM_LOAD:
    case FORM_STORE:
        check_mem(&c, vd, shape.eew, shape.form == FORM_LOAD);
        break;
    case FORM_LOAD_INDEX:
    case FORM_STORE_INDEX:
        check_mem_index(&c, vd, vs2, shape.eew, shape.form == FORM_LOAD_INDEX);
        break;
    case FORM_ALIGN_ONLY:
        if (!align_ok(vd, c.lmul)) {
            fail(&c, LATR_RVV_ILLEGAL_ALIGN);
        }
        break;
    case FORM_ALIGN_PAIR:
        if (!align_ok(vd, c.lmul) || !align_ok(vs1, c.lmul)) {
            fail(&c, LATR_RVV_ILLEGAL_ALIGN);
        }
        break;
    case FORM_SCAN_SET:
        if (!vm_ok(insn->arg.vm, vd)) {
            fail(&c, LATR_RVV_ILLEGAL_MASK);
        }
        if (vd == vs2) {
            fail(&c, LATR_RVV_ILLEGAL_OVERLAP);
        }
        break;
    case FORM_VIOTA:
        if (overlaps(vd, group_size(c.lmul), vs2, 1)) {
            fail(&c, LATR_RVV_ILLEGAL_OVERLAP);
        }
        if (!vm_ok(insn->arg.vm, vd)) {
            fail(&c, LATR_RVV_ILLEGAL_MASK);
        }
        if (!align_ok(vd, c.lmul)) {
            fail(&c, LATR_RVV_ILLEGAL_ALIGN);
        }
        break;
    case FORM_VID:
        if (!align_ok(vd, c.lmul)) {
            fail(&c, LATR_RVV_ILLEGAL_ALIGN);
        }
        if (!vm_ok(insn->arg.vm, vd)) {
            fail(&c, LATR_RVV_ILLEGAL_MASK);
        }
        break;
    case FORM_WHOLE_MOVE: {
        int nf_log = whole_log2(latr_rvv_whole_nf((unsigned)insn->opcode));

        /* QEMU trans_vmvNr_v(): both the source and the destination must start
         * on a register-group boundary of their own width. */
        if (!align_ok(vd, nf_log) || !align_ok(vs2, nf_log)) {
            fail(&c, LATR_RVV_ILLEGAL_ALIGN);
        }
        break;
    }
    case FORM_WHOLE_MEM: {
        int nf_log = whole_log2(latr_rvv_whole_nf((unsigned)insn->opcode));

        /* trans_vl<n>re_v()/trans_vs<n>r_v(): the base register only has to be
         * group aligned; the transfer covers nf*VLEN bits from there. */
        if (!align_ok(vd, nf_log)) {
            fail(&c, LATR_RVV_ILLEGAL_ALIGN);
        }
        break;
    }
    default:
        break;
    }
    return c.why;
}

/*
 * Every QEMU translation that calls gen_set_rm()/gen_set_rm_chkfrm() traps
 * when frm holds a reserved value; the FP compares are the only vector FP
 * instructions that do not. LATR therefore gates `frm < 5` for everything in
 * this list and nothing else.
 */
bool latr_rvv_uses_frm(unsigned opcode)
{
    switch (opcode) {
    case LATR_RV_VFWMACC_VV: case LATR_RV_VFWMACC_VF:
    case LATR_RV_VFWNMACC_VV: case LATR_RV_VFWNMACC_VF:
    case LATR_RV_VFWMSAC_VV: case LATR_RV_VFWMSAC_VF:
    case LATR_RV_VFWNMSAC_VV: case LATR_RV_VFWNMSAC_VF:
    case LATR_RV_VFMACC_VV: case LATR_RV_VFMACC_VF:
    case LATR_RV_VFNMACC_VV: case LATR_RV_VFNMACC_VF:
    case LATR_RV_VFMSAC_VV: case LATR_RV_VFMSAC_VF:
    case LATR_RV_VFNMSC_VV: case LATR_RV_VFNMSC_VF:
    case LATR_RV_VFMADD_VV: case LATR_RV_VFMADD_VF:
    case LATR_RV_VFNMADD_VV: case LATR_RV_VFNMADD_VF:
    case LATR_RV_VFMSUB_VV: case LATR_RV_VFMSUB_VF:
    case LATR_RV_VFNMSSUB_VV: case LATR_RV_VFNMSSUB_VF:
    case LATR_RV_VFADD_VV: case LATR_RV_VFADD_VF:
    case LATR_RV_VFSUB_VV: case LATR_RV_VFSUB_VF:
    case LATR_RV_VFMUL_VV: case LATR_RV_VFMUL_VF:
    case LATR_RV_VFDIV_VV: case LATR_RV_VFDIV_VF:
    case LATR_RV_VFMIN_VV: case LATR_RV_VFMIN_VF:
    case LATR_RV_VFMAX_VV: case LATR_RV_VFMAX_VF:
    case LATR_RV_VFSQRT_V: case LATR_RV_VFCLASS_V:
    case LATR_RV_VFSGNJ_VV: case LATR_RV_VFSGNJ_VF:
    case LATR_RV_VFSGNJN_VV: case LATR_RV_VFSGNJN_VF:
    case LATR_RV_VFSGNJX_VV: case LATR_RV_VFSGNJX_VF:
    case LATR_RV_VFMERGE_VFM:
    case LATR_RV_VFMV_V_F: case LATR_RV_VFMV_F_S: case LATR_RV_VFMV_S_F:
    case LATR_RV_VFCVT_XU_F_V: case LATR_RV_VFCVT_X_F_V:
    case LATR_RV_VFCVT_F_XU_V: case LATR_RV_VFCVT_F_X_V:
    case LATR_RV_VFCVT_RTZ_XU_F_V: case LATR_RV_VFCVT_RTZ_X_F_V:
    case LATR_RV_VFWCVT_XU_F_V: case LATR_RV_VFWCVT_X_F_V:
    case LATR_RV_VFWCVT_F_XU_V: case LATR_RV_VFWCVT_F_X_V:
    case LATR_RV_VFWCVT_F_F_V:
    case LATR_RV_VFWCVT_RTZ_XU_F_V: case LATR_RV_VFWCVT_RTZ_X_F_V:
    case LATR_RV_VFNCVT_XU_F_W: case LATR_RV_VFNCVT_X_F_W:
    case LATR_RV_VFNCVT_F_XU_W: case LATR_RV_VFNCVT_F_X_W:
    case LATR_RV_VFNCVT_F_F_W: case LATR_RV_VFNCVT_ROD_F_F_W:
    case LATR_RV_VFNCVT_RTZ_XU_F_W: case LATR_RV_VFNCVT_RTZ_X_F_W:
    case LATR_RV_VFREDUSUM_VS: case LATR_RV_VFREDOSUM_VS:
    case LATR_RV_VFREDMIN_VS: case LATR_RV_VFREDMAX_VS:
    case LATR_RV_VFWREDUSUM_VS: case LATR_RV_VFWREDOSUM_VS:
    case LATR_RV_VFWADD_VV: case LATR_RV_VFWADD_VF:
    case LATR_RV_VFWADD_WV: case LATR_RV_VFWADD_WF:
    case LATR_RV_VFWSUB_VV: case LATR_RV_VFWSUB_VF:
    case LATR_RV_VFWSUB_WV: case LATR_RV_VFWSUB_WF:
    case LATR_RV_VFWMUL_VV: case LATR_RV_VFWMUL_VF:
        return true;
    default:
        return false;
    }
}

const char *latr_rvv_legality_name(LatrRvvLegality why)
{
    switch (why) {
    case LATR_RVV_LEGAL:            return "legal";
    case LATR_RVV_ILLEGAL_VILL:     return "vill";
    case LATR_RVV_ILLEGAL_EMUL:     return "emul";
    case LATR_RVV_ILLEGAL_ALIGN:    return "align";
    case LATR_RVV_ILLEGAL_OVERLAP:  return "overlap";
    case LATR_RVV_ILLEGAL_MASK:     return "mask";
    case LATR_RVV_ILLEGAL_NF:       return "nf";
    case LATR_RVV_ILLEGAL_ELEN:     return "elen";
    case LATR_RVV_ILLEGAL_INDEX:    return "index";
    default:                        return "unknown";
    }
}
