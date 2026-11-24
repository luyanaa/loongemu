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
#define LATR_TB_MAP_SLOTS 4
#define LATR_TB_MAP_SPANS 512
/* Bounded binding pool per map: a guest-insn span only carries a binding set
 * when it can fault architecturally (load/store/amo/trap) AND the regcache
 * held uncommitted guest state there. 6 itemps cap the live set anyway. */
#define LATR_TB_RECOVERY_BINDS 512
#define LATR_RECOVERY_MAX_BINDS_PER_SPAN 8

typedef enum LatrRecoverySite {
    LATR_RECOV_GUEST_ACCESS = 0, /* guest load/store/amo: fault projects to guest */
    LATR_RECOV_GUEST_CHECK,      /* explicit guest trap (ecall/break/mret/...) */
    LATR_RECOV_HELPER,           /* helper call that may fault (reserved) */
    LATR_RECOV_NATIVE_INTERNAL,  /* fault here = host bug; do not project */
} LatrRecoverySite;

typedef struct LatrRecoveryBinding {
    uint8_t guest_reg;   /* guest GPR index holding an uncommitted value */
    int8_t  temp;        /* host itemp slot (reg_itemp_map[temp] -> phys reg) */
} LatrRecoveryBinding;

typedef struct LatrTbInsnSpan {
    uint64_t guest_pc;
    uint32_t native_start;
    uint32_t native_end;
    uint32_t raw_len;
    /* Precise-recovery metadata (C1): site kind + guest-insn-retired count +
     * a slice of the map's binding pool for uncommitted regcache state. */
    uint8_t  site;             /* LatrRecoverySite */
    uint8_t  bind_cnt;
    uint16_t bind_first;
    uint32_t retired_before;
} LatrTbInsnSpan;

typedef struct LatrTbInsnMap {
    uintptr_t code_base;
    uint32_t code_size;
    uint32_t count;
    uint32_t generation;
    uint32_t bind_pool_used;
    bool valid;
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
} LatrRegCache;



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
    /* Per-insn recovery staging (C1): site kind + regcache binding snapshot
     * captured during translate, committed into the map at assemble. */
    uint8_t tb_insn_site[LATR_TB_MAP_SPANS];
    uint8_t tb_insn_bind_cnt[LATR_TB_MAP_SPANS];
    LatrRecoveryBinding tb_insn_binds[LATR_TB_MAP_SPANS]
                                     [LATR_RECOVERY_MAX_BINDS_PER_SPAN];

    /* Native-offset -> guest-PC attribution for precise exceptions. */
    LatrTbInsnMap insn_maps[LATR_TB_MAP_SLOTS];
    uint32_t insn_map_generation;
    LatrTbInsnMap *active_insn_map;
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
                          uint8_t site, uint32_t retired_before,
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
                            const uint64_t host_gprs[32], CPUArchState *env);

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
void latr_regcache_flush(void);
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
