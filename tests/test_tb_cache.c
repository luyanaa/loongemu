/*
 * Unit tests for the translation-block cache.
 *
 * The cache decides whether a guest PC may reuse already-translated code, so
 * a wrong key or a missed invalidation shows up as executing stale code --
 * the hardest class of bug to find at runtime.  Every rule in tb_cache.h is
 * checked here, including the ones that only matter after a CR3 write or an
 * SMC store.
 */
#include "loongemu/tb_cache.h"
#include "../shared/backend/loongarch/include/ir2-patch.h"

#include <stddef.h>

#include <stdio.h>

static int failures;
static int checks;

#define CHECK(cond)                                                      \
    do {                                                                 \
        checks++;                                                        \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            failures++;                                                  \
        }                                                                \
    } while (0)

#define CAP 64

static TbCacheEntry g_entries[CAP];
static uint32_t g_buckets[CAP];
static TbCache g_cache;

static void reset(void)
{
    CHECK(tbc_init(&g_cache, g_entries, g_buckets, CAP));
}

/* Distinct key components must not be interchangeable. */
static void test_key_isolation(void)
{
    reset();

    CHECK(tbc_insert(&g_cache, 0x1000, 0, 0x5000, 0) != 0);
    CHECK(tbc_lookup(&g_cache, 0x1000, 0, 0x5000, 0) != 0);

    /* Every component varied independently must miss. */
    CHECK(tbc_lookup(&g_cache, 0x1004, 0, 0x5000, 0) == 0);   /* pc */
    CHECK(tbc_lookup(&g_cache, 0x1000, 0x100, 0x5000, 0) == 0); /* cs */
    CHECK(tbc_lookup(&g_cache, 0x1000, 0, 0x6000, 0) == 0);   /* cr3 */
    CHECK(tbc_lookup(&g_cache, 0x1000, 0, 0x5000, 3) == 0);   /* cpl */

    /* ... and each of those is a separate entry, not a replacement. */
    CHECK(tbc_insert(&g_cache, 0x1004, 0, 0x5000, 0) != 0);
    CHECK(tbc_insert(&g_cache, 0x1000, 0x100, 0x5000, 0) != 0);
    CHECK(tbc_insert(&g_cache, 0x1000, 0, 0x6000, 0) != 0);
    CHECK(tbc_insert(&g_cache, 0x1000, 0, 0x5000, 3) != 0);
    CHECK(tbc_count(&g_cache) == 5);
}

/*
 * Two keys that differ only in CR3 but hash to the same bucket.  The bucket
 * hash alone must never be treated as the key: if the comparison stopped at
 * pc/cs/cpl, the first chained entry would answer for the other address
 * space and the guest would run code translated for a different CR3.
 *
 * The pair below is a real collision in a 64-slot table, computed from the
 * mix64 finaliser the cache uses, so the chain is guaranteed non-empty.
 */
static void test_bucket_collision_still_distinguishes_cr3(void)
{
    TbCacheEntry *a, *b;

    reset();
    a = tbc_insert(&g_cache, 0x1234, 0, 0x2000, 0);
    b = tbc_insert(&g_cache, 0x1234, 0, 0x6000, 0);
    CHECK(a != 0);
    CHECK(b != 0);
    CHECK(a != b);
    CHECK(a->cr3 == 0x2000);
    CHECK(b->cr3 == 0x6000);

    CHECK(tbc_lookup(&g_cache, 0x1234, 0, 0x2000, 0) == a);
    CHECK(tbc_lookup(&g_cache, 0x1234, 0, 0x6000, 0) == b);

    /* Dropping one leaves the other, in the same chain. */
    tbc_flush_asid(&g_cache, 0x2000);
    CHECK(tbc_lookup(&g_cache, 0x1234, 0, 0x2000, 0) == 0);
    CHECK(tbc_lookup(&g_cache, 0x1234, 0, 0x6000, 0) == b);
}

/* Re-inserting the same key returns the same slot and does not grow. */
static void test_insert_is_idempotent(void)
{
    TbCacheEntry *a, *b;

    reset();
    a = tbc_insert(&g_cache, 0x2000, 0, 0x9000, 0);
    CHECK(a != 0);
    a->code = 0xdeadbeef;
    a->code_size = 32;

    b = tbc_insert(&g_cache, 0x2000, 0, 0x9000, 0);
    CHECK(b == a);
    CHECK(b->code == UINT64_C(0xdeadbeef));
    CHECK(tbc_count(&g_cache) == 1);
}

typedef struct FakeTb {
    struct {
        uint8_t *ptr;
        size_t size;
    } tc;
    uint16_t jmp_insn_offset[2];
    uint16_t jmp_reset_offset[2];
    uintptr_t jmp_dest[2];
    uintptr_t jmp_list_head;
    uintptr_t jmp_list_next[2];
    struct FakeTb *next_tb[2];
} FakeTb;

static uintptr_t decode_b_target(const uint32_t *site)
{
    uint32_t imm = *site & UINT32_C(0x03ffffff);
    int64_t words = (imm & UINT32_C(0x02000000))
                        ? (int64_t)imm - INT64_C(0x04000000)
                        : (int64_t)imm;
    int64_t bytes = words * 4;
    uintptr_t pc = (uintptr_t)site;

    return bytes < 0 ? pc - (uintptr_t)(-bytes)
                     : pc + (uintptr_t)bytes;
}

static bool fake_set_jump(FakeTb *tb, int n, uintptr_t target)
{
    uint16_t offset = tb->jmp_insn_offset[n];
    FakeTb *dest = tb->next_tb[n];
    uint32_t *site;

    if ((size_t)offset > tb->tc.size ||
        tb->tc.size - (size_t)offset < sizeof(uint32_t)) {
        return false;
    }
    site = (uint32_t *)(void *)(tb->tc.ptr + offset);
    if (!loongarch_patch_b(site, target)) {
        return false;
    }
    tb->jmp_dest[n] = target;
    if (dest != NULL && (uintptr_t)dest->tc.ptr == target) {
        tb->jmp_list_next[n] = dest->jmp_list_head;
        dest->jmp_list_head = (uintptr_t)tb | (uintptr_t)n;
    }
    return true;
}

static void fake_reset_jump(FakeTb *tb, int n)
{
    FakeTb *dest = tb->next_tb[n];
    uintptr_t wanted = (uintptr_t)tb | (uintptr_t)n;
    uintptr_t *link = dest != NULL ? &dest->jmp_list_head : NULL;
    uint32_t *site =
        (uint32_t *)(void *)(tb->tc.ptr + tb->jmp_insn_offset[n]);
    uintptr_t reset = (uintptr_t)tb->tc.ptr + tb->jmp_reset_offset[n];

    if (!loongarch_patch_b(site, reset)) {
        return;
    }
    while (link != NULL && *link != 0) {
        uintptr_t node = *link;
        FakeTb *origin = (FakeTb *)(node & ~(uintptr_t)1);
        int index = (int)(node & 1u);

        if (node == wanted) {
            *link = tb->jmp_list_next[n];
            break;
        }
        link = &origin->jmp_list_next[index];
    }
    tb->jmp_list_next[n] = 0;
    tb->jmp_dest[n] = 0;
}

static void fake_invalidate_tb(FakeTb *tb)
{
    while (tb->jmp_list_head != 0) {
        uintptr_t node = tb->jmp_list_head;
        FakeTb *origin = (FakeTb *)(node & ~(uintptr_t)1);
        int n = (int)(node & 1u);
        uint32_t *site = (uint32_t *)(void *)(
            origin->tc.ptr + origin->jmp_insn_offset[n]);
        uintptr_t reset =
            (uintptr_t)origin->tc.ptr + origin->jmp_reset_offset[n];

        if (!loongarch_patch_b(site, reset)) {
            break;
        }
        tb->jmp_list_head = origin->jmp_list_next[n];
        origin->jmp_list_next[n] = 0;
        origin->jmp_dest[n] = 0;
    }
}

/* A two-TB chain patches to TB B; invalidating B unlinks and restores TB A's
 * original dispatch stub. This decodes the emitted LoongArch instruction on
 * the host, which need not itself execute LoongArch code. */
static void test_direct_branch_chain_and_unlink(void)
{
    uint32_t code_a[4] = { UINT32_C(0x50000001) };
    uint32_t code_b[4] = { UINT32_C(0x50000001) };
    FakeTb a = {0}, b = {0};
    uint32_t *site = &code_a[0];

    a.tc.ptr = (uint8_t *)code_a;
    a.tc.size = sizeof(code_a);
    a.jmp_insn_offset[0] = 0;
    a.jmp_reset_offset[0] = 4;
    a.next_tb[0] = &b;
    b.tc.ptr = (uint8_t *)code_b;
    b.tc.size = sizeof(code_b);

    CHECK(fake_set_jump(&a, 0, (uintptr_t)b.tc.ptr));
    CHECK((*site & UINT32_C(0xfc000000)) == UINT32_C(0x50000000));
    CHECK(decode_b_target(site) == (uintptr_t)b.tc.ptr);
    CHECK(a.jmp_dest[0] == (uintptr_t)b.tc.ptr);
    CHECK(b.jmp_list_head == (uintptr_t)&a);

    fake_invalidate_tb(&b);
    CHECK(decode_b_target(site) == (uintptr_t)&code_a[1]);
    CHECK(a.jmp_dest[0] == 0);
    CHECK(a.jmp_list_next[0] == 0);
    CHECK(b.jmp_list_head == 0);
}

static void test_direct_branch_range(void)
{
    uint32_t code[2] = { UINT32_C(0x50000000), 0 };
    uint32_t original = code[0];
    uintptr_t site = (uintptr_t)&code[0];

    CHECK(loongarch_patch_b(&code[0], site - UINT32_C(0x08000000)));
    CHECK(decode_b_target(&code[0]) == site - UINT32_C(0x08000000));
    code[0] = original;
    CHECK(loongarch_patch_b(&code[0], site + UINT32_C(0x07fffffc)));
    CHECK(decode_b_target(&code[0]) == site + UINT32_C(0x07fffffc));
    code[0] = original;
    CHECK(!loongarch_patch_b(&code[0], site + UINT32_C(0x08000000)));
    CHECK(code[0] == original);
}

/* An SMC store must drop the blocks whose code covers the written page. */
static void test_invalidate_range(void)
{
    TbCacheEntry *e;

    reset();
    e = tbc_insert(&g_cache, 0x3000, 0, 0x1000, 0);
    e->code_size = 16;
    tbc_insert(&g_cache, 0x5000, 0, 0x1000, 0)->code_size = 16;

    /* Writing elsewhere leaves both alone. */
    tbc_invalidate_range(&g_cache, 0x1000, 0x8000, 4);
    CHECK(tbc_lookup(&g_cache, 0x3000, 0, 0x1000, 0) != 0);
    CHECK(tbc_lookup(&g_cache, 0x5000, 0, 0x1000, 0) != 0);

    /* Writing into the block's page drops it, and only it. */
    tbc_invalidate_range(&g_cache, 0x1000, 0x3001, 1);
    CHECK(tbc_lookup(&g_cache, 0x3000, 0, 0x1000, 0) == 0);
    CHECK(tbc_lookup(&g_cache, 0x5000, 0, 0x1000, 0) != 0);
    CHECK(tbc_count(&g_cache) == 1);
}

static void test_invalidate_page(void)
{
    reset();
    tbc_insert(&g_cache, 0x3000, 0, 0x1000, 0);
    tbc_insert(&g_cache, 0x4000, 0, 0x1000, 0);

    tbc_invalidate_page(&g_cache, 0x1000, 0x3fff);
    CHECK(tbc_lookup(&g_cache, 0x3000, 0, 0x1000, 0) == 0);
    CHECK(tbc_lookup(&g_cache, 0x4000, 0, 0x1000, 0) != 0);
}

/* A block straddling a page boundary must be dropped by a write to either. */
static void test_invalidate_cross_page(void)
{
    TbCacheEntry *e;

    reset();
    e = tbc_insert(&g_cache, 0x4ff0, 0, 0x2000, 0);
    e->guest_size = 32;         /* 0x4ff0..0x500f spans two pages */

    tbc_invalidate_range(&g_cache, 0x2000, 0x5000, 1);
    CHECK(tbc_lookup(&g_cache, 0x4ff0, 0, 0x2000, 0) == 0);
}

/* Invalidation must not reach into another address space. */
static void test_invalidate_is_asid_scoped(void)
{
    reset();
    tbc_insert(&g_cache, 0x3000, 0, 0x1000, 0);
    tbc_insert(&g_cache, 0x3000, 0, 0x2000, 0);

    tbc_invalidate_range(&g_cache, 0x1000, 0x3000, 1);
    CHECK(tbc_lookup(&g_cache, 0x3000, 0, 0x1000, 0) == 0);
    CHECK(tbc_lookup(&g_cache, 0x3000, 0, 0x2000, 0) != 0);
}

/* A CR3 write drops that address space and nothing else. */
static void test_flush_asid(void)
{
    reset();
    tbc_insert(&g_cache, 0x1000, 0, 0xa000, 0);
    tbc_insert(&g_cache, 0x2000, 0, 0xa000, 0);
    tbc_insert(&g_cache, 0x1000, 0, 0xb000, 0);

    tbc_flush_asid(&g_cache, 0xa000);
    CHECK(tbc_lookup(&g_cache, 0x1000, 0, 0xa000, 0) == 0);
    CHECK(tbc_lookup(&g_cache, 0x2000, 0, 0xa000, 0) == 0);
    CHECK(tbc_lookup(&g_cache, 0x1000, 0, 0xb000, 0) != 0);
    CHECK(tbc_count(&g_cache) == 1);
}

static void test_flush_all(void)
{
    reset();
    for (uint64_t i = 0; i < 10; i++) {
        tbc_insert(&g_cache, 0x1000 + i * 16, 0, 0x1000, 0);
    }
    CHECK(tbc_count(&g_cache) == 10);
    tbc_flush_all(&g_cache);
    CHECK(tbc_count(&g_cache) == 0);
    CHECK(tbc_lookup(&g_cache, 0x1000, 0, 0x1000, 0) == 0);
    /* The table is usable again after a flush. */
    CHECK(tbc_insert(&g_cache, 0x1000, 0, 0x1000, 0) != 0);
    CHECK(tbc_count(&g_cache) == 1);
}

/*
 * Filling beyond capacity must evict, not corrupt: the table stays internally
 * consistent and the most recent insert is always findable.
 */
static void test_eviction_keeps_table_consistent(void)
{
    uint32_t i;

    reset();
    for (i = 0; i < CAP * 4; i++) {
        TbCacheEntry *e = tbc_insert(&g_cache, 0x10000 + i * 4, 0, 0x1000, 0);

        CHECK(e != 0);
    }
    CHECK(tbc_count(&g_cache) <= CAP);

    /* The newest insert must still be present. */
    CHECK(tbc_lookup(&g_cache, 0x10000 + (CAP * 4 - 1) * 4, 0, 0x1000, 0) != 0);

    /* Every live entry must be reachable through its own key. */
    for (i = 0; i < CAP; i++) {
        TbCacheEntry *e = &g_entries[i];

        if (!e->used) {
            continue;
        }
        CHECK(tbc_lookup(&g_cache, e->pc, e->cs_base, e->cr3, e->cpl) == e);
    }
}

/* After heavy churn, invalidation must still leave a consistent table. */
static void test_churn_then_invalidate(void)
{
    uint32_t i;

    reset();
    for (i = 0; i < CAP * 2; i++) {
        TbCacheEntry *e = tbc_insert(&g_cache, 0x8000 + i * 4, 0, 0x4000, 0);

        if (e) {
            e->code_size = 8;
        }
    }
    tbc_invalidate_range(&g_cache, 0x4000, 0x8000,
                         (uint64_t)CAP * 2 * 4);
    CHECK(tbc_count(&g_cache) == 0);
    for (i = 0; i < CAP; i++) {
        CHECK(!g_entries[i].used);
    }
    for (i = 0; i < CAP; i++) {
        CHECK(g_buckets[i] == TBC_NO_ENTRY);
    }
}

/* Bad geometry must be refused rather than producing a broken table. */
static void test_init_rejects_bad_geometry(void)
{
    CHECK(!tbc_init(&g_cache, g_entries, g_buckets, 0));
    CHECK(!tbc_init(&g_cache, g_entries, g_buckets, 1));
    CHECK(!tbc_init(&g_cache, g_entries, g_buckets, 3));   /* not a power of two */
    CHECK(!tbc_init(&g_cache, 0, g_buckets, CAP));
    CHECK(!tbc_init(&g_cache, g_entries, 0, CAP));
    CHECK(!tbc_init(0, g_entries, g_buckets, CAP));
    reset();
    CHECK(tbc_count(&g_cache) == 0);
}

int main(void)
{
    test_key_isolation();
    test_bucket_collision_still_distinguishes_cr3();
    test_insert_is_idempotent();
    test_direct_branch_chain_and_unlink();
    test_direct_branch_range();
    test_invalidate_range();
    test_invalidate_page();
    test_invalidate_cross_page();
    test_invalidate_is_asid_scoped();
    test_flush_asid();
    test_flush_all();
    test_eviction_keeps_table_consistent();
    test_churn_then_invalidate();
    test_init_rejects_bad_geometry();

    printf("tb_cache: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
