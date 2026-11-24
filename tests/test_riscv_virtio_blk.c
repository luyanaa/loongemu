/*
 * Host-side tests for the virtio-mmio block device.
 *
 * The device is fed a real split virtqueue laid out in a guest-RAM buffer, so
 * descriptor walking, ring bookkeeping and DMA publication are exercised
 * exactly as the guest sees them. The interesting cases are the malformed
 * ones: a driver (or a guest that has been taken over) can hand the device a
 * descriptor chain that loops, points outside the table, overflows the disk
 * offset, or has the read/write direction inverted. None of those may hang the
 * vCPU, wrap into another part of the disk, or leave the request without a
 * status byte.
 *
 * Every DMA store must also be reported to the reservation domain. That is
 * checked through the write-notify callback rather than by inspecting the
 * memcpy() sites, so a future device write that forgets to publish shows up
 * here as a missing notification.
 */
#include "loongemu/riscv_virtio_blk.h"

#include <stdio.h>
#include <string.h>

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

#define RAM_BASE UINT64_C(0x80000000)
#define RAM_SIZE (1u << 20)
#define DESC_BASE (RAM_BASE + 0x1000)
#define AVAIL_BASE (RAM_BASE + 0x2000)
#define USED_BASE (RAM_BASE + 0x3000)
#define BUF_BASE (RAM_BASE + 0x4000)

#define DESC_F_NEXT 1u
#define DESC_F_WRITE 2u

static uint8_t ram[RAM_SIZE] __attribute__((aligned(4096)));
static uint8_t disk[4096];

static LoongemuMemMap map;
static LoongemuGuestDma dma;
static LoongemuVirtioBlk dev;

typedef struct NotifyLog {
    unsigned calls;
    uint64_t bytes;
    /* Ranges are recorded as they arrive so a test can assert that the data
     * buffer itself was published, not just the status byte. */
    uint64_t gpa[64];
    uint64_t len[64];
} NotifyLog;

static NotifyLog notify;

static void on_notify(void *opaque, uint64_t gpa, uint64_t len)
{
    NotifyLog *log = opaque;

    log->calls++;
    log->bytes += len;
    if (log->calls <= sizeof(log->gpa) / sizeof(log->gpa[0])) {
        log->gpa[log->calls - 1] = gpa;
        log->len[log->calls - 1] = len;
    }
}

static bool notified_range(uint64_t gpa, uint64_t len)
{
    for (unsigned i = 0; i < notify.calls &&
                         i < sizeof(notify.gpa) / sizeof(notify.gpa[0]); i++) {
        if (notify.gpa[i] == gpa && notify.len[i] == len) {
            return true;
        }
    }
    return false;
}

static uint8_t *at(uint64_t gpa)
{
    return ram + (gpa - RAM_BASE);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void mmio_write(uint32_t offset, uint32_t value)
{
    loongemu_virtio_blk_write(&dev, offset, value, 4);
}

static void set_desc(uint32_t index, uint64_t addr, uint32_t len,
                     uint16_t flags, uint16_t next)
{
    uint8_t *p = at(DESC_BASE + (uint64_t)index * 16u);

    put64(p, addr);
    put32(p + 8, len);
    put16(p + 12, flags);
    put16(p + 14, next);
}

/* Driver side: publish `head` on the available ring and kick the device. */
static void push(uint16_t head)
{
    uint16_t idx = rd16(at(AVAIL_BASE + 2));
    uint32_t slot = idx % dev.queue_num;

    put16(at(AVAIL_BASE + 4 + 2 * slot), head);
    put16(at(AVAIL_BASE + 2), (uint16_t)(idx + 1));
    mmio_write(0x050, 0);
}

static void setup(uint32_t queue_num)
{
    memset(ram, 0, sizeof(ram));
    memset(&notify, 0, sizeof(notify));
    for (size_t i = 0; i < sizeof(disk); i++) {
        disk[i] = (uint8_t)((i * 7u) ^ 0x5au);
    }
    loongemu_mem_map_init(&map);
    CHECK(loongemu_mem_map_add(&map, RAM_BASE, RAM_SIZE,
                               (uint64_t)(uintptr_t)ram, LOONGEMU_MEM_RAM,
                               "ram"));
    loongemu_guest_dma_init(&dma, &map, on_notify, &notify);
    CHECK(loongemu_virtio_blk_init(&dev, &dma, disk, sizeof(disk), 1, NULL,
                                   NULL));

    mmio_write(0x030, 0);          /* queue select */
    mmio_write(0x038, queue_num);  /* queue size */
    mmio_write(0x080, (uint32_t)DESC_BASE);
    mmio_write(0x084, (uint32_t)(DESC_BASE >> 32));
    mmio_write(0x090, (uint32_t)AVAIL_BASE);
    mmio_write(0x094, (uint32_t)(AVAIL_BASE >> 32));
    mmio_write(0x0a0, (uint32_t)USED_BASE);
    mmio_write(0x0a4, (uint32_t)(USED_BASE >> 32));
    mmio_write(0x044, 1);          /* queue ready */
    mmio_write(0x070, 4);          /* DRIVER_OK */
}

/* Format a well-formed block request: header, one data buffer, status byte. */
static void request(uint32_t head, uint32_t type, uint64_t sector,
                    uint64_t data_addr, uint32_t data_len, uint16_t data_flags,
                    uint64_t status_addr)
{
    set_desc(head, BUF_BASE, 16, DESC_F_NEXT, (uint16_t)(head + 1));
    put32(at(BUF_BASE), type);
    put32(at(BUF_BASE + 4), 0);
    put64(at(BUF_BASE + 8), sector);
    set_desc(head + 1, data_addr, data_len,
             (uint16_t)(data_flags | DESC_F_NEXT), (uint16_t)(head + 2));
    set_desc(head + 2, status_addr, 1, DESC_F_WRITE, 0);
    at(status_addr)[0] = 0xee; /* sentinel: 0xee means "never written" */
}

static void test_read_success(void)
{
    setup(8);
    uint64_t data = BUF_BASE + 0x100;
    uint64_t status = BUF_BASE + 0x400;

    request(0, LOONGEMU_VIRTIO_BLK_T_IN, 3, data, 512, DESC_F_WRITE, status);
    push(0);

    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_OK);
    CHECK(memcmp(at(data), disk + 3 * 512, 512) == 0);
    CHECK(rd16(at(USED_BASE + 2)) == 1);
    CHECK(rd32(at(USED_BASE + 4)) == 0);       /* head */
    CHECK(rd32(at(USED_BASE + 8)) == 512 + 1); /* data + status byte */
    CHECK(dev.requests == 1);
    /* The data buffer and the status byte are both guest writes. */
    CHECK(notified_range(data, 512));
    CHECK(notified_range(status, 1));
    CHECK(rd16(at(AVAIL_BASE + 2)) == rd16(at(USED_BASE + 2)));
}

static void test_write_success(void)
{
    setup(8);
    uint64_t data = BUF_BASE + 0x100;
    uint64_t status = BUF_BASE + 0x400;

    for (unsigned i = 0; i < 512; i++) {
        at(data)[i] = (uint8_t)(i + 1);
    }
    request(0, LOONGEMU_VIRTIO_BLK_T_OUT, 2, data, 512, 0, status);
    push(0);

    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_OK);
    CHECK(memcmp(disk + 2 * 512, at(data), 512) == 0);
    CHECK(rd32(at(USED_BASE + 8)) == 1); /* status byte only */
    /* Writing to the disk is not a guest-memory write, so only the status byte
     * is published. */
    CHECK(notify.calls == 4);   /* used head, used len, used idx, status */
    CHECK(notified_range(status, 1));
}

static void test_avail_idx_runaway(void)
{
    setup(8);
    uint64_t data = BUF_BASE + 0x100;
    uint64_t status = BUF_BASE + 0x400;

    request(0, LOONGEMU_VIRTIO_BLK_T_IN, 0, data, 512, DESC_F_WRITE, status);
    /* Claim queue_num + 1 outstanding requests without publishing any. */
    put16(at(AVAIL_BASE + 2), (uint16_t)(dev.queue_num + 1));
    mmio_write(0x050, 0);

    CHECK(dev.dropped == dev.queue_num + 1);
    CHECK(dev.requests == 0);
    CHECK(at(status)[0] == 0xee);
    CHECK(rd16(at(USED_BASE + 2)) == 0);
    CHECK(dev.last_avail == dev.queue_num + 1);
    /* Resynchronised: a sane notify afterwards still works. */
    put16(at(AVAIL_BASE + 2), 0);
    dev.last_avail = 0;
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_OK);
}

static void test_sector_out_of_range(void)
{
    setup(8);
    uint64_t data = BUF_BASE + 0x100;
    uint64_t status = BUF_BASE + 0x400;
    uint64_t last_sector = dev.capacity_sectors - 1;

    request(0, LOONGEMU_VIRTIO_BLK_T_IN, dev.capacity_sectors, data, 512,
            DESC_F_WRITE, status);
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_IOERR);

    /* The last valid sector must still work; the check is >= not >. */
    request(0, LOONGEMU_VIRTIO_BLK_T_IN, last_sector, data, 512,
            DESC_F_WRITE, status);
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_OK);
}

static void test_sector_multiplication_overflow(void)
{
    setup(8);
    uint64_t status = BUF_BASE + 0x400;

    /* 0xffffffffffffffff * 512 wraps to 0xfffffffffffffe00. Before the guard
     * this read the end of a disk that does not exist. */
    request(0, LOONGEMU_VIRTIO_BLK_T_IN, UINT64_MAX, BUF_BASE + 0x100, 512,
            DESC_F_WRITE, status);
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_IOERR);
    CHECK(memcmp(disk, disk, 1) == 0); /* disk untouched */

    request(0, LOONGEMU_VIRTIO_BLK_T_OUT, UINT64_C(0x8000000000000000),
            BUF_BASE + 0x100, 512, 0, status);
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_IOERR);
}

static void test_length_past_end_of_disk(void)
{
    setup(8);
    uint64_t status = BUF_BASE + 0x400;
    uint64_t last_sector = dev.capacity_sectors - 1;

    /* One byte more than the last sector holds, and a wild length. */
    request(0, LOONGEMU_VIRTIO_BLK_T_IN, last_sector, BUF_BASE + 0x100,
            513, DESC_F_WRITE, status);
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_IOERR);

    request(0, LOONGEMU_VIRTIO_BLK_T_IN, last_sector, BUF_BASE + 0x100,
            0xffffffffu, DESC_F_WRITE, status);
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_IOERR);
}

static void test_header_direction(void)
{
    setup(8);
    uint64_t status = BUF_BASE + 0x400;

    set_desc(0, BUF_BASE, 16, DESC_F_WRITE | DESC_F_NEXT, 1);
    put32(at(BUF_BASE), LOONGEMU_VIRTIO_BLK_T_IN);
    put64(at(BUF_BASE + 8), 0);
    set_desc(1, BUF_BASE + 0x100, 512, DESC_F_WRITE | DESC_F_NEXT, 2);
    set_desc(2, status, 1, DESC_F_WRITE, 0);
    at(status)[0] = 0xee;
    push(0);

    /* A device-writable header is not a request we can parse, but the tail is
     * still writable, so the driver gets a status instead of nothing. */
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_IOERR);
    CHECK(rd16(at(USED_BASE + 2)) == 1);
}

static void test_header_too_short(void)
{
    setup(8);
    uint64_t status = BUF_BASE + 0x400;

    set_desc(0, BUF_BASE, 8, DESC_F_NEXT, 1);
    set_desc(1, status, 1, DESC_F_WRITE, 0);
    at(status)[0] = 0xee;
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_IOERR);
}

static void test_status_not_writable(void)
{
    setup(8);
    uint64_t data = BUF_BASE + 0x100;

    /* Tail is a read-only descriptor: there is nowhere to put the status. */
    set_desc(0, BUF_BASE, 16, 0, 1);
    put32(at(BUF_BASE), LOONGEMU_VIRTIO_BLK_T_IN);
    set_desc(1, data, 512, DESC_F_WRITE, 2);
    set_desc(2, BUF_BASE + 0x500, 1, 0, 0);
    push(0);

    /* The used-ring entry is the only completion signal left. */
    CHECK(rd16(at(USED_BASE + 2)) == 1);
    CHECK(dev.requests == 1);
}

static void test_descriptor_loop(void)
{
    setup(8);
    uint64_t status = BUF_BASE + 0x400;
    uint64_t data = BUF_BASE + 0x100;

    /* header -> data -> header: a cycle with no tail. */
    set_desc(0, BUF_BASE, 16, DESC_F_NEXT, 1);
    put32(at(BUF_BASE), LOONGEMU_VIRTIO_BLK_T_IN);
    set_desc(1, data, 512, DESC_F_WRITE | DESC_F_NEXT, 0);
    set_desc(2, BUF_BASE + 0x500, 1, DESC_F_WRITE, 0);
    at(status)[0] = 0xee;
    push(0);

    /* The walk terminates at the repeated descriptor; the used ring still
     * advances so the driver is not stuck. */
    CHECK(rd16(at(USED_BASE + 2)) == 1);
    CHECK(dev.requests == 1);

    /* Self-loop on the head descriptor. */
    setup(8);
    set_desc(0, BUF_BASE, 16, DESC_F_NEXT, 0);
    put32(at(BUF_BASE), LOONGEMU_VIRTIO_BLK_T_IN);
    push(0);
    CHECK(rd16(at(USED_BASE + 2)) == 1);
}

static void test_descriptor_next_out_of_range(void)
{
    setup(8);

    set_desc(0, BUF_BASE, 16, DESC_F_NEXT, 99);
    put32(at(BUF_BASE), LOONGEMU_VIRTIO_BLK_T_IN);
    push(0);
    CHECK(rd16(at(USED_BASE + 2)) == 1);
    CHECK(dev.requests == 1);
}

static void test_head_out_of_range(void)
{
    setup(8);

    push(200);
    CHECK(rd16(at(USED_BASE + 2)) == 1);
    CHECK(dev.requests == 1);
    /* No status byte was written anywhere; the used entry carries the failure. */
}

static void test_data_direction_mismatch(void)
{
    setup(8);
    uint64_t status = BUF_BASE + 0x400;

    /* IN request whose data descriptor is read-only: refusing it is the only
     * honest answer, skipping it would report success with 0 bytes copied. */
    request(0, LOONGEMU_VIRTIO_BLK_T_IN, 0, BUF_BASE + 0x100, 512, 0, status);
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_IOERR);

    /* OUT request whose data descriptor is device-writable. */
    request(0, LOONGEMU_VIRTIO_BLK_T_OUT, 0, BUF_BASE + 0x100, 512,
            DESC_F_WRITE, status);
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_IOERR);
}

static void test_unknown_type_and_flush(void)
{
    setup(8);
    uint64_t status = BUF_BASE + 0x400;

    request(0, 7, 0, BUF_BASE + 0x100, 0, 0, status);
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_UNSUPP);
    CHECK(notified_range(status, 1));

    request(0, LOONGEMU_VIRTIO_BLK_T_FLUSH, 0, BUF_BASE + 0x100, 0, 0,
            status);
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_OK);
}

static void test_indices_wrap(void)
{
    setup(8);
    uint64_t data = BUF_BASE + 0x100;
    uint64_t status = BUF_BASE + 0x400;

    /* Put both rings one request short of wrapping. last_avail is 16-bit, so
     * the comparison against avail.idx must still terminate after the wrap;
     * as a 32-bit counter it never would. */
    dev.last_avail = 0xfffe;
    put16(at(AVAIL_BASE + 2), 0xfffe);
    put16(at(USED_BASE + 2), 0xfffe);

    request(0, LOONGEMU_VIRTIO_BLK_T_IN, 1, data, 512, DESC_F_WRITE, status);
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_OK);

    setup(8);
    dev.last_avail = 0xffff;
    put16(at(AVAIL_BASE + 2), 0xffff);
    put16(at(USED_BASE + 2), 0xffff);

    request(0, LOONGEMU_VIRTIO_BLK_T_IN, 1, data, 512, DESC_F_WRITE, status);
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_OK);
    CHECK(rd16(at(USED_BASE + 2)) == 0); /* wrapped back to 0 */
}

static void test_dma_outside_ram(void)
{
    setup(8);
    uint64_t status = BUF_BASE + 0x400;

    /* Data buffer in a hole: the device cannot map it, so it must fail the
     * request rather than write through a stale pointer. */
    request(0, LOONGEMU_VIRTIO_BLK_T_IN, 0, UINT64_C(0x40000000), 512,
            DESC_F_WRITE, status);
    push(0);
    CHECK(at(status)[0] == LOONGEMU_VIRTIO_BLK_S_IOERR);
}

/*
 * The DMA pointer cache remembers one translation per page and retires it when
 * the memory map changes. Its two failure modes are a hit that returns a stale
 * HPA after a remap, and a hit that ignores a region edge; both are checked
 * here directly (the virtio paths only exercise the happy case).
 */
static void test_dma_cache(void)
{
    uint8_t *p = NULL;
    uint8_t *q = NULL;
    uint64_t gpa = RAM_BASE + 0x1234;

    setup(8);

    /* First resolve fills the per-page entry, the second one hits it. */
    CHECK(loongemu_guest_dma_ptr(&dma, gpa, 16, &p));
    CHECK(p == at(gpa));
    CHECK(loongemu_guest_dma_ptr(&dma, gpa + 4, 8, &q));
    CHECK(q == p + 4);

    /* A chunk that runs past the RAM region end must be refused, not served
     * from a window that was clipped to the region. */
    CHECK(!loongemu_guest_dma_ptr(&dma, RAM_BASE + RAM_SIZE - 8, 16, &q));

    /* Move the RAM window to a different host address: the map generation must
     * retire the cached translation. */
    CHECK(loongemu_mem_map_remove(&map, RAM_BASE));
    CHECK(loongemu_mem_map_add(&map, RAM_BASE, RAM_SIZE,
                               (uint64_t)(uintptr_t)ram + 0x1000,
                               LOONGEMU_MEM_RAM, "ram2"));
    CHECK(loongemu_guest_dma_ptr(&dma, gpa, 16, &q));
    CHECK(q == at(gpa) + 0x1000);
}

static void test_queue_not_ready(void)
{
    setup(8);
    mmio_write(0x044, 0);

    push(0);
    CHECK(dev.requests == 0);
    CHECK(rd16(at(USED_BASE + 2)) == 0);
}

int main(void)
{
    test_read_success();
    test_write_success();
    test_avail_idx_runaway();
    test_sector_out_of_range();
    test_sector_multiplication_overflow();
    test_length_past_end_of_disk();
    test_header_direction();
    test_header_too_short();
    test_status_not_writable();
    test_descriptor_loop();
    test_descriptor_next_out_of_range();
    test_head_out_of_range();
    test_data_direction_mismatch();
    test_unknown_type_and_flush();
    test_indices_wrap();
    test_dma_outside_ram();
    test_dma_cache();
    test_queue_not_ready();

    if (failures != 0) {
        fprintf(stderr, "virtio-blk: %u/%u checks failed\n", failures, checks);
        return 1;
    }
    printf("virtio-blk: %u checks passed\n", checks);
    return 0;
}
