#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "lata.h"
#include "lata-mem.h"
#include "lata-vcache.h"
#include "target/arm/tcg/translate.h"
#include "target/arm/lata/translator/sve-lasx-lower.h"

#include "exec/exec-all.h"
#include "qemu/log.h"
#include "env.h"
#ifdef CONFIG_LATA_TU
#include "tu.h"
#endif
//sp
//tp
//zero---unused
//a0-----goto_tb
//s8-----env 
//t0-t5--itemp
//FLAG---pstate
const int arm_la_map[] = {
    [armv8_x0] = la_ra,
    [armv8_x1] = la_a1,
    [armv8_x2] = la_a2,
    [armv8_x3] = la_a3,
    [armv8_x4] = la_a4,
    [armv8_x5] = la_a5,
    [armv8_x6] = la_a6,
    [armv8_x7] = la_a7,
    [armv8_x8] = la_fp,
    [armv8_x9] = la_t8,
    [armv8_x10] = la_t6,
    [armv8_x11] = -1,
    [armv8_x12] = -1,
    [armv8_x13] = -1,
    [armv8_x14] = -1,
    [armv8_x15] = -1,
    [armv8_x16] = -1,
    [armv8_x17] = -1,
    [armv8_x18] = -1,
    [armv8_x19] = la_s6,
    [armv8_x20] = la_s0,
    [armv8_x21] = la_s1,
    [armv8_x22] = la_s2,
    [armv8_x23] = la_s3,
    [armv8_x24] = la_s4,
    [armv8_x25] = -1,
    [armv8_x26] = -1,
    [armv8_x27] = -1,
    [armv8_x28] = la_s7,
    [armv8_x29] = la_t7,
    [armv8_x30] = la_s5,
    [armv8_xzr] = la_r21
};

const int arm_la_reverse_map[] = {
    [la_zero] = -1,
    [la_ra]  = armv8_x0,
    [la_sp]  = -1,
    [la_t7]  = armv8_x29,
    [la_s0]  = armv8_x20,
    [la_s1]  = armv8_x21,
    [la_s8]  = -1,
    [la_t8]  = armv8_x9,
    [la_a2]  = armv8_x2,
    [la_a3]  = armv8_x3,
    [la_a4]  = armv8_x4,
    [la_a5]  = armv8_x5,
    [la_a6]  = armv8_x6,
    [la_a7]  = armv8_x7,
    [la_s2]  = armv8_x22,
    [la_s3]  = armv8_x23,
    [la_s4]  = armv8_x24,
    [la_s5]  = armv8_x30,
    [la_s6]  = armv8_x19,
    [la_s7]  = armv8_x28,
    [la_t4]  = -1,
    [la_t5]  = -1,
    [la_t6]  = armv8_x10,
    [la_r21] = armv8_xzr,
    [la_tp] = -1,
    [la_a0] = -1,
    [la_a1] = armv8_x1,
    [la_t0] = -1,
    [la_t1] = -1,
    [la_t2] = -1,
    [la_t3] = -1,
    [la_fp] = armv8_x8 
};

const int arm_la_fmap[] = {
    [armv8_v0] = 0,
    [armv8_v1] = 1,
    [armv8_v2] = 2,
    [armv8_v3] = 3,
    [armv8_v4] = 4,
    [armv8_v5] = 5,
    [armv8_v6] = 6,
    [armv8_v7] = 7,
    [armv8_v8] = 8,
    [armv8_v9] = -1,
    [armv8_v10] = -1,
    [armv8_v11] = -1,
    [armv8_v12] = -1,
    [armv8_v13] = -1,
    [armv8_v14] = -1,
    [armv8_v15] = -1,
    [armv8_v16] = 16,
    [armv8_v17] = 17,
    [armv8_v18] = 18,
    [armv8_v19] = 19,
    [armv8_v20] = 20,
    [armv8_v21] = 21,
    [armv8_v22] = 22,
    [armv8_v23] = 23,
    [armv8_v24] = 24,
    [armv8_v25] = 25,
    [armv8_v26] = 26,
    [armv8_v27] = 27,
    [armv8_v28] = 28,
    [armv8_v29] = 29,
    [armv8_v30] = 30,
    [armv8_v31] = 31
};

const int arm_la_reverse_fmap[] = {
    [0] = 0,
    [1] = 1,
    [2] = 2,
    [3] = 3,
    [4] = 4,
    [5] = 5,
    [6] = 6,
    [7] = 7,
    [8] = 8,
    [9] = -1,
    [10] = -1,
    [11] = -1,
    [12] = -1,
    [13] = -1,
    [14] = -1,
    [la_fsmask] = -1,
    [16] = 16,
    [17] = 17,
    [18] = 18,
    [19] = 19,
    [20] = 20,
    [21] = 21,
    [22] = 22,
    [23] = 23,
    [24] = 24,
    [25] = 25,
    [26] = 26,
    [27] = 27,
    [28] = 28,
    [29] = 29,
    [30] = 30,
    [31] = 31

};

void lata(void);
__attribute__((constructor))
void lata(void){
    // printf("lata enabled\n");
}

LATA_TLS ENV *lsenv;
static LataContext default_ctx;
static LATA_TLS LataContext *bound_ctx;
uint64_t *ibtc_cache;
uint64_t *fam_cache;

LATA_TLS TRANSLATION_DATA *tr_data;

bool lata_dump = 0;
uint64_t context_switch_bt_to_native;

/*
 * Set by a TB exit that already published the resident vector registers.
 * The shared exit stub skips its own publish when a translated TB has done it,
 * and still publishes for hand-built / untranslated paths.
 */
uint32_t lata_vreg_published;
uint64_t context_switch_native_to_bt_ret_0;
uint64_t context_switch_native_to_bt;

static void global_register_init(void)
{
    env_ir2_opnd = INIT_RA(IR2_OPND_GPR, reg_statics_map[S_ENV]);

    /* All LA reg */
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
    fsmask_ir2_opnd = INIT_RA(IR2_OPND_FPR, la_fsmask); //mask for scalar
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

LataContext *lata_context_current(void)
{
    if (bound_ctx != NULL) {
        return bound_ctx;
    }
    return &default_ctx;
}

void lata_context_init(LataContext *ctx, CPUArchState *env)
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->cpu_state = env;
    ctx->env.cpu_state = env;
    ctx->env.tr_data = &ctx->tr_data;
    lata_vcache_state_init(&ctx->vcache, true);
    lata_ftemp_init_context(ctx);
    ctx->sve_const_pred_opt_enabled = true;
    ctx->mem_ops = &lata_default_guest_mem_ops;
    ctx->initialized = true;

    LataContext *prev = bound_ctx;
    bound_ctx = ctx;
    lata_vreg_init_residency();
    bound_ctx = prev;
}

void lata_context_destroy(LataContext *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->tr_data.ir2_inst_array != NULL) {
        mm_free(ctx->tr_data.ir2_inst_array);
        ctx->tr_data.ir2_inst_array = NULL;
    }
    ctx->tr_data.ir2_inst_num_max = 0;
    if (ctx->ir1_list != NULL) {
        mm_free(ctx->ir1_list);
        ctx->ir1_list = NULL;
    }
    if (ctx->db_rel != NULL) {
        mm_free(ctx->db_rel);
        ctx->db_rel = NULL;
    }
    if (ctx->sve_helper_list != NULL) {
        lata_sve_helper_list_free(ctx->sve_helper_list);
        ctx->sve_helper_list = NULL;
    }
    if (bound_ctx == ctx) {
        bound_ctx = NULL;
        lata_vcache_bind_state(NULL);
    }
    ctx->initialized = false;
}

LataContext *lata_context_new(CPUArchState *env)
{
    LataContext *ctx = (LataContext *)mm_calloc(1, sizeof(LataContext));
    lata_context_init(ctx, env);
    return ctx;
}

void lata_context_delete(LataContext *ctx)
{
    if (ctx == NULL) {
        return;
    }
    lata_context_destroy(ctx);
    mm_free(ctx);
}

void lata_context_bind(LataContext *ctx)
{
    LataContext *target = ctx != NULL ? ctx : &default_ctx;
    if (!target->initialized) {
        lata_context_init(target, target->cpu_state);
    }
    bound_ctx = target;
    lsenv = &target->env;
    tr_data = &target->tr_data;
    lsenv->tr_data = tr_data;
    lsenv->cpu_state = target->cpu_state;
    lata_vcache_bind_state(&target->vcache);
#ifdef CONFIG_LATA_TU
    tu_control_init();
#endif
    global_register_init();
}

void lata_tr_data_init(CPUArchState *env)
{
    LataContext *ctx = lata_context_current();
    if (!ctx->initialized) {
        bool preserve_vcache = lata_vcache_enabled();
        lata_context_init(ctx, env);
        ctx->vcache.enabled = preserve_vcache;
    }
    ctx->cpu_state = env;
    ctx->env.cpu_state = env;
    lata_context_bind(ctx);
}

/*
 * Single-chunk read cache: home register f15 (guest V15 is env-backed and f15
 * is outside the f9-f14 temporary pool).  Stores always go straight to env and
 * invalidate the chunk, so a TB can never end with borrowed state.
 */
#define LATA_VCACHE_HOME_REG 15

static void emit_env_xvector_memory(IR2_OPND vector, int offset, bool store);

#define LATA_VCACHE_ENTRIES 2
/* Homes f14 and f15: f15 is unowned by the guest map, and f14 is taken out of
 * the temporary pool while the window is enabled (ra_ftemp_pool_size()). */
#define LATA_VCACHE_HOME_BASE 14

static IR2_OPND vcache_home(unsigned slot)
{
    return ir2_opnd_new(IR2_OPND_FPR, LATA_VCACHE_HOME_BASE + (int)slot);
}

static void vcache_flush_slot(unsigned slot)
{
    LataContext *ctx = lata_context_current();
    if (!ctx->zcache[slot].valid || !ctx->zcache[slot].dirty) {
        ctx->zcache[slot].dirty = false;
        return;
    }
    lata_vcache_note_env_vector_write();
    lata_vcache_note_z_write();
    emit_env_xvector_memory(vcache_home(slot), ctx->zcache[slot].offset, true);
    ctx->zcache[slot].dirty = false;
    lata_vcache_clear_dirty(slot);
}

void lata_zcache_flush(void)
{
    LataContext *ctx = lata_context_current();
    for (unsigned slot = 0; slot < LATA_VCACHE_ENTRIES; ++slot) {
        vcache_flush_slot(slot);
        ctx->zcache[slot].valid = false;
    }
}

void lata_zcache_flush_reg(int reg)
{
    LataContext *ctx = lata_context_current();
    int low = env_offset_zreg(reg);
    int high;

    if (low < 0) {
        return;
    }
    high = low + (int)ARM_SVE_MAX_VL_BYTES;
    for (unsigned slot = 0; slot < LATA_VCACHE_ENTRIES; ++slot) {
        if (ctx->zcache[slot].valid && ctx->zcache[slot].offset >= low &&
            ctx->zcache[slot].offset < high) {
            vcache_flush_slot(slot);
        }
    }
}

void lata_zcache_invalidate_reg(int reg)
{
    LataContext *ctx = lata_context_current();
    int low = env_offset_zreg(reg);
    int high;

    if (low < 0) {
        return;
    }
    high = low + (int)ARM_SVE_MAX_VL_BYTES;
    for (unsigned slot = 0; slot < LATA_VCACHE_ENTRIES; ++slot) {
        if (ctx->zcache[slot].valid && ctx->zcache[slot].offset >= low &&
            ctx->zcache[slot].offset < high) {
            ctx->zcache[slot].valid = false;
            ctx->zcache[slot].dirty = false;
            lata_vcache_clear_dirty(slot);
        }
    }
}

static void vcache_invalidate(void)
{
    LataContext *ctx = lata_context_current();
    for (unsigned slot = 0; slot < LATA_VCACHE_ENTRIES; ++slot) {
        ctx->zcache[slot].valid = false;
        ctx->zcache[slot].dirty = false;
    }
    /* Deterministic victim order per TB: otherwise a stale round-robin index
     * from a previous TB evicts a dirty chunk and defeats dead-store removal. */
    ctx->zcache_victim = 0;
}

/* Write back every dirty chunk but keep the clean ones in place. */
static void vcache_flush_dirty(void)
{
    for (unsigned slot = 0; slot < LATA_VCACHE_ENTRIES; ++slot) {
        vcache_flush_slot(slot);
    }
}

/* Only Z chunks are window candidates: the mask scratch churns every
 * instruction and the expanded-mask cache already handles its reuse. */
static bool vcache_offset_is_z(int offset)
{
    int low = env_offset_zreg(0);
    int high = env_offset_zreg((int)ARM_SVE_NUM_ZREGS - 1) + ARM_SVE_MAX_VL_BYTES;

    if (low < 0) {
        return false;
    }
    return offset >= low && offset < high;
}

static int vcache_lookup(int offset)
{
    LataContext *ctx = lata_context_current();
    for (unsigned slot = 0; slot < LATA_VCACHE_ENTRIES; ++slot) {
        if (ctx->zcache[slot].valid && ctx->zcache[slot].offset == offset) {
            return (int)slot;
        }
    }
    return -1;
}

static unsigned vcache_choose_slot(void)
{
    LataContext *ctx = lata_context_current();
    for (unsigned slot = 0; slot < LATA_VCACHE_ENTRIES; ++slot) {
        if (!ctx->zcache[slot].valid) {
            return slot;
        }
    }
    /* Prefer evicting a clean read-only slot over a dirty destination slot. */
    for (unsigned slot = 0; slot < LATA_VCACHE_ENTRIES; ++slot) {
        if (!ctx->zcache[slot].dirty) {
            return slot;
        }
    }
    ctx->zcache_victim = (ctx->zcache_victim + 1u) % LATA_VCACHE_ENTRIES;
    return ctx->zcache_victim;
}

void lata_zcache_flush_for_exit(void)
{
    /*
     * Called from every TB-exit emitter.  Today the cache is read-only, so no
     * instruction is emitted and the translation stays byte-identical; the hook
     * exists so a write-back window has a single, already-wired flush point.
     */
    lata_vcache_note_exit_hook();
    lata_zcache_flush();
}

void tr_init(void* tb)
{
    TRANSLATION_DATA *t = lsenv->tr_data;

    lata_vcache_begin_tb();
    vcache_invalidate();
    //int i = 0;

    t->curr_tb = tb;
    /* register allocation init */
    ra_free_all();

    /* reset ir2 array */
    if (t->ir2_inst_array == NULL) {
        lsassert(t->ir2_inst_num_max == 0);
        t->ir2_inst_array = (IR2_INST *)mm_calloc(400, sizeof(IR2_INST));
        t->ir2_inst_num_max = 400;
    }
    t->ir2_inst_num_current = 0;
    //uint64 k = &(t->ir2_inst_num_current);
    t->real_ir2_inst_num = 0;

    /* reset ir2 first/last/num */
    t->first_ir2 = NULL;
    t->last_ir2 = NULL;

    /* label number */
    t->label_num = 0;

    t->vreg_resident_touched = 0;
    t->vreg_resident_dirty = 0;
    t->vreg_v_written_mask = 0;
    t->ftemp_alloc_failed = false;
    lata_vreg_init_residency();
    /* Nothing is in a host register yet: the entry stub no longer preloads. */
    lata_context_current()->vreg_tb_resident = 0;
}

struct IR2_INST *lata_first_ir2(void)
{
    return lsenv->tr_data->first_ir2;
}

void tr_fini(void)
{
    TRANSLATION_DATA *t = lsenv->tr_data;
    LataContext *ctx = lata_context_current();

    /* I0: no borrowed vector register may survive the TB. */
    lata_vcache_end_tb();
    /* set current tb and ir1 */

    /* reset ir2 array */
    t->ir2_inst_num_current = 0;
    t->real_ir2_inst_num = 0;

    /* reset ir2 first/last/num */
    t->first_ir2 = NULL;
    t->last_ir2 = NULL;

    /* label number */
    t->label_num = 0;
    t->vreg_v_written_mask = 0;
    t->ftemp_alloc_failed = false;

    /* I0 for registers: every dirty resident register must be published. */
    if (t->vreg_resident_dirty != 0) {
        ctx->vreg_exit_violations++;
        t->vreg_resident_dirty = 0;
    }
}

#define EXTRA_SPACE             40
#define REG_LEN                 8
#define S0_EXTRA_SPACE          EXTRA_SPACE
#define S1_EXTRA_SPACE          (S0_EXTRA_SPACE + REG_LEN)
#define S2_EXTRA_SPACE          (S1_EXTRA_SPACE + REG_LEN)
#define S3_EXTRA_SPACE          (S2_EXTRA_SPACE + REG_LEN)
#define S4_EXTRA_SPACE          (S3_EXTRA_SPACE + REG_LEN)
#define S5_EXTRA_SPACE          (S4_EXTRA_SPACE + REG_LEN)
#define S6_EXTRA_SPACE          (S5_EXTRA_SPACE + REG_LEN)
#define S7_EXTRA_SPACE          (S6_EXTRA_SPACE + REG_LEN)
#define S8_EXTRA_SPACE          (S7_EXTRA_SPACE + REG_LEN)
#define FP_EXTRA_SPACE          (S8_EXTRA_SPACE + REG_LEN)
#define RA_EXTRA_SPACE          (FP_EXTRA_SPACE + REG_LEN)
#define FCSR_EXTRA_SPACE        (RA_EXTRA_SPACE + REG_LEN)



static void emit_env_vector_memory(IR2_OPND vector, int offset, bool store)
{
    LataVectorAddressPlan plan = env_vector_address_plan(offset);
    IR2_OPND base = env_ir2_opnd;
    IR2_OPND address;

    if (plan.materialize_address) {
        address = ra_alloc_itemp();
        li_d(address, (int64_t)offset);
        la_add_d(address, env_ir2_opnd, address);
        base = address;
    }
    if (store) {
        la_vst(vector, base, plan.displacement);
    } else {
        la_vld(vector, base, plan.displacement);
    }
    if (plan.materialize_address) {
        ra_free_temp(address);
    }
}

void lata_env_vld(IR2_OPND vector, int offset)
{
    if (lata_vcache_enabled() && vcache_offset_is_z(offset)) {
        vcache_flush_dirty();
    }
    lata_vcache_note_env_vector_read();
    emit_env_vector_memory(vector, offset, false);
}

void lata_env_vst(IR2_OPND vector, int offset)
{
    if (lata_vcache_enabled() && vcache_offset_is_z(offset)) {
        vcache_flush_dirty();
    }
    lata_vcache_note_env_vector_write();
    emit_env_vector_memory(vector, offset, true);
}

static void emit_env_xvector_memory(IR2_OPND vector, int offset, bool store)
{
    LataVectorAddressPlan plan = env_vector_address_plan(offset);
    IR2_OPND base = env_ir2_opnd;
    IR2_OPND address;

    if (plan.materialize_address) {
        address = ra_alloc_itemp();
        li_d(address, (int64_t)offset);
        la_add_d(address, env_ir2_opnd, address);
        base = address;
    }
    if (store) {
        la_xvst(vector, base, plan.displacement);
    } else {
        la_xvld(vector, base, plan.displacement);
    }
    if (plan.materialize_address) {
        ra_free_temp(address);
    }
}

void lata_env_xvld(IR2_OPND vector, int offset)
{
    LataContext *ctx = lata_context_current();
    if (lata_vcache_enabled() && latx_cpu_has_lasx() &&
        vcache_offset_is_z(offset)) {
        int slot = vcache_lookup(offset);

        if (slot >= 0) {
            lata_vcache_note_hit();
            la_xvor_v(vector, vcache_home((unsigned)slot),
                      vcache_home((unsigned)slot));
            return;
        }
        lata_vcache_note_miss();
        slot = (int)vcache_choose_slot();
        vcache_flush_slot((unsigned)slot);
        lata_vcache_note_env_vector_read();
        emit_env_xvector_memory(vector, offset, false);
        ctx->zcache[slot].valid = true;
        ctx->zcache[slot].dirty = false;
        ctx->zcache[slot].offset = offset;
        la_xvor_v(vcache_home((unsigned)slot), vector, vector);
        return;
    }
    lata_vcache_note_env_vector_read();
    emit_env_xvector_memory(vector, offset, false);
}

void lata_env_xvst(IR2_OPND vector, int offset)
{
    LataContext *ctx = lata_context_current();
    if (lata_vcache_enabled() && latx_cpu_has_lasx() &&
        vcache_offset_is_z(offset)) {
        int slot = vcache_lookup(offset);

        if (slot < 0) {
            lata_vcache_note_miss();
            slot = (int)vcache_choose_slot();
            /* Evicting a dirty victim must write it back first. */
            vcache_flush_slot((unsigned)slot);
            ctx->zcache[slot].valid = true;
            ctx->zcache[slot].offset = offset;
            ctx->zcache[slot].dirty = false;
        }
        /* Deferred: env is updated when the chunk is flushed. */
        la_xvor_v(vcache_home((unsigned)slot), vector, vector);
        ctx->zcache[slot].dirty = true;
        lata_vcache_mark_dirty((unsigned)slot);
        return;
    }
    lata_vcache_note_env_vector_write();
    if (vcache_offset_is_z(offset)) {
        lata_vcache_note_z_write();
    }
    emit_env_xvector_memory(vector, offset, true);
}

static IR2_OPND env_scalar_address(int offset, LataVectorAddressPlan *plan)
{
    IR2_OPND base = env_ir2_opnd;

    *plan = env_vector_address_plan(offset);
    if (plan->materialize_address) {
        base = ra_alloc_itemp();
        li_d(base, (int64_t)offset);
        la_add_d(base, env_ir2_opnd, base);
    }
    return base;
}

void lata_env_ld_wu(IR2_OPND scalar, int offset)
{
    if (lata_vcache_enabled() && vcache_offset_is_z(offset)) {
        vcache_flush_dirty();
    }
    LataVectorAddressPlan plan;
    IR2_OPND base = env_scalar_address(offset, &plan);

    lata_vcache_note_env_scalar_read();
    la_ld_wu(scalar, base, plan.displacement);
    if (plan.materialize_address) {
        ra_free_temp(base);
    }
}

void lata_env_ld_du(IR2_OPND scalar, int offset)
{
    if (lata_vcache_enabled() && vcache_offset_is_z(offset)) {
        vcache_flush_dirty();
    }
    LataVectorAddressPlan plan;
    IR2_OPND base = env_scalar_address(offset, &plan);

    lata_vcache_note_env_scalar_read();
    la_ld_d(scalar, base, plan.displacement);
    if (plan.materialize_address) {
        ra_free_temp(base);
    }
}

void lata_env_st_b(IR2_OPND scalar, int offset)
{
    if (lata_vcache_enabled() && vcache_offset_is_z(offset)) {
        vcache_flush_dirty();
    }
    LataVectorAddressPlan plan;
    IR2_OPND base = env_scalar_address(offset, &plan);

    lata_vcache_note_env_scalar_write();
    la_st_b(scalar, base, plan.displacement);
    if (plan.materialize_address) {
        ra_free_temp(base);
    }
}

static void generate_context_switch_bt_to_native(void)
{
    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, -256);

    /* save callee-saved LA registers. s0-s8 */
    la_st_d(s0_ir2_opnd, sp_ir2_opnd, S0_EXTRA_SPACE);
    la_st_d(s1_ir2_opnd, sp_ir2_opnd, S1_EXTRA_SPACE);
    la_st_d(s2_ir2_opnd, sp_ir2_opnd, S2_EXTRA_SPACE);
    la_st_d(s3_ir2_opnd, sp_ir2_opnd, S3_EXTRA_SPACE);
    la_st_d(s4_ir2_opnd, sp_ir2_opnd, S4_EXTRA_SPACE);
    la_st_d(s5_ir2_opnd, sp_ir2_opnd, S5_EXTRA_SPACE);
    la_st_d(s6_ir2_opnd, sp_ir2_opnd, S6_EXTRA_SPACE);
    la_st_d(s7_ir2_opnd, sp_ir2_opnd, S7_EXTRA_SPACE);
    la_st_d(s8_ir2_opnd, sp_ir2_opnd, S8_EXTRA_SPACE);

    /* save fp and ra */
    la_st_d(fp_ir2_opnd, sp_ir2_opnd, FP_EXTRA_SPACE);
    la_st_d(ra_ir2_opnd, sp_ir2_opnd, RA_EXTRA_SPACE);

    /* set env_opnd */
    la_mov64(env_ir2_opnd, a0_ir2_opnd);
    la_mov64(a0_ir2_opnd, a1_ir2_opnd);

    /* NZCV remains in env->pstate; context entry does not use LBT flags. */
    /* set fcsr, rm set by FPSCR[23:22], others was cleared */
    li_d(a5_ir2_opnd, env_offset_FPSCR());
    la_ldx_wu(a7_ir2_opnd, env_ir2_opnd, a5_ir2_opnd);
    la_bstrpick_d(a7_ir2_opnd, a7_ir2_opnd, 23, 22);
    li_d(a6_ir2_opnd, 1 << 6 | 3 << 4 | 2 << 2 | 0);
    la_slli_d(a7_ir2_opnd, a7_ir2_opnd, 1);
    la_srl_d(a7_ir2_opnd, a6_ir2_opnd, a7_ir2_opnd);
    la_andi(a7_ir2_opnd, a7_ir2_opnd, 0x3);
    la_slli_d(a7_ir2_opnd, a7_ir2_opnd, 8);
    la_movgr2fcsr(fcsr_ir2_opnd, a7_ir2_opnd);


    /* GPRs are preloaded; vector registers load lazily on first use. */
    for(int i = 0; i <= 31; ++i) {
        if (arm_la_map[i] > 0) {
            la_ld_d(ir2_opnd_new(IR2_OPND_GPR, arm_la_map[i]), env_ir2_opnd, env_offset_gpr(i));
        }
    }
    for(int i = 0; i <= 31; ++i) {
        if(arm_la_map[i] > 0) {
            la_ld_d(ir2_opnd_new(IR2_OPND_GPR, arm_la_map[i]), env_ir2_opnd, env_offset_gpr(i));
        }
    }

    /* Initialize scalar mask register when LSX is available */
    if (latx_cpu_has_lsx()) {
        la_vldi(fsmask_ir2_opnd, 0x190f);
        la_vextl_qu_du(fsmask_ir2_opnd, fsmask_ir2_opnd);
    }

    /* jmp to tb */
    la_jirl(zero_ir2_opnd, a0_ir2_opnd, 0);

}

static void generate_context_switch_native_to_bt(void)
{
    la_mov64(a0_ir2_opnd, zero_ir2_opnd);
    for(int i = 0; i <= 31; ++i) {
        if(arm_la_map[i] > 0) {
            la_st_d(ir2_opnd_new(IR2_OPND_GPR, arm_la_map[i]), env_ir2_opnd, env_offset_gpr(i));
        }
    }
    /*
     * Vector registers: a translated TB publishes them at its exit site
     * (lata_emit_dirty_vreg_stores).  Hand-built or partially instrumented
     * paths leave the flag clear, and the stub then publishes everything.
     */
    {
        IR2_OPND skip = ir2_opnd_new_type(IR2_OPND_LABEL);

        li_d(t0_ir2_opnd, (int64_t)(uintptr_t)&lata_vreg_published);
        la_ld_wu(t1_ir2_opnd, t0_ir2_opnd, 0);
        la_bnez(t1_ir2_opnd, skip);
        if (latx_cpu_has_lsx()) {
            for(int i = 0; i <= 31; ++i) {
                if (arm_la_fmap[i] >= 0) {
                    lata_env_vst(ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[i]),
                                 env_offset_vreg(i));
                }
            }
        }
        la_label(skip);
        /* A new TB starts unpublished; its exit publishes its own registers. */
        li_d(t0_ir2_opnd, (int64_t)(uintptr_t)&lata_vreg_published);
        la_st_w(zero_ir2_opnd, t0_ir2_opnd, 0);
    }
    /* NZCV remains in env->pstate; context exit does not use LBT flags. */
    /* store fcsr(rouding mode) to vfp.xregs[ARM_VFP_FPSCR] */
    la_movfcsr2gr(t0_ir2_opnd, fcsr3_ir2_opnd);
    li_d(t1_ir2_opnd, 2 << 6 | 1 << 4 | 3 << 2 | 0);
    la_srli_d(t0_ir2_opnd, t0_ir2_opnd, 7);
    la_srl_d(t0_ir2_opnd, t1_ir2_opnd, t0_ir2_opnd);
    la_andi(t0_ir2_opnd, t0_ir2_opnd, 0x3);
    li_d(t2_ir2_opnd, env_offset_FPSCR());
    la_ldx_wu(t1_ir2_opnd, env_ir2_opnd, t2_ir2_opnd);
    la_bstrins_d(t1_ir2_opnd, t0_ir2_opnd, 23, 22);
    la_stx_w(t1_ir2_opnd, env_ir2_opnd, t2_ir2_opnd);

    /* load callee-saved LA registers. s0-s8 */
    la_ld_d(s0_ir2_opnd, sp_ir2_opnd, S0_EXTRA_SPACE);
    la_ld_d(s1_ir2_opnd, sp_ir2_opnd, S1_EXTRA_SPACE);
    la_ld_d(s2_ir2_opnd, sp_ir2_opnd, S2_EXTRA_SPACE);
    la_ld_d(s3_ir2_opnd, sp_ir2_opnd, S3_EXTRA_SPACE);
    la_ld_d(s4_ir2_opnd, sp_ir2_opnd, S4_EXTRA_SPACE);
    la_ld_d(s5_ir2_opnd, sp_ir2_opnd, S5_EXTRA_SPACE);
    la_ld_d(s6_ir2_opnd, sp_ir2_opnd, S6_EXTRA_SPACE);
    la_ld_d(s7_ir2_opnd, sp_ir2_opnd, S7_EXTRA_SPACE);
    la_ld_d(s8_ir2_opnd, sp_ir2_opnd, S8_EXTRA_SPACE);

    /* load fp and ra */
    la_ld_d(fp_ir2_opnd, sp_ir2_opnd, FP_EXTRA_SPACE);
    la_ld_d(ra_ir2_opnd, sp_ir2_opnd, RA_EXTRA_SPACE);

    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, 256);

    /* TODO: clear fcsr*/
    la_movgr2fcsr(fcsr_ir2_opnd, zero_ir2_opnd);

    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
}



/* bt -> native */
int lata_gen_prologue(TCGContext *tcg_ctx)
{
    lsassert(context_switch_bt_to_native == 0);
    void *code_buf_rw = tcg_ctx->code_ptr;
    const void *code_buf_rx = tcg_splitwx_to_rx(code_buf_rw);
    context_switch_bt_to_native = (uint64_t)code_buf_rx;

    if (lata_dump)
        qemu_log("[LATA] context_switch_bt_to_native = %p\n",
                (void *)context_switch_bt_to_native);

    tr_init(NULL);
    generate_context_switch_bt_to_native();
//    int ins_nr = la_encode(tcg_ctx, code_buf_rw);
    int ins_nr = tr_ir2_assemble(code_buf_rw);
    tr_fini();

    return ins_nr;
}

/* native -> bt */
int lata_gen_epilogue(TCGContext *tcg_ctx)
{
    lsassert(context_switch_native_to_bt == 0);
    void *code_buf_rw = tcg_ctx->code_ptr;
    const void *code_buf_rx = tcg_splitwx_to_rx(code_buf_rw);
    context_switch_native_to_bt_ret_0 = (uint64_t)code_buf_rx;
    context_switch_native_to_bt = (uint64_t)code_buf_rx + 4;

    if (lata_dump)
        qemu_log("[LATA] context_switch_native_to_bt = %p\n",
                (void *)context_switch_native_to_bt);

    tr_init(NULL);
    generate_context_switch_native_to_bt();
    int ins_nr = tr_ir2_assemble(code_buf_rw);
    tr_fini();

    return ins_nr;
}

void label_dispose(void)
{
    /* 1. record the positions of label */
    int *label_num_to_ir2_num =
        (int *)alloca(lsenv->tr_data->label_num * 4 + 20);
    memset(label_num_to_ir2_num, -1, lsenv->tr_data->label_num * 4 + 20);
    int ir2_num = 0;
    IR2_INST *ir2_current = lsenv->tr_data->first_ir2;
    while (ir2_current != NULL) {
        if (ir2_current->_opcode == LISA_LABEL) {
            int label_num = ir2_opnd_label_id(&ir2_current->_opnd[0]);
            lsassertm(label_num_to_ir2_num[label_num] == -1,
                      "label %d is in multiple positions\n", label_num);
            label_num_to_ir2_num[label_num] = ir2_num;
        } else if (ir2_current->_opcode == LISA_X86_INST) {
            /* will not be assembled */
        } else {
            ir2_num++;
        }
        ir2_current = ir2_next(ir2_current);
    }

    /* 2. resolve the offset of successor linkage code */
    /* @jmp_target_arg recoed the jmp  inst position */
    /* @jmp_reset_offset record the successor inst of jmp(exclude delay slot).
     */
    /*                   when the tb is removed from buffer. the jmp inst use
     */
    /*                   this position to revert the original "fall through".
     */
    {
        TranslationBlock *tb = lsenv->tr_data->curr_tb;

        /* prologue/epilogue has no tb */
        if (tb) {
            /* ctx->jmp_insn_offset point to tb->jmp_target_arg */
            int label_id_0 = tb->jmp_reset_offset[0];
            if (label_id_0 != TB_JMP_OFFSET_INVALID) {
                tb->jmp_insn_offset[0] =
                    (label_num_to_ir2_num[label_id_0] << 2);
                tb->jmp_reset_offset[0] =
                    (label_num_to_ir2_num[label_id_0] << 2) + B_STUB_SIZE;
                // tb->jmp_target_addr[0] = (label_num_to_ir2_num[label_id_0] << 2);
            }

            int label_id_1 = tb->jmp_reset_offset[1];
            if (label_id_1 != TB_JMP_OFFSET_INVALID) {
                tb->jmp_insn_offset[1] =
                    (label_num_to_ir2_num[label_id_1] << 2);
                tb->jmp_reset_offset[1] =
                    (label_num_to_ir2_num[label_id_1] << 2) + B_STUB_SIZE;
                // tb->jmp_target_addr[1] = (label_num_to_ir2_num[label_id_1] << 2);
            }

#ifdef CONFIG_LATA_INSTS_PATTERN
            label_id_0 = tb->nzcv_save[0];
            if (label_id_0 != TB_JMP_OFFSET_INVALID) {
                tb->nzcv_save[0] = (label_num_to_ir2_num[label_id_0] << 2);
            }

            label_id_1 = tb->nzcv_save[1];
            if (label_id_1 != TB_JMP_OFFSET_INVALID) {
                tb->nzcv_save[1] = (label_num_to_ir2_num[label_id_1] << 2);
            }
#endif
        }
    }

    /* 3. resolve the branch instructions */
    ir2_num = 0;
    ir2_current = lsenv->tr_data->first_ir2;
    while (ir2_current != NULL) {
        IR2_OPCODE opcode = ir2_current->_opcode;
        if (ir2_opcode_is_branch(opcode) || ir2_opcode_is_f_branch(opcode)) {
            IR2_OPND *label_opnd = NULL;
            if (ir2_current->_opcode == LISA_B ||
                ir2_current->_opcode == LISA_BL) {
                label_opnd = &ir2_current->_opnd[0];
            } else if (ir2_opcode_is_branch_with_3opnds(opcode)) {
                label_opnd = &ir2_current->_opnd[2];
            } else if (ir2_opcode_is_f_branch(opcode) ||
                       ir2_opcode_is_branch_with_2opnds(opcode)) {
                label_opnd = &ir2_current->_opnd[1];
            }
            if(label_opnd && ir2_opnd_is_label(label_opnd)) {
                int label_num = label_opnd->_label_id;
                lsassert(label_num > 0 && label_num <= lsenv->tr_data->label_num);
                int target_ir2_num = label_num_to_ir2_num[label_num];
                lsassertm(target_ir2_num != -1, "label %d is not inserted\n",
                          label_num);
                ir2_opnd_convert_label_to_imm(label_opnd,
                                              target_ir2_num - ir2_num);
            }
        }

        if (ir2_current->_opcode != LISA_LABEL &&
            ir2_current->_opcode != LISA_X86_INST) {
            ir2_num++;
        }
        ir2_current = ir2_next(ir2_current);
    }
}

int tr_ir2_assemble(const void *code_start_addr)
{
    if (lata_dump) {
        qemu_log("[LATA] Assemble IR2.\n");
    }

    /* 1. assign temp register to physical register */
    /* now, temp register is physical register */

    /* 2. label dispose */
    label_dispose();

    /* 3. assemble */
    IR2_INST *pir2 = lsenv->tr_data->first_ir2;

    void *code_addr = (void *) code_start_addr;
    int code_nr = 0;

    while (pir2 != NULL) {
        if (ir2_opcode(pir2) != LISA_LABEL &&
            ir2_opcode(pir2) != LISA_X86_INST) {
            uint32 result = ir2_assemble(pir2);

            if (lata_dump) {
                qemu_log("IR2[%03d] at %p 0x%08x \n", pir2->_id, code_addr,
                        result);
                ir2_dump(pir2);
            }

            *(uint32 *)code_addr = result;
            code_addr = code_addr + 4;
            code_nr += 1;
        }
        pir2 = ir2_next(pir2);
    }

    return code_nr;
}

static void emit_zero_z_upper(int rd);

void lata_gen_call_helper_prologue(TCGContext *tcg_ctx)
{
    for(int i = 0; i <= 31; ++i) {
        if(arm_la_map[i] > 0) {
            la_st_d(ir2_opnd_new(IR2_OPND_GPR, arm_la_map[i]), env_ir2_opnd, env_offset_gpr(i));
        }
    }

    for(int i = 0; i <= 31; ++i) {
        if (arm_la_fmap[i] >= 0 &&
            (lsenv->tr_data->vreg_resident_dirty & (1u << i)) != 0) {
            lata_zcache_invalidate_reg(i);
            lata_env_vst(ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[i]), env_offset_vreg(i));
            if (lsenv->tr_data->vreg_v_written_mask & (1u << i)) {
                emit_zero_z_upper(i);
            }
        }
    }
    lsenv->tr_data->vreg_resident_dirty = 0;
    lsenv->tr_data->vreg_v_written_mask = 0;

    /* Helper boundaries keep NZCV in env->pstate, not the LBT flag register. */
    /* store fcsr(rouding mode) to vfp.xregs[ARM_VFP_FPSCR] */
    la_movfcsr2gr(t0_ir2_opnd, fcsr3_ir2_opnd);
    li_d(t1_ir2_opnd, 2 << 6 | 1 << 4 | 3 << 2 | 0);
    la_srli_d(t0_ir2_opnd, t0_ir2_opnd, 7);
    la_srl_d(t0_ir2_opnd, t1_ir2_opnd, t0_ir2_opnd);
    la_andi(t0_ir2_opnd, t0_ir2_opnd, 0x3);
    li_d(t2_ir2_opnd, env_offset_FPSCR());
    la_ldx_wu(t1_ir2_opnd, env_ir2_opnd, t2_ir2_opnd);
    la_bstrins_d(t1_ir2_opnd, t0_ir2_opnd, 23, 22);
    la_stx_w(t1_ir2_opnd, env_ir2_opnd, t2_ir2_opnd);

    /* clear fcsr */
    la_movgr2fcsr(fcsr_ir2_opnd, zero_ir2_opnd);
}

void lata_gen_call_helper_epilogue(TCGContext *tcg_ctx)
{
    /* Restore scalar/FPSCR state below; NZCV remains in env->pstate. */

    /* set fcsr, rm set by FPSCR[23:22], others was cleared */
    li_d(a5_ir2_opnd, env_offset_FPSCR());
    la_ldx_wu(a7_ir2_opnd, env_ir2_opnd, a5_ir2_opnd);
    la_bstrpick_d(a7_ir2_opnd, a7_ir2_opnd, 23, 22);
    li_d(a6_ir2_opnd, 1 << 6 | 3 << 4 | 2 << 2 | 0);
    la_slli_d(a7_ir2_opnd, a7_ir2_opnd, 1);
    la_srl_d(a7_ir2_opnd, a6_ir2_opnd, a7_ir2_opnd);
    la_andi(a7_ir2_opnd, a7_ir2_opnd, 0x3);
    la_slli_d(a7_ir2_opnd, a7_ir2_opnd, 8);
    la_movgr2fcsr(fcsr_ir2_opnd, a7_ir2_opnd);

    for(int i = 0; i <= 31; ++i) {
        if (arm_la_fmap[i] >= 0 && (lata_context_current()->vreg_tb_resident & (1u << i)) != 0) {
            lata_env_vld(ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[i]), env_offset_vreg(i));
        }
    }

    for(int i = 0; i <= 31; ++i) {
        if(arm_la_map[i] > 0) {
            la_ld_d(ir2_opnd_new(IR2_OPND_GPR, arm_la_map[i]), env_ir2_opnd, env_offset_gpr(i));
        }
    }
}

void lata_fast_jmp_cache_add(CPUArchState *env, uint64_t guest_pc, uint64_t host_pc)
{
    if(option_fam_jmp_cache){
        if (guest_pc < env->mapping_range[0] || guest_pc >= env->mapping_range[1]) {
            lsassertm(0, "option_fam_jmp_cache error pc, %lx, %lx, %lx\n",
                    guest_pc, env->mapping_range[0], env->mapping_range[1]);
        }
        uint64_t* pc_map_cache_ptr = (uint64_t*)env->pc_map_cache;
        pc_map_cache_ptr[guest_pc] = host_pc;  
        // printf("cache:%p guest_pc:%p  host_pc:%p store_pc:%p store_pos:%p\n",(void*)env->pc_map_cache,(void*)guest_pc,(void*)host_pc,(void *)pc_map_cache_ptr[guest_pc],(void *)(uint64_t*)(cpu->pc_map_cache + guest_pc));
        return;
    }
    int64_t index = ((guest_pc >> LATA_PC_LOW_BIT) & (TB_JMP_CACHE_SIZE - 1)) << 4;
    *(uint64_t*)((uint8_t*)env->pc_map_cache + index) = guest_pc;
    *(uint64_t*)((uint8_t*)env->pc_map_cache + index + 8) = host_pc;
}

void lata_fast_jmp_cache_init(CPUArchState *env,uint64_t start_code,uint64_t end_code)
{
    size_t malloc_size;
    if(option_fam_jmp_cache){
        env->mapping_range[0] = start_code;
        env->mapping_range[1] = end_code; 
        malloc_size = (end_code - start_code) * 8;
        void* p = malloc(malloc_size);
        if (!p) {
            lsassertm(0, "fam_indirect malloc error!\n");
        }
        memset(p, 0, malloc_size);
        fam_cache = p - (start_code * 8);
        env->pc_map_cache = fam_cache;    
        return;    
    }
    malloc_size = sizeof(uint64_t) * TB_JMP_CACHE_SIZE * 2;
    ibtc_cache = malloc(malloc_size);
    if (!ibtc_cache) {
        lsassertm(0, "ibtc_cache malloc error!\n");
    }
    env->pc_map_cache = ibtc_cache;    
}

/* gpr同时为src和dst, 使用alloc_gpr_src,并且需要store回去 */
IR2_OPND alloc_gpr_src(int i) {
    if(i==31){
        return ir2_opnd_new(IR2_OPND_GPR, la_zero);
    }
    if (arm_la_map[i] >= 0) {
        return ir2_opnd_new(IR2_OPND_GPR, arm_la_map[i]);
    } else {
        IR2_OPND t = ra_alloc_itemp();
        la_ld_d(t, env_ir2_opnd, env_offset_gpr(i));
        return t;
    }
}

IR2_OPND alloc_gpr_src_sp(int i) {
    if (arm_la_map[i] >= 0) {
        return ir2_opnd_new(IR2_OPND_GPR, arm_la_map[i]);
    } else {
        IR2_OPND t = ra_alloc_itemp();
        la_ld_d(t, env_ir2_opnd, env_offset_gpr(i));
        return t;
    }
}

IR2_OPND alloc_gpr_dst(int i) {
    if(i==31){
        return ir2_opnd_new(IR2_OPND_GPR, la_zero);
    }
    if (arm_la_map[i] >= 0) {
        return ir2_opnd_new(IR2_OPND_GPR, arm_la_map[i]);
    } else {
        return ra_alloc_itemp();
    }
}

IR2_OPND alloc_gpr_dst_sp(int i) {
    if (arm_la_map[i] >= 0) {
        return ir2_opnd_new(IR2_OPND_GPR, arm_la_map[i]);
    } else {
        return ra_alloc_itemp();
    }
}

/* la_zero作为目的寄存器不需要写回 */
void store_gpr_dst(int i, IR2_OPND opnd) {
    if (arm_la_map[i] < 0 && opnd.val != 0) {
        la_st_d(opnd, env_ir2_opnd, env_offset_gpr(i));
    } else if (i >= 0 && i < 31 && arm_la_map[i] >= 0 &&
               opnd.val != arm_la_map[i]) {
        la_mov64(ir2_opnd_new(IR2_OPND_GPR, arm_la_map[i]), opnd);
    }
}

/* la_zero写回 */
void store_gpr_zero(int i) {
    if (arm_la_map[i] < 0) {
        la_st_d(zero_ir2_opnd, env_ir2_opnd, env_offset_gpr(i));
    }else{
        IR2_OPND reg_d = alloc_gpr_dst(i);
        la_ori(reg_d, zero_ir2_opnd, 0);
        free_alloc_gpr(reg_d);
    }   
}

/* la_zero不会被映射，但也不是临时寄存器 */
void free_alloc_gpr(IR2_OPND opnd) {
    if (arm_la_reverse_map[opnd.val] == -1 && opnd.val != 0) {
        ra_free_temp(opnd);
    }
}

// fpr
/* --- guest V-register residency ------------------------------------------ */

static void emit_zero_z_upper(int rd)
{
    CPUARMState *cpu_env = (CPUARMState *)lsenv->cpu_state;
    int zoff = env_offset_zreg(rd);
    if (zoff < 0) {
        return;
    }
    if (cpu_env != NULL && !arm_isar_has_aa64_sve(&cpu_env->arm_cpu.isar)) {
        return;
    }
    /* AArch64 V/Q/D/S/H/B writes zero Z[rd][VL-1 : 128] when VL > 128. */
    IR2_OPND end = ra_alloc_itemp();
    lata_env_ld_wu(end, env_offset_sve_active_vl_bits());
    IR2_OPND ptr = ra_alloc_itemp();
    IR2_OPND skip = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND loop = ir2_opnd_new_type(IR2_OPND_LABEL);

    la_srli_d(end, end, 3);
    li_d(ptr, 16);
    la_bgeu(ptr, end, skip);
    li_d(ptr, zoff);
    la_add_d(ptr, env_ir2_opnd, ptr);
    la_add_d(end, ptr, end);
    la_addi_d(ptr, ptr, 16);
    la_label(loop);
    la_st_d(zero_ir2_opnd, ptr, 0);
    la_addi_d(ptr, ptr, 8);
    la_bltu(ptr, end, loop);
    la_label(skip);
    ra_free_temp(ptr);
    ra_free_temp(end);
}

uint32_t lata_vreg_exit_violations(void)
{
    return lata_context_current()->vreg_exit_violations;
}

uint32_t lata_vreg_tb_publishes(void)
{
    return lata_context_current()->vreg_publish_count;
}

uint32_t lata_vreg_lazy_loads(void)
{
    return lata_context_current()->vreg_lazy_loads;
}

uint32_t lata_vreg_temp_loads(void)
{
    return lata_context_current()->vreg_temp_loads;
}

void lata_note_vreg_v_write(int rd)
{
    if (rd < 0 || rd > 31) {
        return;
    }
    lata_zcache_invalidate_reg(rd);
    emit_zero_z_upper(rd);
    lata_context_current()->vreg_tb_resident &= ~(1u << rd);
    lsenv->tr_data->vreg_resident_dirty &= ~(1u << rd);
    lsenv->tr_data->vreg_v_written_mask &= ~(1u << rd);
}

void lata_flush_vregs_for_sve(void)
{
    LataContext *ctx = lata_context_current();
    uint32_t dirty = lsenv->tr_data->vreg_resident_dirty;
    uint32_t vmask = lsenv->tr_data->vreg_v_written_mask;

    if (dirty != 0 || vmask != 0) {
        for (int i = 0; i <= 31; ++i) {
            if (dirty & (1u << i)) {
                lata_env_vst(ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[i]),
                             env_offset_vreg(i));
                ctx->vreg_publish_count++;
            }
            if ((dirty & (1u << i)) || (vmask & (1u << i))) {
                lata_zcache_invalidate_reg(i);
                emit_zero_z_upper(i);
            }
        }
        lsenv->tr_data->vreg_resident_dirty = 0;
        lsenv->tr_data->vreg_v_written_mask = 0;
    }
    /* SVE may modify low 128 bits of any Zreg; reload on subsequent NEON use. */
    ctx->vreg_tb_resident = 0;
}

/*
 * Per-TB exit stores: publish exactly the resident registers this TB wrote.
 * Clearing the mask keeps repeated exits cheap and makes an un-discharged TB
 * visible at tr_fini().
 */
void lata_emit_dirty_vreg_stores(void)
{
    LataContext *ctx = lata_context_current();
    uint32_t dirty = lsenv->tr_data->vreg_resident_dirty;
    uint32_t vmask = lsenv->tr_data->vreg_v_written_mask;

    for (int i = 0; i <= 31; ++i) {
        if (!(dirty & (1u << i))) {
            continue;
        }
        lata_zcache_invalidate_reg(i);
        lata_env_vst(ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[i]),
                     env_offset_vreg(i));
        if (vmask & (1u << i)) {
            emit_zero_z_upper(i);
        }
        ctx->vreg_publish_count++;
    }
    lsenv->tr_data->vreg_resident_dirty = 0;
    lsenv->tr_data->vreg_v_written_mask = 0;
    {
        IR2_OPND addr = ra_alloc_itemp();
        IR2_OPND one = ra_alloc_itemp();

        li_d(addr, (int64_t)(uintptr_t)&lata_vreg_published);
        li_d(one, 1);
        la_st_w(one, addr, 0);
        ra_free_temp(one);
        ra_free_temp(addr);
    }
}

bool lata_vreg_resident(int index)
{
    if (index < 0 || index > 31) {
        return false;
    }
    return (lata_context_current()->vreg_resident_mask & (1u << index)) != 0;
}

void lata_vreg_init_residency(void)
{
    LataContext *ctx = lata_context_current();
    ctx->vreg_resident_mask = 0;
    for (int i = 0; i <= 31; ++i) {
        if (arm_la_fmap[i] < 0) {
            continue;
        }
        /* A register that now serves as a temporary must stay env-backed, or
         * the pool and the guest register would own the same register. */
        if (ra_ftemp_phys_in_pool(arm_la_fmap[i])) {
            continue;
        }
        ctx->vreg_resident_mask |= 1u << i;
    }
}

void lata_vreg_clear_residency_for_test(void)
{
    lata_context_current()->vreg_resident_mask = 0;
}

int lata_vreg_partition_violations(void)
{
    int violations = 0;

    for (int i = 0; i <= 31; ++i) {
        /* Demoted registers are env-backed by design; only a register the
         * translator still considers resident can be double-owned. */
        if (!lata_vreg_resident(i)) {
            continue;
        }
        if (ra_ftemp_phys_in_pool(arm_la_fmap[i])) {
            violations++;
        }
        if (arm_la_fmap[i] == LATA_VCACHE_HOME_BASE ||
            arm_la_fmap[i] == LATA_VCACHE_HOME_BASE + 1) {
            violations++;
        }
    }
    return violations;
}

uint32_t lata_vreg_spills(void)
{
    return lata_context_current()->vreg_spill_count;
}

uint32_t lata_vreg_resident_count(void)
{
    uint32_t count = 0;

    for (int i = 0; i <= 31; ++i) {
        if (lata_vreg_resident(i)) {
            count++;
        }
    }
    return count;
}

/*
 * Spill one *cold* resident register: the TB has not touched it, so no live
 * temporary refers to it and writing it back cannot lose a newer value.
 * Returns the guest index, or -1 when every resident register is in use.
 */
int lata_vreg_spill_cold(void)
{
    LataContext *ctx = lata_context_current();
    for (int i = 0; i <= 31; ++i) {
        if (arm_la_fmap[i] < 0) {
            continue;
        }
        if (!lata_vreg_resident(i)) {
            continue;
        }
        if (lsenv->tr_data->vreg_resident_touched & (1u << i)) {
            continue;
        }
        if (arm_la_fmap[i] == LATA_VCACHE_HOME_BASE ||
            arm_la_fmap[i] == LATA_VCACHE_HOME_BASE + 1) {
            continue; /* window homes */
        }
        if ((ctx->vreg_tb_resident & (1u << i)) != 0) {
            lata_env_vst(ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[i]),
                         env_offset_vreg(i));
        }
        ctx->vreg_resident_mask &= ~(1u << i);
        ctx->vreg_spill_count++;
        return i;
    }
    return -1;
}

IR2_OPND alloc_fpr_src(int i) {
    LataContext *ctx = lata_context_current();
    if (lata_vreg_resident(i)) {
        lsenv->tr_data->vreg_resident_touched |= 1u << i;
        if (!(ctx->vreg_tb_resident & (1u << i))) {
            /* P2: load on first use instead of preloading every register. */
            lata_env_vld(ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[i]),
                         env_offset_vreg(i));
            ctx->vreg_tb_resident |= 1u << i;
            ctx->vreg_lazy_loads++;
        }
        return ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[i]);
    } else {
        IR2_OPND t = ra_alloc_ftemp();

        ctx->vreg_temp_loads++;
        lata_env_vld(t, env_offset_vreg(i));

        return t;
    }
}

IR2_OPND alloc_fpr_dst(int i) {
    LataContext *ctx = lata_context_current();
    if (lata_vreg_resident(i)) {
        lsenv->tr_data->vreg_resident_touched |= 1u << i;
        lsenv->tr_data->vreg_resident_dirty |= 1u << i;
        lsenv->tr_data->vreg_v_written_mask |= 1u << i;
        ctx->vreg_tb_resident |= 1u << i;
        return ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[i]);
    } else {
        return ra_alloc_ftemp();
    }
}

void store_fpr_dst(int i, IR2_OPND opnd) {
    lata_zcache_invalidate_reg(i);
    if (!lata_vreg_resident(i)) {
        lata_env_vst(opnd, env_offset_vreg(i));
        emit_zero_z_upper(i);
    } else {
        lsenv->tr_data->vreg_resident_touched |= 1u << i;
        lsenv->tr_data->vreg_resident_dirty |= 1u << i;
        lsenv->tr_data->vreg_v_written_mask |= 1u << i;
    }
}

void free_alloc_fpr(IR2_OPND opnd) {
    if (arm_la_reverse_fmap[opnd.val] == -1 ||
        !lata_vreg_resident(arm_la_reverse_fmap[opnd.val])) {
        ra_free_temp(opnd);
    }
}

void lata_ir2_snapshot_take(LataIr2Snapshot *snap)
{
    TRANSLATION_DATA *t;
    LataContext *ctx = lata_context_current();

    if (snap == NULL || lsenv == NULL || lsenv->tr_data == NULL) {
        return;
    }
    t = lsenv->tr_data;
    snap->ir2_inst_num_current = t->ir2_inst_num_current;
    snap->real_ir2_inst_num = t->real_ir2_inst_num;
    snap->first_ir2_id = t->first_ir2 ? ir2_get_id(t->first_ir2) : -1;
    snap->last_ir2_id = t->last_ir2 ? ir2_get_id(t->last_ir2) : -1;
    snap->last_ir2_next = t->last_ir2 ? t->last_ir2->_next : -1;
    snap->label_num = t->label_num;
    snap->all_temp_status = t->all_temp_status;
    snap->vreg_resident_touched = t->vreg_resident_touched;
    snap->vreg_resident_dirty = t->vreg_resident_dirty;
    snap->vreg_v_written_mask = t->vreg_v_written_mask;
    snap->vreg_tb_resident = ctx->vreg_tb_resident;
    snap->w_write_flag = t->w_write_flag;
    snap->ftemp_alloc_failed = t->ftemp_alloc_failed;
    for (unsigned slot = 0; slot < LATA_VCACHE_ENTRIES; ++slot) {
        snap->zcache_slots[slot].valid = ctx->zcache[slot].valid;
        snap->zcache_slots[slot].dirty = ctx->zcache[slot].dirty;
        snap->zcache_slots[slot].offset = ctx->zcache[slot].offset;
    }
    snap->zcache_victim = ctx->zcache_victim;
    lata_vcache_snapshot_take(&snap->vcache_snap);
}

void lata_ir2_snapshot_rollback(const LataIr2Snapshot *snap)
{
    TRANSLATION_DATA *t;
    LataContext *ctx = lata_context_current();

    if (snap == NULL || lsenv == NULL || lsenv->tr_data == NULL) {
        return;
    }
    t = lsenv->tr_data;
    t->ir2_inst_num_current = snap->ir2_inst_num_current;
    t->real_ir2_inst_num = snap->real_ir2_inst_num;
    t->first_ir2 = (snap->first_ir2_id >= 0 && t->ir2_inst_array != NULL)
                       ? &t->ir2_inst_array[snap->first_ir2_id]
                       : NULL;
    t->last_ir2 = (snap->last_ir2_id >= 0 && t->ir2_inst_array != NULL)
                      ? &t->ir2_inst_array[snap->last_ir2_id]
                      : NULL;
    if (t->last_ir2 != NULL) {
        t->last_ir2->_next = snap->last_ir2_next;
    }
    t->label_num = snap->label_num;
    t->all_temp_status = snap->all_temp_status;
    t->vreg_resident_touched = snap->vreg_resident_touched;
    t->vreg_resident_dirty = snap->vreg_resident_dirty;
    t->vreg_v_written_mask = snap->vreg_v_written_mask;
    ctx->vreg_tb_resident = snap->vreg_tb_resident;
    t->w_write_flag = snap->w_write_flag;
    t->ftemp_alloc_failed = snap->ftemp_alloc_failed;
    for (unsigned slot = 0; slot < LATA_VCACHE_ENTRIES; ++slot) {
        ctx->zcache[slot].valid = snap->zcache_slots[slot].valid;
        ctx->zcache[slot].dirty = snap->zcache_slots[slot].dirty;
        ctx->zcache[slot].offset = snap->zcache_slots[slot].offset;
    }
    ctx->zcache_victim = snap->zcache_victim;
    lata_vcache_snapshot_restore(&snap->vcache_snap);
}
