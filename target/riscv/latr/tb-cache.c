/*
 * Per-vCPU TB cache index. The runtime owns translation and the code arena;
 * this module only answers "is this PC translated for this ASID/privilege/
 * satp mode" and applies shootdowns. Entries are never reused for a different
 * key, which is what makes privilege switches and satp rewrites safe.
 */
#include "latr.h"
#include "mmu.h"

#define SATP_ASID_SHIFT 44
#define SATP_MODE_SHIFT 60
#define SATP_ASID_MASK  UINT64_C(0xffff)

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
}

LatrTbKey latr_tb_cache_key(const CPUArchState *env)
{
    LatrTbKey key = { 0, 0, 0, 0, 0 };
    uint64_t satp;

    if (env == 0) {
        return key;
    }
    satp = env->csr[0x180];
    key.satp_mode = (uint32_t)(satp >> SATP_MODE_SHIFT);
    key.asid = (uint32_t)((satp >> SATP_ASID_SHIFT) & SATP_ASID_MASK);
    key.privilege = (uint32_t)env->privilege;
    key.vspec = (uint32_t)((env->vtype & 0x3ffu) | ((env->vill & 1u) << 10) |
                           (((uint32_t)env->vl & 0xffu) << 16));
    key.root_ppn = satp & LATR_PTE_PPN_MASK;
    return key;
}

static int key_equal(const LatrTbKey *a, const LatrTbKey *b)
{
    return a->satp_mode == b->satp_mode && a->privilege == b->privilege &&
           a->asid == b->asid && a->vspec == b->vspec &&
           a->root_ppn == b->root_ppn;
}

LatrTbEntry *latr_tb_cache_lookup(LatrTbCache *cache, uint64_t pc,
                                  LatrTbKey key)
{
    if (cache == 0) {
        return 0;
    }
    for (int i = 0; i < LATR_TB_CACHE_ENTRIES; i++) {
        LatrTbEntry *entry = &cache->entries[i];
        if (entry->valid && entry->guest_pc == pc &&
            key_equal(&entry->key, &key)) {
            return entry;
        }
    }
    return 0;
}

LatrTbEntry *latr_tb_cache_insert(LatrTbCache *cache, uint64_t pc,
                                  LatrTbKey key, const void *code,
                                  uint32_t code_size, uint32_t icount)
{
    if (cache == 0 || cache->arena == 0 || code_size == 0 ||
        cache->arena_used + code_size > cache->arena_size) {
        return 0;
    }
    int slot = -1;
    for (int i = 0; i < LATR_TB_CACHE_ENTRIES; i++) {
        if (!cache->entries[i].valid) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return 0; /* full: the runtime must flush or grow the cache */
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
    return entry;
}

void latr_tb_shootdown_all(LatrTbCache *cache)
{
    if (cache == 0) {
        return;
    }
    for (int i = 0; i < LATR_TB_CACHE_ENTRIES; i++) {
        cache->entries[i].valid = 0;
    }
    cache->count = 0;
    cache->arena_used = 0; /* the whole arena becomes reusable */
    cache->generation++;
    latr_pvir_cache_clear();
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
    latr_pvir_cache_clear();
}

void latr_tb_shootdown_privilege(LatrTbCache *cache, uint32_t privilege)
{
    if (cache == 0) {
        return;
    }
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
    latr_pvir_cache_clear();
}

void latr_set_cpu_env(CPUArchState *env)
{
    if (lsenv != 0) {
        lsenv->cpu_state = env;
    }
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
