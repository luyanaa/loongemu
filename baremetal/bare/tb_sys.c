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
#include "loongemu/hyperv.h"

#include "loongemu/loongarch_paging.h"
#include "loongemu/acpi_tables.h"
#include "loongemu/smbios_tables.h"
#include "loongemu/ovmf_board.h"
#include "loongemu/pci_host.h"
#include "loongemu/vmm_handoff.h"
#include "loongemu/tb_cache.h"
#include "loongemu/x86_64_paging.h"
#include "loongemu/phys_mem.h"
#include "loongemu/vclock.h"
#include "loongemu/loongarch_smp.h"
#include "lbt_emu.h"
#include "tb_harness.h"

extern void helper_outb(CPUX86State *env, uint32_t port, uint32_t data);
extern void helper_outw(CPUX86State *env, uint32_t port, uint32_t data);
extern void helper_outl(CPUX86State *env, uint32_t port, uint32_t data);
extern void latx_x86_reset_real_mode(CPUX86State *env);
extern uint32_t latx_guest_nr_cpus;
extern uint32_t latx_guest_apic_id;
extern uint64_t latx_host_timer_hz;
extern uint32_t latx_hv_vendor;
extern uint64_t latx_random_u64(void);
#define GUEST_RAM_SIZE (128u * 1024u * 1024u)
#define BOCHS_FB_FALLBACK_SIZE (4u * 1024u * 1024u)


static uint64_t g_uart_base = 0x1fe001e0ULL;
static uint8_t g_uart_reg_shift;
#define UART0_THR ((volatile unsigned char *)(uintptr_t)(g_uart_base + (0u << g_uart_reg_shift)))
#define UART0_RBR ((volatile unsigned char *)(uintptr_t)(g_uart_base + (0u << g_uart_reg_shift)))
#define UART0_LSR ((volatile unsigned char *)(uintptr_t)(g_uart_base + (5u << g_uart_reg_shift)))

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

static LoongemuSpinLock g_uart_lock;

void latx_bare_uart_putc(int c)
{
    /* Poll 16550 LSR.THRE (bit 5) for real LoongArch hardware UARTs. */
    for (uint32_t spin = 0; spin < 100000u; spin++) {
        if (((*UART0_LSR) & 0x20u) != 0u) {
            break;
        }
    }
    *UART0_THR = (unsigned char)c;
}
void latx_bare_uart_puts(const char *s)
{
    loongemu_spin_lock(&g_uart_lock);
    while (*s) {
        latx_bare_uart_putc(*s++);
    }
    loongemu_spin_unlock(&g_uart_lock);
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
static volatile uint32_t g_pcpu_online[MAX_VCPUS];
static volatile uint32_t g_phys_cpus_online = 1u;
static volatile uint32_t g_pcpu_vcpu[MAX_VCPUS];
static volatile uint32_t g_vp_owner[MAX_VCPUS];
static volatile int g_vcpu_online[MAX_VCPUS];
static volatile int g_vcpu_halted[MAX_VCPUS];
static volatile int g_vcpu_kicked[MAX_VCPUS];
static volatile int g_vcpu_init_pending[MAX_VCPUS];
static volatile int g_vcpu_sipi_pending[MAX_VCPUS];
static volatile uint32_t g_vcpu_sipi_vec[MAX_VCPUS];
static CPUX86State g_vcpu_env[MAX_VCPUS] __attribute__((aligned(4096)));
static inline uint32_t cur_pcpu(void)
{
    uint32_t id = loongemu_phys_cpu_id();
    return (id < MAX_VCPUS) ? id : 0u;
}
#define g_cur_vcpu (g_pcpu_vcpu[cur_pcpu()])
#define cur_env    (&g_vcpu_env[g_cur_vcpu])
#define env        (*cur_env)
uint32_t latx_platform_cpu_id(void)  { return g_cur_vcpu; }
uint32_t latx_platform_apic_id(void) { return g_cur_vcpu; }
static uint8_t g_pic_imr[2] = { 0xffu, 0xffu };
static uint8_t g_pic_icw_idx[2];
static uint8_t g_pic_icw_rem[2];
static uint32_t g_pic_irq0_vec = 0x30;
volatile uint64_t g_plv3_timer_preempt_arr[MAX_VCPUS] __attribute__((used));
#define g_plv3_timer_preempt (g_plv3_timer_preempt_arr[cur_pcpu()])
static volatile int g_timer_irq_pending[MAX_VCPUS];
static volatile int g_vcpu_nmi_pending[MAX_VCPUS];
static volatile int g_vcpu_preempt_needed_arr[MAX_VCPUS];
#define g_vcpu_preempt_needed (g_vcpu_preempt_needed_arr[cur_pcpu()])
static int deliver_x86_exception(uint32_t vec, uint64_t push_rip,
                                 uint32_t error_code, int has_err);
void helper_hypercall(CPUX86State *e);
uint64_t hv_request_flush_all(void);
int hv_flush_barrier_ready(uint64_t ticket);
int hv_release_flush_barrier(uint64_t ticket);
void hv_cancel_flush_barrier(uint64_t ticket);
static uint64_t hv_barrier_now(void);
static uint64_t hv_barrier_timeout_ticks(void);
static void hv_barrier_watchdog_fail(uint64_t ticket) __attribute__((noreturn));
static LoongemuMemMap g_mem_map;
static LoongemuVClock g_vclock;
static OvmfBoard g_ovmf_board;
static LoongemuPciHost g_pci_host;
static LoongemuVmmHandoff g_vmm_handoff;
static uint8_t *g_ovmf_code_rom;
static uint8_t *g_ovmf_vars_flash;
static uint64_t g_bochs_fb_fallback_hpa;
static uint64_t g_bochs_fb_bar_gpa;
static uint64_t g_bochs_fb_bar_size;
static uint64_t g_bochs_fb_valid_size;
static uint64_t g_bochs_fb_sink_gpa;
static uint64_t g_bochs_fb_sink_size;
static uint64_t g_bochs_mmio_bar_gpa;
static uint64_t g_bochs_mmio_bar_size;
typedef struct BochsBarPending {
    uint64_t gpa;
    uint64_t size;
    uint32_t owner_vcpu;
    uint8_t enabled;
    uint8_t valid;
    uint8_t reserved[2];
} BochsBarPending;
static BochsBarPending g_bochs_bar_pending[2];
static volatile int g_bochs_mmio_bar_write;
static int bochs_bar_pending_present(void);
static int bochs_helper_context(void);
static void bochs_set_helper_bar_pending(uint32_t pending);
static void switch_vcpu(uint32_t next_id);
static int bochs_process_pending_bar_maps(void);
static uint64_t g_guest_ram_size = GUEST_RAM_SIZE;
static int g_guest_ram_external;
static int g_ovmf_storage_external;
static int g_phys_mem_initialized;
#ifndef LOONGEMU_BOOT_OVMF
static uint8_t g_fb_fallback_pool[BOCHS_FB_FALLBACK_SIZE]
    __attribute__((aligned(4096), section(".fb_fallback")));
static uint8_t g_default_ovmf_code[LOONGEMU_OVMF_DEFAULT_ROM_SZ]
    __attribute__((aligned(4096), section(".ovmf_store")));
static uint8_t g_default_ovmf_vars[LOONGEMU_OVMF_DEFAULT_VARS_SZ]
    __attribute__((aligned(4096), section(".ovmf_store")));
#endif
static LoongemuSpinLock g_bochs_bar_lock;
static LoongemuSpinLock g_lapic_lock;
static LoongemuSpinLock g_ioapic_lock;
static LoongemuSpinLock g_pic_lock;
static LoongemuSpinLock g_translate_lock;
static LoongemuSpinLock g_code_page_lock;
static LoongemuSpinLock g_hv_barrier_lock;
static volatile uint64_t g_hv_barrier_generation;
static volatile uint64_t g_hv_barrier_active;
static volatile uint64_t g_hv_barrier_released;
static volatile uint64_t g_hv_barrier_targets;
static volatile uint32_t g_hv_barrier_coordinator;
static volatile uint64_t g_hv_barrier_last_seen[MAX_VCPUS];
static volatile uint64_t g_hv_barrier_timeouts;
static volatile uint64_t g_isolation_violations;
static volatile uint64_t g_tlb_shootdowns;
static volatile uint64_t g_ipi_shootdowns_handled;
static volatile uint64_t g_ipi_smc_handled;

uint64_t latx_platform_io_read(uint32_t port, int size)
{
    uint64_t ovmf_val = 0;
    if (ovmf_board_io_read(&g_ovmf_board, port, size, &ovmf_val)) {
        return ovmf_val;
    }
    if (port == LOONGEMU_QEMU_DEBUGCON_PORT) {
        return 0xe9u;
    }
    if (loongemu_pci_io_read(&g_pci_host, port, size, &ovmf_val)) {
        return ovmf_val;
    }
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
        return loongemu_vclock_read_acpi_pm24(&g_vclock);
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
    return 0xffffffffull;
}

void latx_platform_io_write(uint32_t port, int size, uint64_t value)
{
    if (ovmf_board_io_write(&g_ovmf_board, port, size, value)) {
        return;
    }
    if (port == LOONGEMU_QEMU_DEBUGCON_PORT) {
        latx_bare_uart_putc((int)(value & 0xffu));
        return;
    }
    if (port >= LOONGEMU_PCI_CFG_ADDR_PORT && port <= LOONGEMU_PCI_CFG_DATA_PORT + 3u) {
        g_pci_config_writes++;
    }
    if (loongemu_pci_io_write(&g_pci_host, port, size, value)) {
        return;
    }
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

/* Guest-visible RAM matches the external UEFI allocation and Linux boot map. */

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

/* Dynamic real-hardware parameters (default values match QEMU ELF boot). */
static uint64_t g_guest_ram_hpa = 0ULL;
static uint64_t g_vmm_image_base = VMM_IMAGE_BASE;
static uint64_t g_vmm_image_end = 0ULL;
static uint64_t g_host_timer_hz = UINT64_C(100000000);

static inline void *gpa_to_host_ptr(uint64_t gpa)
{
    return (void *)(uintptr_t)(g_guest_ram_hpa + gpa);
}

static int vmm_handoff_range_valid(uint64_t base, uint64_t size)
{
    return base != 0 && size != 0 && base <= UINT64_MAX - size &&
           (base & (LAPG_PAGE_SIZE - 1u)) == 0 &&
           (size & (LAPG_PAGE_SIZE - 1u)) == 0;
}

static int vmm_handoff_ranges_overlap(uint64_t a, uint64_t as,
                                      uint64_t b, uint64_t bs)
{
    return a < b + bs && b < a + as;
}

static int vmm_handoff_valid(const LoongemuVmmHandoff *handoff)
{
    uint64_t gop_span = 0;

    if (handoff == 0 || handoff->magic != LOONGEMU_VMM_HANDOFF_MAGIC ||
        handoff->version != LOONGEMU_VMM_HANDOFF_VERSION ||
        handoff->guest_ram_size < GUEST_RAM_SIZE ||
        handoff->host_timer_hz == 0 || handoff->host_uart_phys == 0 ||
        handoff->host_uart_reg_shift > 2u ||
        !vmm_handoff_range_valid(handoff->vmm_image_base,
                                 handoff->vmm_image_size) ||
        !vmm_handoff_range_valid(handoff->guest_ram_hpa,
                                 handoff->guest_ram_size) ||
        !vmm_handoff_range_valid(handoff->fb_fallback_hpa,
                                 handoff->fb_fallback_size) ||
        handoff->fb_fallback_size < BOCHS_FB_FALLBACK_SIZE ||
        !vmm_handoff_range_valid(handoff->ovmf_code_hpa,
                                 handoff->ovmf_code_size) ||
        !vmm_handoff_range_valid(handoff->ovmf_vars_hpa,
                                 handoff->ovmf_vars_size)) {
        return 0;
    }

    if (handoff->gop.valid) {
        const LoongemuGopHandover *gop = &handoff->gop;
        uint64_t visible_size;

        if (gop->owner != (uint8_t)LOONGEMU_FB_OWNER_HOST_GOP ||
            (gop->host_mat != (uint8_t)LOONGEMU_MAT_WUC &&
             gop->host_mat != (uint8_t)LOONGEMU_MAT_SUC) ||
            gop->bpp != 32u || gop->pixel_format > 1u ||
            gop->width == 0 || gop->height == 0 ||
            gop->stride_px < gop->width ||
            (uint64_t)gop->stride_px >
                UINT64_MAX / gop->height / 4u ||
            gop->fb_hpa == 0 || gop->fb_size == 0 ||
            gop->fb_hpa > UINT64_MAX - gop->fb_size ||
            gop->fb_size > UINT64_MAX - (LAPG_PAGE_SIZE - 1u)) {
            return 0;
        }
        visible_size = (uint64_t)gop->stride_px * gop->height * 4u;
        gop_span = (gop->fb_size + LAPG_PAGE_SIZE - 1u) &
                   ~(uint64_t)(LAPG_PAGE_SIZE - 1u);
        if (visible_size > gop->fb_size ||
            !vmm_handoff_range_valid(gop->fb_hpa, gop_span)) {
            return 0;
        }
    }

    if (vmm_handoff_ranges_overlap(handoff->vmm_image_base,
                                   handoff->vmm_image_size,
                                   handoff->guest_ram_hpa,
                                   handoff->guest_ram_size) ||
        vmm_handoff_ranges_overlap(handoff->vmm_image_base,
                                   handoff->vmm_image_size,
                                   handoff->fb_fallback_hpa,
                                   handoff->fb_fallback_size) ||
        vmm_handoff_ranges_overlap(handoff->vmm_image_base,
                                   handoff->vmm_image_size,
                                   handoff->ovmf_code_hpa,
                                   handoff->ovmf_code_size) ||
        vmm_handoff_ranges_overlap(handoff->vmm_image_base,
                                   handoff->vmm_image_size,
                                   handoff->ovmf_vars_hpa,
                                   handoff->ovmf_vars_size) ||
        vmm_handoff_ranges_overlap(handoff->guest_ram_hpa,
                                   handoff->guest_ram_size,
                                   handoff->fb_fallback_hpa,
                                   handoff->fb_fallback_size) ||
        vmm_handoff_ranges_overlap(handoff->guest_ram_hpa,
                                   handoff->guest_ram_size,
                                   handoff->ovmf_code_hpa,
                                   handoff->ovmf_code_size) ||
        vmm_handoff_ranges_overlap(handoff->guest_ram_hpa,
                                   handoff->guest_ram_size,
                                   handoff->ovmf_vars_hpa,
                                   handoff->ovmf_vars_size) ||
        vmm_handoff_ranges_overlap(handoff->fb_fallback_hpa,
                                   handoff->fb_fallback_size,
                                   handoff->ovmf_code_hpa,
                                   handoff->ovmf_code_size) ||
        vmm_handoff_ranges_overlap(handoff->fb_fallback_hpa,
                                   handoff->fb_fallback_size,
                                   handoff->ovmf_vars_hpa,
                                   handoff->ovmf_vars_size) ||
        vmm_handoff_ranges_overlap(handoff->ovmf_code_hpa,
                                   handoff->ovmf_code_size,
                                   handoff->ovmf_vars_hpa,
                                   handoff->ovmf_vars_size)) {
        return 0;
    }
    if (gop_span != 0 &&
        (vmm_handoff_ranges_overlap(handoff->gop.fb_hpa, gop_span,
                                    handoff->vmm_image_base,
                                    handoff->vmm_image_size) ||
         vmm_handoff_ranges_overlap(handoff->gop.fb_hpa, gop_span,
                                    handoff->guest_ram_hpa,
                                    handoff->guest_ram_size) ||
         vmm_handoff_ranges_overlap(handoff->gop.fb_hpa, gop_span,
                                    handoff->fb_fallback_hpa,
                                    handoff->fb_fallback_size) ||
         vmm_handoff_ranges_overlap(handoff->gop.fb_hpa, gop_span,
                                    handoff->ovmf_code_hpa,
                                    handoff->ovmf_code_size) ||
         vmm_handoff_ranges_overlap(handoff->gop.fb_hpa, gop_span,
                                    handoff->ovmf_vars_hpa,
                                    handoff->ovmf_vars_size))) {
        return 0;
    }
    return 1;
}

void kmain(void);
extern unsigned char kstack[][65536];

static void __attribute__((noreturn)) vmm_switch_stack_and_run(void)
{
    uint64_t sp = (uint64_t)(uintptr_t)&kstack[0][sizeof(kstack[0])];

    sp &= ~UINT64_C(15);
    /* kmain never returns; abandon the EFI stack before any PLV3 entry. */
    __asm__ volatile("move $sp, %0\n bl kmain\n1: b 1b"
                     :: "r"(sp) : "memory");
    __builtin_unreachable();
}

void tb_sys_vmm_main(const LoongemuVmmHandoff *handoff)
{
    if (!vmm_handoff_valid(handoff)) {
        latx_platform_refuse("invalid UEFI VMM handoff geometry");
    }

    g_vmm_handoff = *handoff;
    g_vmm_image_base = handoff->vmm_image_base;
    g_vmm_image_end = handoff->vmm_image_base + handoff->vmm_image_size;
    g_guest_ram_hpa = handoff->guest_ram_hpa;
    g_guest_ram_size = handoff->guest_ram_size;
    g_guest_ram_external = 1;
    g_bochs_fb_fallback_hpa = handoff->fb_fallback_hpa;
    g_ovmf_code_rom = (uint8_t *)(uintptr_t)handoff->ovmf_code_hpa;
    g_ovmf_vars_flash = (uint8_t *)(uintptr_t)handoff->ovmf_vars_hpa;
    g_ovmf_storage_external = 1;
    g_host_timer_hz = handoff->host_timer_hz;
    g_uart_base = handoff->host_uart_phys;
    g_uart_reg_shift = handoff->host_uart_reg_shift;
    latx_host_timer_hz = g_host_timer_hz;
    tb_sys_set_vmm_handoff(handoff);
    vmm_switch_stack_and_run();
}

static inline uint64_t timer_ticks_1ms(void)
{
    uint64_t t = g_host_timer_hz / 1000ULL;
    return t ? t : 100000ULL;
}

static inline void loongemu_sync_icache_range(uint64_t addr, uint64_t len)
{
    (void)addr;
    (void)len;
    __asm__ volatile("dbar 0\n ibar 0" ::: "memory");
}

/* ---------------- Generic MMIO Registry ---------------- */
typedef uint64_t (*MmioReadCb)(void *opaque, uint64_t offset, unsigned size);
typedef void (*MmioWriteCb)(void *opaque, uint64_t offset, uint64_t val, unsigned size);

typedef struct MmioRegion {
    uint64_t gpa_base;
    uint64_t size;
    MmioReadCb read_cb;
    MmioWriteCb write_cb;
    void *opaque;
} MmioRegion;

#define MAX_MMIO_REGIONS 32u
static MmioRegion g_mmio_regions[MAX_MMIO_REGIONS];
static uint32_t g_mmio_region_count;

static int mmio_register(uint64_t gpa_base, uint64_t size,
                         MmioReadCb read_cb, MmioWriteCb write_cb, void *opaque)
{
    for (uint32_t i = 0; i < g_mmio_region_count; i++) {
        if (g_mmio_regions[i].gpa_base == gpa_base &&
            g_mmio_regions[i].size == size) {
            return g_mmio_regions[i].read_cb == read_cb &&
                   g_mmio_regions[i].write_cb == write_cb &&
                   g_mmio_regions[i].opaque == opaque;
        }
    }
    if (g_mmio_region_count >= MAX_MMIO_REGIONS) {
        return 0;
    }
    g_mmio_regions[g_mmio_region_count].gpa_base = gpa_base;
    g_mmio_regions[g_mmio_region_count].size = size;
    g_mmio_regions[g_mmio_region_count].read_cb = read_cb;
    g_mmio_regions[g_mmio_region_count].write_cb = write_cb;
    g_mmio_regions[g_mmio_region_count].opaque = opaque;
    g_mmio_region_count++;
    return 1;
}

static const MmioRegion *mmio_find_region(uint64_t gpa)
{
    for (uint32_t i = 0; i < g_mmio_region_count; i++) {
        uint64_t base = g_mmio_regions[i].gpa_base;
        if (gpa >= base && (gpa - base) < g_mmio_regions[i].size) {
            return &g_mmio_regions[i];
        }
    }
    return 0;
}

static int mem_map_range_available_except(const LoongemuMemMap *map,
                                          uint64_t base, uint64_t size,
                                          const char *owner_name,
                                          uint64_t owner_base)
{
    uint64_t end;

    if (size == 0 || base > UINT64_MAX - size) {
        return 0;
    }
    end = base + size;
    for (uint32_t i = 0; i < map->count; i++) {
        const LoongemuMemRegion *region = &map->regions[i];
        uint64_t other_end;

        if (owner_name != 0 && region->gpa_base == owner_base &&
            region->name != 0 && strcmp(region->name, owner_name) == 0) {
            continue;
        }
        if (region->size == 0 || region->gpa_base > UINT64_MAX - region->size) {
            return 0;
        }
        other_end = region->gpa_base + region->size;
        if (base < other_end && region->gpa_base < end) {
            return 0;
        }
    }
    return 1;
}

static int mem_map_remove_owned(LoongemuMemMap *map, const char *name,
                                uint64_t base)
{
    if (!map || !name) {
        return 0;
    }
    for (uint32_t i = 0; i < map->count; i++) {
        if (map->regions[i].gpa_base != base ||
            map->regions[i].name == 0 ||
            strcmp(map->regions[i].name, name) != 0) {
            continue;
        }
        for (uint32_t j = i + 1u; j < map->count; j++) {
            map->regions[j - 1u] = map->regions[j];
        }
        map->count--;
        return 1;
    }
    return 0;
}

static int mmio_range_available_except(uint64_t base, uint64_t size,
                                       MmioReadCb owner_read,
                                       void *owner_opaque)
{
    uint64_t end;

    if (size == 0 || base > UINT64_MAX - size) {
        return 0;
    }
    end = base + size;
    for (uint32_t i = 0; i < g_mmio_region_count; i++) {
        const MmioRegion *region = &g_mmio_regions[i];
        if (region->read_cb == owner_read &&
            region->opaque == owner_opaque) {
            continue;
        }
        if (region->size == 0 || region->gpa_base > UINT64_MAX - region->size ||
            (base < region->gpa_base + region->size && region->gpa_base < end)) {
            return 0;
        }
    }
    return 1;
}


static int mem_map_find_owned(const LoongemuMemMap *map, const char *name,
                              uint64_t base, LoongemuMemRegion *out)
{
    if (!map || !name) {
        return 0;
    }
    for (uint32_t i = 0; i < map->count; i++) {
        if (map->regions[i].gpa_base == base &&
            map->regions[i].name != 0 &&
            strcmp(map->regions[i].name, name) == 0) {
            if (out) {
                *out = map->regions[i];
            }
            return 1;
        }
    }
    return 0;
}


static int mmio_unregister_by_cb(MmioReadCb read_cb, void *opaque)
{
    int removed = 0;
    for (uint32_t i = 0; i < g_mmio_region_count;) {
        if (g_mmio_regions[i].read_cb == read_cb && g_mmio_regions[i].opaque == opaque) {
            for (uint32_t j = i + 1u; j < g_mmio_region_count; j++) {
                g_mmio_regions[j - 1u] = g_mmio_regions[j];
            }
            g_mmio_region_count--;
            removed = 1;
        } else {
            i++;
        }
    }
    return removed;
}
static int mmio_unregister_exact(MmioReadCb read_cb, void *opaque,
                                 uint64_t gpa_base, uint64_t size)
{
    for (uint32_t i = 0; i < g_mmio_region_count; i++) {
        if (g_mmio_regions[i].read_cb == read_cb &&
            g_mmio_regions[i].opaque == opaque &&
            g_mmio_regions[i].gpa_base == gpa_base &&
            g_mmio_regions[i].size == size) {
            for (uint32_t j = i + 1u; j < g_mmio_region_count; j++) {
                g_mmio_regions[j - 1u] = g_mmio_regions[j];
            }
            g_mmio_region_count--;
            return 1;
        }
    }
    return 0;
}

static uint64_t bochs_oob_sink_read(void *opaque, uint64_t off, unsigned size)
{
    (void)opaque;
    (void)off;
    (void)size;
    return 0;
}

static void bochs_oob_sink_write(void *opaque, uint64_t off,
                                 uint64_t val, unsigned size)
{
    (void)opaque;
    (void)off;
    (void)val;
    (void)size;
}

static int bochs_fb_hpa_allowed(uint64_t hpa, uint64_t size, uint8_t owner)
{
    uint64_t end;

    if (hpa == 0 || size == 0 || hpa > UINT64_MAX - size ||
        (hpa & (LAPG_PAGE_SIZE - 1u)) != 0) {
        return 0;
    }
    end = hpa + size;
    if (g_guest_ram_external) {
        const LoongemuVmmHandoff *h = &g_vmm_handoff;
        uint64_t owner_base;
        uint64_t owner_size;

        if (!vmm_handoff_valid(h)) {
            return 0;
        }
        if (owner == (uint8_t)LOONGEMU_FB_OWNER_HOST_GOP) {
            owner_base = h->gop.fb_hpa;
            owner_size = h->gop.fb_size;
        } else if (owner == (uint8_t)LOONGEMU_FB_OWNER_VMM_ALLOC) {
            owner_base = h->fb_fallback_hpa;
            owner_size = h->fb_fallback_size;
        } else {
            return 0;
        }
        if (owner_base == 0 || owner_size == 0 ||
            owner_base > UINT64_MAX - owner_size ||
            hpa < owner_base || end > owner_base + owner_size) {
            return 0;
        }
        if (vmm_handoff_ranges_overlap(hpa, size, h->vmm_image_base,
                                       h->vmm_image_size) ||
            vmm_handoff_ranges_overlap(hpa, size, h->guest_ram_hpa,
                                       h->guest_ram_size) ||
            vmm_handoff_ranges_overlap(hpa, size, h->ovmf_code_hpa,
                                       h->ovmf_code_size) ||
            vmm_handoff_ranges_overlap(hpa, size, h->ovmf_vars_hpa,
                                       h->ovmf_vars_size) ||
            (owner != (uint8_t)LOONGEMU_FB_OWNER_VMM_ALLOC &&
             vmm_handoff_ranges_overlap(hpa, size, h->fb_fallback_hpa,
                                        h->fb_fallback_size))) {
            return 0;
        }
        return 1;
    }
    if (owner == (uint8_t)LOONGEMU_FB_OWNER_HOST_GOP) {
        const LoongemuGopHandover *gop = &g_pci_host.display.gop;
        return gop->valid && gop->fb_hpa != 0 && gop->fb_size != 0 &&
               gop->fb_hpa <= UINT64_MAX - gop->fb_size &&
               hpa >= gop->fb_hpa &&
               end <= gop->fb_hpa + gop->fb_size;
    }
#ifndef LOONGEMU_BOOT_OVMF
    return owner == (uint8_t)LOONGEMU_FB_OWNER_VMM_ALLOC &&
           hpa == (uint64_t)(uintptr_t)g_fb_fallback_pool &&
           size <= sizeof(g_fb_fallback_pool);
#else
    return 0;
#endif
}

static uint64_t bochs_fb_get_backing(uint64_t bar_size, uint64_t *hpa_out,
                                     uint8_t *mat_out)
{
    const LoongemuGopHandover *gop = &g_pci_host.display.gop;
    uint64_t hpa = 0;
    uint64_t available = 0;
    uint8_t owner = (uint8_t)LOONGEMU_FB_OWNER_NONE;
    uint8_t mat = (uint8_t)LOONGEMU_MAT_CC;

    if (gop->valid && gop->fb_hpa != 0 &&
        (gop->owner == (uint8_t)LOONGEMU_FB_OWNER_HOST_GOP ||
         gop->owner == (uint8_t)LOONGEMU_FB_OWNER_VMM_ALLOC)) {
        hpa = gop->fb_hpa;
        available = gop->fb_size & ~UINT64_C(0xfff);
        owner = gop->owner;
        mat = gop->host_mat;
        if (owner == (uint8_t)LOONGEMU_FB_OWNER_HOST_GOP &&
            mat != (uint8_t)LOONGEMU_MAT_WUC &&
            mat != (uint8_t)LOONGEMU_MAT_SUC) {
            mat = (uint8_t)LOONGEMU_MAT_WUC;
        } else if (owner == (uint8_t)LOONGEMU_FB_OWNER_VMM_ALLOC) {
            mat = (uint8_t)LOONGEMU_MAT_CC;
        }
    }
    if (available == 0 || !bochs_fb_hpa_allowed(hpa, available, owner)) {
        hpa = g_bochs_fb_fallback_hpa;
        available = (hpa != 0) ? BOCHS_FB_FALLBACK_SIZE : 0;
        owner = (uint8_t)LOONGEMU_FB_OWNER_VMM_ALLOC;
        mat = (uint8_t)LOONGEMU_MAT_CC;
    }
    if (available > bar_size) {
        available = bar_size;
    }
    available &= ~UINT64_C(0xfff);
    if (available == 0 || !bochs_fb_hpa_allowed(hpa, available, owner)) {
        hpa = 0;
        available = 0;
    }
    *hpa_out = hpa;
    *mat_out = mat;
    return available;
}

static int bochs_bar_map_can_apply(uint32_t bar_idx, uint64_t new_gpa,
                                   uint64_t size, int enabled)
{
    const char *map_name;
    uint64_t old_base;

    if (!enabled) {
        return 1;
    }
    if (size == 0 || (size & (LAPG_PAGE_SIZE - 1u)) != 0 ||
        (new_gpa & (LAPG_PAGE_SIZE - 1u)) != 0 ||
        new_gpa > UINT64_MAX - size) {
        return 0;
    }
    if (bar_idx == 0u) {
        uint64_t fb_hpa;
        uint64_t valid_size;
        uint8_t mat;
        uint64_t sink_gpa;
        uint64_t sink_size;
        int map_exists;
        int sink_exists = 0;

        map_name = "bochs_fb";
        old_base = g_bochs_fb_bar_gpa;
        valid_size = bochs_fb_get_backing(size, &fb_hpa, &mat);
        if (valid_size == 0 ||
            !mem_map_range_available_except(&g_mem_map, new_gpa, size,
                                            map_name, old_base) ||
            !mmio_range_available_except(new_gpa, size,
                                         bochs_oob_sink_read, 0)) {
            return 0;
        }
        map_exists = mem_map_find_owned(&g_mem_map, map_name, new_gpa, 0);
        if (g_mem_map.count + (map_exists ? 0u : 1u) >
            LOONGEMU_MEM_MAX_REGIONS) {
            return 0;
        }
        sink_size = size - valid_size;
        sink_gpa = new_gpa + valid_size;
        if (sink_size != 0) {
            for (uint32_t i = 0; i < g_mmio_region_count; i++) {
                if (g_mmio_regions[i].read_cb == bochs_oob_sink_read &&
                    g_mmio_regions[i].opaque == 0 &&
                    g_mmio_regions[i].gpa_base == sink_gpa &&
                    g_mmio_regions[i].size == sink_size) {
                    sink_exists = 1;
                    break;
                }
            }
        }
        return g_mmio_region_count +
                   ((sink_size != 0 && !sink_exists) ? 1u : 0u) <=
               MAX_MMIO_REGIONS;
    }
    if (bar_idx == 2u) {
        int map_exists;
        int mmio_exists = 0;

        map_name = "bochs_mmio";
        old_base = g_bochs_mmio_bar_gpa;
        if (!mem_map_range_available_except(&g_mem_map, new_gpa, size,
                                            map_name, old_base) ||
            !mmio_range_available_except(new_gpa, size,
                                         loongemu_bochs_mmio_read,
                                         &g_pci_host.display)) {
            return 0;
        }
        map_exists = mem_map_find_owned(&g_mem_map, map_name, new_gpa, 0);
        if (g_mem_map.count + (map_exists ? 0u : 1u) >
            LOONGEMU_MEM_MAX_REGIONS) {
            return 0;
        }
        for (uint32_t i = 0; i < g_mmio_region_count; i++) {
            if (g_mmio_regions[i].read_cb == loongemu_bochs_mmio_read &&
                g_mmio_regions[i].opaque == &g_pci_host.display &&
                g_mmio_regions[i].gpa_base == new_gpa &&
                g_mmio_regions[i].size == size) {
                mmio_exists = 1;
                break;
            }
        }
        return g_mmio_region_count + (mmio_exists ? 0u : 1u) <=
               MAX_MMIO_REGIONS;
    }
    return 1;
}

static int bochs_bar_matches_live(uint32_t bar_idx, uint64_t gpa,
                                  uint64_t size, int enabled)
{
    if (bar_idx == 0u) {
        return enabled
                   ? (g_bochs_fb_bar_gpa == gpa &&
                      g_bochs_fb_bar_size == size)
                   : (g_bochs_fb_bar_gpa == 0);
    }
    if (bar_idx == 2u) {
        return enabled
                   ? (g_bochs_mmio_bar_gpa == gpa &&
                      g_bochs_mmio_bar_size == size)
                   : (g_bochs_mmio_bar_gpa == 0);
    }
    return 1;
}

static int bochs_bar_map_apply(uint32_t bar_idx, uint64_t new_gpa,
                               uint64_t size, int enabled)
{
    if (!bochs_bar_map_can_apply(bar_idx, new_gpa, size, enabled)) {
        return 0;
    }
    if (bar_idx == 0u) {
        LoongemuMemRegion old_region;
        int had_region = mem_map_find_owned(&g_mem_map, "bochs_fb",
                                            g_bochs_fb_bar_gpa, &old_region);
        uint64_t old_bar_gpa = g_bochs_fb_bar_gpa;
        uint64_t old_sink_gpa = g_bochs_fb_sink_gpa;
        uint64_t old_sink_size = g_bochs_fb_sink_size;
        uint64_t fb_hpa = 0;
        uint8_t mat = (uint8_t)LOONGEMU_MAT_CC;
        uint64_t valid_size = enabled
                                  ? bochs_fb_get_backing(size, &fb_hpa, &mat)
                                  : 0;
        uint64_t sink_gpa = enabled ? new_gpa + valid_size : 0;
        uint64_t sink_size = enabled ? size - valid_size : 0;
        int replaced_region = 0;
        int added_region = 0;
        int added_sink = 0;
        int same_sink = sink_size != 0 &&
                        sink_gpa == old_sink_gpa &&
                        sink_size == old_sink_size;

        if (enabled) {
            if (valid_size == 0) {
                return 0;
            }
            if (had_region && old_bar_gpa == new_gpa) {
                for (uint32_t i = 0; i < g_mem_map.count; i++) {
                    LoongemuMemRegion *region = &g_mem_map.regions[i];
                    if (region->gpa_base == new_gpa &&
                        region->name != 0 &&
                        strcmp(region->name, "bochs_fb") == 0) {
                        region->size = valid_size;
                        region->hpa_base = fb_hpa;
                        region->kind = LOONGEMU_MEM_RAM;
                        region->host_mat = mat;
                        replaced_region = 1;
                        break;
                    }
                }
            } else {
                if (!loongemu_mem_map_add_mat(&g_mem_map, new_gpa, valid_size,
                                              fb_hpa, LOONGEMU_MEM_RAM, mat,
                                              "bochs_fb")) {
                    return 0;
                }
                added_region = 1;
            }
            if (sink_size != 0 && !same_sink) {
                if (!mmio_register(sink_gpa, sink_size,
                                   bochs_oob_sink_read,
                                   bochs_oob_sink_write, 0)) {
                    if (added_region) {
                        mem_map_remove_owned(&g_mem_map, "bochs_fb", new_gpa);
                    } else if (replaced_region) {
                        for (uint32_t i = 0; i < g_mem_map.count; i++) {
                            if (g_mem_map.regions[i].gpa_base == old_region.gpa_base &&
                                g_mem_map.regions[i].name != 0 &&
                                strcmp(g_mem_map.regions[i].name, "bochs_fb") == 0) {
                                g_mem_map.regions[i] = old_region;
                                break;
                            }
                        }
                    }
                    return 0;
                }
                added_sink = 1;
            }
        }

        if (had_region && (!enabled || old_bar_gpa != new_gpa)) {
            mem_map_remove_owned(&g_mem_map, "bochs_fb", old_bar_gpa);
        }
        if (old_sink_size != 0 && !same_sink) {
            (void)mmio_unregister_exact(bochs_oob_sink_read, 0,
                                        old_sink_gpa, old_sink_size);
        }
        (void)added_sink;
        g_bochs_fb_bar_gpa = enabled ? new_gpa : 0;
        g_bochs_fb_bar_size = enabled ? size : 0;
        g_bochs_fb_valid_size = enabled ? valid_size : 0;
        g_bochs_fb_sink_gpa = sink_size != 0 ? sink_gpa : 0;
        g_bochs_fb_sink_size = sink_size;
        g_pci_host.display.fb_gpa = new_gpa;
        g_pci_host.display.fb_mapped = enabled && valid_size != 0;
        g_pci_host.display.fb_mapped_size = enabled ? valid_size : 0;
        {
            uint64_t mem64k = g_pci_host.display.fb_mapped_size /
                             UINT64_C(65536);
            g_pci_host.display.dispi_regs[0x0a] =
                mem64k > UINT16_MAX ? UINT16_MAX : (uint16_t)mem64k;
        }
        return 1;
    }
    if (bar_idx == 2u) {
        LoongemuMemRegion old_region;
        int had_region = mem_map_find_owned(&g_mem_map, "bochs_mmio",
                                            g_bochs_mmio_bar_gpa, &old_region);
        uint64_t old_bar_gpa = g_bochs_mmio_bar_gpa;
        uint64_t old_bar_size = g_bochs_mmio_bar_size;
        int replaced_region = 0;
        int added_region = 0;
        int mmio_exists = 0;

        if (enabled) {
            if (had_region && old_bar_gpa == new_gpa) {
                for (uint32_t i = 0; i < g_mem_map.count; i++) {
                    LoongemuMemRegion *region = &g_mem_map.regions[i];
                    if (region->gpa_base == new_gpa &&
                        region->name != 0 &&
                        strcmp(region->name, "bochs_mmio") == 0) {
                        region->size = size;
                        region->hpa_base = 0;
                        region->kind = LOONGEMU_MEM_MMIO;
                        region->host_mat = (uint8_t)LOONGEMU_MAT_SUC;
                        replaced_region = 1;
                        break;
                    }
                }
            } else {
                if (!loongemu_mem_map_add(&g_mem_map, new_gpa, size, 0,
                                          LOONGEMU_MEM_MMIO, "bochs_mmio")) {
                    return 0;
                }
                added_region = 1;
            }
            for (uint32_t i = 0; i < g_mmio_region_count; i++) {
                if (g_mmio_regions[i].read_cb == loongemu_bochs_mmio_read &&
                    g_mmio_regions[i].opaque == &g_pci_host.display &&
                    g_mmio_regions[i].gpa_base == new_gpa &&
                    g_mmio_regions[i].size == size) {
                    mmio_exists = 1;
                    break;
                }
            }
            if (!mmio_register(new_gpa, size, loongemu_bochs_mmio_read,
                               loongemu_bochs_mmio_write,
                               &g_pci_host.display)) {
                if (added_region) {
                    mem_map_remove_owned(&g_mem_map, "bochs_mmio", new_gpa);
                } else if (replaced_region) {
                    for (uint32_t i = 0; i < g_mem_map.count; i++) {
                        if (g_mem_map.regions[i].gpa_base == old_region.gpa_base &&
                            g_mem_map.regions[i].name != 0 &&
                            strcmp(g_mem_map.regions[i].name, "bochs_mmio") == 0) {
                            g_mem_map.regions[i] = old_region;
                            break;
                        }
                    }
                }
                return 0;
            }
        }
        if (had_region && (!enabled || old_bar_gpa != new_gpa)) {
            mem_map_remove_owned(&g_mem_map, "bochs_mmio", old_bar_gpa);
        }
        if (old_bar_gpa != 0 &&
            (!enabled || old_bar_gpa != new_gpa || old_bar_size != size)) {
            (void)mmio_unregister_exact(loongemu_bochs_mmio_read,
                                        &g_pci_host.display,
                                        old_bar_gpa, old_bar_size);
        }
        (void)mmio_exists;
        g_bochs_mmio_bar_gpa = enabled ? new_gpa : 0;
        g_bochs_mmio_bar_size = enabled ? size : 0;
        g_pci_host.display.mmio_gpa = new_gpa;
        g_pci_host.display.mmio_mapped = enabled != 0;
        return 1;
    }
    return 1;
}

static int bochs_display_bar_map_cb(void *opaque, uint32_t bar_idx,
                                    uint64_t new_gpa, uint64_t size, int enabled)
{
    uint32_t slot;
    (void)opaque;

    if (bar_idx == 0u) {
        slot = 0;
    } else if (bar_idx == 2u) {
        slot = 1;
    } else {
        return 1;
    }
    if (!g_phys_mem_initialized) {
        return 1;
    }
    if (bochs_helper_context() || g_bochs_mmio_bar_write != 0) {
        uint32_t other = slot ^ 1u;

        loongemu_spin_lock(&g_bochs_bar_lock);
        if (!bochs_bar_map_can_apply(bar_idx, new_gpa, size, enabled)) {
            loongemu_spin_unlock(&g_bochs_bar_lock);
            return 0;
        }
        if (g_bochs_bar_pending[slot].valid &&
            g_bochs_bar_pending[slot].owner_vcpu != g_cur_vcpu) {
            loongemu_spin_unlock(&g_bochs_bar_lock);
            return 0;
        }
        if (bochs_bar_matches_live(bar_idx, new_gpa, size, enabled)) {
            g_bochs_bar_pending[slot].valid = 0;
            if (bochs_helper_context()) {
                bochs_set_helper_bar_pending(
                    g_bochs_bar_pending[other].valid &&
                    g_bochs_bar_pending[other].owner_vcpu == g_cur_vcpu);
            }
            loongemu_spin_unlock(&g_bochs_bar_lock);
            return 1;
        }
        if (g_bochs_bar_pending[other].valid &&
            enabled && g_bochs_bar_pending[other].enabled &&
            new_gpa < g_bochs_bar_pending[other].gpa +
                          g_bochs_bar_pending[other].size &&
            g_bochs_bar_pending[other].gpa < new_gpa + size) {
            loongemu_spin_unlock(&g_bochs_bar_lock);
            return 0;
        }
        g_bochs_bar_pending[slot].gpa = new_gpa;
        g_bochs_bar_pending[slot].size = size;
        g_bochs_bar_pending[slot].enabled = enabled != 0;
        g_bochs_bar_pending[slot].owner_vcpu = g_cur_vcpu;
        g_bochs_bar_pending[slot].valid = 1;
        if (bochs_helper_context()) {
            bochs_set_helper_bar_pending(1);
        }
        loongemu_spin_unlock(&g_bochs_bar_lock);
        return 1;
    }
    if (!bochs_bar_map_can_apply(bar_idx, new_gpa, size, enabled)) {
        return 0;
    }
#ifdef LOONGEMU_BOOT_OVMF
    if (bar_idx == 0u && enabled) {
        latx_bare_uart_puts("ovmf_pci: bochs BAR");
        puthex((uint64_t)bar_idx);
        latx_bare_uart_puts(" gpa=");
        puthex(new_gpa);
        latx_bare_uart_puts(" sz=");
        puthex(size);
        latx_bare_uart_puts("\n");
    }
#endif
    return bochs_bar_map_apply(bar_idx, new_gpa, size, enabled);
}

static int bochs_process_pending_bar_maps(void)
{
    BochsBarPending ops[2];
    uint64_t ticket;
    uint32_t map_additions = 0;
    uint32_t mmio_additions = 0;
    int pending = 0;

    loongemu_spin_lock(&g_bochs_bar_lock);
    pending = g_bochs_bar_pending[0].valid ||
              g_bochs_bar_pending[1].valid;
    loongemu_spin_unlock(&g_bochs_bar_lock);
    if (!pending) {
        return 0;
    }
    if (bochs_helper_context()) {
        return -1;
    }
    ticket = hv_request_flush_all();
    if (ticket == 0) {
        return -1;
    }
    {
        uint64_t deadline = hv_barrier_now() + hv_barrier_timeout_ticks();
        while (!hv_flush_barrier_ready(ticket)) {
            if (hv_barrier_now() > deadline) {
                hv_barrier_watchdog_fail(ticket);
            }
            __asm__ volatile("dbar 0x700" ::: "memory");
        }
    }

    loongemu_spin_lock(&g_bochs_bar_lock);
    for (uint32_t i = 0; i < 2u; i++) {
        ops[i] = g_bochs_bar_pending[i];
        g_bochs_bar_pending[i].valid = 0;
    }
    bochs_set_helper_bar_pending(0);
    loongemu_spin_unlock(&g_bochs_bar_lock);

    if (ops[0].valid && ops[1].valid &&
        ops[0].enabled && ops[1].enabled &&
        ops[0].gpa < ops[1].gpa + ops[1].size &&
        ops[1].gpa < ops[0].gpa + ops[0].size) {
        latx_platform_refuse("overlapping pending Bochs BAR mappings");
    }
    for (uint32_t i = 0; i < 2u; i++) {
        if (!ops[i].valid) {
            continue;
        }
        uint32_t bar_idx = i == 0u ? 0u : 2u;
        if (!bochs_bar_map_can_apply(bar_idx, ops[i].gpa,
                                     ops[i].size, ops[i].enabled)) {
            latx_platform_refuse("Bochs BAR remap became unsafe before commit");
        }
        if (!ops[i].enabled) {
            continue;
        }
        if (!mem_map_find_owned(&g_mem_map,
                                bar_idx == 0u ? "bochs_fb" : "bochs_mmio",
                                ops[i].gpa, 0)) {
            map_additions++;
        }
        if (bar_idx == 0u) {
            uint64_t fb_hpa;
            uint8_t mat;
            uint64_t backed = bochs_fb_get_backing(ops[i].size,
                                                   &fb_hpa, &mat);
            uint64_t sink_size = ops[i].size - backed;
            uint64_t sink_gpa = ops[i].gpa + backed;
            int sink_exists = 0;
            if (sink_size != 0) {
                for (uint32_t j = 0; j < g_mmio_region_count; j++) {
                    if (g_mmio_regions[j].read_cb == bochs_oob_sink_read &&
                        g_mmio_regions[j].opaque == 0 &&
                        g_mmio_regions[j].gpa_base == sink_gpa &&
                        g_mmio_regions[j].size == sink_size) {
                        sink_exists = 1;
                        break;
                    }
                }
                if (!sink_exists) {
                    mmio_additions++;
                }
            }
        } else {
            int mmio_exists = 0;
            for (uint32_t j = 0; j < g_mmio_region_count; j++) {
                if (g_mmio_regions[j].read_cb == loongemu_bochs_mmio_read &&
                    g_mmio_regions[j].opaque == &g_pci_host.display &&
                    g_mmio_regions[j].gpa_base == ops[i].gpa &&
                    g_mmio_regions[j].size == ops[i].size) {
                    mmio_exists = 1;
                    break;
                }
            }
            if (!mmio_exists) {
                mmio_additions++;
            }
        }
    }
    if (g_mem_map.count + map_additions > LOONGEMU_MEM_MAX_REGIONS ||
        g_mmio_region_count + mmio_additions > MAX_MMIO_REGIONS) {
        latx_platform_refuse("Bochs BAR remap exceeds mapping capacity");
    }
    for (uint32_t i = 0; i < 2u; i++) {
        if (ops[i].valid) {
            uint32_t bar_idx = i == 0u ? 0u : 2u;
            if (!bochs_bar_map_apply(bar_idx, ops[i].gpa,
                                     ops[i].size, ops[i].enabled)) {
                latx_platform_refuse("Bochs BAR remap commit failed");
            }
        }
    }
    if (!hv_release_flush_barrier(ticket)) {
        latx_platform_refuse("Bochs BAR remap flush release failed");
    }
    return 1;
}

void tb_sys_set_vmm_handoff(const LoongemuVmmHandoff *handoff)
{
    if (!handoff || handoff->magic != LOONGEMU_VMM_HANDOFF_MAGIC) {
        return;
    }
    g_vmm_handoff = *handoff;
    const LoongemuGopHandover *gop = g_vmm_handoff.gop.valid ? &g_vmm_handoff.gop : 0;
    loongemu_pci_host_init(&g_pci_host, gop, bochs_display_bar_map_cb, 0);
    if (g_vmm_handoff.ls7a_mcfg_valid) {
        loongemu_pci_host_attach_ls7a(&g_pci_host,
                                      g_vmm_handoff.ls7a_cfg_type0_base,
                                      g_vmm_handoff.ls7a_cfg_type1_base,
                                      (int)(g_vmm_handoff.hide_host_dc_gpu != 0u),
                                      0, 0);
    }
}

static uint64_t lapic_mmio_read_cb(void *opaque, uint64_t off, unsigned size);
static void lapic_mmio_write_cb(void *opaque, uint64_t off, uint64_t val, unsigned size);
static uint64_t ioapic_mmio_read_cb(void *opaque, uint64_t off, unsigned size);
static void ioapic_mmio_write_cb(void *opaque, uint64_t off, uint64_t val, unsigned size);
uint64_t latx_platform_phys_read(uint64_t pa, int size);
void latx_platform_phys_write(uint64_t pa, int size, uint64_t value);
static int guest_phys_read_checked(uint64_t pa, int size, uint64_t *out);
static int guest_phys_write_checked(uint64_t pa, int size, uint64_t value);
static uint64_t vclock_wall_sec_cb(void *opaque) { return loongemu_vclock_wall_sec((LoongemuVClock *)opaque); }

static uint64_t vclock_read_host_rdtime(void *opaque)
{
    uint64_t now;
    (void)opaque;
    __asm__ volatile("rdtime.d %0, $zero" : "=r"(now) :: "memory");
    return now;
}

static void hv_init_shard_locks(void);
static void init_phys_mem_and_vclock(void)
{
#ifdef LOONGEMU_BOOT_OVMF
    g_vmm_image_base = g_vmm_handoff.vmm_image_base;
    g_vmm_image_end = g_vmm_handoff.vmm_image_base +
                       g_vmm_handoff.vmm_image_size;
#else
    extern char _start[], _end[];
    uint64_t img_start = (uint64_t)(uintptr_t)_start & LAPG_PAGE_MASK;
    uint64_t vmm_end = ((uint64_t)(uintptr_t)_end + LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    g_vmm_image_base = img_start ? img_start : VMM_IMAGE_BASE;
    g_vmm_image_end = vmm_end;
#endif
    if (!g_guest_ram_external) {
        g_host_timer_hz = loongemu_host_timer_hz();
    }
    latx_host_timer_hz = g_host_timer_hz;

    loongemu_mem_map_init(&g_mem_map);
    /* Reserve host image addresses only when they do not overlap guest GPA RAM. */
    if (!g_guest_ram_external ||
        !vmm_handoff_ranges_overlap(g_vmm_image_base,
                                    g_vmm_image_end - g_vmm_image_base,
                                    GUEST_BASE, g_guest_ram_size)) {
        loongemu_mem_map_add(&g_mem_map, g_vmm_image_base,
                             g_vmm_image_end - g_vmm_image_base,
                             g_vmm_image_base, LOONGEMU_MEM_RESERVED,
                             "vmm_reserved");
    }
    /* Guest RAM GPA is backed by the dedicated UEFI allocation when handed off. */
    loongemu_mem_map_add(&g_mem_map, GUEST_BASE, g_guest_ram_size,
                         g_guest_ram_hpa, LOONGEMU_MEM_RAM, "guest_ram");
    /* MMIO windows (trapped, never mapped directly as host RAM) */
    loongemu_mem_map_add(&g_mem_map, LOONGEMU_IOAPIC_GPA, 0x1000ULL,
                         0, LOONGEMU_MEM_MMIO, "ioapic");
    loongemu_mem_map_add(&g_mem_map, APIC_MMIO_BASE, APIC_MMIO_SIZE,
                         0, LOONGEMU_MEM_MMIO, "lapic");

    g_mmio_region_count = 0;
    mmio_register(LOONGEMU_LAPIC_GPA, 0x1000ULL,
                  lapic_mmio_read_cb, lapic_mmio_write_cb, 0);
    mmio_register(LOONGEMU_IOAPIC_GPA, 0x1000ULL,
                  ioapic_mmio_read_cb, ioapic_mmio_write_cb, 0);

    /* OVMF storage is UEFI-allocated and handed over; the ELF test uses owned image buffers. */
    uint64_t rom_sz;
    uint64_t vars_sz;
#ifdef LOONGEMU_BOOT_OVMF
    if (!g_ovmf_storage_external) {
        latx_platform_refuse("missing UEFI OVMF backing buffers");
    }
    rom_sz = g_vmm_handoff.ovmf_code_size;
    vars_sz = g_vmm_handoff.ovmf_vars_size;
#else
    rom_sz = LOONGEMU_OVMF_DEFAULT_ROM_SZ;
    vars_sz = LOONGEMU_OVMF_DEFAULT_VARS_SZ;
    g_ovmf_code_rom = g_default_ovmf_code;
    g_ovmf_vars_flash = g_default_ovmf_vars;
    memset(g_ovmf_code_rom, 0x90, (size_t)rom_sz);
    memset(g_ovmf_vars_flash, 0xff, (size_t)vars_sz);
    /* Minimal reset stub is confined to the non-UEFI ELF self-test image. */
    {
        static const uint8_t rv16[] = {
            0xb8, 0x34, 0x12,
            0x8e, 0xd8,
            0xf4,
        };
        memcpy(&g_ovmf_code_rom[rom_sz - 16u], rv16, sizeof(rv16));
    }
    g_bochs_fb_fallback_hpa =
        (uint64_t)(uintptr_t)g_fb_fallback_pool;
#endif
    loongemu_vclock_init(&g_vclock, LOONGEMU_VCLOCK_HOST_MONO,
                         g_host_timer_hz, g_host_timer_hz,
                         g_host_timer_hz, UINT64_C(62500000),
                         vclock_read_host_rdtime, 0);
    ovmf_board_init(&g_ovmf_board, g_guest_ram_size, MAX_VCPUS,
                    guest_phys_read_checked, guest_phys_write_checked);
    ovmf_board_set_wall_clock(&g_ovmf_board, vclock_wall_sec_cb, &g_vclock);
    ovmf_board_map_firmware(&g_ovmf_board, &g_mem_map,
                            g_ovmf_code_rom, rom_sz,
                            g_ovmf_vars_flash, vars_sz);
    mmio_register(g_ovmf_board.vars_flash.gpa_base, g_ovmf_board.vars_flash.size,
                  ovmf_pflash_mmio_read, ovmf_pflash_mmio_write,
                  &g_ovmf_board.vars_flash);
    /* PCI Host Bridge (0xCF8/0xCFC + ECAM 0xB0000000) + Bochs Display GOP handover */
    loongemu_mem_map_add(&g_mem_map, LOONGEMU_PCI_ECAM_GPA, LOONGEMU_PCI_ECAM_SIZE,
                         0, LOONGEMU_MEM_MMIO, "pci_ecam");
    mmio_register(LOONGEMU_PCI_ECAM_GPA, LOONGEMU_PCI_ECAM_SIZE,
                  loongemu_pci_ecam_read, loongemu_pci_ecam_write, &g_pci_host);
    g_phys_mem_initialized = 1;
    if (g_vmm_handoff.magic == LOONGEMU_VMM_HANDOFF_MAGIC) {
        tb_sys_set_vmm_handoff(&g_vmm_handoff);
    } else {
        loongemu_pci_host_init(&g_pci_host, 0, bochs_display_bar_map_cb, 0);
    }
    loongemu_spin_init(&g_uart_lock);
    loongemu_spin_init(&g_lapic_lock);
    loongemu_spin_init(&g_ioapic_lock);
    loongemu_spin_init(&g_pic_lock);
    loongemu_spin_init(&g_translate_lock);
    loongemu_spin_init(&g_code_page_lock);
    loongemu_spin_init(&g_hv_barrier_lock);
    loongemu_spin_init(&g_bochs_bar_lock);
    hv_init_shard_locks();
}

uint64_t latx_platform_phys_read(uint64_t pa, int size)
{
    LoongemuMemLookup lk;
    LoongemuMemKind k;
    uint64_t value = 0;

    if (size <= 0 || size > 8) {
        return ~UINT64_C(0);
    }
    k = loongemu_mem_resolve(&g_mem_map, pa, (uint64_t)size, &lk);
    if (k == LOONGEMU_MEM_RESERVED) {
        g_isolation_violations++;
        return ~UINT64_C(0);
    }
    if (k != LOONGEMU_MEM_RAM && k != LOONGEMU_MEM_ROM) {
        return ~UINT64_C(0);
    }
    for (int i = size - 1; i >= 0; i--) {
        value <<= 8;
        value |= *(volatile uint8_t *)(uintptr_t)(lk.hpa + (uint64_t)i);
    }
    return value;
}

void latx_platform_phys_write(uint64_t pa, int size, uint64_t value)
{
    LoongemuMemLookup lk;
    LoongemuMemKind k;

    if (size <= 0 || size > 8) {
        return;
    }
    k = loongemu_mem_resolve(&g_mem_map, pa, (uint64_t)size, &lk);
    if (k == LOONGEMU_MEM_RESERVED) {
        g_isolation_violations++;
        return;
    }
    if (k != LOONGEMU_MEM_RAM) {
        return;
    }
    for (int i = 0; i < size; i++) {
        *(volatile uint8_t *)(uintptr_t)(lk.hpa + (uint64_t)i) =
            (uint8_t)(value >> (8 * i));
    }
}

int latx_platform_phys_read_u64_checked(uint64_t pa, uint64_t *value)
{
    LoongemuMemLookup lk;
    LoongemuMemKind k;

    if ((pa & 7u) != 0) {
        return -1;
    }
    k = loongemu_mem_resolve(&g_mem_map, pa, 8, &lk);
    if (k == LOONGEMU_MEM_RESERVED) {
        g_isolation_violations++;
        return -1;
    }
    if (k != LOONGEMU_MEM_RAM && k != LOONGEMU_MEM_ROM) {
        return -1;
    }
    *value = *(const volatile uint64_t *)(uintptr_t)lk.hpa;
    return 0;
}

int latx_platform_phys_atomic_or_u64(uint64_t pa, uint64_t bits,
                                     uint64_t *new_value)
{
    LoongemuMemLookup lk;
    LoongemuMemKind k;
    volatile uint64_t *ptr;
    uint64_t cur;

    if ((pa & 7u) != 0) {
        return -1;
    }
    k = loongemu_mem_resolve(&g_mem_map, pa, 8, &lk);
    if (k == LOONGEMU_MEM_RESERVED) {
        g_isolation_violations++;
        return -1;
    }
    if (k != LOONGEMU_MEM_RAM) {
        return -1;
    }
    ptr = (volatile uint64_t *)(uintptr_t)lk.hpa;
    cur = *ptr;
    if ((cur & bits) != bits) {
        cur = __atomic_fetch_or(ptr, bits, __ATOMIC_SEQ_CST) | bits;
    }
    *new_value = cur;
    return 0;
}

int latx_platform_phys_cmpxchg_u64(uint64_t pa, uint64_t *expected,
                                   uint64_t desired)
{
    LoongemuMemLookup lk;
    LoongemuMemKind k;
    volatile uint64_t *ptr;
    uint64_t exp;

    if ((pa & 7u) != 0 || expected == 0) {
        return -1;
    }
    k = loongemu_mem_resolve(&g_mem_map, pa, 8, &lk);
    if (k == LOONGEMU_MEM_RESERVED) {
        g_isolation_violations++;
        return -1;
    }
    if (k != LOONGEMU_MEM_RAM) {
        return -1;
    }
    ptr = (volatile uint64_t *)(uintptr_t)lk.hpa;
    exp = *expected;
    if (__atomic_compare_exchange_n(ptr, &exp, desired, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    *expected = exp;
    return 1;
}

static int guest_phys_read_checked(uint64_t pa, int size, uint64_t *out)
{
    LoongemuMemLookup lk;
    LoongemuMemKind k;
    uint64_t value = 0;

    if (!out || size <= 0 || size > 8 || (pa + (uint64_t)size < pa)) {
        return 0;
    }
    k = loongemu_mem_resolve(&g_mem_map, pa, (uint64_t)size, &lk);
    if (k == LOONGEMU_MEM_RESERVED) {
        g_isolation_violations++;
        return 0;
    }
    if (k != LOONGEMU_MEM_RAM && k != LOONGEMU_MEM_ROM) {
        return 0;
    }
    for (int i = size - 1; i >= 0; i--) {
        value <<= 8;
        value |= *(const volatile uint8_t *)(uintptr_t)(lk.hpa + (uint64_t)i);
    }
    *out = value;
    return 1;
}

static int guest_phys_write_checked(uint64_t pa, int size, uint64_t value)
{
    LoongemuMemLookup lk;
    LoongemuMemKind k;

    if (size <= 0 || size > 8 || (pa + (uint64_t)size < pa)) {
        return 0;
    }
    k = loongemu_mem_resolve(&g_mem_map, pa, (uint64_t)size, &lk);
    if (k == LOONGEMU_MEM_RESERVED) {
        g_isolation_violations++;
        return 0;
    }
    if (k != LOONGEMU_MEM_RAM) {
        return 0;
    }
    for (int i = 0; i < size; i++) {
        *(volatile uint8_t *)(uintptr_t)(lk.hpa + (uint64_t)i) =
            (uint8_t)((value >> (i * 8)) & 0xffu);
    }
    return 1;
}

uint8_t latx_bare_ldub_code(uint64_t vaddr)
{
    /* Route flat/unpaged fetches through g_mem_map so RAM and high-4GiB ROM work. */
    return (uint8_t)latx_platform_phys_read(vaddr, 1);
}

/* ---------------- translator state ---------------- */

static TCGContext tcg_storage;
static TRANSLATION_DATA tr;
static ENV lsenv_storage;
typedef struct FastJmpEntry {
    uint64_t pc;
    uint64_t code;
} FastJmpEntry;

#define FAST_JMP_CACHE_SIZE (1u << TB_JMP_CACHE_BITS)
static FastJmpEntry g_fast_jmp_cache[MAX_VCPUS][FAST_JMP_CACHE_SIZE]
    __attribute__((aligned(4096)));
#define fast_jmp_cache (g_fast_jmp_cache[g_cur_vcpu])

#define CODE_CACHE_PER_CORE (4 * 1024 * 1024)
#define CODE_CACHE_SIZE CODE_CACHE_PER_CORE
static uint8_t epi_buf[4096] __attribute__((aligned(4096)));
static uint8_t pro_buf[4096] __attribute__((aligned(4096)));
static uint8_t g_code_cache[MAX_VCPUS][CODE_CACHE_PER_CORE]
    __attribute__((aligned(4096)));
#define code_cache (g_code_cache[cur_pcpu()])

#define TB_TABLE_CAP 4096
#define ASID_SLOTS 32
#define CODE_PAGE_MAX_PFN (GUEST_RAM_SIZE >> 12)
#define SMC_DIRTY_PFN_CAP 64u

typedef struct TbCodeSpan {
    uint64_t code_start;
    uint64_t code_end;
    uint32_t slot_idx;
} TbCodeSpan;

typedef struct AsidSlot {
    uint64_t cr3;
    uint32_t vcpu;
    uint16_t pcid;
    uint8_t  cpl;
    uint8_t  mmu_flags;
    uint16_t asid;
    uint8_t  valid;
} AsidSlot;

typedef struct PerCoreShard {
    uint64_t code_used;
    TbCacheEntry tb_entries[TB_TABLE_CAP];
    uint32_t tb_buckets[TB_TABLE_CAP];
    TbCache tb_cache;
    TbInsnMap tb_insn_maps[TB_TABLE_CAP];
    uint64_t tb_code_hash[TB_TABLE_CAP];
    uint16_t tb_code_len[TB_TABLE_CAP];
    uint32_t tb_pfn0[TB_TABLE_CAP];
    uint32_t tb_pfn1[TB_TABLE_CAP];
    uint32_t tb_highwater;
    TbCodeSpan tb_by_code[TB_TABLE_CAP];
    uint32_t tb_by_code_count;
    volatile uint32_t smc_dirty_pfns[SMC_DIRTY_PFN_CAP];
    volatile uint32_t smc_dirty_count;
    volatile int smc_dirty_overflow;
    volatile int smc_dirty;
    volatile int tlb_shootdown_pending;
    LoongemuSpinLock hv_flush_lock;
    volatile uint64_t hv_flush_pending_vps;
    volatile uint64_t hv_flush_requested;
    volatile uint64_t hv_flush_completed;
    volatile uint64_t hv_barrier_pending;
    volatile uint64_t hv_barrier_parked;
    volatile uint32_t hv_recycle_pending;
    volatile uint64_t current_tb_code;
    volatile uint32_t current_tb_size;
    volatile uint64_t current_tb_pc;
    const TbInsnMap *current_tb_map;
    volatile uint64_t active_helper_fn;
    volatile uint64_t active_helper_a1;
    volatile uint64_t active_helper_a2;
    volatile uint64_t active_helper_ra;
    volatile uint32_t active_helper_bar_pending;
    AsidSlot asid_slots[ASID_SLOTS];
    uint16_t next_asid;
    uint16_t cur_asid;
    uint64_t cur_asid_cr3;
    uint32_t cur_asid_vcpu;
    uint16_t cur_asid_pcid;
    uint8_t  cur_asid_cpl;
    uint8_t  cur_asid_mmu_flags;
} PerCoreShard;

static PerCoreShard g_shard[MAX_VCPUS];

static void hv_init_shard_locks(void)
{
    for (uint32_t pcpu = 0; pcpu < MAX_VCPUS; pcpu++) {
        loongemu_spin_init(&g_shard[pcpu].hv_flush_lock);
    }
}
#define cur_shard (&g_shard[cur_pcpu()])
#define code_used (cur_shard->code_used)
#define tb_entries (cur_shard->tb_entries)
#define tb_buckets (cur_shard->tb_buckets)
#define tb_cache (cur_shard->tb_cache)
#define tb_insn_maps (cur_shard->tb_insn_maps)
#define tb_code_hash (cur_shard->tb_code_hash)
#define tb_code_len (cur_shard->tb_code_len)
#define tb_pfn0 (cur_shard->tb_pfn0)
#define tb_pfn1 (cur_shard->tb_pfn1)
#define g_tb_highwater (cur_shard->tb_highwater)
#define g_tb_by_code (cur_shard->tb_by_code)
#define g_tb_by_code_count (cur_shard->tb_by_code_count)
#define g_smc_dirty_pfns (cur_shard->smc_dirty_pfns)
#define g_smc_dirty_count (cur_shard->smc_dirty_count)
#define g_smc_dirty_overflow (cur_shard->smc_dirty_overflow)
#define g_smc_dirty (cur_shard->smc_dirty)
#define g_current_tb_code (cur_shard->current_tb_code)
#define g_current_tb_size (cur_shard->current_tb_size)
#define g_current_tb_pc (cur_shard->current_tb_pc)
#define g_current_tb_map (cur_shard->current_tb_map)
#define g_active_helper_fn (cur_shard->active_helper_fn)
#define g_active_helper_a1 (cur_shard->active_helper_a1)
#define g_active_helper_a2 (cur_shard->active_helper_a2)
#define g_active_helper_ra (cur_shard->active_helper_ra)
#define g_active_helper_bar_pending (cur_shard->active_helper_bar_pending)
#define g_asid_slots (cur_shard->asid_slots)
#define g_next_asid (cur_shard->next_asid)
#define g_cur_asid (cur_shard->cur_asid)
#define g_cur_asid_cr3 (cur_shard->cur_asid_cr3)
#define g_cur_asid_vcpu (cur_shard->cur_asid_vcpu)
#define g_cur_asid_pcid (cur_shard->cur_asid_pcid)
#define g_cur_asid_cpl (cur_shard->cur_asid_cpl)
#define g_cur_asid_mmu_flags (cur_shard->cur_asid_mmu_flags)
static void bochs_set_helper_bar_pending(uint32_t pending)
{
    g_active_helper_bar_pending = pending;
}
static int bochs_helper_context(void)
{
    return g_active_helper_fn != 0;
}
static int bochs_helper_is_pci_cfg_out(void)
{
    uint64_t fn = g_active_helper_fn;
    uint32_t port = (uint32_t)g_active_helper_a1;

    return (fn == (uint64_t)(uintptr_t)helper_outb ||
            fn == (uint64_t)(uintptr_t)helper_outw ||
            fn == (uint64_t)(uintptr_t)helper_outl) &&
           port >= LOONGEMU_PCI_CFG_DATA_PORT &&
           port <= LOONGEMU_PCI_CFG_DATA_PORT + 3u;
}
static int bochs_bar_pending_present(void)
{
    int pending;

    loongemu_spin_lock(&g_bochs_bar_lock);
    pending = g_bochs_bar_pending[0].valid ||
              g_bochs_bar_pending[1].valid;
    loongemu_spin_unlock(&g_bochs_bar_lock);
    return pending;
}
static int bochs_helper_requires_tb_exit(void)
{
    return g_active_helper_bar_pending &&
           bochs_helper_is_pci_cfg_out() &&
           bochs_bar_pending_present();
}

static uint64_t g_code_page_bitmap[CODE_PAGE_MAX_PFN / 64];
static volatile uint64_t g_smc_granular_evicts;

static void smc_record_dirty_pfn_for_shard(PerCoreShard *sh, uint32_t pfn)
{
    sh->smc_dirty = 1;
    if (sh->smc_dirty_overflow) {
        return;
    }
    for (uint32_t i = 0; i < sh->smc_dirty_count; i++) {
        if (sh->smc_dirty_pfns[i] == pfn) {
            return;
        }
    }
    if (sh->smc_dirty_count < SMC_DIRTY_PFN_CAP) {
        sh->smc_dirty_pfns[sh->smc_dirty_count++] = pfn;
    } else {
        sh->smc_dirty_overflow = 1;
    }
}

static void smc_record_dirty_pfn(uint32_t pfn)
{
    uint32_t self = cur_pcpu();
    loongemu_spin_lock(&g_code_page_lock);
    smc_record_dirty_pfn_for_shard(&g_shard[self], pfn);
    for (uint32_t c = 0; c < MAX_VCPUS; c++) {
        if (c != self && g_pcpu_online[c]) {
            smc_record_dirty_pfn_for_shard(&g_shard[c], pfn);
            loongemu_ipi_send(c, 2u);
        }
    }
    loongemu_spin_unlock(&g_code_page_lock);
}

#define PLV3_HOST_ALIAS_BASE UINT64_C(0x9000000000000000)
#define PLV3_HOST_ALIAS_PREFIX_MASK UINT64_C(0xffff000000000000)
#define PLV3_HOST_ALIAS_PHYS_MASK UINT64_C(0x0000ffffffffffff)

static uint64_t plv3_code_pc_to_phys(uint64_t code_pc)
{
    if ((code_pc & PLV3_HOST_ALIAS_PREFIX_MASK) ==
        PLV3_HOST_ALIAS_BASE) {
        return code_pc & PLV3_HOST_ALIAS_PHYS_MASK;
    }
    return code_pc;
}

static int find_tb_slot_by_code_pc(uint64_t fault_pc)
{
    fault_pc = plv3_code_pc_to_phys(fault_pc);
    uint32_t lo = 0, hi = g_tb_by_code_count;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (fault_pc < g_tb_by_code[mid].code_start) {
            hi = mid;
        } else if (fault_pc >= g_tb_by_code[mid].code_end) {
            lo = mid + 1;
        } else {
            uint32_t slot = g_tb_by_code[mid].slot_idx;
            if (tb_entries[slot].used &&
                tb_entries[slot].code == g_tb_by_code[mid].code_start) {
                return (int)slot;
            }
            return -1;
        }
    }
    return -1;
}

unsigned char kstack[MAX_VCPUS][65536] __attribute__((aligned(4096)));
void kmain(void);
void ap_main(uint32_t pcpu);
extern void _start_ap(void);

__asm__(".text\n.globl _start\n_start:\n  la $sp, kstack\n  lu12i.w $t0, 16\n"
        "  add.d $sp, $sp, $t0\n  bl kmain\n1:\n  b 1b\n"
        ".globl _start_ap\n_start_ap:\n"
        "  csrrd $t0, 0x20\n  andi $t0, $t0, 3\n"
        "  slli.d $t1, $t0, 16\n  la $sp, kstack\n  add.d $sp, $sp, $t1\n"
        "  lu12i.w $t2, 16\n  add.d $sp, $sp, $t2\n"
        "  move $a0, $t0\n  bl ap_main\n2:\n  b 2b\n"
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
static volatile uint64_t g_hw_guest_gp;
static uint64_t resolve_fault_x86_pc(uint64_t fault_pc)
{
    fault_pc = plv3_code_pc_to_phys(fault_pc);
    if (g_current_tb_map != 0 &&
        fault_pc >= g_current_tb_code &&
        fault_pc < g_current_tb_code + (uint64_t)g_current_tb_size) {
        return tb_harness_resolve_x86_pc(g_current_tb_map,
                                         g_current_tb_code,
                                         fault_pc,
                                         g_current_tb_pc);
    }
    int slot = find_tb_slot_by_code_pc(fault_pc);
    if (slot >= 0) {
        return tb_harness_resolve_x86_pc(&tb_insn_maps[slot],
                                         tb_entries[slot].code,
                                         fault_pc,
                                         tb_entries[slot].pc);
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
    uint64_t _pad; /* pad to 64 bytes (1 << 6) for assembly CPUID indexing */
} Plv3ExitInfo;

volatile Plv3ExitInfo g_plv3_exit_arr[MAX_VCPUS] __attribute__((used, aligned(64)));
#define g_plv3_exit (g_plv3_exit_arr[cur_pcpu()])
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
    R(0x88) R(0x89) R(0x8a) R(0x8f) R(0x180) R(0x181) R(0x182) R(0x183)
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
    W(0x88) W(0x8a) W(0x8c) W(0x8d) W(0x8e) W(0x8f) W(0x180) W(0x181) W(0x182) W(0x183)
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
extern struct lat_lock lat_lock[16];

static int is_host_vmm_va(uint64_t va)
{
    uint64_t page = va & LAPG_PAGE_MASK;
    uint64_t cc_start = (uint64_t)(uintptr_t)g_code_cache & LAPG_PAGE_MASK;
    uint64_t cc_end = ((uint64_t)(uintptr_t)g_code_cache + sizeof(g_code_cache) +
                       LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    uint64_t pro_page = (uint64_t)(uintptr_t)pro_buf & LAPG_PAGE_MASK;
    uint64_t epi_page = (uint64_t)(uintptr_t)epi_buf & LAPG_PAGE_MASK;
    uint64_t env_start = (uint64_t)(uintptr_t)g_vcpu_env & LAPG_PAGE_MASK;
    uint64_t env_end = ((uint64_t)(uintptr_t)g_vcpu_env + sizeof(g_vcpu_env) +
                        LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    uint64_t jc_start = (uint64_t)(uintptr_t)g_fast_jmp_cache & LAPG_PAGE_MASK;
    uint64_t jc_end = ((uint64_t)(uintptr_t)g_fast_jmp_cache +
                       sizeof(g_fast_jmp_cache) + LAPG_PAGE_SIZE - 1) &
                      LAPG_PAGE_MASK;
    uint64_t ks_start = (uint64_t)(uintptr_t)kstack & LAPG_PAGE_MASK;
    uint64_t ks_end = ((uint64_t)(uintptr_t)kstack + sizeof(kstack) +
                       LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    uint64_t pf_start = (uint64_t)(uintptr_t)pf_table & LAPG_PAGE_MASK;
    uint64_t pf_end = ((uint64_t)(uintptr_t)pf_table + sizeof(pf_table) +
                       LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    uint64_t ll_start = (uint64_t)(uintptr_t)lat_lock & LAPG_PAGE_MASK;
    uint64_t ll_end = ((uint64_t)(uintptr_t)lat_lock + sizeof(lat_lock) +
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
    if (page >= ll_start && page < ll_end) {
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
    /* W^X host isolation: code_cache, pro_buf, epi_buf are RX-only at PLV3;
     * PLV0 writes translated code through CSR.DMW0. */
    map_host_range((uint64_t)(uintptr_t)g_code_cache, sizeof(g_code_cache),
                   LAPG_PERM_READ | LAPG_PERM_EXEC |
                   LAPG_PERM_USER | LAPG_PERM_GLOBAL);
    map_host_range((uint64_t)(uintptr_t)pro_buf, sizeof(pro_buf),
                   LAPG_PERM_READ | LAPG_PERM_EXEC |
                   LAPG_PERM_USER | LAPG_PERM_GLOBAL);
    map_host_range((uint64_t)(uintptr_t)epi_buf, sizeof(epi_buf),
                   LAPG_PERM_READ | LAPG_PERM_EXEC |
                   LAPG_PERM_USER | LAPG_PERM_GLOBAL);
    map_host_range((uint64_t)(uintptr_t)g_vcpu_env, sizeof(g_vcpu_env),
                   LAPG_PERM_READ | LAPG_PERM_WRITE |
                   LAPG_PERM_USER | LAPG_PERM_GLOBAL);
    map_host_range((uint64_t)(uintptr_t)g_fast_jmp_cache, sizeof(g_fast_jmp_cache),
                   LAPG_PERM_READ | LAPG_PERM_WRITE |
                   LAPG_PERM_USER | LAPG_PERM_GLOBAL);
    map_host_range((uint64_t)(uintptr_t)kstack, sizeof(kstack),
                   LAPG_PERM_READ | LAPG_PERM_WRITE |
                   LAPG_PERM_USER | LAPG_PERM_GLOBAL);
    map_host_range((uint64_t)(uintptr_t)pf_table, sizeof(pf_table),
                   LAPG_PERM_READ | LAPG_PERM_USER | LAPG_PERM_GLOBAL);
    map_host_range((uint64_t)(uintptr_t)lat_lock, sizeof(lat_lock),
                   LAPG_PERM_READ | LAPG_PERM_WRITE |
                   LAPG_PERM_USER | LAPG_PERM_GLOBAL);
}

static volatile uint64_t g_asid_hits;
static volatile uint64_t g_asid_allocs;
static volatile uint64_t g_asid_rollovers;

static uint8_t current_mmu_flags(void)
{
    uint8_t f = 0;
    if (env.cr[0] & CR0_PG_MASK)  f |= (1u << 0);
    if (env.cr[0] & CR0_WP_MASK)  f |= (1u << 1);
    if (env.cr[4] & X86_CR4_SMEP) f |= (1u << 2);
    if (env.cr[4] & X86_CR4_SMAP) f |= (1u << 3);
    if (env.eflags & X86_RFLAGS_AC) f |= (1u << 4);
    if (env.cr[4] & X86_CR4_PKE)  f |= (1u << 5);
    if (env.cr[4] & X86_CR4_PGE)  f |= (1u << 6);
    return f;
}

static uint16_t asid_alloc_fresh(uint32_t vcpu, uint64_t cr3, uint16_t pcid,
                                 uint8_t cpl, uint8_t mmu_flags)
{
    if (g_next_asid >= 1024u) {
        __asm__ volatile("invtlb 0x3, $zero, $zero" ::: "memory");
        memset(g_asid_slots, 0, sizeof(g_asid_slots));
        g_next_asid = 1;
        g_asid_rollovers++;
    }
    uint16_t asid = g_next_asid++;
    g_asid_allocs++;
    int victim = -1;
    for (int i = 0; i < ASID_SLOTS; i++) {
        if (g_asid_slots[i].valid && g_asid_slots[i].vcpu == vcpu &&
            g_asid_slots[i].cr3 == cr3 && g_asid_slots[i].pcid == pcid &&
            g_asid_slots[i].cpl == cpl && g_asid_slots[i].mmu_flags == mmu_flags) {
            victim = i;
            break;
        }
        if (!g_asid_slots[i].valid && victim < 0) {
            victim = i;
        }
    }
    if (victim < 0) {
        victim = (int)((vcpu * 8u + (uint32_t)cpl * 4u + (uint32_t)(cr3 >> 12)) &
                       (ASID_SLOTS - 1));
    }
    g_asid_slots[victim].vcpu = vcpu;
    g_asid_slots[victim].cr3 = cr3;
    g_asid_slots[victim].pcid = pcid;
    g_asid_slots[victim].cpl = cpl;
    g_asid_slots[victim].mmu_flags = mmu_flags;
    g_asid_slots[victim].asid = asid;
    g_asid_slots[victim].valid = 1;
    return asid;
}

static void asid_activate(int force_flush)
{
    uint32_t vcpu = g_cur_vcpu;
    uint64_t cr3 = env.cr[3] & UINT64_C(0x000000fffffff000);
    uint16_t pcid = (env.cr[4] & X86_CR4_PCIDE) ? (uint16_t)(env.cr[3] & 0xfffu) : 0u;
    uint8_t cpl = (uint8_t)(env.hflags & HF_CPL_MASK);
    uint8_t mmu_flags = current_mmu_flags();
    uint16_t asid = 0;

    if (force_flush) {
        for (int i = 0; i < ASID_SLOTS; i++) {
            if (g_asid_slots[i].valid && g_asid_slots[i].vcpu == vcpu &&
                g_asid_slots[i].cr3 == cr3 && g_asid_slots[i].pcid == pcid) {
                g_asid_slots[i].valid = 0;
            }
        }
        asid = asid_alloc_fresh(vcpu, cr3, pcid, cpl, mmu_flags);
    } else {
        for (int i = 0; i < ASID_SLOTS; i++) {
            if (g_asid_slots[i].valid && g_asid_slots[i].vcpu == vcpu &&
                g_asid_slots[i].cr3 == cr3 && g_asid_slots[i].pcid == pcid &&
                g_asid_slots[i].cpl == cpl && g_asid_slots[i].mmu_flags == mmu_flags) {
                asid = g_asid_slots[i].asid;
                g_asid_hits++;
                break;
            }
        }
        if (asid == 0) {
            asid = asid_alloc_fresh(vcpu, cr3, pcid, cpl, mmu_flags);
        }
    }
    g_cur_asid = asid;
    g_cur_asid_vcpu = vcpu;
    g_cur_asid_cr3 = cr3;
    g_cur_asid_pcid = pcid;
    g_cur_asid_cpl = cpl;
    g_cur_asid_mmu_flags = mmu_flags;
    csrwr(0x18, (uint64_t)asid);
}

static uint32_t hv_vp_owner_pcpu(uint32_t vp)
{
    uint32_t owner;

    if (vp >= MAX_VCPUS) {
        return 0;
    }
    owner = __atomic_load_n(&g_vp_owner[vp], __ATOMIC_ACQUIRE);
    if (owner < MAX_VCPUS &&
        __atomic_load_n(&g_pcpu_online[owner], __ATOMIC_ACQUIRE)) {
        return owner;
    }
    return 0;
}

/*
 * TLB requests may invalidate this shard while a PLV3 helper continuation
 * still returns through its translated TB.  The TB index is cut over before
 * ACK; code storage is recycled only after the helper has left PLV3.
 */
static int hv_recycle_tb_shard(void)
{
    if (!__atomic_load_n(&cur_shard->hv_recycle_pending, __ATOMIC_ACQUIRE) ||
        g_active_helper_fn != 0) {
        return 0;
    }
    if (!__atomic_exchange_n(&cur_shard->hv_recycle_pending, 0,
                             __ATOMIC_ACQ_REL)) {
        return 0;
    }
    code_used = 0x2000;
    g_current_tb_code = 0;
    g_current_tb_size = 0;
    g_current_tb_pc = 0;
    g_current_tb_map = 0;
    __asm__ volatile("dbar 0x700\n ibar 0" ::: "memory");
    return 1;
}

static uint64_t hv_publish_flush_request(uint32_t pcpu, uint64_t vp_mask,
                                         uint64_t barrier_ticket)
{
    PerCoreShard *shard = &g_shard[pcpu];
    uint64_t ticket;

    /* Order guest page-table stores before publishing this mailbox request. */
    __asm__ volatile("dbar 0x700" ::: "memory");
    loongemu_spin_lock(&shard->hv_flush_lock);
    shard->hv_flush_pending_vps |= vp_mask;
    if (barrier_ticket != 0) {
        shard->hv_barrier_pending = barrier_ticket;
    }
    ticket = ++shard->hv_flush_requested;
    loongemu_spin_unlock(&shard->hv_flush_lock);
    return ticket;
}

static uint64_t hv_barrier_now(void)
{
    return vclock_read_host_rdtime(0);
}

static uint64_t hv_barrier_timeout_ticks(void)
{
    uint64_t hz = g_host_timer_hz ? g_host_timer_hz : UINT64_C(100000000);

    return hz * UINT64_C(5); /* five seconds of host time */
}

static void __attribute__((noreturn))
hv_barrier_watchdog_fail(uint64_t ticket)
{
    uint64_t targets = __atomic_load_n(&g_hv_barrier_targets, __ATOMIC_ACQUIRE);
    uint32_t coordinator =
        __atomic_load_n(&g_hv_barrier_coordinator, __ATOMIC_ACQUIRE);
    uint64_t parked = 0;
    uint32_t last = UINT32_MAX;
    uint64_t last_ticks = 0;

    g_hv_barrier_timeouts++;
    for (uint32_t pcpu = 0; pcpu < MAX_VCPUS; pcpu++) {
        if (__atomic_load_n(&g_shard[pcpu].hv_barrier_parked,
                            __ATOMIC_ACQUIRE) >= ticket) {
            parked |= UINT64_C(1) << pcpu;
        }
        if (targets & (UINT64_C(1) << pcpu)) {
            uint64_t seen =
                __atomic_load_n(&g_hv_barrier_last_seen[pcpu],
                                __ATOMIC_ACQUIRE);
            if (last == UINT32_MAX || seen < last_ticks) {
                last = pcpu;
                last_ticks = seen;
            }
        }
    }
    latx_bare_uart_puts("tb-sys: hv_barrier_timeout gen=");
    puthex(ticket);
    latx_bare_uart_puts(" coordinator=");
    puthex(coordinator);
    latx_bare_uart_puts(" targets=");
    puthex(targets);
    latx_bare_uart_puts(" parked=");
    puthex(parked);
    latx_bare_uart_puts(" last_pcpu=");
    puthex(last);
    latx_bare_uart_puts("\n");
    latx_platform_refuse("Hyper-V/VMM barrier watchdog timeout");
}

/*
 * Called only from a dispatcher-safe point, or from the terminal VMCALL /
 * VMMCALL helper.  An IPI handler only leaves the mailbox pending: an
 * arbitrary interrupted TB must finish through the PLV0 epilogue first.
 */
static int hv_service_flush_requests(void)
{
    PerCoreShard *shard = cur_shard;
    uint64_t vp_mask;
    uint64_t ticket;
    uint64_t barrier_ticket;
    uint32_t self = cur_pcpu();

    if (g_active_helper_fn != 0 &&
        g_active_helper_fn != (uint64_t)(uintptr_t)helper_hypercall) {
        return 0;
    }

    loongemu_spin_lock(&shard->hv_flush_lock);
    vp_mask = shard->hv_flush_pending_vps;
    ticket = shard->hv_flush_requested;
    barrier_ticket = shard->hv_barrier_pending;
    shard->hv_flush_pending_vps = 0;
    shard->hv_barrier_pending = 0;
    loongemu_spin_unlock(&shard->hv_flush_lock);
    if (vp_mask == 0) {
        return 0;
    }

    for (uint32_t vp = 0; vp < MAX_VCPUS; vp++) {
        if (vp_mask & (UINT64_C(1) << vp)) {
            latx_x86_tlb_flush_cpu(vp);
        }
    }
    __asm__ volatile("invtlb 0x3, $zero, $zero" ::: "memory");
    memset(g_asid_slots, 0, sizeof(g_asid_slots));
    g_next_asid = 1;
    asid_activate(1);

    /*
     * The entire per-pCPU TB index is shared by its VPs.  With fast indirect
     * jumps disabled, clear every VP cache that can name code on this shard;
     * never touch caches owned by a different active pCPU.
     */
    for (uint32_t vp = 0; vp < MAX_VCPUS; vp++) {
        if ((vp_mask & (UINT64_C(1) << vp)) ||
            hv_vp_owner_pcpu(vp) == self) {
            memset(g_fast_jmp_cache[vp], 0, sizeof(g_fast_jmp_cache[0]));
        }
    }
    tbc_flush_all(&tb_cache);
    g_tb_highwater = 0;
    g_tb_by_code_count = 0;
    __atomic_store_n(&shard->hv_recycle_pending, 1u, __ATOMIC_RELEASE);
    __asm__ volatile("dbar 0x700\n ibar 0\n dbar 0x700" ::: "memory");

    (void)hv_recycle_tb_shard();
    __atomic_store_n(&shard->hv_flush_completed, ticket, __ATOMIC_RELEASE);
    if (barrier_ticket != 0) {
        __atomic_store_n(&g_hv_barrier_last_seen[self], hv_barrier_now(),
                         __ATOMIC_RELEASE);
        __atomic_store_n(&shard->hv_barrier_parked, barrier_ticket,
                         __ATOMIC_RELEASE);
        if (self != __atomic_load_n(&g_hv_barrier_coordinator,
                                    __ATOMIC_ACQUIRE)) {
            uint64_t deadline = hv_barrier_now() + hv_barrier_timeout_ticks();
            while (__atomic_load_n(&g_hv_barrier_released,
                                   __ATOMIC_ACQUIRE) < barrier_ticket) {
                if (hv_barrier_now() > deadline) {
                    hv_barrier_watchdog_fail(barrier_ticket);
                }
                (void)hv_service_flush_requests();
                __asm__ volatile("dbar 0x700" ::: "memory");
            }
        }
    }
    return 1;
}


static void hv_flush_vp_mask(uint64_t mask)
{
    uint64_t owner_mask[MAX_VCPUS] = {0};
    uint64_t ticket[MAX_VCPUS] = {0};
    uint32_t self = cur_pcpu();

    for (uint32_t vp = 0; vp < MAX_VCPUS; vp++) {
        if ((mask & (UINT64_C(1) << vp)) != 0) {
            uint32_t owner = hv_vp_owner_pcpu(vp);
            owner_mask[owner] |= UINT64_C(1) << vp;
        }
    }

    for (uint32_t pcpu = 0; pcpu < MAX_VCPUS; pcpu++) {
        if (owner_mask[pcpu] == 0) {
            continue;
        }
        ticket[pcpu] = hv_publish_flush_request(pcpu, owner_mask[pcpu], 0);
        if (pcpu != self) {
            __asm__ volatile("dbar 0x700" ::: "memory");
            loongemu_ipi_send(pcpu, LOONGEMU_IPI_TLB_SHOOTDOWN);
        }
    }

    /*
     * Handle a local request synchronously, then service reciprocal requests
     * while waiting.  The mailbox lock is never held across a wait.
     */
    if (ticket[self] != 0) {
        (void)hv_service_flush_requests();
    }
    {
        uint64_t deadline = hv_barrier_now() + hv_barrier_timeout_ticks();
        for (uint32_t pcpu = 0; pcpu < MAX_VCPUS; pcpu++) {
            while (ticket[pcpu] != 0 &&
                   __atomic_load_n(&g_shard[pcpu].hv_flush_completed,
                                   __ATOMIC_ACQUIRE) < ticket[pcpu]) {
                if (hv_barrier_now() > deadline) {
                    hv_barrier_watchdog_fail(ticket[pcpu]);
                }
                (void)hv_service_flush_requests();
                __asm__ volatile("dbar 0x700" ::: "memory");
            }
        }
    }
}

/*
 * Hand a VP from the BSP multiplexer to its physical AP only after stopping
 * the old owner and completing a local flush.  Publishing the owner before
 * that flush lets a remote request ACK against a TLB the BSP is still using.
 * Runs only at a dispatcher-safe point.
 */
static void hv_try_transfer_ap_ownership(void)
{
    uint32_t self = cur_pcpu();

    for (uint32_t vp = 1; vp < MAX_VCPUS; vp++) {
        if (__atomic_load_n(&g_vcpu_online[vp], __ATOMIC_ACQUIRE) == 0 ||
            __atomic_load_n(&g_pcpu_online[vp], __ATOMIC_ACQUIRE) == 0 ||
            __atomic_load_n(&g_vp_owner[vp], __ATOMIC_ACQUIRE) != 0) {
            continue;
        }
        if (g_cur_vcpu == vp) {
            switch_vcpu(0);
        }
        hv_flush_vp_mask(UINT64_C(1) << vp);
        __atomic_store_n(&g_vp_owner[vp], vp, __ATOMIC_RELEASE);
        if (vp != self) {
            loongemu_ipi_send(vp, 0u);
        }
    }
}

/*
 * Start a two-phase all-owner cutover from a PLV0 safe point.  The caller
 * must not change mappings until every owner is parked and acknowledged.
 */
uint64_t hv_request_flush_all(void)
{
    uint64_t owner_mask[MAX_VCPUS] = {0};
    uint64_t targets = 0;
    uint64_t ticket;
    uint32_t self = cur_pcpu();

    if (g_active_helper_fn != 0) {
        return 0;
    }
    for (uint32_t vp = 0; vp < MAX_VCPUS; vp++) {
        owner_mask[hv_vp_owner_pcpu(vp)] |= UINT64_C(1) << vp;
    }
    for (uint32_t pcpu = 0; pcpu < MAX_VCPUS; pcpu++) {
        if (owner_mask[pcpu] != 0) {
            targets |= UINT64_C(1) << pcpu;
        }
    }

    loongemu_spin_lock(&g_hv_barrier_lock);
    if (__atomic_load_n(&g_hv_barrier_active, __ATOMIC_ACQUIRE) != 0) {
        loongemu_spin_unlock(&g_hv_barrier_lock);
        return 0;
    }
    ticket = ++g_hv_barrier_generation;
    if (ticket == 0) {
        ticket = ++g_hv_barrier_generation;
    }
    __atomic_store_n(&g_hv_barrier_coordinator, self, __ATOMIC_RELAXED);
    __atomic_store_n(&g_hv_barrier_targets, targets, __ATOMIC_RELAXED);
    __atomic_store_n(&g_hv_barrier_active, ticket, __ATOMIC_RELEASE);
    for (uint32_t pcpu = 0; pcpu < MAX_VCPUS; pcpu++) {
        if (owner_mask[pcpu] == 0) {
            continue;
        }
        (void)hv_publish_flush_request(pcpu, owner_mask[pcpu], ticket);
        if (pcpu != self) {
            __asm__ volatile("dbar 0x700" ::: "memory");
            loongemu_ipi_send(pcpu, LOONGEMU_IPI_TLB_SHOOTDOWN);
        }
    }
    loongemu_spin_unlock(&g_hv_barrier_lock);

    if (owner_mask[self] != 0) {
        (void)hv_service_flush_requests();
    }
    return ticket;
}

int hv_flush_barrier_ready(uint64_t ticket)
{
    uint64_t targets;

    if (ticket == 0 ||
        __atomic_load_n(&g_hv_barrier_active, __ATOMIC_ACQUIRE) != ticket) {
        return 0;
    }
    (void)hv_service_flush_requests();
    targets = __atomic_load_n(&g_hv_barrier_targets, __ATOMIC_ACQUIRE);
    for (uint32_t pcpu = 0; pcpu < MAX_VCPUS; pcpu++) {
        if ((targets & (UINT64_C(1) << pcpu)) != 0 &&
            __atomic_load_n(&g_shard[pcpu].hv_barrier_parked,
                            __ATOMIC_ACQUIRE) < ticket) {
            return 0;
        }
    }
    return 1;
}

int hv_release_flush_barrier(uint64_t ticket)
{
    if (ticket == 0 ||
        __atomic_load_n(&g_hv_barrier_active, __ATOMIC_ACQUIRE) != ticket ||
        __atomic_load_n(&g_hv_barrier_coordinator, __ATOMIC_ACQUIRE) !=
            cur_pcpu() ||
        !hv_flush_barrier_ready(ticket)) {
        return 0;
    }
    __atomic_store_n(&g_hv_barrier_released, ticket, __ATOMIC_RELEASE);
    __atomic_store_n(&g_hv_barrier_targets, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_hv_barrier_active, 0, __ATOMIC_RELEASE);
    return 1;
}

void hv_cancel_flush_barrier(uint64_t ticket)
{
    if (ticket == 0 ||
        __atomic_load_n(&g_hv_barrier_active, __ATOMIC_ACQUIRE) != ticket ||
        __atomic_load_n(&g_hv_barrier_coordinator, __ATOMIC_ACQUIRE) !=
            cur_pcpu()) {
        return;
    }
    /*
     * Do not release a generation while a target still has its mailbox
     * request unconsumed: the next barrier could overwrite it and the old
     * target would park forever.  Drain every target before reuse.
     */
    {
        uint64_t deadline = hv_barrier_now() + hv_barrier_timeout_ticks();
        while (!hv_flush_barrier_ready(ticket)) {
            if (hv_barrier_now() > deadline) {
                hv_barrier_watchdog_fail(ticket);
            }
            (void)hv_service_flush_requests();
            __asm__ volatile("dbar 0x700" ::: "memory");
        }
    }
    /* Safe only before the caller commits the remap. */
    __atomic_store_n(&g_hv_barrier_released, ticket, __ATOMIC_RELEASE);
    __atomic_store_n(&g_hv_barrier_targets, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_hv_barrier_active, 0, __ATOMIC_RELEASE);
}

static int hv_wait_for_barrier_release(void)
{
    uint32_t self = cur_pcpu();
    uint64_t ticket = __atomic_load_n(&g_hv_barrier_active, __ATOMIC_ACQUIRE);

    if (ticket == 0 ||
        __atomic_load_n(&g_hv_barrier_coordinator, __ATOMIC_ACQUIRE) != self) {
        return 0;
    }
    {
        uint64_t deadline = hv_barrier_now() + hv_barrier_timeout_ticks();
        while (__atomic_load_n(&g_hv_barrier_active, __ATOMIC_ACQUIRE) ==
               ticket) {
            if (hv_barrier_now() > deadline) {
                hv_barrier_watchdog_fail(ticket);
            }
            (void)hv_service_flush_requests();
            __asm__ volatile("dbar 0x700" ::: "memory");
        }
    }

    /* Discard code translated while a coordinator was waiting on the barrier. */
    for (uint32_t vp = 0; vp < MAX_VCPUS; vp++) {
        if (hv_vp_owner_pcpu(vp) == self) {
            memset(g_fast_jmp_cache[vp], 0, sizeof(g_fast_jmp_cache[0]));
        }
    }
    tbc_flush_all(&tb_cache);
    g_tb_highwater = 0;
    g_tb_by_code_count = 0;
    __atomic_store_n(&cur_shard->hv_recycle_pending, 1u, __ATOMIC_RELEASE);
    (void)hv_recycle_tb_shard();
    return 1;
}

static void latx_platform_tlb_shootdown_all(void)
{
    uint32_t self = cur_pcpu();
    __asm__ volatile("invtlb 0x3, $zero, $zero" ::: "memory");
    memset(g_asid_slots, 0, sizeof(g_asid_slots));
    g_next_asid = 1;
    __atomic_fetch_add(&g_tlb_shootdowns, 1u, __ATOMIC_RELAXED);
    asid_activate(1);
    for (uint32_t c = 0; c < MAX_VCPUS; c++) {
        if (c != self && __atomic_load_n(&g_pcpu_online[c], __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_shard[c].tlb_shootdown_pending, 1u,
                             __ATOMIC_RELEASE);
            loongemu_ipi_send(c, 1u);
        }
    }
}

/*
 * Hardware TLB flush hooks invoked by cpu_x86_update_cr0(),
 * cpu_x86_update_cr3(), cpu_x86_update_cr4(), and helper_invlpg().
 */
void latx_platform_tlb_flush_all(void)
{
    uint64_t cr3 = env.cr[3] & UINT64_C(0x000000fffffff000);
    uint16_t pcid = (env.cr[4] & X86_CR4_PCIDE) ? (uint16_t)(env.cr[3] & 0xfffu) : 0u;
    uint8_t cpl = (uint8_t)(env.hflags & HF_CPL_MASK);
    uint8_t mmu_flags = current_mmu_flags();
    if (g_cur_vcpu == g_cur_asid_vcpu && cr3 == g_cur_asid_cr3 &&
        pcid == g_cur_asid_pcid && mmu_flags == g_cur_asid_mmu_flags &&
        cpl != g_cur_asid_cpl) {
        /* Pure CPL switch (syscall/sysret/iret/exception): reuse ASID without invtlb. */
        asid_activate(0);
        return;
    }
    memset(fast_jmp_cache, 0, sizeof(g_fast_jmp_cache[0]));
    asid_activate(1);
}

void latx_platform_tlb_flush_page(uint64_t addr)
{
    uint64_t vppn = addr & UINT64_C(0x0000ffffffffe000);
    uint64_t asid = (uint64_t)g_cur_asid;

    __asm__ volatile("invtlb 0x5, %0, %1" :: "r"(asid), "r"(vppn) : "memory");
    for (int i = 0; i < ASID_SLOTS; i++) {
        if (g_asid_slots[i].valid && g_asid_slots[i].vcpu == g_cur_vcpu &&
            g_asid_slots[i].cr3 == env.cr[3] && g_asid_slots[i].asid != g_cur_asid) {
            uint64_t other = (uint64_t)g_asid_slots[i].asid;
            __asm__ volatile("invtlb 0x5, %0, %1" :: "r"(other), "r"(vppn) : "memory");
        }
    }
}

void latx_platform_tlb_switch_cr3(int noflush)
{
    if (!noflush) {
        latx_platform_tlb_flush_all();
        return;
    }
    /*
     * PCID-qualified MOV CR3 does not flush other address spaces.  Reuse the
     * software ASID bank; keep the software TLB keyed by (CR3, PCID), but
     * discard the single fast-jump root because it is not yet banked.
     */
    memset(fast_jmp_cache, 0, sizeof(fast_jmp_cache));
    asid_activate(0);
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
            uint32_t op22 = (insn >> 15) & 0x7ffu;
            /* stx.{b,h,w,d} (0x020..0x038) and atomic am* (0x0c0..0x0ff) */
            if (op22 == 0x020 || op22 == 0x028 || op22 == 0x030 ||
                op22 == 0x038 || op22 == 0x080 || op22 == 0x088 ||
                op22 == 0x090 || op22 == 0x098 ||
                (op22 >= 0x0c0 && op22 <= 0x0ff)) {
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
static volatile uint64_t g_sw_lbt_traps;

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

        if (option_sw_lbt) {
            cc = (uint32_t)regs[la_tp];
        } else if (g_use_sw_lbt) {
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
        uint64_t img_start = g_vmm_image_base;
        uint64_t img_end = g_vmm_image_end;

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
    /* Host VMM pages are all >= VMM_IMAGE_BASE; set G on both halves so
     * hardware TLB sets G = TLBELO0.G & TLBELO1.G = 1 across all ASIDs. */
    pte0 |= LAPG_PTE_G;
    pte1 |= LAPG_PTE_G;

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
    if (cpu < MAX_VCPUS && vec >= 0x10u && vec < 256u) {
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

int cpu_get_apic_tpr(CPUX86State *e)
{
    (void)e;
    return (int)((g_lapic[g_cur_vcpu].tpr >> 4) & 0xfu);
}

void cpu_set_apic_tpr(CPUX86State *e, uint8_t val)
{
    (void)e;
    g_lapic[g_cur_vcpu].tpr = ((uint32_t)(val & 0xfu)) << 4;
}

static uint32_t lapic_highest_isr_prio(const LapicState *l)
{
    for (int i = 7; i >= 0; i--) {
        uint32_t bits = l->isr[i];
        if (bits != 0) {
            for (int b = 31; b >= 0; b--) {
                if (bits & (1u << b)) {
                    uint32_t vec = (uint32_t)(i * 32 + b);
                    return (vec >> 4) & 0xfu;
                }
            }
        }
    }
    return 0;
}

static void pv_eoi_sync_locked(uint32_t cpu);
static void pv_eoi_arm_locked(uint32_t cpu);
static void pv_eoi_clear_locked(uint32_t cpu);

static uint32_t lapic_pop_irr(uint32_t cpu)
{
    LapicState *l = &g_lapic[cpu];
    pv_eoi_sync_locked(cpu);
    uint32_t tpr_prio = (l->tpr >> 4) & 0xfu;
    uint32_t isr_prio = lapic_highest_isr_prio(l);
    uint32_t min_prio = (tpr_prio > isr_prio) ? tpr_prio : isr_prio;
    for (int i = 7; i >= 0; i--) {
        uint32_t bits = l->irr[i];
        if (bits != 0) {
            for (int b = 31; b >= 0; b--) {
                if (bits & (1u << b)) {
                    uint32_t vec = (uint32_t)(i * 32 + b);
                    if (vec < 0x10u || ((vec >> 4) & 0xfu) <= min_prio) {
                        return 0;
                    }
                    if (isr_prio != 0u) {
                        /* A nested interrupt requires a conventional EOI. */
                        pv_eoi_clear_locked(cpu);
                    }
                    l->irr[i] &= ~(1u << b);
                    l->isr[i] |= (1u << b);
                    if (isr_prio == 0u && !lapic_has_irr(cpu)) {
                        pv_eoi_arm_locked(cpu);
                    }
                    return vec;
                }
            }
        }
    }
    return 0;
}

/* Generic IOAPIC GSI interrupt assertion (shared by COM1 and all devices). */
static int ioapic_assert_irq(uint32_t gsi)
{
    uint64_t red;
    uint32_t vec, dest, dest_mode;
    uint32_t self_pcpu = cur_pcpu();
    int injected = 0;

    if (gsi >= 24u) {
        return 0;
    }
    loongemu_spin_lock(&g_ioapic_lock);
    red = g_ioapic_redtbl[gsi];
    loongemu_spin_unlock(&g_ioapic_lock);

    if ((red & (1ULL << 16)) != 0 || (red & 0xffu) < 0x20u) {
        if (gsi < 8u && (g_pic_imr[0] & (1u << gsi)) == 0) {
            loongemu_spin_lock(&g_lapic_lock);
            lapic_set_irr(0, g_pic_irq0_vec + gsi);
            g_vcpu_halted[0] = 0;
            loongemu_spin_unlock(&g_lapic_lock);
            return 1;
        }
        return 0;
    }

    vec = (uint32_t)(red & 0xffu);
    dest = (uint32_t)((red >> 56) & 0xffu);
    dest_mode = (uint32_t)((red >> 11) & 1u);

    loongemu_spin_lock(&g_lapic_lock);
    for (uint32_t c = 0; c < MAX_VCPUS; c++) {
        int match = 0;
        if (dest_mode == 0u) {
            match = (dest == 0xffu) ? (c == 0) : (c == dest);
        } else {
            uint32_t ldr_id = (g_lapic[c].ldr >> 24) & 0xffu;
            if (ldr_id == 0u) {
                ldr_id = (1u << c);
            }
            match = ((ldr_id & dest) != 0u);
        }
        if (match) {
            lapic_set_irr(c, vec);
            g_vcpu_halted[c] = 0;
            injected = 1;
            if (c != self_pcpu && __atomic_load_n(&g_pcpu_online[c], __ATOMIC_ACQUIRE)) {
                loongemu_ipi_send(c, 0u);
            }
        }
    }
    loongemu_spin_unlock(&g_lapic_lock);
    return injected;
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
        uint32_t div = 1u;
        uint32_t dv = ((l->tdcr >> 1) & 4u) | (l->tdcr & 3u);
        if (l->tmict == 0) {
            return 0;
        }
        if (dv == 0) div = 2; else if (dv == 1) div = 4; else if (dv == 2) div = 8;
        else if (dv == 3) div = 16; else if (dv == 4) div = 32; else if (dv == 5) div = 64;
        else if (dv == 6) div = 128; else div = 1;
        return loongemu_vclock_lapic_current(&g_vclock, l->tm_start_ts,
                                             l->tmict, div,
                                             (l->lvtt & (1u << 17)) != 0, 0);
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
    uint32_t self_pcpu = cur_pcpu();

    loongemu_spin_lock(&g_lapic_lock);
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
            if (t != self_pcpu && __atomic_load_n(&g_pcpu_online[t], __ATOMIC_ACQUIRE)) {
                loongemu_ipi_send(t, 0u);
            }
        } else if (deliv == 0u || deliv == 1u) { /* Fixed / Lowest Priority */
            lapic_set_irr(t, vec);
            g_vcpu_halted[t] = 0;
            if (t != self_pcpu && __atomic_load_n(&g_pcpu_online[t], __ATOMIC_ACQUIRE)) {
                loongemu_ipi_send(t, 0u);
            }
        }
    }
    loongemu_spin_unlock(&g_lapic_lock);
}

static void lapic_mmio_write(uint32_t off, uint32_t val)
{
    LapicState *l = &g_lapic[g_cur_vcpu];
    switch (off & 0xff0u) {
    case 0x080u: l->tpr = val & 0xffu; break;
    case 0x0b0u:
        loongemu_spin_lock(&g_lapic_lock);
        pv_eoi_clear_locked(g_cur_vcpu);
        lapic_eoi(g_cur_vcpu);
        loongemu_spin_unlock(&g_lapic_lock);
        break;
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
        l->tm_start_ts = loongemu_vclock_now_ns(&g_vclock);
        break;
    case 0x3e0u: l->tdcr = val; break;
    default: break;
    }
}

static uint32_t ioapic_mmio_read(uint32_t off)
{
    uint32_t res = 0;
    loongemu_spin_lock(&g_ioapic_lock);
    if ((off & 0x1fu) == 0x00u) {
        res = g_ioapic_regsel;
    } else if ((off & 0x1fu) == 0x10u) {
        uint32_t sel = g_ioapic_regsel & 0xffu;
        if (sel == 0x00u) res = g_ioapic_id << 24;
        else if (sel == 0x01u) res = 0x00170011u;
        else if (sel == 0x02u) res = 0;
        else if (sel >= 0x10u && sel < 0x40u) {
            uint32_t idx = (sel - 0x10u) >> 1;
            res = (sel & 1u) ? (uint32_t)(g_ioapic_redtbl[idx] >> 32)
                             : (uint32_t)(g_ioapic_redtbl[idx] & 0xffffffffu);
        }
    }
    loongemu_spin_unlock(&g_ioapic_lock);
    return res;
}

static void ioapic_mmio_write(uint32_t off, uint32_t val)
{
    loongemu_spin_lock(&g_ioapic_lock);
    if ((off & 0x1fu) == 0x00u) {
        g_ioapic_regsel = val & 0xffu;
    } else if ((off & 0x1fu) == 0x10u) {
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
    loongemu_spin_unlock(&g_ioapic_lock);
}

static uint64_t lapic_mmio_read_cb(void *opaque, uint64_t off, unsigned size)
{
    (void)opaque;
    (void)size;
    return lapic_mmio_read((uint32_t)off);
}

static void lapic_mmio_write_cb(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    (void)opaque;
    (void)size;
    lapic_mmio_write((uint32_t)off, (uint32_t)val);
}

static uint64_t ioapic_mmio_read_cb(void *opaque, uint64_t off, unsigned size)
{
    (void)opaque;
    (void)size;
    return ioapic_mmio_read((uint32_t)off);
}

static void ioapic_mmio_write_cb(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    (void)opaque;
    (void)size;
    ioapic_mmio_write((uint32_t)off, (uint32_t)val);
}

/* ---------------- KVM PV MSRs, Hyper-V MSRs & Hypercalls ---------------- */
extern uint32_t latx_hv_vendor;
static uint64_t g_kvm_sys_time_msr[MAX_VCPUS];
static uint64_t g_kvm_wall_clock_msr;
static uint64_t g_kvm_pv_eoi_msr[MAX_VCPUS];
static volatile uint8_t g_kvm_pv_eoi_pending[MAX_VCPUS];
static uint32_t g_kvm_pvclock_ver[MAX_VCPUS];
static uint64_t g_hv_guest_os_id;
static uint64_t g_hv_hypercall_msr;
static uint64_t g_hv_apic_assist[MAX_VCPUS];
static volatile uint8_t g_hv_apic_assist_pending[MAX_VCPUS];
static volatile uint64_t g_hv_flush_calls;
static volatile uint32_t g_last_ipi_pcpu = UINT32_MAX;

static void pv_eoi_sync_locked(uint32_t cpu)
{
    if (cpu >= MAX_VCPUS) {
        return;
    }
    if (g_kvm_pv_eoi_pending[cpu] && (g_kvm_pv_eoi_msr[cpu] & 1u) != 0) {
        uint64_t gpa = g_kvm_pv_eoi_msr[cpu] & ~UINT64_C(3);
        uint64_t word = 1;
        if (guest_phys_read_checked(gpa, 4, &word) && (word & 1u) == 0u) {
            g_kvm_pv_eoi_pending[cpu] = 0;
            lapic_eoi(cpu);
        }
    }
    if (g_hv_apic_assist_pending[cpu] && (g_hv_apic_assist[cpu] & 1u) != 0) {
        uint64_t gpa = g_hv_apic_assist[cpu] & ~UINT64_C(0xfff);
        uint64_t word = 1;
        if (guest_phys_read_checked(gpa, 4, &word) && (word & 1u) == 0u) {
            g_hv_apic_assist_pending[cpu] = 0;
            lapic_eoi(cpu);
        }
    }
}

static void pv_eoi_arm_locked(uint32_t cpu)
{
    if (cpu >= MAX_VCPUS) {
        return;
    }
    if ((g_kvm_pv_eoi_msr[cpu] & 1u) != 0) {
        uint64_t gpa = g_kvm_pv_eoi_msr[cpu] & ~UINT64_C(3);
        if (guest_phys_write_checked(gpa, 4, 1u)) {
            g_kvm_pv_eoi_pending[cpu] = 1;
        }
    }
    if ((g_hv_apic_assist[cpu] & 1u) != 0) {
        uint64_t gpa = g_hv_apic_assist[cpu] & ~UINT64_C(0xfff);
        if (guest_phys_write_checked(gpa, 4, 1u)) {
            g_hv_apic_assist_pending[cpu] = 1;
        }
    }
}

static void pv_eoi_clear_locked(uint32_t cpu)
{
    if (cpu >= MAX_VCPUS) {
        return;
    }
    if (g_kvm_pv_eoi_pending[cpu]) {
        g_kvm_pv_eoi_pending[cpu] = 0;
        if ((g_kvm_pv_eoi_msr[cpu] & 1u) != 0) {
            (void)guest_phys_write_checked(g_kvm_pv_eoi_msr[cpu] & ~UINT64_C(3), 4, 0u);
        }
    }
    if (g_hv_apic_assist_pending[cpu]) {
        g_hv_apic_assist_pending[cpu] = 0;
        if ((g_hv_apic_assist[cpu] & 1u) != 0) {
            (void)guest_phys_write_checked(g_hv_apic_assist[cpu] & ~UINT64_C(0xfff), 4, 0u);
        }
    }
}

static void kvm_update_pvclock(uint32_t cpu, uint64_t gpa)
{
    uint64_t hz = g_host_timer_hz ? g_host_timer_hz : UINT64_C(100000000);
    uint64_t tsc = vclock_read_host_rdtime(0);
    uint64_t sys_ns = loongemu_vclock_now_ns(&g_vclock);
    int8_t shift = 0;
    __uint128_t num = (__uint128_t)1000000000ULL << 32;
    uint64_t denom = hz;
    uint32_t mul;

    while ((num / denom) > UINT64_C(0xffffffff) && shift < 31) {
        denom <<= 1;
        shift++;
    }
    while ((num / denom) < UINT64_C(0x80000000) && shift > -31 && (denom & 1u) == 0u) {
        denom >>= 1;
        shift--;
    }
    mul = (uint32_t)(num / denom);

    g_kvm_pvclock_ver[cpu] += 2u;
    uint32_t ver = g_kvm_pvclock_ver[cpu];
    latx_platform_phys_write(gpa + 0u, 4, ver - 1u);
    latx_platform_phys_write(gpa + 8u, 8, tsc);
    latx_platform_phys_write(gpa + 16u, 8, sys_ns);
    latx_platform_phys_write(gpa + 24u, 4, mul);
    latx_platform_phys_write(gpa + 28u, 1, (uint8_t)shift);
    latx_platform_phys_write(gpa + 29u, 1, 1u); /* PVCLOCK_TSC_STABLE_BIT */
    latx_platform_phys_write(gpa + 30u, 2, 0u);
    __asm__ volatile("dbar 0" ::: "memory");
    latx_platform_phys_write(gpa + 0u, 4, ver);
}

void latx_platform_kvm_wrmsr(CPUX86State *e, uint32_t msr, uint64_t val)
{
    uint32_t cpu = g_cur_vcpu;
    (void)e;
    switch (msr) {
    case 0x4b564d00u: /* MSR_KVM_WALL_CLOCK_NEW */
        g_kvm_wall_clock_msr = val;
        if (val != 0) {
            uint64_t wsec = 0;
            uint32_t wnsec = 0;
            loongemu_vclock_wall_timespec(&g_vclock, &wsec, &wnsec);
            /* pvclock_wall_clock: version, sec, nsec anchored to same vclock epoch as CMOS/RTC */
            latx_platform_phys_write(val + 0u, 4, 2u);
            latx_platform_phys_write(val + 4u, 4, (uint32_t)wsec);
            latx_platform_phys_write(val + 8u, 4, wnsec);
        }
        break;
    case 0x4b564d01u: /* MSR_KVM_SYSTEM_TIME_NEW */
        g_kvm_sys_time_msr[cpu] = val;
        if ((val & 1u) != 0) {
            kvm_update_pvclock(cpu, val & ~UINT64_C(1));
        }
        break;
    case 0x4b564d04u: /* MSR_KVM_PV_EOI_EN */
        loongemu_spin_lock(&g_lapic_lock);
        g_kvm_pv_eoi_pending[cpu] = 0;
        if ((val & 1u) != 0) {
            uint64_t gpa = val & ~UINT64_C(3);
            if (guest_phys_write_checked(gpa, 4, 0u)) {
                g_kvm_pv_eoi_msr[cpu] = val;
            } else {
                g_kvm_pv_eoi_msr[cpu] = 0;
            }
        } else {
            g_kvm_pv_eoi_msr[cpu] = 0;
        }
        loongemu_spin_unlock(&g_lapic_lock);
        break;
    case 0x40000000u: /* HV_X64_MSR_GUEST_OS_ID */
        g_hv_guest_os_id = val;
        break;
    case 0x40000001u: /* HV_X64_MSR_HYPERCALL */
        g_hv_hypercall_msr = val;
        if ((val & 1u) != 0) {
            uint64_t gpa = val & ~UINT64_C(0xfff);
            /* Install executable x86-64 hypercall stub: vmcall; ret */
            if (guest_phys_write_checked(gpa + 0u, 4, 0xc3c1010fu)) {
                uint64_t pfn = gpa >> 12;
                if (pfn < CODE_PAGE_MAX_PFN) {
                    smc_record_dirty_pfn((uint32_t)pfn);
                }
            }
        }
        break;
    case 0x40000070u: /* HV_X64_MSR_EOI */
        loongemu_spin_lock(&g_lapic_lock);
        pv_eoi_clear_locked(cpu);
        lapic_eoi(cpu);
        loongemu_spin_unlock(&g_lapic_lock);
        break;
    case 0x40000071u: /* HV_X64_MSR_ICR */
        g_lapic[cpu].icr_hi = (uint32_t)(val >> 32);
        g_lapic[cpu].icr_lo = (uint32_t)(val & 0xffffffffu);
        lapic_deliver_icr(g_lapic[cpu].icr_lo, g_lapic[cpu].icr_hi);
        break;
    case 0x40000072u: /* HV_X64_MSR_TPR */
        g_lapic[cpu].tpr = (uint32_t)(val & 0xffu);
        break;
    case 0x40000073u: /* HV_X64_MSR_APIC_ASSIST_PAGE */
        loongemu_spin_lock(&g_lapic_lock);
        pv_eoi_sync_locked(cpu);
        if ((val & UINT64_C(0xffe)) == 0u && (val & 1u) != 0) {
            uint64_t gpa = val & ~UINT64_C(0xfff);
            LoongemuMemLookup lk;
            if (loongemu_mem_resolve(&g_mem_map, gpa, 4096u, &lk) == LOONGEMU_MEM_RAM &&
                guest_phys_write_checked(gpa, 4, 0u)) {
                pv_eoi_clear_locked(cpu);
                g_hv_apic_assist[cpu] = val;
            }
        } else if ((val & 1u) == 0 && (val & UINT64_C(0xffe)) == 0u) {
            pv_eoi_clear_locked(cpu);
            g_hv_apic_assist[cpu] = 0;
        }
        loongemu_spin_unlock(&g_lapic_lock);
        break;
    default:
        break;
    }
}

int latx_platform_kvm_rdmsr(CPUX86State *e, uint32_t msr, uint64_t *val)
{
    uint32_t cpu = g_cur_vcpu;
    (void)e;
    switch (msr) {
    case 0x4b564d00u:
        *val = g_kvm_wall_clock_msr;
        return 1;
    case 0x4b564d01u:
        *val = g_kvm_sys_time_msr[cpu];
        return 1;
    case 0x4b564d04u:
        *val = g_kvm_pv_eoi_msr[cpu];
        return 1;
    case 0x40000000u:
        *val = g_hv_guest_os_id;
        return 1;
    case 0x40000001u:
        *val = g_hv_hypercall_msr;
        return 1;
    case 0x40000002u: /* HV_X64_MSR_VP_INDEX */
        *val = (uint64_t)cpu;
        return 1;
    case 0x40000020u: /* HV_X64_MSR_TIME_REF_COUNT (100ns units) */
        *val = loongemu_vclock_now_ns(&g_vclock) / 100ULL;
        return 1;
    case 0x40000071u: /* HV_X64_MSR_ICR */
        *val = ((uint64_t)g_lapic[cpu].icr_hi << 32) | (uint64_t)g_lapic[cpu].icr_lo;
        return 1;
    case 0x40000072u: /* HV_X64_MSR_TPR */
        *val = (uint64_t)g_lapic[cpu].tpr;
        return 1;
    case 0x40000073u: /* HV_X64_MSR_APIC_ASSIST_PAGE */
        *val = g_hv_apic_assist[cpu];
        return 1;
    default:
        return 0;
    }
}

static int hv_read_guest_qword(void *opaque, uint64_t gpa, uint64_t *value)
{
    LoongemuMemLookup lk;
    (void)opaque;
    return loongemu_mem_resolve(&g_mem_map, gpa, 8u, &lk) == LOONGEMU_MEM_RAM &&
           guest_phys_read_checked(gpa, 8, value);
}

static uint64_t hv_read_xmm_qword(void *opaque, unsigned index)
{
    CPUX86State *e = (CPUX86State *)opaque;
    return e->xmm_regs[index / 2u].ZMM_Q(index % 2u);
}

static void hv_flush_callback(void *opaque, uint64_t mask,
                              uint64_t address_space, uint64_t flags)
{
    (void)opaque;
    (void)address_space;
    (void)flags;
    __atomic_fetch_add(&g_hv_flush_calls, 1u, __ATOMIC_RELAXED);
    /* Full flush is a permitted superset for selected VPs and address spaces. */
    hv_flush_vp_mask(mask);
}

static void hv_send_ipi_to_vp_owner(uint32_t vp)
{
    uint32_t self = cur_pcpu();
    uint32_t pcpu = hv_vp_owner_pcpu(vp);
    uint32_t after;

    if (pcpu != self &&
        __atomic_load_n(&g_pcpu_online[pcpu], __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_last_ipi_pcpu, pcpu, __ATOMIC_RELEASE);
        loongemu_ipi_send(pcpu, 0u);
    }
    /*
     * Ownership can move between the lookup and the IPI.  The new owner must
     * also observe the already-published LAPIC IRR/kick, so recheck once and
     * send there as well; duplicate wake IPIs are harmless.
     */
    after = hv_vp_owner_pcpu(vp);
    if (after != pcpu && after != self &&
        __atomic_load_n(&g_pcpu_online[after], __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_last_ipi_pcpu, after, __ATOMIC_RELEASE);
        loongemu_ipi_send(after, 0u);
    }
}

static void hv_send_ipi_callback(void *opaque, uint64_t mask, uint8_t vector)
{
    (void)opaque;
    loongemu_spin_lock(&g_lapic_lock);
    for (uint32_t v = 0; v < MAX_VCPUS; v++) {
        if ((mask & (UINT64_C(1) << v)) == 0 ||
            !__atomic_load_n(&g_vcpu_online[v], __ATOMIC_ACQUIRE)) {
            continue;
        }
        lapic_set_irr(v, vector);
        __atomic_store_n(&g_vcpu_halted[v], 0, __ATOMIC_RELEASE);
        hv_send_ipi_to_vp_owner(v);
    }
    loongemu_spin_unlock(&g_lapic_lock);
}

void helper_hypercall(CPUX86State *e)
{
    uint32_t self_pcpu = cur_pcpu();

    if (latx_hv_vendor == 1u) {
        LoongemuHypervOps ops = {e, latx_guest_nr_cpus, hv_read_guest_qword,
                                 hv_read_xmm_qword, hv_flush_callback,
                                 hv_send_ipi_callback};
        e->regs[R_EAX] = loongemu_hyperv_call(&ops, e->regs[R_ECX],
                                              e->regs[R_EDX], e->regs[R_R8]);
        return;
    }

    uint64_t nr = e->regs[R_EAX];
    uint64_t a0 = e->regs[R_EBX];
    uint64_t a1 = e->regs[R_ECX];
    uint64_t a2 = e->regs[R_EDX];
    uint64_t a3 = e->regs[R_ESI];

    if (nr == 5u) { /* KVM_HC_KICK_CPU(flags, apic_id) */
        uint32_t target = (uint32_t)a1;
        if (target < MAX_VCPUS) {
            __atomic_store_n(&g_vcpu_kicked[target], 1, __ATOMIC_RELEASE);
            g_vcpu_halted[target] = 0;
            hv_send_ipi_to_vp_owner(target);
        }
        e->regs[R_EAX] = 0;
        return;
    }
    if (nr == 10u) { /* KVM_HC_SEND_IPI(lo_mask, hi_mask, min_apic, icr) */
        uint32_t min_apic = (uint32_t)a2;
        uint32_t icr = (uint32_t)a3;
        uint32_t vec = icr & 0xffu;
        uint32_t deliv = (icr >> 8) & 7u;
        loongemu_spin_lock(&g_lapic_lock);
        for (uint32_t b = 0; b < 64u && (min_apic + b) < MAX_VCPUS; b++) {
            if ((a0 & (UINT64_C(1) << b)) == 0) {
                continue;
            }
            uint32_t t = min_apic + b;
            if (deliv == 4u) {
                g_vcpu_nmi_pending[t] = 1;
            } else {
                lapic_set_irr(t, vec);
            }
            g_vcpu_halted[t] = 0;
            if (t != self_pcpu && __atomic_load_n(&g_pcpu_online[t], __ATOMIC_ACQUIRE)) {
                loongemu_ipi_send(t, 0u);
            }
        }
        loongemu_spin_unlock(&g_lapic_lock);
        e->regs[R_EAX] = 0;
        return;
    }
    e->regs[R_EAX] = (uint64_t)(int64_t)-1000; /* -KVM_ENOSYS */
}

static int emulate_mmio_insn(uint64_t fault_pc, uint64_t gpa,
                             uint64_t *regs, int is_refill)
{
    const MmioRegion *reg = mmio_find_region(gpa);
    uint32_t insn = *(volatile uint32_t *)(uintptr_t)fault_pc;
    uint32_t major = (insn >> 26) & 0x3fu;
    uint32_t rd = insn & 0x1fu;
    int is_store = 0;
    int is_signed = 0;
    int size = 4;

    if (reg == 0) {
        return 0;
    }

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
        uint32_t op22 = (insn >> 15) & 0x7ffu;
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

    uint64_t off = gpa - reg->gpa_base;

    if (is_store) {
        if (reg->write_cb != 0) {
            g_bochs_mmio_bar_write++;
            reg->write_cb(reg->opaque, off, regs[rd], (unsigned)size);
            g_bochs_mmio_bar_write--;
        }
    } else {
        uint64_t val = reg->read_cb ? reg->read_cb(reg->opaque, off, (unsigned)size) : 0;
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

    if (bochs_bar_pending_present()) {
        /*
         * The store completed, but the BAR move is queued for the all-owner
         * barrier.  Return to the PLV0 dispatcher now so no later instruction
         * in this TB can touch the old framebuffer mapping before cutover.
         */
        if (is_refill) {
            csrwr(0x8a,
                  ((uint64_t)(uintptr_t)tb_sys_plv3_done & ~UINT64_C(3)) | 1u);
        } else {
            csrwr(0x6, (uint64_t)(uintptr_t)tb_sys_plv3_done);
        }
    } else if (is_refill) {
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
    /* Zero-copy vCPU switch on UP (-smp 1): update per-core vCPU slot,
     * keep per-vCPU fast_jmp_cache, and switch CSR.ASID without invtlb 0x0. */
    g_pcpu_vcpu[cur_pcpu()] = next_id;
    latx_guest_apic_id = next_id;
    cc = (uint32_t)(env.eflags & 0x8d5u);
    if (option_sw_lbt) {
        /* pro_buf loads env.eflags into $tp on PLV3 entry. */
    } else if (g_use_sw_lbt) {
        sw_lbt.eflags = cc;
    } else {
        __asm__ volatile("move $t0, %0\n"
                         ".word 0x005cfc2c\n" /* x86mtflag $t0, 0x3f */
                         :: "r"(cc) : "t0");
    }
    asid_activate(0);
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
        uint32_t t_start = *(volatile uint32_t *)gpa_to_host_ptr(hdr + 0x08);
        uint32_t t_hdr = *(volatile uint32_t *)gpa_to_host_ptr(hdr + 0x0c);
        if (t_start == (uint32_t)sipi_pa &&
            t_hdr >= (uint32_t)hdr && t_hdr < (uint32_t)(hdr + 0x10000u)) {
            static const uint32_t pgd_offs[3] = { 0x18u, 0x14u, 0x10u };
            uint32_t t_pgd = 0;
            for (int k = 0; k < 3; k++) {
                uint32_t cand = *(volatile uint32_t *)gpa_to_host_ptr(hdr + pgd_offs[k]);
                if (cand != 0 && (cand & 0xfffu) == 0 && cand < g_guest_ram_size &&
                    (*(volatile uint64_t *)gpa_to_host_ptr(cand + 511u * 8u) & 1u) != 0) {
                    t_pgd = cand;
                    break;
                }
            }
            if (t_pgd != 0) {
                uint64_t s = *(volatile uint64_t *)gpa_to_host_ptr(t_hdr + 0x00);
                uint64_t ef = *(volatile uint64_t *)gpa_to_host_ptr(t_hdr + 0x08);
                uint32_t c4 = *(volatile uint32_t *)gpa_to_host_ptr(t_hdr + 0x10);
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

    CPUX86State *ap = &g_vcpu_env[cpu_id];
    memset(ap, 0, sizeof(*ap));
    env_cpu(ap)->cpu_index = (int)cpu_id;
    latx_x86_env_init_features(ap);
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
    memset(g_fast_jmp_cache[cpu_id], 0, sizeof(g_fast_jmp_cache[0]));
    ap->tb_jmp_cache_ptr = 0;
    g_vcpu_halted[cpu_id] = 0;
    g_vcpu_kicked[cpu_id] = 0;
    g_vcpu_init_pending[cpu_id] = 0;
    g_vcpu_sipi_pending[cpu_id] = 0;
    __atomic_store_n(&g_vcpu_online[cpu_id], 1, __ATOMIC_RELEASE);
    return 1;
}

static uint64_t build_guest_leaf_pte(uint32_t x86_r,
                                     const LatxX86Translation *x86_tr,
                                     uint32_t x86_access)
{
    uint64_t gpa_page = x86_tr->gpa & LAPG_PAGE_MASK;
    uint64_t hpa_page = 0;
    uint32_t perm = LAPG_PERM_READ | LAPG_PERM_USER;
    uint32_t mat = LAPG_MAT_SUC;
    LoongemuMemLookup lk;
    LoongemuMemKind mk = loongemu_mem_resolve(&g_mem_map, gpa_page,
                                              LAPG_PAGE_SIZE, &lk);

    if (mk == LOONGEMU_MEM_RESERVED) {
        /* Strict host/guest isolation: never map VMM-reserved HPA to PLV3. */
        g_isolation_violations++;
        return 0;
    } else if (mk == LOONGEMU_MEM_RAM || mk == LOONGEMU_MEM_ROM) {
        hpa_page = lk.hpa;
        mat = (uint32_t)lk.host_mat;
    } else if (mk == LOONGEMU_MEM_MMIO) {
        hpa_page = (gpa_page >= APIC_MMIO_BASE &&
                    gpa_page < APIC_MMIO_BASE + APIC_MMIO_SIZE)
                       ? (uint64_t)(uintptr_t)g_apic_mmio_page
                       : (uint64_t)(uintptr_t)g_mmio_dummy_page;
        mat = (uint32_t)lk.host_mat;
    } else if (gpa_page >= UINT64_C(0x10000000)) {
        /* Unclaimed high MMIO/PCI probe window -> harmless uncached dummy sink. */
        hpa_page = (uint64_t)(uintptr_t)g_mmio_dummy_page;
    } else {
        return 0;
    }
    if ((hpa_page & (LAPG_PAGE_SIZE - 1u)) != 0) {
        return 0;
    }

    if (x86_r == LATX_X86_WALK_PAGING_OFF) {
        perm |= LAPG_PERM_WRITE | LAPG_PERM_EXEC;
    } else {
        int user_access = (x86_access & LATX_X86_ACCESS_USER) != 0;
        int user_page = (x86_tr->perm & LATX_X86_ACCESS_USER) != 0;
        if ((x86_tr->perm & LATX_X86_ACCESS_WRITE) ||
            (!user_access && !(env.cr[0] & CR0_WP_MASK))) {
            perm |= LAPG_PERM_WRITE;
        }
        if (x86_tr->perm & LATX_X86_ACCESS_EXEC) {
            perm |= LAPG_PERM_EXEC;
        }
        /* Enforce SMEP on installed LoongArch PTE */
        if (!user_access && user_page && (env.cr[4] & X86_CR4_SMEP)) {
            perm &= ~LAPG_PERM_EXEC;
        }
        /* Enforce PKU write-deny on installed LoongArch PTE so writes trap */
        if (user_access && user_page && (env.cr[4] & X86_CR4_PKE)) {
            uint32_t rights = (env.pkru >> (x86_tr->pkey * 2u)) & 3u;
            if (rights & 2u) {
                perm &= ~LAPG_PERM_WRITE;
            }
        }
    }
    if (mk == LOONGEMU_MEM_ROM) {
        perm &= ~LAPG_PERM_WRITE;
    }
    if (mk == LOONGEMU_MEM_RAM) {
        uint64_t gpa_pfn = gpa_page >> 12;
        if (gpa_pfn < CODE_PAGE_MAX_PFN &&
            ((g_code_page_bitmap[gpa_pfn >> 6] >> (gpa_pfn & 63u)) & 1u)) {
            if (x86_access & LATX_X86_ACCESS_WRITE) {
                smc_record_dirty_pfn((uint32_t)gpa_pfn);
                __atomic_fetch_and(&g_code_page_bitmap[gpa_pfn >> 6],
                                   ~(UINT64_C(1) << (gpa_pfn & 63u)),
                                   __ATOMIC_RELAXED);
            } else {
                perm &= ~LAPG_PERM_WRITE;
            }
        }
    }
    return lapg_make_leaf_mat(hpa_page >> LAPG_PAGE_BITS, perm, mat);
}

static void test_vmm_handoff_and_bochs_fb(void)
{
    static uint8_t test_fb[8192] __attribute__((aligned(4096)));
    LoongemuVmmHandoff handoff;
    LoongemuMemMap saved_mem_map = g_mem_map;
    MmioRegion saved_mmio_regions[MAX_MMIO_REGIONS];
    LoongemuBochsDisplay saved_display = g_pci_host.display;
    uint32_t saved_mmio_count = g_mmio_region_count;
    uint64_t saved_fallback = g_bochs_fb_fallback_hpa;
    uint64_t saved_bar_gpa = g_bochs_fb_bar_gpa;
    uint64_t saved_bar_size = g_bochs_fb_bar_size;
    uint64_t saved_valid_size = g_bochs_fb_valid_size;
    uint64_t saved_sink_gpa = g_bochs_fb_sink_gpa;
    uint64_t saved_sink_size = g_bochs_fb_sink_size;
    uint64_t saved_mmio_bar_gpa = g_bochs_mmio_bar_gpa;
    uint64_t saved_mmio_bar_size = g_bochs_mmio_bar_size;
    int saved_guest_ram_external = g_guest_ram_external;
    BochsBarPending saved_bar_pending[2];
    uint64_t saved_helper_fn = g_active_helper_fn;
    uint64_t saved_helper_a1 = g_active_helper_a1;
    uint64_t saved_helper_a2 = g_active_helper_a2;
    uint64_t saved_helper_ra = g_active_helper_ra;
    uint32_t saved_helper_bar_pending = g_active_helper_bar_pending;
    LoongemuMemLookup lookup;
    LatxX86Translation x86_tr = {0};
    uint64_t pte;
    int canary_ok = 1;
    const uint64_t fb_gpa = UINT64_C(0x40000000);
    const uint64_t moved_fb_gpa = UINT64_C(0x50000000);
    const uint64_t fallback_test_gpa = UINT64_C(0x60000000);
    const uint64_t owner_test_gpa = UINT64_C(0x70000000);
    const uint64_t reserved_test_gpa = UINT64_C(0x70010000);
    const uint64_t mmio_overlap_gpa = UINT64_C(0x70030000);
    const uint64_t no_backing_gpa = UINT64_C(0x71000000);
    const uint64_t queued_fb_gpa = UINT64_C(0x70040000);
    int callback_ok;

    memcpy(saved_bar_pending, g_bochs_bar_pending,
           sizeof(saved_bar_pending));
    memset(g_bochs_bar_pending, 0, sizeof(g_bochs_bar_pending));
    memset(&handoff, 0, sizeof(handoff));
    handoff.magic = LOONGEMU_VMM_HANDOFF_MAGIC;
    handoff.version = LOONGEMU_VMM_HANDOFF_VERSION;
    handoff.host_timer_hz = UINT64_C(100000000);
    handoff.host_uart_phys = UINT64_C(0x1fe001e0);
    handoff.vmm_image_base = UINT64_C(0x10000000);
    handoff.vmm_image_size = UINT64_C(0x00100000);
    handoff.guest_ram_hpa = UINT64_C(0x20000000);
    handoff.guest_ram_size = GUEST_RAM_SIZE;
    handoff.fb_fallback_hpa = UINT64_C(0x30000000);
    handoff.fb_fallback_size = BOCHS_FB_FALLBACK_SIZE;
    handoff.ovmf_code_hpa = UINT64_C(0x30400000);
    handoff.ovmf_code_size = UINT64_C(0x00200000);
    handoff.ovmf_vars_hpa = UINT64_C(0x30600000);
    handoff.ovmf_vars_size = UINT64_C(0x00200000);
    handoff.gop.fb_hpa = UINT64_C(0x30800000);
    handoff.gop.fb_size = UINT64_C(0x4000);
    handoff.gop.width = 64u;
    handoff.gop.height = 64u;
    handoff.gop.stride_px = 64u;
    handoff.gop.bpp = 32u;
    handoff.gop.pixel_format = 0u;
    handoff.gop.valid = 1u;
    handoff.gop.owner = (uint8_t)LOONGEMU_FB_OWNER_HOST_GOP;
    handoff.gop.host_mat = (uint8_t)LOONGEMU_MAT_WUC;
    check(vmm_handoff_valid(&handoff),
          "UEFI handoff accepts page-aligned non-overlapping buffers");
    handoff.gop.fb_hpa = handoff.guest_ram_hpa;
    check(!vmm_handoff_valid(&handoff),
          "UEFI handoff rejects GOP backing that overlaps guest RAM");
    handoff.gop.fb_hpa = UINT64_C(0x30800000);
    handoff.gop.fb_hpa = handoff.vmm_image_base;
    check(!vmm_handoff_valid(&handoff),
          "UEFI handoff rejects GOP backing that overlaps the VMM image");
    handoff.gop.fb_hpa = handoff.fb_fallback_hpa;
    check(!vmm_handoff_valid(&handoff),
          "UEFI handoff rejects GOP backing that overlaps fallback storage");
    handoff.gop.fb_hpa = handoff.ovmf_code_hpa;
    check(!vmm_handoff_valid(&handoff),
          "UEFI handoff rejects GOP backing that overlaps firmware");
    handoff.gop.fb_hpa = UINT64_C(0x30800000);
    handoff.fb_fallback_hpa = handoff.guest_ram_hpa +
                              handoff.guest_ram_size - LAPG_PAGE_SIZE;
    check(!vmm_handoff_valid(&handoff),
          "UEFI handoff rejects overlapping guest and fallback allocations");
    handoff.fb_fallback_hpa = UINT64_C(0x30000000);
    handoff.ovmf_vars_hpa = handoff.ovmf_code_hpa + LAPG_PAGE_SIZE;
    check(!vmm_handoff_valid(&handoff),
          "UEFI handoff rejects overlapping OVMF buffers");
    handoff.ovmf_vars_hpa = UINT64_C(0x30600000);
    handoff.guest_ram_hpa++;
    check(!vmm_handoff_valid(&handoff),
          "UEFI handoff rejects unaligned physical allocations");

    memcpy(saved_mmio_regions, g_mmio_regions, sizeof(saved_mmio_regions));
    loongemu_mem_map_init(&g_mem_map);
    memset(g_mmio_regions, 0, sizeof(g_mmio_regions));
    g_mmio_region_count = 0;
    g_bochs_fb_bar_gpa = 0;
    g_bochs_fb_bar_size = 0;
    g_bochs_fb_valid_size = 0;
    g_bochs_fb_sink_gpa = 0;
    g_bochs_fb_sink_size = 0;
    g_guest_ram_external = 0;
    memset(test_fb, 0xa5, sizeof(test_fb));
    g_pci_host.display.gop.valid = 1u;
    g_pci_host.display.gop.owner = (uint8_t)LOONGEMU_FB_OWNER_HOST_GOP;
    g_pci_host.display.gop.fb_hpa = (uint64_t)(uintptr_t)test_fb;
    g_pci_host.display.gop.fb_size = 5000u;
    g_pci_host.display.gop.host_mat = (uint8_t)LOONGEMU_MAT_WUC;
    callback_ok = bochs_display_bar_map_cb(0, 0, fb_gpa, UINT64_C(0x5000), 1);
    check(callback_ok &&
          loongemu_mem_resolve(&g_mem_map, fb_gpa, LAPG_PAGE_SIZE, &lookup) ==
              LOONGEMU_MEM_RAM &&
          lookup.hpa == (uint64_t)(uintptr_t)test_fb &&
          lookup.host_mat == (uint8_t)LOONGEMU_MAT_WUC &&
          g_bochs_fb_valid_size == LAPG_PAGE_SIZE,
          "Bochs BAR0 maps only complete host-GOP pages with WUC ownership");
    x86_tr.gpa = fb_gpa;
    x86_tr.perm = LATX_X86_ACCESS_READ | LATX_X86_ACCESS_USER;
    pte = build_guest_leaf_pte(LATX_X86_WALK_PAGING_OFF, &x86_tr,
                               LATX_X86_ACCESS_READ | LATX_X86_ACCESS_USER);
    check(((pte & LAPG_PTE_MAT_MASK) >> LAPG_PTE_MAT_SHIFT) == LAPG_MAT_WUC,
          "host-GOP MAT reaches the actual guest shadow PTE");
    {
        const MmioRegion *sink = mmio_find_region(fb_gpa + LAPG_PAGE_SIZE);
        check(sink != 0 && sink->gpa_base == fb_gpa + LAPG_PAGE_SIZE &&
              sink->size == UINT64_C(0x4000) &&
              sink->read_cb == bochs_oob_sink_read &&
              sink->write_cb == bochs_oob_sink_write &&
              sink->read_cb(sink->opaque, 0, 4) == 0,
              "Bochs BAR0 tail is an MMIO read/write sink");
        if (sink != 0) {
            sink->write_cb(sink->opaque, 0, UINT64_MAX, 4);
        }
    }
    callback_ok = bochs_display_bar_map_cb(0, 0, moved_fb_gpa,
                                            UINT64_C(0x5000), 1);
    check(callback_ok &&
          loongemu_mem_resolve(&g_mem_map, fb_gpa, LAPG_PAGE_SIZE, &lookup) ==
              LOONGEMU_MEM_UNMAPPED &&
          mmio_find_region(fb_gpa + LAPG_PAGE_SIZE) == 0,
          "Bochs BAR0 relocation removes the old RAM mapping and sink");
    for (uint32_t i = 0; i < sizeof(test_fb); i++) {
        if (test_fb[i] != 0xa5u) {
            canary_ok = 0;
            break;
        }
    }
    check(canary_ok, "Bochs tail writes leave the host framebuffer canary intact");
    g_guest_ram_external = saved_guest_ram_external;

    g_pci_host.display.gop = (LoongemuGopHandover){0};
    callback_ok = bochs_display_bar_map_cb(0, 0, fallback_test_gpa,
                                            UINT64_C(0x5000), 1);
    check(callback_ok &&
          loongemu_mem_resolve(&g_mem_map, fallback_test_gpa, LAPG_PAGE_SIZE,
                               &lookup) == LOONGEMU_MEM_RAM &&
          lookup.hpa == g_bochs_fb_fallback_hpa &&
          lookup.host_mat == (uint8_t)LOONGEMU_MAT_CC,
          "Bochs fallback BAR uses its independent allocation with CC MAT");
    x86_tr.gpa = fallback_test_gpa;
    pte = build_guest_leaf_pte(LATX_X86_WALK_PAGING_OFF, &x86_tr,
                               LATX_X86_ACCESS_READ | LATX_X86_ACCESS_USER);
    check(((pte & LAPG_PTE_MAT_MASK) >> LAPG_PTE_MAT_SHIFT) == LAPG_MAT_CC,
          "VMM-allocated fallback MAT reaches the actual guest shadow PTE");

    g_pci_host.display.gop.valid = 1u;
    g_pci_host.display.gop.owner = (uint8_t)LOONGEMU_FB_OWNER_VMM_ALLOC;
    g_pci_host.display.gop.fb_hpa = saved_fallback;
    g_pci_host.display.gop.fb_size = UINT64_C(0x3000);
    g_pci_host.display.gop.host_mat = (uint8_t)LOONGEMU_MAT_WUC;
    callback_ok = bochs_display_bar_map_cb(0, 0, owner_test_gpa,
                                            UINT64_C(0x3000), 1);
    check(callback_ok &&
          loongemu_mem_resolve(&g_mem_map, owner_test_gpa, LAPG_PAGE_SIZE,
                               &lookup) == LOONGEMU_MEM_RAM &&
          lookup.hpa == saved_fallback &&
          lookup.host_mat == (uint8_t)LOONGEMU_MAT_CC,
          "VMM_ALLOC framebuffer ownership forces coherent cached MAT");
    check(loongemu_mem_map_add(&g_mem_map, reserved_test_gpa, UINT64_C(0x10000),
                               0, LOONGEMU_MEM_RESERVED, "fb_test_reserved") == 1,
          "framebuffer overlap test reserves a conflicting guest range");
    callback_ok = bochs_display_bar_map_cb(0, 0, reserved_test_gpa,
                                            UINT64_C(0x3000), 1);
    check(!callback_ok &&
          loongemu_mem_resolve(&g_mem_map, reserved_test_gpa, LAPG_PAGE_SIZE,
                               &lookup) == LOONGEMU_MEM_RESERVED &&
          mmio_find_region(reserved_test_gpa) == 0 &&
          loongemu_mem_resolve(&g_mem_map, owner_test_gpa, LAPG_PAGE_SIZE,
                               &lookup) == LOONGEMU_MEM_RAM &&
          lookup.hpa == saved_fallback,
          "Bochs BAR0 cannot overlap a reserved guest range");
    check(mmio_register(mmio_overlap_gpa, LAPG_PAGE_SIZE,
                        loongemu_bochs_mmio_read, loongemu_bochs_mmio_write,
                        &g_pci_host.display),
          "framebuffer overlap test registers an existing MMIO range");
    callback_ok = bochs_display_bar_map_cb(0, 0, mmio_overlap_gpa,
                                            UINT64_C(0x3000), 1);
    {
        const MmioRegion *existing = mmio_find_region(mmio_overlap_gpa);
        check(!callback_ok && existing != 0 &&
              existing->read_cb == loongemu_bochs_mmio_read &&
              loongemu_mem_resolve(&g_mem_map, mmio_overlap_gpa,
                                   LAPG_PAGE_SIZE, &lookup) ==
                  LOONGEMU_MEM_UNMAPPED,
              "Bochs BAR0 cannot overlap an existing MMIO range");
    }
    g_pci_host.display.gop.owner = (uint8_t)LOONGEMU_FB_OWNER_HOST_GOP;
    g_pci_host.display.gop.fb_hpa = (uint64_t)(uintptr_t)test_fb + 1u;
    g_pci_host.display.gop.fb_size = LAPG_PAGE_SIZE;
    callback_ok = bochs_display_bar_map_cb(
        0, 0, owner_test_gpa + UINT64_C(0x20000), UINT64_C(0x3000), 1);
    check(callback_ok &&
          loongemu_mem_resolve(&g_mem_map, owner_test_gpa + UINT64_C(0x20000),
                               LAPG_PAGE_SIZE, &lookup) == LOONGEMU_MEM_RAM &&
          lookup.hpa == saved_fallback &&
          lookup.host_mat == (uint8_t)LOONGEMU_MAT_CC,
          "unaligned host-GOP backing is never exposed to the guest");
    g_pci_host.display.gop = (LoongemuGopHandover){0};
    g_bochs_fb_fallback_hpa = 0;
    callback_ok = bochs_display_bar_map_cb(0, 0, no_backing_gpa,
                                            UINT64_C(0x3000), 1);
    check(!callback_ok &&
          loongemu_mem_resolve(&g_mem_map, no_backing_gpa, LAPG_PAGE_SIZE,
                               &lookup) == LOONGEMU_MEM_UNMAPPED &&
          loongemu_mem_resolve(&g_mem_map, owner_test_gpa + UINT64_C(0x20000),
                               LAPG_PAGE_SIZE,
                               &lookup) == LOONGEMU_MEM_RAM &&
          lookup.hpa == saved_fallback,
          "missing real and fallback backing refuses BAR mapping without losing old BAR");
    g_bochs_fb_fallback_hpa = saved_fallback;
    g_active_helper_fn = (uint64_t)(uintptr_t)helper_outl;
    g_active_helper_a1 = LOONGEMU_PCI_CFG_DATA_PORT;
    g_active_helper_bar_pending = 0;
    callback_ok = bochs_display_bar_map_cb(
        0, 0, queued_fb_gpa, UINT64_C(0x3000), 1);
    check(callback_ok && g_active_helper_bar_pending &&
          g_bochs_bar_pending[0].valid &&
          g_bochs_bar_pending[0].owner_vcpu == g_cur_vcpu &&
          bochs_helper_requires_tb_exit() &&
          loongemu_mem_resolve(&g_mem_map, queued_fb_gpa, LAPG_PAGE_SIZE,
                               &lookup) == LOONGEMU_MEM_UNMAPPED &&
          loongemu_mem_resolve(&g_mem_map, g_bochs_fb_bar_gpa,
                               LAPG_PAGE_SIZE, &lookup) == LOONGEMU_MEM_RAM,
          "PCI OUT remap exits its TB before a following framebuffer access");
    g_bochs_bar_pending[0].valid = 0;
    g_active_helper_bar_pending = 0;
    g_active_helper_fn = 0;
    g_bochs_mmio_bar_write = 1;
    callback_ok = bochs_display_bar_map_cb(
        0, 0, queued_fb_gpa + UINT64_C(0x10000), UINT64_C(0x3000), 1);
    g_bochs_mmio_bar_write = 0;
    check(callback_ok && g_bochs_bar_pending[0].valid &&
          !g_active_helper_bar_pending,
          "ECAM BAR write queues the remap without a helper context");
    g_bochs_bar_pending[0].valid = 0;
    g_active_helper_a1 = UINT64_C(0x80);
    check(!bochs_helper_requires_tb_exit(),
          "unrelated OUT does not trigger framebuffer TB exit");
    g_active_helper_fn = 0;
    g_active_helper_a1 = 0;
    g_active_helper_a2 = 0;
    g_active_helper_ra = 0;
    g_active_helper_bar_pending = 0;
    g_mem_map = saved_mem_map;
    memcpy(g_mmio_regions, saved_mmio_regions, sizeof(saved_mmio_regions));
    g_mmio_region_count = saved_mmio_count;
    g_pci_host.display = saved_display;
    g_bochs_fb_fallback_hpa = saved_fallback;
    g_bochs_fb_bar_gpa = saved_bar_gpa;
    g_bochs_fb_bar_size = saved_bar_size;
    g_bochs_fb_valid_size = saved_valid_size;
    g_bochs_fb_sink_gpa = saved_sink_gpa;
    g_bochs_fb_sink_size = saved_sink_size;
    g_guest_ram_external = saved_guest_ram_external;
    g_bochs_mmio_bar_gpa = saved_mmio_bar_gpa;
    g_bochs_mmio_bar_size = saved_mmio_bar_size;
    memcpy(g_bochs_bar_pending, saved_bar_pending,
           sizeof(saved_bar_pending));
    g_active_helper_fn = saved_helper_fn;
    g_active_helper_a1 = saved_helper_a1;
    g_active_helper_a2 = saved_helper_a2;
    g_active_helper_ra = saved_helper_ra;
    g_active_helper_bar_pending = saved_helper_bar_pending;
}

static void fill_hw_tlb_guest(uint64_t badv, uint64_t fault_pte, int is_refill)
{
    uint64_t vppn = badv & UINT64_C(0x0000ffffffffe000);
    uint64_t fault_page = badv & LAPG_PAGE_MASK;
    uint64_t sib_page = fault_page ^ LAPG_PAGE_SIZE;
    uint64_t sib_pte = 0;

    {
        uint32_t sib_acc = LATX_X86_ACCESS_READ |
                           ((env.hflags & HF_CPL_MASK) ? LATX_X86_ACCESS_USER : 0);
        LatxX86Translation sib_tr;
        LatxX86WalkResult sib_r = latx_x86_walk_cached(&env, sib_page, sib_acc, &sib_tr);
        if ((sib_r == LATX_X86_WALK_OK ||
             sib_r == LATX_X86_WALK_PAGING_OFF) &&
            mmio_find_region(sib_tr.gpa) == 0) {
            /* MMIO, including the framebuffer tail sink, must trap on access. */
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
        __asm__ volatile("invtlb 0x5, %0, %1" :: "r"((uint64_t)g_cur_asid), "r"(vppn) : "memory");
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

    {
        uint64_t ecfg;

        __asm__ volatile("csrrd %0, 0x4" : "=r"(ecfg) :: "memory");
        ecfg |= (1ULL << 11);
        __asm__ volatile("csrwr %0, 0x4" : "+r"(ecfg) :: "memory");
    }

    __asm__ volatile("csrwr %0, 0x41" : "+r"(tcfg) :: "memory");
}

static void timer_stop(void)
{
    uint64_t tcfg = 0;
    uint64_t ecfg;

    __asm__ volatile("csrwr %0, 0x41" : "+r"(tcfg) :: "memory");

    __asm__ volatile("csrrd %0, 0x4" : "=r"(ecfg) :: "memory");
    ecfg &= ~(1ULL << 11);
    __asm__ volatile("csrwr %0, 0x4" : "+r"(ecfg) :: "memory");
}

/* Acknowledge one timer interrupt at the source. */
static void timer_ack(void)
{
    uint64_t one = 1;

    __asm__ volatile("csrwr %0, 0x44" : "+r"(one) :: "memory");
}
static int guest_linear_read_u64(uint64_t linear, uint64_t *value)
{
    uint64_t result = 0;

    for (unsigned i = 0; i < 8u; i++) {
        LatxX86Translation translation;
        uint64_t byte;
        LatxX86WalkResult walk =
            latx_x86_walk(&env, linear + i, LATX_X86_ACCESS_READ, &translation);
        if ((walk != LATX_X86_WALK_OK && walk != LATX_X86_WALK_PAGING_OFF) ||
            !guest_phys_read_checked(translation.gpa, 1, &byte)) {
            return 0;
        }
        result |= (byte & 0xffu) << (i * 8u);
    }
    *value = result;
    return 1;
}

static int guest_linear_write_u64(uint64_t linear, uint64_t value)
{
    for (unsigned i = 0; i < 8u; i++) {
        LatxX86Translation translation;
        LatxX86WalkResult walk =
            latx_x86_walk(&env, linear + i, LATX_X86_ACCESS_WRITE, &translation);
        if ((walk != LATX_X86_WALK_OK && walk != LATX_X86_WALK_PAGING_OFF) ||
            !guest_phys_write_checked(translation.gpa, 1, value >> (i * 8u))) {
            return 0;
        }
    }
    return 1;
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
    uint64_t idt_base, entry, hi, handler, rflags, rsp, stack_gva;
    uint16_t cs_sel;
    int vec = (int)g_timer_vector;

    rflags = env.eflags;
    if ((rflags & IF_MASK) == 0) {
        g_timer_masked++;
        return 0;
    }

    idt_base = env.idt.base;
    if (idt_base == 0 ||
        !guest_linear_read_u64(idt_base + (uint64_t)vec * 16u, &entry) ||
        !guest_linear_read_u64(idt_base + (uint64_t)vec * 16u + 8u, &hi)) {
        g_timer_masked++;
        return 0;
    }

    handler = (entry & 0xffffULL) | (((entry >> 48) & 0xffffULL) << 16) |
              ((hi & 0xffffffffULL) << 32);
    if (handler == 0) {
        g_timer_masked++;
        return 0;
    }

    cs_sel = (uint16_t)((entry >> 16) & 0xffff);
    rsp = env.regs[R_ESP];
    if (rsp < 40u) {
        return 0;
    }
    stack_gva = rsp - 40u;
    if (!guest_linear_write_u64(stack_gva + 0u, env.eip) ||
        !guest_linear_write_u64(stack_gva + 8u, cs_sel) ||
        !guest_linear_write_u64(stack_gva + 16u, rflags) ||
        !guest_linear_write_u64(stack_gva + 24u, rsp) ||
        !guest_linear_write_u64(stack_gva + 32u, env.segs[R_SS].selector)) {
        return 0;
    }

    env.regs[R_ESP] = stack_gva;
    env.eip = handler;
    env.eflags = rflags & ~IF_MASK;
    g_timer_delivered++;
    return 1;
}

static int resolve_exact_insn_boundary_x86_pc(uint64_t fault_pc,
                                              uint64_t *out_x86_pc)
{
    /* Only preempt at TB entry (off == 0) where flag_reduction and IR2
     * optimizations have materialized all architectural EFLAGS and GPRs. */
    int slot = find_tb_slot_by_code_pc(fault_pc);
    if (slot >= 0 && fault_pc == tb_entries[slot].code) {
        *out_x86_pc = tb_entries[slot].pc;
        return 1;
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
#ifdef LOONGEMU_BOOT_OVMF
static uint64_t g_ovmf_uniq_pcs[16];
static uint16_t g_ovmf_uniq_cs[16];
static uint64_t g_ovmf_uniq_count;
static uint32_t g_ovmf_exc_logs;
#endif
static uint64_t g_run_tb_hits;
static uint64_t g_run_tb_misses;
static uint64_t g_run_tb_flushes;
static uint64_t g_sync_core_calls;
static uint64_t g_sync_core_flushes;

static void mark_code_pages_ro(uint32_t slot_idx, uint64_t gva, uint16_t len)
{
    uint64_t cur = gva & LAPG_PAGE_MASK;
    uint64_t end = (gva + (uint64_t)len + LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    uint32_t p0 = UINT32_MAX, p1 = UINT32_MAX;

    for (; cur < end; cur += LAPG_PAGE_SIZE) {
        LatxX86Translation tr_g;
        if (!walk_ok(latx_x86_walk_cached(&env, cur, LATX_X86_ACCESS_READ, &tr_g))) {
            continue;
        }
        uint64_t pfn = tr_g.gpa >> 12;
        if (pfn >= CODE_PAGE_MAX_PFN) {
            continue;
        }
        if (p0 == UINT32_MAX) {
            p0 = (uint32_t)pfn;
        } else {
            p1 = (uint32_t)pfn;
        }
        uint64_t mask = UINT64_C(1) << (pfn & 63u);
        loongemu_spin_lock(&g_code_page_lock);
        int already = ((g_code_page_bitmap[pfn >> 6] & mask) != 0);
        if (!already) {
            g_code_page_bitmap[pfn >> 6] |= mask;
        }
        loongemu_spin_unlock(&g_code_page_lock);
        if (already) {
            continue;
        }
        /*
         * Flush all non-global HW TLB entries on this pCPU when a PFN first
         * becomes code-protected (0 -> 1). This removes writable entries at
         * any virtual alias (kernel text, direct-map, or fixmap) so future
         * writes to this PFN are guaranteed to trap via EXC_PME.
         */
        __asm__ volatile("invtlb 0x3, $zero, $zero" ::: "memory");
    }
    tb_pfn0[slot_idx] = p0;
    tb_pfn1[slot_idx] = (p1 == UINT32_MAX) ? p0 : p1;
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
        LoongemuMemLookup lk_h;
        LoongemuMemKind mk_h = loongemu_mem_resolve(&g_mem_map, tr_g.gpa, chunk, &lk_h);
        if (mk_h != LOONGEMU_MEM_RAM && mk_h != LOONGEMU_MEM_ROM) {
            return 0;
        }
        const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)lk_h.hpa;
        for (uint16_t i = 0; i < chunk; i++) {
            h ^= (uint64_t)p[i];
            h *= UINT64_C(1099511628211);
        }
        cur += chunk;
        rem -= chunk;
    }
    return h ? h : 1;
}


static inline uint64_t current_tb_cs_base(void)
{
    return (env.sys.codemode != LATX_DT_X64) ? env.segs[R_CS].base : 0ULL;
}

static inline uint8_t current_tb_mode_flags(void)
{
    uint8_t f = (uint8_t)(env.sys.codemode & 3u);
    if (env.cr[0] & CR0_PE_MASK) f |= (1u << 2);
    if (env.hflags & HF_CS32_MASK) f |= (1u << 3);
    if (env.hflags & HF_CS64_MASK) f |= (1u << 4);
    return f;
}


static int translate_tb_locked(uint64_t pc, uint8_t *buf, size_t max_bytes,
                               TbInsnMap *map)
{
    int rc;
    loongemu_spin_lock(&g_translate_lock);
    lsenv->cpu_state = cur_env;
    tcg_storage.code_gen_buffer = code_cache;
    tcg_storage.code_gen_buffer_size = CODE_CACHE_SIZE;
    tcg_storage.code_gen_ptr = buf;
    tcg_storage.code_gen_highwater = code_cache + CODE_CACHE_SIZE;
    rc = tb_harness_translate_at_with_map(pc, buf, max_bytes, map);
    loongemu_spin_unlock(&g_translate_lock);
    return rc;
}

static void smc_flush_local_shard(int32_t *prev_slot_io)
{
    uint32_t local_pfns[SMC_DIRTY_PFN_CAP];
    uint32_t local_cnt = 0;
    int local_overflow = 0;

    if (!g_smc_dirty) {
        return;
    }
    loongemu_spin_lock(&g_code_page_lock);
    local_overflow = g_smc_dirty_overflow;
    local_cnt = g_smc_dirty_count;
    if (!local_overflow && local_cnt > 0) {
        for (uint32_t d = 0; d < local_cnt; d++) {
            local_pfns[d] = g_smc_dirty_pfns[d];
        }
    }
    g_smc_dirty = 0;
    g_smc_dirty_count = 0;
    g_smc_dirty_overflow = 0;
    loongemu_spin_unlock(&g_code_page_lock);

    if (local_overflow) {
        tbc_flush_all(&tb_cache);
        g_tb_highwater = 0;
        g_tb_by_code_count = 0;
        *prev_slot_io = -1;
        memset(fast_jmp_cache, 0, sizeof(g_fast_jmp_cache[0]));
        code_used = 0x2000;
        __atomic_fetch_add(&g_sync_core_flushes, 1u, __ATOMIC_RELAXED);
        __asm__ volatile("dbar 0\n ibar 0" ::: "memory");
        return;
    }
    if (local_cnt > 0) {
        uint64_t evicted_pcs[128];
        uint32_t evicted_cnt = 0;
        int overflow_evict = 0;
        /*
         * Evict EVERY TB residing on a dirty PFN (do not keep "unchanged"
         * sibling TBs on the same 4 KiB page): g_code_page_bitmap[pfn] was
         * cleared on the first write fault, so keeping any TB on that page
         * would let subsequent writes to the same page (e.g. text_poke_early
         * patching multiple sites on one page) bypass EXC_PME and leave stale
         * TBs live. Once all TBs on pfn are evicted, the next translation on
         * pfn re-arms g_code_page_bitmap[pfn] and flushes writable TLB entries.
         */
        for (uint32_t i = 0; i < g_tb_highwater; i++) {
            if (!tb_entries[i].used) {
                continue;
            }
            int pfn_hit = 0;
            for (uint32_t d = 0; d < local_cnt; d++) {
                if (tb_pfn0[i] == local_pfns[d] ||
                    tb_pfn1[i] == local_pfns[d]) {
                    pfn_hit = 1;
                    break;
                }
            }
            if (!pfn_hit) {
                continue;
            }
            uint64_t epc = tb_entries[i].pc;
            uint32_t h = (uint32_t)(((epc >> TB_JMP_CACHE_BITS) ^ epc) &
                                    (FAST_JMP_CACHE_SIZE - 1));
            if (fast_jmp_cache[h].pc == epc) {
                fast_jmp_cache[h].pc = 0;
                fast_jmp_cache[h].code = 0;
            }
            tbc_invalidate_slot(&tb_cache, i);
            __atomic_fetch_add(&g_smc_granular_evicts, 1u, __ATOMIC_RELAXED);
            if (evicted_cnt < 128u) {
                evicted_pcs[evicted_cnt++] = epc;
            } else {
                overflow_evict = 1;
            }
        }
        if (overflow_evict) {
            tbc_flush_all(&tb_cache);
            g_tb_highwater = 0;
            g_tb_by_code_count = 0;
            *prev_slot_io = -1;
            memset(fast_jmp_cache, 0, sizeof(g_fast_jmp_cache[0]));
            code_used = 0x2000;
            __atomic_fetch_add(&g_sync_core_flushes, 1u, __ATOMIC_RELAXED);
            __asm__ volatile("dbar 0\n ibar 0" ::: "memory");
        } else if (evicted_cnt > 0) {
            for (uint32_t j = 0; j < g_tb_highwater; j++) {
                if (!tb_entries[j].used) {
                    continue;
                }
                TbInsnMap *jm = &tb_insn_maps[j];
                for (int e_i = 0; e_i < 2; e_i++) {
                    if (jm->jmp_slot_off[e_i] < 0) continue;
                    uint64_t tpc = jm->jmp_target_pc[e_i];
                    for (uint32_t k = 0; k < evicted_cnt; k++) {
                        if (tpc == evicted_pcs[k]) {
                            uint32_t *sp = (uint32_t *)(uintptr_t)(
                                tb_entries[j].code + (uint64_t)jm->jmp_slot_off[e_i]);
                            *sp = 0x03400000u;
                            break;
                        }
                    }
                }
            }
            *prev_slot_io = -1;
            __asm__ volatile("dbar 0\n ibar 0" ::: "memory");
        }
    }
}

static void run_ap_vcpu_loop(uint32_t pcpu)
{
    int32_t prev_slot = -1;

    g_pcpu_vcpu[pcpu] = pcpu;
    tbc_init(&tb_cache, tb_entries, tb_buckets, TB_TABLE_CAP);
    g_tb_highwater = 0;
    g_tb_by_code_count = 0;
    code_used = 0x2000;
    g_smc_dirty = 0;
    g_smc_dirty_count = 0;
    g_smc_dirty_overflow = 0;
    memset(fast_jmp_cache, 0, sizeof(g_fast_jmp_cache[0]));
    env.tb_jmp_cache_ptr = 0;
    latx_x86_tlb_flush_all();
    asid_activate(1);
    g_plv3_timer_preempt = 1;
    timer_ack();
    timer_start(timer_ticks_1ms());

    for (;;) {
        if (hv_service_flush_requests()) {
            prev_slot = -1;
        }
        int bar_map_status = bochs_process_pending_bar_maps();
        if (bar_map_status != 0) {
            prev_slot = -1;
        }
        if (bar_map_status < 0) {
            continue;
        }
        if (hv_recycle_tb_shard()) {
            prev_slot = -1;
        }
        if (__atomic_exchange_n(&cur_shard->tlb_shootdown_pending, 0,
                                __ATOMIC_ACQ_REL)) {
            __asm__ volatile("invtlb 0x3, $zero, $zero" ::: "memory");
            latx_x86_tlb_flush_all();
            memset(g_asid_slots, 0, sizeof(g_asid_slots));
            g_next_asid = 1;
            asid_activate(1);
            __atomic_fetch_add(&g_ipi_shootdowns_handled, 1u, __ATOMIC_RELAXED);
        }
        if (g_smc_dirty) {
            smc_flush_local_shard(&prev_slot);
        }
        uint64_t before = env.eip;
        uint64_t tb_cs_base = current_tb_cs_base();
        uint8_t tb_mode = current_tb_mode_flags();
        uint64_t fetch_lin = tb_cs_base + before;
        uint8_t cpl = (uint8_t)(env.hflags & HF_CPL_MASK);
        uint64_t tb_cr3 = (cpl == 0 && (fetch_lin >> 63) != 0) ? 0 : env.cr[3];
        TbCacheEntry *e = tbc_lookup_mode(&tb_cache, before, tb_cs_base, tb_cr3, cpl, tb_mode);
        uint32_t slot_idx;

        if (e == 0) {
            uint32_t fetch_acc = LATX_X86_ACCESS_EXEC |
                                 (cpl ? LATX_X86_ACCESS_USER : 0);
            LatxX86Translation tr_fetch;
            LatxX86WalkResult fr = latx_x86_walk(&env, fetch_lin, fetch_acc, &tr_fetch);
            if (fr != LATX_X86_WALK_OK && fr != LATX_X86_WALK_PAGING_OFF) {
                latx_x86_tlb_flush_page(fetch_lin);
                env.cr[2] = fetch_lin;
                env.exception_index = EXCP0E_PAGE;
                env.error_code = (int)(tr_fetch.fault_error_code
                                           ? tr_fetch.fault_error_code
                                           : latx_x86_fault_error_code(&env, fr, fetch_acc));
                if (latx_x86_deliver_fault(&env, 14) != 0) {
                    for (;;) { __asm__ volatile("dbar 0x700" ::: "memory"); }
                }
                latx_platform_tlb_flush_all();
                prev_slot = -1;
                continue;
            }
            if (code_used + 32768 > CODE_CACHE_SIZE ||
                tbc_count(&tb_cache) >= TB_TABLE_CAP - 1 ||
                g_tb_by_code_count >= TB_TABLE_CAP - 1) {
                tbc_flush_all(&tb_cache);
                g_tb_highwater = 0;
                g_tb_by_code_count = 0;
                prev_slot = -1;
                memset(fast_jmp_cache, 0, sizeof(g_fast_jmp_cache[0]));
                code_used = 0x2000;
                __asm__ volatile("ibar 0" ::: "memory");
            }
            e = tbc_insert_mode(&tb_cache, before, tb_cs_base, tb_cr3, cpl, tb_mode);
            if (e == 0) {
                tbc_flush_all(&tb_cache);
                g_tb_highwater = 0;
                g_tb_by_code_count = 0;
                prev_slot = -1;
                memset(fast_jmp_cache, 0, sizeof(g_fast_jmp_cache[0]));
                code_used = 0x2000;
                e = tbc_insert_mode(&tb_cache, before, tb_cs_base, tb_cr3, cpl, tb_mode);
            }
            slot_idx = (uint32_t)(e - tb_entries);
            if (slot_idx + 1 > g_tb_highwater) {
                g_tb_highwater = slot_idx + 1;
            }
            uint8_t *code = code_cache + code_used;
            env.exception_index = -1;
            int rc = translate_tb_locked(before, code, CODE_CACHE_SIZE - code_used,
                                         &tb_insn_maps[slot_idx]);
            if (env.exception_index == EXCP0E_PAGE) {
                e->used = 0;
                if (latx_x86_deliver_fault(&env, 14) != 0) {
                    for (;;) { __asm__ volatile("dbar 0x700" ::: "memory"); }
                }
                latx_platform_tlb_flush_all();
                prev_slot = -1;
                continue;
            }
            if (rc <= 0) {
                continue;
            }
            e->code = (uint64_t)(uintptr_t)code;
            e->code_size = (uint32_t)rc;
            if (g_tb_by_code_count < TB_TABLE_CAP) {
                g_tb_by_code[g_tb_by_code_count].code_start = e->code;
                g_tb_by_code[g_tb_by_code_count].code_end = e->code + (uint64_t)e->code_size;
                g_tb_by_code[g_tb_by_code_count].slot_idx = slot_idx;
                g_tb_by_code_count++;
            }
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
                e->guest_size = clen;
                tb_code_hash[slot_idx] = hash_guest_code(fetch_lin, clen);
                mark_code_pages_ro(slot_idx, fetch_lin, clen);
            }
            code_used += (((uint64_t)rc + 255) & ~UINT64_C(255));
            __asm__ volatile("ibar 0" ::: "memory");
        }

        slot_idx = (uint32_t)(e - tb_entries);
        g_current_tb_code = e->code;
        g_current_tb_size = e->code_size;
        g_current_tb_pc = before;
        g_current_tb_map = &tb_insn_maps[slot_idx];
        if (hv_service_flush_requests()) {
            prev_slot = -1;
            continue;
        }
        if (hv_wait_for_barrier_release()) {
            prev_slot = -1;
            continue;
        }

        tb_sys_run_plv3((void *)(uintptr_t)pro_buf,
                        (void *)(uintptr_t)e->code, &env);
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
                if (env.eip == iret_next && g_smc_dirty) {
                    smc_flush_local_shard(&prev_slot);
                }
            }
            int is_hlt = 0;
            {
                LatxX86Translation tr_hlt;
                LoongemuMemLookup lk_hlt;
                uint64_t hlt_lin = current_tb_cs_base() + env.eip;
                if (walk_ok(latx_x86_walk_cached(&env, hlt_lin,
                                                 LATX_X86_ACCESS_READ,
                                                 &tr_hlt))) {
                    LoongemuMemKind mk_hlt = loongemu_mem_resolve(
                        &g_mem_map, tr_hlt.gpa, 1u, &lk_hlt);
                    if ((mk_hlt == LOONGEMU_MEM_RAM ||
                         mk_hlt == LOONGEMU_MEM_ROM) &&
                        *(const volatile uint8_t *)(uintptr_t)lk_hlt.hpa == 0xf4u) {
                        is_hlt = 1;
                    }
                }
            }
            uint64_t irq_rip = is_hlt ? (env.eip + 1) : env.eip;
            if (g_vcpu_nmi_pending[pcpu] && env.idt.base != 0) {
                g_vcpu_nmi_pending[pcpu] = 0;
                if (deliver_x86_exception(2u, irq_rip, 0u, 0)) {
                    g_vcpu_halted[pcpu] = 0;
                    g_vcpu_kicked[pcpu] = 0;
                    prev_slot = -1;
                    continue;
                }
            }
            if ((env.eflags & IF_MASK) != 0) {
                loongemu_spin_lock(&g_lapic_lock);
                uint32_t ipi_vec = lapic_pop_irr(pcpu);
                loongemu_spin_unlock(&g_lapic_lock);
                if (ipi_vec != 0 && deliver_kernel_irq(ipi_vec, irq_rip)) {
                    g_vcpu_halted[pcpu] = 0;
                    g_vcpu_kicked[pcpu] = 0;
                    prev_slot = -1;
                    continue;
                }
            }
            if (g_timer_irq_pending[pcpu]) {
                if (deliver_kernel_timer_tick(irq_rip)) {
                    g_timer_irq_pending[pcpu] = 0;
                    g_vcpu_halted[pcpu] = 0;
                    g_vcpu_kicked[pcpu] = 0;
                    timer_start(timer_ticks_1ms());
                    prev_slot = -1;
                    continue;
                }
            }
            if (is_hlt) {
                int maintenance_only = 0;
                /* KVM PV_UNHALT: consume pending kick or wait (handles both IF=0 and IF=1 HLT). */
                if (__atomic_exchange_n(&g_vcpu_kicked[pcpu], 0, __ATOMIC_ACQ_REL)) {
                    g_vcpu_halted[pcpu] = 0;
                    env.eip = env.eip + 1;
                    prev_slot = -1;
                    continue;
                }
                g_vcpu_halted[pcpu] = 1;
                for (int w = 0; w < 2000; w++) {
                    if (hv_recycle_tb_shard()) {
                        prev_slot = -1;
                    }
                    if (hv_service_flush_requests()) {
                        prev_slot = -1;
                        maintenance_only = 1;
                    }
                    if (g_vcpu_nmi_pending[pcpu] ||
                        __atomic_load_n(&g_vcpu_kicked[pcpu], __ATOMIC_ACQUIRE) ||
                        ((env.eflags & IF_MASK) != 0 && (lapic_has_irr(pcpu) || g_timer_irq_pending[pcpu])) ||
                        !g_vcpu_halted[pcpu]) {
                        break;
                    }
                    uint32_t ipi = loongemu_ipi_status();
                    if (ipi != 0u) {
                        loongemu_ipi_clear(ipi);
                        if ((ipi & (LOONGEMU_IPI_TLB_SHOOTDOWN |
                                    LOONGEMU_IPI_SMC_BROADCAST)) != 0u &&
                            (ipi & LOONGEMU_IPI_WAKE) == 0u) {
                            maintenance_only = 1;
                        }
                        if (hv_service_flush_requests()) {
                            prev_slot = -1;
                            maintenance_only = 1;
                        }
                        break;
                    }
                    __asm__ volatile("dbar 0x700" ::: "memory");
                }
                if (__atomic_exchange_n(&g_vcpu_kicked[pcpu], 0, __ATOMIC_ACQ_REL) ||
                    ((env.eflags & IF_MASK) == 0 && !maintenance_only)) {
                    g_vcpu_halted[pcpu] = 0;
                    if (!g_vcpu_nmi_pending[pcpu]) {
                        env.eip = env.eip + 1;
                        prev_slot = -1;
                    }
                } else if (!maintenance_only &&
                           !g_vcpu_nmi_pending[pcpu] && !lapic_has_irr(pcpu)) {
                    if (deliver_kernel_timer_tick(env.eip + 1)) {
                        g_timer_irq_pending[pcpu] = 0;
                        timer_ack();
                        timer_start(timer_ticks_1ms());
                        g_vcpu_halted[pcpu] = 0;
                        prev_slot = -1;
                    }
                }
            }
        } else if (g_plv3_exit.reason == PLV3_EXIT_TRAP) {
            static volatile int ap_trap_logged[MAX_VCPUS];
            if (!ap_trap_logged[pcpu]) {
                ap_trap_logged[pcpu] = 1;
                latx_bare_uart_puts("tb-sys: ap_trap cpu=");
                puthex(pcpu);
                latx_bare_uart_puts(" ecode=");
                puthex(g_plv3_exit.ecode);
                latx_bare_uart_puts(" era=");
                puthex(g_plv3_exit.era);
                latx_bare_uart_puts(" badv=");
                puthex(g_plv3_exit.badv);
                latx_bare_uart_puts(" rip=");
                puthex(env.eip);
                latx_bare_uart_puts("\n");
            }
        }
    }
}

void ap_main(uint32_t pcpu)
{
    uint64_t euen, crmd, ecfg;
    uint32_t cfg2 = 0;

    __asm__ volatile("cpucfg %0, %1" : "=r"(cfg2) : "r"(2));
    euen = csrrd(0x2);
    euen |= 0x1 | 0x2 | 0x40 | 0x80;
    if ((cfg2 & (1u << 7)) != 0) {
        euen |= 0x4;
    }
    csrwr(0x2, euen);
    crmd = csrrd(0x0);
    crmd &= ~(uint64_t)0x4;
    csrwr(0x0, crmd);

    csrwr(0x1c, lapg_pwcl_value());
    csrwr(0x1d, lapg_pwch_value());
    csrwr(0x1e, lapg_stlbps_value());
    ecfg = csrrd(0x4) & ~(UINT64_C(0x7) << 16);
    ecfg |= (UINT64_C(1) << IRQ_TIMER) | (UINT64_C(1) << LOONGEMU_IRQ_IPI);
    csrwr(0x4, ecfg);
    csrwr(0xc, (uint64_t)(uintptr_t)tb_sys_trap);
    csrwr(0x88, (uint64_t)(uintptr_t)tb_sys_trap);
    csrwr(0x180, ((uint64_t)0 << 60) | (UINT64_C(1) << 4) | 1u);
    /* AP firmware state is not trusted; leave only the PLV0 window. */
    csrwr(0x181, 0);
    csrwr(0x182, 0);
    csrwr(0x183, 0);
    crmd = (csrrd(0x0) & ~UINT64_C(0x1f)) | (UINT64_C(1) << 4);
    csrwr(0x0, crmd);
    __asm__ volatile("invtlb 0x0, $zero, $zero" ::: "memory");
    loongemu_ipi_enable(0xffffffffu);
    loongemu_ipi_clear(0xffffffffu);

    g_pcpu_vcpu[pcpu] = pcpu;
    g_shard[pcpu].next_asid = 1;
    asid_activate(1);
    __atomic_store_n(&g_pcpu_online[pcpu], 1u, __ATOMIC_RELEASE);

    while (!__atomic_load_n(&g_vcpu_online[pcpu], __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&g_vp_owner[pcpu], __ATOMIC_ACQUIRE) != pcpu) {
        (void)hv_service_flush_requests();
        (void)hv_recycle_tb_shard();
        uint32_t ipi = loongemu_ipi_status();
        if (ipi != 0u) {
            loongemu_ipi_clear(ipi);
            if (__atomic_exchange_n(&g_shard[pcpu].tlb_shootdown_pending, 0,
                                    __ATOMIC_ACQ_REL)) {
                __asm__ volatile("invtlb 0x3, $zero, $zero" ::: "memory");
                latx_x86_tlb_flush_all();
                memset(g_asid_slots, 0, sizeof(g_asid_slots));
                g_next_asid = 1;
                asid_activate(1);
                __atomic_fetch_add(&g_ipi_shootdowns_handled, 1u, __ATOMIC_RELAXED);
            }
        }
        __asm__ volatile("dbar 0x700" ::: "memory");
    }
    run_ap_vcpu_loop(pcpu);
}

static void boot_physical_aps(void)
{
    uint32_t online = 1u;
    g_pcpu_online[0] = 1u;
    for (uint32_t c = 1u; c < MAX_VCPUS; c++) {
        g_pcpu_online[c] = 0u;
        loongemu_boot_secondary_core(c, (uint64_t)(uintptr_t)_start_ap, (uint64_t)c);
    }
    uint64_t t0 = vclock_read_host_rdtime(0);
    while ((vclock_read_host_rdtime(0) - t0) < 2000000ULL) {
        (void)hv_service_flush_requests();
        (void)hv_recycle_tb_shard();
        uint32_t cnt = 1u;
        for (uint32_t c = 1u; c < MAX_VCPUS; c++) {
            if (__atomic_load_n(&g_pcpu_online[c], __ATOMIC_ACQUIRE)) {
                cnt++;
            }
        }
        online = cnt;
        if (online == MAX_VCPUS) {
            break;
        }
        __asm__ volatile("dbar 0x700" ::: "memory");
    }
    g_phys_cpus_online = online;
    latx_bare_uart_puts("tb-sys: phys_cpus_online=");
    puthex((uint64_t)online);
    latx_bare_uart_puts("\n");
    if (online > 1u) {
        uint64_t before = __atomic_load_n(&g_ipi_shootdowns_handled, __ATOMIC_ACQUIRE);
        uint64_t expect = before + (uint64_t)(online - 1u);
        latx_platform_tlb_shootdown_all();
        uint64_t ts = vclock_read_host_rdtime(0);
        while ((vclock_read_host_rdtime(0) - ts) < 2000000ULL) {
            (void)hv_service_flush_requests();
            (void)hv_recycle_tb_shard();
            if (__atomic_load_n(&g_ipi_shootdowns_handled, __ATOMIC_ACQUIRE) >= expect) {
                break;
            }
            __asm__ volatile("dbar 0x700" ::: "memory");
        }
        uint64_t after = __atomic_load_n(&g_ipi_shootdowns_handled, __ATOMIC_ACQUIRE);
        check(after >= expect, "physical SMP: remote cores acknowledged TLB shootdown IPI");
        latx_bare_uart_puts("tb-sys: ipi_shootdowns_ack=");
        puthex(after - before);
        latx_bare_uart_puts("\n");
    }
}

static int run_blocks_from(uint64_t pc, int max_blocks, uint64_t *end_pc,
                           int reset, int keep_running)
{
    int ran = 0;
    int stuck = 0;
    uint64_t last_fp = 0;
    int32_t prev_slot = -1;

    g_run_blocks_exit = RUN_EXIT_MAX_BLOCKS;
    if (reset) {
        env.eip = pc;
        env.tb_jmp_cache_ptr = 0;
        g_recent_count = 0;
        g_run_tb_hits = 0;
        g_run_tb_misses = 0;
        g_run_tb_flushes = 0;
        g_sync_core_calls = 0;
        g_sync_core_flushes = 0;
        tbc_flush_all(&tb_cache);
        g_tb_highwater = 0;
        g_tb_by_code_count = 0;
        memset(g_code_page_bitmap, 0, sizeof(g_code_page_bitmap));
        g_smc_dirty = 0;
        g_smc_dirty_count = 0;
        g_smc_dirty_overflow = 0;
        g_smc_granular_evicts = 0;
        memset(g_fast_jmp_cache, 0, sizeof(g_fast_jmp_cache));
        code_used = 0x2000;
        __asm__ volatile("ibar 0" ::: "memory");
        latx_x86_tlb_flush_all();
        memset((void *)&g_plv3_exit, 0, sizeof(g_plv3_exit));
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
        timer_start(timer_ticks_1ms());
    }

    while (ran < max_blocks) {
        if (hv_service_flush_requests()) {
            prev_slot = -1;
        }
        hv_try_transfer_ap_ownership();
        int bar_map_status = bochs_process_pending_bar_maps();
        if (bar_map_status != 0) {
            prev_slot = -1;
        }
        if (bar_map_status < 0) {
            continue;
        }
        if (hv_recycle_tb_shard()) {
            prev_slot = -1;
        }
        if (g_smc_dirty) {
            smc_flush_local_shard(&prev_slot);
        }
        for (uint32_t t = 1; t < MAX_VCPUS; t++) {
            if (g_vcpu_sipi_pending[t]) {
                g_vcpu_sipi_pending[t] = 0;
                if (start_ap_from_sipi(t, g_vcpu_sipi_vec[t])) {
                    if (__atomic_load_n(&g_pcpu_online[t], __ATOMIC_ACQUIRE)) {
                        __atomic_store_n(&g_vcpu_online[t], 1, __ATOMIC_RELEASE);
                        loongemu_ipi_send(t, 0u);
                    } else {
                        /* BSP executes the VP until its AP owns it. */
                        __atomic_store_n(&g_vcpu_online[t], 1, __ATOMIC_RELEASE);
                        switch_vcpu(t);
                        prev_slot = -1;
                        stuck = 0;
                    }
                }
            }
        }
        uint64_t before = env.eip;
        uint64_t tb_cs_base = current_tb_cs_base();
        uint8_t tb_mode = current_tb_mode_flags();
        uint64_t fetch_lin = tb_cs_base + before;
        uint8_t cpl = (uint8_t)(env.hflags & HF_CPL_MASK);
        uint64_t tb_cr3 = (cpl == 0 && (fetch_lin >> 63) != 0) ? 0 : env.cr[3];
        TbCacheEntry *e;
        uint32_t slot_idx;

        g_recent_pcs[g_recent_count & 15] = before;
        g_recent_count++;
#ifdef LOONGEMU_BOOT_OVMF
        if (before < 0x10000ULL && tb_cs_base == 0ULL && env.cr[3] >= 0x01000000ULL) {
            g_run_blocks_exit = 0xdeadULL;
            break;
        }
        if (ran > 0 && (ran & 0x3fffff) == 0) {
            latx_bare_uart_puts("ovmf_hb: blk=");
            puthex((uint64_t)ran);
            latx_bare_uart_puts(" rip=");
            puthex(before);
            latx_bare_uart_puts(" cr3=");
            puthex(env.cr[3]);
            latx_bare_uart_puts(" gdt=");
            puthex(env.gdt.base);
            latx_bare_uart_puts(" pci_w=");
            puthex(g_pci_config_writes);
            latx_bare_uart_puts("\n");
        }
        if (g_ovmf_uniq_count == 0 || g_ovmf_uniq_pcs[(g_ovmf_uniq_count - 1) & 15] != before) {
            g_ovmf_uniq_pcs[g_ovmf_uniq_count & 15] = before;
            g_ovmf_uniq_cs[g_ovmf_uniq_count & 15] = (uint16_t)env.segs[R_CS].selector;
            g_ovmf_uniq_count++;
        }
#endif

        e = tbc_lookup_mode(&tb_cache, before, tb_cs_base, tb_cr3, cpl, tb_mode);
        if (e != 0) {
            g_run_tb_hits++;
        } else {
            uint8_t *code;
            int rc;
            uint32_t fetch_acc = LATX_X86_ACCESS_EXEC |
                                 (cpl ? LATX_X86_ACCESS_USER : 0);
            LatxX86Translation tr_fetch;
            LatxX86WalkResult fr = latx_x86_walk(&env, fetch_lin, fetch_acc,
                                                 &tr_fetch);
            if (fr != LATX_X86_WALK_OK && fr != LATX_X86_WALK_PAGING_OFF) {
                latx_x86_tlb_flush_page(fetch_lin);
                env.cr[2] = fetch_lin;
                env.exception_index = EXCP0E_PAGE;
                env.error_code = (int)(tr_fetch.fault_error_code
                                           ? tr_fetch.fault_error_code
                                           : latx_x86_fault_error_code(&env, fr,
                                                                       fetch_acc));
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
                tbc_count(&tb_cache) >= TB_TABLE_CAP - 1 ||
                g_tb_by_code_count >= TB_TABLE_CAP - 1) {
                tbc_flush_all(&tb_cache);
                g_tb_highwater = 0;
                g_tb_by_code_count = 0;
                prev_slot = -1;
                memset(g_code_page_bitmap, 0, sizeof(g_code_page_bitmap));
                g_smc_dirty = 0;
                g_smc_dirty_count = 0;
                g_smc_dirty_overflow = 0;
                memset(g_fast_jmp_cache, 0, sizeof(g_fast_jmp_cache));
                code_used = 0x2000;
                g_run_tb_flushes++;
                __asm__ volatile("ibar 0" ::: "memory");
            }
            e = tbc_insert_mode(&tb_cache, before, tb_cs_base, tb_cr3, cpl, tb_mode);
            if (e == 0) {
                tbc_flush_all(&tb_cache);
                g_tb_highwater = 0;
                g_tb_by_code_count = 0;
                prev_slot = -1;
                memset(g_code_page_bitmap, 0, sizeof(g_code_page_bitmap));
                g_smc_dirty = 0;
                g_smc_dirty_count = 0;
                g_smc_dirty_overflow = 0;
                memset(g_fast_jmp_cache, 0, sizeof(g_fast_jmp_cache));
                code_used = 0x2000;
                g_run_tb_flushes++;
                __asm__ volatile("ibar 0" ::: "memory");
                e = tbc_insert_mode(&tb_cache, before, tb_cs_base, tb_cr3, cpl, tb_mode);
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
            rc = translate_tb_locked(
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
                g_tb_by_code_count = 0;
                prev_slot = -1;
                memset(g_code_page_bitmap, 0, sizeof(g_code_page_bitmap));
                g_smc_dirty = 0;
                g_smc_dirty_count = 0;
                g_smc_dirty_overflow = 0;
                memset(g_fast_jmp_cache, 0, sizeof(g_fast_jmp_cache));
                g_run_blocks_exit = RUN_EXIT_TRANSLATE_FAIL;
                break;
            }
            e->code = (uint64_t)(uintptr_t)code;
            e->code_size = (uint32_t)rc;
            if (g_tb_by_code_count < TB_TABLE_CAP) {
                g_tb_by_code[g_tb_by_code_count].code_start = e->code;
                g_tb_by_code[g_tb_by_code_count].code_end = e->code + (uint64_t)e->code_size;
                g_tb_by_code[g_tb_by_code_count].slot_idx = slot_idx;
                g_tb_by_code_count++;
            }
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
                e->guest_size = clen;
                tb_code_hash[slot_idx] = hash_guest_code(fetch_lin, clen);
                mark_code_pages_ro(slot_idx, fetch_lin, clen);
            }
            code_used += (((uint64_t)rc + 255) & ~UINT64_C(255));
            __asm__ volatile("ibar 0" ::: "memory");
        }


        /* Publish this block before running it: the fault path resolves the
         * faulting guest RIP through the executing block's map. */
        g_current_tb_code = e->code;
        g_current_tb_size = e->code_size;
        g_current_tb_pc = before;
        g_current_tb_map = &tb_insn_maps[slot_idx];
        if (hv_service_flush_requests()) {
            prev_slot = -1;
            continue;
        }
        if (hv_wait_for_barrier_release()) {
            prev_slot = -1;
            continue;
        }

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
                        smc_flush_local_shard(&prev_slot);
                    }
                }
            }
            int is_hlt = 0;
            {
                LatxX86Translation tr_hlt;
                LoongemuMemLookup lk_hlt;
                uint64_t hlt_lin = current_tb_cs_base() + env.eip;
                if (walk_ok(latx_x86_walk_cached(&env, hlt_lin,
                                                 LATX_X86_ACCESS_READ,
                                                 &tr_hlt))) {
                    LoongemuMemKind mk_hlt = loongemu_mem_resolve(&g_mem_map, tr_hlt.gpa, 1u, &lk_hlt);
                    if ((mk_hlt == LOONGEMU_MEM_RAM || mk_hlt == LOONGEMU_MEM_ROM) &&
                        *(const volatile uint8_t *)(uintptr_t)lk_hlt.hpa == 0xf4u) {
                        is_hlt = 1;
                    }
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
                loongemu_spin_lock(&g_lapic_lock);
                uint32_t ipi_vec = lapic_pop_irr(g_cur_vcpu);
                loongemu_spin_unlock(&g_lapic_lock);
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
                    timer_start(timer_ticks_1ms());
                    stuck = 0;
                    prev_slot = -1;
                    continue;
                }
            }
            if (is_hlt && (env.eflags & IF_MASK) == 0) {
                if (__atomic_exchange_n(&g_vcpu_kicked[g_cur_vcpu], 0, __ATOMIC_ACQ_REL)) {
                    g_vcpu_halted[g_cur_vcpu] = 0;
                    env.eip = env.eip + 1;
                    stuck = 0;
                    prev_slot = -1;
                    continue;
                }
                g_vcpu_halted[g_cur_vcpu] = 1;
                for (int w = 0; w < 2000; w++) {
                    if (g_vcpu_nmi_pending[g_cur_vcpu] ||
                        __atomic_load_n(&g_vcpu_kicked[g_cur_vcpu], __ATOMIC_ACQUIRE) ||
                        !g_vcpu_halted[g_cur_vcpu]) {
                        break;
                    }
                    __asm__ volatile("dbar 0x700" ::: "memory");
                }
                __atomic_store_n(&g_vcpu_kicked[g_cur_vcpu], 0, __ATOMIC_RELEASE);
                g_vcpu_halted[g_cur_vcpu] = 0;
                if (!g_vcpu_nmi_pending[g_cur_vcpu]) {
                    env.eip = env.eip + 1;
                    stuck = 0;
                    prev_slot = -1;
                    continue;
                }
            }
            if (is_hlt && (env.eflags & IF_MASK) != 0) {
                if (__atomic_exchange_n(&g_vcpu_kicked[g_cur_vcpu], 0, __ATOMIC_ACQ_REL)) {
                    g_vcpu_halted[g_cur_vcpu] = 0;
                    env.eip = env.eip + 1;
                    stuck = 0;
                    prev_slot = -1;
                    continue;
                }
                if (MAX_VCPUS > 1u) {
                    uint32_t target = MAX_VCPUS;
                    for (uint32_t step = 1; step < MAX_VCPUS; step++) {
                        uint32_t c = (g_cur_vcpu + step) % MAX_VCPUS;
                        int urg = g_vcpu_nmi_pending[c] ||
                                  (lapic_has_irr(c) &&
                                   (g_vcpu_env[c].eflags & IF_MASK) != 0) ||
                                  (g_timer_irq_pending[c] &&
                                   (g_vcpu_env[c].eflags & IF_MASK) != 0) ||
                                  (com1_irq_dest_match(c) && com1_irq_pending() &&
                                   (g_vcpu_env[c].eflags & IF_MASK) != 0);
                        if (g_vcpu_online[c] && hv_vp_owner_pcpu(c) == 0 &&
                            (!g_vcpu_halted[c] || urg)) {
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
                    timer_start(timer_ticks_1ms());
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
                    if (g_vcpu_online[c] && hv_vp_owner_pcpu(c) == 0 &&
                        (g_vcpu_nmi_pending[c] ||
                         (lapic_has_irr(c) &&
                          (g_vcpu_env[c].eflags & IF_MASK) != 0))) {
                        target = c;
                        break;
                    }
                }
                if (target == MAX_VCPUS && (g_vcpu_preempt_needed || (ran & 127) == 0)) {
                    g_vcpu_preempt_needed = 0;
                    for (uint32_t step = 1; step < MAX_VCPUS; step++) {
                        uint32_t c = (g_cur_vcpu + step) % MAX_VCPUS;
                        if (g_vcpu_online[c] && hv_vp_owner_pcpu(c) == 0 && !g_vcpu_halted[c]) {
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
    (void)hv_service_flush_requests();
    (void)hv_recycle_tb_shard();
    while (bochs_process_pending_bar_maps() < 0) {
        (void)hv_service_flush_requests();
    }

    if (!keep_running) {
        timer_stop();
        timer_ack();
        g_plv3_timer_preempt = 0;
        for (uint32_t i = 0; i < MAX_VCPUS; i++) {
            g_timer_irq_pending[i] = 0;
        }
        env.tb_jmp_cache_ptr = 0;
    }
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

    *(volatile uint8_t *)gpa_to_host_ptr(HLT_PC_TEST) = 0xf4;
    *(volatile uint32_t *)gpa_to_host_ptr(TICK_COUNTER_PC) = 0;

    /* The ISR must be in place: this test is about the loop, not the gate. */
    {
        static const uint8_t isr_code[] = {
            0xff, 0x04, 0x25, 0x00, 0xb1, 0x00, 0x00,  /* inc dword [0xb100] */
            0x48, 0xcf,                                 /* iretq             */
        };
        uint64_t gate_lo, gate_hi;

        memcpy(gpa_to_host_ptr(TICK_ISR_PC), isr_code, sizeof(isr_code));
        gate_lo = ((uint64_t)TICK_ISR_PC & 0xffffULL) |
                  ((uint64_t)0x08 << 16) |
                  ((uint64_t)0x8e << 40) |
                  (((((uint64_t)TICK_ISR_PC) >> 16) & 0xffffULL) << 48);
        gate_hi = ((uint64_t)TICK_ISR_PC >> 32) & 0xffffffffULL;
        *(volatile uint64_t *)gpa_to_host_ptr(TICK_IDT_PC + TICK_VECTOR * 16) =
            gate_lo;
        *(volatile uint64_t *)gpa_to_host_ptr(TICK_IDT_PC +
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

    *(volatile uint8_t *)gpa_to_host_ptr(HLT_PC_TEST) = 0xf4;

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

    *(volatile uint32_t *)gpa_to_host_ptr(TICK_COUNTER_PC) = 0;
    memcpy(gpa_to_host_ptr(TICK_ISR_PC), isr_code, sizeof(isr_code));

    gate_lo = ((uint64_t)TICK_ISR_PC & 0xffffULL) |
              ((uint64_t)0x08 << 16) |
              ((uint64_t)0x8e << 40) |
              (((((uint64_t)TICK_ISR_PC) >> 16) & 0xffffULL) << 48);
    gate_hi = ((uint64_t)TICK_ISR_PC >> 32) & 0xffffffffULL;
    *(volatile uint64_t *)gpa_to_host_ptr(TICK_IDT_PC + TICK_VECTOR * 16) = gate_lo;
    *(volatile uint64_t *)gpa_to_host_ptr(TICK_IDT_PC + TICK_VECTOR * 16 + 8) = gate_hi;

    /*
     * The body is a one-instruction spin at its own address, so "the guest
     * resumed" is a single comparison against a known RIP rather than a
     * function of anything the frame pushed.
     */
    {
        uint8_t spin[2];

        spin[0] = 0xeb;                       /* jmp $ */
        spin[1] = 0xfe;
        memcpy(gpa_to_host_ptr(TICK_SPIN_PC), spin, sizeof(spin));
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


/* g_active_helper_* are per-physical-core fields inside g_shard[cur_pcpu()]. */

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

#ifdef LOONGEMU_BOOT_OVMF
    if (g_ovmf_exc_logs < 3u) {
        g_ovmf_exc_logs++;
        latx_bare_uart_puts("ovmf_exc: vec=");
        puthex((uint64_t)vec);
        latx_bare_uart_puts(" rip=");
        puthex(push_rip);
        latx_bare_uart_puts(" err=");
        puthex((uint64_t)error_code);
        latx_bare_uart_puts(" ecx=");
        puthex(env.regs[R_ECX]);
        latx_bare_uart_puts(" eax=");
        puthex(env.regs[R_EAX]);
        latx_bare_uart_puts(" idt=");
        puthex(idt_base);
        latx_bare_uart_puts(" bytes=");
        {
            LatxX86Translation tr_b;
            if (walk_ok(latx_x86_walk(&env, push_rip, LATX_X86_ACCESS_READ, &tr_b))) {
                LoongemuMemLookup lk_b;
                LoongemuMemKind mk_b = loongemu_mem_resolve(&g_mem_map, tr_b.gpa, 8u, &lk_b);
                if (mk_b == LOONGEMU_MEM_RAM || mk_b == LOONGEMU_MEM_ROM) {
                    const uint8_t *bp = (const uint8_t *)(uintptr_t)lk_b.hpa;
                    uint64_t qw = 0;
                    for (int bi = 0; bi < 8; bi++) qw |= ((uint64_t)bp[bi]) << (bi * 8);
                    puthex(qw);
                }
            }
        }
        latx_bare_uart_puts("\n");
    }
#endif
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
    extern void helper_raise_int(void);
    CPUState *eff_cs = (cs != 0) ? cs : env_cpu(cur_env);
    int vec = (eff_cs != 0 && eff_cs->exception_index >= 0) ? eff_cs->exception_index
                                                            : env.exception_index;
    int is_int = env.exception_is_int ||
                 (g_active_helper_fn == (uint64_t)(uintptr_t)helper_raise_int) ||
                 (vec == 3);
    uint32_t err = (uint32_t)env.error_code;
    uint64_t fault_rip = resolve_fault_x86_pc(g_active_helper_ra);
    uint64_t push_rip = is_int
                            ? (env.exception_next_eip ? env.exception_next_eip
                                                      : (fault_rip + 1))
                            : fault_rip;
    int has_err = !is_int &&
                  (vec == 8 || vec == 10 || vec == 11 || vec == 12 ||
                   vec == 13 || vec == 14 || vec == 17 || vec == 21);
    uint64_t tlbrera = csrrd(0x8a);
    int is_refill = (int)(tlbrera & 1u);

    if (eff_cs != 0) {
        eff_cs->exception_index = -1;
    }
    env.exception_index = -1;
    env.exception_is_int = 0;
    env.exception_next_eip = 0;
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
        found_slot = find_tb_slot_by_code_pc(fault_pc);
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
            memset(fast_jmp_cache, 0, sizeof(g_fast_jmp_cache[0]));
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
        g_sw_lbt_traps++;
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

        if (isr & (1u << LOONGEMU_IRQ_IPI)) {
            uint32_t ipi = loongemu_ipi_status();
            if (ipi != 0u) {
                loongemu_ipi_clear(ipi);
            }
            if (__atomic_exchange_n(&cur_shard->tlb_shootdown_pending, 0,
                                    __ATOMIC_ACQ_REL)) {
                __asm__ volatile("invtlb 0x3, $zero, $zero" ::: "memory");
                latx_x86_tlb_flush_all();
                memset(g_asid_slots, 0, sizeof(g_asid_slots));
                g_next_asid = 1;
                asid_activate(1);
                __atomic_fetch_add(&g_ipi_shootdowns_handled, 1u, __ATOMIC_RELAXED);
            }
            if (ipi & LOONGEMU_IPI_SMC_BROADCAST) {
                __atomic_fetch_add(&g_ipi_smc_handled, 1u, __ATOMIC_RELAXED);
                unlink_tb_at_pc(fault_pc);
            }
            if (ipi & LOONGEMU_IPI_WAKE) {
                unlink_tb_at_pc(fault_pc);
            }
            if ((isr & (1u << IRQ_TIMER)) == 0) {
                return;
            }
        }

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
        int abort_after_bar_map;

        g_active_helper_fn = badv;
        g_active_helper_a1 = regs[la_a1];
        g_active_helper_a2 = regs[la_a2];
        g_active_helper_ra = regs[la_ra];
        g_active_helper_bar_pending = 0;
        uint64_t ret = fn(regs[la_a0], regs[la_a1], regs[la_a2],
                          regs[la_a3], regs[la_a4]);
        abort_after_bar_map = bochs_helper_requires_tb_exit();
        g_active_helper_bar_pending = 0;
        g_active_helper_fn = 0;
        regs[la_a0] = ret;
        if (abort_after_bar_map) {
            const TbInsnMap *map = g_current_tb_map;
            uint64_t code_base = plv3_code_pc_to_phys(g_current_tb_code);
            uint64_t return_pc = plv3_code_pc_to_phys(g_active_helper_ra);
            uint64_t fault_x86_pc = 0;
            uint8_t ea_seg_class = IR1_EA_SEG_UNKNOWN;
            int mapped = 0;

            if (map != 0) {
                mapped = tb_harness_resolve_x86_fault(
                    map, code_base, return_pc, g_current_tb_pc,
                    &fault_x86_pc, &ea_seg_class);
            }
            if (!mapped) {
                int slot = find_tb_slot_by_code_pc(return_pc);
                if (slot >= 0) {
                    map = &tb_insn_maps[slot];
                    code_base = tb_entries[slot].code;
                    mapped = tb_harness_resolve_x86_fault(
                        map, code_base, return_pc, tb_entries[slot].pc,
                        &fault_x86_pc, &ea_seg_class);
                }
            }
            const TbInsnSpan *fault_span = 0;
            if (mapped && map != 0) {
                for (uint32_t i = 0; i < map->count; i++) {
                    if (map->spans[i].x86_pc == fault_x86_pc &&
                        map->spans[i].x86_size != 0) {
                        fault_span = &map->spans[i];
                        break;
                    }
                }
            }
            if (fault_span == 0) {
                latx_platform_refuse("Bochs BAR helper lacks a mapped x86 span");
            }
            env.eip = fault_span->x86_pc + fault_span->x86_size;
            sync_env_to_trap_regs(regs);
            g_plv3_exit.reason = PLV3_EXIT_NORMAL;
            g_plv3_exit.ecode = 0;
            g_plv3_exit.esubcode = 0;
            g_plv3_exit.is_refill = 0;
            g_plv3_exit.era = return_pc;
            g_plv3_exit.badv = 0;
            g_plv3_exit.guest_rip = env.eip;
            if (is_refill) {
                csrwr(0x8a,
                      ((uint64_t)(uintptr_t)tb_sys_plv3_done &
                       ~UINT64_C(3)) | 1u);
            } else {
                csrwr(0x6, (uint64_t)(uintptr_t)tb_sys_plv3_done);
            }
            return;
        }
        if (is_refill) {
            /* Keep TLBRERA.ISTLBR=1 so ERTN returns to the PLV3 caller. */
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
        /*
         * Host control mappings share the PLV3 virtual space with guest
         * addresses.  This path is not a security boundary: a warm TLB entry
         * for a host page is reached before any fault, so the trap predicate
         * cannot distinguish a guest access from a host helper access.  See
         * README Known limitations; untrusted guests are unsupported until
         * translator-side sandboxing or a separate hardware context exists.
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
                if (mmio_find_region(x86_tr.gpa) != 0 &&
                    emulate_mmio_insn(fault_pc, x86_tr.gpa, regs, is_refill)) {
                    return;
                }
                uint64_t fault_pte = build_guest_leaf_pte((uint32_t)x86_r,
                                                          &x86_tr, x86_access);
                if (fault_pte != 0) {
                    fill_hw_tlb_guest(badv, fault_pte, is_refill);
                    return;
                }
                x86_r = LATX_X86_WALK_NOT_PRESENT;
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
            env.error_code = (int)(x86_tr.fault_error_code
                                       ? x86_tr.fault_error_code
                                       : latx_x86_fault_error_code(&env, x86_r,
                                                                   x86_access));
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
     * 3d. Non-canonical guest address / address error (EXC_ADE = 0x8):
     *     Classify by fault source before injecting guest exceptions:
     *     - Fault outside active translated TB or on a canonical address is a
     *       Host/DBT internal address error (report PLV3_EXIT_TRAP).
     *     - Non-canonical access via SS segment / stack (RSP/RBP base, 0x36
     *       override, push/pop/call/ret/leave/enter) injects #SS(0) (vec 12).
     *     - All other guest non-canonical accesses inject #GP(0) (vec 13).
     */
    if (!is_refill && ecode == EXC_ADE) {
        uint64_t code_pc = plv3_code_pc_to_phys(fault_pc);
        uint64_t current_code = plv3_code_pc_to_phys(g_current_tb_code);
        uint64_t fault_code_base = 0;
        const TbInsnMap *fault_map = 0;
        int in_tb = (g_current_tb_code != 0 &&
                     code_pc >= current_code &&
                     code_pc < current_code + (uint64_t)g_current_tb_size);
        int mapped_insn = 0;
        uint8_t ea_seg_class = IR1_EA_SEG_UNKNOWN;
        uint64_t fault_x86_pc = env.eip;
        int non_canonical = (((int64_t)(badv << 16) >> 16) != (int64_t)badv);

        sync_trap_regs_to_env(regs);
        if (in_tb && g_current_tb_map != 0) {
            fault_map = g_current_tb_map;
            fault_code_base = current_code;
            mapped_insn = tb_harness_resolve_x86_fault(
                fault_map, fault_code_base, code_pc,
                g_current_tb_pc, &fault_x86_pc, &ea_seg_class);
        } else {
            int slot = find_tb_slot_by_code_pc(code_pc);
            if (slot >= 0) {
                fault_map = &tb_insn_maps[slot];
                fault_code_base = tb_entries[slot].code;
                mapped_insn = tb_harness_resolve_x86_fault(
                    fault_map, fault_code_base, code_pc,
                    tb_entries[slot].pc, &fault_x86_pc, &ea_seg_class);
                in_tb = mapped_insn;
            }
        }
        if (mapped_insn && ea_seg_class == IR1_EA_SEG_UNKNOWN &&
            (env.cr[0] & CR0_PG_MASK) == 0) {
            (void)tb_harness_refine_x86_fault_ea(
                fault_map, fault_code_base, code_pc, badv, env.regs,
                env.segs[R_FS].base, env.segs[R_GS].base, &ea_seg_class);
        }
        env.eip = mapped_insn ? fault_x86_pc : resolve_fault_x86_pc(fault_pc);
        g_plv3_exit.ecode = ecode;
        g_plv3_exit.esubcode = (estat >> 22) & 0x1ffu;
        g_plv3_exit.is_refill = 0;
        g_plv3_exit.era = fault_pc;
        g_plv3_exit.badv = badv;

        if (in_tb && non_canonical && mapped_insn &&
            (ea_seg_class == IR1_EA_SEG_SS ||
             ea_seg_class == IR1_EA_SEG_NOT_SS)) {
            uint32_t exc_vec = ea_seg_class == IR1_EA_SEG_SS ? 12u : 13u;
            g_hw_guest_gp++;
            if (deliver_x86_exception(exc_vec, env.eip, 0u, 1)) {
                g_plv3_exit.reason = PLV3_EXIT_GUEST_PF;
                g_plv3_exit.guest_rip = env.eip;
                sync_env_to_trap_regs(regs);
            } else {
                g_plv3_exit.reason = PLV3_EXIT_TRAP;
                g_plv3_exit.guest_rip = env.eip;
            }
        } else {
            /* Unknown metadata is not enough evidence for #SS or #GP. */
            g_plv3_exit.reason = PLV3_EXIT_TRAP;
            g_plv3_exit.guest_rip = env.eip;
        }
        {
            uint64_t prmd = csrrd(0x1) & ~UINT64_C(0x7);
            csrwr(0x1, prmd);
            csrwr(0x6, (uint64_t)(uintptr_t)tb_sys_abort_plv0);
        }
        return;
    }

    /*
     * 4. Unhandled PLV3 trap (e.g. EXC_ALE=0x9, EXC_SYS=0xb,
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
uint64_t g_saved_plv0_sp_arr[MAX_VCPUS] __attribute__((used));

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
"  csrrd $t2, 0x20\n"
"  andi  $t2, $t2, 3\n"
"  slli.d $t3, $t2, 3\n"
"  la   $t0, g_saved_plv0_sp_arr\n"
"  add.d $t0, $t0, $t3\n"
"  st.d $sp, $t0, 0\n"
/* Reset g_plv3_exit_arr[cpuid].reason = PLV3_EXIT_NONE before entering PLV3 */
"  slli.d $t4, $t2, 6\n"
"  la   $t0, g_plv3_exit_arr\n"
"  add.d $t0, $t0, $t4\n"
"  st.d $zero, $t0, 0\n"
/* Set PRMD.PPLV = 3, and PRMD.PIE = (g_plv3_timer_preempt_arr[cpuid] != 0) */
"  csrrd $t0, 0x1\n"
"  ori   $t0, $t0, 0x7\n"
"  xori  $t0, $t0, 0x4\n"
"  la    $t1, g_plv3_timer_preempt_arr\n"
"  add.d $t1, $t1, $t3\n"
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
/* Fallback capture: if g_plv3_exit_arr[cpuid].reason is still 0, snapshot ESTAT/ERA/BADV */
"  csrrd $t3, 0x20\n"
"  andi  $t3, $t3, 3\n"
"  slli.d $t4, $t3, 6\n"
"  la   $t0, g_plv3_exit_arr\n"
"  add.d $t0, $t0, $t4\n"
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
"  slli.d $t4, $t3, 3\n"
"  la   $t0, g_saved_plv0_sp_arr\n"
"  add.d $t0, $t0, $t4\n"
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
    init_phys_mem_and_vclock();
    test_vmm_handoff_and_bochs_fb();

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

    memset(gpa_to_host_ptr(GUEST_BASE), 0, (size_t)g_guest_ram_size);
    memcpy(gpa_to_host_ptr(PROGRAM_PC), program, sizeof(program));
    memcpy(gpa_to_host_ptr(GDT_PC), gdt_descriptor, sizeof(gdt_descriptor));
    {
        static const uint8_t idt_descriptor[10] = {
            0xff, 0x00,                     /* limit 0xff (16 gates)  */
            0xe0, 0x14, 0x00, 0x00,         /* base 0x14e0             */
            0x00, 0x00, 0x00, 0x00,
        };

        memcpy(gpa_to_host_ptr(IDT_DESC_PC), idt_descriptor,
               sizeof(idt_descriptor));
    }
    memcpy(gpa_to_host_ptr(STR_PC), banner, sizeof(banner));
    *(volatile uint64_t *)gpa_to_host_ptr(ALIAS_PA) = 0;
    {
        uint32_t str_addr = STR_PC;

        memcpy(gpa_to_host_ptr(PROGRAM_PC + 0xf6), &str_addr,
               sizeof(str_addr));
    }

    memset(&env, 0, sizeof(env));
    memset(&tr, 0, sizeof(tr));
    memset(&lsenv_storage, 0, sizeof(lsenv_storage));
    memset(&tcg_storage, 0, sizeof(tcg_storage));
    memset(g_fast_jmp_cache, 0, sizeof(g_fast_jmp_cache));
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

        /* Verify Hypervisor / RDRAND / RDSEED / Nehalem-or-Skylake CPUID leaves. */
        cpu_x86_cpuid(&env, 0x0, 0, &a, &b, &c, &d);
        check(latx_cpu_has_lasx() ? (a == 0x16u || a == 0x0du) : (a == 0x08u),
              "CPUID.0 max leaf matches LSX(Nehalem 0x08)/LASX(Skylake 0x16)");
        cpu_x86_cpuid(&env, 0x1, 0, &a, &b, &c, &d);
        check((c & (1u << 31)) != 0, "CPUID.1:ECX[31] HYPERVISOR set");
        check((c & (1u << 30)) == 0, "CPUID.1:ECX[30] RDRAND hidden without trusted TRNG");
        cpu_x86_cpuid(&env, 0x7, 0, &a, &b, &c, &d);
        check((b & (1u << 18)) == 0, "CPUID.7:EBX[18] RDSEED hidden without trusted TRNG");
        cpu_x86_cpuid(&env, 0x40000000u, 0, &a, &b, &c, &d);
        check(a == 0x40000010u && b == 0x4b4d564bu && c == 0x564b4d56u && d == 0x4du,
              "CPUID.40000000 advertises KVMKVMKVM with max leaf 0x40000010");
        cpu_x86_cpuid(&env, 0x40000001u, 0, &a, &b, &c, &d);
        check(a == ((1u << 1) | (1u << 3) | (1u << 6) | (1u << 7) | (1u << 11) | (1u << 24)),
              "CPUID.40000001 advertises verified KVM PV clock/EOI/unhalt/IPI");
        cpu_x86_cpuid(&env, 0x40000010u, 0, &a, &b, &c, &d);
        check(a == (uint32_t)(g_host_timer_hz / 1000ULL),
              "CPUID.40000010 TSC kHz matches dynamic cpucfg rdtime freq");
    }

    /*
     * Disable software guest-paging instrumentation: all guest memory
     * accesses now go through the LoongArch 4 KiB hardware MMU/TLB at PLV3!
     */
    option_guest_paging = 0;
    option_save_xmm = 0xff;
    option_sw_lbt = g_use_sw_lbt;
    option_lbt_jiscr = !g_use_sw_lbt;
    /* Emit hardware LoongArch LL/SC & am* atomics for LOCK/XCHG in SMP. */
    close_latx_parallel = 1;
    latx_bare_uart_puts("tb-sys: lbt_hw=");
    puthex((uint64_t)!g_use_sw_lbt);
    latx_bare_uart_puts(" sw_lbt=");
    puthex((uint64_t)option_sw_lbt);
    latx_bare_uart_puts(" lbt_jiscr=");
    puthex((uint64_t)option_lbt_jiscr);
    latx_bare_uart_puts("\n");

    latx_gen_epilogue(epi_buf);
    latx_gen_prologue(pro_buf);
    qemu_loglevel |= CPU_LOG_TB_NOCHAIN;
    g_pcpu_online[0] = 1u;
    g_pcpu_vcpu[0] = 0u;
    g_shard[0].next_asid = 1u;
    check(tbc_init(&tb_cache, tb_entries, tb_buckets, TB_TABLE_CAP) == 1,
          "tb cache init");

    rebuild_host_mappings();

    csrwr(0x1c, lapg_pwcl_value());
    csrwr(0x1d, lapg_pwch_value());
    csrwr(0x1e, lapg_stlbps_value());
    {
        uint64_t ecfg = (csrrd(0x4) & ~(UINT64_C(0x7) << 16)) |
                        (UINT64_C(1) << LOONGEMU_IRQ_IPI);
        csrwr(0x4, ecfg);
    }
    csrwr(0xc, (uint64_t)(uintptr_t)tb_sys_trap);
    csrwr(0x88, (uint64_t)(uintptr_t)tb_sys_trap);

    /* DMW0: VSEG=0, MAT=CC (1<<4), PLV0=1 */
    csrwr(0x180, ((uint64_t)0 << 60) | (UINT64_C(1) << 4) | 1u);
    /* Keep DMW1's direct map but strip every PLV1..3 enable bit. */
    csrwr(0x181, (csrrd(0x181) & ~UINT64_C(0x0e)) | 1u);
    csrwr(0x182, 0);
    csrwr(0x183, 0);
    check((csrrd(0x181) & UINT64_C(0x0e)) == 0u &&
          (csrrd(0x182) & UINT64_C(0x0e)) == 0u &&
          (csrrd(0x183) & UINT64_C(0x0e)) == 0u,
          "inherited PLV1..3 DMW windows cleared");
    {
        uint64_t crmd = csrrd(0x0);

        crmd &= ~UINT64_C(0x1f);       /* clear PLV, IE, DA, PG */
        crmd |= UINT64_C(1) << 4;      /* PG = 1, DA = 0, PLV = 0 */
        csrwr(0x0, crmd);
    }
    __asm__ volatile("invtlb 0x0, $zero, $zero" ::: "memory");
    loongemu_ipi_enable(0xffffffffu);
    loongemu_ipi_clear(0xffffffffu);
    asid_activate(1);

    /*
     * Save host page 0x201000 before the guest test writes to PA 0x201000
     * (`RO_PC`), then restore it right after the guest loop finishes.
     */
    memcpy(host_page_201000_backup, gpa_to_host_ptr(RO_PC), 4096);
    *(volatile uint8_t *)gpa_to_host_ptr(RO_PC) = 0;

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



        e = tbc_lookup_mode(&tb_cache, pc, current_tb_cs_base(),
                            env.cr[3], (uint8_t)(hflags & HF_CPL_MASK),
                            current_tb_mode_flags());
        if (e == 0) {
            e = tbc_insert_mode(&tb_cache, pc, current_tb_cs_base(), env.cr[3],
                                (uint8_t)(hflags & HF_CPL_MASK),
                                current_tb_mode_flags());
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

    ro_byte_observed = *(volatile uint8_t *)gpa_to_host_ptr(RO_PC);
    memcpy(gpa_to_host_ptr(RO_PC), host_page_201000_backup, 4096);
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
    puthex(*(volatile uint32_t *)gpa_to_host_ptr(0x1628));
    latx_bare_uart_puts(" cr2=");
    puthex(*(volatile uint64_t *)gpa_to_host_ptr(0x1600));
    latx_bare_uart_puts(" ec=");
    puthex(*(volatile uint32_t *)gpa_to_host_ptr(0x1620));
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("tb-sys: alias r14=");
    puthex(env.regs[R_R14]);
    latx_bare_uart_puts(" pa100000=");
    puthex(*(volatile uint64_t *)gpa_to_host_ptr(ALIAS_PA));
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
    check(*(volatile uint32_t *)gpa_to_host_ptr(0x1628) == 1,
          "the #PF handler ran exactly once");
    check(*(volatile uint64_t *)gpa_to_host_ptr(0x1600) == RO_PC,
          "handler saw CR2 = the faulting store address");
    check(*(volatile uint64_t *)gpa_to_host_ptr(0x1610) == STORE_PC,
          "the frame RIP is the faulting instruction");
    check(*(volatile uint32_t *)gpa_to_host_ptr(0x1620) == 0x3u,
          "error code is P|W (supervisor write)");
    check((*(volatile uint64_t *)gpa_to_host_ptr(0x5008) & ~UINT64_C(0x60)) == 0x00201003u &&
          (*(volatile uint64_t *)gpa_to_host_ptr(0x5008) & UINT64_C(0x60)) == UINT64_C(0x60),
          "the handler made the leaf writable (with A|D set by authoritative walker)");
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
        *(volatile uint64_t *)gpa_to_host_ptr(0x4000) = 0x81;
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
        *(volatile uint64_t *)gpa_to_host_ptr(0x4000) = 0x8000000000000083ull;
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
        *(volatile uint64_t *)gpa_to_host_ptr(0x4000) = 0x81;      /* read-only leaf */
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_OK && t.gpa == 0x1000, "cached walk fills");
        env.cr[0] |= CR0_WP_MASK;
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_PROTECTION,
              "cached leaf permissions are re-checked without a walk");
        /* Back to writable in the table, but the cache still says read-only. */
        *(volatile uint64_t *)gpa_to_host_ptr(0x4000) = 0x83;
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_PROTECTION, "no invalidation keeps the old leaf");
        /* INVLPG goes through helper_invlpg -> tlb_flush_page. */
        helper_invlpg(&env, 0x1000);
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_OK, "INVLPG drops the stale leaf");
        env.cr[0] &= ~(uint64_t)CR0_WP_MASK;
        /* A CR3 write drops everything, as a real address-space switch must. */
        *(volatile uint64_t *)gpa_to_host_ptr(0x4000) = 0x81;
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_OK, "cache refilled after INVLPG");
        *(volatile uint64_t *)gpa_to_host_ptr(0x4000) = 0x83;
        cpu_x86_update_cr3(&env, env.cr[3]);
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_OK, "CR3 write drops the cache");

        /* PCID-qualified CR3 switches reuse the software ASID bank. */
        {
            uint64_t saved_cr3 = env.cr[3];
            uint64_t saved_cr4 = env.cr[4];
            uint64_t hits_before = g_asid_hits;
            uint16_t asid_a;

            env.cr[4] |= X86_CR4_PCIDE;
            cpu_x86_update_cr3(&env,
                               UINT64_C(0x3000) | (UINT64_C(1) << 63) | 1u);
            asid_a = g_cur_asid;
            cpu_x86_update_cr3(&env,
                               UINT64_C(0x4000) | (UINT64_C(1) << 63) | 2u);
            cpu_x86_update_cr3(&env,
                               UINT64_C(0x3000) | (UINT64_C(1) << 63) | 1u);
            check(g_cur_asid == asid_a && g_asid_hits > hits_before,
                  "PCID bank reuses an address-space ASID");
            env.cr[3] = saved_cr3;
            env.cr[4] = saved_cr4;
            asid_activate(1);
        }

        /* PS at the PML4 level is a reserved encoding, not a 512 GiB page. */
        *(volatile uint64_t *)gpa_to_host_ptr(0x4000) = 0x83;
        *(volatile uint64_t *)gpa_to_host_ptr(0x2000) = 0x3003 | 0x80;
        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_RESERVED, "PS at the PML4 level is reserved");
        *(volatile uint64_t *)gpa_to_host_ptr(0x2000) = 0x3003;

        /* Authoritative x86_64_paging.c semantics: A/D bits, SMEP, SMAP+AC, PKU */
        {
            uint64_t saved_cr4 = env.cr[4];
            uint64_t saved_eflags = env.eflags;
            uint32_t saved_pkru = env.pkru;

            *(volatile uint64_t *)gpa_to_host_ptr(0x2000) = 0x3007;
            *(volatile uint64_t *)gpa_to_host_ptr(0x3000) = 0x4007;
            *(volatile uint64_t *)gpa_to_host_ptr(0x4000) = 0x87;
            latx_x86_tlb_flush_all();

            r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
            check(r == LATX_X86_WALK_OK &&
                  ((*(volatile uint64_t *)gpa_to_host_ptr(0x4000) & (1ULL << 5)) != 0) &&
                  ((*(volatile uint64_t *)gpa_to_host_ptr(0x4000) & (1ULL << 6)) == 0),
                  "authoritative walk sets Accessed bit without Dirty on read");

            r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
            check(r == LATX_X86_WALK_OK &&
                  ((*(volatile uint64_t *)gpa_to_host_ptr(0x4000) & (1ULL << 6)) != 0),
                  "cached write promotes Dirty bit via authoritative walker");

            env.cr[4] = saved_cr4 | X86_CR4_SMEP;
            r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_EXEC, &t);
            check(r == LATX_X86_WALK_PROTECTION,
                  "SMEP denies supervisor exec on user page");

            env.cr[4] = saved_cr4 | X86_CR4_SMAP;
            env.eflags = saved_eflags & ~X86_RFLAGS_AC;
            r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
            check(r == LATX_X86_WALK_PROTECTION,
                  "SMAP denies supervisor data read on user page when AC=0");
            env.eflags = saved_eflags | X86_RFLAGS_AC;
            r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
            check(r == LATX_X86_WALK_OK,
                  "SMAP allows supervisor data read when RFLAGS.AC=1");

            env.cr[4] = saved_cr4 | X86_CR4_PKE;
            env.eflags = saved_eflags;
            *(volatile uint64_t *)gpa_to_host_ptr(0x4000) = 0x87 | (UINT64_C(2) << 59);
            env.pkru = (2u << (2u * 2u));
            latx_x86_tlb_flush_all();
            r = latx_x86_walk(&env, 0x1000,
                              LATX_X86_ACCESS_WRITE | LATX_X86_ACCESS_USER, &t);
            check(r == LATX_X86_WALK_PROTECTION &&
                  (t.fault_error_code & X86_PF_PROTECTION_KEY) != 0,
                  "PKU write deny reports X86_PF_PROTECTION_KEY");

            {
                uint64_t viol_before = g_isolation_violations;
                uint64_t pte_vmm;
                uint64_t saved_pte = *(volatile uint64_t *)gpa_to_host_ptr(0x6008);
                /* A 2 MiB leaf cannot address an arbitrary page-aligned EFI image. */
                *(volatile uint64_t *)gpa_to_host_ptr(0x4000) = 0x6003;
                *(volatile uint64_t *)gpa_to_host_ptr(0x6008) =
                    (g_vmm_image_base & LAPG_PAGE_MASK) | 0x03;
                latx_x86_tlb_flush_all();
                r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
                pte_vmm = (r == LATX_X86_WALK_OK)
                              ? build_guest_leaf_pte((uint32_t)r, &t, LATX_X86_ACCESS_READ)
                              : 0;
                check(pte_vmm == 0 && g_isolation_violations > viol_before,
                      "host/guest isolation rejects VMM_IMAGE_BASE shadow PTE");
                *(volatile uint64_t *)gpa_to_host_ptr(0x6008) = saved_pte;
            }

            {
                LoongemuVClock tv;
                int expired = 0;
                loongemu_vclock_init(&tv, LOONGEMU_VCLOCK_VIRTUAL_STEP,
                                     100000000ULL, 100000000ULL,
                                     100000000ULL, 62500000ULL, 0, 0);
                loongemu_vclock_advance_ns(&tv, 1000000000ULL);
                check(loongemu_vclock_read_tsc(&tv) == 100000000ULL,
                      "vclock: 1s -> 100M TSC ticks");
                check(loongemu_vclock_read_acpi_pm24(&tv) == 3579545u,
                      "vclock: 1s -> 3.579545M ACPI PM ticks");
                check(loongemu_vclock_read_hpet(&tv) == 10000000ULL,
                      "vclock: 1s -> 10M HPET ticks");
                check(loongemu_vclock_read_arm_cntvct(&tv) == 62500000ULL,
                      "vclock: 1s -> 62.5M AArch64 CNTVCT ticks");
                check(loongemu_vclock_lapic_current(&tv, 0, 50000000u, 2u, 0, &expired) == 0 &&
                      expired == 1,
                      "vclock: LAPIC one-shot expires cleanly at deadline");
            }

            *(volatile uint64_t *)gpa_to_host_ptr(0x2000) = 0x3003;
            *(volatile uint64_t *)gpa_to_host_ptr(0x3000) = 0x4003;
            *(volatile uint64_t *)gpa_to_host_ptr(0x4000) = 0x83;
            env.cr[4] = saved_cr4;
            env.eflags = saved_eflags;
            env.pkru = saved_pkru;
            latx_x86_tlb_flush_all();
        }
    }

    {
        LatxX86Translation probe;

        check(latx_x86_walk(&env, ALIAS_VA, LATX_X86_ACCESS_READ, &probe) ==
              LATX_X86_WALK_OK && probe.gpa == ALIAS_PA &&
              probe.page_bits == 12,
              "walk maps the aliased VA to PA 0x100000");
    }
    check(*(volatile uint64_t *)gpa_to_host_ptr(ALIAS_PA) == 0x44332211ull,
          "store through the aliased VA reached PA 0x100000");
    check(env.regs[R_R14] == 0x44332211u, "read through the aliased VA agrees");

    /* the 4-level tables the guest built, read back through the data path */
    check((*(volatile uint64_t *)gpa_to_host_ptr(0x2000) & ~UINT64_C(0x20)) == 0x3003u, "PML4 entry");
    check((*(volatile uint64_t *)gpa_to_host_ptr(0x3000) & ~UINT64_C(0x20)) == 0x4003u, "PDPT entry");
    check((*(volatile uint64_t *)gpa_to_host_ptr(0x4000) & ~UINT64_C(0x60)) == 0x83u, "PD entry (2 MiB leaf)");
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
        *(volatile uint64_t *)gpa_to_host_ptr(TIMER_IDT_PC + 0x20 * 16) =
            (uint64_t)TIMER_STUB_PC & 0xffff;
        *(volatile uint64_t *)gpa_to_host_ptr(TIMER_IDT_PC + 0x20 * 16 + 8) =
            ((uint64_t)TIMER_STUB_PC >> 16) & 0xffff;

        {
            uint64_t *slot =
                (uint64_t *)gpa_to_host_ptr(TIMER_IDT_PC + 0x20 * 16 + 8);

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
        latx_platform_io_write(0xcf8, 4, 0x80000000); /* PCI CONFIG_ADDR 00:00.0 */
        check(g_pit_port_writes == 2, "PIT port writes recorded");
        check(g_pci_config_writes == 1, "PCI config write recorded");
        check(latx_platform_io_read(0x40, 1) == 0x1234,
              "PIT counter reads back");
        check(latx_platform_io_read(0xcfc, 4) == 0x12378086u,
              "PCI 00:00.0 reports i440FX host bridge (8086:1237)");
        latx_platform_io_write(0xcf8, 4, 0x8000f800); /* unoccupied 00:1f.0 */
        check(latx_platform_io_read(0xcfc, 4) == 0xffffffffu,
              "PCI unoccupied slot reports 0xffffffff");
        /* Verify PCI Host Bridge devices, ECAM, Bochs Display GOP/EDID, and Loongson 7A split-CFG */
        {
            uint64_t bochs_id = loongemu_pci_ecam_read(&g_pci_host, (2ULL << 15), 4u);
            uint64_t piix3_id = loongemu_pci_ecam_read(&g_pci_host, (1ULL << 15), 4u);
            uint64_t piix4_id = loongemu_pci_ecam_read(&g_pci_host, (1ULL << 15) | (3ULL << 12), 4u);
            uint64_t edid0 = loongemu_bochs_mmio_read(&g_pci_host.display, 0x000u, 4u);
            uint64_t dispi_id = loongemu_bochs_mmio_read(&g_pci_host.display, 0x500u, 2u);
            uint64_t ls7a_t0 = loongemu_ls7a_cfg_addr(LOONGSON_7A_CFG_TYPE0_BASE,
                                                      LOONGSON_7A_CFG_TYPE1_BASE,
                                                      0u, 6u, 1u, 0x104u);
            uint64_t ls7a_t1 = loongemu_ls7a_cfg_addr(LOONGSON_7A_CFG_TYPE0_BASE,
                                                      LOONGSON_7A_CFG_TYPE1_BASE,
                                                      2u, 0u, 0u, 0x208u);
            check(piix3_id == 0x70008086u && piix4_id == 0x71138086u && bochs_id == 0x11111234u,
                  "PCI ECAM enumerates PIIX3 (8086:7000), PIIX4 PM (8086:7113), Bochs Display (1234:1111)");
            check(edid0 == 0xffffff00u && dispi_id == 0xb0c5u,
                  "Bochs Display BAR2 exposes valid EDID header and DISPI ID 0xB0C5");
            check(ls7a_t0 == UINT64_C(0x00000efe01003104) && ls7a_t1 == UINT64_C(0x00000efe12020008),
                  "Loongson 7A1000/7A2000 HT split-CFG encodes [27:24]=reg[11:8] and [7:0]=reg[7:0]");
            check(loongemu_ls7a_is_display_or_gpu(0x0014u, 0x7a06u) &&
                  loongemu_ls7a_is_display_or_gpu(0x0014u, 0x7a36u) &&
                  loongemu_ls7a_is_display_or_gpu(0x0014u, 0x7a15u) &&
                  loongemu_ls7a_is_display_or_gpu(0x0014u, 0x7a25u) &&
                  loongemu_ls7a_is_internal_bridge(0x0014u, 0x7a09u),
                  "Loongson 7A DC/GPU filter & internal root port quirk classification verified");
            check(latx_platform_io_read(LOONGEMU_QEMU_DEBUGCON_PORT, 1) == 0xe9u,
                  "QEMU debugcon port 0x402 returns 0xE9 signature");
            /* Verify the GOP and 7A portions of the VMM handoff. */
            {
                LoongemuVmmHandoff h;
                LoongemuMemLookup lk_fb;
                LoongemuVmmHandoff saved_handoff = g_vmm_handoff;
                int saved_external = g_guest_ram_external;
                memset(&h, 0, sizeof(h));
                h.magic = LOONGEMU_VMM_HANDOFF_MAGIC;
                h.version = LOONGEMU_VMM_HANDOFF_VERSION;
                h.gop.fb_hpa = g_bochs_fb_fallback_hpa;
                h.gop.fb_size = BOCHS_FB_FALLBACK_SIZE;
                h.gop.width = 1024u;
                h.gop.height = 768u;
                h.gop.stride_px = 1024u;
                h.gop.bpp = 32u;
                h.gop.pixel_format = 1u;
                h.gop.valid = 1u;
                h.gop.owner = (uint8_t)LOONGEMU_FB_OWNER_HOST_GOP;
                h.gop.host_mat = (uint8_t)LOONGEMU_MAT_WUC;
                h.ls7a_cfg_type0_base = LOONGSON_7A_CFG_TYPE0_BASE;
                h.ls7a_cfg_type1_base = LOONGSON_7A_CFG_TYPE1_BASE;
                h.ls7a_mcfg_valid = 1u;
                h.hide_host_dc_gpu = 1u;
                /* Synthetic GOP backed by the allocated test buffer. */
                g_guest_ram_external = 0;
                tb_sys_set_vmm_handoff(&h);
                LoongemuMemKind k_fb = loongemu_mem_resolve(&g_mem_map, LOONGEMU_BOCHS_FB_DEFAULT_GPA, 4u, &lk_fb);
                uint64_t xres = loongemu_bochs_mmio_read(&g_pci_host.display, 0x502u, 2u);
                uint64_t yres = loongemu_bochs_mmio_read(&g_pci_host.display, 0x504u, 2u);
                check(k_fb == LOONGEMU_MEM_RAM && lk_fb.hpa == h.gop.fb_hpa &&
                      xres == 1024u && yres == 768u && g_pci_host.ls7a.hide_host_dc_gpu == 1,
                      "VMM handoff binds Bochs BAR0 to GOP backing and geometry");
                check(g_pci_host.ls7a.enabled == 0,
                      "physical LS7A passthrough remains closed without callbacks");
                bochs_display_bar_map_cb(0, 0, 0, 0, 0);
                g_guest_ram_external = saved_external;
                g_vmm_handoff = saved_handoff;
                if (saved_handoff.magic == LOONGEMU_VMM_HANDOFF_MAGIC) {
                    tb_sys_set_vmm_handoff(&saved_handoff);
                } else {
                    loongemu_pci_host_init(&g_pci_host, 0, bochs_display_bar_map_cb, 0);
                }
            }
        }

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

            *(volatile uint8_t *)gpa_to_host_ptr(HLT_PC_TEST) = 0xf4;  /* hlt */
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

            *(volatile uint8_t *)gpa_to_host_ptr(HLT_PC_TEST) = 0xf4;
            gate_lo = ((uint64_t)TICK_ISR_PC & 0xffffULL) |
                      ((uint64_t)0x08 << 16) |
                      ((uint64_t)0x8e << 40) |
                      (((((uint64_t)TICK_ISR_PC) >> 16) & 0xffffULL) << 48);
            gate_hi = ((uint64_t)TICK_ISR_PC >> 32) & 0xffffffffULL;
            *(volatile uint64_t *)gpa_to_host_ptr(TICK_IDT_PC + TICK_VECTOR * 16) =
                gate_lo;
            *(volatile uint64_t *)gpa_to_host_ptr(TICK_IDT_PC +
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
                check(*(volatile uint32_t *)gpa_to_host_ptr(TICK_COUNTER_PC) == 4,
                      "idle loop: the handler's own inc ran four times");
                latx_bare_uart_puts("tb-sys: idle_rounds=");
                puthex((uint64_t)(idle < 0 ? 0 : idle));
                latx_bare_uart_puts(" idle_count=");
                puthex(*(volatile uint32_t *)gpa_to_host_ptr(TICK_COUNTER_PC));
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

        memcpy(gpa_to_host_ptr(base), kernel_head, sizeof(kernel_head));

        /*
         * Walk instruction by instruction rather than in fixed strides: a
         * block boundary is wherever a block ends, and stepping 16 bytes at a
         * time lands mid-instruction and reports a decode failure that is an
         * artifact of the walk, not of the translator.
         */
        while (blocks < 40 && pc < base + 96) {
            TbInsnMap map;
            int rc = tb_harness_translate_at_with_map(
                         pc, gpa_to_host_ptr(CODE_SCRATCH + 0x20000),
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

        memcpy(gpa_to_host_ptr(pc), ovmf_reset, sizeof(ovmf_reset));
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
        int has_gr2scr = 0, has_jiscr0 = 0, has_jirl = 0;

        memcpy(gpa_to_host_ptr(src_pc), src_x86, sizeof(src_x86));
        memcpy(gpa_to_host_ptr(dst_pc), dst_x86, sizeof(dst_x86));
        memset(fast_jmp_cache, 0, sizeof(fast_jmp_cache));
        env.tb_jmp_cache_ptr = fast_jmp_cache;
        option_lbt_jiscr = !option_sw_lbt;
        latx_x86_tlb_flush_all();

        rc_dst = tb_harness_translate_at(dst_pc, dst_code, 4096);
        rc_src = tb_harness_translate_at(src_pc, src_code, 4096);
        check(rc_dst > 0 && rc_src > 0, "indirect jump fast-path blocks translate");
        if (rc_src > 0) {
            const uint32_t *insns = (const uint32_t *)(uintptr_t)src_code;
            for (int k = 0; k < rc_src / 4; k++) {
                if ((insns[k] & 0xfffffc00u) == 0x00000800u) {
                    has_gr2scr = 1;
                }
                if (insns[k] == 0x48000200u) {
                    has_jiscr0 = 1;
                }
                if ((insns[k] & 0xfc00fc1fu) == 0x4c000000u && ((insns[k] >> 5) & 0x1fu) != 1u) {
                    has_jirl = 1;
                }
            }
        }
        if (option_sw_lbt) {
            check(!has_gr2scr && !has_jiscr0 && has_jirl,
                  "sw_lbt indirect goto emitted pure-GPR jirl without gr2scr/jiscr0");
        } else {
            check(has_gr2scr && has_jiscr0, "indirect goto emitted gr2scr + jiscr0");
        }
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

        memcpy(gpa_to_host_ptr(pc), kernel_entry, sizeof(kernel_entry));

        emitted = tb_harness_translate_at(pc, gpa_to_host_ptr(CODE_SCRATCH), 4096);
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

        memset(gpa_to_host_ptr(pml4), 0, 0x4000);
        /* PML4[511] -> PDPT, PDPT[510] -> PD, PD[0] -> PT, PT[0] -> 16 MiB */
        /*
         * Low identity map: PML4[0] -> PDPT[0] -> PD, PD[8..] -> 2 MiB leaves
         * at 16 MiB and up.  The kernel is placed at its physical load
         * address, so the address it computes from RIP and the address the
         * tables translate are the same number.
         */
        *(volatile uint64_t *)gpa_to_host_ptr(pml4 + 0 * 8) = pdpt | 0x3;
        *(volatile uint64_t *)gpa_to_host_ptr(pdpt + 0 * 8) = pd | 0x3;

        /*
         * The kernel's own prologue loads RSP from a RIP-relative LEA that
         * still points into the image's link-time half (0xffffffff8280xxxx),
         * so the negative-half window has to resolve too -- mapped onto the
         * same physical leaves.  Running at the load address satisfies the
         * entry's `rdi >> 46` check; it does not change the addresses the
         * image computes for itself.
         */
        *(volatile uint64_t *)gpa_to_host_ptr(pml4 + 511 * 8) = pdpt | 0x3;
        *(volatile uint64_t *)gpa_to_host_ptr(pdpt + 510 * 8) = pd | 0x3;
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

                *(volatile uint64_t *)gpa_to_host_ptr(pd + i * 8) =
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
                memcpy(gpa_to_host_ptr(gpa), _ktext_start, KERNEL_TEXT_SIZE);
                memcpy(gpa_to_host_ptr(gpa + KERNEL_SEG1_REL),
                       _kseg1_start, KERNEL_SEG1_SIZE);
                memcpy(gpa_to_host_ptr(gpa + KERNEL_SEG2_REL),
                       _kseg2_start, KERNEL_SEG2_SIZE);
                memcpy(gpa_to_host_ptr(gpa + KERNEL_SEG3_REL),
                       _kseg3_start, KERNEL_SEG3_SIZE);
            }

            {
                uint64_t saved_cr4 = env.cr[4];

                env.cr[4] = saved_cr4 | CR4_PAE_MASK;
                /*
                 * Populate a minimal Linux x86-64 boot_params at
                 * BOOT_PARAMS_GPA (0x10000) with a 128 MiB e820 RAM map and
                 * an earlyprintk serial command line at 0x20000.
                 */
                {
                    extern const uint8_t _initramfs_start[];
                    extern const uint8_t _initramfs_end[];
                    uint64_t initramfs_gpa = 0x03900000ULL;
                    uint32_t initramfs_img = (uint32_t)initramfs_gpa;
                    uint32_t initramfs_sz = (uint32_t)(_initramfs_end - _initramfs_start);
                    uint8_t *bp = (uint8_t *)gpa_to_host_ptr(BOOT_PARAMS_GPA);
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
                    loongemu_acpi_build((uint8_t *)gpa_to_host_ptr(GUEST_BASE),
                                        LOONGEMU_ACPI_RSDP_GPA,
                                        LOONGEMU_ACPI_TABLES_GPA,
                                        MAX_VCPUS, &acpi_info);
                    {
                        LoongemuSmbiosInfo smbios_info;
                        LoongemuPlatformIdentity ident;
                        loongemu_identity_default(&ident, 0);
                        int sm_ok = loongemu_smbios_build(
                            (uint8_t *)gpa_to_host_ptr(GUEST_BASE),
                            LOONGEMU_SMBIOS_EP_GPA,
                            LOONGEMU_SMBIOS_TABLE_GPA,
                            MAX_VCPUS, (uint64_t)GUEST_RAM_SIZE,
                            (uint32_t)(g_host_timer_hz / 1000000ULL),
                            &smbios_info);
                        check(sm_ok == 1 && smbios_info.table_length > 100u,
                              "SMBIOS 3.0 tables built in guest RAM");
                        check(ident.override_pci_ids == 0 && ident.nic_mac[0] == 0x00u,
                              "platform identity defaults: Linux PCI untouched, non-virt OUI");
                    }
                    /* Verify IOAPIC generic GSI injection, APIC TPR/ISRV, KVM PV & RDRAND/RDSEED. */
                    {
                        lapic_reset_all();
                        ioapic_reset();
                        g_ioapic_redtbl[5] = 0x45ULL; /* unmasked, vec 0x45, dest CPU 0 */
                        check(ioapic_assert_irq(5u) == 1, "ioapic_assert_irq injects GSI 5");
                        cpu_set_apic_tpr(&env, 0x5u); /* TPR=0x50 masks vec 0x45 */
                        check(lapic_pop_irr(0) == 0u, "APIC TPR (CR8=5) masks vector 0x45");
                        cpu_set_apic_tpr(&env, 0x3u); /* TPR=0x30 allows vec 0x45 */
                        check(lapic_pop_irr(0) == 0x45u, "APIC TPR (CR8=3) unmasks vector 0x45");
                        lapic_set_irr(0, 0x42u); /* same prio class 4 blocked by ISRV=0x45 */
                        check(lapic_pop_irr(0) == 0u, "APIC ISRV blocks equal/lower prio vector");
                        lapic_eoi(0);
                        check(lapic_pop_irr(0) == 0x42u, "APIC EOI unblocks queued vector 0x42");
                        lapic_eoi(0);
                        cpu_set_apic_tpr(&env, 0u);

                        uint64_t hv_t0 = 0, hv_t1 = 0;
                        latx_platform_kvm_wrmsr(&env, 0x4b564d01u, 0x00018001ULL);
                        check((latx_platform_phys_read(0x00018000ULL, 4) & 1u) == 0u &&
                              latx_platform_phys_read(0x00018000ULL + 24u, 4) != 0u,
                              "KVM pvclock MSR populated valid time_info");
                        check(latx_platform_kvm_rdmsr(&env, 0x40000020u, &hv_t0) == 1 &&
                              latx_platform_kvm_rdmsr(&env, 0x40000020u, &hv_t1) == 1 &&
                              hv_t1 >= hv_t0,
                              "HV_X64_MSR_TIME_REF_COUNT monotonic");

                        static const uint8_t rng_x86[] = {
                            0x48, 0x0f, 0xc7, 0xf0, /* rdrand rax */
                            0x48, 0x0f, 0xc7, 0xfb, /* rdseed rbx */
                            0xf4,                   /* hlt        */
                        };
                        uint64_t rng_pc = 0x0000e800ULL;
                        uint8_t *rng_code = code_cache + 0x12000;
                        memcpy(gpa_to_host_ptr(rng_pc), rng_x86, sizeof(rng_x86));
                        uint64_t saved_eip_r = env.eip;
                        uint64_t saved_cr0_r = env.cr[0];
                        env.cr[0] &= ~(uint64_t)CR0_PG_MASK;
                        latx_x86_tlb_flush_all();
                        /* P0-A & P0-C: RDRAND/RDSEED hidden without trusted entropy; KVM PV_EOI omitted */
                        {
                            uint32_t ca, cb, cc, cd;
                            cpu_x86_cpuid(&env, 0x1, 0, &ca, &cb, &cc, &cd);
                            check((cc & (1u << 30)) == 0u,
                                  "P0-A: RDRAND hidden in CPUID.1:ECX[30] without trusted TRNG");
                            cpu_x86_cpuid(&env, 0x7, 0, &ca, &cb, &cc, &cd);
                            check((cb & (1u << 18)) == 0u,
                                  "P0-A: RDSEED hidden in CPUID.7:EBX[18] without trusted TRNG");
                            cpu_x86_cpuid(&env, 0x40000001u, 0, &ca, &cb, &cc, &cd);
                            check((ca & (1u << 6)) != 0u,
                                  "KVM_FEATURE_PV_EOI advertised after end-to-end LAPIC wiring");
                        }
                        /* Verify KVM PV_EOI end-to-end LAPIC ISR retirement without MMIO EOI */
                        {
                            uint64_t pveoi_gpa = 0x00019200ULL;
                            lapic_reset_all();
                            latx_platform_kvm_wrmsr(&env, 0x4b564d04u, pveoi_gpa | 1u);
                            lapic_set_irr(0, 0x45u);
                            check(lapic_pop_irr(0) == 0x45u &&
                                  latx_platform_phys_read(pveoi_gpa, 4) == 1u,
                                  "KVM PV_EOI arms shared guest word=1 on vector 0x45 injection");
                            lapic_set_irr(0, 0x42u);
                            check(lapic_pop_irr(0) == 0u,
                                  "KVM PV_EOI keeps same-prio 0x42 blocked while word=1");
                            /* Guest performs fast PV EOI by clearing bit 0 in RAM (no MMIO EOI) */
                            latx_platform_phys_write(pveoi_gpa, 4, 0u);
                            check(lapic_pop_irr(0) == 0x42u,
                                  "KVM PV_EOI retires ISR when guest clears word=0 and delivers 0x42");
                            latx_platform_phys_write(pveoi_gpa, 4, 0u);
                            check(lapic_pop_irr(0) == 0u, "KVM PV_EOI retires 0x42 cleanly");
                            latx_platform_kvm_wrmsr(&env, 0x4b564d04u, 0u);
                        }
                        /* Verify Hyper-V 0x40000003/0x40000004 CPUID, APIC MSRs/Assist & Hypercalls */
                        {
                            uint32_t saved_hv = latx_hv_vendor;
                            uint32_t ha, hb, hc, hd;
                            uint64_t hv_hc_gpa = 0x0001a000ULL;
                            uint64_t hv_as_gpa = 0x0001b000ULL;
                            uint64_t rval = 0;

                            latx_hv_vendor = 1u;
                            __atomic_store_n(&g_vcpu_online[0], 1, __ATOMIC_RELEASE);
                            cpu_x86_cpuid(&env, 0x40000003u, 0, &ha, &hb, &hc, &hd);
                            check(ha == ((1u << 1) | (1u << 4) | (1u << 5) | (1u << 6)) &&
                                  (hd & (1u << 4)) != 0u,
                                  "Hyper-V CPUID.40000003 advertises implemented MSRs & hypercalls");
                            cpu_x86_cpuid(&env, 0x40000004u, 0, &ha, &hb, &hc, &hd);
                            check(ha == ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) |
                                         (1u << 10) | (1u << 11)) &&
                                  hb == 0x00000fffu,
                                  "Hyper-V CPUID.40000004 advertises backed TLB/APIC/timing/IPI recs");

                            /* Hypercall page stub installation (vmcall; ret = 0xc3c1010f) */
                            latx_platform_kvm_wrmsr(&env, 0x40000001u, hv_hc_gpa | 1u);
                            check(latx_platform_phys_read(hv_hc_gpa, 4) == 0xc3c1010fu,
                                  "HV_X64_MSR_HYPERCALL installs executable vmcall;ret stub");

                            /* Synthetic APIC MSRs (TPR/EOI) + APIC Assist Page fast EOI */
                            lapic_reset_all();
                            latx_platform_kvm_wrmsr(&env, 0x40000072u, 0x50u);
                            check(latx_platform_kvm_rdmsr(&env, 0x40000072u, &rval) == 1 && rval == 0x50u,
                                  "HV_X64_MSR_TPR reads/writes LAPIC TPR");
                            latx_platform_kvm_wrmsr(&env, 0x40000072u, 0u);
                            latx_platform_kvm_wrmsr(&env, 0x40000073u, hv_as_gpa | 1u);
                            latx_platform_kvm_wrmsr(&env, 0x40000073u, hv_as_gpa | 3u);
                            check(latx_platform_kvm_rdmsr(&env, 0x40000073u, &rval) &&
                                  rval == (hv_as_gpa | 1u),
                                  "APIC assist rejects reserved unaligned bits");
                            lapic_set_irr(0, 0x55u);
                            check(lapic_pop_irr(0) == 0x55u &&
                                  latx_platform_phys_read(hv_as_gpa, 4) == 1u,
                                  "HV_X64_MSR_APIC_ASSIST_PAGE arms NO_EOI_REQUIRED=1");
                            latx_platform_phys_write(hv_as_gpa, 4, 0u);
                            lapic_set_irr(0, 0x52u);
                            check(lapic_pop_irr(0) == 0x52u,
                                  "Hyper-V APIC Assist fast EOI retires ISR and delivers 0x52");
                            latx_platform_kvm_wrmsr(&env, 0x40000070u, 0u); /* HV_X64_MSR_EOI */
                            lapic_set_irr(0, 0x55u);
                            check(lapic_pop_irr(0) == 0x55u &&
                                  latx_platform_phys_read(hv_as_gpa, 4) == 1u,
                                  "APIC assist arms before nesting");
                            lapic_set_irr(0, 0x71u);
                            check(lapic_pop_irr(0) == 0x71u &&
                                  latx_platform_phys_read(hv_as_gpa, 4) == 0u,
                                  "APIC assist clears on nested interrupt");
                            latx_platform_kvm_wrmsr(&env, 0x40000070u, 0u);
                            check((g_lapic[0].isr[0x55u >> 5] & (1u << (0x55u & 31u))) != 0u,
                                  "nested EOI retains underlying ISR");
                            latx_platform_kvm_wrmsr(&env, 0x40000070u, 0u);
                            latx_platform_kvm_wrmsr(&env, 0x40000073u, 0u);

                            /* Synthetic IPIs must use the physical owner, not the VP index. */
                            {
                                uint32_t saved_pcpu1 =
                                    __atomic_load_n(&g_pcpu_online[1],
                                                    __ATOMIC_ACQUIRE);
                                uint32_t saved_owner1 =
                                    __atomic_load_n(&g_vp_owner[1],
                                                    __ATOMIC_ACQUIRE);
                                int saved_vcpu1 =
                                    __atomic_load_n(&g_vcpu_online[1],
                                                    __ATOMIC_ACQUIRE);

                                __atomic_store_n(&g_pcpu_online[1], 1u,
                                                 __ATOMIC_RELEASE);
                                __atomic_store_n(&g_vp_owner[1], 1u,
                                                 __ATOMIC_RELEASE);
                                __atomic_store_n(&g_vcpu_online[1], 1,
                                                 __ATOMIC_RELEASE);
                                g_last_ipi_pcpu = UINT32_MAX;
                                hv_send_ipi_callback(0, UINT64_C(2), 0x61u);
                                check(g_last_ipi_pcpu == 1u &&
                                      lapic_has_irr(1),
                                      "Hyper-V synthetic IPI wakes the physical VP owner");
                                (void)lapic_pop_irr(1);

                                latx_hv_vendor = 0u;
                                env.regs[R_EAX] = 5u; /* KVM_HC_KICK_CPU */
                                env.regs[R_EBX] = 0u;
                                env.regs[R_ECX] = 1u;
                                __atomic_store_n(&g_vcpu_kicked[1], 0,
                                                 __ATOMIC_RELEASE);
                                g_last_ipi_pcpu = UINT32_MAX;
                                helper_hypercall(&env);
                                check(g_last_ipi_pcpu == 1u &&
                                      __atomic_load_n(&g_vcpu_kicked[1],
                                                      __ATOMIC_ACQUIRE),
                                      "KVM kick routes to the physical VP owner");
                                __atomic_store_n(&g_vcpu_kicked[1], 0,
                                                 __ATOMIC_RELEASE);
                                __atomic_store_n(&g_vcpu_online[1], saved_vcpu1,
                                                 __ATOMIC_RELEASE);
                                __atomic_store_n(&g_vp_owner[1], saved_owner1,
                                                 __ATOMIC_RELEASE);
                                __atomic_store_n(&g_pcpu_online[1], saved_pcpu1,
                                                 __ATOMIC_RELEASE);
                                /* The remainder of this block is Hyper-V mode. */
                                latx_hv_vendor = 1u;
                            }

                            /* Hyper-V Hypercalls: FlushVirtualAddressSpace + SendSyntheticClusterIpi */
                            env.regs[R_ECX] = 0x00010002u; /* fast HvCallFlushVirtualAddressSpace */
                            env.regs[R_EDX] = 0u;
                            env.regs[R_R8] = 1u;
                            env.xmm_regs[0].ZMM_Q(0) = 1u;
                            helper_hypercall(&env);
                            check(env.regs[R_EAX] == 0u,
                                  "HvCallFlushVirtualAddressSpace returns HV_STATUS_SUCCESS");
                            env.regs[R_ECX] = 0x0001000bu; /* fast HvCallSendSyntheticClusterIpi */
                            env.regs[R_EDX] = 0x61u;
                            env.regs[8] = 0x1u;            /* CPU 0 mask */
                            helper_hypercall(&env);
                            check(env.regs[R_EAX] == 0u && lapic_pop_irr(0) == 0x61u,
                                  "HvCallSendSyntheticClusterIpi injects vector 0x61 cleanly");
                            latx_platform_kvm_wrmsr(&env, 0x40000070u, 0u);
                            env.regs[R_ECX] = 0x0001000bu;
                            env.regs[R_EDX] = 0x0fu;
                            helper_hypercall(&env);
                            check(env.regs[R_EAX] == 3u, "Hyper-V rejects reserved IPI vectors");
                            env.regs[R_ECX] = 0x0002u;
                            env.regs[R_EDX] = hv_as_gpa + 1u;
                            helper_hypercall(&env);
                            check(env.regs[R_EAX] == 4u, "Hyper-V rejects unaligned input GPA");

                            /*
                             * Fast flush input qwords 2+ travel in XMM0..XMM5.
                             * Exercise the translated VMCALL path, not just a
                             * direct C helper call, so a missing XMM spill fails.
                             */
                            {
                                static const uint8_t hv_vmcall_x86[] = {
                                    0xb8, 0x01, 0x00, 0x00, 0x00, /* mov eax,1 */
                                    0x66, 0x0f, 0x6e, 0xc0,       /* movd xmm0,eax */
                                    0xb9, 0x02, 0x00, 0x01, 0x00, /* mov ecx,0x10002 */
                                    0x31, 0xd2,                   /* xor edx,edx */
                                    0x45, 0x31, 0xc0,             /* xor r8d,r8d */
                                    0x0f, 0x01, 0xc1,             /* vmcall */
                                    0xf4,                         /* hlt */
                                };
                                uint64_t hv_pc = 0x0000e900ULL;
                                uint8_t *hv_code = code_cache + 0x17000;
                                uint64_t flushes_before =
                                    __atomic_load_n(&g_hv_flush_calls,
                                                    __ATOMIC_RELAXED);

                                memcpy(gpa_to_host_ptr(hv_pc), hv_vmcall_x86,
                                       sizeof(hv_vmcall_x86));
                                env.xmm_regs[0].ZMM_Q(0) = 0;
                                env.eip = hv_pc;
                                env.eflags = 0x2;
                                latx_x86_tlb_flush_all();
                                int rc_hv = tb_harness_translate_at(
                                    hv_pc, hv_code, 4096);
                                check(rc_hv > 0,
                                      "fast Hyper-V VMCALL block translates");
                                if (rc_hv > 0) {
                                    loongemu_sync_icache_range(
                                        (uint64_t)(uintptr_t)hv_code,
                                        (uint64_t)rc_hv);
                                    tb_sys_run_plv3(
                                        (void *)(uintptr_t)pro_buf,
                                        hv_code, &env);
                                    check(env.regs[R_EAX] == 0u &&
                                          __atomic_load_n(&g_hv_flush_calls,
                                                          __ATOMIC_RELAXED) ==
                                              flushes_before + 1u,
                                          "translated VMCALL flushes XMM0 mask");
                                }
                            }

                            latx_platform_kvm_wrmsr(&env, 0x40000001u, 0u);
                            latx_hv_vendor = saved_hv;
                        }
                        int rc_rng = tb_harness_translate_at(rng_pc, rng_code, 4096);
                        check(rc_rng > 0, "RDRAND/RDSEED block translates");
                        if (rc_rng > 0) {
                            loongemu_sync_icache_range((uint64_t)(uintptr_t)rng_code, (uint64_t)rc_rng);
                            env.eip = rng_pc;
                            env.regs[R_EAX] = 0x1111ULL;
                            env.regs[R_EBX] = 0x2222ULL;
                            env.eflags = 0x3;
                            tb_sys_run_plv3((void *)(uintptr_t)pro_buf, rng_code, &env);
                            check(env.regs[R_EAX] == 0 && env.regs[R_EBX] == 0 &&
                                  (env.eflags & 1u) == 0u,
                                  "P0-A: RDRAND/RDSEED return 0 with CF=0 when entropy unavailable");
                        }

                        /* Verify XGETBV / XSETBV + CR4.OSXSAVE CPUID linkage at PLV3. */
                        if (latx_cpu_has_lasx()) {
                            static const uint8_t xbv_x86[] = {
                                0x31, 0xc9,                   /* xor ecx, ecx */
                                0x0f, 0x01, 0xd0,             /* xgetbv       */
                                0x89, 0xc3,                   /* mov ebx, eax */
                                0xb8, 0x07, 0x00, 0x00, 0x00, /* mov eax, 7   */
                                0x31, 0xd2,                   /* xor edx, edx */
                                0x0f, 0x01, 0xd1,             /* xsetbv       */
                                0x31, 0xc0,                   /* xor eax, eax */
                                0x0f, 0x01, 0xd0,             /* xgetbv       */
                                0xf4,                         /* hlt          */
                            };
                            uint64_t xbv_pc = 0x0000e840ULL;
                            uint8_t *xbv_code = code_cache + 0x13000;
                            uint64_t saved_cr4_x = env.cr[4];
                            uint64_t saved_xcr0_x = env.xcr0;
                            uint32_t xa, xb, xc, xd;

                            env.xcr0 = XSTATE_FP_MASK;
                            env.cr[4] = saved_cr4_x | CR4_OSXSAVE_MASK;
                            cpu_x86_cpuid(&env, 0x1, 0, &xa, &xb, &xc, &xd);
                            check((xc & (1u << 27)) != 0u,
                                  "CR4.OSXSAVE sets CPUID.1:ECX[27]");
                            memcpy(gpa_to_host_ptr(xbv_pc), xbv_x86, sizeof(xbv_x86));
                            int rc_xbv = tb_harness_translate_at(xbv_pc, xbv_code, 4096);
                            check(rc_xbv > 0, "XGETBV/XSETBV block translates");
                            if (rc_xbv > 0) {
                                loongemu_sync_icache_range((uint64_t)(uintptr_t)xbv_code, (uint64_t)rc_xbv);
                                env.eip = xbv_pc;
                                env.regs[R_EAX] = 0;
                                env.regs[R_EBX] = 0;
                                env.regs[R_ECX] = 0;
                                env.regs[R_EDX] = 0;
                                env.eflags = 0x2;
                                tb_sys_run_plv3((void *)(uintptr_t)pro_buf, xbv_code, &env);
                                check(env.regs[R_EBX] == 1u && env.regs[R_EAX] == 7u && env.xcr0 == 7u,
                                      "XGETBV/XSETBV round-trip XCR0 1 -> 7 at PLV3");
                                cpu_x86_cpuid(&env, 0xd, 0, &xa, &xb, &xc, &xd);
                                check(xa == 7u && xb == 0x340u,
                                      "CPUID.0D:EBX reflects enabled XCR0=7 size (0x340)");
                            }
                            env.cr[4] = saved_cr4_x;
                            env.xcr0 = saved_xcr0_x;
                        }

                        /* Verify non-canonical address EXC_ADE -> guest #GP(0) delivery. */
                        {
                            static const uint8_t ade_x86[] = {
                                0x48, 0xb8, 0x21, 0x01, 0x00, 0x00, 0x00, 0x00, 0xad, 0xde,
                                0x48, 0x8b, 0x00, /* mov rax, [rax] -> EXC_ADE */
                                0xf4,
                            };
                            uint64_t ade_pc = 0x0000e900ULL;
                            uint64_t gp_isr = 0x0000e980ULL;
                            uint64_t gp_idt = 0x0000ea00ULL;
                            uint64_t gp_stk = 0x0000ef00ULL;
                            uint8_t *ade_code = code_cache + 0x14000;
                            uint64_t saved_idt_base = env.idt.base;
                            uint32_t saved_idt_limit = env.idt.limit;
                            uint64_t saved_rsp = env.regs[R_ESP];
                            uint64_t gp_before = g_hw_guest_gp;
                            uint64_t gate_lo = (gp_isr & 0xffffULL) |
                                               ((uint64_t)0x08u << 16) |
                                               ((uint64_t)0x8eu << 40) |
                                               (((gp_isr >> 16) & 0xffffULL) << 48);
                            uint64_t gate_hi = (gp_isr >> 32) & 0xffffffffULL;
                            TbInsnMap ade_map;

                            (void)guest_linear_write_u64(gp_idt + 13u * 16u, gate_lo);
                            (void)guest_linear_write_u64(gp_idt + 13u * 16u + 8u, gate_hi);
                            memcpy(gpa_to_host_ptr(ade_pc), ade_x86, sizeof(ade_x86));
                            env.idt.base = gp_idt;
                            env.idt.limit = 0xfffu;
                            env.segs[R_CS].selector = 0x08u;
                            env.segs[R_SS].selector = 0x10u;
                            env.regs[R_ESP] = gp_stk;
                            env.eip = ade_pc;
                            env.eflags = 0x2;
                            int rc_ade = tb_harness_translate_at_with_map(ade_pc, ade_code, 4096, &ade_map);
                            check(rc_ade > 0, "non-canonical EXC_ADE test block translates");
                            if (rc_ade > 0) {
                                loongemu_sync_icache_range((uint64_t)(uintptr_t)ade_code, (uint64_t)rc_ade);
                                g_current_tb_code = (uint64_t)(uintptr_t)ade_code;
                                g_current_tb_size = (uint32_t)rc_ade;
                                g_current_tb_pc = ade_pc;
                                g_current_tb_map = &ade_map;
                                tb_sys_run_plv3((void *)(uintptr_t)pro_buf, ade_code, &env);
                                g_current_tb_code = 0;
                                g_current_tb_size = 0;
                                g_current_tb_pc = 0;
                                g_current_tb_map = 0;
                                uint64_t ade_stack_error = UINT64_MAX;
                                uint64_t ade_stack_rip = UINT64_MAX;
                                int ade_stack_ok =
                                    guest_linear_read_u64(env.regs[R_ESP], &ade_stack_error) &&
                                    guest_linear_read_u64(env.regs[R_ESP] + 8u, &ade_stack_rip);
                                check(g_hw_guest_gp == gp_before + 1u &&
                                      g_plv3_exit.reason == PLV3_EXIT_GUEST_PF &&
                                      env.eip == gp_isr && ade_stack_ok &&
                                      ade_stack_error == 0ULL && ade_stack_rip == ade_pc + 10u,
                                      "EXC_ADE delivers #GP(0) to guest IDT[13] with exact faulting RIP");
                            }
                            /* P0-D: SS-segment non-canonical access ([rbp+8]) -> #SS(0) IDT[12] */
                            {
                                static const uint8_t ss_x86[] = {
                                    0x48, 0xbd, 0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0xad, 0xde, /* mov rbp, 0xdead000000000120 */
                                    0x48, 0x8b, 0x45, 0x08,                                     /* mov rax, [rbp+8] -> #SS(0)  */
                                    0xf4,
                                };
                                uint64_t ss_pc = 0x0000e940ULL;
                                uint64_t ss_isr = 0x0000e9a0ULL;
                                uint8_t *ss_code = code_cache + 0x14800;
                                uint64_t ss_lo = (ss_isr & 0xffffULL) |
                                                 ((uint64_t)0x08u << 16) |
                                                 ((uint64_t)0x8eu << 40) |
                                                 (((ss_isr >> 16) & 0xffffULL) << 48);
                                uint64_t ss_hi = (ss_isr >> 32) & 0xffffffffULL;
                                TbInsnMap ss_map;
                                (void)guest_linear_write_u64(gp_idt + 12u * 16u, ss_lo);
                                (void)guest_linear_write_u64(gp_idt + 12u * 16u + 8u, ss_hi);
                                memcpy(gpa_to_host_ptr(ss_pc), ss_x86, sizeof(ss_x86));
                                env.regs[R_ESP] = gp_stk;
                                env.eip = ss_pc;
                                env.eflags = 0x2;
                                int rc_ss = tb_harness_translate_at_with_map(ss_pc, ss_code, 4096, &ss_map);
                                if (rc_ss > 0) {
                                    loongemu_sync_icache_range((uint64_t)(uintptr_t)ss_code, (uint64_t)rc_ss);
                                    g_current_tb_code = (uint64_t)(uintptr_t)ss_code;
                                    g_current_tb_size = (uint32_t)rc_ss;
                                    g_current_tb_pc = ss_pc;
                                    g_current_tb_map = &ss_map;
                                    tb_sys_run_plv3((void *)(uintptr_t)pro_buf, ss_code, &env);
                                    g_current_tb_code = 0;
                                    g_current_tb_size = 0;
                                    g_current_tb_pc = 0;
                                    g_current_tb_map = 0;
                                    uint64_t ss_stack_error = UINT64_MAX;
                                    uint64_t ss_stack_rip = UINT64_MAX;
                                    int ss_stack_ok =
                                        guest_linear_read_u64(env.regs[R_ESP], &ss_stack_error) &&
                                        guest_linear_read_u64(env.regs[R_ESP] + 8u, &ss_stack_rip);
                                    check(g_plv3_exit.reason == PLV3_EXIT_GUEST_PF &&
                                          env.eip == ss_isr && ss_stack_ok &&
                                          ss_stack_error == 0ULL && ss_stack_rip == ss_pc + 10u,
                                          "P0-D: SS-segment non-canonical EXC_ADE delivers #SS(0) to IDT[12]");
                                }
                            }
                            env.idt.base = saved_idt_base;
                            env.idt.limit = saved_idt_limit;
                            env.regs[R_ESP] = saved_rsp;
                        }

                        /* Verify 16-bit Real Mode (CS_MODE_16, seg.base=sel<<4, ljmp, 4GiB ROM reset vector) & tb_cache mode isolation. */
                        {
                            static CPUX86State saved_env_rm;
                            uint8_t *rm_code = code_cache + 0x15000;
                            saved_env_rm = env;

                            /* 1. Reset vector at GPA 0xFFFFFFF0 (CS=0xF000, CS.base=0xFFFF0000, IP=0xFFF0). */
                            latx_x86_reset_real_mode(&env);
                            latx_x86_tlb_flush_all();
                            check(env.sys.codemode == LATX_DT_X16 &&
                                  env.segs[R_CS].selector == 0xf000u &&
                                  env.segs[R_CS].base == UINT64_C(0xffff0000) &&
                                  env.eip == 0xfff0u,
                                  "latx_x86_reset_real_mode sets CS=0xF000/base=0xFFFF0000/IP=0xFFF0");

#ifndef LOONGEMU_BOOT_OVMF
                            int rc_rm = tb_harness_translate_at(env.eip, rm_code, 4096);
                            check(rc_rm > 0, "16-bit real-mode OVMF reset vector at 0xFFFFFFF0 translates");
                            if (rc_rm > 0) {
                                loongemu_sync_icache_range((uint64_t)(uintptr_t)rm_code, (uint64_t)rc_rm);
                                env.regs[R_EAX] = 0;
                                env.segs[R_DS].selector = 0;
                                env.segs[R_DS].base = 0;
                                tb_sys_run_plv3((void *)(uintptr_t)pro_buf, rm_code, &env);
                                check((env.regs[R_EAX] & 0xffffu) == 0x1234u &&
                                      env.segs[R_DS].selector == 0x1234u &&
                                      env.segs[R_DS].base == UINT64_C(0x12340),
                                      "16-bit real-mode ROM executes mov ax,0x1234; mov ds,ax (ds.base=0x12340)");
                            }
#endif

                            /* 2. Direct far jump ljmp 0x0008:0x1000 in 16-bit real mode. */
                            {
                                static const uint8_t ljmp16[] = { 0xea, 0x00, 0x10, 0x08, 0x00 };
                                uint64_t lj_pc = 0x00000600ULL;
                                uint8_t *lj_code = code_cache + 0x16000;
                                env.segs[R_CS].selector = 0;
                                env.segs[R_CS].base = 0;
                                env.eip = lj_pc;
                                memcpy(gpa_to_host_ptr(lj_pc), ljmp16, sizeof(ljmp16));
                                int rc_lj = tb_harness_translate_at(lj_pc, lj_code, 4096);
                                check(rc_lj > 0, "16-bit real-mode direct ljmp translates");
                                if (rc_lj > 0) {
                                    loongemu_sync_icache_range((uint64_t)(uintptr_t)lj_code, (uint64_t)rc_lj);
                                    tb_sys_run_plv3((void *)(uintptr_t)pro_buf, lj_code, &env);
                                    check(env.segs[R_CS].selector == 0x0008u &&
                                          env.segs[R_CS].base == 0x80u &&
                                          env.eip == 0x1000u,
                                          "16-bit ljmp updates CS=0x08, CS.base=0x80, IP=0x1000");
                                }
                            }

                            /* 3. tb_cache mode_flags + cs_base isolation at identical PC. */
                            {
                                TbCacheEntry *e16 = tbc_insert_mode(&tb_cache, 0x1000ULL, 0x0ULL, 0ULL, 0u, 0x02u);
                                TbCacheEntry *e64 = tbc_insert_mode(&tb_cache, 0x1000ULL, 0x0ULL, 0ULL, 0u, 0x15u);
                                check(e16 != 0 && e64 != 0 && e16 != e64 &&
                                      tbc_lookup_mode(&tb_cache, 0x1000ULL, 0x0ULL, 0ULL, 0u, 0x02u) == e16 &&
                                      tbc_lookup_mode(&tb_cache, 0x1000ULL, 0x0ULL, 0ULL, 0u, 0x15u) == e64,
                                      "tb_cache isolates 16-bit and 64-bit TBs at identical PC");
                            }

                            env = saved_env_rm;
                        }

                        /* Verify OVMF board peripherals: CMOS/RTC, QEMU fw_cfg PIO+DMA, and CFI pflash. */
                        {
                            /* CMOS/RTC memory sizing & SMP count */
                            latx_platform_io_write(LOONGEMU_CMOS_INDEX_PORT, 1, 0x15u);
                            uint32_t base_kb = (uint32_t)latx_platform_io_read(LOONGEMU_CMOS_DATA_PORT, 1);
                            latx_platform_io_write(LOONGEMU_CMOS_INDEX_PORT, 1, 0x16u);
                            base_kb |= ((uint32_t)latx_platform_io_read(LOONGEMU_CMOS_DATA_PORT, 1) << 8);
                            latx_platform_io_write(LOONGEMU_CMOS_INDEX_PORT, 1, 0x34u);
                            uint32_t ext64k = (uint32_t)latx_platform_io_read(LOONGEMU_CMOS_DATA_PORT, 1);
                            latx_platform_io_write(LOONGEMU_CMOS_INDEX_PORT, 1, 0x35u);
                            ext64k |= ((uint32_t)latx_platform_io_read(LOONGEMU_CMOS_DATA_PORT, 1) << 8);
                            latx_platform_io_write(LOONGEMU_CMOS_INDEX_PORT, 1, 0x5fu);
                            uint32_t cmos_smp = (uint32_t)latx_platform_io_read(LOONGEMU_CMOS_DATA_PORT, 1);
                            check(base_kb == 640u && ext64k == ((GUEST_RAM_SIZE - 0x1000000u) / 65536u) &&
                                  cmos_smp == (MAX_VCPUS - 1u),
                                  "CMOS/RTC reports 640K base, above-16M RAM, and SMP count");

                            /* fw_cfg PIO: signature, ID, NB_CPUS */
                            latx_platform_io_write(LOONGEMU_FW_CFG_SEL_PORT, 2, FW_CFG_SIGNATURE);
                            uint32_t fw_sig = (uint32_t)latx_platform_io_read(LOONGEMU_FW_CFG_DATA_PORT, 4);
                            latx_platform_io_write(LOONGEMU_FW_CFG_SEL_PORT, 2, FW_CFG_ID);
                            uint32_t fw_id = (uint32_t)latx_platform_io_read(LOONGEMU_FW_CFG_DATA_PORT, 4);
                            latx_platform_io_write(LOONGEMU_FW_CFG_SEL_PORT, 2, FW_CFG_NB_CPUS);
                            uint16_t fw_cpus = (uint16_t)latx_platform_io_read(LOONGEMU_FW_CFG_DATA_PORT, 2);
                            check(fw_sig == 0x554d4551u && fw_id == 3u && fw_cpus == MAX_VCPUS,
                                  "QEMU fw_cfg PIO returns QEMU signature, DMA ID=3, and NB_CPUS");

                            /* fw_cfg DMA: SELECT(FW_CFG_SIGNATURE) | READ into guest RAM (cross-page) */
                            uint64_t dma_desc = 0x00019000ULL;
                            uint64_t dma_dst = 0x00019ffeULL; /* crosses 0x19fff -> 0x1a000 */
                            uint32_t ctrl_be = __builtin_bswap32(((uint32_t)FW_CFG_SIGNATURE << 16) |
                                                                 FW_CFG_DMA_CTL_SELECT |
                                                                 FW_CFG_DMA_CTL_READ);
                            uint32_t err_be = __builtin_bswap32(FW_CFG_DMA_CTL_ERROR);
                            uint32_t len_be = __builtin_bswap32(4u);
                            latx_platform_phys_write(dma_dst, 4, 0u);
                            latx_platform_phys_write(dma_desc + 0u, 4, ctrl_be);
                            latx_platform_phys_write(dma_desc + 4u, 4, len_be);
                            latx_platform_phys_write(dma_desc + 8u, 8, __builtin_bswap64(dma_dst));
                            latx_platform_io_write(LOONGEMU_FW_CFG_DMA_HI_PORT, 4, 0u);
                            latx_platform_io_write(LOONGEMU_FW_CFG_DMA_LO_PORT, 4,
                                                   __builtin_bswap32((uint32_t)dma_desc));
                            check(latx_platform_phys_read(dma_desc, 4) == 0u &&
                                  latx_platform_phys_read(dma_dst, 4) == 0x554d4551u,
                                  "QEMU fw_cfg DMA cross-page READ|SELECT transfers QEMU signature");

                            /* P0-B negative tests: OOB RAM, read-only ROM, oversized len, invalid descriptor */
                            latx_platform_phys_write(dma_desc + 0u, 4, ctrl_be);
                            latx_platform_phys_write(dma_desc + 4u, 4, len_be);
                            latx_platform_phys_write(dma_desc + 8u, 8, __builtin_bswap64(0x20000000ULL));
                            latx_platform_io_write(LOONGEMU_FW_CFG_DMA_HI_PORT, 4, 0u);
                            latx_platform_io_write(LOONGEMU_FW_CFG_DMA_LO_PORT, 4,
                                                   __builtin_bswap32((uint32_t)dma_desc));
                            check(latx_platform_phys_read(dma_desc, 4) == err_be,
                                  "P0-B: fw_cfg DMA rejects out-of-bounds GPA with ERROR bit");

                            latx_platform_phys_write(dma_desc + 0u, 4, ctrl_be);
                            latx_platform_phys_write(dma_desc + 4u, 4, len_be);
                            latx_platform_phys_write(dma_desc + 8u, 8, __builtin_bswap64(0xffff0000ULL));
                            latx_platform_io_write(LOONGEMU_FW_CFG_DMA_HI_PORT, 4, 0u);
                            latx_platform_io_write(LOONGEMU_FW_CFG_DMA_LO_PORT, 4,
                                                   __builtin_bswap32((uint32_t)dma_desc));
                            check(latx_platform_phys_read(dma_desc, 4) == err_be,
                                  "P0-B: fw_cfg DMA rejects read-only ROM target with ERROR bit");

                            latx_platform_phys_write(dma_desc + 0u, 4, ctrl_be);
                            latx_platform_phys_write(dma_desc + 4u, 4, __builtin_bswap32(0x01000000u));
                            latx_platform_phys_write(dma_desc + 8u, 8, __builtin_bswap64(0x00019040ULL));
                            latx_platform_io_write(LOONGEMU_FW_CFG_DMA_HI_PORT, 4, 0u);
                            latx_platform_io_write(LOONGEMU_FW_CFG_DMA_LO_PORT, 4,
                                                   __builtin_bswap32((uint32_t)dma_desc));
                            check(latx_platform_phys_read(dma_desc, 4) == err_be,
                                  "P0-B: fw_cfg DMA rejects oversized length (>1MiB) with ERROR bit");

                            /* Invalid descriptor GPA must not fault or hang */
                            latx_platform_io_write(LOONGEMU_FW_CFG_DMA_HI_PORT, 4, 0u);
                            latx_platform_io_write(LOONGEMU_FW_CFG_DMA_LO_PORT, 4,
                                                   __builtin_bswap32(0x20000000u));

                            /* P1: unified virtual wallclock consistency (CMOS/RTC & KVM wallclock) */
                            {
                                uint64_t wc_gpa = 0x00019100ULL;
                                latx_platform_kvm_wrmsr(&env, 0x4b564d00u, wc_gpa);
                                uint32_t wc_sec = (uint32_t)latx_platform_phys_read(wc_gpa + 4u, 4);
                                latx_platform_io_write(LOONGEMU_CMOS_INDEX_PORT, 1, 0x09u);
                                uint8_t rtc_yr = (uint8_t)latx_platform_io_read(LOONGEMU_CMOS_DATA_PORT, 1);
                                check(wc_sec >= 1767225600u && rtc_yr == 0x26u,
                                      "P1: KVM wallclock and CMOS/RTC share unified 2026 virtual epoch");
                            }

                            /* P1: tbc_invalidate_range uses guest_size rather than host code_size */
                            {
                                TbCacheEntry *et = tbc_insert_mode(&tb_cache, 0x2000ULL, 0ULL, 0x3000ULL, 0u, 0x15u);
                                if (et) {
                                    et->code_size = 8192u;  /* large host code_size */
                                    et->guest_size = 16u;   /* small guest_size on page 0x2000 */
                                }
                                tbc_invalidate_range(&tb_cache, 0x3000ULL, 0x3000ULL, 0x1000ULL);
                                check(tbc_lookup_mode(&tb_cache, 0x2000ULL, 0ULL, 0x3000ULL, 0u, 0x15u) == et,
                                      "P1: tbc_invalidate_range ignores host code_size past guest_size");
                                tbc_invalidate_range(&tb_cache, 0x3000ULL, 0x2000ULL, 0x1000ULL);
                                check(tbc_lookup_mode(&tb_cache, 0x2000ULL, 0ULL, 0x3000ULL, 0u, 0x15u) == 0,
                                      "P1: tbc_invalidate_range evicts TB overlapping guest_size");
                            }

                            /* CFI pflash (OVMF_VARS.fd) query, erase, program, read-array */
                            ovmf_pflash_mmio_write(&g_ovmf_board.vars_flash, 0u, 0x98u, 1u);
                            uint8_t q = (uint8_t)ovmf_pflash_mmio_read(&g_ovmf_board.vars_flash, 0x10u, 1u);
                            uint8_t r = (uint8_t)ovmf_pflash_mmio_read(&g_ovmf_board.vars_flash, 0x11u, 1u);
                            uint8_t y = (uint8_t)ovmf_pflash_mmio_read(&g_ovmf_board.vars_flash, 0x12u, 1u);
                            ovmf_pflash_mmio_write(&g_ovmf_board.vars_flash, 0u, 0x20u, 1u);
                            ovmf_pflash_mmio_write(&g_ovmf_board.vars_flash, 0u, 0xd0u, 1u);
                            ovmf_pflash_mmio_write(&g_ovmf_board.vars_flash, 0x10u, 0x40u, 1u);
                            ovmf_pflash_mmio_write(&g_ovmf_board.vars_flash, 0x10u, 0x5au, 1u);
                            ovmf_pflash_mmio_write(&g_ovmf_board.vars_flash, 0u, 0xffu, 1u);
                            uint8_t pf_val = (uint8_t)ovmf_pflash_mmio_read(&g_ovmf_board.vars_flash, 0x10u, 1u);
                            check(q == 'Q' && r == 'R' && y == 'Y' && pf_val == 0x5au,
                                  "OVMF_VARS CFI pflash supports QRY query, erase, and program");
                        }

                        env.cr[0] = saved_cr0_r;
                        env.eip = saved_eip_r;
                        latx_x86_tlb_flush_all();
                    }
                    memcpy(gpa_to_host_ptr(initramfs_gpa), _initramfs_start,
                           initramfs_sz);
                    memset(bp, 0, 4096);
                    memcpy(gpa_to_host_ptr(cmdline_gpa), cmdline,
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

#ifndef LOONGEMU_BOOT_OVMF
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
                            boot_physical_aps();
                            int ran = run_blocks_from(gva + 0x90, 4194304,
                                                      &stop, 1, 0);

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
                            latx_bare_uart_puts(" sync=");
                            puthex(g_sync_core_flushes);
                            latx_bare_uart_puts("/");
                            puthex(g_sync_core_calls);
                            latx_bare_uart_puts(" ticks=");
                            puthex(g_timer_delivered);
                            latx_bare_uart_puts(" irqs=");
                            puthex(g_timer_ticks);
                            latx_bare_uart_puts(" sw_lbt_traps=");
                            puthex(g_sw_lbt_traps);
                            latx_bare_uart_puts(" asid_hits=");
                            puthex(g_asid_hits);
                            latx_bare_uart_puts(" asid_allocs=");
                            puthex(g_asid_allocs);
                            latx_bare_uart_puts(" smc_evicts=");
                            puthex(g_smc_granular_evicts);
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
#endif

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
     * The low 128 MiB belongs to the guest and the VMM starts above it.  A
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
        memset(gpa_to_host_ptr(probe), 0x90, 256);   /* nop sled */
        emitted = tb_harness_translate_at(probe, code_cache, 1024);
        env.cr[0] = saved_cr0;
        latx_x86_tlb_flush_all();
        check(emitted > 0, "guest RAM reaches 128 MiB");
        check(g_bad_fetch == 0, "no fetch outside guest RAM");
    }

#ifdef LOONGEMU_BOOT_OVMF
    {
        uint64_t stop = 0;
        latx_bare_uart_puts("tb-sys: booting real OVMF_CODE.fd rom_base=");
        puthex(g_ovmf_board.rom_gpa_base);
        latx_bare_uart_puts(" rom_sz=");
        puthex(g_ovmf_board.rom_size);
        latx_bare_uart_puts(" vars_base=");
        puthex(g_ovmf_board.vars_flash.gpa_base);
        latx_bare_uart_puts(" vars_sz=");
        puthex(g_ovmf_board.vars_flash.size);
        latx_bare_uart_puts("\n");

        if (fails != 0) {
            latx_platform_refuse("integrated VMM selftests failed");
        }

        /* Clear guest RAM of prior self-test artifacts before real OVMF boot. */
        memset(gpa_to_host_ptr(GUEST_BASE), 0, (size_t)g_guest_ram_size);
        latx_guest_nr_cpus = 1u;
        latx_x86_reset_real_mode(&env);
        latx_x86_tlb_flush_all();
        tbc_flush_all(&tb_cache);
        int ran = run_blocks_from(0xfff0ULL, 4194304, &stop, 1, 1);
        latx_bare_uart_puts("tb-sys: ovmf_blocks_run=");
        puthex((uint64_t)(ran < 0 ? 0 : ran));
        latx_bare_uart_puts(" stop=");
        puthex(stop);
        latx_bare_uart_puts(" cs=");
        puthex(env.segs[R_CS].selector);
        latx_bare_uart_puts(" cs_base=");
        puthex(env.segs[R_CS].base);
        latx_bare_uart_puts(" cr0=");
        puthex(env.cr[0]);
        latx_bare_uart_puts(" efer=");
        puthex(env.efer);
        latx_bare_uart_puts(" mode=");
        puthex((uint64_t)env.sys.codemode);
        latx_bare_uart_puts(" run_exit=");
        puthex(g_run_blocks_exit);
        latx_bare_uart_puts(" plv3_exit=");
        puthex(g_plv3_exit.reason);
        latx_bare_uart_puts("\n");
        if (g_plv3_exit.reason != PLV3_EXIT_NORMAL || g_run_blocks_exit != 0) {
            uint64_t n = g_recent_count < 8 ? g_recent_count : 8;
            uint64_t start = g_recent_count - n;
            latx_bare_uart_puts("tb-sys: ovmf_abort ecode=");
            puthex(g_plv3_exit.ecode);
            latx_bare_uart_puts(" esub=");
            puthex(g_plv3_exit.esubcode);
            latx_bare_uart_puts(" era=");
            puthex(g_plv3_exit.era);
            latx_bare_uart_puts(" badv=");
            puthex(g_plv3_exit.badv);
            latx_bare_uart_puts(" grip=");
            puthex(g_plv3_exit.guest_rip);
            latx_bare_uart_puts("\n");
            latx_bare_uart_puts("tb-sys: ovmf_regs rax=");
            puthex(env.regs[R_EAX]);
            latx_bare_uart_puts(" rsp=");
            puthex(env.regs[R_ESP]);
            latx_bare_uart_puts(" rbp=");
            puthex(env.regs[R_EBP]);
            latx_bare_uart_puts(" cr2=");
            puthex(env.cr[2]);
            latx_bare_uart_puts(" cr3=");
            puthex(env.cr[3]);
            latx_bare_uart_puts(" idt=");
            puthex(env.idt.base);
            latx_bare_uart_puts(" gdt=");
            puthex(env.gdt.base);
            latx_bare_uart_puts("\n");
            latx_bare_uart_puts("tb-sys: ovmf_uniq_pcs=");
            uint64_t un = g_ovmf_uniq_count < 16 ? g_ovmf_uniq_count : 16;
            uint64_t us = g_ovmf_uniq_count - un;
            for (uint64_t idx = us; idx < g_ovmf_uniq_count; idx++) {
                if (idx > us) latx_bare_uart_puts(" ");
                puthex(g_ovmf_uniq_pcs[idx & 15]);
                latx_bare_uart_puts(":");
                puthex((uint64_t)g_ovmf_uniq_cs[idx & 15]);
            }
            latx_bare_uart_puts("\n");
        }
        check(ran >= 1, "real OVMF_CODE.fd executes at least one block from reset vector");
        latx_bare_uart_puts("tb-sys: checks=");
        puthex((uint64_t)checks);
        latx_bare_uart_puts(" fails=");
        puthex((uint64_t)fails);
        latx_bare_uart_puts(fails ? " BAD\n" : " ok\n");
        if (fails != 0) {
            latx_platform_refuse("integrated VMM selftests failed");
        }
        if (g_plv3_exit.reason != PLV3_EXIT_NORMAL ||
            g_run_blocks_exit != RUN_EXIT_MAX_BLOCKS) {
            latx_platform_refuse("OVMF runner stopped before live execution");
        }
        latx_bare_uart_puts("tb-sys: ovmf_run_live=1 rip=");
        puthex(env.eip);
        latx_bare_uart_puts("\n");
        for (;;) {
            (void)run_blocks_from(env.eip, 4194304, &stop, 0, 1);
            if (g_plv3_exit.reason != PLV3_EXIT_NORMAL ||
                g_run_blocks_exit != RUN_EXIT_MAX_BLOCKS) {
                latx_bare_uart_puts("tb-sys: ovmf_persistent_abort exit=");
                puthex(g_run_blocks_exit);
                latx_bare_uart_puts(" reason=");
                puthex(g_plv3_exit.reason);
                latx_bare_uart_puts(" rip=");
                puthex(env.eip);
                latx_bare_uart_puts("\n");
                latx_platform_refuse("persistent OVMF runner stopped");
            }
        }
    }
#endif

    latx_bare_uart_puts("tb-sys: checks=");
    puthex((uint64_t)checks);
    latx_bare_uart_puts(" fails=");
    puthex((uint64_t)fails);
    latx_bare_uart_puts(fails ? " BAD\n" : " ok\n");
    latx_bare_uart_puts("tb-sys: done\n");
    for (;;) {
        (void)hv_service_flush_requests();
        (void)hv_recycle_tb_shard();
        __asm__ volatile("dbar 0x700" ::: "memory");
    }
}
