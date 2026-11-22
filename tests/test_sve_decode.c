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

static void test_expanded_sve_decode(void)
{
    static const struct {
        uint32_t word;
        AARCH64_INSN_TYPE_A64 want_type;
    } cases[] = {
        /* FFR & fault-tolerant loads + COMPACT */
        { 0x25289040u, AARCH64_A64_SVE_WRFFR },      /* wrffr p2.b */
        { 0x2518f020u, AARCH64_A64_SVE_RDFFR },      /* rdffr p0.b, p1/z */
        { 0x2558f020u, AARCH64_A64_SVE_RDFFR },      /* rdffrs p0.b, p1/z */
        { 0xa4bf6861u, AARCH64_A64_SVE_LDFF1H },     /* ldff1h {z1.h}, p2/z, [x3] */
        { 0xa552b4c4u, AARCH64_A64_SVE_LDNF1W },     /* ldnf1w {z4.s}, p5/z, [x6, #2, mul vl] */
        { 0x05a18440u, AARCH64_A64_SVE_COMPACT },    /* compact z0.s, p1, z2.s */
        /* Predicate family */
        { 0x2550c440u, AARCH64_A64_SVE_PTEST },      /* ptest p1, p2.b */
        { 0x25a21420u, AARCH64_A64_SVE_WHILE },      /* whilelt p0.s, x1, x2 */
        { 0x25a21430u, AARCH64_A64_SVE_WHILE },      /* whilele p0.s, x1, x2 */
        { 0x25a21c30u, AARCH64_A64_SVE_WHILE },      /* whilels p0.s, x1, x2 */
        { 0x25a21030u, AARCH64_A64_SVE_WHILE },      /* whilegt p0.s, x1, x2 */
        { 0x25a21020u, AARCH64_A64_SVE_WHILE },      /* whilege p0.s, x1, x2 */
        { 0x25a21830u, AARCH64_A64_SVE_WHILE },      /* whilehi p0.s, x1, x2 */
        { 0x25a21820u, AARCH64_A64_SVE_WHILE },      /* whilehs p0.s, x1, x2 */
        { 0x25a23030u, AARCH64_A64_SVE_WHILE },      /* whilerw p0.s, x1, x2 */
        { 0x25a23020u, AARCH64_A64_SVE_WHILE },      /* whilewr p0.s, x1, x2 */
        { 0x25a08440u, AARCH64_A64_SVE_CNTP },       /* cntp x0, p1, p2.s */
        { 0x0421e3e0u, AARCH64_A64_SVE_CNT_SCALAR }, /* cntb x0, all, mul #2 */
        { 0x04b2e3e0u, AARCH64_A64_SVE_CNT_SCALAR }, /* incw x0, all, mul #3 */
        { 0x04a0f3e0u, AARCH64_A64_SVE_CNT_SCALAR }, /* sqincw x0, w0 */
        { 0x04b0ffe0u, AARCH64_A64_SVE_CNT_SCALAR }, /* uqdecw x0 */
        { 0x25104440u, AARCH64_A64_SVE_BRK },        /* brka p0.b, p1/z, p2.b */
        { 0x25904450u, AARCH64_A64_SVE_BRK },        /* brkb p0.b, p1/m, p2.b */
        { 0x2558c020u, AARCH64_A64_SVE_PFIRST },     /* pfirst p0.b, p1, p0.b */
        { 0x2599c420u, AARCH64_A64_SVE_PNEXT },      /* pnext p0.s, p1, p0.s */
        { 0x05a0a440u, AARCH64_A64_SVE_LAST },       /* lasta w0, p1, z2.s */
        { 0x05a38440u, AARCH64_A64_SVE_LAST },       /* lastb s0, p1, z2.s */
        { 0x05b0a440u, AARCH64_A64_SVE_LAST },       /* clasta w0, p1, w0, z2.s */
        { 0x05ab8440u, AARCH64_A64_SVE_LAST },       /* clastb s0, p1, s0, z2.s */
        { 0x25034440u, AARCH64_A64_SVE_PRED_LOGICAL }, /* and p0.b */
        { 0x25834440u, AARCH64_A64_SVE_PRED_LOGICAL }, /* orr p0.b */
        { 0x25034640u, AARCH64_A64_SVE_PRED_LOGICAL }, /* eor p0.b */
        { 0x25034450u, AARCH64_A64_SVE_PRED_LOGICAL }, /* bic p0.b */
        { 0x25834450u, AARCH64_A64_SVE_PRED_LOGICAL }, /* orn p0.b */
        { 0x25834640u, AARCH64_A64_SVE_PRED_LOGICAL }, /* nor p0.b */
        { 0x25834650u, AARCH64_A64_SVE_PRED_LOGICAL }, /* nand p0.b */
        { 0x25034650u, AARCH64_A64_SVE_PRED_LOGICAL }, /* sel p0.b */
        /* SVE Memory */
        { 0xa5434440u, AARCH64_A64_SVE_LD1_SCALAR }, /* ld1w [x2, x3, lsl #2] */
        { 0xe5434440u, AARCH64_A64_SVE_ST1_SCALAR }, /* st1w [x2, x3, lsl #2] */
        { 0xa520e440u, AARCH64_A64_SVE_LDN },        /* ld2w */
        { 0xe530e440u, AARCH64_A64_SVE_STN },        /* st2w */
        { 0x8541c440u, AARCH64_A64_SVE_LD1R },       /* ld1rw */
        { 0xa5012440u, AARCH64_A64_SVE_LD1RQ },      /* ld1rqw */
        { 0x85234440u, AARCH64_A64_SVE_GATHER_LD },  /* gather ld1w */
        { 0xe5638440u, AARCH64_A64_SVE_SCATTER_ST }, /* scatter st1w */
        { 0xa501e440u, AARCH64_A64_SVE_LDNT1 },      /* ldnt1w */
        { 0xe511e440u, AARCH64_A64_SVE_STNT1 },      /* stnt1w */
        /* SVE FP */
        { 0x65a30440u, AARCH64_A64_SVE_FMLA },       /* fmla */
        { 0x65a32440u, AARCH64_A64_SVE_FMLA },       /* fmls */
        { 0x65a38440u, AARCH64_A64_SVE_FMLA },       /* fmad */
        { 0x65858440u, AARCH64_A64_SVE_FMINMAX },    /* fminnm */
        { 0x65848440u, AARCH64_A64_SVE_FMINMAX },    /* fmaxnm */
        { 0x65878440u, AARCH64_A64_SVE_FMINMAX },    /* fmin */
        { 0x65868440u, AARCH64_A64_SVE_FMINMAX },    /* fmax */
        { 0x65836440u, AARCH64_A64_SVE_FCMP },       /* fcmeq */
        { 0x65834450u, AARCH64_A64_SVE_FCMP },       /* fcmgt */
        { 0x65902440u, AARCH64_A64_SVE_FCMP },       /* fcmge #0.0 */
        { 0x6583c440u, AARCH64_A64_SVE_FCMP },       /* fcmuo */
        { 0x65caa440u, AARCH64_A64_SVE_FCVT },       /* fcvt .s <- .d */
        { 0x659ca440u, AARCH64_A64_SVE_FCVT },       /* fcvtzs .s */
        { 0x6594a440u, AARCH64_A64_SVE_FCVT },       /* scvtf .s */
        { 0x658da440u, AARCH64_A64_SVE_FP_MATH },    /* fsqrt */
        { 0x658e3040u, AARCH64_A64_SVE_FP_MATH },    /* frecpe */
        { 0x658f3040u, AARCH64_A64_SVE_FP_MATH },    /* frsqrte */
        { 0x65831840u, AARCH64_A64_SVE_FP_MATH },    /* frecps */
        { 0x65831c40u, AARCH64_A64_SVE_FP_MATH },    /* frsqrts */
        { 0x65802440u, AARCH64_A64_SVE_FREDUCE },    /* faddv */
        { 0x65982440u, AARCH64_A64_SVE_FREDUCE },    /* fadda */
        { 0x65842440u, AARCH64_A64_SVE_FREDUCE },    /* fmaxnmv */
        { 0x65862440u, AARCH64_A64_SVE_FREDUCE },    /* fmaxv */
        /* SVE Integer */
        { 0x04812440u, AARCH64_A64_SVE_IREDUCE },    /* uaddv */
        { 0x04802440u, AARCH64_A64_SVE_IREDUCE },    /* saddv */
        { 0x04882440u, AARCH64_A64_SVE_IREDUCE },    /* smaxv */
        { 0x048b2440u, AARCH64_A64_SVE_IREDUCE },    /* uminv */
        { 0x04982440u, AARCH64_A64_SVE_IREDUCE },    /* orv */
        { 0x04992440u, AARCH64_A64_SVE_IREDUCE },    /* eorv */
        { 0x049a2440u, AARCH64_A64_SVE_IREDUCE },    /* andv */
        { 0x04a21020u, AARCH64_A64_SVE_FIXED_INT },  /* sqadd */
        { 0x04a21c20u, AARCH64_A64_SVE_FIXED_INT },  /* uqsub */
        { 0x44908440u, AARCH64_A64_SVE_FIXED_INT },  /* shadd */
        { 0x44948440u, AARCH64_A64_SVE_FIXED_INT },  /* srhadd */
        { 0x44928440u, AARCH64_A64_SVE_FIXED_INT },  /* shsub */
        { 0x04639c20u, AARCH64_A64_SVE_SHIFT_IMM },  /* lsl #3 */
        { 0x044087c0u, AARCH64_A64_SVE_SHIFT_IMM },  /* asr #2 */
        { 0x044487c0u, AARCH64_A64_SVE_SHIFT_IMM },  /* asrd #2 */
        { 0x0499a440u, AARCH64_A64_SVE_INT_UNARY },  /* clz */
        { 0x0498a440u, AARCH64_A64_SVE_INT_UNARY },  /* cls */
        { 0x049aa440u, AARCH64_A64_SVE_INT_UNARY },  /* cnt */
        { 0x05a78440u, AARCH64_A64_SVE_INT_UNARY },  /* rbit */
        { 0x0496a440u, AARCH64_A64_SVE_INT_UNARY },  /* abs */
        { 0x0497a440u, AARCH64_A64_SVE_INT_UNARY },  /* neg */
        { 0x049ba440u, AARCH64_A64_SVE_INT_UNARY },  /* cnot */
        { 0x0490a440u, AARCH64_A64_SVE_INT_UNARY },  /* sxtb */
        { 0x05b83820u, AARCH64_A64_SVE_REV },        /* rev */
        { 0x05a48440u, AARCH64_A64_SVE_REV },        /* revb */
        { 0x05a58440u, AARCH64_A64_SVE_REV },        /* revh */
        { 0x05e68440u, AARCH64_A64_SVE_REV },        /* revw */
        { 0x05a23020u, AARCH64_A64_SVE_PERMUTE },    /* tbl */
        { 0x05a22c20u, AARCH64_A64_SVE_PERMUTE },    /* tbx */
        { 0x05a26020u, AARCH64_A64_SVE_PERMUTE },    /* zip1 */
        { 0x05a26820u, AARCH64_A64_SVE_PERMUTE },    /* uzp1 */
        { 0x05a27020u, AARCH64_A64_SVE_PERMUTE },    /* trn1 */
        { 0x05ac8440u, AARCH64_A64_SVE_PERMUTE },    /* splice */
        { 0x05201040u, AARCH64_A64_SVE_PERMUTE },    /* ext */
        { 0x05a43820u, AARCH64_A64_SVE_PERMUTE },    /* insr */
        { 0x04a24020u, AARCH64_A64_SVE_INDEX },      /* index */
        { 0x45c27020u, AARCH64_A64_SVE_WMUL },       /* smullb */
        { 0x04834440u, AARCH64_A64_SVE_WMUL },       /* mla */
        { 0x058000e0u, AARCH64_A64_SVE_IMM_BIN },    /* and #0xff */
        { 0x05000020u, AARCH64_A64_SVE_IMM_BIN },    /* orr #0x3 */
        { 0x25a0c0a0u, AARCH64_A64_SVE_IMM_BIN },    /* add #5 */
        { 0x25a3c140u, AARCH64_A64_SVE_IMM_BIN },    /* subr #10 */
        { 0x2483a440u, AARCH64_A64_SVE_ICMP },       /* cmpeq */
        { 0x25830450u, AARCH64_A64_SVE_ICMP },       /* cmpgt #3 */
        { 0x05a3c440u, AARCH64_A64_SVE_SEL },        /* sel */
        { 0x04940440u, AARCH64_A64_SVE_IDIV },       /* sdiv */
        { 0x04950440u, AARCH64_A64_SVE_IDIV },       /* udiv */
    };
    A64SVEIR1Instruction out;

    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        CHECK(decode(cases[k].word, &out) == A64_SVE_DECODE_MATCH);
        CHECK(out.insn_type == cases[k].want_type);
    }
}

int main(void)
{
    A64SVEIR1Instruction insn;
    A64SVEDecodeStatus status;
    static const uint32_t unsupported_prefix_words[] = {
        0x2518e3f0u, 0x04c02000u, 0xa4000000u, 0xe4000000u
    };
    unsigned i;
    test_sve_feature_state();
    test_expanded_sve_decode();

    /* DDI 0487 M.d C8.2 encodings assembled with llvm-mc -mattr=+sve. */
    status = decode(0x2519e3e0u, &insn); /* PTRUES p0.b, all */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_PTRUE);
    CHECK(insn.arg.f_sve_ptrue.setflags);
    CHECK(insn.arg.f_sve_ptrue.pattern == 31);

    status = decode(0x25d9e040u, &insn); /* PTRUES p0.d, vl2 */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_ptrue.setflags);
    CHECK(insn.arg.f_sve_ptrue.pattern == 2);
    CHECK(insn.arg.f_sve_ptrue.element_bits == 64);

    status = decode(0x2518e3e0u, &insn); /* PTRUE p0.b, all */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_PTRUE);
    CHECK(!insn.arg.f_sve_ptrue.setflags);
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

    status = decode(0x04080020u, &insn); /* SMAX z0.b,p0/m,z0.b,z1.b */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_SMAX);
    CHECK(insn.arg.f_sve_smax.zd == 0);
    CHECK(insn.arg.f_sve_smax.zm == 1);
    CHECK(insn.arg.f_sve_smax.pg == 0);
    CHECK(insn.arg.f_sve_smax.element_bits == 8);
    CHECK(insn.arg.f_sve_smax.is_signed);

    status = decode(0x044a0462u, &insn); /* SMIN z2.h,p1/m,z2.h,z3.h */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_SMIN);
    CHECK(insn.arg.f_sve_smin.zd == 2);
    CHECK(insn.arg.f_sve_smin.zm == 3);
    CHECK(insn.arg.f_sve_smin.pg == 1);
    CHECK(insn.arg.f_sve_smin.element_bits == 16);
    CHECK(insn.arg.f_sve_smin.is_signed);

    status = decode(0x048908a4u, &insn); /* UMAX z4.s,p2/m,z4.s,z5.s */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_UMAX);
    CHECK(insn.arg.f_sve_umax.zd == 4);
    CHECK(insn.arg.f_sve_umax.zm == 5);
    CHECK(insn.arg.f_sve_umax.element_bits == 32);
    CHECK(!insn.arg.f_sve_umax.is_signed);

    status = decode(0x04cb0ce6u, &insn); /* UMIN z6.d,p3/m,z6.d,z7.d */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_UMIN);
    CHECK(insn.arg.f_sve_umin.zd == 6);
    CHECK(insn.arg.f_sve_umin.zm == 7);
    CHECK(insn.arg.f_sve_umin.element_bits == 64);
    CHECK(!insn.arg.f_sve_umin.is_signed);

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

    /* FP predicated arithmetic: encodings from llvm-mc -mattr=+sve. */
    status = decode(0x65808020u, &insn); /* FADD z0.s,p0/m,z0.s,z1.s */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_FADD);
    CHECK(insn.arg.f_sve_fadd.zd == 0);
    CHECK(insn.arg.f_sve_fadd.zm == 1);
    CHECK(insn.arg.f_sve_fadd.pg == 0);
    CHECK(insn.arg.f_sve_fadd.element_bits == 32);
    CHECK(insn.arg.f_sve_fadd.effects.inactive_policy == PVIR_INACTIVE_MERGE);

    status = decode(0x65c08d25u, &insn); /* FADD z5.d,p3/m,z5.d,z9.d */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_FADD);
    CHECK(insn.arg.f_sve_fadd.zd == 5);
    CHECK(insn.arg.f_sve_fadd.zm == 9);
    CHECK(insn.arg.f_sve_fadd.pg == 3);
    CHECK(insn.arg.f_sve_fadd.element_bits == 64);

    status = decode(0x65c18462u, &insn); /* FSUB z2.d,p1/m,z2.d,z3.d */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_FSUB);
    CHECK(insn.arg.f_sve_fsub.zd == 2);
    CHECK(insn.arg.f_sve_fsub.zm == 3);
    CHECK(insn.arg.f_sve_fsub.pg == 1);
    CHECK(insn.arg.f_sve_fsub.element_bits == 64);

    status = decode(0x65828020u, &insn); /* FMUL z0.s,p0/m,z0.s,z1.s */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_FMUL);
    CHECK(insn.arg.f_sve_fmul.element_bits == 32);

    status = decode(0x658d88e6u, &insn); /* FDIV z6.s,p2/m,z6.s,z7.s */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_FDIV);
    CHECK(insn.arg.f_sve_fdiv.zd == 6);
    CHECK(insn.arg.f_sve_fdiv.zm == 7);
    CHECK(insn.arg.f_sve_fdiv.pg == 2);
    CHECK(insn.arg.f_sve_fdiv.element_bits == 32);

    /* .H FP has no LASX lowering: it must fail closed, not match. */
    status = decode(0x654288a4u, &insn); /* FMUL z4.h,p2/m,z4.h,z5.h */
    CHECK(status == A64_SVE_DECODE_UNSUPPORTED_ENCODING);
    CHECK(insn.insn_type == AARCH64_A64_SVE_UNSUPPORTED_ENCODING);
    /* FMAX is now supported; unmodelled 0x65 subfamilies (e.g. FRINTA) remain fail-closed. */
    status = decode(0x65868128u, &insn); /* FMAX z8.s,p0/m,z8.s,z9.s */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_FMINMAX);
    status = decode(0x6584a020u, &insn); /* FRINTA z0.s,p0/m,z1.s */
    CHECK(status == A64_SVE_DECODE_UNSUPPORTED_ENCODING);

    /* FABS/FNEG (predicated): pure sign-bit ops, llvm-mc words. */
    status = decode(0x049ca020u, &insn); /* FABS z0.s,p0/m,z1.s */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_FABS);
    CHECK(insn.arg.f_sve_fabs.zd == 0);
    CHECK(insn.arg.f_sve_fabs.zm == 1);
    CHECK(insn.arg.f_sve_fabs.pg == 0);
    CHECK(insn.arg.f_sve_fabs.element_bits == 32);
    CHECK(insn.arg.f_sve_fabs.effects.inactive_policy == PVIR_INACTIVE_MERGE);

    status = decode(0x04dcb8e5u, &insn); /* FABS z5.d,p6/m,z7.d */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_FABS);
    CHECK(insn.arg.f_sve_fabs.zd == 5);
    CHECK(insn.arg.f_sve_fabs.zm == 7);
    CHECK(insn.arg.f_sve_fabs.pg == 6);
    CHECK(insn.arg.f_sve_fabs.element_bits == 64);

    status = decode(0x049da020u, &insn); /* FNEG z0.s,p0/m,z1.s */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_FNEG);
    CHECK(insn.arg.f_sve_fneg.element_bits == 32);

    status = decode(0x04dda020u, &insn); /* FNEG z0.d,p0/m,z1.d */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_FNEG);
    CHECK(insn.arg.f_sve_fneg.element_bits == 64);

    /* .H has no LASX lowering and must not be claimed. */
    status = decode(0x045ca020u, &insn); /* FABS z0.h,p0/m,z1.h */
    CHECK(status == A64_SVE_DECODE_UNSUPPORTED_ENCODING);

    /* UADDV is now supported; unallocated SADDV .D must fail closed. */
    status = decode(0x04012020u, &insn); /* UADDV d0,p0,z1.b */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_IREDUCE);
    status = decode(0x04c02020u, &insn); /* unallocated SADDV .D */
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

    /* Wider contiguous memory element sizes, llvm-mc words. */
    status = decode(0xa4a0a000u, &insn); /* LD1H {z0.h},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_LD1H);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 16);
    CHECK(insn.arg.f_sve_contiguous.element_bits == 16);
    CHECK(insn.arg.f_sve_contiguous.effects.inactive_policy ==
          PVIR_INACTIVE_ZERO);

    status = decode(0xa540a000u, &insn); /* LD1W {z0.s},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_LD1W);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 32);
    CHECK(insn.arg.f_sve_contiguous.element_bits == 32);

    status = decode(0xa5e0a000u, &insn); /* LD1D {z0.d},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_LD1D);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 64);
    CHECK(insn.arg.f_sve_contiguous.element_bits == 64);

    status = decode(0xe4a0e000u, &insn); /* ST1H {z0.h},p0,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_ST1H);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 16);

    status = decode(0xe540e000u, &insn); /* ST1W {z0.s},p0,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_ST1W);
    CHECK(insn.arg.f_sve_contiguous.effects.inactive_policy ==
          PVIR_INACTIVE_NOT_APPLICABLE);

    status = decode(0xe5e0e000u, &insn); /* ST1D {z0.d},p0,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_ST1D);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 64);

    /* DUP/MOV scalar to vector: 0x05203820/0x05603820/0x05a03820/0x05e03820. */
    status = decode(0x05203820u, &insn); /* MOV z0.b, w1 */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_DUP);
    CHECK(insn.arg.f_sve_dup.zd == 0);
    CHECK(insn.arg.f_sve_dup.rn == 1);
    CHECK(insn.arg.f_sve_dup.element_bits == 8);

    status = decode(0x05e03800u, &insn); /* MOV z0.d, x0 */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_dup.rn == 0);
    CHECK(insn.arg.f_sve_dup.element_bits == 64);

    /* NOT (predicated): llvm-mc 0x041ea020/0x045ea020/0x049ea020/0x04dea020. */
    status = decode(0x041ea020u, &insn); /* NOT z0.b,p0/m,z1.b */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_NOT);
    CHECK(insn.arg.f_sve_not.element_bits == 8);
    CHECK(insn.arg.f_sve_not.effects.inactive_policy == PVIR_INACTIVE_MERGE);

    status = decode(0x049ea020u, &insn); /* NOT z0.s,p0/m,z1.s */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_not.element_bits == 32);

    status = decode(0x04dea020u, &insn); /* NOT z0.d,p0/m,z1.d */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_not.element_bits == 64);

    /* Sign-extending loads: widths come from the mnemonic, not a field. */
    status = decode(0xa5c0a000u, &insn); /* LD1SB {z0.h},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_LD1SB);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 8);
    CHECK(insn.arg.f_sve_contiguous.element_bits == 16);
    CHECK(insn.arg.f_sve_contiguous.sign_extend);

    status = decode(0xa5a0a000u, &insn); /* LD1SB {z0.s},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 8);
    CHECK(insn.arg.f_sve_contiguous.element_bits == 32);

    status = decode(0xa580a000u, &insn); /* LD1SB {z0.d},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 8);
    CHECK(insn.arg.f_sve_contiguous.element_bits == 64);

    status = decode(0xa520a000u, &insn); /* LD1SH {z0.s},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_LD1SH);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 16);
    CHECK(insn.arg.f_sve_contiguous.element_bits == 32);

    status = decode(0xa500a000u, &insn); /* LD1SH {z0.d},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 16);
    CHECK(insn.arg.f_sve_contiguous.element_bits == 64);

    status = decode(0xa480a000u, &insn); /* LD1SW {z0.d},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.insn_type == AARCH64_A64_SVE_LD1SW);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 32);
    CHECK(insn.arg.f_sve_contiguous.element_bits == 64);

    /* Zero-extending widening loads are supported with explicit widths. */
    status = decode(0xa560a000u, &insn); /* LD1W {z0.d},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 32);
    CHECK(insn.arg.f_sve_contiguous.element_bits == 64);
    CHECK(!insn.arg.f_sve_contiguous.sign_extend);

    status = decode(0xa420a000u, &insn); /* LD1B {z0.h},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 8);
    CHECK(insn.arg.f_sve_contiguous.element_bits == 16);

    status = decode(0xa460a000u, &insn); /* LD1B {z0.d},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 8);
    CHECK(insn.arg.f_sve_contiguous.element_bits == 64);

    status = decode(0xa4e0a000u, &insn); /* LD1H {z0.d},p0/z,[x0] */
    CHECK(status == A64_SVE_DECODE_MATCH);
    CHECK(insn.arg.f_sve_contiguous.memory_bits == 16);
    CHECK(insn.arg.f_sve_contiguous.element_bits == 64);

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
