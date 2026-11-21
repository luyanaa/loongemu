#ifndef LATR_H
#define LATR_H

#include <stdint.h>
#include "qemu/osdep.h"
#include "exec/exec-all.h"
#include "tcg/tcg.h"
#include "mem.h"
#include "ir2.h"
#include "la-append.h"
#include "macro-inst.h"
#include "riscv-env.h"


typedef struct TRANSLATION_DATA {
    void *curr_tb;
    uint32_t w_write_flag;
    IR2_INST *ir2_inst_array;
    int ir2_inst_num_max;
    int ir2_inst_num_current;
    int real_ir2_inst_num;
    IR2_INST *first_ir2;
    IR2_INST *last_ir2;
    int label_num;
    union {
        uint32_t all_temp_status;
        struct {
            uint16_t itemp_status;
            uint16_t ftemp_status;
        };
    };
    uint8_t curr_ir1_skipped_eflags;
} TRANSLATION_DATA;

typedef struct ENV {
    void *cpu_state;
    TRANSLATION_DATA *tr_data;
} ENV;

#include "reg-alloc.h"

void latr_tr_data_init(CPUArchState *env);
void tr_init(void *tb);
void tr_fini(void);
int tr_ir2_assemble(const void *code_start_addr);
void label_dispose(void);

extern __thread TRANSLATION_DATA *tr_data;
extern __thread ENV *lsenv;
extern bool latr_dump;
extern const int latr_la_map[32];
extern const int latr_la_fmap[32];

IR2_OPND alloc_gpr_src(int reg);
IR2_OPND alloc_gpr_dst(int reg);
void store_gpr_dst(int reg, IR2_OPND opnd);
void free_alloc_gpr(IR2_OPND opnd);
IR2_OPND alloc_fpr_src(int reg);
IR2_OPND alloc_fpr_dst(int reg);
void store_fpr_dst(int reg, IR2_OPND opnd);
void free_alloc_fpr(IR2_OPND opnd);

uint16_t latr_guest_lduw(uint64_t address);
uint32_t latr_guest_ldl(uint64_t address);
void target_disasm(TranslationBlock *tb, int *max_insns, CPUState *cpu);
bool tr_ir2_generate(TranslationBlock *tb);

#endif
