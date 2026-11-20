#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "internal.h"
#include "latm-helpers.h"
#include "latm-platform.h"

void latm_jmp_cache_flush(void *cpu) { (void)cpu; }

int latm_tb_insn_map_resolve(void *trd, uintptr_t host_pc,
                             uint64_t *guest_pc, uint8_t *delay_slot)
{
    (void)trd; (void)host_pc; (void)guest_pc; (void)delay_slot;
    return 0;
}

static const LatmPlatform *active_platform;
void latm_platform_set(const LatmPlatform *p) { active_platform = p; }
const LatmPlatform *latm_platform_get(void) { return active_platform; }

static uint32_t fake_count;
static uint32_t fake_compare;
static int exits;
static int store_count_calls;

void cpu_loop_exit(struct CPUState *cs)
{
    (void)cs;
    exits++;
}

static uint32_t platform_count(CPUMIPSState *env, uint64_t hz)
{
    (void)env;
    (void)hz;
    return fake_count;
}

static void platform_store_count(CPUMIPSState *env, uint32_t count)
{
    (void)env;
    store_count_calls++;
    fake_count = count;
}

static void platform_compare(CPUMIPSState *env, uint32_t compare)
{
    (void)env;
    fake_compare = compare;
}

static const LatmPlatform platform = {
    .read_count = platform_count,
    .store_count = platform_store_count,
    .set_compare = platform_compare,
};

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

static void reset_env(CPUMIPSState *env)
{
    memset(env, 0, sizeof(*env));
    env->exception_base = 0x80000000u;
    env->CP0_IntCtl = 0xe0000000u; /* IPTI=7 -> IP7 */
    env->CP0_Status = (1u << CP0St_IE) | (1u << (CP0Ca_IP + 7));
    env->active_tc.PC = 0x1000;
    fake_count = 0;
    fake_compare = 0;
    exits = 0;
    store_count_calls = 0;
}

int main(void)
{
    CPUMIPSState env;
    const uint32_t ti = 1u << CP0Ca_TI;
    const uint32_t ip7 = 1u << (CP0Ca_IP + 7);

    latm_platform_set(&platform);

    reset_env(&env);
    fake_count = 10;
    check(latm_helper_cp0_read(&env, 9, 0), 10, "count read uses platform");
    check(store_count_calls, 0, "read does not store");

    latm_helper_cp0_write(&env, 11, 0, 20);
    check(fake_compare, 20, "compare callback");
    check(env.CP0_Compare, 20, "compare register");
    fake_count = 19;
    check(cpu_mips_get_count(&env), 19, "before compare");
    check(env.CP0_Cause & ti, 0, "TI not set before compare");
    check(env.CP0_Cause & ip7, 0, "IP7 not set before compare");

    fake_count = 21;
    check(cpu_mips_get_count(&env), 21, "at compare");
    check(env.CP0_Cause & ti, ti, "TI set at compare");
    check(env.CP0_Cause & ip7, ip7, "IP7 set at compare");
    latm_helper_cp0_write(&env, 11, 0, 40);
    check(env.CP0_Cause & (ti | ip7), 0, "compare write clears timer IRQ");

    reset_env(&env);
    latm_helper_cp0_write(&env, 11, 0, 25);
    fake_count = 30;
    latm_check_interrupts(&env);
    check(exits, 1, "interrupt delivered");
    check(env.CP0_Status & (1u << CP0St_EXL), 1u << CP0St_EXL, "EXL set");
    check(env.active_tc.PC, 0x80000180u, "generic interrupt vector");
    check((env.CP0_Cause >> 2) & 0x1f, 0, "ExcCode interrupt");
    check(env.CP0_EPC, 0x1000, "EPC interrupt PC");

    reset_env(&env);
    latm_helper_wait(&env);
    check(env.active_tc.CP0_TCHalt & 1, 1, "WAIT halts");
    latm_helper_cp0_write(&env, 11, 0, 5);
    fake_count = 8;
    latm_check_interrupts(&env);
    check(env.active_tc.CP0_TCHalt, 0, "WAIT wakes on interrupt");
    check(exits, 1, "WAIT interrupt delivered");

    reset_env(&env);
    latm_helper_cp0_write(&env, 9, 0, 500);
    check(store_count_calls, 1, "count write callback");
    check(fake_count, 500, "count write value");
    check(latm_helper_cp0_read(&env, 9, 0), 500, "count write read back");

    printf("latm-interrupts: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
