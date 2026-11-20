#include "qemu/osdep.h"
#include "latm-pci.h"

void latm_pci_init(LatmPci *pci, const LoongemuGopHandover *gop)
{
    if (!pci) return;
    memset(pci, 0, sizeof(*pci));
    loongemu_pci_host_init(&pci->host, gop, NULL, NULL);
}

void latm_pci_attach_ls7a_mock(LatmPci *pci, uint64_t type0, uint64_t type1,
                               uint32_t (*read32)(uint64_t),
                               void (*write32)(uint64_t, uint32_t))
{
    if (!pci) return;
    loongemu_pci_host_attach_ls7a(&pci->host, type0, type1, 1,
                                  read32, write32);
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
    /* Real 7A devices are not touched until the host handoff path exists. */
    return pci->ls7a_mock ? 0 : 1;
}
