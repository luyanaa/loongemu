#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "internal.h"
#include "latm-helpers.h"
#include "latm-platform.h"
#include "latm-llsc.h"

static int checks, fails, raised;
static uint8_t backing[0x4000] __attribute__((aligned(16)));
static const LatmPlatform *platform;

void latm_platform_set(const LatmPlatform *p) { platform = p; }
const LatmPlatform *latm_platform_get(void) { return platform; }
void do_raise_exception(CPUMIPSState *env, uint32_t exception, uintptr_t pc)
{ (void)env; (void)exception; (void)pc; raised++; }
void do_raise_exception_err(CPUMIPSState *env, uint32_t exception,
                            int error_code, uintptr_t pc)
{ (void)env; (void)exception; (void)error_code; (void)pc; raised++; }
void cpu_loop_exit(struct CPUState *cs) { (void)cs; }
void latm_tb_flush_range(void *cpu, uint64_t start, uint64_t end)
{ (void)cpu; (void)start; (void)end; }
uint64_t cpu_mips_translate_address(CPUMIPSState *env, uint64_t addr,
                                    MMUAccessType type, uintptr_t ra)
{ (void)env; (void)type; (void)ra; return addr; }
uint32_t cpu_ldl_mmuidx_ra(CPUMIPSState *e, uint64_t a, int m, uintptr_t r)
{ (void)e;(void)a;(void)m;(void)r; return 0; }
uint64_t cpu_ldq_mmuidx_ra(CPUMIPSState *e, uint64_t a, int m, uintptr_t r)
{ (void)e;(void)a;(void)m;(void)r; return 0; }

static int backing_offset(uint64_t gpa, unsigned size, unsigned *off)
{
    if (gpa < 0x1000 || gpa - 0x1000 > sizeof(backing) ||
        size > sizeof(backing) - (gpa - 0x1000)) return 0;
    *off = (unsigned)(gpa - 0x1000);
    return 1;
}
static int atomic_ll(CPUMIPSState *e, uint64_t gpa, unsigned size,
                     uint64_t *value)
{
    unsigned off;
    (void)e;
    if (!value || !backing_offset(gpa, size, &off)) return LATM_MEM_FAULT;
    *value = 0;
    for (unsigned i = 0; i < size; i++) *value |= (uint64_t)backing[off+i] << (8*i);
    return LATM_MEM_OK;
}
static int atomic_sc(CPUMIPSState *e, uint64_t gpa, unsigned size,
                     uint64_t expected, uint64_t desired, int *ok)
{
    unsigned off;
    (void)e;
    if (!ok || !backing_offset(gpa, size, &off)) return LATM_MEM_FAULT;
    *ok = 0;
    if (size == 4) {
        uint32_t old = (uint32_t)expected;
        *ok = __atomic_compare_exchange_n((uint32_t *)(backing+off), &old,
                                          (uint32_t)desired, false,
                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    } else {
        uint64_t old = expected;
        *ok = __atomic_compare_exchange_n((uint64_t *)(backing+off), &old,
                                          desired, false,
                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    return LATM_MEM_OK;
}
static const LatmPlatform p = { .atomic_ll = atomic_ll, .atomic_sc = atomic_sc };

static void check(uint64_t got, uint64_t want, const char *what)
{
    checks++;
    if (got != want) {
        fails++;
        printf("FAIL %s got=%llx want=%llx\n", what,
               (unsigned long long)got, (unsigned long long)want);
    }
}

/*
 * Cross-core reservation coherence. The per-hart flag on CPUMIPSState cannot see
 * another hart's store, so the board keeps a shared table: a store by any hart
 * to a reserved range drops every overlapping reservation, which is what stops
 * two harts from both entering a lock built from LL/SC.
 */
static void test_cross_core_reservations(void)
{
    LatmLlscTable t;

    latm_llsc_init(&t);
    latm_llsc_reserve(&t, 1, 0x2000, 8);
    check(latm_llsc_valid(&t, 1, 0x2000, 8), 1, "hart 1 holds its reservation");

    /* A store from hart 0 to the reserved word drops hart 1's reservation. */
    latm_llsc_store(&t, 0, 0x2000, 8);
    check(latm_llsc_valid(&t, 1, 0x2000, 8), 0,
          "hart 0's store to the reserved word clears hart 1");

    /* A store elsewhere does not. */
    latm_llsc_reserve(&t, 1, 0x2000, 8);
    latm_llsc_store(&t, 0, 0x3000, 8);
    check(latm_llsc_valid(&t, 1, 0x2000, 8), 1,
          "hart 0's store elsewhere leaves hart 1 alone");

    /* Overlap, not equality: an 8-byte store over a 4-byte reservation counts,
     * and so does a store that starts inside the reserved range. */
    latm_llsc_reserve(&t, 2, 0x2004, 4);
    latm_llsc_store(&t, 0, 0x2000, 8);
    check(latm_llsc_valid(&t, 2, 0x2004, 4), 0,
          "an overlapping wide store clears a narrow reservation");

    latm_llsc_reserve(&t, 2, 0x2000, 8);
    latm_llsc_store(&t, 1, 0x2004, 4);
    check(latm_llsc_valid(&t, 2, 0x2000, 8), 0,
          "a store inside the reserved range clears it");

    /* Adjacent ranges must not interfere - otherwise every store near a
     * reservation would break it and the lock would livelock. */
    latm_llsc_reserve(&t, 3, 0x2000, 8);
    latm_llsc_store(&t, 0, 0x2008, 8);
    check(latm_llsc_valid(&t, 3, 0x2000, 8), 1,
          "a store directly after the reservation does not clear it");
    latm_llsc_store(&t, 0, 0x1ff8, 8);
    check(latm_llsc_valid(&t, 3, 0x2000, 8), 1,
          "a store directly before the reservation does not clear it");

    /* The issuer's own plain store must break its own reservation too: an SC
     * after a store in the same sequence has to fail. */
    latm_llsc_reserve(&t, 0, 0x2000, 8);
    latm_llsc_store(&t, 0, 0x2000, 8);
    check(latm_llsc_valid(&t, 0, 0x2000, 8), 0,
          "a hart's own store clears its own reservation");

    /* Explicit clear, as an SC does when it completes. */
    latm_llsc_reserve(&t, 0, 0x2000, 8);
    latm_llsc_clear(&t, 0);
    check(latm_llsc_valid(&t, 0, 0x2000, 8), 0, "an SC clears the reservation");

    /* A reservation only answers for its own address and width. */
    latm_llsc_reserve(&t, 0, 0x2000, 8);
    check(latm_llsc_valid(&t, 0, 0x2004, 8), 0,
          "a reservation does not match another address");
    check(latm_llsc_valid(&t, 0, 0x2000, 4), 0,
          "a reservation does not match another width");
}

int main(void)
{
    CPUMIPSState env;
    uint64_t v;

    memset(&env, 0, sizeof(env));
    memset(backing, 0, sizeof(backing));
    latm_platform_set(&p);
    *(uint32_t *)(backing + 0x20) = 0x11223344u;
    v = latm_helper_ll(&env, MIPS_MEM_LL, 0x1020);
    check(v, 0x11223344u, "LL GPA value");
    check(env.llvalid, 1, "LL reservation valid");
    check(env.llpaddr, 0x1020, "LL GPA reservation");
    check(latm_helper_sc(&env, MIPS_MEM_SC, 0x1020, 0xaabbccdd),
          1, "SC success");
    check(*(uint32_t *)(backing + 0x20), 0xaabbccdd, "SC backing update");
    check(env.llvalid, 0, "SC clears reservation");

    v = latm_helper_ll(&env, MIPS_MEM_LL, 0x1020);
    check(v, 0xffffffffaabbccddull, "LL after SC");
    check(latm_helper_sc(&env, MIPS_MEM_SC, 0x1024, 1), 0,
          "SC wrong address");
    check(*(uint32_t *)(backing + 0x20), 0xaabbccdd, "wrong SC no store");

    raised = 0;
    (void)latm_helper_ll(&env, MIPS_MEM_LL, 0x5000);
    check(raised, 1, "LL outside RAM rejected");
    raised = 0;
    (void)latm_helper_sc(&env, MIPS_MEM_SC, 0x5000, 1);
    check(raised, 0, "SC without reservation no fault");

    test_cross_core_reservations();
    printf("latm-llsc: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
