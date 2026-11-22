#ifndef LOONGEMU_RISCV_VIRTIO_BLK_H
#define LOONGEMU_RISCV_VIRTIO_BLK_H

#include <stdint.h>
#include "loongemu/mmio_bus.h"
#include "loongemu/phys_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * QEMU-virt-compatible virtio-mmio transport (modern, version 2) with a
 * single block device backed by a host buffer.
 *
 * The device owns one split virtqueue. Guest descriptor/available/used rings
 * and request buffers are reached through the guest physical map, so a
 * request is serviced exactly as a real DMA-capable device would see it.
 * The interrupt line is level-triggered and meant to feed a PLIC source.
 */

#define LOONGEMU_VIRTIO_MMIO_SIZE    0x200
#define LOONGEMU_VIRTIO_BLK_QUEUE_MAX 8
#define LOONGEMU_VIRTIO_BLK_SECTOR    512u

#define LOONGEMU_VIRTIO_BLK_T_IN     0u
#define LOONGEMU_VIRTIO_BLK_T_OUT    1u
#define LOONGEMU_VIRTIO_BLK_T_FLUSH  4u

#define LOONGEMU_VIRTIO_BLK_S_OK     0u
#define LOONGEMU_VIRTIO_BLK_S_IOERR  1u
#define LOONGEMU_VIRTIO_BLK_S_UNSUPP 2u

typedef void (*LoongemuVirtioIrqFn)(void *opaque, uint32_t irq, int level);

typedef struct LoongemuVirtioBlk {
    const LoongemuMemMap *map;
    uint8_t *disk;
    uint64_t disk_size;
    uint64_t capacity_sectors;
    uint32_t irq;
    LoongemuVirtioIrqFn irq_fn;
    void *irq_opaque;

    uint32_t status;
    uint32_t interrupt_status;
    uint32_t device_features_sel;
    uint32_t driver_features_sel;
    uint32_t driver_features[2];
    uint32_t queue_sel;
    uint32_t queue_num;
    uint32_t queue_ready;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
    uint32_t last_avail;
    uint32_t requests;
    uint32_t bytes;
    int irq_level;
} LoongemuVirtioBlk;

/* `disk` must stay alive for the device's lifetime; it is the sector store. */
int loongemu_virtio_blk_init(LoongemuVirtioBlk *dev,
                             const LoongemuMemMap *map, uint8_t *disk,
                             uint64_t disk_size, uint32_t irq,
                             LoongemuVirtioIrqFn irq_fn, void *irq_opaque);

/* Registers the transport window (0x200 bytes) on the MMIO bus. */
int loongemu_virtio_blk_attach(LoongemuMmioBus *bus, LoongemuVirtioBlk *dev,
                               uint64_t base);

uint64_t loongemu_virtio_blk_read(void *opaque, uint64_t offset,
                                  unsigned size);
void loongemu_virtio_blk_write(void *opaque, uint64_t offset, uint64_t value,
                               unsigned size);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_RISCV_VIRTIO_BLK_H */
