/*
 * Host tests for the chain-control state machine (P0).
 *
 * Design: docs/design/chain-control-ras.md. The state machine's whole job is to
 * be *the* word of truth about "has anything changed since this block was
 * entered". That is only true if the epoch moves on exactly the six events and
 * on nothing else, so this test drives each one and asserts both the bucket and
 * the epoch, and asserts that reading a counter does not move anything.
 *
 * A future break reason must extend the enum, the design table and this test;
 * that is the point of pinning the set here.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "latr-chain.h"

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

static void test_init(void)
{
    LatrChainControl chain;

    memset(&chain, 0xa5, sizeof(chain));
    latr_chain_init(&chain, 0);
    CHECK(latr_chain_epoch(&chain) == 0, "init: epoch starts at 0");
    CHECK(latr_chain_tb_generation(&chain) == 0, "init: generation starts at 0");
    CHECK(latr_chain_links(&chain) == 0, "init: no links (P0 never links)");
    CHECK(latr_chain_total_breaks(&chain) == 0, "init: no breaks recorded");
    CHECK(chain.max_chain == 0, "init: chaining disabled by policy");
    for (unsigned i = 0; i < LATR_CHAIN_BREAK_COUNT; i++) {
        if (latr_chain_breaks(&chain, (LatrChainBreak)i) != 0) {
            CHECK(0, "init: every break bucket is zero");
            break;
        }
    }
}

static void test_breaks(void)
{
    LatrChainControl chain;
    uint64_t epoch = 0;

    latr_chain_init(&chain, 4);
    for (unsigned i = 0; i < LATR_CHAIN_BREAK_COUNT; i++) {
        latr_chain_break(&chain, (LatrChainBreak)i);
        CHECK(latr_chain_breaks(&chain, (LatrChainBreak)i) == 1,
              "break: the event lands in its own bucket");
        CHECK(latr_chain_epoch(&chain) == epoch + 1,
              "break: the epoch moves exactly once per event");
        epoch++;
    }
    CHECK(latr_chain_total_breaks(&chain) == LATR_CHAIN_BREAK_COUNT,
          "break: total counts every bucket");
    for (unsigned i = 0; i < LATR_CHAIN_BREAK_COUNT; i++) {
        if (latr_chain_breaks(&chain, (LatrChainBreak)i) != 1) {
            CHECK(0, "break: buckets do not leak into each other");
            break;
        }
    }
    /* A second event in the same bucket only moves that bucket. */
    latr_chain_break(&chain, LATR_CHAIN_BREAK_MISS);
    CHECK(latr_chain_breaks(&chain, LATR_CHAIN_BREAK_MISS) == 2,
          "break: repeated events accumulate");
    CHECK(latr_chain_breaks(&chain, LATR_CHAIN_BREAK_WFI) == 1,
          "break: unrelated buckets stay put");
    CHECK(latr_chain_epoch(&chain) == epoch + 1, "break: epoch moved again");

    /* Reading is inert. */
    (void)latr_chain_epoch(&chain);
    (void)latr_chain_tb_generation(&chain);
    (void)latr_chain_links(&chain);
    (void)latr_chain_total_breaks(&chain);
    CHECK(latr_chain_epoch(&chain) == epoch + 1,
          "read: querying the state does not move the epoch");
}

static void test_generation(void)
{
    LatrChainControl chain;

    latr_chain_init(&chain, 8);
    latr_chain_tb_changed(&chain);
    CHECK(latr_chain_tb_generation(&chain) == 1, "generation moves on insert");
    latr_chain_tb_changed(&chain);
    CHECK(latr_chain_tb_generation(&chain) == 2, "generation moves again");
    CHECK(latr_chain_epoch(&chain) == 0,
          "generation is a separate word: a cache change is not a break");
    latr_chain_break(&chain, LATR_CHAIN_BREAK_SFENCE);
    CHECK(latr_chain_tb_generation(&chain) == 2,
          "a break does not move the generation");
}

static void test_lengths(void)
{
    LatrChainControl chain;

    latr_chain_init(&chain, 8);
    latr_chain_record_length(&chain, 0);  /* bucket 0 */
    latr_chain_record_length(&chain, 1);  /* bucket 0 */
    latr_chain_record_length(&chain, 2);  /* bucket 1 */
    latr_chain_record_length(&chain, 3);  /* bucket 1 */
    latr_chain_record_length(&chain, 4);  /* bucket 2 */
    latr_chain_record_length(&chain, 255); /* bucket 7 */
    CHECK(chain.length_histogram[0] == 2, "length: <2");
    CHECK(chain.length_histogram[1] == 2, "length: 2-3");
    CHECK(chain.length_histogram[2] == 1, "length: 4-7");
    CHECK(chain.length_histogram[7] == 1, "length: >=128");
    CHECK(latr_chain_links(&chain) == 0,
          "length: recording a chain does not fake links");
}

/* Phase A: the candidate cache is a link only when the whole condition holds. */
static void test_candidates(void)
{
    static uint8_t arena[4096];
    static LatrTbCache cache;
    LatrChainControl chain;
    LatrTbKey key;
    LatrTbEntry *entry;
    const uint32_t code[4] = {1, 2, 3, 4};

    memset(&key, 0, sizeof(key));
    key.satp_mode = 8;
    key.privilege = 3;
    key.asid = 5;
    latr_tb_cache_init(&cache, arena, sizeof(arena));
    latr_chain_init(&chain, 16);

    entry = latr_tb_cache_insert(&cache, 0x80000000u, key, code,
                                 sizeof(code), 2);
    CHECK(entry != NULL, "candidate: insert succeeded");
    latr_chain_remember(&chain, entry, &key);

    CHECK(latr_chain_link_candidate(&chain, &cache, 0x80000000u, &key) == entry,
          "candidate: same pc and key links");
    CHECK(latr_chain_links(&chain) == 1, "candidate: a link is counted");
    CHECK(latr_chain_epoch(&chain) == 0, "candidate: linking does not break");

    /* Different PC, different key, invalidated entry: all must miss. */
    CHECK(latr_chain_link_candidate(&chain, &cache, 0x80000004u, &key) == NULL,
          "candidate: a different pc does not link");
    {
        LatrTbKey other = key;

        other.asid = 6;
        CHECK(latr_chain_link_candidate(&chain, &cache, 0x80000000u, &other) ==
                  NULL,
              "candidate: a different ASID does not link");
        other = key;
        other.privilege = 1;
        CHECK(latr_chain_link_candidate(&chain, &cache, 0x80000000u, &other) ==
                  NULL,
              "candidate: a different privilege does not link");
    }
    CHECK(latr_chain_link_candidate(&chain, &cache, 0x80000000u, &key) == entry,
          "candidate: still links after a rejected probe");

    /* A shootdown invalidates the entry itself, so the candidate must stop
     * linking without the chain state having to notice. */
    latr_tb_shootdown_all(&cache);
    CHECK(latr_chain_link_candidate(&chain, &cache, 0x80000000u, &key) == NULL,
          "candidate: shootdown stops the link");
    {
        uint64_t links = latr_chain_links(&chain);

        /* Arena reset after the shootdown: the code range is gone too. */
        latr_tb_cache_safepoint(&cache);
        CHECK(latr_chain_link_candidate(&chain, &cache, 0x80000000u, &key) ==
                  NULL,
              "candidate: a reclaimed arena stops the link");
        CHECK(latr_chain_links(&chain) == links, "candidate: misses add no links");
    }

    /* A fresh insert at the same PC and key links again. */
    entry = latr_tb_cache_insert(&cache, 0x80000000u, key, code, sizeof(code), 2);
    CHECK(entry != NULL, "candidate: re-insert after reclaim");
    latr_chain_remember(&chain, entry, &key);
    CHECK(latr_chain_link_candidate(&chain, &cache, 0x80000000u, &key) == entry,
          "candidate: re-translated block links again");
    CHECK(latr_chain_candidate_hits(&chain) == 3,
          "candidate: hit counter tracks links");
}

static void test_null_and_bounds(void)
{
    LatrChainControl chain;

    /* Out-of-range reasons must not corrupt the array. */
    latr_chain_init(&chain, 1);
    latr_chain_break(&chain, (LatrChainBreak)LATR_CHAIN_BREAK_COUNT);
    CHECK(latr_chain_epoch(&chain) == 0, "bounds: out-of-range break ignored");
    CHECK(latr_chain_breaks(&chain, (LatrChainBreak)LATR_CHAIN_BREAK_COUNT) == 0,
          "bounds: out-of-range query returns 0");
    CHECK(latr_chain_break_name(LATR_CHAIN_BREAK_WFI)[0] == 'w',
          "names: break names are stable");
    latr_chain_init(0, 0); /* must not crash */
    CHECK(latr_chain_epoch(0) == 0, "null: queries are safe");
    latr_chain_break(0, LATR_CHAIN_BREAK_MISS);
    latr_chain_tb_changed(0);
    CHECK(1, "null: mutations are safe");
}

int main(void)
{
    test_init();
    test_breaks();
    test_generation();
    test_lengths();
    test_candidates();
    test_null_and_bounds();

    printf("latr-chain: %d checks, %d fails\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
