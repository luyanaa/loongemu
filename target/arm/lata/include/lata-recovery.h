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

typedef enum LataRecoverySite {
    LATA_RECOV_GUEST_ACCESS = 0, /* guest load/store/amo: fault projects to guest */
    LATA_RECOV_GUEST_CHECK,      /* explicit guest trap (svc/brk/hlt/sm*...) */
    LATA_RECOV_HELPER,           /* helper call that may fault (reserved) */
    LATA_RECOV_NATIVE_INTERNAL,  /* fault here = host bug; do not project */
} LataRecoverySite;

typedef struct LataRecoveryBinding {
    uint8_t guest_reg;   /* guest GPR index holding an uncommitted value */
    int8_t  temp;        /* host phys reg number (la_t0..la_t5 == 12..17) */
} LataRecoveryBinding;

typedef struct LataTbInsnSpan {
    uint64_t guest_pc;
    uint32_t native_start;
    uint32_t native_end;
    uint32_t raw_len;
    uint8_t  site;             /* LataRecoverySite */
    uint8_t  bind_cnt;
    uint16_t bind_first;
    uint32_t retired_before;
} LataTbInsnSpan;

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
