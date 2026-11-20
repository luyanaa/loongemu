#ifndef LATM_TRANSLATE_H
#define LATM_TRANSLATE_H

#include "qemu/osdep.h"
#include "exec/exec-all.h"
#include "ir1.h"
#include "latm.h"

typedef enum {
    DISAS_NEXT,
    DISAS_TOO_MANY,
    DISAS_UPDATE_EXIT,
    DISAS_EXIT,
    DISAS_NORETURN,
} DisasJumpType;

#ifndef TARGET_PAGE_SIZE
#define TARGET_PAGE_SIZE 4096
#endif
#ifndef TARGET_PAGE_MASK
#define TARGET_PAGE_MASK (~(TARGET_PAGE_SIZE - 1))
#endif
#ifndef TCG_MAX_INSNS
#define TCG_MAX_INSNS 512
#endif

typedef struct DisasContextBase {
    TranslationBlock *tb;
    uint64_t pc_first;
    uint64_t pc_next;
    int is_jmp;
    int num_insns;
    int max_insns;
    bool singlestep_enabled;
    void *host_addr[2];
} DisasContextBase;

typedef struct DisasContext {
    DisasContextBase *base;
    MipsIR1 ir1;
} DisasContext;

typedef bool (*TranslateFn)(DisasContext *s);

extern TranslateFn translate_functions[MIPS_OP__COUNT];

void target_disasm(TranslationBlock *tb, int *max_insns, void *cpu);
bool tr_ir2_generate(TranslationBlock *tb);

#endif
