#include "loongemu/loongbt.h"
#include "loongemu/loongbt_ir2_opcodes.h"

#include <stdio.h>

static int failures;
static int checks;

#define CHECK(cond)                                                      \
    do {                                                                 \
        checks++;                                                        \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            failures++;                                                  \
        }                                                                \
    } while (0)

#define EF(st) ((st).eflags)

static LoongbtState fresh(void)
{
    LoongbtState st = { .scr = { 0 }, .eflags = 0, .top = 0, .tm = false };
    return st;
}

uint64_t loongbt_exec_soft(LoongbtState *st, LoongbtOp op, LoongbtWidth width,
                           uint64_t a, uint64_t b);

static void test_add(void)
{
    LoongbtState st = fresh();
    uint64_t r;

    r = loongbt_exec_soft(&st, LOONGBT_ADD, LOONGBT_W, 0x7fffffffu, 1);
    CHECK(r == 0x80000000u);
    CHECK(EF(st) & LOONGBT_EF_OF);
    CHECK(EF(st) & LOONGBT_EF_SF);
    CHECK(EF(st) & LOONGBT_EF_AF);
    CHECK(EF(st) & LOONGBT_EF_PF);
    CHECK(!(EF(st) & LOONGBT_EF_CF));
    CHECK(!(EF(st) & LOONGBT_EF_ZF));

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_ADD, LOONGBT_W, 0xffffffffu, 1);
    CHECK(r == 0);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(EF(st) & LOONGBT_EF_ZF);
    CHECK(EF(st) & LOONGBT_EF_AF);
    CHECK(!(EF(st) & LOONGBT_EF_OF));

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_ADD, LOONGBT_B, 0x7f, 1);
    CHECK(r == 0x80);
    CHECK(EF(st) & LOONGBT_EF_OF);

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_ADD, LOONGBT_H, 0xffff, 1);
    CHECK(r == 0);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(EF(st) & LOONGBT_EF_ZF);

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_ADD, LOONGBT_D, UINT64_MAX, 1);
    CHECK(r == 0);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(EF(st) & LOONGBT_EF_ZF);
}

static void test_adc_sbc(void)
{
    LoongbtState st = fresh();
    uint64_t r;

    st.eflags = LOONGBT_EF_CF;
    r = loongbt_exec_soft(&st, LOONGBT_ADC, LOONGBT_W, 0xffffffffu, 0);
    CHECK(r == 0);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(EF(st) & LOONGBT_EF_ZF);

    st = fresh();
    st.eflags = LOONGBT_EF_CF;
    r = loongbt_exec_soft(&st, LOONGBT_SBC, LOONGBT_W, 5, 2);
    CHECK(r == 2);
    CHECK(!(EF(st) & LOONGBT_EF_CF));

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_SUB, LOONGBT_W, 0, 1);
    CHECK(r == 0xffffffffu);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(EF(st) & LOONGBT_EF_SF);
    CHECK(EF(st) & LOONGBT_EF_AF);
    CHECK(EF(st) & LOONGBT_EF_PF);
    CHECK(!(EF(st) & LOONGBT_EF_OF));
}

static void test_logic_incdec(void)
{
    LoongbtState st = fresh();
    uint64_t r;

    r = loongbt_exec_soft(&st, LOONGBT_AND, LOONGBT_W, 0xf0f0, 0x0f0f);
    CHECK(r == 0);
    CHECK(EF(st) & LOONGBT_EF_ZF);
    CHECK(EF(st) & LOONGBT_EF_PF);
    CHECK(!(EF(st) & (LOONGBT_EF_CF | LOONGBT_EF_OF | LOONGBT_EF_AF)));

    st = fresh();
    st.eflags = LOONGBT_EF_CF;
    r = loongbt_exec_soft(&st, LOONGBT_INC, LOONGBT_W, 0x7fffffffu, 0);
    CHECK(r == 0x80000000u);
    CHECK(EF(st) & LOONGBT_EF_OF);
    CHECK(EF(st) & LOONGBT_EF_CF); /* preserved */

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_DEC, LOONGBT_W, 0, 0);
    CHECK(r == 0xffffffffu);
    CHECK(EF(st) & LOONGBT_EF_SF);
    CHECK(EF(st) & LOONGBT_EF_AF);
    CHECK(!(EF(st) & LOONGBT_EF_CF)); /* preserved as 0 */
    CHECK(!(EF(st) & LOONGBT_EF_OF));
}

static void test_mul(void)
{
    LoongbtState st = fresh();
    uint64_t r;

    r = loongbt_exec_soft(&st, LOONGBT_MULU, LOONGBT_W, 0x10000, 0x10000);
    CHECK(r == 0);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(EF(st) & LOONGBT_EF_OF);
    CHECK(!(EF(st) & (LOONGBT_EF_ZF | LOONGBT_EF_SF | LOONGBT_EF_PF | LOONGBT_EF_AF)));

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_MUL, LOONGBT_W, 0xffffffffu, 1);
    CHECK(r == 0xffffffffu);
    CHECK(!(EF(st) & (LOONGBT_EF_CF | LOONGBT_EF_OF)));

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_MUL, LOONGBT_W, 0x7fffffffu, 2);
    CHECK(r == 0xfffffffeu);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(EF(st) & LOONGBT_EF_OF);
}

static void test_rotate_shift(void)
{
    LoongbtState st = fresh();
    uint64_t r;

    r = loongbt_exec_soft(&st, LOONGBT_ROTL, LOONGBT_W, 0x80000001u, 1);
    CHECK(r == 3);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(EF(st) & LOONGBT_EF_OF);

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_ROTR, LOONGBT_W, 1, 1);
    CHECK(r == 0x80000000u);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(EF(st) & LOONGBT_EF_OF);

    st = fresh();
    st.eflags = LOONGBT_EF_CF;
    r = loongbt_exec_soft(&st, LOONGBT_ROTRI, LOONGBT_W, 0x12345678u, 0);
    CHECK(r == 0x12345678u);
    CHECK(EF(st) == LOONGBT_EF_CF); /* count 0 leaves flags alone */

    st = fresh(); /* RCL: 33-bit ring, CF feeds bit 32 */
    r = loongbt_exec_soft(&st, LOONGBT_RCL, LOONGBT_W, 0x80000000u, 1);
    CHECK(r == 0);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(EF(st) & LOONGBT_EF_OF);

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_SLL, LOONGBT_W, 0x80000000u, 1);
    CHECK(r == 0);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(EF(st) & LOONGBT_EF_OF);
    CHECK(EF(st) & LOONGBT_EF_ZF);

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_SRL, LOONGBT_W, 1, 1);
    CHECK(r == 0);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(!(EF(st) & LOONGBT_EF_OF));

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_SRA, LOONGBT_W, 0x80000000u, 1);
    CHECK(r == 0xc0000000u);
    CHECK(!(EF(st) & LOONGBT_EF_CF));
    CHECK(!(EF(st) & LOONGBT_EF_OF));
}

static void test_flag_moves(void)
{
    LoongbtState st = fresh();
    uint64_t packed;

    st.eflags = LOONGBT_EF_CF | LOONGBT_EF_ZF | LOONGBT_EF_OF;
    packed = loongbt_exec_soft(&st, LOONGBT_MFFLAG, LOONGBT_W, 0, 63);
    CHECK(packed == (1u | (1u << 6) | (1u << 11)));

    st = fresh();
    loongbt_exec_soft(&st, LOONGBT_MTFLAG, LOONGBT_W, packed, 63);
    CHECK(st.eflags == (LOONGBT_EF_CF | LOONGBT_EF_ZF | LOONGBT_EF_OF));

    st = fresh();
    loongbt_exec_soft(&st, LOONGBT_MTFLAG, LOONGBT_W, 0xffffffffu, 0x01);
    CHECK(st.eflags == LOONGBT_EF_CF); /* only CF selected */
}

static void test_conditions(void)
{
    LoongbtState st = fresh();

    st.eflags = LOONGBT_EF_ZF;
    CHECK(loongbt_exec_soft(&st, LOONGBT_SETX86J, LOONGBT_W, 0, 4) == 1); /* E  */
    CHECK(loongbt_exec_soft(&st, LOONGBT_SETX86J, LOONGBT_W, 0, 5) == 0); /* NE */
    CHECK(loongbt_exec_soft(&st, LOONGBT_SETX86J, LOONGBT_W, 0, 0) == 0); /* A  */
    CHECK(loongbt_exec_soft(&st, LOONGBT_SETX86J, LOONGBT_W, 0, 3) == 1); /* BE */
    CHECK(loongbt_exec_soft(&st, LOONGBT_SETX86J, LOONGBT_W, 0, 14) == 0); /* P: PF=0 */

    st.eflags = LOONGBT_EF_SF;
    CHECK(loongbt_exec_soft(&st, LOONGBT_SETX86J, LOONGBT_W, 0, 8) == 1);  /* L  */
    CHECK(loongbt_exec_soft(&st, LOONGBT_SETX86J, LOONGBT_W, 0, 7) == 0);  /* GE */

    st.eflags = LOONGBT_EF_ZF;
    CHECK(loongbt_exec_soft(&st, LOONGBT_LOOPE, LOONGBT_W, 1, 0) == 1);
    CHECK(loongbt_exec_soft(&st, LOONGBT_LOOPNE, LOONGBT_W, 1, 0) == 0);
    CHECK(loongbt_exec_soft(&st, LOONGBT_LOOPE, LOONGBT_W, 0, 0) == 0);
}

static void test_top_tm(void)
{
    LoongbtState st = fresh();

    loongbt_exec_soft(&st, LOONGBT_MTTOP, LOONGBT_W, 0, 5);
    CHECK(st.top == 5);
    CHECK(loongbt_exec_soft(&st, LOONGBT_MFTOP, LOONGBT_W, 0, 0) == 5);
    loongbt_exec_soft(&st, LOONGBT_INCTOP, LOONGBT_W, 0, 0);
    CHECK(st.top == 6);
    loongbt_exec_soft(&st, LOONGBT_INCTOP, LOONGBT_W, 0, 0);
    CHECK(st.top == 7);
    loongbt_exec_soft(&st, LOONGBT_INCTOP, LOONGBT_W, 0, 0);
    CHECK(st.top == 0); /* wraps */
    loongbt_exec_soft(&st, LOONGBT_DECTOP, LOONGBT_W, 0, 0);
    CHECK(st.top == 7);

    loongbt_exec_soft(&st, LOONGBT_SETTM, LOONGBT_W, 0, 0);
    CHECK(st.tm);
    loongbt_exec_soft(&st, LOONGBT_CLRTM, LOONGBT_W, 0, 0);
    CHECK(!st.tm);
}

static void test_unsigned_variants(void)
{
    LoongbtState st = fresh();
    uint64_t r;

    r = loongbt_exec_soft(&st, LOONGBT_ADDU, LOONGBT_W, 0xffffffffu, 1);
    CHECK(r == 0);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(!(EF(st) & LOONGBT_EF_OF)); /* x86add.wu forces OF=0 */
    CHECK(EF(st) & LOONGBT_EF_ZF);

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_SUBU, LOONGBT_W, 0, 1);
    CHECK(r == 0xffffffffu);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(!(EF(st) & LOONGBT_EF_OF)); /* x86sub.wu forces OF=0 */
    CHECK(EF(st) & LOONGBT_EF_SF);
}

static void test_ir2_mapping(void)
{
    LoongbtState st = fresh(), ref = fresh();
    LoongbtOp op;
    LoongbtWidth w;
    bool ok = false;
    uint64_t via_ir2, via_soft;

    CHECK(loongbt_op_from_ir2(LISA_X86ADD_W, &op, &w));
    CHECK(op == LOONGBT_ADD && w == LOONGBT_W);
    CHECK(loongbt_op_from_ir2(LISA_X86ADD_WU, &op, &w));
    CHECK(op == LOONGBT_ADDU && w == LOONGBT_W);
    CHECK(loongbt_op_from_ir2(LISA_X86SUB_DU, &op, &w));
    CHECK(op == LOONGBT_SUBU && w == LOONGBT_D);
    CHECK(loongbt_op_from_ir2(LISA_X86MUL_WU, &op, &w));
    CHECK(op == LOONGBT_MULU && w == LOONGBT_W);
    CHECK(loongbt_op_from_ir2(LISA_X86ROTRI_B, &op, &w));
    CHECK(op == LOONGBT_ROTRI && w == LOONGBT_B);
    CHECK(loongbt_op_from_ir2(LISA_X86MTFLAG, &op, &w));
    CHECK(op == LOONGBT_MTFLAG);
    CHECK(loongbt_op_from_ir2(LISA_SETX86J, &op, &w));
    CHECK(op == LOONGBT_SETX86J);
    CHECK(!loongbt_op_from_ir2(LISA_X86SETTAG, &op, &w)); /* not modelled yet */
    CHECK(!loongbt_op_from_ir2(LISA_X86_INST, &op, &w));  /* marker */
    CHECK(!loongbt_op_from_ir2(LISA_MOV64, &op, &w));

    via_ir2 = loongbt_exec_ir2(&st, LISA_X86SBC_W, 5, 2, &ok);
    CHECK(ok);
    via_soft = loongbt_exec_soft(&ref, LOONGBT_SBC, LOONGBT_W, 5, 2);
    CHECK(via_ir2 == via_soft);
    CHECK(st.eflags == ref.eflags);

    via_ir2 = loongbt_exec_ir2(&st, LISA_X86SETTAG, 0, 0, &ok);
    CHECK(!ok);
    CHECK(via_ir2 == 0);
}

static void test_logic_or_xor(void)
{
    LoongbtState st = fresh();
    uint64_t r;

    /* OR clears CF and OF, and derives ZF/SF/PF/AF from the result. */
    st.eflags = LOONGBT_EF_CF | LOONGBT_EF_OF;
    r = loongbt_exec_soft(&st, LOONGBT_OR, LOONGBT_W, 0xf0f0, 0x0f0f);
    CHECK(r == 0xffffu);
    CHECK(!(EF(st) & (LOONGBT_EF_CF | LOONGBT_EF_OF | LOONGBT_EF_AF)));
    CHECK(!(EF(st) & LOONGBT_EF_ZF));
    CHECK(!(EF(st) & LOONGBT_EF_SF));

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_OR, LOONGBT_W, 0, 0);
    CHECK(r == 0);
    CHECK(EF(st) & LOONGBT_EF_ZF);
    CHECK(EF(st) & LOONGBT_EF_PF);
    CHECK(!(EF(st) & (LOONGBT_EF_CF | LOONGBT_EF_OF | LOONGBT_EF_AF)));

    /* The sign bit of the result decides SF, not the inputs. */
    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_OR, LOONGBT_B, 0x80, 0x01);
    CHECK(r == 0x81);
    CHECK(EF(st) & LOONGBT_EF_SF);
    CHECK(!(EF(st) & LOONGBT_EF_ZF));

    st = fresh();
    st.eflags = LOONGBT_EF_CF | LOONGBT_EF_OF;
    r = loongbt_exec_soft(&st, LOONGBT_XOR, LOONGBT_W, 0xffffffffu, 0xffffffffu);
    CHECK(r == 0);
    CHECK(EF(st) & LOONGBT_EF_ZF);
    CHECK(EF(st) & LOONGBT_EF_PF);
    CHECK(!(EF(st) & (LOONGBT_EF_CF | LOONGBT_EF_OF | LOONGBT_EF_AF)));
    CHECK(!(EF(st) & LOONGBT_EF_SF));

    /* XOR is its own inverse over the masked width. */
    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_XOR, LOONGBT_H, 0x00ff, 0xff00);
    CHECK(r == 0xffff);
    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_XOR, LOONGBT_H, r, 0xff00);
    CHECK(r == 0x00ff);
}

static void test_rotate_through_carry(void)
{
    LoongbtState st = fresh();
    uint64_t r;

    /*
     * The 33-bit ring shifts the incoming CF into the top bit and lets the
     * old bit 0 fall out through CF; a rotate by one therefore moves the
     * sign bit, which is what sets OF.
     */
    st.eflags = LOONGBT_EF_CF; /* CF in = 1 */
    r = loongbt_exec_soft(&st, LOONGBT_RCR, LOONGBT_W, 0, 1);
    CHECK(r == 0x80000000u);          /* CF in lands in bit 31 */
    CHECK(!(EF(st) & LOONGBT_EF_CF)); /* old bit 0 was 0 */
    CHECK(EF(st) & LOONGBT_EF_OF);    /* sign bit flipped */

    /* With CF in = 0 the top bit is filled with a zero instead. */
    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_RCR, LOONGBT_W, 1, 1);
    CHECK(r == 0);
    CHECK(EF(st) & LOONGBT_EF_CF); /* old bit 0 was 1 */
    CHECK(!(EF(st) & LOONGBT_EF_OF));

    st = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_RCR, LOONGBT_W, 0x12345678u, 1);
    CHECK(r == 0x091a2b3cu);
    CHECK(!(EF(st) & LOONGBT_EF_CF));

    st = fresh();
    st.eflags = LOONGBT_EF_CF;
    r = loongbt_exec_soft(&st, LOONGBT_RCR, LOONGBT_W, 0x12345678u, 1);
    CHECK(r == 0x891a2b3cu);
    CHECK(!(EF(st) & LOONGBT_EF_CF));

    /*
     * The count is masked to five bits for a 32-bit rotate, so 33 is one
     * rotate, not a full turn; a 33-bit ring cannot be walked in one
     * instruction.  The masked count is what makes the opcode well defined.
     */
    st = fresh();
    st.eflags = LOONGBT_EF_CF;
    r = loongbt_exec_soft(&st, LOONGBT_RCR, LOONGBT_W, 0x12345678u, 33);
    CHECK(r == 0x891a2b3cu);

    st = fresh();
    st.eflags = LOONGBT_EF_CF;
    r = loongbt_exec_soft(&st, LOONGBT_RCL, LOONGBT_W, 0x89abcdefu, 1);
    CHECK(r == 0x13579bdfu);
    CHECK(EF(st) & LOONGBT_EF_CF);
    CHECK(EF(st) & LOONGBT_EF_OF);

    st = fresh();
    st.eflags = LOONGBT_EF_CF;
    r = loongbt_exec_soft(&st, LOONGBT_RCL, LOONGBT_W, 0x89abcdefu, 33);
    CHECK(r == 0x13579bdfu); /* same as count 1 after masking */

    /* Count 0 must not disturb anything, including CF. */
    st = fresh();
    st.eflags = LOONGBT_EF_CF | LOONGBT_EF_ZF;
    r = loongbt_exec_soft(&st, LOONGBT_RCR, LOONGBT_W, 0xdeadbeefu, 0);
    CHECK(r == 0xdeadbeefu);
    CHECK(EF(st) == (LOONGBT_EF_CF | LOONGBT_EF_ZF));
}

static void test_immediate_variants(void)
{
    LoongbtState st = fresh(), imm = fresh();
    uint64_t r;

    /*
     * The I forms share the register forms' implementation, but they are
     * separate opcodes; a mis-wired dispatch would silently rotate by the
     * wrong amount.  Compare each pair on the same operands.
     */
    r = loongbt_exec_soft(&st, LOONGBT_ROTL, LOONGBT_W, 0x80000001u, 1);
    CHECK(loongbt_exec_soft(&imm, LOONGBT_ROTLI, LOONGBT_W, 0x80000001u, 1) == r);
    CHECK(imm.eflags == st.eflags);

    st = fresh();
    imm = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_ROTR, LOONGBT_B, 0x81, 1);
    CHECK(loongbt_exec_soft(&imm, LOONGBT_ROTRI, LOONGBT_B, 0x81, 1) == r);
    CHECK(imm.eflags == st.eflags);

    st = fresh();
    imm = fresh();
    st.eflags = LOONGBT_EF_CF;
    imm.eflags = LOONGBT_EF_CF;
    r = loongbt_exec_soft(&st, LOONGBT_RCL, LOONGBT_W, 0x80000000u, 1);
    CHECK(loongbt_exec_soft(&imm, LOONGBT_RCLI, LOONGBT_W, 0x80000000u, 1) == r);
    CHECK(imm.eflags == st.eflags);

    st = fresh();
    imm = fresh();
    st.eflags = LOONGBT_EF_CF;
    imm.eflags = LOONGBT_EF_CF;
    r = loongbt_exec_soft(&st, LOONGBT_RCR, LOONGBT_W, 1, 1);
    CHECK(loongbt_exec_soft(&imm, LOONGBT_RCRI, LOONGBT_W, 1, 1) == r);
    CHECK(imm.eflags == st.eflags);

    st = fresh();
    imm = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_SLL, LOONGBT_W, 0x80000000u, 1);
    CHECK(loongbt_exec_soft(&imm, LOONGBT_SLLI, LOONGBT_W, 0x80000000u, 1) == r);
    CHECK(imm.eflags == st.eflags);

    st = fresh();
    imm = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_SRL, LOONGBT_H, 0x8000, 3);
    CHECK(loongbt_exec_soft(&imm, LOONGBT_SRLI, LOONGBT_H, 0x8000, 3) == r);
    CHECK(imm.eflags == st.eflags);

    st = fresh();
    imm = fresh();
    r = loongbt_exec_soft(&st, LOONGBT_SRA, LOONGBT_W, 0x80000000u, 1);
    CHECK(loongbt_exec_soft(&imm, LOONGBT_SRAI, LOONGBT_W, 0x80000000u, 1) == r);
    CHECK(imm.eflags == st.eflags);

    /* And they must actually compute, not just agree with each other. */
    st = fresh();
    CHECK(loongbt_exec_soft(&st, LOONGBT_ROTLI, LOONGBT_W, 0x80000001u, 1) == 3);
    st = fresh();
    CHECK(loongbt_exec_soft(&st, LOONGBT_SRAI, LOONGBT_W, 0x80000000u, 1) ==
          0xc0000000u);
    st = fresh();
    CHECK(loongbt_exec_soft(&st, LOONGBT_SRLI, LOONGBT_W, 1, 1) == 0);
}

static void test_dispatch(void)
{
    LoongbtState a_state = fresh(), b_state = fresh();
    uint64_t soft, via_dispatch;

    /* Without LBT_X86 in cpu_flag the dispatcher must take the fallback. */
    loongbt_set_cpu_features(0);
    loongbt_set_hw_available(false);
    CHECK(!loongbt_hw_available());

    soft = loongbt_exec_soft(&a_state, LOONGBT_ADD, LOONGBT_W, 0x1234, 0x5678);
    via_dispatch = loongbt_exec(&b_state, LOONGBT_ADD, LOONGBT_W, 0x1234, 0x5678);
    CHECK(soft == via_dispatch);
    CHECK(a_state.eflags == b_state.eflags);

    /* LBT_X86 advertised, but this build has no hardware back end: the probe
     * must report false and the dispatcher must still use the fallback. */
    loongbt_set_cpu_features(LOONGEMU_CPU_LBT_X86);
    loongbt_set_hw_available(true);
    CHECK(!loongbt_hw_probe());
    soft = loongbt_exec_soft(&a_state, LOONGBT_SUB, LOONGBT_W, 5, 2);
    via_dispatch = loongbt_exec(&b_state, LOONGBT_SUB, LOONGBT_W, 5, 2);
    CHECK(soft == via_dispatch);
}

int main(void)
{
    test_add();
    test_adc_sbc();
    test_logic_incdec();
    test_logic_or_xor();
    test_rotate_through_carry();
    test_immediate_variants();
    test_mul();
    test_rotate_shift();
    test_flag_moves();
    test_conditions();
    test_top_tm();
    test_unsigned_variants();
    test_ir2_mapping();
    test_dispatch();

    printf("loongbt: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
