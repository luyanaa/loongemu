/*
 * LATR system firmware smoke:
 *   board init -> ACPI/fw_cfg -> TB cache + translated guest -> UART -> exit.
 *
 * Two guest entries are exercised with the same raw image:
 *   1) RAM at 0x80000000;
 *   2) pflash code at 0x20000000 (instruction fetch goes through the MMIO bus).
 * The guest drives QEMU fw_cfg DMA (SELECT + READ) and verifies the signature.
 */
#include <stdint.h>
#include "latr.h"
#include "latr-system.h"
#include "latr-machine.h"
#include "translate.h"
#include "qemu/cpu-features.h"

extern void latr_enter_tb(void *env, void *code);
extern const unsigned char latr_boot_guest[];
extern const unsigned int latr_boot_guest_len;

#define UART0_THR ((volatile uint8_t *)0x1fe001e0)
void latx_bare_uart_putc(int c) { *UART0_THR = (uint8_t)c; }
void latx_bare_uart_puts(const char *s)
{
    while (*s) {
        latx_bare_uart_putc(*s++);
    }
}

static void puthex64(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 60; i >= 0; i -= 4) {
        latx_bare_uart_putc(digits[(v >> i) & 15]);
    }
}

__asm__(
".text\n"
".globl latr_enter_tb\n"
"latr_enter_tb:\n"
"  addi.d $sp, $sp, -16\n"
"  st.d   $ra, $sp, 0\n"
"  st.d   $s8, $sp, 8\n"
"  move   $s8, $a0\n"
"  jirl   $ra, $a1, 0\n"
"  ld.d   $ra, $sp, 0\n"
"  ld.d   $s8, $sp, 8\n"
"  addi.d $sp, $sp, 16\n"
"  jirl   $zero, $ra, 0\n");

void kmain(void);
__asm__(".text\n.globl _start\n_start:\n  la $sp, latr_boot_stack\n"
        "  lu12i.w $t0, 128\n  add.d $sp, $sp, $t0\n  bl kmain\n1: b 1b\n");
static uint8_t latr_boot_stack[512u << 10] __attribute__((aligned(16), used));

static uint8_t ram[16u << 20] __attribute__((aligned(4096)));
static uint8_t code_buf[1u << 18] __attribute__((aligned(4096)));
static uint8_t cache_arena[1u << 20] __attribute__((aligned(4096)));
static LatrSystem sys;
static LatrTbCache cache;
static TranslationBlock tb;
static CPUState cpu;
static uint64_t mapped_guest_pc;
static uint32_t mapped_raw_len;
static uint32_t pflash_tb_hits;
static uint32_t ram_cache_count;

static int run_guest(void)
{
    pflash_tb_hits = 0;
    for (int tbs = 0; tbs < 512; tbs++) {
        LatrTbKey key;
        LatrTbEntry *entry;
        uint64_t code;
        uint64_t pc_before;

        latr_system_update(&sys);
        if (latr_deliver_interrupt(&sys.env)) {
            sys.env.exception = 0;
        }
        pc_before = sys.env.pc;
        if (pc_before >= LATR_SYSTEM_PFLASH_CODE &&
            pc_before < LATR_SYSTEM_PFLASH_CODE +
                       LATR_SYSTEM_PFLASH_CODE_SIZE) {
            pflash_tb_hits++;
        }
        key = latr_tb_cache_key(&sys.env);
        entry = latr_tb_cache_lookup(&cache, sys.env.pc, key);
        if (entry == NULL) {
            int limit = 64;
            int words;
            memset(&tb, 0, sizeof(tb));
            tb.pc = sys.env.pc;
            tb.tc.ptr = code_buf;
            tb.cflags = 0;
            target_disasm(&tb, &limit, &cpu);
            if (tb.icount == 0) {
                return -1;
            }
            tr_init(&tb);
            if (!tr_ir2_generate(&tb)) {
                tr_fini();
                return -1;
            }
            words = tr_ir2_assemble(code_buf);
            entry = latr_tb_cache_insert(&cache, tb.pc, key, code_buf,
                                         (uint32_t)words * 4, tb.icount);
            if (entry != NULL) {
                latr_tb_insn_map_rebase(code_buf, entry->code);
            }
            tr_fini();
        }
        code = entry != NULL ? (uint64_t)(uintptr_t)entry->code
                             : (uint64_t)(uintptr_t)code_buf;
        latr_enter_tb(&sys.env, (void *)(uintptr_t)code);
        (void)latr_tb_insn_map_resolve((uintptr_t)code, &mapped_guest_pc,
                                       &mapped_raw_len);
        if (sys.env.sfence_pending) {
            latr_tb_shootdown_asid(&cache, (uint32_t)sys.env.sfence_asid);
            sys.env.sfence_pending = 0;
        }
        if (sys.env.fence_i_pending) {
            latr_tb_shootdown_all(&cache);
            sys.env.fence_i_pending = 0;
        }
        if (sys.env.wfi_pending) {
            latr_idle_advance(&sys.env);
            sys.env.wfi_pending = 0;
        }
        if (sys.env.exception || sys.env.mem_fault_cause ||
            sys.env.fetch_faulted) {
            latx_bare_uart_puts("\nexc=");
            puthex64(sys.env.exception);
            latx_bare_uart_puts(" fault=");
            puthex64(sys.env.mem_fault_cause);
            latx_bare_uart_puts(" va=");
            puthex64(sys.env.mem_va);
            latx_bare_uart_puts(" fetch=");
            puthex64(sys.env.fetch_fault_cause);
            latx_bare_uart_puts(" fva=");
            puthex64(sys.env.fetch_fault_va);
            latx_bare_uart_puts(" pc=");
            puthex64(sys.env.pc);
            sys.env.exception = 0;
            sys.env.mem_fault_cause = 0;
            sys.env.fetch_faulted = 0;
        }
        if (sys.exit_request) {
            return (int)sys.exit_request;
        }
    }
    return -1;
}

static int guest_ok(void)
{
    return sys.exit_request == 0x5555 &&
           sys.uart_log[0] == 'F' && sys.uart_log[1] == 'W' &&
           sys.uart_log[2] == '\0';
}

static int prepare_board(void)
{
    uint64_t entry = 0;

    if (!latr_system_init(&sys, ram, sizeof(ram), 0)) {
        return 0;
    }
    if (!latr_system_build_acpi(&sys, &entry) ||
        memcmp(ram + (entry - sys.ram_base), "RSD PTR ", 8) != 0) {
        return 0;
    }
    latr_tr_data_init(&sys.env);
    latr_tb_cache_init(&cache, cache_arena, sizeof(cache_arena));
    latr_tb_cache_register(&cache);
    memset(&cpu, 0, sizeof(cpu));
    cpu.env_ptr = &sys.env;
    return 1;
}

/* --- QEMU-virt-style machine boot chain --- */

static LatrMachine machine;
static uint8_t machine_work[256u << 10] __attribute__((aligned(16)));
static uint8_t machine_disk[8u << 10] __attribute__((aligned(512)));
static uint8_t machine_initrd[1024] __attribute__((aligned(16)));

static const char machine_disk_sig[] = "LATR-DISK-0";
static const char machine_out_sig[] = "LATR-OUT-1";
static uint32_t machine_saved_dtb;
static int machine_saved_disk;
static int machine_saved_initrd;
static int machine_saved_smp;

#define VIO_TEST_BASE    UINT64_C(0x80800000)
#define VIO_DESC_GPA     (VIO_TEST_BASE + 0x000)
#define VIO_AVAIL_GPA    (VIO_TEST_BASE + 0x100)
#define VIO_USED_GPA     (VIO_TEST_BASE + 0x200)
#define VIO_HEADER_GPA   (VIO_TEST_BASE + 0x400)
#define VIO_DATA_GPA     (VIO_TEST_BASE + 0x500)
#define VIO_STATUS_GPA   (VIO_TEST_BASE + 0x700)

static uint8_t *guest_ptr(uint64_t gpa)
{
    return ram + (gpa - LATR_SYSTEM_RAM_BASE);
}

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void store_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void store_le64(uint8_t *p, uint64_t v)
{
    store_le32(p, (uint32_t)v);
    store_le32(p + 4, (uint32_t)(v >> 32));
}

static uint8_t *find_bytes(const uint8_t *hay, uint32_t hay_len,
                           const char *needle)
{
    uint32_t n = (uint32_t)strlen(needle);
    uint32_t i;

    if (n == 0 || n > hay_len) {
        return NULL;
    }
    for (i = 0; i + n <= hay_len; i++) {
        if (memcmp(hay + i, needle, n) == 0) {
            return (uint8_t *)(uintptr_t)(hay + i);
        }
    }
    return NULL;
}

static uint32_t vio_read32(uint64_t offset)
{
    uint64_t value = 0;

    if (loongemu_mmio_bus_read(&machine.sys.bus,
                               LATR_MACHINE_VIRTIO_BASE + offset, 4,
                               &value) != 0) {
        return 0xffffffffu;
    }
    return (uint32_t)value;
}

static void vio_write32(uint64_t offset, uint32_t value)
{
    loongemu_mmio_bus_write(&machine.sys.bus,
                            LATR_MACHINE_VIRTIO_BASE + offset, 4, value);
}

/* One virtio-blk request chain: header + 512-byte payload + status byte. */
static void vio_request(uint32_t type, uint64_t sector, int payload_in)
{
    uint8_t *desc = guest_ptr(VIO_DESC_GPA);
    uint8_t *avail = guest_ptr(VIO_AVAIL_GPA);
    uint8_t *used = guest_ptr(VIO_USED_GPA);
    uint8_t *header = guest_ptr(VIO_HEADER_GPA);
    uint16_t avail_idx = (uint16_t)(avail[2] | (uint16_t)avail[3] << 8);
    uint32_t slot = (uint32_t)avail_idx % 8u;

    store_le64(desc + 0, VIO_HEADER_GPA);
    store_le32(desc + 8, 16);
    store_le16(desc + 12, 1);
    store_le16(desc + 14, 1);

    store_le64(desc + 16, VIO_DATA_GPA);
    store_le32(desc + 24, 512);
    store_le16(desc + 28, (uint16_t)(payload_in ? 3 : 1));
    store_le16(desc + 30, 2);

    store_le64(desc + 32, VIO_STATUS_GPA);
    store_le32(desc + 40, 1);
    store_le16(desc + 44, 2);
    store_le16(desc + 46, 0);

    store_le32(header, type);
    store_le32(header + 4, 0);
    store_le64(header + 8, sector);

    avail[0] = 0;
    avail[1] = 0;
    store_le16(avail + 2, (uint16_t)(avail_idx + 1));
    store_le16(avail + 4 + 2 * slot, 0);
    if (avail_idx == 0) {
        used[0] = 0;
        used[1] = 0;
        store_le16(used + 2, 0);
    }
    vio_write32(0x050, 0);
}

static int run_virtio_phase(void)
{
    uint8_t *used = guest_ptr(VIO_USED_GPA);
    uint8_t *data = guest_ptr(VIO_DATA_GPA);
    uint8_t *status = guest_ptr(VIO_STATUS_GPA);

    if (vio_read32(0x000) != 0x74726976u || vio_read32(0x004) != 2u ||
        vio_read32(0x008) != 2u) {
        return 0;
    }
    vio_write32(0x070, 0);
    vio_write32(0x070, 1 | 2);
    vio_write32(0x014, 1);
    if (vio_read32(0x010) != 1u) {
        return 0;
    }
    vio_write32(0x024, 1);
    vio_write32(0x020, 1);
    vio_write32(0x070, 1 | 2 | 8);
    if ((vio_read32(0x070) & 8u) == 0) {
        return 0;
    }

    vio_write32(0x030, 0);
    if (vio_read32(0x034) != LOONGEMU_VIRTIO_BLK_QUEUE_MAX) {
        return 0;
    }
    vio_write32(0x038, 8);
    vio_write32(0x080, (uint32_t)VIO_DESC_GPA);
    vio_write32(0x084, (uint32_t)(VIO_DESC_GPA >> 32));
    vio_write32(0x090, (uint32_t)VIO_AVAIL_GPA);
    vio_write32(0x094, (uint32_t)(VIO_AVAIL_GPA >> 32));
    vio_write32(0x0a0, (uint32_t)VIO_USED_GPA);
    vio_write32(0x0a4, (uint32_t)(VIO_USED_GPA >> 32));
    vio_write32(0x044, 1);
    vio_write32(0x070, 1 | 2 | 8 | 4);

    /* Read sector 0. */
    vio_request(0, 0, 1);
    if ((used[2] | (uint16_t)used[3] << 8) != 1 || *status != 0 ||
        memcmp(data, machine_disk_sig, strlen(machine_disk_sig)) != 0) {
        return 0;
    }
    if (!machine.virtio_blk.irq_level || vio_read32(0x060) != 1u) {
        return 0;
    }
    vio_write32(0x064, 1);
    if (machine.virtio_blk.irq_level != 0) {
        return 0;
    }

    /* Write sector 1 and read it back from the backing store. */
    memset(data, 0, 512);
    memcpy(data, machine_out_sig, strlen(machine_out_sig));
    vio_request(1, 1, 0);
    if ((used[2] | (uint16_t)used[3] << 8) != 2 || *status != 0 ||
        memcmp(machine_disk + 512, machine_out_sig,
               strlen(machine_out_sig)) != 0) {
        return 0;
    }
    vio_write32(0x064, 1);
    return 1;
}

/* Minimal OpenSBI stand-in: set MPP=S, delegate all traps, mret to the image
 * at 0x80200000 with a0/a1 (hartid/DTB) untouched.
 * lui sign-extends bit 31 on RV64, so clear the upper 32 bits explicitly
 * before writing mepc; otherwise the guest would jump to 0xFFFFFFFF80200000. */
static const uint32_t machine_firmware[] = {
    0x000012b7, /* lui t0, 1 */
    0x0012d293, /* srli t0, t0, 1 */
    0x3002a073, /* csrs mstatus, t0 */
    0xfff00293, /* addi t0, x0, -1 */
    0x30229073, /* csrw medeleg, t0 */
    0x22200293, /* addi t0, x0, 0x222 */
    0x30329073, /* csrw mideleg, t0 */
    0x802002b7, /* lui t0, 0x80200 */
    0x02029293, /* slli t0, t0, 32 */
    0x0202d293, /* srli t0, t0, 32 */
    0x34129073, /* csrw mepc, t0 */
    /* Sample the virtio-mmio magic through a guest load and stash it in RAM
     * at 0x80001000, so the host can prove the MMIO path is live. */
    0x100012b7, /* lui t0, 0x10001 */
    0x0002a303, /* lw t1, 0(t0) */
    0x800013b7, /* lui t2, 0x80001 */
    0x02039393, /* slli t2, t2, 32 */
    0x0203d393, /* srli t2, t2, 32 */
    0x0063a023, /* sw t1, 0(t2) */
    0x30200073, /* mret */
};

/* S-mode payload: print 'K' on the UART, then write the exit device. */
static const uint32_t machine_kernel[] = {
    0x100002b7, /* lui t0, 0x10000 */
    0x04b00313, /* addi t1, x0, 'K' */
    0x00628023, /* sb t1, 0(t0) */
    0x001002b7, /* lui t0, 0x100 */
    0x00100313, /* addi t1, x0, 1 */
    0x0062a023, /* sw t1, 0(t0) */
    0x0000006f, /* jal x0, 0 */
};

static void machine_enter_tb(void *env, void *code)
{
    latr_enter_tb(env, code);
}

/* SMP payload: both harts print 'K'; hart 1 also writes the exit device and
 * hart 0 spins, so hart 0 gets a dispatch in the same round before exit. */
static const uint32_t machine_smp_kernel[] = {
    0x100002b7, /* lui t0, 0x10000 */
    0x04b00313, /* addi t1, x0, 'K' */
    0x00628023, /* sb t1, 0(t0) */
    0x00050863, /* beq a0, x0, 16 */
    0x001002b7, /* lui t0, 0x100 */
    0x00100313, /* addi t1, x0, 1 */
    0x0062a023, /* sw t1, 0(t0) */
    0x0000006f, /* jal x0, 0 */
};

static void machine_fail(const char *where)
{
    latx_bare_uart_puts("latr-boot: machine fail at ");
    latx_bare_uart_puts(where);
    latx_bare_uart_puts(" pc=");
    puthex64((uint32_t)machine.harts[0].env.pc);
    latx_bare_uart_puts(" trap-pc=");
    puthex64((uint32_t)machine.first_trap_pc);
    latx_bare_uart_puts(" trap=");
    puthex64((uint32_t)machine.first_trap_cause);
    latx_bare_uart_puts(" bad=");
    puthex64((uint32_t)machine.first_trap_bad);
    latx_bare_uart_puts(" fetch=");
    puthex64((uint32_t)machine.first_trap_fetch);
    latx_bare_uart_puts(" mem=");
    puthex64((uint32_t)machine.first_trap_mem);
    latx_bare_uart_puts(" lastpc=");
    puthex64((uint32_t)machine.last_pc);
    latx_bare_uart_puts(" n=");
    puthex64((uint32_t)machine.dispatch_count);
    latx_bare_uart_puts(" fw=");
    puthex64((uint32_t)machine.firmware_entry);
    latx_bare_uart_puts(" ke=");
    puthex64((uint32_t)machine.kernel_entry);
    latx_bare_uart_puts(" booted=");
    puthex64((uint32_t)machine.booted);
    latx_bare_uart_puts(" priv=");
    puthex64((uint32_t)machine.harts[0].env.privilege);
    latx_bare_uart_puts(" uart=");
    latx_bare_uart_puts(machine.sys.uart_log);
    latx_bare_uart_puts(" exit=");
    puthex64((uint32_t)machine.sys.exit_request);
    latx_bare_uart_puts(" a0=");
    puthex64((uint32_t)machine.harts[0].env.gpr[10]);
    latx_bare_uart_puts(" a1=");
    puthex64((uint32_t)machine.harts[1].env.gpr[10]);
    latx_bare_uart_puts(" tb0=");
    puthex64((uint32_t)machine.harts[0].tbs);
    latx_bare_uart_puts(" tb1=");
    puthex64((uint32_t)machine.harts[1].tbs);
    latx_bare_uart_puts(" p0=");
    puthex64((uint32_t)machine.harts[0].env.privilege);
    latx_bare_uart_puts(" p1=");
    puthex64((uint32_t)machine.harts[1].env.privilege);
    latx_bare_uart_puts("\n");
}

static int run_machine_phase(void)
{
    uint64_t dtb_off;
    uint8_t *dtb;
    int rc;

    if (!latr_machine_init(&machine, ram, sizeof(ram), 1, machine_work,
                           sizeof(machine_work))) {
        latx_bare_uart_puts("latr-boot: machine fail init\n");
        return 0;
    }
    latr_machine_set_enter_tb(&machine, machine_enter_tb);
    if (!latr_machine_set_bootargs(&machine,
                                   "console=ttyS0 earlycon=uart8250,mmio,"
                                   "0x10000000 keep_bootcon")) {
        latx_bare_uart_puts("latr-boot: machine fail bootargs\n");
        return 0;
    }
    if (!latr_machine_load_firmware(&machine,
                                    (const uint8_t *)machine_firmware,
                                    sizeof(machine_firmware))) {
        machine_fail("firmware");
        return 0;
    }
    if (!latr_machine_load_kernel(&machine, (const uint8_t *)machine_kernel,
                                  sizeof(machine_kernel))) {
        machine_fail("kernel");
        return 0;
    }

    /*
     * "No -drive" degradation: handing over no image must succeed and leave the
     * board without a block device -- no MMIO window, no PLIC line, and no virtio
     * node in a DTB built from that state. This is the configuration a guest gets
     * when it is started without a disk, so it has to be a supported path rather
     * than an error the caller has to special-case.
     */
    if (!latr_machine_attach_disk(&machine, NULL, 0) || machine.disk_attached ||
        !latr_machine_build_dtb(&machine)) {
        machine_fail("no-disk");
        return 0;
    }
    /*
     * Note: the root node's compatible string is "riscv-virtio", so the node
     * itself is identified by "virtio,mmio" / "virtio_mmio@", not by "virtio".
     */
    if (find_bytes(guest_ptr(machine.dtb_addr), (uint32_t)machine.dtb_size,
                   "virtio,mmio") != NULL ||
        find_bytes(guest_ptr(machine.dtb_addr), (uint32_t)machine.dtb_size,
                   "virtio_mmio@") != NULL) {
        machine_fail("no-disk-dtb");
        return 0;
    }

    memset(machine_disk, 0, sizeof(machine_disk));
    memcpy(machine_disk, machine_disk_sig, strlen(machine_disk_sig));
    if (!latr_machine_attach_disk(&machine, machine_disk,
                                  sizeof(machine_disk))) {
        machine_fail("disk");
        return 0;
    }

    memset(machine_initrd, 0, sizeof(machine_initrd));
    memcpy(machine_initrd, "LATR-INITRD", 11);
    if (!latr_machine_load_initrd(&machine, machine_initrd,
                                  sizeof(machine_initrd))) {
        machine_fail("initrd");
        return 0;
    }

    if (!latr_machine_build_dtb(&machine)) {
        machine_fail("dtb");
        return 0;
    }
    if (!latr_machine_boot(&machine)) {
        machine_fail("boot");
        return 0;
    }
    rc = latr_machine_run(&machine, 256);
    if (rc != 1) {
        machine_fail("run");
        return 0;
    }
    dtb_off = machine.dtb_addr - LATR_SYSTEM_RAM_BASE;
    dtb = guest_ptr(machine.dtb_addr);
    if (machine.sys.exit_request != 1) {
        machine_fail("exit");
        return 0;
    }
    if (machine.sys.uart_log[0] != 'K') {
        machine_fail("uart");
        return 0;
    }
    if (machine.harts[0].env.gpr[10] != 0 ||
        machine.harts[0].env.gpr[11] != machine.dtb_addr ||
        machine.harts[0].env.privilege != LATR_RISCV_PRIV_S) {
        machine_fail("state");
        return 0;
    }
    if (ram[dtb_off + 0] != 0xd0 || ram[dtb_off + 1] != 0x0d ||
        ram[dtb_off + 2] != 0xfe || ram[dtb_off + 3] != 0xed ||
        machine.dtb_size == 0) {
        machine_fail("dtb-magic");
        return 0;
    }
    /* Host-fault dispatcher: attribute a native PC back to the guest TB that
     * owns it, restore the guest PC/cause, and drop the stale native block. */
    {
        LatrMachineFaultInfo fault;
        uint64_t expected_pc = machine.last_pc;

        if (!latr_machine_host_fault(&machine, 0,
                                     (uintptr_t)machine.harts[0].last_code,
                                     13, UINT64_C(0xdead), &fault) ||
            !fault.resolved || fault.guest_pc != expected_pc ||
            machine.harts[0].env.pc != expected_pc ||
            machine.harts[0].env.exception != 13 ||
            machine.harts[0].env.badaddr != UINT64_C(0xdead)) {
            machine_fail("host-fault");
            return 0;
        }
    }
    /* The guest fetched the virtio magic through an MMIO load. */
    if (load_le32(guest_ptr(LATR_SYSTEM_RAM_BASE + 0x1000)) != 0x74726976u) {
        machine_fail("mmio-magic");
        return 0;
    }
    /* Disk: virtio-mmio block request/response and interrupt line. */
    if (!run_virtio_phase()) {
        machine_fail("virtio");
        return 0;
    }
    /* Initrd: below the DTB, above the kernel, bytes intact, described. */
    if (machine.initrd_start < machine.kernel_end ||
        machine.initrd_end > machine.dtb_addr ||
        machine.initrd_end - machine.initrd_start != sizeof(machine_initrd) ||
        memcmp(guest_ptr(machine.initrd_start), machine_initrd,
               sizeof(machine_initrd)) != 0) {
        machine_fail("initrd-range");
        return 0;
    }
    {
        uint32_t flags = 0;

        if (find_bytes(dtb, (uint32_t)machine.dtb_size,
                       "virtio,mmio") != NULL) {
            flags |= 1u;
        }
        if (find_bytes(dtb, (uint32_t)machine.dtb_size,
                       "linux,initrd-start") != NULL) {
            flags |= 2u;
        }
        if (find_bytes(dtb, (uint32_t)machine.dtb_size,
                       "riscv,plic0") != NULL) {
            flags |= 4u;
        }
        if (find_bytes(dtb, (uint32_t)machine.dtb_size,
                       "rv64imafdc") != NULL) {
            flags |= 8u;
        }
        if (flags != 0xfu) {
            latx_bare_uart_puts("latr-boot: dtb-flags=");
            puthex64((uint32_t)flags);
            latx_bare_uart_puts("\n");
            machine_fail("dtb-content");
            return 0;
        }
    }
    machine_saved_dtb = (uint32_t)machine.dtb_size;
    machine_saved_disk = machine.disk_attached ? 1 : 0;
    machine_saved_initrd = machine.initrd_loaded ? 1 : 0;
    latr_machine_fini(&machine);
    return 1;
}

/*
 * Proves that CLINT state is per hart: a timer expiry or an IPI aimed at hart 1
 * must never appear in hart 0's MIP. Runs before boot so the probe cannot
 * disturb the payload.
 */
static int probe_per_hart_interrupts(void)
{
    CPUArchState *h0 = &machine.harts[0].env;
    CPUArchState *h1 = &machine.harts[1].env;

    /* Power-on comparators are far in the future, not 0. */
    if (machine.sys.clint.mtimecmp[0] != UINT64_MAX ||
        machine.sys.clint.mtimecmp[1] != UINT64_MAX) {
        return 0;
    }
    /* Expire only hart 1's comparator. */
    latr_system_mmio_write(&machine.sys, LATR_SYSTEM_CLINT_BASE + 0x4008, 8, 0);
    latr_system_tick(&machine.sys, 1000);
    latr_system_update_hart(&machine.sys, h1, 1);
    latr_system_update_hart(&machine.sys, h0, 0);
    if ((h1->csr[0x344] & (UINT64_C(1) << 7)) == 0) {
        return 0; /* hart 1 MTIP missing */
    }
    if ((h0->csr[0x344] & (UINT64_C(1) << 7)) != 0) {
        return 0; /* hart 0 saw hart 1's timer */
    }
    /* Software interrupt aimed at hart 1 only. */
    latr_system_mmio_write(&machine.sys, LATR_SYSTEM_CLINT_BASE + 0x4, 4, 1);
    latr_system_update_hart(&machine.sys, h1, 1);
    latr_system_update_hart(&machine.sys, h0, 0);
    if ((h1->csr[0x344] & (UINT64_C(1) << 3)) == 0) {
        return 0; /* IPI not delivered */
    }
    if ((h0->csr[0x344] & (UINT64_C(1) << 3)) != 0) {
        return 0; /* IPI leaked to hart 0 */
    }
    /* Restore a quiet board for the payload. */
    latr_system_mmio_write(&machine.sys, LATR_SYSTEM_CLINT_BASE + 0x4, 4, 0);
    latr_system_mmio_write(&machine.sys, LATR_SYSTEM_CLINT_BASE + 0x4008, 8,
                           UINT64_MAX);
    latr_system_update_hart(&machine.sys, h1, 1);
    if ((h1->csr[0x344] & ((UINT64_C(1) << 3) | (UINT64_C(1) << 7))) != 0) {
        return 0; /* probe left interrupts pending */
    }
    return 1;
}

/* Phase 4: two harts through the same machine. Dispatch is cooperative
 * (round-robin in one host thread), which is enough to prove that contexts,
 * TB caches and PVIR caches are per-hart and that hartid/DTB arguments differ. */
static int run_smp_phase(void)
{
    uint8_t *dtb;
    int rc;

    if (!latr_machine_init(&machine, ram, sizeof(ram), 2, machine_work,
                           sizeof(machine_work))) {
        machine_fail("smp-init");
        return 0;
    }
    if (!probe_per_hart_interrupts()) {
        machine_fail("smp-irq");
        return 0;
    }
    latr_machine_set_enter_tb(&machine, machine_enter_tb);
    if (!latr_machine_set_bootargs(&machine, "console=ttyS0")) {
        machine_fail("smp-bootargs");
        return 0;
    }
    if (!latr_machine_load_firmware(&machine,
                                    (const uint8_t *)machine_firmware,
                                    sizeof(machine_firmware)) ||
        !latr_machine_load_kernel(&machine, (const uint8_t *)machine_smp_kernel,
                                  sizeof(machine_smp_kernel))) {
        machine_fail("smp-image");
        return 0;
    }
    memset(machine_disk, 0, sizeof(machine_disk));
    memcpy(machine_disk, machine_disk_sig, strlen(machine_disk_sig));
    if (!latr_machine_attach_disk(&machine, machine_disk,
                                  sizeof(machine_disk))) {
        machine_fail("smp-disk");
        return 0;
    }
    if (!latr_machine_build_dtb(&machine) || !latr_machine_boot(&machine)) {
        machine_fail("smp-boot");
        return 0;
    }
    rc = latr_machine_run(&machine, 256);
    dtb = guest_ptr(machine.dtb_addr);
    if (rc != 1 || machine.sys.exit_request != 1 ||
        machine.harts[0].env.gpr[10] != 0 ||
        machine.harts[1].env.gpr[10] != 1 ||
        machine.harts[0].env.privilege != LATR_RISCV_PRIV_S ||
        machine.harts[1].env.privilege != LATR_RISCV_PRIV_S ||
        machine.harts[0].tbs == 0 || machine.harts[1].tbs == 0 ||
        machine.sys.uart_log[0] != 'K' || machine.sys.uart_log[1] != 'K') {
        machine_fail("smp-state");
        return 0;
    }
    if (machine.harts[0].ctx == machine.harts[1].ctx ||
        machine.harts[0].ctx->pvir_cache == machine.harts[1].ctx->pvir_cache ||
        &machine.harts[0].tb_cache == &machine.harts[1].tb_cache) {
        machine_fail("smp-context");
        return 0;
    }
    /* Borrowing a hart's context and putting the caller's back is what a host
     * fault dispatcher does; it must be a round trip. */
    {
        LatrTranslatorContext *outer = latr_context_current();
        LatrTranslatorContext *saved = latr_context_bind(machine.harts[1].ctx);

        if (latr_context_current() != machine.harts[1].ctx || saved != outer) {
            machine_fail("smp-bind");
            return 0;
        }
        latr_context_restore(saved);
        if (latr_context_current() != outer) {
            machine_fail("smp-restore");
            return 0;
        }
    }
    if (find_bytes(dtb, (uint32_t)machine.dtb_size, "cpu@1") == NULL) {
        machine_fail("smp-dtb");
        return 0;
    }
    latr_machine_fini(&machine);
    return 1;
}

/*
 * Phase 5: no firmware at all. The machine must install the S-mode state that
 * OpenSBI would have left behind (MPP=S, delegated SSI/STI/SEI, mcounteren, and
 * a bootable stvec), otherwise the first rdtime/CSR access traps.
 */
static int run_direct_smode_phase(void)
{
    int rc;

    if (!latr_machine_init(&machine, ram, sizeof(ram), 1, machine_work,
                           sizeof(machine_work))) {
        machine_fail("direct-init");
        return 0;
    }
    latr_machine_set_enter_tb(&machine, machine_enter_tb);
    if (!latr_machine_set_bootargs(&machine,
                                   "console=ttyS0 earlycon=uart8250,mmio,0x10000000")) {
        machine_fail("direct-bootargs");
        return 0;
    }
    if (!latr_machine_load_kernel(&machine, (const uint8_t *)machine_kernel,
                                  sizeof(machine_kernel)) ||
        !latr_machine_build_dtb(&machine) || !latr_machine_boot(&machine)) {
        machine_fail("direct-boot");
        return 0;
    }
    {
        CPUArchState *h = &machine.harts[0].env;

        if (h->privilege != LATR_RISCV_PRIV_S) {
            machine_fail("direct-priv");
            return 0;
        }
        if (((h->csr[0x300] >> 11) & 3u) != 1u) {
            machine_fail("direct-mpp");
            return 0;
        }
        if (h->csr[0x303] != UINT64_C(0x222) || h->csr[0x304] != UINT64_C(0x222)) {
            machine_fail("direct-deleg");
            return 0;
        }
        if (h->csr[0x306] != 7) {
            machine_fail("direct-mcounteren");
            return 0;
        }
        if (h->gpr[11] != machine.dtb_addr || h->gpr[12] != 0) {
            machine_fail("direct-args");
            return 0;
        }
    }
    rc = latr_machine_run(&machine, 64);
    if (rc != 1 || machine.sys.exit_request != 1 ||
        machine.sys.uart_log[0] != 'K' ||
        machine.harts[0].env.privilege != LATR_RISCV_PRIV_S) {
        machine_fail("direct-run");
        return 0;
    }
    latr_machine_fini(&machine);
    return 1;
}

void kmain(void)
{
    latx_bare_uart_puts("latr-boot: begin\n");
    {
        uint64_t euen;
        __asm__ volatile("csrrd %0, 0x2" : "=r"(euen));
        euen |= 1u;
        if (latx_cpu_has_lsx()) {
            euen |= 2u;
        }
        if (latx_cpu_has_lasx()) {
            euen |= 4u;
        }
        __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    }

    /* Phase 1: raw image executed from RAM. */
    if (!prepare_board()) {
        latx_bare_uart_puts("latr-boot: FAIL setup ram\n");
        return;
    }
    if (latr_boot_guest_len > sizeof(ram) ||
        !latr_boot_guest_len) {
        latx_bare_uart_puts("latr-boot: FAIL guest size\n");
        return;
    }
    memcpy(ram, latr_boot_guest, latr_boot_guest_len);
    sys.env.pc = LATR_SYSTEM_RAM_BASE;
    {
        int rc = run_guest();
        if (rc != 0x5555 || !guest_ok()) {
            latx_bare_uart_puts("latr-boot: FAIL ram guest rc=");
            puthex64((uint32_t)rc);
            latx_bare_uart_puts(" pc=");
            puthex64(sys.env.pc);
            latx_bare_uart_puts(" uart=");
            latx_bare_uart_puts(sys.uart_log);
            latx_bare_uart_puts("\n");
            return;
        }
    }
    ram_cache_count = (uint32_t)cache.count;

    /* Phase 2: the same image executes in place from pflash through MMIO. */
    if (!prepare_board()) {
        latx_bare_uart_puts("latr-boot: FAIL setup pflash\n");
        return;
    }
    if (!latr_system_load_firmware(&sys, latr_boot_guest,
                                   latr_boot_guest_len)) {
        latx_bare_uart_puts("latr-boot: FAIL pflash load\n");
        return;
    }
    if (run_guest() != 0x5555 || !guest_ok() || pflash_tb_hits == 0) {
        latx_bare_uart_puts("latr-boot: FAIL pflash guest\n");
        return;
    }

    /* Phase 3: M-mode firmware -> S-mode payload through the virt machine,
     * with a generated DTB passed in a1. */
    if (!run_machine_phase()) {
        latx_bare_uart_puts("latr-boot: FAIL machine\n");
        return;
    }
    /* Phase 4: two harts, per-hart contexts/caches, hartid and DTB args. */
    if (!run_smp_phase()) {
        machine_saved_smp = 0;
        latx_bare_uart_puts("latr-boot: FAIL smp\n");
        return;
    }
    machine_saved_smp = 1;

    /* Phase 5: S-mode without firmware (Linux Image booted directly). */
    if (!run_direct_smode_phase()) {
        latx_bare_uart_puts("latr-boot: FAIL direct\n");
        return;
    }

    latx_bare_uart_puts("latr-boot: PASS uart=");
    latx_bare_uart_puts(sys.uart_log);
    latx_bare_uart_puts(" ram-cache=");
    {
        char digits[2] = { (char)('0' + (ram_cache_count & 7)), 0 };
        latx_bare_uart_puts(digits);
    }
    latx_bare_uart_puts(" pflash-tbs=");
    {
        char digits[2] = { (char)('0' + (pflash_tb_hits & 7)), 0 };
        latx_bare_uart_puts(digits);
    }
    latx_bare_uart_puts(" map=");
    {
        char digits[2] = { (char)('0' + (mapped_raw_len & 7)), 0 };
        latx_bare_uart_puts(digits);
    }
    latx_bare_uart_puts(" machine=");
    latx_bare_uart_puts(machine_saved_dtb != 0 ? "1" : "0");
    latx_bare_uart_puts(" disk=");
    latx_bare_uart_puts(machine_saved_disk ? "1" : "0");
    latx_bare_uart_puts(" initrd=");
    latx_bare_uart_puts(machine_saved_initrd ? "1" : "0");
    latx_bare_uart_puts(" smp=");
    latx_bare_uart_puts(machine_saved_smp ? "1" : "0");
    latx_bare_uart_puts(" dtb=");
    puthex64(machine_saved_dtb);
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("latr-boot: done\n");
}
