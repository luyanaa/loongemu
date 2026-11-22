/*
 * Host-side unit test for latr_rvv_check_operands(). The instruction words are
 * real encodings (assembled with llvm-mc), so this also pins the operand field
 * extraction that the legality rules depend on.
 */
#include <stdio.h>

#include "ir1.h"
#include "latr-rvv-legality.h"

static int checks;
static int failures;

#define CHECK(condition, description) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "LATR rvv legality: FAIL %s\n", description); \
        failures++; \
    } \
} while (0)

/* vlmul field: 0..3 = LMUL 1,2,4,8; 5..7 = 1/8,1/4,1/2. */
#define VT(sew_log, vlmul) (((uint64_t)(sew_log) << 3) | (uint64_t)(vlmul))

static LatrRvvLegality check_word(uint32_t word, uint64_t vtype)
{
    RiscvIR1 ir1;
    LatrVectorState state;

    if (!latr_decode(0x1000, word, &ir1)) {
        fprintf(stderr, "LATR rvv legality: cannot decode 0x%08x\n", word);
        return LATR_RVV_LEGAL;
    }
    state.vtype = vtype;
    state.vill = 0;
    state.elen = 64;
    state.xlen = 64;
    return latr_rvv_check_operands(&ir1, &state);
}

int main(void)
{
    RiscvIR1 ir1;
    LatrVectorState state;

    /* The group of v31 spans past v31 with LMUL=8: it must be rejected rather
     * than wrapping around into v0..v6. */
    CHECK(check_word(0x02008fd7, VT(3, 3)) == LATR_RVV_ILLEGAL_ALIGN,
          "vadd.vv v31,v0,v1 with LMUL=8 is illegal");
    CHECK(check_word(0x02008457, VT(3, 3)) == LATR_RVV_ILLEGAL_ALIGN,
          "vadd.vv v8,v0,v1 with LMUL=8 rejects misaligned vs1");
    CHECK(check_word(0x02040457, VT(3, 3)) == LATR_RVV_LEGAL,
          "vadd.vv v8,v0,v8 with LMUL=8 is legal");

    /* Masked destination must not clobber v0. */
    CHECK(check_word(0x001100d7, VT(0, 0)) == LATR_RVV_LEGAL,
          "masked vadd.vv v1,v1,v2 is legal");
    CHECK(check_word(0x00110057, VT(0, 0)) == LATR_RVV_ILLEGAL_MASK,
          "masked vadd.vv into v0 is illegal");

    /* EMUL for loads/stores follows EEW - SEW + LMUL. */
    CHECK(check_word(0x0200e187, VT(2, 1)) == LATR_RVV_ILLEGAL_ALIGN,
          "vle32.v v3 with LMUL=2 is illegal");
    CHECK(check_word(0x0200e187, VT(2, 0)) == LATR_RVV_LEGAL,
          "vle32.v v3 with LMUL=1 is legal");
    CHECK(check_word(0x0200e1a7, VT(2, 1)) == LATR_RVV_ILLEGAL_ALIGN,
          "vse32.v v3 with LMUL=2 is illegal");
    CHECK(check_word(0x0200f407, VT(3, 3)) == LATR_RVV_LEGAL,
          "vle64.v v8 with LMUL=8 is legal");

    /* Widening/narrowing conversions clip at 8-bit sources. */
    CHECK(check_word(0x4a2321d7, VT(1, 0)) == LATR_RVV_LEGAL,
          "vzext.vf2 with SEW=16 is legal");
    CHECK(check_word(0x4a2321d7, VT(0, 0)) == LATR_RVV_ILLEGAL_ELEN,
          "vzext.vf2 with SEW=8 is illegal");
    CHECK(check_word(0x4a2121d7, VT(3, 0)) == LATR_RVV_LEGAL,
          "vzext.vf8 with SEW=64 is legal");
    CHECK(check_word(0x4a2121d7, VT(2, 0)) == LATR_RVV_ILLEGAL_ELEN,
          "vzext.vf8 with SEW=32 is illegal");
    CHECK(check_word(0x4a22a257, VT(2, 0)) == LATR_RVV_LEGAL,
          "vsext.vf4 with SEW=32 is legal");
    CHECK(check_word(0x4a22a257, VT(1, 0)) == LATR_RVV_ILLEGAL_ELEN,
          "vsext.vf4 with SEW=16 is illegal");

    /* 2*SEW must fit ELEN and LMUL <= 2. */
    CHECK(check_word(0xc6222457, VT(3, 0)) == LATR_RVV_ILLEGAL_ELEN,
          "vwadd.vv with SEW=64 is illegal");
    CHECK(check_word(0xc6222457, VT(2, 0)) == LATR_RVV_LEGAL,
          "vwadd.vv with SEW=32 is legal");
    CHECK(check_word(0xc6222457, VT(2, 3)) == LATR_RVV_ILLEGAL_ELEN,
          "vwadd.vv with LMUL=8 is illegal");
    CHECK(check_word(0xb220b257, VT(2, 0)) == LATR_RVV_LEGAL,
          "vnsrl.wi v4,v2 with SEW=32 is legal");
    CHECK(check_word(0xb220b257, VT(3, 0)) == LATR_RVV_ILLEGAL_ELEN,
          "vnsrl.wi with SEW=64 is illegal");
    /* The narrow destination may only overlap the low part of its source
     * group, so v3 inside the {v2,v3} source group is rejected. */
    CHECK(check_word(0xb220b1d7, VT(2, 0)) == LATR_RVV_ILLEGAL_OVERLAP,
          "vnsrl.wi v3,v2 overlaps the high half of the source group");

    /* Reductions and slides follow the source/destination group rules. */
    CHECK(check_word(0x0220a457, VT(3, 3)) == LATR_RVV_ILLEGAL_ALIGN,
          "vredsum.vs with a misaligned source is illegal");
    CHECK(check_word(0x03002457, VT(3, 3)) == LATR_RVV_LEGAL,
          "vredsum.vs with aligned groups is legal");
    CHECK(check_word(0x3a20c257, VT(2, 0)) == LATR_RVV_LEGAL,
          "vslideup.vx v4,v2 is legal");
    CHECK(check_word(0x3a40c257, VT(2, 0)) == LATR_RVV_ILLEGAL_OVERLAP,
          "vslideup.vx v4,v4 is illegal");
    CHECK(check_word(0x32208257, VT(2, 0)) == LATR_RVV_LEGAL,
          "vrgather.vv v4,v2,v1 is legal");
    CHECK(check_word(0x32408257, VT(2, 0)) == LATR_RVV_ILLEGAL_OVERLAP,
          "vrgather.vv v4,v4,v1 is illegal");
    CHECK(check_word(0x3220b257, VT(2, 0)) == LATR_RVV_LEGAL,
          "vrgather.vi v4,v2,1 is legal");

    /* ---- whole-register transfers -------------------------------------
     * vmv<n>r.v, vl<n>re<eew>.v and vs<n>r.v move nf complete registers and
     * ignore vl/vtype entirely; the only legality rule is that each register
     * group starts on an nf-aligned boundary. */
    CHECK(check_word(0x9e80b257, VT(2, 0)) == LATR_RVV_LEGAL,
          "vmv2r.v v4,v8 is legal");
    CHECK(check_word(0x9e80b1d7, VT(2, 0)) == LATR_RVV_ILLEGAL_ALIGN,
          "vmv2r.v v3,v8 rejects a misaligned destination");
    CHECK(check_word(0x9e30b257, VT(2, 0)) == LATR_RVV_ILLEGAL_ALIGN,
          "vmv2r.v v4,v3 rejects a misaligned source");
    CHECK(check_word(0x9e83bc57, VT(2, 0)) == LATR_RVV_LEGAL,
          "vmv8r.v v24,v8 is legal");
    CHECK(check_word(0x9e83ba57, VT(2, 0)) == LATR_RVV_ILLEGAL_ALIGN,
          "vmv8r.v v20,v8 rejects a destination that is not 8-aligned");
    CHECK(check_word(0x9f43bc57, VT(2, 0)) == LATR_RVV_ILLEGAL_ALIGN,
          "vmv8r.v v24,v20 rejects a source that is not 8-aligned");

    CHECK(check_word(0x22856207, VT(2, 0)) == LATR_RVV_LEGAL,
          "vl2re32.v v4 is legal");
    CHECK(check_word(0x22856187, VT(2, 0)) == LATR_RVV_ILLEGAL_ALIGN,
          "vl2re32.v v3 rejects a misaligned destination");
    CHECK(check_word(0xe2856807, VT(2, 0)) == LATR_RVV_LEGAL,
          "vl8re32.v v16 is legal");
    CHECK(check_word(0xe2856a07, VT(2, 0)) == LATR_RVV_ILLEGAL_ALIGN,
          "vl8re32.v v20 rejects a destination that is not 8-aligned");
    CHECK(check_word(0x22850227, VT(2, 0)) == LATR_RVV_LEGAL,
          "vs2r.v v4 is legal");
    CHECK(check_word(0x228501a7, VT(2, 0)) == LATR_RVV_ILLEGAL_ALIGN,
          "vs2r.v v3 rejects a misaligned source");
    CHECK(check_word(0xe2850c27, VT(2, 0)) == LATR_RVV_LEGAL,
          "vs8r.v v24 is legal");
    CHECK(check_word(0xe2850a27, VT(2, 0)) == LATR_RVV_ILLEGAL_ALIGN,
          "vs8r.v v20 rejects a source that is not 8-aligned");
    /* nf comes from the opcode, so vtype must not influence the outcome. */
    CHECK(check_word(0xe2857c07, VT(3, 3)) == LATR_RVV_LEGAL,
          "vl8re64.v v24 ignores SEW/LMUL");
    CHECK(check_word(0x02850027, VT(3, 0)) == LATR_RVV_LEGAL,
          "vs1r.v v0 is legal for any vtype");

    /* ---- segment accesses (nf from the %nf field) ---------------------
     * The segment forms share the unit-stride decode tag and differ only in
     * bits [31:29], which QEMU extracts as nf-1. */
    CHECK(check_word(0x22056207, VT(2, 0)) == LATR_RVV_LEGAL,
          "vlseg2e32.v v4 is legal");
    CHECK(check_word(0x62056407, VT(2, 0)) == LATR_RVV_LEGAL,
          "vlseg4e32.v v8 is legal");
    CHECK(check_word(0xe2050c07, VT(0, 0)) == LATR_RVV_LEGAL,
          "vlseg8e8.v v24 is legal");
    CHECK(check_word(0xe2050e07, VT(0, 0)) == LATR_RVV_ILLEGAL_NF,
          "vlseg8e8.v v28 runs past v31");
    CHECK(check_word(0x62056207, VT(2, 2)) == LATR_RVV_ILLEGAL_NF,
          "vlseg4e32.v v4 with LMUL=4 needs nf*EMUL <= 8");
    CHECK(check_word(0x22056187, VT(2, 1)) == LATR_RVV_ILLEGAL_ALIGN,
          "vlseg2e32.v v3 with LMUL=2 is misaligned");
    CHECK(check_word(0x20056007, VT(2, 0)) == LATR_RVV_ILLEGAL_MASK,
          "masked vlseg2e32.v into v0 is illegal");
    CHECK(check_word(0x2ab56207, VT(2, 0)) == LATR_RVV_LEGAL,
          "vlsseg2e32.v v4 is legal");
    CHECK(check_word(0x22056227, VT(2, 0)) == LATR_RVV_LEGAL,
          "vsseg2e32.v v4 has no mask rule");
    CHECK(check_word(0xe2050c27, VT(0, 0)) == LATR_RVV_LEGAL,
          "vsseg8e8.v v24 is legal");

    /* ---- fault-only-first loads --------------------------------------
     * vle<eew>ff.v obeys the ordinary unit-stride load rules; only the
     * runtime behaviour on a fault differs. */
    CHECK(check_word(0x03056207, VT(2, 0)) == LATR_RVV_LEGAL,
          "vle32ff.v v4 is legal");
    CHECK(check_word(0x03057407, VT(3, 0)) == LATR_RVV_LEGAL,
          "vle64ff.v v8 with LMUL=1 is legal");
    CHECK(check_word(0x03050f87, VT(0, 0)) == LATR_RVV_LEGAL,
          "vle8ff.v v31 with LMUL=1 is legal");
    CHECK(check_word(0x03056187, VT(2, 1)) == LATR_RVV_ILLEGAL_ALIGN,
          "vle32ff.v v3 with LMUL=2 is misaligned");
    CHECK(check_word(0x03057087, VT(3, 3)) == LATR_RVV_ILLEGAL_ALIGN,
          "vle64ff.v v1 with LMUL=8 is misaligned");
    CHECK(check_word(0x03057207, VT(3, 3)) == LATR_RVV_ILLEGAL_ALIGN,
          "vle64ff.v v4 with LMUL=8 is misaligned");

    /* The decode contract these families rely on: nf is bits [31:29] + 1. */
    CHECK(latr_decode(0x1000, 0x22056207, &ir1), "decode vlseg2e32.v");
    CHECK(ir1.arg.nf == 2, "vlseg2e32.v decodes as nf=2");
    CHECK(latr_decode(0x1000, 0xe2050c07, &ir1), "decode vlseg8e8.v");
    CHECK(ir1.arg.nf == 8, "vlseg8e8.v decodes as nf=8");
    CHECK(latr_decode(0x1000, 0x02056207, &ir1), "decode vle32.v");
    CHECK(ir1.arg.nf == 1, "vle32.v decodes as nf=1");

    /* Mask instructions are single-register: QEMU checks only vill. */
    CHECK(check_word(0x6621a0d7, VT(3, 3)) == LATR_RVV_LEGAL,
          "vmand.mm ignores LMUL alignment");

    /* Register-gather of a whole group, and compress. */
    CHECK(check_word(0x33880457, VT(3, 3)) == LATR_RVV_LEGAL,
          "vrgather.vv with four aligned LMUL=8 groups is legal");
    CHECK(check_word(0x5e2020d7, VT(2, 0)) == LATR_RVV_LEGAL,
          "vcompress.vm with distinct groups is legal");
    CHECK(check_word(0x5e2020d7, VT(3, 3)) == LATR_RVV_ILLEGAL_ALIGN,
          "vcompress.vm with a misaligned destination is illegal");

    /* Moves, masks and videos. */
    CHECK(check_word(0x5e00c057, VT(3, 3)) == LATR_RVV_LEGAL,
          "vmv.v.x v0 with LMUL=8 is legal");
    CHECK(check_word(0x02b08087, VT(3, 3)) == LATR_RVV_LEGAL,
          "vlm.v ignores LMUL alignment");
    CHECK(check_word(0x5208a0d7, VT(0, 0)) == LATR_RVV_LEGAL,
          "vid.v v1 is legal");
    CHECK(check_word(0x5208a0d7, VT(0, 3)) == LATR_RVV_ILLEGAL_ALIGN,
          "vid.v v1 with LMUL=8 is illegal");
    CHECK(check_word(0x5008a0d7, VT(0, 0)) == LATR_RVV_LEGAL,
          "masked vid.v v1 is legal");
    CHECK(check_word(0x5008a057, VT(0, 0)) == LATR_RVV_ILLEGAL_MASK,
          "masked vid.v into v0 is illegal");

    /* FP and mixed-width FP. */
    CHECK(check_word(0x022190d7, VT(2, 0)) == LATR_RVV_LEGAL,
          "vfadd.vv v1,v2,v3 is legal");
    CHECK(check_word(0xc2209257, VT(2, 0)) == LATR_RVV_LEGAL,
          "vfwadd.vv v4,v2,v1 is legal");

    /* Immediate forms keep their own operand shape. */
    CHECK(check_word(0x021fb1d7, VT(0, 0)) == LATR_RVV_LEGAL,
          "vadd.vi v3,v1,-1 is legal");
    CHECK(check_word(0x6212b1d7, VT(0, 0)) == LATR_RVV_LEGAL,
          "vmseq.vi v3,v1,5 is legal");

    /* vl/vstart are runtime state and must not be consulted here. */
    CHECK(latr_decode(0x1000, 0x02040457, &ir1), "decode vadd.vv for state test");
    state.vtype = VT(3, 3);
    state.vill = 1;
    state.elen = 64;
    state.xlen = 64;
    CHECK(latr_rvv_check_operands(&ir1, &state) == LATR_RVV_ILLEGAL_VILL,
          "vill makes every vector instruction illegal");
    CHECK(latr_rvv_check_operands(0, &state) == LATR_RVV_LEGAL,
          "null instruction is not judged");

    /* Only the instructions that sample frm may be gated on it. */
    CHECK(latr_rvv_uses_frm(LATR_RV_VFADD_VV), "vfadd.vv uses frm");
    CHECK(latr_rvv_uses_frm(LATR_RV_VFCVT_X_F_V), "vfcvt uses frm");
    CHECK(latr_rvv_uses_frm(LATR_RV_VFREDUSUM_VS), "vfredusum uses frm");
    CHECK(!latr_rvv_uses_frm(LATR_RV_VMFEQ_VV), "vmfeq.vv does not use frm");
    CHECK(!latr_rvv_uses_frm(LATR_RV_VADD_VV), "vadd.vv does not use frm");

    /* Whole-register dispatch: the translator picks its helper from this, so
     * the three families must stay distinguishable from ordinary loads. */
    CHECK(latr_rvv_whole_kind(LATR_RV_VMV2R_V) == LATR_RVV_WHOLE_MOVE,
          "vmv2r.v is a whole-register move");
    CHECK(latr_rvv_whole_kind(LATR_RV_VL2RE32_V) == LATR_RVV_WHOLE_LOAD,
          "vl2re32.v is a whole-register load");
    CHECK(latr_rvv_whole_kind(LATR_RV_VS2R_V) == LATR_RVV_WHOLE_STORE,
          "vs2r.v is a whole-register store");
    CHECK(latr_rvv_whole_kind(LATR_RV_VLE32_V) == LATR_RVV_WHOLE_NONE,
          "vle32.v is not a whole-register transfer");
    CHECK(latr_rvv_whole_nf(LATR_RV_VMV8R_V) == 8, "vmv8r.v moves 8 registers");
    CHECK(latr_rvv_whole_nf(LATR_RV_VL1RE8_V) == 1, "vl1re8.v moves 1 register");
    CHECK(latr_rvv_whole_nf(LATR_RV_VS4R_V) == 4, "vs4r.v moves 4 registers");
    CHECK(latr_rvv_whole_nf(LATR_RV_VLE32_V) == 0,
          "ordinary loads have no whole-register nf");
    CHECK(latr_rvv_whole_eew(LATR_RV_VL2RE32_V) == 2, "vl2re32.v is 32-bit");
    CHECK(latr_rvv_whole_eew(LATR_RV_VL8RE64_V) == 3, "vl8re64.v is 64-bit");
    CHECK(latr_rvv_whole_eew(LATR_RV_VS2R_V) == 0,
          "vs<n>r.v has no width field");
    CHECK(latr_rvv_is_ldff(LATR_RV_VLE8FF_V), "vle8ff.v is fault-only-first");
    CHECK(latr_rvv_is_ldff(LATR_RV_VLE64FF_V), "vle64ff.v is fault-only-first");
    CHECK(!latr_rvv_is_ldff(LATR_RV_VLE32_V), "vle32.v is not fault-only-first");
    CHECK(!latr_rvv_is_ldff(LATR_RV_VL2RE32_V),
          "vl2re32.v is not fault-only-first");

    CHECK(latr_rvv_legality_name(LATR_RVV_ILLEGAL_ALIGN)[0] != '\0',
          "legality reasons are printable");

    if (failures != 0) {
        fprintf(stderr, "LATR rvv legality: %d/%d checks failed\n",
                failures, checks);
        return 1;
    }
    printf("LATR rvv legality: %d checks passed\n", checks);
    return 0;
}
