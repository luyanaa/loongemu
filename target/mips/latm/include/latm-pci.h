#ifndef LATM_PCI_H
#define LATM_PCI_H

#include <stdint.h>
#include "loongemu/pci_host.h"
#include "latm-system.h"

typedef struct LatmPci {
    LoongemuPciHost host;
    int ls7a_mock;
} LatmPci;

void latm_pci_init(LatmPci *pci, const LoongemuGopHandover *gop);
void latm_pci_attach_ls7a_mock(LatmPci *pci, uint64_t type0, uint64_t type1,
                               uint32_t (*read32)(uint64_t),
                               void (*write32)(uint64_t, uint32_t));
uint32_t latm_pci_cfg_read(LatmPci *pci, uint8_t bus, uint8_t dev,
                           uint8_t fn, uint16_t reg, unsigned size);
void latm_pci_cfg_write(LatmPci *pci, uint8_t bus, uint8_t dev, uint8_t fn,
                        uint16_t reg, uint32_t value, unsigned size);
int latm_pci_map_bochs_mmio(LatmPci *pci, LatmMmioBus *bus);
int latm_pci_quiesce(LatmPci *pci);

#endif
