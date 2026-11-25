/* Guest-physical MMIO bus: window registry and width-checked dispatch. */
#include "loongemu/mmio_bus.h"

void loongemu_mmio_bus_init(LoongemuMmioBus *bus)
{
    if (bus == 0) {
        return;
    }
    for (int i = 0; i < LOONGEMU_MMIO_MAX_DEVICES; i++) {
        bus->devices[i].valid = 0;
    }
    for (int i = 0; i < LOONGEMU_MMIO_ROUTE_SLOTS; i++) {
        bus->routes[i].valid = 0;
    }
    bus->generation++;
}

static int overlaps(uint64_t a_base, uint64_t a_size, uint64_t b_base,
                    uint64_t b_size)
{
    return a_base < b_base + b_size && b_base < a_base + a_size;
}

int loongemu_mmio_bus_add(LoongemuMmioBus *bus, uint64_t base, uint64_t size,
                          LoongemuMmioReadFn read, LoongemuMmioWriteFn write,
                          void *opaque, const char *name)
{
    if (bus == 0 || size == 0 || (read == 0 && write == 0)) {
        return 0;
    }
    for (int i = 0; i < LOONGEMU_MMIO_MAX_DEVICES; i++) {
        if (bus->devices[i].valid &&
            overlaps(base, size, bus->devices[i].base,
                     bus->devices[i].size)) {
            return 0;
        }
    }
    for (int i = 0; i < LOONGEMU_MMIO_MAX_DEVICES; i++) {
        if (!bus->devices[i].valid) {
            bus->devices[i].base = base;
            bus->devices[i].size = size;
            bus->devices[i].read = read;
            bus->devices[i].write = write;
            bus->devices[i].opaque = opaque;
            bus->devices[i].name = name;
            bus->devices[i].valid = 1;
            bus->generation++;
            return 1;
        }
    }
    return 0;
}

int loongemu_mmio_bus_remove(LoongemuMmioBus *bus, uint64_t base)
{
    if (bus == 0) {
        return 0;
    }
    for (int i = 0; i < LOONGEMU_MMIO_MAX_DEVICES; i++) {
        if (bus->devices[i].valid && bus->devices[i].base == base) {
            bus->devices[i].valid = 0;
            bus->generation++;
            return 1;
        }
    }
    return 0;
}

static LoongemuMmioDevice *find_device(LoongemuMmioBus *bus, uint64_t pa,
                                       unsigned size)
{
    LoongemuMmioRoute *route;

    if (bus == 0 || size == 0 || size > 8) {
        return 0;
    }
    /* Same address and width as the last access through this slot, and the
     * registry has not changed since: the device index still answers it. */
    route = &bus->routes[(pa >> 2) & (LOONGEMU_MMIO_ROUTE_SLOTS - 1)];
    if (route->valid && route->pa == pa && route->size == size &&
        route->generation == bus->generation &&
        bus->devices[route->device].valid) {
        return &bus->devices[route->device];
    }
    for (int i = 0; i < LOONGEMU_MMIO_MAX_DEVICES; i++) {
        LoongemuMmioDevice *dev = &bus->devices[i];
        if (!dev->valid) {
            continue;
        }
        if (pa >= dev->base && pa + size <= dev->base + dev->size) {
            route->pa = pa;
            route->size = size;
            route->device = (uint32_t)i;
            route->generation = bus->generation;
            route->valid = 1;
            return dev;
        }
    }
    route->valid = 0;
    return 0;
}

void loongemu_mmio_bus_set_lock(LoongemuMmioBus *bus, LoongemuMmioLockFn lock,
                                LoongemuMmioLockFn unlock, void *opaque)
{
    if (bus == 0) {
        return;
    }
    bus->lock = lock;
    bus->unlock = unlock;
    bus->lock_opaque = opaque;
}

int loongemu_mmio_bus_read(LoongemuMmioBus *bus, uint64_t pa, unsigned size,
                           uint64_t *value)
{
    LoongemuMmioDevice *dev = find_device(bus, pa, size);
    uint64_t result;

    if (dev == 0 || dev->read == 0) {
        return -1;
    }
    if (bus->lock != 0) {
        bus->lock(bus->lock_opaque);
    }
    result = dev->read(dev->opaque, pa - dev->base, size);
    if (bus->unlock != 0) {
        bus->unlock(bus->lock_opaque);
    }
    if (value != 0) {
        *value = result;
    }
    return 0;
}

int loongemu_mmio_bus_write(LoongemuMmioBus *bus, uint64_t pa, unsigned size,
                            uint64_t value)
{
    LoongemuMmioDevice *dev = find_device(bus, pa, size);

    if (dev == 0 || dev->write == 0) {
        return -1;
    }
    if (bus->lock != 0) {
        bus->lock(bus->lock_opaque);
    }
    dev->write(dev->opaque, pa - dev->base, value, size);
    if (bus->unlock != 0) {
        bus->unlock(bus->lock_opaque);
    }
    return 0;
}
