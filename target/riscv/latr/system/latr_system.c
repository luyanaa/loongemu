#include <string.h>
#include "latr-system.h"
#include "mmu.h"
#include "loongemu/riscv_acpi.h"
#include "loongemu/riscv_fw.h"
#include "loongemu/ovmf_board.h"

#include "latr-elf.h"
static void sys_bus_lock(void *opaque);
static void sys_bus_unlock(void *opaque);

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

/*
 * Device writes take the guest store domain, not just its epoch: with vCPUs on
 * separate threads a CPU store and a DMA store to the same bytes would
 * otherwise interleave inside a memcpy. Blocking is safe because no CPU store
 * path holds the domain across a device call (MMIO is outside it in both the
 * emitted fast path and the C misaligned path), so a synchronous DMA from a
 * device cannot deadlock on it.
 */
static uint64_t system_dma_lock(void *opaque)
{
    (void)opaque;
    return latr_reservation_lock();
}

static void system_dma_unlock(void *opaque, uint64_t epoch)
{
    (void)opaque;
    latr_reservation_unlock(epoch);
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
    loongemu_guest_dma_set_write_domain(&sys->dma, system_dma_lock,
                                        system_dma_unlock, NULL);
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
    sys->device_lock = 0;
    sys->device_lock_acquisitions = 0;
    sys->device_lock_failures = 0;
    sys->env.plic = &sys->plic;
    sys->env.plic_context = UINT64_C(2) * boot_hart;
    sys->env.csr[0x3a0] = 0x0f;
    sys->env.csr[0x3b0] = (UINT64_C(1) << 54) - 1;
    sys->env.csr[0x30a] = UINT64_C(1) << 61; /* ADUE */
    sys->env.pc = LATR_SYSTEM_RAM_BASE;
    /* Guest MMIO reaches the bus directly from translated code, so the bus
     * itself takes the device lock around one access. */
    loongemu_mmio_bus_set_lock(&sys->bus, sys_bus_lock, sys_bus_unlock, sys);
    return 1;
}

/*
 * Device-model lock. Bounded on purpose: a store that never releases must make
 * the machine fail (design invariant I6) instead of freezing every hart. The
 * count of failures is published so the dispatcher can notice.
 */
static void sys_lock(LatrSystem *sys)
{
    uint64_t spins = LATR_SYSTEM_LOCK_SPINS;

    while (__atomic_exchange_n(&sys->device_lock, 1, __ATOMIC_ACQUIRE)) {
        if (spins-- == 0) {
            __atomic_add_fetch(&sys->device_lock_failures, 1,
                               __ATOMIC_RELAXED);
            return;
        }
        while (__atomic_load_n(&sys->device_lock, __ATOMIC_RELAXED) != 0) {
            if (spins-- == 0) {
                __atomic_add_fetch(&sys->device_lock_failures, 1,
                                   __ATOMIC_RELAXED);
                return;
            }
        }
    }
    __atomic_add_fetch(&sys->device_lock_acquisitions, 1, __ATOMIC_RELAXED);
}

static void sys_unlock(LatrSystem *sys)
{
    __atomic_store_n(&sys->device_lock, 0, __ATOMIC_RELEASE);
}

static void sys_bus_lock(void *opaque)
{
    sys_lock((LatrSystem *)opaque);
}

static void sys_bus_unlock(void *opaque)
{
    sys_unlock((LatrSystem *)opaque);
}

uint64_t latr_system_lock_failures(const LatrSystem *sys)
{
    return sys == 0 ? 0
                    : __atomic_load_n(&sys->device_lock_failures,
                                      __ATOMIC_RELAXED);
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
    sys_lock(sys);
    loongemu_vclock_advance_ns(&sys->vclock, ns);
    sys_unlock(sys);
}

/*
 * Timer due-ness for one hart: MTIP from the CLINT comparator and STIP from
 * stimecmp. `env` fields are read atomically because the hart may be dispatching
 * on another thread while the ticker samples them; a stale sample only costs a
 * spurious or delayed wake, and the hart re-samples at its own safe point.
 */
int latr_system_timer_due(LatrSystem *sys, const CPUArchState *env,
                          uint32_t hartid)
{
    uint64_t now;
    uint64_t mtimecmp;
    uint64_t stimecmp;

    if (sys == NULL || env == NULL || hartid >= sys->clint.hart_count) {
        return 0;
    }
    now = loongemu_vclock_now_ns(&sys->vclock);
    mtimecmp = __atomic_load_n(&sys->clint.mtimecmp[hartid], __ATOMIC_ACQUIRE);
    stimecmp = __atomic_load_n(&env->csr[LATR_RISCV_CSR_STIMECMP],
                               __ATOMIC_ACQUIRE);
    return now >= mtimecmp || now >= stimecmp ? 1 : 0;
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
    /* Another hart may be walking a device right now, and this reads the CLINT
     * and the PLIC for this hart's context. */
    sys_lock(sys);
    loongemu_clint_tick(&sys->clint, loongemu_vclock_now_ns(&sys->vclock),
                        &env->csr[0x344], hartid);
    sys_unlock(sys);
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
