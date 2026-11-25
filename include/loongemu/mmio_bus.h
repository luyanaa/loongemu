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

/*
 * Direct-mapped route cache. Guest MMIO traffic is dominated by a handful of
 * doorbell/status registers polled or written over and over, so remembering
 * which window answered the last access at an address skips the device scan.
 * `generation` moves on init/add/remove; a route that does not carry the
 * current generation is ignored, so stale slots can never redirect an access.
 */
#define LOONGEMU_MMIO_ROUTE_SLOTS 8

typedef struct LoongemuMmioRoute {
    uint64_t pa;
    uint32_t size;
    uint32_t device; /* index into devices[] */
    uint32_t generation;
    int valid;
} LoongemuMmioRoute;

/*
 * Optional serialisation for a machine whose harts run on separate host
 * threads. Device windows are shared mutable state, and the emitter calls the
 * bus directly from translated code, so the lock hook has to live on the bus
 * rather than in the caller. A single-threaded runtime leaves them NULL.
 */
typedef void (*LoongemuMmioLockFn)(void *opaque);

typedef struct LoongemuMmioBus {
    LoongemuMmioDevice devices[LOONGEMU_MMIO_MAX_DEVICES];
    LoongemuMmioRoute routes[LOONGEMU_MMIO_ROUTE_SLOTS];
    uint32_t generation;
    LoongemuMmioLockFn lock;
    LoongemuMmioLockFn unlock;
    void *lock_opaque;
} LoongemuMmioBus;

void loongemu_mmio_bus_init(LoongemuMmioBus *bus);
/*
 * Installs the lock used around every routed access (NULL disables). The lock
 * must not be held across a guest instruction, only across one device access.
 */
void loongemu_mmio_bus_set_lock(LoongemuMmioBus *bus, LoongemuMmioLockFn lock,
                                LoongemuMmioLockFn unlock, void *opaque);

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
