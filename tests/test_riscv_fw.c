/* Host tests for the RISC-V firmware bridge (fw_cfg MMIO/DMA, pflash, RSDP). */
#include <stdio.h>
#include <string.h>
#include "loongemu/riscv_acpi.h"
#include "loongemu/riscv_fw.h"

static int checks;
static int failures;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FW: FAIL %s\n", name);                         \
        }                                                                   \
    } while (0)

static uint8_t guest_mem[4096];

static int gpa_read(void *opaque, uint64_t gpa, void *buf, uint32_t len)
{
    (void)opaque;
    if (gpa + len > sizeof(guest_mem)) {
        return -1;
    }
    memcpy(buf, guest_mem + gpa, len);
    return 0;
}

static int gpa_write(void *opaque, uint64_t gpa, const void *buf,
                     uint32_t len)
{
    (void)opaque;
    if (gpa + len > sizeof(guest_mem)) {
        return -1;
    }
    memcpy(guest_mem + gpa, buf, len);
    return 0;
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void write_dma_desc(uint64_t gpa, uint32_t control, uint32_t length,
                           uint64_t address)
{
    put_le32(guest_mem + gpa, control);
    put_le32(guest_mem + gpa + 4, length);
    for (int i = 0; i < 8; i++) {
        guest_mem[gpa + 8 + i] = (uint8_t)(address >> (8 * i));
    }
}

int main(void)
{
    static const uint8_t rsdp_item[36] = { 'R', 'S', 'D', ' ', 'P', 'T',
                                           'R', ' ' };
    static const uint8_t table_blob[64] = { 1, 2, 3, 4 };
    LoongemuFwCfg cfg;
    LoongemuFwCfgDmaOps ops = { gpa_read, gpa_write, NULL };
    LoongemuRiscvPflash pflash;
    LoongemuMemMap map;
    uint8_t code[4096];
    uint8_t vars[4096];
    uint8_t rsdp[36];
    uint8_t xsdt[64];
    uint64_t entries[2] = { 0x7fe00000, 0x7fe01000 };
    LoongemuRiscvAcpi acpi_cfg;

    memset(&acpi_cfg, 0, sizeof(acpi_cfg));
    acpi_cfg.oem_id = "LOONGX";
    acpi_cfg.oem_table_id = "RISCVFW";

    loongemu_fw_cfg_init(&cfg, 2, 8);
    CHECK(cfg.nb_cpus == 2 && cfg.max_cpus == 8, "fw_cfg init");
    CHECK(cfg.selector == 0 && cfg.offset == 0, "fw_cfg initial state");

    uint16_t rsdp_sel = loongemu_fw_cfg_add_file(&cfg, "etc/acpi/rsdp",
                                                 rsdp_item, sizeof(rsdp_item));
    CHECK(rsdp_sel == FW_CFG_FILE_FIRST_SELECT, "first file selector is 0x20");
    CHECK(loongemu_fw_cfg_add_file(&cfg, "etc/acpi/tables", table_blob,
                                   sizeof(table_blob)) ==
          FW_CFG_FILE_FIRST_SELECT + 1,
          "second file selector is 0x21");
    CHECK(loongemu_fw_cfg_add_file(&cfg, "", table_blob, 1) == 0,
          "empty names are rejected");

    /* Byte reads through the data register (offset 0), big-endian assembly. */
    cfg.selector = 0x0000;
    cfg.offset = 0;
    uint64_t sig = loongemu_fw_cfg_mmio_read(&cfg, 0, 4);
    CHECK(sig == 0x51454d55u, "signature reads as QEMU (big-endian)");
    cfg.selector = 0x0001;
    cfg.offset = 0;
    uint64_t features = loongemu_fw_cfg_mmio_read(&cfg, 0, 4);
    CHECK(features == 0x03000000u, "feature bitmap advertises DMA");

    /* File directory. */
    cfg.selector = FW_CFG_FILE_DIR_SELECT;
    cfg.offset = 0;
    uint8_t dir_hdr[4];
    for (int i = 0; i < 4; i++) {
        dir_hdr[i] = (uint8_t)loongemu_fw_cfg_mmio_read(&cfg, 0, 1);
    }
    CHECK(get_le32(dir_hdr) == 2, "file directory lists both files");

    /* Selector register is big-endian on MMIO. */
    /* MMIO selector is big-endian: the guest stores byteswap16(0x20). */
    loongemu_fw_cfg_mmio_write(&cfg, 8, 0x2000, 2, &ops);
    CHECK(cfg.selector == 0x20 && cfg.offset == 0,
          "selector write selects the file");
    CHECK(loongemu_fw_cfg_mmio_read(&cfg, 8, 2) == 0x2000,
          "selector reads back big-endian");
    CHECK(loongemu_fw_cfg_mmio_read(&cfg, 16, 8) ==
          UINT64_C(0x51454d5520434647),
          "DMA address register probe reads QEMU CFG");

    /* DMA select + read of the RSDP item into guest memory. */
    memset(guest_mem, 0, sizeof(guest_mem));
    write_dma_desc(0x100, 0x08 /* SELECT */, rsdp_sel, 0);
    CHECK(loongemu_fw_cfg_dma(&cfg, 0x100, &ops) == 0, "DMA select succeeds");
    CHECK(cfg.selector == rsdp_sel, "DMA select updates the selector");
    CHECK(get_le32(guest_mem + 0x100) == 0, "DMA clears the control word");
    write_dma_desc(0x100, 0x02 /* READ */, sizeof(rsdp_item), 0x200);
    CHECK(loongemu_fw_cfg_dma(&cfg, 0x100, &ops) == 0, "DMA read succeeds");
    CHECK(memcmp(guest_mem + 0x200, rsdp_item, sizeof(rsdp_item)) == 0,
          "DMA copies the file bytes");
    CHECK(get_le32(guest_mem + 0x100) == 0, "DMA read clears control");

    /* A write request is rejected (read-only item). */
    write_dma_desc(0x100, 0x10 /* WRITE */, 4, 0x300);
    CHECK(loongemu_fw_cfg_dma(&cfg, 0x100, &ops) != 0, "DMA write fails");
    CHECK(get_le32(guest_mem + 0x100) & 1, "DMA failure sets the error bit");
    CHECK(loongemu_fw_cfg_dma(&cfg, 0x3ff0, &ops) != 0,
          "DMA descriptor outside guest memory fails");

    /* MMIO-write DMA trigger path. */
    memset(guest_mem, 0, sizeof(guest_mem));
    write_dma_desc(0x100, 0x08, rsdp_sel, 0);
    loongemu_fw_cfg_mmio_write(&cfg, 16, 0x100, 8, &ops);
    CHECK(cfg.selector == rsdp_sel, "MMIO DMA trigger performs the transfer");

    /* RSDP revision 2 with both checksums. */
    loongemu_riscv_acpi_rsdp_build(rsdp, 0, 0x7fe00000, "LOONGX");
    CHECK(memcmp(rsdp, "RSD PTR ", 8) == 0, "RSDP signature");
    CHECK(rsdp[15] == 2, "RSDP revision 2");
    CHECK(rsdp[20] == 36, "RSDP length");
    uint64_t xsdt_addr = 0;
    for (int i = 0; i < 8; i++) {
        xsdt_addr |= (uint64_t)rsdp[24 + i] << (8 * i);
    }
    CHECK(xsdt_addr == 0x7fe00000, "RSDP points at the XSDT");
    uint8_t sum = 0;
    for (int i = 0; i < 20; i++) {
        sum = (uint8_t)(sum + rsdp[i]);
    }
    CHECK(sum == 0, "RSDP legacy checksum");
    sum = 0;
    for (int i = 0; i < 36; i++) {
        sum = (uint8_t)(sum + rsdp[i]);
    }
    CHECK(sum == 0, "RSDP extended checksum");

    /* XSDT lists the placed tables. */
    CHECK(loongemu_riscv_acpi_xsdt_size(2) == 52, "XSDT size");
    uint32_t xsdt_len = loongemu_riscv_acpi_xsdt_build(xsdt, sizeof(xsdt),
                                                       entries, 2, &acpi_cfg);
    CHECK(xsdt_len == 52, "XSDT build length");
    CHECK(memcmp(xsdt, "XSDT", 4) == 0, "XSDT signature");
    uint64_t first_entry = 0;
    for (int i = 0; i < 8; i++) {
        first_entry |= (uint64_t)xsdt[36 + i] << (8 * i);
    }
    CHECK(first_entry == entries[0], "XSDT first table pointer");
    sum = 0;
    for (uint32_t i = 0; i < xsdt_len; i++) {
        sum = (uint8_t)(sum + xsdt[i]);
    }
    CHECK(sum == 0, "XSDT checksum");

    /* pflash: code is ROM-like, vars writable, both mapped as MMIO. */
    memset(code, 0xA5, sizeof(code));
    memset(vars, 0x5A, sizeof(vars));
    int pflash_ok = loongemu_riscv_pflash_init(
        &pflash, code, sizeof(code), vars, sizeof(vars),
        LOONGEMU_RISCV_PFLASH_CODE_GPA, LOONGEMU_RISCV_PFLASH_VARS_GPA);
    CHECK(pflash_ok == 1, "pflash init");
    loongemu_mem_map_init(&map);
    CHECK(loongemu_riscv_pflash_map(&pflash, &map) == 1, "pflash maps");
    CHECK(map.count == 2, "two pflash regions");
    CHECK(map.regions[0].kind == LOONGEMU_MEM_MMIO &&
          map.regions[0].gpa_base == LOONGEMU_RISCV_PFLASH_CODE_GPA,
          "code slot mapped at its GPA");
    CHECK(map.regions[1].kind == LOONGEMU_MEM_MMIO &&
          map.regions[1].gpa_base == LOONGEMU_RISCV_PFLASH_VARS_GPA,
          "vars slot mapped at its GPA");
    CHECK((uint8_t)ovmf_pflash_mmio_read(&pflash.code, 0, 1) == 0xA5,
          "code reads from storage");
    ovmf_pflash_mmio_write(&pflash.code, 0, 0x00, 1);
    CHECK(code[0] == 0xA5, "code slot ignores writes");
    CHECK(pflash.vars.read_only == 0, "vars slot is writable");
    CHECK(pflash.code.read_only == 1, "code slot is read-only");

    /* NULL inputs are rejected. */
    CHECK(loongemu_riscv_pflash_init(NULL, code, 1, vars, 1, 0, 0) == 0,
          "NULL pflash rejected");
    CHECK(loongemu_riscv_pflash_map(NULL, &map) == 0, "NULL map inputs");
    CHECK(loongemu_fw_cfg_dma(NULL, 0, &ops) != 0, "NULL fw_cfg DMA");

    printf("FW: %d checks passed\n", checks);
    return failures == 0 ? 0 : 1;
}
