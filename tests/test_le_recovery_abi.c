/*
 * The shared recovery ABI: one row schema for four translators.
 *
 * The point of this test is not the predicates themselves but the properties
 * they encode, because those are what the ports would otherwise each get
 * slightly wrong:
 *
 *   1. every field the contract requires is present in the row;
 *   2. a row from a reclaimed arena never projects a fault (generation check);
 *   3. a row that binds registers it cannot replay never projects, and neither
 *      does a row whose flags are still deferred;
 *   4. a native-internal fault never projects, whatever else is true.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "le-recovery.h"

static int checks;
static int fails;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

static LeRecoverySpan row(uint32_t generation, uint8_t flags_state,
                          uint8_t bind_cnt)
{
    LeRecoverySpan span;

    memset(&span, 0, sizeof(span));
    span.guest_pc = 0x4000;
    span.native_start = 16;
    span.native_end = 32;
    span.site = LE_RECOV_GUEST_ACCESS;
    span.phase = LE_PHASE_AFTER_FALLTHROUGH;
    span.tb_generation = generation;
    span.flags_state = flags_state;
    span.bind_cnt = bind_cnt;
    return span;
}

int main(void)
{
    const uint32_t gen = 7;

    /* 1. Contract fields exist and the row is what a port can name. */
    {
        LeRecoverySpan span = row(gen, LE_FLAGS_NONE, 0);

        check(span.guest_pc == 0x4000, "guest_pc is carried");
        check(span.native_end > span.native_start, "host_pc_range is carried");
        check(span.phase == LE_PHASE_AFTER_FALLTHROUGH, "commit_state is carried");
        check(span.flags_state == LE_FLAGS_NONE, "flags_state is carried");
        check(span.tb_generation == gen, "tb_generation is carried");
        check(span.aux == 0, "arch_payload slot exists");
        check(sizeof(span) == sizeof(LeRecoverySpan), "row type is the shared one");
    }

    /* 2. A live row projects; the same row after a reclaim does not. */
    {
        LeRecoverySpan span = row(gen, LE_FLAGS_NONE, 0);

        check(le_recovery_can_project(&span, gen, false),
              "clean live row projects without a snapshot");
        check(!le_recovery_can_project(&span, gen + 1, true),
              "stale generation never projects");
        check(!le_recovery_span_is_live(&span, gen + 1),
              "generation predicate agrees");
    }

    /* 3. Bindings and deferred flags both require a snapshot. */
    {
        LeRecoverySpan binds = row(gen, LE_FLAGS_NONE, 2);
        LeRecoverySpan overflow = row(gen, LE_FLAGS_NONE, 0);
        LeRecoverySpan flags = row(gen, LE_FLAGS_DEFERRED, 0);

        overflow.bind_flags = LE_BIND_OVERFLOW;
        check(!le_recovery_can_project(&binds, gen, false),
              "bound row without a snapshot fails closed");
        check(le_recovery_can_project(&binds, gen, true),
              "bound row with a snapshot projects");
        check(!le_recovery_can_project(&overflow, gen, false),
              "truncated bindings fail closed");
        check(le_recovery_flags_need_snapshot(&flags),
              "deferred flags ask for a snapshot");
        check(!le_recovery_can_project(&flags, gen, false),
              "deferred flags without a snapshot fail closed");
        check(le_recovery_can_project(&flags, gen, true),
              "deferred flags with a snapshot project");
        check(!le_recovery_span_binds(&flags), "flag row binds no registers");
    }

    /* 4. A host-internal fault is never projected, even when everything else
     *    would allow it. */
    {
        LeRecoverySpan span = row(gen, LE_FLAGS_MATERIALIZED, 0);

        span.site = LE_RECOV_NATIVE_INTERNAL;
        check(!le_recovery_can_project(&span, gen, true),
              "native-internal never projects");
        span.site = LE_RECOV_HELPER;
        check(le_recovery_can_project(&span, gen, true),
              "helper site projects when generations match");
    }

    /* A zeroed row must not accidentally look live: generation 0 is the value
     * zeroed storage has, so callers pass a non-zero current generation. */
    {
        LeRecoverySpan zero;
        memset(&zero, 0, sizeof(zero));
        check(!le_recovery_can_project(&zero, 1, false),
              "uninitialised row does not project");
    }

    printf("le-recovery-abi: %d checks, %d fails\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
