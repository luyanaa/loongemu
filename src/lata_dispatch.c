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
        ops->address_space == NULL || ops->exception_level == NULL ||
        ops->halt == NULL) {
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
    if (tbc_init(&d->cache, entries, buckets, capacity) == 0) {
        return false;
    }
    return true;
}

void lata_dispatch_flush(LataDispatch *d)
{
    if (d != NULL) {
        tbc_flush_all(&d->cache);
    }
}

uint64_t lata_dispatch_run(LataDispatch *d)
{
    if (d == NULL) {
        return 0;
    }
    while (!d->ops->halt(d->opaque)) {
        uint64_t pc = d->ops->next_pc(d->opaque);
        uint64_t asid = d->ops->address_space(d->opaque);
        uint8_t el = d->ops->exception_level(d->opaque);

        TbCacheEntry *e = tbc_lookup_mode(&d->cache, pc, 0, asid, el, 0);
        void *code;
        if (e != NULL) {
            d->cache_hits++;
            code = (void *)(uintptr_t)e->code;
        } else {
            /* Emit into the arena, aligned so the next TB starts clean. */
            uint32_t off = (d->arena_used + 15u) & ~15u;
            if (off >= d->arena_size) {
                /* No room for a new TB; stop rather than corrupt the arena. */
                break;
            }
            code = d->arena + off;
            uint32_t bytes = d->ops->translate(d->opaque, pc, code);
            if (bytes == 0) {
                break;
            }
            d->translated++;
            e = tbc_insert_mode(&d->cache, pc, 0, asid, el, 0);
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
