#ifndef LATM_H
#define LATM_H

#include <stdint.h>
#include "qemu/osdep.h"
#include "tcg/tcg.h"
#include "exec/exec-all.h"
#include "mem.h"
#include "ir2.h"
#include "la-append.h"
#include "macro-inst.h"

#ifdef B_STUB_SIZE
#undef B_STUB_SIZE
#endif
#define B_STUB_SIZE 4
#define TARGET_INSN_START_WORDS 2

typedef struct TRANSLATION_DATA {
    void *curr_tb;

    /* ir2 */
    IR2_INST *ir2_inst_array;
    int ir2_inst_num_max;
    int ir2_inst_num_current;
    int real_ir2_inst_num;

    IR2_INST *first_ir2;
    IR2_INST *last_ir2;

    int label_num;

    union {
        uint32 all_temp_status;
        struct {
            uint16 itemp_status;
            uint16 ftemp_status;
        };
    };
} TRANSLATION_DATA;

typedef struct ENV {
    void *cpu_state;
    TRANSLATION_DATA *tr_data;
} ENV;

void lata_tr_data_init(void *env);
void tr_init(void *tb);
void tr_fini(void);

extern const int mips_la_map[];
extern const int mips_la_fmap[];
extern __thread TRANSLATION_DATA *tr_data;
extern __thread ENV *lsenv;
extern bool lata_dump;

extern uint64_t context_switch_bt_to_native;
extern uint64_t context_switch_native_to_bt_ret_0;
extern uint64_t context_switch_native_to_bt;

int lata_gen_prologue(TCGContext *tcg_ctx);
int lata_gen_epilogue(TCGContext *tcg_ctx);
void lata_fast_jmp_cache_init(void *env, uint64_t start_code, uint64_t end_code);
void lata_fast_jmp_cache_add(void *env, uint64_t guest_pc, uint64_t host_pc);

int tr_ir2_assemble(const void *code_start_addr);
void label_dispose(void);

void lata_gen_call_helper_prologue(TCGContext *tcg_ctx);
void lata_gen_call_helper_epilogue(TCGContext *tcg_ctx);

void lata_env_vld(IR2_OPND vector, int offset);
void lata_env_vst(IR2_OPND vector, int offset);

/* CPUMIPSState is larger than the signed 12-bit displacement range. */
void latm_env_ld_d(IR2_OPND dst, int offset);
void latm_env_st_d(IR2_OPND val, int offset);
void latm_env_ld_w(IR2_OPND dst, int offset);
void latm_env_st_w(IR2_OPND val, int offset);

#include "reg-alloc.h"

IR2_OPND alloc_gpr_src(int i);
IR2_OPND alloc_gpr_dst(int i);
void store_gpr_dst(int i, IR2_OPND opnd);
void store_gpr32_dst(int i, IR2_OPND opnd);
void store_gpr_zero(int i);
void free_alloc_gpr(IR2_OPND opnd);

IR2_OPND alloc_fpr_src(int i);
IR2_OPND alloc_fpr_dst(int i);
void store_fpr_dst(int i, IR2_OPND opnd);
void free_alloc_fpr(IR2_OPND opnd);

#endif
