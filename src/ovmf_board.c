#include "loongemu/ovmf_board.h"

static void ovmf_memset(void *dst, uint8_t v, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) {
        p[i] = v;
    }
}

static void ovmf_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void ovmf_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void ovmf_put_le64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8u; i++) {
        p[i] = (uint8_t)((v >> (i * 8u)) & 0xffu);
    }
}

static void ovmf_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xffu);
    p[1] = (uint8_t)(v & 0xffu);
}

static void ovmf_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

static uint32_t ovmf_bswap32(uint32_t v)
{
    return ((v & 0x000000ffu) << 24) |
           ((v & 0x0000ff00u) << 8) |
           ((v & 0x00ff0000u) >> 8) |
           ((v & 0xff000000u) >> 24);
}

static uint64_t ovmf_bswap64(uint64_t v)
{
    return ((uint64_t)ovmf_bswap32((uint32_t)v) << 32) |
           (uint64_t)ovmf_bswap32((uint32_t)(v >> 32));
}

static void ovmf_cmos_init(OvmfBoard *b)
{
    uint64_t low_ram = b->ram_size;
    uint64_t high_ram = 0;
    uint32_t ext16m_kb = 0;
    uint32_t above16m_64k = 0;
    uint32_t above4g_64k = 0;

    if (low_ram > UINT64_C(0x100000000)) {
        high_ram = low_ram - UINT64_C(0x100000000);
        low_ram = UINT64_C(0x100000000);
    }
    if (low_ram > UINT64_C(0x100000)) {
        uint64_t ext = low_ram - UINT64_C(0x100000);
        if (ext > UINT64_C(15) * 1024u * 1024u) {
            ext16m_kb = 15u * 1024u;
        } else {
            ext16m_kb = (uint32_t)(ext / 1024u);
        }
    }
    if (low_ram > UINT64_C(0x1000000)) {
        above16m_64k = (uint32_t)((low_ram - UINT64_C(0x1000000)) / 65536u);
    }
    if (high_ram > 0) {
        above4g_64k = (uint32_t)(high_ram / 65536u);
    }

    ovmf_memset(b->cmos, 0, sizeof(b->cmos));
    /* RTC BCD time: 2026-01-01 00:00:00 */
    b->cmos[0x00] = 0x00u; /* seconds */
    b->cmos[0x02] = 0x00u; /* minutes */
    b->cmos[0x04] = 0x00u; /* hours */
    b->cmos[0x06] = 0x05u; /* weekday (Thu) */
    b->cmos[0x07] = 0x01u; /* day */
    b->cmos[0x08] = 0x01u; /* month */
    b->cmos[0x09] = 0x26u; /* year */
    b->cmos[0x0a] = 0x26u; /* Status A: UIP=0, 32.768 kHz */
    b->cmos[0x0b] = 0x02u; /* Status B: 24-hour mode */
    b->cmos[0x0c] = 0x00u; /* Status C */
    b->cmos[0x0d] = 0x80u; /* Status D: VRT=1 (valid RAM & time) */
    b->cmos[0x32] = 0x20u; /* Century (20) */

    /* Conventional memory: 640 KiB */
    ovmf_put_le16(&b->cmos[0x15], 640u);
    /* Extended memory (1 MiB .. 16 MiB) in KiB */
    ovmf_put_le16(&b->cmos[0x17], (uint16_t)ext16m_kb);
    ovmf_put_le16(&b->cmos[0x30], (uint16_t)ext16m_kb);
    /* Extended memory above 16 MiB in 64 KiB blocks */
    ovmf_put_le16(&b->cmos[0x34], (uint16_t)above16m_64k);
    /* High memory above 4 GiB in 64 KiB blocks */
    b->cmos[0x5b] = (uint8_t)(above4g_64k & 0xffu);
    b->cmos[0x5c] = (uint8_t)((above4g_64k >> 8) & 0xffu);
    b->cmos[0x5d] = (uint8_t)((above4g_64k >> 16) & 0xffu);
    /* SMP CPU count minus 1 (QEMU/SeaBIOS/OVMF convention) */
    b->cmos[0x5f] = (uint8_t)(b->nr_cpus > 0 ? (b->nr_cpus - 1u) : 0u);
}

void ovmf_board_init(OvmfBoard *b, uint64_t ram_size, uint32_t nr_cpus,
                     OvmfPhysReadFn phys_read, OvmfPhysWriteFn phys_write)
{
    ovmf_memset(b, 0, sizeof(*b));
    b->ram_size = ram_size;
    b->nr_cpus = nr_cpus ? nr_cpus : 1u;
    b->phys_read = phys_read;
    b->phys_write = phys_write;
    ovmf_cmos_init(b);

    /* Populate default etc/e820 table (3 entries: low RAM, ACPI, high RAM). */
    ovmf_put_le64(&b->e820_buf[0], 0ULL);
    ovmf_put_le64(&b->e820_buf[8], UINT64_C(0x0009fc00));
    ovmf_put_le32(&b->e820_buf[16], 1u); /* E820_RAM */

    ovmf_put_le64(&b->e820_buf[20], UINT64_C(0x000e0000));
    ovmf_put_le64(&b->e820_buf[28], UINT64_C(0x00020000));
    ovmf_put_le32(&b->e820_buf[36], 2u); /* E820_RESERVED */

    ovmf_put_le64(&b->e820_buf[40], UINT64_C(0x00100000));
    ovmf_put_le64(&b->e820_buf[48],
                  ram_size > UINT64_C(0x100000) ? (ram_size - UINT64_C(0x100000)) : 0ULL);
    ovmf_put_le32(&b->e820_buf[56], 1u); /* E820_RAM */
    b->e820_len = 60u;

    ovmf_board_add_file(b, "etc/e820", b->e820_buf, b->e820_len);
}

uint16_t ovmf_board_add_file(OvmfBoard *b, const char *name,
                             const uint8_t *data, uint32_t size)
{
    if (!b || !name || b->file_count >= LOONGEMU_OVMF_MAX_FILES) {
        return 0;
    }
    OvmfFwCfgFile *f = &b->files[b->file_count];
    f->selector = (uint16_t)(FW_CFG_FILE_FIRST + b->file_count);
    f->size = size;
    f->data = data;
    ovmf_memset(f->name, 0, sizeof(f->name));
    for (size_t i = 0; i < sizeof(f->name) - 1u && name[i] != '\0'; i++) {
        f->name[i] = name[i];
    }
    b->file_count++;
    return f->selector;
}

int ovmf_board_map_firmware(OvmfBoard *b, LoongemuMemMap *mem_map,
                            const uint8_t *rom_buf, uint64_t rom_size,
                            uint8_t *vars_buf, uint64_t vars_size)
{
    if (!b || !mem_map || !rom_buf || rom_size == 0 || (rom_size & 0xfffu) != 0) {
        return 0;
    }
    b->rom_gpa_base = LOONGEMU_OVMF_TOP_GPA - rom_size;
    b->rom_size = rom_size;
    b->rom_data = rom_buf;

    if (!loongemu_mem_map_add(mem_map, b->rom_gpa_base, rom_size,
                              (uint64_t)(uintptr_t)rom_buf,
                              LOONGEMU_MEM_ROM, "ovmf_code")) {
        return 0;
    }

    if (vars_buf != 0 && vars_size > 0 && (vars_size & 0xfffu) == 0) {
        b->vars_flash.gpa_base = b->rom_gpa_base - vars_size;
        b->vars_flash.size = vars_size;
        b->vars_flash.storage = vars_buf;
        b->vars_flash.cmd = 0xffu; /* Read Array mode */
        b->vars_flash.status = 0x80u; /* Ready */
        b->vars_flash.write_cycle = 0;
        b->vars_flash.read_only = 0;
        if (!loongemu_mem_map_add(mem_map, b->vars_flash.gpa_base, vars_size,
                                  0, LOONGEMU_MEM_MMIO, "ovmf_vars")) {
            return 0;
        }
    }
    return 1;
}

static uint8_t fw_cfg_read_byte(OvmfBoard *b)
{
    uint16_t sel = b->fw_cfg_sel & 0x3fffu;
    uint32_t off = b->fw_cfg_off++;
    uint8_t tmp[8];

    switch (sel) {
    case FW_CFG_SIGNATURE: {
        static const uint8_t sig[4] = { 'Q', 'E', 'M', 'U' };
        return (off < 4u) ? sig[off] : 0u;
    }
    case FW_CFG_ID:
        ovmf_put_le32(tmp, 0x00000003u); /* traditional + DMA */
        return (off < 4u) ? tmp[off] : 0u;
    case FW_CFG_RAM_SIZE:
        ovmf_put_le64(tmp, b->ram_size);
        return (off < 8u) ? tmp[off] : 0u;
    case FW_CFG_NOGRAPHIC:
        ovmf_put_le16(tmp, 1u);
        return (off < 2u) ? tmp[off] : 0u;
    case FW_CFG_NB_CPUS:
    case FW_CFG_MAX_CPUS:
        ovmf_put_le16(tmp, (uint16_t)b->nr_cpus);
        return (off < 2u) ? tmp[off] : 0u;
    case FW_CFG_FILE_DIR: {
        if (off < 4u) {
            ovmf_put_be32(tmp, b->file_count);
            return tmp[off];
        }
        uint32_t rel = off - 4u;
        uint32_t idx = rel / 64u;
        uint32_t foff = rel % 64u;
        if (idx >= b->file_count) {
            return 0u;
        }
        const OvmfFwCfgFile *f = &b->files[idx];
        if (foff < 4u) {
            ovmf_put_be32(tmp, f->size);
            return tmp[foff];
        }
        if (foff < 6u) {
            ovmf_put_be16(tmp, f->selector);
            return tmp[foff - 4u];
        }
        if (foff < 8u) {
            return 0u;
        }
        return (uint8_t)f->name[foff - 8u];
    }
    default:
        for (uint32_t i = 0; i < b->file_count; i++) {
            if (b->files[i].selector == sel) {
                if (b->files[i].data != 0 && off < b->files[i].size) {
                    return b->files[i].data[off];
                }
                return 0u;
            }
        }
        return 0u;
    }
}

static void fw_cfg_do_dma(OvmfBoard *b, uint64_t dma_gpa)
{
    if (!b->phys_read || !b->phys_write) {
        return;
    }
    uint32_t ctrl = ovmf_bswap32((uint32_t)b->phys_read(dma_gpa + 0u, 4));
    uint32_t len = ovmf_bswap32((uint32_t)b->phys_read(dma_gpa + 4u, 4));
    uint64_t addr = ovmf_bswap64(b->phys_read(dma_gpa + 8u, 8));

    if (ctrl & FW_CFG_DMA_CTL_SELECT) {
        b->fw_cfg_sel = (uint16_t)(ctrl >> 16);
        b->fw_cfg_off = 0;
    }
    if (ctrl & FW_CFG_DMA_CTL_READ) {
        for (uint32_t i = 0; i < len; i++) {
            uint8_t byte = fw_cfg_read_byte(b);
            b->phys_write(addr + i, 1, byte);
        }
    } else if (ctrl & FW_CFG_DMA_CTL_SKIP) {
        b->fw_cfg_off += len;
    } else if (ctrl & FW_CFG_DMA_CTL_WRITE) {
        /* Read-only config entries ignore writes after advancing offset. */
        b->fw_cfg_off += len;
    }
    b->phys_write(dma_gpa + 0u, 4, 0u);
}

int ovmf_board_io_read(OvmfBoard *b, uint32_t port, int size, uint64_t *out)
{
    if (!b || !out) {
        return 0;
    }
    if (port == LOONGEMU_CMOS_INDEX_PORT) {
        *out = b->cmos_index;
        return 1;
    }
    if (port == LOONGEMU_CMOS_DATA_PORT) {
        *out = b->cmos[b->cmos_index & 0x7fu];
        return 1;
    }
    if (port == LOONGEMU_FW_CFG_DATA_PORT) {
        uint64_t val = 0;
        int n = (size > 0 && size <= 8) ? size : 1;
        for (int i = 0; i < n; i++) {
            val |= ((uint64_t)fw_cfg_read_byte(b)) << (i * 8);
        }
        *out = val;
        return 1;
    }
    if (port == LOONGEMU_FW_CFG_DMA_HI_PORT) {
        /* Signature "QEMU" in big-endian (0x51454d55) for DMA presence probe */
        *out = UINT64_C(0x554d4551);
        return 1;
    }
    if (port == LOONGEMU_FW_CFG_DMA_LO_PORT) {
        /* Signature " CFG" in big-endian (0x20434647) */
        *out = UINT64_C(0x47464320);
        return 1;
    }
    return 0;
}

int ovmf_board_io_write(OvmfBoard *b, uint32_t port, int size, uint64_t value)
{
    (void)size;
    if (!b) {
        return 0;
    }
    if (port == LOONGEMU_CMOS_INDEX_PORT) {
        b->cmos_index = (uint8_t)(value & 0x7fu);
        return 1;
    }
    if (port == LOONGEMU_CMOS_DATA_PORT) {
        uint8_t idx = b->cmos_index & 0x7fu;
        /* Keep Status D (VRT) read-only */
        if (idx != 0x0du) {
            b->cmos[idx] = (uint8_t)(value & 0xffu);
        }
        return 1;
    }
    if (port == LOONGEMU_FW_CFG_SEL_PORT) {
        b->fw_cfg_sel = (uint16_t)(value & 0xffffu);
        b->fw_cfg_off = 0;
        return 1;
    }
    if (port == LOONGEMU_FW_CFG_DATA_PORT) {
        return 1;
    }
    if (port == LOONGEMU_FW_CFG_DMA_HI_PORT) {
        uint32_t hi = ovmf_bswap32((uint32_t)(value & 0xffffffffu));
        b->fw_cfg_dma_addr = ((uint64_t)hi << 32) | (b->fw_cfg_dma_addr & 0xffffffffULL);
        return 1;
    }
    if (port == LOONGEMU_FW_CFG_DMA_LO_PORT) {
        uint32_t lo = ovmf_bswap32((uint32_t)(value & 0xffffffffu));
        b->fw_cfg_dma_addr = (b->fw_cfg_dma_addr & ~UINT64_C(0xffffffff)) | (uint64_t)lo;
        fw_cfg_do_dma(b, b->fw_cfg_dma_addr);
        b->fw_cfg_dma_addr = 0;
        return 1;
    }
    return 0;
}

static uint8_t pflash_cfi_query_byte(const OvmfPflash *pf, uint64_t off)
{
    uint64_t idx = off;
    uint32_t blocks = (uint32_t)(pf->size / 4096u);
    uint8_t dev_log2 = 16u; /* default 64 KiB */
    uint64_t sz = pf->size;
    while (sz > (UINT64_C(1) << dev_log2) && dev_log2 < 30u) {
        dev_log2++;
    }
    switch (idx) {
    case 0x10u: return 'Q';
    case 0x11u: return 'R';
    case 0x12u: return 'Y';
    case 0x13u: return 0x01u; /* Intel/Sharp command set */
    case 0x14u: return 0x00u;
    case 0x27u: return dev_log2;
    case 0x2cu: return 0x01u; /* 1 erase block region */
    case 0x2du: return (uint8_t)((blocks - 1u) & 0xffu);
    case 0x2eu: return (uint8_t)(((blocks - 1u) >> 8) & 0xffu);
    case 0x2fu: return 0x10u; /* 4096 / 256 = 16 */
    case 0x30u: return 0x00u;
    default:    return 0x00u;
    }
}

uint64_t ovmf_pflash_mmio_read(void *opaque, uint64_t offset, unsigned size)
{
    OvmfPflash *pf = (OvmfPflash *)opaque;
    uint64_t val = 0;
    if (!pf || !pf->storage || offset >= pf->size) {
        return ~UINT64_C(0);
    }
    if (size == 0 || size > 8u) {
        size = 1u;
    }
    if (pf->cmd == 0x70u) {
        return pf->status;
    }
    if (pf->cmd == 0x98u) {
        return pflash_cfi_query_byte(pf, offset);
    }
    for (unsigned i = 0; i < size && (offset + i) < pf->size; i++) {
        val |= ((uint64_t)pf->storage[offset + i]) << (i * 8u);
    }
    return val;
}

void ovmf_pflash_mmio_write(void *opaque, uint64_t offset, uint64_t val, unsigned size)
{
    OvmfPflash *pf = (OvmfPflash *)opaque;
    uint8_t cmd = (uint8_t)(val & 0xffu);
    if (!pf || !pf->storage || offset >= pf->size) {
        return;
    }
    if (size == 0 || size > 8u) {
        size = 1u;
    }
    if (pf->write_cycle) {
        if (!pf->read_only) {
            if (pf->cmd == 0x40u || pf->cmd == 0x10u) {
                for (unsigned i = 0; i < size && (offset + i) < pf->size; i++) {
                    pf->storage[offset + i] &= (uint8_t)((val >> (i * 8u)) & 0xffu);
                }
            } else if (pf->cmd == 0x20u && cmd == 0xd0u) {
                uint64_t blk_base = offset & ~UINT64_C(0xfff);
                for (uint64_t i = 0; i < 4096u && (blk_base + i) < pf->size; i++) {
                    pf->storage[blk_base + i] = 0xffu;
                }
            }
        }
        pf->write_cycle = 0;
        pf->status = 0x80u;
        pf->cmd = 0x70u;
        return;
    }

    switch (cmd) {
    case 0xffu: /* Read Array */
        pf->cmd = 0xffu;
        break;
    case 0x98u: /* CFI Query */
        pf->cmd = 0x98u;
        break;
    case 0x70u: /* Read Status */
        pf->cmd = 0x70u;
        break;
    case 0x50u: /* Clear Status */
        pf->status = 0x80u;
        pf->cmd = 0xffu;
        break;
    case 0x40u: /* Single Byte/Word Program */
    case 0x10u:
    case 0x20u: /* Block Erase setup */
        pf->cmd = cmd;
        pf->write_cycle = 1;
        break;
    default:
        pf->cmd = 0xffu;
        break;
    }
}
