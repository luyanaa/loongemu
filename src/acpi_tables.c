/*
 * Minimal x86-64 ACPI 2.0 table generator (RSDP, RSDT, XSDT, FADT, DSDT, MADT)
 * and host UEFI ACPI RSDP validation helper.
 *
 * Freestanding: no libc dependencies so it links cleanly into both the bare
 * harness (tb_sys.elf) and the UEFI VMM application (vmm_entry.efi).
 */
#include "loongemu/acpi_tables.h"

static void mem_zero(void *dst, uint64_t n)
{
    uint8_t *p = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) {
        p[i] = 0;
    }
}

static void mem_copy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static uint8_t acpi_checksum(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return (uint8_t)(0u - sum);
}

static void fill_sdt_header(AcpiSdtHeader *h, const char sig[4],
                            uint32_t length, uint8_t revision,
                            const char table_id[8])
{
    mem_copy(h->signature, sig, 4);
    h->length = length;
    h->revision = revision;
    h->checksum = 0;
    mem_copy(h->oem_id, "LNGEMU", 6);
    mem_copy(h->oem_table_id, table_id, 8);
    h->oem_revision = 1;
    h->creator_id = 0x554d454cu; /* "LEMU" */
    h->creator_revision = 1;
}

static uint64_t align16(uint64_t v)
{
    return (v + 15ULL) & ~15ULL;
}

int loongemu_acpi_build(uint8_t *guest_ram,
                        uint64_t rsdp_gpa,
                        uint64_t tables_gpa,
                        uint32_t nr_cpus,
                        LoongemuAcpiInfo *out_info)
{
    uint64_t cur = align16(tables_gpa);
    uint64_t dsdt_gpa, fadt_gpa, madt_gpa, rsdt_gpa, xsdt_gpa;

    if (nr_cpus == 0 || nr_cpus > 64) {
        return 0;
    }

    /* 1. DSDT (minimal valid empty AML definition block: header only). */
    dsdt_gpa = cur;
    {
        AcpiSdtHeader *dsdt = (AcpiSdtHeader *)(guest_ram + dsdt_gpa);
        mem_zero(dsdt, sizeof(*dsdt));
        fill_sdt_header(dsdt, "DSDT", (uint32_t)sizeof(*dsdt), 2, "LNGDSDT ");
        dsdt->checksum = acpi_checksum(dsdt, dsdt->length);
        cur = align16(cur + dsdt->length);
    }

    /* 2. FADT (FACP rev 4, 244 bytes) with 24-bit 3.579545 MHz PM Timer. */
    fadt_gpa = cur;
    {
        AcpiFadt *fadt = (AcpiFadt *)(guest_ram + fadt_gpa);
        mem_zero(fadt, sizeof(*fadt));
        fill_sdt_header(&fadt->hdr, "FACP", (uint32_t)sizeof(*fadt), 4,
                        "LNGFADT ");
        fadt->dsdt = (uint32_t)dsdt_gpa;
        fadt->x_dsdt = dsdt_gpa;
        fadt->preferred_pm_profile = 1; /* Desktop */
        fadt->sci_int = 9;
        fadt->pm1a_evt_blk = LOONGEMU_ACPI_PM1A_EVT_PORT;
        fadt->pm1a_cnt_blk = LOONGEMU_ACPI_PM1A_CNT_PORT;
        fadt->pm_tmr_blk = LOONGEMU_ACPI_PM_TMR_PORT;
        fadt->pm1_evt_len = 4;
        fadt->pm1_cnt_len = 2;
        fadt->pm_tmr_len = 4;
        fadt->iapc_boot_arch = 0x0001u; /* LEGACY_DEVICES */
        /* WBINVD (bit 0) | PWR_BUTTON (bit 4) | SLP_BUTTON (bit 5) */
        fadt->flags = (1u << 0) | (1u << 4) | (1u << 5);

        fadt->x_pm1a_evt_blk.address_space_id = 1; /* System I/O */
        fadt->x_pm1a_evt_blk.register_bit_width = 32;
        fadt->x_pm1a_evt_blk.access_size = 2;
        fadt->x_pm1a_evt_blk.address = LOONGEMU_ACPI_PM1A_EVT_PORT;

        fadt->x_pm1a_cnt_blk.address_space_id = 1;
        fadt->x_pm1a_cnt_blk.register_bit_width = 16;
        fadt->x_pm1a_cnt_blk.access_size = 2;
        fadt->x_pm1a_cnt_blk.address = LOONGEMU_ACPI_PM1A_CNT_PORT;

        fadt->x_pm_tmr_blk.address_space_id = 1;
        fadt->x_pm_tmr_blk.register_bit_width = 32;
        fadt->x_pm_tmr_blk.access_size = 3;
        fadt->x_pm_tmr_blk.address = LOONGEMU_ACPI_PM_TMR_PORT;

        fadt->hdr.checksum = acpi_checksum(fadt, fadt->hdr.length);
        cur = align16(cur + fadt->hdr.length);
    }

    /* 3. MADT (APIC): Local APICs for nr_cpus + 1 IO-APIC + IRQ0->GSI2 ISO. */
    madt_gpa = cur;
    {
        uint32_t madt_len = (uint32_t)(sizeof(AcpiMadtHeader) +
                                       nr_cpus * sizeof(AcpiMadtLapic) +
                                       sizeof(AcpiMadtIoapic) +
                                       sizeof(AcpiMadtIso));
        uint8_t *base = guest_ram + madt_gpa;
        AcpiMadtHeader *madt = (AcpiMadtHeader *)base;
        uint32_t off = (uint32_t)sizeof(AcpiMadtHeader);

        mem_zero(base, madt_len);
        fill_sdt_header(&madt->hdr, "APIC", madt_len, 3, "LNGMADT ");
        madt->lapic_addr = (uint32_t)LOONGEMU_LAPIC_GPA;
        madt->flags = 1u; /* PCAT_COMPAT: dual 8259 PICs present */

        for (uint32_t i = 0; i < nr_cpus; i++) {
            AcpiMadtLapic *la = (AcpiMadtLapic *)(base + off);
            la->type = 0;
            la->length = (uint8_t)sizeof(*la);
            la->acpi_processor_id = (uint8_t)i;
            la->apic_id = (uint8_t)i;
            la->flags = 1u; /* Processor Enabled */
            off += (uint32_t)sizeof(*la);
        }

        {
            AcpiMadtIoapic *ioa = (AcpiMadtIoapic *)(base + off);
            ioa->type = 1;
            ioa->length = (uint8_t)sizeof(*ioa);
            ioa->ioapic_id = (uint8_t)nr_cpus;
            ioa->reserved = 0;
            ioa->ioapic_addr = (uint32_t)LOONGEMU_IOAPIC_GPA;
            ioa->gsi_base = 0;
            off += (uint32_t)sizeof(*ioa);
        }

        {
            AcpiMadtIso *iso = (AcpiMadtIso *)(base + off);
            iso->type = 2;
            iso->length = (uint8_t)sizeof(*iso);
            iso->bus = 0;     /* ISA */
            iso->source = 0;  /* PIT IRQ 0 */
            iso->gsi = 2;     /* Routed to IO-APIC pin 2 */
            iso->flags = 0;   /* Conforms to bus specs */
            off += (uint32_t)sizeof(*iso);
        }

        madt->hdr.checksum = acpi_checksum(base, madt_len);
        cur = align16(cur + madt_len);
    }

    /* 4. RSDT (32-bit fallback pointers to FADT and MADT). */
    rsdt_gpa = cur;
    {
        uint32_t rsdt_len = (uint32_t)(sizeof(AcpiSdtHeader) + 2u * sizeof(uint32_t));
        uint8_t *base = guest_ram + rsdt_gpa;
        AcpiSdtHeader *rsdt = (AcpiSdtHeader *)base;
        uint32_t *ents = (uint32_t *)(base + sizeof(AcpiSdtHeader));

        mem_zero(base, rsdt_len);
        fill_sdt_header(rsdt, "RSDT", rsdt_len, 1, "LNGRSDT ");
        ents[0] = (uint32_t)fadt_gpa;
        ents[1] = (uint32_t)madt_gpa;
        rsdt->checksum = acpi_checksum(base, rsdt_len);
        cur = align16(cur + rsdt_len);
    }

    /* 5. XSDT (64-bit pointers to FADT and MADT). */
    xsdt_gpa = cur;
    {
        uint32_t xsdt_len = (uint32_t)(sizeof(AcpiSdtHeader) + 2u * sizeof(uint64_t));
        uint8_t *base = guest_ram + xsdt_gpa;
        AcpiSdtHeader *xsdt = (AcpiSdtHeader *)base;
        uint64_t *ents = (uint64_t *)(base + sizeof(AcpiSdtHeader));

        mem_zero(base, xsdt_len);
        fill_sdt_header(xsdt, "XSDT", xsdt_len, 1, "LNGXSDT ");
        ents[0] = fadt_gpa;
        ents[1] = madt_gpa;
        xsdt->checksum = acpi_checksum(base, xsdt_len);
        cur = align16(cur + xsdt_len);
    }

    /* 6. RSDP v2 at rsdp_gpa. */
    {
        AcpiRsdp *rsdp = (AcpiRsdp *)(guest_ram + rsdp_gpa);
        mem_zero(rsdp, sizeof(*rsdp));
        mem_copy(rsdp->signature, "RSD PTR ", 8);
        mem_copy(rsdp->oem_id, "LNGEMU", 6);
        rsdp->revision = 2;
        rsdp->rsdt_address = (uint32_t)rsdt_gpa;
        rsdp->length = (uint32_t)sizeof(*rsdp);
        rsdp->xsdt_address = xsdt_gpa;
        rsdp->checksum = acpi_checksum(rsdp, 20);
        rsdp->ext_checksum = acpi_checksum(rsdp, rsdp->length);
    }

    if (out_info != 0) {
        out_info->rsdp_gpa = rsdp_gpa;
        out_info->xsdt_gpa = xsdt_gpa;
        out_info->fadt_gpa = fadt_gpa;
        out_info->madt_gpa = madt_gpa;
        out_info->dsdt_gpa = dsdt_gpa;
        out_info->nr_cpus = nr_cpus;
    }
    return 1;
}

int loongemu_acpi_validate_rsdp(const void *rsdp_ptr,
                                uint8_t *out_rev,
                                uint64_t *out_xsdt_pa,
                                char out_oem_id[7])
{
    const AcpiRsdp *r = (const AcpiRsdp *)rsdp_ptr;
    static const char sig[8] = { 'R', 'S', 'D', ' ', 'P', 'T', 'R', ' ' };

    if (r == 0) {
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        if (r->signature[i] != sig[i]) {
            return 0;
        }
    }
    if (acpi_checksum(r, 20) != 0) {
        return 0;
    }
    if (r->revision >= 2) {
        if (r->length < 36 || acpi_checksum(r, r->length) != 0) {
            return 0;
        }
    }
    if (out_rev != 0) {
        *out_rev = r->revision;
    }
    if (out_xsdt_pa != 0) {
        *out_xsdt_pa = (r->revision >= 2) ? r->xsdt_address
                                          : (uint64_t)r->rsdt_address;
    }
    if (out_oem_id != 0) {
        for (int i = 0; i < 6; i++) {
            out_oem_id[i] = r->oem_id[i];
        }
        out_oem_id[6] = '\0';
    }
    return 1;
}
