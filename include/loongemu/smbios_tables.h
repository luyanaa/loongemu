#ifndef LOONGEMU_SMBIOS_TABLES_H
#define LOONGEMU_SMBIOS_TABLES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compile-time configurable SMBIOS 3.0 / DMI & hardware identity defaults.
 * Override any macro via -D at compile time or pass a custom
 * LoongemuSmbiosConfig at runtime.
 */
#ifndef LOONGEMU_SMBIOS_BIOS_VENDOR
#define LOONGEMU_SMBIOS_BIOS_VENDOR    "American Megatrends Inc."
#endif
#ifndef LOONGEMU_SMBIOS_BIOS_VERSION
#define LOONGEMU_SMBIOS_BIOS_VERSION   "F24"
#endif
#ifndef LOONGEMU_SMBIOS_BIOS_DATE
#define LOONGEMU_SMBIOS_BIOS_DATE      "05/12/2025"
#endif
#ifndef LOONGEMU_SMBIOS_SYS_MANUF
#define LOONGEMU_SMBIOS_SYS_MANUF      "Loongson Technology"
#endif
#ifndef LOONGEMU_SMBIOS_SYS_PRODUCT
#define LOONGEMU_SMBIOS_SYS_PRODUCT    "LS3A6000-Workstation"
#endif
#ifndef LOONGEMU_SMBIOS_SYS_VERSION
#define LOONGEMU_SMBIOS_SYS_VERSION    "1.0"
#endif
#ifndef LOONGEMU_SMBIOS_SYS_SERIAL
#define LOONGEMU_SMBIOS_SYS_SERIAL     "LS6A202600018472"
#endif
#ifndef LOONGEMU_SMBIOS_BOARD_MANUF
#define LOONGEMU_SMBIOS_BOARD_MANUF    "Loongson Technology"
#endif
#ifndef LOONGEMU_SMBIOS_BOARD_PRODUCT
#define LOONGEMU_SMBIOS_BOARD_PRODUCT  "XA61200-ATX"
#endif
#ifndef LOONGEMU_SMBIOS_BOARD_SERIAL
#define LOONGEMU_SMBIOS_BOARD_SERIAL   "MB6A202600018472"
#endif
#ifndef LOONGEMU_SMBIOS_CPU_MANUF
#define LOONGEMU_SMBIOS_CPU_MANUF      "Intel(R) Corporation"
#endif
#ifndef LOONGEMU_SMBIOS_CPU_VERSION
#define LOONGEMU_SMBIOS_CPU_VERSION    "Intel(R) Core(TM) i7-6700 CPU @ 3.40GHz"
#endif

/* Windows-configurable storage/network identity (non-virtualized OUI). */
#ifndef LOONGEMU_DISK_MODEL
#define LOONGEMU_DISK_MODEL            "SAMSUNG MZVL21T0HCLR-00B00"
#endif
#ifndef LOONGEMU_DISK_SERIAL
#define LOONGEMU_DISK_SERIAL           "S676NX0R804219"
#endif
#ifndef LOONGEMU_DISK_WWN
#define LOONGEMU_DISK_WWN              UINT64_C(0x5002538f41804219)
#endif
/* Intel Corp. non-virtualized unicast OUI (00:1B:21) by default. */
#ifndef LOONGEMU_NIC_MAC_0
#define LOONGEMU_NIC_MAC_0 0x00u
#define LOONGEMU_NIC_MAC_1 0x1bu
#define LOONGEMU_NIC_MAC_2 0x21u
#define LOONGEMU_NIC_MAC_3 0x6au
#define LOONGEMU_NIC_MAC_4 0x42u
#define LOONGEMU_NIC_MAC_5 0x19u
#endif

#define LOONGEMU_SMBIOS_EP_GPA     0x000f0000ULL
#define LOONGEMU_SMBIOS_TABLE_GPA  0x000f0100ULL

typedef struct __attribute__((packed)) Smbios3EntryPoint {
    char     anchor[5];         /* "_SM3_" */
    uint8_t  checksum;
    uint8_t  length;            /* 24 */
    uint8_t  major_ver;         /* 3 */
    uint8_t  minor_ver;         /* 0 */
    uint8_t  docrev;
    uint8_t  entry_point_rev;   /* 1 */
    uint8_t  reserved;
    uint32_t max_struct_size;
    uint64_t table_address;
} Smbios3EntryPoint;

typedef struct LoongemuPlatformIdentity {
    const char *disk_model;
    const char *disk_serial;
    uint64_t    disk_wwn;
    uint8_t     nic_mac[6];
    int         override_pci_ids; /* 0 = untouched (Linux default), 1 = Windows override */
    uint16_t    pci_vendor_id;
    uint16_t    pci_device_id;
} LoongemuPlatformIdentity;

typedef struct LoongemuSmbiosInfo {
    uint64_t ep_gpa;
    uint64_t table_gpa;
    uint32_t table_length;
} LoongemuSmbiosInfo;

void loongemu_identity_default(LoongemuPlatformIdentity *out_id, int for_windows);

/*
 * Build hardcoded SMBIOS 3.0 tables (Types 0, 1, 2, 3, 4, 16, 17, 127)
 * into guest RAM at ep_gpa / table_gpa. Returns 1 on success, 0 on error.
 */
int loongemu_smbios_build(uint8_t *guest_ram,
                          uint64_t ep_gpa,
                          uint64_t table_gpa,
                          uint32_t nr_cpus,
                          uint64_t ram_bytes,
                          uint32_t cpu_mhz,
                          LoongemuSmbiosInfo *out_info);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_SMBIOS_TABLES_H */
