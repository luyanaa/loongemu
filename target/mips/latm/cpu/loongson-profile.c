#include "qemu/osdep.h"
#include "latm-cpu.h"
#include "latm-helpers.h"

#define CONFIG0_MIPS64  (1u << CP0C0_M)
#define CONFIG0_IMPL    ((uint32_t)0x14u << CP0C0_Impl)
#define CONFIG1_MMU64   (5u << CP0C1_MMU) /* log2(64) - 1 */

const LatmCpuProfile latm_cpu_3a4000 = {
    .name = "Loongson-3A4000",
    .prid = 0x0014c004u,
    .config = {
        CONFIG0_MIPS64 | CONFIG0_IMPL,
        CONFIG1_MMU64 | (1u << CP0C1_FP),
        0,
        0,
        0,
        0,
        (1u << CP0C6_VTLBONLY), /* no FTLB in the first software model */
    },
    .diag = 0,
    .vtlb_entries = 64,
    .ftlb = 0,
    .features = LATM_CPU_FEAT_LEXT | LATM_CPU_FEAT_LEXT2 |
                LATM_CPU_FEAT_IOCSR | LATM_CPU_FEAT_AMO |
                LATM_CPU_FEAT_CRC | LATM_CPU_FEAT_MMI |
                LATM_CPU_FEAT_LSX | LATM_CPU_FEAT_LASX |
                LATM_CPU_FEAT_EXTIOI,
};

const LatmCpuProfile latm_cpu_3a3000 = {
    .name = "Loongson-3A3000",
    .prid = 0x0014c000u,
    .config = {
        CONFIG0_MIPS64 | CONFIG0_IMPL,
        CONFIG1_MMU64 | (1u << CP0C1_FP),
        0,
        0,
        0,
        0,
        (1u << CP0C6_VTLBONLY),
    },
    .diag = 0,
    .vtlb_entries = 64,
    .ftlb = 0,
    .features = LATM_CPU_FEAT_LEXT | LATM_CPU_FEAT_LEXT2 |
                LATM_CPU_FEAT_MMI | LATM_CPU_FEAT_HT_IRQ,
};

void latm_cpu_profile_apply(CPUMIPSState *env, const LatmCpuProfile *profile)
{
    if (!env || !profile) {
        return;
    }
    env->CP0_PRid = (int32_t)profile->prid;
    env->CP0_Config0 = (int32_t)profile->config[0];
    env->CP0_Config1 = (int32_t)profile->config[1];
    env->CP0_Config2 = (int32_t)profile->config[2];
    env->CP0_Config3 = (int32_t)profile->config[3];
    env->CP0_Config4 = (int32_t)profile->config[4];
    env->CP0_Config5 = (int32_t)profile->config[5];
    env->CP0_Config6 = (int32_t)profile->config[6];
    env->CP0_Diag = profile->diag;
}

void latm_cpu_profile_tlb(const LatmCpuProfile *profile,
                          CPUMIPSState *env, CPUMIPSTLBContext *tlb)
{
    if (!profile || !env || !tlb) {
        return;
    }
    latm_tlb_init(env, tlb, profile->vtlb_entries);
    env->CP0_Config1 = (env->CP0_Config1 & ~(0x3fu << CP0C1_MMU)) |
                       ((int32_t)(5u << CP0C1_MMU));
    if (!profile->ftlb) {
        env->CP0_Config6 |= 1u << CP0C6_VTLBONLY;
    }
}
