/*
 * LATA multi-TB dispatch loop: cache emitted host code by (pc, address
 * space, exception level) and follow env.pc until the embedder halts.
 */
#include "loongemu/lata_dispatch.h"

#include <stddef.h>

static bool is_pow2(uint32_t v)
{
    return v != 0 && (v & (v - 1u)) == 0;
}

bool lata_dispatch_init(LataDispatch *d, const LataDispatchOps *ops,
                        void *opaque, TbCacheEntry *entries,
                        uint32_t *buckets, uint32_t capacity,
                        uint8_t *arena, uint32_t arena_size)
{
    if (d == NULL || ops == NULL || entries == NULL || buckets == NULL ||
        arena == NULL) {
        return false;
    }
    if (!is_pow2(capacity) || arena_size == 0) {
        return false;
    }
    if (ops->translate == NULL || ops->enter == NULL || ops->next_pc == NULL ||
        ops->address_space == NULL || ops->mapping_epoch == NULL ||
        ops->exception_level == NULL || ops->halt == NULL) {
        return false;
    }
    d->ops = ops;
    d->opaque = opaque;
    d->entries = entries;
    d->buckets = buckets;
    d->capacity = capacity;
    d->arena = arena;
    d->arena_size = arena_size;
    d->arena_used = 0;
    d->translated = 0;
    d->executed = 0;
    d->cache_hits = 0;
    d->reclaims = 0;
    d->refused = 0;
    if (tbc_init(&d->cache, entries, buckets, capacity) == 0) {
        return false;
    }
    return true;
}

void lata_dispatch_flush(LataDispatch *d)
{
    if (d != NULL) {
        tbc_flush_all(&d->cache);
        /* The arena is only reclaimable together with the cache: every entry
         * points into it. Callers must not be executing out of it, which is why
         * the loop only flushes between TBs. */
        d->arena_used = 0;
    }
}

/*
 * Make room for one more TB: drop the cache, then reuse the arena from zero.
 * Returns false when the arena is already empty, i.e. a single TB does not fit
 * and flushing cannot help.
 */
static bool lata_dispatch_reclaim(LataDispatch *d)
{
    if (d->arena_used == 0) {
        return false;
    }
    tbc_flush_all(&d->cache);
    d->arena_used = 0;
    d->reclaims++;
    return true;
}

uint64_t lata_dispatch_run(LataDispatch *d)
{
    if (d == NULL) {
        return 0;
    }
    while (!d->ops->halt(d->opaque)) {
        uint64_t pc = d->ops->next_pc(d->opaque);
        uint64_t asid = d->ops->address_space(d->opaque);
        uint64_t mapping_epoch = d->ops->mapping_epoch(d->opaque);
        uint8_t el = d->ops->exception_level(d->opaque);

        /* AArch64 has no CS base, so retain the full epoch in that key slot
         * while leaving the TTBR in cr3. */
        TbCacheEntry *e = tbc_lookup_mode(&d->cache, pc, mapping_epoch, asid,
                                          el, 0);
        void *code;
        if (e != NULL) {
            d->cache_hits++;
            code = (void *)(uintptr_t)e->code;
        } else {
            /* Emit into the arena, aligned so the next TB starts clean. */
            uint32_t off = (d->arena_used + 15u) & ~15u;
            if (off >= d->arena_size) {
                /* Out of arena: reclaim and retry once. A TB larger than the
                 * whole arena cannot be helped by a flush, so stop then. */
                if (!lata_dispatch_reclaim(d)) {
                    break;
                }
                continue;
            }
            code = d->arena + off;
            uint32_t capacity = d->arena_size - off;
            TbEmitResult result = {0};
            if (d->ops->translate(d->opaque, pc, code, capacity, &result) != 0 ||
                result.code_size == 0) {
                break;
            }
            /*
             * The translator promised the size it used. Trusting it blindly is
             * exactly how a bump allocator turns into an out-of-bounds write,
             * so a size that exceeds the reservation is refused, counted, and
             * the loop stops rather than caching a lie.
             */
            if (result.code_size > capacity) {
                d->refused++;
                break;
            }
            if (d->ops->sync_icache != NULL) {
                d->ops->sync_icache(d->opaque, code, result.code_size);
            }
            uint32_t bytes = result.code_size;
            d->translated++;
            e = tbc_insert_mode(&d->cache, pc, mapping_epoch, asid, el, 0);
            if (e == NULL) {
                /* Cache full and all slots live: still runnable, just not
                 * remembered. Executed below without caching. */
                d->arena_used = off + ((bytes + 15u) & ~15u);
                d->ops->enter(d->opaque, pc, code);
                d->executed++;
                continue;
            }
            e->code = (uint64_t)(uintptr_t)code;
            e->code_size = bytes;
            d->arena_used = off + ((bytes + 15u) & ~15u);
        }
        d->ops->enter(d->opaque, pc, code);
        d->executed++;
    }
    return d->executed;
}
