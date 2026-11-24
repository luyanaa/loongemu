#include "loongemu/guest_dma.h"

#include <string.h>

void loongemu_guest_dma_init(LoongemuGuestDma *dma, const LoongemuMemMap *map,
                             LoongemuGuestWriteNotify notify, void *opaque)
{
    if (dma == NULL) {
        return;
    }
    dma->map = map;
    dma->notify = notify;
    dma->notify_opaque = opaque;
    dma->write_lock = 0;
    dma->write_gpa = 0;
    dma->write_len = 0;
    dma->write_active = 0;
    for (unsigned i = 0; i < LOONGEMU_GUEST_DMA_CACHE_SLOTS; i++) {
        dma->cache[i].valid = 0;
    }
}

bool loongemu_guest_dma_ptr(const LoongemuGuestDma *dma, uint64_t gpa,
                            uint64_t len, uint8_t **out)
{
    LoongemuMemLookup lookup;
    LoongemuGuestDmaEntry *entry;

    if (out != NULL) {
        *out = NULL;
    }
    if (dma == NULL || dma->map == NULL || len == 0 || out == NULL) {
        return false;
    }
    /*
     * Reject a range that wraps before any fast-path arithmetic can see it. The
     * slow path (loongemu_mem_resolve) checks the same thing, but the cache hit
     * below it did not, so a valid entry plus a huge len used to make
     * `gpa + len` wrap and pass the containment test - and the caller then
     * memcpy'd past the region.
     */
    if (gpa > UINT64_MAX - len) {
        return false;
    }
    /*
     * The cache is keyed by page base, so a descriptor that walks a buffer in
     * 4 KiB steps resolves the region once per page instead of once per chunk.
     * Every hit is validated against the map generation; a region add/remove
     * therefore retires the whole cache instead of returning a stale HPA.
     */
    entry = (LoongemuGuestDmaEntry *)&dma->cache[(gpa >> 12) &
                                                 (LOONGEMU_GUEST_DMA_CACHE_SLOTS - 1)];
    {
        uint64_t page = gpa & ~UINT64_C(0xfff);

        /*
         * Subtraction form: containment is expressed without adding to `gpa`, so
         * it cannot wrap even if the guard above is ever relaxed.
         */
        if (entry->valid && entry->generation == dma->map->generation &&
            gpa >= entry->gpa) {
            uint64_t offset = gpa - entry->gpa;

            if (offset <= entry->span && len <= entry->span - offset) {
                *out = (uint8_t *)(uintptr_t)(entry->hpa + offset);
                return true;
            }
        }
        if (loongemu_mem_resolve(dma->map, gpa, len, &lookup) !=
            LOONGEMU_MEM_RAM) {
            entry->valid = 0;
            return false;
        }
        *out = (uint8_t *)(uintptr_t)lookup.hpa;
        {
            uint64_t region_start = gpa - lookup.offset;
            uint64_t region_end = gpa + lookup.avail;
            uint64_t lo = region_start > page ? region_start : page;
            uint64_t hi = region_end < page + UINT64_C(0x1000)
                              ? region_end
                              : page + UINT64_C(0x1000);

            if (gpa < lo || gpa + len > hi) {
                entry->valid = 0; /* page straddles a region edge */
                return true;
            }
            entry->gpa = lo;
            entry->hpa = lookup.hpa - (gpa - lo);
            entry->span = hi - lo;
            entry->generation = dma->map->generation;
            entry->valid = 1;
        }
        return true;
    }
}

int loongemu_guest_dma_read(const LoongemuGuestDma *dma, uint64_t gpa,
                            void *buf, uint64_t len)
{
    uint8_t *host;

    if (buf == NULL || !loongemu_guest_dma_ptr(dma, gpa, len, &host)) {
        return -1;
    }
    memcpy(buf, host, (size_t)len);
    return 0;
}

static void dma_notify(LoongemuGuestDma *dma, uint64_t gpa, uint64_t len)
{
    if (dma->notify != NULL) {
        dma->notify(dma->notify_opaque, gpa, len);
    }
}

static int dma_lock(LoongemuGuestDma *dma)
{
    return __atomic_exchange_n(&dma->write_lock, 1, __ATOMIC_ACQUIRE) == 0;
}

static void dma_unlock(LoongemuGuestDma *dma)
{
    __atomic_store_n(&dma->write_lock, 0, __ATOMIC_RELEASE);
}

int loongemu_guest_write_begin(LoongemuGuestDma *dma, uint64_t gpa,
                               uint64_t len, uint8_t **host)
{
    if (dma == NULL || !dma_lock(dma)) {
        return -1;
    }
    if (!loongemu_guest_dma_ptr(dma, gpa, len, host)) {
        dma_unlock(dma);
        return -1;
    }
    dma->write_gpa = gpa;
    dma->write_len = len;
    dma->write_active = 1;
    return 0;
}

int loongemu_guest_write_commit(LoongemuGuestDma *dma, uint64_t gpa,
                                const void *data, uint64_t len)
{
    uint8_t *host;

    if (dma == NULL || data == NULL || len == 0 || !dma->write_active) {
        return -1;
    }
    if (gpa < dma->write_gpa || len > dma->write_len ||
        gpa - dma->write_gpa > dma->write_len - len) {
        return -1; /* outside the range this transaction reserved */
    }
    if (!loongemu_guest_dma_ptr(dma, gpa, len, &host)) {
        return -1;
    }
    memcpy(host, data, (size_t)len);
    return 0;
}

void loongemu_guest_write_end(LoongemuGuestDma *dma)
{
    uint64_t gpa;
    uint64_t len;

    if (dma == NULL || !dma->write_active) {
        return;
    }
    gpa = dma->write_gpa;
    len = dma->write_len;
    dma->write_active = 0;
    dma->write_len = 0;
    /* Notify while still holding the lock: the store and its reservation
     * invalidation are one critical section. */
    dma_notify(dma, gpa, len);
    dma_unlock(dma);
}

int loongemu_guest_dma_write(LoongemuGuestDma *dma, uint64_t gpa,
                             const void *buf, uint64_t len)
{
    uint8_t *host;

    if (buf == NULL || len == 0 ||
        loongemu_guest_write_begin(dma, gpa, len, &host) != 0) {
        return -1;
    }
    memcpy(host, buf, (size_t)len);
    loongemu_guest_write_end(dma);
    return 0;
}

int loongemu_guest_dma_fill(LoongemuGuestDma *dma, uint64_t gpa, uint8_t value,
                            uint64_t len)
{
    uint8_t *host;

    if (len == 0 || loongemu_guest_write_begin(dma, gpa, len, &host) != 0) {
        return -1;
    }
    memset(host, value, (size_t)len);
    loongemu_guest_write_end(dma);
    return 0;
}

int loongemu_guest_dma_write_le(LoongemuGuestDma *dma, uint64_t gpa,
                                uint64_t value, unsigned size)
{
    uint8_t bytes[8];

    if (size == 0 || size > sizeof(bytes)) {
        return -1;
    }
    for (unsigned i = 0; i < size; i++) {
        bytes[i] = (uint8_t)(value >> (8u * i));
    }
    return loongemu_guest_dma_write(dma, gpa, bytes, size);
}
