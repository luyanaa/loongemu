/*
 * LATM runtime: MIPS guest register map, IR2 assembly, and TB entry/exit.
 *
 * The context-switch shape is adapted from LATA (target/arm/lata/lata.c).
 * Guest FPRs live in CPUMIPSState; FPU instructions call C helpers with the
 * shared softfloat status, so no FPR is pinned to a host register.
 */
#include "qemu/osdep.h"
#define LATM_REGCACHE_WRAP
#include "qemu/cpu-features.h"
#include "latm.h"
#include "mem.h"
#include "translate.h"
#include "reg-alloc.h"
#include "reg-map.h"
#include "env.h"
#include "internal.h"
#include "macro-inst.h"
#include "loongemu-perf.h"

/* MIPS guest -> LoongArch GPR map.  $0 is virtual; $31 uses $ra and is
 * saved/restored around helper calls. */
const int mips_la_map[32] = {
    [0]  = NOT_DEF,
    [1]  = la_a1,  [2]  = la_a2,  [3]  = la_a3,  [4]  = la_a4,
    [5]  = la_a5,  [6]  = la_a6,  [7]  = la_a7,
    [8]  = la_t3,  [9]  = la_t4,  [10] = la_t5,  [11] = la_t6,
    [12] = la_t7,  [13] = la_t8,  [14] = la_fp,
    [15] = la_s0,  [16] = la_s1,  [17] = la_s2,  [18] = la_s3,
    [19] = la_s4,  [20] = la_s5,  [21] = la_s6,  [22] = la_s7,
    [23] = NOT_DEF, [24] = NOT_DEF, [25] = NOT_DEF,
    [26] = NOT_DEF, [27] = NOT_DEF, [28] = NOT_DEF,
    [29] = la_s8,  [30] = la_r21, [31] = la_ra,
};

const int mips_la_reverse_map[32] = {
    [la_zero] = NOT_DEF, [la_ra] = 31, [la_tp] = NOT_DEF,
    [la_sp] = NOT_DEF, [la_a0] = NOT_DEF,
    [la_a1] = 1, [la_a2] = 2, [la_a3] = 3, [la_a4] = 4,
    [la_a5] = 5, [la_a6] = 6, [la_a7] = 7,
    [la_t0] = NOT_DEF, [la_t1] = NOT_DEF, [la_t2] = NOT_DEF,
    [la_t3] = 8, [la_t4] = 9, [la_t5] = 10, [la_t6] = 11,
    [la_t7] = 12, [la_t8] = 13, [la_r21] = 30, [la_fp] = 14,
    [la_s0] = 15, [la_s1] = 16, [la_s2] = 17, [la_s3] = 18,
    [la_s4] = 19, [la_s5] = 20, [la_s6] = 21, [la_s7] = 22,
    [la_s8] = 29,
};

/* All FPRs live in CPUMIPSState. */
const int mips_la_fmap[32] = {
    [0 ... 31] = NOT_DEF,
};
const int mips_la_reverse_fmap[32] = {
    [0 ... 31] = NOT_DEF,
};

LATM_TLS ENV *lsenv;
LATM_TLS TRANSLATION_DATA *tr_data;

bool lata_dump;
bool latm_system_memory;
static uint8_t latm_translation_lock_flag;

/* The shared target-independent IR2 dump path asks for a LataContext it can
 * tag lines with. LATM has no ARM translation context, so hand it a single
 * process-wide counter; mirrors the shim in target/riscv/latr/latr.c. */
static LataContext latm_dump_context;

LataContext *lata_context_current(void)
{
    return &latm_dump_context;
}

void latm_set_system_memory(bool on)
{
    latm_system_memory = on;
}

static int global_register_init_state;

static void global_register_init(void)
{
    int expected = 0;
    int state = __atomic_load_n(&global_register_init_state, __ATOMIC_ACQUIRE);

    if (state == 2) {
        return;
    }
    if (!__atomic_compare_exchange_n(&global_register_init_state, &expected, 1,
                                     false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&global_register_init_state,
                               __ATOMIC_ACQUIRE) != 2) {
        }
        return;
    }

    env_ir2_opnd = INIT_RA(IR2_OPND_GPR, reg_statics_map[S_ENV]);
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
    fsmask_ir2_opnd = INIT_RA(IR2_OPND_FPR, la_fsmask);
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
    __atomic_store_n(&global_register_init_state, 2, __ATOMIC_RELEASE);
}

#if defined(CONFIG_LATM_TRANSLATE_TLS)
void latm_translate_lock(void) { }
void latm_translate_unlock(void) { }
#else
void latm_translate_lock(void)
{
    while (__atomic_test_and_set(&latm_translation_lock_flag,
                                 __ATOMIC_ACQUIRE)) {
    }
}

void latm_translate_unlock(void)
{
    __atomic_clear(&latm_translation_lock_flag, __ATOMIC_RELEASE);
}
#endif

void latm_bind_env(void *cpu)
{
    CPUMIPSState *env = cpu;
    if (!env) {
        return;
    }
    if (!env->latm_env) {
        lata_tr_data_init(env);
        return;
    }
    lsenv = env->latm_env;
    tr_data = env->latm_tr_data;
}

void lata_tr_data_init(void *envp)
{
    CPUMIPSState *env = envp;
    if (!env->latm_env) {
        env->latm_env = mm_calloc(1, sizeof(ENV));
        env->latm_tr_data = mm_calloc(1, sizeof(TRANSLATION_DATA));
        env->latm_jmp_cache = mm_calloc(LATM_JMP_CACHE_SIZE,
                                        sizeof(LatmJmpCacheEntry));
    }
    /* The translator emits `gr2scr`/`jiscr0` only when the host decodes LBT;
     * without the probe a non-LBT host takes an instruction-nonexistent fault
     * on the first chained indirect jump. */
    latm_have_lbt = latx_cpu_has_lbt() != 0;
    lsenv = env->latm_env;
    tr_data = env->latm_tr_data;
    lsenv->cpu_state = env;
    lsenv->tr_data = tr_data;
    if (!env->lcsr_cpucfg1) {
        env->lcsr_cpucfg1 = (1u << CPUCFG1_FP) | (1u << CPUCFG1_MMI) |
                            (1u << CPUCFG1_LSLDR0) | (1u << CPUCFG1_LLEXC);
    }
    if (!env->lcsr_cpucfg2) {
        env->lcsr_cpucfg2 = (1u << CPUCFG2_LEXT1) | (1u << CPUCFG2_LEXT2) |
                            (1u << CPUCFG2_LEXT3) | (1u << CPUCFG2_LSPW) |
                            (1u << CPUCFG2_LCSRP);
    }
    if (!tr_data->ir1_list) {
        tr_data->ir1_list = mm_calloc(TCG_MAX_INSNS, sizeof(DisasContext));
        tr_data->db_rel = mm_calloc(1, sizeof(DisasContextBase));
    }
    global_register_init();
}

void tr_init(void *tb)
{
    TRANSLATION_DATA *t = lsenv->tr_data;
    t->curr_tb = tb;

    latm_regcache_reset();
    ra_free_all();

    if (t->ir2_inst_array == NULL) {
        t->ir2_inst_array = (IR2_INST *)mm_calloc(1024, sizeof(IR2_INST));
        t->ir2_inst_num_max = 1024;
    }
    t->ir2_inst_num_current = 0;
    t->real_ir2_inst_num = 0;
    t->first_ir2 = NULL;
    t->last_ir2 = NULL;
    t->label_num = 0;
    if (!t->ir1_list) {
        t->ir1_list = mm_calloc(TCG_MAX_INSNS, sizeof(DisasContext));
        t->db_rel = mm_calloc(1, sizeof(DisasContextBase));
    }
}

void tr_fini(void)
{
    latm_regcache_flush();
    ra_free_all();
    latm_regcache_reset();
    TRANSLATION_DATA *t = lsenv->tr_data;
    t->ir2_inst_num_current = 0;
    t->real_ir2_inst_num = 0;
    t->first_ir2 = NULL;
    t->last_ir2 = NULL;
    t->label_num = 0;
    if (t->lock_held) {
        t->lock_held = false;
        latm_translate_unlock();
    }
}

#define EXTRA_SPACE 40
#define REG_LEN     8
#define S0_EXTRA_SPACE  EXTRA_SPACE
#define S1_EXTRA_SPACE  (S0_EXTRA_SPACE + REG_LEN)
#define S2_EXTRA_SPACE  (S1_EXTRA_SPACE + REG_LEN)
#define S3_EXTRA_SPACE  (S2_EXTRA_SPACE + REG_LEN)
#define S4_EXTRA_SPACE  (S3_EXTRA_SPACE + REG_LEN)
#define S5_EXTRA_SPACE  (S4_EXTRA_SPACE + REG_LEN)
#define S6_EXTRA_SPACE  (S5_EXTRA_SPACE + REG_LEN)
#define S7_EXTRA_SPACE  (S6_EXTRA_SPACE + REG_LEN)
#define S8_EXTRA_SPACE  (S7_EXTRA_SPACE + REG_LEN)
#define FP_EXTRA_SPACE  (S8_EXTRA_SPACE + REG_LEN)
#define RA_EXTRA_SPACE  (FP_EXTRA_SPACE + REG_LEN)

static void emit_gpr_from_env(IR2_OPND opnd, int guest_reg)
{
    latm_env_ld_d(opnd, env_offset_gpr(guest_reg));
}

static void emit_gpr_to_env(IR2_OPND opnd, int guest_reg)
{
    latm_env_st_d(opnd, env_offset_gpr(guest_reg));
}

void latm_env_ld_d(IR2_OPND dst, int offset)
{
    if (offset >= -2048 && offset <= 2047) {
        la_ld_d(dst, env_ir2_opnd, offset);
        return;
    }
    li_d(a0_ir2_opnd, offset);
    la_add_d(a0_ir2_opnd, env_ir2_opnd, a0_ir2_opnd);
    la_ld_d(dst, a0_ir2_opnd, 0);
}

void latm_env_st_d(IR2_OPND val, int offset)
{
    if (offset >= -2048 && offset <= 2047) {
        la_st_d(val, env_ir2_opnd, offset);
        return;
    }
    li_d(a0_ir2_opnd, offset);
    la_add_d(a0_ir2_opnd, env_ir2_opnd, a0_ir2_opnd);
    la_st_d(val, a0_ir2_opnd, 0);
}

void latm_env_ld_w(IR2_OPND dst, int offset)
{
    if (offset >= -2048 && offset <= 2047) {
        la_ld_w(dst, env_ir2_opnd, offset);
        return;
    }
    li_d(a0_ir2_opnd, offset);
    la_add_d(a0_ir2_opnd, env_ir2_opnd, a0_ir2_opnd);
    la_ld_w(dst, a0_ir2_opnd, 0);
}

void latm_env_st_w(IR2_OPND val, int offset)
{
    if (offset >= -2048 && offset <= 2047) {
        la_st_w(val, env_ir2_opnd, offset);
        return;
    }
    li_d(a0_ir2_opnd, offset);
    la_add_d(a0_ir2_opnd, env_ir2_opnd, a0_ir2_opnd);
    la_st_w(val, a0_ir2_opnd, 0);
}

static int regcache_temp_slot(int temp)
{
    if (temp < 0 || temp >= 32) {
        return -1;
    }
    int slot = reg_itemp_reverse_map[temp];
    return slot >= 0 && slot < 3 ? slot : -1;
}

static LatmRegCacheEntry *regcache_owner(int temp)
{
    LatmRegCacheEntry *entries = lsenv->tr_data->regcache.gpr;
    for (int i = 0; i < 32; i++) {
        if (entries[i].valid && entries[i].temp == temp) {
            return &entries[i];
        }
    }
    return NULL;
}

static void regcache_touch(LatmRegCacheEntry *entry)
{
    entry->age = ++lsenv->tr_data->regcache.clock;
}

static void regcache_writeback(LatmRegCacheEntry *entry)
{
    int reg = (int)(entry - lsenv->tr_data->regcache.gpr);
    IR2_OPND value = ir2_opnd_new(IR2_OPND_GPR, entry->temp);
    latm_env_st_d(value, env_offset_gpr(reg));
}

/*
 * Drops guest register `reg` from the cache if it is still resident in host
 * temporary `temp`, writing it back when dirty. Called when a temporary is about
 * to be returned to the shared pool: leaving the entry valid would let the next
 * allocation overwrite the guest's value (the `jalr $25` corruption), while
 * dropping it silently would lose an unflushed write.
 */
void latm_regcache_release_temp(int reg, int temp)
{
    LatmRegCacheEntry *entry;

    if (reg <= 0 || reg >= 32) {
        return;
    }
    entry = &lsenv->tr_data->regcache.gpr[reg];
    if (!entry->valid || entry->temp != temp) {
        return;
    }
    if (entry->dirty) {
        regcache_writeback(entry);
    }
    entry->valid = 0;
    entry->dirty = 0;
    entry->temp = -1;
}

static void regcache_release(LatmRegCacheEntry *entry, bool writeback)
{
    if (!entry->valid) {
        return;
    }
    if (entry->dirty && writeback) {
        regcache_writeback(entry);
    }

    int temp = entry->temp;
    int slot = regcache_temp_slot(temp);
    uint8_t *borrows = lsenv->tr_data->regcache.itemp_borrows;
    entry->temp = -1;
    entry->valid = 0;
    entry->dirty = 0;
    entry->age = 0;
    if (slot >= 0 && borrows[slot] == 0) {
        ra_free_itemp(temp);
    }
}

static bool regcache_evict(void)
{
    LatmRegCacheEntry *entries = lsenv->tr_data->regcache.gpr;
    uint8_t *borrows = lsenv->tr_data->regcache.itemp_borrows;
    LatmRegCacheEntry *victim = NULL;
    bool victim_dirty = false;

    /* Reuse clean entries before forcing a pending architectural write. */
    for (int pass = 0; pass < 2 && victim == NULL; pass++) {
        for (int i = 0; i < 32; i++) {
            LatmRegCacheEntry *entry = &entries[i];
            int slot;
            if (!entry->valid || (slot = regcache_temp_slot(entry->temp)) < 0 ||
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
    regcache_release(victim, victim_dirty);
    return true;
}

void latm_regcache_reset(void)
{
    LatmRegCache *cache = &lsenv->tr_data->regcache;
    memset(cache, 0, sizeof(*cache));
    for (int i = 0; i < 32; i++) {
        cache->gpr[i].temp = -1;
    }
}

void latm_regcache_flush(void)
{
    LatmRegCacheEntry *gpr = lsenv->tr_data->regcache.gpr;
    for (int i = 0; i < 32; i++) {
        if (gpr[i].valid && gpr[i].dirty) {
            regcache_writeback(&gpr[i]);
        }
    }
    for (int i = 0; i < 32; i++) {
        regcache_release(&gpr[i], false);
    }
}

bool latm_regcache_evict_itemp(void)
{
    return regcache_evict();
}

static void regcache_borrow(IR2_OPND value)
{
    int slot = regcache_temp_slot(value._reg_num);
    lsassertm(slot >= 0, "guest register cache requires an itemp");
    uint8_t *borrows = lsenv->tr_data->regcache.itemp_borrows;
    lsassertm(borrows[slot] != UINT8_MAX, "register-cache borrow overflow");
    borrows[slot]++;
}

static void regcache_free_borrow(IR2_OPND value)
{
    int slot = regcache_temp_slot(value._reg_num);
    if (slot < 0) {
        ra_free_itemp(value._reg_num);
        return;
    }
    uint8_t *borrows = lsenv->tr_data->regcache.itemp_borrows;
    if (borrows[slot] != 0) {
        borrows[slot]--;
        if (borrows[slot] == 0 && regcache_owner(value._reg_num) == NULL) {
            ra_free_itemp(value._reg_num);
        }
    } else if (regcache_owner(value._reg_num) == NULL) {
        ra_free_itemp(value._reg_num);
    }
}

static void regcache_store(int reg, IR2_OPND value)
{
    LatmRegCacheEntry *entry = &lsenv->tr_data->regcache.gpr[reg];
    lsassertm(regcache_temp_slot(value._reg_num) >= 0,
              "guest register writeback requires an itemp");

    if (entry->valid && entry->temp == value._reg_num) {
        entry->dirty = 1;
        regcache_touch(entry);
        return;
    }
    regcache_release(entry, false);

    LatmRegCacheEntry *old_owner = regcache_owner(value._reg_num);
    if (old_owner != NULL) {
        if (old_owner->dirty) {
            regcache_writeback(old_owner);
        }
        old_owner->temp = -1;
        old_owner->valid = 0;
        old_owner->dirty = 0;
        old_owner->age = 0;
    }
    entry->temp = value._reg_num;
    entry->valid = 1;
    entry->dirty = 1;
    regcache_touch(entry);
}

static void regcache_store_zero(int reg)
{
    LatmRegCacheEntry *entry = &lsenv->tr_data->regcache.gpr[reg];
    int slot = entry->valid ? regcache_temp_slot(entry->temp) : -1;
    if (slot >= 0 && lsenv->tr_data->regcache.itemp_borrows[slot] == 0) {
        IR2_OPND value = ir2_opnd_new(IR2_OPND_GPR, entry->temp);
        la_ori(value, zero_ir2_opnd, 0);
        entry->dirty = 1;
        regcache_touch(entry);
        return;
    }

    IR2_OPND value = ra_alloc_itemp();
    la_ori(value, zero_ir2_opnd, 0);
    regcache_store(reg, value);
}


static void generate_context_switch_bt_to_native(void)
{
    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, -256);
    la_st_d(s0_ir2_opnd, sp_ir2_opnd, S0_EXTRA_SPACE);
    la_st_d(s1_ir2_opnd, sp_ir2_opnd, S1_EXTRA_SPACE);
    la_st_d(s2_ir2_opnd, sp_ir2_opnd, S2_EXTRA_SPACE);
    la_st_d(s3_ir2_opnd, sp_ir2_opnd, S3_EXTRA_SPACE);
    la_st_d(s4_ir2_opnd, sp_ir2_opnd, S4_EXTRA_SPACE);
    la_st_d(s5_ir2_opnd, sp_ir2_opnd, S5_EXTRA_SPACE);
    la_st_d(s6_ir2_opnd, sp_ir2_opnd, S6_EXTRA_SPACE);
    la_st_d(s7_ir2_opnd, sp_ir2_opnd, S7_EXTRA_SPACE);
    la_st_d(s8_ir2_opnd, sp_ir2_opnd, S8_EXTRA_SPACE);
    la_st_d(fp_ir2_opnd, sp_ir2_opnd, FP_EXTRA_SPACE);
    la_st_d(ra_ir2_opnd, sp_ir2_opnd, RA_EXTRA_SPACE);

    la_mov64(env_ir2_opnd, a0_ir2_opnd);
    /* a0 is not a guest register (r4 maps to a4); keep the TB target there. */
    la_mov64(a0_ir2_opnd, a1_ir2_opnd);

    for (int i = 1; i < 32; ++i) {
        if (mips_la_map[i] > 0) {
            emit_gpr_from_env(ir2_opnd_new(IR2_OPND_GPR, mips_la_map[i]), i);
        }
    }

    la_jirl(zero_ir2_opnd, a0_ir2_opnd, 0);
}

static void generate_context_switch_native_to_bt(void)
{
    la_mov64(a0_ir2_opnd, zero_ir2_opnd);

    for (int i = 1; i < 32; ++i) {
        if (mips_la_map[i] > 0) {
            emit_gpr_to_env(ir2_opnd_new(IR2_OPND_GPR, mips_la_map[i]), i);
        }
    }

    la_ld_d(s0_ir2_opnd, sp_ir2_opnd, S0_EXTRA_SPACE);
    la_ld_d(s1_ir2_opnd, sp_ir2_opnd, S1_EXTRA_SPACE);
    la_ld_d(s2_ir2_opnd, sp_ir2_opnd, S2_EXTRA_SPACE);
    la_ld_d(s3_ir2_opnd, sp_ir2_opnd, S3_EXTRA_SPACE);
    la_ld_d(s4_ir2_opnd, sp_ir2_opnd, S4_EXTRA_SPACE);
    la_ld_d(s5_ir2_opnd, sp_ir2_opnd, S5_EXTRA_SPACE);
    la_ld_d(s6_ir2_opnd, sp_ir2_opnd, S6_EXTRA_SPACE);
    la_ld_d(s7_ir2_opnd, sp_ir2_opnd, S7_EXTRA_SPACE);
    la_ld_d(s8_ir2_opnd, sp_ir2_opnd, S8_EXTRA_SPACE);
    la_ld_d(fp_ir2_opnd, sp_ir2_opnd, FP_EXTRA_SPACE);
    la_ld_d(ra_ir2_opnd, sp_ir2_opnd, RA_EXTRA_SPACE);
    la_addi_d(sp_ir2_opnd, sp_ir2_opnd, 256);

    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
}

/* C3: classify a guest insn's fault site. ir1.op is a coarse category — the
 * load/store/AMO classes all project a host fault to the guest memory access;
 * explicit guest traps (syscall/break/trap) and privileged ops are guest
 * checks; everything else faulting is a host bug. */
uint8_t latm_recovery_site_of(int op)
{
    switch (op) {
    case MIPS_OP_LOAD:
    case MIPS_OP_STORE:
    case MIPS_OP_AMO:
    case MIPS_OP_LOONGSON_MEM:
    case MIPS_OP_FPU_LDST:
        return LATM_RECOV_GUEST_ACCESS;
    case MIPS_OP_SYSCALL:
    case MIPS_OP_BREAK:
    case MIPS_OP_TRAP:
    case MIPS_OP_CP0_OP:
    case MIPS_OP_TLB:
        return LATM_RECOV_GUEST_CHECK;
    default:
        return LATM_RECOV_NATIVE_INTERNAL;
    }
}

/* Snapshot env-resident guest GPRs holding a deferred (uncommitted) value in
 * a host temp. `mips_la_map[r] < 0` marks env-resident; the regcache tracks a
 * live binding only when valid+dirty. Returns the count (<= max). */
uint8_t latm_regcache_snapshot_gpr_binds(LatmRecoveryBinding *out)
{
    const LatmRegCacheEntry *gpr = lsenv->tr_data->regcache.gpr;
    uint8_t n = 0;

    for (int r = 1; r < 32; r++) {
        if (mips_la_map[r] >= 0) {
            continue;
        }
        const LatmRegCacheEntry *e = &gpr[r];
        if (!e->valid || !e->dirty || e->temp < 0) {
            continue;
        }
        if (n == LATM_RECOVERY_MAX_BINDS_PER_SPAN) {
            break;
        }
        out[n].guest_reg = (uint8_t)r;
        out[n].temp = e->temp;
        out[n].is_fpr = 0;
        out[n].is_spill = 0;
        n++;
    }
    return n;
}

int lata_gen_prologue_env(void *cpu, TCGContext *tcg_ctx)
{
    void *code_buf_rw = tcg_ctx->code_ptr;
    const void *code_buf_rx = tcg_splitwx_to_rx(code_buf_rw);

    latm_translate_lock();
    latm_bind_env(cpu);
    tr_init(NULL);
    lsenv->tr_data->cs_bt_to_native = (uint64_t)code_buf_rx;
    generate_context_switch_bt_to_native();
    int ins_nr = tr_ir2_assemble(code_buf_rw);
    tr_fini();
    latm_translate_unlock();
    return ins_nr;
}

int lata_gen_epilogue_env(void *cpu, TCGContext *tcg_ctx)
{
    void *code_buf_rw = tcg_ctx->code_ptr;
    const void *code_buf_rx = tcg_splitwx_to_rx(code_buf_rw);

    latm_translate_lock();
    latm_bind_env(cpu);
    tr_init(NULL);
    lsenv->tr_data->cs_native_to_bt_ret_0 = (uint64_t)code_buf_rx;
    lsenv->tr_data->cs_native_to_bt = (uint64_t)code_buf_rx + 4;
    generate_context_switch_native_to_bt();
    int ins_nr = tr_ir2_assemble(code_buf_rw);
    tr_fini();
    latm_translate_unlock();
    return ins_nr;
}

int lata_gen_prologue(TCGContext *tcg_ctx)
{
    void *cpu = lsenv ? lsenv->cpu_state : NULL;
    return lata_gen_prologue_env(cpu, tcg_ctx);
}

int lata_gen_epilogue(TCGContext *tcg_ctx)
{
    void *cpu = lsenv ? lsenv->cpu_state : NULL;
    return lata_gen_epilogue_env(cpu, tcg_ctx);
}

void lata_gen_call_helper_prologue(TCGContext *tcg_ctx)
{
    (void)tcg_ctx;
    latm_regcache_flush();
    for (int i = 1; i < 32; ++i) {
        int la = mips_la_map[i];
        if (la == la_a1 || la == la_a2 || la == la_a3 || la == la_a4 ||
            la == la_a5 || la == la_a6 || la == la_a7 ||
            la == la_t3 || la == la_t4 || la == la_t5 ||
            la == la_t6 || la == la_t7 || la == la_t8 || la == la_ra) {
            emit_gpr_to_env(ir2_opnd_new(IR2_OPND_GPR, la), i);
        }
    }
}

void lata_gen_call_helper_epilogue(TCGContext *tcg_ctx)
{
    (void)tcg_ctx;
    for (int i = 1; i < 32; ++i) {
        int la = mips_la_map[i];
        if (la == la_a1 || la == la_a2 || la == la_a3 || la == la_a4 ||
            la == la_a5 || la == la_a6 || la == la_a7 ||
            la == la_t3 || la == la_t4 || la == la_t5 ||
            la == la_t6 || la == la_t7 || la == la_t8 || la == la_ra) {
            emit_gpr_from_env(ir2_opnd_new(IR2_OPND_GPR, la), i);
        }
    }
}

void label_dispose(void)
{
#if defined(LOONGEMU_IR2_PEEPHOLE) && LOONGEMU_IR2_PEEPHOLE
    (void)ir2_peephole_optimize(
        lsenv->tr_data->ir2_inst_array, &lsenv->tr_data->first_ir2,
        &lsenv->tr_data->last_ir2, &lsenv->tr_data->real_ir2_inst_num,
        env_ir2_opnd);
#endif
    int *label_num_to_ir2_num =
        (int *)alloca(lsenv->tr_data->label_num * sizeof(int) + 20);
    memset(label_num_to_ir2_num, -1, lsenv->tr_data->label_num * sizeof(int) + 20);
    int ir2_num = 0;
    IR2_INST *ir2_current = lsenv->tr_data->first_ir2;

    while (ir2_current != NULL) {
        if (ir2_current->_opcode == LISA_LABEL) {
            int label_num = ir2_opnd_label_id(&ir2_current->_opnd[0]);
            label_num_to_ir2_num[label_num] = ir2_num;
        } else if (ir2_current->_opcode != LISA_X86_INST) {
            ir2_num++;
        }
        ir2_current = ir2_next(ir2_current);
    }

    {
        TranslationBlock *tb = lsenv->tr_data->curr_tb;
        if (tb) {
            int label_id_0 = tb->jmp_reset_offset[0];
            if (label_id_0 != TB_JMP_OFFSET_INVALID) {
                tb->jmp_insn_offset[0] = label_num_to_ir2_num[label_id_0] << 2;
                tb->jmp_reset_offset[0] =
                    (label_num_to_ir2_num[label_id_0] << 2) + B_STUB_SIZE;
            }
            int label_id_1 = tb->jmp_reset_offset[1];
            if (label_id_1 != TB_JMP_OFFSET_INVALID) {
                tb->jmp_insn_offset[1] = label_num_to_ir2_num[label_id_1] << 2;
                tb->jmp_reset_offset[1] =
                    (label_num_to_ir2_num[label_id_1] << 2) + B_STUB_SIZE;
            }
        }
    }

    ir2_num = 0;
    ir2_current = lsenv->tr_data->first_ir2;
    while (ir2_current != NULL) {
        IR2_OPCODE opcode = ir2_current->_opcode;
        if (ir2_opcode_is_branch(opcode) || ir2_opcode_is_f_branch(opcode)) {
            IR2_OPND *label_opnd = NULL;
            if (opcode == LISA_B || opcode == LISA_BL) {
                label_opnd = &ir2_current->_opnd[0];
            } else if (ir2_opcode_is_branch_with_3opnds(opcode)) {
                label_opnd = &ir2_current->_opnd[2];
            } else if (ir2_opcode_is_f_branch(opcode) ||
                       ir2_opcode_is_branch_with_2opnds(opcode)) {
                label_opnd = &ir2_current->_opnd[1];
            }
            if (label_opnd && ir2_opnd_is_label(label_opnd)) {
                int label_num = label_opnd->_label_id;
                int target_ir2_num = label_num_to_ir2_num[label_num];
                if (target_ir2_num >= 0) {
                    ir2_opnd_convert_label_to_imm(label_opnd,
                                                  target_ir2_num - ir2_num);
                }
            }
        }
        if (opcode != LISA_LABEL && opcode != LISA_X86_INST) {
            ir2_num++;
        }
        ir2_current = ir2_next(ir2_current);
    }
}

#if defined(LOONGEMU_IR2_PEEPHOLE) && LOONGEMU_IR2_PEEPHOLE
static uint32_t latm_rebase_ir2_offset(const TRANSLATION_DATA *data,
                                       uint32_t old_offset)
{
    uint32_t new_offset = 0;

    for (IR2_INST *inst = data->first_ir2; inst != NULL; inst = ir2_next(inst)) {
        IR2_OPCODE opcode = ir2_opcode(inst);
        if (opcode != LISA_LABEL && opcode != LISA_X86_INST &&
            inst->_addr < old_offset) {
            new_offset += 4;
        }
    }
    return new_offset;
}
#endif
int tr_ir2_assemble(const void *code_start_addr)
{
    label_dispose();
    TRANSLATION_DATA *data = lsenv->tr_data;
#if defined(LOONGEMU_IR2_PEEPHOLE) && LOONGEMU_IR2_PEEPHOLE
    LatmTbInsnMap *map = data->active_insn_map;
    if (map != NULL) {
        for (unsigned i = 0; i < map->count; i++) {
            LatmTbInsnSpan *span = &map->spans[i];
            uint32_t old_start = span->native_start;
            uint32_t old_end = span->native_end;
            span->native_start = latm_rebase_ir2_offset(data, old_start);
            span->native_end = latm_rebase_ir2_offset(data, old_end);
        }
    }
#endif

    IR2_INST *pir2 = data->first_ir2;
    void *code_addr = (void *)code_start_addr;
    int code_nr = 0;

    while (pir2 != NULL) {
        if (ir2_opcode(pir2) != LISA_LABEL && ir2_opcode(pir2) != LISA_X86_INST) {
            uint32 result = ir2_assemble(pir2);
            *(uint32 *)code_addr = result;
            code_addr = (char *)code_addr + 4;
            code_nr++;
        }
        pir2 = ir2_next(pir2);
    }
    TranslationBlock *tb = data->curr_tb;
    loongemu_perf_record_tb((uint64_t)code_nr,
                            tb != NULL ? (uint64_t)tb->icount : 0);
    return code_nr;
}

IR2_OPND alloc_gpr_src(int i)
{
    if (i == 0) {
        return zero_ir2_opnd;
    }
    if (mips_la_map[i] >= 0) {
        return ir2_opnd_new(IR2_OPND_GPR, mips_la_map[i]);
    }

    LatmRegCacheEntry *entry = &lsenv->tr_data->regcache.gpr[i];
    if (entry->valid) {
        IR2_OPND value = ir2_opnd_new(IR2_OPND_GPR, entry->temp);
        regcache_borrow(value);
        regcache_touch(entry);
        return value;
    }

    IR2_OPND value = ra_alloc_itemp();
    regcache_borrow(value);
    entry->temp = value._reg_num;
    entry->valid = 1;
    entry->dirty = 0;
    regcache_touch(entry);
    latm_env_ld_d(value, env_offset_gpr(i));
    return value;
}

IR2_OPND alloc_gpr_dst(int i)
{
    if (i == 0) {
        return ra_alloc_itemp();
    }
    if (mips_la_map[i] >= 0) {
        return ir2_opnd_new(IR2_OPND_GPR, mips_la_map[i]);
    }
    IR2_OPND value = ra_alloc_itemp();
    regcache_borrow(value);
    return value;
}

void store_gpr_dst(int i, IR2_OPND opnd)
{
    if (i == 0) {
        return;
    }
    if (mips_la_map[i] < 0) {
        regcache_store(i, opnd);
    } else if (opnd._reg_num != mips_la_map[i]) {
        la_mov64(ir2_opnd_new(IR2_OPND_GPR, mips_la_map[i]), opnd);
    }
}

void store_gpr32_dst(int i, IR2_OPND opnd)
{
    if (i == 0) {
        return;
    }
    la_slli_w(opnd, opnd, 0);
    store_gpr_dst(i, opnd);
}

void store_gpr_zero(int i)
{
    if (i == 0) {
        return;
    }
    if (mips_la_map[i] < 0) {
        regcache_store_zero(i);
    } else {
        la_ori(ir2_opnd_new(IR2_OPND_GPR, mips_la_map[i]), zero_ir2_opnd, 0);
    }
}

void free_alloc_gpr(IR2_OPND opnd)
{
    if (ir2_opnd_is_itemp(&opnd)) {
        regcache_free_borrow(opnd);
    }
}

IR2_OPND alloc_fpr_src(int i)
{
    IR2_OPND t = ra_alloc_ftemp();
    la_fld_d(t, env_ir2_opnd, env_offset_fpr(i));
    return t;
}

IR2_OPND alloc_fpr_dst(int i)
{
    (void)i;
    return ra_alloc_ftemp();
}

void store_fpr_dst(int i, IR2_OPND opnd)
{
    la_fst_d(opnd, env_ir2_opnd, env_offset_fpr(i));
}

void free_alloc_fpr(IR2_OPND opnd)
{
    if (opnd._type == IR2_OPND_FPR) {
        ra_free_ftemp(opnd._reg_num);
    }
}

void lata_env_vld(IR2_OPND vector, int offset)
{
    la_vld(vector, env_ir2_opnd, offset);
}

void lata_env_vst(IR2_OPND vector, int offset)
{
    la_vst(vector, env_ir2_opnd, offset);
}

bool latm_have_lbt = true;
bool latm_link_use_jiscr1;

static inline uint32_t latm_jmp_set(uint64_t guest_pc)
{
    return (uint32_t)((guest_pc >> 2) & LATM_JMP_CACHE_SET_MASK);
}

static LatmJmpCacheEntry *latm_jmp_entries(void *cpu)
{
    return ((CPUMIPSState *)cpu)->latm_jmp_cache;
}

static void latm_jmp_cache_add_key(void *cpu, uint64_t key,
                                   uint64_t guest_pc, uint64_t host_pc,
                                   uint32_t guest_size)
{
    CPUMIPSState *env = cpu;
    LatmJmpCacheEntry *cache = latm_jmp_entries(cpu);
    LatmJmpCacheEntry *set;
    LatmJmpCacheEntry *victim = NULL;
    uint32_t age = ++env->latm_jmp_generation;

    if (!cache) {
        return;
    }
    set = &cache[latm_jmp_set(guest_pc) * LATM_JMP_CACHE_WAYS];
    for (unsigned way = 0; way < LATM_JMP_CACHE_WAYS; way++) {
        LatmJmpCacheEntry *e = &set[way];
        if (e->host_pc && e->guest_pc == guest_pc && e->key == key) {
            e->host_pc = host_pc;
            e->guest_size = guest_size;
            e->age = age;
            return;
        }
        if (!e->host_pc) {
            victim = e;
            break;
        }
        if (!victim || e->age < victim->age) {
            victim = e;
        }
    }
    victim->guest_pc = guest_pc;
    victim->host_pc = host_pc;
    victim->guest_size = guest_size;
    victim->key = key;
    victim->age = age;
}

void latm_jmp_cache_add(void *cpu, uint64_t guest_pc, uint64_t host_pc,
                        uint32_t guest_size)
{
    CPUMIPSState *env = cpu;
    latm_jmp_cache_add_key(cpu, latm_cache_key(env), guest_pc, host_pc,
                           guest_size);
}

void latm_jmp_cache_flush(void *cpu)
{
    LatmJmpCacheEntry *cache = latm_jmp_entries(cpu);

    if (cache) {
        memset(cache, 0, LATM_JMP_CACHE_SIZE * sizeof(*cache));
    }
}

uint64_t latm_jmp_cache_lookup(void *cpu, uint64_t guest_pc)
{
    CPUMIPSState *env = cpu;
    LatmJmpCacheEntry *set;

    if (!env || !env->latm_jmp_cache) {
        return 0;
    }
    set = &((LatmJmpCacheEntry *)env->latm_jmp_cache)
          [latm_jmp_set(guest_pc) * LATM_JMP_CACHE_WAYS];
    for (unsigned way = 0; way < LATM_JMP_CACHE_WAYS; way++) {
        LatmJmpCacheEntry *e = &set[way];
        if (e->host_pc && e->guest_pc == guest_pc &&
            e->key == latm_cache_key(env)) {
            e->age = ++env->latm_jmp_generation;
            return e->host_pc;
        }
    }
    return 0;
}

void latm_tb_register(void *cpu, void *tb_ptr)
{
    CPUMIPSState *env = cpu;
    TranslationBlock *tb = tb_ptr;
    TRANSLATION_DATA *t = env ? env->latm_tr_data : NULL;

    if (tb && tb->tc.ptr) {
        uint64_t key = t ? t->cache_key : latm_cache_key(env);
        latm_jmp_cache_add_key(cpu, key, tb->pc, (uintptr_t)tb->tc.ptr,
                               tb->size);
    }
}

void latm_tb_flush_range(void *cpu, uint64_t start, uint64_t end)
{
    CPUMIPSState *env = cpu;
    LatmJmpCacheEntry *cache = latm_jmp_entries(cpu);

    if (env && env->latm_tr_data) {
        latm_tb_insn_map_flush(env->latm_tr_data);
    }
    if (!cache) {
        return;
    }
    for (unsigned i = 0; i < LATM_JMP_CACHE_SIZE; i++) {
        LatmJmpCacheEntry *e = &cache[i];
        uint64_t e_end;

        if (!e->host_pc) {
            continue;
        }
        e_end = e->guest_pc + (e->guest_size ? e->guest_size : 4);
        if (e->guest_pc < end && start < e_end) {
            memset(e, 0, sizeof(*e));
        }
    }
#if defined(__loongarch__)
    __asm__ volatile("ibar 0" ::: "memory");
#endif
    latm_platform_smc_broadcast(start, end);
}

void lata_fast_jmp_cache_init(void *env, uint64_t start_code, uint64_t end_code)
{
    (void)env;
    (void)start_code;
    (void)end_code;
    latm_jmp_cache_flush(env);
}

void lata_fast_jmp_cache_add(void *env, uint64_t guest_pc, uint64_t host_pc)
{
    latm_jmp_cache_add(env, guest_pc, host_pc, 4);
}
