/*
 * Translation-block cache: fixed-size, allocation-free, chained buckets.
 *
 * See tb_cache.h for the key definition.  Eviction is round-robin over the
 * generation counter rather than LRU: the point is to bound the table, and a
 * guest that thrashes will be slow either way.
 */
#include "loongemu/tb_cache.h"

static uint32_t mix64(uint64_t v)
{
    /* splitmix64 finaliser: cheap, and it spreads PCs, which differ mostly in
     * their low bits, across the bucket array. */
    v ^= v >> 30;
    v *= UINT64_C(0xbf58476d1ce4e5b9);
    v ^= v >> 27;
    v *= UINT64_C(0x94d049bb133111eb);
    v ^= v >> 31;
    return (uint32_t)v;
}

static uint32_t key_hash(uint64_t pc, uint64_t cs_base, uint64_t cr3,
                         uint8_t cpl, uint8_t mode_flags)
{
    uint64_t h = pc;
    uint64_t cm = ((uint64_t)mode_flags << 8) | (uint64_t)cpl;

    h ^= cs_base + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
    h ^= cr3 + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
    h ^= cm + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
    return mix64(h);
}

static int key_matches(const TbCacheEntry *e, uint64_t pc, uint64_t cs_base,
                       uint64_t cr3, uint8_t cpl, uint8_t mode_flags)
{
    return e->used && e->pc == pc && e->cs_base == cs_base &&
           e->cr3 == cr3 && e->cpl == cpl && e->mode_flags == mode_flags;
}

int tbc_init(TbCache *cache, TbCacheEntry *storage, uint32_t *bucket_storage,
             uint32_t capacity)
{
    uint32_t i;

    if (cache == 0 || storage == 0 || bucket_storage == 0 || capacity < 2) {
        return 0;
    }
    if ((capacity & (capacity - 1)) != 0) {
        return 0;
    }

    cache->entries = storage;
    cache->buckets = bucket_storage;
    cache->capacity = capacity;
    cache->mask = capacity - 1;
    cache->count = 0;
    cache->generation = 0;

    for (i = 0; i < capacity; i++) {
        storage[i].used = 0;
        storage[i].hash_next = TBC_NO_ENTRY;
        storage[i].gen = 0;
    }
    for (i = 0; i < capacity; i++) {
        bucket_storage[i] = TBC_NO_ENTRY;
    }
    return 1;
}

/* Unlink an entry from its bucket chain. */
static void bucket_unlink(TbCache *cache, uint32_t index)
{
    uint32_t h = key_hash(cache->entries[index].pc,
                          cache->entries[index].cs_base,
                          cache->entries[index].cr3,
                          cache->entries[index].cpl,
                          cache->entries[index].mode_flags) & cache->mask;
    uint32_t *link = &cache->buckets[h];

    while (*link != TBC_NO_ENTRY) {
        if (*link == index) {
            *link = cache->entries[index].hash_next;
            return;
        }
        link = &cache->entries[*link].hash_next;
    }
}

TbCacheEntry *tbc_lookup_mode(TbCache *cache, uint64_t pc, uint64_t cs_base,
                              uint64_t cr3, uint8_t cpl, uint8_t mode_flags)
{
    uint32_t h = key_hash(pc, cs_base, cr3, cpl, mode_flags) & cache->mask;
    uint32_t idx = cache->buckets[h];

    while (idx != TBC_NO_ENTRY) {
        TbCacheEntry *e = &cache->entries[idx];

        if (key_matches(e, pc, cs_base, cr3, cpl, mode_flags)) {
            return e;
        }
        idx = e->hash_next;
    }
    return 0;
}

TbCacheEntry *tbc_lookup(TbCache *cache, uint64_t pc, uint64_t cs_base,
                         uint64_t cr3, uint8_t cpl)
{
    return tbc_lookup_mode(cache, pc, cs_base, cr3, cpl, 0);
}

/*
 * Pick a slot: a free one if there is one, otherwise the oldest generation.
 * Returns TBC_NO_ENTRY only if every slot is live and the caller asked for a
 * reserve of zero capacity.
 */
static uint32_t pick_slot(TbCache *cache)
{
    uint32_t i, oldest = 0;
    uint32_t oldest_gen = UINT32_MAX;

    for (i = 0; i < cache->capacity; i++) {
        if (!cache->entries[i].used) {
            return i;
        }
    }
    for (i = 0; i < cache->capacity; i++) {
        if (cache->entries[i].gen < oldest_gen) {
            oldest_gen = cache->entries[i].gen;
            oldest = i;
        }
    }
    return oldest;
}

TbCacheEntry *tbc_insert_mode(TbCache *cache, uint64_t pc, uint64_t cs_base,
                              uint64_t cr3, uint8_t cpl, uint8_t mode_flags)
{
    TbCacheEntry *existing = tbc_lookup_mode(cache, pc, cs_base, cr3, cpl, mode_flags);
    TbCacheEntry *e;
    uint32_t idx, h;

    if (existing != 0) {
        return existing;
    }
    if (cache->capacity == 0) {
        return 0;
    }

    idx = pick_slot(cache);
    if (cache->entries[idx].used) {
        /* Replacing a live entry: unlink it before reusing the slot. */
        bucket_unlink(cache, idx);
        cache->count--;
    }

    e = &cache->entries[idx];
    e->pc = pc;
    e->cs_base = cs_base;
    e->cr3 = cr3;
    e->cpl = cpl;
    e->mode_flags = mode_flags;
    e->code = 0;
    e->code_size = 0;
    e->used = 1;
    e->gen = ++cache->generation;

    h = key_hash(pc, cs_base, cr3, cpl, mode_flags) & cache->mask;
    e->hash_next = cache->buckets[h];
    cache->buckets[h] = idx;
    cache->count++;
    return e;
}

TbCacheEntry *tbc_insert(TbCache *cache, uint64_t pc, uint64_t cs_base,
                         uint64_t cr3, uint8_t cpl)
{
    return tbc_insert_mode(cache, pc, cs_base, cr3, cpl, 0);
}

TbCacheEntry *tbc_lookup_mips(TbCache *cache, uint64_t pc, uint32_t asid,
                              uint8_t hflags)
{
    return tbc_lookup_mode(cache, pc, 0, asid, 0, hflags);
}

TbCacheEntry *tbc_insert_mips(TbCache *cache, uint64_t pc, uint32_t asid,
                              uint8_t hflags)
{
    return tbc_insert_mode(cache, pc, 0, asid, 0, hflags);
}

void tbc_invalidate_range(TbCache *cache, uint64_t cr3, uint64_t addr,
                          uint64_t len)
{
    uint64_t first = addr & TBC_PAGE_MASK;
    uint64_t last;

    if (cache == 0 || cache->capacity == 0 || len == 0) {
        return;
    }
    last = (addr + len - 1) & TBC_PAGE_MASK;

    for (uint32_t i = 0; i < cache->capacity; i++) {
        TbCacheEntry *e = &cache->entries[i];
        uint64_t start, end;

        if (!e->used || e->cr3 != cr3) {
            continue;
        }
        start = e->pc & TBC_PAGE_MASK;
        /* Use guest instruction length (guest_size), never host code_size. */
        {
            uint64_t glen = e->guest_size ? (uint64_t)e->guest_size : UINT64_C(1);
            uint64_t last_byte = (e->pc + glen - 1u >= e->pc)
                                     ? (e->pc + glen - 1u)
                                     : UINT64_MAX;
            end = last_byte & TBC_PAGE_MASK;
        }
        if (end < start) {
            end = start;
        }
        /* Overlap of [start,end] with [first,last], page granular. */
        if (start <= last && end >= first) {
            bucket_unlink(cache, i);
            e->used = 0;
            cache->count--;
        }
    }
}

void tbc_invalidate_page(TbCache *cache, uint64_t cr3, uint64_t addr)
{
    tbc_invalidate_range(cache, cr3, addr & TBC_PAGE_MASK, TBC_PAGE_SIZE);
}

void tbc_invalidate_slot(TbCache *cache, uint32_t index)
{
    if (cache == 0 || index >= cache->capacity || !cache->entries[index].used) {
        return;
    }
    bucket_unlink(cache, index);
    cache->entries[index].used = 0;
    cache->count--;
}

void tbc_flush_asid(TbCache *cache, uint64_t cr3)
{
    if (cache == 0 || cache->capacity == 0) {
        return;
    }
    for (uint32_t i = 0; i < cache->capacity; i++) {
        TbCacheEntry *e = &cache->entries[i];

        if (e->used && e->cr3 == cr3) {
            bucket_unlink(cache, i);
            e->used = 0;
            cache->count--;
        }
    }
}

void tbc_flush_all(TbCache *cache)
{
    if (cache == 0 || cache->capacity == 0) {
        return;
    }
    for (uint32_t i = 0; i < cache->capacity; i++) {
        cache->entries[i].used = 0;
        cache->entries[i].hash_next = TBC_NO_ENTRY;
        cache->buckets[i] = TBC_NO_ENTRY;
    }
    cache->count = 0;
}

uint32_t tbc_count(const TbCache *cache)
{
    return cache == 0 ? 0 : cache->count;
}
