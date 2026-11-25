/*
 * Host tests for the fence-completion protocol (design:
 * docs/design/latr-smp-concurrency.md section 5).
 *
 * What this pins, without needing a guest or a second hart thread:
 *   - a shootdown is not complete until every registered hart acknowledges at
 *     a safe point, and the wait is bounded;
 *   - the sequence is monotonic, and a stale acknowledgement never satisfies a
 *     newer request;
 *   - exhaustion is reported (timeout counter + how far behind the worst hart
 *     was) instead of returning success;
 *   - a hart that is not registered does not block anyone.
 *
 * The threaded scheduler consumes exactly this API; a regression here is a
 * fence that retires while another hart still executes pre-fence code.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "latr-tb-cache.h"

static int checks;
static int failures;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what);            \
        }                                                                    \
    } while (0)

#define ARENA_SIZE 1024u

static uint8_t arena_a[ARENA_SIZE];
static uint8_t arena_b[ARENA_SIZE];
static LatrTbCache cache_a;
static LatrTbCache cache_b;

static void setup(void)
{
    memset(&cache_a, 0, sizeof(cache_a));
    memset(&cache_b, 0, sizeof(cache_b));
    latr_tb_cache_init(&cache_a, arena_a, sizeof(arena_a));
    latr_tb_cache_init(&cache_b, arena_b, sizeof(arena_b));
    CHECK(latr_tb_cache_register(&cache_a) == 1, "register cache A");
    CHECK(latr_tb_cache_register(&cache_b) == 1, "register cache B");
}

static void teardown(void)
{
    latr_tb_cache_unregister(&cache_a);
    latr_tb_cache_unregister(&cache_b);
}

int main(void)
{
    uint64_t seq;

    /* --- a request nobody has acknowledged cannot complete -------------- */
    setup();
    seq = latr_tb_fence_begin(0, 0);
    CHECK(seq == 1, "first request takes sequence 1");
    CHECK(latr_tb_fence_wait(seq, 4) == -1,
          "unacknowledged request times out, it does not report success");
    CHECK(latr_tb_fence_timeouts() == 1, "timeout is counted");
    CHECK(latr_tb_fence_last_missing() == 1,
          "diagnostics report how far the worst hart is behind");

    /* One hart acking is not enough. */
    latr_tb_fence_ack(&cache_a);
    CHECK(latr_tb_fence_wait(seq, 4) == -1, "one acknowledgement is not enough");
    latr_tb_fence_ack(&cache_b);
    CHECK(latr_tb_fence_wait(seq, 0) == 0, "both acknowledged: completes");

    /* --- sequence monotonicity and stale acks --------------------------- */
    seq = latr_tb_fence_begin(7, 0);
    CHECK(seq == 2, "sequence is monotonic");
    CHECK(latr_tb_fence_wait(seq, 4) == -1,
          "a hart whose ack is one request old does not satisfy the new one");
    latr_tb_fence_ack(&cache_a);
    latr_tb_fence_ack(&cache_b);
    CHECK(latr_tb_fence_wait(seq, 0) == 0, "fresh acks satisfy the request");

    /* An ack covers every request published before it was taken. */
    seq = latr_tb_fence_begin(0, 1);
    latr_tb_fence_ack(&cache_a);
    latr_tb_fence_ack(&cache_b);
    CHECK(latr_tb_fence_wait(seq - 1, 0) == 0, "older request still complete");
    CHECK(latr_tb_fence_wait(seq, 0) == 0, "FENCE.I request complete");

    /* --- in_code accounting --------------------------------------------- */
    latr_tb_code_enter(&cache_a);
    CHECK(__atomic_load_n(&cache_a.in_code, __ATOMIC_ACQUIRE) == 1,
          "in_code tracks a live block");
    latr_tb_code_enter(&cache_a);
    CHECK(__atomic_load_n(&cache_a.in_code, __ATOMIC_ACQUIRE) == 2,
          "in_code nests");
    latr_tb_code_exit(&cache_a);
    latr_tb_code_exit(&cache_a);
    CHECK(__atomic_load_n(&cache_a.in_code, __ATOMIC_ACQUIRE) == 0,
          "in_code returns to zero");

    /* --- unregistered hart does not block ------------------------------- */
    latr_tb_cache_unregister(&cache_b);
    seq = latr_tb_fence_begin(0, 0);
    latr_tb_fence_ack(&cache_a);
    CHECK(latr_tb_fence_wait(seq, 0) == 0,
          "an offline hart is not waited for");
    teardown();

    /* --- repeated requests keep working (no sequence wrap at small counts) */
    setup();
    {
        uint64_t base = latr_tb_fence_seq();

        for (int i = 0; i < 64; i++) {
            seq = latr_tb_fence_begin((uint32_t)i & 0xffffu, i & 1);
            latr_tb_fence_ack(&cache_a);
            latr_tb_fence_ack(&cache_b);
            if (latr_tb_fence_wait(seq, 0) != 0) {
                CHECK(0, "repeated fences complete");
                break;
            }
        }
        CHECK(seq == base + 64, "64 more requests advanced the sequence");
    }
    teardown();

    printf("latr-fence-ack: %d checks, %d fails\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
