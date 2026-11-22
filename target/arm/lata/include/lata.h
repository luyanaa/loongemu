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

#include "tcg/tcg.h"
//#include "env.h"

#define B_STUB_SIZE 4

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

    /*
     * Guest V registers the current TB actually loaded into their resident
     * LASX register.  A helper call writes those registers back to env; doing
     * that for untouched registers would overwrite env with stale contents
     * (the SVE lowering keeps vector state authoritative in env).
     */
    uint32_t vreg_resident_touched;
    /* Resident registers written in this TB; discharged by the exit code. */
    uint32_t vreg_resident_dirty;

    /* TODO : support static translation */
    uint8 curr_ir1_skipped_eflags; /* these eflag calculation can be skipped */
                                   /* (because of flag pattern, etc) */
} TRANSLATION_DATA;


typedef struct ENV {
    void *cpu_state;            /* from QEMU,CPUArchState */
    TRANSLATION_DATA *tr_data;  /* from LATR */
} ENV;


void lata_tr_data_init(CPUArchState *env);
void tr_init(void*);
void tr_fini(void);

extern const int arm_la_map[];
extern const int arm_la_fmap[];
extern __thread TRANSLATION_DATA *tr_data;
extern __thread ENV *lsenv;
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
/* TBs that reached tr_fini() without discharging their dirty registers. */
uint32_t lata_vreg_exit_violations(void);
/* Number of per-TB vector stores emitted (proves the per-TB exit ran). */
uint32_t lata_vreg_tb_publishes(void);
/* Vector loads emitted on first use instead of preloaded by the entry stub. */
uint32_t lata_vreg_lazy_loads(void);
uint32_t lata_vreg_temp_loads(void);
/* Temporary-pool allocations that could not be satisfied (must stay 0). */
uint32_t ra_ftemp_alloc_failures(void);

/*
 * Guest V-register residency.  A resident register holds the live value; a
 * demoted one is env-backed.  Spilling picks a register the current TB has not
 * touched, writes it back and hands its LASX register to the temporary pool.
 */
bool lata_vreg_resident(int index);
void lata_vreg_init_residency(void);
int lata_vreg_spill_cold(void);
uint32_t lata_vreg_spills(void);
/* Number of guest V registers currently resident in host registers. */
uint32_t lata_vreg_resident_count(void);
/* Test/debug knob: shrink the temporary base pool to force spilling. */
void lata_ftemp_set_base_pool(int pool);
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

IR2_OPND alloc_gpr_src(int i);
IR2_OPND alloc_gpr_src_sp(int i);
IR2_OPND alloc_gpr_dst(int i);
IR2_OPND alloc_gpr_dst_sp(int i);
void store_gpr_dst(int i, IR2_OPND opnd);
void store_gpr_zero(int i);
void free_alloc_gpr(IR2_OPND opnd);

IR2_OPND alloc_fpr_src(int i);
IR2_OPND alloc_fpr_dst(int i);
void store_fpr_dst(int i, IR2_OPND opnd);
void free_alloc_fpr(IR2_OPND opnd);

typedef struct LataIr2Snapshot {
    int ir2_inst_num_current;
    int real_ir2_inst_num;
    int first_ir2_id;
    int last_ir2_id;
    int last_ir2_next;
    int label_num;
    uint32_t all_temp_status;
    uint32_t vreg_resident_touched;
    uint32_t vreg_resident_dirty;
    uint32_t vreg_tb_resident;
    uint32_t w_write_flag;
    struct {
        bool valid;
        bool dirty;
        int offset;
    } zcache_slots[2];
    unsigned zcache_victim;
    LataVcacheSnapshot vcache_snap;
} LataIr2Snapshot;

void lata_ir2_snapshot_take(LataIr2Snapshot *snap);
void lata_ir2_snapshot_rollback(const LataIr2Snapshot *snap);

#endif
