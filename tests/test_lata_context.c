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
    .direct_range_stable = NULL,
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
        assert(!lata_guest_mem_direct_range_stable(&env, va, 4, false));
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

int main(void)
{
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

    if (failures != 0) {
        printf("LATA context: %u/%u checks failed\n", failures, checks);
        return 1;
    }
    printf("LATA context: %u checks passed\n", checks);
    return 0;
}
