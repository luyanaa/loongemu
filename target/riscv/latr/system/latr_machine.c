#include <string.h>
#include "latr-machine.h"
#include "latr-fdt.h"
#include "mmu.h"
#include "translate.h"

#define PLIC_PHANDLE (1u + LATR_MACHINE_MAX_HARTS)
#define TEST_PHANDLE (2u + LATR_MACHINE_MAX_HARTS)
#define CPU_INTC_PHANDLE(hart) (1u + (hart))

static uint64_t align_down(uint64_t value, uint64_t align)
{
    return value & ~(align - 1u);
}

static uint64_t align_up(uint64_t value, uint64_t align)
{
    return (value + align - 1u) & ~(align - 1u);
}

/* Device-tree unit addresses are hexadecimal and must match the literal paths
 * used by /chosen/stdout-path and /aliases. */
static void format_node(char *out, const char *prefix, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[12];
    unsigned d = 0;
    size_t n = strlen(prefix);

    memcpy(out, prefix, n);
    if (value == 0) {
        tmp[d++] = '0';
    }
    while (value != 0) {
        tmp[d++] = digits[value & 15u];
        value >>= 4;
    }
    while (d != 0) {
        out[n++] = tmp[--d];
    }
    out[n] = '\0';
}

int latr_machine_init(LatrMachine *m, uint8_t *ram, uint64_t ram_size,
                      uint32_t nharts, uint8_t *work, uint32_t work_size)
{
    uint32_t per;

    if (m == NULL || ram == NULL || work == NULL ||
        nharts == 0 || nharts > LATR_MACHINE_MAX_HARTS) {
        return 0;
    }
    memset(m, 0, sizeof(*m));
    m->nharts = nharts;
    m->boot_hart = 0;
    if (ram_size < UINT64_C(8) << 20) {
        return 0;
    }
    m->dtb_addr = align_down(
        LATR_SYSTEM_RAM_BASE + ram_size - LATR_MACHINE_DTB_RESERVE, 0x1000);
    if (m->dtb_addr <= LATR_MACHINE_KERNEL_BASE) {
        return 0;
    }
    if (!latr_system_init_harts(&m->sys, ram, ram_size, nharts,
                                m->boot_hart)) {
        return 0;
    }
    /* QEMU virt exposes readable low memory; fw_dynamic probes address 0
     * before deciding whether a2 carries a fw_dynamic_info structure. */
    {
        uint64_t *info = (uint64_t *)(void *)(m->boot_rom +
                                              LATR_MACHINE_FW_DYNAMIC_INFO);

        info[0] = UINT64_C(0x4942534f); /* FW_DYNAMIC_INFO_MAGIC */
        info[1] = 2;                    /* version */
        info[2] = LATR_MACHINE_KERNEL_BASE;
        info[3] = 1;                    /* next stage: S-mode */
        info[4] = 0;                    /* options */
        info[5] = m->boot_hart;
    }
    if (!loongemu_mem_map_add(&m->sys.map, 0, sizeof(m->boot_rom),
                              (uint64_t)(uintptr_t)m->boot_rom,
                              LOONGEMU_MEM_ROM, "boot-rom")) {
        return 0;
    }
    per = work_size / (nharts * 2u);
    per &= ~15u;
    if (per < 4096) {
        return 0;
    }
    for (uint32_t i = 0; i < nharts; i++) {
        LatrMachineHart *h = &m->harts[i];

        h->env = m->sys.env;
        h->env.hartid = i;
        h->env.plic_context = UINT64_C(2) * i;
        h->env.pc = LATR_MACHINE_FIRMWARE_BASE;
        h->env.privilege = LATR_RISCV_PRIV_M;
        h->env.exception = 0;
        h->env.mem_fault_cause = 0;
        h->env.fetch_faulted = 0;
        h->cpu.env_ptr = &h->env;
        h->hartid = i;
        h->online = true;
        h->arena = work + (size_t)i * per;
        h->arena_size = per;
        h->code_buf = work + (size_t)(nharts + i) * per;
        h->code_buf_size = per;
        latr_tb_cache_init(&h->tb_cache, h->arena, h->arena_size);
        latr_tb_cache_register(&h->tb_cache);
        h->ctx = latr_context_create(&h->env, &h->tb_cache, h->code_buf,
                                     h->code_buf_size);
        if (h->ctx == NULL) {
            latr_machine_fini(m);
            return 0;
        }
    }
    return 1;
}

void latr_machine_fini(LatrMachine *m)
{
    if (m == NULL) {
        return;
    }
    for (uint32_t i = 0; i < LATR_MACHINE_MAX_HARTS; i++) {
        if (m->harts[i].ctx != NULL) {
            latr_tb_cache_unregister(&m->harts[i].tb_cache);
            latr_context_destroy(m->harts[i].ctx);
            m->harts[i].ctx = NULL;
        }
    }
}

static int load_raw(LatrMachine *m, uint64_t addr, const uint8_t *image,
                    uint64_t len, uint64_t limit)
{
    if (m == NULL || image == NULL || len == 0 ||
        addr < m->sys.ram_base ||
        addr - m->sys.ram_base > m->sys.ram_size ||
        len > m->sys.ram_size - (addr - m->sys.ram_base) ||
        addr + len > limit) {
        return 0;
    }
    memcpy(m->sys.ram + (addr - m->sys.ram_base), image, len);
    return 1;
}

int latr_machine_load_firmware(LatrMachine *m, const uint8_t *image,
                               uint64_t len)
{
    if (m == NULL || len == 0 || len > LATR_MACHINE_FIRMWARE_MAX) {
        return 0;
    }
    m->firmware_entry = LATR_MACHINE_FIRMWARE_BASE;
    if (!load_raw(m, m->firmware_entry, image, len, m->dtb_addr)) {
        return 0;
    }
    m->firmware_loaded = true;
    return 1;
}

int latr_machine_load_firmware_elf(LatrMachine *m, const uint8_t *elf,
                                   uint64_t len)
{
    uint64_t entry = 0;

    if (m == NULL || elf == NULL ||
        !latr_system_load_elf(&m->sys, elf, len, &entry)) {
        return 0;
    }
    if (entry < m->sys.ram_base ||
        entry >= m->sys.ram_base + m->sys.ram_size ||
        entry >= LATR_MACHINE_KERNEL_BASE) {
        return 0;
    }
    /*
     * Checking only the entry point is not enough: a segment may start below
     * the kernel window and still extend into it, which would let the firmware
     * overwrite the image we load at +2 MiB. latr_elf_load() already rejected
     * anything outside RAM, so `image_end` is a valid exclusive bound.
     */
    if (m->sys.image_end > LATR_MACHINE_KERNEL_BASE) {
        return 0;
    }
    m->firmware_entry = entry;
    m->firmware_loaded = true;
    return 1;
}

int latr_machine_load_kernel(LatrMachine *m, const uint8_t *image,
                             uint64_t len)
{
    if (m == NULL || len == 0) {
        return 0;
    }
    if (!load_raw(m, LATR_MACHINE_KERNEL_BASE, image, len, m->dtb_addr)) {
        return 0;
    }
    m->kernel_entry = LATR_MACHINE_KERNEL_BASE;
    m->kernel_end = align_up(LATR_MACHINE_KERNEL_BASE + len, 0x1000);
    m->kernel_loaded = true;
    return 1;
}

int latr_machine_load_initrd(LatrMachine *m, const uint8_t *image,
                             uint64_t len)
{
    uint64_t start;

    if (m == NULL || image == NULL || len == 0) {
        return 0;
    }
    start = align_down(m->dtb_addr - len, 0x1000);
    if (start < (m->kernel_loaded ? m->kernel_end : LATR_MACHINE_KERNEL_BASE) ||
        !load_raw(m, start, image, len, m->dtb_addr)) {
        return 0;
    }
    m->initrd_start = start;
    m->initrd_end = start + len;
    m->initrd_loaded = true;
    return 1;
}

static void machine_virtio_irq(void *opaque, uint32_t irq, int level)
{
    LatrMachine *m = opaque;

    loongemu_plic_set_irq(&m->sys.plic, irq, level);
}

int latr_machine_attach_disk(LatrMachine *m, uint8_t *image, uint64_t len)
{
    if (m == NULL) {
        return 0;
    }
    /*
     * No image means the board was started without -drive. That is a supported
     * configuration: the guest simply sees no block device. Everything the
     * device would have contributed (MMIO window, PLIC line, DTB node) is keyed
     * off `disk_attached`, which stays false here, so bailing out early *is* the
     * degradation -- reporting it as a failure would make "-drive is optional"
     * impossible for callers to express.
     */
    if (len == 0) {
        return 1;
    }
    if (image == NULL) {
        /* A size without a buffer is not "no disk", it is a caller bug. */
        return 0;
    }
    if (len < LOONGEMU_VIRTIO_BLK_SECTOR || m->disk_attached) {
        return 0;
    }
    if (!loongemu_mem_map_add(&m->sys.map, LATR_MACHINE_VIRTIO_BASE,
                              LATR_MACHINE_VIRTIO_STRIDE *
                                  LATR_MACHINE_VIRTIO_COUNT,
                              0, LOONGEMU_MEM_MMIO, "virtio")) {
        return 0;
    }
    if (!loongemu_virtio_blk_init(&m->virtio_blk, &m->sys.map, image, len,
                                  LATR_MACHINE_VIRTIO_IRQ_BASE,
                                  machine_virtio_irq, m)) {
        return 0;
    }
    if (!loongemu_virtio_blk_attach(&m->sys.bus, &m->virtio_blk,
                                    LATR_MACHINE_VIRTIO_BASE)) {
        return 0;
    }
    m->disk = image;
    m->disk_size = len;
    m->disk_attached = true;
    m->dtb_ready = false;
    return 1;
}

int latr_machine_set_bootargs(LatrMachine *m, const char *args)
{
    size_t len;

    if (m == NULL || args == NULL) {
        return 0;
    }
    len = strlen(args);
    if (len >= sizeof(m->bootargs)) {
        return 0;
    }
    memcpy(m->bootargs, args, len + 1u);
    m->bootargs[len] = '\0';
    m->dtb_ready = false;
    return 1;
}

void latr_machine_set_enter_tb(LatrMachine *m, LatrMachineEnterTb fn)
{
    if (m != NULL) {
        m->enter_tb = fn;
    }
}

void latr_machine_set_translate_tb(LatrMachine *m, LatrMachineTranslateTb fn)
{
    if (m != NULL) {
        m->translate_tb = fn;
    }
}

void latr_machine_set_translated_tb(LatrMachine *m, LatrMachineTranslatedTb fn)
{
    if (m != NULL) {
        m->translated_tb = fn;
    }
}

static void build_cpu_node(LatrFdt *fdt, uint32_t hart, bool online)
{
    char name[16];

    format_node(name, "cpu@", hart);
    latr_fdt_begin_node(fdt, name);
    latr_fdt_prop_string(fdt, "device_type", "cpu");
    latr_fdt_prop_u32(fdt, "reg", hart);
    latr_fdt_prop_string(fdt, "status", online ? "okay" : "disabled");
    latr_fdt_prop_string(fdt, "compatible", "riscv");
    latr_fdt_prop_string(fdt, "riscv,isa", "rv64imafdc");
    {
        static const char *const extensions[] = {
            "i", "m", "a", "f", "d", "c", "zicsr", "zifencei",
        };

        latr_fdt_prop_strings(fdt, "riscv,isa-extensions", extensions,
                              sizeof(extensions) / sizeof(extensions[0]));
    }
    latr_fdt_prop_string(fdt, "mmu-type", "riscv,sv39");
    latr_fdt_prop_u32(fdt, "clock-frequency", 1000000000u);
    latr_fdt_begin_node(fdt, "interrupt-controller");
    latr_fdt_prop_u32(fdt, "#interrupt-cells", 1);
    latr_fdt_prop_empty(fdt, "interrupt-controller");
    latr_fdt_prop_string(fdt, "compatible", "riscv,cpu-intc");
    latr_fdt_prop_u32(fdt, "phandle", CPU_INTC_PHANDLE(hart));
    latr_fdt_end_node(fdt);
    latr_fdt_end_node(fdt);
}

int latr_machine_build_dtb(LatrMachine *m)
{
    LatrFdt fdt;
    uint8_t *buf;
    uint32_t size;
    uint32_t cells[LATR_MACHINE_MAX_HARTS * 4u];
    uint32_t n = 0;
    uint32_t reg[4];
    char node[24];

    if (m == NULL) {
        return 0;
    }
    if (m->dtb_addr < m->sys.ram_base ||
        m->dtb_addr - m->sys.ram_base > m->sys.ram_size ||
        LATR_MACHINE_DTB_SIZE > m->sys.ram_size -
                                    (m->dtb_addr - m->sys.ram_base)) {
        return 0;
    }
    buf = m->sys.ram + (m->dtb_addr - m->sys.ram_base);
    latr_fdt_init(&fdt, buf, LATR_MACHINE_DTB_SIZE, m->boot_hart);

    latr_fdt_begin_node(&fdt, "");
    latr_fdt_prop_u32(&fdt, "#address-cells", 2);
    latr_fdt_prop_u32(&fdt, "#size-cells", 2);
    latr_fdt_prop_string(&fdt, "compatible", "riscv-virtio");
    latr_fdt_prop_string(&fdt, "model", "latr-riscv64-virt");

    latr_fdt_begin_node(&fdt, "chosen");
    if (m->bootargs[0] != '\0') {
        latr_fdt_prop_string(&fdt, "bootargs", m->bootargs);
    } else {
        latr_fdt_prop_string(
            &fdt, "bootargs",
            "console=ttyS0 earlycon=uart8250,mmio,0x10000000 keep_bootcon");
    }
    latr_fdt_prop_string(&fdt, "stdout-path", "/soc/uart@10000000");
    if (m->initrd_loaded) {
        latr_fdt_prop_u64(&fdt, "initrd-start", m->initrd_start);
        latr_fdt_prop_u64(&fdt, "initrd-end", m->initrd_end);
        latr_fdt_prop_u64(&fdt, "linux,initrd-start", m->initrd_start);
        latr_fdt_prop_u64(&fdt, "linux,initrd-end", m->initrd_end);
    }
    latr_fdt_end_node(&fdt);

    /* Poweroff / reboot through the SiFive test device, exactly like QEMU virt.
     * Without these two nodes OpenSBI's SRST extension reports
     * SBI_ERR_NOT_SUPPORTED, so an S-mode `system_reset` ecall cannot stop the
     * machine and every clean shutdown has to poke the test device behind the
     * firmware's back. Linux's poweroff path looks for the same nodes. */
    latr_fdt_begin_node(&fdt, "poweroff");
    latr_fdt_prop_string(&fdt, "compatible", "syscon-poweroff");
    latr_fdt_prop_u32(&fdt, "regmap", TEST_PHANDLE);
    latr_fdt_prop_u32(&fdt, "offset", 0);
    latr_fdt_prop_u32(&fdt, "value", 0x5555);
    latr_fdt_end_node(&fdt);

    latr_fdt_begin_node(&fdt, "reboot");
    latr_fdt_prop_string(&fdt, "compatible", "syscon-reboot");
    latr_fdt_prop_u32(&fdt, "regmap", TEST_PHANDLE);
    latr_fdt_prop_u32(&fdt, "offset", 0);
    latr_fdt_prop_u32(&fdt, "value", 0x7777);
    latr_fdt_end_node(&fdt);

    reg[0] = (uint32_t)(LATR_SYSTEM_RAM_BASE >> 32);
    reg[1] = (uint32_t)LATR_SYSTEM_RAM_BASE;
    reg[2] = (uint32_t)(m->sys.ram_size >> 32);
    reg[3] = (uint32_t)m->sys.ram_size;
    latr_fdt_begin_node(&fdt, "memory@80000000");
    latr_fdt_prop_string(&fdt, "device_type", "memory");
    latr_fdt_prop_cells(&fdt, "reg", reg, 4);
    latr_fdt_end_node(&fdt);

    latr_fdt_begin_node(&fdt, "cpus");
    latr_fdt_prop_u32(&fdt, "#address-cells", 1);
    latr_fdt_prop_u32(&fdt, "#size-cells", 0);
    latr_fdt_prop_u32(&fdt, "timebase-frequency", LATR_MACHINE_TIMEBASE);
    for (uint32_t i = 0; i < m->nharts; i++) {
        build_cpu_node(&fdt, i, m->harts[i].online);
    }
    latr_fdt_end_node(&fdt);

    latr_fdt_begin_node(&fdt, "soc");
    latr_fdt_prop_u32(&fdt, "#address-cells", 2);
    latr_fdt_prop_u32(&fdt, "#size-cells", 2);
    latr_fdt_prop_string(&fdt, "compatible", "simple-bus");
    latr_fdt_prop_empty(&fdt, "ranges");

    latr_fdt_begin_node(&fdt, "clint@2000000");
    latr_fdt_prop_string(&fdt, "compatible", "riscv,clint0");
    reg[0] = 0;
    reg[1] = (uint32_t)LATR_SYSTEM_CLINT_BASE;
    reg[2] = 0;
    reg[3] = 0x10000;
    latr_fdt_prop_cells(&fdt, "reg", reg, 4);
    n = 0;
    for (uint32_t i = 0; i < m->nharts; i++) {
        cells[n++] = CPU_INTC_PHANDLE(i);
        cells[n++] = 3;
        cells[n++] = CPU_INTC_PHANDLE(i);
        cells[n++] = 7;
    }
    latr_fdt_prop_cells(&fdt, "interrupts-extended", cells, n);
    latr_fdt_end_node(&fdt);

    latr_fdt_begin_node(&fdt, "plic@c000000");
    latr_fdt_prop_string(&fdt, "compatible", "riscv,plic0");
    reg[0] = 0;
    reg[1] = (uint32_t)LATR_SYSTEM_PLIC_BASE;
    reg[2] = 0;
    reg[3] = 0x400000;
    latr_fdt_prop_cells(&fdt, "reg", reg, 4);
    latr_fdt_prop_u32(&fdt, "#interrupt-cells", 1);
    latr_fdt_prop_empty(&fdt, "interrupt-controller");
    latr_fdt_prop_u32(&fdt, "riscv,ndev", 64);
    latr_fdt_prop_u32(&fdt, "phandle", PLIC_PHANDLE);
    n = 0;
    for (uint32_t i = 0; i < m->nharts; i++) {
        cells[n++] = CPU_INTC_PHANDLE(i);
        cells[n++] = 11; /* M-mode external */
        cells[n++] = CPU_INTC_PHANDLE(i);
        cells[n++] = 9;  /* S-mode external */
    }
    latr_fdt_prop_cells(&fdt, "interrupts-extended", cells, n);
    latr_fdt_end_node(&fdt);

    if (m->disk_attached) {
        latr_fdt_begin_node(&fdt, "virtio_mmio@10001000");
        latr_fdt_prop_string(&fdt, "compatible", "virtio,mmio");
        reg[0] = 0;
        reg[1] = (uint32_t)LATR_MACHINE_VIRTIO_BASE;
        reg[2] = 0;
        reg[3] = LOONGEMU_VIRTIO_MMIO_SIZE;
        latr_fdt_prop_cells(&fdt, "reg", reg, 4);
        latr_fdt_prop_u32(&fdt, "interrupt-parent", PLIC_PHANDLE);
        latr_fdt_prop_u32(&fdt, "interrupts", LATR_MACHINE_VIRTIO_IRQ_BASE);
        latr_fdt_end_node(&fdt);
    }

    format_node(node, "uart@", 0x10000000u);
    latr_fdt_begin_node(&fdt, node);
    latr_fdt_prop_string(&fdt, "compatible", "ns16550a");
    reg[0] = 0;
    reg[1] = (uint32_t)LATR_SYSTEM_UART_BASE;
    reg[2] = 0;
    reg[3] = 0x100;
    latr_fdt_prop_cells(&fdt, "reg", reg, 4);
    latr_fdt_prop_u32(&fdt, "clock-frequency", 3686400u);
    latr_fdt_prop_u32(&fdt, "current-speed", 115200u);
    latr_fdt_prop_u32(&fdt, "interrupt-parent", PLIC_PHANDLE);
    latr_fdt_prop_u32(&fdt, "interrupts", LATR_MACHINE_UART_IRQ);
    latr_fdt_end_node(&fdt);

    /* SiFive test device. OpenSBI's fdt_reset_sifive_test binds to
     * "sifive,test0"/"sifive,test1" and implements SRST by writing 0x5555
     * (poweroff) or 0x7777 (reboot) to offset 0, which is exactly what the
     * exit device already decodes. */
    latr_fdt_begin_node(&fdt, "test@100000");
    {
        static const char *const test_compat[] = {
            "sifive,test1", "sifive,test0", "syscon",
        };

        latr_fdt_prop_strings(&fdt, "compatible", test_compat, 3);
    }
    reg[0] = 0;
    reg[1] = (uint32_t)LATR_SYSTEM_EXIT_BASE;
    reg[2] = 0;
    reg[3] = 0x1000;
    latr_fdt_prop_cells(&fdt, "reg", reg, 4);
    latr_fdt_prop_u32(&fdt, "phandle", TEST_PHANDLE);
    latr_fdt_end_node(&fdt);

    latr_fdt_end_node(&fdt); /* soc */
    latr_fdt_end_node(&fdt); /* root */

    size = latr_fdt_finish(&fdt);
    if (size == 0) {
        return 0;
    }
    m->dtb_size = size;
    m->dtb_ready = true;
    return 1;
}

int latr_machine_host_fault(LatrMachine *m, uint32_t hartid,
                            uintptr_t host_pc, uint32_t cause,
                            uint64_t badaddr, LatrMachineFaultInfo *info)
{
    LatrMachineHart *h;
    LatrTranslatorContext *saved;
    uint64_t guest_pc = 0;
    bool resolved;

    if (info != NULL) {
        memset(info, 0, sizeof(*info));
        info->host_pc = host_pc;
        info->cause = cause;
        info->badaddr = badaddr;
        info->hartid = hartid;
    }
    if (m == NULL || hartid >= m->nharts) {
        return 0;
    }
    h = &m->harts[hartid];
    if (h->ctx == NULL) {
        return 0;
    }
    /*
     * Borrow the hart's translator context to resolve the native PC, then put
     * the caller's context back. Without the restore, a host fault raised while
     * dispatching hart 1 would leave hart 0's state pointing at hart 1 -- which
     * only shows up once each hart has its own TLS context.
     */
    saved = latr_context_bind(h->ctx);
    latr_set_cpu_env(&h->env);
    resolved = latr_tb_insn_map_resolve(host_pc, &guest_pc, NULL);
    if (!resolved) {
        latr_context_restore(saved);
        return 0;
    }
    if (info != NULL) {
        info->guest_pc = guest_pc;
        info->resolved = true;
    }
    /* Instructions before the faulting one already committed their effects;
     * resume the guest at the faulting instruction itself with the guest
     * exception latched. */
    h->env.pc = guest_pc;
    h->env.exception = cause;
    h->env.badaddr = badaddr;
    h->env.mem_fault_cause = 0;
    h->env.fetch_faulted = 0;
    latr_tb_shootdown_all(&h->tb_cache);
    latr_context_restore(saved);
    return 1;
}

int latr_machine_boot(LatrMachine *m)
{
    if (m == NULL || !m->dtb_ready) {
        return 0;
    }
    for (uint32_t i = 0; i < m->nharts; i++) {
        LatrMachineHart *h = &m->harts[i];

        if (!h->online) {
            continue;
        }
        h->env.exception = 0;
        h->env.mem_fault_cause = 0;
        h->env.fetch_faulted = 0;
        h->env.gpr[10] = i;           /* a0 = hartid */
        h->env.gpr[11] = m->dtb_addr; /* a1 = DTB */
        h->env.gpr[12] = m->firmware_loaded
                             ? LATR_MACHINE_FW_DYNAMIC_INFO
                             : 0; /* a2 = fw_dynamic_info (OpenSBI) */
        if (m->firmware_loaded) {
            h->env.pc = m->firmware_entry;
            h->env.privilege = LATR_RISCV_PRIV_M;
        } else if (m->kernel_loaded) {
            /*
             * No firmware: start Linux directly in S-mode with the state
             * OpenSBI would have installed. Without these the very first
             * rdtime/csr access traps and the kernel dies before earlycon.
             */
            uint64_t status = h->env.csr[0x300] & ~(UINT64_C(3) << 11);

            status |= UINT64_C(1) << 11; /* MPP = S */
            h->env.csr[0x300] = status;
            h->env.csr[0x303] = UINT64_C(0x222); /* delegate SSI/STI/SEI */
            h->env.csr[0x304] = UINT64_C(0x222);
            h->env.csr[0x306] = 7;               /* mcounteren: cycle/time/instret */
            h->env.pc = m->kernel_entry;
            h->env.privilege = LATR_RISCV_PRIV_S;
        } else {
            return 0;
        }
    }
    m->booted = true;
    return 1;
}

/* Publish freshly generated code to the instruction stream. */
static inline void latr_icache_sync(void)
{
#if defined(__loongarch__)
    __asm__ volatile("dbar 0\n\tibar 0" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

static int run_hart(LatrMachine *m, LatrMachineHart *h)
{
    TranslationBlock tb;
    LatrTbKey key;
    LatrTbEntry *entry;
    uint64_t code;
    uint64_t pc_before;
    int limit = 64;
    int words;

    latr_context_bind(h->ctx);
    latr_set_cpu_env(&h->env);
    /*
     * Per-hart timer and IPI state: CLINT MSIP/MTIP are per hart and must be
     * latched into *this* hart's MIP. The clock itself is advanced once per
     * machine step by the caller.
     */
    latr_system_update_hart(&m->sys, &h->env, h->hartid);
    if (h->env.sfence_pending) {
        latr_tb_shootdown_asid(&h->tb_cache, (uint32_t)h->env.sfence_asid);
        h->env.sfence_pending = 0;
    }
    if (h->env.fence_i_pending) {
        latr_tb_shootdown_all(&h->tb_cache);
        h->env.fence_i_pending = 0;
    }
    latr_context_safepoint(h->ctx);
    if (latr_deliver_interrupt(&h->env)) {
        h->env.exception = 0;
    }

    key = latr_tb_cache_key(&h->env);
    pc_before = h->env.pc;
    entry = latr_tb_cache_lookup(&h->tb_cache, h->env.pc, key);
    if (entry == NULL) {
        if (m->translate_tb != NULL) {
            m->translate_tb(&h->env, h->env.pc);
        }
        memset(&tb, 0, sizeof(tb));
        tb.pc = h->env.pc;
        tb.tc.ptr = h->code_buf;
        tb.cflags = 0;
        target_disasm(&tb, &limit, &h->cpu);
        if (tb.icount == 0) {
            return 0;
        }
        tr_init(&tb);
        if (!tr_ir2_generate(&tb)) {
            tr_fini();
            return 0;
        }
        words = tr_ir2_assemble(h->code_buf);
        tr_fini();
        if (m->translated_tb != NULL) {
            m->translated_tb(&h->env, tb.pc);
        }
        entry = latr_tb_cache_insert(&h->tb_cache, tb.pc, key, h->code_buf,
                                     (uint32_t)words * 4u, tb.icount);
        if (entry == NULL) {
            /* Between TBs on the owning hart: safe to recycle the arena. */
            latr_tb_cache_reclaim_all(&h->tb_cache);
            entry = latr_tb_cache_insert(&h->tb_cache, tb.pc, key,
                                         h->code_buf, (uint32_t)words * 4u,
                                         tb.icount);
        }
        if (entry != NULL) {
            latr_tb_insn_map_rebase(h->code_buf, entry->code);
        }
        /*
         * The block has just written machine code into the arena (or into
         * code_buf on the uncached fallback). LoongArch keeps instructions in a
         * separate cache, and a VMM that is itself running on LoongArch also
         * has the host's translated-block cache to invalidate. Without this the
         * hart keeps executing whatever bytes used to live at that address: an
         * arena slot recycled for a different guest PC then runs an unrelated
         * block, which shows up as an impossible PC (0) or as a jump into the
         * translator's own C code.
         */
        latr_icache_sync();
    }
    code = entry != NULL ? (uint64_t)(uintptr_t)entry->code
                         : (uint64_t)(uintptr_t)h->code_buf;
    h->last_code = code;
    if (m->enter_tb == NULL) {
        return 0;
    }
    m->enter_tb(&h->env, (void *)(uintptr_t)code);
    if (h->env.wfi_pending) {
        latr_idle_advance(&h->env);
        h->env.wfi_pending = 0;
    }
    /* Guest traps are normal: the translator already redirected PC to the
     * trap vector, so just clear the latched indicators. Keep the first one
     * for diagnostics. */
    if ((h->env.exception || h->env.mem_fault_cause ||
         h->env.fetch_faulted) && !m->trapped) {
        m->first_trap_pc = pc_before;
        m->first_trap_cause = h->env.exception ? h->env.exception
                                               : h->env.mem_fault_cause;
        m->first_trap_bad = h->env.badaddr;
        m->first_trap_fetch = h->env.fetch_faulted;
        m->first_trap_mem = h->env.mem_fault_cause;
        m->trapped = true;
    }
    h->env.exception = 0;
    h->env.mem_fault_cause = 0;
    h->env.fetch_faulted = 0;
    m->last_pc = pc_before;
    m->dispatch_count++;
    h->tbs++;
    return 1;
}

int latr_machine_run(LatrMachine *m, uint32_t max_tbs)
{
    uint32_t executed = 0;

    if (m == NULL || !m->booted) {
        return 0;
    }
    while (executed < max_tbs) {
        bool online = false;

        /* The virtual clock is global: advance it once, then let every hart
         * latch its own CLINT comparators against it. */
        latr_system_tick(&m->sys, UINT64_C(1000));
        for (uint32_t i = 0; i < m->nharts; i++) {
            LatrMachineHart *h = &m->harts[i];

            if (!h->online) {
                continue;
            }
            online = true;
            if (!run_hart(m, h)) {
                m->failed = true;
                return 0;
            }
            executed++;
            if (m->sys.exit_request != 0) {
                return 1;
            }
            if (executed >= max_tbs) {
                break;
            }
        }
        if (!online) {
            return 1;
        }
    }
    return 2;
}
