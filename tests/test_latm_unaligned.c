#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "internal.h"
#include "latm-helpers.h"
#include "latm-platform.h"

static int checks, fails, raised, reads;
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
    reads++;
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
static int probe(CPUMIPSState *e, uint64_t gpa, uint32_t size)
{
    (void)e;
    return gpa + size <= sizeof(mem) ? LATM_MEM_OK : LATM_MEM_FAULT;
}
static const LatmPlatform p = { .mem_read = rd, .mem_write = wr };
static const LatmPlatform p_probe = {
    .mem_read = rd, .mem_write = wr, .mem_probe = probe,
};

static uint64_t read_le(const uint8_t *p, unsigned size)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static uint64_t ref_lx(CPUMIPSState *env, unsigned size, uint64_t addr,
                       uint64_t old, int left)
{
    unsigned mask = size - 1;
    unsigned pos = (unsigned)addr & mask;
    uint64_t word = read_le(mem + (addr & ~(uint64_t)mask), size);
    unsigned shift;
    uint64_t result;
    if (left) {
        shift = (((env->CP0_Config0 & (1u << CP0C0_BE)) != 0) ? pos :
                 (mask - pos)) * 8u;
        result = (old & ~((size == 4 ? UINT64_C(0xffffffff) : UINT64_MAX)
                          << shift)) | (word << shift);
    } else {
        shift = (((env->CP0_Config0 & (1u << CP0C0_BE)) != 0) ?
                 (mask - pos) : pos) * 8u;
        word >>= shift;
        result = (old & (~UINT64_C(1) << (size * 8u - 1u - shift))) | word;
    }
    return size == 4 ? (uint64_t)(int64_t)(int32_t)result : result;
}

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

    /* Differential LWL/LWR/LDL/LDR against the QEMU merge rule. */
    static const uint64_t olds[] = {
        0, UINT64_MAX, 0xdeadbeefcafef00dull, 0x80000000ull,
        0xffffffffull, 0x123456789abcdef0ull,
    };
    uint64_t words[] = { 0x11223344u, 0x89abcdefu, 0x01020304u };
    for (unsigned w = 0; w < 3; w++) {
        for (unsigned pos = 0; pos < 4; pos++) {
            for (unsigned o = 0; o < sizeof(olds) / sizeof(olds[0]); o++) {
                uint64_t base = 0x400 + w * 0x20;
                memset(mem + base, 0, 16);
                for (unsigned i = 0; i < 4; i++)
                    mem[base + i] = (uint8_t)(words[w] >> (8 * i));
                uint64_t a = base + pos;
                check(latm_helper_unaligned_load(&env, MIPS_MEM_LWL, a,
                                                 olds[o]),
                      ref_lx(&env, 4, a, olds[o], 1), "LWL differential");
                check(latm_helper_unaligned_load(&env, MIPS_MEM_LWR, a,
                                                 olds[o]),
                      ref_lx(&env, 4, a, olds[o], 0), "LWR differential");
                memset(mem + base, 0, 16);
                for (unsigned i = 0; i < 8; i++)
                    mem[base + i] = (uint8_t)(0xf0e1d2c3b4a59687ull >>
                                              (8 * i));
                check(latm_helper_unaligned_load(&env, MIPS_MEM_LDL, a,
                                                 olds[o]),
                      ref_lx(&env, 8, a, olds[o], 1), "LDL differential");
                check(latm_helper_unaligned_load(&env, MIPS_MEM_LDR, a,
                                                 olds[o]),
                      ref_lx(&env, 8, a, olds[o], 0), "LDR differential");
            }
        }
    }

    /* A probe-aware platform must not perform the extra MMIO byte read. */
    latm_platform_set(&p_probe);
    reads = 0;
    (void)latm_helper_unaligned_load(&env, MIPS_MEM_LWL, 0x400, 0);
    check(reads, 1, "unaligned load probes without extra read");
    latm_platform_set(&p);

    /* Explicit bit-31 boundary: LWR at an aligned address must replace it. */
    mem[0x600] = 0x44; mem[0x601] = 0x33; mem[0x602] = 0x22;
    mem[0x603] = 0x11;
    check(latm_helper_unaligned_load(&env, MIPS_MEM_LWR, 0x600,
                                     0x80000000ull), 0x11223344u,
          "LWR clears old bit 31");

    printf("latm-unaligned: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
