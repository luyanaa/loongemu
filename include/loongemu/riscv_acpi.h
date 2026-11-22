#ifndef LOONGEMU_RISCV_ACPI_H
#define LOONGEMU_RISCV_ACPI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RISC-V ACPI table builder: MADT (APIC) with RINTC + IMSIC + APLIC entries
 * and the RHCT (RISC-V Hart Capabilities Table).
 *
 * Layouts follow ACPI 6.6 and QEMU's hw/riscv/virt-acpi-build.c so guests
 * written against the QEMU virt platform parse the same structures. The
 * builder only synthesizes the tables; where they land (guest memory, fw_cfg,
 * pflash) is the caller's job.
 */

#define LOONGEMU_ACPI_MADT_RINTC          0x18
#define LOONGEMU_ACPI_MADT_IMSIC          0x19
#define LOONGEMU_ACPI_MADT_APLIC          0x1A
#define LOONGEMU_ACPI_MADT_PLIC           0x1B

#define LOONGEMU_ACPI_MADT_REVISION       7
#define LOONGEMU_ACPI_RHCT_REVISION       1
#define LOONGEMU_ACPI_FADT_REVISION        6
#define LOONGEMU_ACPI_FADT_LENGTH          276
#define LOONGEMU_ACPI_SPCR_REVISION        4
#define LOONGEMU_ACPI_SPCR_LENGTH          89
#define LOONGEMU_ACPI_MCFG_REVISION        1
#define LOONGEMU_ACPI_FADT_HW_REDUCED      (UINT32_C(1) << 20)
#define LOONGEMU_ACPI_DSDT_REVISION        2
/* Internal AML scratch: the DSDT is built twice (size probe, then layout). */
#define LOONGEMU_ACPI_DSDT_SCRATCH         16384

#define LOONGEMU_RHCT_ISA_NODE            0x0000
#define LOONGEMU_RHCT_CMO_NODE            0x0001
#define LOONGEMU_RHCT_MMU_NODE            0x0002
#define LOONGEMU_RHCT_HART_INFO_NODE      0xFFFF

#define LOONGEMU_RHCT_MMU_SV39            0
#define LOONGEMU_RHCT_MMU_SV48            1
#define LOONGEMU_RHCT_MMU_SV57            2

typedef struct __attribute__((packed)) LoongemuAcpiSdtHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} LoongemuAcpiSdtHeader;

typedef struct __attribute__((packed)) LoongemuAcpiRintc {
    uint8_t type;
    uint8_t length;
    uint8_t version;
    uint8_t reserved;
    uint32_t flags;      /* bit 0: enabled, bit 1: online capable */
    uint64_t hart_id;
    uint32_t uid;        /* ACPI processor UID */
    uint32_t ext_intc_id;
    uint64_t imsic_addr; /* 0 unless AIA with IMSIC */
    uint32_t imsic_size;
} LoongemuAcpiRintc;

typedef struct __attribute__((packed)) LoongemuAcpiImsic {
    uint8_t type;
    uint8_t length;
    uint8_t version;
    uint8_t reserved;
    uint32_t flags;
    uint16_t num_ids;
    uint16_t num_guest_ids;
    uint8_t guest_index_bits;
    uint8_t hart_index_bits;
    uint8_t group_index_bits;
    uint8_t group_index_shift;
} LoongemuAcpiImsic;

typedef struct __attribute__((packed)) LoongemuAcpiAplic {
    uint8_t type;
    uint8_t length;
    uint8_t version;
    uint8_t id;
    uint32_t flags;
    uint8_t hw_id[8];
    uint16_t num_idcs;
    uint16_t num_sources;
    uint32_t gsi_base;
    uint64_t base_addr;
    uint32_t size;
} LoongemuAcpiAplic;

typedef struct __attribute__((packed)) LoongemuAcpiPlic {
    uint8_t type;
    uint8_t length;
    uint8_t version;
    uint8_t id;
    uint8_t hw_id[8];
    uint16_t num_irqs;
    uint16_t max_prio;
    uint32_t flags;
    uint32_t size;
    uint64_t base_addr;
    uint32_t gsi_base;
} LoongemuAcpiPlic;

typedef struct LoongemuRiscvAcpiVirtio {
    uint64_t base;
    uint32_t size;
    uint32_t gsi;
} LoongemuRiscvAcpiVirtio;

typedef struct LoongemuRiscvAcpiHart {
    uint64_t hart_id;
    uint32_t uid;
    uint32_t ext_intc_id;
    uint64_t imsic_addr;
    uint32_t imsic_size;
    uint32_t enabled;
} LoongemuRiscvAcpiHart;

typedef struct LoongemuRiscvAcpi {
    const char *oem_id;      /* up to 6 chars, padded */
    const char *oem_table_id;/* up to 8 chars, padded */

    /* MADT */
    const LoongemuRiscvAcpiHart *harts;
    uint32_t hart_count;
    int has_imsic;
    uint16_t imsic_num_ids;
    uint16_t imsic_num_guest_ids;
    uint8_t imsic_guest_index_bits;
    uint8_t imsic_hart_index_bits;
    uint8_t imsic_group_index_bits;
    uint8_t imsic_group_index_shift;
    int has_aplic;
    uint8_t aplic_id;
    const char *aplic_hw_id; /* up to 8 chars, padded */
    uint16_t aplic_num_idcs;
    uint16_t aplic_num_sources;
    uint32_t aplic_gsi_base;
    uint64_t aplic_addr;
    uint32_t aplic_size;
    int has_plic;
    uint8_t plic_id;
    const char *plic_hw_id; /* up to 8 chars, padded */
    uint16_t plic_num_irqs;
    uint16_t plic_max_prio;
    uint64_t plic_addr;
    uint32_t plic_size;
    uint32_t plic_gsi_base;

    /* FADT (revision 6, hardware-reduced). DSDT address is patched by the
     * caller once the DSDT table placement is known. */
    uint64_t dsdt_gpa;
    uint8_t fadt_minor_version;
    uint16_t sci_int;
    uint32_t iapc_boot_arch;
    uint32_t fadt_flags_extra; /* OR'ed with HW_REDUCED_ACPI */

    /* SPCR: 16550-compatible UART. */
    int has_spcr;
    uint8_t spcr_interface_type; /* 0x12 = 16550 compatible */
    uint64_t spcr_base;
    uint32_t spcr_gsi;
    uint8_t spcr_baud_rate;
    uint8_t spcr_terminal_type;
    const char *spcr_namespace; /* e.g. "." */

    /* MCFG: PCIe ECAM region. */
    int has_mcfg;
    uint64_t mcfg_ecam_base;
    uint16_t mcfg_segment;
    uint8_t mcfg_bus_start;
    uint8_t mcfg_bus_end;

    /* DSDT: minimal AML for CPUs, PCIe host bridge, UART and virtio-mmio. */
    int has_dsdt;
    const char *pci_hid;      /* "PNP0A08" (PCIe) or "PNP0A03" (PCI) */
    uint16_t pci_segment;
    uint8_t pci_bus_start;
    uint8_t pci_bus_end;
    uint64_t pci_ecam_base;
    uint64_t pci_ecam_size;
    uint64_t pci_mmio_base;
    uint64_t pci_mmio_size;
    const char *uart_hid;     /* "PNP0501" */
    uint64_t uart_base;
    uint32_t uart_size;
    uint32_t uart_gsi;
    const LoongemuRiscvAcpiVirtio *virtio;
    uint32_t virtio_count;

    /* RHCT */
    const char *isa;         /* e.g. "rv64imafdc" */
    uint64_t timer_hz;
    int has_cmo;
    uint8_t cbom_block_log2;
    uint8_t cboz_block_log2;
    int has_mmu;
    uint8_t mmu_type;        /* LOONGEMU_RHCT_MMU_* */
} LoongemuRiscvAcpi;

/* Sizes include the 36-byte SDT header and all configured entries. */
uint32_t loongemu_riscv_acpi_madt_size(const LoongemuRiscvAcpi *cfg);
uint32_t loongemu_riscv_acpi_rhct_size(const LoongemuRiscvAcpi *cfg);
uint32_t loongemu_riscv_acpi_fadt_size(const LoongemuRiscvAcpi *cfg);
uint32_t loongemu_riscv_acpi_spcr_size(const LoongemuRiscvAcpi *cfg);
uint32_t loongemu_riscv_acpi_mcfg_size(const LoongemuRiscvAcpi *cfg);
uint32_t loongemu_riscv_acpi_dsdt_size(const LoongemuRiscvAcpi *cfg);

/* RSDP (revision 2, 36 bytes) and XSDT, for fw_cfg-mediated handoff. */
void loongemu_riscv_acpi_rsdp_build(uint8_t out[36], uint32_t rsdt_gpa,
                                    uint64_t xsdt_gpa, const char *oem_id);
uint32_t loongemu_riscv_acpi_xsdt_size(uint32_t count);
uint32_t loongemu_riscv_acpi_xsdt_build(uint8_t *buf, uint32_t cap,
                                        const uint64_t *table_gpas,
                                        uint32_t count,
                                        const LoongemuRiscvAcpi *cfg);

/*
 * Builds the table into `buf`. Returns the number of bytes written, or 0 when
 * the buffer is smaller than the size reported above. The ACPI checksum is
 * computed over the whole table.
 */
uint32_t loongemu_riscv_acpi_madt_build(const LoongemuRiscvAcpi *cfg,
                                        uint8_t *buf, uint32_t cap);
uint32_t loongemu_riscv_acpi_rhct_build(const LoongemuRiscvAcpi *cfg,
                                        uint8_t *buf, uint32_t cap);
uint32_t loongemu_riscv_acpi_fadt_build(const LoongemuRiscvAcpi *cfg,
                                        uint8_t *buf, uint32_t cap);
uint32_t loongemu_riscv_acpi_spcr_build(const LoongemuRiscvAcpi *cfg,
                                        uint8_t *buf, uint32_t cap);
uint32_t loongemu_riscv_acpi_mcfg_build(const LoongemuRiscvAcpi *cfg,
                                        uint8_t *buf, uint32_t cap);
uint32_t loongemu_riscv_acpi_dsdt_build(const LoongemuRiscvAcpi *cfg,
                                        uint8_t *buf, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_RISCV_ACPI_H */
