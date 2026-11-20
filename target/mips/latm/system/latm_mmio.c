#include "qemu/osdep.h"
#include "latm-system.h"

void latm_mmio_init(LatmMmioBus *bus)
{
    if (bus) {
        memset(bus, 0, sizeof(*bus));
    }
}

int latm_mmio_add(LatmMmioBus *bus, uint64_t base, uint64_t size,
                  LatmMmioKind kind, uint8_t mat, LatmMmioReadFn read,
                  LatmMmioWriteFn write, void *opaque, const char *name)
{
    if (!bus || size == 0 || bus->count >= LATM_MMIO_MAX_REGIONS ||
        base + size < base) {
        return 0;
    }
    for (unsigned i = 0; i < bus->count; i++) {
        LatmMmioRegion *r = &bus->regions[i];
        if (base < r->base + r->size && r->base < base + size) {
            return 0;
        }
    }
    LatmMmioRegion *r = &bus->regions[bus->count++];
    memset(r, 0, sizeof(*r));
    r->base = base;
    r->size = size;
    r->kind = kind;
    r->mat = mat;
    r->used = 1;
    r->read = read;
    r->write = write;
    r->opaque = opaque;
    r->name = name;
    return 1;
}

int latm_mmio_remove(LatmMmioBus *bus, uint64_t base)
{
    if (!bus) return 0;
    for (unsigned i = 0; i < bus->count; i++) {
        if (bus->regions[i].base == base) {
            bus->regions[i] = bus->regions[--bus->count];
            return 1;
        }
    }
    return 0;
}

LatmMmioRegion *latm_mmio_find(LatmMmioBus *bus, uint64_t addr, unsigned size)
{
    if (!bus || size == 0) return NULL;
    for (unsigned i = 0; i < bus->count; i++) {
        LatmMmioRegion *r = &bus->regions[i];
        if (addr >= r->base && addr - r->base <= r->size &&
            size <= r->size - (addr - r->base)) {
            return r;
        }
    }
    return NULL;
}

int latm_mmio_read(LatmMmioBus *bus, uint64_t addr, unsigned size,
                   uint64_t *value)
{
    LatmMmioRegion *r = latm_mmio_find(bus, addr, size);
    if (!r || !r->read || !value) {
        return 0;
    }
    *value = r->read(r->opaque, addr - r->base, size);
    return 1;
}

int latm_mmio_write(LatmMmioBus *bus, uint64_t addr, unsigned size,
                    uint64_t value)
{
    LatmMmioRegion *r = latm_mmio_find(bus, addr, size);
    if (!r || !r->write) {
        return 0;
    }
    r->write(r->opaque, addr - r->base, value, size);
    return 1;
}

uint64_t latm_mmio_ram_read(void *opaque, uint64_t offset, unsigned size)
{
    if (!opaque || (size != 1 && size != 2 && size != 4 && size != 8)) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)opaque + offset;
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

void latm_mmio_ram_write(void *opaque, uint64_t offset, uint64_t value,
                         unsigned size)
{
    if (!opaque || (size != 1 && size != 2 && size != 4 && size != 8)) {
        return;
    }
    uint8_t *p = (uint8_t *)opaque + offset;
    for (unsigned i = 0; i < size; i++) {
        p[i] = (uint8_t)(value >> (8 * i));
    }
}
