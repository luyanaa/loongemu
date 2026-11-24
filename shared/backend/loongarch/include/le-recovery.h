#ifndef LE_RECOVERY_H
#define LE_RECOVERY_H

/*
 * Shared host-fault -> guest-state recovery model (Stela block/recovery.rs
 * + retirement.rs lineage).
 *
 * A RecoverySite pins a *native* faultable operation to the guest state that
 * must exist if the host faults at that point: the span of host code that
 * shares the same recovery decision, the guest PC that owns it, the count of
 * guest insns fully retired before it, and the phase describing how much of
 * the faulting operation's architectural effect is already visible.
 *
 * Ports embed `LeRecoverySpan` as the span row of their per-TB insn map and
 * `LeRecoveryBinding` as the binding pool entries. The port keeps ownership:
 * begin/add/resolve stay per-port so span layout, lifetime and regcache
 * staging are unchanged; only the row schema is unified here.
 */
#include <stdbool.h>
#include <stdint.h>

/* Why a host fault at this span may legitimately project to the guest. */
typedef enum LeRecoverySiteKind {
    LE_RECOV_GUEST_ACCESS = 0, /* guest load/store/amo: fault projects to guest */
    LE_RECOV_GUEST_CHECK,      /* explicit guest trap (svc/ecall/brk/mret...) */
    LE_RECOV_HELPER,           /* helper call that may fault (reserved) */
    LE_RECOV_NATIVE_INTERNAL,  /* fault here = host bug; never project */
} LeRecoverySiteKind;

/* How much of the faulting operation's architectural effect is visible.
 * BEFORE_INSN:      nothing of this insn committed (single faultable access,
 *                   or the access precedes all other effects in the span).
 * AFTER_FALLTHROUGH: all earlier effects retired; only the faulting access
 *                   outstanding (multi-access insns, e.g. ldp 2nd access).
 * PARTIAL_EFFECT:   the access itself partially completed and the guest
 *                   tracks progress — RVV vstart, SVE first-fault element
 *                   index, MIPS delay-slot EPC/BD. `effect_id` names the
 *                   in-span progress indicator the port defined. */
typedef enum LeRecoveryPhase {
    LE_PHASE_BEFORE_INSN = 0,
    LE_PHASE_AFTER_FALLTHROUGH,
    LE_PHASE_PARTIAL_EFFECT,
} LeRecoveryPhase;

/* span->bind_flags */
#define LE_BIND_OVERFLOW 0x1u /* binding snapshot truncated; deny optimized
                                 code paths that rely on precise recovery. */

typedef struct LeRecoveryBinding {
    uint8_t  guest_reg; /* guest GPR or FPR index holding an uncommitted value */
    int8_t   temp;      /* host phys reg, or spill slot when is_spill=1 */
    uint8_t  is_fpr;    /* 1 = value lives in host_fprs[temp] */
    uint8_t  is_spill;  /* 1 = value spilled to host stack slot `temp` */
} LeRecoveryBinding;

typedef struct LeRecoverySpan {
    uint64_t guest_pc;
    uint32_t native_start;
    uint32_t native_end;
    /* Port-defined payload: LATR/LATA pack guest raw_len here; LATM packs
     * delay_slot in bit0 and a partial-effect id in the upper bits. */
    uint32_t aux;
    uint8_t  site;        /* LeRecoverySiteKind */
    uint8_t  phase;       /* LeRecoveryPhase */
    uint8_t  bind_cnt;
    uint8_t  bind_flags;  /* LE_BIND_* */
    uint16_t bind_first;
    uint16_t effect_id;   /* port-defined partial-effect progress id */
    uint16_t reserved;
    uint32_t retired_before;
} LeRecoverySpan;

#endif /* LE_RECOVERY_H */
