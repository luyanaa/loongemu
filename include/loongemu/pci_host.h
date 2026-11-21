#ifndef LOONGEMU_PCI_HOST_H
#define LOONGEMU_PCI_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "loongemu/phys_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard x86 PCI Configuration Mechanism #1 I/O ports */
#define LOONGEMU_PCI_CFG_ADDR_PORT   0x0cf8u
#define LOONGEMU_PCI_CFG_DATA_PORT   0x0cfcu

/* Standard contiguous PCIe ECAM window presented to x86 Guest */
#define LOONGEMU_PCI_ECAM_GPA        UINT64_C(0xb0000000)
#define LOONGEMU_PCI_ECAM_SIZE       UINT64_C(0x01000000) /* Buses 0..15 (16 MiB) */

/* Bochs VBE DISPI legacy I/O ports + QEMU debugcon */
#define LOONGEMU_BOCHS_DISPI_INDEX   0x01ceu
#define LOONGEMU_BOCHS_DISPI_DATA    0x01cfu
#define LOONGEMU_BOCHS_DISPI_DATA2   0x01d0u
#define LOONGEMU_QEMU_DEBUGCON_PORT  0x0402u

/* Default guest MMIO BAR placements for Bochs Display (relocatable by OVMF) */
#define LOONGEMU_BOCHS_FB_DEFAULT_GPA   UINT64_C(0x80000000)
#define LOONGEMU_BOCHS_FB_DEFAULT_SIZE  UINT64_C(0x01000000) /* 16 MiB */
#define LOONGEMU_BOCHS_MMIO_DEFAULT_GPA UINT64_C(0x81000000)
#define LOONGEMU_BOCHS_MMIO_SIZE        UINT64_C(0x00001000) /* 4 KiB */

/* Loongson 7A1000 / 7A2000 HT split-CFG physical bases (node 0) */
#define LOONGSON_7A_CFG_TYPE0_BASE   UINT64_C(0x00000efe00000000)
#define LOONGSON_7A_CFG_TYPE1_BASE   UINT64_C(0x00000efe10000000)
#define LOONGSON_VENDOR_ID           0x0014u
#define LOONGSON_7A1000_DC_DID       0x7a06u
#define LOONGSON_7A2000_DC_DID       0x7a36u
#define LOONGSON_7A1000_GPU_DID      0x7a15u
#define LOONGSON_7A2000_GPU_DID      0x7a25u

#define LOONGEMU_PCI_MAX_DEVS        16u
#define LOONGEMU_PCI_NUM_BARS        6u

/* Host LoongArch UEFI GOP handover descriptor */
typedef struct LoongemuGopHandover {
    uint64_t fb_hpa;        /* Host physical address of linear framebuffer */
    uint64_t fb_size;       /* Total usable framebuffer size in bytes */
    uint32_t width;         /* Horizontal resolution in pixels */
    uint32_t height;        /* Vertical resolution in pixels */
    uint32_t stride_px;     /* PixelsPerScanLine */
    uint32_t bpp;           /* Bits per pixel (32 for BGRX/RGBX) */
    uint32_t pixel_format;  /* 0=RGBX, 1=BGRX (matches EFI_GRAPHICS_PIXEL_FORMAT) */
    uint32_t valid;         /* Non-zero if backed by real Host UEFI GOP */
    uint8_t  owner;         /* LoongemuFbOwner */
    uint8_t  host_mat;      /* LoongemuHostMat: HOST_GOP -> WUC/SUC, VMM_ALLOC -> CC */
    uint8_t  reserved[6];
} LoongemuGopHandover;

typedef int (*LoongemuPciBarMapFn)(void *opaque, uint32_t bar_idx,
                                   uint64_t new_gpa, uint64_t size, int enabled);

typedef struct LoongemuPciDev {
    uint8_t  used;
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  fn;
    uint8_t  cfg[256];
    uint32_t bar_size[LOONGEMU_PCI_NUM_BARS];
    uint32_t bar_flags[LOONGEMU_PCI_NUM_BARS]; /* e.g. 0x08 prefetchable */
    uint64_t bar_gpa[LOONGEMU_PCI_NUM_BARS];
    LoongemuPciBarMapFn bar_map_cb;
    void *bar_opaque;
} LoongemuPciDev;

/* Bochs Display (1234:1111) backed by Host UEFI GOP handover */
typedef struct LoongemuBochsDisplay {
    LoongemuGopHandover gop;
    uint16_t dispi_index;
    uint16_t dispi_regs[16];
    uint8_t  vga_regs[32];
    uint8_t  edid[128];
    uint64_t fb_gpa;
    uint64_t mmio_gpa;
    uint64_t fb_mapped_size; /* real backing mapped in host physical memory */
    int      fb_mapped;
    int      mmio_mapped;
} LoongemuBochsDisplay;

/* Must issue one naturally aligned native config cycle (size 1, 2, or 4). */
typedef void (*LoongemuLs7aConfigWriteFn)(uint64_t phys_addr,
                                          uint32_t val, unsigned size);

/* Loongson 7A1000 / 7A2000 physical PCIe passthrough backend */
typedef struct LoongemuLs7aBackend {
    uint64_t cfg_type0_base; /* e.g. 0xefe00000000 or ACPI MCFG base */
    uint64_t cfg_type1_base; /* e.g. 0xefe10000000 */
    int      enabled;
    int      hide_host_dc_gpu; /* Legacy flag; known DC/GPU IDs are always hidden */
    uint32_t (*hw_read32)(uint64_t phys_addr);
    LoongemuLs7aConfigWriteFn hw_write_config;
} LoongemuLs7aBackend;

typedef struct LoongemuPciHost {
    uint32_t cfg_addr; /* Latch for 0xCF8 */
    uint32_t dev_count;
    LoongemuPciDev devs[LOONGEMU_PCI_MAX_DEVS];
    LoongemuBochsDisplay display;
    LoongemuLs7aBackend ls7a;
} LoongemuPciHost;

/* Initialize i440FX (00:00.0), PIIX3 ISA (00:01.0), PIIX4 PM (00:01.3),
 * and Bochs Display (00:02.0) with optional Host UEFI GOP handover. */
void loongemu_pci_host_init(LoongemuPciHost *host,
                            const LoongemuGopHandover *gop,
                            LoongemuPciBarMapFn disp_bar_cb,
                            void *disp_bar_opaque);

/* Configure Loongson 7A1000/7A2000 HT split-CFG passthrough backend. */
/* A NULL write callback leaves the physical config backend read-only. */
void loongemu_pci_host_attach_ls7a(LoongemuPciHost *host,
                                   uint64_t cfg_type0_base,
                                   uint64_t cfg_type1_base,
                                   int hide_host_dc_gpu,
                                   uint32_t (*hw_read32)(uint64_t),
                                   LoongemuLs7aConfigWriteFn hw_write_config);

/* Compute Loongson 7A1000/7A2000 HT split-CFG physical address (split [27:24]|[7:0]). */
uint64_t loongemu_ls7a_cfg_addr(uint64_t cfg_t0_base, uint64_t cfg_t1_base,
                                uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg);

/* Returns 1 if vendor/device is a 7A1000/7A2000 internal Display Controller or GPU. */
int loongemu_ls7a_is_display_or_gpu(uint16_t vendor_id, uint16_t device_id);

/* Returns 1 if device is a 7A internal PCIe Root Port subject to FLAG_DEV_FIX. */
int loongemu_ls7a_is_internal_bridge(uint16_t vendor_id, uint16_t device_id);

/* Direct config space read/write (1, 2, or 4 bytes). */
uint32_t loongemu_pci_cfg_read(LoongemuPciHost *host,
                               uint8_t bus, uint8_t dev, uint8_t fn,
                               uint16_t reg, unsigned size);
void loongemu_pci_cfg_write(LoongemuPciHost *host,
                            uint8_t bus, uint8_t dev, uint8_t fn,
                            uint16_t reg, uint32_t val, unsigned size);

/* I/O port dispatch: claims 0xCF8..0xCFF, 0x1CE..0x1D0, 0x3C0..0x3DF. */
int loongemu_pci_io_read(LoongemuPciHost *host, uint32_t port, int size, uint64_t *out);
int loongemu_pci_io_write(LoongemuPciHost *host, uint32_t port, int size, uint64_t val);

/* ECAM MMIO callbacks (register at LOONGEMU_PCI_ECAM_GPA). */
uint64_t loongemu_pci_ecam_read(void *opaque, uint64_t offset, unsigned size);
void loongemu_pci_ecam_write(void *opaque, uint64_t offset, uint64_t val, unsigned size);

/* Bochs Display BAR2 MMIO callbacks (EDID + VGA + DISPI + QEMU ext regs). */
uint64_t loongemu_bochs_mmio_read(void *opaque, uint64_t offset, unsigned size);
void loongemu_bochs_mmio_write(void *opaque, uint64_t offset, uint64_t val, unsigned size);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_PCI_HOST_H */
