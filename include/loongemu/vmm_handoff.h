#ifndef LOONGEMU_VMM_HANDOFF_H
#define LOONGEMU_VMM_HANDOFF_H

#include <stdint.h>

#include "loongemu/pci_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOONGEMU_VMM_HANDOFF_MAGIC   0x4c474d56u /* "LGMV" */
#define LOONGEMU_VMM_HANDOFF_VERSION 1u

/*
 * Shared Host LoongArch UEFI -> Guest x86 VMM (tb_sys) handoff block.
 *
 * Populated by baremetal/efi/vmm_entry.c before ExitBootServices() using
 * EFI_GRAPHICS_OUTPUT_PROTOCOL and Host ACPI 2.0 tables (SPCR + MCFG), then
 * consumed by tb_sys.c to bind Bochs Display BAR0 zero-copy to the Host GOP
 * framebuffer HPA and configure Loongson 7A1000/7A2000 Split-CFG + DC hiding.
 */
typedef struct LoongemuVmmHandoff {
    uint32_t magic;
    uint32_t version;

    /* Host UEFI GOP linear framebuffer descriptor (survives ExitBootServices) */
    LoongemuGopHandover gop;

    /* Host ACPI 2.0 & console/timer discovery */
    uint64_t host_acpi_rsdp;
    uint64_t host_uart_phys;
    uint64_t host_timer_hz;
    uint8_t  host_uart_reg_shift;
    uint8_t  reserved0[7];

    /* Host Loongson 7A1000/7A2000 PCIe Split-CFG bases (from ACPI MCFG) */
    uint64_t ls7a_cfg_type0_base;
    uint64_t ls7a_cfg_type1_base;
    uint16_t ls7a_pci_segment;
    uint8_t  ls7a_start_bus;
    uint8_t  ls7a_end_bus;
    uint32_t ls7a_mcfg_valid;
    uint32_t hide_host_dc_gpu;
    uint32_t reserved1;
} LoongemuVmmHandoff;

/* Exported by baremetal/efi/vmm_entry.c */
const LoongemuVmmHandoff *loongemu_vmm_get_handoff(void);

/* Consumed by baremetal/bare/tb_sys.c */
void tb_sys_set_vmm_handoff(const LoongemuVmmHandoff *handoff);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_VMM_HANDOFF_H */
