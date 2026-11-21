/*
 * Translation-block cache.
 *
 * The single-block smoke harness rebuilt one hand-made TB and patched its
 * exit with a hard-coded jump.  A real run needs a keyed table instead: the
 * same guest PC means different code when the address space, the segment
 * base or the privilege level differs, and a self-modifying guest must be
 * able to drop the entries for a page.
 *
 * The table is fixed-size and allocation-free so it can live in a bare-metal
 * pool, and it stores only what the executor needs to resume: the key, the
 * code address and its size.  Everything else stays in the TranslationBlock
 * the translator filled in.
 *
 * Keys:
 *   pc       guest instruction pointer
 *   cs_base  segment base the TB was translated against
 *   cr3      page-table root, i.e. the address space
 *   cpl      privilege level the code was translated for
 *
 * A hit requires all four to match.  tbc_invalidate_page() drops every block
 * that overlaps a guest page (SMC and INVLPG), and tbc_flush_asid() drops
 * everything for an address space (a CR3 write).
 */
#ifndef LOONGEMU_TB_CACHE_H
#define LOONGEMU_TB_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TBC_NO_ENTRY UINT32_C(0xffffffff)

typedef struct TbCacheEntry {
    uint64_t pc;
    uint64_t cs_base;
    uint64_t cr3;
    uint64_t code;          /* translated code address */
    uint32_t code_size;     /* bytes of emitted host code */
    uint32_t guest_size;    /* bytes of guest x86 instructions covered */
    uint32_t hash_next;     /* next entry in the same bucket */
    uint32_t gen;           /* insertion generation, for eviction */
    uint8_t  cpl;
    uint8_t  mode_flags;    /* codemode (16/32/64) | PE | CS32 */
    uint8_t  used;
} TbCacheEntry;

typedef struct TbCache {
    TbCacheEntry *entries;
    uint32_t *buckets;
    uint32_t capacity;      /* number of entry slots, a power of two */
    uint32_t mask;          /* capacity - 1 */
    uint32_t count;         /* live entries */
    uint32_t generation;
} TbCache;

#define TBC_PAGE_BITS 12
#define TBC_PAGE_SIZE (UINT64_C(1) << TBC_PAGE_BITS)
#define TBC_PAGE_MASK (~(TBC_PAGE_SIZE - 1))

/*
 * capacity must be a power of two and at least 2.  storage must hold
 * capacity entries and capacity bucket heads.  Returns 0 on a bad geometry.
 */
int tbc_init(TbCache *cache, TbCacheEntry *storage, uint32_t *bucket_storage,
             uint32_t capacity);

/* Exact-key lookup (default mode_flags=0); NULL when absent. */
TbCacheEntry *tbc_lookup(TbCache *cache, uint64_t pc, uint64_t cs_base,
                         uint64_t cr3, uint8_t cpl);

/* Mode-aware exact-key lookup separating 16/32/64-bit TBs at the same PC. */
TbCacheEntry *tbc_lookup_mode(TbCache *cache, uint64_t pc, uint64_t cs_base,
                              uint64_t cr3, uint8_t cpl, uint8_t mode_flags);

/*
 * Insert or replace a key.  Returns the entry, or NULL when the table is
 * full and nothing could be evicted (callers should then flush).
 */
TbCacheEntry *tbc_insert(TbCache *cache, uint64_t pc, uint64_t cs_base,
                         uint64_t cr3, uint8_t cpl);

TbCacheEntry *tbc_insert_mode(TbCache *cache, uint64_t pc, uint64_t cs_base,
                              uint64_t cr3, uint8_t cpl, uint8_t mode_flags);

/* MIPS key: PC + CP0 ASID + execution mode/hflags. */
TbCacheEntry *tbc_lookup_mips(TbCache *cache, uint64_t pc, uint32_t asid,
                              uint8_t hflags);
TbCacheEntry *tbc_insert_mips(TbCache *cache, uint64_t pc, uint32_t asid,
                              uint8_t hflags);

/* Drop every block overlapping [addr, addr+len) in the given address space. */
void tbc_invalidate_range(TbCache *cache, uint64_t cr3, uint64_t addr,
                          uint64_t len);

/* Drop a single slot by index. */
void tbc_invalidate_slot(TbCache *cache, uint32_t index);

/* Drop every block of one address space (CR3 write). */
void tbc_flush_asid(TbCache *cache, uint64_t cr3);

/* Drop everything. */
void tbc_flush_all(TbCache *cache);

/* Number of live entries, for the bare-metal smoke output. */
uint32_t tbc_count(const TbCache *cache);

#ifdef __cplusplus
}
#endif

#endif
