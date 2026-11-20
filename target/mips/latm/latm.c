/*
 * LATM runtime: MIPS guest register map, IR2 assembly, and TB entry/exit.
 *
 * The context-switch shape is adapted from LATA (target/arm/lata/lata.c).
 * Guest FPRs live in CPUMIPSState; FPU instructions call C helpers with the
 * shared softfloat status, so no FPR is pinned to a host register.
 */
#include "qemu/osdep.h"
#include "latm.h"
#include "mem.h"
#include "translate.h"
#include "reg-alloc.h"
#include "reg-map.h"
#include "env.h"
#include "internal.h"
#include "macro-inst.h"

/* MIPS guest -> LoongArch GPR map.  $0 is virtual; $31 uses $ra and is
 * saved/restored around helper calls. */
const int mips_la_map[32] = {
    [0]  = NOT_DEF,
    [1]  = la_a1,  [2]  = la_a2,  [3]  = la_a3,  [4]  = la_a4,
    [5]  = la_a5,  [6]  = la_a6,  [7]  = la_a7,
    [8]  = la_t3,  [9]  = la_t4,  [10] = la_t5,  [11] = la_t6,
    [12] = la_t7,  [13] = la_t8,  [14] = la_fp,
    [15] = la_s0,  [16] = la_s1,  [17] = la_s2,  [18] = la_s3,
    [19] = la_s4,  [20] = la_s5,  [21] = la_s6,  [22] = la_s7,
    [23] = NOT_DEF, [24] = NOT_DEF, [25] = NOT_DEF,
    [26] = NOT_DEF, [27] = NOT_DEF, [28] = NOT_DEF,
    [29] = la_s8,  [30] = la_r21, [31] = la_ra,
};

const int mips_la_reverse_map[32] = {
    [la_zero] = NOT_DEF, [la_ra] = 31, [la_tp] = NOT_DEF,
    [la_sp] = NOT_DEF, [la_a0] = NOT_DEF,
    [la_a1] = 1, [la_a2] = 2, [la_a3] = 3, [la_a4] = 4,
    [la_a5] = 5, [la_a6] = 6, [la_a7] = 7,
    [la_t0] = NOT_DEF, [la_t1] = NOT_DEF, [la_t2] = NOT_DEF,
    [la_t3] = 8, [la_t4] = 9, [la_t5] = 10, [la_t6] = 11,
    [la_t7] = 12, [la_t8] = 13, [la_r21] = 30, [la_fp] = 14,
    [la_s0] = 15, [la_s1] = 16, [la_s2] = 17, [la_s3] = 18,
    [la_s4] = 19, [la_s5] = 20, [la_s6] = 21, [la_s7] = 22,
    [la_s8] = 29,
};

/* All FPRs live in CPUMIPSState. */
const int mips_la_fmap[32] = {
    [0 ... 31] = NOT_DEF,
};
const int mips_la_reverse_fmap[32] = {
    [0 ... 31] = NOT_DEF,
};

__thread ENV *lsenv;
static __thread ENV lsenv_real;
static __thread TRANSLATION_DATA tr_data_real;
__thread TRANSLATION_DATA *tr_data;

bool lata_dump;
uint64_t context_switch_bt_to_native;
uint64_t context_switch_native_to_bt_ret_0;
uint64_t context_switch_native_to_bt;

static void global_register_init(void)
{
    env_ir2_opnd = INIT_RA(IR2_OPND_GPR, reg_statics_map[S_ENV]);
    zero_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_zero);
    ra_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_ra);
    tp_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_tp);
    sp_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_sp);
    a0_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a0);
    a1_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a1);
    a2_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a2);
    a3_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a3);
    a4_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a4);
    a5_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a5);
    a6_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a6);
    a7_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a7);
    t0_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t0);
    t1_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t1);
    t2_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t2);
    t3_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t3);
    t4_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t4);
    t5_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t5);
    t6_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t6);
    t7_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t7);
    t8_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t8);
    r21_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_r21);
    fp_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_fp);
    s0_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s0);
    s1_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s1);
    s2_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s2);
    s3_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s3);
    s4_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s4);
    s5_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s5);
    s6_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s6);
    s7_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s7);
    s8_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s8);
    fsmask_ir2_opnd = INIT_RA(IR2_OPND_FPR, la_fsmask);
    fcsr_ir2_opnd = INIT_RA(IR2_OPND_FCSR, 0);
    fcsr1_ir2_opnd = INIT_RA(IR2_OPND_FCSR, 1);
    fcsr2_ir2_opnd = INIT_RA(IR2_OPND_FCSR, 2);
    fcsr3_ir2_opnd = INIT_RA(IR2_OPND_FCSR, 3);
    fcc0_ir2_opnd = INIT_RA(IR2_OPND_CC, 0);
    fcc1_ir2_opnd = INIT_RA(IR2_OPND_CC, 1);
    fcc2_ir2_opnd = INIT_RA(IR2_OPND_CC, 2);
    fcc3_ir2_opnd = INIT_RA(IR2_OPND_CC, 3);
    fcc4_ir2_opnd = INIT_RA(IR2_OPND_CC, 4);
    fcc5_ir2_opnd = INIT_RA(IR2_OPND_CC, 5);
    fcc6_ir2_opnd = INIT_RA(IR2_OPND_CC, 6);
    fcc7_ir2_opnd = INIT_RA(IR2_OPND_CC, 7);
    scr0_ir2_opnd = INIT_RA(IR2_OPND_SCR, 0);
    scr1_ir2_opnd = INIT_RA(IR2_OPND_SCR, 1);
    scr2_ir2_opnd = INIT_RA(IR2_OPND_SCR, 2);
    scr3_ir2_opnd = INIT_RA(IR2_OPND_SCR, 3);
}

void lata_tr_data_init(void *env)
{
    lsenv = &lsenv_real;
    lsenv->cpu_state = env;
    lsenv->tr_data = &tr_data_real;
    global_register_init();
}

void tr_init(void *tb)
{
    TRANSLATION_DATA *t = lsenv->tr_data;

    t->curr_tb = tb;
    ra_free_all();

    if (t->ir2_inst_array == NULL) {
        t->ir2_inst_array = (IR2_INST *)mm_calloc(1024, sizeof(IR2_INST));
        t->ir2_inst_num_max = 1024;
    }
    t->ir2_inst_num_current = 0;
    t->real_ir2_inst_num = 0;
    t->first_ir2 = NULL;
    t->last_ir2 = NULL;
    t->label_num = 0;
}

void tr_fini(void)
{
    TRANSLATION_DATA *t = lsenv->tr_data;
    t->ir2_inst_num_current = 0;
    t->real_ir2_inst_num = 0;
    t->first_ir2 = NULL;
    t->last_ir2 = NULL;
    t->label_num = 0;
}

#define EXTRA_SPACE 40
#define REG_LEN     8
#define S0_EXTRA_SPACE  EXTRA_SPACE
#define S1_EXTRA_SPACE  (S0_EXTRA_SPACE + REG_LEN)
#define S2_EXTRA_SPACE  (S1_EXTRA_SPACE + REG_LEN)
#define S3_EXTRA_SPACE  (S2_EXTRA_SPACE + REG_LEN)
#define S4_EXTRA_SPACE  (S3_EXTRA_SPACE + REG_LEN)
#define S5_EXTRA_SPACE  (S4_EXTRA_SPACE + REG_LEN)
#define S6_EXTRA_SPACE  (S5_EXTRA_SPACE + REG_LEN)
#define S7_EXTRA_SPACE  (S6_EXTRA_SPACE + REG_LEN)
#define S8_EXTRA_SPACE  (S7_EXTRA_SPACE + REG_LEN)
#define FP_EXTRA_SPACE  (S8_EXTRA_SPACE + REG_LEN)
#define RA_EXTRA_SPACE  (FP_EXTRA_SPACE + REG_LEN)

static void emit_gpr_from_env(IR2_OPND opnd, int guest_reg)
{
    latm_env_ld_d(opnd, env_offset_gpr(guest_reg));
}

static void emit_gpr_to_env(IR2_OPND opnd, int guest_reg)
{
    latm_env_st_d(opnd, env_offset_gpr(guest_reg));
}

void latm_env_ld_d(IR2_OPND dst, int offset)
{
    if (offset >= -2048 && offset <= 2047) {
        la_ld_d(dst, env_ir2_opnd, offset);
        return;
    }
    li_d(a0_ir2_opnd, offset);
    la_add_d(a0_ir2_opnd, env_ir2_opnd, a0_ir2_opnd);
    la_ld_d(dst, a0_ir2_opnd, 0);
}

void latm_env_st_d(IR2_OPND val, int offset)
{
    if (offset >= -2048 && offset <= 2047) {
        la_st_d(val, env_ir2_opnd, offset);
        return;
    }
    li_d(a0_ir2_opnd, offset);
    la_add_d(a0_ir2_opnd, env_ir2_opnd, a0_ir2_opnd);
    la_st_d(val, a0_ir2_opnd, 0);
}

void latm_env_ld_w(IR2_OPND dst, int offset)
{
    if (offset >= -2048 && offset <= 2047) {
        la_ld_w(dst, env_ir2_opnd, offset);
        return;
    }
    li_d(a0_ir2_opnd, offset);
    la_add_d(a0_ir2_opnd, env_ir2_opnd, a0_ir2_opnd);
    la_ld_w(dst, a0_ir2_opnd, 0);
}

void latm_env_st_w(IR2_OPND val, int offset)
{
    if (offset >= -2048 && offset <= 2047) {
        la_st_w(val, env_ir2_opnd, offset);
        return;
    }
    li_d(a0_ir2_opnd, offset);
    la_add_d(a0_ir2_opnd, env_ir2_opnd, a0_ir2_opnd);
    la_st_w(val, a0_ir2_opnd, 0);
}

static void generate_context_switch_bt_to_native(void)
{
    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, -256);
    la_st_d(s0_ir2_opnd, sp_ir2_opnd, S0_EXTRA_SPACE);
    la_st_d(s1_ir2_opnd, sp_ir2_opnd, S1_EXTRA_SPACE);
    la_st_d(s2_ir2_opnd, sp_ir2_opnd, S2_EXTRA_SPACE);
    la_st_d(s3_ir2_opnd, sp_ir2_opnd, S3_EXTRA_SPACE);
    la_st_d(s4_ir2_opnd, sp_ir2_opnd, S4_EXTRA_SPACE);
    la_st_d(s5_ir2_opnd, sp_ir2_opnd, S5_EXTRA_SPACE);
    la_st_d(s6_ir2_opnd, sp_ir2_opnd, S6_EXTRA_SPACE);
    la_st_d(s7_ir2_opnd, sp_ir2_opnd, S7_EXTRA_SPACE);
    la_st_d(s8_ir2_opnd, sp_ir2_opnd, S8_EXTRA_SPACE);
    la_st_d(fp_ir2_opnd, sp_ir2_opnd, FP_EXTRA_SPACE);
    la_st_d(ra_ir2_opnd, sp_ir2_opnd, RA_EXTRA_SPACE);

    la_mov64(env_ir2_opnd, a0_ir2_opnd);
    /* a0 is not a guest register (r4 maps to a4); keep the TB target there. */
    la_mov64(a0_ir2_opnd, a1_ir2_opnd);

    for (int i = 1; i < 32; ++i) {
        if (mips_la_map[i] > 0) {
            emit_gpr_from_env(ir2_opnd_new(IR2_OPND_GPR, mips_la_map[i]), i);
        }
    }

    la_jirl(zero_ir2_opnd, a0_ir2_opnd, 0);
}

static void generate_context_switch_native_to_bt(void)
{
    la_mov64(a0_ir2_opnd, zero_ir2_opnd);

    for (int i = 1; i < 32; ++i) {
        if (mips_la_map[i] > 0) {
            emit_gpr_to_env(ir2_opnd_new(IR2_OPND_GPR, mips_la_map[i]), i);
        }
    }

    la_ld_d(s0_ir2_opnd, sp_ir2_opnd, S0_EXTRA_SPACE);
    la_ld_d(s1_ir2_opnd, sp_ir2_opnd, S1_EXTRA_SPACE);
    la_ld_d(s2_ir2_opnd, sp_ir2_opnd, S2_EXTRA_SPACE);
    la_ld_d(s3_ir2_opnd, sp_ir2_opnd, S3_EXTRA_SPACE);
    la_ld_d(s4_ir2_opnd, sp_ir2_opnd, S4_EXTRA_SPACE);
    la_ld_d(s5_ir2_opnd, sp_ir2_opnd, S5_EXTRA_SPACE);
    la_ld_d(s6_ir2_opnd, sp_ir2_opnd, S6_EXTRA_SPACE);
    la_ld_d(s7_ir2_opnd, sp_ir2_opnd, S7_EXTRA_SPACE);
    la_ld_d(s8_ir2_opnd, sp_ir2_opnd, S8_EXTRA_SPACE);
    la_ld_d(fp_ir2_opnd, sp_ir2_opnd, FP_EXTRA_SPACE);
    la_ld_d(ra_ir2_opnd, sp_ir2_opnd, RA_EXTRA_SPACE);
    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, 256);

    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
}

int lata_gen_prologue(TCGContext *tcg_ctx)
{
    void *code_buf_rw = tcg_ctx->code_ptr;
    const void *code_buf_rx = tcg_splitwx_to_rx(code_buf_rw);
    context_switch_bt_to_native = (uint64_t)code_buf_rx;

    tr_init(NULL);
    generate_context_switch_bt_to_native();
    int ins_nr = tr_ir2_assemble(code_buf_rw);
    tr_fini();
    return ins_nr;
}

int lata_gen_epilogue(TCGContext *tcg_ctx)
{
    void *code_buf_rw = tcg_ctx->code_ptr;
    const void *code_buf_rx = tcg_splitwx_to_rx(code_buf_rw);
    context_switch_native_to_bt_ret_0 = (uint64_t)code_buf_rx;
    context_switch_native_to_bt = (uint64_t)code_buf_rx + 4;

    tr_init(NULL);
    generate_context_switch_native_to_bt();
    int ins_nr = tr_ir2_assemble(code_buf_rw);
    tr_fini();
    return ins_nr;
}

void lata_gen_call_helper_prologue(TCGContext *tcg_ctx)
{
    (void)tcg_ctx;
    for (int i = 1; i < 32; ++i) {
        int la = mips_la_map[i];
        if (la == la_a1 || la == la_a2 || la == la_a3 || la == la_a4 ||
            la == la_a5 || la == la_a6 || la == la_a7 ||
            la == la_t3 || la == la_t4 || la == la_t5 ||
            la == la_t6 || la == la_t7 || la == la_t8 || la == la_ra) {
            emit_gpr_to_env(ir2_opnd_new(IR2_OPND_GPR, la), i);
        }
    }
}

void lata_gen_call_helper_epilogue(TCGContext *tcg_ctx)
{
    (void)tcg_ctx;
    for (int i = 1; i < 32; ++i) {
        int la = mips_la_map[i];
        if (la == la_a1 || la == la_a2 || la == la_a3 || la == la_a4 ||
            la == la_a5 || la == la_a6 || la == la_a7 ||
            la == la_t3 || la == la_t4 || la == la_t5 ||
            la == la_t6 || la == la_t7 || la == la_t8 || la == la_ra) {
            emit_gpr_from_env(ir2_opnd_new(IR2_OPND_GPR, la), i);
        }
    }
}

void label_dispose(void)
{
    int *label_num_to_ir2_num =
        (int *)alloca(lsenv->tr_data->label_num * sizeof(int) + 20);
    memset(label_num_to_ir2_num, -1, lsenv->tr_data->label_num * sizeof(int) + 20);
    int ir2_num = 0;
    IR2_INST *ir2_current = lsenv->tr_data->first_ir2;

    while (ir2_current != NULL) {
        if (ir2_current->_opcode == LISA_LABEL) {
            int label_num = ir2_opnd_label_id(&ir2_current->_opnd[0]);
            label_num_to_ir2_num[label_num] = ir2_num;
        } else if (ir2_current->_opcode != LISA_X86_INST) {
            ir2_num++;
        }
        ir2_current = ir2_next(ir2_current);
    }

    {
        TranslationBlock *tb = lsenv->tr_data->curr_tb;
        if (tb) {
            int label_id_0 = tb->jmp_reset_offset[0];
            if (label_id_0 != TB_JMP_OFFSET_INVALID) {
                tb->jmp_insn_offset[0] = label_num_to_ir2_num[label_id_0] << 2;
                tb->jmp_reset_offset[0] =
                    (label_num_to_ir2_num[label_id_0] << 2) + B_STUB_SIZE;
            }
            int label_id_1 = tb->jmp_reset_offset[1];
            if (label_id_1 != TB_JMP_OFFSET_INVALID) {
                tb->jmp_insn_offset[1] = label_num_to_ir2_num[label_id_1] << 2;
                tb->jmp_reset_offset[1] =
                    (label_num_to_ir2_num[label_id_1] << 2) + B_STUB_SIZE;
            }
        }
    }

    ir2_num = 0;
    ir2_current = lsenv->tr_data->first_ir2;
    while (ir2_current != NULL) {
        IR2_OPCODE opcode = ir2_current->_opcode;
        if (ir2_opcode_is_branch(opcode) || ir2_opcode_is_f_branch(opcode)) {
            IR2_OPND *label_opnd = NULL;
            if (opcode == LISA_B || opcode == LISA_BL) {
                label_opnd = &ir2_current->_opnd[0];
            } else if (ir2_opcode_is_branch_with_3opnds(opcode)) {
                label_opnd = &ir2_current->_opnd[2];
            } else if (ir2_opcode_is_f_branch(opcode) ||
                       ir2_opcode_is_branch_with_2opnds(opcode)) {
                label_opnd = &ir2_current->_opnd[1];
            }
            if (label_opnd && ir2_opnd_is_label(label_opnd)) {
                int label_num = label_opnd->_label_id;
                int target_ir2_num = label_num_to_ir2_num[label_num];
                if (target_ir2_num >= 0) {
                    ir2_opnd_convert_label_to_imm(label_opnd,
                                                  target_ir2_num - ir2_num);
                }
            }
        }
        if (opcode != LISA_LABEL && opcode != LISA_X86_INST) {
            ir2_num++;
        }
        ir2_current = ir2_next(ir2_current);
    }
}

int tr_ir2_assemble(const void *code_start_addr)
{
    label_dispose();

    IR2_INST *pir2 = lsenv->tr_data->first_ir2;
    void *code_addr = (void *)code_start_addr;
    int code_nr = 0;

    while (pir2 != NULL) {
        if (ir2_opcode(pir2) != LISA_LABEL && ir2_opcode(pir2) != LISA_X86_INST) {
            uint32 result = ir2_assemble(pir2);
            *(uint32 *)code_addr = result;
            code_addr = (char *)code_addr + 4;
            code_nr++;
        }
        pir2 = ir2_next(pir2);
    }
    return code_nr;
}

IR2_OPND alloc_gpr_src(int i)
{
    if (i == 0) {
        return zero_ir2_opnd;
    }
    if (mips_la_map[i] >= 0) {
        return ir2_opnd_new(IR2_OPND_GPR, mips_la_map[i]);
    }
    IR2_OPND t = ra_alloc_itemp();
    latm_env_ld_d(t, env_offset_gpr(i));
    return t;
}

IR2_OPND alloc_gpr_dst(int i)
{
    if (i == 0) {
        return ra_alloc_itemp();
    }
    if (mips_la_map[i] >= 0) {
        return ir2_opnd_new(IR2_OPND_GPR, mips_la_map[i]);
    }
    return ra_alloc_itemp();
}

void store_gpr_dst(int i, IR2_OPND opnd)
{
    if (i == 0) {
        return;
    }
    if (mips_la_map[i] < 0) {
        latm_env_st_d(opnd, env_offset_gpr(i));
    } else if (opnd._reg_num != mips_la_map[i]) {
        la_mov64(ir2_opnd_new(IR2_OPND_GPR, mips_la_map[i]), opnd);
    }
}

void store_gpr32_dst(int i, IR2_OPND opnd)
{
    if (i == 0) {
        return;
    }
    la_slli_w(opnd, opnd, 0);
    store_gpr_dst(i, opnd);
}

void store_gpr_zero(int i)
{
    if (i == 0) {
        return;
    }
    if (mips_la_map[i] < 0) {
        latm_env_st_d(zero_ir2_opnd, env_offset_gpr(i));
    } else {
        la_ori(ir2_opnd_new(IR2_OPND_GPR, mips_la_map[i]), zero_ir2_opnd, 0);
    }
}

void free_alloc_gpr(IR2_OPND opnd)
{
    if (opnd._type == IR2_OPND_GPR && opnd._reg_num != la_zero &&
        mips_la_reverse_map[opnd._reg_num] == NOT_DEF) {
        ra_free_itemp(opnd._reg_num);
    }
}

IR2_OPND alloc_fpr_src(int i)
{
    IR2_OPND t = ra_alloc_ftemp();
    la_fld_d(t, env_ir2_opnd, env_offset_fpr(i));
    return t;
}

IR2_OPND alloc_fpr_dst(int i)
{
    (void)i;
    return ra_alloc_ftemp();
}

void store_fpr_dst(int i, IR2_OPND opnd)
{
    la_fst_d(opnd, env_ir2_opnd, env_offset_fpr(i));
}

void free_alloc_fpr(IR2_OPND opnd)
{
    if (opnd._type == IR2_OPND_FPR) {
        ra_free_ftemp(opnd._reg_num);
    }
}

void lata_env_vld(IR2_OPND vector, int offset)
{
    la_vld(vector, env_ir2_opnd, offset);
}

void lata_env_vst(IR2_OPND vector, int offset)
{
    la_vst(vector, env_ir2_opnd, offset);
}

void lata_fast_jmp_cache_init(void *env, uint64_t start_code, uint64_t end_code)
{
    (void)env;
    (void)start_code;
    (void)end_code;
}

void lata_fast_jmp_cache_add(void *env, uint64_t guest_pc, uint64_t host_pc)
{
    (void)env;
    (void)guest_pc;
    (void)host_pc;
}
