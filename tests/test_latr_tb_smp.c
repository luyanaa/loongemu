/*
 * Host unit tests for the per-hart TB cache.
 *
 * These cover the invariants SMP depends on and that a bare-metal run cannot
 * show cheaply:
 *   - a translation is never reused across ASID / privilege / satp mode / RVV
 *     config, because the key carries all four;
 *   - shootdowns only invalidate entries, they never rewind the arena another
 *     hart may still be executing;
 *   - only the owner's safe point recycles the arena, and it reports that fact
 *     so the caller can drop host-PC -> guest-PC maps.
 */
#include <stdio.h>
#include <pthread.h>
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

#define ARENA_SIZE 4096u

static uint8_t arena_a[ARENA_SIZE];
static uint8_t arena_b[ARENA_SIZE];
/* One 16-byte block per index slot plus slack, so the index can be filled and
 * then evicted once without the arena itself running out. */
static uint8_t arena_big[LATR_TB_CACHE_ENTRIES * 16u + 128u];
static LatrTbCache cache_a;
static LatrTbCache cache_b;

static LatrTbKey key(uint32_t asid, uint32_t priv)
{
    return latr_tb_cache_key_parts(UINT64_C(8) << 60 | (uint64_t)asid << 44 |
                                       UINT64_C(0x12345),
                                   priv, 0, 0, 0);
}

static uint8_t blob[16];

typedef struct ExecutionRace {
    LatrTbCache *cache;
    LatrTbKey key;
    volatile int running;
    volatile int leave_tb;
    volatile int bad_code;
    volatile int reads;
    volatile int reclaimed;
} ExecutionRace;

static void *execute_during_shootdown(void *opaque)
{
    ExecutionRace *race = opaque;
    LatrTbEntry *entry = latr_tb_cache_lookup(race->cache, 0x6000u,
                                               race->key);
    uint8_t *code = entry != NULL ? entry->code : NULL;

    if (code == NULL) {
        __atomic_store_n(&race->bad_code, 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&race->running, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&race->leave_tb, __ATOMIC_ACQUIRE)) {
        if (code != NULL && code[0] != 0x5a) {
            __atomic_store_n(&race->bad_code, 1, __ATOMIC_RELAXED);
        }
        __atomic_add_fetch(&race->reads, 1, __ATOMIC_RELAXED);
    }
    if (latr_tb_cache_safepoint(race->cache)) {
        __atomic_store_n(&race->reclaimed, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

int main(void)
{
    /* 1. Key derivation. */
    {
        LatrTbKey k = latr_tb_cache_key_parts(
            UINT64_C(8) << 60 | UINT64_C(0xbeef) << 44 | UINT64_C(0x123456), 1,
            0x123, 1, 0x2a);

        CHECK(k.satp_mode == 8, "satp mode is bits 63:60");
        CHECK(k.asid == 0xbeef, "ASID is bits 59:44");
        CHECK(k.root_ppn == UINT64_C(0x123456), "root PPN is the low 44 bits");
        CHECK(k.privilege == 1, "privilege recorded");
        CHECK((k.vspec & 0x3ffu) == 0x123, "vtype occupies the low 10 bits");
        CHECK(((k.vspec >> 10) & 1u) == 1, "vill is bit 10");
        CHECK((k.vspec >> 16) == 0x2a, "vl starts at bit 16");
    }

    /* 2. Insert then look up. */
    latr_tb_cache_init(&cache_a, arena_a, sizeof(arena_a));
    {
        LatrTbKey k = key(1, 1);
        LatrTbEntry *e = latr_tb_cache_insert(&cache_a, 0x80000000u, k, blob,
                                              sizeof(blob), 3);

        CHECK(e != NULL, "insert succeeds");
        CHECK(e == latr_tb_cache_lookup(&cache_a, 0x80000000u, k),
              "lookup returns the inserted entry");
        CHECK(e->code == arena_a, "first entry starts at the arena base");
        CHECK(e->icount == 3, "icount stored");
        CHECK(e->code_size == sizeof(blob), "code size stored");
        CHECK(latr_tb_cache_available(&cache_a) == ARENA_SIZE - sizeof(blob),
              "arena accounting");
    }

    /* 3. A different key is a different translation. */
    {
        LatrTbKey other_asid = key(2, 1);
        LatrTbKey other_priv = key(1, 0);

        CHECK(latr_tb_cache_lookup(&cache_a, 0x80000000u, other_asid) == NULL,
              "same PC with another ASID misses");
        CHECK(latr_tb_cache_lookup(&cache_a, 0x80000000u, other_priv) == NULL,
              "same PC in another privilege misses");
        CHECK(latr_tb_cache_insert(&cache_a, 0x80000000u, other_asid, blob,
                                   sizeof(blob), 3) != NULL,
              "same PC can be translated again for another ASID");
        CHECK(cache_a.count == 2, "two independent entries");
    }

    /* 4. RVV config changes invalidate through the key. */
    {
        LatrTbKey v1 = latr_tb_cache_key_parts(0, 1, 0x00, 0, 4);
        LatrTbKey v2 = latr_tb_cache_key_parts(0, 1, 0x00, 0, 8);

        CHECK(latr_tb_cache_insert(&cache_a, 0x90000000u, v1, blob,
                                   sizeof(blob), 1) != NULL, "vl=4 insert");
        CHECK(latr_tb_cache_lookup(&cache_a, 0x90000000u, v2) == NULL,
              "a different vl does not hit the vl=4 translation");
        CHECK(latr_tb_cache_lookup(&cache_a, 0x90000000u, v1) != NULL,
              "the original vl still hits");
    }

    /* 5. Shootdown invalidates but leaves the arena alone until a safe point. */
    {
        uint32_t used_before = cache_a.arena_used;
        uint64_t gen_before = cache_a.generation;

        latr_tb_shootdown_all(&cache_a);
        CHECK(cache_a.count == 0, "shootdown drops every entry");
        CHECK(cache_a.generation == gen_before + 1, "shootdown bumps generation");
        CHECK(cache_a.arena_used == used_before,
              "shootdown does not rewind the arena");
        CHECK(cache_a.reclaim_pending == 1, "shootdown requests reclamation");
        CHECK(latr_tb_cache_safepoint(&cache_a), "safe point reports a reclaim");
        CHECK(cache_a.arena_used == 0, "safe point rewinds the arena");
        CHECK(cache_a.reclaim_pending == 0, "pending flag cleared");
        CHECK(!latr_tb_cache_safepoint(&cache_a),
              "a second safe point is a no-op");
    }

    /* 6. A safe point with live entries must not recycle anything. */
    {
        LatrTbKey k = key(3, 1);

        CHECK(latr_tb_cache_insert(&cache_a, 0xa0000000u, k, blob,
                                   sizeof(blob), 1) != NULL, "insert live TB");
        CHECK(!latr_tb_cache_safepoint(&cache_a),
              "safe point keeps the arena while entries are live");
        CHECK(latr_tb_cache_lookup(&cache_a, 0xa0000000u, k) != NULL,
              "live entry survives the safe point");
    }

    /* 7. ASID-selective shootdown. */
    {
        LatrTbKey a1 = key(1, 1);
        LatrTbKey a2 = key(2, 1);
        LatrTbCache c;

        latr_tb_cache_init(&c, arena_b, sizeof(arena_b));
        CHECK(latr_tb_cache_insert(&c, 0x1000u, a1, blob, sizeof(blob), 1),
              "asid1 TB");
        CHECK(latr_tb_cache_insert(&c, 0x2000u, a2, blob, sizeof(blob), 1),
              "asid2 TB");
        latr_tb_shootdown_asid(&c, 1);
        CHECK(latr_tb_cache_lookup(&c, 0x1000u, a1) == NULL,
              "asid1 translation dropped");
        CHECK(latr_tb_cache_lookup(&c, 0x2000u, a2) != NULL,
              "asid2 translation kept");
        latr_tb_shootdown_asid(&c, 0);
        CHECK(c.count == 0, "asid 0 means every entry");
    }

    /* 8. Privilege-selective shootdown. */
    {
        LatrTbKey s = key(1, 1);
        LatrTbKey m = key(1, 3);
        LatrTbCache c;

        latr_tb_cache_init(&c, arena_b, sizeof(arena_b));
        CHECK(latr_tb_cache_insert(&c, 0x3000u, s, blob, sizeof(blob), 1),
              "S-mode TB");
        CHECK(latr_tb_cache_insert(&c, 0x4000u, m, blob, sizeof(blob), 1),
              "M-mode TB");
        latr_tb_shootdown_privilege(&c, 1);
        CHECK(latr_tb_cache_lookup(&c, 0x3000u, s) == NULL,
              "S-mode translations dropped on privilege shootdown");
        CHECK(latr_tb_cache_lookup(&c, 0x4000u, m) != NULL,
              "M-mode translations kept");
    }

    /* 9. Round-robin replacement once the index is full. */
    {
        LatrTbCache c;

        bool filled = true;

        latr_tb_cache_init(&c, arena_big, sizeof(arena_big));
        for (uint32_t i = 0; i < LATR_TB_CACHE_ENTRIES; i++) {
            if (latr_tb_cache_insert(&c, 0x10000u + i * 4u, key(1, 1), blob,
                                     sizeof(blob), 1) == NULL) {
                filled = false;
            }
        }
        CHECK(filled, "every index slot accepts an entry");
        CHECK(c.count == LATR_TB_CACHE_ENTRIES, "index is full");
        CHECK(latr_tb_cache_insert(&c, 0x20000u, key(1, 1), blob, sizeof(blob),
                                   1) != NULL,
              "overflow replaces an entry instead of failing");
        CHECK(c.count == LATR_TB_CACHE_ENTRIES, "count stays at capacity");
        CHECK(latr_tb_cache_lookup(&c, 0x10000u, key(1, 1)) == NULL,
              "the round-robin victim was replaced");
        CHECK(latr_tb_cache_lookup(&c, 0x20000u, key(1, 1)) != NULL,
              "the new entry is present");
    }

    /* 10. Arena exhaustion asks for a safe point, then succeeds after it. */
    {
        LatrTbCache c;
        uint32_t  inserted = 0;

        latr_tb_cache_init(&c, arena_b, 256);
        for (uint32_t i = 0; i < 32; i++) {
            LatrTbEntry *e = latr_tb_cache_insert(&c, 0x30000u + i * 4u,
                                                  key(1, 1), blob,
                                                  sizeof(blob), 1);
            if (e == NULL) {
                break;
            }
            inserted++;
        }
        CHECK(inserted == 16, "a 256-byte arena holds 16 x 16-byte blocks");
        CHECK(latr_tb_cache_insert(&c, 0x40000u, key(1, 1), blob, sizeof(blob),
                                   1) == NULL,
              "exhausted arena refuses the insert");
        CHECK(c.reclaim_pending == 1, "refusal sets reclaim_pending");
        latr_tb_shootdown_all(&c);
        CHECK(latr_tb_cache_safepoint(&c), "safe point reclaims");
        CHECK(latr_tb_cache_insert(&c, 0x40000u, key(1, 1), blob, sizeof(blob),
                                   1) != NULL,
              "insert works again after reclamation");
    }

    /* 11. Cross-core shootdown reaches every registered cache. */
    {
        LatrTbKey k = key(7, 1);

        latr_tb_cache_init(&cache_a, arena_a, sizeof(arena_a));
        latr_tb_cache_init(&cache_b, arena_b, sizeof(arena_b));
        CHECK(latr_tb_cache_register(&cache_a), "register hart 0");
        CHECK(latr_tb_cache_register(&cache_b), "register hart 1");
        CHECK(latr_tb_cache_register(&cache_a), "registering twice is harmless");
        CHECK(latr_tb_cache_insert(&cache_a, 0x5000u, k, blob, sizeof(blob), 1),
              "hart0 TB");
        CHECK(latr_tb_cache_insert(&cache_b, 0x5000u, k, blob, sizeof(blob), 1),
              "hart1 TB");
        latr_tb_shootdown_asid_all_cores(7);
        CHECK(cache_a.count == 0, "hart0 shot down remotely");
        CHECK(cache_b.count == 0, "hart1 shot down remotely");
        CHECK(latr_tb_cache_insert(&cache_a, 0x5000u, k, blob, sizeof(blob), 1),
              "hart0 re-translates");
        CHECK(latr_tb_cache_insert(&cache_b, 0x5000u, k, blob, sizeof(blob), 1),
              "hart1 re-translates");
        latr_tb_fence_i_all_cores();
        CHECK(cache_a.count == 0 && cache_b.count == 0,
              "FENCE.I clears every core");
        latr_tb_cache_unregister(&cache_b);
        CHECK(latr_tb_cache_insert(&cache_b, 0x5000u, k, blob, sizeof(blob), 1),
              "hart1 re-translates after unregister");
        latr_tb_fence_i_all_cores();
        CHECK(cache_b.count == 1,
              "an unregistered cache no longer receives cross-core shootdowns");
        latr_tb_cache_unregister(&cache_a);
    }


    /* 12. A shootdown retires an entry while another hart is executing its
     * code. The arena remains intact until the executing owner reaches its
     * safe point, then reclamation becomes legal. */
    {
        static const uint64_t PC = 0x6000u;
        uint8_t code[16];
        ExecutionRace race = {0};
        pthread_t thread;
        int thread_status;

        memset(code, 0x5a, sizeof(code));
        latr_tb_cache_init(&cache_a, arena_a, sizeof(arena_a));
        race.cache = &cache_a;
        race.key = key(9, 1);
        CHECK(latr_tb_cache_insert(&cache_a, PC, race.key, code, sizeof(code),
                                   1) != NULL,
              "concurrent execution TB inserted");
        thread_status = pthread_create(&thread, NULL,
                                       execute_during_shootdown, &race);
        CHECK(thread_status == 0, "execution worker starts");
        if (thread_status == 0) {
            while (!__atomic_load_n(&race.running, __ATOMIC_ACQUIRE) ||
                   __atomic_load_n(&race.reads, __ATOMIC_RELAXED) == 0) {
            }
            latr_tb_shootdown_all(&cache_a);
            CHECK(cache_a.count == 0, "shootdown retires live lookup entry");
            CHECK(cache_a.arena_used == sizeof(code),
                  "active code arena is not reclaimed by shootdown");
            CHECK(cache_a.reclaim_pending == 1,
                  "shootdown defers reclamation to owner safe point");
            __atomic_store_n(&race.leave_tb, 1, __ATOMIC_RELEASE);
            thread_status = pthread_join(thread, NULL);
            CHECK(thread_status == 0, "execution worker joins");
            CHECK(__atomic_load_n(&race.bad_code, __ATOMIC_RELAXED) == 0,
                  "executing code remains mapped and unchanged after shootdown");
            CHECK(__atomic_load_n(&race.reclaimed, __ATOMIC_ACQUIRE) == 1,
                  "owner reclaims after leaving translated code");
            CHECK(cache_a.arena_used == 0,
                  "arena rewinds only after the safe point");
        }
    }
    printf("test-latr-tb-smp: %d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("test-latr-tb-smp: PASS\n");
        return 0;
    }
    return 1;
}
