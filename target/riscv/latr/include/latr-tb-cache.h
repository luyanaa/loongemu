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
 *
 * Eight ways (1024 entries, still fitting the 4 MiB arena) is an open question,
 * not a rejected one. One run reached 9.28M entries on 68,608 translations
 * (7.4 per million) where four ways needed 63,382 for 5.72M (11.1 per million),
 * i.e. the translation *rate* clearly falls; but paired end-to-end runs could
 * not be compared because the host load swung the distance travelled by ~40%
 * between identically-built runs. Measure it on a quiet host: build with PERF=1,
 * run both geometries for the same wall time, and compare tbs, not the tick
 * counters -- those are per-phase and a mid-run snapshot matches a different
 * phase than another run's final one.
 */
#define LATR_TB_CACHE_WAYS 4
#define LATR_TB_CACHE_SETS 128
#define LATR_TB_CACHE_ENTRIES (LATR_TB_CACHE_WAYS * LATR_TB_CACHE_SETS)

/*
 * Guest code bytes kept per block so a fence.i can tell "this block's source
 * changed" from "some unrelated page was patched".
 *
 * A block's translation is a function of its guest bytes and its cache key
 * (pc, cs_base, cr3, cpl) -- anything else it specializes on is already part of
 * the key. Comparing the bytes is therefore an exact test, not a heuristic, and
 * it needs no write tracking: the emitted fast path stores straight to guest RAM
 * and is not visible from C.
 *
 * 32 bytes covers 8 32-bit instructions, which is most blocks; a longer block is
 * marked unverifiable and is still dropped on every fence.i, i.e. it keeps the
 * old conservative behaviour.
 */
#define LATR_TB_CODE_SNAP 32

typedef struct LatrTbEntry {
    uint64_t guest_pc;
    uint8_t *code;
    uint32_t code_size;
    uint32_t icount;
    LatrTbKey key;
    uint16_t guest_size;   /* guest code bytes covered, 0 = not recorded */
    uint8_t verifiable;    /* 1 when snap[] holds all guest_size bytes */
    uint8_t snap[LATR_TB_CODE_SNAP];
    int valid;
} LatrTbEntry;

/*
 * Drops the blocks whose guest code changed. `same` answers "are this entry's
 * bytes still exactly what it was translated from"; a false answer, a faulted
 * read or an unverifiable entry all mean "drop". `on_drop` receives the code
 * address of each dropped entry so the caller can release its recovery map,
 * which the cache itself does not own.
 */
typedef int (*LatrTbSameCode)(void *ctx, const LatrTbEntry *entry);
typedef void (*LatrTbOnDrop)(void *ctx, uintptr_t code);


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
    /*
     * Code address of the entry the last insert evicted, 0 when it took a free
     * slot. The caller drops that block's recovery map: the bytes stay mapped
     * but the entry is unreachable, and keeping one ~22 KiB map per evicted
     * block is what exhausts the freestanding allocator.
     */
    uintptr_t evicted_code;
    /*
     * Fence-completion bookkeeping (design: docs/design/latr-smp-concurrency.md
     * section 5). `applied_seq` is the last shootdown sequence this hart has
     * observed while outside generated code; `in_code` counts live executions.
     * Both are written only by the owning hart and read by a fence issuer.
     */
    volatile uint64_t applied_seq;
    volatile uint32_t in_code;
    /*
     * Wake target for this hart. A hart parked in WFI cannot acknowledge a
     * fence on its own: the issuer must bump its wake word so it re-enters the
     * dispatcher and acks. Without this every SFENCE.VMA issued while any hart
     * is idle waits out its bound and fails the machine.
     */
    struct LatrHartSync *park;
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
/* Caller guarantees no concurrent mutator of `cache` (see the definition). */
uint32_t latr_tb_cache_drop_stale(LatrTbCache *cache, LatrTbSameCode same,
                                  void *same_ctx, LatrTbOnDrop on_drop,
                                  void *drop_ctx);
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
/*
 * Fence completion.
 *
 * A shootdown is not complete when the issuer has invalidated every cache: a
 * hart may already be inside a block that was fetched before the request. The
 * issuer publishes a sequence number with latr_tb_fence_begin(), and every hart
 * calls latr_tb_fence_ack() at its next safe point (between TBs). Waiting for
 * all online harts is bounded so a hart that never reaches a safe point fails
 * the machine instead of retiring a fence that is not visible.
 *
 * The bound is a spin count rather than a wall-clock deadline so this module
 * stays usable in the freestanding build (no clock source); the threaded
 * scheduler picks it from LATR_SMP_ACK_SPINS (design section 8).
 */
void latr_tb_cache_set_park(LatrTbCache *cache, struct LatrHartSync *park);
uint64_t latr_tb_fence_begin(uint32_t asid, int fence_i);
void latr_tb_fence_ack(LatrTbCache *cache);
void latr_tb_code_enter(LatrTbCache *cache);
void latr_tb_code_exit(LatrTbCache *cache);
uint64_t latr_tb_fence_seq(void);
/* 0 = every registered hart has acknowledged, -1 = spin budget exhausted. */
int latr_tb_fence_wait(uint64_t seq, uint64_t spin_limit);
uint64_t latr_tb_fence_timeouts(void);
/* Hart that was furthest behind at the last timeout (diagnostics). */
uint64_t latr_tb_fence_last_missing(void);

int latr_tb_cache_register(LatrTbCache *cache);
void latr_tb_cache_unregister(LatrTbCache *cache);
void latr_tb_shootdown_asid_all_cores(uint32_t asid);
void latr_tb_fence_i_all_cores(void);
/*
 * fence.i for the hart that issued it: verify that hart's cache block by block
 * and drop only the stale ones, then fall back to a full shootdown for every
 * other hart (their bytes can only be compared by their own owner). Bumps the
 * same completion sequence as latr_tb_fence_begin(0, 1), so the acknowledgement
 * protocol is unchanged.
 */
uint64_t latr_tb_fence_i_local(LatrTbCache *self, LatrTbSameCode same,
                               void *same_ctx, LatrTbOnDrop on_drop,
                               void *drop_ctx);

#endif
