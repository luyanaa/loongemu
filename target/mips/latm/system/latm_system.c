#include "qemu/osdep.h"
#include "latm-system-board.h"

static uint64_t sys_mailbox_read(void *opaque, uint64_t off, unsigned size)
{
    LatmSystem *sys = opaque;
    uint64_t v = 0;
    latm_uncore_mailbox_read(&sys->uncore,
                             LATM_MAILBOX_BASE + (uint32_t)off, size, &v);
    return v;
}

static void sys_mailbox_write(void *opaque, uint64_t off, uint64_t value,
                              unsigned size)
{
    LatmSystem *sys = opaque;
    latm_uncore_mailbox_write(&sys->uncore,
                              LATM_MAILBOX_BASE + (uint32_t)off, size, value);
}

static uint64_t sys_pic_read(void *opaque, uint64_t off, unsigned size)
{
    LatmSystem *sys = opaque;
    uint64_t v = 0;
    latm_uncore_pic_read(&sys->uncore, (uint32_t)off, size, &v);
    return v;
}

static void sys_pic_write(void *opaque, uint64_t off, uint64_t value,
                          unsigned size)
{
    LatmSystem *sys = opaque;
    latm_uncore_pic_write(&sys->uncore, (uint32_t)off, size, value);
}

static uint64_t sys_msi_read(void *opaque, uint64_t off, unsigned size)
{
    (void)opaque; (void)off; (void)size;
    return 0;
}

static void sys_msi_write(void *opaque, uint64_t off, uint64_t value,
                          unsigned size)
{
    LatmSystem *sys = opaque;
    latm_uncore_msi_write(&sys->uncore, LATM_PCH_MSI_BASE + (uint32_t)off,
                          size, value);
}

int latm_system_init_3a4000(LatmSystem *sys, uint8_t *ram, uint64_t ram_hpa,
                            uint64_t ram_size, uint64_t vmm_base,
                            uint64_t vmm_size, uint64_t code_base,
                            uint64_t code_size)
{
    if (!sys || !ram || !ram_size) return 0;
    memset(sys, 0, sizeof(*sys));
    sys->ram = ram;
    sys->ram_hpa = ram_hpa;
    sys->ram_size = ram_size;
    latm_uncore_init(&sys->uncore);
    latm_uart_init(&sys->uart);
    latm_mmio_init(&sys->mmio);
    if (!latm_memory_setup_3a4000(&sys->memory, ram_hpa, ram_size,
                                  vmm_base, vmm_size, code_base, code_size)) {
        return 0;
    }
    uint64_t low = sys->memory.low_ram_size;
    uint64_t high = sys->memory.high_ram_size;
    if (!latm_mmio_add(&sys->mmio, 0, low, LATM_MMIO_RAM, 1,
                       latm_mmio_ram_read, latm_mmio_ram_write, ram,
                       "low-ram")) return 0;
    if (high && !latm_mmio_add(&sys->mmio, 0x90000000ull, high,
                               LATM_MMIO_RAM, 1, latm_mmio_ram_read,
                               latm_mmio_ram_write, ram + low, "high-ram")) {
        return 0;
    }
    latm_mmio_add(&sys->mmio, LATM_MAILBOX_BASE,
                  LATM_MAX_CPUS * LATM_MAILBOX_CORE_STRIDE, LATM_MMIO_DEVICE,
                  0, sys_mailbox_read, sys_mailbox_write, sys, "mailbox");
    latm_mmio_add(&sys->mmio, 0x1fe001e0ull, 0x100, LATM_MMIO_DEVICE, 0,
                  latm_uart_read, latm_uart_write, &sys->uart, "uart0");
    latm_mmio_add(&sys->mmio, LATM_PCH_PIC_BASE, 0x1000, LATM_MMIO_DEVICE,
                  0, sys_pic_read, sys_pic_write, sys, "pch-pic");
    latm_mmio_add(&sys->mmio, LATM_PCH_MSI_BASE, 0x100000,
                  LATM_MMIO_DEVICE, 0, sys_msi_read, sys_msi_write, sys,
                  "pch-msi");
    latm_pci_init(&sys->pci, NULL);
    latm_pci_map_bochs_mmio(&sys->pci, &sys->mmio);
    latm_boot_info_init(&sys->boot);
    return 1;
}

int latm_system_mmio_read(LatmSystem *sys, uint64_t gpa, unsigned size,
                          uint64_t *value)
{
    return sys && latm_mmio_read(&sys->mmio, gpa, size, value);
}

int latm_system_mmio_write(LatmSystem *sys, uint64_t gpa, unsigned size,
                           uint64_t value)
{
    return sys && latm_mmio_write(&sys->mmio, gpa, size, value);
}

size_t latm_system_build_dtb(LatmSystem *sys, uint8_t *out, size_t cap,
                             uint8_t *struct_buf, size_t struct_cap,
                             uint8_t *strings_buf, size_t strings_cap)
{
    return sys ? latm_dtb_build_3a4000(out, cap, struct_buf, struct_cap,
                                       strings_buf, strings_cap,
                                       &sys->memory, &sys->boot) : 0;
}

void latm_system_set_boot_args(LatmSystem *sys, uint64_t dtb_gpa,
                               uint64_t initrd_start, uint64_t initrd_end,
                               const char *cmdline)
{
    if (!sys) return;
    latm_boot_info_set_dtb(&sys->boot, dtb_gpa, 0);
    latm_boot_info_set_initrd(&sys->boot, initrd_start, initrd_end);
    latm_boot_info_set_cmdline(&sys->boot, cmdline);
}

void latm_system_apply_boot_args(LatmSystem *sys, void *env)
{
    if (sys) latm_boot_info_apply_args(&sys->boot, env);
}
