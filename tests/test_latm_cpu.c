#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "internal.h"
#include "latm-cpu.h"
#include "latm-helpers.h"
#include "latm-platform.h"

void do_raise_exception(CPUMIPSState *env, uint32_t exception, uintptr_t pc)
{ (void)env; (void)exception; (void)pc; }
void do_raise_exception_err(CPUMIPSState *env, uint32_t exception,
                            int error_code, uintptr_t pc)
{ (void)env; (void)exception; (void)error_code; (void)pc; }
void latm_jmp_cache_flush(void *cpu) { (void)cpu; }
static const LatmPlatform *dummy_platform;
void latm_platform_set(const LatmPlatform *p) { dummy_platform = p; }
const LatmPlatform *latm_platform_get(void) { return dummy_platform; }

static int checks, fails;
static void check(uint64_t got, uint64_t want, const char *what)
{
    checks++;
    if (got != want) {
        fails++;
        printf("FAIL %s got=%llx want=%llx\n", what,
               (unsigned long long)got, (unsigned long long)want);
    }
}

int main(void)
{
    CPUMIPSState env;
    CPUMIPSTLBContext tlb;

    memset(&env, 0, sizeof(env));
    env.SEGBITS = 42;
    env.PABITS = 36;
    latm_cpu_profile_apply(&env, &latm_cpu_3a4000);
    check(env.CP0_PRid, 0x0014c004, "3A4000 PRId");
    check((env.CP0_Config1 >> CP0C1_MMU) & 0x3f, 5, "Config1 MMU log2");
    check((env.CP0_Config6 >> CP0C6_VTLBONLY) & 1, 1, "VTLB-only");
    check(latm_helper_cp0_read(&env, 22, 0), 0, "Diag read");
    env.CP0_Diag = 0x12345678u;
    check(latm_helper_cp0_read(&env, 22, 0), 0x12345678u, "Diag write path");
    latm_cpu_profile_tlb(&latm_cpu_3a4000, &env, &tlb);
    check(tlb.nb_tlb, 64, "64 VTLB entries");

    latm_cpu_profile_apply(&env, &latm_cpu_3a3000);
    check(env.CP0_PRid, 0x0014c000, "3A3000 PRId");
    check((latm_cpu_3a3000.features & LATM_CPU_FEAT_HT_IRQ) != 0, 1,
          "3A3000 HT IRQ");
    check((latm_cpu_3a3000.features & LATM_CPU_FEAT_EXTIOI) == 0, 1,
          "3A3000 no EXTIOI");

    printf("latm-cpu: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
