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

/*
 * How the guest's flag state exists at this span. Flags are guest state, so a
 * recovery decision is only precise if it knows whether the flags the guest
 * would observe are already in env or still live in a host register.
 */
typedef enum LeRecoveryFlagsState {
    LE_FLAGS_NONE = 0,         /* this insn does not write flags */
    LE_FLAGS_MATERIALIZED = 1, /* flags already canonical in env */
    LE_FLAGS_DEFERRED = 2,     /* flags live in a host reg: must be bound */
} LeRecoveryFlagsState;

/* span->bind_flags */
#define LE_BIND_OVERFLOW 0x1u /* binding snapshot truncated; deny optimized
                                 code paths that rely on precise recovery. */

typedef struct LeRecoveryBinding {
    uint8_t  guest_reg; /* guest GPR or FPR index holding an uncommitted value */
    int8_t   temp;      /* host phys reg, or spill slot when is_spill=1 */
    uint8_t  is_fpr;    /* 1 = value lives in host_fprs[temp] */
    uint8_t  is_spill;  /* 1 = value spilled to host stack slot `temp` */
} LeRecoveryBinding;

/*
 * The shared row. Every field the four translators need is named here once;
 * a port that needs more puts it in `aux`/`effect_id` and documents it in its
 * own header, which is what LATR/LATA/LATM already do.
 *
 *   guest_pc        precise exception PC
 *   native_start/end host PC range: attributing a native fault to guest code
 *   aux             arch_payload: MIPS delay-slot bit and BD target, RVV
 *                   vstart, AArch64 raw instruction length, ...
 *   effect_id       port-defined partial-effect progress id
 *   site/phase      commit_state: how much of the operation retired
 *   bind_*          register_bindings: uncommitted guest values and where
 *   flags_state     flag materialisation (see LeRecoveryFlagsState)
 *   tb_generation   the map generation this row belongs to, so a row can never
 *                   be used after the arena it described was reclaimed
 */
typedef struct LeRecoverySpan {
    uint64_t guest_pc;
    uint32_t native_start;
    uint32_t native_end;
    uint32_t aux;           /* arch_payload (port-defined) */
    uint32_t tb_generation; /* map generation this row was built for */
    uint32_t retired_before;
    uint16_t bind_first;
    uint16_t effect_id;
    uint8_t  site;        /* LeRecoverySiteKind */
    uint8_t  phase;       /* LeRecoveryPhase */
    uint8_t  bind_cnt;
    uint8_t  bind_flags;  /* LE_BIND_* */
    uint8_t  flags_state; /* LeRecoveryFlagsState */
    uint8_t  reserved[3];
} LeRecoverySpan;

/*
 * One decision function shared by every port, so "may this span project a host
 * fault onto the guest?" cannot drift between translators. `have_snapshot`
 * means the caller has a host register file to replay bindings from.
 *
 * The rule is fail-closed in the two ways that bite in practice: a row from a
 * reclaimed arena (generation mismatch) never projects, and a row that binds
 * registers it cannot replay never projects.
 */
static inline bool le_recovery_span_is_live(const LeRecoverySpan *span,
                                            uint32_t current_generation)
{
    return span != NULL && span->tb_generation == current_generation;
}

static inline bool le_recovery_span_binds(const LeRecoverySpan *span)
{
    return span != NULL &&
           (span->bind_cnt != 0 || (span->bind_flags & LE_BIND_OVERFLOW) != 0);
}

static inline bool le_recovery_flags_need_snapshot(const LeRecoverySpan *span)
{
    return span != NULL && span->flags_state == LE_FLAGS_DEFERRED;
}

static inline bool le_recovery_can_project(const LeRecoverySpan *span,
                                           uint32_t current_generation,
                                           bool have_snapshot)
{
    if (span == NULL || span->site == LE_RECOV_NATIVE_INTERNAL) {
        return false;
    }
    if (!le_recovery_span_is_live(span, current_generation)) {
        return false;
    }
    if (!have_snapshot && (le_recovery_span_binds(span) ||
                           le_recovery_flags_need_snapshot(span))) {
        return false;
    }
    return true;
}

#endif /* LE_RECOVERY_H */
