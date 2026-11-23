/*
 * LATA multi-TB dispatch loop.
 *
 * The ISA harness translates and executes one translation block per call. A
 * real AArch64 program chains control flow across many TBs, so the loop
 * below keeps a keyed cache (pc + address space + exception level) of
 * already-emitted host code and only re-translates on a miss.
 *
 * translation-table root (TTBR) in the `cr3` slot, and the mapping epoch in
 * the otherwise-unused `cs_base` slot (AArch64 has no segment base). A hit
 * therefore cannot confuse different address spaces, ELs, or direct mappings.
 *
 * The caller owns the code arena, the cache storage, and the LATA plumbing
 * (tr_init/tr_ir2_generate/tr_ir2_assemble/lata_enter_tb). The loop itself
 * stays freestanding and allocation-free.
 */
#ifndef LOONGEMU_LATA_DISPATCH_H
#define LOONGEMU_LATA_DISPATCH_H

#include <stdint.h>
#include <stdbool.h>
#include "loongemu/tb_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where a new TB's host code is emitted and how the loop drives it. The
 * embedder supplies these; the dispatch loop calls them in order. */
typedef struct LataDispatchOps {
    /* Fill `tb` for guest `pc` (disasm + generate), assemble into
     * `code_addr`, and return emitted byte count, or 0 on failure. Must not
     * advance the arena itself; the loop bumps it by the returned size. */
    uint32_t (*translate)(void *opaque, uint64_t pc, void *code_addr);
    /* Enter previously-emitted host code for `pc`; returns when the TB
     * exits. `env.pc` afterwards is the next guest PC to dispatch. */
    void (*enter)(void *opaque, uint64_t pc, void *code_addr);
    /* Read the next guest PC after a TB exits. */
    uint64_t (*next_pc)(void *opaque);
    /* Translation-table root + exception level that qualify a cached TB.
     * Map TTBR to cr3 so equal keys never alias across address spaces. */
    uint64_t (*address_space)(void *opaque);
    /* Current direct-memory mapping epoch; return 0 when no direct policy is
     * installed. It is keyed in cs_base without truncation. */
    uint64_t (*mapping_epoch)(void *opaque);
    uint8_t (*exception_level)(void *opaque);
    /* Stop condition: return true to end the loop (e.g. env.pc==halt, or a
     * budget of dispatched TBs exhausted). */
    bool (*halt)(void *opaque);
} LataDispatchOps;

typedef struct LataDispatch {
    const LataDispatchOps *ops;
    void *opaque;
    TbCache cache;
    /* Caller-owned storage, power-of-two capacity. */
    TbCacheEntry *entries;
    uint32_t *buckets;
    uint32_t capacity;
    /* Code arena: bump allocator, no reclaim for now (S6 lifecycle). */
    uint8_t *arena;
    uint32_t arena_size;
    uint32_t arena_used;
    /* Counters for the perf baseline and tests. */
    uint64_t translated;    /* TBs actually disassembled+generated */
    uint64_t executed;      /* TBs entered (hits + misses) */
    uint64_t cache_hits;    /* served from cache without re-translating */
} LataDispatch;

/* Init with caller-owned, zeroed storage. `capacity` must be a power of two
 * and match the entry/bucket array extents. Returns false on bad geometry. */
bool lata_dispatch_init(LataDispatch *d, const LataDispatchOps *ops,
                        void *opaque, TbCacheEntry *entries,
                        uint32_t *buckets, uint32_t capacity,
                        uint8_t *arena, uint32_t arena_size);

/* Run the dispatch loop until `ops->halt` or the arena/cache exhausts.
 * Returns the number of TBs executed. */
uint64_t lata_dispatch_run(LataDispatch *d);

/* Drop all cached TBs (e.g. after an address-space switch or SMC). */
void lata_dispatch_flush(LataDispatch *d);

#ifdef __cplusplus
}
#endif
#endif /* LOONGEMU_LATA_DISPATCH_H */
