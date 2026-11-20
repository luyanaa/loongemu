/*
 * MIPS64r2 IR1 decoder for LATM.
 *
 * The decode tree and opcode constants are adapted from QEMU
 * target/mips/tcg/translate.c (LGPL-2.1-or-later).  The QEMU translator
 * emits TCG; this decoder records the same decoded fields in MipsIR1 so
 * translate-mips.c can emit IR2.
 */
#include "qemu/osdep.h"
#include "ir1.h"
#include "mips-opcodes.h"

static bool ri(MipsIR1 *ir1)
{
    ir1->op = MIPS_OP_RI;
    return true;
}

static bool set_alu(MipsIR1 *ir1, unsigned subop)
{
    ir1->op = MIPS_OP_ALU;
    ir1->subop = subop;
    return true;
}

static bool set_imm(MipsIR1 *ir1, unsigned subop)
{
    ir1->op = MIPS_OP_ALU_IMM;
    ir1->subop = subop;
    return true;
}

static bool set_branch(MipsIR1 *ir1, unsigned subop, bool likely, bool link)
{
    ir1->op = MIPS_OP_BRANCH;
    ir1->subop = subop;
    ir1->likely = likely;
    ir1->link = link;
    return true;
}

static bool decode_special(MipsIR1 *ir1, uint32_t insn)
{
    unsigned funct = insn & 0x3f;
    unsigned rs = ir1->rs;

    switch (funct) {
    case 0x00: /* SLL / NOP / SSNOP / EHB / PAUSE */
        if (insn == 0) {
            ir1->op = MIPS_OP_NOP;
            return true;
        }
        if (insn == 0x00000040) { /* SSNOP */
            ir1->op = MIPS_OP_NOP;
            return true;
        }
        if (insn == 0x000000c0) { /* EHB */
            ir1->op = MIPS_OP_EHB;
            return true;
        }
        if (insn == 0x00000140) { /* PAUSE */
            ir1->op = MIPS_OP_PAUSE;
            return true;
        }
        return ir1->op = MIPS_OP_SHIFT, ir1->subop = MIPS_SH_SLL, true;
    case 0x02: /* SRL / ROTR */
        if (rs == 1) {
            ir1->op = MIPS_OP_SHIFT_V;
            ir1->subop = MIPS_SHV_ROTR;
            return true;
        }
        ir1->op = MIPS_OP_SHIFT;
        ir1->subop = MIPS_SH_SRL;
        return true;
    case 0x03:
        ir1->op = MIPS_OP_SHIFT;
        ir1->subop = MIPS_SH_SRA;
        return true;
    case 0x04:
        ir1->op = MIPS_OP_SHIFT_V;
        ir1->subop = MIPS_SHV_SLLV;
        return true;
    case 0x06: /* SRLV / ROTRV */
        if (((insn >> 6) & 0x1f) == 1) {
            ir1->op = MIPS_OP_SHIFT_V;
            ir1->subop = MIPS_SHV_ROTRV;
            return true;
        }
        ir1->op = MIPS_OP_SHIFT_V;
        ir1->subop = MIPS_SHV_SRLV;
        return true;
    case 0x07:
        ir1->op = MIPS_OP_SHIFT_V;
        ir1->subop = MIPS_SHV_SRAV;
        return true;
    case 0x08: /* JR / JR.HB */
        if (ir1->sa != 0 && ir1->sa != 16) {
            return ri(ir1);
        }
        ir1->op = MIPS_OP_JUMP;
        ir1->subop = MIPS_JP_JR;
        return true;
    case 0x09: /* JALR / JALR.HB */
        if (ir1->sa != 0 && ir1->sa != 16) {
            return ri(ir1);
        }
        ir1->op = MIPS_OP_JUMP;
        ir1->subop = MIPS_JP_JALR;
        ir1->link = true;
        return true;
    case 0x0a:
        return set_alu(ir1, MIPS_ALU_MOVZ);
    case 0x0b:
        return set_alu(ir1, MIPS_ALU_MOVN);
    case 0x0c:
        ir1->op = MIPS_OP_SYSCALL;
        return true;
    case 0x0d:
        ir1->op = MIPS_OP_BREAK;
        return true;
    case 0x0f:
        ir1->op = MIPS_OP_SYNC;
        return true;
    case 0x10: /* MFHI */
        ir1->op = MIPS_OP_HILO;
        ir1->subop = MIPS_HILO_MFHI;
        return true;
    case 0x11: /* MTHI */
        ir1->op = MIPS_OP_HILO;
        ir1->subop = MIPS_HILO_MTHI;
        return true;
    case 0x12: /* MFLO */
        ir1->op = MIPS_OP_HILO;
        ir1->subop = MIPS_HILO_MFLO;
        return true;
    case 0x13: /* MTLO */
        ir1->op = MIPS_OP_HILO;
        ir1->subop = MIPS_HILO_MTLO;
        return true;
    case 0x14:
        ir1->op = MIPS_OP_SHIFT_V;
        ir1->subop = MIPS_SHV_DSLLV;
        return true;
    case 0x16: /* DSRLV / DROTRV */
        if (((insn >> 6) & 0x1f) == 1) {
            ir1->op = MIPS_OP_SHIFT_V;
            ir1->subop = MIPS_SHV_DROTRV;
            return true;
        }
        ir1->op = MIPS_OP_SHIFT_V;
        ir1->subop = MIPS_SHV_DSRLV;
        return true;
    case 0x17:
        ir1->op = MIPS_OP_SHIFT_V;
        ir1->subop = MIPS_SHV_DSRAV;
        return true;
    case 0x18:
        ir1->op = MIPS_OP_MULDIV;
        ir1->subop = MIPS_MD_MULT;
        return true;
    case 0x19:
        ir1->op = MIPS_OP_MULDIV;
        ir1->subop = MIPS_MD_MULTU;
        return true;
    case 0x1a:
        ir1->op = MIPS_OP_MULDIV;
        ir1->subop = MIPS_MD_DIV;
        return true;
    case 0x1b:
        ir1->op = MIPS_OP_MULDIV;
        ir1->subop = MIPS_MD_DIVU;
        return true;
    case 0x1c:
        ir1->op = MIPS_OP_MULDIV;
        ir1->subop = MIPS_MD_DMULT;
        return true;
    case 0x1d:
        ir1->op = MIPS_OP_MULDIV;
        ir1->subop = MIPS_MD_DMULTU;
        return true;
    case 0x1e:
        ir1->op = MIPS_OP_MULDIV;
        ir1->subop = MIPS_MD_DDIV;
        return true;
    case 0x1f:
        ir1->op = MIPS_OP_MULDIV;
        ir1->subop = MIPS_MD_DDIVU;
        return true;
    case 0x20:
        return set_alu(ir1, MIPS_ALU_ADD);
    case 0x21:
        return set_alu(ir1, MIPS_ALU_ADDU);
    case 0x22:
        return set_alu(ir1, MIPS_ALU_SUB);
    case 0x23:
        return set_alu(ir1, MIPS_ALU_SUBU);
    case 0x24:
        return set_alu(ir1, MIPS_ALU_AND);
    case 0x25:
        return set_alu(ir1, MIPS_ALU_OR);
    case 0x26:
        return set_alu(ir1, MIPS_ALU_XOR);
    case 0x27:
        return set_alu(ir1, MIPS_ALU_NOR);
    case 0x2a:
        return set_alu(ir1, MIPS_ALU_SLT);
    case 0x2b:
        return set_alu(ir1, MIPS_ALU_SLTU);
    case 0x2c:
        return set_alu(ir1, MIPS_ALU_DADD);
    case 0x2d:
        return set_alu(ir1, MIPS_ALU_DADDU);
    case 0x2e:
        return set_alu(ir1, MIPS_ALU_DSUB);
    case 0x2f:
        return set_alu(ir1, MIPS_ALU_DSUBU);
    case 0x30: ir1->op = MIPS_OP_TRAP; ir1->subop = MIPS_TRAP_TGE; return true;
    case 0x31: ir1->op = MIPS_OP_TRAP; ir1->subop = MIPS_TRAP_TGEU; return true;
    case 0x32: ir1->op = MIPS_OP_TRAP; ir1->subop = MIPS_TRAP_TLT; return true;
    case 0x33: ir1->op = MIPS_OP_TRAP; ir1->subop = MIPS_TRAP_TLTU; return true;
    case 0x34: ir1->op = MIPS_OP_TRAP; ir1->subop = MIPS_TRAP_TEQ; return true;
    case 0x36: ir1->op = MIPS_OP_TRAP; ir1->subop = MIPS_TRAP_TNE; return true;
    case 0x38: /* DSLL */
        ir1->op = MIPS_OP_SHIFT; ir1->subop = MIPS_SH_DSLL; return true;
    case 0x3a: /* DSRL / DROTR */
        if (rs == 1) {
            ir1->op = MIPS_OP_SHIFT; ir1->subop = MIPS_SHV_DROTR; return true;
        }
        ir1->op = MIPS_OP_SHIFT; ir1->subop = MIPS_SH_DSRL; return true;
    case 0x3b:
        ir1->op = MIPS_OP_SHIFT; ir1->subop = MIPS_SH_DSRA; return true;
    case 0x3c:
        ir1->op = MIPS_OP_SHIFT; ir1->subop = MIPS_SH_DSLL32; return true;
    case 0x3e: /* DSRL32 / DROTR32 */
        if (rs == 1) {
            ir1->op = MIPS_OP_SHIFT; ir1->subop = MIPS_SHV_DROTR32; return true;
        }
        ir1->op = MIPS_OP_SHIFT; ir1->subop = MIPS_SH_DSRL32; return true;
    case 0x3f:
        ir1->op = MIPS_OP_SHIFT; ir1->subop = MIPS_SH_DSRA32; return true;
    default:
        return ri(ir1);
    }
}

static bool decode_regimm(MipsIR1 *ir1, uint32_t insn)
{
    unsigned rt = ir1->rt;

    (void)insn;

    switch (rt) {
    case 0x00: return set_branch(ir1, MIPS_BR_BLTZ, false, false);
    case 0x01: return set_branch(ir1, MIPS_BR_BGEZ, false, false);
    case 0x02: return set_branch(ir1, MIPS_BR_BLTZL, true, false);
    case 0x03: return set_branch(ir1, MIPS_BR_BGEZL, true, false);
    case 0x08: ir1->op = MIPS_OP_TRAP; ir1->subop = MIPS_TRAP_TGE; return true;
    case 0x09: ir1->op = MIPS_OP_TRAP; ir1->subop = MIPS_TRAP_TGEU; return true;
    case 0x0a: ir1->op = MIPS_OP_TRAP; ir1->subop = MIPS_TRAP_TLT; return true;
    case 0x0b: ir1->op = MIPS_OP_TRAP; ir1->subop = MIPS_TRAP_TLTU; return true;
    case 0x0c: ir1->op = MIPS_OP_TRAP; ir1->subop = MIPS_TRAP_TEQ; return true;
    case 0x0e: ir1->op = MIPS_OP_TRAP; ir1->subop = MIPS_TRAP_TNE; return true;
    case 0x10: return set_branch(ir1, MIPS_BR_BLTZAL, false, true);
    case 0x11: return set_branch(ir1, MIPS_BR_BGEZAL, false, true);
    case 0x12: return set_branch(ir1, MIPS_BR_BLTZALL, true, true);
    case 0x13: return set_branch(ir1, MIPS_BR_BGEZALL, true, true);
    case 0x1f:
        ir1->op = MIPS_OP_SYNCI;
        return true;
    default:
        return ri(ir1);
    }
}

static bool decode_special2(MipsIR1 *ir1, uint32_t insn)
{
    unsigned funct = insn & 0x3f;

    switch (funct) {
    case 0x00: ir1->op = MIPS_OP_MULDIV; ir1->subop = MIPS_MD_MADD; return true;
    case 0x01: ir1->op = MIPS_OP_MULDIV; ir1->subop = MIPS_MD_MADDU; return true;
    case 0x02: return set_alu(ir1, MIPS_ALU_MUL);
    case 0x04: ir1->op = MIPS_OP_MULDIV; ir1->subop = MIPS_MD_MSUB; return true;
    case 0x05: ir1->op = MIPS_OP_MULDIV; ir1->subop = MIPS_MD_MSUBU; return true;
    case 0x20: ir1->op = MIPS_OP_MULDIV; ir1->subop = MIPS_MD_CLZ; return true;
    case 0x21: ir1->op = MIPS_OP_MULDIV; ir1->subop = MIPS_MD_CLO; return true;
    case 0x24: ir1->op = MIPS_OP_MULDIV; ir1->subop = MIPS_MD_DCLZ; return true;
    case 0x25: ir1->op = MIPS_OP_MULDIV; ir1->subop = MIPS_MD_DCLO; return true;
    default:
        return ri(ir1);
    }
}

static bool decode_special3(MipsIR1 *ir1, uint32_t insn)
{
    unsigned funct = insn & 0x3f;

    switch (funct) {
    case 0x00: ir1->op = MIPS_OP_BITFIELD; ir1->subop = MIPS_BF_EXT; return true;
    case 0x01: ir1->op = MIPS_OP_BITFIELD; ir1->subop = MIPS_BF_DEXTM; return true;
    case 0x02: ir1->op = MIPS_OP_BITFIELD; ir1->subop = MIPS_BF_DEXTU; return true;
    case 0x03: ir1->op = MIPS_OP_BITFIELD; ir1->subop = MIPS_BF_DEXT; return true;
    case 0x04: ir1->op = MIPS_OP_BITFIELD; ir1->subop = MIPS_BF_INS; return true;
    case 0x05: ir1->op = MIPS_OP_BITFIELD; ir1->subop = MIPS_BF_DINSM; return true;
    case 0x06: ir1->op = MIPS_OP_BITFIELD; ir1->subop = MIPS_BF_DINSU; return true;
    case 0x07: ir1->op = MIPS_OP_BITFIELD; ir1->subop = MIPS_BF_DINS; return true;
    case 0x20: /* BSHFL */
        switch (MASK_BSHFL(insn)) {
        case OPC_WSBH: ir1->op = MIPS_OP_BSHFL; ir1->subop = MIPS_BS_WSBH; return true;
        case OPC_SEB:  ir1->op = MIPS_OP_BSHFL; ir1->subop = MIPS_BS_SEB;  return true;
        case OPC_SEH:  ir1->op = MIPS_OP_BSHFL; ir1->subop = MIPS_BS_SEH;  return true;
        default: return ri(ir1);
        }
    case 0x24: /* DBSHFL */
        switch (MASK_DBSHFL(insn)) {
        case OPC_DSBH: ir1->op = MIPS_OP_BSHFL; ir1->subop = MIPS_BS_DSBH; return true;
        case OPC_DSHD: ir1->op = MIPS_OP_BSHFL; ir1->subop = MIPS_BS_DSHD; return true;
        default: return ri(ir1);
        }
    case 0x3b: /* RDHWR */
        ir1->op = MIPS_OP_RDHWR;
        return true;
    default:
        return ri(ir1);
    }
}

static bool decode_cop0(MipsIR1 *ir1, uint32_t insn)
{
    unsigned rs = ir1->rs;

    switch (rs) {
    case 0x00: ir1->op = MIPS_OP_CP0_MOVE; ir1->subop = MIPS_CP0_MFC0; return true;
    case 0x01: ir1->op = MIPS_OP_CP0_MOVE; ir1->subop = MIPS_CP0_DMFC0; return true;
    case 0x02: ir1->op = MIPS_OP_CP0_MOVE; ir1->subop = MIPS_CP0_MFHC0; return true;
    case 0x04: ir1->op = MIPS_OP_CP0_MOVE; ir1->subop = MIPS_CP0_MTC0; return true;
    case 0x05: ir1->op = MIPS_OP_CP0_MOVE; ir1->subop = MIPS_CP0_DMTC0; return true;
    case 0x06: ir1->op = MIPS_OP_CP0_MOVE; ir1->subop = MIPS_CP0_MTHC0; return true;
    case 0x0a: ir1->op = MIPS_OP_CP0_MOVE; ir1->subop = MIPS_CP0_RDPGPR; return true;
    case 0x0e: ir1->op = MIPS_OP_CP0_MOVE; ir1->subop = MIPS_CP0_WRPGPR; return true;
    case 0x0b: /* MFMC0: DI/EI */
        switch (MASK_MFMC0(insn)) {
        case OPC_DI: ir1->op = MIPS_OP_CP0_OP; ir1->subop = MIPS_CP0OP_DI; return true;
        case OPC_EI: ir1->op = MIPS_OP_CP0_OP; ir1->subop = MIPS_CP0OP_EI; return true;
        default: return ri(ir1);
        }
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        switch (MASK_C0(insn)) {
        case OPC_TLBR:  ir1->op = MIPS_OP_TLB; ir1->subop = MIPS_TLB_TLBR; return true;
        case OPC_TLBWI: ir1->op = MIPS_OP_TLB; ir1->subop = MIPS_TLB_TLBWI; return true;
        case OPC_TLBWR: ir1->op = MIPS_OP_TLB; ir1->subop = MIPS_TLB_TLBWR; return true;
        case OPC_TLBP:  ir1->op = MIPS_OP_TLB; ir1->subop = MIPS_TLB_TLBP; return true;
        case OPC_ERET:  ir1->op = MIPS_OP_CP0_OP; ir1->subop = MIPS_CP0OP_ERET; return true;
        case OPC_DERET: ir1->op = MIPS_OP_CP0_OP; ir1->subop = MIPS_CP0OP_DERET; return true;
        case OPC_WAIT:  ir1->op = MIPS_OP_CP0_OP; ir1->subop = MIPS_CP0OP_WAIT; return true;
        default: return ri(ir1);
        }
    default:
        return ri(ir1);
    }
}

/* CP1 scalar decode.  PS and E/Q formats are rejected because the 5KEf
 * MIPS64R2 model used as the oracle has no FCR0.PS and no MIPS3D ASE. */
static bool decode_cop1(MipsIR1 *ir1, uint32_t insn)
{
    unsigned rs = ir1->rs;
    unsigned fmt = (insn >> 21) & 0x1f;
    unsigned funct = insn & 0x3f;
    unsigned ft = ir1->rt;
    unsigned fs = ir1->rd;
    unsigned fd = ir1->sa;

    switch (rs) {
    case 0x00: ir1->op = MIPS_OP_FPU_GPR; ir1->subop = MIPS_FGPR_MFC1; ir1->rs = fs; return true;
    case 0x01: ir1->op = MIPS_OP_FPU_GPR; ir1->subop = MIPS_FGPR_DMFC1; ir1->rs = fs; return true;
    case 0x02: ir1->op = MIPS_OP_FPU_GPR; ir1->subop = MIPS_FGPR_CFC1; ir1->rs = fs; return true;
    case 0x03: ir1->op = MIPS_OP_FPU_GPR; ir1->subop = MIPS_FGPR_MFHC1; ir1->rs = fs; return true;
    case 0x04: ir1->op = MIPS_OP_FPU_GPR; ir1->subop = MIPS_FGPR_MTC1; ir1->rs = fs; return true;
    case 0x05: ir1->op = MIPS_OP_FPU_GPR; ir1->subop = MIPS_FGPR_DMTC1; ir1->rs = fs; return true;
    case 0x06: ir1->op = MIPS_OP_FPU_GPR; ir1->subop = MIPS_FGPR_CTC1; ir1->rs = fs; return true;
    case 0x07: ir1->op = MIPS_OP_FPU_GPR; ir1->subop = MIPS_FGPR_MTHC1; ir1->rs = fs; return true;
    case 0x08: /* BC1: rt = {cc[2:0], likely, tf} */
        ir1->op = MIPS_OP_FPU_BC;
        ir1->cond = ft;
        ir1->likely = (ft & 2) != 0;
        ir1->link = false;
        return true;
    default:
        break;
    }

    if (fmt != 16 && fmt != 17 && fmt != 20 && fmt != 21) {
        return ri(ir1); /* PS, E, Q and reserved formats */
    }

    if (funct >= 48) {
        ir1->op = MIPS_OP_FPU_CMP;
        ir1->fmt = fmt;
        ir1->cond = funct - 48;
        ir1->rs = fs;
        ir1->rt = ft;
        ir1->rd = fd;
        return true;
    }

    switch (funct) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
    case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:
        ir1->op = MIPS_OP_FPU_ARITH;
        ir1->fmt = fmt;
        ir1->subop = funct;
        ir1->rs = fs;
        ir1->rt = ft;
        ir1->rd = fd;
        return true;
    case 17: /* MOVF/MOVT */
        ir1->op = MIPS_OP_FPU_MOVC;
        ir1->fmt = fmt;
        ir1->cond = (ft >> 2) & 7;
        ir1->sa = ft & 1;
        ir1->rs = fs;
        ir1->rd = fd;
        return true;
    case 18: case 19: /* MOVZ/MOVN */
        ir1->op = MIPS_OP_FPU_MOVE;
        ir1->fmt = fmt;
        ir1->subop = funct == 18 ? MIPS_FMOVE_MOVZ : MIPS_FMOVE_MOVN;
        ir1->rs = fs;
        ir1->rt = ft;
        ir1->rd = fd;
        return true;
    case 21: case 22: /* RECIP/RSQRT */
        ir1->op = MIPS_OP_FPU_ARITH;
        ir1->fmt = fmt;
        ir1->subop = funct;
        ir1->rs = fs;
        ir1->rd = fd;
        return true;
    case 28: case 29: case 30: case 31: /* RECIP2/RECIP1/RSQRT1/RSQRT2 */
        ir1->op = MIPS_OP_FPU_ARITH;
        ir1->fmt = fmt;
        ir1->subop = funct;
        ir1->rs = fs;
        ir1->rt = ft;
        ir1->rd = fd;
        return true;
    case 32: case 33: case 36: case 37: case 38:
        ir1->op = MIPS_OP_FPU_CVT;
        ir1->fmt = fmt;
        ir1->subop = funct;
        ir1->rs = fs;
        ir1->rd = fd;
        return true;
    default:
        return ri(ir1);
    }
}

static bool decode_cop1x(MipsIR1 *ir1, uint32_t insn)
{
    unsigned funct = insn & 0x3f;
    unsigned fr = (insn >> 21) & 0x1f;
    unsigned ft = ir1->rt;
    unsigned fs = ir1->rd;
    unsigned fd = ir1->sa;

    switch (funct) {
    case 0x00: /* LWXC1 */
    case 0x01: /* LDXC1 */
    case 0x05: /* LUXC1 */
    case 0x08: /* SWXC1 */
    case 0x09: /* SDXC1 */
    case 0x0d: /* SUXC1 */
        ir1->op = MIPS_OP_FPU_LDST;
        ir1->subop = funct == 0x00 ? MIPS_FLDST_LWXC1 :
                     funct == 0x01 ? MIPS_FLDST_LWXC1 /* LDXC1 */ :
                     funct == 0x05 ? MIPS_FLDST_LUXC1 :
                     funct == 0x08 ? MIPS_FLDST_SWXC1 :
                     funct == 0x09 ? MIPS_FLDST_SWXC1 /* SDXC1 */ :
                                     MIPS_FLDST_SUXC1;
        ir1->rs = fr;      /* base GPR */
        ir1->rt = ft;      /* index GPR */
        ir1->rd = fd;      /* load destination / unused for store */
        ir1->sa = fs;      /* store source */
        ir1->sel = (uint8_t)funct;
        return true;
    case 0x0f: /* PREFX */
        ir1->op = MIPS_OP_PREF;
        ir1->rs = fr;      /* base GPR */
        ir1->rt = ft;      /* index GPR */
        return true;
    case 0x20: case 0x21: case 0x28: case 0x29:
    case 0x30: case 0x31: case 0x38: case 0x39:
        if (funct == 0x21 || funct == 0x29 || funct == 0x31 || funct == 0x39) {
            /* Raw CP1 format fields, matching decode_cop1(). */
            ir1->fmt = 17;
        } else {
            ir1->fmt = 16;
        }
        ir1->op = MIPS_OP_FPU_MADD;
        ir1->subop = funct == 0x20 || funct == 0x21 ? MIPS_FMADD_MADD :
                     funct == 0x28 || funct == 0x29 ? MIPS_FMADD_MSUB :
                     funct == 0x30 || funct == 0x31 ? MIPS_FMADD_NMADD :
                                                    MIPS_FMADD_NMSUB;
        ir1->rs = fs;
        ir1->rt = ft;
        ir1->rd = fd;
        ir1->sa = fr;
        return true;
    default:
        return ri(ir1);
    }
}

static bool decode_primary(MipsIR1 *ir1, uint32_t insn)
{
    unsigned opcode = insn >> 26;

    switch (opcode) {
    case 0x00: return decode_special(ir1, insn);
    case 0x01: return decode_regimm(ir1, insn);
    case 0x02:
        ir1->op = MIPS_OP_JUMP; ir1->subop = MIPS_JP_J; return true;
    case 0x03:
        ir1->op = MIPS_OP_JUMP; ir1->subop = MIPS_JP_JAL;
        ir1->link = true; return true;
    case 0x04: return set_branch(ir1, MIPS_BR_BEQ, false, false);
    case 0x05: return set_branch(ir1, MIPS_BR_BNE, false, false);
    case 0x06: return set_branch(ir1, MIPS_BR_BLEZ, false, false);
    case 0x07: return set_branch(ir1, MIPS_BR_BGTZ, false, false);
    case 0x08: return set_imm(ir1, MIPS_IMM_ADDI);
    case 0x09: return set_imm(ir1, MIPS_IMM_ADDIU);
    case 0x0a: return set_imm(ir1, MIPS_IMM_SLTI);
    case 0x0b: return set_imm(ir1, MIPS_IMM_SLTIU);
    case 0x0c: return set_imm(ir1, MIPS_IMM_ANDI);
    case 0x0d: return set_imm(ir1, MIPS_IMM_ORI);
    case 0x0e: return set_imm(ir1, MIPS_IMM_XORI);
    case 0x0f: return set_imm(ir1, MIPS_IMM_LUI);
    case 0x10: return decode_cop0(ir1, insn);
    case 0x11: return decode_cop1(ir1, insn);
    case 0x12: return ri(ir1); /* COP2 */
    case 0x13: return decode_cop1x(ir1, insn);
    case 0x14: return set_branch(ir1, MIPS_BR_BEQL, true, false);
    case 0x15: return set_branch(ir1, MIPS_BR_BNEL, true, false);
    case 0x16: return set_branch(ir1, MIPS_BR_BLEZL, true, false);
    case 0x17: return set_branch(ir1, MIPS_BR_BGTZL, true, false);
    case 0x18: return set_imm(ir1, MIPS_IMM_DADDI);
    case 0x19: return set_imm(ir1, MIPS_IMM_DADDIU);
    case 0x1a: ir1->op = MIPS_OP_LOAD; ir1->subop = MIPS_MEM_LDL; return true;
    case 0x1b: ir1->op = MIPS_OP_LOAD; ir1->subop = MIPS_MEM_LDR; return true;
    case 0x1c: return decode_special2(ir1, insn);
    case 0x1d: return ri(ir1); /* JALX / DAUI (MIPS16/R6) */
    case 0x1e: return ri(ir1); /* MDMX */
    case 0x1f: return decode_special3(ir1, insn);
    case 0x20: ir1->op = MIPS_OP_LOAD; ir1->subop = MIPS_MEM_LB; return true;
    case 0x21: ir1->op = MIPS_OP_LOAD; ir1->subop = MIPS_MEM_LH; return true;
    case 0x22: ir1->op = MIPS_OP_LOAD; ir1->subop = MIPS_MEM_LWL; return true;
    case 0x23: ir1->op = MIPS_OP_LOAD; ir1->subop = MIPS_MEM_LW; return true;
    case 0x24: ir1->op = MIPS_OP_LOAD; ir1->subop = MIPS_MEM_LBU; return true;
    case 0x25: ir1->op = MIPS_OP_LOAD; ir1->subop = MIPS_MEM_LHU; return true;
    case 0x26: ir1->op = MIPS_OP_LOAD; ir1->subop = MIPS_MEM_LWR; return true;
    case 0x27: ir1->op = MIPS_OP_LOAD; ir1->subop = MIPS_MEM_LWU; return true;
    case 0x28: ir1->op = MIPS_OP_STORE; ir1->subop = MIPS_MEM_SB; return true;
    case 0x29: ir1->op = MIPS_OP_STORE; ir1->subop = MIPS_MEM_SH; return true;
    case 0x2a: ir1->op = MIPS_OP_STORE; ir1->subop = MIPS_MEM_SWL; return true;
    case 0x2b: ir1->op = MIPS_OP_STORE; ir1->subop = MIPS_MEM_SW; return true;
    case 0x2c: ir1->op = MIPS_OP_STORE; ir1->subop = MIPS_MEM_SDL; return true;
    case 0x2d: ir1->op = MIPS_OP_STORE; ir1->subop = MIPS_MEM_SDR; return true;
    case 0x2e: ir1->op = MIPS_OP_STORE; ir1->subop = MIPS_MEM_SWR; return true;
    case 0x2f: ir1->op = MIPS_OP_CACHE; return true;
    case 0x30: ir1->op = MIPS_OP_LOAD; ir1->subop = MIPS_MEM_LL; return true;
    case 0x31: ir1->op = MIPS_OP_FPU_LDST; ir1->subop = MIPS_FLDST_LWC1; return true;
    case 0x33: ir1->op = MIPS_OP_PREF; return true;
    case 0x34: ir1->op = MIPS_OP_LOAD; ir1->subop = MIPS_MEM_LLD; return true;
    case 0x35: ir1->op = MIPS_OP_FPU_LDST; ir1->subop = MIPS_FLDST_LDC1; return true;
    case 0x37: ir1->op = MIPS_OP_LOAD; ir1->subop = MIPS_MEM_LD; return true;
    case 0x38: ir1->op = MIPS_OP_STORE; ir1->subop = MIPS_MEM_SC; return true;
    case 0x39: ir1->op = MIPS_OP_FPU_LDST; ir1->subop = MIPS_FLDST_SWC1; return true;
    case 0x3c: ir1->op = MIPS_OP_STORE; ir1->subop = MIPS_MEM_SCD; return true;
    case 0x3d: ir1->op = MIPS_OP_FPU_LDST; ir1->subop = MIPS_FLDST_SDC1; return true;
    case 0x3f: ir1->op = MIPS_OP_STORE; ir1->subop = MIPS_MEM_SD; return true;
    default:
        return ri(ir1);
    }
}

bool mips_decode_one(MipsIR1 *ir1, uint32_t insn, uint64_t pc)
{
    mips_ir1_init(ir1, insn, pc);
    return decode_primary(ir1, insn);
}

bool mips_ir1_is_branch(const MipsIR1 *ir1)
{
    return ir1->op == MIPS_OP_BRANCH || ir1->op == MIPS_OP_JUMP ||
           ir1->op == MIPS_OP_FPU_BC;
}
