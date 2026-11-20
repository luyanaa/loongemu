#ifndef LOONGEMU_OVMF_BOARD_H
#define LOONGEMU_OVMF_BOARD_H

#include <stddef.h>
#include <stdint.h>

#include "loongemu/phys_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard PC/QEMU I/O ports required by OVMF SEC/PEI/DXE */
#define LOONGEMU_CMOS_INDEX_PORT    0x0070u
#define LOONGEMU_CMOS_DATA_PORT     0x0071u
#define LOONGEMU_FW_CFG_SEL_PORT    0x0510u
#define LOONGEMU_FW_CFG_DATA_PORT   0x0511u
#define LOONGEMU_FW_CFG_DMA_HI_PORT 0x0514u
#define LOONGEMU_FW_CFG_DMA_LO_PORT 0x0518u

/* Standard QEMU fw_cfg selectors */
#define FW_CFG_SIGNATURE            0x0000u
#define FW_CFG_ID                   0x0001u
#define FW_CFG_UUID                 0x0002u
#define FW_CFG_RAM_SIZE             0x0003u
#define FW_CFG_NOGRAPHIC            0x0004u
#define FW_CFG_NB_CPUS              0x0005u
#define FW_CFG_MAX_CPUS             0x000fu
#define FW_CFG_FILE_DIR             0x0019u
#define FW_CFG_FILE_FIRST           0x0020u

/* fw_cfg DMA control flags */
#define FW_CFG_DMA_CTL_ERROR        0x01u
#define FW_CFG_DMA_CTL_READ         0x02u
#define FW_CFG_DMA_CTL_SKIP         0x04u
#define FW_CFG_DMA_CTL_SELECT       0x08u
#define FW_CFG_DMA_CTL_WRITE        0x10u

/* Safety budget caps for synchronous fw_cfg DMA transfers */
#define LOONGEMU_FW_CFG_DMA_MAX_LEN  (1u * 1024u * 1024u)
#define LOONGEMU_FW_CFG_DMA_CHUNK_SZ 4096u

/* Default OVMF 4 GiB top-of-32-bit ROM / pflash layout */
#define LOONGEMU_OVMF_TOP_GPA         UINT64_C(0x100000000)
#define LOONGEMU_OVMF_DEFAULT_ROM_SZ  (64u * 1024u)
#define LOONGEMU_OVMF_DEFAULT_VARS_SZ (64u * 1024u)
#define LOONGEMU_OVMF_MAX_FILES       8u

/* Checked guest-physical accessors: return 1 on success, 0 on fault/OOB/RO. */
typedef int (*OvmfPhysReadCheckedFn)(uint64_t gpa, int size, uint64_t *out);
typedef int (*OvmfPhysWriteCheckedFn)(uint64_t gpa, int size, uint64_t value);

/* Optional live virtual wall-clock provider (returns UNIX seconds since 1970). */
typedef uint64_t (*OvmfWallTimeSecFn)(void *opaque);

typedef struct OvmfFwCfgFile {
    uint16_t selector;
    uint32_t size;
    char name[56];
    const uint8_t *data;
} OvmfFwCfgFile;

typedef struct OvmfPflash {
    uint64_t gpa_base;
    uint64_t size;
    uint8_t *storage;
    uint8_t cmd;
    uint8_t status;
    uint8_t write_cycle;
    uint8_t read_only;
} OvmfPflash;

typedef struct OvmfBoard {
    uint64_t ram_size;
    uint32_t nr_cpus;

    /* CMOS / RTC state */
    uint8_t cmos_index;
    uint8_t cmos[128];
    OvmfWallTimeSecFn wall_time_fn;
    void *wall_time_opaque;

    /* QEMU fw_cfg state */
    uint16_t fw_cfg_sel;
    uint32_t fw_cfg_off;
    uint64_t fw_cfg_dma_addr;
    uint32_t file_count;
    OvmfFwCfgFile files[LOONGEMU_OVMF_MAX_FILES];
    uint8_t e820_buf[60]; /* 3 * 20-byte e820 entries */
    uint32_t e820_len;
    OvmfPhysReadCheckedFn phys_read_checked;
    OvmfPhysWriteCheckedFn phys_write_checked;

    /* High 4 GiB OVMF_CODE.fd ROM & OVMF_VARS.fd CFI pflash */
    uint64_t rom_gpa_base;
    uint64_t rom_size;
    const uint8_t *rom_data;
    OvmfPflash vars_flash;
} OvmfBoard;

void ovmf_board_init(OvmfBoard *b, uint64_t ram_size, uint32_t nr_cpus,
                     OvmfPhysReadCheckedFn phys_read_checked,
                     OvmfPhysWriteCheckedFn phys_write_checked);

/* Attach a unified virtual wall-clock source for live CMOS/RTC calendar reads. */
void ovmf_board_set_wall_clock(OvmfBoard *b, OvmfWallTimeSecFn fn, void *opaque);

/* Register a named file in fw_cfg (returns selector >= 0x20, or 0 on error). */
uint16_t ovmf_board_add_file(OvmfBoard *b, const char *name,
                             const uint8_t *data, uint32_t size);

/*
 * Map OVMF_CODE.fd at [4GiB - rom_size, 4GiB) as LOONGEMU_MEM_ROM and
 * OVMF_VARS.fd right below it as LOONGEMU_MEM_MMIO (CFI pflash).
 */
int ovmf_board_map_firmware(OvmfBoard *b, LoongemuMemMap *mem_map,
                            const uint8_t *rom_buf, uint64_t rom_size,
                            uint8_t *vars_buf, uint64_t vars_size);

/* I/O port dispatch (returns 1 if port was claimed by CMOS or fw_cfg). */
int ovmf_board_io_read(OvmfBoard *b, uint32_t port, int size, uint64_t *out);
int ovmf_board_io_write(OvmfBoard *b, uint32_t port, int size, uint64_t value);

/* CFI pflash MMIO callbacks (compatible with tb_sys.c mmio_register). */
uint64_t ovmf_pflash_mmio_read(void *opaque, uint64_t offset, unsigned size);
void ovmf_pflash_mmio_write(void *opaque, uint64_t offset, uint64_t val, unsigned size);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_OVMF_BOARD_H */
