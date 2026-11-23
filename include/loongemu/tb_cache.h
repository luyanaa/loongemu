/*
 * Translation-block cache.
 *
 * The table is a fixed-size, allocation-free lookup index.  It stores the
 * translation key and emitted-code address; the caller owns the code arena
 * and the lifetime of the bytes at that address.
 *
 * Keys:
 *   pc       guest instruction pointer
 *   cs_base  segment base the TB was translated against
 *   cr3      page-table root, i.e. the address space
 *   cpl      privilege level the code was translated for
 *
 * Lifecycle owners:
 *   allocate   the translation caller reserves arena space and emits code;
 *   publish    tbc_insert* makes the key visible; LATA tb_link_page records
 *              the TB's physical-page membership;
 *   execute    the dispatcher enters code obtained from a successful lookup;
 *   invalidate tbc_invalidate_* removes keys, while LATA tb_invalidate_page
 *              and tb_flush mark page-linked TBs invalid;
 *   unlink     before retirement, the LATA runtime drains incoming
 *              jmp_list_head links and resets outgoing branches with the
 *              resolved jmp_reset_offset (tb_reset_jump);
 *   retire     invalidation removes a TB from lookup/page indexes but does
 *              not free or reuse its emitted code;
 *   reclaim    only the code-arena owner may reuse bytes after all execution
 *              has left the arena (a safe point / quiescent state).
 *
 * TbCache itself is not synchronized and never owns/reclaims code. Callers
 * serialize lookup, publish and invalidation, and must not rewind their code
 * arena merely because an entry was invalidated.
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
/* Drop every block overlapping the guest page containing addr. */
void tbc_invalidate_page(TbCache *cache, uint64_t cr3, uint64_t addr);

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
