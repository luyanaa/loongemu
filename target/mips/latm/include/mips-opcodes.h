/*
 * MIPS opcode constants, copied from QEMU target/mips/tcg/translate.c.
 * Copyright (c) 2004-2020 QEMU MIPS contributors; LGPL-2.1-or-later.
 */
#ifndef LATM_MIPS_OPCODES_H
#define LATM_MIPS_OPCODES_H

#define OPC_CP1 (0x11 << 26)
#define MASK_OP_MAJOR(op) ((op) & (0x3F << 26))

enum {
    /* indirect opcode tables */
    OPC_SPECIAL  = (0x00 << 26),
    OPC_REGIMM   = (0x01 << 26),
    OPC_CP0      = (0x10 << 26),
    OPC_CP2      = (0x12 << 26),
    OPC_CP3      = (0x13 << 26),
    OPC_SPECIAL2 = (0x1C << 26),
    OPC_SPECIAL3 = (0x1F << 26),
    /* arithmetic with immediate */
    OPC_ADDI     = (0x08 << 26),
    OPC_ADDIU    = (0x09 << 26),
    OPC_SLTI     = (0x0A << 26),
    OPC_SLTIU    = (0x0B << 26),
    /* logic with immediate */
    OPC_ANDI     = (0x0C << 26),
    OPC_ORI      = (0x0D << 26),
    OPC_XORI     = (0x0E << 26),
    OPC_LUI      = (0x0F << 26),
    /* arithmetic with immediate */
    OPC_DADDI    = (0x18 << 26),
    OPC_DADDIU   = (0x19 << 26),
    /* Jump and branches */
    OPC_J        = (0x02 << 26),
    OPC_JAL      = (0x03 << 26),
    OPC_BEQ      = (0x04 << 26),  /* Unconditional if rs = rt = 0 (B) */
    OPC_BEQL     = (0x14 << 26),
    OPC_BNE      = (0x05 << 26),
    OPC_BNEL     = (0x15 << 26),
    OPC_BLEZ     = (0x06 << 26),
    OPC_BLEZL    = (0x16 << 26),
    OPC_BGTZ     = (0x07 << 26),
    OPC_BGTZL    = (0x17 << 26),
    OPC_JALX     = (0x1D << 26),
    OPC_DAUI     = (0x1D << 26),
    /* Load and stores */
    OPC_LDL      = (0x1A << 26),
    OPC_LDR      = (0x1B << 26),
    OPC_LB       = (0x20 << 26),
    OPC_LH       = (0x21 << 26),
    OPC_LWL      = (0x22 << 26),
    OPC_LW       = (0x23 << 26),
    OPC_LWPC     = OPC_LW | 0x5,
    OPC_LBU      = (0x24 << 26),
    OPC_LHU      = (0x25 << 26),
    OPC_LWR      = (0x26 << 26),
    OPC_LWU      = (0x27 << 26),
    OPC_SB       = (0x28 << 26),
    OPC_SH       = (0x29 << 26),
    OPC_SWL      = (0x2A << 26),
    OPC_SW       = (0x2B << 26),
    OPC_SDL      = (0x2C << 26),
    OPC_SDR      = (0x2D << 26),
    OPC_SWR      = (0x2E << 26),
    OPC_LL       = (0x30 << 26),
    OPC_LLD      = (0x34 << 26),
    OPC_LD       = (0x37 << 26),
    OPC_LDPC     = OPC_LD | 0x5,
    OPC_SC       = (0x38 << 26),
    OPC_SCD      = (0x3C << 26),
    OPC_SD       = (0x3F << 26),
    /* Floating point load/store */
    OPC_LWC1     = (0x31 << 26),
    OPC_LWC2     = (0x32 << 26),
    OPC_LDC1     = (0x35 << 26),
    OPC_LDC2     = (0x36 << 26),
    OPC_SWC1     = (0x39 << 26),
    OPC_SWC2     = (0x3A << 26),
    OPC_SDC1     = (0x3D << 26),
    OPC_SDC2     = (0x3E << 26),
    /* Compact Branches */
    OPC_BLEZALC  = (0x06 << 26),
    OPC_BGEZALC  = (0x06 << 26),
    OPC_BGEUC    = (0x06 << 26),
    OPC_BGTZALC  = (0x07 << 26),
    OPC_BLTZALC  = (0x07 << 26),
    OPC_BLTUC    = (0x07 << 26),
    OPC_BOVC     = (0x08 << 26),
    OPC_BEQZALC  = (0x08 << 26),
    OPC_BEQC     = (0x08 << 26),
    OPC_BLEZC    = (0x16 << 26),
    OPC_BGEZC    = (0x16 << 26),
    OPC_BGEC     = (0x16 << 26),
    OPC_BGTZC    = (0x17 << 26),
    OPC_BLTZC    = (0x17 << 26),
    OPC_BLTC     = (0x17 << 26),
    OPC_BNVC     = (0x18 << 26),
    OPC_BNEZALC  = (0x18 << 26),
    OPC_BNEC     = (0x18 << 26),
    OPC_BC       = (0x32 << 26),
    OPC_BEQZC    = (0x36 << 26),
    OPC_JIC      = (0x36 << 26),
    OPC_BALC     = (0x3A << 26),
    OPC_BNEZC    = (0x3E << 26),
    OPC_JIALC    = (0x3E << 26),
    /* MDMX ASE specific */
    OPC_MDMX     = (0x1E << 26),
    /* Cache and prefetch */
    OPC_CACHE    = (0x2F << 26),
    OPC_PREF     = (0x33 << 26),
    /* PC-relative address computation / loads */
    OPC_PCREL    = (0x3B << 26),
};

/* PC-relative address computation / loads  */
#define MASK_OPC_PCREL_TOP2BITS(op) (MASK_OP_MAJOR(op) | (op & (3 << 19)))
#define MASK_OPC_PCREL_TOP5BITS(op) (MASK_OP_MAJOR(op) | (op & (0x1f << 16)))
enum {
    /* Instructions determined by bits 19 and 20 */
    OPC_ADDIUPC = OPC_PCREL | (0 << 19),
    R6_OPC_LWPC = OPC_PCREL | (1 << 19),
    OPC_LWUPC   = OPC_PCREL | (2 << 19),

    /* Instructions determined by bits 16 ... 20 */
    OPC_AUIPC   = OPC_PCREL | (0x1e << 16),
    OPC_ALUIPC  = OPC_PCREL | (0x1f << 16),

    /* Other */
    R6_OPC_LDPC = OPC_PCREL | (6 << 18),
};

/* MIPS special opcodes */
#define MASK_SPECIAL(op)            (MASK_OP_MAJOR(op) | (op & 0x3F))

enum {
    /* Shifts */
    OPC_SLL      = 0x00 | OPC_SPECIAL,
    /* NOP is SLL r0, r0, 0   */
    /* SSNOP is SLL r0, r0, 1 */
    /* EHB is SLL r0, r0, 3 */
    OPC_SRL      = 0x02 | OPC_SPECIAL, /* also ROTR */
    OPC_ROTR     = OPC_SRL | (1 << 21),
    OPC_SRA      = 0x03 | OPC_SPECIAL,
    OPC_SLLV     = 0x04 | OPC_SPECIAL,
    OPC_SRLV     = 0x06 | OPC_SPECIAL, /* also ROTRV */
    OPC_ROTRV    = OPC_SRLV | (1 << 6),
    OPC_SRAV     = 0x07 | OPC_SPECIAL,
    OPC_DSLLV    = 0x14 | OPC_SPECIAL,
    OPC_DSRLV    = 0x16 | OPC_SPECIAL, /* also DROTRV */
    OPC_DROTRV   = OPC_DSRLV | (1 << 6),
    OPC_DSRAV    = 0x17 | OPC_SPECIAL,
    OPC_DSLL     = 0x38 | OPC_SPECIAL,
    OPC_DSRL     = 0x3A | OPC_SPECIAL, /* also DROTR */
    OPC_DROTR    = OPC_DSRL | (1 << 21),
    OPC_DSRA     = 0x3B | OPC_SPECIAL,
    OPC_DSLL32   = 0x3C | OPC_SPECIAL,
    OPC_DSRL32   = 0x3E | OPC_SPECIAL, /* also DROTR32 */
    OPC_DROTR32  = OPC_DSRL32 | (1 << 21),
    OPC_DSRA32   = 0x3F | OPC_SPECIAL,
    /* Multiplication / division */
    OPC_MULT     = 0x18 | OPC_SPECIAL,
    OPC_MULTU    = 0x19 | OPC_SPECIAL,
    OPC_DIV      = 0x1A | OPC_SPECIAL,
    OPC_DIVU     = 0x1B | OPC_SPECIAL,
    OPC_DMULT    = 0x1C | OPC_SPECIAL,
    OPC_DMULTU   = 0x1D | OPC_SPECIAL,
    OPC_DDIV     = 0x1E | OPC_SPECIAL,
    OPC_DDIVU    = 0x1F | OPC_SPECIAL,

    /* 2 registers arithmetic / logic */
    OPC_ADD      = 0x20 | OPC_SPECIAL,
    OPC_ADDU     = 0x21 | OPC_SPECIAL,
    OPC_SUB      = 0x22 | OPC_SPECIAL,
    OPC_SUBU     = 0x23 | OPC_SPECIAL,
    OPC_AND      = 0x24 | OPC_SPECIAL,
    OPC_OR       = 0x25 | OPC_SPECIAL,
    OPC_XOR      = 0x26 | OPC_SPECIAL,
    OPC_NOR      = 0x27 | OPC_SPECIAL,
    OPC_SLT      = 0x2A | OPC_SPECIAL,
    OPC_SLTU     = 0x2B | OPC_SPECIAL,
    OPC_DADD     = 0x2C | OPC_SPECIAL,
    OPC_DADDU    = 0x2D | OPC_SPECIAL,
    OPC_DSUB     = 0x2E | OPC_SPECIAL,
    OPC_DSUBU    = 0x2F | OPC_SPECIAL,
    /* Jumps */
    OPC_JR       = 0x08 | OPC_SPECIAL, /* Also JR.HB */
    OPC_JALR     = 0x09 | OPC_SPECIAL, /* Also JALR.HB */
    /* Traps */
    OPC_TGE      = 0x30 | OPC_SPECIAL,
    OPC_TGEU     = 0x31 | OPC_SPECIAL,
    OPC_TLT      = 0x32 | OPC_SPECIAL,
    OPC_TLTU     = 0x33 | OPC_SPECIAL,
    OPC_TEQ      = 0x34 | OPC_SPECIAL,
    OPC_TNE      = 0x36 | OPC_SPECIAL,
    /* HI / LO registers load & stores */
    OPC_MFHI     = 0x10 | OPC_SPECIAL,
    OPC_MTHI     = 0x11 | OPC_SPECIAL,
    OPC_MFLO     = 0x12 | OPC_SPECIAL,
    OPC_MTLO     = 0x13 | OPC_SPECIAL,
    /* Conditional moves */
    OPC_MOVZ     = 0x0A | OPC_SPECIAL,
    OPC_MOVN     = 0x0B | OPC_SPECIAL,

    OPC_SELEQZ   = 0x35 | OPC_SPECIAL,
    OPC_SELNEZ   = 0x37 | OPC_SPECIAL,

    OPC_MOVCI    = 0x01 | OPC_SPECIAL,

    /* Special */
    OPC_PMON     = 0x05 | OPC_SPECIAL, /* unofficial */
    OPC_SYSCALL  = 0x0C | OPC_SPECIAL,
    OPC_BREAK    = 0x0D | OPC_SPECIAL,
    OPC_SPIM     = 0x0E | OPC_SPECIAL, /* unofficial */
    OPC_SYNC     = 0x0F | OPC_SPECIAL,

    OPC_SPECIAL28_RESERVED = 0x28 | OPC_SPECIAL,
    OPC_SPECIAL29_RESERVED = 0x29 | OPC_SPECIAL,
    OPC_SPECIAL39_RESERVED = 0x39 | OPC_SPECIAL,
    OPC_SPECIAL3D_RESERVED = 0x3D | OPC_SPECIAL,
};

/*
 * R6 Multiply and Divide instructions have the same opcode
 * and function field as legacy OPC_MULT[U]/OPC_DIV[U]
 */
#define MASK_R6_MULDIV(op)          (MASK_SPECIAL(op) | (op & (0x7ff)))

enum {
    R6_OPC_MUL   = OPC_MULT  | (2 << 6),
    R6_OPC_MUH   = OPC_MULT  | (3 << 6),
    R6_OPC_MULU  = OPC_MULTU | (2 << 6),
    R6_OPC_MUHU  = OPC_MULTU | (3 << 6),
    R6_OPC_DIV   = OPC_DIV   | (2 << 6),
    R6_OPC_MOD   = OPC_DIV   | (3 << 6),
    R6_OPC_DIVU  = OPC_DIVU  | (2 << 6),
    R6_OPC_MODU  = OPC_DIVU  | (3 << 6),

    R6_OPC_DMUL   = OPC_DMULT  | (2 << 6),
    R6_OPC_DMUH   = OPC_DMULT  | (3 << 6),
    R6_OPC_DMULU  = OPC_DMULTU | (2 << 6),
    R6_OPC_DMUHU  = OPC_DMULTU | (3 << 6),
    R6_OPC_DDIV   = OPC_DDIV   | (2 << 6),
    R6_OPC_DMOD   = OPC_DDIV   | (3 << 6),
    R6_OPC_DDIVU  = OPC_DDIVU  | (2 << 6),
    R6_OPC_DMODU  = OPC_DDIVU  | (3 << 6),

    R6_OPC_CLZ      = 0x10 | OPC_SPECIAL,
    R6_OPC_CLO      = 0x11 | OPC_SPECIAL,
    R6_OPC_DCLZ     = 0x12 | OPC_SPECIAL,
    R6_OPC_DCLO     = 0x13 | OPC_SPECIAL,
    R6_OPC_SDBBP    = 0x0e | OPC_SPECIAL,
};

/* REGIMM (rt field) opcodes */
#define MASK_REGIMM(op)             (MASK_OP_MAJOR(op) | (op & (0x1F << 16)))

enum {
    OPC_BLTZ     = (0x00 << 16) | OPC_REGIMM,
    OPC_BLTZL    = (0x02 << 16) | OPC_REGIMM,
    OPC_BGEZ     = (0x01 << 16) | OPC_REGIMM,
    OPC_BGEZL    = (0x03 << 16) | OPC_REGIMM,
    OPC_BLTZAL   = (0x10 << 16) | OPC_REGIMM,
    OPC_BLTZALL  = (0x12 << 16) | OPC_REGIMM,
    OPC_BGEZAL   = (0x11 << 16) | OPC_REGIMM,
    OPC_BGEZALL  = (0x13 << 16) | OPC_REGIMM,
    OPC_TGEI     = (0x08 << 16) | OPC_REGIMM,
    OPC_TGEIU    = (0x09 << 16) | OPC_REGIMM,
    OPC_TLTI     = (0x0A << 16) | OPC_REGIMM,
    OPC_TLTIU    = (0x0B << 16) | OPC_REGIMM,
    OPC_TEQI     = (0x0C << 16) | OPC_REGIMM,
    OPC_TNEI     = (0x0E << 16) | OPC_REGIMM,
    OPC_SIGRIE   = (0x17 << 16) | OPC_REGIMM,
    OPC_SYNCI    = (0x1F << 16) | OPC_REGIMM,

    OPC_DAHI     = (0x06 << 16) | OPC_REGIMM,
    OPC_DATI     = (0x1e << 16) | OPC_REGIMM,
};

/* Special2 opcodes */
#define MASK_SPECIAL2(op)           (MASK_OP_MAJOR(op) | (op & 0x3F))

enum {
    /* Multiply & xxx operations */
    OPC_MADD     = 0x00 | OPC_SPECIAL2,
    OPC_MADDU    = 0x01 | OPC_SPECIAL2,
    OPC_MUL      = 0x02 | OPC_SPECIAL2,
    OPC_MSUB     = 0x04 | OPC_SPECIAL2,
    OPC_MSUBU    = 0x05 | OPC_SPECIAL2,
    /* Loongson 2F */
    OPC_MULT_G_2F   = 0x10 | OPC_SPECIAL2,
    OPC_DMULT_G_2F  = 0x11 | OPC_SPECIAL2,
    OPC_MULTU_G_2F  = 0x12 | OPC_SPECIAL2,
    OPC_DMULTU_G_2F = 0x13 | OPC_SPECIAL2,
    OPC_DIV_G_2F    = 0x14 | OPC_SPECIAL2,
    OPC_DDIV_G_2F   = 0x15 | OPC_SPECIAL2,
    OPC_DIVU_G_2F   = 0x16 | OPC_SPECIAL2,
    OPC_DDIVU_G_2F  = 0x17 | OPC_SPECIAL2,
    OPC_MOD_G_2F    = 0x1c | OPC_SPECIAL2,
    OPC_DMOD_G_2F   = 0x1d | OPC_SPECIAL2,
    OPC_MODU_G_2F   = 0x1e | OPC_SPECIAL2,
    OPC_DMODU_G_2F  = 0x1f | OPC_SPECIAL2,
    /* Misc */
    OPC_CLZ      = 0x20 | OPC_SPECIAL2,
    OPC_CLO      = 0x21 | OPC_SPECIAL2,
    OPC_DCLZ     = 0x24 | OPC_SPECIAL2,
    OPC_DCLO     = 0x25 | OPC_SPECIAL2,
    /* Special */
    OPC_SDBBP    = 0x3F | OPC_SPECIAL2,
};

/* Special3 opcodes */
#define MASK_SPECIAL3(op)           (MASK_OP_MAJOR(op) | (op & 0x3F))

enum {
    OPC_EXT      = 0x00 | OPC_SPECIAL3,
    OPC_DEXTM    = 0x01 | OPC_SPECIAL3,
    OPC_DEXTU    = 0x02 | OPC_SPECIAL3,
    OPC_DEXT     = 0x03 | OPC_SPECIAL3,
    OPC_INS      = 0x04 | OPC_SPECIAL3,
    OPC_DINSM    = 0x05 | OPC_SPECIAL3,
    OPC_DINSU    = 0x06 | OPC_SPECIAL3,
    OPC_DINS     = 0x07 | OPC_SPECIAL3,
    OPC_FORK     = 0x08 | OPC_SPECIAL3,
    OPC_YIELD    = 0x09 | OPC_SPECIAL3,
    OPC_BSHFL    = 0x20 | OPC_SPECIAL3,
    OPC_DBSHFL   = 0x24 | OPC_SPECIAL3,
    OPC_RDHWR    = 0x3B | OPC_SPECIAL3,
    OPC_GINV     = 0x3D | OPC_SPECIAL3,

    /* Loongson 2E */
    OPC_MULT_G_2E   = 0x18 | OPC_SPECIAL3,
    OPC_MULTU_G_2E  = 0x19 | OPC_SPECIAL3,
    OPC_DIV_G_2E    = 0x1A | OPC_SPECIAL3,
    OPC_DIVU_G_2E   = 0x1B | OPC_SPECIAL3,
    OPC_DMULT_G_2E  = 0x1C | OPC_SPECIAL3,
    OPC_DMULTU_G_2E = 0x1D | OPC_SPECIAL3,
    OPC_DDIV_G_2E   = 0x1E | OPC_SPECIAL3,
    OPC_DDIVU_G_2E  = 0x1F | OPC_SPECIAL3,
    OPC_MOD_G_2E    = 0x22 | OPC_SPECIAL3,
    OPC_MODU_G_2E   = 0x23 | OPC_SPECIAL3,
    OPC_DMOD_G_2E   = 0x26 | OPC_SPECIAL3,
    OPC_DMODU_G_2E  = 0x27 | OPC_SPECIAL3,

    /* MIPS DSP Load */
    OPC_LX_DSP         = 0x0A | OPC_SPECIAL3,
    /* MIPS DSP Arithmetic */
    OPC_ADDU_QB_DSP    = 0x10 | OPC_SPECIAL3,
    OPC_ADDU_OB_DSP    = 0x14 | OPC_SPECIAL3,
    OPC_ABSQ_S_PH_DSP  = 0x12 | OPC_SPECIAL3,
    OPC_ABSQ_S_QH_DSP  = 0x16 | OPC_SPECIAL3,
    /* OPC_ADDUH_QB_DSP is same as OPC_MULT_G_2E.  */
    /* OPC_ADDUH_QB_DSP   = 0x18 | OPC_SPECIAL3,  */
    OPC_CMPU_EQ_QB_DSP = 0x11 | OPC_SPECIAL3,
    OPC_CMPU_EQ_OB_DSP = 0x15 | OPC_SPECIAL3,
    /* MIPS DSP GPR-Based Shift Sub-class */
    OPC_SHLL_QB_DSP    = 0x13 | OPC_SPECIAL3,
    OPC_SHLL_OB_DSP    = 0x17 | OPC_SPECIAL3,
    /* MIPS DSP Multiply Sub-class insns */
    /* OPC_MUL_PH_DSP is same as OPC_ADDUH_QB_DSP.  */
    /* OPC_MUL_PH_DSP     = 0x18 | OPC_SPECIAL3,  */
    OPC_DPA_W_PH_DSP   = 0x30 | OPC_SPECIAL3,
    OPC_DPAQ_W_QH_DSP  = 0x34 | OPC_SPECIAL3,
    /* DSP Bit/Manipulation Sub-class */
    OPC_INSV_DSP       = 0x0C | OPC_SPECIAL3,
    OPC_DINSV_DSP      = 0x0D | OPC_SPECIAL3,
    /* MIPS DSP Append Sub-class */
    OPC_APPEND_DSP     = 0x31 | OPC_SPECIAL3,
    OPC_DAPPEND_DSP    = 0x35 | OPC_SPECIAL3,
    /* MIPS DSP Accumulator and DSPControl Access Sub-class */
    OPC_EXTR_W_DSP     = 0x38 | OPC_SPECIAL3,
    OPC_DEXTR_W_DSP    = 0x3C | OPC_SPECIAL3,

    /* EVA */
    OPC_LWLE           = 0x19 | OPC_SPECIAL3,
    OPC_LWRE           = 0x1A | OPC_SPECIAL3,
    OPC_CACHEE         = 0x1B | OPC_SPECIAL3,
    OPC_SBE            = 0x1C | OPC_SPECIAL3,
    OPC_SHE            = 0x1D | OPC_SPECIAL3,
    OPC_SCE            = 0x1E | OPC_SPECIAL3,
    OPC_SWE            = 0x1F | OPC_SPECIAL3,
    OPC_SWLE           = 0x21 | OPC_SPECIAL3,
    OPC_SWRE           = 0x22 | OPC_SPECIAL3,
    OPC_PREFE          = 0x23 | OPC_SPECIAL3,
    OPC_LBUE           = 0x28 | OPC_SPECIAL3,
    OPC_LHUE           = 0x29 | OPC_SPECIAL3,
    OPC_LBE            = 0x2C | OPC_SPECIAL3,
    OPC_LHE            = 0x2D | OPC_SPECIAL3,
    OPC_LLE            = 0x2E | OPC_SPECIAL3,
    OPC_LWE            = 0x2F | OPC_SPECIAL3,

    /* R6 */
    R6_OPC_PREF        = 0x35 | OPC_SPECIAL3,
    R6_OPC_CACHE       = 0x25 | OPC_SPECIAL3,
    R6_OPC_LL          = 0x36 | OPC_SPECIAL3,
    R6_OPC_SC          = 0x26 | OPC_SPECIAL3,
    R6_OPC_LLD         = 0x37 | OPC_SPECIAL3,
    R6_OPC_SCD         = 0x27 | OPC_SPECIAL3,
};

/* Loongson EXT load/store quad word opcodes */
#define MASK_LOONGSON_GSLSQ(op)           (MASK_OP_MAJOR(op) | (op & 0x8020))
enum {
    OPC_GSLQ        = 0x0020 | OPC_LWC2,
    OPC_GSLQC1      = 0x8020 | OPC_LWC2,
    OPC_GSSHFL      = OPC_LWC2,
    OPC_GSSQ        = 0x0020 | OPC_SWC2,
    OPC_GSSQC1      = 0x8020 | OPC_SWC2,
    OPC_GSSHFS      = OPC_SWC2,
};

/* Loongson EXT shifted load/store opcodes */
#define MASK_LOONGSON_GSSHFLS(op)         (MASK_OP_MAJOR(op) | (op & 0xc03f))
enum {
    OPC_GSLWLC1     = 0x4 | OPC_GSSHFL,
    OPC_GSLWRC1     = 0x5 | OPC_GSSHFL,
    OPC_GSLDLC1     = 0x6 | OPC_GSSHFL,
    OPC_GSLDRC1     = 0x7 | OPC_GSSHFL,
    OPC_GSSWLC1     = 0x4 | OPC_GSSHFS,
    OPC_GSSWRC1     = 0x5 | OPC_GSSHFS,
    OPC_GSSDLC1     = 0x6 | OPC_GSSHFS,
    OPC_GSSDRC1     = 0x7 | OPC_GSSHFS,
};

/* Loongson EXT LDC2/SDC2 opcodes */
#define MASK_LOONGSON_LSDC2(op)           (MASK_OP_MAJOR(op) | (op & 0x7))

enum {
    OPC_GSLBX      = 0x0 | OPC_LDC2,
    OPC_GSLHX      = 0x1 | OPC_LDC2,
    OPC_GSLWX      = 0x2 | OPC_LDC2,
    OPC_GSLDX      = 0x3 | OPC_LDC2,
    OPC_GSLWXC1    = 0x6 | OPC_LDC2,
    OPC_GSLDXC1    = 0x7 | OPC_LDC2,
    OPC_GSSBX      = 0x0 | OPC_SDC2,
    OPC_GSSHX      = 0x1 | OPC_SDC2,
    OPC_GSSWX      = 0x2 | OPC_SDC2,
    OPC_GSSDX      = 0x3 | OPC_SDC2,
    OPC_GSSWXC1    = 0x6 | OPC_SDC2,
    OPC_GSSDXC1    = 0x7 | OPC_SDC2,
};

/* BSHFL opcodes */
#define MASK_BSHFL(op)              (MASK_SPECIAL3(op) | (op & (0x1F << 6)))

enum {
    OPC_WSBH      = (0x02 << 6) | OPC_BSHFL,
    OPC_SEB       = (0x10 << 6) | OPC_BSHFL,
    OPC_SEH       = (0x18 << 6) | OPC_BSHFL,
    OPC_ALIGN     = (0x08 << 6) | OPC_BSHFL, /* 010.bp (010.00 to 010.11) */
    OPC_ALIGN_1   = (0x09 << 6) | OPC_BSHFL,
    OPC_ALIGN_2   = (0x0A << 6) | OPC_BSHFL,
    OPC_ALIGN_3   = (0x0B << 6) | OPC_BSHFL,
    OPC_BITSWAP   = (0x00 << 6) | OPC_BSHFL  /* 00000 */
};

/* DBSHFL opcodes */
#define MASK_DBSHFL(op)             (MASK_SPECIAL3(op) | (op & (0x1F << 6)))

enum {
    OPC_DSBH       = (0x02 << 6) | OPC_DBSHFL,
    OPC_DSHD       = (0x05 << 6) | OPC_DBSHFL,
    OPC_DALIGN     = (0x08 << 6) | OPC_DBSHFL, /* 01.bp (01.000 to 01.111) */
    OPC_DALIGN_1   = (0x09 << 6) | OPC_DBSHFL,
    OPC_DALIGN_2   = (0x0A << 6) | OPC_DBSHFL,
    OPC_DALIGN_3   = (0x0B << 6) | OPC_DBSHFL,
    OPC_DALIGN_4   = (0x0C << 6) | OPC_DBSHFL,
    OPC_DALIGN_5   = (0x0D << 6) | OPC_DBSHFL,
    OPC_DALIGN_6   = (0x0E << 6) | OPC_DBSHFL,
    OPC_DALIGN_7   = (0x0F << 6) | OPC_DBSHFL,
    OPC_DBITSWAP   = (0x00 << 6) | OPC_DBSHFL, /* 00000 */
};

/* MIPS DSP REGIMM opcodes */
enum {
    OPC_BPOSGE32 = (0x1C << 16) | OPC_REGIMM,
    OPC_BPOSGE64 = (0x1D << 16) | OPC_REGIMM,
};

#define MASK_LX(op)                 (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
/* MIPS DSP Load */
enum {
    OPC_LBUX = (0x06 << 6) | OPC_LX_DSP,
    OPC_LHX  = (0x04 << 6) | OPC_LX_DSP,
    OPC_LWX  = (0x00 << 6) | OPC_LX_DSP,
    OPC_LDX = (0x08 << 6) | OPC_LX_DSP,
};

#define MASK_ADDU_QB(op)            (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* MIPS DSP Arithmetic Sub-class */
    OPC_ADDQ_PH        = (0x0A << 6) | OPC_ADDU_QB_DSP,
    OPC_ADDQ_S_PH      = (0x0E << 6) | OPC_ADDU_QB_DSP,
    OPC_ADDQ_S_W       = (0x16 << 6) | OPC_ADDU_QB_DSP,
    OPC_ADDU_QB        = (0x00 << 6) | OPC_ADDU_QB_DSP,
    OPC_ADDU_S_QB      = (0x04 << 6) | OPC_ADDU_QB_DSP,
    OPC_ADDU_PH        = (0x08 << 6) | OPC_ADDU_QB_DSP,
    OPC_ADDU_S_PH      = (0x0C << 6) | OPC_ADDU_QB_DSP,
    OPC_SUBQ_PH        = (0x0B << 6) | OPC_ADDU_QB_DSP,
    OPC_SUBQ_S_PH      = (0x0F << 6) | OPC_ADDU_QB_DSP,
    OPC_SUBQ_S_W       = (0x17 << 6) | OPC_ADDU_QB_DSP,
    OPC_SUBU_QB        = (0x01 << 6) | OPC_ADDU_QB_DSP,
    OPC_SUBU_S_QB      = (0x05 << 6) | OPC_ADDU_QB_DSP,
    OPC_SUBU_PH        = (0x09 << 6) | OPC_ADDU_QB_DSP,
    OPC_SUBU_S_PH      = (0x0D << 6) | OPC_ADDU_QB_DSP,
    OPC_ADDSC          = (0x10 << 6) | OPC_ADDU_QB_DSP,
    OPC_ADDWC          = (0x11 << 6) | OPC_ADDU_QB_DSP,
    OPC_MODSUB         = (0x12 << 6) | OPC_ADDU_QB_DSP,
    OPC_RADDU_W_QB     = (0x14 << 6) | OPC_ADDU_QB_DSP,
    /* MIPS DSP Multiply Sub-class insns */
    OPC_MULEU_S_PH_QBL = (0x06 << 6) | OPC_ADDU_QB_DSP,
    OPC_MULEU_S_PH_QBR = (0x07 << 6) | OPC_ADDU_QB_DSP,
    OPC_MULQ_RS_PH     = (0x1F << 6) | OPC_ADDU_QB_DSP,
    OPC_MULEQ_S_W_PHL  = (0x1C << 6) | OPC_ADDU_QB_DSP,
    OPC_MULEQ_S_W_PHR  = (0x1D << 6) | OPC_ADDU_QB_DSP,
    OPC_MULQ_S_PH      = (0x1E << 6) | OPC_ADDU_QB_DSP,
};

#define OPC_ADDUH_QB_DSP OPC_MULT_G_2E
#define MASK_ADDUH_QB(op)           (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* MIPS DSP Arithmetic Sub-class */
    OPC_ADDUH_QB   = (0x00 << 6) | OPC_ADDUH_QB_DSP,
    OPC_ADDUH_R_QB = (0x02 << 6) | OPC_ADDUH_QB_DSP,
    OPC_ADDQH_PH   = (0x08 << 6) | OPC_ADDUH_QB_DSP,
    OPC_ADDQH_R_PH = (0x0A << 6) | OPC_ADDUH_QB_DSP,
    OPC_ADDQH_W    = (0x10 << 6) | OPC_ADDUH_QB_DSP,
    OPC_ADDQH_R_W  = (0x12 << 6) | OPC_ADDUH_QB_DSP,
    OPC_SUBUH_QB   = (0x01 << 6) | OPC_ADDUH_QB_DSP,
    OPC_SUBUH_R_QB = (0x03 << 6) | OPC_ADDUH_QB_DSP,
    OPC_SUBQH_PH   = (0x09 << 6) | OPC_ADDUH_QB_DSP,
    OPC_SUBQH_R_PH = (0x0B << 6) | OPC_ADDUH_QB_DSP,
    OPC_SUBQH_W    = (0x11 << 6) | OPC_ADDUH_QB_DSP,
    OPC_SUBQH_R_W  = (0x13 << 6) | OPC_ADDUH_QB_DSP,
    /* MIPS DSP Multiply Sub-class insns */
    OPC_MUL_PH     = (0x0C << 6) | OPC_ADDUH_QB_DSP,
    OPC_MUL_S_PH   = (0x0E << 6) | OPC_ADDUH_QB_DSP,
    OPC_MULQ_S_W   = (0x16 << 6) | OPC_ADDUH_QB_DSP,
    OPC_MULQ_RS_W  = (0x17 << 6) | OPC_ADDUH_QB_DSP,
};

#define MASK_ABSQ_S_PH(op)          (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* MIPS DSP Arithmetic Sub-class */
    OPC_ABSQ_S_QB       = (0x01 << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_ABSQ_S_PH       = (0x09 << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_ABSQ_S_W        = (0x11 << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_PRECEQ_W_PHL    = (0x0C << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_PRECEQ_W_PHR    = (0x0D << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_PRECEQU_PH_QBL  = (0x04 << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_PRECEQU_PH_QBR  = (0x05 << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_PRECEQU_PH_QBLA = (0x06 << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_PRECEQU_PH_QBRA = (0x07 << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_PRECEU_PH_QBL   = (0x1C << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_PRECEU_PH_QBR   = (0x1D << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_PRECEU_PH_QBLA  = (0x1E << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_PRECEU_PH_QBRA  = (0x1F << 6) | OPC_ABSQ_S_PH_DSP,
    /* DSP Bit/Manipulation Sub-class */
    OPC_BITREV          = (0x1B << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_REPL_QB         = (0x02 << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_REPLV_QB        = (0x03 << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_REPL_PH         = (0x0A << 6) | OPC_ABSQ_S_PH_DSP,
    OPC_REPLV_PH        = (0x0B << 6) | OPC_ABSQ_S_PH_DSP,
};

#define MASK_CMPU_EQ_QB(op)         (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* MIPS DSP Arithmetic Sub-class */
    OPC_PRECR_QB_PH      = (0x0D << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_PRECRQ_QB_PH     = (0x0C << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_PRECR_SRA_PH_W   = (0x1E << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_PRECR_SRA_R_PH_W = (0x1F << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_PRECRQ_PH_W      = (0x14 << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_PRECRQ_RS_PH_W   = (0x15 << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_PRECRQU_S_QB_PH  = (0x0F << 6) | OPC_CMPU_EQ_QB_DSP,
    /* DSP Compare-Pick Sub-class */
    OPC_CMPU_EQ_QB       = (0x00 << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_CMPU_LT_QB       = (0x01 << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_CMPU_LE_QB       = (0x02 << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_CMPGU_EQ_QB      = (0x04 << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_CMPGU_LT_QB      = (0x05 << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_CMPGU_LE_QB      = (0x06 << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_CMPGDU_EQ_QB     = (0x18 << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_CMPGDU_LT_QB     = (0x19 << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_CMPGDU_LE_QB     = (0x1A << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_CMP_EQ_PH        = (0x08 << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_CMP_LT_PH        = (0x09 << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_CMP_LE_PH        = (0x0A << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_PICK_QB          = (0x03 << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_PICK_PH          = (0x0B << 6) | OPC_CMPU_EQ_QB_DSP,
    OPC_PACKRL_PH        = (0x0E << 6) | OPC_CMPU_EQ_QB_DSP,
};

#define MASK_SHLL_QB(op)            (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* MIPS DSP GPR-Based Shift Sub-class */
    OPC_SHLL_QB    = (0x00 << 6) | OPC_SHLL_QB_DSP,
    OPC_SHLLV_QB   = (0x02 << 6) | OPC_SHLL_QB_DSP,
    OPC_SHLL_PH    = (0x08 << 6) | OPC_SHLL_QB_DSP,
    OPC_SHLLV_PH   = (0x0A << 6) | OPC_SHLL_QB_DSP,
    OPC_SHLL_S_PH  = (0x0C << 6) | OPC_SHLL_QB_DSP,
    OPC_SHLLV_S_PH = (0x0E << 6) | OPC_SHLL_QB_DSP,
    OPC_SHLL_S_W   = (0x14 << 6) | OPC_SHLL_QB_DSP,
    OPC_SHLLV_S_W  = (0x16 << 6) | OPC_SHLL_QB_DSP,
    OPC_SHRL_QB    = (0x01 << 6) | OPC_SHLL_QB_DSP,
    OPC_SHRLV_QB   = (0x03 << 6) | OPC_SHLL_QB_DSP,
    OPC_SHRL_PH    = (0x19 << 6) | OPC_SHLL_QB_DSP,
    OPC_SHRLV_PH   = (0x1B << 6) | OPC_SHLL_QB_DSP,
    OPC_SHRA_QB    = (0x04 << 6) | OPC_SHLL_QB_DSP,
    OPC_SHRA_R_QB  = (0x05 << 6) | OPC_SHLL_QB_DSP,
    OPC_SHRAV_QB   = (0x06 << 6) | OPC_SHLL_QB_DSP,
    OPC_SHRAV_R_QB = (0x07 << 6) | OPC_SHLL_QB_DSP,
    OPC_SHRA_PH    = (0x09 << 6) | OPC_SHLL_QB_DSP,
    OPC_SHRAV_PH   = (0x0B << 6) | OPC_SHLL_QB_DSP,
    OPC_SHRA_R_PH  = (0x0D << 6) | OPC_SHLL_QB_DSP,
    OPC_SHRAV_R_PH = (0x0F << 6) | OPC_SHLL_QB_DSP,
    OPC_SHRA_R_W   = (0x15 << 6) | OPC_SHLL_QB_DSP,
    OPC_SHRAV_R_W  = (0x17 << 6) | OPC_SHLL_QB_DSP,
};

#define MASK_DPA_W_PH(op)           (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* MIPS DSP Multiply Sub-class insns */
    OPC_DPAU_H_QBL    = (0x03 << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPAU_H_QBR    = (0x07 << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPSU_H_QBL    = (0x0B << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPSU_H_QBR    = (0x0F << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPA_W_PH      = (0x00 << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPAX_W_PH     = (0x08 << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPAQ_S_W_PH   = (0x04 << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPAQX_S_W_PH  = (0x18 << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPAQX_SA_W_PH = (0x1A << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPS_W_PH      = (0x01 << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPSX_W_PH     = (0x09 << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPSQ_S_W_PH   = (0x05 << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPSQX_S_W_PH  = (0x19 << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPSQX_SA_W_PH = (0x1B << 6) | OPC_DPA_W_PH_DSP,
    OPC_MULSAQ_S_W_PH = (0x06 << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPAQ_SA_L_W   = (0x0C << 6) | OPC_DPA_W_PH_DSP,
    OPC_DPSQ_SA_L_W   = (0x0D << 6) | OPC_DPA_W_PH_DSP,
    OPC_MAQ_S_W_PHL   = (0x14 << 6) | OPC_DPA_W_PH_DSP,
    OPC_MAQ_S_W_PHR   = (0x16 << 6) | OPC_DPA_W_PH_DSP,
    OPC_MAQ_SA_W_PHL  = (0x10 << 6) | OPC_DPA_W_PH_DSP,
    OPC_MAQ_SA_W_PHR  = (0x12 << 6) | OPC_DPA_W_PH_DSP,
    OPC_MULSA_W_PH    = (0x02 << 6) | OPC_DPA_W_PH_DSP,
};

#define MASK_INSV(op)               (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* DSP Bit/Manipulation Sub-class */
    OPC_INSV = (0x00 << 6) | OPC_INSV_DSP,
};

#define MASK_APPEND(op)             (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* MIPS DSP Append Sub-class */
    OPC_APPEND  = (0x00 << 6) | OPC_APPEND_DSP,
    OPC_PREPEND = (0x01 << 6) | OPC_APPEND_DSP,
    OPC_BALIGN  = (0x10 << 6) | OPC_APPEND_DSP,
};

#define MASK_EXTR_W(op)             (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* MIPS DSP Accumulator and DSPControl Access Sub-class */
    OPC_EXTR_W     = (0x00 << 6) | OPC_EXTR_W_DSP,
    OPC_EXTR_R_W   = (0x04 << 6) | OPC_EXTR_W_DSP,
    OPC_EXTR_RS_W  = (0x06 << 6) | OPC_EXTR_W_DSP,
    OPC_EXTR_S_H   = (0x0E << 6) | OPC_EXTR_W_DSP,
    OPC_EXTRV_S_H  = (0x0F << 6) | OPC_EXTR_W_DSP,
    OPC_EXTRV_W    = (0x01 << 6) | OPC_EXTR_W_DSP,
    OPC_EXTRV_R_W  = (0x05 << 6) | OPC_EXTR_W_DSP,
    OPC_EXTRV_RS_W = (0x07 << 6) | OPC_EXTR_W_DSP,
    OPC_EXTP       = (0x02 << 6) | OPC_EXTR_W_DSP,
    OPC_EXTPV      = (0x03 << 6) | OPC_EXTR_W_DSP,
    OPC_EXTPDP     = (0x0A << 6) | OPC_EXTR_W_DSP,
    OPC_EXTPDPV    = (0x0B << 6) | OPC_EXTR_W_DSP,
    OPC_SHILO      = (0x1A << 6) | OPC_EXTR_W_DSP,
    OPC_SHILOV     = (0x1B << 6) | OPC_EXTR_W_DSP,
    OPC_MTHLIP     = (0x1F << 6) | OPC_EXTR_W_DSP,
    OPC_WRDSP      = (0x13 << 6) | OPC_EXTR_W_DSP,
    OPC_RDDSP      = (0x12 << 6) | OPC_EXTR_W_DSP,
};

#define MASK_ABSQ_S_QH(op)          (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* MIPS DSP Arithmetic Sub-class */
    OPC_PRECEQ_L_PWL    = (0x14 << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_PRECEQ_L_PWR    = (0x15 << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_PRECEQ_PW_QHL   = (0x0C << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_PRECEQ_PW_QHR   = (0x0D << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_PRECEQ_PW_QHLA  = (0x0E << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_PRECEQ_PW_QHRA  = (0x0F << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_PRECEQU_QH_OBL  = (0x04 << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_PRECEQU_QH_OBR  = (0x05 << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_PRECEQU_QH_OBLA = (0x06 << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_PRECEQU_QH_OBRA = (0x07 << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_PRECEU_QH_OBL   = (0x1C << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_PRECEU_QH_OBR   = (0x1D << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_PRECEU_QH_OBLA  = (0x1E << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_PRECEU_QH_OBRA  = (0x1F << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_ABSQ_S_OB       = (0x01 << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_ABSQ_S_PW       = (0x11 << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_ABSQ_S_QH       = (0x09 << 6) | OPC_ABSQ_S_QH_DSP,
    /* DSP Bit/Manipulation Sub-class */
    OPC_REPL_OB         = (0x02 << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_REPL_PW         = (0x12 << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_REPL_QH         = (0x0A << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_REPLV_OB        = (0x03 << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_REPLV_PW        = (0x13 << 6) | OPC_ABSQ_S_QH_DSP,
    OPC_REPLV_QH        = (0x0B << 6) | OPC_ABSQ_S_QH_DSP,
};

#define MASK_ADDU_OB(op)            (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* MIPS DSP Multiply Sub-class insns */
    OPC_MULEQ_S_PW_QHL = (0x1C << 6) | OPC_ADDU_OB_DSP,
    OPC_MULEQ_S_PW_QHR = (0x1D << 6) | OPC_ADDU_OB_DSP,
    OPC_MULEU_S_QH_OBL = (0x06 << 6) | OPC_ADDU_OB_DSP,
    OPC_MULEU_S_QH_OBR = (0x07 << 6) | OPC_ADDU_OB_DSP,
    OPC_MULQ_RS_QH     = (0x1F << 6) | OPC_ADDU_OB_DSP,
    /* MIPS DSP Arithmetic Sub-class */
    OPC_RADDU_L_OB     = (0x14 << 6) | OPC_ADDU_OB_DSP,
    OPC_SUBQ_PW        = (0x13 << 6) | OPC_ADDU_OB_DSP,
    OPC_SUBQ_S_PW      = (0x17 << 6) | OPC_ADDU_OB_DSP,
    OPC_SUBQ_QH        = (0x0B << 6) | OPC_ADDU_OB_DSP,
    OPC_SUBQ_S_QH      = (0x0F << 6) | OPC_ADDU_OB_DSP,
    OPC_SUBU_OB        = (0x01 << 6) | OPC_ADDU_OB_DSP,
    OPC_SUBU_S_OB      = (0x05 << 6) | OPC_ADDU_OB_DSP,
    OPC_SUBU_QH        = (0x09 << 6) | OPC_ADDU_OB_DSP,
    OPC_SUBU_S_QH      = (0x0D << 6) | OPC_ADDU_OB_DSP,
    OPC_SUBUH_OB       = (0x19 << 6) | OPC_ADDU_OB_DSP,
    OPC_SUBUH_R_OB     = (0x1B << 6) | OPC_ADDU_OB_DSP,
    OPC_ADDQ_PW        = (0x12 << 6) | OPC_ADDU_OB_DSP,
    OPC_ADDQ_S_PW      = (0x16 << 6) | OPC_ADDU_OB_DSP,
    OPC_ADDQ_QH        = (0x0A << 6) | OPC_ADDU_OB_DSP,
    OPC_ADDQ_S_QH      = (0x0E << 6) | OPC_ADDU_OB_DSP,
    OPC_ADDU_OB        = (0x00 << 6) | OPC_ADDU_OB_DSP,
    OPC_ADDU_S_OB      = (0x04 << 6) | OPC_ADDU_OB_DSP,
    OPC_ADDU_QH        = (0x08 << 6) | OPC_ADDU_OB_DSP,
    OPC_ADDU_S_QH      = (0x0C << 6) | OPC_ADDU_OB_DSP,
    OPC_ADDUH_OB       = (0x18 << 6) | OPC_ADDU_OB_DSP,
    OPC_ADDUH_R_OB     = (0x1A << 6) | OPC_ADDU_OB_DSP,
};

#define MASK_CMPU_EQ_OB(op)         (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* DSP Compare-Pick Sub-class */
    OPC_CMP_EQ_PW         = (0x10 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_CMP_LT_PW         = (0x11 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_CMP_LE_PW         = (0x12 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_CMP_EQ_QH         = (0x08 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_CMP_LT_QH         = (0x09 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_CMP_LE_QH         = (0x0A << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_CMPGDU_EQ_OB      = (0x18 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_CMPGDU_LT_OB      = (0x19 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_CMPGDU_LE_OB      = (0x1A << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_CMPGU_EQ_OB       = (0x04 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_CMPGU_LT_OB       = (0x05 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_CMPGU_LE_OB       = (0x06 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_CMPU_EQ_OB        = (0x00 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_CMPU_LT_OB        = (0x01 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_CMPU_LE_OB        = (0x02 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_PACKRL_PW         = (0x0E << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_PICK_OB           = (0x03 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_PICK_PW           = (0x13 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_PICK_QH           = (0x0B << 6) | OPC_CMPU_EQ_OB_DSP,
    /* MIPS DSP Arithmetic Sub-class */
    OPC_PRECR_OB_QH       = (0x0D << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_PRECR_SRA_QH_PW   = (0x1E << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_PRECR_SRA_R_QH_PW = (0x1F << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_PRECRQ_OB_QH      = (0x0C << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_PRECRQ_PW_L       = (0x1C << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_PRECRQ_QH_PW      = (0x14 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_PRECRQ_RS_QH_PW   = (0x15 << 6) | OPC_CMPU_EQ_OB_DSP,
    OPC_PRECRQU_S_OB_QH   = (0x0F << 6) | OPC_CMPU_EQ_OB_DSP,
};

#define MASK_DAPPEND(op)            (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* DSP Append Sub-class */
    OPC_DAPPEND  = (0x00 << 6) | OPC_DAPPEND_DSP,
    OPC_PREPENDD = (0x03 << 6) | OPC_DAPPEND_DSP,
    OPC_PREPENDW = (0x01 << 6) | OPC_DAPPEND_DSP,
    OPC_DBALIGN  = (0x10 << 6) | OPC_DAPPEND_DSP,
};

#define MASK_DEXTR_W(op)            (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* MIPS DSP Accumulator and DSPControl Access Sub-class */
    OPC_DMTHLIP     = (0x1F << 6) | OPC_DEXTR_W_DSP,
    OPC_DSHILO      = (0x1A << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTP       = (0x02 << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTPDP     = (0x0A << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTPDPV    = (0x0B << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTPV      = (0x03 << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTR_L     = (0x10 << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTR_R_L   = (0x14 << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTR_RS_L  = (0x16 << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTR_W     = (0x00 << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTR_R_W   = (0x04 << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTR_RS_W  = (0x06 << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTR_S_H   = (0x0E << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTRV_L    = (0x11 << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTRV_R_L  = (0x15 << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTRV_RS_L = (0x17 << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTRV_S_H  = (0x0F << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTRV_W    = (0x01 << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTRV_R_W  = (0x05 << 6) | OPC_DEXTR_W_DSP,
    OPC_DEXTRV_RS_W = (0x07 << 6) | OPC_DEXTR_W_DSP,
    OPC_DSHILOV     = (0x1B << 6) | OPC_DEXTR_W_DSP,
};

#define MASK_DINSV(op)              (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* DSP Bit/Manipulation Sub-class */
    OPC_DINSV = (0x00 << 6) | OPC_DINSV_DSP,
};

#define MASK_DPAQ_W_QH(op)          (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* MIPS DSP Multiply Sub-class insns */
    OPC_DMADD         = (0x19 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_DMADDU        = (0x1D << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_DMSUB         = (0x1B << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_DMSUBU        = (0x1F << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_DPA_W_QH      = (0x00 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_DPAQ_S_W_QH   = (0x04 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_DPAQ_SA_L_PW  = (0x0C << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_DPAU_H_OBL    = (0x03 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_DPAU_H_OBR    = (0x07 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_DPS_W_QH      = (0x01 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_DPSQ_S_W_QH   = (0x05 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_DPSQ_SA_L_PW  = (0x0D << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_DPSU_H_OBL    = (0x0B << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_DPSU_H_OBR    = (0x0F << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_MAQ_S_L_PWL   = (0x1C << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_MAQ_S_L_PWR   = (0x1E << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_MAQ_S_W_QHLL  = (0x14 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_MAQ_SA_W_QHLL = (0x10 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_MAQ_S_W_QHLR  = (0x15 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_MAQ_SA_W_QHLR = (0x11 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_MAQ_S_W_QHRL  = (0x16 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_MAQ_SA_W_QHRL = (0x12 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_MAQ_S_W_QHRR  = (0x17 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_MAQ_SA_W_QHRR = (0x13 << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_MULSAQ_S_L_PW = (0x0E << 6) | OPC_DPAQ_W_QH_DSP,
    OPC_MULSAQ_S_W_QH = (0x06 << 6) | OPC_DPAQ_W_QH_DSP,
};

#define MASK_SHLL_OB(op)            (MASK_SPECIAL3(op) | (op & (0x1F << 6)))
enum {
    /* MIPS DSP GPR-Based Shift Sub-class */
    OPC_SHLL_PW    = (0x10 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHLL_S_PW  = (0x14 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHLLV_OB   = (0x02 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHLLV_PW   = (0x12 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHLLV_S_PW = (0x16 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHLLV_QH   = (0x0A << 6) | OPC_SHLL_OB_DSP,
    OPC_SHLLV_S_QH = (0x0E << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRA_PW    = (0x11 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRA_R_PW  = (0x15 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRAV_OB   = (0x06 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRAV_R_OB = (0x07 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRAV_PW   = (0x13 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRAV_R_PW = (0x17 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRAV_QH   = (0x0B << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRAV_R_QH = (0x0F << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRLV_OB   = (0x03 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRLV_QH   = (0x1B << 6) | OPC_SHLL_OB_DSP,
    OPC_SHLL_OB    = (0x00 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHLL_QH    = (0x08 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHLL_S_QH  = (0x0C << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRA_OB    = (0x04 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRA_R_OB  = (0x05 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRA_QH    = (0x09 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRA_R_QH  = (0x0D << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRL_OB    = (0x01 << 6) | OPC_SHLL_OB_DSP,
    OPC_SHRL_QH    = (0x19 << 6) | OPC_SHLL_OB_DSP,
};

/* Coprocessor 0 (rs field) */
#define MASK_CP0(op)                (MASK_OP_MAJOR(op) | (op & (0x1F << 21)))

enum {
    OPC_MFC0     = (0x00 << 21) | OPC_CP0,
    OPC_DMFC0    = (0x01 << 21) | OPC_CP0,
    OPC_MFHC0    = (0x02 << 21) | OPC_CP0,
    OPC_MTC0     = (0x04 << 21) | OPC_CP0,
    OPC_DMTC0    = (0x05 << 21) | OPC_CP0,
    OPC_MTHC0    = (0x06 << 21) | OPC_CP0,
    OPC_MFTR     = (0x08 << 21) | OPC_CP0,
    OPC_RDPGPR   = (0x0A << 21) | OPC_CP0,
    OPC_MFMC0    = (0x0B << 21) | OPC_CP0,
    OPC_MTTR     = (0x0C << 21) | OPC_CP0,
    OPC_WRPGPR   = (0x0E << 21) | OPC_CP0,
    OPC_C0       = (0x10 << 21) | OPC_CP0,
    OPC_C0_1     = (0x11 << 21) | OPC_CP0,
    OPC_C0_2     = (0x12 << 21) | OPC_CP0,
    OPC_C0_3     = (0x13 << 21) | OPC_CP0,
    OPC_C0_4     = (0x14 << 21) | OPC_CP0,
    OPC_C0_5     = (0x15 << 21) | OPC_CP0,
    OPC_C0_6     = (0x16 << 21) | OPC_CP0,
    OPC_C0_7     = (0x17 << 21) | OPC_CP0,
    OPC_C0_8     = (0x18 << 21) | OPC_CP0,
    OPC_C0_9     = (0x19 << 21) | OPC_CP0,
    OPC_C0_A     = (0x1A << 21) | OPC_CP0,
    OPC_C0_B     = (0x1B << 21) | OPC_CP0,
    OPC_C0_C     = (0x1C << 21) | OPC_CP0,
    OPC_C0_D     = (0x1D << 21) | OPC_CP0,
    OPC_C0_E     = (0x1E << 21) | OPC_CP0,
    OPC_C0_F     = (0x1F << 21) | OPC_CP0,
};

/* MFMC0 opcodes */
#define MASK_MFMC0(op)              (MASK_CP0(op) | (op & 0xFFFF))

enum {
    OPC_DMT      = 0x01 | (0 << 5) | (0x0F << 6) | (0x01 << 11) | OPC_MFMC0,
    OPC_EMT      = 0x01 | (1 << 5) | (0x0F << 6) | (0x01 << 11) | OPC_MFMC0,
    OPC_DVPE     = 0x01 | (0 << 5) | OPC_MFMC0,
    OPC_EVPE     = 0x01 | (1 << 5) | OPC_MFMC0,
    OPC_DI       = (0 << 5) | (0x0C << 11) | OPC_MFMC0,
    OPC_EI       = (1 << 5) | (0x0C << 11) | OPC_MFMC0,
    OPC_DVP      = 0x04 | (0 << 3) | (1 << 5) | (0 << 11) | OPC_MFMC0,
    OPC_EVP      = 0x04 | (0 << 3) | (0 << 5) | (0 << 11) | OPC_MFMC0,
};

/* Coprocessor 0 (with rs == C0) */
#define MASK_C0(op)                 (MASK_CP0(op) | (op & 0x3F))

enum {
    OPC_TLBR     = 0x01 | OPC_C0,
    OPC_TLBWI    = 0x02 | OPC_C0,
    OPC_TLBINV   = 0x03 | OPC_C0,
    OPC_TLBINVF  = 0x04 | OPC_C0,
    OPC_TLBWR    = 0x06 | OPC_C0,
    OPC_TLBP     = 0x08 | OPC_C0,
    OPC_RFE      = 0x10 | OPC_C0,
    OPC_ERET     = 0x18 | OPC_C0,
    OPC_DERET    = 0x1F | OPC_C0,
    OPC_WAIT     = 0x20 | OPC_C0,
};

#define MASK_CP2(op)                (MASK_OP_MAJOR(op) | (op & (0x1F << 21)))

enum {
    OPC_MFC2    = (0x00 << 21) | OPC_CP2,
    OPC_DMFC2   = (0x01 << 21) | OPC_CP2,
    OPC_CFC2    = (0x02 << 21) | OPC_CP2,
    OPC_MFHC2   = (0x03 << 21) | OPC_CP2,
    OPC_MTC2    = (0x04 << 21) | OPC_CP2,
    OPC_DMTC2   = (0x05 << 21) | OPC_CP2,
    OPC_CTC2    = (0x06 << 21) | OPC_CP2,
    OPC_MTHC2   = (0x07 << 21) | OPC_CP2,
    OPC_BC2     = (0x08 << 21) | OPC_CP2,
    OPC_BC2EQZ  = (0x09 << 21) | OPC_CP2,
    OPC_BC2NEZ  = (0x0D << 21) | OPC_CP2,
};

#define MASK_LMMI(op)    (MASK_OP_MAJOR(op) | (op & (0x1F << 21)) | (op & 0x1F))

enum {
    OPC_PADDSH      = (24 << 21) | (0x00) | OPC_CP2,
    OPC_PADDUSH     = (25 << 21) | (0x00) | OPC_CP2,
    OPC_PADDH       = (26 << 21) | (0x00) | OPC_CP2,
    OPC_PADDW       = (27 << 21) | (0x00) | OPC_CP2,
    OPC_PADDSB      = (28 << 21) | (0x00) | OPC_CP2,
    OPC_PADDUSB     = (29 << 21) | (0x00) | OPC_CP2,
    OPC_PADDB       = (30 << 21) | (0x00) | OPC_CP2,
    OPC_PADDD       = (31 << 21) | (0x00) | OPC_CP2,

    OPC_PSUBSH      = (24 << 21) | (0x01) | OPC_CP2,
    OPC_PSUBUSH     = (25 << 21) | (0x01) | OPC_CP2,
    OPC_PSUBH       = (26 << 21) | (0x01) | OPC_CP2,
    OPC_PSUBW       = (27 << 21) | (0x01) | OPC_CP2,
    OPC_PSUBSB      = (28 << 21) | (0x01) | OPC_CP2,
    OPC_PSUBUSB     = (29 << 21) | (0x01) | OPC_CP2,
    OPC_PSUBB       = (30 << 21) | (0x01) | OPC_CP2,
    OPC_PSUBD       = (31 << 21) | (0x01) | OPC_CP2,

    OPC_PSHUFH      = (24 << 21) | (0x02) | OPC_CP2,
    OPC_PACKSSWH    = (25 << 21) | (0x02) | OPC_CP2,
    OPC_PACKSSHB    = (26 << 21) | (0x02) | OPC_CP2,
    OPC_PACKUSHB    = (27 << 21) | (0x02) | OPC_CP2,
    OPC_XOR_CP2     = (28 << 21) | (0x02) | OPC_CP2,
    OPC_NOR_CP2     = (29 << 21) | (0x02) | OPC_CP2,
    OPC_AND_CP2     = (30 << 21) | (0x02) | OPC_CP2,
    OPC_PANDN       = (31 << 21) | (0x02) | OPC_CP2,

    OPC_PUNPCKLHW   = (24 << 21) | (0x03) | OPC_CP2,
    OPC_PUNPCKHHW   = (25 << 21) | (0x03) | OPC_CP2,
    OPC_PUNPCKLBH   = (26 << 21) | (0x03) | OPC_CP2,
    OPC_PUNPCKHBH   = (27 << 21) | (0x03) | OPC_CP2,
    OPC_PINSRH_0    = (28 << 21) | (0x03) | OPC_CP2,
    OPC_PINSRH_1    = (29 << 21) | (0x03) | OPC_CP2,
    OPC_PINSRH_2    = (30 << 21) | (0x03) | OPC_CP2,
    OPC_PINSRH_3    = (31 << 21) | (0x03) | OPC_CP2,

    OPC_PAVGH       = (24 << 21) | (0x08) | OPC_CP2,
    OPC_PAVGB       = (25 << 21) | (0x08) | OPC_CP2,
    OPC_PMAXSH      = (26 << 21) | (0x08) | OPC_CP2,
    OPC_PMINSH      = (27 << 21) | (0x08) | OPC_CP2,
    OPC_PMAXUB      = (28 << 21) | (0x08) | OPC_CP2,
    OPC_PMINUB      = (29 << 21) | (0x08) | OPC_CP2,

    OPC_PCMPEQW     = (24 << 21) | (0x09) | OPC_CP2,
    OPC_PCMPGTW     = (25 << 21) | (0x09) | OPC_CP2,
    OPC_PCMPEQH     = (26 << 21) | (0x09) | OPC_CP2,
    OPC_PCMPGTH     = (27 << 21) | (0x09) | OPC_CP2,
    OPC_PCMPEQB     = (28 << 21) | (0x09) | OPC_CP2,
    OPC_PCMPGTB     = (29 << 21) | (0x09) | OPC_CP2,

    OPC_PSLLW       = (24 << 21) | (0x0A) | OPC_CP2,
    OPC_PSLLH       = (25 << 21) | (0x0A) | OPC_CP2,
    OPC_PMULLH      = (26 << 21) | (0x0A) | OPC_CP2,
    OPC_PMULHH      = (27 << 21) | (0x0A) | OPC_CP2,
    OPC_PMULUW      = (28 << 21) | (0x0A) | OPC_CP2,
    OPC_PMULHUH     = (29 << 21) | (0x0A) | OPC_CP2,

    OPC_PSRLW       = (24 << 21) | (0x0B) | OPC_CP2,
    OPC_PSRLH       = (25 << 21) | (0x0B) | OPC_CP2,
    OPC_PSRAW       = (26 << 21) | (0x0B) | OPC_CP2,
    OPC_PSRAH       = (27 << 21) | (0x0B) | OPC_CP2,
    OPC_PUNPCKLWD   = (28 << 21) | (0x0B) | OPC_CP2,
    OPC_PUNPCKHWD   = (29 << 21) | (0x0B) | OPC_CP2,

    OPC_ADDU_CP2    = (24 << 21) | (0x0C) | OPC_CP2,
    OPC_OR_CP2      = (25 << 21) | (0x0C) | OPC_CP2,
    OPC_ADD_CP2     = (26 << 21) | (0x0C) | OPC_CP2,
    OPC_DADD_CP2    = (27 << 21) | (0x0C) | OPC_CP2,
    OPC_SEQU_CP2    = (28 << 21) | (0x0C) | OPC_CP2,
    OPC_SEQ_CP2     = (29 << 21) | (0x0C) | OPC_CP2,

    OPC_SUBU_CP2    = (24 << 21) | (0x0D) | OPC_CP2,
    OPC_PASUBUB     = (25 << 21) | (0x0D) | OPC_CP2,
    OPC_SUB_CP2     = (26 << 21) | (0x0D) | OPC_CP2,
    OPC_DSUB_CP2    = (27 << 21) | (0x0D) | OPC_CP2,
    OPC_SLTU_CP2    = (28 << 21) | (0x0D) | OPC_CP2,
    OPC_SLT_CP2     = (29 << 21) | (0x0D) | OPC_CP2,

    OPC_SLL_CP2     = (24 << 21) | (0x0E) | OPC_CP2,
    OPC_DSLL_CP2    = (25 << 21) | (0x0E) | OPC_CP2,
    OPC_PEXTRH      = (26 << 21) | (0x0E) | OPC_CP2,
    OPC_PMADDHW     = (27 << 21) | (0x0E) | OPC_CP2,
    OPC_SLEU_CP2    = (28 << 21) | (0x0E) | OPC_CP2,
    OPC_SLE_CP2     = (29 << 21) | (0x0E) | OPC_CP2,

    OPC_SRL_CP2     = (24 << 21) | (0x0F) | OPC_CP2,
    OPC_DSRL_CP2    = (25 << 21) | (0x0F) | OPC_CP2,
    OPC_SRA_CP2     = (26 << 21) | (0x0F) | OPC_CP2,
    OPC_DSRA_CP2    = (27 << 21) | (0x0F) | OPC_CP2,
    OPC_BIADD       = (28 << 21) | (0x0F) | OPC_CP2,
    OPC_PMOVMSKB    = (29 << 21) | (0x0F) | OPC_CP2,
};


#define MASK_CP3(op)                (MASK_OP_MAJOR(op) | (op & 0x3F))

enum {
    OPC_LWXC1       = 0x00 | OPC_CP3,
    OPC_LDXC1       = 0x01 | OPC_CP3,
    OPC_LUXC1       = 0x05 | OPC_CP3,
    OPC_SWXC1       = 0x08 | OPC_CP3,
    OPC_SDXC1       = 0x09 | OPC_CP3,
    OPC_SUXC1       = 0x0D | OPC_CP3,
    OPC_PREFX       = 0x0F | OPC_CP3,
    OPC_ALNV_PS     = 0x1E | OPC_CP3,
    OPC_MADD_S      = 0x20 | OPC_CP3,
    OPC_MADD_D      = 0x21 | OPC_CP3,
    OPC_MADD_PS     = 0x26 | OPC_CP3,
    OPC_MSUB_S      = 0x28 | OPC_CP3,
    OPC_MSUB_D      = 0x29 | OPC_CP3,
    OPC_MSUB_PS     = 0x2E | OPC_CP3,
    OPC_NMADD_S     = 0x30 | OPC_CP3,
    OPC_NMADD_D     = 0x31 | OPC_CP3,
    OPC_NMADD_PS    = 0x36 | OPC_CP3,
    OPC_NMSUB_S     = 0x38 | OPC_CP3,
    OPC_NMSUB_D     = 0x39 | OPC_CP3,
    OPC_NMSUB_PS    = 0x3E | OPC_CP3,
};

/*
 *     MMI (MultiMedia Instruction) encodings
 *     ======================================
 *
 * MMI instructions encoding table keys:
 *
 *     *   This code is reserved for future use. An attempt to execute it
 *         causes a Reserved Instruction exception.
 *     %   This code indicates an instruction class. The instruction word
 *         must be further decoded by examining additional tables that show
 *         the values for other instruction fields.
 *     #   This code is reserved for the unsupported instructions DMULT,
 *         DMULTU, DDIV, DDIVU, LL, LLD, SC, SCD, LWC2 and SWC2. An attempt
 *         to execute it causes a Reserved Instruction exception.
 *
 * MMI instructions encoded by opcode field (MMI, LQ, SQ):
 *
 *  31    26                                        0
 * +--------+----------------------------------------+
 * | opcode |                                        |
 * +--------+----------------------------------------+
 *
 *   opcode  bits 28..26
 *     bits |   0   |   1   |   2   |   3   |   4   |   5   |   6   |   7
 *   31..29 |  000  |  001  |  010  |  011  |  100  |  101  |  110  |  111
 *   -------+-------+-------+-------+-------+-------+-------+-------+-------
 *    0 000 |SPECIAL| REGIMM|   J   |  JAL  |  BEQ  |  BNE  |  BLEZ |  BGTZ
 *    1 001 |  ADDI | ADDIU |  SLTI | SLTIU |  ANDI |  ORI  |  XORI |  LUI
 *    2 010 |  COP0 |  COP1 |   *   |   *   |  BEQL |  BNEL | BLEZL | BGTZL
 *    3 011 | DADDI | DADDIU|  LDL  |  LDR  |  MMI% |   *   |   LQ  |   SQ
 *    4 100 |   LB  |   LH  |  LWL  |   LW  |  LBU  |  LHU  |  LWR  |  LWU
 *    5 101 |   SB  |   SH  |  SWL  |   SW  |  SDL  |  SDR  |  SWR  | CACHE
 *    6 110 |   #   |  LWC1 |   #   |  PREF |   #   |  LDC1 |   #   |   LD
 *    7 111 |   #   |  SWC1 |   #   |   *   |   #   |  SDC1 |   #   |   SD
 */

enum {
    MMI_OPC_CLASS_MMI = 0x1C << 26,    /* Same as OPC_SPECIAL2 */
    MMI_OPC_SQ        = 0x1F << 26,    /* Same as OPC_SPECIAL3 */
};

/*
 * MMI instructions with opcode field = MMI:
 *
 *  31    26                                 5      0
 * +--------+-------------------------------+--------+
 * |   MMI  |                               |function|
 * +--------+-------------------------------+--------+
 *
 * function  bits 2..0
 *     bits |   0   |   1   |   2   |   3   |   4   |   5   |   6   |   7
 *     5..3 |  000  |  001  |  010  |  011  |  100  |  101  |  110  |  111
 *   -------+-------+-------+-------+-------+-------+-------+-------+-------
 *    0 000 |  MADD | MADDU |   *   |   *   | PLZCW |   *   |   *   |   *
 *    1 001 | MMI0% | MMI2% |   *   |   *   |   *   |   *   |   *   |   *
 *    2 010 | MFHI1 | MTHI1 | MFLO1 | MTLO1 |   *   |   *   |   *   |   *
 *    3 011 | MULT1 | MULTU1|  DIV1 | DIVU1 |   *   |   *   |   *   |   *
 *    4 100 | MADD1 | MADDU1|   *   |   *   |   *   |   *   |   *   |   *
 *    5 101 | MMI1% | MMI3% |   *   |   *   |   *   |   *   |   *   |   *
 *    6 110 | PMFHL | PMTHL |   *   |   *   | PSLLH |   *   | PSRLH | PSRAH
 *    7 111 |   *   |   *   |   *   |   *   | PSLLW |   *   | PSRLW | PSRAW
 */

#define MASK_MMI(op) (MASK_OP_MAJOR(op) | ((op) & 0x3F))
enum {
    MMI_OPC_MADD       = 0x00 | MMI_OPC_CLASS_MMI, /* Same as OPC_MADD */
    MMI_OPC_MADDU      = 0x01 | MMI_OPC_CLASS_MMI, /* Same as OPC_MADDU */
    MMI_OPC_MULT1      = 0x18 | MMI_OPC_CLASS_MMI, /* Same minor as OPC_MULT */
    MMI_OPC_MULTU1     = 0x19 | MMI_OPC_CLASS_MMI, /* Same min. as OPC_MULTU */
    MMI_OPC_DIV1       = 0x1A | MMI_OPC_CLASS_MMI, /* Same minor as OPC_DIV  */
    MMI_OPC_DIVU1      = 0x1B | MMI_OPC_CLASS_MMI, /* Same minor as OPC_DIVU */
    MMI_OPC_MADD1      = 0x20 | MMI_OPC_CLASS_MMI,
    MMI_OPC_MADDU1     = 0x21 | MMI_OPC_CLASS_MMI,
};

#endif /* LATM_MIPS_OPCODES_H */
