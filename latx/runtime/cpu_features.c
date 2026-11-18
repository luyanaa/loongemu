/*
 * Runtime ISA probe for the bare translator.  The LoongArch CPUCFG.2 word
 * reports the vector extensions; x86 feature exposure is derived from them:
 *   LASX  -> x86-64-v3 style: AVX2/AVX/FMA/BMI (+ XSAVE/OSXSAVE)
 *   LSX   -> x86-64-v2 baseline: SSE4.2/POPCNT, no AVX
 *   none  -> x86-64 baseline: SSE2
 */
#include "qemu/osdep.h"

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

void latx_cpu_set_vector_features(int lsx, int lasx)
{
    forced_lsx = lsx;
    forced_lasx = lasx;
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
