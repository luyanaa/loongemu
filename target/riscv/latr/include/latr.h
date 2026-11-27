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
#include "loongemu/riscv_sbi.h"

/* Unit harnesses may translate through control-flow terminators and page
 * crossings in one block; the real TB dispatcher leaves this clear. */
#define LATR_TB_CFLAGS_ALLOW_FLOW (UINT32_C(1) << 30)

/*
 * Host TLS for translator state. The freestanding LoongArch build has no TLS
 * runtime, so it compiles to plain globals; a VMM host build defines
 * LATR_ENABLE_TLS so every vCPU thread gets its own context pointers.
 */
#if defined(LATR_ENABLE_TLS)
#define LATR_TLS _Thread_local
#else
#define LATR_TLS
#endif


/* Native-offset -> guest-PC spans for precise exception attribution. */
#define LATR_TB_MAP_SPANS 512
/* Bounded binding pool per map: a guest-insn span only carries a binding set
 * when it can fault architecturally (load/store/amo/trap) AND the regcache
 * held uncommitted guest state there. 6 itemps cap the live set anyway. */
#define LATR_TB_RECOVERY_BINDS 512
#define LATR_RECOVERY_MAX_BINDS_PER_SPAN 8

#include "le-recovery.h"

/* Span rows and binding pool entries come from shared/recovery. The port
 * aliases keep call sites readable while the layout is the shared schema.
 * aux carries the guest insn raw_len (bytes) for fetch-fault decode. */
typedef LeRecoverySiteKind LatrRecoverySite;
#define LATR_RECOV_GUEST_ACCESS    LE_RECOV_GUEST_ACCESS
#define LATR_RECOV_GUEST_CHECK     LE_RECOV_GUEST_CHECK
#define LATR_RECOV_HELPER          LE_RECOV_HELPER
#define LATR_RECOV_NATIVE_INTERNAL LE_RECOV_NATIVE_INTERNAL

typedef LeRecoveryBinding LatrRecoveryBinding;
typedef LeRecoverySpan    LatrTbInsnSpan;
_Static_assert(sizeof(LatrTbInsnSpan) == sizeof(LeRecoverySpan),
               "LatrTbInsnSpan must stay the shared recovery row");


#define latr_span_raw_len(s)      ((s)->aux)
#define latr_span_set_raw_len(s,v) ((s)->aux = (v))

typedef struct LatrTbInsnMap {
    /* Where the code lives *now* (the arena slot after rebase); this is what
     * host-PC lookups match. */
    uintptr_t code_base;
    /*
     * Where the block was assembled, i.e. the translator's staging buffer.
     * latr_tb_insn_map_rebase() moves code_base but the next block is always
     * assembled at the same staging address, so begin() must find this map by
     * the staging address -- otherwise every block allocated a fresh ~22 KiB
     * map and a long boot exhausted the freestanding allocator.
     */
    uintptr_t stage_base;
    uint32_t code_size;
    uint32_t count;
    uint32_t generation;
    uint32_t bind_pool_used;
    bool valid;
    struct LatrTbInsnMap *next;
    LatrTbInsnSpan spans[LATR_TB_MAP_SPANS];
    LatrRecoveryBinding binds[LATR_TB_RECOVERY_BINDS];
} LatrTbInsnMap;

typedef struct LatrRegCacheEntry {
    int8_t temp;
    uint8_t valid;
    uint8_t dirty;
    uint64_t age;
} LatrRegCacheEntry;

typedef struct LatrRegCache {
    LatrRegCacheEntry gpr[32];
    LatrRegCacheEntry fpr[32];
    uint8_t itemp_borrows[6];
    uint8_t ftemp_borrows[6];
    uint64_t clock;
    /*
     * Dirty counts per bank. Measured on the ISA image: 12801 flushes for 282
     * writebacks, i.e. 98% of flushes walk 64 entries to find nothing. A bank
     * whose count is zero can skip the walk entirely; the release loop below
     * still runs because a clean entry must still leave the cache at a
     * control-flow edge.
     */
    uint8_t gpr_dirty;
    uint8_t fpr_dirty;
    /* Resident-count per bank: a bank with no valid entry makes the flush's
     * release loop and writeback scan both no-ops, so skip them by count. */
    uint8_t gpr_valid;
    uint8_t fpr_valid;
} LatrRegCache;



/*
 * Host-address code symbols (design: docs/design/persistent-code-cache.md, P0).
 *
 * Emitted code materialises a handful of absolute host addresses - the helper
 * entry points it calls. Those are the only addresses in a block that a
 * persistent cache cannot simply copy: they change with PIE/ASLR and with any
 * rebuild. Recording them at emission turns "which words need relocation" into
 * an enumerable list instead of a code scan.
 */
/* Measured on the ISA suite: 485 sites over 286 blocks, at most 20 in one
 * block, so 24 was one block away from truncating a relocation list. */
#define LATR_TB_CODE_SYMS 64

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
    LatrRegCache regcache;
    uint8_t curr_ir1_skipped_eflags;
    /* Filled by tr_ir2_generate(), consumed by tr_ir2_assemble(). */
    int tb_insn_count;
    uint64_t tb_insn_pc[LATR_TB_MAP_SPANS];
    uint32_t tb_insn_len[LATR_TB_MAP_SPANS];
    int tb_insn_label[LATR_TB_MAP_SPANS + 1];
    /* Host addresses emitted by this block, with the IR2 index of the
     * instruction that materialised each one. */
    int tb_code_sym_count;
    uint8_t tb_code_sym_overflow;
    int tb_code_sym_ir2[LATR_TB_CODE_SYMS];
    /* li_d expands into several IR2 instructions, so a symbol owns the half-open
     * IR2 index range [start, end) rather than a single instruction. */
    int tb_code_sym_ir2_end[LATR_TB_CODE_SYMS];
    const void *tb_code_sym_fn[LATR_TB_CODE_SYMS];
    uint8_t tb_code_sym_reg[LATR_TB_CODE_SYMS];
    /* Filled by tr_ir2_assemble(): the word span that materialises the symbol.
     * li_d is a variable-length expansion, so the count is measured, not
     * assumed - see docs/design/persistent-code-cache.md for why that matters
     * to relocation. */
    uint32_t tb_code_sym_word[LATR_TB_CODE_SYMS];
    uint32_t tb_code_sym_words[LATR_TB_CODE_SYMS];
    /* Per-insn recovery staging (C1): site kind + regcache binding snapshot
     * captured during translate, committed into the map at assemble. */
    uint8_t tb_insn_site[LATR_TB_MAP_SPANS];
    uint8_t tb_insn_phase[LATR_TB_MAP_SPANS];
    uint8_t tb_insn_bind_cnt[LATR_TB_MAP_SPANS];
    LatrRecoveryBinding tb_insn_binds[LATR_TB_MAP_SPANS]
                                     [LATR_RECOVERY_MAX_BINDS_PER_SPAN];

    /* Native-offset -> guest-PC attribution for precise exceptions. Maps are
     * owned by the TB code range they describe (a live LatrTbEntry's arena
     * slot), NOT an LRU pool: a map lives in insn_map_list until its code is
     * forgotten/rebased away, so an old still-executing TB always resolves. */
    LatrTbInsnMap *insn_map_list;
    uint32_t insn_map_count;
    uint32_t insn_map_generation;
    LatrTbInsnMap *active_insn_map;
    /*
     * Recycled maps. A map is ~22 KiB (512 spans + 512 bindings) and a boot
     * translates hundreds of thousands of blocks, so one calloc per assembled
     * block exhausts the freestanding 32 MiB arena in ~1500 blocks and the
     * harness dies in malloc -- which looks like a translator bug. The arena
     * recycles whole code ranges every few hundred blocks, so dropping those
     * maps to the allocator and reusing them here keeps the working set at the
     * number of live TBs instead of the number of TBs ever translated.
     */
    LatrTbInsnMap *insn_map_free_list;
} TRANSLATION_DATA;

typedef struct ENV {
    void *cpu_state;
    TRANSLATION_DATA *tr_data;
    /* Per-context dirty-flush hook for ir2_stream_rollback(). */
    void (*ir2_dirty_flush_fn)(void *opaque);
    void *ir2_dirty_flush_opaque;
} ENV;

struct LatrTbCache;
typedef struct LatrPvirCache LatrPvirCache;

/*
 * All translator state belonging to one vCPU. A VMM thread binds its hart's
 * context with latr_context_bind(); `tr_data`/`lsenv` then point into this
 * object, so two harts never share IR2 lists, labels, register allocation,
 * instruction maps or the PVIR descriptor cache.
 */
typedef struct LatrTranslatorContext {
    TRANSLATION_DATA tr_data;
    ENV env;
    CPUArchState *cpu;
    struct LatrTbCache *tb_cache;
    LatrPvirCache *pvir_cache;
    uint64_t pvir_generation;
    uint8_t *code_buf;
    uint32_t code_buf_size;
} LatrTranslatorContext;

LatrTranslatorContext *latr_context_create(CPUArchState *cpu,
                                           struct LatrTbCache *tb_cache,
                                           uint8_t *code_buf,
                                           uint32_t code_buf_size);
void latr_context_destroy(LatrTranslatorContext *ctx);
/*
 * Makes `ctx` the current hart on this host thread and returns the context
 * that was current before, so a fault dispatcher can borrow another hart and
 * put the caller's context back afterwards. With LATR_ENABLE_TLS this is a
 * per-thread switch; without it (freestanding build, one hart at a time) it is
 * still correct because every dispatch rebinds first.
 */
LatrTranslatorContext *latr_context_bind(LatrTranslatorContext *ctx);
/* Restores what latr_context_bind() returned; NULL simply unbinds. */
void latr_context_restore(LatrTranslatorContext *ctx);
LatrTranslatorContext *latr_context_current(void);
/*
 * Per-hart safe point: reclaims the TB arena once no translation is live and
 * drops this hart's PVIR cache when its TB generation moved. Call between TB
 * dispatches, never while executing generated code.
 */
void latr_context_safepoint(LatrTranslatorContext *ctx);

#include "reg-alloc.h"

#include "latr-tb-cache.h"

/* Key over the pieces of CPUArchState that make a translation ambiguous. */
static inline LatrTbKey latr_tb_cache_key(const CPUArchState *env)
{
    if (env == 0) {
        LatrTbKey zero = { 0, 0, 0, 0, 0 };

        return zero;
    }
    return latr_tb_cache_key_parts(env->csr[0x180], (uint32_t)env->privilege,
                                   (uint32_t)env->vtype, (uint32_t)env->vill,
                                   (uint32_t)env->vl);
}

/* Switches the runtime to another vCPU's environment. */
void latr_set_cpu_env(CPUArchState *env);

/* Per-vCPU PVIR descriptor cache (see translator/pvir-exec.c). */
void latr_pvir_cache_clear(void);
void latr_pvir_cache_reset(LatrPvirCache *cache);
LatrPvirCache *latr_pvir_cache_create(void);
void latr_pvir_cache_destroy(LatrPvirCache *cache);

void latr_tb_insn_map_begin(void *code, uint32_t code_size);
void latr_tb_insn_map_add(uint64_t guest_pc, uint32_t native_start,
                          uint32_t native_end, uint32_t raw_len,
                          uint8_t site, uint8_t phase, uint32_t retired_before,
                          const LatrRecoveryBinding *binds, uint8_t bind_cnt);
bool latr_tb_insn_map_resolve(uintptr_t host_pc, uint64_t *guest_pc,
                              uint32_t *raw_len);
void latr_tb_insn_map_rebase(void *old_code, void *new_code);
void latr_tb_insn_map_forget(void *code);
/*
 * Drops every map whose code range intersects [base, base + size). Called when
 * the TB arena is recycled: the offsets those maps describe no longer stand
 * for the same instructions, and a host fault inside a reused arena must not be
 * blamed on the guest PC that used to live there.
 */
void latr_tb_insn_map_forget_range(void *base, uint32_t size);

/* C1 precise recovery: extend a resolved span with its fault-site kind, the
 * count of guest insns retired before it, and the regcache bindings that must
 * be replayed to reconstruct architectural guest state. `latr_tb_recovery_
 * resolve` fills `*row` with the span covering host_pc (NULL-safe outs);
 * returns false if host_pc is not inside a mapped TB. `latr_tb_recovery_apply`
 * replays the row's bindings from captured host GPRs (ucontext order = the
 * host's `$r0..$r31` array) into `env`, sets env->pc, and returns false when
 * the row's site is NATIVE_INTERNAL (a host bug, never projected). */
const LatrTbInsnSpan *latr_tb_recovery_resolve(uintptr_t host_pc,
                                               const LatrTbInsnMap **map_out);
bool latr_tb_recovery_apply(const LatrTbInsnSpan *span,
                            const LatrTbInsnMap *map,
                            const uint64_t host_gprs[32],
                            const uint64_t host_fprs[32],
                            CPUArchState *env);

void latr_tr_data_init(CPUArchState *env);

/* Services an S-mode ECALL through the VMM-side SBI dispatcher. */
bool latr_service_sbi(CPUArchState *env, LoongemuSbi *sbi);

/* RVV vsetvl helper: validates vtype, updates vl/vtype/vstart/vill. */
uint64_t latr_rvv_vsetvl(CPUArchState *env, uint64_t s1, uint64_t vtype);

/* Interrupt state maintenance and delivery (Sstc + PLIC + VMM mip writes). */
void latr_update_interrupts(CPUArchState *env);
bool latr_interrupt_pending(CPUArchState *env);
bool latr_deliver_interrupt(CPUArchState *env);
void latr_idle_advance(CPUArchState *env);
/* Runtime synchronous-exception entry: the C mirror of the generated
 * transfer_exception() (M/S target via medeleg, mstatus, epc/cause/tval,
 * non-vectored trap-vector PC). Used by the native-fault dispatcher. */
void latr_take_guest_trap(CPUArchState *env, uint64_t cause, uint64_t tval);
void tr_init(void *tb);
void tr_fini(void);
int tr_ir2_assemble(const void *code_start_addr);
/* Emitted words and instruction count per guest opcode (measurement only).
 * Sized by LATR_RV_OPCODE_COUNT, so ir1.h must be visible here. */
#include "ir1.h"
/*
 * Emitter tags. Every IR2 records the site that emitted it, so "who produced
 * these 15 env-field address computations" is a histogram rather than a guess.
 */
enum {
    LATR_TAG_INSN = 0,   /* the guest instruction's own translation */
    LATR_TAG_INSTRET,    /* latr_emit_instret() */
    LATR_TAG_RESOLVE,    /* latr_emit_resolve() */
    LATR_TAG_MMIO,       /* MMIO dispatch call */
    LATR_TAG_WRITEBACK,  /* register-cache writeback */
    LATR_TAG_RELEASE,    /* register-cache release / temp free */
    LATR_TAG_PC_EXIT,    /* store_pc_constant + block exit */
    LATR_TAG_FAULT,      /* branch-on-fault / trap entry */
    LATR_TAG_OTHER,      /* anything else */
    LATR_TAG_COUNT
};

extern unsigned latr_current_tag;
extern uint64_t latr_tag_ir2[LATR_TAG_COUNT];
const char *latr_tag_name(unsigned tag);
/* Sets the tag for every IR2 emitted until the next call. */
void latr_tag(unsigned tag);

extern uint64_t latr_flush_ir2;
extern uint64_t latr_flush_emit_calls;
extern uint32_t latr_insn_worst_ir2;
extern uint64_t latr_insn_worst_pc;
extern uint32_t latr_insn_worst_opcode;
extern uint64_t latr_block_entry_ir2;
extern uint64_t latr_block_exit_ir2;
extern uint64_t latr_block_ir2_total;
extern uint64_t latr_block_words_total;
extern uint64_t latr_block_count;
extern uint64_t latr_block_worst_ratio;
extern uint64_t latr_block_worst_pc;
extern uint32_t latr_block_worst_icount;
extern uint32_t latr_block_worst_ir2;
extern uint32_t latr_block_worst_words;
extern uint64_t latr_opcode_ir2[LATR_RV_OPCODE_COUNT];
extern uint64_t latr_opcode_insns[LATR_RV_OPCODE_COUNT];
/* Name of an opcode, for the histogram report. */
const char *latr_opcode_name(unsigned opcode);
void label_dispose(void);

extern LATR_TLS TRANSLATION_DATA *tr_data;
extern LATR_TLS ENV *lsenv;
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

/* TB-local guest register cache hooks. */
void latr_regcache_reset(void);
/*
 * Register-cache accounting (design: docs/design/ir2-value-version.md, P0).
 *
 * Pure bookkeeping for deciding whether the versioned-state work is worth
 * building: how often the cache is flushed, how many guest-state stores that
 * emits, how often a value has to be reloaded from env, and how often a resident
 * value is dropped to free a slot. Nothing here may change translation, and the
 * counters live outside LatrRegCache so latr_regcache_reset() does not clear
 * them per block.
 */
typedef struct LatrRegCacheStats {
    uint64_t flushes;    /* latr_regcache_flush() calls */
    uint64_t empty_flushes; /* ... of which found entries but nothing dirty */
    uint64_t scan_entries;  /* entries examined by flush/eviction scans */
    uint64_t writebacks; /* guest-state stores emitted (flush and eviction) */
    uint64_t reloads;    /* env loads because the value was not resident */
    uint64_t stores;     /* guest-register writes taken by store_gpr_dst() */
    uint64_t evictions;  /* resident entries dropped to free a slot */
    /*
     * 5.1 step 4 evidence (see docs/design/ir2-value-version.md section 5):
     *   elided_flushes    flush calls that returned before any scan because
     *                     nothing was resident - pure call overhead;
     *   release_only      flushes with resident-but-clean entries, i.e. the
     *                     flush only freed temporaries and wrote nothing;
     *   dead_stores       a guest register's dirty value replaced by a newer
     *                     write with no writeback in between. This is the
     *                     dead-write case: the old value never reaches memory,
     *                     so there is nothing left to eliminate.
     */
    uint64_t elided_flushes;
    uint64_t release_only_flushes;
    uint64_t dead_stores;
    /*
     * Translation-time invariant: every borrowed itemp/ftemp must be returned
     * before the next guest instruction is translated. A leak here means a
     * temporary is never freed (or freed twice), so later translations in the
     * same block can be handed a register that still belongs to a guest value -
     * which shows up as wrong code far from the instruction that caused it.
     * Counted per instruction, with the first offending opcode kept.
     */
    uint64_t borrow_leaks;
    uint64_t borrow_leak_opcode;
    uint64_t borrow_outstanding_max;
} LatrRegCacheStats;

void latr_regcache_stats_reset(void);
/* Number of itemp/ftemp slots currently borrowed (both banks). */
unsigned latr_regcache_outstanding_borrows(void);
LatrRegCacheStats latr_regcache_stats(void);

void latr_regcache_flush(void);

/* Emit `dst = sym` and record the symbol as a relocation site. */
void latr_emit_code_symbol(IR2_OPND dst, const void *sym);
int latr_tb_code_symbol_count(void);
const void *latr_tb_code_symbol(int index);
int latr_tb_code_symbol_ir2(int index);
int latr_tb_code_symbols_overflowed(void);
/* Word span of the symbol's materialisation inside the assembled block. */
int latr_tb_code_symbol_span(int index, uint32_t *word_start,
                             uint32_t *word_count);
/* Physical register the symbol is loaded into (needed to re-emit the load). */
int latr_tb_code_symbol_reg(int index);
bool latr_regcache_evict_itemp(void);
bool latr_regcache_evict_ftemp(void);
void latr_regcache_prepare_fpr_write(IR2_OPND value);

/*
 * Register-cache state must agree across every control-flow edge. Flush on
 * both branch emission and target labels: a branch can bypass the fallthrough
 * flush immediately before its label.
 */
static inline IR2_INST *latr_regcache_label(IR2_OPND label)
{
    latr_regcache_flush();
    return (la_label)(label);
}

static inline IR2_INST *latr_regcache_jirl(IR2_OPND rd, IR2_OPND rj,
                                            int offset)
{
    latr_regcache_flush();
    return (la_jirl)(rd, rj, offset);
}

static inline IR2_INST *latr_regcache_b(IR2_OPND label)
{
    latr_regcache_flush();
    return (la_b)(label);
}

static inline IR2_INST *latr_regcache_beqz(IR2_OPND reg, IR2_OPND label)
{
    latr_regcache_flush();
    return (la_beqz)(reg, label);
}

static inline IR2_INST *latr_regcache_bnez(IR2_OPND reg, IR2_OPND label)
{
    latr_regcache_flush();
    return (la_bnez)(reg, label);
}

static inline IR2_INST *latr_regcache_beq(IR2_OPND lhs, IR2_OPND rhs,
                                           IR2_OPND label)
{
    latr_regcache_flush();
    return (la_beq)(lhs, rhs, label);
}

static inline IR2_INST *latr_regcache_bne(IR2_OPND lhs, IR2_OPND rhs,
                                           IR2_OPND label)
{
    latr_regcache_flush();
    return (la_bne)(lhs, rhs, label);
}

static inline IR2_INST *latr_regcache_blt(IR2_OPND lhs, IR2_OPND rhs,
                                           IR2_OPND label)
{
    latr_regcache_flush();
    return (la_blt)(lhs, rhs, label);
}

static inline IR2_INST *latr_regcache_bge(IR2_OPND lhs, IR2_OPND rhs,
                                           IR2_OPND label)
{
    latr_regcache_flush();
    return (la_bge)(lhs, rhs, label);
}

static inline IR2_INST *latr_regcache_bltu(IR2_OPND lhs, IR2_OPND rhs,
                                            IR2_OPND label)
{
    latr_regcache_flush();
    return (la_bltu)(lhs, rhs, label);
}

static inline IR2_INST *latr_regcache_bgeu(IR2_OPND lhs, IR2_OPND rhs,
                                            IR2_OPND label)
{
    latr_regcache_flush();
    return (la_bgeu)(lhs, rhs, label);
}

#define la_label(label) latr_regcache_label(label)
#define la_jirl(rd, rj, offset) latr_regcache_jirl(rd, rj, offset)
#define la_b(label) latr_regcache_b(label)
#define la_beqz(reg, label) latr_regcache_beqz(reg, label)
#define la_bnez(reg, label) latr_regcache_bnez(reg, label)
#define la_beq(lhs, rhs, label) latr_regcache_beq(lhs, rhs, label)
#define la_bne(lhs, rhs, label) latr_regcache_bne(lhs, rhs, label)
#define la_blt(lhs, rhs, label) latr_regcache_blt(lhs, rhs, label)
#define la_bge(lhs, rhs, label) latr_regcache_bge(lhs, rhs, label)
#define la_bltu(lhs, rhs, label) latr_regcache_bltu(lhs, rhs, label)
#define la_bgeu(lhs, rhs, label) latr_regcache_bgeu(lhs, rhs, label)
void free_alloc_fpr(IR2_OPND opnd);

uint16_t latr_guest_lduw(uint64_t address);
uint32_t latr_guest_ldl(uint64_t address);
void target_disasm(TranslationBlock *tb, int *max_insns, CPUState *cpu);
bool tr_ir2_generate(TranslationBlock *tb);

#endif
