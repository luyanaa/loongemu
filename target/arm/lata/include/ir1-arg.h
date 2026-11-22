#ifndef _IR1_ARG_H_
#define _IR1_ARG_H_
#include <stdbool.h>
#include <stdint.h>
#include "la-pvir.h"

typedef struct {
    int rs;
    int rn;
    int rt;
    int a;
    int r;
    int sz;
} arg_atomic;

typedef struct {
    int rd;
    int rn;
    int sf;
    int immr;
    int imms;
} arg_bitfield;

typedef struct {
    int rn;
    int rm;
    int m;
} arg_bra;

typedef struct {
    int rn;
    int m;
} arg_braz;

typedef struct {
    int rt;
    int imm;
    int sf;
    int nz;
} arg_cbz;

typedef struct {
    int cond;
    int imm;
} arg_decode_insn3211;

typedef struct {
} arg_decode_insn3215;

typedef struct {
    int domain;
    int types;
} arg_decode_insn3216;

typedef struct {
    int mask;
    int imm;
} arg_decode_insn3217;

typedef struct {
    int l;
    int op1;
    int crn;
    int crm;
    int op2;
    int rt;
    int op0;
} arg_decode_insn3218;

typedef struct {
    int rs;
    int rn;
    int rt;
    int sz;
} arg_decode_insn3221;

typedef struct {
    int sz;
    int rn;
    int rt;
} arg_decode_insn3227;

typedef struct {
    int m;
    int w;
    int rn;
    int rt;
    int imm;
} arg_decode_insn3228;

typedef struct {
    int q;
    int p;
    int rm;
    int scale;
    int rn;
    int rt;
    int selem;
} arg_decode_insn3232;

typedef struct {
    int rd;
    int rn;
    int rm;
    int imm;
    int sf;
} arg_extract;

typedef struct {
    int imm;
} arg_i;

typedef struct {
    int rn;
    int rt;
    int imm;
    int sz;
    int sign;
    int ext;
} arg_ldapr_stlr_i;

typedef struct {
    int rt;
    int imm;
    int sz;
    int sign;
} arg_ldlit;

typedef struct {
    int rm;
    int rn;
    int rt;
    int sign;
    int ext;
    int sz;
    int opt;
    int s;
} arg_ldst;

typedef struct {
    int rt;
    int rn;
    int imm;
    int sz;
    int sign;
    int w;
    int p;
    int unpriv;
    int ext;
} arg_ldst_imm;

typedef struct {
    int rm;
    int rn;
    int rt;
    int sz;
    int q;
    int p;
    int rpt;
    int selem;
} arg_ldst_mult;

typedef struct {
    int rm;
    int rn;
    int rt;
    int p;
    int selem;
    int index;
    int scale;
} arg_ldst_single;

typedef struct {
    int rn;
    int rt;
    int imm;
    int p;
    int w;
} arg_ldst_tag;

typedef struct {
    int rt2;
    int rt;
    int rn;
    int imm;
    int sz;
    int sign;
    int w;
    int p;
} arg_ldstpair;

typedef struct {
    int rd;
    int sf;
    int imm;
    int hw;
} arg_movw;

typedef struct {
    int rn;
} arg_r;

typedef struct {
    int m;
} arg_reta;

typedef struct {
    int rd;
    int imm;
} arg_ri;

typedef struct {
    int rd;
    int rn;
    int sf;
    int dbm;
} arg_rri_log;

typedef struct {
    int rd;
    int rn;
    int imm;
    int sf;
} arg_rri_sf;

typedef struct {
    int rd;
    int rn;
    int uimm6;
    int uimm4;
} arg_rri_tag;

typedef struct {
    int rn;
    int rt;
    int sz;
    int lasr;
} arg_stlr;

typedef struct {
    int rn;
    int rt;
    int rt2;
    int rs;
    int sz;
    int lasr;
} arg_stxr;

typedef struct {
    int rt;
    int imm;
    int nz;
    int bitpos;
} arg_tbz;

typedef arg_ri arg_ADR;
typedef arg_ri arg_ADRP;
typedef arg_rri_sf arg_ADD_i;
typedef arg_rri_sf arg_ADDS_i;
typedef arg_rri_sf arg_SUB_i;
typedef arg_rri_sf arg_SUBS_i;
typedef arg_rri_tag arg_ADDG_i;
typedef arg_rri_tag arg_SUBG_i;
typedef arg_rri_log arg_AND_i;
typedef arg_rri_log arg_ORR_i;
typedef arg_rri_log arg_EOR_i;
typedef arg_rri_log arg_ANDS_i;
typedef arg_movw arg_MOVN;
typedef arg_movw arg_MOVZ;
typedef arg_movw arg_MOVK;
typedef arg_bitfield arg_SBFM;
typedef arg_bitfield arg_BFM;
typedef arg_bitfield arg_UBFM;
typedef arg_extract arg_EXTR;
typedef arg_i arg_B;
typedef arg_i arg_BL;
typedef arg_cbz arg_CBZ;
typedef arg_tbz arg_TBZ;
typedef arg_decode_insn3211 arg_B_cond;
typedef arg_r arg_BR;
typedef arg_r arg_BLR;
typedef arg_r arg_RET;
typedef arg_braz arg_BRAZ;
typedef arg_braz arg_BLRAZ;
typedef arg_reta arg_RETA;
typedef arg_bra arg_BRA;
typedef arg_bra arg_BLRA;
typedef arg_decode_insn3215 arg_ERET;
typedef arg_reta arg_ERETA;
typedef arg_decode_insn3215 arg_YIELD;
typedef arg_decode_insn3215 arg_WFE;
typedef arg_decode_insn3215 arg_WFI;
typedef arg_decode_insn3215 arg_XPACLRI;
typedef arg_decode_insn3215 arg_PACIA1716;
typedef arg_decode_insn3215 arg_PACIB1716;
typedef arg_decode_insn3215 arg_AUTIA1716;
typedef arg_decode_insn3215 arg_AUTIB1716;
typedef arg_decode_insn3215 arg_ESB;
typedef arg_decode_insn3215 arg_PACIAZ;
typedef arg_decode_insn3215 arg_PACIASP;
typedef arg_decode_insn3215 arg_PACIBZ;
typedef arg_decode_insn3215 arg_PACIBSP;
typedef arg_decode_insn3215 arg_AUTIAZ;
typedef arg_decode_insn3215 arg_AUTIASP;
typedef arg_decode_insn3215 arg_AUTIBZ;
typedef arg_decode_insn3215 arg_AUTIBSP;
typedef arg_decode_insn3215 arg_NOP;
typedef arg_decode_insn3215 arg_CLREX;
typedef arg_decode_insn3216 arg_DSB_DMB;
typedef arg_decode_insn3215 arg_ISB;
typedef arg_decode_insn3215 arg_SB;
typedef arg_decode_insn3215 arg_CFINV;
typedef arg_decode_insn3215 arg_XAFLAG;
typedef arg_decode_insn3215 arg_AXFLAG;
typedef arg_i arg_MSR_i_UAO;
typedef arg_i arg_MSR_i_PAN;
typedef arg_i arg_MSR_i_SPSEL;
typedef arg_i arg_MSR_i_SBSS;
typedef arg_i arg_MSR_i_DIT;
typedef arg_i arg_MSR_i_TCO;
typedef arg_i arg_MSR_i_DAIFSET;
typedef arg_i arg_MSR_i_DAIFCLEAR;
typedef arg_decode_insn3217 arg_MSR_i_SVCR;
typedef arg_decode_insn3218 arg_SYS;
typedef arg_i arg_SVC;
typedef arg_i arg_HVC;
typedef arg_i arg_SMC;
typedef arg_i arg_BRK;
typedef arg_i arg_HLT;
typedef arg_stxr arg_STXR;
typedef arg_stxr arg_LDXR;
typedef arg_stlr arg_STLR;
typedef arg_stlr arg_LDAR;
typedef arg_stxr arg_STXP;
typedef arg_stxr arg_LDXP;
typedef arg_decode_insn3221 arg_CASP;
typedef arg_decode_insn3221 arg_CAS;
typedef arg_ldlit arg_LD_lit;
typedef arg_ldlit arg_LD_lit_v;
typedef arg_ldstpair arg_STP;
typedef arg_ldstpair arg_LDP;
typedef arg_ldstpair arg_STP_v;
typedef arg_ldstpair arg_LDP_v;
typedef arg_ldstpair arg_STGP;
typedef arg_ldst_imm arg_STR_i;
typedef arg_ldst_imm arg_LDR_i;
typedef arg_ldst_imm arg_STR_v_i;
typedef arg_ldst_imm arg_LDR_v_i;
typedef arg_ldst arg_STR;
typedef arg_ldst arg_LDR;
typedef arg_ldst arg_STR_v;
typedef arg_ldst arg_LDR_v;
typedef arg_atomic arg_LDADD;
typedef arg_atomic arg_LDCLR;
typedef arg_atomic arg_LDEOR;
typedef arg_atomic arg_LDSET;
typedef arg_atomic arg_LDSMAX;
typedef arg_atomic arg_LDSMIN;
typedef arg_atomic arg_LDUMAX;
typedef arg_atomic arg_LDUMIN;
typedef arg_atomic arg_SWP;
typedef arg_decode_insn3227 arg_LDAPR;
typedef arg_decode_insn3228 arg_LDRA;
typedef arg_ldapr_stlr_i arg_STLR_i;
typedef arg_ldapr_stlr_i arg_LDAPR_i;
typedef arg_ldst_mult arg_ST_mult;
typedef arg_ldst_mult arg_LD_mult;
typedef arg_ldst_single arg_ST_single;
typedef arg_ldst_single arg_LD_single;
typedef arg_decode_insn3232 arg_LD_single_repl;
typedef arg_ldst_tag arg_STZGM;
typedef arg_ldst_tag arg_STG;
typedef arg_ldst_tag arg_LDG;
typedef arg_ldst_tag arg_STZG;
typedef arg_ldst_tag arg_STGM;
typedef arg_ldst_tag arg_ST2G;
typedef arg_ldst_tag arg_LDGM;
typedef arg_ldst_tag arg_STZ2G;


/* PVIR-facing semantic intent retained by the SVE IR1 decoder. */
typedef struct {
    PvirInactivePolicy inactive_policy;
    PvirMemoryEffect memory;
    PvirFaultBehavior fault_behavior;
    PvirFaultOrder fault_order;
    PvirFaultResultKind fault_result;
    PvirArchStateDomain state_domain;
    PvirArchStateAction state_action;
    PvirArchStateUpdateKind state_update;
    bool result_unknown_from_fault_boundary;
    bool result_unknown_where_ffr_input_clear;
    bool writes_nzcv;
    bool ffr_initializes_all_true;
    bool ffr_clears_from_fault_boundary;
} arg_sve_effect_intent;

typedef enum {
    SVE_PRED_OP_AND = 0,
    SVE_PRED_OP_ORR,
    SVE_PRED_OP_EOR,
    SVE_PRED_OP_BIC,
    SVE_PRED_OP_ORN,
    SVE_PRED_OP_NOR,
    SVE_PRED_OP_NAND,
    SVE_PRED_OP_SEL,
    SVE_PRED_OP_PTEST,
    SVE_PRED_OP_BRKA,
    SVE_PRED_OP_BRKB,
    SVE_PRED_OP_BRKN,
    SVE_PRED_OP_PFIRST,
    SVE_PRED_OP_PNEXT,
} SvePredSubop;

typedef enum {
    SVE_CNT_SUBOP_CNTP = 0,
    SVE_CNT_SUBOP_CNT,
    SVE_CNT_SUBOP_INC,
    SVE_CNT_SUBOP_DEC,
    SVE_CNT_SUBOP_SQINC,
    SVE_CNT_SUBOP_UQINC,
    SVE_CNT_SUBOP_SQDEC,
    SVE_CNT_SUBOP_UQDEC,
} SveCntSubop;

typedef enum {
    SVE_FP_EXT_FMLA = 0,
    SVE_FP_EXT_FMLS,
    SVE_FP_EXT_FNMLA,
    SVE_FP_EXT_FNMLS,
    SVE_FP_EXT_FMINNM,
    SVE_FP_EXT_FMAXNM,
    SVE_FP_EXT_FMIN,
    SVE_FP_EXT_FMAX,
    SVE_FP_EXT_FSQRT,
    SVE_FP_EXT_FRECPE,
    SVE_FP_EXT_FRSQRTE,
    SVE_FP_EXT_FRECPS,
    SVE_FP_EXT_FRSQRTS,
} SveFpExtSubop;

typedef enum {
    SVE_FCVT_F_TO_F = 0,
    SVE_FCVT_F_TO_S,
    SVE_FCVT_F_TO_U,
    SVE_FCVT_S_TO_F,
    SVE_FCVT_U_TO_F,
} SveFcvtMode;

typedef enum {
    SVE_SHIFT_IMM_LSL = 0,
    SVE_SHIFT_IMM_LSR,
    SVE_SHIFT_IMM_ASR,
    SVE_SHIFT_IMM_ASRD,
} SveShiftImmSubop;

typedef enum {
    SVE_UNARY_CLZ = 0,
    SVE_UNARY_CLS,
    SVE_UNARY_CNT,
    SVE_UNARY_RBIT,
    SVE_UNARY_ABS,
    SVE_UNARY_NEG,
    SVE_UNARY_CNOT,
    SVE_UNARY_SXTB,
    SVE_UNARY_SXTH,
    SVE_UNARY_SXTW,
    SVE_UNARY_UXTB,
    SVE_UNARY_UXTH,
    SVE_UNARY_UXTW,
} SveUnarySubop;

typedef enum {
    SVE_IMM_BIN_AND = 0,
    SVE_IMM_BIN_ORR,
    SVE_IMM_BIN_EOR,
    SVE_IMM_BIN_ADD,
    SVE_IMM_BIN_SUB,
    SVE_IMM_BIN_SUBR,
    SVE_IMM_BIN_MUL,
    SVE_IMM_BIN_DUP,
} SveImmBinSubop;

typedef enum {
    SVE_WMUL_MULL = 0,
    SVE_WMUL_MLA,
    SVE_WMUL_MLS,
} SveWmulSubop;

typedef enum {
    SVE_IDIV_SDIV = 0,
    SVE_IDIV_UDIV,
    SVE_IDIV_SDIVR,
    SVE_IDIV_UDIVR,
} SveIdivSubop;

typedef struct {
    uint8_t pd;
    uint8_t element_bits;
    uint8_t pattern;
    /* PTRUES additionally writes PSTATE.NZCV from the PredTest result. */
    bool setflags;
    arg_sve_effect_intent effects;
} arg_sve_ptrue;

typedef struct {
    uint8_t pd;
    uint8_t rn;
    uint8_t rm;
    uint8_t element_bits;
    uint8_t scalar_bits;
    PvirCompareCondition cond;
    bool is_conflict;
    bool conflict_wr;
    arg_sve_effect_intent effects;
} arg_sve_whilelo;

typedef struct {
    uint8_t zd;
    uint8_t pg;
    uint8_t zm;
    uint8_t element_bits;
    bool is_signed;
    arg_sve_effect_intent effects;
} arg_sve_integer_binary;
typedef arg_sve_integer_binary arg_sve_add;
typedef arg_sve_integer_binary arg_sve_orr;
typedef arg_sve_integer_binary arg_sve_and;
typedef arg_sve_integer_binary arg_sve_eor;
typedef arg_sve_integer_binary arg_sve_lsl;
typedef arg_sve_integer_binary arg_sve_lsr;
typedef arg_sve_integer_binary arg_sve_asr;
typedef arg_sve_integer_binary arg_sve_sub;
typedef arg_sve_integer_binary arg_sve_smax;
typedef arg_sve_integer_binary arg_sve_umax;
typedef arg_sve_integer_binary arg_sve_smin;
typedef arg_sve_integer_binary arg_sve_umin;
typedef arg_sve_integer_binary arg_sve_mul;
/* Zdn is both the addend source and the merge value; /M is the only policy. */
typedef arg_sve_integer_binary arg_sve_fp_binary;
typedef arg_sve_fp_binary arg_sve_fadd;
typedef arg_sve_fp_binary arg_sve_fsub;
typedef arg_sve_fp_binary arg_sve_fmul;
typedef arg_sve_fp_binary arg_sve_fdiv;
/* Unary predicated sign-bit ops: Zd = op(Zn), inactive lanes hold old Zd. */
typedef arg_sve_integer_binary arg_sve_fp_unary;
typedef arg_sve_fp_unary arg_sve_fabs;
typedef arg_sve_fp_unary arg_sve_fneg;
typedef arg_sve_fp_unary arg_sve_int_unary;
typedef arg_sve_int_unary arg_sve_not;

typedef struct {
    uint8_t zd;
    uint8_t rn;          /* guest GPR holding the scalar to broadcast */
    uint8_t element_bits; /* 8/16/32/64 */
    arg_sve_effect_intent effects;
} arg_sve_dup;

typedef struct {
    uint8_t zt;
    uint8_t pg;
    uint8_t rn;
    uint8_t element_bits;
    uint8_t memory_bits;
    int8_t immediate_vl;
    /* LD1SB/LD1SH/LD1SW sign-extend the memory element into the lane. */
    bool sign_extend;
    bool base_is_sp;
    arg_sve_effect_intent effects;
} arg_sve_contiguous;

typedef struct {
    uint8_t pd;
    uint8_t pg;
    uint8_t pn;
    bool has_pd;
    bool predicated;
    bool setflags;
    bool is_write;
    arg_sve_effect_intent effects;
} arg_sve_ffr;

typedef struct {
    uint8_t zt;
    uint8_t pg;
    uint8_t rn;
    uint8_t rm;
    uint8_t element_bits;
    uint8_t memory_bits;
    bool sign_extend;
    bool base_is_sp;
    bool has_index;
    arg_sve_effect_intent effects;
} arg_sve_fault_load;

typedef struct {
    uint8_t zt;
    uint8_t pg;
    uint8_t rn;
    uint8_t element_bits;
    uint8_t memory_bits;
    int8_t immediate_vl;
    bool sign_extend;
    bool base_is_sp;
    arg_sve_effect_intent effects;
} arg_sve_nonfault_load;

typedef struct {
    uint8_t pd;
    uint8_t pg;
    uint8_t pn;
    uint8_t pm;
    uint8_t element_bits;
    uint8_t subop; /* AND/ORR/EOR/BIC/ORN/NOR/NAND/SEL / BRKA/BRKB/BRKN / PTEST/PFIRST/PNEXT */
    bool merge;
    bool setflags;
    arg_sve_effect_intent effects;
} arg_sve_pred_op;

typedef struct {
    uint8_t rd;
    uint8_t pg;
    uint8_t pn;
    uint8_t element_bits;
    uint8_t pattern;
    uint8_t imm_mul;
    uint8_t subop; /* 0=CNTP, 1=CNTx, 2=INCx, 3=DECx, 4=SQINCx, 5=UQINCx, 6=SQDECx, 7=UQDECx */
    bool is_64;
    bool is_signed;
    arg_sve_effect_intent effects;
} arg_sve_cnt;

typedef struct {
    uint8_t rd_zd;
    uint8_t pg;
    uint8_t zm;
    uint8_t element_bits;
    bool is_after;      /* LASTA / CLASTA vs LASTB / CLASTB */
    bool is_cond;       /* CLAST* vs LAST* */
    bool dst_is_vector; /* true -> Zreg / FP Vreg, false -> X/W GPR */
    arg_sve_effect_intent effects;
} arg_sve_last;

typedef enum {
    SVE_MEM_SCALAR_REG = 0,
    SVE_MEM_STRUCT_IMM,
    SVE_MEM_STRUCT_REG,
    SVE_MEM_REP_IMM,
    SVE_MEM_REP_QUAD,
    SVE_MEM_GATHER_VEC_BASE,
    SVE_MEM_GATHER_VEC_OFFSET,
    SVE_MEM_NT_IMM,
    SVE_MEM_NT_REG,
} SveMemAddrKind;

typedef struct {
    uint8_t zt;
    uint8_t pg;
    uint8_t rn;
    uint8_t rm;
    uint8_t nregs; /* 1..4 for LD1..LD4 */
    uint8_t element_bits;
    uint8_t memory_bits;
    int32_t imm;
    bool sign_extend;
    bool base_is_sp;
    bool is_load;
    bool scaled;
    bool offset_is_signed; /* sxtw vs uxtw/lsl */
    bool offset_is_64;     /* Zm.D vs Zm.S */
    SveMemAddrKind kind;
    arg_sve_effect_intent effects;
} arg_sve_mem_ext;

typedef struct {
    uint8_t zd;
    uint8_t pg;
    uint8_t zn;
    uint8_t zm;
    uint8_t za;
    uint8_t element_bits;
    uint8_t subop; /* FMLA/FMLS/FNMLA/FNMLS/FMAD/FMSB/FNMAD/FNMSB / FMIN/FMAX/FMINNM/FMAXNM / FSQRT/FRECPE/FRSQRTE/FRECPS/FRSQRTS */
    bool destructive_madd; /* FMAD/FMSB: Zd is first multiplicand */
    arg_sve_effect_intent effects;
} arg_sve_fp_ext;

typedef struct {
    uint8_t zd_pd;
    uint8_t pg;
    uint8_t zn;
    uint8_t zm;
    uint8_t src_bits;
    uint8_t dst_bits;
    uint8_t subop; /* condition / FCVT mode / integer op kind */
    int64_t imm;
    bool cmp_with_imm;
    bool is_float;
    bool is_signed;
    bool predicated;
    arg_sve_effect_intent effects;
} arg_sve_ext_op;

typedef struct {
    uint8_t zd;
    uint8_t pg;
    uint8_t zn;
    uint8_t zm;
    uint8_t za;
    uint8_t element_bits;
    uint8_t subop; /* REDUCE kind, PERMUTE kind, FIXED kind, UNARY kind, WMUL kind, INDEX kind, IMM_BIN kind */
    int64_t imm1;
    int64_t imm2;
    uint64_t mask_imm;
    bool predicated;
    bool is_signed;
    bool src1_is_imm;
    bool src2_is_imm;
    bool top_half;
    bool destructive;
    arg_sve_effect_intent effects;
} arg_sve_general;

typedef struct {
    uint32_t raw_insn;
    int recognized_type;
} arg_sve_access_failure;

typedef struct {
    uint32_t raw_insn;
} arg_sve_unsupported_encoding;

typedef struct {
    uint32_t raw_insn;
    int recognized_type;
} arg_sve_lasx_failure;
/* arguments for translation function*/
union a64_u{
    arg_atomic f_atomic;
    arg_bitfield f_bitfield;
    arg_bra f_bra;
    arg_braz f_braz;
    arg_cbz f_cbz;
    arg_decode_insn3211 f_decode_insn3211;
    arg_decode_insn3215 f_decode_insn3215;
    arg_decode_insn3216 f_decode_insn3216;
    arg_decode_insn3217 f_decode_insn3217;
    arg_decode_insn3218 f_decode_insn3218;
    arg_decode_insn3221 f_decode_insn3221;
    arg_decode_insn3227 f_decode_insn3227;
    arg_decode_insn3228 f_decode_insn3228;
    arg_decode_insn3232 f_decode_insn3232;
    arg_extract f_extract;
    arg_i f_i;
    arg_ldapr_stlr_i f_ldapr_stlr_i;
    arg_ldlit f_ldlit;
    arg_ldst f_ldst;
    arg_ldst_imm f_ldst_imm;
    arg_ldst_mult f_ldst_mult;
    arg_ldst_single f_ldst_single;
    arg_ldst_tag f_ldst_tag;
    arg_ldstpair f_ldstpair;
    arg_movw f_movw;
    arg_r f_r;
    arg_reta f_reta;
    arg_ri f_ri;
    arg_rri_log f_rri_log;
    arg_rri_sf f_rri_sf;
    arg_rri_tag f_rri_tag;
    arg_stlr f_stlr;
    arg_stxr f_stxr;
    arg_tbz f_tbz;
    arg_sve_ptrue f_sve_ptrue;
    arg_sve_whilelo f_sve_whilelo;
    arg_sve_add f_sve_add;
    arg_sve_orr f_sve_orr;
    arg_sve_and f_sve_and;
    arg_sve_eor f_sve_eor;
    arg_sve_lsl f_sve_lsl;
    arg_sve_lsr f_sve_lsr;
    arg_sve_asr f_sve_asr;
    arg_sve_sub f_sve_sub;
    arg_sve_mul f_sve_mul;
    arg_sve_smax f_sve_smax;
    arg_sve_umax f_sve_umax;
    arg_sve_smin f_sve_smin;
    arg_sve_umin f_sve_umin;
    arg_sve_fadd f_sve_fadd;
    arg_sve_fsub f_sve_fsub;
    arg_sve_fmul f_sve_fmul;
    arg_sve_fdiv f_sve_fdiv;
    arg_sve_fp_unary f_sve_fabs;
    arg_sve_fp_unary f_sve_fneg;
    arg_sve_int_unary f_sve_not;
    arg_sve_dup f_sve_dup;
    arg_sve_contiguous f_sve_contiguous;
    arg_sve_ffr f_sve_ffr;
    arg_sve_fault_load f_sve_fault_load;
    arg_sve_nonfault_load f_sve_nonfault_load;
    arg_sve_pred_op f_sve_pred_op;
    arg_sve_cnt f_sve_cnt;
    arg_sve_last f_sve_last;
    arg_sve_mem_ext f_sve_mem_ext;
    arg_sve_fp_ext f_sve_fp_ext;
    arg_sve_ext_op f_sve_ext_op;
    arg_sve_general f_sve_general;
    arg_sve_access_failure f_sve_access_failure;
    arg_sve_unsupported_encoding f_sve_unsupported_encoding;
    arg_sve_lasx_failure f_sve_lasx_failure;
} ;

#endif