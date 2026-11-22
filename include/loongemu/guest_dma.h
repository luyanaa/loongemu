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

typedef struct LoongemuGuestDma {
    const LoongemuMemMap *map;
    /* May be NULL: a host-side unit test has no reservation domain. In the
     * VMM this is latr_reservation_notify_write() wrapped by the machine. */
    LoongemuGuestWriteNotify notify;
    void *notify_opaque;
} LoongemuGuestDma;

void loongemu_guest_dma_init(LoongemuGuestDma *dma, const LoongemuMemMap *map,
                             LoongemuGuestWriteNotify notify, void *opaque);

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

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_GUEST_DMA_H */
