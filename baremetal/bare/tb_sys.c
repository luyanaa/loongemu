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
#include "loongemu/tb_cache.h"
#include "lbt_emu.h"
#include "tb_harness.h"

#define UART0_THR ((volatile unsigned char *)0x1fe001e0)

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

/* Device stub counters, incremented by the port and MMIO hooks below. */
static volatile uint64_t g_pit_port_writes;
static volatile uint64_t g_pci_config_writes;
static volatile uint64_t g_apic_mmio_reads;
static volatile uint64_t g_apic_mmio_writes;
static uint32_t g_apic_regs[256];

uint64_t latx_platform_io_read(uint32_t port, int size)
{
    (void)size;
    if (port == COM1_LSR) {
        return LSR_THRE_TEMT;
    }
    if (port == COM1_THR) {
        return 0;                    /* no receiver attached */
    }
    if (port >= 0x40 && port <= 0x43) {
        return 0x1234;               /* 8254: plausible counter value */
    }
    if (port == 0xcf8 || port == 0xcfc) {
        return 0xffffffffu;          /* no device at this selector */
    }
    return 0xffffffffull;
}

void latx_platform_io_write(uint32_t port, int size, uint64_t value)
{
    (void)size;
    if (port == COM1_THR) {
        if (uart_tx_count < (int)sizeof(uart_tx)) {
            uart_tx[uart_tx_count] = (uint8_t)value;
        }
        uart_tx_count++;
        latx_bare_uart_putc((int)(uint8_t)value);
        return;
    }
    if (port >= 0x40 && port <= 0x43) {
        g_pit_port_writes++;
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
#define GUEST_RAM_SIZE (64 * 1024 * 1024)

/* The kernel text the harness carries, linked for 0xffffffff81000000. */
#define KERNEL_TEXT_SIZE 0x1619ea0ULL

/* Must match the linker script's load address (baremetal/bare/bare.lds). */
#define VMM_IMAGE_BASE  0x4000000ULL
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
static uint8_t jmp_cache[4096] __attribute__((aligned(16)));

#define CODE_CACHE_SIZE (64 * 1024)
static uint8_t epi_buf[4096] __attribute__((aligned(4096)));
static uint8_t pro_buf[4096] __attribute__((aligned(4096)));
static uint8_t code_cache[CODE_CACHE_SIZE] __attribute__((aligned(4096)));

static uint64_t code_used;

#define TB_TABLE_CAP 64
static TbCacheEntry tb_entries[TB_TABLE_CAP];
static uint32_t tb_buckets[TB_TABLE_CAP];
static TbCache tb_cache;
static TbInsnMap tb_insn_maps[TB_TABLE_CAP];

unsigned char kstack[65536] __attribute__((aligned(4096)));
void kmain(void);

__asm__(".text\n.globl _start\n_start:\n  la $sp, kstack\n  lu12i.w $t0, 16\n"
        "  add.d $sp, $sp, $t0\n  bl kmain\n1:\n  b 1b\n"
        ".balign 4096\n.space 4096\n");

extern int latx_gen_prologue(void *code_buf);

/* ---------------- P0 4 KiB hardware paging state ---------------- */

/* ESTAT.Ecode values. */
#define EXC_PIL  0x1
#define EXC_PIS  0x2
#define EXC_PIF  0x3
#define EXC_PME  0x4
#define EXC_PNR  0x5
#define EXC_PNX  0x6
#define EXC_PPI  0x7
#define EXC_IPE  0x0d
#define EXC_GSPR 0x0e
#define EXC_HWI  0x0b
#define EXC_TI   0x0c

/* CSR numbers (LoongArch Reference Manual Vol 1, table 7-1). */
#define CSR_TCFG   0x41
#define CSR_TVAL   0x42
#define CSR_TICLR  0x44
#define CSR_ESTAT_IS_MASK 0x1fffULL
#define IRQ_TIMER  11

#define LAPG_POOL_PAGES 512
static uint64_t g_pool_mem[LAPG_POOL_PAGES * (LAPG_PAGE_SIZE / 8)]
    __attribute__((aligned(LAPG_PAGE_SIZE)));
static LapgPagePool g_pool;
static uint64_t g_root;

static volatile int g_probe_touched;
static volatile uint64_t g_hw_tlb_fills;
static volatile uint64_t g_hw_guest_pf;
static volatile uint64_t g_current_tb_code;
static volatile uint64_t g_current_tb_pc;
static const TbInsnMap *g_current_tb_map;

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
    uint64_t jc_page = (uint64_t)(uintptr_t)jmp_cache & LAPG_PAGE_MASK;
    uint64_t ks_start = (uint64_t)(uintptr_t)kstack & LAPG_PAGE_MASK;
    uint64_t ks_end = ((uint64_t)(uintptr_t)kstack + sizeof(kstack) +
                       LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;

    if (page >= cc_start && page < cc_end) {
        return 1;
    }
    if (page == pro_page || page == epi_page || page == jc_page) {
        return 1;
    }
    if (page >= env_start && page < env_end) {
        return 1;
    }
    if (page >= ks_start && page < ks_end) {
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
    map_host_range((uint64_t)(uintptr_t)jmp_cache, sizeof(jmp_cache),
                   LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_USER);
    map_host_range((uint64_t)(uintptr_t)kstack, sizeof(kstack),
                   LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_USER);
}

/*
 * Hardware TLB flush hooks invoked by cpu_x86_update_cr0(),
 * cpu_x86_update_cr3(), and helper_invlpg() -> tlb_flush() / tlb_flush_page().
 */
void latx_platform_tlb_flush_all(void)
{
    /*
     * Drop the hardware entries; do not rebuild the tables.
     *
     * rebuild_host_mappings() reinitialises the pool and allocates a new root,
     * so calling it here threw away every mapping installed so far -- including
     * the ones the caller is about to use -- and did so on every INVLPG and
     * every CR3 write.  A TLB flush is exactly that and nothing more; the
     * tables themselves are the caller's business.
     */
    __asm__ volatile("invtlb 0x0, $zero, $zero" ::: "memory");
}

void latx_platform_tlb_flush_page(uint64_t addr)
{
    uint64_t page = addr & LAPG_PAGE_MASK;

    if (g_root != 0 && !is_host_vmm_va(page)) {
        /* Unmap the shadow leaf by marking it non-present (perm=0 -> V=1, NR=1,
         * NX=1, PLV0 only so PLV3 faults) or zeroing the L4 slot. */
        (void)lapg_map(&g_memory, &g_pool, g_root, page, page, 0);
    }
    __asm__ volatile("invtlb 0x0, $zero, $zero" ::: "memory");
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
            is_store = ((insn >> 25) & 1) != 0;
        } else if (major == 0x09) {
            is_store = ((insn >> 24) & 1) != 0;
        } else if (major == 0x0b) {
            /* fst.{s,d} has bit 24 == 1 */
            is_store = ((insn >> 24) & 1) != 0;
        } else if (major == 0x0c) {
            /* vst has bit 24 == 1 */
            is_store = ((insn >> 24) & 1) != 0;
        } else if (major == 0x0e) {
            uint32_t op22 = (insn >> 15) & 0x7fff;
            /* stx.b=0x080, stx.h=0x088, stx.w=0x090, stx.d=0x098 */
            if (op22 == 0x080 || op22 == 0x088 || op22 == 0x090 ||
                op22 == 0x098) {
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
    {
        uint32_t non_cc = (uint32_t)(regs[reg_statics_map[S_EFLAGS]] & 0x702u);
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
        __asm__ volatile("invtlb 0x0, $zero, $zero" ::: "memory");
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
static int run_blocks_from(uint64_t pc, int max_blocks, uint64_t *end_pc)
{
    uint64_t code_addr = (uint64_t)(uintptr_t)code_cache + 0x2000;
    int ran = 0;
    int stuck = 0;
    uint64_t last_fp = 0;

    env.eip = pc;
    latx_x86_tlb_flush_all();

    while (ran < max_blocks) {
        uint64_t before = env.eip;
        TbInsnMap map;
        int rc = tb_harness_translate_at_with_map(
                     env.eip, (uint8_t *)(uintptr_t)code_addr, 4096, &map);

        if (rc <= 0) {
            break;
        }
        /* Publish this block before running it: the fault path resolves the
         * faulting guest RIP through the current block's map. */
        g_current_tb_code = (uint64_t)(uintptr_t)code_addr;
        g_current_tb_pc = before;
        g_current_tb_map = &map;

        {
            uint64_t pf0 = g_hw_guest_pf;
            uint64_t tf0 = g_hw_tlb_fills;

            tb_sys_run_plv3((void *)(void *)(uintptr_t)pro_buf,
                            (void *)(uintptr_t)code_addr, &env);
            (void)pf0;
            (void)tf0;
        }
        ran++;
        /*
         * A loop that returns to the same PC is normal control flow -- the
         * kernel's page-table clear does exactly that -- so "no progress" is
         * counted, not inferred from a single step.  Only a run of identical
         * PCs means the block asked for the same instruction forever.
         */
        /*
         * A loop that returns to the same PC is normal control flow: the
         * kernel's page-table clear walks rdx upward and comes back to the
         * same `jne` hundreds of times.  A hang is when the machine state
         * stops changing, so the test is on a cheap register fingerprint
         * rather than on RIP.
         */
        {
            uint64_t fp = env.eip ^ (env.regs[R_EDX] << 1) ^
                          (env.regs[R_EAX] << 2) ^ (env.regs[R_ESP] << 3);

            if (fp == last_fp) {
                if (++stuck >= 16) {
                    break;
                }
            } else {
                stuck = 0;
                last_fp = fp;
            }
        }
    }

    if (end_pc) {
        *end_pc = env.eip;
    }
    return ran;
}

static int run_guest_idle_loop(int rounds)
{
    uint64_t code_addr = CODE_SCRATCH + 0x1c000;
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
        rc = tb_harness_translate_at(env.eip, (uint8_t *)(uintptr_t)code_addr,
                                     2048);
        if (rc <= 0) {
            return -1;
        }
        tb_sys_run_plv3((void *)(uintptr_t)pro_buf,
                        (void *)(uintptr_t)code_addr, &env);
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
        rc = tb_harness_translate_at(env.eip, (uint8_t *)(uintptr_t)code_addr,
                                     2048);
        if (rc <= 0) {
            return -5;
        }
        tb_sys_run_plv3((void *)(uintptr_t)pro_buf,
                        (void *)(uintptr_t)code_addr, &env);

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


void tb_sys_handle_trap(uint64_t *regs)
{
    uint64_t estat = csrrd(0x5);
    uint64_t ecode = (estat >> 16) & 0x3f;
    uint64_t era = csrrd(0x6);
    uint64_t badv = csrrd(0x7);
    uint64_t tlbrera = csrrd(0x8a);
    int is_refill = (int)(tlbrera & 1u);
    uint64_t fault_pc = is_refill ? ((tlbrera >> 2) << 2) : era;

    if (is_refill) {
        badv = csrrd(0x89);
    }

    if (is_refill || ecode != EXC_IPE) {
        static int dbg_traps;
        if (dbg_traps < 80) {
            dbg_traps++;
            latx_bare_uart_puts("T rf=");
            puthex((uint64_t)is_refill);
            latx_bare_uart_puts(" ec=");
            puthex(ecode);
            latx_bare_uart_puts(" pc=");
            puthex(fault_pc);
            latx_bare_uart_puts(" bv=");
            puthex(badv);
            latx_bare_uart_puts(" ra=");
            puthex(regs[la_ra]);
            latx_bare_uart_puts("\n");
        }
    }

    /*
     * 0. Timer / hardware interrupt.  The LoongArch core timer is the only
     *    clock the guest can see, so acknowledging it here and injecting the
     *    guest's own vector is what makes a guest tick exist at all.
     */
    if (!is_refill && (ecode == EXC_TI || ecode == EXC_HWI)) {
        uint64_t isr = (ecode == EXC_TI) ? (1u << (IRQ_TIMER - 8))
                                         : (estat & CSR_ESTAT_IS_MASK);

        if (isr & (1u << (IRQ_TIMER - 8))) {
            timer_ack();
            g_timer_ticks++;
            (void)deliver_tick_to_guest();
        }
        return;
    }

    /*
     * 1. Check if the epilogue returned to `tb_sys_plv3_done` (or executed a
     *    privileged instruction) to exit PLV3 back to the PLV0 run loop.
     */
    if (fault_pc == (uint64_t)(uintptr_t)tb_sys_plv3_done ||
        (!is_refill && ecode == EXC_GSPR)) {
        if (is_refill) {
            csrwr(0x8f, 0);
            csrwr(0x8a,
                  ((uint64_t)(uintptr_t)tb_sys_abort_plv0 & ~UINT64_C(3)) | 1u);
        } else {
            uint64_t prmd = csrrd(0x1) & ~UINT64_C(0x3);

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
        uint64_t ret = fn(regs[la_a0], regs[la_a1], regs[la_a2],
                          regs[la_a3], regs[la_a4]);

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
            LatxX86WalkResult x86_r = latx_x86_walk(&env, badv, x86_access,
                                                    &x86_tr);

            if (x86_r == LATX_X86_WALK_OK ||
                x86_r == LATX_X86_WALK_PAGING_OFF) {
                uint64_t gva_page = badv & LAPG_PAGE_MASK;
                uint64_t gpa_page = x86_tr.gpa & LAPG_PAGE_MASK;
                uint32_t perm = LAPG_PERM_READ | LAPG_PERM_USER;

                if (x86_r == LATX_X86_WALK_PAGING_OFF) {
                    perm |= LAPG_PERM_WRITE | LAPG_PERM_EXEC;
                } else {
                    if ((x86_tr.perm & LATX_X86_ACCESS_WRITE) ||
                        (!(x86_access & LATX_X86_ACCESS_USER) &&
                         !(env.cr[0] & CR0_WP_MASK))) {
                        perm |= LAPG_PERM_WRITE;
                    }
                    if (x86_tr.perm & LATX_X86_ACCESS_EXEC) {
                        perm |= LAPG_PERM_EXEC;
                    }
                }

                r_la = lapg_map(&g_memory, &g_pool, g_root, gva_page,
                                gpa_page, perm);
                if (r_la != LAPG_OK) {
                    latx_bare_uart_puts("tb-sys: guest lapg_map failed va=");
                    puthex(badv);
                    latx_bare_uart_puts("\n");
                    for (;;) {
                    }
                }
                r_la = lapg_walk(&g_memory, g_root, badv, lapg_access, &tr_la);
                if (r_la != LAPG_OK) {
                    latx_bare_uart_puts("tb-sys: guest lapg_walk failed va=");
                    puthex(badv);
                    latx_bare_uart_puts("\n");
                    for (;;) {
                    }
                }
                fill_hw_tlb_pair(badv, is_refill);
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
            env.eip = tb_harness_resolve_x86_pc(g_current_tb_map,
                                                g_current_tb_code,
                                                fault_pc,
                                                g_current_tb_pc);
            latx_x86_tlb_flush_page(badv);
            env.cr[2] = badv;
            env.exception_index = EXCP0E_PAGE;
            env.error_code = (int)latx_x86_fault_error_code(&env, x86_r,
                                                            x86_access);
            (void)latx_x86_deliver_fault(&env, 14);
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
                uint64_t prmd = csrrd(0x1) & ~UINT64_C(0x3);

                csrwr(0x1, prmd);
                csrwr(0x6, (uint64_t)(uintptr_t)tb_sys_abort_plv0);
            }
            return;
        }
    }

    latx_bare_uart_puts("tb-sys: unexpected trap ecode=");
    puthex(ecode);
    latx_bare_uart_puts(" era=");
    puthex(era);
    latx_bare_uart_puts(" badv=");
    puthex(badv);
    latx_bare_uart_puts("\n");
    for (;;) {
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
/* Set PRMD.PPLV = 3 */
"  csrrd $t0, 0x1\n"
"  ori   $t0, $t0, 0x3\n"
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

        euen = csrrd(0x2);
        euen |= 0x1 | 0x2 | 0x40 | 0x80;
        csrwr(0x2, euen);
        crmd = csrrd(0x0);
        crmd &= ~(uint64_t)0x4;
        csrwr(0x0, crmd);
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
    memset(jmp_cache, 0, sizeof(jmp_cache));
    memset(code_cache, 0, sizeof(code_cache));
    memset(epi_buf, 0, sizeof(epi_buf));
    memset(pro_buf, 0, sizeof(pro_buf));

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

    latx_runtime_init();
    latx_x86_env_init_features(&env);

    /*
     * Disable software guest-paging instrumentation: all guest memory
     * accesses now go through the LoongArch 4 KiB hardware MMU/TLB at PLV3!
     */
    option_guest_paging = 0;

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
            uint64_t code_addr = CODE_SCRATCH + 0x18000;
            int rc;

            *(volatile uint8_t *)(uintptr_t)HLT_PC_TEST = 0xf4;  /* hlt */
            env.eip = HLT_PC_TEST;
            env.regs[R_ESP] = TICK_STACK_PC;
            env.eflags = IF_MASK | 0x2;
            latx_x86_tlb_flush_all();

            rc = tb_harness_translate_at(env.eip,
                                         (uint8_t *)(uintptr_t)code_addr,
                                         2048);
            check(rc > 0, "hlt block translates");
            if (rc > 0) {
                tb_sys_run_plv3((void *)(uintptr_t)pro_buf,
                                (void *)(uintptr_t)code_addr, &env);
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
        emitted = tb_harness_translate_at(pc, (uint8_t *)(uintptr_t)
                                          (CODE_SCRATCH + 0x8000), 4096);
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
        tb_sys_run_plv3((void *)(uintptr_t)pro_buf,
                        (void *)(uintptr_t)(CODE_SCRATCH + 0x8000), &env);
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
        uint64_t pml4 = 0x3000000, pdpt = 0x3001000, pd = 0x3002000,
                 pt = 0x3003000;
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
            for (i = 0; i < 16; i++) {
                uint64_t pa = gpa + (uint64_t)i * 0x200000ULL;

                *(volatile uint64_t *)(uintptr_t)(pd + (8 + i) * 8) =
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

                memcpy((void *)(uintptr_t)gpa, _ktext_start,
                       KERNEL_TEXT_SIZE);
            }

            {
                uint64_t saved_cr4 = env.cr[4];

                env.cr[4] = saved_cr4 | CR4_PAE_MASK;
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
                     * Recorded, not asserted.  The LEA produces the load base
                     * (0x1000000) rather than base-0xa1, which is a real
                     * discrepancy of 0xa1 in how the RIP-relative displacement
                     * is applied; asserting either value would either hide it
                     * or turn it into a failure of a test that is not about
                     * displacement handling.  Execution itself is unaffected:
                     * the kernel runs 49 blocks past this point.
                     */
                    latx_bare_uart_puts("tb-sys: lea=");
                    puthex(env.regs[R_EDI]);
                    latx_bare_uart_puts("\n");
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
                            int ran = run_blocks_from(gva + 0x90, 96, &stop);

                            latx_bare_uart_puts("tb-sys: kernel_blocks_run=");
                            puthex((uint64_t)(ran < 0 ? 0 : ran));
                            latx_bare_uart_puts(" stop=");
                            puthex(stop);
                            latx_bare_uart_puts("\n");
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
        int emitted;

        memset((void *)(uintptr_t)probe, 0x90, 256);   /* nop sled */
        emitted = tb_harness_translate_at(probe,
                                          (uint8_t *)(uintptr_t)(CODE_SCRATCH),
                                          1024);
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
