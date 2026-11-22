#ifndef LATR_SYSTEM_H
#define LATR_SYSTEM_H

#include "latr.h"
#include "loongemu/mmio_bus.h"
#include "loongemu/phys_mem.h"
#include "loongemu/riscv_clint.h"
#include "loongemu/riscv_plic.h"
#include "loongemu/uart16550.h"
#include "loongemu/riscv_fw.h"
#include "loongemu/ovmf_board.h"
#include "loongemu/vclock.h"

/* Minimal RISC-V virt board used by the LATR system/boot loop. */
#define LATR_SYSTEM_RAM_BASE    UINT64_C(0x80000000)
#define LATR_SYSTEM_CLINT_BASE  UINT64_C(0x02000000)
#define LATR_SYSTEM_PLIC_BASE   UINT64_C(0x0c000000)
#define LATR_SYSTEM_UART_BASE   UINT64_C(0x10000000)
#define LATR_SYSTEM_EXIT_BASE   UINT64_C(0x00100000)
#define LATR_SYSTEM_FWCFG_BASE  UINT64_C(0x10100000)
#define LATR_SYSTEM_PFLASH_CODE UINT64_C(0x20000000)
#define LATR_SYSTEM_PFLASH_VARS UINT64_C(0x22000000)
#define LATR_SYSTEM_PFLASH_CODE_SIZE (1u << 20)
#define LATR_SYSTEM_PFLASH_VARS_SIZE (1u << 16)

typedef struct LatrSystem {
    CPUArchState env;
    LoongemuMemMap map;
    LoongemuMmioBus bus;
    LoongemuClint clint;
    LoongemuPlic plic;
    LoongemuUart16550 uart;
    LoongemuVClock vclock;
    uint8_t *ram;
    uint64_t ram_base;
    uint64_t ram_size;
    uint64_t exit_request;
    char uart_log[256];
    uint32_t uart_log_len;
    uint64_t rsdp_gpa;
    uint64_t acpi_tables_gpa;
    uint32_t acpi_tables_len;
    LoongemuFwCfg fw_cfg;
    LoongemuFwCfgDmaOps fw_dma_ops;
    LoongemuRiscvPflash pflash;
    uint8_t pflash_code[LATR_SYSTEM_PFLASH_CODE_SIZE];
    uint8_t pflash_vars[LATR_SYSTEM_PFLASH_VARS_SIZE];
} LatrSystem;

int latr_system_init(LatrSystem *sys, uint8_t *ram, uint64_t ram_size,
                     uint32_t hartid);
int latr_system_load_elf(LatrSystem *sys, const uint8_t *elf, uint64_t len,
                         uint64_t *entry);
void latr_system_update(LatrSystem *sys);
void latr_system_mmio_write(LatrSystem *sys, uint64_t pa, unsigned size,
                            uint64_t value);
const char *latr_system_uart_log(const LatrSystem *sys);
int latr_system_build_acpi(LatrSystem *sys, uint64_t *rsdp_gpa);
int latr_system_load_firmware(LatrSystem *sys, const uint8_t *image,
                              uint64_t len);

#endif
