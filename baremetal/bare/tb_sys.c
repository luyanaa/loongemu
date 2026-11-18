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

uint64_t latx_platform_io_read(uint32_t port, int size)
{
    (void)size;
    if (port == COM1_LSR) {
        return LSR_THRE_TEMT;
    }
    if (port == COM1_THR) {
        return 0;                    /* no receiver attached */
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
    }
}

/* ---------------- guest memory ---------------- */

#define GUEST_RAM_SIZE (64 * 1024)
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
    if (vaddr >= GUEST_RAM_SIZE) {
        g_bad_fetch++;
    }
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

#define LAPG_POOL_PAGES 512
static uint64_t g_pool_mem[LAPG_POOL_PAGES * (LAPG_PAGE_SIZE / 8)]
    __attribute__((aligned(LAPG_PAGE_SIZE)));
static LapgPagePool g_pool;
static uint64_t g_root;

static volatile uint64_t g_hw_tlb_fills;
static volatile uint64_t g_hw_guest_pf;
static volatile uint64_t g_current_tb_code;
static volatile uint64_t g_current_tb_pc;
static const TbInsnMap *g_current_tb_map;

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
    if (g_root != 0) {
        rebuild_host_mappings();
    }
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
    return addr >= 0x200000ULL && addr < 0x3a0000ULL && !is_host_vmm_va(addr);
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

    latx_bare_uart_puts("tb-sys: checks=");
    puthex((uint64_t)checks);
    latx_bare_uart_puts(" fails=");
    puthex((uint64_t)fails);
    latx_bare_uart_puts(fails ? " BAD\n" : " ok\n");
    latx_bare_uart_puts("tb-sys: done\n");
    for (;;) {
    }
}
