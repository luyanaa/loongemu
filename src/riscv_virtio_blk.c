#include "loongemu/riscv_virtio_blk.h"

#include <stdbool.h>
#include <string.h>

/* virtio-mmio register offsets (modern transport, version 2). */
#define VIRTIO_MMIO_MAGIC_VALUE        0x000
#define VIRTIO_MMIO_VERSION            0x004
#define VIRTIO_MMIO_DEVICE_ID          0x008
#define VIRTIO_MMIO_VENDOR_ID          0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES    0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES    0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL          0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX      0x034
#define VIRTIO_MMIO_QUEUE_NUM          0x038
#define VIRTIO_MMIO_QUEUE_READY        0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY       0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS   0x060
#define VIRTIO_MMIO_INTERRUPT_ACK      0x064
#define VIRTIO_MMIO_STATUS             0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW     0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH    0x084
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW   0x090
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH  0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW   0x0a0
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH  0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION  0x0fc
#define VIRTIO_MMIO_CONFIG             0x100

#define VIRTIO_MMIO_MAGIC 0x74726976u /* "virt" */
#define VIRTIO_MMIO_VERSION_MODERN 2u
#define VIRTIO_MMIO_DEVICE_BLOCK 2u
#define VIRTIO_MMIO_VENDOR_QEMU 0x554d4551u /* "QEMU" */

#define VIRTIO_F_VERSION_1 32u
#define VIRTIO_STATUS_FEATURES_OK 8u
#define VIRTIO_STATUS_DRIVER_OK 4u

#define VIRTQ_DESC_F_NEXT  1u
#define VIRTQ_DESC_F_WRITE 2u

#define VIRTQ_AVAIL_SIZE(num) (4u + 2u * (num))
#define VIRTQ_USED_SIZE(num)  (4u + 8u * (num))

typedef struct VqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} VqDesc;

/* Ring and request structures are little-endian on both sides. Only loads are
 * open-coded: every store into guest memory has to go through
 * loongemu_guest_dma_write()/write_le() so the LR/SC reservation domain is
 * notified, so there is deliberately no st16/st32/st64 here. */
static uint16_t ld16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint64_t ld64(const uint8_t *p)
{
    return (uint64_t)ld32(p) | (uint64_t)ld32(p + 4) << 32;
}

/* Resolves a guest physical range to a host pointer inside guest RAM. */
static uint8_t *gpa_host(const LoongemuVirtioBlk *dev, uint64_t gpa,
                         uint64_t len)
{
    uint8_t *host = NULL;

    if (!loongemu_guest_dma_ptr(&dev->dma, gpa, len, &host)) {
        return NULL;
    }
    return host;
}

static void raise_irq(LoongemuVirtioBlk *dev)
{
    if (dev->interrupt_status != 0 && !dev->irq_level) {
        dev->irq_level = 1;
        if (dev->irq_fn != NULL) {
            dev->irq_fn(dev->irq_opaque, dev->irq, 1);
        }
    }
}

static void lower_irq(LoongemuVirtioBlk *dev)
{
    if (dev->irq_level) {
        dev->irq_level = 0;
        if (dev->irq_fn != NULL) {
            dev->irq_fn(dev->irq_opaque, dev->irq, 0);
        }
    }
}

static void device_reset(LoongemuVirtioBlk *dev)
{
    dev->status = 0;
    dev->interrupt_status = 0;
    dev->driver_features[0] = 0;
    dev->driver_features[1] = 0;
    dev->queue_ready = 0;
    dev->queue_num = 0;
    dev->queue_desc = 0;
    dev->queue_driver = 0;
    dev->queue_device = 0;
    dev->last_avail = 0;
    dev->dropped = 0;
    lower_irq(dev);
}

int loongemu_virtio_blk_init(LoongemuVirtioBlk *dev,
                             const LoongemuGuestDma *dma, uint8_t *disk,
                             uint64_t disk_size, uint32_t irq,
                             LoongemuVirtioIrqFn irq_fn, void *irq_opaque)
{
    if (dev == NULL || dma == NULL || dma->map == NULL || disk == NULL ||
        disk_size < LOONGEMU_VIRTIO_BLK_SECTOR) {
        return 0;
    }
    memset(dev, 0, sizeof(*dev));
    dev->dma = *dma;
    dev->disk = disk;
    dev->disk_size = disk_size - disk_size % LOONGEMU_VIRTIO_BLK_SECTOR;
    dev->capacity_sectors = dev->disk_size / LOONGEMU_VIRTIO_BLK_SECTOR;
    dev->irq = irq;
    dev->irq_fn = irq_fn;
    dev->irq_opaque = irq_opaque;
    return 1;
}

int loongemu_virtio_blk_attach(LoongemuMmioBus *bus, LoongemuVirtioBlk *dev,
                               uint64_t base)
{
    if (bus == NULL || dev == NULL) {
        return 0;
    }
    return loongemu_mmio_bus_add(bus, base, LOONGEMU_VIRTIO_MMIO_SIZE,
                                 loongemu_virtio_blk_read,
                                 loongemu_virtio_blk_write, dev, "virtio-blk");
}

static int read_desc(const LoongemuVirtioBlk *dev, uint32_t index, VqDesc *out)
{
    uint8_t *p;

    if (index >= dev->queue_num) {
        return 0;
    }
    /* The descriptor table is guest-supplied; reject a base address that would
     * wrap the 64-bit addition below instead of aliasing it to another page. */
    if (dev->queue_desc >
        UINT64_MAX - (uint64_t)index * sizeof(VqDesc) - sizeof(VqDesc)) {
        return 0;
    }
    p = gpa_host(dev, dev->queue_desc + (uint64_t)index * 16u, 16);
    if (p == NULL) {
        return 0;
    }
    out->addr = ld64(p);
    out->len = ld32(p + 8);
    out->flags = ld16(p + 12);
    out->next = ld16(p + 14);
    return 1;
}

/* A collected descriptor chain, bounded by the queue size. */
typedef struct VqChain {
    VqDesc desc[LOONGEMU_VIRTIO_BLK_QUEUE_MAX];
    uint32_t index[LOONGEMU_VIRTIO_BLK_QUEUE_MAX];
    uint32_t count;
    /* Descriptor that ended the chain without VIRTQ_DESC_F_NEXT, or -1 when the
     * walk was cut short (loop, out-of-range link, unreadable descriptor). */
    int32_t tail;
} VqChain;

/*
 * Walk the chain starting at `head`. A malicious or corrupted driver can point
 * NEXT at itself, at a descriptor already in the chain, or off the end of the
 * table; each of those must terminate the walk rather than spin, so the walk
 * is bounded by the queue size and every hop is range-checked.
 */
static bool collect_chain(const LoongemuVirtioBlk *dev, uint32_t head,
                          VqChain *chain)
{
    uint32_t index = head;

    chain->count = 0;
    chain->tail = -1;
    if (head >= dev->queue_num) {
        return false;
    }
    while (chain->count < dev->queue_num &&
           chain->count < LOONGEMU_VIRTIO_BLK_QUEUE_MAX) {
        VqDesc *d = &chain->desc[chain->count];

        if (!read_desc(dev, index, d)) {
            return false;
        }
        chain->index[chain->count] = index;
        chain->count++;
        if ((d->flags & VIRTQ_DESC_F_NEXT) == 0) {
            chain->tail = (int32_t)(chain->count - 1);
            return true;
        }
        if (d->next >= dev->queue_num) {
            return false;
        }
        for (uint32_t i = 0; i < chain->count; i++) {
            if (chain->index[i] == d->next) {
                return false;
            }
        }
        index = d->next;
    }
    return false;
}

/*
 * Publish the status byte into the last descriptor.
 *
 * Called on success *and* on every failure path: a driver that never sees a
 * status byte waits forever for the request to complete. When the tail is not
 * device-writable (or is an inaccessible descriptor) there is nowhere to put
 * it, and the used-ring entry posted by process_queue() is the only completion
 * signal left. Returns true when the byte was written.
 */
static bool report_status(LoongemuVirtioBlk *dev, const VqChain *chain,
                          uint8_t status)
{
    const VqDesc *tail;

    if (chain->count == 0) {
        return false;
    }
    tail = &chain->desc[chain->count - 1];
    if ((tail->flags & VIRTQ_DESC_F_WRITE) == 0 || tail->len < 1) {
        return false;
    }
    return loongemu_guest_dma_write(&dev->dma, tail->addr, &status, 1) == 0;
}

/* Copies one request chain. Returns the virtio-blk status byte. */
static uint8_t handle_chain(LoongemuVirtioBlk *dev, uint32_t head,
                            uint32_t *used_len)
{
    VqChain chain;
    uint8_t status = LOONGEMU_VIRTIO_BLK_S_OK;
    uint32_t type;
    uint64_t sector;
    uint64_t offset;
    uint64_t moved = 0;
    uint8_t *header;

    *used_len = 0;
    if (!collect_chain(dev, head, &chain)) {
        /* Nothing usable to report into: the tail is unknown, so the used-ring
         * entry is the only signal. */
        dev->requests++;
        return LOONGEMU_VIRTIO_BLK_S_IOERR;
    }

    /* Header descriptor: device-readable, at least struct virtio_blk_req. */
    if (chain.count < 2 || (chain.desc[0].flags & VIRTQ_DESC_F_WRITE) != 0 ||
        chain.desc[0].len < 16) {
        status = LOONGEMU_VIRTIO_BLK_S_IOERR;
        goto report;
    }
    header = gpa_host(dev, chain.desc[0].addr, 16);
    if (header == NULL) {
        status = LOONGEMU_VIRTIO_BLK_S_IOERR;
        goto report;
    }
    type = ld32(header);
    sector = ld64(header + 8);

    /* Status descriptor: device-writable, at least one byte. Checking it up
     * front means a rejected request still returns a status the driver can
     * read, instead of leaving the request outstanding. */
    if ((chain.desc[chain.count - 1].flags & VIRTQ_DESC_F_WRITE) == 0 ||
        chain.desc[chain.count - 1].len < 1) {
        status = LOONGEMU_VIRTIO_BLK_S_IOERR;
        goto report;
    }

    if (type == LOONGEMU_VIRTIO_BLK_T_IN || type == LOONGEMU_VIRTIO_BLK_T_OUT) {
        /* Reject a sector past the end before multiplying: `sector * 512` on a
         * wild sector would wrap and silently address the start of the disk.
         * capacity_sectors is derived from the host buffer, so a sector below
         * it cannot overflow the product. */
        if (sector >= dev->capacity_sectors) {
            status = LOONGEMU_VIRTIO_BLK_S_IOERR;
            goto report;
        }
        offset = sector * LOONGEMU_VIRTIO_BLK_SECTOR;

        for (uint32_t i = 1; i + 1 < chain.count; i++) {
            const VqDesc *d = &chain.desc[i];
            bool writable = (d->flags & VIRTQ_DESC_F_WRITE) != 0;

            /* Direction is part of the contract: a read buffer must be
             * device-writable and a write buffer device-readable. Skipping a
             * descriptor with the wrong direction would report success while
             * transferring fewer bytes than the driver asked for. */
            if (writable != (type == LOONGEMU_VIRTIO_BLK_T_IN)) {
                status = LOONGEMU_VIRTIO_BLK_S_IOERR;
                goto report;
            }
            if (d->len == 0) {
                continue;
            }
            /* `offset + len` in this form cannot overflow, and `offset` stays
             * <= disk_size because it only advances by lengths that passed. */
            if ((uint64_t)d->len > dev->disk_size - offset) {
                status = LOONGEMU_VIRTIO_BLK_S_IOERR;
                goto report;
            }
            if (type == LOONGEMU_VIRTIO_BLK_T_IN) {
                if (loongemu_guest_dma_write(&dev->dma, d->addr,
                                             dev->disk + offset, d->len) != 0) {
                    status = LOONGEMU_VIRTIO_BLK_S_IOERR;
                    goto report;
                }
                /* Only reads move data *into* guest memory, so only they count
                 * toward used.len (the spec's "bytes written into the
                 * device-writable part"). */
                moved += d->len;
            } else if (loongemu_guest_dma_read(&dev->dma, d->addr,
                                               dev->disk + offset,
                                               d->len) != 0) {
                status = LOONGEMU_VIRTIO_BLK_S_IOERR;
                goto report;
            }
            offset += d->len;
        }
    } else if (type != LOONGEMU_VIRTIO_BLK_T_FLUSH) {
        status = LOONGEMU_VIRTIO_BLK_S_UNSUPP;
    }

report:
    if (report_status(dev, &chain, status)) {
        moved += 1;
    }
    *used_len = (uint32_t)moved;
    dev->requests++;
    return status;
}

static void process_queue(LoongemuVirtioBlk *dev)
{
    uint8_t *avail;
    uint8_t *used;
    uint16_t avail_idx;
    uint16_t used_idx;
    uint16_t pending;
    bool processed = false;

    if (!dev->queue_ready || dev->queue_num == 0) {
        return;
    }
    avail = gpa_host(dev, dev->queue_driver,
                     VIRTQ_AVAIL_SIZE(dev->queue_num));
    used = gpa_host(dev, dev->queue_device,
                    VIRTQ_USED_SIZE(dev->queue_num));
    if (avail == NULL || used == NULL) {
        return;
    }
    avail_idx = ld16(avail + 2);
    used_idx = ld16(used + 2);

    /*
     * avail.idx is guest-writable and wraps at 16 bits. A driver can only ever
     * have queue_num requests outstanding, so anything further is corruption
     * or an attack: processing "until we catch up" would run up to 65535
     * chains inside one register write, each with a full descriptor walk.
     * Drop the queue position instead and resynchronise on the next notify.
     */
    pending = (uint16_t)(avail_idx - dev->last_avail);
    if (pending > dev->queue_num) {
        dev->dropped += pending;
        dev->last_avail = avail_idx;
        return;
    }

    while (dev->last_avail != avail_idx) {
        uint32_t slot = dev->last_avail % dev->queue_num;
        uint16_t head = ld16(avail + 4 + 2 * slot);
        uint32_t used_len = 0;

        (void)handle_chain(dev, head, &used_len);
        if (loongemu_guest_dma_write_le(&dev->dma,
                                         dev->queue_device + 4 +
                                             8u * (used_idx % dev->queue_num),
                                         head, 4) != 0 ||
            loongemu_guest_dma_write_le(&dev->dma,
                                         dev->queue_device + 8 +
                                             8u * (used_idx % dev->queue_num),
                                         used_len, 4) != 0) {
            /* The used ring went away mid-request; stop rather than spinning. */
            dev->last_avail = avail_idx;
            return;
        }
        used_idx = (uint16_t)(used_idx + 1);
        loongemu_guest_dma_write_le(&dev->dma, dev->queue_device + 2,
                                    used_idx, 2);
        dev->last_avail = (uint16_t)(dev->last_avail + 1);
        processed = true;
    }
    if (processed && (dev->status & VIRTIO_STATUS_DRIVER_OK)) {
        dev->interrupt_status |= 1u;
        raise_irq(dev);
    }
}

static uint32_t register_read(const LoongemuVirtioBlk *dev, uint32_t offset)
{
    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE:
        return VIRTIO_MMIO_MAGIC;
    case VIRTIO_MMIO_VERSION:
        return VIRTIO_MMIO_VERSION_MODERN;
    case VIRTIO_MMIO_DEVICE_ID:
        return VIRTIO_MMIO_DEVICE_BLOCK;
    case VIRTIO_MMIO_VENDOR_ID:
        return VIRTIO_MMIO_VENDOR_QEMU;
    case VIRTIO_MMIO_DEVICE_FEATURES:
        return dev->device_features_sel == 1
                   ? (1u << (VIRTIO_F_VERSION_1 - 32)) : 0u;
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        return LOONGEMU_VIRTIO_BLK_QUEUE_MAX;
    case VIRTIO_MMIO_QUEUE_NUM:
        return dev->queue_num;
    case VIRTIO_MMIO_QUEUE_READY:
        return dev->queue_ready;
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        return dev->interrupt_status;
    case VIRTIO_MMIO_STATUS:
        return dev->status;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        return (uint32_t)dev->queue_desc;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        return (uint32_t)(dev->queue_desc >> 32);
    case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
        return (uint32_t)dev->queue_driver;
    case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
        return (uint32_t)(dev->queue_driver >> 32);
    case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
        return (uint32_t)dev->queue_device;
    case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
        return (uint32_t)(dev->queue_device >> 32);
    case VIRTIO_MMIO_CONFIG_GENERATION:
        return 0;
    default:
        return 0;
    }
}

uint64_t loongemu_virtio_blk_read(void *opaque, uint64_t offset, unsigned size)
{
    LoongemuVirtioBlk *dev = opaque;
    uint32_t off = (uint32_t)offset;
    uint64_t value = 0;

    if (dev == NULL || off >= LOONGEMU_VIRTIO_MMIO_SIZE) {
        return 0;
    }
    if (off >= VIRTIO_MMIO_CONFIG) {
        /* struct virtio_blk_config { __le64 capacity; } */
        uint32_t shift = (off - VIRTIO_MMIO_CONFIG) * 8u;
        value = shift < 64 ? dev->capacity_sectors >> shift : 0;
    } else if ((off & 3u) == 0) {
        value = register_read(dev, off & ~3u);
    } else {
        uint32_t word = register_read(dev, off & ~3u);
        value = word >> ((off & 3u) * 8u);
    }
    if (size >= 8) {
        return value;
    }
    return value & ((UINT64_C(1) << (size * 8)) - 1u);
}

static void register_write(LoongemuVirtioBlk *dev, uint32_t offset,
                           uint32_t value)
{
    switch (offset) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        dev->device_features_sel = value;
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        if (dev->driver_features_sel < 2) {
            dev->driver_features[dev->driver_features_sel] = value;
        }
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        dev->driver_features_sel = value;
        break;
    case VIRTIO_MMIO_QUEUE_SEL:
        /* Only queue 0 exists; selecting another one is ignored. */
        dev->queue_sel = value == 0 ? 0u : value;
        break;
    case VIRTIO_MMIO_QUEUE_NUM:
        if ((value & (value - 1u)) == 0 &&
            value <= LOONGEMU_VIRTIO_BLK_QUEUE_MAX) {
            dev->queue_num = value;
        }
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        dev->queue_ready = value & 1u;
        break;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        dev->queue_desc = (dev->queue_desc & ~UINT64_C(0xffffffff)) | value;
        break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        dev->queue_desc =
            (dev->queue_desc & UINT64_C(0xffffffff)) | (uint64_t)value << 32;
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
        dev->queue_driver =
            (dev->queue_driver & ~UINT64_C(0xffffffff)) | value;
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
        dev->queue_driver =
            (dev->queue_driver & UINT64_C(0xffffffff)) | (uint64_t)value << 32;
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
        dev->queue_device =
            (dev->queue_device & ~UINT64_C(0xffffffff)) | value;
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
        dev->queue_device =
            (dev->queue_device & UINT64_C(0xffffffff)) | (uint64_t)value << 32;
        break;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        process_queue(dev);
        break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        dev->interrupt_status &= ~value;
        if (dev->interrupt_status == 0) {
            lower_irq(dev);
        }
        break;
    case VIRTIO_MMIO_STATUS:
        dev->status = value;
        if (value == 0) {
            device_reset(dev);
        }
        break;
    default:
        break;
    }
}

void loongemu_virtio_blk_write(void *opaque, uint64_t offset, uint64_t value,
                               unsigned size)
{
    LoongemuVirtioBlk *dev = opaque;
    uint32_t off = (uint32_t)offset;

    (void)size;
    if (dev == NULL || off >= VIRTIO_MMIO_CONFIG) {
        return;
    }
    if ((off & 3u) != 0) {
        return;
    }
    register_write(dev, off & ~3u, (uint32_t)value);
}
