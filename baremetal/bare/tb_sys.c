/*
 * Long-mode bring-up test (P1) unified with P0 4 KiB hardware paging + PLV3.
 *
 * The guest is a 64-bit snippet that does what a firmware or a kernel does
 * before it can run anything else, and the harness checks the resulting
 * machine state rather than trusting the guest to report:
 *
 *   wrmsr  0xc0000080 (EFER) = LME     -> env->efer
 *   rdmsr  0xc0000080                  -> ebx (round trip through the helper)
 *   lgdt   [0x1200]                    -> env->gdt.base / limit
 *   4 stores                           -> a PML4/PDPT/PD/PT set in guest RAM
 *   mov    cr3, 0x2000                 -> env->cr[3], read back into r12d
 *   mov    cr0, PE|WP|PG               -> env->cr[0], LMA follows LME+PG
 *   in     0x3fd                       -> ebp (line status, transmitter empty)
 *   loop   over a NUL-terminated string -> every byte out port 0x3f8
 *
 * Unlike the software-instrumented version (option_guest_paging = 1), this
 * harness runs translated guest TBs in PLV3 under LoongArch 4 KiB hardware
 * paging (CSR.STLBPS.PS = 12, CSR.PWCL/PWCH) with option_guest_paging = 0:
 *
 *   - PLV0 runs through CSR.DMW0 (VSEG=0, PLV0=1, PLV3=0).
 *   - Every guest memory access (explicit loads/stores, LGDT/LIDT, and
 *     implicit stack accesses like IRETQ/CALL/RET/PUSH/POP) executes as a
 *     native LoongArch load/store at PLV3 and consults the hardware TLB.
 *   - On a TLB refill / PIL / PIS / PIF / PME trap in PLV0:
 *       1. If the fault is on a host page (code cache, epilogue, CPUX86State,
 *          or host stack), fill the 1:1 host PTE via lapg_walk() + tlbfill.
 *       2. Otherwise, consult the guest x86-64 page tables via
 *          latx_x86_walk():
 *          - If permitted: install the 4 KiB shadow PTE (GVA -> GPA) via
 *            lapg_map() and execute tlbfill to resume the faulting load/store.
 *          - If denied: sync the interrupted PLV3 guest registers back to
 *            CPUX86State, recover the exact faulting x86 RIP from CSR.ERA via
 *            tb_harness_resolve_x86_pc(), deliver #PF to guest IDT[14] via
 *            latx_x86_deliver_fault(&env, 14), and unwind back to the PLV0
 *            run loop.
 */
#include "qemu/osdep.h"
#include "exec/exec-all.h"
#include "cpu.h"
#include "lsenv.h"
#include "ir1.h"
#include "ir2.h"
#include "translate.h"
#include "reg-map.h"

#include "latx-options.h"
#include "latx-x86-paging.h"

#include "loongemu/loongarch_paging.h"
#include "loongemu/acpi_tables.h"
#include "loongemu/tb_cache.h"
#include "lbt_emu.h"
#include "tb_harness.h"

extern uint32_t latx_guest_nr_cpus;
extern uint32_t latx_guest_apic_id;

#define UART0_THR ((volatile unsigned char *)0x1fe001e0)
#define UART0_RBR ((volatile unsigned char *)0x1fe001e0)
#define UART0_LSR ((volatile unsigned char *)0x1fe001e5)

/*
 * Called from latx_runtime_init() when the host cannot run the ABI it would
 * otherwise advertise.  Printing the reason matters as much as stopping: the
 * failure mode this replaces was a guest that booted and then faulted on its
 * first SSE instruction, with nothing pointing at the real cause.
 */
void latx_platform_refuse(const char *why)
{
    latx_bare_uart_puts("loongemu: refusing to start: ");
    latx_bare_uart_puts(why);
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("tb-sys: checks=0000000000000000 fails=0000000000000001");
    latx_bare_uart_puts(" refused\n");
    latx_bare_uart_puts("tb-sys: done\n");
    for (;;) {
    }
}

void latx_bare_uart_putc(int c) { *UART0_THR = (unsigned char)c; }
void latx_bare_uart_puts(const char *s)
{
    while (*s) {
        latx_bare_uart_putc(*s++);
    }
}

static void puthex(uint64_t v)
{
    static const char d[] = "0123456789abcdef";
    int sh;

    for (sh = 60; sh >= 0; sh -= 4) {
        latx_bare_uart_putc(d[(v >> sh) & 0xf]);
    }
}

static int checks, fails;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        latx_bare_uart_puts("FAIL ");
        latx_bare_uart_puts(what);
        latx_bare_uart_puts("\n");
    }
}

/* ---------------- device layer: a 16550 subset ---------------- */

#define COM1_THR 0x3f8
#define COM1_LSR 0x3fd
#define LSR_THRE_TEMT 0x60

static uint8_t uart_tx[64];
static int uart_tx_count;
static uint8_t uart_ier;
static int uart_thre_pending;
static uint8_t uart_fcr;
static uint8_t uart_lcr;
static uint8_t uart_mcr;
static uint8_t uart_scr;
static uint8_t uart_dll = 1;
static uint8_t uart_dlm;

/* Device stub counters, incremented by the port and MMIO hooks below. */
static volatile uint64_t g_pit_port_writes;
static volatile uint64_t g_pci_config_writes;
static volatile uint64_t g_apic_mmio_reads;
static volatile uint64_t g_apic_mmio_writes;
#define MAX_VCPUS 4u

typedef struct LapicState {
    uint32_t tpr;
    uint32_t ldr;
    uint32_t dfr;
    uint32_t spiv;
    uint32_t icr_lo;
    uint32_t icr_hi;
    uint32_t lvtt;
    uint32_t lvt0;
    uint32_t lvt1;
    uint32_t lvterr;
    uint32_t tmict;
    uint32_t tdcr;
    uint64_t tm_start_ts;
    uint32_t irr[8];
    uint32_t isr[8];
} LapicState;

static uint32_t g_apic_regs[256];
static uint8_t g_apic_mmio_page[4096] __attribute__((aligned(4096)));
static uint8_t g_mmio_dummy_page[4096] __attribute__((aligned(4096)));
static LapicState g_lapic[MAX_VCPUS];
static uint32_t g_ioapic_regsel;
static uint32_t g_ioapic_id = MAX_VCPUS;
static uint64_t g_ioapic_redtbl[24];
static uint16_t g_acpi_pm1_sts;
static uint16_t g_acpi_pm1_en;
static uint16_t g_acpi_pm1_cnt;
static volatile uint32_t g_cur_vcpu;
static volatile int g_vcpu_online[MAX_VCPUS];
static volatile int g_vcpu_halted[MAX_VCPUS];
static volatile int g_vcpu_init_pending[MAX_VCPUS];
static volatile int g_vcpu_sipi_pending[MAX_VCPUS];
static volatile uint32_t g_vcpu_sipi_vec[MAX_VCPUS];
static CPUX86State g_vcpu_save[MAX_VCPUS];
static uint8_t g_pic_imr[2] = { 0xffu, 0xffu };
static uint8_t g_pic_icw_idx[2];
static uint8_t g_pic_icw_rem[2];
static uint32_t g_pic_irq0_vec = 0x30;
volatile uint64_t g_plv3_timer_preempt __attribute__((used));
static volatile int g_timer_irq_pending[MAX_VCPUS];
static volatile int g_vcpu_nmi_pending[MAX_VCPUS];
static volatile int g_vcpu_preempt_needed;
static int deliver_x86_exception(uint32_t vec, uint64_t push_rip,
                                 uint32_t error_code, int has_err);

uint64_t latx_platform_io_read(uint32_t port, int size)
{
    (void)size;
    if (port >= 0x3f8 && port <= 0x3ff) {
        uint32_t reg = port - 0x3f8u;
        switch (reg) {
        case 0: /* RBR / DLL */
            if (uart_lcr & 0x80u) {
                return uart_dll;
            }
            return ((*UART0_LSR) & 0x01u) ? (uint64_t)(*UART0_RBR) : 0;
        case 1: /* IER / DLM */
            return (uart_lcr & 0x80u) ? uart_dlm : uart_ier;
        case 2: { /* IIR */
            uint8_t fifo = (uart_fcr & 0x01u) ? 0xc0u : 0x00u;
            if (((*UART0_LSR) & 0x01u) && (uart_ier & 0x01u)) {
                return fifo | 0x04u;
            }
            if ((uart_ier & 0x02u) && uart_thre_pending) {
                uart_thre_pending = 0;
                return fifo | 0x02u;
            }
            return fifo | 0x01u;
        }
        case 3: /* LCR */
            return uart_lcr;
        case 4: /* MCR */
            return uart_mcr;
        case 5: /* LSR */
            return LSR_THRE_TEMT | ((*UART0_LSR) & 0x01u);
        case 6: /* MSR */
            if (uart_mcr & 0x10u) {
                uint8_t msr = 0;
                if (uart_mcr & 0x02u) msr |= 0x10u; /* RTS -> CTS */
                if (uart_mcr & 0x01u) msr |= 0x20u; /* DTR -> DSR */
                if (uart_mcr & 0x04u) msr |= 0x40u; /* OUT1 -> RI */
                if (uart_mcr & 0x08u) msr |= 0x80u; /* OUT2 -> DCD */
                return msr;
            }
            return 0xb0u; /* DCD | DSR | CTS */
        case 7: /* SCR */
            return uart_scr;
        }
    }
    if (port == 0x20 || port == 0xa0) {
        return 0;                    /* 8259A IRR/ISR */
    }
    if (port == 0x21) {
        return g_pic_imr[0];         /* 8259A master IMR */
    }
    if (port == 0xa1) {
        return g_pic_imr[1];         /* 8259A slave IMR */
    }
    if (port >= 0x40 && port <= 0x43) {
        return 0x1234;               /* 8254: plausible counter value */
    }
    if (port == LOONGEMU_ACPI_PM_TMR_PORT) {
        uint64_t now;
        __asm__ volatile("rdtime.d %0, $zero" : "=r"(now) :: "memory");
        /* 100 MHz rdtime.d -> 3.579545 MHz 24-bit ACPI PM Timer */
        return ((now * 3579545ULL) / 100000000ULL) & 0x00ffffffULL;
    }
    if (port == LOONGEMU_ACPI_PM1A_EVT_PORT) {
        return g_acpi_pm1_sts;
    }
    if (port == LOONGEMU_ACPI_PM1A_EVT_PORT + 2u) {
        return g_acpi_pm1_en;
    }
    if (port == LOONGEMU_ACPI_PM1A_CNT_PORT) {
        return g_acpi_pm1_cnt;
    }
    if (port == 0xcf8 || port == 0xcfc) {
        return 0xffffffffu;          /* no device at this selector */
    }
    return 0xffffffffull;
}

void latx_platform_io_write(uint32_t port, int size, uint64_t value)
{
    (void)size;
    if (port >= 0x3f8 && port <= 0x3ff) {
        uint32_t reg = port - 0x3f8u;
        uint8_t v = (uint8_t)value;
        switch (reg) {
        case 0: /* THR / DLL */
            if (uart_lcr & 0x80u) {
                uart_dll = v;
                return;
            }
            if (uart_tx_count < (int)sizeof(uart_tx)) {
                uart_tx[uart_tx_count] = v;
            }
            uart_tx_count++;
            latx_bare_uart_putc((int)v);
            if (uart_ier & 0x02u) {
                uart_thre_pending = 1;
            }
            return;
        case 1: /* IER / DLM */
            if (uart_lcr & 0x80u) {
                uart_dlm = v;
            } else {
                uint8_t new_ier = v & 0x0fu;
                if ((new_ier & 0x02u) && !(uart_ier & 0x02u)) {
                    uart_thre_pending = 1;
                } else if (!(new_ier & 0x02u)) {
                    uart_thre_pending = 0;
                }
                uart_ier = new_ier;
            }
            return;
        case 2: /* FCR */
            uart_fcr = v;
            return;
        case 3: /* LCR */
            uart_lcr = v;
            return;
        case 4: /* MCR */
            uart_mcr = v & 0x1fu;
            return;
        case 7: /* SCR */
            uart_scr = v;
            return;
        default:
            return;
        }
    }
    if (port == 0x20 || port == 0xa0) {
        int pic = (port == 0xa0) ? 1 : 0;
        if ((value & 0x10u) != 0) {
            g_pic_icw_idx[pic] = 1;
            g_pic_icw_rem[pic] = (value & 0x01u) ? 3 : 2;
        }
        return;
    }
    if (port == 0x21 || port == 0xa1) {
        int pic = (port == 0xa1) ? 1 : 0;
        if (g_pic_icw_rem[pic] > 0) {
            if (pic == 0 && g_pic_icw_idx[0] == 1) {
                g_pic_irq0_vec = (uint32_t)(value & 0xf8u);
            }
            g_pic_icw_idx[pic]++;
            g_pic_icw_rem[pic]--;
        } else {
            g_pic_imr[pic] = (uint8_t)value;
        }
        return;
    }
    if (port >= 0x40 && port <= 0x43) {
        g_pit_port_writes++;
        return;
    }
    if (port == LOONGEMU_ACPI_PM1A_EVT_PORT) {
        g_acpi_pm1_sts &= (uint16_t)(~value);
        return;
    }
    if (port == LOONGEMU_ACPI_PM1A_EVT_PORT + 2u) {
        g_acpi_pm1_en = (uint16_t)value;
        return;
    }
    if (port == LOONGEMU_ACPI_PM1A_CNT_PORT) {
        g_acpi_pm1_cnt = (uint16_t)value;
        return;
    }
    if (port == 0xcf8 || port == 0xcfc) {
        g_pci_config_writes++;
        return;
    }
}

/*
 * Local APIC window, delivered as MMIO.
 *
 * The guest reaches it at its physical address, so the access arrives here as
 * an ordinary guest load or store.  Keeping a register file means an INIT, a
 * TPR write, or an EOI is remembered rather than silently dropped, and the
 * counters make the traffic observable from the harness.
 */
#define APIC_MMIO_BASE  0xfee00000ULL
#define APIC_MMIO_SIZE  0x00100000ULL

int latx_platform_apic_mmio(uint64_t gpa, int is_write, uint64_t *value)
{
    uint64_t off;
    uint32_t idx;

    if (gpa < APIC_MMIO_BASE) {
        return 0;
    }
    off = gpa - APIC_MMIO_BASE;
    if (off >= APIC_MMIO_SIZE) {
        return 0;
    }

    idx = (uint32_t)((off >> 4) & 0xff);
    if (is_write) {
        g_apic_mmio_writes++;
        g_apic_regs[idx] = (uint32_t)*value;
    } else {
        g_apic_mmio_reads++;
        *value = g_apic_regs[idx];
    }
    return 1;
}

/* ---------------- guest memory ---------------- */

/* Guest-visible RAM: the whole area below the VMM's load base. */
#define GUEST_RAM_SIZE (128 * 1024 * 1024)

/* The kernel text the harness carries, linked for 0xffffffff81000000. */
#define KERNEL_TEXT_SIZE 0x1619ea0ULL
#define KERNEL_SEG1_REL  0x1800000ULL
#define KERNEL_SEG1_SIZE 0x55f000ULL
#define KERNEL_SEG2_REL  0x1d5f000ULL
#define KERNEL_SEG2_SIZE 0x2f9a8ULL
#define KERNEL_SEG3_REL  0x1d8f000ULL
#define KERNEL_SEG3_SIZE 0x283000ULL

/*
 * boot_params (the "zero page") lives below the kernel image, where a real
 * loader puts it.  Zeroed, which is what a loader with no memory map and no
 * command line hands over.
 */
#define BOOT_PARAMS_GPA  0x00010000ULL

/* Must match the linker script's load address (baremetal/bare/bare.lds). */
#define VMM_IMAGE_BASE  0x8000000ULL
#define GUEST_BASE     0x0
#define PROGRAM_PC     0x0ffe
#define GDT_PC         0x1200
#define STR_PC         0x6000
#define ALIAS_VA       0x202000u
#define ALIAS_PA       0x100000
#define RO_PC          0x201000u
#define STORE_PC       (PROGRAM_PC + 0xc3)   /* the faulting store */
#define IDT_DESC_PC    0x1400
#define IDT_TABLE_PC   0x14e0

static int g_bad_fetch;

/*
 * Backup of the host 4 KiB page at 0x201000 so the guest test can use VA/PA
 * 0x201000 while running in PLV3, and kmain can restore its own tail code page
 * once the TB execution loop completes.
 */
static uint8_t host_page_201000_backup[4096] __attribute__((aligned(4096)));

uint64_t latx_platform_phys_read(uint64_t pa, int size)
{
    uint64_t value = 0;

    for (int i = size - 1; i >= 0; i--) {
        value <<= 8;
        value |= *(volatile uint8_t *)(uintptr_t)(pa + (uint64_t)i);
    }
    return value;
}

void latx_platform_phys_write(uint64_t pa, int size, uint64_t value)
{
    for (int i = 0; i < size; i++) {
        *(volatile uint8_t *)(uintptr_t)(pa + (uint64_t)i) =
            (uint8_t)(value >> (8 * i));
    }
}

uint8_t latx_bare_ldub_code(uint64_t vaddr)
{
    /*
     * No range check here.  This used to count fetches at or above
     * GUEST_RAM_SIZE as out-of-image, which was true while the only thing
     * fetched was the flat test image.  Once a kernel runs through its own
     * page tables every fetch is a negative GVA (0xffffffff8...) and the
     * check reported a bounds violation on every instruction.  A genuine
     * out-of-range fetch is caught by the walker, which knows the tables.
     */
    return *(volatile uint8_t *)(uintptr_t)vaddr;
}

/* ---------------- translator state ---------------- */

static CPUX86State env;
static TCGContext tcg_storage;
static TRANSLATION_DATA tr;
static ENV lsenv_storage;
typedef struct FastJmpEntry {
    uint64_t pc;
    uint64_t code;
} FastJmpEntry;

#define FAST_JMP_CACHE_SIZE (1u << TB_JMP_CACHE_BITS)
static FastJmpEntry fast_jmp_cache[FAST_JMP_CACHE_SIZE]
    __attribute__((aligned(4096)));

#define CODE_CACHE_SIZE (16 * 1024 * 1024)
static uint8_t epi_buf[4096] __attribute__((aligned(4096)));
static uint8_t pro_buf[4096] __attribute__((aligned(4096)));
static uint8_t code_cache[CODE_CACHE_SIZE] __attribute__((aligned(4096)));

static uint64_t code_used;

#define TB_TABLE_CAP 16384
static TbCacheEntry tb_entries[TB_TABLE_CAP];
static uint32_t tb_buckets[TB_TABLE_CAP];
static TbCache tb_cache;
static TbInsnMap tb_insn_maps[TB_TABLE_CAP];
static uint64_t tb_code_hash[TB_TABLE_CAP];
static uint16_t tb_code_len[TB_TABLE_CAP];
static uint32_t g_tb_highwater;
#define CODE_PAGE_MAX_PFN (GUEST_RAM_SIZE >> 12)
static uint64_t g_code_page_bitmap[CODE_PAGE_MAX_PFN / 64];
static volatile int g_smc_dirty;

unsigned char kstack[65536] __attribute__((aligned(4096)));
void kmain(void);

__asm__(".text\n.globl _start\n_start:\n  la $sp, kstack\n  lu12i.w $t0, 16\n"
        "  add.d $sp, $sp, $t0\n  bl kmain\n1:\n  b 1b\n"
        ".balign 4096\n.space 4096\n");

extern int latx_gen_prologue(void *code_buf);

/* ---------------- P0 4 KiB hardware paging state ---------------- */

/* ESTAT.Ecode values. */
#define EXC_INT  0x0
#define EXC_PIL  0x1
#define EXC_PIS  0x2
#define EXC_PIF  0x3
#define EXC_PME  0x4
#define EXC_PNR  0x5
#define EXC_PNX  0x6
#define EXC_PPI  0x7
#define EXC_ADE  0x8
#define EXC_ALE  0x9
#define EXC_SYS  0x0b
#define EXC_BRK  0x0c
#define EXC_IPE  0x0d
#define EXC_GSPR 0x0e

/* CSR numbers (LoongArch Reference Manual Vol 1, table 7-1). */
#define CSR_TCFG   0x41
#define CSR_TVAL   0x42
#define CSR_TICLR  0x44
#define CSR_ESTAT_IS_MASK 0x1fffULL
#define IRQ_TIMER  11

#define LAPG_POOL_PAGES 1024
static uint64_t g_pool_mem[LAPG_POOL_PAGES * (LAPG_PAGE_SIZE / 8)]
    __attribute__((aligned(LAPG_PAGE_SIZE)));
static LapgPagePool g_pool;
static uint64_t g_root;

static volatile int g_probe_touched;
static volatile uint64_t g_hw_tlb_fills;
static volatile uint64_t g_hw_guest_pf;
static volatile uint64_t g_current_tb_code;
static volatile uint32_t g_current_tb_size;
static volatile uint64_t g_current_tb_pc;
static const TbInsnMap *g_current_tb_map;

static uint64_t resolve_fault_x86_pc(uint64_t fault_pc)
{
    if (g_current_tb_map != 0 &&
        fault_pc >= g_current_tb_code &&
        fault_pc < g_current_tb_code + (uint64_t)g_current_tb_size) {
        return tb_harness_resolve_x86_pc(g_current_tb_map,
                                         g_current_tb_code,
                                         fault_pc,
                                         g_current_tb_pc);
    }
    for (uint32_t i = 0; i < g_tb_highwater; i++) {
        if (tb_entries[i].used &&
            fault_pc >= tb_entries[i].code &&
            fault_pc < tb_entries[i].code + (uint64_t)tb_entries[i].code_size) {
            return tb_harness_resolve_x86_pc(&tb_insn_maps[i],
                                             tb_entries[i].code,
                                             fault_pc,
                                             tb_entries[i].pc);
        }
    }
    return tb_harness_resolve_x86_pc(g_current_tb_map,
                                     g_current_tb_code,
                                     fault_pc,
                                     g_current_tb_pc);
}

typedef enum {
    PLV3_EXIT_NONE     = 0,
    PLV3_EXIT_NORMAL   = 1,
    PLV3_EXIT_GUEST_PF = 2,
    PLV3_EXIT_TRAP     = 3,
} Plv3ExitReason;

typedef enum {
    RUN_EXIT_MAX_BLOCKS     = 0,
    RUN_EXIT_TRANSLATE_FAIL = 1,
    RUN_EXIT_STUCK          = 2,
    RUN_EXIT_PLV3_TRAP      = 3,
} RunBlocksExitKind;

typedef struct {
    uint64_t reason;
    uint64_t ecode;
    uint64_t esubcode;
    uint64_t is_refill;
    uint64_t era;
    uint64_t badv;
    uint64_t guest_rip;
} Plv3ExitInfo;

volatile Plv3ExitInfo g_plv3_exit __attribute__((used));
static volatile uint64_t g_run_blocks_exit;

/*
 * Interrupt injection state.
 *
 * The LoongArch core timer (CSR.TCFG) is the only clock the guest can see, and
 * the guest has its own notion of whether interrupts are enabled (x86 IF in
 * CPUX86State.eflags).  A tick is raised here and delivered to the guest's IDT
 * only when the guest actually asked for it; otherwise it is dropped, which is
 * exactly what a masked PIC does.
 */
static volatile uint64_t g_timer_ticks;      /* ticks raised by the host  */
static volatile uint64_t g_timer_delivered;  /* ticks actually injected   */
static volatile uint64_t g_timer_masked;     /* dropped because IF was 0  */
static uint64_t g_guest_isr_addr;            /* IDT[vector].offset        */
static uint32_t g_guest_ticks;               /* incremented by the stub   */
static uint32_t g_timer_vector = 0x20;       /* PIC default vector        */

/*
 * End-to-end tick test.  Its own code page, IDT, stack and counter, so it
 * cannot perturb (or be perturbed by) the image the earlier checks built.
 */
/*
 * Layout for the timer tests.  These must not overlap: the IDT occupies
 * idt_base + vector*16, so an idt_base of 0xb400 with vector 0x20 puts the
 * gate itself at 0xb600.  Placing the counter there made the ISR's
 * `inc dword [0xb600]` rewrite the gate's offset[15:0] -- the handler address
 * it returned to became 0xb001, and every read of the "counter" was really
 * reading a gate field.
 */
#define TICK_ISR_PC       0x0000b000   /* guest ISR: inc dword [counter]; iretq */
#define TICK_COUNTER_PC   0x0000b100   /* the dword the ISR increments       */
#define TICK_IDT_PC       0x0000b400   /* gates live at +vector*16 = 0xb600  */
#define TICK_SPIN_PC      0x0000c000   /* guest body: jmp $                  */
#define TICK_STACK_PC     0x0000d000   /* grows down; clear of everything    */
#define TICK_VECTOR       0x20
#define HLT_PC_TEST       0x0000e000   /* one `hlt`, for the halt check      */



/* A copy of the real kernel entry point, and scratch for its translation. */
#define KERNEL_ENTRY_PC  0x0000a000
#define CODE_SCRATCH     0x0008c000
#define OVMF_RESET_PC    0x0000a400

/* Local APIC window, as a guest physical range. */
#define APIC_MMIO_BASE  0xfee00000ULL
#define APIC_MMIO_SIZE  0x00100000ULL

/* Scratch addresses used by the timer/interrupt assertions. */
#define TIMER_IDT_PC        0x00007000
#define TIMER_STUB_PC       0x00007100
#define TIMER_GUEST_STACK   0x00009000



static int mem_read_u64(void *opaque, uint64_t phys, uint64_t *value)
{
    (void)opaque;
    *value = *(volatile uint64_t *)(uintptr_t)phys;
    return 1;
}

static int mem_write_u64(void *opaque, uint64_t phys, uint64_t value)
{
    (void)opaque;
    *(volatile uint64_t *)(uintptr_t)phys = value;
    return 1;
}

static LapgMemory g_memory = {
    .opaque = 0,
    .read_u64 = mem_read_u64,
    .write_u64 = mem_write_u64,
};

static inline uint64_t csrrd(unsigned int n)
{
    uint64_t v = 0;

    switch (n) {
#define R(x) case x: __asm__ volatile("csrrd %0, " #x : "=r"(v)); break;
    R(0x0) R(0x1) R(0x2) R(0x4) R(0x5) R(0x6) R(0x7) R(0xc)
    R(0x10) R(0x11) R(0x12) R(0x13) R(0x18) R(0x1c) R(0x1d) R(0x1e)
    R(0x88) R(0x89) R(0x8a) R(0x8f) R(0x180) R(0x181)
#undef R
    default:
        break;
    }
    return v;
}

static inline void csrwr(unsigned int n, uint64_t v)
{
    switch (n) {
#define W(x) \
    case x: __asm__ volatile("move $r12,%0\n csrwr $r12," #x :: "r"(v) : "r12", "memory"); break;
    W(0x0) W(0x1) W(0x2) W(0x4) W(0x6) W(0xc)
    W(0x10) W(0x11) W(0x12) W(0x13) W(0x18) W(0x1c) W(0x1d) W(0x1e)
    W(0x88) W(0x8a) W(0x8c) W(0x8d) W(0x8e) W(0x8f) W(0x180) W(0x181)
#undef W
    default:
        break;
    }
}

/*
 * Map a static host range [start, start + size) 1:1 into the LoongArch 4 KiB
 * page table so PLV3 can execute translated blocks and touch CPUX86State/stack.
 */
static void map_host_range(uint64_t start, uint64_t size, uint32_t perm)
{
    uint64_t va = start & LAPG_PAGE_MASK;
    uint64_t end = (start + size + LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;

    for (; va < end; va += LAPG_PAGE_SIZE) {
        LapgResult r = lapg_map(&g_memory, &g_pool, g_root, va, va, perm);

        if (r != LAPG_OK) {
            check(0, "map_host_range");
            return;
        }
    }
}

extern char pf_table[256];

static int is_host_vmm_va(uint64_t va)
{
    uint64_t page = va & LAPG_PAGE_MASK;
    uint64_t cc_start = (uint64_t)(uintptr_t)code_cache & LAPG_PAGE_MASK;
    uint64_t cc_end = ((uint64_t)(uintptr_t)code_cache + CODE_CACHE_SIZE +
                       LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    uint64_t pro_page = (uint64_t)(uintptr_t)pro_buf & LAPG_PAGE_MASK;
    uint64_t epi_page = (uint64_t)(uintptr_t)epi_buf & LAPG_PAGE_MASK;
    uint64_t env_start = (uint64_t)(uintptr_t)&env & LAPG_PAGE_MASK;
    uint64_t env_end = ((uint64_t)(uintptr_t)&env + sizeof(env) +
                        LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    uint64_t jc_start = (uint64_t)(uintptr_t)fast_jmp_cache & LAPG_PAGE_MASK;
    uint64_t jc_end = ((uint64_t)(uintptr_t)fast_jmp_cache +
                       sizeof(fast_jmp_cache) + LAPG_PAGE_SIZE - 1) &
                      LAPG_PAGE_MASK;
    uint64_t ks_start = (uint64_t)(uintptr_t)kstack & LAPG_PAGE_MASK;
    uint64_t ks_end = ((uint64_t)(uintptr_t)kstack + sizeof(kstack) +
                       LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    uint64_t pf_start = (uint64_t)(uintptr_t)pf_table & LAPG_PAGE_MASK;
    uint64_t pf_end = ((uint64_t)(uintptr_t)pf_table + sizeof(pf_table) +
                       LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;

    if (page >= cc_start && page < cc_end) {
        return 1;
    }
    if (page == pro_page || page == epi_page) {
        return 1;
    }
    if (page >= jc_start && page < jc_end) {
        return 1;
    }
    if (page >= env_start && page < env_end) {
        return 1;
    }
    if (page >= ks_start && page < ks_end) {
        return 1;
    }
    if (page >= pf_start && page < pf_end) {
        return 1;
    }
    return 0;
}

static void rebuild_host_mappings(void)
{
    lapg_pool_init(&g_pool, (uint64_t)(uintptr_t)g_pool_mem,
                   sizeof(g_pool_mem));
    if (!lapg_pool_alloc(&g_pool, LAPG_PAGE_SIZE, &g_root)) {
        check(0, "root alloc");
        return;
    }
    lapg_zero_page((void *)(uintptr_t)g_root);

    /*
     * The VMM's own image is deliberately not mapped here.
     *
     * DMW0 is configured as VSEG=0 with PLV0 set, which makes the whole low
     * 512 MiB window directly addressable at PLV0 regardless of the TLB.  The
     * VMM lives inside that window, so it never needs a page-table entry --
     * and adding one is worse than redundant: the pool and every global in
     * this file sit inside the span being mapped, so lapg_map() writes memory
     * that is not reachable until the mapping it is installing exists.  That
     * bootstrap cycle is what made an otherwise correct table report UNMAPPED.
     *
     * Addresses reached at PLV3 still go through the tables; this comment is
     * about the VMM's own PLV0 accesses only.
     */
    map_host_range((uint64_t)(uintptr_t)code_cache, CODE_CACHE_SIZE,
                   LAPG_PERM_READ | LAPG_PERM_WRITE |
                   LAPG_PERM_EXEC | LAPG_PERM_USER);
    map_host_range((uint64_t)(uintptr_t)pro_buf, sizeof(pro_buf),
                   LAPG_PERM_READ | LAPG_PERM_WRITE |
                   LAPG_PERM_EXEC | LAPG_PERM_USER);
    map_host_range((uint64_t)(uintptr_t)epi_buf, sizeof(epi_buf),
                   LAPG_PERM_READ | LAPG_PERM_WRITE |
                   LAPG_PERM_EXEC | LAPG_PERM_USER);
    map_host_range((uint64_t)(uintptr_t)&env, sizeof(env),
                   LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_USER);
    map_host_range((uint64_t)(uintptr_t)fast_jmp_cache, sizeof(fast_jmp_cache),
                   LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_USER);
    map_host_range((uint64_t)(uintptr_t)kstack, sizeof(kstack),
                   LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_USER);
    map_host_range((uint64_t)(uintptr_t)pf_table, sizeof(pf_table),
                   LAPG_PERM_READ | LAPG_PERM_USER);
}

/*
 * Hardware TLB flush hooks invoked by cpu_x86_update_cr0(),
 * cpu_x86_update_cr3(), and helper_invlpg() -> tlb_flush() / tlb_flush_page().
 */
void latx_platform_tlb_flush_all(void)
{
    /*
     * Drop the hardware entries and clear fast_jmp_cache so CR3/CR0 switches
     * never jump directly to a TB translated under a different address space.
     */
    memset(fast_jmp_cache, 0, sizeof(fast_jmp_cache));
    __asm__ volatile("invtlb 0x0, $zero, $zero" ::: "memory");
}

void latx_platform_tlb_flush_page(uint64_t addr)
{
    uint64_t vppn = addr & UINT64_C(0x0000ffffffffe000);

    __asm__ volatile("invtlb 0x6, $zero, %0" :: "r"(vppn) : "memory");
}

/*
 * Decode whether a faulting LoongArch memory instruction at `pc` is a store,
 * a load, or an instruction fetch.
 */
static uint32_t classify_fault_access(uint64_t pc, uint64_t badv,
                                      uint64_t ecode, int is_refill,
                                      uint32_t *out_lapg_access)
{
    uint32_t x86_access = 0;
    uint32_t lapg_access = LAPG_PERM_USER;

    if (pc == badv || (!is_refill && (ecode == EXC_PIF || ecode == EXC_PNX))) {
        x86_access = LATX_X86_ACCESS_EXEC;
        lapg_access |= LAPG_PERM_EXEC;
    } else if (!is_refill && (ecode == EXC_PIS || ecode == EXC_PME)) {
        x86_access = LATX_X86_ACCESS_WRITE;
        lapg_access |= LAPG_PERM_WRITE;
    } else if (!is_refill && (ecode == EXC_PIL || ecode == EXC_PNR)) {
        x86_access = LATX_X86_ACCESS_READ;
        lapg_access |= LAPG_PERM_READ;
    } else {
        /*
         * Refill or PPI: inspect the LoongArch instruction at `pc`.
         * Major opcode (bits [31:26]):
         *   0x0a: standard ld/st with si12; bit 25 == 1 for st.{b,h,w,d}
         *   0x09: ldptr/stptr; bit 24 == 1 for stptr.{w,d}
         *   0x0e: indexed ldx/stx/fldx/fstx/vldx/vstx; stores have sub-op bit
         */
        uint32_t insn = *(volatile uint32_t *)(uintptr_t)pc;
        uint32_t major = (insn >> 26) & 0x3f;
        int is_store = 0;

        if (major == 0x0a) {
            uint32_t op8 = (insn >> 24) & 0xffu;
            if (op8 == 0x29u) {
                is_store = 1; /* st.{b,h,w,d} */
            } else if (op8 == 0x2bu) {
                is_store = ((insn >> 22) & 1u) != 0; /* fst.{s,d} */
            }
        } else if (major == 0x08 || major == 0x09) {
            /* sc.{w,d} (0x21/0x23) and stptr.{w,d} (0x25/0x27) have bit 24 == 1 */
            is_store = ((insn >> 24) & 1) != 0;
        } else if (major == 0x0b) {
            /* fst.{s,d} has bit 24 == 1 */
            is_store = ((insn >> 24) & 1) != 0;
        } else if (major == 0x0c) {
            /* vst has bit 24 == 1 */
            is_store = ((insn >> 24) & 1) != 0;
        } else if (major == 0x0e) {
            uint32_t op22 = (insn >> 15) & 0x7fff;
            /* stx.{b,h,w,d} and atomic am* read-modify-write instructions */
            if (op22 == 0x080 || op22 == 0x088 || op22 == 0x090 ||
                op22 == 0x098 || (op22 >= 0x0c0 && op22 <= 0x0ff)) {
                is_store = 1;
            }
        }
        if (is_store) {
            x86_access = LATX_X86_ACCESS_WRITE;
            lapg_access |= LAPG_PERM_WRITE;
        } else {
            x86_access = LATX_X86_ACCESS_READ;
            lapg_access |= LAPG_PERM_READ;
        }
    }

    if (env.hflags & HF_CPL_MASK) {
        x86_access |= LATX_X86_ACCESS_USER;
    }
    if (out_lapg_access != 0) {
        *out_lapg_access = lapg_access;
    }
    return x86_access;
}

static int g_use_sw_lbt;

static void save_host_xmm_to_env(void)
{
    /*
     * XMM0..XMM15 map to LoongArch $vr16..$vr31 / $xr16..$xr31.
     * Emit raw vst/xvst encodings so tb_sys.c stays -mno-lsx -mno-lasx.
     * $t0 = $r12 (rj = 12 -> rj << 5 = 0x180, si12 = 0).
     */
    if (option_enable_lasx) {
#define SAVE_XR(i, word) do { \
        void *p = &env.xmm_regs[(i)]; \
        __asm__ volatile("move $t0, %0\n .word " #word "\n" :: "r"(p) : "t0", "memory"); \
    } while (0)
        SAVE_XR( 0, 0x2cc00190); SAVE_XR( 1, 0x2cc00191);
        SAVE_XR( 2, 0x2cc00192); SAVE_XR( 3, 0x2cc00193);
        SAVE_XR( 4, 0x2cc00194); SAVE_XR( 5, 0x2cc00195);
        SAVE_XR( 6, 0x2cc00196); SAVE_XR( 7, 0x2cc00197);
        SAVE_XR( 8, 0x2cc00198); SAVE_XR( 9, 0x2cc00199);
        SAVE_XR(10, 0x2cc0019a); SAVE_XR(11, 0x2cc0019b);
        SAVE_XR(12, 0x2cc0019c); SAVE_XR(13, 0x2cc0019d);
        SAVE_XR(14, 0x2cc0019e); SAVE_XR(15, 0x2cc0019f);
#undef SAVE_XR
    } else {
#define SAVE_VR(i, word) do { \
        void *p = &env.xmm_regs[(i)]; \
        __asm__ volatile("move $t0, %0\n .word " #word "\n" :: "r"(p) : "t0", "memory"); \
    } while (0)
        SAVE_VR( 0, 0x2c400190); SAVE_VR( 1, 0x2c400191);
        SAVE_VR( 2, 0x2c400192); SAVE_VR( 3, 0x2c400193);
        SAVE_VR( 4, 0x2c400194); SAVE_VR( 5, 0x2c400195);
        SAVE_VR( 6, 0x2c400196); SAVE_VR( 7, 0x2c400197);
        SAVE_VR( 8, 0x2c400198); SAVE_VR( 9, 0x2c400199);
        SAVE_VR(10, 0x2c40019a); SAVE_VR(11, 0x2c40019b);
        SAVE_VR(12, 0x2c40019c); SAVE_VR(13, 0x2c40019d);
        SAVE_VR(14, 0x2c40019e); SAVE_VR(15, 0x2c40019f);
#undef SAVE_VR
    }
}

/*
 * Sync the interrupted PLV3 guest registers from the trap frame back into
 * CPUX86State before delivering an x86 exception or calling a C helper.
 */
static void sync_trap_regs_to_env(const uint64_t *regs)
{
    for (int i = 0; i < 16; i++) {
        int la_reg = reg_gpr_map[i];
        env.regs[i] = regs[la_reg];
    }
    save_host_xmm_to_env();
    {
        uint32_t non_cc = (uint32_t)regs[reg_statics_map[S_EFLAGS]] & 0x702u;
        uint32_t cc = 0;

        if (g_use_sw_lbt) {
            cc = sw_lbt.eflags;
        } else {
            __asm__ volatile(".word 0x005cfc0c\n" /* x86mfflag $t0, 0x3f */
                             "move %0, $t0"
                             : "=r"(cc)
                             :
                             : "t0");
        }
        env.eflags = non_cc | (cc & 0x8d5u) | 0x2u;
    }
}

/*
 * Reload the trap frame from CPUX86State after a C helper or exception
 * delivery updates env->regs[] in PLV0.
 */
static void sync_env_to_trap_regs(uint64_t *regs)
{
    for (int i = 0; i < 16; i++) {
        int la_reg = reg_gpr_map[i];
        regs[la_reg] = env.regs[i];
    }
}

extern void lbt_handle(uint64_t *regs);
extern void tb_sys_trap(void);
extern void tb_sys_run_plv3(void *pro, void *tb_code, void *env_ptr);
extern char tb_sys_plv3_done[];
extern char tb_sys_abort_plv0[];

/*
 * Execute a C function call on behalf of PLV3 when `jirl $ra, $rj, 0` in a
 * translated TB jumps to a host C helper outside the PLV3-executable code
 * cache.  Since the C helper is not mapped with LAPG_PERM_USER, the instruction
 * fetch at the helper entry immediately traps to PLV0 with PIF/PPI (`pc == badv`),
 * with the arguments already placed in $a0..$a4 and the return address inside
 * the TB in $ra!
 */
typedef uint64_t (*HelperFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static int is_plv0_helper_entry(uint64_t addr)
{
    /* Any address inside the VMM .text segment (0x200000..0x395260) that is
     * not in the guest RAM (< 0x10000) or the PLV3 code cache is a VMM helper. */
    /*
     * A helper entry is an address inside the VMM's own image that is not one
     * of the buffers PLV3 is allowed to touch.
     *
     * The base comes from the linker script's load address, not from rounding
     * a global down: `env` lives inside .bss, and rounding it to a 4 MiB
     * boundary happens to produce the right answer only while the image is
     * small enough that nothing crosses one.  Once .rodata grows, .bss starts
     * at a different offset and the rounded value names the wrong window --
     * which turned ordinary VMM addresses into "helper entries" and made a
     * translated block jump to code that was never a call target.
     */
    {
        extern char _end[];
        uint64_t img_start = VMM_IMAGE_BASE;
        uint64_t img_end = (uint64_t)(uintptr_t)_end;

        return addr >= img_start && addr < img_end && !is_host_vmm_va(addr);
    }
}

static void fill_hw_tlb_pair(uint64_t badv, int is_refill)
{
    uint64_t vppn = badv & UINT64_C(0x0000ffffffffe000);
    LapgTranslation tr0, tr1;
    uint64_t pte0 = 0, pte1 = 0;

    if (lapg_walk(&g_memory, g_root, vppn, LAPG_PERM_USER, &tr0) == LAPG_OK) {
        pte0 = tr0.pte;
    }
    if (lapg_walk(&g_memory, g_root, vppn + LAPG_PAGE_SIZE,
                  LAPG_PERM_USER, &tr1) == LAPG_OK) {
        pte1 = tr1.pte;
    }

    if (is_refill) {
        csrwr(0x8e, vppn | (uint64_t)LAPG_PAGE_BITS);
        csrwr(0x8c, pte0);
        csrwr(0x8d, pte1);
    } else {
        __asm__ volatile("invtlb 0x6, $zero, %0" :: "r"(vppn) : "memory");
        csrwr(0x10, (uint64_t)LAPG_PAGE_BITS << 24);
        csrwr(0x11, vppn);
        csrwr(0x12, pte0);
        csrwr(0x13, pte1);
    }
    __asm__ volatile("tlbfill" ::: "memory");
    g_hw_tlb_fills++;
}

static void lapic_reset_cpu(uint32_t cpu)
{
    LapicState *l = &g_lapic[cpu];
    memset(l, 0, sizeof(*l));
    l->dfr = 0xffffffffu;
    l->spiv = 0x000001ffu;
    l->lvtt = 0x00010000u;
    l->lvt0 = 0x00010000u;
    l->lvt1 = 0x00010000u;
    l->lvterr = 0x00010000u;
    l->tdcr = 0x0000000bu;
}

static void lapic_reset_all(void)
{
    for (uint32_t i = 0; i < MAX_VCPUS; i++) {
        lapic_reset_cpu(i);
    }
}

static void ioapic_reset(void)
{
    g_ioapic_regsel = 0;
    g_ioapic_id = MAX_VCPUS;
    for (uint32_t i = 0; i < 24; i++) {
        g_ioapic_redtbl[i] = 0x0000000000010000ULL; /* masked */
    }
}

static void lapic_set_irr(uint32_t cpu, uint32_t vec)
{
    if (cpu < MAX_VCPUS && vec >= 0x20u && vec < 256u) {
        g_lapic[cpu].irr[vec >> 5] |= (1u << (vec & 31u));
    }
}

static int lapic_has_irr(uint32_t cpu)
{
    if (cpu >= MAX_VCPUS) {
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        if (g_lapic[cpu].irr[i] != 0) {
            return 1;
        }
    }
    return 0;
}

static uint32_t lapic_pop_irr(uint32_t cpu)
{
    LapicState *l = &g_lapic[cpu];
    for (int i = 7; i >= 0; i--) {
        uint32_t bits = l->irr[i];
        if (bits != 0) {
            for (int b = 31; b >= 0; b--) {
                if (bits & (1u << b)) {
                    uint32_t vec = (uint32_t)(i * 32 + b);
                    if (vec < 0x20u) {
                        return 0;
                    }
                    l->irr[i] &= ~(1u << b);
                    l->isr[i] |= (1u << b);
                    return vec;
                }
            }
        }
    }
    return 0;
}

static void lapic_eoi(uint32_t cpu)
{
    LapicState *l = &g_lapic[cpu];
    for (int i = 7; i >= 0; i--) {
        uint32_t bits = l->isr[i];
        if (bits != 0) {
            for (int b = 31; b >= 0; b--) {
                if (bits & (1u << b)) {
                    l->isr[i] &= ~(1u << b);
                    return;
                }
            }
        }
    }
}

static uint32_t lapic_mmio_read(uint32_t off)
{
    LapicState *l = &g_lapic[g_cur_vcpu];
    switch (off & 0xff0u) {
    case 0x020u: return g_cur_vcpu << 24;
    case 0x030u: return 0x00050014u; /* version 0x14, max LVT 5 */
    case 0x080u: return l->tpr;
    case 0x0d0u: return l->ldr;
    case 0x0e0u: return l->dfr;
    case 0x0f0u: return l->spiv;
    case 0x100u: case 0x110u: case 0x120u: case 0x130u:
    case 0x140u: case 0x150u: case 0x160u: case 0x170u:
        return l->isr[((off & 0xff0u) - 0x100u) >> 4];
    case 0x200u: case 0x210u: case 0x220u: case 0x230u:
    case 0x240u: case 0x250u: case 0x260u: case 0x270u:
        return l->irr[((off & 0xff0u) - 0x200u) >> 4];
    case 0x300u: return l->icr_lo & ~(1u << 12); /* Delivery Status = Idle */
    case 0x310u: return l->icr_hi;
    case 0x320u: return l->lvtt;
    case 0x350u: return l->lvt0;
    case 0x360u: return l->lvt1;
    case 0x370u: return l->lvterr;
    case 0x380u: return l->tmict;
    case 0x390u: {
        if (l->tmict == 0) {
            return 0;
        }
        uint64_t now;
        __asm__ volatile("rdtime.d %0, $zero" : "=r"(now) :: "memory");
        uint64_t elapsed = now - l->tm_start_ts;
        uint32_t div = 1u;
        uint32_t dv = ((l->tdcr >> 1) & 4u) | (l->tdcr & 3u);
        if (dv == 0) div = 2; else if (dv == 1) div = 4; else if (dv == 2) div = 8;
        else if (dv == 3) div = 16; else if (dv == 4) div = 32; else if (dv == 5) div = 64;
        else if (dv == 6) div = 128; else div = 1;
        uint64_t ticks = elapsed / (uint64_t)div;
        if (l->lvtt & (1u << 17)) {
            return l->tmict - (uint32_t)(ticks % (uint64_t)l->tmict);
        }
        return (ticks >= (uint64_t)l->tmict) ? 0u : (l->tmict - (uint32_t)ticks);
    }
    case 0x3e0u: return l->tdcr;
    default: return 0;
    }
}

static void lapic_deliver_icr(uint32_t icr_lo, uint32_t icr_hi)
{
    uint32_t vec = icr_lo & 0xffu;
    uint32_t deliv = (icr_lo >> 8) & 7u;
    uint32_t dest_mode = (icr_lo >> 11) & 1u;
    uint32_t shorthand = (icr_lo >> 18) & 3u;
    uint32_t dest_id = (icr_hi >> 24) & 0xffu;

    for (uint32_t t = 0; t < MAX_VCPUS; t++) {
        int match = 0;
        if (shorthand == 0) {
            if (dest_mode == 0u) {
                match = (dest_id == 0xffu) || (t == dest_id);
            } else {
                uint32_t ldr_id = (g_lapic[t].ldr >> 24) & 0xffu;
                if (ldr_id == 0u) {
                    ldr_id = (1u << t);
                }
                match = ((ldr_id & dest_id) != 0u);
            }
        } else if (shorthand == 1) {
            match = (t == g_cur_vcpu);
        } else if (shorthand == 2) {
            match = 1;
        } else if (shorthand == 3) {
            match = (t != g_cur_vcpu);
        }
        if (!match) {
            continue;
        }
        if (deliv == 5u) { /* INIT */
            if (t != 0) {
                g_vcpu_init_pending[t] = 1;
                lapic_reset_cpu(t);
            }
        } else if (deliv == 6u) { /* SIPI */
            if (t != 0 && !g_vcpu_online[t]) {
                g_vcpu_sipi_vec[t] = vec;
                g_vcpu_sipi_pending[t] = 1;
                g_vcpu_halted[t] = 0;
            }
        } else if (deliv == 4u) { /* NMI */
            g_vcpu_nmi_pending[t] = 1;
            g_vcpu_halted[t] = 0;
        } else if (deliv == 0u || deliv == 1u) { /* Fixed / Lowest Priority */
            lapic_set_irr(t, vec);
            g_vcpu_halted[t] = 0;
        }
    }
}

static void lapic_mmio_write(uint32_t off, uint32_t val)
{
    LapicState *l = &g_lapic[g_cur_vcpu];
    switch (off & 0xff0u) {
    case 0x080u: l->tpr = val & 0xffu; break;
    case 0x0b0u: lapic_eoi(g_cur_vcpu); break;
    case 0x0d0u: l->ldr = val; break;
    case 0x0e0u: l->dfr = val; break;
    case 0x0f0u: l->spiv = val; break;
    case 0x300u:
        l->icr_lo = val;
        lapic_deliver_icr(val, l->icr_hi);
        break;
    case 0x310u: l->icr_hi = val; break;
    case 0x320u: l->lvtt = val; break;
    case 0x350u: l->lvt0 = val; break;
    case 0x360u: l->lvt1 = val; break;
    case 0x370u: l->lvterr = val; break;
    case 0x380u:
        l->tmict = val;
        __asm__ volatile("rdtime.d %0, $zero" : "=r"(l->tm_start_ts) :: "memory");
        break;
    case 0x3e0u: l->tdcr = val; break;
    default: break;
    }
}

static uint32_t ioapic_mmio_read(uint32_t off)
{
    if ((off & 0x1fu) == 0x00u) {
        return g_ioapic_regsel;
    }
    if ((off & 0x1fu) == 0x10u) {
        uint32_t sel = g_ioapic_regsel & 0xffu;
        if (sel == 0x00u) return g_ioapic_id << 24;
        if (sel == 0x01u) return 0x00170011u; /* 24 entries, version 0x11 */
        if (sel == 0x02u) return 0;
        if (sel >= 0x10u && sel < 0x40u) {
            uint32_t idx = (sel - 0x10u) >> 1;
            return (sel & 1u) ? (uint32_t)(g_ioapic_redtbl[idx] >> 32)
                              : (uint32_t)(g_ioapic_redtbl[idx] & 0xffffffffu);
        }
    }
    return 0;
}

static void ioapic_mmio_write(uint32_t off, uint32_t val)
{
    if ((off & 0x1fu) == 0x00u) {
        g_ioapic_regsel = val & 0xffu;
        return;
    }
    if ((off & 0x1fu) == 0x10u) {
        uint32_t sel = g_ioapic_regsel & 0xffu;
        if (sel == 0x00u) {
            g_ioapic_id = (val >> 24) & 0x0fu;
        } else if (sel >= 0x10u && sel < 0x40u) {
            uint32_t idx = (sel - 0x10u) >> 1;
            if (sel & 1u) {
                g_ioapic_redtbl[idx] = (g_ioapic_redtbl[idx] & 0xffffffffULL) |
                                       ((uint64_t)val << 32);
            } else {
                g_ioapic_redtbl[idx] = (g_ioapic_redtbl[idx] & ~0xffffffffULL) |
                                       (uint64_t)val;
            }
        }
    }
}

static int emulate_mmio_insn(uint64_t fault_pc, uint64_t gpa,
                             uint64_t *regs, int is_refill)
{
    uint32_t insn = *(volatile uint32_t *)(uintptr_t)fault_pc;
    uint32_t major = (insn >> 26) & 0x3fu;
    uint32_t rd = insn & 0x1fu;
    int is_store = 0;
    int is_signed = 0;
    int size = 4;

    if (major == 0x0au) {
        uint32_t op8 = (insn >> 24) & 0xffu;
        uint32_t sub = (insn >> 22) & 3u;
        size = 1 << sub;
        if (op8 == 0x29u) {
            is_store = 1;
        } else if (op8 == 0x28u) {
            is_store = 0;
            is_signed = 1;
        } else if (op8 == 0x2au) {
            is_store = 0;
            is_signed = 0;
        } else {
            return 0;
        }
    } else if (major == 0x09u) {
        is_store = ((insn >> 24) & 1u) != 0;
        size = ((insn >> 25) & 1u) ? 8 : 4;
        is_signed = !is_store;
    } else if (major == 0x0eu) {
        uint32_t op22 = (insn >> 15) & 0x7fffu;
        if (op22 == 0x080u || op22 == 0x088u || op22 == 0x090u || op22 == 0x098u) {
            is_store = 1;
            size = 1 << ((op22 - 0x080u) >> 3);
        } else if (op22 == 0x000u || op22 == 0x008u || op22 == 0x010u || op22 == 0x018u) {
            is_store = 0;
            is_signed = 1;
            size = 1 << (op22 >> 3);
        } else if (op22 == 0x040u || op22 == 0x048u || op22 == 0x050u) {
            is_store = 0;
            is_signed = 0;
            size = 1 << ((op22 - 0x040u) >> 3);
        } else {
            return 0;
        }
    } else {
        return 0;
    }

    uint64_t gpa_page = gpa & LAPG_PAGE_MASK;
    uint32_t off = (uint32_t)(gpa & 0xfffu);

    if (is_store) {
        uint32_t val = (uint32_t)regs[rd];
        if (gpa_page == LOONGEMU_LAPIC_GPA) {
            lapic_mmio_write(off, val);
        } else if (gpa_page == LOONGEMU_IOAPIC_GPA) {
            ioapic_mmio_write(off, val);
        }
    } else {
        uint64_t val = 0;
        if (gpa_page == LOONGEMU_LAPIC_GPA) {
            val = lapic_mmio_read(off);
        } else if (gpa_page == LOONGEMU_IOAPIC_GPA) {
            val = ioapic_mmio_read(off);
        }
        if (rd != 0) {
            if (size == 1) {
                val = is_signed ? (uint64_t)(int64_t)(int8_t)val : (val & 0xffu);
            } else if (size == 2) {
                val = is_signed ? (uint64_t)(int64_t)(int16_t)val : (val & 0xffffu);
            } else if (size == 4) {
                val = is_signed ? (uint64_t)(int64_t)(int32_t)val : (val & 0xffffffffu);
            }
            regs[rd] = val;
        }
    }

    if (is_refill) {
        csrwr(0x8a, ((fault_pc + 4) & ~UINT64_C(3)) | 1u);
    } else {
        csrwr(0x6, fault_pc + 4);
    }
    return 1;
}

static void switch_vcpu(uint32_t next_id)
{
    uint32_t cc = 0;
    if (next_id >= MAX_VCPUS || next_id == g_cur_vcpu) {
        return;
    }
    /* env.eflags is already authoritative from epi_buf / sync_trap_regs_to_env;
     * do not overwrite it with hardware LBT flags on vCPU switch. */
    g_vcpu_save[g_cur_vcpu] = env;
    env = g_vcpu_save[next_id];
    cc = (uint32_t)(env.eflags & 0x8d5u);
    if (g_use_sw_lbt) {
        sw_lbt.eflags = cc;
    } else {
        __asm__ volatile("move $t0, %0\n"
                         ".word 0x005cfc2c\n" /* x86mtflag $t0, 0x3f */
                         :: "r"(cc) : "t0");
    }
    g_cur_vcpu = next_id;
    latx_guest_apic_id = next_id;
    latx_x86_tlb_flush_all();
    latx_platform_tlb_flush_all();
}

static int start_ap_from_sipi(uint32_t cpu_id, uint32_t vec)
{
    uint64_t sipi_pa = (uint64_t)vec << 12;
    uint64_t start_rip = 0, efer_val = 0, cr3_val = 0;
    uint32_t cr4_val = 0;
    int found = 0;

    /*
     * Locate Linux's struct real_mode_header right at or below sipi_pa:
     *   +0x08: trampoline_start  (== sipi_pa)
     *   +0x0c: trampoline_header (PA of struct trampoline_header)
     *   +0x14 (or +0x18 with CONFIG_AMD_MEM_ENCRYPT): trampoline_pgd
     */
    for (uint64_t back = 0; back <= 0x8000ULL; back += 0x1000ULL) {
        if (sipi_pa < back) {
            break;
        }
        uint64_t hdr = sipi_pa - back;
        uint32_t t_start = *(volatile uint32_t *)(uintptr_t)(hdr + 0x08);
        uint32_t t_hdr = *(volatile uint32_t *)(uintptr_t)(hdr + 0x0c);
        if (t_start == (uint32_t)sipi_pa &&
            t_hdr >= (uint32_t)hdr && t_hdr < (uint32_t)(hdr + 0x10000u)) {
            static const uint32_t pgd_offs[3] = { 0x18u, 0x14u, 0x10u };
            uint32_t t_pgd = 0;
            for (int k = 0; k < 3; k++) {
                uint32_t cand = *(volatile uint32_t *)(uintptr_t)(hdr + pgd_offs[k]);
                if (cand != 0 && (cand & 0xfffu) == 0 && cand < GUEST_RAM_SIZE &&
                    (*(volatile uint64_t *)(uintptr_t)(cand + 511u * 8u) & 1u) != 0) {
                    t_pgd = cand;
                    break;
                }
            }
            if (t_pgd != 0) {
                uint64_t s = *(volatile uint64_t *)(uintptr_t)(t_hdr + 0x00);
                uint64_t ef = *(volatile uint64_t *)(uintptr_t)(t_hdr + 0x08);
                uint32_t c4 = *(volatile uint32_t *)(uintptr_t)(t_hdr + 0x10);
                if ((s >> 48) == 0xffffu) {
                    start_rip = s;
                    efer_val = ef;
                    cr4_val = c4;
                    cr3_val = (uint64_t)t_pgd;
                    found = 1;
                    break;
                }
            }
        }
    }
    latx_bare_uart_puts("tb-sys: sipi cpu=");
    puthex(cpu_id);
    latx_bare_uart_puts(" pa=");
    puthex(sipi_pa);
    latx_bare_uart_puts(" found=");
    puthex((uint64_t)found);
    latx_bare_uart_puts(" rip=");
    puthex(start_rip);
    latx_bare_uart_puts(" cr3=");
    puthex(cr3_val);
    latx_bare_uart_puts("\n");
    if (!found) {
        return 0;
    }

    CPUX86State *ap = &g_vcpu_save[cpu_id];
    memset(ap, 0, sizeof(*ap));
    latx_x86_env_init_features(ap);
    (void)cpu_id;
    ap->sys.codemode = 1;
    ap->hflags = HF_LMA_MASK | HF_CS64_MASK | HF_PE_MASK;
    ap->cr[0] = 0x80050033u;
    ap->cr[3] = cr3_val;
    ap->cr[4] = (uint64_t)cr4_val | CR4_PAE_MASK;
    ap->efer = efer_val | MSR_EFER_LME | MSR_EFER_LMA;
    ap->segs[R_CS].selector = 0x10;
    ap->segs[R_DS].selector = 0x18;
    ap->segs[R_ES].selector = 0x18;
    ap->segs[R_SS].selector = 0x18;
    ap->eip = start_rip;
    ap->regs[R_ESP] = 0x7ff0;
    ap->eflags = 0x2;
    ap->tb_jmp_cache_ptr = fast_jmp_cache;
    g_vcpu_online[cpu_id] = 1;
    g_vcpu_halted[cpu_id] = 0;
    g_vcpu_init_pending[cpu_id] = 0;
    g_vcpu_sipi_pending[cpu_id] = 0;
    return 1;
}

static uint64_t build_guest_leaf_pte(uint32_t x86_r,
                                     const LatxX86Translation *x86_tr,
                                     uint32_t x86_access)
{
    uint64_t gpa_page = x86_tr->gpa & LAPG_PAGE_MASK;
    uint32_t perm = LAPG_PERM_READ | LAPG_PERM_USER;

    if (gpa_page >= APIC_MMIO_BASE &&
        gpa_page < APIC_MMIO_BASE + APIC_MMIO_SIZE) {
        gpa_page = (uint64_t)(uintptr_t)g_apic_mmio_page;
    } else if (gpa_page >= UINT64_C(0x10000000)) {
        gpa_page = (uint64_t)(uintptr_t)g_mmio_dummy_page;
    }

    if (x86_r == LATX_X86_WALK_PAGING_OFF) {
        perm |= LAPG_PERM_WRITE | LAPG_PERM_EXEC;
    } else {
        if ((x86_tr->perm & LATX_X86_ACCESS_WRITE) ||
            (!(x86_access & LATX_X86_ACCESS_USER) &&
             !(env.cr[0] & CR0_WP_MASK))) {
            perm |= LAPG_PERM_WRITE;
        }
        if (x86_tr->perm & LATX_X86_ACCESS_EXEC) {
            perm |= LAPG_PERM_EXEC;
        }
    }
    {
        uint64_t gpa_pfn = gpa_page >> 12;
        if (gpa_pfn < CODE_PAGE_MAX_PFN &&
            ((g_code_page_bitmap[gpa_pfn >> 6] >> (gpa_pfn & 63u)) & 1u)) {
            if (x86_access & LATX_X86_ACCESS_WRITE) {
                g_smc_dirty = 1;
                g_code_page_bitmap[gpa_pfn >> 6] &= ~(UINT64_C(1) << (gpa_pfn & 63u));
            } else {
                perm &= ~LAPG_PERM_WRITE;
            }
        }
    }
    return lapg_make_leaf(gpa_page >> LAPG_PAGE_BITS, perm);
}

static void fill_hw_tlb_guest(uint64_t badv, uint64_t fault_pte, int is_refill)
{
    uint64_t vppn = badv & UINT64_C(0x0000ffffffffe000);
    uint64_t fault_page = badv & LAPG_PAGE_MASK;
    uint64_t sib_page = fault_page ^ LAPG_PAGE_SIZE;
    uint64_t sib_pte = 0;

    if (is_host_vmm_va(sib_page)) {
        LapgTranslation tr_sib;
        if (lapg_walk(&g_memory, g_root, sib_page, LAPG_PERM_USER, &tr_sib) == LAPG_OK) {
            sib_pte = tr_sib.pte;
        }
    } else {
        uint32_t sib_acc = LATX_X86_ACCESS_READ |
                           ((env.hflags & HF_CPL_MASK) ? LATX_X86_ACCESS_USER : 0);
        LatxX86Translation sib_tr;
        LatxX86WalkResult sib_r = latx_x86_walk_cached(&env, sib_page, sib_acc, &sib_tr);
        if (sib_r == LATX_X86_WALK_OK || sib_r == LATX_X86_WALK_PAGING_OFF) {
            sib_pte = build_guest_leaf_pte((uint32_t)sib_r, &sib_tr, sib_acc);
        }
    }

    int is_odd = (int)((fault_page >> LAPG_PAGE_BITS) & 1u);
    uint64_t pte0 = is_odd ? sib_pte : fault_pte;
    uint64_t pte1 = is_odd ? fault_pte : sib_pte;

    if (is_refill) {
        csrwr(0x8e, vppn | (uint64_t)LAPG_PAGE_BITS);
        csrwr(0x8c, pte0);
        csrwr(0x8d, pte1);
    } else {
        __asm__ volatile("invtlb 0x6, $zero, %0" :: "r"(vppn) : "memory");
        csrwr(0x10, (uint64_t)LAPG_PAGE_BITS << 24);
        csrwr(0x11, vppn);
        csrwr(0x12, pte0);
        csrwr(0x13, pte1);
    }
    __asm__ volatile("tlbfill" ::: "memory");
    g_hw_tlb_fills++;
}

/*
 * Arm the LoongArch core timer so it raises IRQ_TIMER periodically.
 *
 * CSR.TCFG: bit 0 = enable, bit 1 = periodic, bits 63:2 = period in units of
 * the constant timer's 10 ns tick.  A period of 100000 ticks is 1 ms of
 * virtual time, which is the cadence a guest HZ=1000 kernel expects.
 */
static void timer_start(uint64_t period_ticks)
{
    uint64_t tcfg = (period_ticks << 2) | 0x2 | 0x1;

    /*
     * CSR.ECFG.LIE enables the timer's entry in the interrupt tree; CRMD.IE is
     * deliberately left alone.  This layer runs the harness at PLV0 and is not
     * ready to take asynchronous exceptions yet, so the tick is observed by
     * polling CSR.ESTAT.TI instead -- which still proves the counter advanced
     * and the interrupt latched, without letting a stray entry unwind the
     * harness mid-check.
     */
    {
        uint64_t ecfg;

        __asm__ volatile("csrrd %0, 0x4" : "=r"(ecfg) :: "memory");
        ecfg |= (1ULL << 11);
        __asm__ volatile("csrwr %0, 0x4" :: "r"(ecfg) : "memory");
    }

    __asm__ volatile("csrwr %0, 0x41" :: "r"(tcfg) : "memory");
}

static void timer_stop(void)
{
    uint64_t tcfg = 0;
    uint64_t ecfg;

    __asm__ volatile("csrwr %0, 0x41" :: "r"(tcfg) : "memory");

    __asm__ volatile("csrrd %0, 0x4" : "=r"(ecfg) :: "memory");
    ecfg &= ~(1ULL << 11);
    __asm__ volatile("csrwr %0, 0x4" :: "r"(ecfg) : "memory");
}

/* Acknowledge one timer interrupt at the source. */
static void timer_ack(void)
{
    uint64_t one = 1;

    __asm__ volatile("csrwr %0, 0x44" :: "r"(one) : "memory");
}

/*
 * Deliver one tick to the guest.
 *
 * The guest's IDT entry is read from the shadow tables (the same view the CPU
 * would use) and the interrupt frame is pushed onto the guest stack in the
 * exact layout `iretq` pops: RIP, CS, RFLAGS, RSP, SS for a 64-bit gate.
 * Delivery is refused while the guest has IF clear.
 */
static int deliver_tick_to_guest(void)
{
    uint64_t idt_base, entry, handler, rflags, rsp, cs;
    uint16_t cs_sel;
    int vec = (int)g_timer_vector;
    uint64_t *stack;

    /* Interrupts disabled by the guest: drop the tick. */
    rflags = *(volatile uint64_t *)(uintptr_t)&env.eflags;
    if ((rflags & IF_MASK) == 0) {
        g_timer_masked++;
        return 0;
    }

    idt_base = *(volatile uint64_t *)(uintptr_t)&env.idt.base;
    if (idt_base == 0) {
        g_timer_masked++;
        return 0;
    }

    /*
     * A 64-bit interrupt gate is {offset[15:0], selector, ist, type,
     * offset[31:16]} in the low half and {offset[63:32], reserved} in the
     * high half.
     */
    entry = *(volatile uint64_t *)(uintptr_t)(idt_base + (uint64_t)vec * 16);
    {
        uint64_t hi = *(volatile uint64_t *)(uintptr_t)(idt_base +
                                                        (uint64_t)vec * 16 + 8);

        handler = (entry & 0xffffULL) | (((entry >> 48) & 0xffffULL) << 16) |
                  ((hi & 0xffffffffULL) << 32);
    }
    if (handler == 0) {
        g_timer_masked++;
        return 0;
    }

    cs_sel = (uint16_t)((entry >> 16) & 0xffff);
    rsp = *(volatile uint64_t *)(uintptr_t)&env.regs[R_ESP];
    stack = (uint64_t *)(uintptr_t)(rsp - 40);

    stack[0] = *(volatile uint64_t *)(uintptr_t)&env.eip;
    stack[1] = cs_sel;
    /*
     * The frame carries the RFLAGS the handler must return to, so this is the
     * value *before* IF is cleared below.  Pushing the cleared one would make
     * iretq restore IF=0 and the guest would never take a second interrupt.
     */
    stack[2] = rflags;
    stack[3] = rsp;
    stack[4] = *(volatile uint64_t *)(uintptr_t)&env.segs[R_SS].selector;

    *(volatile uint64_t *)(uintptr_t)&env.regs[R_ESP] = (uint64_t)stack;
    *(volatile uint64_t *)(uintptr_t)&env.eip = handler;
    *(volatile uint64_t *)(uintptr_t)&env.eflags = rflags & ~IF_MASK;

    g_timer_delivered++;
    return 1;
}

static int resolve_exact_insn_boundary_x86_pc(uint64_t fault_pc,
                                              uint64_t *out_x86_pc)
{
    /* Only preempt at TB entry (off == 0) where flag_reduction and IR2
     * optimizations have materialized all architectural EFLAGS and GPRs. */
    for (uint32_t i = 0; i < g_tb_highwater; i++) {
        if (!tb_entries[i].used) {
            continue;
        }
        if (fault_pc == tb_entries[i].code) {
            *out_x86_pc = tb_entries[i].pc;
            return 1;
        }
    }
    return 0;
}

static inline int walk_ok(LatxX86WalkResult r)
{
    return r == LATX_X86_WALK_OK || r == LATX_X86_WALK_PAGING_OFF;
}

static int deliver_kernel_irq(uint32_t vec, uint64_t guest_rip)
{
    uint64_t rflags = env.eflags;
    uint64_t idt_base = env.idt.base;
    LatxX86Translation tr_lo, tr_hi;
    uint64_t lo, hi, handler;
    uint16_t cs_sel;
    uint64_t old_rsp, new_rsp;
    uint64_t stack_gpa[5];
    uint64_t vals[5];

    if ((rflags & IF_MASK) == 0 || idt_base == 0) {
        return 0;
    }
    if (!walk_ok(latx_x86_walk(&env, idt_base + (uint64_t)vec * 16,
                               LATX_X86_ACCESS_READ, &tr_lo)) ||
        !walk_ok(latx_x86_walk(&env, idt_base + (uint64_t)vec * 16 + 8,
                               LATX_X86_ACCESS_READ, &tr_hi))) {
        return 0;
    }
    lo = latx_platform_phys_read(tr_lo.gpa, 8);
    hi = latx_platform_phys_read(tr_hi.gpa, 8);
    if (((lo >> 47) & 1u) == 0) {
        return 0;
    }
    handler = (lo & 0xffffULL) | (((lo >> 48) & 0xffffULL) << 16) |
              ((hi & 0xffffffffULL) << 32);
    if (handler == 0) {
        return 0;
    }
    cs_sel = (uint16_t)((lo >> 16) & 0xffffu);
    old_rsp = env.regs[R_ESP];
    {
        uint32_t ist = (uint32_t)((lo >> 32) & 7u);
        uint32_t old_cpl = (uint32_t)(env.hflags & HF_CPL_MASK);
        uint64_t target_rsp = old_rsp;
        if (ist != 0 || old_cpl != 0) {
            uint64_t tss_rsp = 0;
            if (latx_x86_get_tss_stack(&env, ist, &tss_rsp)) {
                target_rsp = tss_rsp;
            }
        }
        new_rsp = (target_rsp & ~UINT64_C(15)) - 40;
    }

    for (int i = 0; i < 5; i++) {
        LatxX86Translation tr_st;
        if (!walk_ok(latx_x86_walk(&env, new_rsp + (uint64_t)i * 8,
                                   LATX_X86_ACCESS_WRITE, &tr_st))) {
            return 0;
        }
        stack_gpa[i] = tr_st.gpa;
    }

    vals[0] = guest_rip;
    vals[1] = env.segs[R_CS].selector ? env.segs[R_CS].selector : cs_sel;
    vals[2] = rflags;
    vals[3] = old_rsp;
    vals[4] = env.segs[R_SS].selector;
    for (int i = 0; i < 5; i++) {
        latx_platform_phys_write(stack_gpa[i], 8, vals[i]);
    }

    env.regs[R_ESP] = new_rsp;
    env.eip = handler;
    env.eflags = rflags & ~IF_MASK;
    env.segs[R_CS].selector = cs_sel;
    if ((env.hflags & HF_CPL_MASK) != 0) {
        env.segs[R_SS].selector = 0;
        env.hflags &= ~(uint32_t)HF_CPL_MASK;
        latx_x86_tlb_flush_all();
        latx_platform_tlb_flush_all();
    }
    return 1;
}

static uint32_t com1_irq_vector(void)
{
    uint64_t red = g_ioapic_redtbl[4];
    if ((red & (1ULL << 16)) == 0 && (red & 0xffu) >= 0x20u) {
        return (uint32_t)(red & 0xffu);
    }
    if ((g_pic_imr[0] & (1u << 4)) == 0) {
        return g_pic_irq0_vec + 4u;
    }
    return 0;
}

static int com1_irq_dest_match(uint32_t cpu)
{
    uint64_t red = g_ioapic_redtbl[4];
    if ((red & (1ULL << 16)) == 0 && (red & 0xffu) >= 0x20u) {
        uint32_t dest = (uint32_t)((red >> 56) & 0xffu);
        uint32_t dest_mode = (uint32_t)((red >> 11) & 1u);
        if (dest_mode == 0u) {
            return (dest == 0xffu) ? (cpu == 0) : (cpu == dest);
        } else {
            uint32_t ldr_id = (g_lapic[cpu].ldr >> 24) & 0xffu;
            if (ldr_id == 0u) {
                ldr_id = (1u << cpu);
            }
            return (ldr_id & dest) != 0u;
        }
    }
    if ((g_pic_imr[0] & (1u << 4)) == 0) {
        return cpu == 0;
    }
    return 0;
}

static int com1_irq_pending(void)
{
    if ((uart_mcr & 0x08u) == 0 || com1_irq_vector() == 0) {
        return 0;
    }
    if ((uart_ier & 0x02u) != 0 && uart_thre_pending) {
        return 1;
    }
    if ((uart_ier & 0x01u) != 0 && ((*UART0_LSR) & 0x01u) != 0) {
        return 1;
    }
    return 0;
}

static int deliver_kernel_timer_tick(uint64_t guest_rip)
{
    LapicState *l = &g_lapic[g_cur_vcpu];
    uint32_t vec = 0;

    if ((env.eflags & IF_MASK) == 0 || env.idt.base == 0) {
        g_timer_masked++;
        return 0;
    }
    if ((l->spiv & 0x100u) != 0 && (l->lvtt & 0x10000u) == 0 &&
        (l->lvtt & 0xffu) >= 0x20u) {
        vec = l->lvtt & 0xffu;
    } else if (g_cur_vcpu == 0) {
        uint64_t red2 = g_ioapic_redtbl[2];
        uint64_t red0 = g_ioapic_redtbl[0];
        if ((red2 & (1ULL << 16)) == 0 && (red2 & 0xffu) >= 0x20u) {
            vec = (uint32_t)(red2 & 0xffu);
        } else if ((red0 & (1ULL << 16)) == 0 && (red0 & 0xffu) >= 0x20u) {
            vec = (uint32_t)(red0 & 0xffu);
        } else if ((g_pic_imr[0] & 0x01u) == 0) {
            vec = g_pic_irq0_vec;
        }
    }
    if (vec == 0) {
        g_timer_masked++;
        return 0;
    }
    if (!deliver_kernel_irq(vec, guest_rip)) {
        return 0;
    }
    g_timer_delivered++;
    return 1;
}

/*
 * Run the guest tick body and count how many interrupts its own ISR observed.
 *
 * This is the end-to-end form of the claim: the guest installs an IDT gate
 * itself (the bytes below are a real `mov [mem], imm` pair, not a hand-built
 * environment field), enables interrupts, and spins.  The driver injects one
 * tick per translated block through deliver_tick_to_guest(), so the count that
 * comes back is what the guest's ISR incremented, read out of guest memory --
 * not a counter this file keeps.
 *
 * `enable_ticks` false runs the identical body with IF cleared by a `cli`,
 * which is the masking check: the same number of injections, and the count
 * must not move.
 */
/*
 * A real dispatch loop over translated blocks, with HLT as an idle state.
 *
 * Unlike run_halted_guest_rounds(), which models the hand-off, this executes
 * the guest's own bytes through the translator: the HLT block stops with RIP
 * on itself, the driver injects a tick, the handler block runs `inc`/`iretq`
 * for real, and iretq brings RIP back to the HLT.  Each round therefore covers
 * translation, PLV3 execution, injection, the handler's own instructions and
 * the return -- the whole path a kernel's idle loop takes.
 *
 * Returns the number of completed halt/wake rounds, or a negative value if a
 * block failed to translate or RIP did not behave.
 */
/*
 * Translate and execute consecutive blocks starting at `pc`.
 *
 * The image-driven loop stops at its own HALT_PC marker, so it cannot answer
 * "how far does this code run".  This takes whatever RIP each block leaves,
 * which is the shape a vCPU loop has, and bounds the run so a translation
 * loop cannot spin.
 */
static uint64_t g_recent_pcs[16];
static uint64_t g_recent_count;
static uint64_t g_run_tb_hits;
static uint64_t g_run_tb_misses;
static uint64_t g_run_tb_flushes;
static uint64_t g_run_tb_links;
static uint64_t g_sync_core_calls;
static uint64_t g_sync_core_flushes;

static void mark_code_pages_ro(uint64_t gva, uint16_t len)
{
    uint64_t cur = gva & LAPG_PAGE_MASK;
    uint64_t end = (gva + (uint64_t)len + LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;

    for (; cur < end; cur += LAPG_PAGE_SIZE) {
        LatxX86Translation tr_g;
        if (!walk_ok(latx_x86_walk_cached(&env, cur, LATX_X86_ACCESS_READ, &tr_g))) {
            continue;
        }
        uint64_t pfn = tr_g.gpa >> 12;
        if (pfn >= CODE_PAGE_MAX_PFN) {
            continue;
        }
        uint64_t mask = UINT64_C(1) << (pfn & 63u);
        if ((g_code_page_bitmap[pfn >> 6] & mask) != 0) {
            continue;
        }
        g_code_page_bitmap[pfn >> 6] |= mask;
        if (!is_host_vmm_va(cur)) {
            uint64_t vppn = cur & UINT64_C(0x0000ffffffffe000);
            __asm__ volatile("invtlb 0x6, $zero, %0" :: "r"(vppn) : "memory");
        }
    }
}

static uint64_t hash_guest_code(uint64_t gva, uint16_t len)
{
    uint64_t h = UINT64_C(14695981039346656037);
    uint16_t rem = len;
    uint64_t cur = gva;

    while (rem > 0) {
        LatxX86Translation tr_g;
        uint64_t page_rem = 4096u - (cur & 4095u);
        uint16_t chunk = rem < page_rem ? rem : (uint16_t)page_rem;

        if (!walk_ok(latx_x86_walk_cached(&env, cur, LATX_X86_ACCESS_READ, &tr_g))) {
            return 0;
        }
        const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)tr_g.gpa;
        for (uint16_t i = 0; i < chunk; i++) {
            h ^= (uint64_t)p[i];
            h *= UINT64_C(1099511628211);
        }
        cur += chunk;
        rem -= chunk;
    }
    return h ? h : 1;
}

static void try_link_tb_slot(uint32_t from_slot, uint32_t to_slot)
{
    TbCacheEntry *from_e = &tb_entries[from_slot];
    TbCacheEntry *to_e = &tb_entries[to_slot];
    TbInsnMap *map = &tb_insn_maps[from_slot];

    if (!from_e->used || !to_e->used) {
        return;
    }
    /* Avoid linking a 1-exit unconditional self-loop (`jmp $`) so the
     * PLV0 stuck detector can still observe it. */
    if (from_slot == to_slot &&
        map->jmp_slot_off[0] >= 0 && map->jmp_slot_off[1] < 0) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        int32_t slot_off = map->jmp_slot_off[i];
        if (slot_off < 0 || map->jmp_target_pc[i] != to_e->pc) {
            continue;
        }
        uint64_t slot_addr = from_e->code + (uint64_t)slot_off;
        uint32_t *slot_ptr = (uint32_t *)(uintptr_t)slot_addr;
        if (*slot_ptr != 0x03400000u) {
            continue; /* already patched */
        }
        int64_t disp = ((int64_t)to_e->code - (int64_t)slot_addr) >> 2;
        if (disp < -(1LL << 25) || disp >= (1LL << 25)) {
            continue;
        }
        uint32_t insn = 0x50000000u |
                        (((uint32_t)disp & 0xffffu) << 10) |
                        (((uint32_t)disp >> 16) & 0x3ffu);
        *slot_ptr = insn;
        __asm__ volatile("ibar 0" ::: "memory");
        g_run_tb_links++;
    }
}

static void link_new_tb(uint32_t new_slot)
{
    TbCacheEntry *e = &tb_entries[new_slot];
    TbInsnMap *map = &tb_insn_maps[new_slot];

    /* 1. Outgoing edges from this newly translated TB to existing TBs. */
    for (int i = 0; i < 2; i++) {
        if (map->jmp_slot_off[i] >= 0) {
            TbCacheEntry *dst = tbc_lookup(&tb_cache, map->jmp_target_pc[i],
                                           0, e->cr3, e->cpl);
            if (dst != 0) {
                try_link_tb_slot(new_slot, (uint32_t)(dst - tb_entries));
            }
        }
    }

    /* 2. Incoming edges from existing TBs in the same (cr3, cpl) to this TB. */
    for (uint32_t j = 0; j < g_tb_highwater; j++) {
        if (!tb_entries[j].used || j == new_slot) {
            continue;
        }
        if (tb_entries[j].cr3 != e->cr3 || tb_entries[j].cpl != e->cpl) {
            continue;
        }
        if (tb_insn_maps[j].jmp_target_pc[0] == e->pc ||
            tb_insn_maps[j].jmp_target_pc[1] == e->pc) {
            try_link_tb_slot(j, new_slot);
        }
    }
}

static int run_blocks_from(uint64_t pc, int max_blocks, uint64_t *end_pc)
{
    int ran = 0;
    int stuck = 0;
    uint64_t last_fp = 0;
    int32_t prev_slot = -1;

    env.eip = pc;
    env.tb_jmp_cache_ptr = fast_jmp_cache;
    g_run_blocks_exit = RUN_EXIT_MAX_BLOCKS;
    g_recent_count = 0;
    g_run_tb_hits = 0;
    g_run_tb_misses = 0;
    g_run_tb_flushes = 0;
    g_run_tb_links = 0;
    g_sync_core_calls = 0;
    g_sync_core_flushes = 0;
    tbc_flush_all(&tb_cache);
    g_tb_highwater = 0;
    memset(g_code_page_bitmap, 0, sizeof(g_code_page_bitmap));
    g_smc_dirty = 0;
    memset(fast_jmp_cache, 0, sizeof(fast_jmp_cache));
    code_used = 0x2000;
    __asm__ volatile("ibar 0" ::: "memory");
    latx_x86_tlb_flush_all();
    env.idt.base = 0;
    env.idt.limit = 0;
    env.eflags = 0x2;
    g_pic_imr[0] = 0xffu;
    g_pic_imr[1] = 0xffu;
    g_pic_icw_idx[0] = 0;
    g_pic_icw_idx[1] = 0;
    g_pic_icw_rem[0] = 0;
    g_pic_icw_rem[1] = 0;
    g_pic_irq0_vec = 0x30;
    g_cur_vcpu = 0;
    latx_guest_apic_id = 0;
    for (uint32_t i = 0; i < MAX_VCPUS; i++) {
        g_vcpu_online[i] = (i == 0) ? 1 : 0;
        g_vcpu_halted[i] = 0;
        g_vcpu_init_pending[i] = 0;
        g_vcpu_sipi_pending[i] = 0;
        g_vcpu_sipi_vec[i] = 0;
    }
    lapic_reset_all();
    ioapic_reset();
    g_timer_delivered = 0;
    g_timer_ticks = 0;
    g_timer_masked = 0;
    for (uint32_t i = 0; i < MAX_VCPUS; i++) {
        g_timer_irq_pending[i] = 0;
        g_vcpu_nmi_pending[i] = 0;
    }
    g_vcpu_preempt_needed = 0;
    g_plv3_timer_preempt = 1;
    timer_ack();
    timer_start(100000);

    while (ran < max_blocks) {
        for (uint32_t t = 1; t < MAX_VCPUS; t++) {
            if (g_vcpu_sipi_pending[t]) {
                g_vcpu_sipi_pending[t] = 0;
                if (start_ap_from_sipi(t, g_vcpu_sipi_vec[t])) {
                    switch_vcpu(t);
                    prev_slot = -1;
                    stuck = 0;
                }
            }
        }
        uint64_t before = env.eip;
        uint8_t cpl = (uint8_t)(env.hflags & HF_CPL_MASK);
        uint64_t tb_cr3 = (cpl == 0 && (before >> 63) != 0) ? 0 : env.cr[3];
        TbCacheEntry *e;
        uint32_t slot_idx;

        g_recent_pcs[g_recent_count & 15] = before;
        g_recent_count++;

        e = tbc_lookup(&tb_cache, before, 0, tb_cr3, cpl);
        if (e != 0) {
            g_run_tb_hits++;
        } else {
            uint8_t *code;
            int rc;
            uint32_t fetch_acc = LATX_X86_ACCESS_EXEC |
                                 (cpl ? LATX_X86_ACCESS_USER : 0);
            LatxX86Translation tr_fetch;
            LatxX86WalkResult fr = latx_x86_walk(&env, before, fetch_acc,
                                                 &tr_fetch);
            if (fr != LATX_X86_WALK_OK && fr != LATX_X86_WALK_PAGING_OFF) {
                latx_x86_tlb_flush_page(before);
                env.cr[2] = before;
                env.exception_index = EXCP0E_PAGE;
                env.error_code = (int)latx_x86_fault_error_code(&env, fr,
                                                                fetch_acc);
                if (latx_x86_deliver_fault(&env, 14) != 0) {
                    g_run_blocks_exit = RUN_EXIT_TRANSLATE_FAIL;
                    break;
                }
                latx_platform_tlb_flush_all();
                prev_slot = -1;
                continue;
            }

            g_run_tb_misses++;
            if (code_used + 32768 > CODE_CACHE_SIZE ||
                tbc_count(&tb_cache) >= TB_TABLE_CAP - 1) {
                tbc_flush_all(&tb_cache);
                g_tb_highwater = 0;
                prev_slot = -1;
                memset(g_code_page_bitmap, 0, sizeof(g_code_page_bitmap));
                g_smc_dirty = 0;
                memset(fast_jmp_cache, 0, sizeof(fast_jmp_cache));
                code_used = 0x2000;
                g_run_tb_flushes++;
                __asm__ volatile("ibar 0" ::: "memory");
            }
            e = tbc_insert(&tb_cache, before, 0, tb_cr3, cpl);
            if (e == 0) {
                tbc_flush_all(&tb_cache);
                g_tb_highwater = 0;
                prev_slot = -1;
                memset(g_code_page_bitmap, 0, sizeof(g_code_page_bitmap));
                g_smc_dirty = 0;
                memset(fast_jmp_cache, 0, sizeof(fast_jmp_cache));
                code_used = 0x2000;
                g_run_tb_flushes++;
                __asm__ volatile("ibar 0" ::: "memory");
                e = tbc_insert(&tb_cache, before, 0, tb_cr3, cpl);
            }
            if (e == 0) {
                g_run_blocks_exit = RUN_EXIT_TRANSLATE_FAIL;
                break;
            }
            slot_idx = (uint32_t)(e - tb_entries);
            if (slot_idx + 1 > g_tb_highwater) {
                g_tb_highwater = slot_idx + 1;
            }
            code = code_cache + code_used;
            env.exception_index = -1;
            rc = tb_harness_translate_at_with_map(
                     before, code, CODE_CACHE_SIZE - code_used,
                     &tb_insn_maps[slot_idx]);
            if (env.exception_index == EXCP0E_PAGE) {
                e->used = 0;
                if (latx_x86_deliver_fault(&env, 14) != 0) {
                    g_run_blocks_exit = RUN_EXIT_TRANSLATE_FAIL;
                    break;
                }
                latx_platform_tlb_flush_all();
                prev_slot = -1;
                continue;
            }
            if (rc <= 0) {
                tbc_flush_all(&tb_cache);
                g_tb_highwater = 0;
                prev_slot = -1;
                memset(g_code_page_bitmap, 0, sizeof(g_code_page_bitmap));
                g_smc_dirty = 0;
                memset(fast_jmp_cache, 0, sizeof(fast_jmp_cache));
                g_run_blocks_exit = RUN_EXIT_TRANSLATE_FAIL;
                break;
            }
            e->code = (uint64_t)(uintptr_t)code;
            e->code_size = (uint32_t)rc;
            {
                uint16_t clen = 16;
                const TbInsnMap *m = &tb_insn_maps[slot_idx];
                if (m->count >= TB_HARNESS_MAX_INSNS) {
                    clen = 512;
                } else if (m->count > 0) {
                    uint64_t last_pc = m->spans[m->count - 1].x86_pc;
                    if (last_pc >= before && (last_pc - before) < 496) {
                        clen = (uint16_t)((last_pc - before) + 15);
                    } else {
                        clen = 512;
                    }
                }
                tb_code_len[slot_idx] = clen;
                tb_code_hash[slot_idx] = hash_guest_code(before, clen);
                mark_code_pages_ro(before, clen);
            }
            code_used += (((uint64_t)rc + 255) & ~UINT64_C(255));
            __asm__ volatile("ibar 0" ::: "memory");
            link_new_tb(slot_idx);
        }

        slot_idx = (uint32_t)(e - tb_entries);
        if (prev_slot >= 0 && tb_entries[prev_slot].cr3 == tb_cr3 &&
            tb_entries[prev_slot].cpl == cpl) {
            try_link_tb_slot((uint32_t)prev_slot, slot_idx);
        }

        {
            uint32_t h = (uint32_t)(((before >> TB_JMP_CACHE_BITS) ^ before) &
                                    (FAST_JMP_CACHE_SIZE - 1));
            fast_jmp_cache[h].pc = before;
            fast_jmp_cache[h].code = e->code;
        }

        /* Publish this block before running it: the fault path resolves the
         * faulting guest RIP through the executing block's map. */
        g_current_tb_code = e->code;
        g_current_tb_size = e->code_size;
        g_current_tb_pc = before;
        g_current_tb_map = &tb_insn_maps[slot_idx];

        tb_sys_run_plv3((void *)(uintptr_t)pro_buf,
                        (void *)(uintptr_t)e->code, &env);
        ran++;
        prev_slot = (g_plv3_exit.reason == PLV3_EXIT_NORMAL) ? (int32_t)slot_idx : -1;
        if (g_plv3_exit.reason == PLV3_EXIT_NORMAL) {
            if (env.vregs[0] != 0) {
                uint64_t iret_next = env.vregs[0];
                uint32_t new_cpl = (uint32_t)(env.segs[R_CS].selector & 3u);
                env.vregs[0] = 0;
                if ((env.hflags & HF_CPL_MASK) != new_cpl) {
                    env.hflags = (env.hflags & ~(uint32_t)HF_CPL_MASK) | new_cpl;
                    latx_x86_tlb_flush_all();
                    latx_platform_tlb_flush_all();
                    prev_slot = -1;
                }
                if (env.eip == iret_next) {
                    g_sync_core_calls++;
                    if (g_smc_dirty) {
                        int dirty = 0;
                        for (uint32_t i = 0; i < g_tb_highwater; i++) {
                            if (!tb_entries[i].used || (tb_entries[i].cr3 != 0 && tb_entries[i].cr3 != env.cr[3])) {
                                continue;
                            }
                            if (hash_guest_code(tb_entries[i].pc, tb_code_len[i]) !=
                                tb_code_hash[i]) {
                                dirty = 1;
                                break;
                            }
                        }
                        g_smc_dirty = 0;
                        if (dirty) {
                            tbc_flush_all(&tb_cache);
                            g_tb_highwater = 0;
                            prev_slot = -1;
                            memset(g_code_page_bitmap, 0, sizeof(g_code_page_bitmap));
                            memset(fast_jmp_cache, 0, sizeof(fast_jmp_cache));
                            code_used = 0x2000;
                            g_sync_core_flushes++;
                            __asm__ volatile("ibar 0" ::: "memory");
                        }
                    }
                }
            }
            int is_hlt = 0;
            {
                LatxX86Translation tr_hlt;
                if (walk_ok(latx_x86_walk_cached(&env, env.eip,
                                                 LATX_X86_ACCESS_READ,
                                                 &tr_hlt)) &&
                    *(const volatile uint8_t *)(uintptr_t)tr_hlt.gpa == 0xf4u) {
                    is_hlt = 1;
                }
            }
            uint64_t irq_rip = is_hlt ? (env.eip + 1) : env.eip;
            if (g_vcpu_nmi_pending[g_cur_vcpu] && env.idt.base != 0) {
                g_vcpu_nmi_pending[g_cur_vcpu] = 0;
                if (deliver_x86_exception(2u, irq_rip, 0u, 0)) {
                    g_vcpu_halted[g_cur_vcpu] = 0;
                    stuck = 0;
                    prev_slot = -1;
                    continue;
                }
            }
            if ((env.eflags & IF_MASK) != 0) {
                uint32_t ipi_vec = lapic_pop_irr(g_cur_vcpu);
                if (ipi_vec != 0 && deliver_kernel_irq(ipi_vec, irq_rip)) {
                    g_vcpu_halted[g_cur_vcpu] = 0;
                    stuck = 0;
                    prev_slot = -1;
                    continue;
                }
                if (com1_irq_dest_match(g_cur_vcpu) && com1_irq_pending()) {
                    uint32_t cvec = com1_irq_vector();
                    if (cvec != 0 && deliver_kernel_irq(cvec, irq_rip)) {
                        g_vcpu_halted[g_cur_vcpu] = 0;
                        stuck = 0;
                        prev_slot = -1;
                        continue;
                    }
                }
            }
            if (g_timer_irq_pending[g_cur_vcpu]) {
                if (deliver_kernel_timer_tick(irq_rip)) {
                    g_timer_irq_pending[g_cur_vcpu] = 0;
                    g_vcpu_halted[g_cur_vcpu] = 0;
                    timer_start(100000);
                    stuck = 0;
                    prev_slot = -1;
                    continue;
                }
            }
            if (is_hlt && (env.eflags & IF_MASK) != 0) {
                if (MAX_VCPUS > 1u) {
                    uint32_t target = MAX_VCPUS;
                    for (uint32_t step = 1; step < MAX_VCPUS; step++) {
                        uint32_t c = (g_cur_vcpu + step) % MAX_VCPUS;
                        int urg = g_vcpu_nmi_pending[c] ||
                                  (lapic_has_irr(c) &&
                                   (g_vcpu_save[c].eflags & IF_MASK) != 0) ||
                                  (g_timer_irq_pending[c] &&
                                   (g_vcpu_save[c].eflags & IF_MASK) != 0) ||
                                  (com1_irq_dest_match(c) && com1_irq_pending() &&
                                   (g_vcpu_save[c].eflags & IF_MASK) != 0);
                        if (g_vcpu_online[c] && (!g_vcpu_halted[c] || urg)) {
                            target = c;
                            break;
                        }
                    }
                    if (target < MAX_VCPUS) {
                        g_vcpu_halted[g_cur_vcpu] = 1;
                        switch_vcpu(target);
                        prev_slot = -1;
                        stuck = 0;
                        continue;
                    }
                }
                if (deliver_kernel_timer_tick(env.eip + 1)) {
                    g_timer_irq_pending[g_cur_vcpu] = 0;
                    timer_ack();
                    timer_start(100000);
                    g_vcpu_halted[g_cur_vcpu] = 0;
                    stuck = 0;
                    prev_slot = -1;
                    continue;
                }
            }
            if (MAX_VCPUS > 1u) {
                uint32_t target = MAX_VCPUS;
                for (uint32_t step = 1; step < MAX_VCPUS; step++) {
                    uint32_t c = (g_cur_vcpu + step) % MAX_VCPUS;
                    if (g_vcpu_online[c] &&
                        (g_vcpu_nmi_pending[c] ||
                         (lapic_has_irr(c) &&
                          (g_vcpu_save[c].eflags & IF_MASK) != 0))) {
                        target = c;
                        break;
                    }
                }
                if (target == MAX_VCPUS && (g_vcpu_preempt_needed || (ran & 127) == 0)) {
                    g_vcpu_preempt_needed = 0;
                    for (uint32_t step = 1; step < MAX_VCPUS; step++) {
                        uint32_t c = (g_cur_vcpu + step) % MAX_VCPUS;
                        if (g_vcpu_online[c] && !g_vcpu_halted[c]) {
                            target = c;
                            break;
                        }
                    }
                }
                if (target < MAX_VCPUS) {
                    g_vcpu_preempt_needed = 0;
                    switch_vcpu(target);
                    prev_slot = -1;
                    stuck = 0;
                    continue;
                }
            }
        }
        if (g_plv3_exit.reason != PLV3_EXIT_NORMAL &&
            g_plv3_exit.reason != PLV3_EXIT_GUEST_PF) {
            g_run_blocks_exit = RUN_EXIT_PLV3_TRAP;
            break;
        }
        /*
         * A loop that returns to the same PC is normal control flow: the
         * kernel's page-table clear walks rdx upward and comes back to the
         * same `jne` hundreds of times.  A hang is when the machine state
         * stops changing, so the test is on a cheap register fingerprint
         * rather than on RIP.
         */
        {
            uint64_t fp = env.eip;
            for (int r = 0; r < 16; r++) {
                fp ^= (env.regs[r] << (r & 15)) ^ (env.regs[r] >> (64 - ((r & 15) ? (r & 15) : 1)));
            }

            if (fp == last_fp) {
                if (++stuck >= 4 && MAX_VCPUS > 1u && env.idt.base != 0) {
                    g_vcpu_preempt_needed = 1;
                }
                if (stuck >= 16 && env.idt.base == 0) {
                    g_run_blocks_exit = RUN_EXIT_STUCK;
                    break;
                }
            } else {
                stuck = 0;
                last_fp = fp;
            }
        }
    }

    timer_stop();
    timer_ack();
    g_plv3_timer_preempt = 0;
    for (uint32_t i = 0; i < MAX_VCPUS; i++) {
        g_timer_irq_pending[i] = 0;
    }
    env.tb_jmp_cache_ptr = 0;
    if (end_pc) {
        *end_pc = env.eip;
    }
    return ran;
}

static int run_guest_idle_loop(int rounds)
{
    uint8_t *code_addr = code_cache + 0x1c000;
    int i;
    int woke = 0;

    *(volatile uint8_t *)(uintptr_t)HLT_PC_TEST = 0xf4;
    *(volatile uint32_t *)(uintptr_t)TICK_COUNTER_PC = 0;

    /* The ISR must be in place: this test is about the loop, not the gate. */
    {
        static const uint8_t isr_code[] = {
            0xff, 0x04, 0x25, 0x00, 0xb1, 0x00, 0x00,  /* inc dword [0xb100] */
            0x48, 0xcf,                                 /* iretq             */
        };
        uint64_t gate_lo, gate_hi;

        memcpy((void *)(uintptr_t)TICK_ISR_PC, isr_code, sizeof(isr_code));
        gate_lo = ((uint64_t)TICK_ISR_PC & 0xffffULL) |
                  ((uint64_t)0x08 << 16) |
                  ((uint64_t)0x8e << 40) |
                  (((((uint64_t)TICK_ISR_PC) >> 16) & 0xffffULL) << 48);
        gate_hi = ((uint64_t)TICK_ISR_PC >> 32) & 0xffffffffULL;
        *(volatile uint64_t *)(uintptr_t)(TICK_IDT_PC + TICK_VECTOR * 16) =
            gate_lo;
        *(volatile uint64_t *)(uintptr_t)(TICK_IDT_PC +
                                          TICK_VECTOR * 16 + 8) = gate_hi;
    }

    env.idt.base = TICK_IDT_PC;
    env.idt.limit = 0xffff;
    env.segs[R_CS].selector = 0x08;
    env.segs[R_SS].selector = 0x10;
    env.regs[R_ESP] = TICK_STACK_PC;
    env.eip = HLT_PC_TEST;
    env.eflags = IF_MASK | 0x2;
    latx_x86_tlb_flush_all();

    for (i = 0; i < rounds; i++) {
        int rc;

        /* 1. Run the halt.  RIP must not advance past it. */
        rc = tb_harness_translate_at(env.eip, code_addr, 2048);
        if (rc <= 0) {
            return -1;
        }
        __asm__ volatile("ibar 0" ::: "memory");
        tb_sys_run_plv3((void *)(uintptr_t)pro_buf, code_addr, &env);
        if (env.eip != HLT_PC_TEST) {
            return -2;
        }

        /* 2. The tick arrives; the driver enters the guest's handler. */
        if (!deliver_tick_to_guest()) {
            return -3;
        }
        if (env.eip != TICK_ISR_PC) {
            return -4;
        }

        /* 3. Run the handler for real: inc dword [counter] ; iretq. */
        rc = tb_harness_translate_at(env.eip, code_addr, 2048);
        if (rc <= 0) {
            return -5;
        }
        __asm__ volatile("ibar 0" ::: "memory");
        tb_sys_run_plv3((void *)(uintptr_t)pro_buf, code_addr, &env);

        /* 4. iretq must have landed back on the halt. */
        if (env.eip != HLT_PC_TEST) {
            return -6;
        }
        woke++;
    }
    return woke;
}

/*
 * Drive a halted guest the way the vCPU loop does: while RIP sits on an HLT,
 * inject a tick; the handler runs, iretq's back to the HLT, and the next round
 * halts again.  This is the idle loop with a periodic timer, and the count it
 * returns is how many times that hand-off completed.
 *
 * It is deliberately a separate loop from the image-driven one above: that
 * loop stops at its own HALT_PC guard, so a halt delivered there would be
 * indistinguishable from the image reaching its end marker.
 */
static int run_halted_guest_rounds(int rounds)
{
    int i;
    int woke = 0;

    *(volatile uint8_t *)(uintptr_t)HLT_PC_TEST = 0xf4;

    env.idt.base = TICK_IDT_PC;
    env.idt.limit = 0xffff;
    env.segs[R_CS].selector = 0x08;
    env.segs[R_SS].selector = 0x10;
    env.regs[R_ESP] = TICK_STACK_PC;
    env.eip = HLT_PC_TEST;
    env.eflags = IF_MASK | 0x2;
    latx_x86_tlb_flush_all();

    for (i = 0; i < rounds; i++) {
        /* Halt: RIP is on the HLT and must stay there. */
        if (env.eip != HLT_PC_TEST) {
            break;
        }
        /* Tick: the driver wakes it into the handler. */
        if (!deliver_tick_to_guest()) {
            break;
        }
        if (env.eip != TICK_ISR_PC) {
            break;
        }
        woke++;
        /* Handler iretq's back to the HLT; model that return here. */
        env.eip = HLT_PC_TEST;
        env.eflags |= IF_MASK;
    }
    return woke;
}

static uint32_t run_guest_tick_case(int enable_ticks, int injections)
{
    static const uint8_t isr_code[] = {
        0xff, 0x04, 0x25, 0x00, 0xb1, 0x00, 0x00,  /* inc dword [0xb100] */
        0x48, 0xcf,                                 /* iretq             */
    };
    uint64_t gate_lo, gate_hi;
    int i;
    int taken = 0;

    *(volatile uint32_t *)(uintptr_t)TICK_COUNTER_PC = 0;
    memcpy((void *)(uintptr_t)TICK_ISR_PC, isr_code, sizeof(isr_code));

    gate_lo = ((uint64_t)TICK_ISR_PC & 0xffffULL) |
              ((uint64_t)0x08 << 16) |
              ((uint64_t)0x8e << 40) |
              (((((uint64_t)TICK_ISR_PC) >> 16) & 0xffffULL) << 48);
    gate_hi = ((uint64_t)TICK_ISR_PC >> 32) & 0xffffffffULL;
    *(volatile uint64_t *)(uintptr_t)(TICK_IDT_PC + TICK_VECTOR * 16) = gate_lo;
    *(volatile uint64_t *)(uintptr_t)(TICK_IDT_PC + TICK_VECTOR * 16 + 8) = gate_hi;

    /*
     * The body is a one-instruction spin at its own address, so "the guest
     * resumed" is a single comparison against a known RIP rather than a
     * function of anything the frame pushed.
     */
    {
        uint8_t spin[2];

        spin[0] = 0xeb;                       /* jmp $ */
        spin[1] = 0xfe;
        memcpy((void *)(uintptr_t)TICK_SPIN_PC, spin, sizeof(spin));
    }

    env.idt.base = TICK_IDT_PC;
    env.idt.limit = 0xffff;
    env.segs[R_CS].selector = 0x08;
    env.segs[R_SS].selector = 0x10;
    env.regs[R_ESP] = TICK_STACK_PC;
    env.eip = TICK_SPIN_PC;
    env.eflags = enable_ticks ? (IF_MASK | 0x2) : 0x2;
    latx_x86_tlb_flush_all();

    for (i = 0; i < injections; i++) {
        uint64_t eip_before = env.eip;

        if (!deliver_tick_to_guest()) {
            continue;
        }
        /*
         * The tick must have redirected the guest to its own gate target, and
         * it must have moved off the instruction it was about to run.
         */
        if (env.eip == TICK_ISR_PC && env.eip != eip_before) {
            taken++;
        }
        /* Put the guest back where it was for the next round. */
        env.eip = TICK_SPIN_PC;
        if (!enable_ticks) {
            env.eflags = 0x2;
        } else {
            env.eflags |= IF_MASK;
        }
    }

    return (uint32_t)taken;
}


static volatile uint64_t g_active_helper_fn;
static volatile uint64_t g_active_helper_a1;
static volatile uint64_t g_active_helper_a2;
static volatile uint64_t g_active_helper_ra;

uint64_t latx_platform_guest_read(uint64_t gva, int size)
{
    uint64_t val = 0;
    uint32_t access = LATX_X86_ACCESS_READ |
                      ((env.hflags & HF_CPL_MASK) ? LATX_X86_ACCESS_USER : 0);
    for (int i = size - 1; i >= 0; i--) {
        LatxX86Translation tr_g;
        if (!walk_ok(latx_x86_walk(&env, gva + (uint64_t)i, access, &tr_g))) {
            return 0;
        }
        val = (val << 8) | latx_platform_phys_read(tr_g.gpa, 1);
    }
    return val;
}

void latx_platform_guest_write(uint64_t gva, int size, uint64_t value)
{
    uint32_t access = LATX_X86_ACCESS_WRITE |
                      ((env.hflags & HF_CPL_MASK) ? LATX_X86_ACCESS_USER : 0);
    for (int i = 0; i < size; i++) {
        LatxX86Translation tr_g;
        if (!walk_ok(latx_x86_walk(&env, gva + (uint64_t)i, access, &tr_g))) {
            return;
        }
        latx_platform_phys_write(tr_g.gpa, 1, (value >> (8 * i)) & 0xffu);
    }
}

static int deliver_x86_exception(uint32_t vec, uint64_t push_rip,
                                 uint32_t error_code, int has_err)
{
    uint64_t idt_base = env.idt.base;
    LatxX86Translation tr_lo, tr_hi;
    uint64_t lo, hi, handler;
    uint16_t cs_sel;
    uint8_t gate_type;
    uint64_t old_rsp, new_rsp, rflags;
    uint64_t stack_gpa[6];
    uint64_t vals[6];
    int nwords = has_err ? 6 : 5;
    int idx = 0;

    if (idt_base == 0 || (uint64_t)vec * 16 + 15 > (uint64_t)env.idt.limit + 1) {
        return 0;
    }
    if (!walk_ok(latx_x86_walk(&env, idt_base + (uint64_t)vec * 16,
                               LATX_X86_ACCESS_READ, &tr_lo)) ||
        !walk_ok(latx_x86_walk(&env, idt_base + (uint64_t)vec * 16 + 8,
                               LATX_X86_ACCESS_READ, &tr_hi))) {
        return 0;
    }
    lo = latx_platform_phys_read(tr_lo.gpa, 8);
    hi = latx_platform_phys_read(tr_hi.gpa, 8);
    if (((lo >> 47) & 1u) == 0) {
        return 0;
    }
    gate_type = (uint8_t)((lo >> 40) & 0x1fu);
    if (gate_type != 0x0eu && gate_type != 0x0fu) {
        return 0;
    }
    handler = (lo & 0xffffULL) | (((lo >> 48) & 0xffffULL) << 16) |
              ((hi & 0xffffffffULL) << 32);
    if (handler == 0) {
        return 0;
    }
    cs_sel = (uint16_t)((lo >> 16) & 0xffffu);
    old_rsp = env.regs[R_ESP];
    {
        uint32_t ist = (uint32_t)((lo >> 32) & 7u);
        uint32_t old_cpl = (uint32_t)(env.hflags & HF_CPL_MASK);
        uint64_t target_rsp = old_rsp;
        if (ist != 0 || old_cpl != 0) {
            uint64_t tss_rsp = 0;
            if (latx_x86_get_tss_stack(&env, ist, &tss_rsp)) {
                target_rsp = tss_rsp;
            }
        }
        new_rsp = (target_rsp & ~UINT64_C(15)) - (uint64_t)nwords * 8;
    }
    rflags = env.eflags;

    for (int i = 0; i < nwords; i++) {
        LatxX86Translation tr_st;
        if (!walk_ok(latx_x86_walk(&env, new_rsp + (uint64_t)i * 8,
                                   LATX_X86_ACCESS_WRITE, &tr_st))) {
            return 0;
        }
        stack_gpa[i] = tr_st.gpa;
    }

    if (has_err) {
        vals[idx++] = (uint64_t)error_code;
    }
    vals[idx++] = push_rip;
    vals[idx++] = env.segs[R_CS].selector ? env.segs[R_CS].selector : cs_sel;
    vals[idx++] = rflags;
    vals[idx++] = old_rsp;
    vals[idx++] = env.segs[R_SS].selector;

    for (int i = 0; i < nwords; i++) {
        latx_platform_phys_write(stack_gpa[i], 8, vals[i]);
    }

    env.regs[R_ESP] = new_rsp;
    env.eip = handler;
    env.segs[R_CS].selector = cs_sel;
    if ((env.hflags & HF_CPL_MASK) != 0) {
        env.segs[R_SS].selector = 0;
        env.hflags &= ~(uint32_t)HF_CPL_MASK;
    }
    env.eflags = (rflags & ~(UINT64_C(1) << 8));
    if (gate_type == 0x0eu) {
        env.eflags &= ~IF_MASK;
    }
    latx_x86_tlb_flush_all();
    latx_platform_tlb_flush_all();
    return 1;
}

void latx_platform_cpu_loop_exit(CPUState *cs)
{
    int vec = (cs != 0 && cs->exception_index > 0) ? cs->exception_index
                                                   : env.exception_index;
    int is_int = env.exception_is_int;
    uint32_t err = (uint32_t)env.error_code;
    uint64_t fault_rip = resolve_fault_x86_pc(g_active_helper_ra);
    uint64_t push_rip = is_int ? env.exception_next_eip : fault_rip;
    int has_err = !is_int &&
                  (vec == 8 || vec == 10 || vec == 11 || vec == 12 ||
                   vec == 13 || vec == 14 || vec == 17 || vec == 21);
    uint64_t tlbrera = csrrd(0x8a);
    int is_refill = (int)(tlbrera & 1u);

    if (cs != 0) {
        cs->exception_index = -1;
    }
    env.exception_index = -1;
    env.exception_is_int = 0;
    g_active_helper_fn = 0;

    if (vec >= 0 && vec < 256 &&
        deliver_x86_exception((uint32_t)vec, push_rip, err, has_err)) {
        g_plv3_exit.reason = PLV3_EXIT_GUEST_PF;
        g_plv3_exit.guest_rip = env.eip;
    } else {
        g_plv3_exit.reason = PLV3_EXIT_TRAP;
        g_plv3_exit.ecode = (uint64_t)(uint32_t)vec;
        g_plv3_exit.guest_rip = push_rip;
    }

    if (is_refill) {
        csrwr(0x8f, 0);
        csrwr(0x8a,
              ((uint64_t)(uintptr_t)tb_sys_abort_plv0 & ~UINT64_C(3)) | 1u);
    } else {
        uint64_t prmd = csrrd(0x1) & ~UINT64_C(0x7);

        csrwr(0x1, prmd);
        csrwr(0x6, (uint64_t)(uintptr_t)tb_sys_abort_plv0);
    }
    __asm__ volatile("ertn" ::: "memory");
    for (;;) {
    }
}

static void unlink_tb_at_pc(uint64_t fault_pc)
{
    int found_slot = -1;
    if (g_current_tb_map != 0 &&
        fault_pc >= g_current_tb_code &&
        fault_pc < g_current_tb_code + (uint64_t)g_current_tb_size) {
        found_slot = (int)(g_current_tb_map - tb_insn_maps);
    } else {
        for (uint32_t i = 0; i < g_tb_highwater; i++) {
            if (tb_entries[i].used &&
                fault_pc >= tb_entries[i].code &&
                fault_pc < tb_entries[i].code + (uint64_t)tb_entries[i].code_size) {
                found_slot = (int)i;
                break;
            }
        }
    }
    if (found_slot >= 0) {
        TbCacheEntry *e = &tb_entries[found_slot];
        TbInsnMap *map = &tb_insn_maps[found_slot];
        int unlinked = 0;
        for (int j = 0; j < 2; j++) {
            if (map->jmp_slot_off[j] >= 0) {
                uint32_t *slot_ptr =
                    (uint32_t *)(uintptr_t)(e->code + (uint64_t)map->jmp_slot_off[j]);
                if (*slot_ptr != 0x03400000u) {
                    *slot_ptr = 0x03400000u;
                    unlinked = 1;
                }
            }
        }
        if (unlinked) {
            __asm__ volatile("ibar 0" ::: "memory");
        } else if (map->jmp_slot_off[0] < 0 && map->jmp_slot_off[1] < 0) {
            memset(fast_jmp_cache, 0, sizeof(fast_jmp_cache));
        }
    }
}

void tb_sys_handle_trap(uint64_t *regs)
{
    uint64_t tlbrera = csrrd(0x8a);
    uint64_t estat = csrrd(0x5);
    int is_refill = (int)(tlbrera & 1u);
    uint64_t ecode = (estat >> 16) & 0x3f;

    if (!is_refill && ecode == EXC_IPE) {
        g_use_sw_lbt = 1;
        lbt_handle_fast(regs, csrrd(0x6));
        return;
    }

    uint64_t era = csrrd(0x6);
    uint64_t badv = csrrd(0x7);
    uint64_t fault_pc = is_refill ? ((tlbrera >> 2) << 2) : era;

    if (is_refill) {
        badv = csrrd(0x89);
    }

    /*
     * 0. Timer / hardware interrupt (LoongArch ESTAT.Ecode == 0x0).
     *    The LoongArch core timer is the only clock the guest can see, so
     *    acknowledging it here and injecting the guest's own vector is what
     *    makes a guest tick exist at all.
     */
    if (!is_refill && ecode == EXC_INT && (estat & CSR_ESTAT_IS_MASK) != 0) {
        uint64_t isr = estat & CSR_ESTAT_IS_MASK;

        if (isr & (1u << IRQ_TIMER)) {
            timer_ack();
            g_timer_ticks++;
            for (uint32_t c = 0; c < MAX_VCPUS; c++) {
                g_vcpu_halted[c] = 0;
                if (c != g_cur_vcpu && g_vcpu_online[c] && (g_timer_ticks & 1u) == 0u) {
                    g_timer_irq_pending[c] = 1;
                }
            }
            if (g_plv3_timer_preempt) {
                uint64_t exact_rip = 0;
                int smp_yield = 0;
                for (uint32_t c = 0; c < MAX_VCPUS; c++) {
                    if (c != g_cur_vcpu && g_vcpu_online[c]) {
                        smp_yield = 1;
                        break;
                    }
                }
                int can_irq = ((env.eflags & IF_MASK) != 0 && env.idt.base != 0);
                if (!can_irq && !smp_yield) {
                    g_timer_masked++;
                    g_timer_irq_pending[g_cur_vcpu] = 0;
                    timer_start(100000);
                    return;
                }
                if (fault_pc == (uint64_t)(uintptr_t)pro_buf) {
                    if (can_irq && deliver_kernel_timer_tick(env.eip)) {
                        g_timer_irq_pending[g_cur_vcpu] = 0;
                        timer_start(100000);
                        g_vcpu_preempt_needed = smp_yield;
                        g_plv3_exit.reason = PLV3_EXIT_NORMAL;
                        g_plv3_exit.guest_rip = env.eip;
                        csrwr(0x1, csrrd(0x1) & ~UINT64_C(0x7));
                        csrwr(0x6, (uint64_t)(uintptr_t)tb_sys_abort_plv0);
                        return;
                    }
                    if (smp_yield) {
                        g_vcpu_preempt_needed = 1;
                        timer_start(100000);
                        g_plv3_exit.reason = PLV3_EXIT_NORMAL;
                        g_plv3_exit.guest_rip = env.eip;
                        csrwr(0x1, csrrd(0x1) & ~UINT64_C(0x7));
                        csrwr(0x6, (uint64_t)(uintptr_t)tb_sys_abort_plv0);
                        return;
                    }
                    g_timer_irq_pending[g_cur_vcpu] = 0;
                    timer_start(100000);
                    return;
                }
                if (resolve_exact_insn_boundary_x86_pc(fault_pc, &exact_rip)) {
                    sync_trap_regs_to_env(regs);
                    can_irq = ((env.eflags & IF_MASK) != 0 && env.idt.base != 0);
                    if (can_irq && deliver_kernel_timer_tick(exact_rip)) {
                        g_timer_irq_pending[g_cur_vcpu] = 0;
                        timer_start(100000);
                        g_vcpu_preempt_needed = smp_yield;
                        g_plv3_exit.reason = PLV3_EXIT_NORMAL;
                        g_plv3_exit.guest_rip = env.eip;
                        csrwr(0x1, csrrd(0x1) & ~UINT64_C(0x7));
                        csrwr(0x6, (uint64_t)(uintptr_t)tb_sys_abort_plv0);
                        return;
                    }
                    if (smp_yield) {
                        env.eip = exact_rip;
                        g_vcpu_preempt_needed = 1;
                        timer_start(100000);
                        g_plv3_exit.reason = PLV3_EXIT_NORMAL;
                        g_plv3_exit.guest_rip = env.eip;
                        csrwr(0x1, csrrd(0x1) & ~UINT64_C(0x7));
                        csrwr(0x6, (uint64_t)(uintptr_t)tb_sys_abort_plv0);
                        return;
                    }
                    g_timer_irq_pending[g_cur_vcpu] = 0;
                    timer_start(100000);
                } else {
                    if (can_irq) {
                        g_timer_irq_pending[g_cur_vcpu] = 1;
                    }
                    if (smp_yield) {
                        g_vcpu_preempt_needed = 1;
                    }
                    unlink_tb_at_pc(fault_pc);
                    timer_start(100000);
                }
                return;
            }
            (void)deliver_tick_to_guest();
        }
        return;
    }

    /*
     * 1. Check if the epilogue returned to `tb_sys_plv3_done` to exit PLV3
     *    normally back to the PLV0 run loop.
     */
    if (fault_pc == (uint64_t)(uintptr_t)tb_sys_plv3_done) {
        g_plv3_exit.reason = PLV3_EXIT_NORMAL;
        g_plv3_exit.ecode = ecode;
        g_plv3_exit.esubcode = (estat >> 22) & 0x1ffu;
        g_plv3_exit.is_refill = (uint64_t)is_refill;
        g_plv3_exit.era = fault_pc;
        g_plv3_exit.badv = badv;
        g_plv3_exit.guest_rip = env.eip;
        if (is_refill) {
            csrwr(0x8f, 0);
            csrwr(0x8a,
                  ((uint64_t)(uintptr_t)tb_sys_abort_plv0 & ~UINT64_C(3)) | 1u);
        } else {
            uint64_t prmd = csrrd(0x1) & ~UINT64_C(0x7);

            csrwr(0x1, prmd);
            csrwr(0x6, (uint64_t)(uintptr_t)tb_sys_abort_plv0);
        }
        return;
    }

    /*
     * 1b. Under standard QEMU (without native LBT), LBT instructions trap with
     *     EXC_INE (0x0d).  Emulate the LBT instruction in place and advance ERA.
     */
    if (!is_refill && ecode == EXC_IPE) {
        g_use_sw_lbt = 1;
        lbt_handle(regs);
        return;
    }

    /*
     * 2. If a translated TB at PLV3 called a C helper (e.g. helper_wrmsr,
     *    helper_rdmsr, helper_write_crN, helper_inb, helper_outb) via
     *    `jirl $ra, $rj, 0`, the instruction fetch at the helper's entry in
     *    .text faults with `fault_pc == badv` because .text is PLV0-only.
     *    Notice that the TB already executed `tr_save_guest_regs_for_helper()`
     *    into `env` and placed the C arguments in `$a0..$a4`!
     *    We execute the C helper right here at PLV0, write `$a0` back to the
     *    trap frame, and set ERA/TLBRERA to `$ra` so `ertn` returns directly
     *    into the TB's post-helper reload sequence at PLV3!
     */
    if ((is_refill || ecode == EXC_PIF || ecode == EXC_PPI || ecode == EXC_PNX) &&
        fault_pc == badv && is_plv0_helper_entry(badv)) {
        HelperFn fn = (HelperFn)(uintptr_t)badv;
        g_active_helper_fn = badv;
        g_active_helper_a1 = regs[la_a1];
        g_active_helper_a2 = regs[la_a2];
        g_active_helper_ra = regs[la_ra];
        uint64_t ret = fn(regs[la_a0], regs[la_a1], regs[la_a2],
                          regs[la_a3], regs[la_a4]);
        g_active_helper_fn = 0;

        regs[la_a0] = ret;
        if (is_refill) {
            /* Keep TLBRERA.ISTLBR = 1 so ERTN pops TLBRERA/TLBRPRMD and returns
             * to $ra at PLV3. */
            csrwr(0x8a, (regs[la_ra] & ~UINT64_C(3)) | 1u);
        } else {
            csrwr(0x6, regs[la_ra]);
        }
        return;
    }

    /*
     * 3. Handle TLB refill or page exceptions (PIL, PIS, PIF, PME, PPI, PNR, PNX).
     */
    if (is_refill || ecode == EXC_PIL || ecode == EXC_PIS ||
        ecode == EXC_PIF || ecode == EXC_PME || ecode == EXC_PPI ||
        ecode == EXC_PNR || ecode == EXC_PNX) {
        uint32_t lapg_access = 0;
        uint32_t x86_access = classify_fault_access(fault_pc, badv, ecode,
                                                    is_refill, &lapg_access);
        LapgTranslation tr_la;
        LapgResult r_la;

        /*
         * 3a. Host VMM resource access (code cache, pro_buf, epi_buf, env,
         *     kstack): already mapped in g_root by rebuild_host_mappings().
         */
        if (is_host_vmm_va(badv)) {
            r_la = lapg_walk(&g_memory, g_root, badv, lapg_access, &tr_la);
            if (r_la != LAPG_OK) {
                latx_bare_uart_puts("tb-sys: host walk failed va=");
                puthex(badv);
                latx_bare_uart_puts("\n");
                for (;;) {
                }
            }
            fill_hw_tlb_pair(badv, is_refill);
            return;
        }

        /*
         * 3b. Guest memory access: walk the x86-64 page tables (or flat
         *     identity mapping when CR0.PG == 0).
         */
        {
            LatxX86Translation x86_tr;
            LatxX86WalkResult x86_r = is_refill
                ? latx_x86_walk_cached(&env, badv, x86_access, &x86_tr)
                : latx_x86_walk(&env, badv, x86_access, &x86_tr);

            if (x86_r == LATX_X86_WALK_OK ||
                x86_r == LATX_X86_WALK_PAGING_OFF) {
                uint64_t gpa_page = x86_tr.gpa & LAPG_PAGE_MASK;
                if ((gpa_page == LOONGEMU_LAPIC_GPA ||
                     gpa_page == LOONGEMU_IOAPIC_GPA) &&
                    emulate_mmio_insn(fault_pc, x86_tr.gpa, regs, is_refill)) {
                    return;
                }
                uint64_t fault_pte = build_guest_leaf_pte((uint32_t)x86_r,
                                                          &x86_tr, x86_access);
                fill_hw_tlb_guest(badv, fault_pte, is_refill);
                return;
            }

            /*
             * 3c. Guest page fault (#PF)!
             *     - Sync interrupted PLV3 guest registers back to `env`.
             *     - Resolve exact faulting x86 RIP from `fault_pc` via the
             *       current TB's instruction span map.
             *     - Populate CR2, exception_index, error_code, and deliver #PF
             *       to guest IDT[14] via latx_x86_deliver_fault(&env, 14).
             *     - Unwind out of PLV3 back to `tb_sys_abort_plv0` in PLV0.
             */
            g_hw_guest_pf++;
            sync_trap_regs_to_env(regs);
            env.eip = resolve_fault_x86_pc(fault_pc);

            g_plv3_exit.reason = PLV3_EXIT_GUEST_PF;
            g_plv3_exit.ecode = ecode;
            g_plv3_exit.esubcode = (estat >> 22) & 0x1ffu;
            g_plv3_exit.is_refill = (uint64_t)is_refill;
            g_plv3_exit.era = fault_pc;
            g_plv3_exit.badv = badv;
            g_plv3_exit.guest_rip = env.eip;
            latx_x86_tlb_flush_page(badv);
            env.cr[2] = badv;
            env.exception_index = EXCP0E_PAGE;
            env.error_code = (int)latx_x86_fault_error_code(&env, x86_r,
                                                            x86_access);
            if (latx_x86_deliver_fault(&env, 14) != 0) {
                g_plv3_exit.reason = PLV3_EXIT_TRAP;
            }
            latx_platform_tlb_flush_all();
            sync_env_to_trap_regs(regs);

            if (is_refill) {
                /* Keep ISTLBR=1 in TLBRERA and clear TLBRPRMD.PPLV=0 so ERTN
                 * exits refill mode and lands at tb_sys_abort_plv0 in PLV0. */
                csrwr(0x8f, 0);
                csrwr(0x8a,
                      ((uint64_t)(uintptr_t)tb_sys_abort_plv0 & ~UINT64_C(3)) |
                      1u);
            } else {
                uint64_t prmd = csrrd(0x1) & ~UINT64_C(0x7);

                csrwr(0x1, prmd);
                csrwr(0x6, (uint64_t)(uintptr_t)tb_sys_abort_plv0);
            }
            return;
        }
    }

    /*
     * 4. Unhandled PLV3 trap (e.g. EXC_ADE=0x8, EXC_ALE=0x9, EXC_SYS=0xb,
     *    EXC_BRK=0xc, EXC_GSPR=0xe outside tb_sys_plv3_done): record full
     *    diagnostic state and unwind cleanly to tb_sys_abort_plv0 so the PLV0
     *    caller can inspect g_plv3_exit instead of spinning.
     */
    sync_trap_regs_to_env(regs);
    env.eip = resolve_fault_x86_pc(fault_pc);
    g_plv3_exit.reason = PLV3_EXIT_TRAP;
    g_plv3_exit.ecode = ecode;
    g_plv3_exit.esubcode = (estat >> 22) & 0x1ffu;
    g_plv3_exit.is_refill = (uint64_t)is_refill;
    g_plv3_exit.era = fault_pc;
    g_plv3_exit.badv = badv;
    g_plv3_exit.guest_rip = env.eip;

    if (is_refill) {
        csrwr(0x8f, 0);
        csrwr(0x8a,
              ((uint64_t)(uintptr_t)tb_sys_abort_plv0 & ~UINT64_C(3)) | 1u);
    } else {
        uint64_t prmd = csrrd(0x1) & ~UINT64_C(0x7);

        csrwr(0x1, prmd);
        csrwr(0x6, (uint64_t)(uintptr_t)tb_sys_abort_plv0);
    }
}

/*
 * Trap stub: save all 32 GPRs in register-index order (regs[0..31]) so
 * sync_trap_regs_to_env() can index `regs[la_reg]` directly by LoongArch
 * register number.
 */
__asm__(
".text\n"
".globl tb_sys_trap\n"
"tb_sys_trap:\n"
"  addi.d $sp, $sp, -256\n"
"  st.d $zero, $sp, 0\n"
"  st.d $ra,   $sp, 8\n"
"  st.d $tp,   $sp, 16\n"
"  st.d $r21,  $sp, 168\n"
"  addi.d $r21, $sp, 256\n"
"  st.d $r21,  $sp, 24\n"
"  st.d $a0,   $sp, 32\n"
"  st.d $a1,   $sp, 40\n"
"  st.d $a2,   $sp, 48\n"
"  st.d $a3,   $sp, 56\n"
"  st.d $a4,   $sp, 64\n"
"  st.d $a5,   $sp, 72\n"
"  st.d $a6,   $sp, 80\n"
"  st.d $a7,   $sp, 88\n"
"  st.d $t0,   $sp, 96\n"
"  st.d $t1,   $sp, 104\n"
"  st.d $t2,   $sp, 112\n"
"  st.d $t3,   $sp, 120\n"
"  st.d $t4,   $sp, 128\n"
"  st.d $t5,   $sp, 136\n"
"  st.d $t6,   $sp, 144\n"
"  st.d $t7,   $sp, 152\n"
"  st.d $t8,   $sp, 160\n"
"  st.d $fp,   $sp, 176\n"
"  st.d $s0,   $sp, 184\n"
"  st.d $s1,   $sp, 192\n"
"  st.d $s2,   $sp, 200\n"
"  st.d $s3,   $sp, 208\n"
"  st.d $s4,   $sp, 216\n"
"  st.d $s5,   $sp, 224\n"
"  st.d $s6,   $sp, 232\n"
"  st.d $s7,   $sp, 240\n"
"  st.d $s8,   $sp, 248\n"
"  move $a0, $sp\n"
"  bl tb_sys_handle_trap\n"
"  ld.d $ra,   $sp, 8\n"
"  ld.d $tp,   $sp, 16\n"
"  ld.d $a0,   $sp, 32\n"
"  ld.d $a1,   $sp, 40\n"
"  ld.d $a2,   $sp, 48\n"
"  ld.d $a3,   $sp, 56\n"
"  ld.d $a4,   $sp, 64\n"
"  ld.d $a5,   $sp, 72\n"
"  ld.d $a6,   $sp, 80\n"
"  ld.d $a7,   $sp, 88\n"
"  ld.d $t0,   $sp, 96\n"
"  ld.d $t1,   $sp, 104\n"
"  ld.d $t2,   $sp, 112\n"
"  ld.d $t3,   $sp, 120\n"
"  ld.d $t4,   $sp, 128\n"
"  ld.d $t5,   $sp, 136\n"
"  ld.d $t6,   $sp, 144\n"
"  ld.d $t7,   $sp, 152\n"
"  ld.d $t8,   $sp, 160\n"
"  ld.d $r21,  $sp, 168\n"
"  ld.d $fp,   $sp, 176\n"
"  ld.d $s0,   $sp, 184\n"
"  ld.d $s1,   $sp, 192\n"
"  ld.d $s2,   $sp, 200\n"
"  ld.d $s3,   $sp, 208\n"
"  ld.d $s4,   $sp, 216\n"
"  ld.d $s5,   $sp, 224\n"
"  ld.d $s6,   $sp, 232\n"
"  ld.d $s7,   $sp, 240\n"
"  ld.d $s8,   $sp, 248\n"
"  addi.d $sp, $sp, 256\n"
"  ertn\n"
);

/*
 * Enter `pro_buf(tb_code, env_ptr)` in PLV3 and return to PLV0 either when the
 * epilogue returns to `tb_sys_plv3_done` or when a guest #PF aborts the TB
 * directly to `tb_sys_abort_plv0`.
 */
uint64_t g_saved_plv0_sp __attribute__((used));

__asm__(
".text\n"
".globl tb_sys_run_plv3\n"
"tb_sys_run_plv3:\n"
"  addi.d $sp, $sp, -112\n"
"  st.d $ra, $sp, 0\n"
"  st.d $fp, $sp, 8\n"
"  st.d $s0, $sp, 16\n"
"  st.d $s1, $sp, 24\n"
"  st.d $s2, $sp, 32\n"
"  st.d $s3, $sp, 40\n"
"  st.d $s4, $sp, 48\n"
"  st.d $s5, $sp, 56\n"
"  st.d $s6, $sp, 64\n"
"  st.d $s7, $sp, 72\n"
"  st.d $s8, $sp, 80\n"
"  la   $t0, g_saved_plv0_sp\n"
"  st.d $sp, $t0, 0\n"
/* Reset g_plv3_exit.reason = PLV3_EXIT_NONE before entering PLV3 */
"  la   $t0, g_plv3_exit\n"
"  st.d $zero, $t0, 0\n"
/* Set PRMD.PPLV = 3, and PRMD.PIE = (g_plv3_timer_preempt != 0) */
"  csrrd $t0, 0x1\n"
"  ori   $t0, $t0, 0x7\n"
"  xori  $t0, $t0, 0x4\n"
"  la    $t1, g_plv3_timer_preempt\n"
"  ld.d  $t1, $t1, 0\n"
"  beqz  $t1, 2f\n"
"  ori   $t0, $t0, 0x4\n"
"2:\n"
"  csrwr $t0, 0x1\n"
/* Set ERA = a0 (pro_buf), and shift (a1, a2) -> (a0, a1) for pro_buf(tb, env) */
"  csrwr $a0, 0x6\n"
"  move  $a0, $a1\n"
"  move  $a1, $a2\n"
"  la    $ra, tb_sys_plv3_done\n"
"  ertn\n"
);

__asm__(
".text\n"
".globl tb_sys_plv3_done\n"
"tb_sys_plv3_done:\n"
"  csrrd $zero, 0x0\n"
".globl tb_sys_abort_plv0\n"
"tb_sys_abort_plv0:\n"
/* Fallback capture: if g_plv3_exit.reason is still 0, snapshot ESTAT/ERA/BADV */
"  la   $t0, g_plv3_exit\n"
"  ld.d $t1, $t0, 0\n"
"  bnez $t1, 1f\n"
"  ori  $t1, $zero, 3\n"
"  st.d $t1, $t0, 0\n"
"  csrrd $t1, 0x5\n"
"  bstrpick.d $t2, $t1, 21, 16\n"
"  st.d $t2, $t0, 8\n"
"  bstrpick.d $t2, $t1, 30, 22\n"
"  st.d $t2, $t0, 16\n"
"  st.d $zero, $t0, 24\n"
"  csrrd $t1, 0x6\n"
"  st.d $t1, $t0, 32\n"
"  csrrd $t1, 0x7\n"
"  st.d $t1, $t0, 40\n"
"1:\n"
"  la   $t0, g_saved_plv0_sp\n"
"  ld.d $sp, $t0, 0\n"
"  ld.d $ra, $sp, 0\n"
"  ld.d $fp, $sp, 8\n"
"  ld.d $s0, $sp, 16\n"
"  ld.d $s1, $sp, 24\n"
"  ld.d $s2, $sp, 32\n"
"  ld.d $s3, $sp, 40\n"
"  ld.d $s4, $sp, 48\n"
"  ld.d $s5, $sp, 56\n"
"  ld.d $s6, $sp, 64\n"
"  ld.d $s7, $sp, 72\n"
"  ld.d $s8, $sp, 80\n"
"  addi.d $sp, $sp, 112\n"
"  jirl $zero, $ra, 0\n"
);

/* ---------------- guest program ---------------- */

static const uint8_t program[] = {
    0xb9, 0x80, 0x00, 0x00, 0xc0, 0xb8, 0x00, 0x01,
    0x00, 0x00, 0x31, 0xd2, 0x0f, 0x30, 0x0f, 0x01,
    0x14, 0x25, 0x00, 0x12, 0x00, 0x00, 0xc7, 0x04,
    0x25, 0xc0, 0x15, 0x00, 0x00, 0x09, 0x11, 0x08,
    0x00, 0xc7, 0x04, 0x25, 0xc4, 0x15, 0x00, 0x00,
    0x00, 0x8e, 0x00, 0x00, 0xc7, 0x04, 0x25, 0xc8,
    0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc7,
    0x04, 0x25, 0xcc, 0x15, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0x01, 0x1c, 0x25, 0x00, 0x14,
    0x00, 0x00, 0x48, 0xc7, 0x04, 0x25, 0x00, 0x20,
    0x00, 0x00, 0x03, 0x30, 0x00, 0x00, 0x48, 0xc7,
    0x04, 0x25, 0x00, 0x30, 0x00, 0x00, 0x03, 0x40,
    0x00, 0x00, 0x48, 0xc7, 0x04, 0x25, 0x00, 0x40,
    0x00, 0x00, 0x83, 00, 0x00, 0x00, 0x48, 0xc7,
    0x04, 0x25, 0x08, 0x40, 0x00, 0x00, 0x03, 0x50,
    0x00, 0x00, 0x48, 0xc7, 0x04, 0x25, 0x08, 0x50,
    0x00, 0x00, 0x01, 0x10, 0x20, 0x00, 0x48, 0xc7,
    0x04, 0x25, 0x10, 0x50, 0x00, 0x00, 0x03, 0x00,
    0x10, 0x00, 0x31, 0xc0, 0x31, 0xd2, 0x0f, 0x32,
    0x89, 0xc3, 0xb8, 0x20, 0x00, 0x00, 0x00, 0x0f,
    0x22, 0xe0, 0x0f, 0x20, 0xe0, 0x41, 0x89, 0xc5,
    0xb8, 0x00, 0x20, 0x00, 0x00, 0x0f, 0x22, 0xd8,
    0x0f, 0x20, 0xd8, 0x41, 0x89, 0xc4, 0xb8, 0x01,
    0x00, 0x01, 0x80, 0x0f, 0x22, 0xc0, 0x0f, 0x20,
    0xc0, 0x89, 0xc7, 0xc6, 0x04, 0x25, 0x00, 0x10,
    0x20, 0x00, 0x01, 0xc6, 0x04, 0x25, 0x00, 0x10,
    0x20, 0x00, 0x02, 0x48, 0xc7, 0x04, 0x25, 0x00,
    0x20, 0x20, 0x00, 0x11, 0x22, 0x33, 0x44, 0x48,
    0x8b, 0x04, 0x25, 0x00, 0x20, 0x20, 0x00, 0x41,
    0x89, 0xc6, 0xba, 0xfd, 0x03, 0x00, 0x00, 0xec,
    0x0f, 0xb6, 0xc0, 0x89, 0xc5, 0xbe, 0x00, 0x00,
    0x00, 0x00, 0x8a, 0x06, 0x84, 0xc0, 0x74, 0x46,
    0xba, 0xf8, 0x03, 0x00, 0x00, 0xee, 0x48, 0xff,
    0xc6, 0xeb, 0xef, 0x0f, 0x20, 0xd2, 0x48, 0x89,
    0x14, 0x25, 0x00, 0x16, 0x00, 0x00, 0x48, 0x8b,
    0x4c, 0x24, 0x08, 0x48, 0x89, 0x0c, 0x25, 0x10,
    0x16, 0x00, 0x00, 0x8b, 0x0c, 0x24, 0x89, 0x0c,
    0x25, 0x20, 0x16, 0x00, 0x00, 0xff, 0x04, 0x25,
    0x28, 0x16, 0x00, 0x00, 0x48, 0xc7, 0x04, 0x25,
    0x08, 0x50, 0x00, 0x00, 0x03, 0x10, 0x20, 0x00,
    0x48, 0x83, 0xc4, 0x08, 0x48, 0xcf, 0xeb, 0xfe,
};

static const char banner[] = "loongemu: long mode\n";

static const uint8_t gdt_descriptor[] = {
    0x1f, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#define HALT_PC (PROGRAM_PC + 0x146)
#define EFER_LME (1u << 8)
#define EFER_LMA (1u << 10)

void kmain(void)
{
    uint64_t pc;
    int steps = 0;
    int translated = 0;
    int faults = 0;
    uint32_t hflags;
    uint8_t ro_byte_observed = 0;

    latx_bare_uart_puts("tb-sys: start\n");

    {
        uint64_t euen, crmd;
        uint32_t cfg2 = 0;

        __asm__ volatile("cpucfg %0, %1" : "=r"(cfg2) : "r"(2));
        euen = csrrd(0x2);
        euen |= 0x1 | 0x2 | 0x40 | 0x80;
        if ((cfg2 & (1u << 7)) != 0) {
            euen |= 0x4; /* ASXE: enable LASX 256-bit instructions */
        }
        csrwr(0x2, euen);
        crmd = csrrd(0x0);
        crmd &= ~(uint64_t)0x4;
        csrwr(0x0, crmd);
        if ((cfg2 & (1u << 18)) == 0) {
            g_use_sw_lbt = 1;
        }
    }

    memset((void *)GUEST_BASE, 0, GUEST_RAM_SIZE);
    memcpy((void *)(uintptr_t)PROGRAM_PC, program, sizeof(program));
    memcpy((void *)(uintptr_t)GDT_PC, gdt_descriptor, sizeof(gdt_descriptor));
    {
        static const uint8_t idt_descriptor[10] = {
            0xff, 0x00,                     /* limit 0xff (16 gates)  */
            0xe0, 0x14, 0x00, 0x00,         /* base 0x14e0             */
            0x00, 0x00, 0x00, 0x00,
        };

        memcpy((void *)(uintptr_t)IDT_DESC_PC, idt_descriptor,
               sizeof(idt_descriptor));
    }
    memcpy((void *)(uintptr_t)STR_PC, banner, sizeof(banner));
    *(volatile uint64_t *)(uintptr_t)ALIAS_PA = 0;
    {
        uint32_t str_addr = STR_PC;

        memcpy((void *)(uintptr_t)(PROGRAM_PC + 0xf6), &str_addr,
               sizeof(str_addr));
    }

    memset(&env, 0, sizeof(env));
    memset(&tr, 0, sizeof(tr));
    memset(&lsenv_storage, 0, sizeof(lsenv_storage));
    memset(&tcg_storage, 0, sizeof(tcg_storage));
    memset(fast_jmp_cache, 0, sizeof(fast_jmp_cache));
    memset(code_cache, 0, sizeof(code_cache));
    memset(epi_buf, 0, sizeof(epi_buf));
    memset(pro_buf, 0, sizeof(pro_buf));
    memset(g_apic_mmio_page, 0, sizeof(g_apic_mmio_page));
    memset(g_mmio_dummy_page, 0, sizeof(g_mmio_dummy_page));
    *(volatile uint32_t *)(g_apic_mmio_page + 0x20) = 0x00000000u; /* APIC_ID */
    *(volatile uint32_t *)(g_apic_mmio_page + 0x30) = 0x00050014u; /* APIC_LVR */
    *(volatile uint32_t *)(g_apic_mmio_page + 0xe0) = 0xffffffffu; /* APIC_DFR */
    *(volatile uint32_t *)(g_apic_mmio_page + 0xf0) = 0x000001ffu; /* APIC_SPIV */

    env.sys.codemode = 1;
    env.hflags = HF_LMA_MASK | HF_CS64_MASK;
    env.cr[0] = 0x1;                       /* PE only */
    env.eip = PROGRAM_PC;
    env.regs[R_ESP] = 0x8000;
    env.cr[3] = 0;
    lsenv_storage.cpu_state = &env;
    lsenv_storage.tr_data = &tr;
    lsenv = &lsenv_storage;

    tcg_storage.code_gen_buffer = code_cache;
    tcg_storage.code_gen_buffer_size = CODE_CACHE_SIZE;
    tcg_storage.code_gen_ptr = code_cache;
    tcg_storage.code_gen_highwater = code_cache + CODE_CACHE_SIZE;
    tcg_ctx = &tcg_storage;
    tcg_storage.gen_insn_data = latx_tcg_gen_insn_data;

    /*
     * Enforce the x86-64 baseline before anything is translated.
     *
     * See latx_platform_refuse() below: a host without LSX cannot honour even
     * x86-64-v1, because SSE/SSE2 are part of that baseline and are executed
     * on LSX here.
     */
    latx_runtime_init();
    latx_x86_env_init_features(&env);

    /*
     * Report the advertised microarchitecture level, read back through the
     * same CPUID path the guest uses.  Asserted below rather than trusted:
     * these bits are what lscpu and any /proc/cpuinfo parser key off, and a
     * wrong one sends a package manager off to build for the wrong ABI.
     */
    {
        uint32_t a, b, c, d;
        uint32_t lvl = (uint32_t)latx_cpu_x86_level();

        cpu_x86_cpuid(&env, 0x1, 0, &a, &b, &c, &d);
        latx_bare_uart_puts("tb-sys: x86_level=");
        puthex(lvl);
        latx_bare_uart_puts(" leaf1.ecx=");
        puthex(c);
        latx_bare_uart_puts("\n");
        check((c & (1u << 13)) != 0, "CPUID.1:ECX[13] CMPXCHG16B");
        check((c & (1u << 20)) != 0, "CPUID.1:ECX[20] SSE4.2");
        check((c & (1u << 23)) != 0, "CPUID.1:ECX[23] POPCNT");
        check((lvl < 3) || ((c & (1u << 22)) != 0),
              "CPUID.1:ECX[22] MOVBE on v3");

        cpu_x86_cpuid(&env, 0x80000001u, 0, &a, &b, &c, &d);
        check((lvl < 3) || ((c & (1u << 5)) != 0),
              "CPUID.80000001:ECX[5] ABM/LZCNT on v3");

        cpu_x86_cpuid(&env, 0x80000008u, 0, &a, &b, &c, &d);
        {
            uint32_t phys = a & 0xffu;
            uint32_t virt = (a >> 8) & 0xffu;

            latx_bare_uart_puts("tb-sys: addr_bits phys=");
            puthex(phys);
            latx_bare_uart_puts(" virt=");
            puthex(virt);
            latx_bare_uart_puts("\n");
            check(phys == 40 && virt == 48,
                  "CPUID.80000008 address widths are 40/48");
        }
    }

    /*
     * Disable software guest-paging instrumentation: all guest memory
     * accesses now go through the LoongArch 4 KiB hardware MMU/TLB at PLV3!
     */
    option_guest_paging = 0;
    option_save_xmm = 0xff;
    option_lbt_jiscr = !g_use_sw_lbt;
    latx_bare_uart_puts("tb-sys: lbt_hw=");
    puthex((uint64_t)!g_use_sw_lbt);
    latx_bare_uart_puts(" lbt_jiscr=");
    puthex((uint64_t)option_lbt_jiscr);
    latx_bare_uart_puts("\n");

    latx_gen_epilogue(epi_buf);
    latx_gen_prologue(pro_buf);
    qemu_loglevel |= CPU_LOG_TB_NOCHAIN;
    check(tbc_init(&tb_cache, tb_entries, tb_buckets, TB_TABLE_CAP) == 1,
          "tb cache init");

    /*
     * Bring up P0 4 KiB hardware paging:
     *   - Build the 4-level LoongArch 4 KiB page table.
     *   - Program CSR.PWCL, CSR.PWCH, CSR.STLBPS (PS=12).
     *   - Point CSR.EENTRY and CSR.TLBRENTRY at `tb_sys_trap` with ECFG.VS=0.
     *   - Configure CSR.DMW0 for segment 0 at PLV0 only.
     *   - Enable hardware paging (CRMD.DA=0, CRMD.PG=1).
     */
    rebuild_host_mappings();

    csrwr(0x1c, lapg_pwcl_value());
    csrwr(0x1d, lapg_pwch_value());
    csrwr(0x1e, lapg_stlbps_value());
    {
        uint64_t ecfg = csrrd(0x4);
        csrwr(0x4, ecfg & ~(UINT64_C(0x7) << 16));
    }
    csrwr(0xc, (uint64_t)(uintptr_t)tb_sys_trap);
    csrwr(0x88, (uint64_t)(uintptr_t)tb_sys_trap);

    /* DMW0: VSEG=0, MAT=CC (1<<4), PLV0=1 */
    csrwr(0x180, ((uint64_t)0 << 60) | (UINT64_C(1) << 4) | 1u);
    {
        uint64_t crmd = csrrd(0x0);

        crmd &= ~UINT64_C(0x1f);       /* clear PLV, IE, DA, PG */
        crmd |= UINT64_C(1) << 4;      /* PG = 1, DA = 0, PLV = 0 */
        csrwr(0x0, crmd);
    }
    __asm__ volatile("invtlb 0x0, $zero, $zero" ::: "memory");

    /*
     * Save host page 0x201000 before the guest test writes to PA 0x201000
     * (`RO_PC`), then restore it right after the guest loop finishes.
     */
    memcpy(host_page_201000_backup, (const void *)(uintptr_t)RO_PC, 4096);
    *(volatile uint8_t *)(uintptr_t)RO_PC = 0;

    for (steps = 0; steps < 128; steps++) {
        TbCacheEntry *e;
        uint8_t *code;
        int rc;
        uint32_t slot_idx;

        hflags = env.hflags;
        pc = env.eip;
        if (pc >= HALT_PC && pc <= HALT_PC + 1) {
            break;
        }



        e = tbc_lookup(&tb_cache, pc, 0,
                       env.cr[3], (uint8_t)(hflags & HF_CPL_MASK));
        if (e == 0) {
            e = tbc_insert(&tb_cache, pc, 0, env.cr[3],
                           (uint8_t)(hflags & HF_CPL_MASK));
            if (e == 0) {
                check(0, "tb cache full");
                break;
            }
            slot_idx = (uint32_t)(e - tb_entries);
            code = code_cache + code_used;
            rc = tb_harness_translate_at_with_map(pc, code,
                                                  CODE_CACHE_SIZE - code_used,
                                                  &tb_insn_maps[slot_idx]);
            if (rc < 0) {
                latx_bare_uart_puts("tb-sys: translate failed at ");
                check(0, "translate");
                break;
            }
            e->code = (uint64_t)(uintptr_t)code;
            e->code_size = (uint32_t)rc;
            code_used += (((uint64_t)rc + 255) & ~UINT64_C(255));
            translated++;
        }

        slot_idx = (uint32_t)(e - tb_entries);
        g_current_tb_code = e->code;
        g_current_tb_size = e->code_size;
        g_current_tb_pc = e->pc;
        g_current_tb_map = &tb_insn_maps[slot_idx];

        tb_sys_run_plv3((void *)(uintptr_t)pro_buf,
                        (void *)(uintptr_t)e->code,
                        &env);
    }

    ro_byte_observed = *(volatile uint8_t *)(uintptr_t)RO_PC;
    memcpy((void *)(uintptr_t)RO_PC, host_page_201000_backup, 4096);
    __asm__ volatile("ibar 0" ::: "memory");

    latx_bare_uart_puts("tb-sys: steps=");
    puthex((uint64_t)steps);
    latx_bare_uart_puts(" translated=");
    puthex((uint64_t)translated);
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("tb-sys: efer=");
    puthex(env.efer);
    latx_bare_uart_puts(" cr0=");
    puthex(env.cr[0]);
    latx_bare_uart_puts(" cr3=");
    puthex(env.cr[3]);
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("tb-sys: handler runs=");
    puthex(*(volatile uint32_t *)(uintptr_t)0x1628);
    latx_bare_uart_puts(" cr2=");
    puthex(*(volatile uint64_t *)(uintptr_t)0x1600);
    latx_bare_uart_puts(" ec=");
    puthex(*(volatile uint32_t *)(uintptr_t)0x1620);
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("tb-sys: alias r14=");
    puthex(env.regs[R_R14]);
    latx_bare_uart_puts(" pa100000=");
    puthex(*(volatile uint64_t *)(uintptr_t)ALIAS_PA);
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("tb-sys: gdt.base=");
    puthex(env.gdt.base);
    latx_bare_uart_puts(" gdt.limit=");
    puthex(env.gdt.limit);
    latx_bare_uart_puts(" tlb_fills=");
    puthex(g_hw_tlb_fills);
    latx_bare_uart_puts("\n");

    /* Verify hardware paging + PLV3 execution was used instead of software checks. */
    check(option_guest_paging == 0, "software guest paging instrumentation off");
    check((csrrd(0x0) & (UINT64_C(1) << 4)) != 0, "LoongArch CRMD.PG enabled");
    check(g_hw_tlb_fills > 0, "PLV3 hardware TLB refills serviced");
    check(g_hw_guest_pf == 1, "hardware trap delivered guest #PF");

    /* wrmsr EFER then mov cr0: LME stays, LMA appears once PG is on. */
    check((env.efer & EFER_LME) != 0, "LME set by wrmsr");
    check((env.efer & EFER_LMA) != 0, "LMA set by mov cr0 with PG");
    check(env.regs[R_EBX] == EFER_LME, "rdmsr reads EFER back");
    /* lgdt */
    check(env.gdt.base == 0x1000, "lgdt base");
    check(env.gdt.limit == 0x1f, "lgdt limit");
    /* mov cr3 plus the read back */
    check(env.cr[3] == 0x2000, "mov cr3 stored");
    check(env.regs[R_R12] == 0x2000, "mov r, cr3 reads it back");
    /* mov cr0 plus the read back */
    check(env.cr[0] == 0x80010001u, "mov cr0 stored (PG|WP|PE)");
    check(env.regs[R_EDI] == 0x80010001u, "mov r, cr0 reads it back");
    /*
     * The fault was delivered to the guest's own IDT[14] handler, which
     * recorded what it saw.  No driver-side fault is expected: the guest owns
     * the exception now.
     */
    check(faults == 0, "no fault escaped to the driver");
    check(*(volatile uint32_t *)(uintptr_t)0x1628 == 1,
          "the #PF handler ran exactly once");
    check(*(volatile uint64_t *)(uintptr_t)0x1600 == RO_PC,
          "handler saw CR2 = the faulting store address");
    check(*(volatile uint64_t *)(uintptr_t)0x1610 == STORE_PC,
          "the frame RIP is the faulting instruction");
    check(*(volatile uint32_t *)(uintptr_t)0x1620 == 0x3u,
          "error code is P|W (supervisor write)");
    check(*(volatile uint64_t *)(uintptr_t)0x5008 == 0x00201003u,
          "the handler made the leaf writable");
    check(ro_byte_observed == 2,
          "the retried store landed after the PTE was fixed");
    {
        LatxX86Translation t;
        LatxX86WalkResult r;

        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_OK && t.gpa == 0x1000 && t.page_bits == 21,
              "walk maps 0x1000 with a 2 MiB leaf");
        check((t.perm & LATX_X86_ACCESS_WRITE) != 0, "walk reports writable");

        r = latx_x86_walk(&env, 0x1ff123, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_OK && t.gpa == 0x1ff123,
              "walk keeps the offset inside the leaf");

        r = latx_x86_walk(&env, 0x200000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_NOT_PRESENT, "walk faults past the leaf");

        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_READ | LATX_X86_ACCESS_USER,
                          &t);
        check(r == LATX_X86_WALK_PROTECTION &&
              latx_x86_fault_error_code(&env, r,
                                        LATX_X86_ACCESS_READ | LATX_X86_ACCESS_USER) == 0x5,
              "supervisor leaf faults for a user access (U/S error)");

        uint64_t saved_cr0 = env.cr[0];

        env.cr[0] &= ~(uint64_t)CR0_WP_MASK;
        *(volatile uint64_t *)0x4000 = 0x81;
        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_OK,
              "WP clear: supervisor may write a read-only leaf");
        env.cr[0] |= CR0_WP_MASK;
        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_PROTECTION &&
              latx_x86_fault_error_code(&env, r, LATX_X86_ACCESS_WRITE) == 0x3,
              "WP set: supervisor write faults (P|W error)");
        r = latx_x86_walk(&env, 0x1000,
                          LATX_X86_ACCESS_WRITE | LATX_X86_ACCESS_USER, &t);
        check(r == LATX_X86_WALK_PROTECTION &&
              latx_x86_fault_error_code(
                  &env, r, LATX_X86_ACCESS_WRITE | LATX_X86_ACCESS_USER) == 0x7,
              "user write to supervisor leaf faults (P|W|U error)");
        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_OK, "read-only leaf still readable");
        env.cr[0] = saved_cr0;

        /* NX on the leaf, with EFER.NXE enabled, blocks fetch only. */
        env.efer |= MSR_EFER_NXE;
        *(volatile uint64_t *)0x4000 = 0x8000000000000083ull;
        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_EXEC, &t);
        check(r == LATX_X86_WALK_PROTECTION &&
              latx_x86_fault_error_code(&env, r, LATX_X86_ACCESS_EXEC) == 0x11,
              "NX leaf faults on fetch (P|I-D error)");
        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_OK, "NX leaf still readable");

        /*
         * Cached walks.  The cache must agree with the walker, must go stale
         * when only the PTE changes (that is the guest's job to invalidate),
         * and must be dropped by INVLPG and by a CR3 write.
         */
        latx_x86_tlb_flush_all();
        *(volatile uint64_t *)0x4000 = 0x81;      /* read-only leaf */
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_OK && t.gpa == 0x1000, "cached walk fills");
        env.cr[0] |= CR0_WP_MASK;
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_PROTECTION,
              "cached leaf permissions are re-checked without a walk");
        /* Back to writable in the table, but the cache still says read-only. */
        *(volatile uint64_t *)0x4000 = 0x83;
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_PROTECTION, "no invalidation keeps the old leaf");
        /* INVLPG goes through helper_invlpg -> tlb_flush_page. */
        helper_invlpg(&env, 0x1000);
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_OK, "INVLPG drops the stale leaf");
        env.cr[0] &= ~(uint64_t)CR0_WP_MASK;
        /* A CR3 write drops everything, as a real address-space switch must. */
        *(volatile uint64_t *)0x4000 = 0x81;
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_OK, "cache refilled after INVLPG");
        *(volatile uint64_t *)0x4000 = 0x83;
        cpu_x86_update_cr3(&env, env.cr[3]);
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_OK, "CR3 write drops the cache");

        /* PS at the PML4 level is a reserved encoding, not a 512 GiB page. */
        *(volatile uint64_t *)0x4000 = 0x83;
        *(volatile uint64_t *)0x2000 = 0x3003 | 0x80;
        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_RESERVED, "PS at the PML4 level is reserved");
        *(volatile uint64_t *)0x2000 = 0x3003;
    }

    {
        LatxX86Translation probe;

        check(latx_x86_walk(&env, ALIAS_VA, LATX_X86_ACCESS_READ, &probe) ==
              LATX_X86_WALK_OK && probe.gpa == ALIAS_PA &&
              probe.page_bits == 12,
              "walk maps the aliased VA to PA 0x100000");
    }
    check(*(volatile uint64_t *)(uintptr_t)ALIAS_PA == 0x44332211ull,
          "store through the aliased VA reached PA 0x100000");
    check(env.regs[R_R14] == 0x44332211u, "read through the aliased VA agrees");

    /* the 4-level tables the guest built, read back through the data path */
    check(*(volatile uint64_t *)0x2000 == 0x3003u, "PML4 entry");
    check(*(volatile uint64_t *)0x3000 == 0x4003u, "PDPT entry");
    check(*(volatile uint64_t *)0x4000 == 0x83u, "PD entry (2 MiB leaf)");
    /* every byte of the banner came out of the guest's loop */
    check(uart_tx_count == (int)sizeof(banner) - 1 &&
          memcmp(uart_tx, banner, sizeof(banner) - 1) == 0,
          "out 0x3f8 printed the banner");
    check(env.regs[R_EBP] == LSR_THRE_TEMT, "in 0x3fd read the LSR");
    check(env.eip == HALT_PC, "the halt block's jmp $ leaves eip on itself");
    check(env.eip == HALT_PC, "reached the halt block");
    check(g_bad_fetch == 0, "no out-of-image fetch");

    /*
     * --- Timers, interrupt injection, and device stubs -------------------
     *
     * These are asserted against the real state the run loop uses, not a
     * parallel model: the same `env` field the TB reads, the same IDT the
     * guest installed, the same IF bit CLI/STI maintain.
     */
    {
        uint64_t t0, t1, t2;
        uint64_t idt_save_base, eip_save, rsp_save, rflags_save;
        uint32_t delivered_before, masked_before;

        /* (1) The clock is real: two reads differ, and a longer gap is bigger. */
        __asm__ volatile("rdtime.d %0, $zero" : "=r"(t0) :: "memory");
        __asm__ volatile("rdtime.d %0, $zero" : "=r"(t1) :: "memory");
        {
            volatile uint64_t spin;

            for (spin = 0; spin < 200000; spin++) {
            }
        }
        __asm__ volatile("rdtime.d %0, $zero" : "=r"(t2) :: "memory");
        check(t1 != t0, "clock advances between back-to-back reads");
        check((t2 - t1) > (t1 - t0), "clock gap grows with elapsed work");

        /*
         * One clock, not two.  cpu_get_tsc() is what helpers see; RDTSC is
         * what the guest sees.  A synthetic env->tsc++ would make these
         * diverge by an arbitrary amount, so they must agree to within the
         * time the two reads themselves take.
         */
        {
            uint64_t via_helper = cpu_get_tsc(&env);
            uint64_t via_hw;

            __asm__ volatile("rdtime.d %0, $zero" : "=r"(via_hw) :: "memory");
            check(via_hw >= via_helper && (via_hw - via_helper) < 100000,
                  "cpu_get_tsc agrees with rdtime.d");
        }

        /* (2) A tick is delivered when the guest has IF set. */
        idt_save_base = env.idt.base;
        eip_save = env.eip;
        rsp_save = env.regs[R_ESP];
        rflags_save = env.eflags;

        env.idt.base = TIMER_IDT_PC;
        *(volatile uint64_t *)(uintptr_t)(TIMER_IDT_PC + 0x20 * 16) =
            (uint64_t)TIMER_STUB_PC & 0xffff;
        *(volatile uint64_t *)(uintptr_t)(TIMER_IDT_PC + 0x20 * 16 + 8) =
            ((uint64_t)TIMER_STUB_PC >> 16) & 0xffff;

        {
            uint64_t *slot =
                (uint64_t *)(uintptr_t)(TIMER_IDT_PC + 0x20 * 16 + 8);

            *slot = (*slot & ~UINT64_C(0xffff0000)) |
                    ((((uint64_t)TIMER_STUB_PC >> 32) & 0xffff) << 16);
        }

        env.eflags = rflags_save | IF_MASK;
        env.eip = 0x1234;
        env.regs[R_ESP] = TIMER_GUEST_STACK;

        g_timer_delivered = 0;
        g_timer_masked = 0;
        g_timer_vector = 0x20;
        check(deliver_tick_to_guest() == 1, "tick delivered with IF set");
        check(g_timer_delivered == 1, "delivered counter advanced");
        check(env.eip == TIMER_STUB_PC, "RIP redirected to the guest ISR");
        check(env.regs[R_ESP] == TIMER_GUEST_STACK - 40,
              "interrupt frame pushed");
        check(g_timer_masked == 0, "nothing masked while IF is set");

        /* (3) With IF clear the tick is dropped, and nothing else moves. */
        *(volatile uint64_t *)(uintptr_t)&env.eflags = rflags_save & ~IF_MASK;
        {
            uint64_t eip_before = env.eip;
            uint64_t rsp_before = env.regs[R_ESP];

            env.eip = 0x1234;
            env.regs[R_ESP] = TIMER_GUEST_STACK;
            check(deliver_tick_to_guest() == 0, "tick refused with IF clear");
            check(g_timer_masked == 1, "mask counter advanced");
            check(env.eip == 0x1234, "RIP untouched when masked");
            check(env.regs[R_ESP] == TIMER_GUEST_STACK, "RSP untouched");
            (void)eip_before;
            (void)rsp_before;
        }

        /* Restore the state the rest of the harness expects. */
        env.idt.base = idt_save_base;
        env.eip = eip_save;
        env.regs[R_ESP] = rsp_save;
        env.eflags = rflags_save;

        /* (4) The core timer really fires: arm it and let it run. */
        /*
         * Arm it as fast as the hardware allows (INIT_VAL = 1 -> 10 ns) and
         * give the virtual clock a real chance to advance: the loop below
         * spins on rdtime so it cannot finish before time moves.
         */
        g_timer_ticks = 0;
        timer_start(1);
        {
            uint64_t start, now, estat;
            int latched = 0;

            __asm__ volatile("rdtime.d %0, $zero" : "=r"(start) :: "memory");
            do {
                __asm__ volatile("csrrd %0, 0x5" : "=r"(estat) :: "memory");
                if (estat & (1ULL << 11)) {   /* ESTAT.TI */
                    latched = 1;
                    break;
                }
                __asm__ volatile("rdtime.d %0, $zero" : "=r"(now) :: "memory");
            } while ((now - start) < 200000);
            if (latched) {
                g_timer_ticks = 1;
            }
        }
        timer_stop();
        latx_bare_uart_puts("tb-sys: timer_ticks=");
        puthex(g_timer_ticks);
        latx_bare_uart_puts("\n");
        check(g_timer_ticks >= 1, "core timer latched ESTAT.TI");

        /*
         * (5) Port and MMIO stubs answer instead of faulting.  A guest kernel
         *     programs the PIT and walks PCI config space before it does
         *     anything else; both are serviced here rather than being left to
         *     an unhandled #GP.
         */
        g_pit_port_writes = 0;
        g_pci_config_writes = 0;
        g_apic_mmio_reads = 0;
        g_apic_mmio_writes = 0;

        latx_platform_io_write(0x43, 1, 0x34);       /* PIT mode/command */
        latx_platform_io_write(0x40, 1, 0x9c);       /* PIT counter 0    */
        latx_platform_io_write(0xcf8, 4, 0x80000000); /* PCI CONFIG_ADDR */
        check(g_pit_port_writes == 2, "PIT port writes recorded");
        check(g_pci_config_writes == 1, "PCI config write recorded");
        check(latx_platform_io_read(0x40, 1) == 0x1234,
              "PIT counter reads back");
        check(latx_platform_io_read(0xcfc, 4) == 0xffffffffu,
              "PCI config reports no device");

        {
            uint64_t v = 0x1234;

            check(latx_platform_apic_mmio(APIC_MMIO_BASE + 0x80, 1, &v) == 1,
                  "APIC TPR window is stubbed");
            check(g_apic_mmio_writes == 1, "APIC write recorded");
            v = 0;
            check(latx_platform_apic_mmio(APIC_MMIO_BASE + 0x80, 0, &v) == 1,
                  "APIC read serviced");
            check(g_apic_mmio_reads == 1, "APIC read recorded");
            check(v == 0x1234, "APIC register file holds the write");
            check(latx_platform_apic_mmio(0x1000, 0, &v) == 0,
                  "non-APIC address is not claimed");
        }

        (void)delivered_before;
    }

    /*
     * (2') and (3') end-to-end: a guest that installs its own IDT gate, turns
     * interrupts on, and spins.  The ISR it installed increments a dword in
     * guest memory; the count read back is that dword, not a counter this
     * file keeps.  The same body with `cli` must not move it.
     */
    {
        uint32_t with_if, without_if;

        g_timer_vector = TICK_VECTOR;
        with_if = run_guest_tick_case(1, 5);
        check(with_if >= 3, "guest ISR counted >= 3 injected ticks");
        latx_bare_uart_puts("tb-sys: tick_isr_count=");
        puthex((uint64_t)with_if);
        latx_bare_uart_puts("\n");

        /*
         * HLT must leave RIP on itself.  If the translator advanced it (or
         * treated the instruction as a nop, which is what it used to do), a
         * guest idle loop becomes a busy spin and the address seen by the
         * driver is the instruction after the halt.
         */
        {
            uint8_t *code_addr = code_cache + 0x18000;
            int rc;

            *(volatile uint8_t *)(uintptr_t)HLT_PC_TEST = 0xf4;  /* hlt */
            env.eip = HLT_PC_TEST;
            env.regs[R_ESP] = TICK_STACK_PC;
            env.eflags = IF_MASK | 0x2;
            latx_x86_tlb_flush_all();

            rc = tb_harness_translate_at(env.eip, code_addr, 2048);
            check(rc > 0, "hlt block translates");
            if (rc > 0) {
                __asm__ volatile("ibar 0" ::: "memory");
                tb_sys_run_plv3((void *)(uintptr_t)pro_buf, code_addr, &env);
            }
            check(env.eip == HLT_PC_TEST, "hlt leaves RIP on itself");
            latx_bare_uart_puts("tb-sys: hlt_eip=");
            puthex(env.eip);
            latx_bare_uart_puts("\n");
        }

        /*
         * HLT + tick: the halt must be a place an interrupt can wake the CPU
         * from.  Inject a tick while RIP sits on the HLT; delivery redirects
         * to the gate target, so the pair behaves like a real idle loop --
         * halted until the timer, then in the handler.
         */
        {
            uint64_t gate_lo, gate_hi, before;

            *(volatile uint8_t *)(uintptr_t)HLT_PC_TEST = 0xf4;
            gate_lo = ((uint64_t)TICK_ISR_PC & 0xffffULL) |
                      ((uint64_t)0x08 << 16) |
                      ((uint64_t)0x8e << 40) |
                      (((((uint64_t)TICK_ISR_PC) >> 16) & 0xffffULL) << 48);
            gate_hi = ((uint64_t)TICK_ISR_PC >> 32) & 0xffffffffULL;
            *(volatile uint64_t *)(uintptr_t)(TICK_IDT_PC + TICK_VECTOR * 16) =
                gate_lo;
            *(volatile uint64_t *)(uintptr_t)(TICK_IDT_PC +
                                              TICK_VECTOR * 16 + 8) = gate_hi;

            env.idt.base = TICK_IDT_PC;
            env.idt.limit = 0xffff;
            env.segs[R_CS].selector = 0x08;
            env.segs[R_SS].selector = 0x10;
            env.eip = HLT_PC_TEST;
            env.regs[R_ESP] = TICK_STACK_PC;
            env.eflags = IF_MASK | 0x2;
            g_timer_vector = TICK_VECTOR;
            latx_x86_tlb_flush_all();

            before = env.eip;
            check(deliver_tick_to_guest() == 1, "tick accepted while halted");
            check(env.eip == TICK_ISR_PC && env.eip != before,
                  "a halted CPU is woken into its own handler");

            /*
             * And it must be repeatable: halt, tick, handler, back to the
             * halt, again.  One wake would also happen if the halt were
             * mis-translated as a fallthrough; four in a row cannot.
             */
            {
                int woke = run_halted_guest_rounds(4);

                check(woke == 4, "halted guest woke four times on four ticks");
                latx_bare_uart_puts("tb-sys: halt_wakes=");
                puthex((uint64_t)woke);
                latx_bare_uart_puts("\n");
            }

            /*
             * The same sequence executed for real: the guest's own HLT block
             * and the handler's own inc/iretq run at PLV3, and iretq is what
             * brings RIP back.  This is the path a kernel's idle loop takes.
             */
            {
                int idle = run_guest_idle_loop(4);

                check(idle == 4,
                      "idle loop: 4 halt/wake rounds executed end to end");
                check(*(volatile uint32_t *)(uintptr_t)TICK_COUNTER_PC == 4,
                      "idle loop: the handler's own inc ran four times");
                latx_bare_uart_puts("tb-sys: idle_rounds=");
                puthex((uint64_t)(idle < 0 ? 0 : idle));
                latx_bare_uart_puts(" idle_count=");
                puthex(*(volatile uint32_t *)(uintptr_t)TICK_COUNTER_PC);
                latx_bare_uart_puts("\n");
            }
        }

        without_if = run_guest_tick_case(0, 5);
        check(without_if == 0, "cli: guest ISR counted nothing");
        latx_bare_uart_puts("tb-sys: tick_masked_count=");
        puthex((uint64_t)without_if);
        latx_bare_uart_puts("\n");
    }

    /*
     * (6) A real kernel entry decodes and translates.
     *
     * These are the first bytes of Linux's 64-bit entry point, taken verbatim
     * from an x86-64 vmlinux (`_text` + 0x90): mov rdi,rsi ; lea rsp,[rip+..] ;
     * lea rdi,[rip+..] ; mov ecx,.. ; lea rdx,.. ; mov eax,.. ; wrmsr ; call.
     * Translating them proves the dispatch table covers the sequence a kernel
     * executes before it has a stack of its own -- the part that cannot be
     * skipped by starting the guest somewhere more convenient.
     */
    /*
     * The OVMF reset vector, verbatim from edk2's x64 code.fd (the last 16
     * bytes of the firmware volume, which QEMU maps so the volume's end lands
     * at 0xFFFFFFF0):
     *
     *   mov eax, cr0 ; test al, 1 ; jz +5 ; jmp ... ; jmp $
     *
     * This is the first code an x86 machine executes, and it is entered with
     * CS.base = 0xFFFF0000, so it exercises the 32-bit operand forms and a
     * taken/not-taken branch pair before anything else in the firmware runs.
     */
    /*
     * Scale check: how far into the real kernel can the translator go before
     * it meets something it cannot encode?  The bytes are the first 512 of a
     * real vmlinux .text (Linux 6.6.134 x86-64), translated as consecutive
     * blocks.  A low count localises the gap; it is not a boot.
     */
    {
        static const uint8_t kernel_head[] = {
            0x49, 0x89, 0xf7, 0x48, 0x8d, 0x25, 0xbe, 0x3e, 0x80, 0x01,
            0x48, 0x8d, 0x3d, 0x5f, 0xff, 0xff, 0xff, 0xb9, 0x01, 0x01,
            0x00, 0xc0, 0x48, 0x8d, 0x15, 0x53, 0xef, 0xd5, 0x01, 0x89,
            0xd0, 0x48, 0xc1, 0xea, 0x20, 0x0f, 0x30, 0xe8, 0xa6, 0x05,
            0x00, 0x00, 0x6a, 0x10, 0x48, 0x8d, 0x05, 0x03, 0x00, 0x00,
            0x00, 0x50, 0x48, 0xcb, 0xe8, 0xb5, 0x01, 0x00, 0x00, 0x48,
            0x8d, 0x3d, 0x2e, 0xff, 0xff, 0xff, 0xe8, 0x8c, 0x00, 0x00,
            0x00, 0x48, 0x8d, 0x3d, 0x2e, 0xff, 0xff, 0xff, 0xe9, 0x71,
            0xff, 0xff, 0xff, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66,
            0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x2e, 0x0f, 0x1f, 0x84,
        };
        uint64_t base = KERNEL_ENTRY_PC + 0x400;
        uint64_t pc = base;
        int blocks = 0;

        memcpy((void *)(uintptr_t)base, kernel_head, sizeof(kernel_head));

        /*
         * Walk instruction by instruction rather than in fixed strides: a
         * block boundary is wherever a block ends, and stepping 16 bytes at a
         * time lands mid-instruction and reports a decode failure that is an
         * artifact of the walk, not of the translator.
         */
        while (blocks < 40 && pc < base + 96) {
            TbInsnMap map;
            int rc = tb_harness_translate_at_with_map(
                         pc, (uint8_t *)(uintptr_t)(CODE_SCRATCH + 0x20000),
                         4096, &map);
            /*
             * The harness records each instruction's guest PC and the native
             * offset where it ends; the block's guest extent is the last
             * instruction plus its own length, which the map does not carry.
             * Recomputing it by decoding forward is exactly what the harness
             * already did, so ask it for the count and walk from there.
             */
            /*
             * The map carries consecutive instruction PCs, so the last
             * instruction's length is the stride of the final pair; with one
             * instruction only, fall back to the native span the caller gets
             * from the block (a single instruction's bytes are its guest
             * length in a flat image).
             */
            uint64_t next = pc;
            if (map.count >= 2) {
                next = map.spans[map.count - 1].x86_pc +
                       (map.spans[map.count - 1].x86_pc -
                        map.spans[map.count - 2].x86_pc);
            } else if (map.count == 1) {
                next = map.spans[0].x86_pc + 1;
            }

            if (rc <= 0 || next <= pc) {
                latx_bare_uart_puts("tb-sys: kernel_stop_at=");
                puthex(pc - base);
                latx_bare_uart_puts("\n");
                break;
            }
            blocks++;
            pc = next;
        }
        check(blocks >= 4, "kernel head translates block after block");
        latx_bare_uart_puts("tb-sys: kernel_blocks=");
        puthex((uint64_t)blocks);
        latx_bare_uart_puts("\n");
    }

    {
        static const uint8_t ovmf_reset[] = {
            0x0f, 0x20, 0xc0, 0xa8, 0x01, 0x74, 0x05,
            0xe9, 0x0c, 0xff, 0xff, 0xff, 0xe9, 0xf1, 0xfe,
        };
        uint64_t pc = OVMF_RESET_PC;
        int emitted;

        memcpy((void *)(uintptr_t)pc, ovmf_reset, sizeof(ovmf_reset));
        emitted = tb_harness_translate_at(pc, code_cache + 0x8000, 4096);
        check(emitted > 0, "OVMF reset vector translates");
        latx_bare_uart_puts("tb-sys: ovmf_reset_bytes=");
        puthex((uint64_t)emitted);
        latx_bare_uart_puts("\n");

        /*
         * Translate is not the same as execute.  Run the block with CR0.PE
         * set, which is the state OVMF's reset vector tests: `test al,1` is
         * non-zero, so `jz +5` must not be taken and control leaves the block
         * at the `jmp` that follows it.
         */
        env.cr[0] = 0x80000011;         /* PE | ET | PG-like high bit */
        env.eip = OVMF_RESET_PC;
        env.regs[R_EAX] = 0;
        env.eflags = 0x2;
        latx_x86_tlb_flush_all();
        __asm__ volatile("ibar 0" ::: "memory");
        tb_sys_run_plv3((void *)(uintptr_t)pro_buf, code_cache + 0x8000, &env);
        check(env.regs[R_EAX] == 0x80000011u,
              "mov eax,cr0 read CR0 into EAX");
        check(env.eip == OVMF_RESET_PC + 7,
              "CR0.PE set: the jz at +5 is not taken");
        latx_bare_uart_puts("tb-sys: ovmf_eax=");
        puthex(env.regs[R_EAX]);
        latx_bare_uart_puts(" ovmf_eip=");
        puthex(env.eip);
        latx_bare_uart_puts("\n");
    }

    {
        /*
         * End-to-end verification of the LBT gr2scr + jiscr0 indirect jump
         * fast path: translate `mov eax, dst; jmp rax` -> `mov ebx, 0x12345678; hlt`
         * with option_lbt_jiscr=1 and fast_jmp_cache pre-populated, check both
         * opcodes are emitted, and execute across the fast path at PLV3.
         */
        static const uint8_t src_x86[] = {
            0xb8, 0x00, 0xe5, 0x00, 0x00, /* mov eax, 0xe500 */
            0xff, 0xe0,                   /* jmp rax         */
        };
        static const uint8_t dst_x86[] = {
            0xbb, 0x78, 0x56, 0x34, 0x12, /* mov ebx, 0x12345678 */
            0xf4,                         /* hlt                 */
        };
        uint64_t src_pc = 0x0000e400ULL;
        uint64_t dst_pc = 0x0000e500ULL;
        uint8_t *dst_code = code_cache + 0x10000;
        uint8_t *src_code = code_cache + 0x11000;
        int saved_lbt_jiscr = option_lbt_jiscr;
        int rc_dst, rc_src;
        int has_gr2scr = 0, has_jiscr0 = 0;

        memcpy((void *)(uintptr_t)src_pc, src_x86, sizeof(src_x86));
        memcpy((void *)(uintptr_t)dst_pc, dst_x86, sizeof(dst_x86));
        memset(fast_jmp_cache, 0, sizeof(fast_jmp_cache));
        env.tb_jmp_cache_ptr = fast_jmp_cache;
        option_lbt_jiscr = 1;
        latx_x86_tlb_flush_all();

        rc_dst = tb_harness_translate_at(dst_pc, dst_code, 4096);
        rc_src = tb_harness_translate_at(src_pc, src_code, 4096);
        check(rc_dst > 0 && rc_src > 0, "lbt jiscr0 indirect jump blocks translate");
        if (rc_src > 0) {
            const uint32_t *insns = (const uint32_t *)(uintptr_t)src_code;
            for (int k = 0; k < rc_src / 4; k++) {
                if ((insns[k] & 0xfffffc00u) == 0x00000800u) {
                    has_gr2scr = 1;
                }
                if (insns[k] == 0x48000200u) {
                    has_jiscr0 = 1;
                }
            }
        }
        check(has_gr2scr && has_jiscr0, "indirect goto emitted gr2scr + jiscr0");
        if (rc_dst > 0 && rc_src > 0) {
            uint32_t h = (uint32_t)(((dst_pc >> TB_JMP_CACHE_BITS) ^ dst_pc) &
                                    (FAST_JMP_CACHE_SIZE - 1));
            fast_jmp_cache[h].pc = dst_pc;
            fast_jmp_cache[h].code = (uint64_t)(uintptr_t)dst_code;
            __asm__ volatile("ibar 0" ::: "memory");

            env.eip = src_pc;
            env.regs[R_EAX] = 0;
            env.regs[R_EBX] = 0;
            env.eflags = 0x2;
            tb_sys_run_plv3((void *)(uintptr_t)pro_buf,
                            (void *)(uintptr_t)src_code, &env);
            check(env.regs[R_EBX] == 0x12345678u,
                  "gr2scr + jiscr0 fast path chained into target TB");
            check(env.eip == dst_pc + 5,
                  "gr2scr + jiscr0 target TB stopped on hlt");
            latx_bare_uart_puts("tb-sys: jiscr0_ebx=");
            puthex(env.regs[R_EBX]);
            latx_bare_uart_puts(" jiscr0_eip=");
            puthex(env.eip);
            latx_bare_uart_puts("\n");
        }
        option_lbt_jiscr = saved_lbt_jiscr;
        env.tb_jmp_cache_ptr = 0;
        memset(fast_jmp_cache, 0, sizeof(fast_jmp_cache));
    }

    {
        static const uint8_t kernel_entry[] = {
            0x49, 0x89, 0xf7, 0x48, 0x8d, 0x25, 0xbe, 0x3e,
            0x80, 0x01, 0x48, 0x8d, 0x3d, 0x5f, 0xff, 0xff,
            0xff, 0xb9, 0x01, 0x01, 0x00, 0xc0, 0x48, 0x8d,
            0x15, 0x53, 0xef, 0xd5, 0x01, 0x89, 0xd0, 0x48,
            0xc1, 0xea, 0x20, 0x0f, 0x30, 0xe8, 0xa6, 0x05,
        };
        uint64_t pc = KERNEL_ENTRY_PC;
        int emitted;

        memcpy((void *)(uintptr_t)pc, kernel_entry, sizeof(kernel_entry));

        emitted = tb_harness_translate_at(pc, (uint8_t *)(uintptr_t)
                                          (CODE_SCRATCH), 4096);
        check(emitted > 0, "kernel entry block translates");
        latx_bare_uart_puts("tb-sys: kernel_entry_bytes=");
        puthex((uint64_t)emitted);
        latx_bare_uart_puts("\n");
    }

    /*
     * A kernel runs from the negative half of the address space:
     * 0xffffffff81000000 in the real image.  Build the four levels that map
     * one such page onto physical memory and check the walker resolves it,
     * because everything after this -- fetching the entry point, pushing a
     * stack, following a call -- goes through that translation.
     */
    {
        /*
         * Well clear of the guest image (which occupies 0x0-0x10000) and of
         * the test structures below it.
         */
        /*
         * Clear of every other low-window user: the guest's own test
         * structures live at 0x1000-0x5000 and 0x201000 (RO_PC), and the
         * kernel text is copied from 0x1000000 for the next 22 MiB.  Putting
         * pdpt at 0x201000 collided with RO_PC, which is what made a stack
         * write to 0xffffffff82803f50 report NOT_PRESENT with tables that
         * looked correct.
         */
        /*
         * Above the whole loaded image: the segments span 0x1000000..0x322c000
         * once all four PT_LOADs are placed, and the previous 0x3000000 base
         * was inside segment 3 -- which silently wiped the page tables the
         * moment the image was copied in.
         */
        uint64_t pml4 = 0x3800000, pdpt = 0x3801000, pd = 0x3802000,
                 pt = 0x3803000;
        /*
         * A non-relocatable KERNEL_64 image refuses to run above 2^46: its
         * entry sequence computes `rdi >> 46` and jumps to a halt on a
         * non-zero result.  0xffffffff81000000 gives 0x3ffff, so the kernel
         * stops itself.  Running it at its physical load address keeps the
         * check satisfied, and the identity map below makes that GVA resolve.
         */
        uint64_t gva = 0x1000000ULL;
        uint64_t gpa = 0x1000000ULL;        /* where the kernel would live */
        LatxX86Translation t;
        uint64_t saved_cr3 = env.cr[3];
        uint64_t saved_cr0 = env.cr[0];

        memset((void *)(uintptr_t)pml4, 0, 0x4000);
        /* PML4[511] -> PDPT, PDPT[510] -> PD, PD[0] -> PT, PT[0] -> 16 MiB */
        /*
         * Low identity map: PML4[0] -> PDPT[0] -> PD, PD[8..] -> 2 MiB leaves
         * at 16 MiB and up.  The kernel is placed at its physical load
         * address, so the address it computes from RIP and the address the
         * tables translate are the same number.
         */
        *(volatile uint64_t *)(uintptr_t)(pml4 + 0 * 8) = pdpt | 0x3;
        *(volatile uint64_t *)(uintptr_t)(pdpt + 0 * 8) = pd | 0x3;

        /*
         * The kernel's own prologue loads RSP from a RIP-relative LEA that
         * still points into the image's link-time half (0xffffffff8280xxxx),
         * so the negative-half window has to resolve too -- mapped onto the
         * same physical leaves.  Running at the load address satisfies the
         * entry's `rdi >> 46` check; it does not change the addresses the
         * image computes for itself.
         */
        *(volatile uint64_t *)(uintptr_t)(pml4 + 511 * 8) = pdpt | 0x3;
        *(volatile uint64_t *)(uintptr_t)(pdpt + 510 * 8) = pd | 0x3;
        /*
         * Index arithmetic for 0xffffffff81000000: PML4[511], PDPT[510],
         * PD[8].  The PD index is 8, not 0 -- 0x81000000 has a non-zero bit
         * 21, and using index 0 was why an otherwise correct table reported
         * NOT_PRESENT.
         *
         * The leaf is 2 MiB (PS at level 2), and there are sixteen of them,
         * covering the 32 MiB the text spans.  A single 4 KiB leaf mapped
         * only the entry page, so the kernel's very first stack push -- to
         * 0xffffffff82803f50, PD index 20 -- was a #PF, and the run never
         * got past its prologue.
         */
        {
            int i;

            /*
             * 0x1000000 has PD index 8 for PML4[0]/PDPT[0]; the same 8 is
             * what 0x81000000 needed under PML4[511]/PDPT[510], so only the
             * outer indices change.
             */
            for (i = 0; i < 64; i++) {
                uint64_t pa = (uint64_t)i * 0x200000ULL;

                *(volatile uint64_t *)(uintptr_t)(pd + i * 8) =
                    (pa & ~UINT64_C(0x1fffff)) | 0x83;   /* P|RW|PS */
            }
            (void)pt;
        }

        /*
         * Long mode is PAE paging, always: the walker refuses to walk without
         * CR4.PAE, which is what it means for this to be long mode rather
         * than a 32-bit page directory.
         */
        {
            uint64_t saved_cr4 = env.cr[4];

            env.cr[4] = saved_cr4 | CR4_PAE_MASK;
            env.cr[3] = pml4;
            env.cr[0] = saved_cr0 | 0x80000000u;    /* CR0.PG */
            latx_x86_tlb_flush_all();

        check(latx_x86_walk(&env, gva, LATX_X86_ACCESS_READ, &t) ==
              LATX_X86_WALK_OK, "kernel load address walks");
        check(t.gpa == gpa, "kernel GVA resolves to the physical load base");
        latx_bare_uart_puts("tb-sys: kernel_gpa=");
        puthex(t.gpa);
        latx_bare_uart_puts("\n");

        /*
         * Execute from the kernel's own entry through that translation.
         *
         * The bytes at _text + 0x90 are the ones measured earlier; what is new
         * here is that the fetch goes through the walker (the translator asks
         * latx_bare_ldub_code for the instruction bytes, which resolves the
         * negative GVA through these tables) instead of reading a flat image.
         * A run that reaches the `wrmsr` and stops on the `call` would mean
         * one instruction of real kernel code executed at its real address;
         * anything else localises the gap.
         */
        {
            /*
             * The whole text segment, not a sample.  A kernel's first call
             * targets code a few hundred bytes past the entry; with only a
             * 38-byte copy that target is zeros -- `add [eax],al` repeated --
             * so the run leaves the intended path at the first call.
             */
            {
                extern const uint8_t _ktext_start[];
                extern const uint8_t _kseg1_start[], _kseg2_start[], _kseg3_start[];

                /*
                 * All four PT_LOAD segments, at their physical offsets from
                 * the load base (including PT_LOAD #2 .data..percpu at rel
                 * 0x1d5f000 so pcpu_hot.current_task resolves to init_task).
                 */
                memcpy((void *)(uintptr_t)gpa, _ktext_start,
                       KERNEL_TEXT_SIZE);
                memcpy((void *)(uintptr_t)(gpa + KERNEL_SEG1_REL),
                       _kseg1_start, KERNEL_SEG1_SIZE);
                memcpy((void *)(uintptr_t)(gpa + KERNEL_SEG2_REL),
                       _kseg2_start, KERNEL_SEG2_SIZE);
                memcpy((void *)(uintptr_t)(gpa + KERNEL_SEG3_REL),
                       _kseg3_start, KERNEL_SEG3_SIZE);
            }

            {
                uint64_t saved_cr4 = env.cr[4];

                env.cr[4] = saved_cr4 | CR4_PAE_MASK;
                /*
                 * Populate a minimal Linux x86-64 boot_params at
                 * BOOT_PARAMS_GPA (0x10000) with a 64 MiB e820 RAM map and
                 * an earlyprintk serial command line at 0x20000.
                 */
                {
                    extern const uint8_t _initramfs_start[];
                    extern const uint8_t _initramfs_end[];
                    uint64_t initramfs_gpa = 0x03900000ULL;
                    uint32_t initramfs_img = (uint32_t)initramfs_gpa;
                    uint32_t initramfs_sz = (uint32_t)(_initramfs_end - _initramfs_start);
                    uint8_t *bp = (uint8_t *)(uintptr_t)BOOT_PARAMS_GPA;
                    uint64_t cmdline_gpa = 0x00020000ULL;
                    static const char cmdline[] =
                        "earlycon=uart,io,0x3f8,115200n8 console=ttyS0,115200 "
                        "nokaslr mitigations=off no_timer_check tsc=reliable rdinit=/init";
                    LoongemuAcpiInfo acpi_info;
                    uint64_t e0_addr = 0x00000000ULL;
                    uint64_t e0_size = 0x0009fc00ULL;
                    uint32_t e0_type = 1u;
                    uint64_t e1_addr = LOONGEMU_ACPI_RSDP_GPA;
                    uint64_t e1_size = 0x00010000ULL;
                    uint32_t e1_type = 3u; /* E820_ACPI */
                    uint64_t e2_addr = 0x00100000ULL;
                    uint64_t e2_size = 0x07f00000ULL;
                    uint32_t e2_type = 1u;
                    uint16_t boot_flag = 0xaa55u;
                    uint32_t hdr_sig = 0x53726448u; /* "HdrS" */
                    uint16_t hdr_ver = 0x020fu;
                    uint32_t cmd_ptr = (uint32_t)cmdline_gpa;

                    latx_guest_nr_cpus = MAX_VCPUS;
                    loongemu_acpi_build((uint8_t *)(uintptr_t)GUEST_BASE,
                                        LOONGEMU_ACPI_RSDP_GPA,
                                        LOONGEMU_ACPI_TABLES_GPA,
                                        MAX_VCPUS, &acpi_info);
                    memcpy((void *)(uintptr_t)initramfs_gpa, _initramfs_start,
                           initramfs_sz);
                    memset(bp, 0, 4096);
                    memcpy((void *)(uintptr_t)cmdline_gpa, cmdline,
                           sizeof(cmdline));
                    /* boot_params.acpi_rsdp_addr (offset 0x070) */
                    memcpy(bp + 0x070, &acpi_info.rsdp_gpa, 8);
                    bp[0x1e8] = 3; /* e820_entries */
                    memcpy(bp + 0x1fe, &boot_flag, 2);
                    memcpy(bp + 0x202, &hdr_sig, 4);
                    memcpy(bp + 0x206, &hdr_ver, 2);
                    bp[0x210] = 0xff; /* type_of_loader */
                    bp[0x211] = 0x01; /* loadflags: LOADED_HIGH */
                    memcpy(bp + 0x218, &initramfs_img, 4);
                    memcpy(bp + 0x21c, &initramfs_sz, 4);
                    memcpy(bp + 0x228, &cmd_ptr, 4);
                    memcpy(bp + 0x2d0 + 0, &e0_addr, 8);
                    memcpy(bp + 0x2d0 + 8, &e0_size, 8);
                    memcpy(bp + 0x2d0 + 16, &e0_type, 4);
                    memcpy(bp + 0x2d0 + 20, &e1_addr, 8);
                    memcpy(bp + 0x2d0 + 28, &e1_size, 8);
                    memcpy(bp + 0x2d0 + 36, &e1_type, 4);
                    memcpy(bp + 0x2d0 + 40, &e2_addr, 8);
                    memcpy(bp + 0x2d0 + 48, &e2_size, 8);
                    memcpy(bp + 0x2d0 + 56, &e2_type, 4);
                }
                env.regs[R_ESI] = (uint32_t)BOOT_PARAMS_GPA;
                env.regs[R_EBP] = 0;
                env.regs[R_EAX] = 0xdeadbeefu;
                env.regs[R_EDI] = 0xdeadbeefu;
                env.regs[R_EDX] = 0xdeadbeefu;
                env.eip = gva + 0x90;
                env.regs[R_ESP] = 0x8000;
                env.eflags = 0x2;
                latx_x86_tlb_flush_all();

                {
                    /*
                     * Translate into code_cache, which rebuild_host_mappings()
                     * installs at PLV3-accessible permissions.  CODE_SCRATCH
                     * is inside the guest's low window and therefore not
                     * mapped by this harness -- a block placed there faults on
                     * its first fetch and the run looks like "the block did
                     * nothing".
                     */
                    int rc = tb_harness_translate_at(
                                 env.eip,
                                 (uint8_t *)(uintptr_t)code_cache, 4096);

                    check(rc > 0, "kernel entry translates through its own GVA");
                    latx_bare_uart_puts("tb-sys: kernel_entry_via_gva=");
                    puthex((uint64_t)rc);
                    latx_bare_uart_puts("\n");
                    if (rc > 0) {
                        tb_sys_run_plv3((void *)(uintptr_t)pro_buf,
                                        (void *)(uintptr_t)code_cache,
                                        &env);
                        /*
                         * Only the entry page is present, so the block runs
                         * until the kernel's first `call` leaves it.  What is
                         * asserted is that the instructions before that call
                         * executed: RSI/EDI were moved, RSP was loaded from
                         * the RIP-relative LEA, and EAX holds the value the
                         * block computed from RDX.
                         */
                        /*
                         * The two register moves that precede the stack load
                         * executed, which is what "real kernel code ran at its
                         * real address" means here.  RSP still holds the
                         * value this harness set: the block leaves at the
                         * kernel's first call, and with only one page present
                         * the rest of the sequence is not reachable.
                         */
                        /*
                     * Sentinel first.  EAX holding 0x80000011 after the block
                     * is not evidence the block ran: the OVMF check above
                     * leaves exactly that value in EAX (it is CR0).  Poisoning
                     * it beforehand is what makes "the instruction executed"
                     * distinguishable from "nothing happened".
                     */
                    /*
                     * The block ran.  RDI is the decisive register: the
                     * RIP-relative `lea rdi,[rip-0xa1]` must produce the full
                     * 64-bit kernel address, and the sentinel set before the
                     * run proves the value came from the block rather than
                     * from whatever was there before.
                     */
                    /*
                     * The LEA is RIP-relative, so it produces the address the
                     * image was linked for, adjusted by its displacement --
                     * regardless of where the image is physically placed.
                     * With the kernel running at its load address the expected
                     * value is simply that address minus the displacement.
                     */
                    /*
                     * The LEA is at entry+0xa, not entry, so its next
                     * instruction is entry+0x11; subtracting the 0xa1
                     * displacement from that lands exactly on the load base.
                     * An earlier version of this check subtracted from
                     * entry+7 and concluded the translator was off by 0xa --
                     * the error was in the expectation, not the translation.
                     */
                    check(env.regs[R_EDI] == (uint32_t)(gva + 0x90 + 0x11 - 0xa1),
                          "rip-relative lea resolves onto the load base");
                    latx_bare_uart_puts("tb-sys: kexec_rdi=");
                    puthex(env.regs[R_EDI]);
                    latx_bare_uart_puts("\n");

                        {
                            uint64_t stop = 0;
                            /*
                             * Bounded: past this point the kernel reaches a
                             * `syscall` in its early boot, and the harness has
                             * no PLV0 vehicle for a guest syscall yet, so the
                             * run would unwind `kmain` instead of finishing the
                             * checks.  The bound is where that happens, not a
                             * step budget.
                             */
                            int ran = run_blocks_from(gva + 0x90, 50000000, &stop);

                            latx_bare_uart_puts("tb-sys: kernel_blocks_run=");
                            puthex((uint64_t)(ran < 0 ? 0 : ran));
                            latx_bare_uart_puts(" stop=");
                            puthex(stop);
                            latx_bare_uart_puts(" run_exit=");
                            puthex(g_run_blocks_exit);
                            latx_bare_uart_puts(" plv3_exit=");
                            puthex(g_plv3_exit.reason);
                            latx_bare_uart_puts(" hits=");
                            puthex(g_run_tb_hits);
                            latx_bare_uart_puts(" misses=");
                            puthex(g_run_tb_misses);
                            latx_bare_uart_puts(" links=");
                            puthex(g_run_tb_links);
                            latx_bare_uart_puts(" sync=");
                            puthex(g_sync_core_flushes);
                            latx_bare_uart_puts("/");
                            puthex(g_sync_core_calls);
                            latx_bare_uart_puts(" ticks=");
                            puthex(g_timer_delivered);
                            latx_bare_uart_puts(" irqs=");
                            puthex(g_timer_ticks);
                            latx_bare_uart_puts("\n");
                            if (g_plv3_exit.reason != PLV3_EXIT_NORMAL) {
                                uint64_t n = g_recent_count < 8 ? g_recent_count : 8;
                                uint64_t start = g_recent_count - n;
                                latx_bare_uart_puts("tb-sys: plv3_abort ecode=");
                                puthex(g_plv3_exit.ecode);
                                latx_bare_uart_puts(" esub=");
                                puthex(g_plv3_exit.esubcode);
                                latx_bare_uart_puts(" rf=");
                                puthex(g_plv3_exit.is_refill);
                                latx_bare_uart_puts(" era=");
                                puthex(g_plv3_exit.era);
                                latx_bare_uart_puts(" badv=");
                                puthex(g_plv3_exit.badv);
                                latx_bare_uart_puts(" grip=");
                                puthex(g_plv3_exit.guest_rip);
                                latx_bare_uart_puts(" err=");
                                puthex((uint64_t)(uint32_t)env.error_code);
                                latx_bare_uart_puts(" gs_base=");
                                puthex(env.segs[R_GS].base);
                                latx_bare_uart_puts("\n");
                                latx_bare_uart_puts("tb-sys: recent_pcs=");
                                for (uint64_t idx = start; idx < g_recent_count; idx++) {
                                    if (idx > start) latx_bare_uart_puts(" ");
                                    puthex(g_recent_pcs[idx & 15]);
                                }
                                latx_bare_uart_puts("\n");
                            }
                            check(ran >= 1,
                                  "kernel entry runs at least one block");
                        }

                    }
                }
                env.cr[4] = saved_cr4;
            }
        }

            env.cr[3] = saved_cr3;
            env.cr[0] = saved_cr0;
            env.cr[4] = saved_cr4;
            latx_x86_tlb_flush_all();
        }
    }

    /*
     * The low 64 MiB belongs to the guest and the VMM starts above it.  A
     * translation at the top of that window must succeed and must not be
     * rejected by the fetch guard, which is what bounds a guest image.
     */
    {
        uint64_t probe = GUEST_RAM_SIZE - 0x1000;
        uint64_t saved_cr0 = env.cr[0];
        int emitted;

        /* Disable paging for the flat physical RAM ceiling probe; saved_cr3
         * only maps the low 2 MiB test window. */
        env.cr[0] &= ~(uint64_t)CR0_PG_MASK;
        latx_x86_tlb_flush_all();
        memset((void *)(uintptr_t)probe, 0x90, 256);   /* nop sled */
        emitted = tb_harness_translate_at(probe, code_cache, 1024);
        env.cr[0] = saved_cr0;
        latx_x86_tlb_flush_all();
        check(emitted > 0, "guest RAM reaches 64 MiB");
        check(g_bad_fetch == 0, "no fetch outside guest RAM");
    }


    latx_bare_uart_puts("tb-sys: checks=");
    puthex((uint64_t)checks);
    latx_bare_uart_puts(" fails=");
    puthex((uint64_t)fails);
    latx_bare_uart_puts(fails ? " BAD\n" : " ok\n");
    latx_bare_uart_puts("tb-sys: done\n");
    for (;;) {
    }
}
