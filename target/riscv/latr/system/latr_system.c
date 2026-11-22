#include <string.h>
#include "latr-system.h"
#include "mmu.h"
#include "loongemu/riscv_acpi.h"
#include "loongemu/riscv_fw.h"
#include "loongemu/ovmf_board.h"

#include "latr-elf.h"

static void sys_uart_tx(void *opaque, uint8_t byte)
{
    LatrSystem *sys = opaque;

    if (sys->uart_log_len + 1 < sizeof(sys->uart_log)) {
        sys->uart_log[sys->uart_log_len++] = (char)byte;
        sys->uart_log[sys->uart_log_len] = 0;
    }
}

static void sys_uart_irq(void *opaque, int level)
{
    LatrSystem *sys = opaque;
    /* QEMU virt maps UART0 to PLIC source 10. */
    loongemu_plic_set_irq(&sys->plic, 10, level);
}

static int sys_exit_write(void *opaque, uint64_t offset, uint64_t value,
                          unsigned size)
{
    LatrSystem *sys = opaque;
    (void)offset;
    (void)size;
    sys->exit_request = value;
    return 0;
}

static uint64_t sys_exit_read(void *opaque, uint64_t offset, unsigned size)
{
    LatrSystem *sys = opaque;
    (void)offset;
    (void)size;
    return sys->exit_request;
}

static void sys_exit_write_cb(void *opaque, uint64_t offset, uint64_t value,
                              unsigned size)
{
    sys_exit_write(opaque, offset, value, size);
}

static uint64_t sys_fwcfg_read(void *opaque, uint64_t offset, unsigned size)
{
    LatrSystem *sys = opaque;
    return loongemu_fw_cfg_mmio_read(&sys->fw_cfg, (uint32_t)offset, size);
}

static void sys_fwcfg_write(void *opaque, uint64_t offset, uint64_t value,
                            unsigned size)
{
    LatrSystem *sys = opaque;
    loongemu_fw_cfg_mmio_write(&sys->fw_cfg, (uint32_t)offset, value, size,
                               &sys->fw_dma_ops);
}

static uint64_t sys_pflash_read(void *opaque, uint64_t offset, unsigned size)
{
    OvmfPflash *flash = opaque;
    return ovmf_pflash_mmio_read(flash, offset, size);
}

static void sys_pflash_write(void *opaque, uint64_t offset, uint64_t value,
                             unsigned size)
{
    OvmfPflash *flash = opaque;
    ovmf_pflash_mmio_write(flash, offset, value, size);
}

static void system_dma_notify(void *opaque, uint64_t gpa, uint64_t len)
{
    (void)opaque;
    /* A device DMA store is a guest-memory write from outside the CPU store
     * path: advance the LR/SC epoch so an outstanding reservation cannot
     * succeed against the value the device just replaced. */
    latr_reservation_notify_write(gpa, len);
}

static int fw_dma_read(void *opaque, uint64_t gpa, void *buf, uint32_t len)
{
    LatrSystem *sys = opaque;

    return loongemu_guest_dma_read(&sys->dma, gpa, buf, len) == 0 ? 0 : -1;
}

static int fw_dma_write(void *opaque, uint64_t gpa, const void *buf,
                        uint32_t len)
{
    LatrSystem *sys = opaque;

    return loongemu_guest_dma_write(&sys->dma, gpa, buf, len) == 0 ? 0 : -1;
}

static int system_init(LatrSystem *sys, uint8_t *ram, uint64_t ram_size,
                       uint32_t nharts, uint32_t boot_hart)
{
    if (sys == NULL || ram == NULL || ram_size == 0 ||
        nharts == 0 || nharts > LOONGEMU_PLIC_MAX_CONTEXTS / 2 ||
        boot_hart >= nharts) {
        return 0;
    }
    memset(sys, 0, sizeof(*sys));
    sys->ram = ram;
    sys->ram_base = LATR_SYSTEM_RAM_BASE;
    sys->ram_size = ram_size;

    loongemu_mem_map_init(&sys->map);
    if (!loongemu_mem_map_add(&sys->map, sys->ram_base, ram_size,
                              (uint64_t)(uintptr_t)ram,
                              LOONGEMU_MEM_RAM, "ram")) {
        return 0;
    }
    if (!loongemu_mem_map_add(&sys->map, LATR_SYSTEM_CLINT_BASE,
                              0x10000, 0, LOONGEMU_MEM_MMIO, "clint") ||
        !loongemu_mem_map_add(&sys->map, LATR_SYSTEM_PLIC_BASE,
                              0x400000, 0, LOONGEMU_MEM_MMIO, "plic") ||
        !loongemu_mem_map_add(&sys->map, LATR_SYSTEM_UART_BASE,
                              0x100, 0, LOONGEMU_MEM_MMIO, "uart") ||
        !loongemu_mem_map_add(&sys->map, LATR_SYSTEM_EXIT_BASE,
                              LATR_SYSTEM_EXIT_SIZE, 0, LOONGEMU_MEM_MMIO,
                              "exit") ||
        !loongemu_mem_map_add(&sys->map, LATR_SYSTEM_FWCFG_BASE,
                              0x100, 0, LOONGEMU_MEM_MMIO, "fwcfg")) {
        return 0;
    }
    loongemu_mmio_bus_init(&sys->bus);
    loongemu_guest_dma_init(&sys->dma, &sys->map, system_dma_notify, sys);
    loongemu_clint_init(&sys->clint, 8);
    if (!loongemu_clint_attach(&sys->bus, &sys->clint,
                               LATR_SYSTEM_CLINT_BASE)) {
        return 0;
    }
    /* Two contexts per hart: 2*hart = M-mode, 2*hart+1 = S-mode. */
    if (!loongemu_plic_init(&sys->plic, 64, 2 * nharts) ||
        !loongemu_plic_attach(&sys->bus, &sys->plic,
                              LATR_SYSTEM_PLIC_BASE)) {
        return 0;
    }
    loongemu_uart16550_init(&sys->uart, sys_uart_tx, sys_uart_irq, sys);
    if (!loongemu_uart16550_attach(&sys->bus, &sys->uart,
                                   LATR_SYSTEM_UART_BASE, 0x100)) {
        return 0;
    }
    if (!loongemu_mmio_bus_add(&sys->bus, LATR_SYSTEM_EXIT_BASE,
                               LATR_SYSTEM_EXIT_SIZE,
                               sys_exit_read, sys_exit_write_cb, sys,
                               "exit")) {
        return 0;
    }
    loongemu_fw_cfg_init(&sys->fw_cfg, 1, 1);
    sys->fw_dma_ops.read = fw_dma_read;
    sys->fw_dma_ops.write = fw_dma_write;
    sys->fw_dma_ops.opaque = sys;
    if (!loongemu_mmio_bus_add(&sys->bus, LATR_SYSTEM_FWCFG_BASE, 0x100,
                               sys_fwcfg_read, sys_fwcfg_write, sys,
                               "fwcfg")) {
        return 0;
    }
    if (!loongemu_riscv_pflash_init(&sys->pflash, sys->pflash_code,
                                    sizeof(sys->pflash_code), sys->pflash_vars,
                                    sizeof(sys->pflash_vars),
                                    LATR_SYSTEM_PFLASH_CODE,
                                    LATR_SYSTEM_PFLASH_VARS) ||
        !loongemu_riscv_pflash_map(&sys->pflash, &sys->map) ||
        !loongemu_mmio_bus_add(&sys->bus, LATR_SYSTEM_PFLASH_CODE,
                               sizeof(sys->pflash_code), sys_pflash_read,
                               sys_pflash_write, &sys->pflash.code,
                               "pflash-code") ||
        !loongemu_mmio_bus_add(&sys->bus, LATR_SYSTEM_PFLASH_VARS,
                               sizeof(sys->pflash_vars), sys_pflash_read,
                               sys_pflash_write, &sys->pflash.vars,
                               "pflash-vars")) {
        return 0;
    }
    loongemu_vclock_init(&sys->vclock, LOONGEMU_VCLOCK_VIRTUAL_STEP,
                         UINT64_C(1000000000), 0, 0, 0, NULL, NULL);

    memset(&sys->env, 0, sizeof(sys->env));
    sys->env.hartid = boot_hart;
    sys->env.privilege = LATR_RISCV_PRIV_M;
    sys->env.mem_map = &sys->map;
    sys->env.mmio = &sys->bus;
    sys->env.vclock = &sys->vclock;
    sys->env.plic = &sys->plic;
    sys->env.plic_context = UINT64_C(2) * boot_hart;
    sys->env.csr[0x3a0] = 0x0f;
    sys->env.csr[0x3b0] = (UINT64_C(1) << 54) - 1;
    sys->env.csr[0x30a] = UINT64_C(1) << 61; /* ADUE */
    sys->env.pc = LATR_SYSTEM_RAM_BASE;
    return 1;
}

int latr_system_init(LatrSystem *sys, uint8_t *ram, uint64_t ram_size,
                     uint32_t hartid)
{
    return system_init(sys, ram, ram_size, 1, hartid);
}

int latr_system_init_harts(LatrSystem *sys, uint8_t *ram, uint64_t ram_size,
                           uint32_t nharts, uint32_t boot_hart)
{
    return system_init(sys, ram, ram_size, nharts, boot_hart);
}

int latr_system_load_elf(LatrSystem *sys, const uint8_t *elf, uint64_t len,
                         uint64_t *entry)
{
    LatrElfImage image;

    if (sys == NULL || sys->ram == NULL) {
        return 0;
    }
    if (!latr_elf_load(elf, len, sys->ram, sys->ram_base, sys->ram_size,
                       &image)) {
        return 0;
    }
    sys->image_end = image.end;
    sys->env.pc = image.entry;
    if (entry != NULL) {
        *entry = image.entry;
    }
    return 1;
}

void latr_system_tick(LatrSystem *sys, uint64_t ns)
{
    if (sys == NULL) {
        return;
    }
    loongemu_vclock_advance_ns(&sys->vclock, ns);
}

/*
 * Reflects this hart's CLINT state (MSIP/MTIP) and device state into its MIP.
 * Split from the clock advance because the clock is global: advancing it once
 * per hart would run N times faster on an N-hart machine.
 */
void latr_system_update_hart(LatrSystem *sys, CPUArchState *env, uint32_t hartid)
{
    if (sys == NULL || env == NULL) {
        return;
    }
    loongemu_clint_tick(&sys->clint, loongemu_vclock_now_ns(&sys->vclock),
                        &env->csr[0x344], hartid);
    latr_update_interrupts(env);
}

void latr_system_update(LatrSystem *sys)
{
    if (sys == NULL) {
        return;
    }
    latr_system_tick(sys, UINT64_C(1000));
    latr_system_update_hart(sys, &sys->env, (uint32_t)sys->env.hartid);
}

void latr_system_mmio_write(LatrSystem *sys, uint64_t pa, unsigned size,
                            uint64_t value)
{
    if (sys != NULL) {
        (void)loongemu_mmio_bus_write(&sys->bus, pa, size, value);
    }
}

int latr_system_build_acpi(LatrSystem *sys, uint64_t *rsdp_gpa)
{
    LoongemuRiscvAcpiHart hart = { 0 };
    LoongemuRiscvAcpi cfg;
    uint8_t *base;
    uint64_t cursor;
    uint64_t tables[4];
    uint64_t tables_start = 0;
    uint64_t tables_end = 0;
    uint32_t count = 0;
    uint32_t size;

    if (sys == NULL || sys->ram_size < 0x20000) {
        return 0;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.oem_id = "LOONGEMU";
    cfg.oem_table_id = "LATR";
    hart.hart_id = sys->env.hartid;
    hart.uid = (uint32_t)sys->env.hartid + 1;
    hart.ext_intc_id = 0;
    hart.enabled = 1;
    cfg.harts = &hart;
    cfg.hart_count = 1;
    cfg.has_plic = 1;
    cfg.plic_id = 1;
    cfg.plic_hw_id = "PLIC";
    cfg.plic_num_irqs = 64;
    cfg.plic_max_prio = 7;
    cfg.plic_addr = LATR_SYSTEM_PLIC_BASE;
    cfg.plic_size = 0x400000;
    cfg.has_spcr = 1;
    cfg.spcr_interface_type = 0x12;
    cfg.spcr_base = LATR_SYSTEM_UART_BASE;
    cfg.spcr_baud_rate = 7;
    cfg.spcr_terminal_type = 3;
    cfg.spcr_namespace = ".";
    cfg.isa = "rv64imafdcv";
    cfg.timer_hz = 1000000000;
    cfg.has_cmo = 1;
    cfg.cbom_block_log2 = 6;
    cfg.cboz_block_log2 = 6;
    cfg.has_mmu = 1;
    cfg.mmu_type = LOONGEMU_RHCT_MMU_SV39;

    cursor = sys->ram_base + sys->ram_size - 0x10000;
    base = sys->ram + (cursor - sys->ram_base);
    {
        uint32_t madt = loongemu_riscv_acpi_madt_size(&cfg);
        uint32_t rhct = loongemu_riscv_acpi_rhct_size(&cfg);
        uint32_t fadt = loongemu_riscv_acpi_fadt_size(&cfg);
        uint32_t spcr = loongemu_riscv_acpi_spcr_size(&cfg);
        uint64_t gpa[4];
        uint32_t xsize;
        if (cursor + madt + rhct + fadt + spcr + 0x1000 >
            sys->ram_base + sys->ram_size) {
            return 0;
        }
        tables_start = cursor;
        size = madt;
        gpa[0] = cursor;
        (void)loongemu_riscv_acpi_madt_build(
            &cfg, sys->ram + (cursor - sys->ram_base), size);
        tables[count++] = cursor;
        cursor = (cursor + size + 7) & ~UINT64_C(7);
        size = rhct;
        gpa[1] = cursor;
        (void)loongemu_riscv_acpi_rhct_build(
            &cfg, sys->ram + (cursor - sys->ram_base), size);
        tables[count++] = cursor;
        cursor = (cursor + size + 7) & ~UINT64_C(7);
        size = fadt;
        gpa[2] = cursor;
        (void)loongemu_riscv_acpi_fadt_build(
            &cfg, sys->ram + (cursor - sys->ram_base), size);
        tables[count++] = cursor;
        cursor = (cursor + size + 7) & ~UINT64_C(7);
        size = spcr;
        gpa[3] = cursor;
        (void)loongemu_riscv_acpi_spcr_build(
            &cfg, sys->ram + (cursor - sys->ram_base), size);
        tables[count++] = cursor;
        cursor = (cursor + size + 7) & ~UINT64_C(7);
        xsize = loongemu_riscv_acpi_xsdt_size(count);
        (void)loongemu_riscv_acpi_xsdt_build(
            sys->ram + (cursor - sys->ram_base), xsize, gpa, count, &cfg);
        tables_end = cursor;
        {
            uint64_t xsdt = cursor;
            cursor = (cursor + xsize + 7) & ~UINT64_C(7);
            loongemu_riscv_acpi_rsdp_build(
                sys->ram + (cursor - sys->ram_base), 0, xsdt, cfg.oem_id);
        }
    }
    sys->rsdp_gpa = cursor;
    sys->acpi_tables_gpa = tables_start;
    sys->acpi_tables_len = (uint32_t)(tables_end - tables_start);
    (void)loongemu_fw_cfg_add_file(
        &sys->fw_cfg, "etc/acpi/tables",
        sys->ram + (tables_start - sys->ram_base), sys->acpi_tables_len);
    (void)loongemu_fw_cfg_add_file(
        &sys->fw_cfg, "etc/acpi/rsdp",
        sys->ram + (sys->rsdp_gpa - sys->ram_base), 36);
    if (rsdp_gpa != NULL) {
        *rsdp_gpa = cursor;
    }
    return 1;
}

int latr_system_load_firmware(LatrSystem *sys, const uint8_t *image,
                              uint64_t len)
{
    if (sys == NULL || image == NULL || len == 0 ||
        len > sizeof(sys->pflash_code)) {
        return 0;
    }
    memcpy(sys->pflash_code, image, len);
    sys->env.pc = LATR_SYSTEM_PFLASH_CODE;
    return 1;
}

const char *latr_system_uart_log(const LatrSystem *sys)
{
    return sys == NULL ? "" : sys->uart_log;
}
