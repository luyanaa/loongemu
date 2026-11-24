#ifndef LATR_TB_CACHE_H
#define LATR_TB_CACHE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Per-vCPU translation-block index.
 *
 * Kept free of QEMU/TCG headers on purpose: this is the piece that decides
 * whether an SMP hart may execute cached code, so it must be unit-testable on
 * the host without dragging in the whole translator. The CPUArchState-based
 * key builder lives in latr.h as an inline wrapper over
 * latr_tb_cache_key_parts().
 */

/* A translation is valid for one ASID / privilege / satp mode / RVV config. */
typedef struct LatrTbKey {
    uint32_t satp_mode;
    uint32_t privilege;
    uint32_t asid;
    uint32_t vspec;
    uint64_t root_ppn;
} LatrTbKey;

/*
 * Set-associative index: firmware and Linux have a working set far above a
 * 64-entry direct scan, but a linear scan over a large flat table makes every
 * dispatch O(entries). Four ways per set keeps lookup at four key comparisons
 * while making full-table flushes rare.
 */
#define LATR_TB_CACHE_WAYS 4
#define LATR_TB_CACHE_SETS 128
#define LATR_TB_CACHE_ENTRIES (LATR_TB_CACHE_WAYS * LATR_TB_CACHE_SETS)

typedef struct LatrTbEntry {
    uint64_t guest_pc;
    uint8_t *code;
    uint32_t code_size;
    uint32_t icount;
    LatrTbKey key;
    int valid;
} LatrTbEntry;

/*
 * Hot-region accounting. A "region" for AOT purposes is a guest 4 KiB page:
 * every successful insert (i.e. every translation) charges the page its PC
 * falls in, and a direct-mapped table keeps the busiest pages. This is the
 * measurement P3 needs to pick what to compile ahead of time; the counters are
 * plain bookkeeping and never affect translation or dispatch.
 */
#define LATR_TB_HOT_SLOTS 64

typedef struct LatrTbHotPage {
    uint64_t page;
    uint32_t translations;
    uint8_t valid;
} LatrTbHotPage;

typedef struct LatrTbCache {
    LatrTbEntry entries[LATR_TB_CACHE_ENTRIES];
    uint8_t *arena;
    uint32_t arena_size;
    uint32_t arena_used;
    uint32_t count;
    uint64_t generation;
    /* Round-robin replacement cursor; evicted code stays mapped until the
     * owner reaches a safe point and reclaims the arena. */
    uint32_t victim;
    /* Serializes cross-core shootdowns against the owner's lookup/insert. */
    volatile int lock;
    /* Set by shootdown; the owner resets the arena at its next safe point. */
    uint32_t reclaim_pending;
    LatrTbHotPage hot[LATR_TB_HOT_SLOTS];
} LatrTbCache;

LatrTbKey latr_tb_cache_key_parts(uint64_t satp, uint32_t privilege,
                                  uint32_t vtype, uint32_t vill, uint32_t vl);
void latr_tb_cache_init(LatrTbCache *cache, uint8_t *arena, uint32_t size);
LatrTbEntry *latr_tb_cache_lookup(LatrTbCache *cache, uint64_t pc,
                                  LatrTbKey key);
LatrTbEntry *latr_tb_cache_insert(LatrTbCache *cache, uint64_t pc,
                                  LatrTbKey key, const void *code,
                                  uint32_t code_size, uint32_t icount);
/* Bumps the generation and drops matching entries (ASID 0 drops all). */
void latr_tb_shootdown_all(LatrTbCache *cache);
void latr_tb_shootdown_asid(LatrTbCache *cache, uint32_t asid);
void latr_tb_shootdown_privilege(LatrTbCache *cache, uint32_t privilege);
/*
 * Owner-only reclamation point. Shootdowns only invalidate entries so code
 * another hart is still executing stays mapped; the arena itself is recycled
 * here. Returns true when the arena was actually reset, telling the caller to
 * drop the host-PC -> guest-PC maps that described the old contents.
 */
bool latr_tb_cache_safepoint(LatrTbCache *cache);
void latr_tb_cache_reclaim_all(LatrTbCache *cache);
uint32_t latr_tb_cache_available(const LatrTbCache *cache);

/*
 * Hot-region accounting. `latr_tb_hot_page()` reports the slot's page base and
 * how many translations landed in it since the cache was initialised; slots
 * are direct-mapped on the page number, so a page that collides inherits the
 * slot's count. `latr_tb_hot_busiest()` returns the slot with the highest count
 * (0 when nothing has been translated). Readers do not take the cache lock:
 * the counters are diagnostic and may observe a concurrent update from their
 * own hart's insert, which is why nothing else reads them.
 */
uint32_t latr_tb_hot_page(const LatrTbCache *cache, unsigned slot,
                          uint64_t *page);
uint32_t latr_tb_hot_busiest(const LatrTbCache *cache, uint64_t *page);

/*
 * Cross-core shootdowns: every registered vCPU cache is invalidated. The VMM
 * registers one cache per vCPU and calls these after an SFENCE.VMA / FENCE.I
 * retires on any core.
 */
int latr_tb_cache_register(LatrTbCache *cache);
void latr_tb_cache_unregister(LatrTbCache *cache);
void latr_tb_shootdown_asid_all_cores(uint32_t asid);
void latr_tb_fence_i_all_cores(void);

#endif
