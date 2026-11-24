#include "latr.h"
#include "lata.h"
#include "translate.h"
#include "mmu.h"
#include "loongemu/riscv_sbi.h"
#include "loongemu/mmio_bus.h"
#include "loongemu-perf.h"

/* One context per vCPU. With LATR_ENABLE_TLS each host thread sees its own
 * bootstrap; without it (freestanding build) a single hart is bound at a
 * time, which is all the bare-metal harness needs. */
static LATR_TLS LatrTranslatorContext latr_bootstrap_context;
static LATR_TLS bool latr_bootstrap_ready;
LATR_TLS LatrTranslatorContext *latr_active_context;
LATR_TLS TRANSLATION_DATA *tr_data;
LATR_TLS ENV *lsenv;
bool latr_dump;

/* IR2 list probes; see label_dispose(). Compiled out by default: the cycle they
 * detect is now also prevented at the source (ir2_append), and a full list walk
 * per TB is pure overhead in the dispatch hot path. Turn it on when touching
 * IR2 insertion, label handling or the instruction array growth. */
#ifndef LATR_IR2_LIST_CHECK
#define LATR_IR2_LIST_CHECK 0
#endif

#if LATR_IR2_LIST_CHECK
static int latr_ir2_cap_at_tb_start;
static int latr_ir2_used_high_water;
#endif

/* The freestanding build has no qemu/osdep.h; the host log sink is provided by
 * latx/compat/libc_min.c (bare-metal) or latr/runtime.c (weak no-op). */
void qemu_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

const int latr_la_map[32] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1
};
const int latr_la_fmap[32] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1
};

/* The shared target/arm/lata/ir2/ir2.c dump path asks for a LataContext it can
 * tag lines with. LATR has no ARM translation context, so hand it a single
 * process-wide counter; see include/lata.h for why the shim exists at all. */
static LataContext latr_dump_context;

LataContext *lata_context_current(void)
{
    return &latr_dump_context;
}

static CPUArchState *latr_cpu_env(void)
{
    return (CPUArchState *)lsenv->cpu_state;
}

/* Fetches one 16-bit half through the page table and physical map. */
static bool latr_fetch16(uint64_t va, uint16_t *out, int *cause)
{
    CPUArchState *env = latr_cpu_env();
    uint64_t host = 0;
    uint64_t phys = 0;
    int fault = latr_translate(env, va, LATR_ACCESS_EXEC, 2, &host, &phys);

    if (fault != 0) {
        *cause = fault;
        return false;
    }
    if (env->mem_mmio) {
        /* Execute-in-place through an MMIO bus (pflash) must not dereference
         * the device address directly. */
        uint64_t value = 0;
        env->mem_mmio = 0;
        if (env->mmio == 0 ||
            loongemu_mmio_bus_read(env->mmio, phys, 2, &value) != 0) {
            *cause = LATR_EXCP_INST_ACCESS_FAULT;
            return false;
        }
        *out = (uint16_t)value;
        return true;
    }
    const uint8_t *p = (const uint8_t *)(uintptr_t)host;
    *out = (uint16_t)(p[0] | (uint16_t)p[1] << 8);
    return true;
}

static void latr_record_fetch_fault(uint64_t va, int cause)
{
    CPUArchState *env = latr_cpu_env();

    env->fetch_faulted = 1;
    env->fetch_fault_va = va;
    env->fetch_fault_cause = (uint64_t)cause;
}

uint16_t latr_guest_lduw(uint64_t address)
{
    CPUArchState *env = latr_cpu_env();
    uint16_t half = 0;

    int cause = 0;

    env->fetch_faulted = 0;
    if (!latr_fetch16(address, &half, &cause)) {
        latr_record_fetch_fault(address, cause);
    }
    return half;
}

/* A 32-bit instruction may straddle two pages: each half walks on its own. */
uint32_t latr_guest_ldl(uint64_t address)
{
    CPUArchState *env = latr_cpu_env();
    uint16_t lo = 0;
    uint16_t hi = 0;

    int cause = 0;

    env->fetch_faulted = 0;
    if (!latr_fetch16(address, &lo, &cause)) {
        latr_record_fetch_fault(address, cause);
        return 0;
    }
    if (!latr_fetch16(address + 2, &hi, &cause)) {
        latr_record_fetch_fault(address + 2, cause);
        return 0;
    }
    return (uint32_t)lo | (uint32_t)hi << 16;
}

LatrTranslatorContext *latr_context_create(CPUArchState *cpu,
                                           struct LatrTbCache *tb_cache,
                                           uint8_t *code_buf,
                                           uint32_t code_buf_size)
{
    LatrTranslatorContext *ctx = mm_calloc(1, sizeof(*ctx));

    if (ctx == NULL) {
        return NULL;
    }
    ctx->pvir_cache = latr_pvir_cache_create();
    if (ctx->pvir_cache == NULL) {
        mm_free(ctx);
        return NULL;
    }
    ctx->cpu = cpu;
    ctx->tb_cache = tb_cache;
    ctx->code_buf = code_buf;
    ctx->code_buf_size = code_buf_size;
    /* Adopt the cache's current generation so the first safepoint does not
     * throw away descriptors that are still valid. */
    ctx->pvir_generation = tb_cache != NULL ? tb_cache->generation : 0;
    ctx->env.cpu_state = cpu;
    ctx->env.tr_data = &ctx->tr_data;
    return ctx;
}

void latr_context_destroy(LatrTranslatorContext *ctx)
{
    if (ctx == NULL || ctx == &latr_bootstrap_context) {
        return;
    }
    if (latr_active_context == ctx) {
        latr_active_context = NULL;
        tr_data = NULL;
        lsenv = NULL;
    }
    if (ctx->tr_data.ir2_inst_array != NULL) {
        mm_free(ctx->tr_data.ir2_inst_array);
    }
    /* TB-owned recovery maps are heap-allocated per assembled block; free the
     * whole list so a destroyed context cannot leak them. */
    while (ctx->tr_data.insn_map_list != NULL) {
        LatrTbInsnMap *dead = ctx->tr_data.insn_map_list;
        ctx->tr_data.insn_map_list = dead->next;
        mm_free(dead);
    }
    latr_pvir_cache_destroy(ctx->pvir_cache);
    mm_free(ctx);
}

LatrTranslatorContext *latr_context_bind(LatrTranslatorContext *ctx)
{
    LatrTranslatorContext *prev = latr_active_context;

    if (ctx == NULL) {
        return prev;
    }
    latr_active_context = ctx;
    lsenv = &ctx->env;
    tr_data = &ctx->tr_data;
    ctx->env.tr_data = &ctx->tr_data;
    ctx->env.cpu_state = ctx->cpu;
    env_ir2_opnd = ir2_opnd_new(IR2_OPND_GPR, la_s8);
    return prev;
}

void latr_context_restore(LatrTranslatorContext *ctx)
{
    if (ctx == NULL) {
        latr_active_context = NULL;
        tr_data = NULL;
        lsenv = NULL;
        return;
    }
    (void)latr_context_bind(ctx);
}

LatrTranslatorContext *latr_context_current(void)
{
    return latr_active_context;
}

void latr_context_safepoint(LatrTranslatorContext *ctx)
{
    uint64_t generation;

    if (ctx == NULL) {
        return;
    }
    /*
     * Reclaiming the arena rewrites the bytes every native-offset map in this
     * context points at. Drop those maps in the same breath, or a host fault
     * inside a recycled arena would resolve to the guest instruction that used
     * to occupy that offset.
     */
    if (latr_tb_cache_safepoint(ctx->tb_cache) && ctx->tb_cache != NULL) {
        latr_tb_insn_map_forget_range(ctx->tb_cache->arena,
                                      ctx->tb_cache->arena_size);
    }
    generation = ctx->tb_cache != NULL ? ctx->tb_cache->generation : 0;
    if (generation != ctx->pvir_generation) {
        latr_pvir_cache_reset(ctx->pvir_cache);
        ctx->pvir_generation = generation;
    }
}

/*
 * Switches the current hart's guest environment. The translator reaches the
 * CPU state through `lsenv->cpu_state`; keeping the context in step here means
 * a fault dispatcher can rebind a hart and immediately resolve host PCs.
 */
void latr_set_cpu_env(CPUArchState *env)
{
    LatrTranslatorContext *ctx = latr_active_context;

    if (ctx != NULL) {
        ctx->cpu = env;
    }
    if (lsenv != NULL) {
        lsenv->cpu_state = env;
    }
}

void latr_tr_data_init(CPUArchState *env)
{
    LatrTranslatorContext *ctx = latr_active_context;

    if (ctx == NULL) {
        if (!latr_bootstrap_ready) {
            memset(&latr_bootstrap_context, 0,
                   sizeof(latr_bootstrap_context));
            latr_bootstrap_context.pvir_cache = latr_pvir_cache_create();
            latr_bootstrap_ready = true;
        }
        ctx = &latr_bootstrap_context;
        latr_context_bind(ctx);
    }
    ctx->cpu = env;
    lsenv->cpu_state = env;
    lsenv->tr_data = &ctx->tr_data;
    tr_data = &ctx->tr_data;
    env_ir2_opnd = ir2_opnd_new(IR2_OPND_GPR, la_s8);
}

static LatrRegCacheStats latr_regcache_stats_counters;

void latr_emit_code_symbol(IR2_OPND dst, const void *sym)
{
    TRANSLATION_DATA *data = lsenv->tr_data;

    if (data->tb_code_sym_count < LATR_TB_CODE_SYMS) {
        int i = data->tb_code_sym_count++;

        data->tb_code_sym_ir2[i] = data->ir2_inst_num_current;
        data->tb_code_sym_fn[i] = sym;
    } else {
        /* A block that needs more slots than this is not a size problem: it is
         * a block whose relocation list is incomplete, so say so loudly. */
        data->tb_code_sym_overflow = 1;
    }
    li_d(dst, (int64_t)(uintptr_t)sym);
}

int latr_tb_code_symbol_count(void)
{
    return lsenv->tr_data->tb_code_sym_count;
}

const void *latr_tb_code_symbol(int index)
{
    if (index < 0 || index >= lsenv->tr_data->tb_code_sym_count) {
        return NULL;
    }
    return lsenv->tr_data->tb_code_sym_fn[index];
}

int latr_tb_code_symbols_overflowed(void)
{
    return lsenv->tr_data->tb_code_sym_overflow;
}

int latr_tb_code_symbol_ir2(int index)
{
    if (index < 0 || index >= lsenv->tr_data->tb_code_sym_count) {
        return -1;
    }
    return lsenv->tr_data->tb_code_sym_ir2[index];
}

void latr_regcache_stats_reset(void)
{
    latr_regcache_stats_counters = (LatrRegCacheStats){0};
}

LatrRegCacheStats latr_regcache_stats(void)
{
    return latr_regcache_stats_counters;
}

static int regcache_temp_slot(int reg, bool is_fpr)
{
    if (reg < 0 || reg >= 32) {
        return -1;
    }
    int slot = is_fpr ? reg_ftemp_reverse_map[reg]
                      : reg_itemp_reverse_map[reg];
    return slot >= 0 && slot < 6 ? slot : -1;
}

static LatrRegCacheEntry *regcache_entries(bool is_fpr)
{
    LatrRegCache *cache = &lsenv->tr_data->regcache;
    return is_fpr ? cache->fpr : cache->gpr;
}

static uint8_t *regcache_borrows(bool is_fpr)
{
    LatrRegCache *cache = &lsenv->tr_data->regcache;
    return is_fpr ? cache->ftemp_borrows : cache->itemp_borrows;
}

static LatrRegCacheEntry *regcache_owner(bool is_fpr, int temp)
{
    LatrRegCacheEntry *entries = regcache_entries(is_fpr);
    for (int i = 0; i < 32; i++) {
        if (entries[i].valid && entries[i].temp == temp) {
            return &entries[i];
        }
    }
    return NULL;
}

static void regcache_touch(LatrRegCacheEntry *entry)
{
    entry->age = ++lsenv->tr_data->regcache.clock;
}

static void regcache_set_dirty(LatrRegCacheEntry *entry, bool is_fpr)
{
    LatrRegCache *cache = &lsenv->tr_data->regcache;

    if (!entry->dirty) {
        entry->dirty = 1;
        if (is_fpr) {
            cache->fpr_dirty++;
        } else {
            cache->gpr_dirty++;
        }
    }
}

static void regcache_set_valid(LatrRegCacheEntry *entry, bool is_fpr)
{
    LatrRegCache *cache = &lsenv->tr_data->regcache;

    if (!entry->valid) {
        entry->valid = 1;
        if (is_fpr) {
            cache->fpr_valid++;
        } else {
            cache->gpr_valid++;
        }
    }
}

static void regcache_clear_valid(LatrRegCacheEntry *entry, bool is_fpr)
{
    LatrRegCache *cache = &lsenv->tr_data->regcache;

    if (entry->valid) {
        entry->valid = 0;
        if (is_fpr) {
            cache->fpr_valid--;
        } else {
            cache->gpr_valid--;
        }
    }
}

static void regcache_clear_dirty(LatrRegCacheEntry *entry, bool is_fpr)
{
    LatrRegCache *cache = &lsenv->tr_data->regcache;

    if (entry->dirty) {
        entry->dirty = 0;
        if (is_fpr) {
            cache->fpr_dirty--;
        } else {
            cache->gpr_dirty--;
        }
    }
}

static void regcache_writeback(LatrRegCacheEntry *entry, bool is_fpr)
{
    IR2_OPND value = ir2_opnd_new(is_fpr ? IR2_OPND_FPR : IR2_OPND_GPR,
                                  entry->temp);
    latr_regcache_stats_counters.writebacks++;
    if (is_fpr) {
        la_fst_d(value, env_ir2_opnd, env_offset_fpr(
            (int)(entry - lsenv->tr_data->regcache.fpr)));
    } else {
        la_st_d(value, env_ir2_opnd, env_offset_gpr(
            (int)(entry - lsenv->tr_data->regcache.gpr)));
    }
}

static void regcache_release(LatrRegCacheEntry *entry, bool is_fpr,
                             bool writeback)
{
    if (!entry->valid) {
        return;
    }
    if (entry->dirty && writeback) {
        regcache_writeback(entry, is_fpr);
    }

    int temp = entry->temp;
    int slot = regcache_temp_slot(temp, is_fpr);
    uint8_t *borrows = regcache_borrows(is_fpr);
    regcache_clear_dirty(entry, is_fpr);
    regcache_clear_valid(entry, is_fpr);
    entry->temp = -1;
    entry->age = 0;
    if (slot >= 0 && borrows[slot] == 0) {
        if (is_fpr) {
            ra_free_ftemp(temp);
        } else {
            ra_free_itemp(temp);
        }
    }
}

static bool regcache_evict(bool is_fpr)
{
    LatrRegCacheEntry *entries = regcache_entries(is_fpr);
    uint8_t *borrows = regcache_borrows(is_fpr);
    LatrRegCacheEntry *victim = NULL;
    bool victim_dirty = false;

    /* Reuse clean entries before forcing a pending architectural write. */
    for (int pass = 0; pass < 2 && victim == NULL; pass++) {
        for (int i = 0; i < 32; i++) {
            LatrRegCacheEntry *entry = &entries[i];
            int slot;
            if (!entry->valid || (slot = regcache_temp_slot(entry->temp,
                                                             is_fpr)) < 0 ||
                borrows[slot] != 0 || (pass == 0 && entry->dirty)) {
                continue;
            }
            if (victim == NULL || entry->age < victim->age) {
                victim = entry;
                victim_dirty = entry->dirty;
            }
        }
    }
    if (victim == NULL) {
        return false;
    }
    latr_regcache_stats_counters.evictions++;
    regcache_release(victim, is_fpr, victim_dirty);
    return true;
}

void latr_regcache_reset(void)
{
    LatrRegCache *cache = &lsenv->tr_data->regcache;
    memset(cache, 0, sizeof(*cache)); /* includes the dirty counters */
    for (int i = 0; i < 32; i++) {
        cache->gpr[i].temp = -1;
        cache->fpr[i].temp = -1;
    }
}

void latr_regcache_flush(void)
{
    LatrRegCache *cache = &lsenv->tr_data->regcache;
    LatrRegCacheEntry *gpr = regcache_entries(false);
    LatrRegCacheEntry *fpr = regcache_entries(true);

    latr_regcache_stats_counters.flushes++;
    if (cache->gpr_dirty == 0 && cache->fpr_dirty == 0) {
        latr_regcache_stats_counters.empty_flushes++;
    }
    /*
     * Only walk a bank that has something to write back. The release loop still
     * visits both banks: a clean entry must leave the cache at a control-flow
     * edge even though it costs no store.
     */
    if (cache->gpr_valid != 0 && cache->gpr_dirty != 0) {
        for (int i = 0; i < 32; i++) {
            latr_regcache_stats_counters.scan_entries++;
            if (gpr[i].valid && gpr[i].dirty) {
                regcache_writeback(&gpr[i], false);
            }
        }
    }
    if (cache->fpr_valid != 0 && cache->fpr_dirty != 0) {
        for (int i = 0; i < 32; i++) {
            latr_regcache_stats_counters.scan_entries++;
            if (fpr[i].valid && fpr[i].dirty) {
                regcache_writeback(&fpr[i], true);
            }
        }
    }
    if (cache->gpr_valid != 0) {
        for (int i = 0; i < 32; i++) {
            latr_regcache_stats_counters.scan_entries++;
            regcache_release(&gpr[i], false, false);
        }
    }
    if (cache->fpr_valid != 0) {
        for (int i = 0; i < 32; i++) {
            latr_regcache_stats_counters.scan_entries++;
            regcache_release(&fpr[i], true, false);
        }
    }
    /*
     * The fast path trusts the counters, so check them: a flush must leave both
     * banks empty and clean. A desync here would show up much later as a stale
     * or missing guest value, which is exactly the failure mode this cache is
     * hardest to debug.
     */
    lsassertm(cache->gpr_valid == 0 && cache->fpr_valid == 0 &&
              cache->gpr_dirty == 0 && cache->fpr_dirty == 0,
              "register-cache counters desynced after flush");
}

bool latr_regcache_evict_itemp(void)
{
    return regcache_evict(false);
}

bool latr_regcache_evict_ftemp(void)
{
    return regcache_evict(true);
}

static void regcache_borrow(IR2_OPND value, bool is_fpr)
{
    int slot = regcache_temp_slot(value._reg_num, is_fpr);
    lsassertm(slot >= 0, "guest register cache requires a temporary operand");
    uint8_t *borrows = regcache_borrows(is_fpr);
    lsassertm(borrows[slot] != UINT8_MAX, "register-cache borrow overflow");
    borrows[slot]++;
}

static void regcache_free_borrow(IR2_OPND value, bool is_fpr)
{
    int slot = regcache_temp_slot(value._reg_num, is_fpr);
    if (slot < 0) {
        if (is_fpr) {
            ra_free_ftemp(value._reg_num);
        } else {
            ra_free_itemp(value._reg_num);
        }
        return;
    }
    uint8_t *borrows = regcache_borrows(is_fpr);
    if (borrows[slot] != 0) {
        borrows[slot]--;
        if (borrows[slot] == 0 &&
            regcache_owner(is_fpr, value._reg_num) == NULL) {
            if (is_fpr) {
                ra_free_ftemp(value._reg_num);
            } else {
                ra_free_itemp(value._reg_num);
            }
        }
    } else if (regcache_owner(is_fpr, value._reg_num) == NULL) {
        if (is_fpr) {
            ra_free_ftemp(value._reg_num);
        } else {
            ra_free_itemp(value._reg_num);
        }
    }
}

static void regcache_store(int reg, IR2_OPND value, bool is_fpr)
{
    LatrRegCacheEntry *entries = regcache_entries(is_fpr);

    latr_regcache_stats_counters.stores++;
    LatrRegCacheEntry *entry = &entries[reg];
    int slot = regcache_temp_slot(value._reg_num, is_fpr);
    lsassertm(slot >= 0, "guest register writeback requires a temporary operand");

    if (entry->valid && entry->temp == value._reg_num) {
        regcache_set_dirty(entry, is_fpr);
        regcache_touch(entry);
        return;
    }
    regcache_release(entry, is_fpr, false);

    LatrRegCacheEntry *old_owner = regcache_owner(is_fpr, value._reg_num);
    if (old_owner != NULL) {
        if (old_owner->dirty) {
            regcache_writeback(old_owner, is_fpr);
        }
        regcache_clear_dirty(old_owner, is_fpr);
        regcache_clear_valid(old_owner, is_fpr);
        old_owner->temp = -1;
        old_owner->age = 0;
    }
    entry->temp = value._reg_num;
    regcache_set_valid(entry, is_fpr);
    regcache_set_dirty(entry, is_fpr);
    regcache_touch(entry);
}

void latr_regcache_prepare_fpr_write(IR2_OPND value)
{
    LatrRegCacheEntry *entry = regcache_owner(true, value._reg_num);
    if (entry != NULL) {
        regcache_release(entry, true, true);
    }
}

/* C1: classify a decoded guest insn by whether a host fault inside its span
 * can be blamed on that guest instruction. Loads/stores/atomics fault as
 * guest memory accesses; explicit trap/return insns fault as guest checks;
 * everything else is a host-internal region -- a fault there is a translator
 * bug and is never projected to a guest exception. */
static uint8_t latr_recovery_site_of(LatrRiscvOpcode opcode)
{
    switch (opcode) {
    case LATR_RV_LB: case LATR_RV_LH: case LATR_RV_LW: case LATR_RV_LD:
    case LATR_RV_LBU: case LATR_RV_LHU: case LATR_RV_LWU:
    case LATR_RV_SB: case LATR_RV_SH: case LATR_RV_SW: case LATR_RV_SD:
    case LATR_RV_FLW: case LATR_RV_FLD: case LATR_RV_FSW: case LATR_RV_FSD:
    case LATR_RV_VLE8_V: case LATR_RV_VLE16_V:
    case LATR_RV_VLE32_V: case LATR_RV_VLE64_V:
    case LATR_RV_VLE8FF_V: case LATR_RV_VLE16FF_V:
    case LATR_RV_VLE32FF_V: case LATR_RV_VLE64FF_V:
    case LATR_RV_VSE8_V: case LATR_RV_VSE16_V:
    case LATR_RV_VSE32_V: case LATR_RV_VSE64_V:
    case LATR_RV_VLSE8_V: case LATR_RV_VLSE16_V:
    case LATR_RV_VLSE32_V: case LATR_RV_VLSE64_V:
    case LATR_RV_VSSE8_V: case LATR_RV_VSSE16_V:
    case LATR_RV_VSSE32_V: case LATR_RV_VSSE64_V:
    case LATR_RV_VLXEI8_V: case LATR_RV_VLXEI16_V:
    case LATR_RV_VLXEI32_V: case LATR_RV_VLXEI64_V:
    case LATR_RV_VSXEI8_V: case LATR_RV_VSXEI16_V:
    case LATR_RV_VSXEI32_V: case LATR_RV_VSXEI64_V:
    case LATR_RV_VLM_V: case LATR_RV_VSM_V:
    case LATR_RV_LR_W: case LATR_RV_SC_W: case LATR_RV_LR_D: case LATR_RV_SC_D:
    case LATR_RV_AMOSWAP_W: case LATR_RV_AMOADD_W: case LATR_RV_AMOXOR_W:
    case LATR_RV_AMOAND_W: case LATR_RV_AMOOR_W: case LATR_RV_AMOMIN_W:
    case LATR_RV_AMOMAX_W: case LATR_RV_AMOMINU_W: case LATR_RV_AMOMAXU_W:
    case LATR_RV_AMOSWAP_D: case LATR_RV_AMOADD_D: case LATR_RV_AMOXOR_D:
    case LATR_RV_AMOAND_D: case LATR_RV_AMOOR_D: case LATR_RV_AMOMIN_D:
    case LATR_RV_AMOMAX_D: case LATR_RV_AMOMINU_D: case LATR_RV_AMOMAXU_D:
        return LATR_RECOV_GUEST_ACCESS;
    case LATR_RV_ECALL: case LATR_RV_EBREAK:
    case LATR_RV_MRET: case LATR_RV_SRET:
    case LATR_RV_WFI: case LATR_RV_SFENCE_VMA: case LATR_RV_FENCE_I:
        return LATR_RECOV_GUEST_CHECK;
    default:
        return LATR_RECOV_NATIVE_INTERNAL;
    }
}

/*
 * How much of a guest instruction may already have landed when the host takes
 * a fault inside its span:
 *
 *   BEFORE_INSN      nothing committed; the guest can re-execute it (scalar
 *                    loads/stores either complete or fault as one host access)
 *   AFTER_FALLTHROUGH the trap is the instruction's effect; pc already points
 *                    at the vector, so the guest must not see it re-executed
 *   PARTIAL_EFFECT   element-wise state (RVV element loads/stores) may hold
 *                    committed prefixes; recovery cannot project that
 */
static uint8_t latr_recovery_phase_of(LatrRiscvOpcode opcode)
{
    switch (opcode) {
    case LATR_RV_ECALL:
    case LATR_RV_EBREAK:
    case LATR_RV_MRET:
    case LATR_RV_SRET:
    case LATR_RV_WFI:
    case LATR_RV_SFENCE_VMA:
    case LATR_RV_FENCE_I:
        return LE_PHASE_AFTER_FALLTHROUGH;
    case LATR_RV_VLE8_V: case LATR_RV_VLE16_V:
    case LATR_RV_VLE32_V: case LATR_RV_VLE64_V:
    case LATR_RV_VLE8FF_V: case LATR_RV_VLE16FF_V:
    case LATR_RV_VLE32FF_V: case LATR_RV_VLE64FF_V:
    case LATR_RV_VSE8_V: case LATR_RV_VSE16_V:
    case LATR_RV_VSE32_V: case LATR_RV_VSE64_V:
    case LATR_RV_VLSE8_V: case LATR_RV_VLSE16_V:
    case LATR_RV_VLSE32_V: case LATR_RV_VLSE64_V:
    case LATR_RV_VSSE8_V: case LATR_RV_VSSE16_V:
    case LATR_RV_VSSE32_V: case LATR_RV_VSSE64_V:
    case LATR_RV_VLXEI8_V: case LATR_RV_VLXEI16_V:
    case LATR_RV_VLXEI32_V: case LATR_RV_VLXEI64_V:
    case LATR_RV_VSXEI8_V: case LATR_RV_VSXEI16_V:
    case LATR_RV_VSXEI32_V: case LATR_RV_VSXEI64_V:
    case LATR_RV_VLM_V: case LATR_RV_VSM_V:
        return LE_PHASE_PARTIAL_EFFECT;
    default:
        return LE_PHASE_BEFORE_INSN;
    }
}

/* C1: capture the GPR regcache bindings live right now -- each guest GPR whose
 * newest value is sitting in a host temp and not yet written back to env->gpr.
 * `temp` is the host phys reg number (reg_itemp_map slot already resolved by
 * the allocator), so recovery can read it straight out of a ucontext. */
static uint8_t latr_regcache_snapshot_gpr_binds(LatrRecoveryBinding *out)
{
    LatrRegCache *cache = &lsenv->tr_data->regcache;
    uint8_t n = 0;
    for (int r = 0; r < 32 && n < LATR_RECOVERY_MAX_BINDS_PER_SPAN; r++) {
        if (cache->gpr[r].valid && cache->gpr[r].dirty) {
            out[n].guest_reg = (uint8_t)r;
            out[n].temp = cache->gpr[r].temp;
            out[n].is_fpr = 0;
            out[n].is_spill = 0;
            n++;
        }
    }
    /* C4: deferred FPR state rides the same span. temp is a phys freg
     * (reg_ftemp_map pool 24..29); apply reads host_fprs[temp]. */
    for (int r = 0; r < 32 && n < LATR_RECOVERY_MAX_BINDS_PER_SPAN; r++) {
        if (cache->fpr[r].valid && cache->fpr[r].dirty) {
            out[n].guest_reg = (uint8_t)r;
            out[n].temp = cache->fpr[r].temp;
            out[n].is_fpr = 1;
            out[n].is_spill = 0;
            n++;
        }
    }
    return n;
}

void tr_init(void *tb)
{
    TRANSLATION_DATA *data = lsenv->tr_data;
    data->curr_tb = tb;
    latr_regcache_reset();
    ra_free_all();
    latr_pvir_cache_clear();
    if (data->ir2_inst_array == NULL) {
        data->ir2_inst_array = mm_calloc(400, sizeof(IR2_INST));
        lsassertm(data->ir2_inst_array != NULL, "cannot allocate LATR IR2 array");
        data->ir2_inst_num_max = 400;
    }
    data->ir2_inst_num_current = 0;
    data->real_ir2_inst_num = 0;
    data->first_ir2 = NULL;
    data->last_ir2 = NULL;
    data->label_num = 0;
    data->tb_code_sym_count = 0;
    data->tb_code_sym_overflow = 0;
#if LATR_IR2_LIST_CHECK
    latr_ir2_cap_at_tb_start = data->ir2_inst_num_max;
#endif
}

/*
 * VMM-side SBI entry: services an S-mode ECALL trap (mcause = 9 with
 * mstatus.MPP = S) and resumes the guest at mepc + 4. Returns false when the
 * pending trap is not an S-mode ECALL, so the caller can handle it normally.
 */
bool latr_service_sbi(CPUArchState *env, LoongemuSbi *sbi)
{
    unsigned cause = (unsigned)env->exception;
    unsigned mpp = (unsigned)((env->csr[0x300] >> 11) & 3);

    if (cause != 9 || mpp != LATR_RISCV_PRIV_S || sbi == NULL) {
        return false;
    }

    uint64_t args[6] = {
        env->gpr[10], env->gpr[11], env->gpr[12],
        env->gpr[13], env->gpr[14], env->gpr[15],
    };
    LoongemuSbiRet ret = loongemu_sbi_call(sbi, env->gpr[17], env->gpr[16],
                                           args);

    env->gpr[10] = (uint64_t)ret.error;
    env->gpr[11] = ret.value;
    env->pc = env->csr[0x341] + 4; /* mepc points at the ECALL */
    env->exception = 0;
    env->privilege = LATR_RISCV_PRIV_S;
    return true;
}

void tr_fini(void)
{
    TRANSLATION_DATA *data = lsenv->tr_data;
    latr_regcache_flush();
    ra_free_all();
    latr_regcache_reset();
    data->ir2_inst_num_current = 0;
    data->real_ir2_inst_num = 0;
    data->first_ir2 = NULL;
    data->last_ir2 = NULL;
    data->label_num = 0;
}

IR2_OPND alloc_gpr_src(int reg)
{
    lsassertm(reg >= 0 && reg < 32, "invalid RISC-V source register %d", reg);
    if (reg == 0) {
        return zero_ir2_opnd;
    }

    LatrRegCacheEntry *entry = &lsenv->tr_data->regcache.gpr[reg];
    if (entry->valid) {
        IR2_OPND value = ir2_opnd_new(IR2_OPND_GPR, entry->temp);
        regcache_borrow(value, false);
        regcache_touch(entry);
        return value;
    }

    IR2_OPND value = ra_alloc_itemp();
    regcache_borrow(value, false);
    entry->temp = value._reg_num;
    regcache_set_valid(entry, false);
    entry->dirty = 0;
    regcache_touch(entry);
    latr_regcache_stats_counters.reloads++;
    la_ld_d(value, env_ir2_opnd, env_offset_gpr(reg));
    return value;
}

IR2_OPND alloc_gpr_dst(int reg)
{
    lsassertm(reg >= 0 && reg < 32, "invalid RISC-V destination register %d", reg);
    if (reg == 0) {
        return zero_ir2_opnd;
    }
    IR2_OPND value = ra_alloc_itemp();
    regcache_borrow(value, false);
    return value;
}

void store_gpr_dst(int reg, IR2_OPND value)
{
    if (reg != 0) {
        lsassertm(reg >= 0 && reg < 32, "invalid RISC-V destination register %d", reg);
        regcache_store(reg, value, false);
    }
}

void free_alloc_gpr(IR2_OPND value)
{
    if (ir2_opnd_is_itemp(&value)) {
        regcache_free_borrow(value, false);
    }
}

IR2_OPND alloc_fpr_src(int reg)
{
    lsassertm(reg >= 0 && reg < 32, "invalid RISC-V FP source register %d", reg);

    LatrRegCacheEntry *entry = &lsenv->tr_data->regcache.fpr[reg];
    if (entry->valid) {
        IR2_OPND value = ir2_opnd_new(IR2_OPND_FPR, entry->temp);
        regcache_borrow(value, true);
        regcache_touch(entry);
        return value;
    }

    IR2_OPND value = ra_alloc_ftemp();
    regcache_borrow(value, true);
    entry->temp = value._reg_num;
    regcache_set_valid(entry, true);
    entry->dirty = 0;
    regcache_touch(entry);
    latr_regcache_stats_counters.reloads++;
    la_fld_d(value, env_ir2_opnd, env_offset_fpr(reg));
    return value;
}

IR2_OPND alloc_fpr_dst(int reg)
{
    lsassertm(reg >= 0 && reg < 32, "invalid RISC-V FP destination register %d", reg);
    IR2_OPND value = ra_alloc_ftemp();
    regcache_borrow(value, true);
    return value;
}

void store_fpr_dst(int reg, IR2_OPND value)
{
    lsassertm(reg >= 0 && reg < 32, "invalid RISC-V FP destination register %d", reg);
    regcache_store(reg, value, true);
}

void free_alloc_fpr(IR2_OPND value)
{
    if (ir2_opnd_is_ftemp(&value)) {
        regcache_free_borrow(value, true);
    }
}

static int find_pc(const RiscvIR1 *insns, int count, uint64_t pc)
{
    for (int i = 0; i <= count; i++) {
        uint64_t candidate = i == count ? insns[count - 1].pc + insns[count - 1].raw_len : insns[i].pc;
        if (candidate == pc) {
            return i;
        }
    }
    return -1;
}

static bool has_direct_target(LatrRiscvOpcode opcode)
{
    switch (opcode) {
    case LATR_RV_JAL:
    case LATR_RV_BEQ:
    case LATR_RV_BNE:
    case LATR_RV_BLT:
    case LATR_RV_BGE:
    case LATR_RV_BLTU:
    case LATR_RV_BGEU:
        return true;
    default:
        return false;
    }
}

static void store_pc_constant(uint64_t pc)
{
    IR2_OPND value = ra_alloc_itemp();
    li_d(value, (int64_t)pc);
    la_st_d(value, env_ir2_opnd, env_offset_pc());
    ra_free_temp(value);
}

bool tr_ir2_generate(TranslationBlock *tb)
{
    if (tb->icount == 0 || tb->icount > TCG_MAX_INSNS) {
        return false;
    }

    RiscvIR1 insns[TCG_MAX_INSNS];
    IR2_OPND labels[TCG_MAX_INSNS + 1];
    bool labels_are_targets[TCG_MAX_INSNS + 1] = { false };
    struct {
        uint64_t pc;
        IR2_OPND label;
    } exits[TCG_MAX_INSNS];
    int exit_count = 0;
    int count = tb->icount;

    TRANSLATION_DATA *data = lsenv->tr_data;

    uint64_t pc = tb->pc;
    for (int i = 0; i < count; i++) {
        uint16_t half = latr_guest_lduw(pc);
        uint32_t raw = (half & 3) == 3 ? latr_guest_ldl(pc) : half;
        data->tb_insn_pc[i] = pc;
        if (!latr_decode(pc, raw, &insns[i])) {
            if (latr_cpu_env()->fetch_faulted) {
                insns[i].opcode = LATR_RV_FETCH_FAULT;
                insns[i].fault_va = latr_cpu_env()->fetch_fault_va;
                insns[i].fault_cause = (int)latr_cpu_env()->fetch_fault_cause;
                insns[i].raw_len = 4;
            }
            data->tb_insn_len[i] = insns[i].raw_len;
            count = i + 1;
            pc += insns[i].raw_len;
            break;
        }
        data->tb_insn_len[i] = insns[i].raw_len;
        pc += insns[i].raw_len;
    }
    data->tb_insn_count = count;

    for (int i = 0; i <= count; i++) {
        labels[i] = ir2_opnd_new_type(IR2_OPND_LABEL);
        data->tb_insn_label[i] = ir2_opnd_label_id(&labels[i]);
    }

    for (int i = 0; i < count; i++) {
        if (!has_direct_target(insns[i].opcode)) {
            continue;
        }
        uint64_t target = insns[i].pc + (int64_t)insns[i].arg.imm;
        int target_index = find_pc(insns, count, target);
        if (target_index > i) {
            labels_are_targets[target_index] = true;
            continue;
        }
        int j;
        for (j = 0; j < exit_count; j++) {
            if (exits[j].pc == target) {
                break;
            }
        }
        if (j == exit_count) {
            exits[j].pc = target;
            exits[j].label = ir2_opnd_new_type(IR2_OPND_LABEL);
            exit_count++;
        }
    }

    for (int i = 0; i < count; i++) {
        LatrTranslateContext ctx = { .ir1 = &insns[i] };
        if (has_direct_target(insns[i].opcode)) {
            uint64_t target = insns[i].pc + (int64_t)insns[i].arg.imm;
            int target_index = find_pc(insns, count, target);
            if (target_index > i) {
                ctx.target_label = labels[target_index];
            } else {
                int j;
                for (j = 0; j < exit_count; j++) {
                    if (exits[j].pc == target) {
                        break;
                    }
                }
                ctx.target_label = exits[j].label;
            }
            ctx.has_target_label = true;
        }
        if (labels_are_targets[i]) {
            latr_regcache_flush();
        }
        /* P0-2: binds describe state live at the faultable access, which
         * opens this guest insn's span. Snapshot BEFORE translate so the
         * insn's own results postdate the fault and are never replayed. */
        data->tb_insn_site[i] = latr_recovery_site_of(insns[i].opcode);
        data->tb_insn_phase[i] = latr_recovery_phase_of(insns[i].opcode);
        data->tb_insn_bind_cnt[i] = latr_regcache_snapshot_gpr_binds(
            data->tb_insn_binds[i]);
        (la_label)(labels[i]);
        if (!latr_translate_ir1(&ctx)) {
            return false;
        }
    }

    latr_regcache_flush();
    (la_label)(labels[count]);
    store_pc_constant(pc);
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);

    for (int i = 0; i < exit_count; i++) {
        latr_regcache_flush();
        (la_label)(exits[i].label);
        store_pc_constant(exits[i].pc);
        la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    }
    return true;
}

static bool tb_terminator(LatrRiscvOpcode opcode)
{
    switch (opcode) {
    case LATR_RV_JAL:
    case LATR_RV_JALR:
    case LATR_RV_BEQ:
    case LATR_RV_BNE:
    case LATR_RV_BLT:
    case LATR_RV_BGE:
    case LATR_RV_BLTU:
    case LATR_RV_BGEU:
    case LATR_RV_MRET:
    case LATR_RV_SRET:
    case LATR_RV_ECALL:
    case LATR_RV_EBREAK:
    case LATR_RV_FENCE_I:
    case LATR_RV_VSETVLI:
    case LATR_RV_VSETVL:
    case LATR_RV_VSETIVLI:
        return true;
    default:
        return false;
    }
}

void target_disasm(TranslationBlock *tb, int *max_insns, CPUState *cpu)
{
    (void)cpu;
    int limit = max_insns == NULL ? TCG_MAX_INSNS : *max_insns;
    if (limit < 0) {
        limit = 0;
    } else if (limit > TCG_MAX_INSNS) {
        limit = TCG_MAX_INSNS;
    }

    uint64_t pc = tb->pc;
    int count = 0;
    bool allow_flow = (tb->cflags & LATR_TB_CFLAGS_ALLOW_FLOW) != 0;

    while (count < limit) {
        uint16_t half = latr_guest_lduw(pc);
        uint32_t raw = (half & 3) == 3 ? latr_guest_ldl(pc) : half;
        RiscvIR1 ir1;
        bool decoded = latr_decode(pc, raw, &ir1);
        uint32_t raw_len = decoded ? (uint32_t)ir1.raw_len : 2;

        if (!decoded || ir1.raw_len <= 0) {
            pc += 2;
            count++;
            break;
        }
        /* Only the first instruction may cross a 4 KiB page: later
         * instructions start a new TB at the boundary. */
        if (count > 0 &&
            ((pc ^ (pc + raw_len - 1)) & ~UINT64_C(0xfff)) != 0) {
            break;
        }
        pc += raw_len;
        count++;
        if (!allow_flow && tb_terminator(ir1.opcode)) {
            break;
        }
    }
    tb->icount = count;
    tb->size = (uint16_t)(pc - tb->pc);
    if (max_insns != NULL) {
        *max_insns = count;
    }
}

/*
 * IR2 list integrity probe.
 *
 * label_dispose() walks the _next chain twice. If that chain ever contains a
 * cycle the walk never terminates, the translator never returns, and the guest
 * simply stops between two TBs with no diagnostic at all -- which looks exactly
 * like a wedged TB. The chain is built only by ir2_append()/ir2_remove(), so a
 * cycle means some structural invariant was already violated; report it with
 * enough state to tell which one.
 *
 * Cost is O(IR2 per TB) on top of two walks that already cost the same, which is
 * why it is off by default now that ir2_append() itself refuses to close a cycle.
 */
#if LATR_IR2_LIST_CHECK
static void latr_ir2_report(const char *why, IR2_INST *at, int steps)
{
    TRANSLATION_DATA *d = lsenv->tr_data;
    int i;

    qemu_log("[LATR] ir2-list %s steps=%d used=%d cap=%d(cap@tb=%d) "
             "labels=%d first=%d last=%d\n",
             why, steps, d->ir2_inst_num_current, d->ir2_inst_num_max,
             latr_ir2_cap_at_tb_start, d->label_num,
             d->first_ir2 != NULL ? ir2_get_id(d->first_ir2) : -1,
             d->last_ir2 != NULL ? ir2_get_id(d->last_ir2) : -1);

    qemu_log("[LATR] ir2-list high-water used=%d\n", latr_ir2_used_high_water);
    for (i = 0; i < 32 && at != NULL; i++) {
        qemu_log("  [%d] op=%d prev=%d next=%d\n", at->_id, at->_opcode,
                 at->_prev, at->_next);
        at = ir2_next(at);
    }
}

static void latr_ir2_list_check(void)
{
    TRANSLATION_DATA *d = lsenv->tr_data;
    int bound = d->ir2_inst_num_current + 8;
    int steps = 0;
    IR2_INST *p = d->first_ir2;

    if (d->ir2_inst_num_current > latr_ir2_used_high_water) {
        latr_ir2_used_high_water = d->ir2_inst_num_current;
    }

    while (p != NULL) {
        if (++steps > bound) {
            latr_ir2_report("CYCLE", p, steps);
            return;
        }
        p = ir2_next(p);
    }

    if ((d->first_ir2 == NULL) != (d->last_ir2 == NULL)) {
        latr_ir2_report("HEAD/TAIL-MISMATCH", d->first_ir2, steps);
    } else if (d->last_ir2 != NULL && d->last_ir2->_next != -1) {
        latr_ir2_report("TAIL-HAS-SUCCESSOR", d->last_ir2, steps);
    }
}

void latr_ir2_list_stats(void)
{
    TRANSLATION_DATA *d = lsenv->tr_data;

    qemu_log("[LATR] ir2-list stats: high_water=%d cap=%d\n",
             latr_ir2_used_high_water, d->ir2_inst_num_max);
}
#else
void latr_ir2_list_stats(void) {}
#endif

void label_dispose(void)
{
#if LATR_IR2_LIST_CHECK
    latr_ir2_list_check();
#endif
#if defined(LOONGEMU_IR2_PEEPHOLE) && LOONGEMU_IR2_PEEPHOLE
    (void)ir2_peephole_optimize(
        lsenv->tr_data->ir2_inst_array, &lsenv->tr_data->first_ir2,
        &lsenv->tr_data->last_ir2, &lsenv->tr_data->real_ir2_inst_num,
        env_ir2_opnd);
#endif
    int label_count = lsenv->tr_data->label_num;
    int positions[label_count + 1];
    for (int i = 0; i <= label_count; i++) {
        positions[i] = -1;
    }

    int ir2_index = 0;
    for (IR2_INST *inst = lsenv->tr_data->first_ir2; inst != NULL; inst = ir2_next(inst)) {
        if (inst->_opcode == LISA_LABEL) {
            int id = ir2_opnd_label_id(&inst->_opnd[0]);
            lsassertm(id > 0 && id <= label_count, "invalid LATR label %d", id);
            lsassertm(positions[id] == -1, "duplicate LATR label %d", id);
            positions[id] = ir2_index;
        } else {
            ir2_index++;
        }
    }

    ir2_index = 0;
    for (IR2_INST *inst = lsenv->tr_data->first_ir2; inst != NULL; inst = ir2_next(inst)) {
        IR2_OPND *target = NULL;
        IR2_OPCODE opcode = ir2_opcode(inst);
        if (opcode == LISA_B || opcode == LISA_BL) {
            target = &inst->_opnd[0];
        } else if (ir2_opcode_is_branch_with_3opnds(opcode)) {
            target = &inst->_opnd[2];
        } else if (ir2_opcode_is_branch_with_2opnds(opcode) || ir2_opcode_is_f_branch(opcode)) {
            target = &inst->_opnd[1];
        }
        if (target != NULL && ir2_opnd_is_label(target)) {
            int id = ir2_opnd_label_id(target);
            lsassertm(id > 0 && id <= label_count && positions[id] >= 0,
                      "unresolved LATR label %d", id);
            ir2_opnd_convert_label_to_imm(target, positions[id] - ir2_index);
        }
        if (opcode != LISA_LABEL) {
            ir2_index++;
        }
    }
}

int tr_ir2_assemble(const void *code_start_addr)
{
    label_dispose();
    TRANSLATION_DATA *data = lsenv->tr_data;
    uint32_t *code = (uint32_t *)code_start_addr;
    int count = 0;
    uint32_t span_off[LATR_TB_MAP_SPANS + 1];

    for (int i = 0; i <= data->tb_insn_count && i <= LATR_TB_MAP_SPANS; i++) {
        span_off[i] = 0;
    }
    for (IR2_INST *inst = data->first_ir2; inst != NULL; inst = ir2_next(inst)) {
        if (ir2_opcode(inst) == LISA_LABEL) {
            int id = ir2_opnd_label_id(&inst->_opnd[0]);
            for (int i = 0; i <= data->tb_insn_count; i++) {
                if (data->tb_insn_label[i] == id) {
                    span_off[i] = (uint32_t)(count * 4);
                }
            }
            continue;
        }
        code[count++] = ir2_assemble(inst);
    }

    latr_tb_insn_map_begin((void *)code_start_addr, (uint32_t)count * 4);
    for (int i = 0; i < data->tb_insn_count; i++) {
        latr_tb_insn_map_add(data->tb_insn_pc[i], span_off[i],
                             span_off[i + 1], data->tb_insn_len[i],
                             data->tb_insn_site[i], data->tb_insn_phase[i],
                             (uint32_t)i,
                             data->tb_insn_binds[i], data->tb_insn_bind_cnt[i]);
    }
    loongemu_perf_record_tb((uint64_t)count,
                            (uint64_t)data->tb_insn_count);
    return count;
}
