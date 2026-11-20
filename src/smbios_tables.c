/*
 * Freestanding SMBIOS 3.0 / DMI table generator for LoongEMU.
 *
 * All strings are compile-time configurable via LOONGEMU_SMBIOS_* macros so
 * the firmware presents realistic platform DMI records to Linux, BSD, and
 * Windows guests without runtime heap allocation.
 */
#include "loongemu/smbios_tables.h"

static void sm_zero(void *dst, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) {
        p[i] = 0;
    }
}

static size_t sm_strlen(const char *s)
{
    size_t n = 0;
    while (s && s[n] != '\0') {
        n++;
    }
    return n;
}

static uint8_t sm_checksum(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return (uint8_t)(0u - sum);
}

typedef struct SmBuf {
    uint8_t *base;
    size_t   pos;
    size_t   cap;
} SmBuf;

static int sm_put_bytes(SmBuf *b, const void *src, size_t len)
{
    const uint8_t *s = (const uint8_t *)src;
    if (b->pos + len > b->cap) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        b->base[b->pos++] = s[i];
    }
    return 1;
}

static int sm_put_u8(SmBuf *b, uint8_t v)
{
    return sm_put_bytes(b, &v, 1);
}

static int sm_put_u16(SmBuf *b, uint16_t v)
{
    uint8_t tmp[2] = { (uint8_t)(v & 0xffu), (uint8_t)(v >> 8) };
    return sm_put_bytes(b, tmp, 2);
}

static int sm_put_u32(SmBuf *b, uint32_t v)
{
    uint8_t tmp[4] = {
        (uint8_t)(v & 0xffu),
        (uint8_t)((v >> 8) & 0xffu),
        (uint8_t)((v >> 16) & 0xffu),
        (uint8_t)((v >> 24) & 0xffu),
    };
    return sm_put_bytes(b, tmp, 4);
}

static int sm_put_str(SmBuf *b, const char *s)
{
    size_t len = sm_strlen(s);
    if (len == 0) {
        return sm_put_u8(b, ' ');
    }
    return sm_put_bytes(b, s, len) && sm_put_u8(b, 0);
}

static int sm_end_strings(SmBuf *b, int had_strings)
{
    if (!had_strings) {
        return sm_put_u8(b, 0) && sm_put_u8(b, 0);
    }
    return sm_put_u8(b, 0);
}

void loongemu_identity_default(LoongemuPlatformIdentity *out_id, int for_windows)
{
    if (out_id == 0) {
        return;
    }
    out_id->disk_model = LOONGEMU_DISK_MODEL;
    out_id->disk_serial = LOONGEMU_DISK_SERIAL;
    out_id->disk_wwn = LOONGEMU_DISK_WWN;
    out_id->nic_mac[0] = LOONGEMU_NIC_MAC_0;
    out_id->nic_mac[1] = LOONGEMU_NIC_MAC_1;
    out_id->nic_mac[2] = LOONGEMU_NIC_MAC_2;
    out_id->nic_mac[3] = LOONGEMU_NIC_MAC_3;
    out_id->nic_mac[4] = LOONGEMU_NIC_MAC_4;
    out_id->nic_mac[5] = LOONGEMU_NIC_MAC_5;
    /* Keep PCI IDs untouched on Linux; enable override capability on Windows. */
    out_id->override_pci_ids = for_windows ? 1 : 0;
    out_id->pci_vendor_id = for_windows ? 0x8086u : 0x0000u;
    out_id->pci_device_id = for_windows ? 0x29c0u : 0x0000u;
}

int loongemu_smbios_build(uint8_t *guest_ram,
                          uint64_t ep_gpa,
                          uint64_t table_gpa,
                          uint32_t nr_cpus,
                          uint64_t ram_bytes,
                          uint32_t cpu_mhz,
                          LoongemuSmbiosInfo *out_info)
{
    SmBuf b;
    uint16_t ram_mb;

    if (nr_cpus == 0 || ram_bytes == 0) {
        return 0;
    }
    if (cpu_mhz == 0) {
        cpu_mhz = 2500u;
    }
    ram_mb = (uint16_t)((ram_bytes >> 20) > 0x7fffu ? 0x7fffu : (ram_bytes >> 20));
    if (ram_mb == 0) {
        ram_mb = 128u;
    }

    b.base = guest_ram + table_gpa;
    b.pos = 0;
    b.cap = 3584u;
    sm_zero(b.base, b.cap);

    /* Type 0: BIOS Information (24 bytes) */
    {
        uint8_t t0[24];
        sm_zero(t0, sizeof(t0));
        t0[0] = 0; t0[1] = 24; t0[2] = 0x00; t0[3] = 0x00; /* handle 0x0000 */
        t0[4] = 1; /* Vendor */
        t0[5] = 2; /* Version */
        t0[6] = 0x00; t0[7] = 0xe8; /* Starting Address Segment */
        t0[8] = 3; /* Release Date */
        t0[9] = 0x3f; /* ROM size = 4 MiB */
        t0[10] = 0x80; t0[11] = 0x08; /* PCI + BIOS shadowing */
        t0[18] = 0x03; /* ACPI + USB legacy */
        t0[19] = 0x0c; /* UEFI + Target Content Distribution */
        t0[20] = 2; t0[21] = 4; /* BIOS major/minor */
        if (!sm_put_bytes(&b, t0, sizeof(t0)) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_BIOS_VENDOR) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_BIOS_VERSION) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_BIOS_DATE) ||
            !sm_end_strings(&b, 1)) {
            return 0;
        }
    }

    /* Type 1: System Information (27 bytes) */
    {
        uint8_t t1[27];
        sm_zero(t1, sizeof(t1));
        t1[0] = 1; t1[1] = 27; t1[2] = 0x01; t1[3] = 0x00; /* handle 0x0001 */
        t1[4] = 1; /* Manufacturer */
        t1[5] = 2; /* Product Name */
        t1[6] = 3; /* Version */
        t1[7] = 4; /* Serial Number */
        /* Deterministic fixed UUID */
        t1[8]  = 0x4cu; t1[9]  = 0x53u; t1[10] = 0x36u; t1[11] = 0x41u;
        t1[12] = 0x20u; t1[13] = 0x26u; t1[14] = 0x40u; t1[15] = 0x11u;
        t1[16] = 0x80u; t1[17] = 0x00u; t1[18] = 0x00u; t1[19] = 0x1bu;
        t1[20] = 0x21u; t1[21] = 0x6au; t1[22] = 0x42u; t1[23] = 0x19u;
        t1[24] = 0x06; /* Wake-up Type: Power Switch */
        t1[25] = 0; t1[26] = 0;
        if (!sm_put_bytes(&b, t1, sizeof(t1)) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_SYS_MANUF) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_SYS_PRODUCT) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_SYS_VERSION) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_SYS_SERIAL) ||
            !sm_end_strings(&b, 1)) {
            return 0;
        }
    }

    /* Type 2: Baseboard Information (15 bytes) */
    {
        uint8_t t2[15];
        sm_zero(t2, sizeof(t2));
        t2[0] = 2; t2[1] = 15; t2[2] = 0x02; t2[3] = 0x00;
        t2[4] = 1; /* Manufacturer */
        t2[5] = 2; /* Product */
        t2[6] = 3; /* Version */
        t2[7] = 4; /* Serial */
        t2[9] = 0x09; /* Hosting board + Replaceable */
        t2[11] = 0x03; t2[12] = 0x00; /* Chassis handle */
        t2[13] = 0x0a; /* Motherboard */
        if (!sm_put_bytes(&b, t2, sizeof(t2)) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_BOARD_MANUF) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_BOARD_PRODUCT) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_SYS_VERSION) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_BOARD_SERIAL) ||
            !sm_end_strings(&b, 1)) {
            return 0;
        }
    }

    /* Type 3: System Enclosure (21 bytes) */
    {
        uint8_t t3[21];
        sm_zero(t3, sizeof(t3));
        t3[0] = 3; t3[1] = 21; t3[2] = 0x03; t3[3] = 0x00;
        t3[4] = 1; /* Manufacturer */
        t3[5] = 0x03; /* Desktop */
        t3[6] = 2; /* Version */
        t3[7] = 3; /* Serial */
        t3[9] = 0x03; t3[10] = 0x03; t3[11] = 0x03; t3[12] = 0x03;
        if (!sm_put_bytes(&b, t3, sizeof(t3)) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_SYS_MANUF) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_SYS_VERSION) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_SYS_SERIAL) ||
            !sm_end_strings(&b, 1)) {
            return 0;
        }
    }

    /* Type 4: Processor Information (42 bytes) */
    {
        uint8_t t4[42];
        sm_zero(t4, sizeof(t4));
        t4[0] = 4; t4[1] = 42; t4[2] = 0x04; t4[3] = 0x00;
        t4[4] = 1; /* Socket Designation */
        t4[5] = 0x03; /* Central Processor */
        t4[6] = 0xc6; /* Intel Core i7 */
        t4[7] = 2; /* Manufacturer */
        t4[16] = 3; /* Version */
        t4[17] = 0x8c; /* 1.2V */
        t4[18] = 100; t4[19] = 0; /* External clock 100 MHz */
        t4[20] = (uint8_t)(cpu_mhz & 0xffu);
        t4[21] = (uint8_t)(cpu_mhz >> 8);
        t4[22] = (uint8_t)(cpu_mhz & 0xffu);
        t4[23] = (uint8_t)(cpu_mhz >> 8);
        t4[24] = 0x41; /* Populated + Enabled */
        t4[25] = 0x01; /* Other */
        t4[26] = 0xff; t4[27] = 0xff; /* L1 handle */
        t4[28] = 0xff; t4[29] = 0xff; /* L2 handle */
        t4[30] = 0xff; t4[31] = 0xff; /* L3 handle */
        t4[35] = (uint8_t)nr_cpus; /* Core Count */
        t4[36] = (uint8_t)nr_cpus; /* Core Enabled */
        t4[37] = (uint8_t)nr_cpus; /* Thread Count */
        t4[38] = 0x04; /* 64-bit capable */
        if (!sm_put_bytes(&b, t4, sizeof(t4)) ||
            !sm_put_str(&b, "LGA1151") ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_CPU_MANUF) ||
            !sm_put_str(&b, LOONGEMU_SMBIOS_CPU_VERSION) ||
            !sm_end_strings(&b, 1)) {
            return 0;
        }
    }

    /* Type 16: Physical Memory Array (23 bytes) */
    {
        uint8_t t16[23];
        uint32_t max_kb = (uint32_t)((ram_bytes >> 10) > 0x7fffffffULL
                                         ? 0x7fffffffULL
                                         : (ram_bytes >> 10));
        sm_zero(t16, sizeof(t16));
        t16[0] = 16; t16[1] = 23; t16[2] = 0x10; t16[3] = 0x00;
        t16[4] = 0x03; /* System board */
        t16[5] = 0x03; /* System memory */
        t16[6] = 0x03; /* None ECC */
        t16[7] = (uint8_t)(max_kb & 0xffu);
        t16[8] = (uint8_t)((max_kb >> 8) & 0xffu);
        t16[9] = (uint8_t)((max_kb >> 16) & 0xffu);
        t16[10] = (uint8_t)((max_kb >> 24) & 0xffu);
        t16[11] = 0xfe; t16[12] = 0xff;
        t16[13] = 1; t16[14] = 0; /* 1 device */
        if (!sm_put_bytes(&b, t16, sizeof(t16)) || !sm_end_strings(&b, 0)) {
            return 0;
        }
    }

    /* Type 17: Memory Device (40 bytes) */
    {
        uint8_t t17[40];
        sm_zero(t17, sizeof(t17));
        t17[0] = 17; t17[1] = 40; t17[2] = 0x11; t17[3] = 0x00;
        t17[4] = 0x10; t17[5] = 0x00; /* Array handle */
        t17[6] = 0xfe; t17[7] = 0xff;
        t17[8] = 64; t17[9] = 0; /* Total width */
        t17[10] = 64; t17[11] = 0; /* Data width */
        t17[12] = (uint8_t)(ram_mb & 0xffu);
        t17[13] = (uint8_t)(ram_mb >> 8);
        t17[14] = 0x09; /* DIMM */
        t17[16] = 1; /* Device Locator */
        t17[18] = 0x1a; /* DDR4 */
        t17[19] = 0x80; t17[20] = 0x00; /* Synchronous */
        t17[21] = (uint8_t)(3200u & 0xffu);
        t17[22] = (uint8_t)(3200u >> 8);
        t17[23] = 2; /* Manufacturer */
        if (!sm_put_bytes(&b, t17, sizeof(t17)) ||
            !sm_put_str(&b, "DIMM_A1") ||
            !sm_put_str(&b, "Micron") ||
            !sm_end_strings(&b, 1)) {
            return 0;
        }
    }

    /* Type 127: End-of-Table */
    if (!sm_put_u8(&b, 127) || !sm_put_u8(&b, 4) ||
        !sm_put_u16(&b, 0x007fu) || !sm_end_strings(&b, 0)) {
        return 0;
    }

    /* SMBIOS 3.0 Entry Point at ep_gpa */
    {
        Smbios3EntryPoint *ep = (Smbios3EntryPoint *)(guest_ram + ep_gpa);
        sm_zero(ep, sizeof(*ep));
        ep->anchor[0] = '_'; ep->anchor[1] = 'S'; ep->anchor[2] = 'M';
        ep->anchor[3] = '3'; ep->anchor[4] = '_';
        ep->length = (uint8_t)sizeof(*ep);
        ep->major_ver = 3;
        ep->minor_ver = 0;
        ep->docrev = 0;
        ep->entry_point_rev = 1;
        ep->max_struct_size = (uint32_t)b.pos;
        ep->table_address = table_gpa;
        ep->checksum = sm_checksum(ep, sizeof(*ep));
    }

    if (out_info != 0) {
        out_info->ep_gpa = ep_gpa;
        out_info->table_gpa = table_gpa;
        out_info->table_length = (uint32_t)b.pos;
    }
    return 1;
}
