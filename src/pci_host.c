#include "loongemu/pci_host.h"
#include "loongemu/acpi_tables.h"

#include <string.h>

/* Bochs VBE DISPI register indices */
#define VBE_DISPI_INDEX_ID              0x0u
#define VBE_DISPI_INDEX_XRES            0x1u
#define VBE_DISPI_INDEX_YRES            0x2u
#define VBE_DISPI_INDEX_BPP             0x3u
#define VBE_DISPI_INDEX_ENABLE          0x4u
#define VBE_DISPI_INDEX_BANK            0x5u
#define VBE_DISPI_INDEX_VIRT_WIDTH      0x6u
#define VBE_DISPI_INDEX_VIRT_HEIGHT     0x7u
#define VBE_DISPI_INDEX_X_OFFSET        0x8u
#define VBE_DISPI_INDEX_Y_OFFSET        0x9u
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 0xau

#define VBE_DISPI_ID5                   0xb0c5u

static inline void wr16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static inline void wr32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

/* Build a valid 128-byte EDID 1.4 block advertising the Host GOP native mode. */
static void build_gop_edid(uint8_t edid[128], uint32_t width, uint32_t height)
{
    memset(edid, 0, 128);
    if (width < 640u || width > 4095u) {
        width = 1024u;
    }
    if (height < 480u || height > 4095u) {
        height = 768u;
    }
    /* Header: 00 FF FF FF FF FF FF 00 */
    edid[0] = 0x00u;
    for (int i = 1; i <= 6; i++) {
        edid[i] = 0xffu;
    }
    edid[7] = 0x00u;
    /* Manufacturer "LGN" (LoongEmu), Product 0x0001, Year 2026 */
    edid[8] = 0x30u;
    edid[9] = 0xeeu;
    edid[10] = 0x01u;
    edid[11] = 0x00u;
    edid[16] = 1u;
    edid[17] = 36u; /* 1990 + 36 = 2026 */
    /* EDID 1.4 */
    edid[18] = 1u;
    edid[19] = 4u;
    /* Digital input, 8 bpc, RGB 4:4:4 */
    edid[20] = 0xa5u;
    edid[21] = 34u; /* 34 cm */
    edid[22] = 27u; /* 27 cm */
    edid[23] = 120u; /* gamma 2.2 */
    edid[24] = 0x0au; /* sRGB + preferred timing */

    /* Detailed Timing Descriptor #1 at offset 54 (18 bytes) */
    uint32_t hblank = 160u;
    uint32_t vblank = 30u;
    uint32_t pclk_10khz = ((width + hblank) * (height + vblank) * 60u) / 10000u;
    if (pclk_10khz == 0u) {
        pclk_10khz = 6500u;
    }
    edid[54] = (uint8_t)(pclk_10khz & 0xffu);
    edid[55] = (uint8_t)((pclk_10khz >> 8) & 0xffu);
    edid[56] = (uint8_t)(width & 0xffu);
    edid[57] = (uint8_t)(hblank & 0xffu);
    edid[58] = (uint8_t)(((width >> 8) & 0x0fu) << 4) | (uint8_t)((hblank >> 8) & 0x0fu);
    edid[59] = (uint8_t)(height & 0xffu);
    edid[60] = (uint8_t)(vblank & 0xffu);
    edid[61] = (uint8_t)(((height >> 8) & 0x0fu) << 4) | (uint8_t)((vblank >> 8) & 0x0fu);
    edid[62] = 48u;  /* hsync offset */
    edid[63] = 32u;  /* hsync pulse */
    edid[64] = 0x35u; /* vsync offset=3, pulse=5 */
    edid[71] = 0x18u; /* digital separate sync */

    /* Monitor Name descriptor at offset 72 */
    edid[75] = 0xfcu;
    memcpy(&edid[77], "LoongEmu GOP\n", 13);

    uint8_t sum = 0;
    for (int i = 0; i < 127; i++) {
        sum = (uint8_t)(sum + edid[i]);
    }
    edid[127] = (uint8_t)(0u - sum);
}

static LoongemuPciDev *pci_add_dev(LoongemuPciHost *host,
                                   uint8_t bus, uint8_t dev, uint8_t fn,
                                   uint16_t vendor, uint16_t device,
                                   uint8_t rev, uint8_t prog_if,
                                   uint8_t sub_cls, uint8_t base_cls,
                                   uint8_t hdr_type)
{
    if (!host || host->dev_count >= LOONGEMU_PCI_MAX_DEVS) {
        return 0;
    }
    LoongemuPciDev *d = &host->devs[host->dev_count++];
    memset(d, 0, sizeof(*d));
    d->used = 1u;
    d->bus = bus;
    d->dev = dev;
    d->fn = fn;
    wr16_le(&d->cfg[0x00], vendor);
    wr16_le(&d->cfg[0x02], device);
    wr16_le(&d->cfg[0x04], 0x0007u); /* IO + MEM + BusMaster */
    wr16_le(&d->cfg[0x06], 0x0000u);
    d->cfg[0x08] = rev;
    d->cfg[0x09] = prog_if;
    d->cfg[0x0a] = sub_cls;
    d->cfg[0x0b] = base_cls;
    d->cfg[0x0e] = hdr_type;
    return d;
}

static LoongemuPciDev *pci_find_dev(LoongemuPciHost *host,
                                    uint8_t bus, uint8_t dev, uint8_t fn)
{
    if (!host) {
        return 0;
    }
    for (uint32_t i = 0; i < host->dev_count; i++) {
        LoongemuPciDev *d = &host->devs[i];
        if (d->used && d->bus == bus && d->dev == dev && d->fn == fn) {
            return d;
        }
    }
    return 0;
}
static void bochs_sync_fb_capacity(LoongemuPciHost *host,
                                   LoongemuPciDev *dev)
{
    uint16_t vendor;
    uint16_t device;
    uint64_t mem64k;

    if (!host || !dev) {
        return;
    }
    vendor = (uint16_t)dev->cfg[0x00] | ((uint16_t)dev->cfg[0x01] << 8);
    device = (uint16_t)dev->cfg[0x02] | ((uint16_t)dev->cfg[0x03] << 8);
    if (vendor != 0x1234u || device != 0x1111u) {
        return;
    }
    mem64k = host->display.fb_mapped_size / UINT64_C(65536);
    if (mem64k > UINT16_MAX) {
        mem64k = UINT16_MAX;
    }
    host->display.dispi_regs[VBE_DISPI_INDEX_VIDEO_MEMORY_64K] =
        (uint16_t)mem64k;
}
static int pci_call_bar_map(LoongemuPciHost *host, LoongemuPciDev *dev,
                            uint32_t bar_idx, uint64_t gpa,
                            uint64_t size, int enabled)
{
    if (!dev || !dev->bar_map_cb ||
        !dev->bar_map_cb(dev->bar_opaque, bar_idx, gpa, size, enabled)) {
        return !dev || !dev->bar_map_cb;
    }
    if (bar_idx == 0u) {
        bochs_sync_fb_capacity(host, dev);
    }
    return 1;
}

static uint64_t bochs_fb_capacity(const LoongemuBochsDisplay *display)
{
    if (!display) {
        return 0;
    }
    return display->fb_mapped_size & ~UINT64_C(0xfff);
}



static void pci_set_bar(LoongemuPciDev *d, uint32_t idx,
                        uint64_t gpa, uint32_t size, uint32_t flags)
{
    if (!d || idx >= LOONGEMU_PCI_NUM_BARS) {
        return;
    }
    d->bar_size[idx] = size;
    d->bar_flags[idx] = flags & 0x0fu;
    d->bar_gpa[idx] = gpa;
    wr32_le(&d->cfg[0x10 + idx * 4u],
            ((uint32_t)gpa & ~(size - 1u)) | d->bar_flags[idx]);
}

void loongemu_pci_host_init(LoongemuPciHost *host,
                            const LoongemuGopHandover *gop,
                            LoongemuPciBarMapFn disp_bar_cb,
                            void *disp_bar_opaque)
{
    if (!host) {
        return;
    }
    memset(host, 0, sizeof(*host));

    /* 00:00.0 Intel 440FX PMC (8086:1237) - probed by OVMF PlatformInitLib */
    LoongemuPciDev *pmc = pci_add_dev(host, 0, 0, 0,
                                      0x8086u, 0x1237u, 0x02u,
                                      0x00u, 0x00u, 0x06u, 0x00u);
    if (pmc) {
        wr16_le(&pmc->cfg[0x2c], 0x1af4u);
        wr16_le(&pmc->cfg[0x2e], 0x1100u);
        /* PAM0..PAM6 (0x59..0x5f): default readable/writable RAM */
        pmc->cfg[0x59] = 0x30u;
        for (int i = 0x5a; i <= 0x5f; i++) {
            pmc->cfg[i] = 0x33u;
        }
    }

    /* 00:01.0 Intel PIIX3 ISA Bridge (8086:7000, multi-function) */
    LoongemuPciDev *isa = pci_add_dev(host, 0, 1, 0,
                                      0x8086u, 0x7000u, 0x00u,
                                      0x00u, 0x01u, 0x06u, 0x80u);
    if (isa) {
        isa->cfg[0x60] = 0x0bu; /* PIRQA -> IRQ 11 */
        isa->cfg[0x61] = 0x0au; /* PIRQB -> IRQ 10 */
        isa->cfg[0x62] = 0x0bu; /* PIRQC -> IRQ 11 */
        isa->cfg[0x63] = 0x0au; /* PIRQD -> IRQ 10 */
    }

    /* 00:01.3 Intel PIIX4 ACPI Power Management (8086:7113)
     * OVMF AcpiTimerLib reads reg 0x40 (PMBA) and reg 0x80 (PMIOSE). */
    LoongemuPciDev *pm = pci_add_dev(host, 0, 1, 3,
                                     0x8086u, 0x7113u, 0x03u,
                                     0x00u, 0x80u, 0x06u, 0x00u);
    if (pm) {
        wr32_le(&pm->cfg[0x40], (uint32_t)LOONGEMU_ACPI_PM1A_EVT_PORT | 1u);
        pm->cfg[0x80] = 0x01u; /* PMIOSE = 1 */
        pm->cfg[0x3c] = 0x09u; /* SCI IRQ 9 */
        pm->cfg[0x3d] = 0x01u; /* INTA */
    }

    /* Configure Bochs Display state from Host UEFI GOP handover */
    LoongemuBochsDisplay *disp = &host->display;
    if (gop && (gop->valid || gop->owner != (uint8_t)LOONGEMU_FB_OWNER_NONE) &&
        gop->width > 0 && gop->height > 0) {
        disp->gop = *gop;
        if (disp->gop.owner == (uint8_t)LOONGEMU_FB_OWNER_NONE) {
            disp->gop.owner = (uint8_t)LOONGEMU_FB_OWNER_HOST_GOP;
        }
        if (disp->gop.owner == (uint8_t)LOONGEMU_FB_OWNER_HOST_GOP &&
            disp->gop.host_mat != (uint8_t)LOONGEMU_MAT_WUC &&
            disp->gop.host_mat != (uint8_t)LOONGEMU_MAT_SUC) {
            disp->gop.host_mat = (uint8_t)LOONGEMU_MAT_WUC;
        } else if (disp->gop.owner == (uint8_t)LOONGEMU_FB_OWNER_VMM_ALLOC &&
                   disp->gop.host_mat > (uint8_t)LOONGEMU_MAT_WUC) {
            disp->gop.host_mat = (uint8_t)LOONGEMU_MAT_CC;
        }
    } else {
        disp->gop.fb_hpa = 0;
        disp->gop.fb_size = LOONGEMU_BOCHS_FB_DEFAULT_SIZE;
        disp->gop.width = 1024u;
        disp->gop.height = 768u;
        disp->gop.stride_px = 1024u;
        disp->gop.bpp = 32u;
        disp->gop.pixel_format = 1u; /* BGRX */
        disp->gop.valid = 0u;
        disp->gop.owner = (uint8_t)LOONGEMU_FB_OWNER_NONE;
        disp->gop.host_mat = (uint8_t)LOONGEMU_MAT_CC;
    }
    uint32_t fb_sz = (uint32_t)LOONGEMU_BOCHS_FB_DEFAULT_SIZE;
    if (disp->gop.fb_size > 0 && gop &&
        (gop->valid || gop->owner != (uint8_t)LOONGEMU_FB_OWNER_NONE)) {
        uint64_t target = disp->gop.fb_size;
        if (target < (UINT64_C(4) * 1024u * 1024u)) {
            target = UINT64_C(4) * 1024u * 1024u;
        }
        if (target > (UINT64_C(64) * 1024u * 1024u)) {
            target = UINT64_C(64) * 1024u * 1024u;
        }
        uint32_t p2 = 4u * 1024u * 1024u;
        while ((uint64_t)p2 < target && p2 < (64u * 1024u * 1024u)) {
            p2 <<= 1;
        }
        fb_sz = p2;
    }
    disp->dispi_regs[VBE_DISPI_INDEX_ID] = VBE_DISPI_ID5;
    disp->dispi_regs[VBE_DISPI_INDEX_XRES] = (uint16_t)disp->gop.width;
    disp->dispi_regs[VBE_DISPI_INDEX_YRES] = (uint16_t)disp->gop.height;
    disp->dispi_regs[VBE_DISPI_INDEX_BPP] = (uint16_t)(disp->gop.bpp ? disp->gop.bpp : 32u);
    disp->dispi_regs[VBE_DISPI_INDEX_ENABLE] = 0x0041u; /* ENABLED | LFB_ENABLED */
    disp->dispi_regs[VBE_DISPI_INDEX_VIRT_WIDTH] = (uint16_t)disp->gop.stride_px;
    disp->dispi_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] = (uint16_t)disp->gop.height;
    {
        uint64_t mapped = 0;
        if (disp->gop.valid && disp->gop.fb_size != 0) {
            mapped = disp->gop.fb_size & ~UINT64_C(0xfff);
        } else {
            mapped = UINT64_C(4) * 1024u * 1024u;
        }
        if (mapped > fb_sz) {
            mapped = fb_sz;
        }
        disp->fb_mapped_size = mapped;
        disp->dispi_regs[VBE_DISPI_INDEX_VIDEO_MEMORY_64K] =
            (uint16_t)(mapped / UINT64_C(65536));
    }
    build_gop_edid(disp->edid, disp->gop.width, disp->gop.height);

    /* 00:02.0 QEMU Standard VGA / Bochs Display (1234:1111, rev 2) */
    LoongemuPciDev *vga = pci_add_dev(host, 0, 2, 0,
                                      0x1234u, 0x1111u, 0x02u,
                                      0x00u, 0x00u, 0x03u, 0x00u);
    if (vga) {
        wr16_le(&vga->cfg[0x2c], 0x1af4u);
        wr16_le(&vga->cfg[0x2e], 0x1100u);
        pci_set_bar(vga, 0, LOONGEMU_BOCHS_FB_DEFAULT_GPA, fb_sz, 0x08u);
        pci_set_bar(vga, 2, LOONGEMU_BOCHS_MMIO_DEFAULT_GPA,
                    (uint32_t)LOONGEMU_BOCHS_MMIO_SIZE, 0x00u);
        vga->bar_map_cb = disp_bar_cb;
        vga->bar_opaque = disp_bar_opaque;
        disp->fb_gpa = LOONGEMU_BOCHS_FB_DEFAULT_GPA;
        disp->mmio_gpa = LOONGEMU_BOCHS_MMIO_DEFAULT_GPA;
        if (disp_bar_cb) {
            if (!disp_bar_cb(disp_bar_opaque, 0, disp->fb_gpa, fb_sz, 1)) {
                disp->fb_mapped_size = 0;
            }
            (void)disp_bar_cb(disp_bar_opaque, 2, disp->mmio_gpa,
                              LOONGEMU_BOCHS_MMIO_SIZE, 1);
            bochs_sync_fb_capacity(host, vga);
        }
    }
}

void loongemu_pci_host_attach_ls7a(LoongemuPciHost *host,
                                   uint64_t cfg_type0_base,
                                   uint64_t cfg_type1_base,
                                   int hide_host_dc_gpu,
                                   uint32_t (*hw_read32)(uint64_t),
                                   LoongemuLs7aConfigWriteFn hw_write_config)
{
    if (!host) {
        return;
    }
    host->ls7a.cfg_type0_base = cfg_type0_base ? cfg_type0_base : LOONGSON_7A_CFG_TYPE0_BASE;
    host->ls7a.cfg_type1_base = cfg_type1_base ? cfg_type1_base : LOONGSON_7A_CFG_TYPE1_BASE;
    host->ls7a.hide_host_dc_gpu = hide_host_dc_gpu ? 1 : 0;
    host->ls7a.hw_read32 = hw_read32;
    host->ls7a.hw_write_config = hw_write_config;
    host->ls7a.enabled = (hw_read32 != 0) ? 1 : 0;
}

/*
 * Loongson 7A1000 / 7A2000 HT Extended PCI Configuration Address:
 *   - Type 0 (bus == 0): cfg_t0_base | ((reg & 0xf00) << 16) | (dev << 11) | (fn << 8) | (reg & 0xff)
 *   - Type 1 (bus > 0):  cfg_t1_base | ((reg & 0xf00) << 16) | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xff)
 * Notice offset[11:8] is split into address bits [27:24] instead of contiguous ECAM!
 */
uint64_t loongemu_ls7a_cfg_addr(uint64_t cfg_t0_base, uint64_t cfg_t1_base,
                                uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg)
{
    uint64_t base = (bus == 0) ? cfg_t0_base : cfg_t1_base;
    uint64_t ext_off = ((uint64_t)(reg & 0x0f00u)) << 16;
    uint64_t bdf = ((uint64_t)bus << 16) | ((uint64_t)(dev & 0x1fu) << 11) |
                   ((uint64_t)(fn & 0x07u) << 8) | ((uint64_t)(reg & 0x00ffu));
    return base | ext_off | bdf;
}

int loongemu_ls7a_is_display_or_gpu(uint16_t vendor_id, uint16_t device_id)
{
    if (vendor_id != LOONGSON_VENDOR_ID) {
        return 0;
    }
    return (device_id == LOONGSON_7A1000_DC_DID ||
            device_id == LOONGSON_7A2000_DC_DID ||
            device_id == LOONGSON_7A1000_GPU_DID ||
            device_id == LOONGSON_7A2000_GPU_DID);
}

int loongemu_ls7a_is_internal_bridge(uint16_t vendor_id, uint16_t device_id)
{
    if (vendor_id != LOONGSON_VENDOR_ID) {
        return 0;
    }
    /* 7A1000/7A2000 internal PCIe Root Ports: 0x7a09, 0x7a19, ..., 0x7a69 */
    return ((device_id & 0xff0fu) == 0x7a09u) && (((device_id >> 4) & 0x0fu) <= 6u);
}

static uint32_t ls7a_read_dw(const LoongemuLs7aBackend *ls7a,
                             uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg)
{
    uint64_t addr = loongemu_ls7a_cfg_addr(ls7a->cfg_type0_base,
                                           ls7a->cfg_type1_base,
                                           bus, dev, fn, (uint16_t)(reg & ~3u));
    return ls7a->hw_read32(addr);
}

/*
 * FLAG_DEV_FIX aliases config reads for nonzero device numbers on a
 * downstream bus. Apply the workaround only when bus 0 contains an actual
 * 7A internal PCIe Root Port whose secondary bus is the requested bus.
 */
static int ls7a_is_phantom_dev(LoongemuPciHost *host, uint8_t bus, uint8_t dev)
{
    const LoongemuLs7aBackend *ls7a = &host->ls7a;
    if (bus == 0u || dev == 0u || !ls7a->hw_read32) {
        return 0;
    }
    for (uint8_t d = 0; d < 32u; d++) {
        for (uint8_t f = 0; f < 8u; f++) {
            if (pci_find_dev(host, 0u, d, f) != 0) {
                if (f == 0u) {
                    break;
                }
                continue;
            }
            uint32_t id = ls7a_read_dw(ls7a, 0u, d, f, 0x00u);
            uint16_t vid = (uint16_t)(id & 0xffffu);
            uint16_t did = (uint16_t)(id >> 16);
            if (vid == 0xffffu) {
                if (f == 0u) {
                    break;
                }
                continue;
            }
            uint32_t bist_hdr = ls7a_read_dw(ls7a, 0u, d, f, 0x0cu);
            uint8_t hdr_type = (uint8_t)(bist_hdr >> 16);
            if (loongemu_ls7a_is_internal_bridge(vid, did) &&
                (hdr_type & 0x7fu) == 1u) {
                uint32_t class_rev = ls7a_read_dw(ls7a, 0u, d, f, 0x08u);
                uint8_t sub_class = (uint8_t)(class_rev >> 16);
                uint8_t base_class = (uint8_t)(class_rev >> 24);
                uint32_t bus_nums = ls7a_read_dw(ls7a, 0u, d, f, 0x18u);
                uint8_t primary_bus = (uint8_t)bus_nums;
                uint8_t secondary_bus = (uint8_t)(bus_nums >> 8);
                uint8_t subordinate_bus = (uint8_t)(bus_nums >> 16);
                if (base_class == 0x06u && sub_class == 0x04u &&
                    primary_bus == 0u && secondary_bus == bus &&
                    subordinate_bus >= secondary_bus) {
                    return 1;
                }
            }
            if (f == 0u && (hdr_type & 0x80u) == 0u) {
                break;
            }
        }
    }
    return 0;
}

static int ls7a_find_pcie_devctl_off(const LoongemuLs7aBackend *ls7a,
                                     uint8_t bus, uint8_t dev, uint8_t fn,
                                     uint16_t *out_off)
{
    if (!ls7a || !ls7a->hw_read32 || !out_off) {
        return 0;
    }
    uint32_t cmd_sts = ls7a_read_dw(ls7a, bus, dev, fn, 0x04u);
    uint16_t status = (uint16_t)(cmd_sts >> 16);
    if ((status & 0x0010u) == 0u) {
        return 0;
    }
    uint32_t cap_reg = ls7a_read_dw(ls7a, bus, dev, fn, 0x34u);
    uint16_t cap_off = (uint16_t)(cap_reg & 0xfcu);
    uint64_t visited = 0;
    for (unsigned hop = 0; hop < 48u && cap_off >= 0x40u; hop++) {
        if (cap_off > 0xfcu) {
            break;
        }
        unsigned bit = cap_off >> 2;
        uint64_t bit_mask = UINT64_C(1) << bit;
        if ((visited & bit_mask) != 0u) {
            break;
        }
        visited |= bit_mask;
        uint32_t cap_hdr = ls7a_read_dw(ls7a, bus, dev, fn, cap_off);
        uint8_t cap_id = (uint8_t)cap_hdr;
        if (cap_id == 0xffu) {
            break;
        }
        if (cap_id == 0x10u) {
            if ((uint32_t)cap_off + 0x0au > 0x100u) {
                return 0;
            }
            *out_off = (uint16_t)(cap_off + 0x08u);
            return 1;
        }
        cap_off = (uint16_t)((cap_hdr >> 8) & 0xfcu);
    }
    return 0;
}

static uint32_t ls7a_read_config(const LoongemuLs7aBackend *ls7a,
                                 uint8_t bus, uint8_t dev, uint8_t fn,
                                 uint16_t reg, unsigned size)
{
    unsigned byte_off = reg & 3u;
    uint64_t value = ls7a_read_dw(ls7a, bus, dev, fn, reg);
    if (byte_off + size > 4u) {
        uint16_t next_reg = (uint16_t)((reg & 0xfffcu) + 4u);
        value |= (uint64_t)ls7a_read_dw(ls7a, bus, dev, fn, next_reg) << 32;
    }
    value >>= byte_off * 8u;
    uint32_t mask = (size == 4u) ? UINT32_MAX : ((1u << (size * 8u)) - 1u);
    return (uint32_t)value & mask;
}

static int ls7a_cfg_byte_is_read_only(uint16_t reg)
{
    return reg < 0x04u || (reg >= 0x08u && reg < 0x0cu) || reg == 0x0eu;
}

/* Use native config cycles so partial writes never replay untouched W1C bits. */
static void ls7a_write_config(const LoongemuLs7aBackend *ls7a,
                              uint8_t bus, uint8_t dev, uint8_t fn,
                              uint16_t reg, uint32_t val, unsigned size,
                              uint16_t devctl_off, int clamp_mrrs)
{
    uint32_t access_end = (uint32_t)reg + size;
    uint32_t byte_reg = reg;
    while (byte_reg < access_end) {
        if (ls7a_cfg_byte_is_read_only((uint16_t)byte_reg)) {
            byte_reg++;
            continue;
        }

        unsigned chunk_size = 1u;
        for (unsigned candidate = 4u; candidate > 1u; candidate >>= 1u) {
            if ((byte_reg & (candidate - 1u)) != 0u ||
                byte_reg + candidate > access_end ||
                (byte_reg & 3u) + candidate > 4u) {
                continue;
            }
            int writable = 1;
            for (unsigned i = 0; i < candidate; i++) {
                if (ls7a_cfg_byte_is_read_only((uint16_t)(byte_reg + i))) {
                    writable = 0;
                    break;
                }
            }
            if (writable) {
                chunk_size = candidate;
                break;
            }
        }

        uint32_t mask = (chunk_size == 4u)
                            ? UINT32_MAX
                            : ((1u << (chunk_size * 8u)) - 1u);
        unsigned value_shift = (unsigned)(byte_reg - reg) * 8u;
        uint32_t chunk_val = (val >> value_shift) & mask;
        if (clamp_mrrs && devctl_off != UINT16_MAX) {
            uint32_t mrrs_byte = (uint32_t)devctl_off + 1u;
            if (mrrs_byte >= byte_reg && mrrs_byte < byte_reg + chunk_size) {
                unsigned mrrs_shift =
                    (unsigned)(mrrs_byte - byte_reg) * 8u + 4u;
                uint32_t mrrs = (chunk_val >> mrrs_shift) & 0x7u;
                if (mrrs > 1u) {
                    chunk_val = (chunk_val & ~(0x7u << mrrs_shift)) |
                                (1u << mrrs_shift);
                }
            }
        }

        uint64_t addr = loongemu_ls7a_cfg_addr(
            ls7a->cfg_type0_base, ls7a->cfg_type1_base,
            bus, dev, fn, (uint16_t)byte_reg);
        ls7a->hw_write_config(addr, chunk_val, chunk_size);
        byte_reg += chunk_size;
    }
}

/* A read-only backend cannot expose a downstream function with MRRS > 256 B. */
static int ls7a_prepare_mrrs(const LoongemuLs7aBackend *ls7a,
                             uint8_t bus, uint8_t dev, uint8_t fn,
                             uint16_t devctl_off)
{
    uint32_t devctl_dw = ls7a_read_dw(ls7a, bus, dev, fn, devctl_off);
    uint32_t mrrs = (devctl_dw >> 12) & 0x7u;
    if (mrrs <= 1u) {
        return 1;
    }
    if (!ls7a->hw_write_config) {
        return 0;
    }
    uint32_t new_control = (devctl_dw & 0x0000ffffu & ~(0x7u << 12)) |
                           (1u << 12);
    uint64_t addr = loongemu_ls7a_cfg_addr(
        ls7a->cfg_type0_base, ls7a->cfg_type1_base,
        bus, dev, fn, devctl_off);
    ls7a->hw_write_config(addr, new_control, 2u);
    return 1;
}


uint32_t loongemu_pci_cfg_read(LoongemuPciHost *host,
                               uint8_t bus, uint8_t dev, uint8_t fn,
                               uint16_t reg, unsigned size)
{
    if (!host || (size != 1u && size != 2u && size != 4u)) {
        return 0xffffffffu;
    }
    LoongemuPciDev *d = pci_find_dev(host, bus, dev, fn);
    if (d) {
        if ((uint32_t)reg + size > 256u) {
            return 0u; /* Standard PCIe extended cap space empty for virtual devices */
        }
        uint32_t val = 0;
        for (unsigned i = 0; i < size; i++) {
            val |= ((uint32_t)d->cfg[reg + i]) << (i * 8u);
        }
        return val;
    }

    /* Optional Loongson 7A1000/7A2000 physical passthrough */
    if (host->ls7a.enabled && host->ls7a.hw_read32) {
        if ((uint32_t)reg + size > 0x1000u ||
            ls7a_is_phantom_dev(host, bus, dev)) {
            return 0xffffffffu;
        }
        uint32_t id = ls7a_read_dw(&host->ls7a, bus, dev, fn, 0x00u);
        uint16_t vid = (uint16_t)(id & 0xffffu);
        uint16_t did = (uint16_t)(id >> 16);
        if (vid == 0xffffu || loongemu_ls7a_is_display_or_gpu(vid, did)) {
            return 0xffffffffu;
        }
        if (bus > 0u) {
            uint16_t devctl_off;
            if (ls7a_find_pcie_devctl_off(&host->ls7a, bus, dev, fn,
                                          &devctl_off) &&
                !ls7a_prepare_mrrs(&host->ls7a, bus, dev, fn, devctl_off)) {
                return 0xffffffffu;
            }
        }
        return ls7a_read_config(&host->ls7a, bus, dev, fn, reg, size);
    }

    return 0xffffffffu;
}

void loongemu_pci_cfg_write(LoongemuPciHost *host,
                            uint8_t bus, uint8_t dev, uint8_t fn,
                            uint16_t reg, uint32_t val, unsigned size)
{
    if (!host || (size != 1u && size != 2u && size != 4u)) {
        return;
    }
    LoongemuPciDev *d = pci_find_dev(host, bus, dev, fn);
    if (d) {
        if ((uint32_t)reg + size > 256u) {
            return;
        }
        /* Keep Vendor/Device ID, Revision, Class, Header Type read-only */
        if (reg < 0x04u || (reg >= 0x08u && reg < 0x0cu) || reg == 0x0eu) {
            return;
        }
        /* BAR sizing & relocation (0x10..0x27) */
        if (reg >= 0x10u && reg < 0x28u && size == 4u && (reg & 3u) == 0u) {
            uint32_t bar_idx = (uint32_t)(reg - 0x10u) >> 2;
            uint32_t bsz = d->bar_size[bar_idx];
            if (bsz == 0u) {
                wr32_le(&d->cfg[reg], 0u);
                return;
            }
            uint32_t mask = ~(bsz - 1u);
            if ((val & mask) == mask) {
                /* PCI BAR size probe (write 0xffffffff -> return ~(size-1) | flags) */
                wr32_le(&d->cfg[reg], mask | d->bar_flags[bar_idx]);
                return;
            }
            uint32_t old_cfg = (uint32_t)d->cfg[reg] |
                               ((uint32_t)d->cfg[reg + 1] << 8) |
                               ((uint32_t)d->cfg[reg + 2] << 16) |
                               ((uint32_t)d->cfg[reg + 3] << 24);
            uint64_t old_base = d->bar_gpa[bar_idx];
            uint32_t new_base = val & mask;
            uint16_t cmd;
            wr32_le(&d->cfg[reg], new_base | d->bar_flags[bar_idx]);
            d->bar_gpa[bar_idx] = (uint64_t)new_base;
            cmd = (uint16_t)d->cfg[0x04] | ((uint16_t)d->cfg[0x05] << 8);
            if (!pci_call_bar_map(host, d, bar_idx, (uint64_t)new_base,
                                  (uint64_t)bsz, (cmd & 0x0002u) != 0)) {
                d->bar_gpa[bar_idx] = old_base;
                wr32_le(&d->cfg[reg], old_cfg);
                bochs_sync_fb_capacity(host, d);
            }
            return;
        }
        /* Normal writable config bytes (Command, Latency, PAM, PMBA, Interrupt Line) */
        uint16_t old_cmd = (uint16_t)d->cfg[0x04] | ((uint16_t)d->cfg[0x05] << 8);
        for (unsigned i = 0; i < size; i++) {
            uint16_t r = (uint16_t)(reg + i);
            if (r < 0x04u || (r >= 0x08u && r < 0x0cu) || r == 0x0eu) {
                continue;
            }
            d->cfg[r] = (uint8_t)((val >> (i * 8u)) & 0xffu);
        }
        if (reg <= 0x05u && d->bar_map_cb) {
            uint16_t new_cmd = (uint16_t)d->cfg[0x04] | ((uint16_t)d->cfg[0x05] << 8);
            if (((old_cmd ^ new_cmd) & 0x0002u) != 0) {
                int ok = 1;
                for (uint32_t b = 0; b < LOONGEMU_PCI_NUM_BARS; b++) {
                    if (d->bar_size[b] != 0u &&
                        !pci_call_bar_map(host, d, b, d->bar_gpa[b],
                                          (uint64_t)d->bar_size[b],
                                          (new_cmd & 0x0002u) != 0)) {
                        ok = 0;
                        break;
                    }
                }
                if (!ok) {
                    wr16_le(&d->cfg[0x04], old_cmd);
                    for (uint32_t b = 0; b < LOONGEMU_PCI_NUM_BARS; b++) {
                        if (d->bar_size[b] != 0u) {
                            (void)pci_call_bar_map(
                                host, d, b, d->bar_gpa[b],
                                (uint64_t)d->bar_size[b],
                                (old_cmd & 0x0002u) != 0);
                        }
                    }
                    bochs_sync_fb_capacity(host, d);
                }
            }
        }
        return;
    }

    if (host->ls7a.enabled && host->ls7a.hw_read32 &&
        host->ls7a.hw_write_config) {
        if ((uint32_t)reg + size > 0x1000u ||
            ls7a_is_phantom_dev(host, bus, dev)) {
            return;
        }
        uint32_t id = ls7a_read_dw(&host->ls7a, bus, dev, fn, 0x00u);
        uint16_t vid = (uint16_t)(id & 0xffffu);
        uint16_t did = (uint16_t)(id >> 16);
        if (vid == 0xffffu || loongemu_ls7a_is_display_or_gpu(vid, did)) {
            return;
        }
        uint16_t devctl_off = UINT16_MAX;
        int has_pcie_cap = 0;
        if (bus > 0u || (reg < 0x100u && (uint32_t)reg + size > 0x48u)) {
            has_pcie_cap = ls7a_find_pcie_devctl_off(
                &host->ls7a, bus, dev, fn, &devctl_off);
        }
        if (bus > 0u && has_pcie_cap &&
            !ls7a_prepare_mrrs(&host->ls7a, bus, dev, fn, devctl_off)) {
            return;
        }
        ls7a_write_config(&host->ls7a, bus, dev, fn, reg, val, size,
                          devctl_off, bus > 0u && has_pcie_cap);
    }
}

int loongemu_pci_io_read(LoongemuPciHost *host, uint32_t port, int size, uint64_t *out)
{
    if (!host || !out) {
        return 0;
    }
    if (port == LOONGEMU_PCI_CFG_ADDR_PORT) {
        *out = host->cfg_addr;
        return 1;
    }
    if (port >= LOONGEMU_PCI_CFG_DATA_PORT && port <= LOONGEMU_PCI_CFG_DATA_PORT + 3u) {
        if ((size != 1 && size != 2 && size != 4) ||
            (unsigned)size > 4u - (port - LOONGEMU_PCI_CFG_DATA_PORT)) {
            *out = 0xffffffffu;
            return 1;
        }
        if ((host->cfg_addr & 0x80000000u) == 0u) {
            *out = 0xffffffffu;
            return 1;
        }
        uint8_t bus = (uint8_t)((host->cfg_addr >> 16) & 0xffu);
        uint8_t dev = (uint8_t)((host->cfg_addr >> 11) & 0x1fu);
        uint8_t fn  = (uint8_t)((host->cfg_addr >> 8) & 0x07u);
        uint16_t reg = (uint16_t)((host->cfg_addr & 0xfcu) + (port - LOONGEMU_PCI_CFG_DATA_PORT));
        *out = loongemu_pci_cfg_read(host, bus, dev, fn, reg, (unsigned)size);
        return 1;
    }
    /* Bochs VBE DISPI I/O ports */
    if (port == LOONGEMU_BOCHS_DISPI_INDEX) {
        *out = host->display.dispi_index;
        return 1;
    }
    if (port == LOONGEMU_BOCHS_DISPI_DATA || port == LOONGEMU_BOCHS_DISPI_DATA2) {
        uint16_t idx = host->display.dispi_index;
        *out = (idx < 16u) ? host->display.dispi_regs[idx] : 0u;
        return 1;
    }
    if (port >= 0x03c0u && port <= 0x03dfu) {
        *out = host->display.vga_regs[port - 0x03c0u];
        return 1;
    }
    return 0;
}

static int bochs_dispi_validate_geometry(const LoongemuBochsDisplay *display,
                                         uint32_t xres, uint32_t yres,
                                         uint32_t bpp, uint32_t virt_w,
                                         uint32_t x_offset, uint32_t y_offset)
{
    uint64_t bytes_per_pixel;
    uint64_t eff_w;
    uint64_t stride;
    uint64_t total_lines;
    uint64_t valid_bytes;

    if (!display) {
        return 0;
    }
    if (bpp != 8u && bpp != 15u && bpp != 16u && bpp != 24u && bpp != 32u) {
        return 0;
    }
    bytes_per_pixel = (bpp + 7u) / 8u;
    eff_w = (virt_w > xres) ? (uint64_t)virt_w : (uint64_t)xres;
    if (eff_w == 0 || xres == 0 || yres == 0) {
        return 0;
    }
    if ((uint64_t)x_offset + (uint64_t)xres > eff_w) {
        return 0;
    }
    stride = eff_w * bytes_per_pixel;
    total_lines = (uint64_t)y_offset + (uint64_t)yres;
    valid_bytes = bochs_fb_capacity(display);
    if (bytes_per_pixel == 0 || stride == 0 || total_lines == 0) {
        return 0;
    }
    if (stride > valid_bytes || total_lines > (valid_bytes / stride)) {
        return 0;
    }
    return 1;
}

static void bochs_dispi_write_reg(LoongemuBochsDisplay *d, uint16_t idx, uint16_t val)
{
    if (!d || idx >= 16u || idx == VBE_DISPI_INDEX_ID ||
        idx == VBE_DISPI_INDEX_VIDEO_MEMORY_64K) {
        return;
    }
    uint32_t xres = d->dispi_regs[VBE_DISPI_INDEX_XRES];
    uint32_t yres = d->dispi_regs[VBE_DISPI_INDEX_YRES];
    uint32_t bpp = d->dispi_regs[VBE_DISPI_INDEX_BPP];
    uint32_t virt_w = d->dispi_regs[VBE_DISPI_INDEX_VIRT_WIDTH];
    uint32_t x_off = d->dispi_regs[VBE_DISPI_INDEX_X_OFFSET];
    uint32_t y_off = d->dispi_regs[VBE_DISPI_INDEX_Y_OFFSET];

    if (idx == VBE_DISPI_INDEX_XRES) {
        xres = val;
    } else if (idx == VBE_DISPI_INDEX_YRES) {
        yres = val;
    } else if (idx == VBE_DISPI_INDEX_BPP) {
        bpp = val;
    } else if (idx == VBE_DISPI_INDEX_VIRT_WIDTH) {
        virt_w = val;
    } else if (idx == VBE_DISPI_INDEX_X_OFFSET) {
        x_off = val;
    } else if (idx == VBE_DISPI_INDEX_Y_OFFSET) {
        y_off = val;
    } else if (idx == VBE_DISPI_INDEX_BANK) {
        uint64_t valid_bytes = bochs_fb_capacity(d);
        if ((uint64_t)val * UINT64_C(65536) >= valid_bytes && val != 0u) {
            return;
        }
        d->dispi_regs[idx] = val;
        return;
    }

    if (idx == VBE_DISPI_INDEX_XRES || idx == VBE_DISPI_INDEX_YRES ||
        idx == VBE_DISPI_INDEX_BPP || idx == VBE_DISPI_INDEX_VIRT_WIDTH ||
        idx == VBE_DISPI_INDEX_X_OFFSET || idx == VBE_DISPI_INDEX_Y_OFFSET ||
        (idx == VBE_DISPI_INDEX_ENABLE && (val & 0x0001u) != 0u)) {
        if (!bochs_dispi_validate_geometry(d, xres, yres, bpp,
                                           virt_w, x_off, y_off)) {
            return;
        }
    }
    d->dispi_regs[idx] = val;
}

int loongemu_pci_io_write(LoongemuPciHost *host, uint32_t port, int size, uint64_t val)
{
    if (!host) {
        return 0;
    }
    if (port == LOONGEMU_PCI_CFG_ADDR_PORT) {
        host->cfg_addr = (uint32_t)val;
        return 1;
    }
    if (port >= LOONGEMU_PCI_CFG_DATA_PORT && port <= LOONGEMU_PCI_CFG_DATA_PORT + 3u) {
        if ((size != 1 && size != 2 && size != 4) ||
            (unsigned)size > 4u - (port - LOONGEMU_PCI_CFG_DATA_PORT)) {
            return 1;
        }
        if ((host->cfg_addr & 0x80000000u) == 0u) {
            return 1;
        }
        uint8_t bus = (uint8_t)((host->cfg_addr >> 16) & 0xffu);
        uint8_t dev = (uint8_t)((host->cfg_addr >> 11) & 0x1fu);
        uint8_t fn  = (uint8_t)((host->cfg_addr >> 8) & 0x07u);
        uint16_t reg = (uint16_t)((host->cfg_addr & 0xfcu) + (port - LOONGEMU_PCI_CFG_DATA_PORT));
        loongemu_pci_cfg_write(host, bus, dev, fn, reg, (uint32_t)val, (unsigned)size);
        return 1;
    }
    if (port == LOONGEMU_BOCHS_DISPI_INDEX) {
        host->display.dispi_index = (uint16_t)val;
        return 1;
    }
    if (port == LOONGEMU_BOCHS_DISPI_DATA || port == LOONGEMU_BOCHS_DISPI_DATA2) {
        bochs_dispi_write_reg(&host->display, host->display.dispi_index, (uint16_t)val);
        return 1;
    }
    if (port >= 0x03c0u && port <= 0x03dfu) {
        host->display.vga_regs[port - 0x03c0u] = (uint8_t)val;
        return 1;
    }
    return 0;
}

static int pci_ecam_access_valid(uint64_t offset, unsigned size)
{
    return (size == 1u || size == 2u || size == 4u) &&
           offset < LOONGEMU_PCI_ECAM_SIZE &&
           (uint64_t)size <= LOONGEMU_PCI_ECAM_SIZE - offset;
}

uint64_t loongemu_pci_ecam_read(void *opaque, uint64_t offset, unsigned size)
{
    LoongemuPciHost *host = (LoongemuPciHost *)opaque;
    if (!pci_ecam_access_valid(offset, size)) {
        return 0xffffffffu;
    }
    uint8_t bus = (uint8_t)((offset >> 20) & 0xffu);
    uint8_t dev = (uint8_t)((offset >> 15) & 0x1fu);
    uint8_t fn  = (uint8_t)((offset >> 12) & 0x07u);
    uint16_t reg = (uint16_t)(offset & 0x0fffu);
    return loongemu_pci_cfg_read(host, bus, dev, fn, reg, size);
}

void loongemu_pci_ecam_write(void *opaque, uint64_t offset, uint64_t val, unsigned size)
{
    LoongemuPciHost *host = (LoongemuPciHost *)opaque;
    if (!pci_ecam_access_valid(offset, size)) {
        return;
    }
    uint8_t bus = (uint8_t)((offset >> 20) & 0xffu);
    uint8_t dev = (uint8_t)((offset >> 15) & 0x1fu);
    uint8_t fn  = (uint8_t)((offset >> 12) & 0x07u);
    uint16_t reg = (uint16_t)(offset & 0x0fffu);
    loongemu_pci_cfg_write(host, bus, dev, fn, reg, (uint32_t)val, size);
}

uint64_t loongemu_bochs_mmio_read(void *opaque, uint64_t offset, unsigned size)
{
    LoongemuBochsDisplay *d = (LoongemuBochsDisplay *)opaque;
    if (!d) {
        return 0;
    }
    /* 0x0000..0x03ff: EDID blob */
    if (offset < 0x0400u) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++) {
            uint64_t pos = offset + i;
            uint8_t b = (pos < sizeof(d->edid)) ? d->edid[pos] : 0u;
            v |= ((uint64_t)b) << (i * 8u);
        }
        return v;
    }
    /* 0x0400..0x041f: remapped VGA ports 0x3c0..0x3df */
    if (offset >= 0x0400u && offset < 0x0420u) {
        return d->vga_regs[offset - 0x0400u];
    }
    /* 0x0500..0x051f: flat Bochs DISPI registers (index << 1) */
    if (offset >= 0x0500u && offset < 0x0520u) {
        uint32_t idx = (uint32_t)((offset - 0x0500u) >> 1);
        return (idx < 16u) ? d->dispi_regs[idx] : 0u;
    }
    /* 0x0600..0x0607: QEMU extended registers (rev >= 2) */
    if (offset == 0x0600u) {
        return 8u; /* region size */
    }
    if (offset == 0x0604u) {
        return 0x1e1e1e1eu; /* little-endian framebuffer */
    }
    return 0;
}

void loongemu_bochs_mmio_write(void *opaque, uint64_t offset, uint64_t val, unsigned size)
{
    LoongemuBochsDisplay *d = (LoongemuBochsDisplay *)opaque;
    (void)size;
    if (!d) {
        return;
    }
    if (offset >= 0x0400u && offset < 0x0420u) {
        d->vga_regs[offset - 0x0400u] = (uint8_t)val;
        return;
    }
    if (offset >= 0x0500u && offset < 0x0520u) {
        uint16_t idx = (uint16_t)((offset - 0x0500u) >> 1);
        bochs_dispi_write_reg(d, idx, (uint16_t)val);
        return;
    }
}
