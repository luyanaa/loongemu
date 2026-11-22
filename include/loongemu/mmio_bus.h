#ifndef LOONGEMU_MMIO_BUS_H
#define LOONGEMU_MMIO_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Guest-physical MMIO bus. Devices register a [base, base+size) window and
 * service width-specific accesses. The emitter calls into this from the
 * translated load/store path, so every access has guest-visible side effects
 * and ordering (no direct dereference of a device HPA).
 */

#define LOONGEMU_MMIO_MAX_DEVICES 16

typedef uint64_t (*LoongemuMmioReadFn)(void *opaque, uint64_t offset,
                                       unsigned size);
typedef void (*LoongemuMmioWriteFn)(void *opaque, uint64_t offset,
                                    uint64_t value, unsigned size);

typedef struct LoongemuMmioDevice {
    uint64_t base;
    uint64_t size;
    LoongemuMmioReadFn read;
    LoongemuMmioWriteFn write;
    void *opaque;
    const char *name;
    int valid;
} LoongemuMmioDevice;

typedef struct LoongemuMmioBus {
    LoongemuMmioDevice devices[LOONGEMU_MMIO_MAX_DEVICES];
} LoongemuMmioBus;

void loongemu_mmio_bus_init(LoongemuMmioBus *bus);

/* Registers a device window; overlapping/zero-sized/out-of-slots fail. */
int loongemu_mmio_bus_add(LoongemuMmioBus *bus, uint64_t base, uint64_t size,
                          LoongemuMmioReadFn read, LoongemuMmioWriteFn write,
                          void *opaque, const char *name);
int loongemu_mmio_bus_remove(LoongemuMmioBus *bus, uint64_t base);

/* Returns 0 on success; -1 when no device claims [pa, pa+size). */
int loongemu_mmio_bus_read(LoongemuMmioBus *bus, uint64_t pa, unsigned size,
                           uint64_t *value);
int loongemu_mmio_bus_write(LoongemuMmioBus *bus, uint64_t pa, unsigned size,
                            uint64_t value);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_MMIO_BUS_H */
