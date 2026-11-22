/*
 * RISC-V ACPI table builder: MADT (RINTC/IMSIC/APLIC) and RHCT.
 * Layouts mirror ACPI 6.6 and QEMU's hw/riscv/virt-acpi-build.c.
 */
#include <string.h>
#include "loongemu/riscv_acpi.h"

_Static_assert(sizeof(LoongemuAcpiSdtHeader) == 36, "SDT header is 36 bytes");
_Static_assert(sizeof(LoongemuAcpiRintc) == 36, "RINTC entry is 36 bytes");
_Static_assert(sizeof(LoongemuAcpiImsic) == 16, "IMSIC entry is 16 bytes");
_Static_assert(sizeof(LoongemuAcpiAplic) == 36, "APLIC entry is 36 bytes");
_Static_assert(sizeof(LoongemuAcpiPlic) == 36, "PLIC entry is 36 bytes");

#define RHCT_BODY_FIXED 20 /* reserved + time base + node count + node offset */

typedef struct AcpiWriter {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    int overflow;
} AcpiWriter;

static void put_u8(AcpiWriter *w, uint8_t value)
{
    if (w->len + 1 > w->cap) {
        w->overflow = 1;
        return;
    }
    w->buf[w->len++] = value;
}

static void put_u16(AcpiWriter *w, uint16_t value)
{
    put_u8(w, (uint8_t)value);
    put_u8(w, (uint8_t)(value >> 8));
}

static void put_u32(AcpiWriter *w, uint32_t value)
{
    put_u16(w, (uint16_t)value);
    put_u16(w, (uint16_t)(value >> 16));
}

static void put_u64(AcpiWriter *w, uint64_t value)
{
    put_u32(w, (uint32_t)value);
    put_u32(w, (uint32_t)(value >> 32));
}

static void put_bytes(AcpiWriter *w, const void *src, uint32_t size)
{
    if (w->len + size > w->cap) {
        w->overflow = 1;
        return;
    }
    memcpy(w->buf + w->len, src, size);
    w->len += size;
}

static void put_padded(AcpiWriter *w, const char *src, uint32_t width)
{
    char tmp[8];
    uint32_t len = 0;

    memset(tmp, ' ', sizeof(tmp));
    if (src != NULL) {
        while (len < width && src[len] != '\0') {
            tmp[len] = src[len];
            len++;
        }
    }
    put_bytes(w, tmp, width);
}

static void put_header(AcpiWriter *w, const char signature[4],
                       const LoongemuRiscvAcpi *cfg, uint8_t revision,
                       uint32_t total_length)
{
    char oem_id[6];
    char oem_table_id[8];

    memset(oem_id, ' ', sizeof(oem_id));
    memset(oem_table_id, ' ', sizeof(oem_table_id));
    if (cfg->oem_id != NULL) {
        for (uint32_t i = 0; i < 6 && cfg->oem_id[i] != '\0'; i++) {
            oem_id[i] = cfg->oem_id[i];
        }
    }
    if (cfg->oem_table_id != NULL) {
        for (uint32_t i = 0; i < 8 && cfg->oem_table_id[i] != '\0'; i++) {
            oem_table_id[i] = cfg->oem_table_id[i];
        }
    }

    put_bytes(w, signature, 4);
    put_u32(w, total_length);
    put_u8(w, revision);
    put_u8(w, 0); /* checksum filled in at the end */
    put_bytes(w, oem_id, 6);
    put_bytes(w, oem_table_id, 8);
    put_u32(w, 1); /* OEM revision */
    put_bytes(w, "LONG", 4); /* creator id */
    put_u32(w, 1); /* creator revision */
}

static void finish_table(AcpiWriter *w)
{
    uint8_t sum = 0;

    if (w->overflow || w->len != ((LoongemuAcpiSdtHeader *)w->buf)->length) {
        return;
    }
    for (uint32_t i = 0; i < w->len; i++) {
        sum = (uint8_t)(sum + w->buf[i]);
    }
    ((LoongemuAcpiSdtHeader *)w->buf)->checksum = (uint8_t)(0u - sum);
}

uint32_t loongemu_riscv_acpi_madt_size(const LoongemuRiscvAcpi *cfg)
{
    uint32_t size = sizeof(LoongemuAcpiSdtHeader) + 8; /* LAPIC+flags */

    if (cfg == NULL) {
        return 0;
    }
    size += cfg->hart_count * (uint32_t)sizeof(LoongemuAcpiRintc);
    if (cfg->has_imsic) {
        size += (uint32_t)sizeof(LoongemuAcpiImsic);
    }
    if (cfg->has_aplic) {
        size += (uint32_t)sizeof(LoongemuAcpiAplic);
    }
    if (cfg->has_plic) {
        size += (uint32_t)sizeof(LoongemuAcpiPlic);
    }
    return size;
}

uint32_t loongemu_riscv_acpi_madt_build(const LoongemuRiscvAcpi *cfg,
                                        uint8_t *buf, uint32_t cap)
{
    uint32_t total = loongemu_riscv_acpi_madt_size(cfg);
    AcpiWriter w = { buf, cap, 0, 0 };

    if (cfg == NULL || buf == NULL || cap < total) {
        return 0;
    }

    put_header(&w, "APIC", cfg, LOONGEMU_ACPI_MADT_REVISION, total);
    put_u32(&w, 0); /* local interrupt controller address (RISC-V: unused) */
    put_u32(&w, 0); /* MADT flags */

    for (uint32_t i = 0; i < cfg->hart_count; i++) {
        const LoongemuRiscvAcpiHart *hart = &cfg->harts[i];
        put_u8(&w, LOONGEMU_ACPI_MADT_RINTC);
        put_u8(&w, (uint8_t)sizeof(LoongemuAcpiRintc));
        put_u8(&w, 1); /* version */
        put_u8(&w, 0); /* reserved */
        put_u32(&w, hart->enabled ? 1u : 0u);
        put_u64(&w, hart->hart_id);
        put_u32(&w, hart->uid);
        put_u32(&w, hart->ext_intc_id);
        put_u64(&w, hart->imsic_addr);
        put_u32(&w, hart->imsic_size);
    }

    if (cfg->has_imsic) {
        put_u8(&w, LOONGEMU_ACPI_MADT_IMSIC);
        put_u8(&w, (uint8_t)sizeof(LoongemuAcpiImsic));
        put_u8(&w, 1); /* version */
        put_u8(&w, 0); /* reserved */
        put_u32(&w, 0); /* flags */
        put_u16(&w, cfg->imsic_num_ids);
        put_u16(&w, cfg->imsic_num_guest_ids);
        put_u8(&w, cfg->imsic_guest_index_bits);
        put_u8(&w, cfg->imsic_hart_index_bits);
        put_u8(&w, cfg->imsic_group_index_bits);
        put_u8(&w, cfg->imsic_group_index_shift);
    }

    if (cfg->has_aplic) {
        put_u8(&w, LOONGEMU_ACPI_MADT_APLIC);
        put_u8(&w, (uint8_t)sizeof(LoongemuAcpiAplic));
        put_u8(&w, 1); /* version */
        put_u8(&w, cfg->aplic_id);
        put_u32(&w, 0); /* flags */
        put_padded(&w, cfg->aplic_hw_id, 8);
        put_u16(&w, cfg->aplic_num_idcs);
        put_u16(&w, cfg->aplic_num_sources);
        put_u32(&w, cfg->aplic_gsi_base);
        put_u64(&w, cfg->aplic_addr);
        put_u32(&w, cfg->aplic_size);
    }

    if (cfg->has_plic) {
        put_u8(&w, LOONGEMU_ACPI_MADT_PLIC);
        put_u8(&w, (uint8_t)sizeof(LoongemuAcpiPlic));
        put_u8(&w, 1); /* version */
        put_u8(&w, cfg->plic_id);
        put_padded(&w, cfg->plic_hw_id, 8);
        put_u16(&w, cfg->plic_num_irqs);
        put_u16(&w, cfg->plic_max_prio);
        put_u32(&w, 0); /* flags */
        put_u32(&w, cfg->plic_size);
        put_u64(&w, cfg->plic_addr);
        put_u32(&w, cfg->plic_gsi_base);
    }

    finish_table(&w);
    return w.overflow ? 0 : w.len;
}

/* ISA node length: node header + IsaLength + string + NUL, padded to 2. */
static uint32_t rhct_isa_node_size(const char *isa)
{
    uint32_t len = 6 + 2 + (uint32_t)strlen(isa) + 1;

    return (len & 1u) ? len + 1 : len;
}

uint32_t loongemu_riscv_acpi_rhct_size(const LoongemuRiscvAcpi *cfg)
{
    uint32_t size;
    uint32_t hart_info;

    if (cfg == NULL) {
        return 0;
    }
    size = sizeof(LoongemuAcpiSdtHeader) + RHCT_BODY_FIXED;
    size += rhct_isa_node_size(cfg->isa != NULL ? cfg->isa : "");
    if (cfg->has_cmo) {
        size += 10;
    }
    if (cfg->has_mmu) {
        size += 8;
    }
    hart_info = 12; /* node header + NumOffsets + ACPI UID */
    if (cfg->has_cmo) {
        hart_info += 4;
    }
    if (cfg->has_mmu) {
        hart_info += 4;
    }
    size += cfg->hart_count * hart_info;
    return size;
}

uint32_t loongemu_riscv_acpi_rhct_build(const LoongemuRiscvAcpi *cfg,
                                        uint8_t *buf, uint32_t cap)
{
    uint32_t total = loongemu_riscv_acpi_rhct_size(cfg);
    uint32_t node_count;
    uint32_t node_array_offset = sizeof(LoongemuAcpiSdtHeader) +
                                 RHCT_BODY_FIXED;
    uint32_t cmo_offset = 0;
    uint32_t mmu_offset = 0;
    AcpiWriter w = { buf, cap, 0, 0 };

    if (cfg == NULL || buf == NULL || cap < total) {
        return 0;
    }

    node_count = 1 + cfg->hart_count; /* ISA string + per-hart info */
    if (cfg->has_cmo) {
        node_count++;
    }
    if (cfg->has_mmu) {
        node_count++;
    }

    put_header(&w, "RHCT", cfg, LOONGEMU_ACPI_RHCT_REVISION, total);
    put_u32(&w, 0); /* reserved */
    put_u64(&w, cfg->timer_hz);
    put_u32(&w, node_count);
    put_u32(&w, node_array_offset);

    /* ISA string node (Type 0). */
    {
        const char *isa = cfg->isa != NULL ? cfg->isa : "";
        uint32_t isa_len = (uint32_t)strlen(isa) + 1;
        uint32_t node_len = rhct_isa_node_size(isa);

        put_u16(&w, LOONGEMU_RHCT_ISA_NODE);
        put_u16(&w, (uint16_t)node_len);
        put_u16(&w, 1); /* revision */
        put_u16(&w, (uint16_t)isa_len);
        put_bytes(&w, isa, isa_len);
        if (node_len != 6 + 2 + isa_len) {
            put_u8(&w, 0); /* alignment padding */
        }
    }

    if (cfg->has_cmo) {
        cmo_offset = w.len;
        put_u16(&w, LOONGEMU_RHCT_CMO_NODE);
        put_u16(&w, 10);
        put_u16(&w, 1);
        put_u8(&w, 0); /* reserved */
        put_u8(&w, cfg->cbom_block_log2);
        put_u8(&w, 0); /* CBOP: no instruction cache in this model */
        put_u8(&w, cfg->cboz_block_log2);
    }

    if (cfg->has_mmu) {
        mmu_offset = w.len;
        put_u16(&w, LOONGEMU_RHCT_MMU_NODE);
        put_u16(&w, 8);
        put_u16(&w, 1);
        put_u8(&w, 0); /* reserved */
        put_u8(&w, cfg->mmu_type);
    }

    /* Hart info nodes (Type 0xFFFF) point back at CMO/MMU nodes. */
    for (uint32_t i = 0; i < cfg->hart_count; i++) {
        uint32_t offsets = 0;
        uint32_t length = 12;

        if (cfg->has_cmo) {
            offsets++;
            length += 4;
        }
        if (cfg->has_mmu) {
            offsets++;
            length += 4;
        }
        put_u16(&w, LOONGEMU_RHCT_HART_INFO_NODE);
        put_u16(&w, (uint16_t)length);
        put_u16(&w, 1); /* revision */
        put_u16(&w, (uint16_t)offsets);
        put_u32(&w, cfg->harts[i].uid);
        if (cfg->has_cmo) {
            put_u32(&w, cmo_offset);
        }
        if (cfg->has_mmu) {
            put_u32(&w, mmu_offset);
        }
    }

    finish_table(&w);
    return w.overflow ? 0 : w.len;
}

/* --- FADT (revision 6, hardware-reduced, no PM blocks) --- */

static void put_gas(AcpiWriter *w, uint8_t space, uint8_t bit_width,
                    uint8_t bit_offset, uint8_t access_size, uint64_t address)
{
    put_u8(w, space);
    put_u8(w, bit_width);
    put_u8(w, bit_offset);
    put_u8(w, access_size);
    put_u64(w, address);
}

static void put_gas_zero(AcpiWriter *w)
{
    put_gas(w, 0, 0, 0, 0, 0);
}

uint32_t loongemu_riscv_acpi_fadt_size(const LoongemuRiscvAcpi *cfg)
{
    return cfg == NULL ? 0 : LOONGEMU_ACPI_FADT_LENGTH;
}

uint32_t loongemu_riscv_acpi_fadt_build(const LoongemuRiscvAcpi *cfg,
                                        uint8_t *buf, uint32_t cap)
{
    AcpiWriter w = { buf, cap, 0, 0 };

    if (cfg == NULL || buf == NULL ||
        cap < LOONGEMU_ACPI_FADT_LENGTH) {
        return 0;
    }

    put_header(&w, "FACP", cfg, LOONGEMU_ACPI_FADT_REVISION,
               LOONGEMU_ACPI_FADT_LENGTH);
    put_u32(&w, 0); /* FIRMWARE_CTRL (no FACS) */
    put_u32(&w, (uint32_t)cfg->dsdt_gpa); /* DSDT */
    put_u8(&w, 0); /* INT_MODEL (reserved in 2.0+) */
    put_u8(&w, 0); /* Preferred_PM_Profile */
    put_u16(&w, cfg->sci_int);
    put_u32(&w, 0); /* SMI_CMD */
    put_u8(&w, 0); /* ACPI_ENABLE */
    put_u8(&w, 0); /* ACPI_DISABLE */
    put_u8(&w, 0); /* S4BIOS_REQ */
    put_u8(&w, 0); /* PSTATE_CNT */
    for (int i = 0; i < 8; i++) {
        put_u32(&w, 0); /* PM1a/PM1b_EVT, PM1a/PM1b_CNT, PM2, PM_TMR, GPE0, GPE1 */
    }
    for (int i = 0; i < 8; i++) {
        put_u8(&w, 0); /* block lengths, GPE1_BASE, CST_CNT */
    }
    put_u16(&w, 0); /* P_LVL2_LAT */
    put_u16(&w, 0); /* P_LVL3_LAT */
    put_u16(&w, 0); /* FLUSH_SIZE */
    put_u16(&w, 0); /* FLUSH_STRIDE */
    put_u8(&w, 0);  /* DUTY_OFFSET */
    put_u8(&w, 0);  /* DUTY_WIDTH */
    put_u8(&w, 0);  /* DAY_ALRM */
    put_u8(&w, 0);  /* MON_ALRM */
    put_u8(&w, 0);  /* CENTURY */
    put_u16(&w, (uint16_t)cfg->iapc_boot_arch);
    put_u8(&w, 0); /* reserved */
    put_u32(&w, LOONGEMU_ACPI_FADT_HW_REDUCED | cfg->fadt_flags_extra);
    put_gas_zero(&w);  /* RESET_REG */
    put_u8(&w, 0);     /* RESET_VALUE */
    put_u16(&w, 0);    /* ARM_BOOT_ARCH */
    put_u8(&w, cfg->fadt_minor_version);
    put_u64(&w, 0);    /* X_FIRMWARE_CTRL */
    put_u64(&w, cfg->dsdt_gpa); /* X_DSDT */
    put_gas_zero(&w);  /* X_PM1a_EVT */
    put_gas_zero(&w);  /* X_PM1b_EVT */
    put_gas_zero(&w);  /* X_PM1a_CNT */
    put_gas_zero(&w);  /* X_PM1b_CNT */
    put_gas_zero(&w);  /* X_PM2_CNT */
    put_gas_zero(&w);  /* X_PM_TMR */
    put_gas_zero(&w);  /* X_GPE0 */
    put_gas_zero(&w);  /* X_GPE1 */
    put_gas_zero(&w);  /* SLEEP_CONTROL_REG */
    put_gas_zero(&w);  /* SLEEP_STATUS_REG */
    put_bytes(&w, "LONGEMU", 8); /* Hypervisor Vendor Identity */

    finish_table(&w);
    return w.overflow || w.len != LOONGEMU_ACPI_FADT_LENGTH ? 0 : w.len;
}

/* --- SPCR (16550-compatible UART) --- */

uint32_t loongemu_riscv_acpi_spcr_size(const LoongemuRiscvAcpi *cfg)
{
    if (cfg == NULL || !cfg->has_spcr) {
        return 0;
    }
    return LOONGEMU_ACPI_SPCR_LENGTH;
}

uint32_t loongemu_riscv_acpi_spcr_build(const LoongemuRiscvAcpi *cfg,
                                        uint8_t *buf, uint32_t cap)
{
    const char *name = cfg != NULL && cfg->spcr_namespace != NULL
                           ? cfg->spcr_namespace : ".";
    AcpiWriter w = { buf, cap, 0, 0 };

    if (cfg == NULL || !cfg->has_spcr || buf == NULL ||
        cap < LOONGEMU_ACPI_SPCR_LENGTH) {
        return 0;
    }

    put_header(&w, "SPCR", cfg, LOONGEMU_ACPI_SPCR_REVISION,
               LOONGEMU_ACPI_SPCR_LENGTH);
    put_u8(&w, cfg->spcr_interface_type);
    put_u8(&w, 0);
    put_u8(&w, 0);
    put_u8(&w, 0); /* reserved */
    /* Base address GAS: system memory, 8-bit registers, byte access. */
    put_gas(&w, 0, 8, 0, 1, cfg->spcr_base);
    put_u8(&w, 0x10); /* interrupt type bit 4: RISC-V PLIC/APLIC */
    put_u8(&w, 0);    /* PC interrupt */
    put_u32(&w, cfg->spcr_gsi);
    put_u8(&w, cfg->spcr_baud_rate);
    put_u8(&w, 0); /* parity */
    put_u8(&w, 1); /* stop bits: 1 */
    put_u8(&w, 0); /* flow control */
    put_u8(&w, cfg->spcr_terminal_type);
    put_u8(&w, 0); /* language */
    put_u16(&w, 0xffff); /* PCI device id: not a PCI device */
    put_u16(&w, 0xffff); /* PCI vendor id */
    put_u8(&w, 0); /* PCI bus */
    put_u8(&w, 0); /* PCI device */
    put_u8(&w, 0); /* PCI function */
    put_u32(&w, 0); /* PCI flags */
    put_u8(&w, 0); /* PCI segment */
    put_u32(&w, 0); /* UART clock frequency: firmware does not know it */
    put_u32(&w, 0); /* precise baud rate */
    put_u16(&w, (uint16_t)strlen(name)); /* namespace string length */
    put_u16(&w, 88); /* namespace string offset (fixed by the layout) */
    put_bytes(&w, name, (uint32_t)strlen(name));

    finish_table(&w);
    return w.overflow || w.len != LOONGEMU_ACPI_SPCR_LENGTH ? 0 : w.len;
}

/* --- MCFG (PCIe ECAM) --- */

uint32_t loongemu_riscv_acpi_mcfg_size(const LoongemuRiscvAcpi *cfg)
{
    if (cfg == NULL || !cfg->has_mcfg) {
        return 0;
    }
    return sizeof(LoongemuAcpiSdtHeader) + 8 + 16;
}

uint32_t loongemu_riscv_acpi_mcfg_build(const LoongemuRiscvAcpi *cfg,
                                        uint8_t *buf, uint32_t cap)
{
    uint32_t total = loongemu_riscv_acpi_mcfg_size(cfg);
    AcpiWriter w = { buf, cap, 0, 0 };

    if (cfg == NULL || buf == NULL || total == 0 || cap < total) {
        return 0;
    }

    put_header(&w, "MCFG", cfg, LOONGEMU_ACPI_MCFG_REVISION, total);
    put_u64(&w, 0); /* reserved */
    put_u64(&w, cfg->mcfg_ecam_base);
    put_u16(&w, cfg->mcfg_segment);
    put_u8(&w, cfg->mcfg_bus_start);
    put_u8(&w, cfg->mcfg_bus_end);
    put_u32(&w, 0); /* reserved */

    finish_table(&w);
    return w.overflow ? 0 : w.len;
}

/* --- DSDT: minimal AML (Scope/Device/Name/String/Integer/Buffer) --- */

/*
 * PkgLength is always emitted in its 4-byte form (leading 0b11), so its width
 * does not depend on the payload and nested objects stay simple. The decoder
 * accepts 1-4 bytes; emitting the widest form is valid AML.
 */

typedef struct AmlFrame {
    uint32_t pkglen_pos;
} AmlFrame;

typedef struct AmlBuilder {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    int overflow;
    AmlFrame frames[16];
    int depth;
} AmlBuilder;

static void aml_byte(AmlBuilder *a, uint8_t value)
{
    if (a->len >= a->cap) {
        a->overflow = 1;
        a->len++;
        return;
    }
    a->buf[a->len++] = value;
}

static void aml_u16(AmlBuilder *a, uint16_t value)
{
    aml_byte(a, (uint8_t)value);
    aml_byte(a, (uint8_t)(value >> 8));
}

static void aml_u32(AmlBuilder *a, uint32_t value)
{
    aml_u16(a, (uint16_t)value);
    aml_u16(a, (uint16_t)(value >> 16));
}

static void aml_u64(AmlBuilder *a, uint64_t value)
{
    aml_u32(a, (uint32_t)value);
    aml_u32(a, (uint32_t)(value >> 32));
}

static void aml_bytes(AmlBuilder *a, const void *src, uint32_t size)
{
    const uint8_t *p = src;
    for (uint32_t i = 0; i < size; i++) {
        aml_byte(a, p[i]);
    }
}

static void aml_name_string(AmlBuilder *a, const char name[4],
                            int root_prefix)
{
    if (root_prefix) {
        aml_byte(a, 0x5C); /* RootChar */
    }
    for (int i = 0; i < 4; i++) {
        aml_byte(a, (uint8_t)name[i]);
    }
}

static void aml_open(AmlBuilder *a, uint8_t op, const char name[4],
                     int root_prefix)
{
    aml_byte(a, op);
    if (op == 0x5B) {
        aml_byte(a, 0x82); /* DeviceOp */
    }
    if (a->depth >= (int)(sizeof(a->frames) / sizeof(a->frames[0]))) {
        a->overflow = 1;
        return;
    }
    a->frames[a->depth++].pkglen_pos = a->len;
    aml_u32(a, 0); /* PkgLength placeholder (4-byte form) */
    aml_name_string(a, name, root_prefix);
}

static void aml_close(AmlBuilder *a)
{
    if (a->depth <= 0) {
        a->overflow = 1;
        return;
    }
    uint32_t pos = a->frames[--a->depth].pkglen_pos;
    uint32_t length = a->len - pos; /* includes the 4 PkgLength bytes */

    if (length > 0x0FFFFFFFu) {
        a->overflow = 1;
        return;
    }
    if (pos + 4 > a->cap) {
        return; /* the dry-run counter has no backing store */
    }
    a->buf[pos + 0] = (uint8_t)(0xC0 | ((length >> 24) & 0x0F));
    a->buf[pos + 1] = (uint8_t)(length >> 16);
    a->buf[pos + 2] = (uint8_t)(length >> 8);
    a->buf[pos + 3] = (uint8_t)length;
}

static void aml_integer(AmlBuilder *a, uint64_t value)
{
    if (value == 0) {
        aml_byte(a, 0x00); /* ZeroOp */
    } else if (value == 1) {
        aml_byte(a, 0x01); /* OneOp */
    } else if (value <= 0xFF) {
        aml_byte(a, 0x0A);
        aml_byte(a, (uint8_t)value);
    } else if (value <= 0xFFFF) {
        aml_byte(a, 0x0B);
        aml_u16(a, (uint16_t)value);
    } else if (value <= 0xFFFFFFFFu) {
        aml_byte(a, 0x0C);
        aml_u32(a, (uint32_t)value);
    } else {
        aml_byte(a, 0x0E);
        aml_u64(a, value);
    }
}

static void aml_string(AmlBuilder *a, const char *text)
{
    aml_byte(a, 0x0D); /* StringPrefix */
    while (*text != '\0') {
        aml_byte(a, (uint8_t)*text++);
    }
    aml_byte(a, 0); /* NUL */
}

static void aml_name_integer(AmlBuilder *a, const char name[4],
                             uint64_t value)
{
    aml_byte(a, 0x08); /* NameOp */
    aml_name_string(a, name, 0);
    aml_integer(a, value);
}

static void aml_name_string_value(AmlBuilder *a, const char name[4],
                                  const char *text)
{
    aml_byte(a, 0x08);
    aml_name_string(a, name, 0);
    aml_string(a, text);
}

static void aml_name_buffer(AmlBuilder *a, const char name[4],
                            const uint8_t *data, uint32_t size)
{
    aml_byte(a, 0x08); /* NameOp */
    aml_name_string(a, name, 0);
    aml_byte(a, 0x11); /* BufferOp */
    uint32_t pkglen_pos = a->len;
    aml_u32(a, 0);
    aml_integer(a, size);
    aml_bytes(a, data, size);
    uint32_t length = a->len - pkglen_pos;
    if (pkglen_pos + 4 <= a->cap) {
        a->buf[pkglen_pos + 0] = (uint8_t)(0xC0 | ((length >> 24) & 0x0F));
        a->buf[pkglen_pos + 1] = (uint8_t)(length >> 16);
        a->buf[pkglen_pos + 2] = (uint8_t)(length >> 8);
        a->buf[pkglen_pos + 3] = (uint8_t)length;
    }
}

/* QWord Address Space descriptor + EndTag: [0x8A len=43, ...], 0x79 0x00. */
static void aml_build_qword_window(uint8_t *out, uint64_t base,
                                   uint64_t size, int read_only)
{
    uint32_t pos = 0;
    out[pos++] = 0x8A; /* QWord Address Space */
    out[pos++] = 43;   /* descriptor length, low byte */
    out[pos++] = 0;    /* length, high byte */
    out[pos++] = 0;    /* resource type: memory */
    out[pos++] = 0x14; /* producer, positive decode, min/max fixed */
    out[pos++] = (uint8_t)(read_only ? 0x03 : 0x02); /* non-cacheable */
    for (int i = 0; i < 8; i++) {
        out[pos++] = (uint8_t)(0); /* granularity */
    }
    for (int i = 0; i < 8; i++) {
        out[pos++] = (uint8_t)(base >> (8 * i));
    }
    for (int i = 0; i < 8; i++) {
        out[pos++] = (uint8_t)(base >> (8 * i)); /* max = min */
    }
    for (int i = 0; i < 8; i++) {
        out[pos++] = 0; /* translation */
    }
    for (int i = 0; i < 8; i++) {
        out[pos++] = (uint8_t)(size >> (8 * i));
    }
    out[pos++] = 0x79; /* EndTag */
    out[pos++] = 0x00;
}

static void aml_name_window(AmlBuilder *a, const char name[4],
                            uint64_t base, uint64_t size, int read_only)
{
    uint8_t window[48];
    aml_build_qword_window(window, base, size, read_only);
    aml_name_buffer(a, name, window, sizeof(window));
}

/* RINTC entry bytes for a hart, matching the MADT layout. */
static void aml_build_rintc(uint8_t out[36], const LoongemuRiscvAcpi *cfg,
                            uint32_t index)
{
    const LoongemuRiscvAcpiHart *hart = &cfg->harts[index];
    memset(out, 0, 36);
    out[0] = LOONGEMU_ACPI_MADT_RINTC;
    out[1] = 36;
    out[2] = 1;
    out[5] = (uint8_t)(hart->enabled ? 1 : 0); /* flags */
    for (int i = 0; i < 8; i++) {
        out[8 + i] = (uint8_t)(hart->hart_id >> (8 * i));
    }
    for (int i = 0; i < 4; i++) {
        out[16 + i] = (uint8_t)(hart->uid >> (8 * i));
        out[20 + i] = (uint8_t)(hart->ext_intc_id >> (8 * i));
    }
    for (int i = 0; i < 8; i++) {
        out[24 + i] = (uint8_t)(hart->imsic_addr >> (8 * i));
    }
    for (int i = 0; i < 4; i++) {
        out[32 + i] = (uint8_t)(hart->imsic_size >> (8 * i));
    }
}

static void aml_device_cpu(AmlBuilder *a, const LoongemuRiscvAcpi *cfg,
                           uint32_t index)
{
    char name[4] = { 'C', '0', '0', '0' };
    char digits[3];
    uint32_t uid = cfg->harts[index].uid;

    digits[0] = (char)('0' + ((uid >> 8) & 0xF));
    digits[1] = (char)('0' + ((uid >> 4) & 0xF));
    digits[2] = (char)('0' + (uid & 0xF));
    name[1] = digits[0];
    name[2] = digits[1];
    name[3] = digits[2];

    aml_open(a, 0x5B, name, 0);
    aml_name_string_value(a, "_HID", "ACPI0007");
    aml_name_integer(a, "_UID", uid);
    {
        uint8_t rintc[36];
        aml_build_rintc(rintc, cfg, index);
        aml_name_buffer(a, "_MAT", rintc, sizeof(rintc));
    }
    aml_close(a);
}

static void aml_device_pci(AmlBuilder *a, const LoongemuRiscvAcpi *cfg)
{
    aml_open(a, 0x5B, "PCI0", 0);
    aml_name_string_value(a, "_HID", cfg->pci_hid != NULL ? cfg->pci_hid
                                                          : "PNP0A08");
    aml_name_integer(a, "_UID", 0);
    aml_name_integer(a, "_SEG", cfg->pci_segment);
    aml_name_integer(a, "_BBN", cfg->pci_bus_start);
    /* ECAM window plus the PCI MMIO aperture. */
    {
        uint8_t windows[96];
        aml_build_qword_window(windows, cfg->pci_ecam_base, cfg->pci_ecam_size,
                               0);
        aml_build_qword_window(windows + 48, cfg->pci_mmio_base,
                               cfg->pci_mmio_size, 0);
        aml_name_buffer(a, "_CRS", windows, sizeof(windows));
    }
    aml_close(a);
}

static void aml_device_uart(AmlBuilder *a, const LoongemuRiscvAcpi *cfg)
{
    aml_open(a, 0x5B, "UART", 0);
    aml_name_string_value(a, "_HID", cfg->uart_hid != NULL ? cfg->uart_hid
                                                           : "PNP0501");
    aml_name_integer(a, "_UID", 1);
    aml_name_window(a, "_CRS", cfg->uart_base, cfg->uart_size, 0);
    aml_close(a);
}

static void aml_device_virtio(AmlBuilder *a, uint32_t index,
                              const LoongemuRiscvAcpiVirtio *dev)
{
    char name[4] = { 'V', '0', '0', '0' };
    name[1] = (char)('0' + ((index / 100) % 10));
    name[2] = (char)('0' + ((index / 10) % 10));
    name[3] = (char)('0' + (index % 10));

    aml_open(a, 0x5B, name, 0);
    aml_name_string_value(a, "_HID", "LNRO0005");
    aml_name_integer(a, "_UID", index);
    aml_name_window(a, "_CRS", dev->base, dev->size, 0);
    aml_close(a);
}

static void aml_build_body(AmlBuilder *a, const LoongemuRiscvAcpi *cfg)
{
    aml_open(a, 0x10, "_SB_", 1); /* Scope(\_SB) */
    for (uint32_t i = 0; i < cfg->hart_count; i++) {
        aml_device_cpu(a, cfg, i);
    }
    aml_device_pci(a, cfg);
    aml_device_uart(a, cfg);
    for (uint32_t i = 0; i < cfg->virtio_count; i++) {
        aml_device_virtio(a, i, &cfg->virtio[i]);
    }
    aml_close(a);
}

static uint32_t dsdt_dry_run(const LoongemuRiscvAcpi *cfg)
{
    AmlBuilder probe = { NULL, 0, 0, 0, { { 0 } }, 0 };
    aml_build_body(&probe, cfg);
    return probe.len;
}

uint32_t loongemu_riscv_acpi_dsdt_size(const LoongemuRiscvAcpi *cfg)
{
    if (cfg == NULL || !cfg->has_dsdt) {
        return 0;
    }
    return (uint32_t)sizeof(LoongemuAcpiSdtHeader) + dsdt_dry_run(cfg);
}

uint32_t loongemu_riscv_acpi_dsdt_build(const LoongemuRiscvAcpi *cfg,
                                        uint8_t *buf, uint32_t cap)
{
    uint32_t aml_size;
    uint32_t total;
    AcpiWriter w = { buf, cap, 0, 0 };

    if (cfg == NULL || !cfg->has_dsdt || buf == NULL) {
        return 0;
    }
    aml_size = dsdt_dry_run(cfg);
    if (aml_size > LOONGEMU_ACPI_DSDT_SCRATCH) {
        return 0;
    }
    total = (uint32_t)sizeof(LoongemuAcpiSdtHeader) + aml_size;
    if (cap < total) {
        return 0;
    }

    static uint8_t scratch[LOONGEMU_ACPI_DSDT_SCRATCH];
    AmlBuilder body = { scratch, LOONGEMU_ACPI_DSDT_SCRATCH, 0, 0,
                        { { 0 } }, 0 };
    aml_build_body(&body, cfg);
    if (body.overflow || body.len != aml_size || body.depth != 0) {
        return 0;
    }

    put_header(&w, "DSDT", cfg, LOONGEMU_ACPI_DSDT_REVISION, total);
    put_bytes(&w, scratch, aml_size);
    finish_table(&w);
    return w.overflow ? 0 : w.len;
}

/* --- RSDP / XSDT for firmware-mediated ACPI handoff --- */

void loongemu_riscv_acpi_rsdp_build(uint8_t out[36], uint32_t rsdt_gpa,
                                    uint64_t xsdt_gpa, const char *oem_id)
{
    memset(out, 0, 36);
    memcpy(out, "RSD PTR ", 8);
    /* checksum filled below */
    for (int i = 0; i < 6; i++) {
        out[9 + i] = ' ';
    }
    if (oem_id != NULL) {
        for (int i = 0; i < 6 && oem_id[i] != '\0'; i++) {
            out[9 + i] = (uint8_t)oem_id[i];
        }
    }
    out[15] = 2; /* ACPI 2.0+ */
    for (int i = 0; i < 4; i++) {
        out[16 + i] = (uint8_t)(rsdt_gpa >> (8 * i));
    }
    out[20] = 36; /* length, low byte */
    for (int i = 0; i < 8; i++) {
        out[24 + i] = (uint8_t)(xsdt_gpa >> (8 * i));
    }
    uint8_t sum = 0;
    for (int i = 0; i < 20; i++) {
        sum = (uint8_t)(sum + out[i]);
    }
    out[8] = (uint8_t)(0u - sum);
    sum = 0;
    for (int i = 0; i < 36; i++) {
        sum = (uint8_t)(sum + out[i]);
    }
    out[32] = (uint8_t)(0u - sum);
}

uint32_t loongemu_riscv_acpi_xsdt_size(uint32_t count)
{
    return (uint32_t)sizeof(LoongemuAcpiSdtHeader) + count * 8u;
}

uint32_t loongemu_riscv_acpi_xsdt_build(uint8_t *buf, uint32_t cap,
                                        const uint64_t *table_gpas,
                                        uint32_t count,
                                        const LoongemuRiscvAcpi *cfg)
{
    uint32_t total = loongemu_riscv_acpi_xsdt_size(count);
    AcpiWriter w = { buf, cap, 0, 0 };

    if (buf == NULL || cap < total || (count != 0 && table_gpas == NULL)) {
        return 0;
    }
    put_header(&w, "XSDT", cfg, 1, total);
    for (uint32_t i = 0; i < count; i++) {
        put_u64(&w, table_gpas[i]);
    }
    finish_table(&w);
    return w.overflow ? 0 : w.len;
}
