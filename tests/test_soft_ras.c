#include <stdio.h>
#include <stdint.h>

#include "loongemu/soft_ras.h"

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond, msg) do {                                      \
    g_checks++;                                                    \
    if (!(cond)) {                                                 \
        g_fails++;                                                 \
        fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);  \
    }                                                              \
} while (0)

int main(void)
{
    SoftRas ras;
    const uint64_t cr3_a = UINT64_C(0x100000);
    const uint64_t cr3_b = UINT64_C(0x200000);

    soft_ras_init(&ras);
    CHECK(ras.top == 0 && ras.count == 0 && ras.pushes == 0,
          "init zeroes state and stats");

    /* 1. Nested push/pop LIFO order. */
    soft_ras_push(&ras, 0x401000, 0x90001000, cr3_a, 0, 1);
    soft_ras_push(&ras, 0x402000, 0x90002000, cr3_a, 0, 1);
    soft_ras_push(&ras, 0x403000, 0x90003000, cr3_a, 0, 1);
    CHECK(ras.count == 3 && ras.pushes == 3, "three pushes tracked");
    CHECK(soft_ras_lookup(&ras, 0x403000, cr3_a, 0, 1) == 0x90003000,
          "LIFO pop 1 hits top");
    CHECK(soft_ras_lookup(&ras, 0x402000, cr3_a, 0, 1) == 0x90002000,
          "LIFO pop 2 hits middle");
    CHECK(soft_ras_lookup(&ras, 0x401000, cr3_a, 0, 1) == 0x90001000,
          "LIFO pop 3 hits bottom");
    CHECK(ras.count == 0 && ras.hits == 3 && ras.misses == 0,
          "stack empty after matched pops");
    CHECK(soft_ras_lookup(&ras, 0x401000, cr3_a, 0, 1) == 0,
          "empty lookup returns 0");
    CHECK(ras.misses == 1, "empty lookup increments misses");

    /* 2. Context isolation (CR3, CPL, mode_flags) & non-destructive miss. */
    soft_ras_push(&ras, 0x501000, 0x90005000, cr3_a, 0, 1);
    CHECK(soft_ras_lookup(&ras, 0x501000, cr3_b, 0, 1) == 0,
          "CR3 mismatch rejected");
    CHECK(soft_ras_lookup(&ras, 0x501000, cr3_a, 3, 1) == 0,
          "CPL mismatch rejected");
    CHECK(soft_ras_lookup(&ras, 0x501000, cr3_a, 0, 3) == 0,
          "mode_flags mismatch rejected");
    CHECK(ras.count == 1, "mismatches preserve valid entry on stack");
    CHECK(soft_ras_lookup(&ras, 0x501000, cr3_a, 0, 1) == 0x90005000,
          "matching lookup still hits after mismatches");

    /* 3. Backfill of entries pushed with native_pc == 0 (even duplicate PCs). */
    soft_ras_push(&ras, 0x601000, 0, cr3_a, 0, 1);
    soft_ras_push(&ras, 0x602000, 0x90006020, cr3_a, 0, 1);
    soft_ras_push(&ras, 0x601000, 0, cr3_a, 0, 1);
    CHECK(soft_ras_backfill(&ras, 0x601000, cr3_b, 0, 1, 0x9999) == 0,
          "backfill ignores different CR3");
    CHECK(soft_ras_backfill(&ras, 0x601000, cr3_a, 0, 1, 0x90006010) == 2,
          "backfill fills both pending frames");
    CHECK(ras.backfills == 2, "backfill stat incremented");
    CHECK(soft_ras_lookup(&ras, 0x601000, cr3_a, 0, 1) == 0x90006010 &&
          soft_ras_lookup(&ras, 0x602000, cr3_a, 0, 1) == 0x90006020 &&
          soft_ras_lookup(&ras, 0x601000, cr3_a, 0, 1) == 0x90006010,
          "backfilled frames pop cleanly in order");

    /* 4. Unfilled entry pops cleanly as miss without wedging deeper frame. */
    soft_ras_push(&ras, 0x604000, 0x90006040, cr3_a, 0, 1);
    soft_ras_push(&ras, 0x605000, 0, cr3_a, 0, 1);
    CHECK(soft_ras_lookup(&ras, 0x605000, cr3_a, 0, 1) == 0,
          "unfilled frame returns 0 on lookup");
    CHECK(soft_ras_lookup(&ras, 0x604000, cr3_a, 0, 1) == 0x90006040,
          "unfilled top was popped so deeper frame hits");

    /* 5. One-frame skew unwind (tail-call / single skipped frame). */
    soft_ras_push(&ras, 0x701000, 0x90007010, cr3_a, 0, 1);
    soft_ras_push(&ras, 0x702000, 0x90007020, cr3_a, 0, 1); /* stale */
    CHECK(soft_ras_lookup(&ras, 0x701000, cr3_a, 0, 1) == 0x90007010,
          "1-frame skew lookup matches depth 1 and unwinds");
    CHECK(ras.count == 0, "unwound count reaches 0");

    /* 6. Overflow wraparound (>32 pushes overwrites oldest, newest 32 survive). */
    for (uint64_t i = 0; i < 40; i++) {
        soft_ras_push(&ras, 0x800000 + i * 16, 0x90080000 + i * 16,
                      cr3_a, 0, 1);
    }
    CHECK(ras.count == SOFT_RAS_DEPTH, "overflow caps count at SOFT_RAS_DEPTH");
    for (uint64_t i = 39; i >= 8; i--) {
        uint64_t expected = 0x90080000 + i * 16;
        uint64_t got = soft_ras_lookup(&ras, 0x800000 + i * 16, cr3_a, 0, 1);
        CHECK(got == expected, "surviving newest 32 frames pop cleanly");
    }
    CHECK(ras.count == 0, "all 32 newest frames popped");

    /* 7. Range invalidation clears affected entry but preserves others. */
    soft_ras_push(&ras, 0x900100, 0x90090100, cr3_a, 0, 1);
    soft_ras_push(&ras, 0x901000, 0x90091000, cr3_a, 0, 1);
    soft_ras_push(&ras, 0x902000, 0x90092000, cr3_a, 0, 1);
    soft_ras_invalidate_range(&ras, 0x901000, 0x901800);
    CHECK(soft_ras_lookup(&ras, 0x902000, cr3_a, 0, 1) == 0x90092000,
          "non-overlapping upper frame intact after range invalidate");
    CHECK(soft_ras_lookup(&ras, 0x901000, cr3_a, 0, 1) == 0,
          "invalidated middle frame misses");
    CHECK(soft_ras_lookup(&ras, 0x900100, cr3_a, 0, 1) == 0x90090100,
          "1-frame skew skips invalidated middle frame cleanly");

    /* 8. Flush preserves cumulative stats and resets active stack. */
    soft_ras_push(&ras, 0xa00000, 0x900a0000, cr3_a, 0, 1);
    soft_ras_flush(&ras);
    CHECK(ras.count == 0 && ras.top == 0 && ras.flushes == 1,
          "flush resets stack and increments flushes");
    CHECK(ras.pushes > 0 && ras.hits > 0 && ras.misses > 0,
          "flush preserves cumulative telemetry counters");
    CHECK(soft_ras_lookup(&ras, 0xa00000, cr3_a, 0, 1) == 0,
          "flushed entry no longer matches");

    printf("test_soft_ras: checks=%d fails=%d\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
