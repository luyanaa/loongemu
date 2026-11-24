/*
 * Runtime ISA probe for the bare translator.  The LoongArch CPUCFG.2 word
 * reports the vector extensions, and the x86 feature level follows from them:
 *
 *   LASX  -> x86-64-v3: v2 plus AVX/AVX2/FMA/F16C/BMI1/BMI2/LZCNT/MOVBE
 *   LSX   -> x86-64-v2: SSE3/SSSE3/SSE4.1/SSE4.2/POPCNT/CMPXCHG16B/LAHF
 *   none  -> NOT USABLE.  x86-64 baseline mandates SSE and SSE2, and this
 *            translator implements SSE on LSX; without it even v1 cannot be
 *            honoured, so the engine refuses to start rather than exposing a
 *            CPU that cannot run the ABI it advertises.
 *
 * The old policy ("none -> SSE2 baseline") claimed SSE2 on hardware that has
 * no vector unit at all: every SSE instruction then faulted at run time, which
 * is worse than declining to start.
 */
#include "qemu/osdep.h"
#include "../compat/qemu/cpu-features.h"

#if defined(__loongarch__)
static uint32_t cpucfg2(void)
{
    uint64_t cfg = 0;
    uint64_t two = 2;

    __asm__ volatile("cpucfg %0, %1" : "=r"(cfg) : "r"(two));
    return (uint32_t)cfg;
}
#else
static uint32_t cpucfg2(void)
{
    return 0;
}
#endif

/* Test hook: a harness may force the feature level. */
static int forced_lsx = -1;
static int forced_lasx = -1;
static int forced_lbt = -1;

void latx_cpu_set_vector_features(int lsx, int lasx)
{
    forced_lsx = lsx;
    forced_lasx = lasx;
}

void latx_cpu_set_lbt_feature(int lbt)
{
    forced_lbt = lbt;
}

int latx_cpu_has_lbt(void)
{
    if (forced_lbt >= 0) {
        return forced_lbt;
    }
    return (cpucfg2() & (1u << 22)) != 0; /* CPUCFG.2[22] = LBT */
}

int latx_cpu_has_lsx(void)
{
    if (forced_lsx >= 0) {
        return forced_lsx;
    }
    return (cpucfg2() & (1u << 6)) != 0;   /* CPUCFG.2[6] = LSX  */
}

int latx_cpu_has_lasx(void)
{
    if (forced_lasx >= 0) {
        return forced_lasx;
    }
    return (cpucfg2() & (1u << 7)) != 0;   /* CPUCFG.2[7] = LASX */
}

/*
 * Which x86-64 microarchitecture level this host can honour.
 *
 * Returns 3 for v3, 2 for v2, and 0 when the host cannot run x86-64 at all
 * (no LSX, hence no SSE/SSE2, hence not even the v1 baseline).
 */
int latx_cpu_x86_level(void)
{
    if (!latx_cpu_has_lsx()) {
        return 0;
    }
    return latx_cpu_has_lasx() ? 3 : 2;
}

/*
 * Refusal hook.  The default has to be to stop: the alternative is to hand the
 * guest a CPU that advertises an ABI this host cannot execute.
 */
__attribute__((weak)) void latx_platform_refuse(const char *why)
{
    (void)why;
    for (;;) {
        __asm__ volatile("" ::: "memory");
    }
}
