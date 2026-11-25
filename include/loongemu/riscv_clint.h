#ifndef LOONGEMU_RISCV_CLINT_H
#define LOONGEMU_RISCV_CLINT_H

#include <stdint.h>
#include "loongemu/mmio_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SiFive CLINT: per-hart MSIP (4 bytes), per-hart MTIMECMP (8 bytes),
 * global MTIME (8 bytes). Offsets follow the standard 0x2000000 layout. */
#define LOONGEMU_CLINT_MSIP_BASE     0x0000u
#define LOONGEMU_CLINT_MTIMECMP_BASE 0x4000u
#define LOONGEMU_CLINT_MTIME         0xbff8u
#define LOONGEMU_CLINT_SIZE          0x10000u
#define LOONGEMU_CLINT_MAX_HARTS     8

/*
 * A software interrupt is the only wake-up a parked hart (WFI) can be given
 * from another hart, and the write itself is the only moment the target can be
 * reached: by the time anybody polls mip, the target is already asleep. The
 * notify hook is therefore called from the MSIP write path, with the hart index
 * whose msip became non-zero. It must not call back into the CLINT.
 */
typedef void (*LoongemuClintMsipNotify)(void *opaque, uint32_t hart);

typedef struct LoongemuClint {
    uint32_t msip[LOONGEMU_CLINT_MAX_HARTS];
    uint64_t mtimecmp[LOONGEMU_CLINT_MAX_HARTS];
    uint64_t mtime;
    uint32_t hart_count;
    LoongemuClintMsipNotify msip_notify;
    void *msip_opaque;
} LoongemuClint;

void loongemu_clint_init(LoongemuClint *clint, uint32_t hart_count);

/* MMIO accessors; offset is relative to the CLINT base. */
uint64_t loongemu_clint_read(LoongemuClint *clint, uint64_t offset,
                             unsigned size);
void loongemu_clint_write(LoongemuClint *clint, uint64_t offset,
                          uint64_t value, unsigned size);

/* Advance MTIME and reflect MSIP/MTIP into MIP for one hart. */
void loongemu_clint_tick(LoongemuClint *clint, uint64_t now,
                         uint64_t *mip, uint32_t hartid);

int loongemu_clint_attach(LoongemuMmioBus *bus, LoongemuClint *clint,
                          uint64_t base);

#ifdef __cplusplus
}
#endif
#endif
