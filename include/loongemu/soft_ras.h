/*
 * Software Return Address Stack (SoftRAS) for indirect RET acceleration.
 *
 * Designed as PLV0-only per-vCPU state (never mapped into PLV3) so translated
 * LoongArch64 blocks can speculatively chain CALL/RET pairs after validation
 * against guest (guest_pc, cr3, cpl, mode_flags) and live TB cache lookup.
 */

#ifndef LOONGEMU_SOFT_RAS_H
#define LOONGEMU_SOFT_RAS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOFT_RAS_DEPTH 32u

typedef struct SoftRasEntry {
    uint64_t guest_pc;
    uint64_t native_pc;
    uint64_t cr3;
    uint8_t  cpl;
    uint8_t  mode_flags;
    uint8_t  valid;
} SoftRasEntry;

typedef struct SoftRas {
    SoftRasEntry entries[SOFT_RAS_DEPTH];
    uint32_t top;
    uint32_t count;
    uint64_t pushes;
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t flushes;
    uint64_t backfills;
} SoftRas;

void     soft_ras_init(SoftRas *ras);
void     soft_ras_flush(SoftRas *ras);
void     soft_ras_push(SoftRas *ras, uint64_t guest_pc, uint64_t native_pc,
                       uint64_t cr3, uint8_t cpl, uint8_t mode_flags);
uint64_t soft_ras_lookup(SoftRas *ras, uint64_t guest_pc, uint64_t cr3,
                         uint8_t cpl, uint8_t mode_flags);
uint32_t soft_ras_backfill(SoftRas *ras, uint64_t guest_pc, uint64_t cr3,
                           uint8_t cpl, uint8_t mode_flags,
                           uint64_t native_pc);
void     soft_ras_invalidate_range(SoftRas *ras, uint64_t start_pc,
                                   uint64_t end_pc);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_SOFT_RAS_H */
