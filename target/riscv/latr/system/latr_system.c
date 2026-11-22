#include <string.h>
#include "latr-system.h"
#include "mmu.h"
#include "loongemu/riscv_acpi.h"
#include "loongemu/riscv_fw.h"
#include "loongemu/ovmf_board.h"

#define ELF_PT_LOAD 1
#define ELF_ET_EXEC 2
#define ELF_EM_RISCV 243
#define ELF_PHDR_SIZE 56
#define ELF_MAX_LOADS 16

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

static int fw_dma_read(void *opaque, uint64_t gpa, void *buf, uint32_t len)
{
    LatrSystem *sys = opaque;
    if (gpa < sys->ram_base || gpa + len > sys->ram_base + sys->ram_size) {
        return -1;
    }
    memcpy(buf, sys->ram + (gpa - sys->ram_base), len);
    return 0;
}

static int fw_dma_write(void *opaque, uint64_t gpa, const void *buf,
                        uint32_t len)
{
    LatrSystem *sys = opaque;
    if (gpa < sys->ram_base || gpa + len > sys->ram_base + sys->ram_size) {
        return -1;
    }
    memcpy(sys->ram + (gpa - sys->ram_base), buf, len);
    /* Device DMA is a guest-memory write from outside the CPU store path. */
    latr_reservation_notify_write(gpa, len);
    return 0;
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
                              4, 0, LOONGEMU_MEM_MMIO, "exit") ||
        !loongemu_mem_map_add(&sys->map, LATR_SYSTEM_FWCFG_BASE,
                              0x100, 0, LOONGEMU_MEM_MMIO, "fwcfg")) {
        return 0;
    }
    loongemu_mmio_bus_init(&sys->bus);
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
    if (!loongemu_mmio_bus_add(&sys->bus, LATR_SYSTEM_EXIT_BASE, 4,
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

static uint64_t elf64(const uint8_t *p, unsigned off, unsigned size)
{
    uint64_t v = 0;

    for (unsigned i = 0; i < size; i++) {
        v |= (uint64_t)p[off + i] << (8 * i);
    }
    return v;
}

/*
 * Loads an ELF64 RISC-V ET_EXEC image at each segment's p_paddr (offset 24;
 * offset 16 is p_vaddr). The whole image is validated before anything is
 * written, so a corrupt firmware cannot leave a partially loaded RAM image.
 */
int latr_system_load_elf(LatrSystem *sys, const uint8_t *elf, uint64_t len,
                         uint64_t *entry)
{
    struct {
        uint64_t start;
        uint64_t end;
    } loads[ELF_MAX_LOADS];
    unsigned nloads = 0;
    uint64_t phoff;
    uint64_t phentsize;
    uint64_t phnum;
    uint64_t entry_addr;
    uint64_t type;
    uint64_t machine;

    if (sys == NULL || elf == NULL || len < 64) {
        return 0;
    }
    if (elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' ||
        elf[4] != 2 || elf[5] != 1) {
        return 0;
    }
    type = elf64(elf, 16, 2);
    machine = elf64(elf, 18, 2);
    entry_addr = elf64(elf, 24, 8);
    phoff = elf64(elf, 32, 8);
    phentsize = elf64(elf, 54, 2);
    phnum = elf64(elf, 56, 2);

    if (type != ELF_ET_EXEC || machine != ELF_EM_RISCV) {
        return 0;
    }
    if (phentsize < ELF_PHDR_SIZE || phnum == 0 || phnum > ELF_MAX_LOADS) {
        return 0;
    }
    if (phoff > len || phnum > (len - phoff) / phentsize) {
        return 0;
    }

    /* Pass 1: validate every PT_LOAD segment and record its RAM range. */
    for (uint64_t i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + phoff + i * phentsize;
        uint64_t off;
        uint64_t paddr;
        uint64_t filesz;
        uint64_t memsz;
        uint64_t delta;
        uint64_t end;

        if ((uint32_t)elf64(ph, 0, 4) != ELF_PT_LOAD) {
            continue;
        }
        off = elf64(ph, 8, 8);
        paddr = elf64(ph, 24, 8); /* p_paddr; p_vaddr lives at offset 16 */
        filesz = elf64(ph, 32, 8);
        memsz = elf64(ph, 40, 8);
        if (filesz > memsz || memsz == 0) {
            return 0;
        }
        if (off > len || filesz > len - off) {
            return 0;
        }
        if (paddr < sys->ram_base) {
            return 0;
        }
        delta = paddr - sys->ram_base;
        if (delta > sys->ram_size || memsz > sys->ram_size - delta) {
            return 0; /* also rejects paddr + memsz overflow */
        }
        end = paddr + memsz;
        for (unsigned j = 0; j < nloads; j++) {
            if (paddr < loads[j].end && loads[j].start < end) {
                return 0; /* overlapping load segments */
            }
        }
        if (nloads == ELF_MAX_LOADS) {
            return 0;
        }
        loads[nloads].start = paddr;
        loads[nloads].end = end;
        nloads++;
    }
    if (nloads == 0) {
        return 0;
    }
    {
        bool entry_ok = false;

        for (unsigned j = 0; j < nloads; j++) {
            if (entry_addr >= loads[j].start && entry_addr < loads[j].end) {
                entry_ok = true;
            }
        }
        if (!entry_ok) {
            return 0;
        }
    }

    /* Pass 2: write. */
    for (uint64_t i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + phoff + i * phentsize;
        uint64_t off;
        uint64_t paddr;
        uint64_t filesz;
        uint64_t memsz;
        uint8_t *dst;

        if ((uint32_t)elf64(ph, 0, 4) != ELF_PT_LOAD) {
            continue;
        }
        off = elf64(ph, 8, 8);
        paddr = elf64(ph, 24, 8);
        filesz = elf64(ph, 32, 8);
        memsz = elf64(ph, 40, 8);
        dst = sys->ram + (paddr - sys->ram_base);
        memcpy(dst, elf + off, filesz);
        if (memsz > filesz) {
            memset(dst + filesz, 0, memsz - filesz);
        }
    }
    sys->env.pc = entry_addr;
    if (entry != NULL) {
        *entry = entry_addr;
    }
    return 1;
}

void latr_system_update(LatrSystem *sys)
{
    if (sys == NULL) {
        return;
    }
    loongemu_vclock_advance_ns(&sys->vclock, UINT64_C(1000));
    loongemu_clint_tick(&sys->clint,
                        loongemu_vclock_now_ns(&sys->vclock),
                        &sys->env.csr[0x344], (uint32_t)sys->env.hartid);
    latr_update_interrupts(&sys->env);
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
