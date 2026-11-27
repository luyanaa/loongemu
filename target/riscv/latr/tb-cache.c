/*
 * Per-vCPU TB cache index. The runtime owns translation and the code arena;
 * this module only answers "is this PC translated for this ASID/privilege/
 * satp mode" and applies shootdowns. Entries are never reused for a different
 * key, which is what makes privilege switches and satp rewrites safe.
 */
#include <string.h>
#include "latr-tb-cache.h"
#include "latr-hart-sync.h"

#define SATP_ASID_SHIFT 44
#define SATP_MODE_SHIFT 60
#define SATP_ASID_MASK  UINT64_C(0xffff)
#define PTE_PPN_MASK ((UINT64_C(1) << 44) - 1)

static void cache_lock(LatrTbCache *cache)
{
    while (__atomic_exchange_n(&cache->lock, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&cache->lock, __ATOMIC_RELAXED) != 0) {
        }
    }
}

static void cache_unlock(LatrTbCache *cache)
{
    __atomic_store_n(&cache->lock, 0, __ATOMIC_RELEASE);
}

void latr_tb_cache_init(LatrTbCache *cache, uint8_t *arena, uint32_t size)
{
    if (cache == 0) {
        return;
    }
    for (int i = 0; i < LATR_TB_CACHE_ENTRIES; i++) {
        cache->entries[i].valid = 0;
    }
    cache->arena = arena;
    cache->arena_size = arena == 0 ? 0 : size;
    cache->arena_used = 0;
    cache->count = 0;
    cache->generation = 1;
    cache->victim = 0;
    cache->lock = 0;
    cache->reclaim_pending = 0;
    cache->applied_seq = 0;
    cache->in_code = 0;
    cache->park = 0;
    for (int i = 0; i < LATR_TB_HOT_SLOTS; i++) {
        cache->hot[i].valid = 0;
    }
}

/* Charges the page `pc` belongs to. Called under the cache lock. */
static void tb_hot_charge(LatrTbCache *cache, uint64_t pc)
{
    uint64_t page = pc & ~UINT64_C(0xfff);
    LatrTbHotPage *slot = &cache->hot[(page >> 12) &
                                      (LATR_TB_HOT_SLOTS - 1)];

    if (!slot->valid || slot->page != page) {
        slot->page = page;
        slot->translations = 0;
        slot->valid = 1;
    }
    slot->translations++;
}

LatrTbKey latr_tb_cache_key_parts(uint64_t satp, uint32_t privilege,
                                  uint32_t vtype, uint32_t vill, uint32_t vl)
{
    LatrTbKey key;

    key.satp_mode = (uint32_t)(satp >> SATP_MODE_SHIFT);
    key.asid = (uint32_t)((satp >> SATP_ASID_SHIFT) & SATP_ASID_MASK);
    key.privilege = privilege;
    key.vspec = (vtype & 0x3ffu) | ((vill & 1u) << 10) | ((vl & 0xffu) << 16);
    key.root_ppn = satp & PTE_PPN_MASK;
    return key;
}

static int key_equal(const LatrTbKey *a, const LatrTbKey *b)
{
    return a->satp_mode == b->satp_mode && a->privilege == b->privilege &&
           a->asid == b->asid && a->vspec == b->vspec &&
           a->root_ppn == b->root_ppn;
}

/*
 * Set index for the 4-way index. This has to be cheap: it runs on every
 * dispatch, and in a bare-metal VMM that cost is pure overhead on top of the
 * translated block. Mixing the key in as well as the PC keeps every ASID /
 * privilege variant from landing in the same set.
 */
static uint32_t tb_set_index(uint64_t pc, LatrTbKey key)
{
    uint32_t h = (uint32_t)(pc >> 2);

    h ^= h >> 7;
    h *= 0x9e3779b1u;
    h ^= key.asid * 0x85ebca6bu;
    h ^= key.vspec * 0xc2b2ae35u;
    h ^= (uint32_t)key.privilege + (uint32_t)key.satp_mode * 31u;
    return h & (uint32_t)(LATR_TB_CACHE_SETS - 1);
}

LatrTbEntry *latr_tb_cache_lookup(LatrTbCache *cache, uint64_t pc,
                                  LatrTbKey key)
{
    LatrTbEntry *found = 0;
    uint32_t set;

    if (cache == 0) {
        return 0;
    }
    set = tb_set_index(pc, key) * LATR_TB_CACHE_WAYS;
    /* The entry pointer stays valid after unlocking: entries live in the
     * cache object and only the owner recycles the arena at a safe point. */
    cache_lock(cache);
    for (int way = 0; way < LATR_TB_CACHE_WAYS; way++) {
        LatrTbEntry *entry = &cache->entries[set + (uint32_t)way];
        if (entry->valid && entry->guest_pc == pc &&
            key_equal(&entry->key, &key)) {
            found = entry;
            break;
        }
    }
    cache_unlock(cache);
    return found;
}

LatrTbEntry *latr_tb_cache_insert(LatrTbCache *cache, uint64_t pc,
                                  LatrTbKey key, const void *code,
                                  uint32_t code_size, uint32_t icount)
{
    LatrTbEntry *result = 0;

    if (cache == 0 || cache->arena == 0 || code_size == 0) {
        return 0;
    }
    cache_lock(cache);
    /* Called by the owning hart between TB dispatches, so an empty index
     * means no live entry references the arena and it can restart at 0. */
    if (cache->count == 0) {
        cache->arena_used = 0;
        cache->reclaim_pending = 0;
    }
    if (cache->arena_used + code_size > cache->arena_size) {
        cache->reclaim_pending = 1;
        cache_unlock(cache);
        return 0;
    }
    int slot = -1;
    uint32_t set = tb_set_index(pc, key) * LATR_TB_CACHE_WAYS;
    for (int way = 0; way < LATR_TB_CACHE_WAYS; way++) {
        if (!cache->entries[set + (uint32_t)way].valid) {
            slot = (int)(set + (uint32_t)way);
            break;
        }
    }
    if (slot < 0) {
        /* Replace one way of *this* set round-robin instead of flushing the
         * whole index. The old code bytes remain in the arena, so a hart still
         * executing them is not disturbed; the arena is recycled at a safe
         * point. A global cursor advancing modulo the way count keeps every
         * set rotating through all of its ways without a per-set counter. */
        slot = (int)(set + (cache->victim % LATR_TB_CACHE_WAYS));
        cache->victim = (uint32_t)(slot + 1);
        /*
         * Drop the victim's recovery map with the entry. The old code bytes
         * stay in the arena (a hart may still be executing them), so a host
         * fault there still resolves -- but nothing can ever *find* this entry
         * again, and a map is ~22 KiB. Without this, a boot that translates
         * hundreds of thousands of blocks holds one map per block ever
         * translated and dies in malloc long before the arena is recycled.
         */
        cache->evicted_code = cache->entries[slot].valid
                                  ? (uintptr_t)cache->entries[slot].code
                                  : 0;
        cache->entries[slot].valid = 0;
        cache->count--;
    }
    uint8_t *dest = cache->arena + cache->arena_used;
    for (uint32_t i = 0; i < code_size; i++) {
        dest[i] = ((const uint8_t *)code)[i];
    }
    LatrTbEntry *entry = &cache->entries[slot];
    entry->guest_pc = pc;
    entry->code = dest;
    entry->code_size = code_size;
    entry->icount = icount;
    entry->key = key;
    entry->valid = 1;
    cache->arena_used += code_size;
    cache->count++;
    tb_hot_charge(cache, pc);
    result = entry;
    cache_unlock(cache);
    return result;
}

void latr_tb_shootdown_all(LatrTbCache *cache)
{
    if (cache == 0) {
        return;
    }
    cache_lock(cache);
    for (int i = 0; i < LATR_TB_CACHE_ENTRIES; i++) {
        cache->entries[i].valid = 0;
    }
    cache->count = 0;
    /* Do not reset arena_used here: a hart may still be executing code from
     * it. The owner recycles the arena in latr_tb_cache_safepoint(). */
    cache->reclaim_pending = 1;
    cache->generation++;
    cache_unlock(cache);
}

void latr_tb_shootdown_asid(LatrTbCache *cache, uint32_t asid)
{
    if (cache == 0) {
        return;
    }
    if (asid == 0 || asid == UINT32_MAX) {
        latr_tb_shootdown_all(cache);
        return;
    }
    cache_lock(cache);
    for (int i = 0; i < LATR_TB_CACHE_ENTRIES; i++) {
        LatrTbEntry *entry = &cache->entries[i];
        if (entry->valid && entry->key.asid == asid) {
            entry->valid = 0;
            if (cache->count > 0) {
                cache->count--;
            }
        }
    }
    cache->generation++;
    cache_unlock(cache);
}

void latr_tb_shootdown_privilege(LatrTbCache *cache, uint32_t privilege)
{
    if (cache == 0) {
        return;
    }
    cache_lock(cache);
    for (int i = 0; i < LATR_TB_CACHE_ENTRIES; i++) {
        LatrTbEntry *entry = &cache->entries[i];
        if (entry->valid && entry->key.privilege == privilege) {
            entry->valid = 0;
            if (cache->count > 0) {
                cache->count--;
            }
        }
    }
    cache->generation++;
    cache_unlock(cache);
}

bool latr_tb_cache_safepoint(LatrTbCache *cache)
{
    bool reclaimed = false;

    if (cache == 0) {
        return false;
    }
    cache_lock(cache);
    /*
     * Recycling is only legal for the owning hart between dispatches: at that
     * instant no generated code from this arena is executing. `count == 0`
     * means every entry has been invalidated (shootdown or eviction), so the
     * whole arena is garbage and reuse can restart at offset 0.
     */
    if (cache->count == 0 && (cache->arena_used != 0 || cache->reclaim_pending)) {
        cache->arena_used = 0;
        cache->reclaim_pending = 0;
        reclaimed = true;
    }
    cache_unlock(cache);
    return reclaimed;
}

void latr_tb_cache_reclaim_all(LatrTbCache *cache)
{
    if (cache == 0) {
        return;
    }
    latr_tb_shootdown_all(cache);
    latr_tb_cache_safepoint(cache);
}

uint32_t latr_tb_hot_page(const LatrTbCache *cache, unsigned slot,
                          uint64_t *page)
{
    if (cache == NULL || slot >= LATR_TB_HOT_SLOTS) {
        return 0;
    }
    if (!cache->hot[slot].valid) {
        return 0;
    }
    if (page != NULL) {
        *page = cache->hot[slot].page;
    }
    return cache->hot[slot].translations;
}

uint32_t latr_tb_hot_busiest(const LatrTbCache *cache, uint64_t *page)
{
    uint32_t best = 0;
    unsigned best_slot = 0;

    if (cache == NULL) {
        return 0;
    }
    for (unsigned i = 0; i < LATR_TB_HOT_SLOTS; i++) {
        if (cache->hot[i].valid && cache->hot[i].translations > best) {
            best = cache->hot[i].translations;
            best_slot = i;
        }
    }
    if (best != 0 && page != NULL) {
        *page = cache->hot[best_slot].page;
    }
    return best;
}

uint32_t latr_tb_cache_available(const LatrTbCache *cache)
{
    if (cache == 0 || cache->arena_used >= cache->arena_size) {
        return 0;
    }
    return cache->arena_size - cache->arena_used;
}

/* --- cross-core shootdown registry --- */

#define LATR_TB_CACHE_MAX_CORES 8

static LatrTbCache *volatile latr_tb_cores[LATR_TB_CACHE_MAX_CORES];

int latr_tb_cache_register(LatrTbCache *cache)
{
    if (cache == 0) {
        return 0;
    }
    for (int i = 0; i < LATR_TB_CACHE_MAX_CORES; i++) {
        LatrTbCache *slot = __atomic_load_n(&latr_tb_cores[i],
                                            __ATOMIC_ACQUIRE);
        if (slot == cache) {
            return 1; /* already registered */
        }
        if (slot == 0) {
            __atomic_store_n(&latr_tb_cores[i], cache, __ATOMIC_RELEASE);
            return 1;
        }
    }
    return 0;
}

void latr_tb_cache_unregister(LatrTbCache *cache)
{
    for (int i = 0; i < LATR_TB_CACHE_MAX_CORES; i++) {
        if (__atomic_load_n(&latr_tb_cores[i], __ATOMIC_ACQUIRE) == cache) {
            __atomic_store_n(&latr_tb_cores[i], (LatrTbCache *)0,
                             __ATOMIC_RELEASE);
            return;
        }
    }
}

void latr_tb_shootdown_asid_all_cores(uint32_t asid)
{
    for (int i = 0; i < LATR_TB_CACHE_MAX_CORES; i++) {
        LatrTbCache *cache = __atomic_load_n(&latr_tb_cores[i],
                                             __ATOMIC_ACQUIRE);
        if (cache != 0) {
            latr_tb_shootdown_asid(cache, asid);
        }
    }
}

/* Global fence sequence; defined here so fence_i_local can bump it. */
static volatile uint64_t latr_tb_fence_seq_word;

uint32_t latr_tb_cache_drop_stale(LatrTbCache *cache, LatrTbSameCode same,
                                  void *same_ctx, LatrTbOnDrop on_drop,
                                  void *drop_ctx)
{
    uint32_t dropped = 0;

    if (cache == 0 || same == 0) {
        return 0;
    }
    cache_lock(cache);
    for (int i = 0; i < LATR_TB_CACHE_ENTRIES; i++) {
        LatrTbEntry *e = &cache->entries[i];
        uintptr_t code;

        if (!e->valid) {
            continue;
        }
        /*
         * verifiable == 0 means the guest bytes could not be recorded (block
         * longer than the snapshot, or the fetch faulted at translate time), so
         * the only safe answer is the old one: drop it.
         */
        if (e->verifiable && same(same_ctx, e)) {
            continue;
        }
        code = (uintptr_t)e->code;
        e->valid = 0;
        cache->count--;
        dropped++;
        if (on_drop != 0) {
            on_drop(drop_ctx, code);
        }
        cache->victim = (uint32_t)i;
    }
    /*
     * Reclaim only when nothing is live: the arena bytes of a dropped block may
     * still be executing on this hart until the next safe point, and a partial
     * drop leaves plenty of room, so recycling the whole arena buys nothing.
     */
    if (cache->count == 0) {
        cache->reclaim_pending = 1;
    }
    cache->generation++;
    cache_unlock(cache);
    return dropped;
}

uint64_t latr_tb_fence_i_local(LatrTbCache *self, LatrTbSameCode same,
                               void *same_ctx, LatrTbOnDrop on_drop,
                               void *drop_ctx)
{
    uint64_t seq = __atomic_add_fetch(&latr_tb_fence_seq_word, 1,
                                      __ATOMIC_ACQ_REL);

    if (self != 0) {
        (void)latr_tb_cache_drop_stale(self, same, same_ctx, on_drop,
                                       drop_ctx);
    }
    /*
     * Other harts: their own owner must do the comparison, and the scheduler has
     * no way to hand a filter to them, so they keep the conservative full
     * shootdown. On the single-hart machine this loop finds only the caller.
     */
    for (int i = 0; i < LATR_TB_CACHE_MAX_CORES; i++) {
        LatrTbCache *cache = __atomic_load_n(&latr_tb_cores[i],
                                             __ATOMIC_ACQUIRE);
        if (cache != 0 && cache != self) {
            latr_tb_shootdown_all(cache);
        }
        if (cache != 0 && cache->park != 0) {
            latr_hart_wake(cache->park);
        }
    }
    return seq;
}

void latr_tb_fence_i_all_cores(void)
{
    for (int i = 0; i < LATR_TB_CACHE_MAX_CORES; i++) {
        LatrTbCache *cache = __atomic_load_n(&latr_tb_cores[i],
                                             __ATOMIC_ACQUIRE);
        if (cache != 0) {
            latr_tb_shootdown_all(cache);
        }
    }
}

/* --- fence completion ------------------------------------------------------
 *
 * See docs/design/latr-smp-concurrency.md section 5. Sequence numbers are
 * global and monotonic; acknowledgement is per hart and only happens at a safe
 * point (between TBs), which is what makes "the hart is outside code fetched
 * before the request" true rather than probabilistic.
 */
static volatile uint64_t latr_tb_fence_timeout_count;
static volatile uint64_t latr_tb_fence_missing_hart;

uint64_t latr_tb_fence_seq(void)
{
    return __atomic_load_n(&latr_tb_fence_seq_word, __ATOMIC_ACQUIRE);
}

void latr_tb_cache_set_park(LatrTbCache *cache, struct LatrHartSync *park)
{
    if (cache != 0) {
        cache->park = park;
    }
}

uint64_t latr_tb_fence_begin(uint32_t asid, int fence_i)
{
    /* Publish the sequence before the invalidation: a hart that acknowledges
     * between the two still ends up outside code that predates the request,
     * and the invalidation itself is applied under each cache's lock. */
    uint64_t seq = __atomic_add_fetch(&latr_tb_fence_seq_word, 1,
                                      __ATOMIC_ACQ_REL);

    if (fence_i) {
        latr_tb_fence_i_all_cores();
    } else {
        latr_tb_shootdown_asid_all_cores(asid);
    }
    /*
     * A hart parked in WFI will not reach its safe point on its own, so a
     * request that needs an acknowledgement must be a wake event (design
     * section 5). Waking is enough: the next dispatcher entry acks before it
     * executes anything.
     */
    for (int i = 0; i < LATR_TB_CACHE_MAX_CORES; i++) {
        LatrTbCache *cache = __atomic_load_n(&latr_tb_cores[i],
                                             __ATOMIC_ACQUIRE);
        if (cache != 0 && cache->park != 0) {
            latr_hart_wake(cache->park);
        }
    }
    return seq;
}

void latr_tb_code_enter(LatrTbCache *cache)
{
    if (cache != 0) {
        __atomic_add_fetch(&cache->in_code, 1, __ATOMIC_ACQ_REL);
    }
}

void latr_tb_code_exit(LatrTbCache *cache)
{
    if (cache != 0) {
        __atomic_sub_fetch(&cache->in_code, 1, __ATOMIC_ACQ_REL);
    }
}

/*
 * Called by the owning hart at a safe point. Reading the global sequence first
 * and publishing it second means a request that arrives in between is also
 * covered: the hart is outside code either way, and every issuer invalidates
 * synchronously under this cache's lock.
 */
void latr_tb_fence_ack(LatrTbCache *cache)
{
    uint64_t seq;

    if (cache == 0) {
        return;
    }
    seq = __atomic_load_n(&latr_tb_fence_seq_word, __ATOMIC_ACQUIRE);
    __atomic_store_n(&cache->applied_seq, seq, __ATOMIC_RELEASE);
}

int latr_tb_fence_wait(uint64_t seq, uint64_t spin_limit)
{
    for (;;) {
        uint64_t worst = 0;
        int pending = 0;

        for (int i = 0; i < LATR_TB_CACHE_MAX_CORES; i++) {
            LatrTbCache *cache = __atomic_load_n(&latr_tb_cores[i],
                                                 __ATOMIC_ACQUIRE);
            uint64_t applied;

            if (cache == 0) {
                continue;
            }
            applied = __atomic_load_n(&cache->applied_seq, __ATOMIC_ACQUIRE);
            if (applied < seq) {
                pending = 1;
                if (seq - applied > worst) {
                    worst = seq - applied;
                }
            }
        }
        if (!pending) {
            return 0;
        }
        if (spin_limit == 0) {
            __atomic_add_fetch(&latr_tb_fence_timeout_count, 1,
                               __ATOMIC_RELAXED);
            __atomic_store_n(&latr_tb_fence_missing_hart, worst,
                             __ATOMIC_RELAXED);
            return -1;
        }
        spin_limit--;
        __asm__ volatile("" ::: "memory");
    }
}

uint64_t latr_tb_fence_timeouts(void)
{
    return __atomic_load_n(&latr_tb_fence_timeout_count, __ATOMIC_RELAXED);
}

uint64_t latr_tb_fence_last_missing(void)
{
    return __atomic_load_n(&latr_tb_fence_missing_hart, __ATOMIC_RELAXED);
}
