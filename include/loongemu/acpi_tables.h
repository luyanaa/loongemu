#ifndef LOONGEMU_ACPI_TABLES_H
#define LOONGEMU_ACPI_TABLES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard guest physical addresses for synthesized x86-64 ACPI 2.0 tables. */
#define LOONGEMU_ACPI_RSDP_GPA      0x000e0000ULL
#define LOONGEMU_ACPI_TABLES_GPA    0x000e1000ULL
#define LOONGEMU_ACPI_PM1A_EVT_PORT 0x0600u
#define LOONGEMU_ACPI_PM1A_CNT_PORT 0x0604u
#define LOONGEMU_ACPI_PM_TMR_PORT   0x0608u
#define LOONGEMU_LAPIC_GPA          0xfee00000ULL
#define LOONGEMU_IOAPIC_GPA         0xfec00000ULL

typedef struct __attribute__((packed)) AcpiRsdp {
    char signature[8];       /* "RSD PTR " */
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;        /* 2 for ACPI 2.0+ */
    uint32_t rsdt_address;
    uint32_t length;         /* 36 */
    uint64_t xsdt_address;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} AcpiRsdp;

typedef struct __attribute__((packed)) AcpiSdtHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} AcpiSdtHeader;

typedef struct __attribute__((packed)) AcpiGas {
    uint8_t address_space_id;  /* 1 = System I/O */
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_size;       /* 2 = 16-bit, 3 = 32-bit */
    uint64_t address;
} AcpiGas;

typedef struct __attribute__((packed)) AcpiFadt {
    AcpiSdtHeader hdr;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint8_t cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved1;
    uint32_t flags;
    AcpiGas reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t fadt_minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    AcpiGas x_pm1a_evt_blk;
    AcpiGas x_pm1b_evt_blk;
    AcpiGas x_pm1a_cnt_blk;
    AcpiGas x_pm1b_cnt_blk;
    AcpiGas x_pm2_cnt_blk;
    AcpiGas x_pm_tmr_blk;
    AcpiGas x_gpe0_blk;
    AcpiGas x_gpe1_blk;
} AcpiFadt;

typedef struct __attribute__((packed)) AcpiMadtHeader {
    AcpiSdtHeader hdr;
    uint32_t lapic_addr;
    uint32_t flags;
} AcpiMadtHeader;

typedef struct __attribute__((packed)) AcpiMadtLapic {
    uint8_t type;              /* 0 */
    uint8_t length;            /* 8 */
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;            /* bit 0: enabled */
} AcpiMadtLapic;

typedef struct __attribute__((packed)) AcpiMadtIoapic {
    uint8_t type;              /* 1 */
    uint8_t length;            /* 12 */
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
} AcpiMadtIoapic;

typedef struct __attribute__((packed)) AcpiMadtIso {
    uint8_t type;              /* 2 */
    uint8_t length;            /* 10 */
    uint8_t bus;               /* 0 = ISA */
    uint8_t source;            /* ISA IRQ */
    uint32_t gsi;              /* Global System Interrupt */
    uint16_t flags;
} AcpiMadtIso;

typedef struct LoongemuAcpiInfo {
    uint64_t rsdp_gpa;
    uint64_t xsdt_gpa;
    uint64_t fadt_gpa;
    uint64_t madt_gpa;
    uint64_t dsdt_gpa;
    uint32_t nr_cpus;
} LoongemuAcpiInfo;

typedef enum LoongemuAcpiProfile {
    LOONGEMU_ACPI_PROFILE_LINUX   = 0, /* Inherit/pass through host UEFI ACPI 6.5 metadata */
    LOONGEMU_ACPI_PROFILE_WINDOWS = 1, /* Windows OEM override + HPET/MCFG hook */
} LoongemuAcpiProfile;

typedef struct LoongemuAcpiSpcrInfo {
    uint64_t base_addr;
    uint8_t  addr_space;   /* 0 = System Memory (MMIO), 1 = System I/O */
    uint8_t  reg_width;    /* bits (8/32) */
    uint8_t  access_size;  /* 1=byte, 2=word, 3=dword */
    uint8_t  interface_type;
    uint8_t  baud_rate;
} LoongemuAcpiSpcrInfo;

/*
 * Build complete x86-64 ACPI 2.0 tables (RSDP, RSDT, XSDT, FADT, DSDT, MADT)
 * inside guest RAM.  Returns 1 on success, 0 on invalid arguments.
 */
int loongemu_acpi_build(uint8_t *guest_ram,
                        uint64_t rsdp_gpa,
                        uint64_t tables_gpa,
                        uint32_t nr_cpus,
                        LoongemuAcpiInfo *out_info);

/*
 * Profile-aware ACPI builder:
 *  - LOONGEMU_ACPI_PROFILE_LINUX: inherits host UEFI ACPI 6.5 OEM ID when
 *    host_rsdp_ptr is provided (and keeps PCI/platform identity unmodified).
 *  - LOONGEMU_ACPI_PROFILE_WINDOWS: allows OEM ID override for Windows HAL.
 */
int loongemu_acpi_build_for_profile(uint8_t *guest_ram,
                                    uint64_t rsdp_gpa,
                                    uint64_t tables_gpa,
                                    uint32_t nr_cpus,
                                    LoongemuAcpiProfile profile,
                                    const void *host_rsdp_ptr,
                                    const char *oem_override_6,
                                    LoongemuAcpiInfo *out_info);

/*
 * Validate a host UEFI ACPI RSDP pointer (passed via EFI_ACPI_20_TABLE_GUID)
 * and extract the OEM ID (6 bytes + NUL).  Returns 1 if valid ACPI 1.0/2.0 RSDP.
 */
int loongemu_acpi_validate_rsdp(const void *rsdp_ptr,
                                uint8_t *out_rev,
                                uint64_t *out_xsdt_pa,
                                char out_oem_id[7]);

/*
 * Parse the host UEFI ACPI XSDT/RSDT to locate the SPCR (Serial Port Console
 * Redirection) table and extract the real hardware UART MMIO/IO base address.
 */
int loongemu_acpi_find_spcr(const void *host_rsdp_ptr,
                            const void *(*pa_to_ptr)(uint64_t pa),
                            LoongemuAcpiSpcrInfo *out_spcr);

#ifdef __cplusplus
}
#endif

#endif
