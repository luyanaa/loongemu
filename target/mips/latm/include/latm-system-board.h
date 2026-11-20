#ifndef LATM_SYSTEM_BOARD_H
#define LATM_SYSTEM_BOARD_H

#include "latm-system.h"
#include "latm-pci.h"
#include "latm-uncore.h"
#include "latm-uart.h"

typedef struct LatmSystem {
    LatmMemoryLayout memory;
    LatmMmioBus mmio;
    LatmPci pci;
    LatmUncore uncore;
    LatmUart uart;
    LatmBootInfo boot;
    uint8_t *ram;
    uint64_t ram_hpa;
    uint64_t ram_size;
} LatmSystem;

int latm_system_init_3a4000(LatmSystem *sys, uint8_t *ram, uint64_t ram_hpa,
                            uint64_t ram_size, uint64_t vmm_base,
                            uint64_t vmm_size, uint64_t code_base,
                            uint64_t code_size);
int latm_system_mmio_read(LatmSystem *sys, uint64_t gpa, unsigned size,
                          uint64_t *value);
int latm_system_mmio_write(LatmSystem *sys, uint64_t gpa, unsigned size,
                           uint64_t value);
size_t latm_system_build_dtb(LatmSystem *sys, uint8_t *out, size_t cap,
                             uint8_t *struct_buf, size_t struct_cap,
                             uint8_t *strings_buf, size_t strings_cap);
void latm_system_set_boot_args(LatmSystem *sys, uint64_t dtb_gpa,
                               uint64_t initrd_start, uint64_t initrd_end,
                               const char *cmdline);
void latm_system_apply_boot_args(LatmSystem *sys, void *env);

#endif
