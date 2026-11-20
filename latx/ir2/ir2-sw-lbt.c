/*
 * Translation-time software LBT lowering pass for LoongArch64 hosts without
 * hardware LBT (e.g. 2K2000/2K3000 or QEMU -cpu la464,lbt=off).
 *
 * When option_sw_lbt != 0, tr_lower_sw_lbt() rewrites every LBT IR2
 * instruction in-place into native LoongArch64 integer instructions right
 * before label_dispose(), eliminating per-instruction EXC_IPE traps.
 *
 * State allocation:
 *   - $tp ($r2)           : x86 condition flags (CF=0, PF=2, AF=4, ZF=6, SF=7, OF=11; mask 0x8d5)
 *   - env->mips_regs[28..30] : spill slots for up to 3 scratch GPRs
 *   - env->mips_regs[31]     : x87 FPU TOP (3-bit)
 */
#include "common.h"
#include "lsenv.h"
#include "reg-alloc.h"
#include "ir2.h"
#include "latx-options.h"
#include "translate.h"

#define SW_EF_CF (1u << 0)
#define SW_EF_PF (1u << 2)
#define SW_EF_AF (1u << 4)
#define SW_EF_ZF (1u << 6)
#define SW_EF_SF (1u << 7)
#define SW_EF_OF (1u << 11)
#define SW_EF_ALL (SW_EF_CF | SW_EF_PF | SW_EF_AF | SW_EF_ZF | SW_EF_SF | SW_EF_OF)

static inline IR2_OPND gpr(int r)
{
    return ir2_opnd_new(IR2_OPND_GPR, r);
}

static void emit_opnd1(int anchor_id, IR2_OPCODE op, IR2_OPND op0)
{
    IR2_INST *p = ir2_allocate();
    p->_opcode = op;
    p->op_count = 1;
    p->_opnd[0] = op0;
    ir2_insert_before(p, anchor_id);
}

static void emit_opnd2(int anchor_id, IR2_OPCODE op, IR2_OPND op0, IR2_OPND op1)
{
    IR2_INST *p = ir2_allocate();
    p->_opcode = op;
    p->op_count = 2;
    p->_opnd[0] = op0;
    p->_opnd[1] = op1;
    ir2_insert_before(p, anchor_id);
}

static void emit_opnd3(int anchor_id, IR2_OPCODE op,
                       IR2_OPND op0, IR2_OPND op1, IR2_OPND op2)
{
    IR2_INST *p = ir2_allocate();
    p->_opcode = op;
    p->op_count = 3;
    p->_opnd[0] = op0;
    p->_opnd[1] = op1;
    p->_opnd[2] = op2;
    ir2_insert_before(p, anchor_id);
}

static void emit_opnd2i(int anchor_id, IR2_OPCODE op,
                        IR2_OPND op0, IR2_OPND op1, int imm)
{
    emit_opnd3(anchor_id, op, op0, op1, create_immh_opnd(imm));
}

static void emit_opnd2ii(int anchor_id, IR2_OPCODE op,
                         IR2_OPND op0, IR2_OPND op1, int imm0, int imm1)
{
    IR2_INST *p = ir2_allocate();
    p->_opcode = op;
    p->op_count = 4;
    p->_opnd[0] = op0;
    p->_opnd[1] = op1;
    p->_opnd[2] = create_immh_opnd(imm0);
    p->_opnd[3] = create_immh_opnd(imm1);
    ir2_insert_before(p, anchor_id);
}

/* Pick `count` (1..3) scratch GPRs disjoint from r0, r1, r2. */
static void pick_scratch(int count, int r0, int r1, int r2, IR2_OPND out[3])
{
    static const int pool[6] = { 12, 13, 14, 15, 16, 11 }; /* $t0..$t4, $a7 */
    int n = 0;

    for (int i = 0; i < 6 && n < count; i++) {
        int c = pool[i];
        if (c != r0 && c != r1 && c != r2) {
            out[n++] = gpr(c);
        }
    }
    lsassert(n == count);
}

static void spill_scratch(int anchor_id, int count, const IR2_OPND s[3])
{
    for (int i = 0; i < count; i++) {
        emit_opnd2i(anchor_id, LISA_ST_D, s[i], env_ir2_opnd,
                    lsenv_offset_of_mips_regs(lsenv, 28 + i));
    }
}

static void restore_scratch(int anchor_id, int count, const IR2_OPND s[3])
{
    for (int i = 0; i < count; i++) {
        emit_opnd2i(anchor_id, LISA_LD_D, s[i], env_ir2_opnd,
                    lsenv_offset_of_mips_regs(lsenv, 28 + i));
    }
}

static uint32_t lbt_imm_to_eflags_mask(uint32_t imm)
{
    uint32_t m = 0;
    if (imm & 1u)  m |= SW_EF_CF;
    if (imm & 2u)  m |= SW_EF_PF;
    if (imm & 4u)  m |= SW_EF_AF;
    if (imm & 8u)  m |= SW_EF_ZF;
    if (imm & 16u) m |= SW_EF_SF;
    if (imm & 32u) m |= SW_EF_OF;
    return m;
}

/*
 * Emit SF, ZF, PF from `res` (already zero-extended to `w` bits) into `$tp`,
 * ORing into existing CF/AF/OF in `$tp`. Clobbers `t0`, `t1`; preserves `res`.
 */
static void emit_szp_into_tp(int anchor_id, unsigned w,
                             IR2_OPND res, IR2_OPND t0, IR2_OPND t1)
{
    /* ZF (bit 6): (res == 0) ? 0x40 : 0 */
    emit_opnd2i(anchor_id, LISA_SLTUI, t0, res, 1);
    emit_opnd2i(anchor_id, LISA_SLLI_D, t0, t0, 6);
    emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, t0);

    /* SF (bit 7): ((res >> (w - 1)) & 1) << 7 */
    emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, t0, res, (int)(w - 1), (int)(w - 1));
    emit_opnd2i(anchor_id, LISA_SLLI_D, t0, t0, 7);
    emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, t0);

    /* PF (bit 2): even parity of res[7:0] */
    emit_opnd2i(anchor_id, LISA_SRLI_D, t0, res, 4);
    emit_opnd3(anchor_id, LISA_XOR, t0, t0, res);
    emit_opnd2i(anchor_id, LISA_SRLI_D, t1, t0, 2);
    emit_opnd3(anchor_id, LISA_XOR, t0, t0, t1);
    emit_opnd2i(anchor_id, LISA_SRLI_D, t1, t0, 1);
    emit_opnd3(anchor_id, LISA_XOR, t0, t0, t1);
    emit_opnd2i(anchor_id, LISA_ANDI, t0, t0, 1);
    emit_opnd2i(anchor_id, LISA_XORI, t0, t0, 1);
    emit_opnd2i(anchor_id, LISA_SLLI_D, t0, t0, 2);
    emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, t0);
}

static void emit_sext(int anchor_id, unsigned w, IR2_OPND dst, IR2_OPND src)
{
    if (w == 8) {
        emit_opnd2(anchor_id, LISA_EXT_W_B, dst, src);
    } else if (w == 16) {
        emit_opnd2(anchor_id, LISA_EXT_W_H, dst, src);
    } else if (w == 32) {
        emit_opnd2i(anchor_id, LISA_SLLI_W, dst, src, 0);
    } else if (!ir2_opnd_cmp(&dst, &src)) {
        emit_opnd3(anchor_id, LISA_OR, dst, src, zero_ir2_opnd);
    }
}

static void emit_zext(int anchor_id, unsigned w, IR2_OPND dst, IR2_OPND src)
{
    if (w < 64) {
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, dst, src, (int)(w - 1), 0);
    } else if (!ir2_opnd_cmp(&dst, &src)) {
        emit_opnd3(anchor_id, LISA_OR, dst, src, zero_ir2_opnd);
    }
}

static void lower_logic(int anchor_id, unsigned w, IR2_OPND rj, IR2_OPND rk,
                        IR2_OPCODE alu_op)
{
    IR2_OPND s[3];
    pick_scratch(3, ir2_opnd_base_reg_num(&rj), ir2_opnd_base_reg_num(&rk), -1, s);
    spill_scratch(anchor_id, 3, s);

    emit_opnd3(anchor_id, alu_op, s[2], rj, rk);
    if (w < 64) {
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[2], s[2], (int)(w - 1), 0);
    }
    emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, zero_ir2_opnd, zero_ir2_opnd);
    emit_szp_into_tp(anchor_id, w, s[2], s[0], s[1]);

    restore_scratch(anchor_id, 3, s);
}

static void lower_add_adc(int anchor_id, unsigned w, IR2_OPND rj, IR2_OPND rk,
                          int with_carry, int unsigned_form)
{
    IR2_OPND s[3];
    pick_scratch(3, ir2_opnd_base_reg_num(&rj), ir2_opnd_base_reg_num(&rk), -1, s);
    spill_scratch(anchor_id, 3, s);

    if (w < 64) {
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[0], rj, (int)(w - 1), 0);
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[1], rk, (int)(w - 1), 0);
        emit_opnd3(anchor_id, LISA_ADD_D, s[2], s[0], s[1]);
        if (with_carry) {
            emit_opnd2i(anchor_id, LISA_ANDI, tp_ir2_opnd, tp_ir2_opnd, 1);
            emit_opnd3(anchor_id, LISA_ADD_D, s[2], s[2], tp_ir2_opnd);
        }
        /* CF = (s2 >> w) */
        emit_opnd2i(anchor_id, LISA_SRLI_D, tp_ir2_opnd, s[2], (int)w);
        /* AF = (x ^ y ^ r) & 0x10; keep x^y in s[0] */
        emit_opnd3(anchor_id, LISA_XOR, s[0], s[0], s[1]);
        emit_opnd3(anchor_id, LISA_XOR, s[1], s[0], s[2]);
        emit_opnd2i(anchor_id, LISA_ANDI, s[1], s[1], 0x10);
        emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[1]);
        /* OF = (~(x ^ y) & (x ^ r)) at bit w-1 */
        if (!unsigned_form) {
            emit_opnd3(anchor_id, LISA_XOR, s[1], rj, s[2]);
            emit_opnd3(anchor_id, LISA_ANDN, s[0], s[1], s[0]);
            emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[0], s[0], (int)(w - 1), (int)(w - 1));
            emit_opnd2i(anchor_id, LISA_SLLI_D, s[0], s[0], 11);
            emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[0]);
        }
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[2], s[2], (int)(w - 1), 0);
    } else {
        emit_opnd3(anchor_id, LISA_ADD_D, s[2], rj, rk);
        emit_opnd3(anchor_id, LISA_SLTU, s[0], s[2], rj);
        if (with_carry) {
            emit_opnd2i(anchor_id, LISA_ANDI, s[1], tp_ir2_opnd, 1);
            emit_opnd3(anchor_id, LISA_ADD_D, s[2], s[2], s[1]);
            emit_opnd3(anchor_id, LISA_SLTU, s[1], s[2], s[1]);
            emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, s[0], s[1]);
        } else {
            emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, s[0], zero_ir2_opnd);
        }
        emit_opnd3(anchor_id, LISA_XOR, s[0], rj, rk);
        emit_opnd3(anchor_id, LISA_XOR, s[1], s[0], s[2]);
        emit_opnd2i(anchor_id, LISA_ANDI, s[1], s[1], 0x10);
        emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[1]);
        if (!unsigned_form) {
            emit_opnd3(anchor_id, LISA_XOR, s[1], rj, s[2]);
            emit_opnd3(anchor_id, LISA_ANDN, s[0], s[1], s[0]);
            emit_opnd2i(anchor_id, LISA_SRLI_D, s[0], s[0], 63);
            emit_opnd2i(anchor_id, LISA_SLLI_D, s[0], s[0], 11);
            emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[0]);
        }
    }
    emit_szp_into_tp(anchor_id, w, s[2], s[0], s[1]);

    restore_scratch(anchor_id, 3, s);
}

static void lower_sub_sbc(int anchor_id, unsigned w, IR2_OPND rj, IR2_OPND rk,
                          int with_borrow, int unsigned_form)
{
    IR2_OPND s[3];
    pick_scratch(3, ir2_opnd_base_reg_num(&rj), ir2_opnd_base_reg_num(&rk), -1, s);
    spill_scratch(anchor_id, 3, s);

    if (w < 64) {
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[0], rj, (int)(w - 1), 0);
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[1], rk, (int)(w - 1), 0);
        emit_opnd3(anchor_id, LISA_SUB_D, s[2], s[0], s[1]);
        if (with_borrow) {
            emit_opnd2i(anchor_id, LISA_ANDI, tp_ir2_opnd, tp_ir2_opnd, 1);
            emit_opnd3(anchor_id, LISA_SUB_D, s[2], s[2], tp_ir2_opnd);
        }
        /* For w < 64, x - y - c < 0 iff bit 63 is 1. */
        emit_opnd2i(anchor_id, LISA_SRLI_D, tp_ir2_opnd, s[2], 63);
        /* AF = (x ^ y ^ r) & 0x10; keep x^y in s[0] */
        emit_opnd3(anchor_id, LISA_XOR, s[0], s[0], s[1]);
        emit_opnd3(anchor_id, LISA_XOR, s[1], s[0], s[2]);
        emit_opnd2i(anchor_id, LISA_ANDI, s[1], s[1], 0x10);
        emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[1]);
        /* OF = ((x ^ y) & (x ^ r)) at bit w-1 */
        if (!unsigned_form) {
            emit_opnd3(anchor_id, LISA_XOR, s[1], rj, s[2]);
            emit_opnd3(anchor_id, LISA_AND, s[0], s[0], s[1]);
            emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[0], s[0], (int)(w - 1), (int)(w - 1));
            emit_opnd2i(anchor_id, LISA_SLLI_D, s[0], s[0], 11);
            emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[0]);
        }
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[2], s[2], (int)(w - 1), 0);
    } else {
        if (with_borrow) {
            emit_opnd2i(anchor_id, LISA_ANDI, s[1], tp_ir2_opnd, 1);
            emit_opnd3(anchor_id, LISA_SLTU, s[0], rj, rk);
            emit_opnd3(anchor_id, LISA_SUB_D, s[2], rj, rk);
            emit_opnd3(anchor_id, LISA_SLTU, tp_ir2_opnd, s[2], s[1]);
            emit_opnd3(anchor_id, LISA_SUB_D, s[2], s[2], s[1]);
            emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[0]);
        } else {
            emit_opnd3(anchor_id, LISA_SUB_D, s[2], rj, rk);
            emit_opnd3(anchor_id, LISA_SLTU, tp_ir2_opnd, rj, rk);
        }
        emit_opnd3(anchor_id, LISA_XOR, s[0], rj, rk);
        emit_opnd3(anchor_id, LISA_XOR, s[1], s[0], s[2]);
        emit_opnd2i(anchor_id, LISA_ANDI, s[1], s[1], 0x10);
        emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[1]);
        if (!unsigned_form) {
            emit_opnd3(anchor_id, LISA_XOR, s[1], rj, s[2]);
            emit_opnd3(anchor_id, LISA_AND, s[0], s[0], s[1]);
            emit_opnd2i(anchor_id, LISA_SRLI_D, s[0], s[0], 63);
            emit_opnd2i(anchor_id, LISA_SLLI_D, s[0], s[0], 11);
            emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[0]);
        }
    }
    emit_szp_into_tp(anchor_id, w, s[2], s[0], s[1]);

    restore_scratch(anchor_id, 3, s);
}

static void lower_inc_dec(int anchor_id, unsigned w, IR2_OPND rj, int is_dec)
{
    IR2_OPND s[3];
    pick_scratch(3, ir2_opnd_base_reg_num(&rj), -1, -1, s);
    spill_scratch(anchor_id, 3, s);

    /* Preserve CF only. */
    emit_opnd2i(anchor_id, LISA_ANDI, tp_ir2_opnd, tp_ir2_opnd, 1);
    emit_opnd2i(anchor_id, LISA_ADDI_D, s[2], rj, is_dec ? -1 : 1);
    if (w < 64) {
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[2], s[2], (int)(w - 1), 0);
    }
    /* AF = (x ^ 1 ^ r) & 0x10 */
    emit_opnd2i(anchor_id, LISA_XORI, s[0], rj, 1);
    emit_opnd3(anchor_id, LISA_XOR, s[0], s[0], s[2]);
    emit_opnd2i(anchor_id, LISA_ANDI, s[0], s[0], 0x10);
    emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[0]);
    /* OF: INC -> (~x & r)[w-1], DEC -> (x & ~r)[w-1] */
    if (is_dec) {
        emit_opnd3(anchor_id, LISA_ANDN, s[0], rj, s[2]);
    } else {
        emit_opnd3(anchor_id, LISA_ANDN, s[0], s[2], rj);
    }
    emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[0], s[0], (int)(w - 1), (int)(w - 1));
    emit_opnd2i(anchor_id, LISA_SLLI_D, s[0], s[0], 11);
    emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[0]);

    emit_szp_into_tp(anchor_id, w, s[2], s[0], s[1]);

    restore_scratch(anchor_id, 3, s);
}

static void lower_mul(int anchor_id, unsigned w, IR2_OPND rj, IR2_OPND rk,
                      int is_signed)
{
    IR2_OPND s[3];
    pick_scratch(3, ir2_opnd_base_reg_num(&rj), ir2_opnd_base_reg_num(&rk), -1, s);
    spill_scratch(anchor_id, 3, s);

    if (w < 64) {
        if (is_signed) {
            emit_sext(anchor_id, w, s[0], rj);
            emit_sext(anchor_id, w, s[1], rk);
            emit_opnd3(anchor_id, LISA_MUL_D, s[2], s[0], s[1]);
            emit_sext(anchor_id, w, s[0], s[2]);
            emit_opnd3(anchor_id, LISA_XOR, s[0], s[0], s[2]);
        } else {
            emit_zext(anchor_id, w, s[0], rj);
            emit_zext(anchor_id, w, s[1], rk);
            emit_opnd3(anchor_id, LISA_MUL_D, s[2], s[0], s[1]);
            emit_opnd2i(anchor_id, LISA_SRLI_D, s[0], s[2], (int)w);
        }
    } else {
        if (is_signed) {
            emit_opnd3(anchor_id, LISA_MUL_D, s[0], rj, rk);
            emit_opnd3(anchor_id, LISA_MULH_D, s[1], rj, rk);
            emit_opnd2i(anchor_id, LISA_SRAI_D, s[0], s[0], 63);
            emit_opnd3(anchor_id, LISA_XOR, s[0], s[0], s[1]);
        } else {
            emit_opnd3(anchor_id, LISA_MULH_DU, s[0], rj, rk);
        }
    }
    emit_opnd2i(anchor_id, LISA_ORI, s[1], zero_ir2_opnd, (int)(SW_EF_CF | SW_EF_OF));
    emit_opnd3(anchor_id, LISA_MASKEQZ, tp_ir2_opnd, s[1], s[0]);

    restore_scratch(anchor_id, 3, s);
}

static void lower_setx86j(int anchor_id, IR2_OPND rd, int cond)
{
    int c = cond & 0xf;
    if (ir2_opnd_base_reg_num(&rd) == 0) {
        return;
    }

    switch (c) {
    case 0: /* A: !CF && !ZF -> ((tp & 0x41) == 0) */
        emit_opnd2i(anchor_id, LISA_ANDI, rd, tp_ir2_opnd, 0x41);
        emit_opnd2i(anchor_id, LISA_SLTUI, rd, rd, 1);
        break;
    case 1: /* AE: !CF */
        emit_opnd2i(anchor_id, LISA_ANDI, rd, tp_ir2_opnd, 1);
        emit_opnd2i(anchor_id, LISA_XORI, rd, rd, 1);
        break;
    case 2: /* B: CF */
        emit_opnd2i(anchor_id, LISA_ANDI, rd, tp_ir2_opnd, 1);
        break;
    case 3: /* BE: CF || ZF -> ((tp & 0x41) != 0) */
        emit_opnd2i(anchor_id, LISA_ANDI, rd, tp_ir2_opnd, 0x41);
        emit_opnd3(anchor_id, LISA_SLTU, rd, zero_ir2_opnd, rd);
        break;
    case 4: /* E: ZF */
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, rd, tp_ir2_opnd, 6, 6);
        break;
    case 5: /* NE: !ZF */
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, rd, tp_ir2_opnd, 6, 6);
        emit_opnd2i(anchor_id, LISA_XORI, rd, rd, 1);
        break;
    case 6: /* G: !ZF && (SF == OF) */
    case 7: /* GE: SF == OF */
    case 8: /* L: SF != OF */
    case 9: /* LE: ZF || (SF != OF) */
    {
        IR2_OPND s[3];
        pick_scratch(1, ir2_opnd_base_reg_num(&rd), -1, -1, s);
        spill_scratch(anchor_id, 1, s);

        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, rd, tp_ir2_opnd, 7, 7);   /* SF */
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[0], tp_ir2_opnd, 11, 11); /* OF */
        emit_opnd3(anchor_id, LISA_XOR, rd, rd, s[0]); /* SF != OF */
        if (c == 7) {
            emit_opnd2i(anchor_id, LISA_XORI, rd, rd, 1);
        } else if (c == 9) {
            emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[0], tp_ir2_opnd, 6, 6);
            emit_opnd3(anchor_id, LISA_OR, rd, rd, s[0]);
        } else if (c == 6) {
            emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[0], tp_ir2_opnd, 6, 6);
            emit_opnd3(anchor_id, LISA_OR, rd, rd, s[0]);
            emit_opnd2i(anchor_id, LISA_XORI, rd, rd, 1);
        }
        restore_scratch(anchor_id, 1, s);
        break;
    }
    case 10: /* S: SF */
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, rd, tp_ir2_opnd, 7, 7);
        break;
    case 11: /* NS: !SF */
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, rd, tp_ir2_opnd, 7, 7);
        emit_opnd2i(anchor_id, LISA_XORI, rd, rd, 1);
        break;
    case 12: /* O: OF */
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, rd, tp_ir2_opnd, 11, 11);
        break;
    case 13: /* NO: !OF */
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, rd, tp_ir2_opnd, 11, 11);
        emit_opnd2i(anchor_id, LISA_XORI, rd, rd, 1);
        break;
    case 14: /* P: PF */
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, rd, tp_ir2_opnd, 2, 2);
        break;
    default: /* NP: !PF */
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, rd, tp_ir2_opnd, 2, 2);
        emit_opnd2i(anchor_id, LISA_XORI, rd, rd, 1);
        break;
    }
}

static void lower_adc_sbc_val(int anchor_id, unsigned w,
                              IR2_OPND rd, IR2_OPND rj, IR2_OPND rk, int is_sub)
{
    IR2_OPND s[3];
    pick_scratch(1, ir2_opnd_base_reg_num(&rd),
                 ir2_opnd_base_reg_num(&rj), ir2_opnd_base_reg_num(&rk), s);
    spill_scratch(anchor_id, 1, s);

    emit_opnd2i(anchor_id, LISA_ANDI, s[0], tp_ir2_opnd, 1);
    if (is_sub) {
        emit_opnd3(anchor_id, LISA_SUB_D, s[0], rj, s[0]);
        emit_opnd3(anchor_id, LISA_SUB_D, rd, s[0], rk);
    } else {
        emit_opnd3(anchor_id, LISA_ADD_D, s[0], rj, s[0]);
        emit_opnd3(anchor_id, LISA_ADD_D, rd, s[0], rk);
    }
    emit_sext(anchor_id, w, rd, rd);

    restore_scratch(anchor_id, 1, s);
}

static void lower_shift(int anchor_id, unsigned w, IR2_OPND rj,
                        int has_rk, IR2_OPND rk, uint32_t imm_cnt, int kind)
{
    /* kind: 0 = sll, 1 = srl, 2 = sra */
    IR2_OPND s[3];
    uint32_t cmask = (w == 64) ? 0x3fu : 0x1fu;
    IR2_OPND lbl_done = ra_alloc_label();

    if (!has_rk) {
        uint32_t c = imm_cnt & cmask;
        if (c == 0) {
            return;
        }
        pick_scratch(3, ir2_opnd_base_reg_num(&rj), -1, -1, s);
        spill_scratch(anchor_id, 3, s);

        if (kind == 0) {
            /* SLL by immediate c (1 <= c < w) */
            emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, tp_ir2_opnd, rj,
                         (int)(w - c), (int)(w - c));
            emit_opnd2i(anchor_id, LISA_SLLI_D, s[2], rj, (int)c);
            if (w < 64) {
                emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[2], s[2], (int)(w - 1), 0);
            }
            if (c == 1) {
                emit_opnd3(anchor_id, LISA_XOR, s[0], rj, s[2]);
                emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[0], s[0],
                             (int)(w - 1), (int)(w - 1));
                emit_opnd2i(anchor_id, LISA_SLLI_D, s[0], s[0], 11);
                emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[0]);
            }
        } else if (kind == 1) {
            /* SRL by immediate c (1 <= c < w) */
            emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, tp_ir2_opnd, rj,
                         (int)(c - 1), (int)(c - 1));
            emit_zext(anchor_id, w, s[2], rj);
            emit_opnd2i(anchor_id, LISA_SRLI_D, s[2], s[2], (int)c);
            if (c == 1) {
                emit_opnd3(anchor_id, LISA_XOR, s[0], rj, s[2]);
                emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[0], s[0],
                             (int)(w - 1), (int)(w - 1));
                emit_opnd2i(anchor_id, LISA_SLLI_D, s[0], s[0], 11);
                emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[0]);
            }
        } else {
            /* SRA by immediate c (1 <= c < w) */
            emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, tp_ir2_opnd, rj,
                         (int)(c - 1), (int)(c - 1));
            emit_sext(anchor_id, w, s[2], rj);
            emit_opnd2i(anchor_id, LISA_SRAI_D, s[2], s[2], (int)c);
            if (w < 64) {
                emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[2], s[2], (int)(w - 1), 0);
            }
        }
        emit_szp_into_tp(anchor_id, w, s[2], s[0], s[1]);
        restore_scratch(anchor_id, 3, s);
        return;
    }

    /* Register count form: s[0] = c, s[1] = temp, s[2] = result */
    pick_scratch(3, ir2_opnd_base_reg_num(&rj), ir2_opnd_base_reg_num(&rk), -1, s);
    spill_scratch(anchor_id, 3, s);

    emit_opnd2i(anchor_id, LISA_ANDI, s[0], rk, (int)cmask);
    emit_opnd2(anchor_id, LISA_BEQZ, s[0], lbl_done);

    if (kind == 0) {
        /* SLL: cf = (c <= w) ? ((x >> (w - c)) & 1) : 0; r = (c < w) ? ((x << c) & m) : 0 */
        IR2_OPND lbl_ge = ra_alloc_label();
        IR2_OPND lbl_szp = ra_alloc_label();
        if (w < 32) {
            emit_opnd2i(anchor_id, LISA_ORI, s[1], zero_ir2_opnd, (int)w);
            emit_opnd3(anchor_id, LISA_BGEU, s[0], s[1], lbl_ge);
        }
        /* 1 <= c < w */
        emit_opnd2i(anchor_id, LISA_ORI, s[1], zero_ir2_opnd, (int)w);
        emit_opnd3(anchor_id, LISA_SUB_D, s[1], s[1], s[0]);
        emit_opnd3(anchor_id, LISA_SRL_D, tp_ir2_opnd, rj, s[1]);
        emit_opnd2i(anchor_id, LISA_ANDI, tp_ir2_opnd, tp_ir2_opnd, 1);
        emit_opnd3(anchor_id, LISA_SLL_D, s[2], rj, s[0]);
        if (w < 64) {
            emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[2], s[2], (int)(w - 1), 0);
        }
        /* OF if c == 1 */
        emit_opnd2i(anchor_id, LISA_ADDI_D, s[0], s[0], -1);
        emit_opnd3(anchor_id, LISA_XOR, s[1], rj, s[2]);
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[1], s[1], (int)(w - 1), (int)(w - 1));
        emit_opnd2i(anchor_id, LISA_SLLI_D, s[1], s[1], 11);
        emit_opnd3(anchor_id, LISA_MASKNEZ, s[1], s[1], s[0]);
        emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[1]);
        if (w < 32) {
            emit_opnd1(anchor_id, LISA_B, lbl_szp);
            emit_opnd1(anchor_id, LISA_LABEL, lbl_ge);
            /* c >= w: r = 0, cf = (c == w) ? (rj & 1) : 0 */
            emit_opnd3(anchor_id, LISA_OR, s[2], zero_ir2_opnd, zero_ir2_opnd);
            emit_opnd2i(anchor_id, LISA_ADDI_D, s[0], s[0], -(int)w);
            emit_opnd2i(anchor_id, LISA_ANDI, tp_ir2_opnd, rj, 1);
            emit_opnd3(anchor_id, LISA_MASKNEZ, tp_ir2_opnd, tp_ir2_opnd, s[0]);
            emit_opnd1(anchor_id, LISA_LABEL, lbl_szp);
        }
    } else if (kind == 1) {
        /* SRL */
        IR2_OPND lbl_ge = ra_alloc_label();
        IR2_OPND lbl_szp = ra_alloc_label();
        if (w < 32) {
            emit_opnd2i(anchor_id, LISA_ORI, s[1], zero_ir2_opnd, (int)w);
            emit_opnd3(anchor_id, LISA_BGEU, s[0], s[1], lbl_ge);
        }
        emit_zext(anchor_id, w, s[2], rj);
        emit_opnd2i(anchor_id, LISA_ADDI_D, s[1], s[0], -1);
        emit_opnd3(anchor_id, LISA_SRL_D, tp_ir2_opnd, s[2], s[1]);
        emit_opnd2i(anchor_id, LISA_ANDI, tp_ir2_opnd, tp_ir2_opnd, 1);
        emit_opnd3(anchor_id, LISA_SRL_D, s[2], s[2], s[0]);
        /* OF if c == 1 */
        emit_opnd2i(anchor_id, LISA_ADDI_D, s[0], s[0], -1);
        emit_opnd3(anchor_id, LISA_XOR, s[1], rj, s[2]);
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[1], s[1], (int)(w - 1), (int)(w - 1));
        emit_opnd2i(anchor_id, LISA_SLLI_D, s[1], s[1], 11);
        emit_opnd3(anchor_id, LISA_MASKNEZ, s[1], s[1], s[0]);
        emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[1]);
        if (w < 32) {
            emit_opnd1(anchor_id, LISA_B, lbl_szp);
            emit_opnd1(anchor_id, LISA_LABEL, lbl_ge);
            emit_opnd3(anchor_id, LISA_OR, s[2], zero_ir2_opnd, zero_ir2_opnd);
            emit_opnd2i(anchor_id, LISA_ADDI_D, s[0], s[0], -(int)w);
            emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, tp_ir2_opnd, rj,
                         (int)(w - 1), (int)(w - 1));
            emit_opnd3(anchor_id, LISA_MASKNEZ, tp_ir2_opnd, tp_ir2_opnd, s[0]);
            emit_opnd1(anchor_id, LISA_LABEL, lbl_szp);
        }
    } else {
        /* SRA */
        emit_sext(anchor_id, w, s[2], rj);
        if (w < 32) {
            IR2_OPND lbl_ok = ra_alloc_label();
            emit_opnd2i(anchor_id, LISA_ORI, s[1], zero_ir2_opnd, (int)w);
            emit_opnd3(anchor_id, LISA_BLTU, s[0], s[1], lbl_ok);
            emit_opnd2i(anchor_id, LISA_ORI, s[0], zero_ir2_opnd, (int)(w - 1));
            emit_opnd1(anchor_id, LISA_LABEL, lbl_ok);
        }
        /* For c > w, s[0] was clamped to w-1, so (s2 >> (w-2))? Wait: if original c >= w,
         * cf = (s < 0) = (s2 >> 63) & 1 for c > w, and (x >> (w-1)) & 1 = (s < 0) for c == w!
         * Since s[2] is sign-extended, (s[2] >> (s[0]-1)) & 1 when s[0]=w-1 is bit w-2, NOT sign!
         * Fix: clamp sh to w (not w-1) for cf check, or compute cf from s[2] >> (c-1) with sra_d!
         * Because s[2] is sign-extended to 64 bits, SRA_D(s[2], c - 1) & 1 is ALWAYS
         * (x >> (c - 1)) & 1 for c <= w AND (s < 0) for c > w (since c - 1 <= 30 < 64)! */
        emit_opnd2i(anchor_id, LISA_ANDI, s[1], rk, (int)cmask);
        emit_opnd2i(anchor_id, LISA_ADDI_D, s[1], s[1], -1);
        emit_opnd3(anchor_id, LISA_SRA_D, tp_ir2_opnd, s[2], s[1]);
        emit_opnd2i(anchor_id, LISA_ANDI, tp_ir2_opnd, tp_ir2_opnd, 1);
        emit_opnd3(anchor_id, LISA_SRA_D, s[2], s[2], s[0]);
        if (w < 64) {
            emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[2], s[2], (int)(w - 1), 0);
        }
    }

    emit_szp_into_tp(anchor_id, w, s[2], s[0], s[1]);
    emit_opnd1(anchor_id, LISA_LABEL, lbl_done);
    restore_scratch(anchor_id, 3, s);
}

static void lower_rot(int anchor_id, unsigned w, IR2_OPND rj,
                      int has_rk, IR2_OPND rk, uint32_t imm_cnt,
                      int left, int through_carry)
{
    IR2_OPND s[3];
    uint32_t cmask = (w == 64) ? 0x3fu : 0x1fu;
    IR2_OPND lbl_done = ra_alloc_label();

    pick_scratch(3, ir2_opnd_base_reg_num(&rj),
                 has_rk ? ir2_opnd_base_reg_num(&rk) : -1, -1, s);
    spill_scratch(anchor_id, 3, s);

    if (has_rk) {
        emit_opnd2i(anchor_id, LISA_ANDI, s[0], rk, (int)cmask);
    } else {
        uint32_t c = imm_cnt & cmask;
        if (c == 0) {
            restore_scratch(anchor_id, 3, s);
            return;
        }
        emit_opnd2i(anchor_id, LISA_ORI, s[0], zero_ir2_opnd, (int)c);
    }
    emit_opnd2(anchor_id, LISA_BEQZ, s[0], lbl_done);

    if (!through_carry) {
        /* n = c & (w - 1); if n == 0 -> done */
        emit_opnd2i(anchor_id, LISA_ANDI, s[1], s[0], (int)(w - 1));
        emit_opnd2(anchor_id, LISA_BEQZ, s[1], lbl_done);
        /* Clear CF & OF in $tp */
        emit_opnd2i(anchor_id, LISA_ORI, s[2], zero_ir2_opnd, (int)(SW_EF_CF | SW_EF_OF));
        emit_opnd3(anchor_id, LISA_ANDN, tp_ir2_opnd, tp_ir2_opnd, s[2]);
        /* Compute new CF in s[2] */
        if (left) {
            /* cf = (x >> (w - n)) & 1 */
            emit_opnd2i(anchor_id, LISA_ORI, s[2], zero_ir2_opnd, (int)w);
            emit_opnd3(anchor_id, LISA_SUB_D, s[2], s[2], s[1]);
            emit_opnd3(anchor_id, LISA_SRL_D, s[2], rj, s[2]);
            emit_opnd2i(anchor_id, LISA_ANDI, s[2], s[2], 1);
        } else {
            /* cf = (x >> (n - 1)) & 1 */
            emit_opnd2i(anchor_id, LISA_ADDI_D, s[2], s[1], -1);
            emit_opnd3(anchor_id, LISA_SRL_D, s[2], rj, s[2]);
            emit_opnd2i(anchor_id, LISA_ANDI, s[2], s[2], 1);
        }
        emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[2]);
        /* OF if c == 1: (x_msb ^ r_msb).
         * For left c==1: r_msb = x[w-2], so OF = x[w-1] ^ x[w-2].
         * For right c==1: r_msb = x[0] = new_cf, so OF = x[w-1] ^ new_cf. */
        emit_opnd2i(anchor_id, LISA_ADDI_D, s[0], s[0], -1);
        emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[1], rj, (int)(w - 1), (int)(w - 1));
        if (left) {
            emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[2], rj, (int)(w - 2), (int)(w - 2));
        }
        emit_opnd3(anchor_id, LISA_XOR, s[1], s[1], s[2]);
        emit_opnd2i(anchor_id, LISA_SLLI_D, s[1], s[1], 11);
        emit_opnd3(anchor_id, LISA_MASKNEZ, s[1], s[1], s[0]);
        emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[1]);
    } else {
        /* RCL / RCR: ring = w + 1. Compute n = c % (w + 1) into s[1]. */
        emit_opnd3(anchor_id, LISA_OR, s[1], s[0], zero_ir2_opnd);
        if (w < 32) {
            IR2_OPND lbl_mod = ra_alloc_label();
            IR2_OPND lbl_mod_done = ra_alloc_label();
            emit_opnd2i(anchor_id, LISA_ORI, s[2], zero_ir2_opnd, (int)(w + 1));
            emit_opnd1(anchor_id, LISA_LABEL, lbl_mod);
            emit_opnd3(anchor_id, LISA_BLTU, s[1], s[2], lbl_mod_done);
            emit_opnd3(anchor_id, LISA_SUB_D, s[1], s[1], s[2]);
            emit_opnd1(anchor_id, LISA_B, lbl_mod);
            emit_opnd1(anchor_id, LISA_LABEL, lbl_mod_done);
        }
        emit_opnd2(anchor_id, LISA_BEQZ, s[1], lbl_done);
        /* For c == 1:
         *   RCL: new_cf = x[w-1], r_msb = x[w-2], OF = x[w-1] ^ x[w-2].
         *   RCR: new_cf = x[0],   r_msb = old_cf, OF = x[w-1] ^ old_cf.
         * For general 1 <= n <= w:
         *   RCL: new_cf = (x >> (w - n)) & 1.
         *   RCR: new_cf = (x >> (n - 1)) & 1. */
        if (left) {
            emit_opnd2i(anchor_id, LISA_ORI, s[2], zero_ir2_opnd, (int)w);
            emit_opnd3(anchor_id, LISA_SUB_D, s[2], s[2], s[1]);
            emit_opnd3(anchor_id, LISA_SRL_D, s[2], rj, s[2]);
            emit_opnd2i(anchor_id, LISA_ANDI, s[2], s[2], 1); /* new_cf */
            /* Clear CF & OF, set new_cf */
            emit_opnd2i(anchor_id, LISA_ORI, s[1], zero_ir2_opnd, (int)(SW_EF_CF | SW_EF_OF));
            emit_opnd3(anchor_id, LISA_ANDN, tp_ir2_opnd, tp_ir2_opnd, s[1]);
            emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[2]);
            /* OF if c == 1: x[w-1] ^ x[w-2] */
            emit_opnd2i(anchor_id, LISA_ADDI_D, s[0], s[0], -1);
            emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[1], rj, (int)(w - 1), (int)(w - 1));
            emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[2], rj, (int)(w - 2), (int)(w - 2));
            emit_opnd3(anchor_id, LISA_XOR, s[1], s[1], s[2]);
            emit_opnd2i(anchor_id, LISA_SLLI_D, s[1], s[1], 11);
            emit_opnd3(anchor_id, LISA_MASKNEZ, s[1], s[1], s[0]);
            emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[1]);
        } else {
            /* Save old_cf in s[2] for OF if c == 1; compute new_cf in s[1] */
            emit_opnd2i(anchor_id, LISA_ADDI_D, s[1], s[1], -1);
            emit_opnd3(anchor_id, LISA_SRL_D, s[1], rj, s[1]);
            emit_opnd2i(anchor_id, LISA_ANDI, s[1], s[1], 1); /* new_cf */
            emit_opnd2i(anchor_id, LISA_ANDI, s[2], tp_ir2_opnd, 1); /* old_cf */
            /* Clear CF & OF in $tp, set new_cf */
            emit_opnd2i(anchor_id, LISA_ORI, tp_ir2_opnd, tp_ir2_opnd, 0); /* no-op avoided below */
            /* Use s[2] after computing OF! */
            emit_opnd2ii(anchor_id, LISA_BSTRPICK_D, s[2], rj, (int)(w - 1), (int)(w - 1));
            /* Wait: we overwrote old_cf! Keep old_cf via xor directly: */
            /* Re-order cleanly: tp currently has old_cf in bit 0! */
            emit_opnd3(anchor_id, LISA_XOR, s[2], s[2], tp_ir2_opnd);
            emit_opnd2i(anchor_id, LISA_ANDI, s[2], s[2], 1); /* x[w-1] ^ old_cf */
            emit_opnd2i(anchor_id, LISA_SLLI_D, s[2], s[2], 11);
            emit_opnd2i(anchor_id, LISA_ADDI_D, s[0], s[0], -1);
            emit_opnd3(anchor_id, LISA_MASKNEZ, s[2], s[2], s[0]);
            emit_opnd3(anchor_id, LISA_OR, s[1], s[1], s[2]); /* new_cf | new_of */
            emit_opnd2i(anchor_id, LISA_ORI, s[2], zero_ir2_opnd, (int)(SW_EF_CF | SW_EF_OF));
            emit_opnd3(anchor_id, LISA_ANDN, tp_ir2_opnd, tp_ir2_opnd, s[2]);
            emit_opnd3(anchor_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[1]);
        }
    }

    emit_opnd1(anchor_id, LISA_LABEL, lbl_done);
    restore_scratch(anchor_id, 3, s);
}

static void lower_rotri_bh(int anchor_id, unsigned w,
                           IR2_OPND rd, IR2_OPND rj, uint32_t sh)
{
    sh &= (w - 1);
    if (sh == 0) {
        emit_sext(anchor_id, w, rd, rj);
        return;
    }
    IR2_OPND s[3];
    pick_scratch(1, ir2_opnd_base_reg_num(&rd), ir2_opnd_base_reg_num(&rj), -1, s);
    spill_scratch(anchor_id, 1, s);

    emit_zext(anchor_id, w, s[0], rj);
    emit_opnd2i(anchor_id, LISA_SLLI_D, rd, s[0], (int)(w - sh));
    emit_opnd2i(anchor_id, LISA_SRLI_D, s[0], s[0], (int)sh);
    emit_opnd3(anchor_id, LISA_OR, rd, rd, s[0]);
    emit_sext(anchor_id, w, rd, rd);

    restore_scratch(anchor_id, 1, s);
}

void tr_lower_sw_lbt(TRANSLATION_DATA *lat_ctx)
{
    if (!option_sw_lbt || lat_ctx == NULL || lat_ctx->first_ir2 == NULL) {
        return;
    }

    int cur_id = ir2_get_id(lat_ctx->first_ir2);
    while (cur_id != -1) {
        IR2_INST orig = lat_ctx->ir2_inst_array[cur_id];
        int next_id = orig._next;
        IR2_OPCODE op = (IR2_OPCODE)orig._opcode;
        int lowered = 1;

        switch (op) {
        case LISA_X86MFFLAG: {
            uint32_t mask = lbt_imm_to_eflags_mask(ir2_opnd_val(&orig._opnd[1]) & 0x3fu);
            emit_opnd2i(cur_id, LISA_ANDI, orig._opnd[0], tp_ir2_opnd, (int)mask);
            break;
        }
        case LISA_X86MTFLAG: {
            uint32_t imm = ir2_opnd_val(&orig._opnd[1]) & 0x3fu;
            uint32_t mask = lbt_imm_to_eflags_mask(imm);
            if (imm == 0x3fu) {
                emit_opnd2i(cur_id, LISA_ANDI, tp_ir2_opnd, orig._opnd[0], (int)SW_EF_ALL);
            } else if (mask != 0) {
                IR2_OPND s[3];
                pick_scratch(1, ir2_opnd_base_reg_num(&orig._opnd[0]), -1, -1, s);
                spill_scratch(cur_id, 1, s);
                emit_opnd2i(cur_id, LISA_ANDI, tp_ir2_opnd, tp_ir2_opnd,
                            (int)(SW_EF_ALL & ~mask));
                emit_opnd2i(cur_id, LISA_ANDI, s[0], orig._opnd[0], (int)mask);
                emit_opnd3(cur_id, LISA_OR, tp_ir2_opnd, tp_ir2_opnd, s[0]);
                restore_scratch(cur_id, 1, s);
            }
            break;
        }
        case LISA_SETX86J:
            lower_setx86j(cur_id, orig._opnd[0], ir2_opnd_imm(&orig._opnd[1]));
            break;

        /* Logic */
        case LISA_X86AND_B: lower_logic(cur_id,  8, orig._opnd[0], orig._opnd[1], LISA_AND); break;
        case LISA_X86AND_H: lower_logic(cur_id, 16, orig._opnd[0], orig._opnd[1], LISA_AND); break;
        case LISA_X86AND_W: lower_logic(cur_id, 32, orig._opnd[0], orig._opnd[1], LISA_AND); break;
        case LISA_X86AND_D: lower_logic(cur_id, 64, orig._opnd[0], orig._opnd[1], LISA_AND); break;
        case LISA_X86OR_B:  lower_logic(cur_id,  8, orig._opnd[0], orig._opnd[1], LISA_OR);  break;
        case LISA_X86OR_H:  lower_logic(cur_id, 16, orig._opnd[0], orig._opnd[1], LISA_OR);  break;
        case LISA_X86OR_W:  lower_logic(cur_id, 32, orig._opnd[0], orig._opnd[1], LISA_OR);  break;
        case LISA_X86OR_D:  lower_logic(cur_id, 64, orig._opnd[0], orig._opnd[1], LISA_OR);  break;
        case LISA_X86XOR_B: lower_logic(cur_id,  8, orig._opnd[0], orig._opnd[1], LISA_XOR); break;
        case LISA_X86XOR_H: lower_logic(cur_id, 16, orig._opnd[0], orig._opnd[1], LISA_XOR); break;
        case LISA_X86XOR_W: lower_logic(cur_id, 32, orig._opnd[0], orig._opnd[1], LISA_XOR); break;
        case LISA_X86XOR_D: lower_logic(cur_id, 64, orig._opnd[0], orig._opnd[1], LISA_XOR); break;

        /* Add / Sub / Adc / Sbc flags */
        case LISA_X86ADD_B:  lower_add_adc(cur_id,  8, orig._opnd[0], orig._opnd[1], 0, 0); break;
        case LISA_X86ADD_H:  lower_add_adc(cur_id, 16, orig._opnd[0], orig._opnd[1], 0, 0); break;
        case LISA_X86ADD_W:  lower_add_adc(cur_id, 32, orig._opnd[0], orig._opnd[1], 0, 0); break;
        case LISA_X86ADD_D:  lower_add_adc(cur_id, 64, orig._opnd[0], orig._opnd[1], 0, 0); break;
        case LISA_X86ADD_WU: lower_add_adc(cur_id, 32, orig._opnd[0], orig._opnd[1], 0, 1); break;
        case LISA_X86ADD_DU: lower_add_adc(cur_id, 64, orig._opnd[0], orig._opnd[1], 0, 1); break;
        case LISA_X86ADC_B:  lower_add_adc(cur_id,  8, orig._opnd[0], orig._opnd[1], 1, 0); break;
        case LISA_X86ADC_H:  lower_add_adc(cur_id, 16, orig._opnd[0], orig._opnd[1], 1, 0); break;
        case LISA_X86ADC_W:  lower_add_adc(cur_id, 32, orig._opnd[0], orig._opnd[1], 1, 0); break;
        case LISA_X86ADC_D:  lower_add_adc(cur_id, 64, orig._opnd[0], orig._opnd[1], 1, 0); break;
        case LISA_X86SUB_B:  lower_sub_sbc(cur_id,  8, orig._opnd[0], orig._opnd[1], 0, 0); break;
        case LISA_X86SUB_H:  lower_sub_sbc(cur_id, 16, orig._opnd[0], orig._opnd[1], 0, 0); break;
        case LISA_X86SUB_W:  lower_sub_sbc(cur_id, 32, orig._opnd[0], orig._opnd[1], 0, 0); break;
        case LISA_X86SUB_D:  lower_sub_sbc(cur_id, 64, orig._opnd[0], orig._opnd[1], 0, 0); break;
        case LISA_X86SUB_WU: lower_sub_sbc(cur_id, 32, orig._opnd[0], orig._opnd[1], 0, 1); break;
        case LISA_X86SUB_DU: lower_sub_sbc(cur_id, 64, orig._opnd[0], orig._opnd[1], 0, 1); break;
        case LISA_X86SBC_B:  lower_sub_sbc(cur_id,  8, orig._opnd[0], orig._opnd[1], 1, 0); break;
        case LISA_X86SBC_H:  lower_sub_sbc(cur_id, 16, orig._opnd[0], orig._opnd[1], 1, 0); break;
        case LISA_X86SBC_W:  lower_sub_sbc(cur_id, 32, orig._opnd[0], orig._opnd[1], 1, 0); break;
        case LISA_X86SBC_D:  lower_sub_sbc(cur_id, 64, orig._opnd[0], orig._opnd[1], 1, 0); break;

        /* Inc / Dec flags */
        case LISA_X86INC_B: lower_inc_dec(cur_id,  8, orig._opnd[0], 0); break;
        case LISA_X86INC_H: lower_inc_dec(cur_id, 16, orig._opnd[0], 0); break;
        case LISA_X86INC_W: lower_inc_dec(cur_id, 32, orig._opnd[0], 0); break;
        case LISA_X86INC_D: lower_inc_dec(cur_id, 64, orig._opnd[0], 0); break;
        case LISA_X86DEC_B: lower_inc_dec(cur_id,  8, orig._opnd[0], 1); break;
        case LISA_X86DEC_H: lower_inc_dec(cur_id, 16, orig._opnd[0], 1); break;
        case LISA_X86DEC_W: lower_inc_dec(cur_id, 32, orig._opnd[0], 1); break;
        case LISA_X86DEC_D: lower_inc_dec(cur_id, 64, orig._opnd[0], 1); break;

        /* ADC / SBC value instructions */
        case LISA_ADC_B: lower_adc_sbc_val(cur_id,  8, orig._opnd[0], orig._opnd[1], orig._opnd[2], 0); break;
        case LISA_ADC_H: lower_adc_sbc_val(cur_id, 16, orig._opnd[0], orig._opnd[1], orig._opnd[2], 0); break;
        case LISA_ADC_W: lower_adc_sbc_val(cur_id, 32, orig._opnd[0], orig._opnd[1], orig._opnd[2], 0); break;
        case LISA_ADC_D: lower_adc_sbc_val(cur_id, 64, orig._opnd[0], orig._opnd[1], orig._opnd[2], 0); break;
        case LISA_SBC_B: lower_adc_sbc_val(cur_id,  8, orig._opnd[0], orig._opnd[1], orig._opnd[2], 1); break;
        case LISA_SBC_H: lower_adc_sbc_val(cur_id, 16, orig._opnd[0], orig._opnd[1], orig._opnd[2], 1); break;
        case LISA_SBC_W: lower_adc_sbc_val(cur_id, 32, orig._opnd[0], orig._opnd[1], orig._opnd[2], 1); break;
        case LISA_SBC_D: lower_adc_sbc_val(cur_id, 64, orig._opnd[0], orig._opnd[1], orig._opnd[2], 1); break;

        /* Multiply flags */
        case LISA_X86MUL_B:  lower_mul(cur_id,  8, orig._opnd[0], orig._opnd[1], 1); break;
        case LISA_X86MUL_H:  lower_mul(cur_id, 16, orig._opnd[0], orig._opnd[1], 1); break;
        case LISA_X86MUL_W:  lower_mul(cur_id, 32, orig._opnd[0], orig._opnd[1], 1); break;
        case LISA_X86MUL_D:  lower_mul(cur_id, 64, orig._opnd[0], orig._opnd[1], 1); break;
        case LISA_X86MUL_BU: lower_mul(cur_id,  8, orig._opnd[0], orig._opnd[1], 0); break;
        case LISA_X86MUL_HU: lower_mul(cur_id, 16, orig._opnd[0], orig._opnd[1], 0); break;
        case LISA_X86MUL_WU: lower_mul(cur_id, 32, orig._opnd[0], orig._opnd[1], 0); break;
        case LISA_X86MUL_DU: lower_mul(cur_id, 64, orig._opnd[0], orig._opnd[1], 0); break;

        /* Shifts (register) */
        case LISA_X86SLL_B: lower_shift(cur_id,  8, orig._opnd[0], 1, orig._opnd[1], 0, 0); break;
        case LISA_X86SLL_H: lower_shift(cur_id, 16, orig._opnd[0], 1, orig._opnd[1], 0, 0); break;
        case LISA_X86SLL_W: lower_shift(cur_id, 32, orig._opnd[0], 1, orig._opnd[1], 0, 0); break;
        case LISA_X86SLL_D: lower_shift(cur_id, 64, orig._opnd[0], 1, orig._opnd[1], 0, 0); break;
        case LISA_X86SRL_B: lower_shift(cur_id,  8, orig._opnd[0], 1, orig._opnd[1], 0, 1); break;
        case LISA_X86SRL_H: lower_shift(cur_id, 16, orig._opnd[0], 1, orig._opnd[1], 0, 1); break;
        case LISA_X86SRL_W: lower_shift(cur_id, 32, orig._opnd[0], 1, orig._opnd[1], 0, 1); break;
        case LISA_X86SRL_D: lower_shift(cur_id, 64, orig._opnd[0], 1, orig._opnd[1], 0, 1); break;
        case LISA_X86SRA_B: lower_shift(cur_id,  8, orig._opnd[0], 1, orig._opnd[1], 0, 2); break;
        case LISA_X86SRA_H: lower_shift(cur_id, 16, orig._opnd[0], 1, orig._opnd[1], 0, 2); break;
        case LISA_X86SRA_W: lower_shift(cur_id, 32, orig._opnd[0], 1, orig._opnd[1], 0, 2); break;
        case LISA_X86SRA_D: lower_shift(cur_id, 64, orig._opnd[0], 1, orig._opnd[1], 0, 2); break;

        /* Shifts (immediate) */
        case LISA_X86SLLI_B: lower_shift(cur_id,  8, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 0); break;
        case LISA_X86SLLI_H: lower_shift(cur_id, 16, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 0); break;
        case LISA_X86SLLI_W: lower_shift(cur_id, 32, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 0); break;
        case LISA_X86SLLI_D: lower_shift(cur_id, 64, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 0); break;
        case LISA_X86SRLI_B: lower_shift(cur_id,  8, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 1); break;
        case LISA_X86SRLI_H: lower_shift(cur_id, 16, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 1); break;
        case LISA_X86SRLI_W: lower_shift(cur_id, 32, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 1); break;
        case LISA_X86SRLI_D: lower_shift(cur_id, 64, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 1); break;
        case LISA_X86SRAI_B: lower_shift(cur_id,  8, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 2); break;
        case LISA_X86SRAI_H: lower_shift(cur_id, 16, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 2); break;
        case LISA_X86SRAI_W: lower_shift(cur_id, 32, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 2); break;
        case LISA_X86SRAI_D: lower_shift(cur_id, 64, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 2); break;

        /* Rotates (register) */
        case LISA_X86ROTL_B: lower_rot(cur_id,  8, orig._opnd[0], 1, orig._opnd[1], 0, 1, 0); break;
        case LISA_X86ROTL_H: lower_rot(cur_id, 16, orig._opnd[0], 1, orig._opnd[1], 0, 1, 0); break;
        case LISA_X86ROTL_W: lower_rot(cur_id, 32, orig._opnd[0], 1, orig._opnd[1], 0, 1, 0); break;
        case LISA_X86ROTL_D: lower_rot(cur_id, 64, orig._opnd[0], 1, orig._opnd[1], 0, 1, 0); break;
        case LISA_X86ROTR_B: lower_rot(cur_id,  8, orig._opnd[0], 1, orig._opnd[1], 0, 0, 0); break;
        case LISA_X86ROTR_H: lower_rot(cur_id, 16, orig._opnd[0], 1, orig._opnd[1], 0, 0, 0); break;
        case LISA_X86ROTR_W: lower_rot(cur_id, 32, orig._opnd[0], 1, orig._opnd[1], 0, 0, 0); break;
        case LISA_X86ROTR_D: lower_rot(cur_id, 64, orig._opnd[0], 1, orig._opnd[1], 0, 0, 0); break;
        case LISA_X86RCL_B:  lower_rot(cur_id,  8, orig._opnd[0], 1, orig._opnd[1], 0, 1, 1); break;
        case LISA_X86RCL_H:  lower_rot(cur_id, 16, orig._opnd[0], 1, orig._opnd[1], 0, 1, 1); break;
        case LISA_X86RCL_W:  lower_rot(cur_id, 32, orig._opnd[0], 1, orig._opnd[1], 0, 1, 1); break;
        case LISA_X86RCL_D:  lower_rot(cur_id, 64, orig._opnd[0], 1, orig._opnd[1], 0, 1, 1); break;
        case LISA_X86RCR_B:  lower_rot(cur_id,  8, orig._opnd[0], 1, orig._opnd[1], 0, 0, 1); break;
        case LISA_X86RCR_H:  lower_rot(cur_id, 16, orig._opnd[0], 1, orig._opnd[1], 0, 0, 1); break;
        case LISA_X86RCR_W:  lower_rot(cur_id, 32, orig._opnd[0], 1, orig._opnd[1], 0, 0, 1); break;
        case LISA_X86RCR_D:  lower_rot(cur_id, 64, orig._opnd[0], 1, orig._opnd[1], 0, 0, 1); break;

        /* Rotates (immediate) */
        case LISA_X86ROTLI_B: lower_rot(cur_id,  8, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 1, 0); break;
        case LISA_X86ROTLI_H: lower_rot(cur_id, 16, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 1, 0); break;
        case LISA_X86ROTLI_W: lower_rot(cur_id, 32, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 1, 0); break;
        case LISA_X86ROTLI_D: lower_rot(cur_id, 64, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 1, 0); break;
        case LISA_X86ROTRI_B: lower_rot(cur_id,  8, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 0, 0); break;
        case LISA_X86ROTRI_H: lower_rot(cur_id, 16, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 0, 0); break;
        case LISA_X86ROTRI_W: lower_rot(cur_id, 32, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 0, 0); break;
        case LISA_X86ROTRI_D: lower_rot(cur_id, 64, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 0, 0); break;
        case LISA_X86RCLI_B:  lower_rot(cur_id,  8, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 1, 1); break;
        case LISA_X86RCLI_H:  lower_rot(cur_id, 16, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 1, 1); break;
        case LISA_X86RCLI_W:  lower_rot(cur_id, 32, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 1, 1); break;
        case LISA_X86RCLI_D:  lower_rot(cur_id, 64, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 1, 1); break;
        case LISA_X86RCRI_B:  lower_rot(cur_id,  8, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 0, 1); break;
        case LISA_X86RCRI_H:  lower_rot(cur_id, 16, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 0, 1); break;
        case LISA_X86RCRI_W:  lower_rot(cur_id, 32, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 0, 1); break;
        case LISA_X86RCRI_D:  lower_rot(cur_id, 64, orig._opnd[0], 0, zero_ir2_opnd, ir2_opnd_val(&orig._opnd[1]), 0, 1); break;

        /* Byte/halfword rotate value instructions (LBT extension) */
        case LISA_ROTRI_B:
            lower_rotri_bh(cur_id, 8, orig._opnd[0], orig._opnd[1],
                           ir2_opnd_val(&orig._opnd[2]));
            break;
        case LISA_ROTRI_H:
            lower_rotri_bh(cur_id, 16, orig._opnd[0], orig._opnd[1],
                           ir2_opnd_val(&orig._opnd[2]));
            break;

        /* Loop helpers */
        case LISA_X86LOOPE:
        case LISA_X86LOOPNE: {
            IR2_OPND s[3];
            pick_scratch(1, ir2_opnd_base_reg_num(&orig._opnd[0]),
                         ir2_opnd_base_reg_num(&orig._opnd[1]), -1, s);
            spill_scratch(cur_id, 1, s);
            emit_opnd2ii(cur_id, LISA_BSTRPICK_D, s[0], tp_ir2_opnd, 6, 6);
            if (op == LISA_X86LOOPNE) {
                emit_opnd2i(cur_id, LISA_XORI, s[0], s[0], 1);
            }
            emit_opnd3(cur_id, LISA_SLTU, orig._opnd[0], zero_ir2_opnd, orig._opnd[1]);
            emit_opnd3(cur_id, LISA_AND, orig._opnd[0], orig._opnd[0], s[0]);
            restore_scratch(cur_id, 1, s);
            break;
        }

        /* x87 TOP / TM */
        case LISA_X86MTTOP: {
            IR2_OPND s[3];
            pick_scratch(1, -1, -1, -1, s);
            spill_scratch(cur_id, 1, s);
            emit_opnd2i(cur_id, LISA_ORI, s[0], zero_ir2_opnd,
                        (int)(ir2_opnd_val(&orig._opnd[0]) & 7u));
            emit_opnd2i(cur_id, LISA_ST_D, s[0], env_ir2_opnd,
                        lsenv_offset_of_mips_regs(lsenv, 31));
            restore_scratch(cur_id, 1, s);
            break;
        }
        case LISA_X86MFTOP:
            emit_opnd2i(cur_id, LISA_LD_D, orig._opnd[0], env_ir2_opnd,
                        lsenv_offset_of_mips_regs(lsenv, 31));
            break;
        case LISA_X86INCTOP:
        case LISA_X86DECTOP: {
            IR2_OPND s[3];
            pick_scratch(1, -1, -1, -1, s);
            spill_scratch(cur_id, 1, s);
            emit_opnd2i(cur_id, LISA_LD_D, s[0], env_ir2_opnd,
                        lsenv_offset_of_mips_regs(lsenv, 31));
            emit_opnd2i(cur_id, LISA_ADDI_D, s[0], s[0],
                        (op == LISA_X86INCTOP) ? 1 : -1);
            emit_opnd2i(cur_id, LISA_ANDI, s[0], s[0], 7);
            emit_opnd2i(cur_id, LISA_ST_D, s[0], env_ir2_opnd,
                        lsenv_offset_of_mips_regs(lsenv, 31));
            restore_scratch(cur_id, 1, s);
            break;
        }
        case LISA_X86SETTM:
        case LISA_X86CLRTM:
            /* No-op in software LBT mode. */
            break;

        default:
            lowered = 0;
            break;
        }

        if (lowered) {
            ir2_remove(cur_id);
        }
        cur_id = next_id;
    }
}
