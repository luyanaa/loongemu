#include <string.h>
#include "loongemu/riscv_clint.h"

#define MIP_MSIP (UINT64_C(1) << 3)
#define MIP_MTIP (UINT64_C(1) << 7)

void loongemu_clint_init(LoongemuClint *clint, uint32_t hart_count)
{
    if (clint == NULL) {
        return;
    }
    memset(clint, 0, sizeof(*clint));
    clint->hart_count = hart_count > LOONGEMU_CLINT_MAX_HARTS
                            ? LOONGEMU_CLINT_MAX_HARTS
                            : hart_count;
    /*
     * QEMU (and real firmware) power up with mtimecmp far in the future, not 0.
     * A zero comparator makes `mtime >= mtimecmp` true from the first tick, so
     * every hart would latch MTIP before it ever programmed the timer.
     */
    for (uint32_t i = 0; i < clint->hart_count; i++) {
        clint->mtimecmp[i] = UINT64_MAX;
    }
}

uint64_t loongemu_clint_read(LoongemuClint *clint, uint64_t offset,
                             unsigned size)
{
    if (clint == NULL || size == 0 || size > 8) {
        return 0;
    }
    if (offset >= LOONGEMU_CLINT_MSIP_BASE &&
        offset + size <= LOONGEMU_CLINT_MSIP_BASE + 4u * clint->hart_count) {
        uint32_t index = (uint32_t)((offset - LOONGEMU_CLINT_MSIP_BASE) / 4);
        uint32_t shift = (uint32_t)(offset & 3u) * 8u;
        return (clint->msip[index] >> shift);
    }
    if (offset >= LOONGEMU_CLINT_MTIMECMP_BASE &&
        offset + size <= LOONGEMU_CLINT_MTIMECMP_BASE + 8u * clint->hart_count) {
        uint32_t index = (uint32_t)((offset - LOONGEMU_CLINT_MTIMECMP_BASE) / 8);
        uint32_t shift = (uint32_t)(offset & 7u) * 8u;
        return clint->mtimecmp[index] >> shift;
    }
    if (offset >= LOONGEMU_CLINT_MTIME &&
        offset + size <= LOONGEMU_CLINT_MTIME + 8u) {
        uint32_t shift = (uint32_t)(offset - LOONGEMU_CLINT_MTIME) * 8u;
        return clint->mtime >> shift;
    }
    return 0;
}

void loongemu_clint_write(LoongemuClint *clint, uint64_t offset,
                          uint64_t value, unsigned size)
{
    if (clint == NULL || size == 0 || size > 8) {
        return;
    }
    if (offset >= LOONGEMU_CLINT_MSIP_BASE &&
        offset + size <= LOONGEMU_CLINT_MSIP_BASE + 4u * clint->hart_count) {
        uint32_t index = (uint32_t)((offset - LOONGEMU_CLINT_MSIP_BASE) / 4);
        uint32_t shift = (uint32_t)(offset & 3u) * 8u;
        uint32_t mask = size == 4 ? UINT32_MAX : ((1u << (size * 8)) - 1u);
        clint->msip[index] =
            (clint->msip[index] & ~(mask << shift)) |
            (((uint32_t)value & mask) << shift);
        return;
    }
    if (offset >= LOONGEMU_CLINT_MTIMECMP_BASE &&
        offset + size <= LOONGEMU_CLINT_MTIMECMP_BASE + 8u * clint->hart_count) {
        uint32_t index = (uint32_t)((offset - LOONGEMU_CLINT_MTIMECMP_BASE) / 8);
        uint32_t shift = (uint32_t)(offset & 7u) * 8u;
        uint64_t mask = size == 8 ? UINT64_MAX : ((UINT64_C(1) << (size * 8)) - 1);
        clint->mtimecmp[index] =
            (clint->mtimecmp[index] & ~(mask << shift)) |
            ((value & mask) << shift);
        return;
    }
    if (offset >= LOONGEMU_CLINT_MTIME &&
        offset + size <= LOONGEMU_CLINT_MTIME + 8u) {
        uint32_t shift = (uint32_t)(offset - LOONGEMU_CLINT_MTIME) * 8u;
        uint64_t mask = size == 8 ? UINT64_MAX : ((UINT64_C(1) << (size * 8)) - 1);
        clint->mtime = (clint->mtime & ~(mask << shift)) |
                       ((value & mask) << shift);
    }
}

void loongemu_clint_tick(LoongemuClint *clint, uint64_t now,
                         uint64_t *mip, uint32_t hartid)
{
    uint64_t bits = 0;

    if (clint == NULL || mip == NULL || hartid >= clint->hart_count) {
        return;
    }
    clint->mtime = now;
    if (clint->msip[hartid] & 1u) {
        bits |= MIP_MSIP;
    }
    if (clint->mtime >= clint->mtimecmp[hartid]) {
        bits |= MIP_MTIP;
    }
    *mip = (*mip & ~(MIP_MSIP | MIP_MTIP)) | bits;
}

static uint64_t clint_read_cb(void *opaque, uint64_t offset, unsigned size)
{
    return loongemu_clint_read(opaque, offset, size);
}

static void clint_write_cb(void *opaque, uint64_t offset, uint64_t value,
                           unsigned size)
{
    loongemu_clint_write(opaque, offset, value, size);
}

int loongemu_clint_attach(LoongemuMmioBus *bus, LoongemuClint *clint,
                          uint64_t base)
{
    if (bus == NULL || clint == NULL) {
        return 0;
    }
    return loongemu_mmio_bus_add(bus, base, LOONGEMU_CLINT_SIZE,
                                 clint_read_cb, clint_write_cb, clint,
                                 "clint");
}
