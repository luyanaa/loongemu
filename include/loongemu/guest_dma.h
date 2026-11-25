#ifndef LOONGEMU_GUEST_DMA_H
#define LOONGEMU_GUEST_DMA_H

#include <stdbool.h>
#include <stdint.h>

#include "loongemu/phys_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Device-side DMA into guest RAM.
 *
 * A DMA store is a guest-memory write performed outside the CPU store path.
 * The guest's LR/SC reservation is only invalidated when the shared write
 * epoch advances, so a device that writes guest memory must notify the
 * reservation domain for the touched range -- otherwise a later SC can still
 * "succeed" against a value the device has already replaced.
 *
 * Every device write goes through this interface, and the notification lives
 * inside loongemu_guest_dma_write() rather than at each call site. That keeps
 * the invariant structural: a device cannot add a memcpy() to guest RAM
 * without going through this header first, and cannot forget the notify.
 *
 * Only LOONGEMU_MEM_RAM ranges are reachable. A descriptor pointing at an
 * MMIO or unmapped page is rejected instead of silently writing somewhere
 * else, because device DMA has no MMU and no fault path of its own.
 */
typedef void (*LoongemuGuestWriteNotify)(void *opaque, uint64_t gpa,
                                         uint64_t len);

/*
 * One remembered GPA -> HPA translation. A device walks descriptors page after
 * page, so the same region lookup would otherwise run for every chunk; the
 * cache keeps the last translation and the `map->generation` it was made under,
 * which makes a stale entry unusable rather than dangerous.
 */
#define LOONGEMU_GUEST_DMA_CACHE_SLOTS 4

typedef struct LoongemuGuestDmaEntry {
    uint64_t gpa;
    uint64_t hpa;
    uint64_t span;   /* bytes from gpa that stay inside the same RAM region */
    uint32_t generation;
    uint8_t valid;
} LoongemuGuestDmaEntry;

typedef struct LoongemuGuestDma {
    const LoongemuMemMap *map;
    /*
     * Coarse write-transaction lock.
     *
     * A device write has two observable steps: the bytes become visible, and the
     * CPU-side reservation domain is told the range changed. Writing first and
     * notifying second leaves a window in which an SC can still succeed against
     * data the device already replaced. Every writer therefore runs inside
     * begin()..end(), with the notification issued at end() while the lock is
     * still held.
     *
     * CPU stores and AMOs must adopt the same contract before the machine runs
     * vCPUs in parallel; today they are serialised by the cooperative
     * dispatcher, so this lock only orders device writes against each other.
     */
    volatile int write_lock;
    uint64_t write_gpa;
    uint64_t write_len;
    int write_active;
    /*
     * Domain hooks: when installed, a device write takes the *same* critical
     * section as a CPU store (the embedder's write domain) instead of the
     * private `write_lock` above. The acquire may block: no CPU store path holds
     * the domain across a device call (the emitted fast path calls MMIO outside
     * it, and the C misaligned path was moved to match), so a device that DMAs
     * synchronously cannot deadlock on it. Nested device writes are refused by
     * `write_active`, not by a non-recursive lock.
     */
    uint64_t (*lock)(void *opaque);
    void (*unlock)(void *opaque, uint64_t token);
    void *lock_opaque;
    uint64_t write_token;
    /* May be NULL: a host-side unit test has no reservation domain. In the
     * VMM this is latr_reservation_notify_write() wrapped by the machine. */
    LoongemuGuestWriteNotify notify;
    void *notify_opaque;
    LoongemuGuestDmaEntry cache[LOONGEMU_GUEST_DMA_CACHE_SLOTS];
} LoongemuGuestDma;

void loongemu_guest_dma_init(LoongemuGuestDma *dma, const LoongemuMemMap *map,
                             LoongemuGuestWriteNotify notify, void *opaque);
/*
 * Serialises device writes against CPU stores through the embedder's write
 * domain (roadmap 3.2): the data store and the reservation invalidation are one
 * critical section, and a CPU store cannot interleave. Passing NULLs restores
 * the private device-only lock.
 */
void loongemu_guest_dma_set_write_domain(LoongemuGuestDma *dma,
                                         uint64_t (*lock)(void *opaque),
                                         void (*unlock)(void *opaque,
                                                        uint64_t token),
                                         void *opaque);

/*
 * Resolve [gpa, gpa+len) to a host pointer inside guest RAM. Returns false for
 * zero length, a hole, an MMIO window, or a range that straddles two regions.
 * The pointer is for the device's own bookkeeping: any store through it would
 * bypass the write notification, so writers must use the helpers below.
 */
bool loongemu_guest_dma_ptr(const LoongemuGuestDma *dma, uint64_t gpa,
                            uint64_t len, uint8_t **out);

int loongemu_guest_dma_read(const LoongemuGuestDma *dma, uint64_t gpa,
                            void *buf, uint64_t len);
int loongemu_guest_dma_write(LoongemuGuestDma *dma, uint64_t gpa,
                             const void *buf, uint64_t len);
int loongemu_guest_dma_fill(LoongemuGuestDma *dma, uint64_t gpa, uint8_t value,
                            uint64_t len);

/* Little-endian store of `size` (1..8) bytes; notifies like any other write. */
int loongemu_guest_dma_write_le(LoongemuGuestDma *dma, uint64_t gpa,
                                uint64_t value, unsigned size);

/*
 * Write transaction: begin resolves and locks, the caller stores through the
 * returned pointer (or uses commit()), and end() notifies the reservation domain
 * and releases. begin() fails if another transaction is open on this domain, so
 * a nested use is a bug the caller sees instead of a silent ordering hole.
 */
int loongemu_guest_write_begin(LoongemuGuestDma *dma, uint64_t gpa,
                               uint64_t len, uint8_t **host);
int loongemu_guest_write_commit(LoongemuGuestDma *dma, uint64_t gpa,
                                const void *data, uint64_t len);
void loongemu_guest_write_end(LoongemuGuestDma *dma);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_GUEST_DMA_H */
