/* RISC-V firmware bridge: MMIO fw_cfg + pflash slots. */
#include <string.h>
#include "loongemu/riscv_fw.h"

#define FW_CFG_ID_DMA_ENABLED (1u << 1)
#define FW_CFG_ID_TRADITIONAL (1u << 0)
#define FW_CFG_DMA_CTL_ERROR  0x01u
#define FW_CFG_DMA_CTL_READ   0x02u
#define FW_CFG_DMA_CTL_SKIP   0x04u
#define FW_CFG_DMA_CTL_SELECT 0x08u
#define FW_CFG_DMA_CTL_WRITE  0x10u

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    put_le16(p, (uint16_t)v);
    put_le16(p + 2, (uint16_t)(v >> 16));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)get_le32(p) | (uint64_t)get_le32(p + 4) << 32;
}

void loongemu_fw_cfg_init(LoongemuFwCfg *cfg, uint16_t nb_cpus,
                          uint16_t max_cpus)
{
    if (cfg == 0) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->nb_cpus = nb_cpus;
    cfg->max_cpus = max_cpus;
}

uint16_t loongemu_fw_cfg_add_file(LoongemuFwCfg *cfg, const char *name,
                                  const uint8_t *data, uint32_t size)
{
    if (cfg == 0 || name == 0 || name[0] == '\0' || size == 0 ||
        cfg->file_count >= LOONGEMU_RISCV_FW_CFG_MAX_FILES) {
        return 0;
    }
    if (strlen(name) >= LOONGEMU_RISCV_FW_CFG_NAME_MAX) {
        return 0;
    }
    LoongemuFwCfgFile *file = &cfg->files[cfg->file_count];
    memset(file, 0, sizeof(*file));
    strcpy(file->name, name);
    file->size = size;
    file->data = data;
    file->select = (uint16_t)(FW_CFG_FILE_FIRST_SELECT + cfg->file_count);
    cfg->file_count++;
    return file->select;
}

static const LoongemuFwCfgFile *find_file(const LoongemuFwCfg *cfg,
                                          uint16_t select)
{
    for (uint32_t i = 0; i < cfg->file_count; i++) {
        if (cfg->files[i].select == select) {
            return &cfg->files[i];
        }
    }
    return 0;
}

/* Materializes a synthetic item; returns its length and fills up to 56 bytes. */
static uint32_t item_bytes(const LoongemuFwCfg *cfg, uint16_t select,
                           uint8_t out[56])
{
    switch (select) {
    case 0x0000: /* signature */
        memcpy(out, "QEMU", 4);
        return 4;
    case 0x0001: /* feature bitmap */
        put_le32(out, FW_CFG_ID_TRADITIONAL | FW_CFG_ID_DMA_ENABLED);
        return 4;
    case 0x0005: /* number of CPUs */
        put_le16(out, cfg->nb_cpus);
        return 2;
    case 0x000f: /* maximum CPUs */
        put_le16(out, cfg->max_cpus);
        return 2;
    default:
        return 0;
    }
}

/* File directory: u32 count, then {u32 size, u16 select, u16 reserved, name}. */
static uint32_t file_dir_bytes(const LoongemuFwCfg *cfg, uint8_t *out,
                               uint32_t cap)
{
    uint32_t need = 4 + cfg->file_count * (4 + 2 + 2 +
                                           LOONGEMU_RISCV_FW_CFG_NAME_MAX);
    if (out == 0 || cap < need) {
        return need;
    }
    put_le32(out, cfg->file_count);
    uint32_t pos = 4;
    for (uint32_t i = 0; i < cfg->file_count; i++) {
        const LoongemuFwCfgFile *file = &cfg->files[i];
        put_le32(out + pos, file->size);
        put_le16(out + pos + 4, file->select);
        put_le16(out + pos + 6, 0);
        memcpy(out + pos + 8, file->name, LOONGEMU_RISCV_FW_CFG_NAME_MAX);
        pos += 8 + LOONGEMU_RISCV_FW_CFG_NAME_MAX;
    }
    return need;
}

/* Byte read at the current selector/offset; advances the offset. */
static uint8_t read_one(LoongemuFwCfg *cfg)
{
    uint8_t item[56];
    uint32_t length;
    uint16_t sel = cfg->selector & 0x7fff;
    uint32_t off = cfg->offset++;

    if (sel == FW_CFG_FILE_DIR_SELECT) {
        static uint8_t dir[4 + LOONGEMU_RISCV_FW_CFG_MAX_FILES *
                            (8 + LOONGEMU_RISCV_FW_CFG_NAME_MAX)];
        length = file_dir_bytes(cfg, dir, sizeof(dir));
        return off < length ? dir[off] : 0;
    }
    const LoongemuFwCfgFile *file = find_file(cfg, sel);
    if (file != 0) {
        return off < file->size ? file->data[off] : 0;
    }
    length = item_bytes(cfg, sel, item);
    return off < length ? item[off] : 0;
}

static void dma_transfer(LoongemuFwCfg *cfg, uint64_t desc_gpa,
                         const LoongemuFwCfgDmaOps *ops, uint32_t *control)
{
    uint8_t desc[16];

    if (ops == 0 || ops->read == 0 ||
        ops->read(ops->opaque, desc_gpa, desc, sizeof(desc)) != 0) {
        *control |= FW_CFG_DMA_CTL_ERROR;
        return;
    }
    uint32_t ctl = get_le32(desc);
    uint32_t length = get_le32(desc + 4);
    uint64_t address = get_le64(desc + 8);
    int failed = 0;

    if (ctl & FW_CFG_DMA_CTL_SELECT) {
        cfg->selector = (uint16_t)length;
        cfg->offset = 0;
    } else if (ctl & FW_CFG_DMA_CTL_SKIP) {
        cfg->offset += length;
    } else if (ctl & FW_CFG_DMA_CTL_READ) {
        uint8_t chunk[64];
        uint32_t done = 0;
        while (done < length) {
            uint32_t step = length - done;
            if (step > sizeof(chunk)) {
                step = sizeof(chunk);
            }
            for (uint32_t i = 0; i < step; i++) {
                chunk[i] = read_one(cfg);
            }
            if (ops->write == 0 ||
                ops->write(ops->opaque, address + done, chunk, step) != 0) {
                failed = 1;
                break;
            }
            done += step;
        }
    } else if (ctl & FW_CFG_DMA_CTL_WRITE) {
        /* Externally provided fw_cfg items are read-only. */
        failed = 1;
    } else {
        failed = 1;
    }

    if (failed) {
        *control = ctl | FW_CFG_DMA_CTL_ERROR;
    } else {
        *control = 0;
    }
    put_le32(desc, *control);
    if (ops->write != 0) {
        ops->write(ops->opaque, desc_gpa, desc, 4);
    }
}

int loongemu_fw_cfg_dma(LoongemuFwCfg *cfg, uint64_t desc_gpa,
                        const LoongemuFwCfgDmaOps *ops)
{
    uint8_t desc[16];
    uint32_t control = 0;

    if (cfg == 0 || ops == 0 || ops->read == 0 ||
        ops->read(ops->opaque, desc_gpa, desc, sizeof(desc)) != 0) {
        return -1;
    }
    control = get_le32(desc);
    dma_transfer(cfg, desc_gpa, ops, &control);
    return (control & FW_CFG_DMA_CTL_ERROR) ? -1 : 0;
}

uint64_t loongemu_fw_cfg_mmio_read(LoongemuFwCfg *cfg, uint32_t offset,
                                   unsigned size)
{
    if (cfg == 0 || size == 0 || size > 8) {
        return ~UINT64_C(0);
    }
    if (offset == 8) {
        /* Selector register, big-endian on MMIO. */
        return size == 2 ? (uint64_t)(((cfg->selector & 0xff) << 8) |
                                      ((cfg->selector >> 8) & 0xff))
                         : (uint64_t)(size == 1 ? (cfg->selector >> 8) & 0xff
                                                : ~UINT64_C(0));
    }
    if (offset >= 16) {
        if (size == 8) {
            return UINT64_C(0x51454d5520434647); /* "QEMU CFG" probe */
        }
        return ~UINT64_C(0);
    }
    if (offset + size > 8) {
        return ~UINT64_C(0);
    }
    uint64_t value = 0;
    for (unsigned i = 0; i < size; i++) {
        value = (value << 8) | read_one(cfg);
    }
    return value;
}

void loongemu_fw_cfg_mmio_write(LoongemuFwCfg *cfg, uint32_t offset,
                                uint64_t value, unsigned size,
                                const LoongemuFwCfgDmaOps *ops)
{
    if (cfg == 0 || size == 0) {
        return;
    }
    if (offset == 8) {
        cfg->selector = (uint16_t)(size == 2
            ? (((value >> 8) & 0xff) | ((value & 0xff) << 8))
            : (value & 0xffff));
        cfg->offset = 0;
        return;
    }
    if (offset >= 16) {
        /* Writing the DMA descriptor address starts a transfer. */
        if (size == 8) {
            uint32_t control = 0;
            dma_transfer(cfg, value, ops, &control);
        }
        return;
    }
    /* Writes to the data register are ignored (read-only items). */
}

int loongemu_riscv_pflash_init(LoongemuRiscvPflash *pf, uint8_t *code_buf,
                               uint64_t code_size, uint8_t *vars_buf,
                               uint64_t vars_size, uint64_t code_gpa,
                               uint64_t vars_gpa)
{
    if (pf == 0 || code_buf == 0 || vars_buf == 0 || code_size == 0 ||
        vars_size == 0) {
        return 0;
    }
    memset(pf, 0, sizeof(*pf));
    pf->code.gpa_base = code_gpa;
    pf->code.size = code_size;
    pf->code.storage = code_buf;
    pf->code.read_only = 1;
    pf->vars.gpa_base = vars_gpa;
    pf->vars.size = vars_size;
    pf->vars.storage = vars_buf;
    pf->vars.read_only = 0;
    return 1;
}

int loongemu_riscv_pflash_map(const LoongemuRiscvPflash *pf,
                              LoongemuMemMap *map)
{
    if (pf == 0 || map == 0) {
        return 0;
    }
    /* CFI reads (0x98) must reach the code image, so both slots are MMIO. */
    if (!loongemu_mem_map_add(map, pf->code.gpa_base, pf->code.size,
                              pf->code.gpa_base, LOONGEMU_MEM_MMIO,
                              "riscv-code")) {
        return 0;
    }
    if (!loongemu_mem_map_add(map, pf->vars.gpa_base, pf->vars.size,
                              pf->vars.gpa_base, LOONGEMU_MEM_MMIO,
                              "riscv-vars")) {
        return 0;
    }
    return 1;
}
