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

static void st16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void st32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void st64(uint8_t *p, uint64_t v)
{
    st32(p, (uint32_t)v);
    st32(p + 4, (uint32_t)(v >> 32));
}

/* Resolves a guest physical range to a host pointer inside guest RAM. */
static uint8_t *gpa_host(const LoongemuVirtioBlk *dev, uint64_t gpa,
                         uint64_t len)
{
    LoongemuMemLookup lookup;

    if (dev->map == NULL || len == 0) {
        return NULL;
    }
    if (loongemu_mem_resolve(dev->map, gpa, len, &lookup) !=
        LOONGEMU_MEM_RAM) {
        return NULL;
    }
    return (uint8_t *)(uintptr_t)lookup.hpa;
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
    lower_irq(dev);
}

int loongemu_virtio_blk_init(LoongemuVirtioBlk *dev,
                             const LoongemuMemMap *map, uint8_t *disk,
                             uint64_t disk_size, uint32_t irq,
                             LoongemuVirtioIrqFn irq_fn, void *irq_opaque)
{
    if (dev == NULL || disk == NULL ||
        disk_size < LOONGEMU_VIRTIO_BLK_SECTOR) {
        return 0;
    }
    memset(dev, 0, sizeof(*dev));
    dev->map = map;
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

/* Copies one request chain. Returns the virtio-blk status byte. */
static uint8_t handle_chain(LoongemuVirtioBlk *dev, uint32_t head,
                            uint32_t *used_len)
{
    VqDesc descs[LOONGEMU_VIRTIO_BLK_QUEUE_MAX];
    uint32_t count = 0;
    uint32_t index = head;
    uint32_t type;
    uint64_t sector;
    uint64_t offset = 0;
    uint8_t status = LOONGEMU_VIRTIO_BLK_S_OK;
    uint8_t *header;
    uint8_t *status_addr;
    bool terminated = false;

    while (count < dev->queue_num &&
           count < LOONGEMU_VIRTIO_BLK_QUEUE_MAX) {
        if (!read_desc(dev, index, &descs[count])) {
            return LOONGEMU_VIRTIO_BLK_S_IOERR;
        }
        count++;
        if ((descs[count - 1].flags & VIRTQ_DESC_F_NEXT) == 0) {
            terminated = true;
            break;
        }
        index = descs[count - 1].next;
    }
    if (!terminated || count < 2) {
        return LOONGEMU_VIRTIO_BLK_S_IOERR;
    }

    header = gpa_host(dev, descs[0].addr, 16);
    if (header == NULL) {
        return LOONGEMU_VIRTIO_BLK_S_IOERR;
    }
    type = ld32(header);
    sector = ld64(header + 8);
    offset = sector * LOONGEMU_VIRTIO_BLK_SECTOR;

    for (uint32_t i = 0; i < count; i++) {
        *used_len += descs[i].len;
    }

    for (uint32_t i = 1; i + 1 < count; i++) {
        uint64_t len = descs[i].len;
        uint8_t *buf;

        if (type == LOONGEMU_VIRTIO_BLK_T_IN) {
            if ((descs[i].flags & VIRTQ_DESC_F_WRITE) == 0) {
                continue;
            }
            buf = gpa_host(dev, descs[i].addr, len);
            if (buf == NULL || offset + len > dev->disk_size) {
                status = LOONGEMU_VIRTIO_BLK_S_IOERR;
                break;
            }
            memcpy(buf, dev->disk + offset, len);
            offset += len;
        } else if (type == LOONGEMU_VIRTIO_BLK_T_OUT) {
            if (descs[i].flags & VIRTQ_DESC_F_WRITE) {
                continue;
            }
            buf = gpa_host(dev, descs[i].addr, len);
            if (buf == NULL || offset + len > dev->disk_size) {
                status = LOONGEMU_VIRTIO_BLK_S_IOERR;
                break;
            }
            memcpy(dev->disk + offset, buf, len);
            offset += len;
        }
    }
    if (type != LOONGEMU_VIRTIO_BLK_T_IN && type != LOONGEMU_VIRTIO_BLK_T_OUT &&
        type != LOONGEMU_VIRTIO_BLK_T_FLUSH) {
        status = LOONGEMU_VIRTIO_BLK_S_UNSUPP;
    }

    status_addr = gpa_host(dev, descs[count - 1].addr, 1);
    if (status_addr == NULL) {
        return LOONGEMU_VIRTIO_BLK_S_IOERR;
    }
    *status_addr = status;
    dev->requests++;
    return status;
}

static void process_queue(LoongemuVirtioBlk *dev)
{
    uint8_t *avail;
    uint8_t *used;
    uint16_t avail_idx;
    uint32_t used_idx;

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
    while (dev->last_avail != avail_idx) {
        uint32_t slot = dev->last_avail % dev->queue_num;
        uint16_t head = ld16(avail + 4 + 2 * slot);
        uint32_t used_len = 0;
        uint8_t *entry;

        (void)handle_chain(dev, head, &used_len);
        entry = used + 4 + 8 * (used_idx % dev->queue_num);
        st32(entry, head);
        st32(entry + 4, used_len);
        used_idx++;
        st16(used + 2, (uint16_t)used_idx);
        dev->last_avail++;
    }
    if (dev->status & VIRTIO_STATUS_DRIVER_OK) {
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
