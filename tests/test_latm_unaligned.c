#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "internal.h"
#include "latm-helpers.h"
#include "latm-platform.h"

static int checks, fails, raised;
static uint8_t mem[0x1000] __attribute__((aligned(16)));
static const LatmPlatform *platform;

void latm_platform_set(const LatmPlatform *p) { platform = p; }
const LatmPlatform *latm_platform_get(void) { return platform; }
void do_raise_exception(CPUMIPSState *env, uint32_t exception, uintptr_t pc)
{ (void)env;(void)exception;(void)pc; raised++; }
void do_raise_exception_err(CPUMIPSState *env, uint32_t exception,
                            int error_code, uintptr_t pc)
{ (void)env;(void)exception;(void)error_code;(void)pc; raised++; }
void cpu_loop_exit(struct CPUState *cs) { (void)cs; }
static int rd(CPUMIPSState *e, uint64_t gpa, uint64_t *v, uint32_t size)
{
    (void)e;
    if (gpa + size > sizeof(mem)) return LATM_MEM_FAULT;
    *v = 0;
    for (uint32_t i=0;i<size;i++) *v |= (uint64_t)mem[gpa+i] << (8*i);
    return LATM_MEM_OK;
}
static int wr(CPUMIPSState *e, uint64_t gpa, uint64_t v, uint32_t size)
{
    (void)e;
    if (gpa + size > sizeof(mem)) return LATM_MEM_FAULT;
    for (uint32_t i=0;i<size;i++) mem[gpa+i]=(uint8_t)(v>>(8*i));
    return LATM_MEM_OK;
}
static const LatmPlatform p = { .mem_read = rd, .mem_write = wr };

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
    memset(&env, 0, sizeof(env));
    latm_platform_set(&p);
    *(uint64_t *)(mem + 0x100) = 0x1122334455667788ull;

    check(latm_helper_load(&env, MIPS_MEM_LD, 0x100),
          0x1122334455667788ull, "aligned LD");
    raised = 0;
    (void)latm_helper_load(&env, MIPS_MEM_LW, 0x101);
    check(raised, 1, "unaligned LW raises AdEL");
    raised = 0;
    (void)latm_helper_store(&env, MIPS_MEM_SW, 0x101, 0);
    check(raised, 1, "unaligned SW raises AdES");

    /* SWL/SWR pair writes the full little-endian word. */
    memset(mem + 0x200, 0, 8);
    latm_helper_unaligned_store(&env, MIPS_MEM_SWL, 0x203, 0x11223344);
    latm_helper_unaligned_store(&env, MIPS_MEM_SWR, 0x200, 0x11223344);
    check(latm_helper_load(&env, MIPS_MEM_LW, 0x200), 0x11223344,
          "SWL/SWR word");

    /* LWL/LWR pair reconstructs the same word. */
    check(latm_helper_unaligned_load(&env, MIPS_MEM_LWL, 0x203, 0),
          0x11223344, "LWL partial");
    check(latm_helper_unaligned_load(&env, MIPS_MEM_LWR, 0x200,
                                     0x11223344), 0x11223344,
          "LWR merges LWL");

    printf("latm-unaligned: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
