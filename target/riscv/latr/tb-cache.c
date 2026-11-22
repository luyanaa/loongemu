/*
 * Per-vCPU TB cache index. The runtime owns translation and the code arena;
 * this module only answers "is this PC translated for this ASID/privilege/
 * satp mode" and applies shootdowns. Entries are never reused for a different
 * key, which is what makes privilege switches and satp rewrites safe.
 */
#include <string.h>
#include "latr-tb-cache.h"

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
