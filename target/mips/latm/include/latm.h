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
#define LATM_TB_MAP_MAX TCG_MAX_INSNS
#define LATM_TB_MAP_POOL 32

typedef struct LatmTbInsnSpan {
    uint64_t guest_pc;
    uint32_t native_start;
    uint32_t native_end;
    uint8_t delay_slot;
} LatmTbInsnSpan;

typedef struct LatmTbInsnMap LatmTbInsnMap;
struct LatmTbInsnMap {
    uintptr_t code_base;
    uint32_t code_size;
    uint32_t generation;
    uint16_t count;
    uint8_t valid;
    uint8_t reserved;
    LatmTbInsnMap *next;
    LatmTbInsnSpan spans[LATM_TB_MAP_MAX];
};

typedef struct LatmRegCacheEntry {
    int8_t temp;
    uint8_t valid;
    uint8_t dirty;
    uint64_t age;
} LatmRegCacheEntry;

typedef struct LatmRegCache {
    LatmRegCacheEntry gpr[32];
    uint8_t itemp_borrows[3];
    uint64_t clock;
} LatmRegCache;

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
    void *ir1_list;
    void *db_rel;
    uint64_t cache_key;
    LatmTbInsnMap *active_insn_map;
    LatmTbInsnMap *insn_map_list;
    uint32_t insn_map_count;
    uint32_t insn_map_generation;
    uint64_t cs_bt_to_native;
    uint64_t cs_native_to_bt_ret_0;
    uint64_t cs_native_to_bt;
    bool lock_held;

    union {
        uint32 all_temp_status;
        struct {
            uint16 itemp_status;
            uint16 ftemp_status;
        };
    };
    LatmRegCache regcache;
} TRANSLATION_DATA;

typedef struct ENV {
    void *cpu_state;
    TRANSLATION_DATA *tr_data;
} ENV;

/* The shared IR2 backend (shared/backend/loongarch/ir2.c) tags its dump
 * lines through a LataContext. LATM has no ARM translation context, so the
 * shim below carries only the field the dump path reads. See the matching
 * shim in target/riscv/latr/latr.c. */
typedef struct LataContext {
    int ir2_dump_ir1_id;
} LataContext;

LataContext *lata_context_current(void);

#define LATM_JMP_CACHE_WAYS 2
#define LATM_JMP_CACHE_SETS 512
#define LATM_JMP_CACHE_SIZE (LATM_JMP_CACHE_SETS * LATM_JMP_CACHE_WAYS)
#define LATM_JMP_CACHE_SET_MASK (LATM_JMP_CACHE_SETS - 1)

/* 32-byte entries: set-associative lookup is performed in C. */
typedef struct LatmJmpCacheEntry {
    uint64_t guest_pc;
    uint64_t host_pc;
    uint64_t key;          /* ASID | (hflags mode << 8) */
    uint32_t guest_size;
    uint32_t age;
} LatmJmpCacheEntry;

extern bool latm_have_lbt;
extern bool latm_link_use_jiscr1;

static inline uint64_t latm_cache_key(void *cpu)
{
    CPUMIPSState *env = cpu;
    return ((uint64_t)env->CP0_EntryHi & 0xff) |
           ((uint64_t)(env->hflags & (MIPS_HFLAG_UM | MIPS_HFLAG_SM)) << 8);
}

void latm_bind_env(void *cpu);
void latm_translate_lock(void);
void latm_translate_unlock(void);
void latm_jmp_cache_add(void *cpu, uint64_t guest_pc, uint64_t host_pc,
                        uint32_t guest_size);
void latm_jmp_cache_flush(void *cpu);
uint64_t latm_jmp_cache_lookup(void *cpu, uint64_t guest_pc);
void latm_tb_insn_map_begin(void *trd, const void *code, uint32_t code_size);
void latm_tb_insn_map_add(void *trd, uint64_t guest_pc, uint32_t native_start,
                          uint32_t native_end, uint8_t delay_slot);
int latm_tb_insn_map_resolve(void *trd, uintptr_t host_pc,
                             uint64_t *guest_pc, uint8_t *delay_slot);
void latm_tb_insn_map_flush(void *trd);
void latm_tb_insn_map_forget(void *trd, const void *code);
void latm_tb_insn_map_release(void *trd, void *tb);
void latm_tb_register(void *cpu, void *tb);
void latm_platform_smc_broadcast(uint64_t start, uint64_t end);

void lata_tr_data_init(void *env);
void tr_init(void *tb);
void tr_fini(void);

extern const int mips_la_map[];
extern const int mips_la_fmap[];
#if defined(CONFIG_LATM_TRANSLATE_TLS)
#define LATM_TLS _Thread_local
#else
#define LATM_TLS
#endif
extern LATM_TLS TRANSLATION_DATA *tr_data;
extern LATM_TLS ENV *lsenv;
extern bool lata_dump;
extern bool latm_system_memory;
void latm_set_system_memory(bool on);


int lata_gen_prologue(TCGContext *tcg_ctx);
int lata_gen_epilogue(TCGContext *tcg_ctx);
int lata_gen_prologue_env(void *cpu, TCGContext *tcg_ctx);
int lata_gen_epilogue_env(void *cpu, TCGContext *tcg_ctx);
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

/* TB-local cache for env-resident MIPS GPRs. */
void latm_regcache_reset(void);
void latm_regcache_flush(void);
bool latm_regcache_evict_itemp(void);

/* Commit cached GPRs across control-flow edges and indirect exits. */
static inline IR2_INST *latm_regcache_label(IR2_OPND label)
{
    latm_regcache_flush();
    return (la_label)(label);
}

static inline IR2_INST *latm_regcache_jirl(IR2_OPND rd, IR2_OPND rj,
                                            int offset)
{
    latm_regcache_flush();
    return (la_jirl)(rd, rj, offset);
}

static inline IR2_INST *latm_regcache_b(IR2_OPND label)
{
    latm_regcache_flush();
    return (la_b)(label);
}

static inline IR2_INST *latm_regcache_beqz(IR2_OPND reg, IR2_OPND label)
{
    latm_regcache_flush();
    return (la_beqz)(reg, label);
}

static inline IR2_INST *latm_regcache_bnez(IR2_OPND reg, IR2_OPND label)
{
    latm_regcache_flush();
    return (la_bnez)(reg, label);
}

static inline IR2_INST *latm_regcache_beq(IR2_OPND lhs, IR2_OPND rhs,
                                           IR2_OPND label)
{
    latm_regcache_flush();
    return (la_beq)(lhs, rhs, label);
}

static inline IR2_INST *latm_regcache_bne(IR2_OPND lhs, IR2_OPND rhs,
                                           IR2_OPND label)
{
    latm_regcache_flush();
    return (la_bne)(lhs, rhs, label);
}

static inline IR2_INST *latm_regcache_blt(IR2_OPND lhs, IR2_OPND rhs,
                                           IR2_OPND label)
{
    latm_regcache_flush();
    return (la_blt)(lhs, rhs, label);
}

static inline IR2_INST *latm_regcache_bge(IR2_OPND lhs, IR2_OPND rhs,
                                           IR2_OPND label)
{
    latm_regcache_flush();
    return (la_bge)(lhs, rhs, label);
}

static inline IR2_INST *latm_regcache_bltu(IR2_OPND lhs, IR2_OPND rhs,
                                            IR2_OPND label)
{
    latm_regcache_flush();
    return (la_bltu)(lhs, rhs, label);
}

static inline IR2_INST *latm_regcache_bgeu(IR2_OPND lhs, IR2_OPND rhs,
                                            IR2_OPND label)
{
    latm_regcache_flush();
    return (la_bgeu)(lhs, rhs, label);
}

#if defined(LATM_REGCACHE_WRAP)
/* Opt-in macro shadow: only translator TUs that #define LATM_REGCACHE_WRAP
 * before including this header get the flushing wrappers.  Shared emitter TUs
 * (la-append.c via reg-alloc.h) must see the real la_* declarations. */
#define la_label(label) latm_regcache_label(label)
#define la_jirl(rd, rj, offset) latm_regcache_jirl(rd, rj, offset)
#define la_b(label) latm_regcache_b(label)
#define la_beqz(reg, label) latm_regcache_beqz(reg, label)
#define la_bnez(reg, label) latm_regcache_bnez(reg, label)
#define la_beq(lhs, rhs, label) latm_regcache_beq(lhs, rhs, label)
#define la_bne(lhs, rhs, label) latm_regcache_bne(lhs, rhs, label)
#define la_blt(lhs, rhs, label) latm_regcache_blt(lhs, rhs, label)
#define la_bge(lhs, rhs, label) latm_regcache_bge(lhs, rhs, label)
#define la_bltu(lhs, rhs, label) latm_regcache_bltu(lhs, rhs, label)
#define la_bgeu(lhs, rhs, label) latm_regcache_bgeu(lhs, rhs, label)
#endif

#endif
