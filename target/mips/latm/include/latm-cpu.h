#ifndef LATM_CPU_H
#define LATM_CPU_H

#include <stdint.h>
#include "cpu.h"

/* Loongson 3A CPU feature bits used by the LATM front-end. */
#define LATM_CPU_FEAT_LEXT      (UINT64_C(1) << 0)
#define LATM_CPU_FEAT_LEXT2     (UINT64_C(1) << 1)
#define LATM_CPU_FEAT_IOCSR     (UINT64_C(1) << 2)
#define LATM_CPU_FEAT_AMO       (UINT64_C(1) << 3)
#define LATM_CPU_FEAT_CRC       (UINT64_C(1) << 4)
#define LATM_CPU_FEAT_MMI       (UINT64_C(1) << 5)
#define LATM_CPU_FEAT_LSX       (UINT64_C(1) << 6)
#define LATM_CPU_FEAT_LASX      (UINT64_C(1) << 7)
#define LATM_CPU_FEAT_EXTIOI    (UINT64_C(1) << 8)
#define LATM_CPU_FEAT_HT_IRQ    (UINT64_C(1) << 9)

typedef struct LatmCpuProfile {
    const char *name;
    uint32_t prid;
    uint32_t config[7];
    uint32_t diag;
    uint16_t vtlb_entries;
    uint8_t ftlb;
    uint64_t features;
} LatmCpuProfile;

extern const LatmCpuProfile latm_cpu_3a3000;
extern const LatmCpuProfile latm_cpu_3a4000;

/* Apply architectural CP0 identification/config defaults.  TLB allocation is
 * intentionally separate so the embedder can choose its TLB context. */
void latm_cpu_profile_apply(CPUMIPSState *env, const LatmCpuProfile *profile);

/* Set up the software TLB count and Config1.MMU field for a profile. */
void latm_cpu_profile_tlb(const LatmCpuProfile *profile,
                          CPUMIPSState *env, CPUMIPSTLBContext *tlb);

#endif
