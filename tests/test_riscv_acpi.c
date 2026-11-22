/* Host tests for the RISC-V ACPI table builder (MADT RINTC/IMSIC/APLIC, RHCT). */
#include <stdio.h>
#include <string.h>
#include "loongemu/riscv_acpi.h"

static int checks;
static int failures;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "ACPI: FAIL %s\n", name);                       \
        }                                                                   \
    } while (0)

static uint8_t buf[4096];

static uint32_t load_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint32_t load_u16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t decode_pkg_length(const uint8_t *p, uint32_t *pos)
{
    uint8_t lead = p[0];
    if ((lead >> 6) == 3) {
        uint32_t value = ((uint32_t)(lead & 0x0F) << 24) |
                         ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
                         (uint32_t)p[3];
        *pos = 4;
        return value;
    }
    *pos = 1;
    return lead & 0x3F;
}

static uint64_t load_u64(const uint8_t *p)
{
    return (uint64_t)load_u32(p) | (uint64_t)load_u32(p + 4) << 32;
}

/* Node header (6) + IsaLength (2) + string + NUL, padded to 2 bytes. */
static uint32_t isa_node_size(const char *isa)
{
    uint32_t len = 6 + 2 + (uint32_t)strlen(isa) + 1;
    return (len & 1u) ? len + 1 : len;
}

int main(void)
{
    const LoongemuRiscvAcpiHart harts[2] = {
        { .hart_id = 0, .uid = 0, .ext_intc_id = 0, .enabled = 1 },
        { .hart_id = 1, .uid = 1, .ext_intc_id = 1, .enabled = 1 },
    };
    LoongemuRiscvAcpi cfg;
    uint32_t len;

    memset(&cfg, 0, sizeof(cfg));
    cfg.oem_id = "LOONGX";
    cfg.oem_table_id = "RISCVIRT";
    cfg.harts = harts;
    cfg.hart_count = 2;
    cfg.has_imsic = 1;
    cfg.imsic_num_ids = 256;
    cfg.imsic_num_guest_ids = 256;
    cfg.imsic_guest_index_bits = 1;
    cfg.imsic_hart_index_bits = 1;
    cfg.imsic_group_index_bits = 0;
    cfg.imsic_group_index_shift = 24;
    cfg.has_aplic = 1;
    cfg.aplic_id = 0;
    cfg.aplic_hw_id = "APLIC";
    cfg.aplic_num_idcs = 2;
    cfg.aplic_num_sources = 96;
    cfg.aplic_gsi_base = 0;
    cfg.aplic_addr = 0xd000000;
    cfg.aplic_size = 0x8000;
    cfg.isa = "rv64imafdch_zicbom";
    cfg.timer_hz = 10000000;
    cfg.has_cmo = 1;
    cfg.cbom_block_log2 = 6;
    cfg.cboz_block_log2 = 6;
    cfg.has_mmu = 1;
    cfg.mmu_type = LOONGEMU_RHCT_MMU_SV39;
    cfg.dsdt_gpa = 0x80010000;
    cfg.fadt_minor_version = 6;
    cfg.has_spcr = 1;
    cfg.spcr_interface_type = 0x12;
    cfg.spcr_base = 0x10000000;
    cfg.spcr_gsi = 10;
    cfg.spcr_baud_rate = 7;
    cfg.spcr_terminal_type = 3;
    cfg.spcr_namespace = ".";
    cfg.has_mcfg = 1;
    cfg.mcfg_ecam_base = 0x30000000;
    cfg.mcfg_segment = 0;
    cfg.mcfg_bus_start = 0;
    cfg.mcfg_bus_end = 255;
    static LoongemuRiscvAcpiVirtio virtio[2] = {
        { .base = 0x10001000, .size = 0x1000, .gsi = 1 },
        { .base = 0x10002000, .size = 0x1000, .gsi = 2 },
    };
    cfg.has_dsdt = 1;
    cfg.pci_hid = "PNP0A08";
    cfg.pci_ecam_base = 0x30000000;
    cfg.pci_ecam_size = 0x10000000;
    cfg.pci_mmio_base = 0x40000000;
    cfg.pci_mmio_size = 0x40000000;
    cfg.uart_hid = "PNP0501";
    cfg.uart_base = 0x10000000;
    cfg.uart_size = 0x100;
    cfg.uart_gsi = 10;
    cfg.virtio = virtio;
    cfg.virtio_count = 2;
    cfg.has_plic = 1;
    cfg.plic_hw_id = "PLIC";
    cfg.plic_num_irqs = 96;
    cfg.plic_max_prio = 7;
    cfg.plic_addr = 0xc000000;
    cfg.plic_size = 0x400000;

    /* MADT. */
    uint32_t madt_size = loongemu_riscv_acpi_madt_size(&cfg);
    CHECK(madt_size == 36 + 8 + 2 * 36 + 16 + 36 + 36,
          "MADT size matches layout");
    memset(buf, 0, sizeof(buf));
    len = loongemu_riscv_acpi_madt_build(&cfg, buf, sizeof(buf));
    CHECK(len == madt_size, "MADT build reports the computed size");
    CHECK(memcmp(buf, "APIC", 4) == 0, "MADT signature");
    CHECK(load_u32(buf + 4) == len, "MADT length field");
    CHECK(buf[8] == LOONGEMU_ACPI_MADT_REVISION, "MADT revision");
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    CHECK(sum == 0, "MADT checksum closes");
    CHECK(memcmp(buf + 10, "LOONGX", 6) == 0, "OEM id is padded");
    CHECK(memcmp(buf + 16, "RISCVIRT", 8) == 0, "OEM table id");
    CHECK(loongemu_riscv_acpi_madt_build(&cfg, buf, madt_size - 1) == 0,
          "MADT rejects an undersized buffer");

    /* Walk the MADT entries. */
    uint32_t offset = 44;
    int rintc = 0;
    int imsic = 0;
    int aplic = 0;
    int plic = 0;
    while (offset < len) {
        uint8_t type = buf[offset];
        uint8_t entry_len = buf[offset + 1];
        if (type == LOONGEMU_ACPI_MADT_RINTC) {
            LoongemuAcpiRintc e;
            memcpy(&e, buf + offset, sizeof(e));
            CHECK(entry_len == sizeof(e), "RINTC length is 36");
            CHECK(e.version == 1, "RINTC version");
            CHECK(e.flags == 1, "RINTC enabled flag");
            CHECK(e.hart_id == harts[rintc].hart_id, "RINTC hart id");
            CHECK(e.uid == harts[rintc].uid, "RINTC ACPI uid");
            CHECK(e.ext_intc_id == harts[rintc].ext_intc_id,
                  "RINTC external interrupt controller id");
            rintc++;
        } else if (type == LOONGEMU_ACPI_MADT_IMSIC) {
            LoongemuAcpiImsic e;
            memcpy(&e, buf + offset, sizeof(e));
            CHECK(entry_len == sizeof(e), "IMSIC length is 16");
            CHECK(e.version == 1, "IMSIC version");
            CHECK(e.num_ids == 256 && e.num_guest_ids == 256,
                  "IMSIC interrupt identities");
            CHECK(e.guest_index_bits == 1 && e.hart_index_bits == 1,
                  "IMSIC index widths");
            CHECK(e.group_index_shift == 24, "IMSIC group shift");
            imsic++;
        } else if (type == LOONGEMU_ACPI_MADT_APLIC) {
            LoongemuAcpiAplic e;
            memcpy(&e, buf + offset, sizeof(e));
            CHECK(entry_len == sizeof(e), "APLIC length is 36");
            CHECK(e.version == 1 && e.id == 0, "APLIC version/id");
            CHECK(memcmp(e.hw_id, "APLIC   ", 8) == 0, "APLIC hardware id");
            CHECK(e.num_idcs == 2 && e.num_sources == 96,
                  "APLIC interrupt counts");
            CHECK(e.base_addr == 0xd000000 && e.size == 0x8000,
                  "APLIC MMIO window");
            aplic++;
        } else if (type == LOONGEMU_ACPI_MADT_PLIC) {
            LoongemuAcpiPlic e;
            memcpy(&e, buf + offset, sizeof(e));
            CHECK(entry_len == sizeof(e), "PLIC length is 36");
            CHECK(e.version == 1 && e.id == 0, "PLIC version/id");
            CHECK(memcmp(e.hw_id, "PLIC    ", 8) == 0, "PLIC hardware id");
            CHECK(e.num_irqs == 96 && e.max_prio == 7,
                  "PLIC interrupt and priority counts");
            CHECK(e.base_addr == 0xc000000 && e.size == 0x400000,
                  "PLIC MMIO window");
            plic++;
        }
        offset += entry_len;
    }
    CHECK(rintc == 2, "two RINTC entries");
    CHECK(imsic == 1, "one IMSIC entry");
    CHECK(aplic == 1, "one APLIC entry");
    CHECK(plic == 1, "one PLIC entry");
    CHECK(offset == len, "MADT entries exactly fill the table");

    /* RHCT. */
    uint32_t rhct_size = loongemu_riscv_acpi_rhct_size(&cfg);
    memset(buf, 0, sizeof(buf));
    len = loongemu_riscv_acpi_rhct_build(&cfg, buf, sizeof(buf));
    CHECK(len == rhct_size, "RHCT build reports the computed size");
    CHECK(memcmp(buf, "RHCT", 4) == 0, "RHCT signature");
    CHECK(load_u32(buf + 4) == len, "RHCT length field");
    CHECK(buf[8] == 1, "RHCT revision");
    sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    CHECK(sum == 0, "RHCT checksum closes");
    CHECK(load_u32(buf + 36) == 0, "RHCT reserved field");
    CHECK(load_u64(buf + 40) == 10000000, "RHCT timer frequency");
    CHECK(load_u32(buf + 48) == 5, "RHCT node count");
    CHECK(load_u32(buf + 52) == 56, "RHCT node array offset");
    CHECK(loongemu_riscv_acpi_rhct_build(&cfg, buf, rhct_size - 1) == 0,
          "RHCT rejects an undersized buffer");

    /* ISA string node. */
    uint32_t isa_offset = 56;
    CHECK(load_u16(buf + isa_offset) == LOONGEMU_RHCT_ISA_NODE,
          "ISA node type");
    uint32_t isa_node_len = load_u16(buf + isa_offset + 2);
    CHECK(isa_node_len % 2 == 0, "ISA node stays 2-byte aligned");
    CHECK(load_u16(buf + isa_offset + 4) == 1, "ISA node revision");
    CHECK(load_u16(buf + isa_offset + 6) == strlen(cfg.isa) + 1,
          "ISA string length includes the NUL");
    CHECK(memcmp(buf + isa_offset + 8, cfg.isa, strlen(cfg.isa)) == 0,
          "ISA string contents");
    CHECK(buf[isa_offset + 8 + strlen(cfg.isa)] == 0, "ISA string is NUL");

    uint32_t cmo_offset = isa_offset + isa_node_len;
    CHECK(load_u16(buf + cmo_offset) == LOONGEMU_RHCT_CMO_NODE,
          "CMO node type");
    CHECK(load_u16(buf + cmo_offset + 2) == 10, "CMO node length");
    CHECK(buf[cmo_offset + 7] == 6 && buf[cmo_offset + 9] == 6,
          "CBO block sizes are log2(64)");

    uint32_t mmu_offset = cmo_offset + 10;
    CHECK(load_u16(buf + mmu_offset) == LOONGEMU_RHCT_MMU_NODE,
          "MMU node type");
    CHECK(load_u16(buf + mmu_offset + 2) == 8, "MMU node length");
    CHECK(buf[mmu_offset + 7] == LOONGEMU_RHCT_MMU_SV39, "MMU type is Sv39");

    /* Hart info nodes reference the CMO/MMU nodes. */
    uint32_t hart_offset = mmu_offset + 8;
    for (int i = 0; i < 2; i++) {
        CHECK(load_u16(buf + hart_offset) == LOONGEMU_RHCT_HART_INFO_NODE,
              "hart info node type");
        CHECK(load_u16(buf + hart_offset + 2) == 20, "hart info node length");
        CHECK(load_u16(buf + hart_offset + 4) == 1, "hart info revision");
        CHECK(load_u16(buf + hart_offset + 6) == 2, "two node offsets");
        CHECK(load_u32(buf + hart_offset + 8) == (uint32_t)i,
              "hart info ACPI uid");
        CHECK(load_u32(buf + hart_offset + 12) == cmo_offset,
              "hart info CMO offset");
        CHECK(load_u32(buf + hart_offset + 16) == mmu_offset,
              "hart info MMU offset");
        hart_offset += 20;
    }
    CHECK(hart_offset == len, "RHCT nodes exactly fill the table");

    /* Without CMO/MMU the hart info nodes shrink accordingly. */
    cfg.has_cmo = 0;
    cfg.has_mmu = 0;
    len = loongemu_riscv_acpi_rhct_build(&cfg, buf, sizeof(buf));
    CHECK(len == loongemu_riscv_acpi_rhct_size(&cfg),
          "minimal RHCT size matches");
    CHECK(load_u32(buf + 48) == 3, "minimal RHCT node count");
    uint32_t minimal_hart = 56 + isa_node_size(cfg.isa);
    CHECK(load_u16(buf + 56) == LOONGEMU_RHCT_ISA_NODE,
          "minimal RHCT still starts with the ISA node");
    CHECK(load_u16(buf + minimal_hart) == LOONGEMU_RHCT_HART_INFO_NODE,
          "minimal RHCT hart info follows the ISA node");
    CHECK(load_u16(buf + minimal_hart + 2) == 12,
          "minimal hart info node is 12 bytes");

    /* FADT rev 6: hardware-reduced, no PM blocks, DSDT pointers. */
    uint32_t fadt_size = loongemu_riscv_acpi_fadt_size(&cfg);
    CHECK(fadt_size == LOONGEMU_ACPI_FADT_LENGTH, "FADT size");
    memset(buf, 0, sizeof(buf));
    len = loongemu_riscv_acpi_fadt_build(&cfg, buf, sizeof(buf));
    CHECK(len == LOONGEMU_ACPI_FADT_LENGTH, "FADT build length");
    CHECK(memcmp(buf, "FACP", 4) == 0, "FADT signature");
    CHECK(buf[8] == 6, "FADT revision 6");
    CHECK(load_u32(buf + 40) == 0x80010000, "FADT DSDT address");
    CHECK(load_u32(buf + 112) == (1u << 20), "FADT is hardware-reduced");
    CHECK(buf[131] == 6, "FADT minor version");
    CHECK(load_u64(buf + 140) == 0x80010000, "FADT X_DSDT address");
    CHECK(load_u64(buf + 148) == 0, "FADT X_PM1a_EVT is zero");
    CHECK(memcmp(buf + 268, "LONGEMU", 8) == 0, "FADT hypervisor vendor");
    sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    CHECK(sum == 0, "FADT checksum closes");
    CHECK(loongemu_riscv_acpi_fadt_build(&cfg, buf, len - 1) == 0,
          "FADT rejects an undersized buffer");

    /* SPCR rev 4 for the 16550-compatible MMIO UART. */
    uint32_t spcr_size = loongemu_riscv_acpi_spcr_size(&cfg);
    CHECK(spcr_size == LOONGEMU_ACPI_SPCR_LENGTH, "SPCR size");
    memset(buf, 0, sizeof(buf));
    len = loongemu_riscv_acpi_spcr_build(&cfg, buf, sizeof(buf));
    CHECK(len == LOONGEMU_ACPI_SPCR_LENGTH, "SPCR build length");
    CHECK(memcmp(buf, "SPCR", 4) == 0, "SPCR signature");
    CHECK(buf[8] == 4, "SPCR revision 4");
    CHECK(buf[36] == 0x12, "SPCR interface type is 16550 compatible");
    CHECK(buf[40] == 0 && buf[41] == 8 && buf[43] == 1,
          "SPCR GAS is byte-accessed system memory");
    CHECK(load_u64(buf + 44) == 0x10000000, "SPCR MMIO base");
    CHECK(buf[52] == 0x10, "SPCR interrupt type is RISC-V PLIC/APLIC");
    CHECK(load_u32(buf + 54) == 10, "SPCR global system interrupt");
    CHECK(buf[58] == 7, "SPCR baud rate code");
    CHECK(load_u16(buf + 84) == 1 && load_u16(buf + 86) == 88,
          "SPCR namespace string length/offset");
    CHECK(buf[88] == '.', "SPCR namespace string");
    sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    CHECK(sum == 0, "SPCR checksum closes");
    cfg.has_spcr = 0;
    CHECK(loongemu_riscv_acpi_spcr_size(&cfg) == 0, "SPCR optional");
    cfg.has_spcr = 1;

    /* MCFG: one ECAM allocation entry. */
    uint32_t mcfg_size = loongemu_riscv_acpi_mcfg_size(&cfg);
    CHECK(mcfg_size == 60, "MCFG size");
    memset(buf, 0, sizeof(buf));
    len = loongemu_riscv_acpi_mcfg_build(&cfg, buf, sizeof(buf));
    CHECK(len == 60, "MCFG build length");
    CHECK(memcmp(buf, "MCFG", 4) == 0, "MCFG signature");
    CHECK(load_u64(buf + 36) == 0, "MCFG reserved field");
    CHECK(load_u64(buf + 44) == 0x30000000, "MCFG ECAM base");
    CHECK(load_u16(buf + 52) == 0, "MCFG PCI segment");
    CHECK(buf[54] == 0 && buf[55] == 255, "MCFG bus range");
    sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    CHECK(sum == 0, "MCFG checksum closes");
    cfg.has_mcfg = 0;
    CHECK(loongemu_riscv_acpi_mcfg_size(&cfg) == 0, "MCFG optional");
    cfg.has_mcfg = 1;

    /* DSDT: structurally validate the AML subset we emit. */
    uint32_t dsdt_size = loongemu_riscv_acpi_dsdt_size(&cfg);
    CHECK(dsdt_size > 36, "DSDT size includes the AML body");
    memset(buf, 0, sizeof(buf));
    len = loongemu_riscv_acpi_dsdt_build(&cfg, buf, sizeof(buf));
    CHECK(len == dsdt_size, "DSDT build length matches the size probe");
    CHECK(memcmp(buf, "DSDT", 4) == 0, "DSDT signature");
    CHECK(buf[8] == LOONGEMU_ACPI_DSDT_REVISION, "DSDT revision");
    sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    CHECK(sum == 0, "DSDT checksum closes");

    {
        const uint8_t *aml = buf + 36;
        uint32_t aml_len = len - 36;
        uint32_t len_bytes = 0;
        uint32_t pos = 0;

        CHECK(aml[pos] == 0x10, "DSDT starts with ScopeOp");
        uint32_t scope_start = pos + 1;
        uint32_t scope_len = decode_pkg_length(aml + scope_start, &len_bytes);
        CHECK(scope_start + scope_len == aml_len,
              "Scope PkgLength spans the whole body");
        pos = scope_start + len_bytes;
        CHECK(aml[pos] == 0x5C, "Scope uses the root prefix");
        CHECK(memcmp(aml + pos + 1, "_SB_", 4) == 0, "Scope is \\\\_SB");
        pos += 5;

        int devices = 0;
        int cpus = 0;
        int saw_pci = 0;
        int saw_uart = 0;
        int saw_virtio = 0;
        int cpu0_hid = 0;
        int cpu0_mat = 0;
        int pci_crs = 0;
        int parse_errors = 0;

        while (pos < aml_len) {
            if (aml[pos] != 0x5B || aml[pos + 1] != 0x82) {
                parse_errors++;
                break;
            }
            uint32_t obj_start = pos + 2;
            uint32_t obj_len = decode_pkg_length(aml + obj_start, &len_bytes);
            uint32_t obj_end = obj_start + obj_len;
            pos = obj_start + len_bytes;
            char name[5];
            memcpy(name, aml + pos, 4);
            name[4] = 0;
            pos += 4;
            devices++;
            if (name[0] == 'C') {
                cpus++;
            } else if (strcmp(name, "PCI0") == 0) {
                saw_pci = 1;
            } else if (strcmp(name, "UART") == 0) {
                saw_uart = 1;
            } else if (name[0] == 'V') {
                saw_virtio++;
            }
            while (pos < obj_end) {
                if (aml[pos] != 0x08) {
                    parse_errors++;
                    pos = obj_end;
                    break;
                }
                pos++;
                char field[5];
                memcpy(field, aml + pos, 4);
                field[4] = 0;
                pos += 4;
                uint8_t tag = aml[pos++];
                if (tag == 0x0D) {
                    const char *str = (const char *)(aml + pos);
                    pos += (uint32_t)strlen(str) + 1;
                    if (strcmp(name, "C000") == 0 &&
                        strcmp(field, "_HID") == 0 &&
                        strcmp(str, "ACPI0007") == 0) {
                        cpu0_hid = 1;
                    }
                } else if (tag == 0x00 || tag == 0x01) {
                    /* zero/one */
                } else if (tag == 0x0A) {
                    pos += 1;
                } else if (tag == 0x0B) {
                    pos += 2;
                } else if (tag == 0x0C) {
                    pos += 4;
                } else if (tag == 0x0E) {
                    pos += 8;
                } else if (tag == 0x11) {
                    uint32_t buf_start = pos;
                    uint32_t buf_len = decode_pkg_length(aml + buf_start,
                                                         &len_bytes);
                    uint32_t buf_end = buf_start + buf_len;
                    pos = buf_start + len_bytes;
                    uint8_t size_tag = aml[pos++];
                    uint32_t size = 0;
                    if (size_tag == 0x00) {
                        size = 0;
                    } else if (size_tag == 0x0A) {
                        size = aml[pos++];
                    } else if (size_tag == 0x0B) {
                        size = load_u16(aml + pos);
                        pos += 2;
                    } else if (size_tag == 0x0C) {
                        size = load_u32(aml + pos);
                        pos += 4;
                    } else {
                        CHECK(0, "DSDT buffer size encoding");
                        parse_errors++;
                        break;
                    }
                    if (strcmp(name, "C000") == 0 &&
                        strcmp(field, "_MAT") == 0) {
                        CHECK(size == 36, "_MAT buffer holds one RINTC");
                        CHECK(aml[pos] == LOONGEMU_ACPI_MADT_RINTC,
                              "_MAT starts with a RINTC structure");
                        cpu0_mat = 1;
                    }
                    if (strcmp(name, "PCI0") == 0 &&
                        strcmp(field, "_CRS") == 0) {
                        CHECK(size == 96, "_CRS holds two QWord windows");
                        CHECK(aml[pos] == 0x8A, "_CRS uses QWord descriptors");
                        pci_crs = 1;
                    }
                    pos += size;
                    CHECK(pos == buf_end, "buffer PkgLength spans its data");
                } else {
                    CHECK(0, "DSDT value encoding");
                    parse_errors++;
                    pos = obj_end;
                    break;
                }
            }
            CHECK(pos == obj_end, "device PkgLength spans its body");
        }
        CHECK(parse_errors == 0, "DSDT AML parses without errors");
        CHECK(devices == 2 + 1 + 1 + 2,
              "DSDT holds CPUs, PCI0, UART and virtio devices");
        CHECK(cpus == 2, "one device per hart");
        CHECK(saw_pci == 1, "PCIe host bridge present");
        CHECK(saw_uart == 1, "UART device present");
        CHECK(saw_virtio == 2, "virtio-mmio devices present");
        CHECK(cpu0_hid == 1, "CPU _HID is ACPI0007");
        CHECK(cpu0_mat == 1, "CPU _MAT carries the RINTC structure");
        CHECK(pci_crs == 1, "PCI _CRS carries the ECAM window");
        CHECK(pos == aml_len, "DSDT body fully parsed");
    }
    cfg.has_dsdt = 0;
    CHECK(loongemu_riscv_acpi_dsdt_size(&cfg) == 0, "DSDT optional");
    cfg.has_dsdt = 1;

    /* NULL configuration is rejected, not dereferenced. */
    CHECK(loongemu_riscv_acpi_madt_size(NULL) == 0, "NULL MADT cfg rejected");
    CHECK(loongemu_riscv_acpi_rhct_size(NULL) == 0, "NULL RHCT cfg rejected");
    CHECK(loongemu_riscv_acpi_fadt_size(NULL) == 0, "NULL FADT cfg rejected");
    CHECK(loongemu_riscv_acpi_spcr_size(NULL) == 0, "NULL SPCR cfg rejected");
    CHECK(loongemu_riscv_acpi_mcfg_size(NULL) == 0, "NULL MCFG cfg rejected");
    CHECK(loongemu_riscv_acpi_dsdt_size(NULL) == 0, "NULL DSDT cfg rejected");

    printf("ACPI: %d checks passed\n", checks);
    return failures == 0 ? 0 : 1;
}
