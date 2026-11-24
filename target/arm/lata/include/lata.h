#ifndef _LATA_H_
#define _LATA_H_

#include <stdint.h>

#include "qemu/osdep.h"
#include "mem.h"
#include "ir2.h"
#include "la-append.h"
#include "reg-alloc.h"
#include "macro-inst.h"
#include "lata-vcache.h"
#include "lata-snapshot.h"
#include "lata-fpsr.h"

#include "tcg/tcg.h"
//#include "env.h"

#ifndef LATA_TLS
#if defined(LATA_ENABLE_TLS) || (!defined(CONFIG_LATA_STANDALONE) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L)
#define LATA_TLS _Thread_local
#else
#define LATA_TLS
#endif
#endif

typedef struct LataRegCacheEntry {
    int8_t temp;
    uint8_t valid;
    uint8_t dirty;
    uint64_t age;
} LataRegCacheEntry;

typedef struct LataRegCache {
    LataRegCacheEntry gpr[32];
    uint8_t itemp_borrows[IR2_ITEMP_MAX];
    uint64_t clock;
} LataRegCache;

#include "lata-recovery.h"

typedef struct TRANSLATION_DATA {

    void *curr_tb; /* from QEMU */

    /* ir1 */
    // IR1_INST *curr_ir1_inst;
    // int curr_ir1_count;
    /* uint8       ir1_dump_threshold[MAX_IR1_NUM_PER_TB]; */

    /*  w_write_flag indicates whether a 32-bit register was written 
        before the current instruction, it should initialize to zero
        Each bit represents one general registers(w0-w31)
    */
    uint32_t w_write_flag;

    /* ir2 */
    IR2_INST *ir2_inst_array;
    int ir2_inst_num_max;
    int ir2_inst_num_current;
    int real_ir2_inst_num;

    /* the list of ir2 */
    IR2_INST *first_ir2;
    IR2_INST *last_ir2;

    /* label number */
    int label_num;

    /* temp reg */
    union {
        uint32 all_temp_status;
        struct {
            uint16 itemp_status;
            uint16 ftemp_status;
        };
    };
    LataRegCache regcache;

    /*
     * Guest V registers the current TB actually loaded into their resident
     * LASX register.  A helper call writes those registers back to env; doing
     * that for untouched registers would overwrite env with stale contents
     * (the SVE lowering keeps vector state authoritative in env).
     */
    uint32_t vreg_resident_touched;
    /* Resident registers written in this TB; discharged by the exit code. */
    uint32_t vreg_resident_dirty;
    /* Guest V registers written by A64 NEON/FP insns requiring Z[VL-1:128]=0. */
    uint32_t vreg_v_written_mask;
    /* Explicit temporary FPR allocation exhaustion flag (checked on rollback). */
    bool ftemp_alloc_failed;

    /* TODO : support static translation */
    uint8 curr_ir1_skipped_eflags; /* these eflag calculation can be skipped */
                                   /* (because of flag pattern, etc) */

    /* C2: per-insn recovery staging, filled by tr_ir2_generate and committed
     * into the map by tr_ir2_assemble (LATR plan C). Each translated guest
     * insn gets a label id so its native span bounds can be resolved at
     * assemble time. */
    int      tb_insn_count;
    uint64_t tb_insn_pc[LATA_TB_MAP_SPANS];
    int      tb_insn_label[LATA_TB_MAP_SPANS + 1];
    uint8_t  tb_insn_site[LATA_TB_MAP_SPANS];
    uint8_t  tb_insn_bind_cnt[LATA_TB_MAP_SPANS];
    LataRecoveryBinding tb_insn_binds[LATA_TB_MAP_SPANS]
                                     [LATA_RECOVERY_MAX_BINDS_PER_SPAN];

    /* Native-offset -> guest-PC attribution for precise exceptions. Maps are
     * owned by the TB code range they describe (the TCG code-buffer block),
     * NOT an LRU pool: a map lives in insn_map_list until the buffer block is
     * reused or the context is destroyed, so a still-live TB always resolves. */
    LataTbInsnMap *insn_map_list;
    uint32_t insn_map_count;
    uint32_t insn_map_generation;
    LataTbInsnMap *active_insn_map;
} TRANSLATION_DATA;


typedef struct ENV {
    void *cpu_state;            /* from QEMU,CPUArchState */
    TRANSLATION_DATA *tr_data;  /* from LATR */
    /* Per-context dirty-flush hook for ir2_stream_rollback(). */
    void (*ir2_dirty_flush_fn)(void *opaque);
    void *ir2_dirty_flush_opaque;
} ENV;

struct LataGuestMemOps;
struct TUControl;

#define LATA_VCACHE_ENTRIES 2

typedef struct LataContext {
    TRANSLATION_DATA tr_data;
    ENV env;
    CPUArchState *cpu_state;
    LataVcacheState vcache;
    struct {
        bool valid;
        bool dirty;
        int offset;
    } zcache[LATA_VCACHE_ENTRIES];
    unsigned zcache_victim;
    uint32_t vreg_resident_mask;
    uint32_t vreg_spill_count;
    uint32_t vreg_tb_resident;
    uint32_t vreg_publish_count;
    uint32_t vreg_exit_violations;
    uint32_t vreg_lazy_loads;
    uint32_t vreg_temp_loads;
    int ftemp_base;
    int ftemp_pool_entries;
    int reg_ftemp_map[32];
    int reg_ftemp_reverse_map[32];
    uint32_t ftemp_alloc_failures;
    uint32_t ftemp_fallbacks;
    bool sve_const_pred_opt_enabled;
    size_t sve_const_pred_folds;
    int ir2_dump_ir1_id;
    const struct LataGuestMemOps *mem_ops;
    void *ir1_list;
    void *db_rel;
    /* SVE helper descriptor chunks; lifetime follows the owning TB, not the
     * context. See lata-sve-lasx-lower.h. */
    void *sve_helper_list;
    void *sve_helper_active;
    const void *sve_helper_owner;
    uint64_t sve_helper_pc;
    uint32_t sve_helper_descs;
    uint32_t sve_helper_chunks;
    /*
     * Code-cache position recorded when the live chunks were allocated.  A
     * flush rewinds the cache, so seeing an earlier position later proves that
     * no code from the old cache can run and every descriptor can be dropped.
     */
    uintptr_t sve_helper_codegen;
    /* Code-cache generation the live chunks belong to. */
    uint64_t sve_helper_codegen_gen;
    struct TUControl *tu_data;
    void *tu_search_offsets;
    bool initialized;
} LataContext;

void lata_context_init(LataContext *ctx, CPUArchState *env);
void lata_context_destroy(LataContext *ctx);
LataContext *lata_context_new(CPUArchState *env);
void lata_context_delete(LataContext *ctx);
void lata_context_bind(LataContext *ctx);
LataContext *lata_context_current(void);

/* C2 precise recovery map/apply API lives in lata-recovery.h (included
 * above). These two helpers are lata-internal, used by the translate loop. */
uint8_t lata_recovery_site_of(int insn_type);
uint8_t lata_regcache_snapshot_gpr_binds(LataRecoveryBinding *out);

/*
 * LATA keeps AArch64 NZCV in the LoongArch binary-translation flag register, so
 * a host without LBT cannot run translated code at all: this records what the
 * host reports, and the ISA harness prints it so a failing gate says why.
 */
extern bool lata_have_lbt;
void lata_force_no_lbt(void);

void lata_tr_data_init(CPUArchState *env);
void tr_init(void*);
void tr_fini(void);

extern const int arm_la_map[];
extern const int arm_la_fmap[];
extern LATA_TLS TRANSLATION_DATA *tr_data;
extern LATA_TLS ENV *lsenv;
extern bool lata_dump;

extern uint64_t context_switch_bt_to_native;
extern uint64_t context_switch_native_to_bt_ret_0;
extern uint64_t context_switch_native_to_bt;

int lata_gen_prologue(TCGContext *tcg_ctx);
int lata_gen_epilogue(TCGContext *tcg_ctx);
void lata_fast_jmp_cache_init(CPUArchState *env, uint64_t start_code, uint64_t end_code);
void lata_fast_jmp_cache_add(CPUArchState *env, uint64_t guest_pc, uint64_t host_pc);

int tr_ir2_assemble(const void *code_start_addr);
/* Debug aid: first instruction of the current TB's IR2 list. */
struct IR2_INST *lata_first_ir2(void);

/*
 * Emit any pending vector-cache write-back.  Every path that leaves the TB
 * (native return and direct linked-TB jump) must call this first; it is a
 * no-op while the cache is read-only or disabled.
 */
void lata_zcache_flush_for_exit(void);

/* Emit per-TB stores for resident registers this TB wrote, then clear them. */
void lata_emit_dirty_vreg_stores(void);
/* Record a V-register write (clears Z[VL-1:128] on SVE VLs > 128). */
void lata_note_vreg_v_write(int rd);
/* Flush dirty V-registers (with Z upper-half zeroing) before SVE ops read Z. */
void lata_flush_vregs_for_sve(void);
/* Flush/invalidate any zcache chunk covering Z register `reg`. */
void lata_zcache_flush_reg(int reg);
void lata_zcache_invalidate_reg(int reg);
/* TBs that reached tr_fini() without discharging their dirty registers. */
uint32_t lata_vreg_exit_violations(void);
/* Number of per-TB vector stores emitted (proves the per-TB exit ran). */
uint32_t lata_vreg_tb_publishes(void);
/* Vector loads emitted on first use instead of preloaded by the entry stub. */
uint32_t lata_vreg_lazy_loads(void);
uint32_t lata_vreg_temp_loads(void);
/* Temporary-pool allocations that could not be satisfied (must stay 0). */
uint32_t ra_ftemp_alloc_failures(void);
bool ra_ftemp_alloc_failed(void);
void ra_ftemp_clear_failed(void);
void ra_ftemp_note_fallback(void);
uint32_t ra_ftemp_fallbacks(void);

/*
 * Guest V-register residency.  A resident register holds the live value; a
 * demoted one is env-backed.  Spilling picks a register the current TB has not
 * touched, writes it back and hands its LASX register to the temporary pool.
 */
bool lata_vreg_resident(int index);
void lata_vreg_init_residency(void);
void lata_vreg_clear_residency_for_test(void);
int lata_vreg_spill_cold(void);
uint32_t lata_vreg_spills(void);
/* Number of guest V registers currently resident in host registers. */
uint32_t lata_vreg_resident_count(void);
/* Test/debug knob: shrink the temporary base pool to force spilling. */
void lata_ftemp_set_base_pool(int pool);

/*
 * Code-cache generation.  Every cache flush (explicit or detected by the
 * allocator rewinding) bumps it; helper descriptors carry the generation they
 * were created under, so a stale group is recognised even when the allocator
 * has already grown back past the old position.
 */
extern uint64_t lata_code_cache_generation;
uint64_t lata_code_cache_bump_generation(void);
uint64_t lata_code_cache_current_generation(void);

/* Guest FPSR helpers live in lata-fpsr.h (included above). */
/* True when the physical register is currently handed out by the temp pool. */
bool ra_ftemp_phys_in_pool(int phys);

/*
 * Register-file partition invariant: a physical LASX register is owned by at
 * most one of {resident guest register, temporary pool, cache window}.  The
 * native<->BT switch code is emitted once at startup, so the resident set must
 * be fixed for the whole session; the partition is what makes cross-TB
 * residency sound.  Returns the number of violations (0 is required).
 */
int lata_vreg_partition_violations(void);
void label_dispose(void);

void lata_gen_call_helper_prologue(TCGContext *tcg_ctx);
void lata_gen_call_helper_epilogue(TCGContext *tcg_ctx);

/* Environment vector accesses materialize offsets outside the signed SI12 range. */
void lata_env_vld(IR2_OPND vector, int offset);
void lata_env_vst(IR2_OPND vector, int offset);
void lata_env_xvld(IR2_OPND vector, int offset);
void lata_env_xvst(IR2_OPND vector, int offset);
void lata_env_ld_wu(IR2_OPND scalar, int offset);
void lata_env_ld_du(IR2_OPND scalar, int offset);
void lata_env_st_b(IR2_OPND scalar, int offset);
void lata_env_st_wu(IR2_OPND scalar, int offset);

IR2_OPND alloc_gpr_src(int i);
IR2_OPND alloc_gpr_src_sp(int i);
IR2_OPND alloc_gpr_dst(int i);
IR2_OPND alloc_gpr_dst_sp(int i);
void store_gpr_dst(int i, IR2_OPND opnd);
void store_gpr_zero(int i);

/* TB-local scalar GPR cache hooks. */
void lata_regcache_reset(void);
void lata_regcache_flush(void);
bool lata_regcache_evict_itemp(void);
bool lata_regcache_free_itemp(int temp);

/*
 * Keep cache state coherent on both control-flow edges: a branch may bypass
 * the fallthrough flush immediately before its target label.
 */
static inline IR2_INST *lata_regcache_label(IR2_OPND label)
{
    lata_regcache_flush();
    return (la_label)(label);
}

static inline IR2_INST *lata_regcache_jirl(IR2_OPND rd, IR2_OPND rj,
                                            int offset)
{
    lata_regcache_flush();
    return (la_jirl)(rd, rj, offset);
}

static inline IR2_INST *lata_regcache_b(IR2_OPND label)
{
    lata_regcache_flush();
    return (la_b)(label);
}

static inline IR2_INST *lata_regcache_beqz(IR2_OPND reg, IR2_OPND label)
{
    lata_regcache_flush();
    return (la_beqz)(reg, label);
}

static inline IR2_INST *lata_regcache_bnez(IR2_OPND reg, IR2_OPND label)
{
    lata_regcache_flush();
    return (la_bnez)(reg, label);
}

static inline IR2_INST *lata_regcache_beq(IR2_OPND lhs, IR2_OPND rhs,
                                           IR2_OPND label)
{
    lata_regcache_flush();
    return (la_beq)(lhs, rhs, label);
}

static inline IR2_INST *lata_regcache_bne(IR2_OPND lhs, IR2_OPND rhs,
                                           IR2_OPND label)
{
    lata_regcache_flush();
    return (la_bne)(lhs, rhs, label);
}

static inline IR2_INST *lata_regcache_blt(IR2_OPND lhs, IR2_OPND rhs,
                                           IR2_OPND label)
{
    lata_regcache_flush();
    return (la_blt)(lhs, rhs, label);
}

static inline IR2_INST *lata_regcache_bge(IR2_OPND lhs, IR2_OPND rhs,
                                           IR2_OPND label)
{
    lata_regcache_flush();
    return (la_bge)(lhs, rhs, label);
}

static inline IR2_INST *lata_regcache_bltu(IR2_OPND lhs, IR2_OPND rhs,
                                            IR2_OPND label)
{
    lata_regcache_flush();
    return (la_bltu)(lhs, rhs, label);
}

static inline IR2_INST *lata_regcache_bgeu(IR2_OPND lhs, IR2_OPND rhs,
                                            IR2_OPND label)
{
    lata_regcache_flush();
    return (la_bgeu)(lhs, rhs, label);
}

static inline IR2_INST *lata_regcache_bcnez(IR2_OPND cond, IR2_OPND label)
{
    lata_regcache_flush();
    return (la_bcnez)(cond, label);
}

void free_alloc_gpr(IR2_OPND opnd);

IR2_OPND alloc_fpr_src(int i);
IR2_OPND alloc_fpr_dst(int i);
void store_fpr_dst(int i, IR2_OPND opnd);
void free_alloc_fpr(IR2_OPND opnd);

void lata_ftemp_pool_snapshot(LataContext *ctx, LataIr2Snapshot *snap);
void lata_ftemp_pool_rollback(LataContext *ctx, const LataIr2Snapshot *snap);
void lata_ir2_snapshot_rollback(const LataIr2Snapshot *snap);

#endif
