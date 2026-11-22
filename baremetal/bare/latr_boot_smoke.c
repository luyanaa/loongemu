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
#include "mmu.h"
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

/* Minimal OpenSBI stand-in: set MPP=S, delegate all traps, mret to the image
 * at 0x80200000 with a0/a1 (hartid/DTB) untouched. */
static const uint32_t machine_firmware[] = {
    0x000012b7, /* lui t0, 1 */
    0x0012d293, /* srli t0, t0, 1 */
    0x3002a073, /* csrs mstatus, t0 */
    0xfff00293, /* addi t0, x0, -1 */
    0x30229073, /* csrw medeleg, t0 */
    0x22200293, /* addi t0, x0, 0x222 */
    0x30329073, /* csrw mideleg, t0 */
    0x802002b7, /* lui t0, 0x80200 */
    0x34129073, /* csrw mepc, t0 */
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

static void machine_fail(const char *where)
{
    latx_bare_uart_puts("latr-boot: machine fail at ");
    latx_bare_uart_puts(where);
    latx_bare_uart_puts(" pc=");
    puthex64((uint32_t)machine.harts[0].env.pc);
    latx_bare_uart_puts(" exc=");
    puthex64((uint32_t)machine.harts[0].env.exception);
    latx_bare_uart_puts(" fault=");
    puthex64((uint32_t)machine.harts[0].env.mem_fault_cause);
    latx_bare_uart_puts(" trap-pc=");
    puthex64((uint32_t)machine.first_trap_pc);
    latx_bare_uart_puts(" trap=");
    puthex64((uint32_t)machine.first_trap_cause);
    latx_bare_uart_puts(" fva=");
    puthex64((uint32_t)machine.harts[0].env.fetch_fault_va);
    latx_bare_uart_puts(" fcause=");
    puthex64((uint32_t)machine.harts[0].env.fetch_fault_cause);
    latx_bare_uart_puts(" bad=");
    puthex64((uint32_t)machine.first_trap_bad);
    latx_bare_uart_puts(" fetch=");
    puthex64((uint32_t)machine.first_trap_fetch);
    latx_bare_uart_puts(" mem=");
    puthex64((uint32_t)machine.first_trap_mem);
    latx_bare_uart_puts(" kfc=");
    puthex64((uint32_t)machine.kernel_fetch_cause);
    latx_bare_uart_puts(" kcs=");
    puthex64((uint32_t)machine.kernel_cpu_state);
    latx_bare_uart_puts(" kenv=");
    puthex64((uint32_t)machine.kernel_env);
    latx_bare_uart_puts(" kpriv=");
    puthex64((uint32_t)machine.kernel_priv);
    latx_bare_uart_puts(" priv=");
    puthex64((uint32_t)machine.harts[0].env.privilege);
    latx_bare_uart_puts(" mstatus=");
    puthex64((uint32_t)machine.harts[0].env.csr[0x300]);
    latx_bare_uart_puts(" uart=");
    latx_bare_uart_puts(machine.sys.uart_log);
    latx_bare_uart_puts("\n");
}

static void machine_probe(void)
{
    uint64_t host = 0;
    uint64_t phys = 0;
    int cause = latr_translate(&machine.harts[0].env, UINT64_C(0x80200000),
                               LATR_ACCESS_EXEC, 2, &host, &phys);

    latx_bare_uart_puts("probe cause=");
    puthex64((uint32_t)cause);
    latx_bare_uart_puts(" priv=");
    puthex64((uint32_t)machine.harts[0].env.privilege);
    latx_bare_uart_puts(" pmpcfg0=");
    puthex64((uint32_t)machine.harts[0].env.csr[0x3a0]);
    latx_bare_uart_puts(" pmpaddr0=");
    puthex64((uint32_t)machine.harts[0].env.csr[0x3b0]);
    latx_bare_uart_puts(" map=");
    puthex64((uint32_t)(uintptr_t)machine.harts[0].env.mem_map);
    machine.harts[0].env.privilege = LATR_RISCV_PRIV_S;
    cause = latr_translate(&machine.harts[0].env, UINT64_C(0x80200000),
                           LATR_ACCESS_EXEC, 2, &host, &phys);
    latx_bare_uart_puts(" s-cause=");
    puthex64((uint32_t)cause);
    latx_bare_uart_puts(" pmp=");
    puthex64((uint32_t)latr_pmp_check(&machine.harts[0].env,
                                      UINT64_C(0x80200000), 2,
                                      LATR_ACCESS_EXEC,
                                      LATR_RISCV_PRIV_S));
    latx_bare_uart_puts(" satp=");
    puthex64((uint32_t)machine.harts[0].env.csr[0x180]);
    machine.harts[0].env.privilege = LATR_RISCV_PRIV_M;
    latx_bare_uart_puts("\n");
}

static int run_machine_phase(void)
{
    uint64_t dtb_off;
    int rc;

    if (!latr_machine_init(&machine, ram, sizeof(ram), 1, machine_work,
                           sizeof(machine_work))) {
        latx_bare_uart_puts("latr-boot: machine fail init\n");
        return 0;
    }
    machine.harts[0].cpu.env_ptr = &machine.harts[0].env;
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
    if (!latr_machine_build_dtb(&machine)) {
        machine_fail("dtb");
        return 0;
    }
    if (!latr_machine_boot(&machine)) {
        machine_fail("boot");
        return 0;
    }
    machine_probe();
    rc = latr_machine_run(&machine, 256);
    if (rc != 1) {
        machine_fail("run");
        return 0;
    }
    dtb_off = machine.dtb_addr - LATR_SYSTEM_RAM_BASE;
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
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("latr-boot: done\n");
}
