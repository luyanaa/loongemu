#ifndef LOONGEMU_VMM_HANDOFF_H
#define LOONGEMU_VMM_HANDOFF_H

#include <stdint.h>

#include "loongemu/pci_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOONGEMU_VMM_HANDOFF_MAGIC   0x4c474d56u /* "LGMV" */
#define LOONGEMU_VMM_HANDOFF_VERSION 2u

/*
 * Shared Host LoongArch UEFI -> Guest x86 VMM (tb_sys) handoff block.
 *
 * Populated by baremetal/efi/vmm_entry.c before ExitBootServices() using
 * EFI_GRAPHICS_OUTPUT_PROTOCOL and Host ACPI 2.0 tables (SPCR + MCFG), then
 * consumed by tb_sys.c via tb_sys_vmm_main() after ExitBootServices().
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

    /* Allocated host physical windows handed from UEFI loader to System VMM */
    uint64_t vmm_image_base;
    uint64_t vmm_image_size;
    uint64_t guest_ram_hpa;
    uint64_t guest_ram_size;
    uint64_t code_cache_hpa;
    uint64_t code_cache_size;
    uint64_t fb_fallback_hpa;
    uint64_t fb_fallback_size;
    uint64_t ovmf_code_hpa;
    uint64_t ovmf_code_size;
    uint64_t ovmf_vars_hpa;
    uint64_t ovmf_vars_size;
} LoongemuVmmHandoff;

/* Exported by baremetal/efi/vmm_entry.c */
const LoongemuVmmHandoff *loongemu_vmm_get_handoff(void);

/* Consumed by baremetal/bare/tb_sys.c */
void tb_sys_set_vmm_handoff(const LoongemuVmmHandoff *handoff);
void tb_sys_vmm_main(const LoongemuVmmHandoff *handoff) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_VMM_HANDOFF_H */
