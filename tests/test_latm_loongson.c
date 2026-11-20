#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "latm-helpers.h"
#include "ir1.h"
#include "latm-platform.h"

static const LatmPlatform *dummy_platform;
void latm_platform_set(const LatmPlatform *p) { dummy_platform = p; }
const LatmPlatform *latm_platform_get(void) { return dummy_platform; }
uint64_t latm_helper_load(CPUMIPSState *env, uint32_t op, uint64_t addr)
{ (void)env; (void)op; (void)addr; return 0; }
void latm_helper_store(CPUMIPSState *env, uint32_t op, uint64_t addr,
                       uint64_t val)
{ (void)env; (void)op; (void)addr; (void)val; }

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
    check(latm_helper_loongson_arith(NULL, MIPS_LG_MULT, 7, 6), 42, "gsmult");
    check(latm_helper_loongson_arith(NULL, MIPS_LG_DIV, 20, 6), 3, "gsdiv");
    check(latm_helper_loongson_arith(NULL, MIPS_LG_MOD, 20, 6), 2, "gsmod");
    check(latm_helper_loongson_arith(NULL, MIPS_LG_DIVU, 0xffffffffu, 2),
          0x7fffffffu, "gsdivu");
    check(latm_helper_loongson_arith(NULL, MIPS_LG_DIV, 1, 0), 0,
          "gsdiv zero");
    check(latm_helper_loongson_arith(NULL, MIPS_LG_DDIV, 1ull << 40, 1 << 10),
          1ull << 30, "gsddiv");
    check(latm_helper_loongson_arith(NULL, MIPS_LG_LE, 3, 3), 1, "gsle");
    check(latm_helper_loongson_arith(NULL, MIPS_LG_GT, 3, 3), 0, "gsgt");

    check(latm_helper_crc(NULL, MIPS_CRC32B, 0x01, 0), 0x77073096u, "crc32b");
    CPUMIPSState cfg = { .CP0_PRid = 0x14c004 };
    check(latm_helper_cpucfg(&cfg, 0), 0x14c004, "cpucfg0");
    check(latm_helper_cpucfg(&cfg, 1) & (1u << CPUCFG1_FP), 1u << CPUCFG1_FP,
          "cpucfg1 FP");
    check(latm_helper_cpucfg(&cfg, 1) & (1u << CPUCFG1_MMI), 1u << CPUCFG1_MMI,
          "cpucfg1 MMI");
    check(latm_helper_cpucfg(&cfg, 2) & (1u << CPUCFG2_LEXT1),
          1u << CPUCFG2_LEXT1, "cpucfg2 LEXT1");
    check(latm_helper_cpucfg(&cfg, 2) & (1u << CPUCFG2_LSPW),
          1u << CPUCFG2_LSPW, "cpucfg2 LSPW");

    CPUMIPSState mem_env;
    memset(&mem_env, 0, sizeof(mem_env));
    mem_env.active_tc.gpr[0] = 0xdeadbeef;
    latm_helper_loongson_mem(&mem_env, MIPS_LM_GSLQ, 0, 0, 0, 0, 5);
    check(mem_env.active_tc.gpr[0], 0xdeadbeef, "gslq preserves $zero");
    latm_helper_loongson_mem(&mem_env, MIPS_LM_GSLWX, 0, 0, 0, 0, 0);
    check(mem_env.active_tc.gpr[0], 0xdeadbeef, "load preserves $zero");

    printf("latm-loongson: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
