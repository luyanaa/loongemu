#ifndef LOONGEMU_RISCV_FW_H
#define LOONGEMU_RISCV_FW_H

#include <stdint.h>
#include "loongemu/phys_mem.h"
#include "loongemu/ovmf_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RISC-V firmware bridge: a memory-mapped QEMU fw_cfg device (the interface
 * RISC-V virt firmware uses for ACPI tables, EDK2 files and the ACPI RSDP)
 * plus the pflash slot helper for RISCV_VIRT_CODE/VARS style images.
 *
 * fw_cfg MMIO register layout (QEMU docs/specs/fw_cfg.rst):
 *   base + 0  : data     (64-bit, big-endian on MMIO)
 *   base + 8  : selector (16-bit, big-endian)
 *   base + 16 : DMA descriptor address (64-bit, big-endian)
 */

#define LOONGEMU_RISCV_FW_CFG_MAX_FILES 16
#define LOONGEMU_RISCV_FW_CFG_NAME_MAX  56

#define FW_CFG_FILE_DIR_SELECT 0x0019u
#define FW_CFG_FILE_FIRST_SELECT 0x0020u

typedef struct LoongemuFwCfgFile {
    char name[LOONGEMU_RISCV_FW_CFG_NAME_MAX];
    uint32_t size;
    uint16_t select;
    const uint8_t *data;
} LoongemuFwCfgFile;

/* Guest-memory access for the DMA descriptor and payload. */
typedef struct LoongemuFwCfgDmaOps {
    int (*read)(void *opaque, uint64_t gpa, void *buf, uint32_t len);
    int (*write)(void *opaque, uint64_t gpa, const void *buf, uint32_t len);
    void *opaque;
} LoongemuFwCfgDmaOps;

typedef struct LoongemuFwCfg {
    uint16_t selector;
    uint32_t offset;
    uint16_t nb_cpus;
    uint16_t max_cpus;
    uint32_t file_count;
    LoongemuFwCfgFile files[LOONGEMU_RISCV_FW_CFG_MAX_FILES];
} LoongemuFwCfg;

void loongemu_fw_cfg_init(LoongemuFwCfg *cfg, uint16_t nb_cpus,
                          uint16_t max_cpus);

/* Registers a read-only file; returns its selector or 0 on error. */
uint16_t loongemu_fw_cfg_add_file(LoongemuFwCfg *cfg, const char *name,
                                  const uint8_t *data, uint32_t size);

/* fw_cfg DMA: executes the 16-byte descriptor at `desc_gpa`. Returns 0 on
 * success (the descriptor control word is updated). */
int loongemu_fw_cfg_dma(LoongemuFwCfg *cfg, uint64_t desc_gpa,
                        const LoongemuFwCfgDmaOps *ops);

/* MMIO accessors; `size` is 1, 2, 4 or 8. Unmapped offsets read as ~0 and
 * writes are ignored. */
uint64_t loongemu_fw_cfg_mmio_read(LoongemuFwCfg *cfg, uint32_t offset,
                                   unsigned size);
void loongemu_fw_cfg_mmio_write(LoongemuFwCfg *cfg, uint32_t offset,
                                uint64_t value, unsigned size,
                                const LoongemuFwCfgDmaOps *ops);

/*
 * Two pflash slots for RISC-V virt style firmware. The code image is mapped
 * as ROM (read-only, executable) and the vars image as MMIO so its CFI
 * command state can be serviced by ovmf_pflash_mmio_read/write.
 */
typedef struct LoongemuRiscvPflash {
    OvmfPflash code;
    OvmfPflash vars;
} LoongemuRiscvPflash;

#define LOONGEMU_RISCV_PFLASH_CODE_GPA UINT64_C(0x20000000)
#define LOONGEMU_RISCV_PFLASH_VARS_GPA UINT64_C(0x22000000)

int loongemu_riscv_pflash_init(LoongemuRiscvPflash *pf, uint8_t *code_buf,
                               uint64_t code_size, uint8_t *vars_buf,
                               uint64_t vars_size, uint64_t code_gpa,
                               uint64_t vars_gpa);
int loongemu_riscv_pflash_map(const LoongemuRiscvPflash *pf,
                              LoongemuMemMap *map);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_RISCV_FW_H */
