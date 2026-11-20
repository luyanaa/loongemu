#include "qemu/osdep.h"
#include <stdio.h>
#include "latm-pci.h"

static int latm_pci_bar_map(void *opaque, uint32_t bar_idx, uint64_t gpa,
                            uint64_t size, int enabled)
{
    LatmPci *pci = opaque;
    if (!pci || !pci->bus) return 1;
    char name[32];
    if (!enabled || !size) {
        if (gpa) latm_mmio_remove(pci->bus, gpa);
        return 1;
    }
    snprintf(name, sizeof(name), "pci-bar%u", bar_idx);
    if (!latm_mmio_add(pci->bus, gpa, size, LATM_MMIO_DEVICE, 0,
                       loongemu_bochs_mmio_read, loongemu_bochs_mmio_write,
                       &pci->host.display, name)) {
        /* A BAR may alias an existing fixed display window. */
        return latm_mmio_find(pci->bus, gpa, 1) != NULL;
    }
    return 1;
}

void latm_pci_init(LatmPci *pci, const LoongemuGopHandover *gop)
{
    if (!pci) return;
    memset(pci, 0, sizeof(*pci));
    loongemu_pci_host_init(&pci->host, gop, latm_pci_bar_map, pci);
}

void latm_pci_bind_bus(LatmPci *pci, LatmMmioBus *bus)
{
    if (pci) pci->bus = bus;
}

void latm_pci_attach_ls7a_mock(LatmPci *pci, uint64_t type0, uint64_t type1,
                               uint32_t (*read32)(uint64_t),
                               LoongemuLs7aConfigWriteFn write_config)
{
    if (!pci) return;
    loongemu_pci_host_attach_ls7a(&pci->host, type0, type1, 1,
                                  read32, write_config);
    pci->ls7a_mock = 1;
}

uint32_t latm_pci_cfg_read(LatmPci *pci, uint8_t bus, uint8_t dev,
                           uint8_t fn, uint16_t reg, unsigned size)
{
    return pci ? loongemu_pci_cfg_read(&pci->host, bus, dev, fn, reg, size)
               : 0xffffffffu;
}

void latm_pci_cfg_write(LatmPci *pci, uint8_t bus, uint8_t dev, uint8_t fn,
                        uint16_t reg, uint32_t value, unsigned size)
{
    if (pci) {
        loongemu_pci_cfg_write(&pci->host, bus, dev, fn, reg, value, size);
    }
}

int latm_pci_map_bochs_mmio(LatmPci *pci, LatmMmioBus *bus)
{
    if (!pci || !bus) return 0;
    return latm_mmio_add(bus, pci->host.display.mmio_gpa,
                         LOONGEMU_BOCHS_MMIO_SIZE, LATM_MMIO_DEVICE, 0,
                         loongemu_bochs_mmio_read, loongemu_bochs_mmio_write,
                         &pci->host.display, "bochs-mmio");
}

int latm_pci_quiesce(LatmPci *pci)
{
    if (!pci) return 0;
    /* Drop bus-master enable before handing the RC back to the host. */
    for (unsigned i = 0; i < LOONGEMU_PCI_MAX_DEVS; i++) {
        LoongemuPciDev *d = &pci->host.devs[i];
        if (d->used) {
            d->cfg[4] &= (uint8_t)~0x04u;
        }
    }
    return pci->ls7a_mock ? 0 : 1;
}
