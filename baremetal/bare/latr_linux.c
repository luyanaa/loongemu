/*
 * RISC-V Linux boot attempt through the LATR machine.
 *
 * Loads a distribution kernel image (flat `Image`, including the PE-wrapped
 * form AOSC ships) at +2 MiB, builds the DTB, starts the hart in S-mode without
 * firmware, and services the kernel's SBI calls from the machine. Success is
 * *not* asserted: the harness reports the console text, the dispatch count and
 * where the hart stopped, which is what the next step needs. The kernel is not
 * vendored - the image comes from $(LATR_LINUX_IMAGE), so this target is
 * local-only and never part of CI.
 *
 * Why 1 GiB of guest RAM: the kernel is ~61 MiB of text+data and Linux sizes
 * its own page tables, mem_map and vmalloc area from the RAM it is given. A
 * 128 MiB machine fails inside mem_init, which looks like a translator bug in
 * the log; 1 GiB is the smallest size that reaches userspace on this image.
 */
#include <string.h>
#include "latr.h"
#include "latr-system.h"
#include "latr-machine.h"
#include "translate.h"

extern void latr_enter_tb(void *env, void *code);
extern const unsigned char latr_linux_image[];
extern const unsigned char latr_linux_image_end[];

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

static void put_dec(uint64_t v)
{
    char buf[24];
    int n = 0;

    if (v == 0) {
        latx_bare_uart_putc('0');
        return;
    }
    while (v != 0 && n < (int)sizeof(buf)) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n != 0) {
        latx_bare_uart_putc(buf[--n]);
    }
}


/* The trampoline the machine calls for every translated block: it saves the
 * harness's return address and the CPU-state register, jumps into generated
 * code with `$s8` bound to the guest env, and comes back on the block's
 * `jirl $zero, $ra, 0`. Without the `$s8` bind every env access in generated
 * code would address the harness's own state.
 *
 * `$a1` (the code pointer) is stashed in last_entered before the jump: the
 * machine's guest PC is not enough to name a wedged block, because the same PC
 * has several arena copies over the life of a boot. */
__asm__(
".text\n"
".globl latr_enter_tb\n"
"latr_enter_tb:\n"
"  addi.d $sp, $sp, -16\n"
"  st.d   $ra, $sp, 0\n"
"  st.d   $s8, $sp, 8\n"
"  la.local $t6, last_entered\n"
"  st.d   $a1, $t6, 0\n"
"  move   $s8, $a0\n"
"  jirl   $ra, $a1, 0\n"
"  ld.d   $ra, $sp, 0\n"
"  ld.d   $s8, $sp, 8\n"
"  addi.d $sp, $sp, 16\n"
"  jirl   $zero, $ra, 0\n");

/* The backing store lives in the LoongArch virt machine's highmem window,
 * which starts at 0x80000000 and is 768 MiB with -m 1G (the lowmem window
 * holds the VMM image itself). */
#define GUEST_RAM_SIZE (768u << 20)
#define STEP_BUDGET 20000u
/* How many translated blocks between progress lines: enough to be cheap, small
 * enough that a stall is reported at the PC where it happened. */
#define PROGRESS_EVERY 20000u

static uint8_t ram[GUEST_RAM_SIZE] __attribute__((aligned(4096),
                                                 section(".guest_ram")));
static uint8_t machine_work[8u << 20] __attribute__((aligned(4096)));
static LatrMachine machine;
static uint64_t last_reported;
static uint32_t console_mirrored;
static uint64_t last_translated;
/* Written from the enter_tb trampoline (inline asm), so it must not be static:
 * the asm names it with a relocation the linker resolves by symbol. */
uint64_t last_entered;

/* Ring of the guest PCs that were translated, so a wedge names the last few
 * blocks instead of only the one that never returned. */
#define PC_RING 48
static uint64_t pc_ring[PC_RING];
static uint32_t pc_ring_at;


/*
 * Host-CPU exception handler.
 *
 * The harness is a LoongArch program that also generates LoongArch code, and
 * its own faults used to go nowhere: EENTRY was 0, so an illegal instruction or
 * an FP operation with the unit disabled trapped to address 0, "executed"
 * whatever garbage lived there, and looped without printing anything. From
 * outside that is indistinguishable from a slow guest. MACHINE.md §7.3 records
 * the same trap from the OpenSBI work.
 *
 * Report ERA/ESTAT/BADV plus the code pointer the trampoline last entered and
 * stop, so the wedged block can be found instead of guessed.
 * ESTAT.Ecode 13 is INE (instruction non-existent), 15 is FPD (FP unit).
 */
volatile uint64_t host_fault_era;
volatile uint64_t host_fault_estat;
volatile uint64_t host_fault_badv;
/* Host register file at the fault. The wedged block is generated code, so its
 * own `$ra`/`$sp`/`$s8` are the only way to tell "returned to the trampoline"
 * from "jumped into nothing". */
volatile uint64_t host_fault_ra;
volatile uint64_t host_fault_sp;
volatile uint64_t host_fault_s8;
volatile uint64_t host_fault_r21;
volatile uint64_t host_fault_a0;

void latr_host_fault_report(void)
{
    uint64_t era = host_fault_era;
    const uint32_t *code = NULL;

    if (era >= (uint64_t)(uintptr_t)machine_work &&
        era + 32 <= (uint64_t)(uintptr_t)machine_work +
                        sizeof(machine_work)) {
        code = (const uint32_t *)(uintptr_t)era;
    }
    latx_bare_uart_puts("\nlatr-linux: HOSTFAULT ecode=");
    puthex64((host_fault_estat >> 16) & 0x3fu);
    latx_bare_uart_puts(" era=");
    puthex64(era);
    latx_bare_uart_puts(" badv=");
    puthex64(host_fault_badv);
    latx_bare_uart_puts(" guestpc=");
    puthex64(machine.harts[0].env.pc);
    latx_bare_uart_puts(" entered=");
    puthex64(last_entered);
    latx_bare_uart_puts("\nlatr-linux: hostregs ra=");
    puthex64(host_fault_ra);
    latx_bare_uart_puts(" sp=");
    puthex64(host_fault_sp);
    latx_bare_uart_puts(" s8=");
    puthex64(host_fault_s8);
    latx_bare_uart_puts(" r21=");
    puthex64(host_fault_r21);
    latx_bare_uart_puts(" a0=");
    puthex64(host_fault_a0);
    latx_bare_uart_puts("\n");
    if (code != NULL) {
        latx_bare_uart_puts("latr-linux: words at era:");
        for (int i = 0; i < 6; i++) {
            latx_bare_uart_puts(" ");
            puthex64(code[i]);
        }
        latx_bare_uart_puts("\n");
    }
    /* The entered block is the evidence that matters: a block that jumps to host
     * address 0 has a `jirl $zero, $ra, 0` whose $ra was clobbered, so its own
     * words show both the clobber and the return. */
    {
        const uint32_t *blk = (const uint32_t *)(uintptr_t)last_entered;

        if (blk >= (const uint32_t *)(uintptr_t)machine_work &&
            blk + 400 <= (const uint32_t *)(uintptr_t)machine_work +
                            sizeof(machine_work) / sizeof(uint32_t)) {
            latx_bare_uart_puts("latr-linux: block:");
            for (int i = 0; i < 400; i++) {
                if (i % 8 == 0) {
                    latx_bare_uart_puts("\nlatr-linux:  ");
                    puthex64((uint64_t)(uintptr_t)&blk[i]);
                    latx_bare_uart_puts(":");
                }
                latx_bare_uart_puts(" ");
                puthex64(blk[i]);
            }
            latx_bare_uart_puts("\n");
        }
    }
    latx_bare_uart_puts("latr-linux: last pcs:");
    for (uint32_t i = 0; i < PC_RING; i++) {
        latx_bare_uart_puts(" ");
        puthex64(pc_ring[(pc_ring_at + i) % PC_RING]);
    }
    latx_bare_uart_puts("\n");
    {
        CPUArchState *h = &machine.harts[0].env;
        {
        CPUArchState *h = &machine.harts[0].env;
        latx_bare_uart_puts("latr-linux: env pc=");
        puthex64(h->pc);
        latx_bare_uart_puts(" exc=");
        puthex64(h->csr[0x342]);   /* mcause */
        latx_bare_uart_puts(" mepc=");
        puthex64(h->csr[0x341]);
        latx_bare_uart_puts(" mtvec=");
        puthex64(h->csr[0x305]);
        latx_bare_uart_puts(" stvec=");
        puthex64(h->csr[0x105]);
        latx_bare_uart_puts(" priv=");
        put_dec(h->privilege);
        latx_bare_uart_puts(" e-exc=");
        puthex64(h->exception);
        latx_bare_uart_puts(" e-bad=");
        puthex64(h->badaddr);
        latx_bare_uart_puts(" hostra=");
        puthex64(h->host_return_address);
        latx_bare_uart_puts(" satp=");
        puthex64(h->csr[0x180]);
        latx_bare_uart_puts("\n");
    }
    latx_bare_uart_puts("latr-linux: gprs:");
        for (int i = 1; i < 32; i++) {
            if (i % 8 == 0) {
                latx_bare_uart_puts("\nlatr-linux:   ");
            }
            latx_bare_uart_puts(" x");
            put_dec((uint64_t)i);
            latx_bare_uart_puts("=");
            puthex64(h->gpr[i]);
        }
        latx_bare_uart_puts("\n");
    }
    latx_bare_uart_puts("latr-linux: done\n");
    for (;;) {
    }
}

__asm__(
".text\n"
".globl latr_host_eentry\n"
"latr_host_eentry:\n"
/*
 * No SAVE registers: they live at CSR 0x30+N, not 0x1a..0x1d, and 0x1a/0x1b
 * are PGDH/PGD in that numbering - "saving" there rewrites the page-table base
 * and the handler faults again before it can report the first fault.
 */
"  csrrd  $t0, 0x6\n"         /* ERA */
"  la.local $t2, host_fault_era\n"
"  st.d   $t0, $t2, 0\n"
"  csrrd  $t0, 0x5\n"         /* ESTAT */
"  la.local $t2, host_fault_estat\n"
"  st.d   $t0, $t2, 0\n"
"  csrrd  $t0, 0x7\n"         /* BADV */
"  la.local $t2, host_fault_badv\n"
"  st.d   $t0, $t2, 0\n"
"  la.local $t2, host_fault_ra\n"
"  st.d   $ra, $t2, 0\n"
"  la.local $t2, host_fault_sp\n"
"  st.d   $sp, $t2, 0\n"
"  la.local $t2, host_fault_s8\n"
"  st.d   $s8, $t2, 0\n"
"  la.local $t2, host_fault_r21\n"
"  st.d   $r21, $t2, 0\n"
"  la.local $t2, host_fault_a0\n"
"  st.d   $a0, $t2, 0\n"
"  bl     latr_host_fault_report\n"
"1: b 1b\n");

extern void latr_host_eentry(void);

static void enable_host_units(void)
{
    uint64_t euen, crmd, eentry = (uint64_t)(uintptr_t)&latr_host_eentry;

    __asm__ volatile("csrwr %0, 0xc" :: "r"(eentry) : "memory");
    __asm__ volatile("csrrd %0, 0x2" : "=r"(euen));
    euen |= UINT64_C(0x1) |    /* FPE  */
            UINT64_C(0x2) |    /* SXE  */
            UINT64_C(0x40) |   /* ASXE */
            UINT64_C(0x80);    /* BTE  */
    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    __asm__ volatile("csrrd %0, 0x0" : "=r"(crmd));
    crmd &= ~UINT64_C(0x4);
    __asm__ volatile("csrwr %0, 0x0" :: "r"(crmd) : "memory");
}


/* Hooks the machine calls around every block: `translated` after assembly,
 * `enter_tb` through the trampoline below. A block that never returns leaves
 * exactly these two at the wedged PC, which is the only evidence available
 * once generated code owns the CPU. */
static void hook_translated(void *env, uint64_t pc)
{
    (void)env;
    last_translated = pc;
}

static void hook_translate(void *env, uint64_t pc)
{
    (void)env;
    last_translated = pc;
    pc_ring[pc_ring_at++ % PC_RING] = pc;
}

static void progress(void)
{
    LatrMachineHart *h = &machine.harts[0];

    latx_bare_uart_puts("latr-linux: tbs=");
    put_dec(h->tbs);
    latx_bare_uart_puts(" pc=");
    puthex64(h->env.pc);
    latx_bare_uart_puts(" priv=");
    put_dec(h->env.privilege);
    latx_bare_uart_puts(" sbi=");
    put_dec(__atomic_load_n(&machine.sbicalls_serviced, __ATOMIC_RELAXED));
    latx_bare_uart_puts(" uart=");
    put_dec(machine.sys.uart_log_len);
    latx_bare_uart_puts("\n");
    /*
     * Mirror whatever the kernel wrote since the last report. The full log is
     * only dumped when the harness exits, and a long boot is normally killed by
     * the runner's watchdog -- which used to throw away every console line the
     * kernel had produced. Streaming the delta is what makes the log readable
     * after a timeout.
     */
    if (machine.sys.uart_log_len > console_mirrored) {
        const char *log = latr_system_uart_log(&machine.sys);
        uint32_t i = console_mirrored;

        latx_bare_uart_puts("latr-linux: console <<\n");
        for (; i < machine.sys.uart_log_len; i++) {
            latx_bare_uart_putc((unsigned char)log[i]);
        }
        latx_bare_uart_puts("\nlatr-linux: >>\n");
        console_mirrored = machine.sys.uart_log_len;
    }
    last_reported = h->tbs;
}

void kmain(void);
__asm__(".text\n.globl _start\n_start:\n"
        "  pcalau12i $sp, %pc_hi20(linux_stack)\n"
        "  addi.d $sp, $sp, %pc_lo12(linux_stack)\n"
        "  lu12i.w $t0, 512\n  add.d $sp, $sp, $t0\n"
        "  bl kmain\n1: b 1b\n");
/* 2 MiB: the `lu12i.w $t0, 512` in _start adds exactly this, and the two must
 * agree or $sp starts above the array and the first deep call overwrites
 * whatever the linker put next. The translator recurses further than a 1 MiB
 * stack allows, so this is not just bookkeeping. */
static uint8_t linux_stack[2u << 20] __attribute__((aligned(16), used));

void kmain(void)
{
    uint64_t image_len = (uint64_t)(latr_linux_image_end - latr_linux_image);
    int rc;

    enable_host_units();
    latx_bare_uart_puts("latr-linux: begin image=");
    put_dec(image_len);
    latx_bare_uart_puts("\n");

    if (!latr_machine_init(&machine, ram, sizeof(ram), 1, machine_work,
                           sizeof(machine_work))) {
        latx_bare_uart_puts("latr-linux: FAIL init\n");
        return;
    }
    latr_machine_set_enter_tb(&machine, latr_enter_tb);
    latr_machine_set_translate_tb(&machine, hook_translate);
    latr_machine_set_translated_tb(&machine, hook_translated);
    if (!latr_machine_set_bootargs(
            &machine, "console=ttyS0 earlycon=sbi keep_bootcon panic=-1")) {
        latx_bare_uart_puts("latr-linux: FAIL bootargs\n");
        return;
    }
    if (!latr_machine_load_kernel(&machine, latr_linux_image, image_len)) {
        latx_bare_uart_puts("latr-linux: FAIL load\n");
        return;
    }
    if (!latr_machine_build_dtb(&machine) || !latr_machine_boot(&machine)) {
        latx_bare_uart_puts("latr-linux: FAIL boot\n");
        return;
    }
    latx_bare_uart_puts("latr-linux: entry=");
    puthex64(machine.kernel_entry);
    latx_bare_uart_puts(" dtb=");
    puthex64(machine.dtb_addr);
    latx_bare_uart_puts(" ram=");
    put_dec(machine.sys.ram_size);
    latx_bare_uart_puts("\n");

    for (uint32_t step = 0; step < STEP_BUDGET; step++) {
        uint64_t before = machine.harts[0].tbs;

        rc = latr_machine_run(&machine, PROGRESS_EVERY);
        if (machine.harts[0].tbs != last_reported) {
            progress();
        }
        if (rc == 0) {
            latx_bare_uart_puts("latr-linux: machine failed\n");
            break;
        }
        if (rc == 1) {
            latx_bare_uart_puts("latr-linux: exit=");
            puthex64(machine.sys.exit_request);
            latx_bare_uart_puts("\n");
            break;
        }
        /* rc == 2 with nothing dispatched: every hart yielded, so nothing can
         * change without an external event. A budget exhaustion instead
         * dispatches the full slice and is not a stall. */
        if (machine.harts[0].tbs == before) {
            latx_bare_uart_puts("latr-linux: stalled pc=");
            puthex64(machine.harts[0].env.pc);
            latx_bare_uart_puts(" last-pc=");
            puthex64(machine.last_pc);
            latx_bare_uart_puts(" translated=");
            puthex64(last_translated);
            latx_bare_uart_puts(" entered=");
            puthex64(last_entered);
            latx_bare_uart_puts(" cmd=");
            put_dec(latr_hart_command(&machine.harts[0].park));
            latx_bare_uart_puts(" exit-req=");
            put_dec(machine.harts[0].park.exit_request);
            latx_bare_uart_puts(" wfi=");
            put_dec(machine.harts[0].env.wfi_pending);
            latx_bare_uart_puts("\n");
            break;
        }
    }
    progress();
    latx_bare_uart_puts("latr-linux: lastpc=");
    puthex64(machine.last_pc);
    latx_bare_uart_puts(" trap=");
    puthex64(machine.first_trap_cause);
    latx_bare_uart_puts(" trap-pc=");
    puthex64(machine.first_trap_pc);
    latx_bare_uart_puts(" fetch=");
    puthex64(machine.first_trap_fetch);
    latx_bare_uart_puts(" mem=");
    puthex64(machine.first_trap_mem);
    latx_bare_uart_puts(" mcause=");
    puthex64(machine.harts[0].env.csr[0x342]);
    latx_bare_uart_puts("\nlatr-linux: uart tail <<\n");
    latx_bare_uart_puts(latr_system_uart_log(&machine.sys));
    latx_bare_uart_puts("\nlatr-linux: >>\n");
    latx_bare_uart_puts("latr-linux: done\n");
}
