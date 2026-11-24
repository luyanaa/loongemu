#ifndef LATA_RECOVERY_H
#define LATA_RECOVERY_H

#include <stdint.h>
#include <stdbool.h>

/*
 * C2 precise recovery types. Kept CPU-agnostic (no target/arm/cpu.h, no
 * env.h) so the bare ISA test -- which cannot include lata.h -- can drive
 * resolve/apply against a fabricated host register file.
 */

#define LATA_TB_MAP_SPANS 512
/* Bounded binding pool per map: only env-resident guest GPRs (arm_la_map<0)
 * can hold a deferred value in a host temp, and the itemp pool caps the live
 * set at IR2_ITEMP_MAX anyway. */
#define LATA_TB_RECOVERY_BINDS 512
#define LATA_RECOVERY_MAX_BINDS_PER_SPAN 8

#include "le-recovery.h"

typedef LeRecoverySiteKind LataRecoverySite;
#define LATA_RECOV_GUEST_ACCESS    LE_RECOV_GUEST_ACCESS
#define LATA_RECOV_GUEST_CHECK     LE_RECOV_GUEST_CHECK
#define LATA_RECOV_HELPER          LE_RECOV_HELPER
#define LATA_RECOV_NATIVE_INTERNAL LE_RECOV_NATIVE_INTERNAL

typedef LeRecoveryBinding LataRecoveryBinding;
/* aux carries the guest insn raw_len (bytes). */
typedef LeRecoverySpan    LataTbInsnSpan;
#define lata_span_raw_len(s)       ((s)->aux)
#define lata_span_set_raw_len(s,v) ((s)->aux = (v))

typedef struct LataTbInsnMap {
    uintptr_t code_base;
    uint32_t code_size;
    uint32_t count;
    uint32_t generation;
    uint32_t bind_pool_used;
    bool valid;
    struct LataTbInsnMap *next;
    LataTbInsnSpan spans[LATA_TB_MAP_SPANS];
    LataRecoveryBinding binds[LATA_TB_RECOVERY_BINDS];
} LataTbInsnMap;

/* Env is the AArch64 compat state (xregs/pc). Forwarded so this header does
 * not need target/arm/cpu.h. */
struct CPUARMState;

void lata_tb_insn_map_begin(void *code, uint32_t code_size);
void lata_tb_insn_map_add(uint64_t guest_pc, uint32_t native_start,
                          uint32_t native_end, uint32_t raw_len,
                          uint8_t site, uint32_t retired_before,
                          const LataRecoveryBinding *binds, uint8_t bind_cnt);
const LataTbInsnSpan *lata_tb_recovery_resolve(uintptr_t host_pc,
                                               const LataTbInsnMap **map_out);
bool lata_tb_recovery_apply(const LataTbInsnSpan *span,
                            const LataTbInsnMap *map,
                            const uint64_t host_gprs[32],
                            const void *host_vregs,
                            struct CPUARMState *env);

#endif /* LATA_RECOVERY_H */
