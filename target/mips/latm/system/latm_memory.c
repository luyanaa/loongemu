#include "qemu/osdep.h"
#include <stdio.h>
#include <string.h>
#include "latm-system.h"
#include "cpu.h"

void latm_memory_init(LatmMemoryLayout *layout)
{
    if (layout) memset(layout, 0, sizeof(*layout));
}

int latm_memory_add(LatmMemoryLayout *layout, uint64_t base, uint64_t size,
                    uint64_t hpa, LatmMmioKind kind, uint8_t mat,
                    const char *name)
{
    if (!layout || size == 0 || base + size < base ||
        layout->count >= LATM_MEMORY_MAX_REGIONS) {
        return 0;
    }
    if (kind != LATM_MMIO_RESERVED) {
        for (unsigned i = 0; i < layout->count; i++) {
            const LatmMemoryRegion *r = &layout->regions[i];
            if (r->kind != LATM_MMIO_RESERVED &&
                base < r->base + r->size && r->base < base + size) {
                return 0;
            }
        }
    }
    LatmMemoryRegion *r = &layout->regions[layout->count++];
    memset(r, 0, sizeof(*r));
    r->base = base;
    r->size = size;
    r->hpa = hpa;
    r->kind = kind;
    r->mat = mat;
    r->name = name;
    return 1;
}

int latm_memory_add_reserved(LatmMemoryLayout *layout, uint64_t base,
                             uint64_t size, const char *name)
{
    return latm_memory_add(layout, base, size, 0, LATM_MMIO_RESERVED, 0, name);
}

int latm_memory_setup_3a4000(LatmMemoryLayout *layout, uint64_t ram_hpa,
                             uint64_t ram_size, uint64_t vmm_base,
                             uint64_t vmm_size, uint64_t code_base,
                             uint64_t code_size)
{
    const uint64_t low_limit = UINT64_C(0x10000000); /* 256 MiB */
    uint64_t low, high;

    if (!layout || ram_size == 0) return 0;
    latm_memory_init(layout);
    low = ram_size < low_limit ? ram_size : low_limit;
    high = ram_size - low;
    if (!latm_memory_add(layout, 0, low, ram_hpa, LATM_MMIO_RAM, 1, "low-ram")) {
        return 0;
    }
    layout->low_ram_base = 0;
    layout->low_ram_size = low;
    if (high) {
        if (!latm_memory_add(layout, UINT64_C(0x90000000), high,
                             ram_hpa + low, LATM_MMIO_RAM, 1, "high-ram")) {
            return 0;
        }
        layout->high_ram_base = UINT64_C(0x90000000);
        layout->high_ram_size = high;
    }
    if (vmm_size) {
        latm_memory_add_reserved(layout, vmm_base, vmm_size, "vmm");
    }
    if (code_size) {
        latm_memory_add_reserved(layout, code_base, code_size, "code-cache");
    }
    /* 3A low-peripheral holes: never describe them as RAM. */
    latm_memory_add(layout, UINT64_C(0x10000000), UINT64_C(0x10000000),
                    0, LATM_MMIO_RESERVED, 0, "ls7a-mmio");
    latm_memory_add(layout, UINT64_C(0x1fc00000), UINT64_C(0x400000),
                    0, LATM_MMIO_ROM, 0, "boot-rom");
    return 1;
}

int latm_memory_overlaps(const LatmMemoryLayout *layout, uint64_t base,
                         uint64_t size)
{
    if (!layout || !size) return 0;
    for (unsigned i = 0; i < layout->count; i++) {
        const LatmMemoryRegion *r = &layout->regions[i];
        if (base < r->base + r->size && r->base < base + size) return 1;
    }
    return 0;
}

const LatmMemoryRegion *latm_memory_find(const LatmMemoryLayout *layout,
                                         uint64_t addr, unsigned size)
{
    if (!layout || !size) return NULL;
    for (unsigned i = 0; i < layout->count; i++) {
        const LatmMemoryRegion *r = &layout->regions[i];
        if (addr >= r->base && addr - r->base < r->size &&
            size <= r->size - (addr - r->base)) {
            return r;
        }
    }
    return NULL;
}

void latm_boot_info_init(LatmBootInfo *info)
{
    if (!info) return;
    memset(info, 0, sizeof(*info));
    info->a0 = 0;
    info->a1 = 0;
    info->a2 = 0;
}

void latm_boot_info_set_dtb(LatmBootInfo *info, uint64_t dtb_gpa,
                            uint64_t dtb_size)
{
    (void)dtb_size;
    if (!info) return;
    info->dtb_gpa = dtb_gpa;
    /* Linux loongson64 checks fw_arg2 for FDT_MAGIC. */
    info->a2 = dtb_gpa;
}

void latm_boot_info_set_initrd(LatmBootInfo *info, uint64_t start, uint64_t end)
{
    if (!info) return;
    info->initrd_start = start;
    info->initrd_end = end;
}

void latm_boot_info_set_cmdline(LatmBootInfo *info, const char *cmdline)
{
    if (!info) return;
    if (!cmdline) { info->cmdline[0] = 0; return; }
    snprintf(info->cmdline, sizeof(info->cmdline), "%s", cmdline);
}

void latm_boot_info_apply_args(LatmBootInfo *info, void *envp)
{
    CPUMIPSState *env = envp;
    if (!info || !env) return;
    env->active_tc.gpr[4] = info->a0;
    env->active_tc.gpr[5] = info->a1;
    env->active_tc.gpr[6] = info->a2;
    if (info->entry_pc) env->active_tc.PC = info->entry_pc;
}
