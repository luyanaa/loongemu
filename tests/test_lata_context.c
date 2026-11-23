#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lata.h"
#include "lata-mem.h"
#include "lata-vcache.h"
#include "target/arm/cpu.h"
#include "target/arm/tcg/translate.h"

static unsigned checks = 0;
static unsigned failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            failures++;                                                        \
        }                                                                      \
    } while (0)

FILE *stderr = NULL;

void latx_bare_assert_fail(const char *expr, const char *file, int line)
{
    printf("ASSERT FAIL %s:%d: %s\n", file, line, expr);
    abort();
}

static LATA_TLS LataContext tls_ctx;

/* mem.c reads the bound event loop env through this TLS root, exactly like the
 * translator does; the fixture has no event loop, so it stays NULL. */
LATA_TLS ENV *lsenv = NULL;

LataContext *lata_context_current(void)
{
    return &tls_ctx;
}

void helper_exception_with_syndrome(CPUARMState *env, uint32_t excp,
                                    uint32_t syndrome)
{
    if (env != NULL) {
        env->sve_ctrl.last_exception_index = excp;
        env->sve_ctrl.last_exception_syndrome = syndrome;
    }
}

typedef struct {
    int id;
    uint32_t expected_hits;
    uint32_t expected_misses;
} ThreadArg;

static LataGuestMemStatus custom_probe(CPUARMState *env, uint64_t vaddr,
                                       size_t size, bool is_write,
                                       bool clean_tbi)
{
    (void)env;
    (void)size;
    (void)clean_tbi;
    if (vaddr == 0x5000 && is_write) {
        return LATA_MEM_FAULT_PERMISSION;
    }
    return LATA_MEM_OK;
}

static LataGuestMemStatus custom_load(CPUARMState *env, uint64_t vaddr,
                                      size_t size, bool is_signed,
                                      uint64_t *out_value)
{
    (void)env;
    (void)size;
    (void)is_signed;
    if (vaddr == 0x6000) {
        return LATA_MEM_FAULT_TRANSLATION;
    }
    *out_value = 0x12345678ULL + vaddr;
    return LATA_MEM_OK;
}

static LataGuestMemStatus custom_store(CPUARMState *env, uint64_t vaddr,
                                       size_t size, uint64_t value)
{
    (void)env;
    (void)size;
    (void)value;
    return (vaddr == 0x5000) ? LATA_MEM_FAULT_PERMISSION : LATA_MEM_OK;
}

static const LataGuestMemOps custom_ops = {
    .probe = custom_probe,
    .load = custom_load,
    .store = custom_store,
    .direct = NULL,
};

/*
 * A provider that claims the direct-access contract. `test_mapping_epoch` is
 * the invalidation source: bumping it must invalidate every proof handed out
 * to already-translated code.
 */
static uint64_t test_mapping_epoch = 7;

static uint64_t test_epoch(CPUARMState *env)
{
    (void)env;
    return test_mapping_epoch;
}

static bool test_span_admissible(CPUARMState *env, uint64_t vaddr,
                                 uint64_t size, bool is_write)
{
    (void)env;
    /* Unmapped guard page: [0x8000, 0x9000). */
    if (vaddr < 0x9000 && vaddr + size > 0x8000) {
        return false;
    }
    /* Read-only window: [0x10000, 0x11000). */
    if (is_write && vaddr < 0x11000 && vaddr + size > 0x10000) {
        return false;
    }
    return true;
}

static const LataGuestMemDirectPolicy test_policy = {
    .name = "test",
    .epoch = test_epoch,
    .span_admissible = test_span_admissible,
};

static const LataGuestMemOps direct_ops = {
    .probe = custom_probe,
    .load = custom_load,
    .store = custom_store,
    .direct = &test_policy,
};

/* A policy that is missing its epoch callback is not a proof. */
static const LataGuestMemDirectPolicy half_policy = {
    .name = "half",
    .epoch = NULL,
    .span_admissible = test_span_admissible,
};

static const LataGuestMemOps half_direct_ops = {
    .probe = custom_probe,
    .load = custom_load,
    .store = custom_store,
    .direct = &half_policy,
};

static void *worker_thread(void *arg_ptr)
{
    ThreadArg *arg = (ThreadArg *)arg_ptr;
    LataVcacheState vc;
    lata_vcache_state_init(&vc, true);
    lata_vcache_bind_state(&vc);

    memset(&tls_ctx, 0, sizeof(tls_ctx));
    tls_ctx.mem_ops = &lata_default_guest_mem_ops;
    tls_ctx.sve_const_pred_opt_enabled = (arg->id % 2 == 0);

    for (uint32_t i = 0; i < arg->expected_hits; ++i) {
        lata_vcache_note_hit();
    }
    for (uint32_t i = 0; i < arg->expected_misses; ++i) {
        lata_vcache_note_miss();
    }
    assert(lata_vcache_hits() == arg->expected_hits);
    assert(lata_vcache_misses() == arg->expected_misses);
    assert(lata_context_current()->sve_const_pred_opt_enabled ==
           (arg->id % 2 == 0));

    CPUARMState env;
    memset(&env, 0, sizeof(env));
    uint8_t buf[32];
    memset(buf, 0xaa, sizeof(buf));
    uint64_t va = (uint64_t)(uintptr_t)buf;
    if (va >= 0x1000 && !(va < 0x20000000ULL && va + 32 > 0x10000000ULL)) {
        lata_guest_mem_clear_fault_regions(&env);
        assert(lata_guest_mem_add_fault_region(&env, va, 16, LATA_MEM_PERM_READ));
        assert(lata_guest_mem_probe(&env, va, 4, false, false) == LATA_MEM_OK);
        assert(lata_guest_mem_probe(&env, va, 4, true, false) ==
               LATA_MEM_FAULT_PERMISSION);
        /* No provider proof: nothing is admissible for inline host access. */
        assert(!lata_guest_mem_direct_available());
        assert(!lata_guest_mem_direct_admit(&env, va, 4, false, 0));
    }

    if (arg->id == 1) {
        lata_set_guest_mem_ops(&custom_ops);
        uint64_t val = 0;
        assert(lata_guest_mem_load(&env, 0x2000, 4, false, &val) == LATA_MEM_OK);
        assert(val == 0x12347678ULL);
        assert(lata_guest_mem_load(&env, 0x6000, 4, false, &val) ==
               LATA_MEM_FAULT_TRANSLATION);
        assert(lata_guest_mem_probe(&env, 0x5000, 4, true, false) ==
               LATA_MEM_FAULT_PERMISSION);
    }

    return NULL;
}

/* A lowering that spills resident vectors and then rolls back its IR2 must
 * also roll back the temp pool: spill entries are permanent for the TB, so a
 * stale phy<->slot map would outlive the instructions that created it. */
static void test_ir2_snapshot_restores_ftemp_pool(void)
{
    LataContext *ctx = lata_context_current();
    LataIr2Snapshot snap;

    memset(ctx, 0, sizeof(*ctx));
    ctx->mem_ops = &lata_default_guest_mem_ops;
    ctx->ftemp_base = 6;
    ctx->ftemp_pool_entries = 6;
    for (int i = 0; i < 32; ++i) {
        ctx->reg_ftemp_map[i] = 100 + i;
        ctx->reg_ftemp_reverse_map[i] = 200 + i;
    }

    lata_ftemp_pool_snapshot(ctx, &snap);

    /* Simulate a spill: the pool grows and the maps are rewritten. */
    ctx->ftemp_base = 5;
    ctx->ftemp_pool_entries = 7;
    ctx->reg_ftemp_map[0] = 42;
    ctx->reg_ftemp_map[6] = 43;
    ctx->reg_ftemp_reverse_map[0] = 9;

    lata_ftemp_pool_rollback(ctx, &snap);

    CHECK(ctx->ftemp_base == 6);
    CHECK(ctx->ftemp_pool_entries == 6);
    CHECK(ctx->reg_ftemp_map[0] == 100);
    CHECK(ctx->reg_ftemp_map[6] == 106);
    CHECK(ctx->reg_ftemp_reverse_map[0] == 200);
    CHECK(ctx->reg_ftemp_reverse_map[31] == 231);

    /* Rollback must be idempotent w.r.t. the snapshot, not the live maps. */
    ctx->ftemp_pool_entries = 7;
    lata_ftemp_pool_rollback(ctx, &snap);
    CHECK(ctx->ftemp_pool_entries == 6);
}

int main(void)
{
    test_ir2_snapshot_restores_ftemp_pool();
    LataVcacheState main_vc;
    lata_vcache_state_init(&main_vc, true);
    lata_vcache_bind_state(&main_vc);
    memset(&tls_ctx, 0, sizeof(tls_ctx));
    tls_ctx.mem_ops = &lata_default_guest_mem_ops;
    tls_ctx.sve_const_pred_opt_enabled = true;

    pthread_t th[4];
    ThreadArg args[4];
    for (int i = 0; i < 4; ++i) {
        args[i].id = i;
        args[i].expected_hits = (uint32_t)(10 + i * 7);
        args[i].expected_misses = (uint32_t)(3 + i * 5);
        CHECK(pthread_create(&th[i], NULL, worker_thread, &args[i]) == 0);
    }
    for (int i = 0; i < 4; ++i) {
        CHECK(pthread_join(th[i], NULL) == 0);
    }

    CHECK(lata_vcache_hits() == 0);
    CHECK(lata_vcache_misses() == 0);
    CHECK(lata_get_guest_mem_ops() == &lata_default_guest_mem_ops);

    CPUARMState env;
    memset(&env, 0, sizeof(env));
    CHECK(lata_guest_mem_probe(&env, 0x100, 4, false, false) ==
          LATA_MEM_FAULT_TRANSLATION);
    CHECK(lata_guest_mem_probe(&env, 0x10000000ULL, 4, false, false) ==
          LATA_MEM_FAULT_UNMAPPED);
    lata_guest_mem_raise_fault(&env, 0x400100, 0xdeadbeefULL, 1,
                               LATA_MEM_FAULT_PERMISSION);
    CHECK(env.pc == 0x400100);
    CHECK(env.sve_ctrl.last_fault_va == 0xdeadbeefULL);
    CHECK(env.sve_ctrl.last_fault_is_write == true);
    CHECK(env.sve_ctrl.last_exception_index == EXCP_DATA_ABORT);

    /*
     * Data Abort syndrome: ESR.EC = 0x25 (Data Abort from a lower EL), IL = 1,
     * WnR in bit 6, and DFSC in bits [5:0] must be the architectural Data
     * Fault Status Code -- never the internal LataGuestMemStatus ordinal.
     */
    {
        static const struct {
            uint32_t status;
            uint32_t dfsc;
            const char *what;
        } cases[] = {
            { LATA_MEM_FAULT_TRANSLATION, 0x07, "translation l3" },
            { LATA_MEM_FAULT_UNMAPPED, 0x07, "unmapped -> translation l3" },
            { LATA_MEM_FAULT_PERMISSION, 0x0f, "permission l3" },
            { LATA_MEM_FAULT_ALIGNMENT, 0x21, "alignment" },
            { LATA_MEM_OK, 0x10, "no status -> external abort" },
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            uint32_t syn;

            lata_guest_mem_raise_fault(&env, 0x400200, 0x2000, 0, cases[i].status);
            syn = env.sve_ctrl.last_exception_syndrome;
            CHECK((syn >> 26) == 0x25u);
            CHECK(((syn >> 25) & 1u) == 1u);
            CHECK((syn & 0x3fu) == cases[i].dfsc);
            CHECK(((syn >> 6) & 1u) == 0u);

            lata_guest_mem_raise_fault(&env, 0x400200, 0x2000, 1, cases[i].status);
            syn = env.sve_ctrl.last_exception_syndrome;
            CHECK((syn & 0x3fu) == cases[i].dfsc);
            CHECK(((syn >> 6) & 1u) == 1u);
            CHECK(lata_guest_mem_status_to_dfsc(cases[i].status) == cases[i].dfsc);
        }
        /* The internal ordinal must not leak into DFSC. */
        CHECK(lata_guest_mem_status_to_dfsc(LATA_MEM_FAULT_PERMISSION) !=
              (uint32_t)LATA_MEM_FAULT_PERMISSION);
    }

    /*
     * Direct-access contract. A provider that publishes no policy leaves the
     * fast path unreachable, which is what the default provider does.
     */
    CHECK(!lata_guest_mem_direct_available());
    CHECK(lata_guest_mem_direct_epoch() == 0);
    CHECK(!lata_guest_mem_direct_admit(&env, 0x2000, 16, false, 0));

    lata_set_guest_mem_ops(&half_direct_ops);
    CHECK(!lata_guest_mem_direct_available());
    CHECK(!lata_guest_mem_direct_admit(&env, 0x2000, 16, false, 7));

    test_mapping_epoch = 7;
    lata_set_guest_mem_ops(&direct_ops);
    CHECK(lata_guest_mem_direct_available());
    CHECK(lata_guest_mem_direct_epoch() == 7);
    /* Matching epoch and an admissible span: the only combination that admits. */
    CHECK(lata_guest_mem_direct_admit(&env, 0x2000, 16, false, 7));
    CHECK(lata_guest_mem_direct_admit(&env, 0x2000, 16, true, 7));
    /* Stale proof: anything translated before an epoch bump is re-routed. */
    test_mapping_epoch = 8;
    CHECK(!lata_guest_mem_direct_admit(&env, 0x2000, 16, false, 7));
    CHECK(lata_guest_mem_direct_admit(&env, 0x2000, 16, false, 8));
    /* Span proofs are per address, not per translation. */
    CHECK(!lata_guest_mem_direct_admit(&env, 0x8ff0, 32, false, 8));
    CHECK(lata_guest_mem_direct_admit(&env, 0x9000, 32, false, 8));
    CHECK(!lata_guest_mem_direct_admit(&env, 0x10ff0, 32, true, 8));
    CHECK(lata_guest_mem_direct_admit(&env, 0x10ff0, 32, false, 8));
    /* Degenerate spans are never admitted. */
    CHECK(!lata_guest_mem_direct_admit(&env, 0x2000, 0, false, 8));
    CHECK(!lata_guest_mem_direct_admit(&env, UINT64_MAX - 7, 16, false, 8));
    /* A fault window installed after translation vetoes the fast path. */
    CHECK(lata_guest_mem_add_fault_region(&env, 0x2000, 16, LATA_MEM_PERM_READ));
    CHECK(!lata_guest_mem_direct_admit(&env, 0x2000, 16, false, 8));
    lata_guest_mem_clear_fault_regions(&env);
    CHECK(lata_guest_mem_direct_admit(&env, 0x2000, 16, false, 8));
    lata_set_guest_mem_ops(NULL);
    CHECK(!lata_guest_mem_direct_available());

    if (failures != 0) {
        printf("LATA context: %u/%u checks failed\n", failures, checks);
        return 1;
    }
    printf("LATA context: %u checks passed\n", checks);
    return 0;
}
