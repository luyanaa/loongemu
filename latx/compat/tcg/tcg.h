#ifndef LATX_BARE_TCG_H
#define LATX_BARE_TCG_H
#include "qemu/osdep.h"
#include "exec/exec-all.h"

typedef struct TCGContext {
    uint8_t  *code_gen_buffer;
    size_t    code_gen_buffer_size;
    uint8_t  *code_gen_highwater;
    uint8_t  *code_gen_ptr;
    uint64_t *gen_insn_data;
    uint16_t  gen_insn_end_off[TCG_MAX_INSNS];
    int       prof;
    uint32_t  tb_cflags;
    uint8_t  *tb_gen_highwater;
    uint8_t  *tb_gen_ptr;
    uint16_t  tb_jmp_insn_offset[2];
    uint16_t  tb_jmp_reset_offset[2];
    uintptr_t tb_jmp_target_addr[2];
    TranslationBlock *tb;
    TranslationBlock *gen_tb;
    uint8_t *code_ptr;
    void *tb_gen_tail;
} TCGContext;

typedef uintptr_t TCGArg;
typedef int TCGOpcode;
typedef uint16_t TCGOpIdx;

extern TCGContext *tcg_ctx;
extern uint64_t latx_tcg_gen_insn_data[TCG_MAX_INSNS * 2];

#define TCG_MO_LD_LD 1
#define TCG_MO_LD_ST 2
#define TCG_MO_ST_LD 4
#define TCG_MO_ST_ST 8
#define TCG_MO_ALL 15
#define TCG_BAR_SC 0x100
void tcg_gen_mb(int flags);
#endif
