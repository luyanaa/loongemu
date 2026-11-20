/*
 * Standalone fast bare-metal regression suite for LATX x86-64 translation
 * (x86-64-v2 / x86-64-v3 / XSAVE / CMPXCHG16B / HLT) and 4-vCPU SMP
 * context isolation, atomic contention, and IPI wake-up under PLV3 hardware
 * paging and timer preemption.
 *
 * Designed to run in ~1-2 seconds under QEMU across multiple host profiles:
 *   - la464                  (LSX + LASX + LBT -> x86-64-v3)
 *   - la464,lasx=off         (LSX + LBT        -> x86-64-v2)
 *   - la464,lsx=off,lasx=off (negative test: refuses to start)
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

void latx_platform_refuse(const char *why)
{
    latx_bare_uart_puts("loongemu: refusing to start: ");
    latx_bare_uart_puts(why);
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("regress: checks=0000000000000000 fails=0000000000000001 refused\n");
    latx_bare_uart_puts("regress: done\n");
    for (;;) {
    }
}

static void puthex(uint64_t v)
{
    static const char d[] = "0123456789abcdef";
    for (int sh = 60; sh >= 0; sh -= 4) {
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

/* ---------------- platform hooks required by liblatx_core.a ---------------- */

uint64_t latx_platform_io_read(uint32_t port, int size)
{
    (void)port; (void)size;
    return 0xffffffffull;
}

void latx_platform_io_write(uint32_t port, int size, uint64_t value)
{
    (void)port; (void)size; (void)value;
}

int latx_platform_apic_mmio(uint64_t gpa, int is_write, uint64_t *value)
{
    (void)gpa; (void)is_write; (void)value;
    return 0;
}

uint64_t latx_platform_phys_read(uint64_t pa, int size)
{
    uint64_t v = 0;
    for (int i = size - 1; i >= 0; i--) {
        v = (v << 8) | *(volatile uint8_t *)(uintptr_t)(pa + (uint64_t)i);
    }
    return v;
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
    return *(volatile uint8_t *)(uintptr_t)vaddr;
}

uint64_t latx_platform_guest_read(uint64_t gva, int size)
{
    return latx_platform_phys_read(gva, size);
}

void latx_platform_guest_write(uint64_t gva, int size, uint64_t value)
{
    latx_platform_phys_write(gva, size, value);
}

/* ---------------- translator, paging & SMP state ---------------- */

#define MAX_VCPUS       4u
#define VMM_IMAGE_BASE  0x8000000ULL
#define CODE_CACHE_SIZE (2 * 1024 * 1024)
#define TB_TABLE_CAP    1024

static CPUX86State env;
static CPUX86State g_vcpu_save[MAX_VCPUS];
static TCGContext tcg_storage;
static TRANSLATION_DATA tr;
static ENV lsenv_storage;

typedef struct FastJmpEntry {
    uint64_t pc;
    uint64_t code;
} FastJmpEntry;

#define FAST_JMP_CACHE_SIZE (1u << TB_JMP_CACHE_BITS)
static FastJmpEntry fast_jmp_cache[FAST_JMP_CACHE_SIZE] __attribute__((aligned(4096)));

static uint8_t epi_buf[4096] __attribute__((aligned(4096)));
static uint8_t pro_buf[4096] __attribute__((aligned(4096)));
static uint8_t code_cache[CODE_CACHE_SIZE] __attribute__((aligned(4096)));
static uint64_t code_used;

static TbCacheEntry tb_entries[TB_TABLE_CAP];
static uint32_t tb_buckets[TB_TABLE_CAP];
static TbCache tb_cache;
static TbInsnMap tb_insn_maps[TB_TABLE_CAP];
static uint32_t g_tb_highwater;

unsigned char kstack[65536] __attribute__((aligned(4096)));
void kmain(void);

__asm__(".text\n.globl _start\n_start:\n  la $sp, kstack\n  lu12i.w $t0, 16\n"
        "  add.d $sp, $sp, $t0\n  bl kmain\n1:\n  b 1b\n"
        ".balign 4096\n.space 4096\n");

extern int latx_gen_prologue(void *code_buf);
extern char pf_table[256];

#define EXC_INT  0x0
#define EXC_PIL  0x1
#define EXC_PIS  0x2
#define EXC_PIF  0x3
#define EXC_PME  0x4
#define EXC_PNR  0x5
#define EXC_PNX  0x6
#define EXC_PPI  0x7
#define EXC_IPE  0x0d
#define CSR_ESTAT_IS_MASK 0x1fffULL
#define IRQ_TIMER 11

#define LAPG_POOL_PAGES 256
static uint64_t g_pool_mem[LAPG_POOL_PAGES * (LAPG_PAGE_SIZE / 8)]
    __attribute__((aligned(LAPG_PAGE_SIZE)));
static LapgPagePool g_pool;
static uint64_t g_root;

static int g_use_sw_lbt;
static uint32_t g_cur_vcpu;
static volatile uint64_t g_plv3_timer_preempt __attribute__((used));
static volatile int g_vcpu_preempt_needed;
static volatile uint64_t g_timer_ticks;
static volatile uint64_t g_vcpu_switches;

typedef struct {
    uint64_t reason;
    uint64_t ecode;
    uint64_t era;
    uint64_t badv;
} Plv3ExitInfo;

volatile Plv3ExitInfo g_plv3_exit __attribute__((used));

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

static LapgMemory g_memory = { 0, mem_read_u64, mem_write_u64 };

static inline uint64_t csrrd(unsigned int n)
{
    uint64_t v = 0;
    switch (n) {
#define R(x) case x: __asm__ volatile("csrrd %0, " #x : "=r"(v)); break;
    R(0x0) R(0x1) R(0x2) R(0x4) R(0x5) R(0x6) R(0x7) R(0xc)
    R(0x10) R(0x11) R(0x12) R(0x13) R(0x1c) R(0x1d) R(0x1e)
    R(0x88) R(0x89) R(0x8a) R(0x8f) R(0x180)
#undef R
    default: break;
    }
    return v;
}

static inline void csrwr(unsigned int n, uint64_t v)
{
    switch (n) {
#define W(x) case x: __asm__ volatile("move $r12,%0\n csrwr $r12," #x :: "r"(v) : "r12", "memory"); break;
    W(0x0) W(0x1) W(0x2) W(0x4) W(0x6) W(0xc)
    W(0x10) W(0x11) W(0x12) W(0x13) W(0x1c) W(0x1d) W(0x1e)
    W(0x88) W(0x8a) W(0x8c) W(0x8d) W(0x8e) W(0x8f) W(0x180)
#undef W
    default: break;
    }
}

void latx_platform_tlb_flush_all(void)
{
    memset(fast_jmp_cache, 0, sizeof(fast_jmp_cache));
    __asm__ volatile("invtlb 0x0, $zero, $zero" ::: "memory");
}

void latx_platform_tlb_flush_page(uint64_t addr)
{
    uint64_t vppn = addr & UINT64_C(0x0000ffffffffe000);
    __asm__ volatile("invtlb 0x6, $zero, %0" :: "r"(vppn) : "memory");
}

static void map_host_range(uint64_t start, uint64_t size, uint32_t perm)
{
    uint64_t va = start & LAPG_PAGE_MASK;
    uint64_t end = (start + size + LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    for (; va < end; va += LAPG_PAGE_SIZE) {
        (void)lapg_map(&g_memory, &g_pool, g_root, va, va, perm);
    }
}

static int is_host_vmm_va(uint64_t va)
{
    uint64_t page = va & LAPG_PAGE_MASK;
    uint64_t cc_start = (uint64_t)(uintptr_t)code_cache & LAPG_PAGE_MASK;
    uint64_t cc_end = ((uint64_t)(uintptr_t)code_cache + CODE_CACHE_SIZE +
                       LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    uint64_t env_start = (uint64_t)(uintptr_t)&env & LAPG_PAGE_MASK;
    uint64_t env_end = ((uint64_t)(uintptr_t)&env + sizeof(env) +
                        LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    uint64_t jc_start = (uint64_t)(uintptr_t)fast_jmp_cache & LAPG_PAGE_MASK;
    uint64_t jc_end = ((uint64_t)(uintptr_t)fast_jmp_cache + sizeof(fast_jmp_cache) +
                       LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    uint64_t ks_start = (uint64_t)(uintptr_t)kstack & LAPG_PAGE_MASK;
    uint64_t ks_end = ((uint64_t)(uintptr_t)kstack + sizeof(kstack) +
                       LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    uint64_t pf_start = (uint64_t)(uintptr_t)pf_table & LAPG_PAGE_MASK;
    uint64_t pf_end = ((uint64_t)(uintptr_t)pf_table + sizeof(pf_table) +
                       LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;

    if (page >= cc_start && page < cc_end) return 1;
    if (page == ((uint64_t)(uintptr_t)pro_buf & LAPG_PAGE_MASK)) return 1;
    if (page == ((uint64_t)(uintptr_t)epi_buf & LAPG_PAGE_MASK)) return 1;
    if (page >= env_start && page < env_end) return 1;
    if (page >= jc_start && page < jc_end) return 1;
    if (page >= ks_start && page < ks_end) return 1;
    if (page >= pf_start && page < pf_end) return 1;
    return 0;
}

static int is_plv0_helper_entry(uint64_t addr)
{
    extern char _end[];
    return addr >= VMM_IMAGE_BASE && addr < (uint64_t)(uintptr_t)_end &&
           !is_host_vmm_va(addr);
}

static void save_host_xmm_to_env(void)
{
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

static void sync_trap_regs_to_env(const uint64_t *regs)
{
    for (int i = 0; i < 16; i++) {
        env.regs[i] = regs[reg_gpr_map[i]];
    }
    save_host_xmm_to_env();
    uint32_t non_cc = (uint32_t)regs[reg_statics_map[S_EFLAGS]] & 0x702u;
    uint32_t cc = 0;
    if (g_use_sw_lbt) {
        cc = sw_lbt.eflags;
    } else {
        __asm__ volatile(".word 0x005cfc0c\n move %0, $t0" : "=r"(cc) :: "t0");
    }
    env.eflags = non_cc | (cc & 0x8d5u) | 0x2u;
}

extern void regress_trap(void);
extern void regress_run_plv3(void *pro, void *tb_code, void *env_ptr);
extern char regress_plv3_done[];
extern char regress_abort_plv0[];

void latx_platform_cpu_loop_exit(CPUState *cs)
{
    (void)cs;
    g_plv3_exit.reason = 3;
    uint64_t prmd = csrrd(0x1) & ~UINT64_C(0x7);
    csrwr(0x1, prmd);
    csrwr(0x6, (uint64_t)(uintptr_t)regress_abort_plv0);
    __asm__ volatile("ertn" ::: "memory");
    for (;;) {}
}

static void timer_start(uint64_t period_ticks)
{
    uint64_t tcfg = (period_ticks << 2) | 0x2 | 0x1;
    uint64_t ecfg = csrrd(0x4) | (1ULL << IRQ_TIMER);
    csrwr(0x4, ecfg);
    __asm__ volatile("csrwr %0, 0x41" : "+r"(tcfg) :: "memory");
}

static void timer_stop(void)
{
    uint64_t zero = 0;
    __asm__ volatile("csrwr %0, 0x41" : "+r"(zero) :: "memory");
    csrwr(0x4, csrrd(0x4) & ~(1ULL << IRQ_TIMER));
}

static void timer_ack(void)
{
    uint64_t one = 1;
    __asm__ volatile("csrwr %0, 0x44" : "+r"(one) :: "memory");
}

static void fill_hw_tlb(uint64_t badv, uint64_t pte0, uint64_t pte1, int is_refill)
{
    uint64_t vppn = badv & UINT64_C(0x0000ffffffffe000);
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
}

typedef uint64_t (*HelperFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static int resolve_tb_entry_pc(uint64_t fault_pc, uint64_t *out_pc)
{
    for (uint32_t i = 0; i < g_tb_highwater; i++) {
        if (tb_entries[i].used && fault_pc == tb_entries[i].code) {
            *out_pc = tb_entries[i].pc;
            return 1;
        }
    }
    return 0;
}

void regress_handle_trap(uint64_t *regs)
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
    uint64_t badv = is_refill ? csrrd(0x89) : csrrd(0x7);
    uint64_t fault_pc = is_refill ? ((tlbrera >> 2) << 2) : era;

    if (!is_refill && ecode == EXC_INT && (estat & (1ULL << IRQ_TIMER))) {
        timer_ack();
        g_timer_ticks++;
        if (g_plv3_timer_preempt) {
            uint64_t exact_rip = 0;
            g_vcpu_preempt_needed = 1;
            if (resolve_tb_entry_pc(fault_pc, &exact_rip)) {
                sync_trap_regs_to_env(regs);
                env.eip = exact_rip;
                g_plv3_exit.reason = 1;
                csrwr(0x1, csrrd(0x1) & ~UINT64_C(0x7));
                csrwr(0x6, (uint64_t)(uintptr_t)regress_abort_plv0);
                return;
            }
        }
        return;
    }

    if (fault_pc == (uint64_t)(uintptr_t)regress_plv3_done) {
        g_plv3_exit.reason = 1;
        if (is_refill) {
            csrwr(0x8f, 0);
            csrwr(0x8a, ((uint64_t)(uintptr_t)regress_abort_plv0 & ~UINT64_C(3)) | 1u);
        } else {
            csrwr(0x1, csrrd(0x1) & ~UINT64_C(0x7));
            csrwr(0x6, (uint64_t)(uintptr_t)regress_abort_plv0);
        }
        return;
    }

    if ((is_refill || ecode == EXC_PIF || ecode == EXC_PPI || ecode == EXC_PNX) &&
        fault_pc == badv && is_plv0_helper_entry(badv)) {
        HelperFn fn = (HelperFn)(uintptr_t)badv;
        uint64_t ret = fn(regs[la_a0], regs[la_a1], regs[la_a2],
                          regs[la_a3], regs[la_a4]);
        regs[la_a0] = ret;
        if (is_refill) {
            csrwr(0x8a, (regs[la_ra] & ~UINT64_C(3)) | 1u);
        } else {
            csrwr(0x6, regs[la_ra]);
        }
        return;
    }

    if (is_refill || ecode == EXC_PIL || ecode == EXC_PIS ||
        ecode == EXC_PIF || ecode == EXC_PME || ecode == EXC_PPI ||
        ecode == EXC_PNR || ecode == EXC_PNX) {
        uint64_t vppn = badv & UINT64_C(0x0000ffffffffe000);
        if (is_host_vmm_va(badv)) {
            LapgTranslation t0, t1;
            uint64_t p0 = (lapg_walk(&g_memory, g_root, vppn, LAPG_PERM_USER, &t0) == LAPG_OK) ? t0.pte : 0;
            uint64_t p1 = (lapg_walk(&g_memory, g_root, vppn + LAPG_PAGE_SIZE, LAPG_PERM_USER, &t1) == LAPG_OK) ? t1.pte : 0;
            fill_hw_tlb(badv, p0, p1, is_refill);
            return;
        }
        if (badv < (64ULL * 1024 * 1024)) {
            uint32_t perm = LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_EXEC | LAPG_PERM_USER;
            uint64_t p0 = lapg_make_leaf(vppn >> LAPG_PAGE_BITS, perm);
            uint64_t p1 = lapg_make_leaf((vppn + LAPG_PAGE_SIZE) >> LAPG_PAGE_BITS, perm);
            fill_hw_tlb(badv, p0, p1, is_refill);
            return;
        }
    }

    sync_trap_regs_to_env(regs);
    g_plv3_exit.reason = 3;
    g_plv3_exit.ecode = ecode;
    g_plv3_exit.era = fault_pc;
    g_plv3_exit.badv = badv;
    if (is_refill) {
        csrwr(0x8f, 0);
        csrwr(0x8a, ((uint64_t)(uintptr_t)regress_abort_plv0 & ~UINT64_C(3)) | 1u);
    } else {
        csrwr(0x1, csrrd(0x1) & ~UINT64_C(0x7));
        csrwr(0x6, (uint64_t)(uintptr_t)regress_abort_plv0);
    }
}

__asm__(
".text\n"
".globl regress_trap\n"
"regress_trap:\n"
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
"  bl regress_handle_trap\n"
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

uint64_t g_saved_plv0_sp __attribute__((used));

__asm__(
".text\n"
".globl regress_run_plv3\n"
"regress_run_plv3:\n"
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
"  la   $t0, g_plv3_exit\n"
"  st.d $zero, $t0, 0\n"
"  csrrd $t0, 0x1\n"
"  ori   $t0, $t0, 0x7\n"
"  xori  $t0, $t0, 0x4\n"
"  la    $t1, g_plv3_timer_preempt\n"
"  ld.d  $t1, $t1, 0\n"
"  beqz  $t1, 2f\n"
"  ori   $t0, $t0, 0x4\n"
"2:\n"
"  csrwr $t0, 0x1\n"
"  csrwr $a0, 0x6\n"
"  move  $a0, $a1\n"
"  move  $a1, $a2\n"
"  la    $ra, regress_plv3_done\n"
"  ertn\n"
);

__asm__(
".text\n"
".globl regress_plv3_done\n"
"regress_plv3_done:\n"
"  csrrd $zero, 0x0\n"
".globl regress_abort_plv0\n"
"regress_abort_plv0:\n"
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

/* ---------------- helper: run single guest snippet until HLT ---------------- */

static int run_snippet_until_hlt(uint64_t entry_pc, int max_blocks)
{
    env.eip = entry_pc;
    for (int step = 0; step < max_blocks; step++) {
        uint64_t pc = env.eip;
        TbCacheEntry *e = tbc_lookup(&tb_cache, pc, 0, 0, 0);
        if (!e) {
            e = tbc_insert(&tb_cache, pc, 0, 0, 0);
            if (!e) return -1;
            uint32_t slot = (uint32_t)(e - tb_entries);
            if (slot + 1 > g_tb_highwater) g_tb_highwater = slot + 1;
            uint8_t *dst = code_cache + code_used;
            int rc = tb_harness_translate_at_with_map(pc, dst, CODE_CACHE_SIZE - code_used,
                                                      &tb_insn_maps[slot]);
            if (rc <= 0) return -2;
            e->code = (uint64_t)(uintptr_t)dst;
            e->code_size = (uint32_t)rc;
            code_used += (((uint64_t)rc + 255) & ~UINT64_C(255));
            __asm__ volatile("ibar 0" ::: "memory");
        }
        regress_run_plv3(pro_buf, (void *)(uintptr_t)e->code, &env);
        if (g_plv3_exit.reason != 1) {
            return -3;
        }
        if (*(volatile uint8_t *)(uintptr_t)env.eip == 0xf4u) {
            return step + 1;
        }
    }
    return -4;
}

static void switch_vcpu(uint32_t next_id)
{
    if (next_id >= MAX_VCPUS || next_id == g_cur_vcpu) return;
    g_vcpu_save[g_cur_vcpu] = env;
    env = g_vcpu_save[next_id];
    uint32_t cc = (uint32_t)(env.eflags & 0x8d5u);
    if (g_use_sw_lbt) {
        sw_lbt.eflags = cc;
    } else {
        __asm__ volatile("move $t0, %0\n .word 0x005cfc2c\n" :: "r"(cc) : "t0");
    }
    g_cur_vcpu = next_id;
    g_vcpu_switches++;
}

static void reset_guest_env(uint64_t rsp)
{
    memset(&env, 0, sizeof(env));
    latx_x86_env_init_features(&env);
    env.sys.codemode = 1;
    env.hflags = HF_LMA_MASK | HF_CS64_MASK | HF_PE_MASK;
    env.cr[0] = 0x1; /* PE=1, PG=0 (flat guest RAM) */
    env.cr[4] = CR4_PAE_MASK | CR4_OSFXSR_MASK | CR4_OSXSAVE_MASK;
    env.efer = MSR_EFER_LME | MSR_EFER_LMA;
    env.eflags = 0x2;
    env.regs[R_ESP] = rsp;
    env.tb_jmp_cache_ptr = 0;
}

/* ---------------- x86-64 test machine code (verified via llvm-mc / clang) ---------------- */

static const uint8_t code_cmpxchg16b[] = {
    0xf0, 0x48, 0x0f, 0xc7, 0x0f, 0x9c, 0x41, 0x58, 0xf4
};

static const uint8_t code_popcnt_v2[] = {
    0xf3, 0x48, 0x0f, 0xb8, 0xc7, 0x9c, 0x41, 0x58,
    0xf3, 0x48, 0x0f, 0xb8, 0xde, 0x9c, 0x41, 0x59, 0xf4
};

static const uint8_t code_sse4_v2[] = {
    0xf3, 0x0f, 0x6f, 0x07, 0xf3, 0x0f, 0x6f, 0x4f, 0x10,
    0x66, 0x0f, 0x38, 0x00, 0xc1, 0xf3, 0x0f, 0x7f, 0x47, 0x20,
    0xf3, 0x0f, 0x6f, 0x17, 0x66, 0x0f, 0x38, 0x40, 0x57, 0x30,
    0xf3, 0x0f, 0x7f, 0x57, 0x40, 0x66, 0x0f, 0x38, 0x29, 0x57, 0x50,
    0xf3, 0x0f, 0x7f, 0x57, 0x60, 0xf2, 0x48, 0x0f, 0x38, 0xf1, 0xc6, 0xf4
};

static const uint8_t code_v3_int[] = {
    0x48, 0x0f, 0x38, 0xf0, 0x07, 0x48, 0x0f, 0x38, 0xf1, 0x47, 0x08,
    0xf3, 0x48, 0x0f, 0xbd, 0xde, 0x9c, 0x41, 0x58,
    0xf3, 0x48, 0x0f, 0xbc, 0xca,
    0xc4, 0x62, 0xe8, 0xf2, 0xce,
    0xc4, 0xe2, 0xa8, 0xf3, 0xca,
    0xc4, 0xe2, 0xa0, 0xf3, 0xda,
    0xc4, 0x63, 0xfb, 0xf0, 0xe6, 0x10,
    0xc4, 0x62, 0x91, 0xf7, 0xf2,
    0xc4, 0x62, 0x93, 0xf7, 0xfe, 0xf4
};

static const uint8_t code_v3_avx2_fma[] = {
    0xc5, 0xfe, 0x6f, 0x07, 0xc5, 0xfe, 0x6f, 0x4f, 0x20,
    0xc5, 0xfd, 0xfe, 0xd1, 0xc5, 0xfe, 0x7f, 0x57, 0x40,
    0xc5, 0xed, 0xef, 0xd9, 0xc5, 0xfe, 0x7f, 0x5f, 0x60,
    0xc5, 0xf8, 0x77, 0xc5, 0xfe, 0x7f, 0x97, 0x80, 0x00, 0x00, 0x00,
    0xc5, 0xf9, 0x10, 0xa7, 0xa0, 0x00, 0x00, 0x00,
    0xc5, 0xf9, 0x10, 0xaf, 0xb0, 0x00, 0x00, 0x00,
    0xc5, 0xf9, 0x10, 0xb7, 0xc0, 0x00, 0x00, 0x00,
    0xc4, 0xe2, 0xd9, 0xb8, 0xf5,
    0xc5, 0xf9, 0x11, 0xb7, 0xd0, 0x00, 0x00, 0x00, 0xf4
};

static const uint8_t code_v3_xsave[] = {
    0x31, 0xc9, 0xb8, 0x07, 0x00, 0x00, 0x00, 0x31, 0xd2,
    0x0f, 0x01, 0xd1, 0x0f, 0x01, 0xd0, 0x49, 0x89, 0xc0,
    0xc5, 0xfe, 0x6f, 0x07,
    0xb8, 0x07, 0x00, 0x00, 0x00, 0x31, 0xd2, 0x48, 0x0f, 0xae, 0x26,
    0xc5, 0xfc, 0x77,
    0xb8, 0x07, 0x00, 0x00, 0x00, 0x31, 0xd2, 0x48, 0x0f, 0xae, 0x2e,
    0xc5, 0xfe, 0x7f, 0x47, 0x20, 0xf4
};

static const uint8_t code_smp_worker_v2[] = {
    0xf3, 0x41, 0x0f, 0x6f, 0x00, 0xf3, 0x41, 0x0f, 0x6f, 0x48, 0x10,
    0x49, 0x89, 0xf1, 0x49, 0xc1, 0xe1, 0x20, 0x49, 0x81, 0xc9, 0x68,
    0x24, 0x57, 0x13, 0x4d, 0x89, 0xca, 0xb8, 0x01, 0x00, 0x00, 0x00,
    0xf0, 0x48, 0x0f, 0xc1, 0x07, 0x48, 0x8b, 0x47, 0x08, 0x48, 0x8d,
    0x48, 0x01, 0xf0, 0x48, 0x0f, 0xb1, 0x4f, 0x08, 0x75, 0xf0, 0x48,
    0x8b, 0x47, 0x10, 0x48, 0x8b, 0x57, 0x18, 0x48, 0x8d, 0x58, 0x01,
    0x48, 0x8d, 0x4a, 0x03, 0xf0, 0x48, 0x0f, 0xc7, 0x4f, 0x10, 0x75,
    0xe8, 0x4d, 0x39, 0xd1, 0x75, 0x42, 0xf3, 0x41, 0x0f, 0x7f, 0x40,
    0x20, 0xf3, 0x41, 0x0f, 0x7f, 0x48, 0x30, 0x49, 0x8b, 0x40, 0x20,
    0x49, 0x3b, 0x00, 0x75, 0x2d, 0x49, 0x8b, 0x40, 0x28, 0x49, 0x3b,
    0x40, 0x08, 0x75, 0x23, 0x49, 0x8b, 0x40, 0x30, 0x49, 0x3b, 0x40,
    0x10, 0x75, 0x19, 0x49, 0x8b, 0x40, 0x38, 0x49, 0x3b, 0x40, 0x18,
    0x75, 0x0f, 0x49, 0x83, 0xeb, 0x01, 0x75, 0x90, 0x49, 0xc7, 0x40,
    0x40, 0x01, 0x00, 0x00, 0x00, 0xf4, 0x49, 0xc7, 0x40, 0x40, 0xad,
    0xde, 0x00, 0x00, 0xf4
};

static const uint8_t code_smp_worker_v3_ymm[] = {
    0xc4, 0xc1, 0x7e, 0x6f, 0x00, 0xc4, 0xc1, 0x7e, 0x6f, 0x48, 0x20,
    0x49, 0x89, 0xf1, 0x49, 0xc1, 0xe1, 0x20, 0x49, 0x81, 0xc9, 0x57,
    0x13, 0x68, 0x24, 0x4d, 0x89, 0xca, 0xb8, 0x01, 0x00, 0x00, 0x00,
    0xf0, 0x48, 0x0f, 0xc1, 0x07, 0xc4, 0xc1, 0x7e, 0x7f, 0x40, 0x40,
    0xc4, 0xc1, 0x7e, 0x7f, 0x48, 0x60, 0x49, 0x8b, 0x40, 0x40, 0x49,
    0x3b, 0x00, 0x75, 0x35, 0x49, 0x8b, 0x40, 0x58, 0x49, 0x3b, 0x40,
    0x18, 0x75, 0x2b, 0x49, 0x8b, 0x40, 0x60, 0x49, 0x3b, 0x40, 0x20,
    0x75, 0x21, 0x49, 0x8b, 0x40, 0x78, 0x49, 0x3b, 0x40, 0x38, 0x75,
    0x17, 0x4d, 0x39, 0xd1, 0x75, 0x12, 0x49, 0x83, 0xeb, 0x01, 0x75,
    0xb8, 0x49, 0xc7, 0x80, 0x80, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0xf4, 0x49, 0xc7, 0x80, 0x80, 0x00, 0x00, 0x00, 0xad, 0xde,
    0x00, 0x00, 0xf4
};

static const uint8_t code_ipi_waiter[] = {
    0xfb, 0xf4, 0x48, 0xc7, 0x07, 0xaa, 0x55, 0x00, 0x00, 0xf4
};

static const uint8_t code_ipi_handler[] = {
    0x48, 0xff, 0x06, 0x48, 0xcf
};

/* ---------------- Suite 1: CPUID / ABI Gate ---------------- */

static void test_cpuid_abi(uint32_t lvl)
{
    uint32_t a, b, c, d;

    reset_guest_env(0x8000);
    env.cr[4] &= ~(uint64_t)CR4_OSXSAVE_MASK;
    cpu_x86_cpuid(&env, 1, 0, &a, &b, &c, &d);
    check((c & (1u << 13)) != 0, "cpuid: CMPXCHG16B set");
    check((c & (1u << 20)) != 0, "cpuid: SSE4.2 set");
    check((c & (1u << 23)) != 0, "cpuid: POPCNT set");
    check((c & (1u << 27)) == 0, "cpuid: OSXSAVE clear when CR4.OSXSAVE=0");

    env.cr[4] |= CR4_OSXSAVE_MASK;
    cpu_x86_cpuid(&env, 1, 0, &a, &b, &c, &d);
    if (lvl >= 3) {
        check((c & (1u << 22)) != 0, "cpuid: v3 MOVBE set");
        check((c & (1u << 27)) != 0, "cpuid: v3 OSXSAVE follows CR4.OSXSAVE=1");
        check((c & (1u << 28)) != 0, "cpuid: v3 AVX set");
    } else {
        check((c & (1u << 22)) == 0, "cpuid: v2 MOVBE clear");
        check((c & (1u << 28)) == 0, "cpuid: v2 AVX clear");
    }

    cpu_x86_cpuid(&env, 0x80000001u, 0, &a, &b, &c, &d);
    if (lvl >= 3) {
        check((c & (1u << 5)) != 0, "cpuid: v3 ABM/LZCNT set");
    } else {
        check((c & (1u << 5)) == 0, "cpuid: v2 ABM/LZCNT clear");
    }

    cpu_x86_cpuid(&env, 0x80000008u, 0, &a, &b, &c, &d);
    check((a & 0xffu) == 40 && ((a >> 8) & 0xffu) == 48, "cpuid: addr bits 40/48");
}

/* ---------------- Suite 2: x86-64-v2 Translation ---------------- */

static void test_v2_translation(void)
{
    /* 2a. LOCK CMPXCHG16B match & mismatch */
    {
        uint64_t pc = 0x10000;
        uint64_t *mem = (uint64_t *)(uintptr_t)0x20000;
        memcpy((void *)(uintptr_t)pc, code_cmpxchg16b, sizeof(code_cmpxchg16b));

        mem[0] = 0x1122334455667788ULL;
        mem[1] = 0x99aabbccddeeff00ULL;
        reset_guest_env(0x18000);
        env.regs[R_EDI] = (uint64_t)(uintptr_t)mem;
        env.regs[R_EAX] = 0x1122334455667788ULL;
        env.regs[R_EDX] = 0x99aabbccddeeff00ULL;
        env.regs[R_EBX] = 0xaaaabbbbccccddddULL;
        env.regs[R_ECX] = 0x123456789abcdef0ULL;
        check(run_snippet_until_hlt(pc, 16) > 0, "v2: cmpxchg16b match ran");
        check(mem[0] == 0xaaaabbbbccccddddULL && mem[1] == 0x123456789abcdef0ULL,
              "v2: cmpxchg16b match updated 128-bit memory");
        check((env.regs[R_R8] & 0x40u) != 0, "v2: cmpxchg16b match set ZF=1");

        reset_guest_env(0x18000);
        env.regs[R_EDI] = (uint64_t)(uintptr_t)mem;
        env.regs[R_EAX] = 0xdeadULL;
        env.regs[R_EDX] = 0xbeefULL;
        env.regs[R_EBX] = 0x1111ULL;
        env.regs[R_ECX] = 0x2222ULL;
        check(run_snippet_until_hlt(pc, 16) > 0, "v2: cmpxchg16b mismatch ran");
        check(mem[0] == 0xaaaabbbbccccddddULL && mem[1] == 0x123456789abcdef0ULL,
              "v2: cmpxchg16b mismatch preserved memory");
        check(env.regs[R_EAX] == 0xaaaabbbbccccddddULL &&
              env.regs[R_EDX] == 0x123456789abcdef0ULL,
              "v2: cmpxchg16b mismatch loaded old RDX:RAX");
        check((env.regs[R_R8] & 0x40u) == 0, "v2: cmpxchg16b mismatch cleared ZF=0");
    }

    /* 2b. POPCNT zero and non-zero */
    {
        uint64_t pc = 0x10100;
        memcpy((void *)(uintptr_t)pc, code_popcnt_v2, sizeof(code_popcnt_v2));
        reset_guest_env(0x18000);
        env.regs[R_EDI] = 0;
        env.regs[R_ESI] = 0xf0f055aa12345678ULL;
        check(run_snippet_until_hlt(pc, 16) > 0, "v2: popcnt ran");
        check(env.regs[R_EAX] == 0 && (env.regs[R_R8] & 0x40u) != 0,
              "v2: popcnt(0) == 0 with ZF=1");
        check(env.regs[R_EBX] == 29 && (env.regs[R_R9] & 0x40u) == 0,
              "v2: popcnt(0xf0f055aa12345678) == 29 with ZF=0");
    }

    /* 2c. SSSE3 PSHUFB + SSE4.1 PMULLD/PCMPEQQ + SSE4.2 CRC32 */
    {
        uint64_t pc = 0x10200;
        uint8_t *buf = (uint8_t *)(uintptr_t)0x20100;
        static const uint8_t ctrl[16] = {
            15,14,13,12, 0x80,0,1,2, 3,4,5,6, 0xff,7,8,9
        };
        static const uint8_t exp_shuf[16] = {
            15,14,13,12, 0,0,1,2, 3,4,5,6, 0,7,8,9
        };
        uint32_t mul_rhs[4] = { 2, 3, 4, 5 };
        uint32_t exp_mul[4] = { 0x06040200u, 0x15120f0cu, 0x2c282420u, 0x4b46413cu };
        uint64_t cmp_rhs[2];

        memcpy((void *)(uintptr_t)pc, code_sse4_v2, sizeof(code_sse4_v2));
        memset(buf, 0, 256);
        for (int i = 0; i < 16; i++) buf[i] = (uint8_t)i;
        memcpy(buf + 16, ctrl, 16);
        memcpy(buf + 48, mul_rhs, 16);
        memcpy(&cmp_rhs[0], exp_mul, 8);
        cmp_rhs[1] = 0x123456789abcdef0ULL; /* intentional mismatch on high qword */
        memcpy(buf + 80, cmp_rhs, 16);

        reset_guest_env(0x18000);
        env.regs[R_EDI] = (uint64_t)(uintptr_t)buf;
        env.regs[R_EAX] = 0xffffffffULL;
        env.regs[R_ESI] = 0x0123456789abcdefULL;
        check(run_snippet_until_hlt(pc, 16) > 0, "v2: sse4/ssse3/crc32 ran");
        check(memcmp(buf + 32, exp_shuf, 16) == 0, "v2: PSHUFB shuffle+zero exact");
        check(memcmp(buf + 64, exp_mul, 16) == 0, "v2: PMULLD exact");
        {
            uint64_t eq_res[2];
            memcpy(eq_res, buf + 96, 16);
            check(eq_res[0] == ~UINT64_C(0) && eq_res[1] == 0, "v2: PCMPEQQ exact");
        }
        /* Reference Castagnoli CRC32C of 8 bytes (LE) starting at 0xffffffff */
        {
            uint32_t crc = 0xffffffffu;
            uint64_t v = 0x0123456789abcdefULL;
            for (int b = 0; b < 8; b++) {
                crc ^= (uint32_t)((v >> (b * 8)) & 0xffu);
                for (int k = 0; k < 8; k++) {
                    crc = (crc & 1u) ? ((crc >> 1) ^ 0x82f63b78u) : (crc >> 1);
                }
            }
            check(env.regs[R_EAX] == (uint64_t)crc, "v2: CRC32Q exact");
        }
    }
}

/* ---------------- Suite 3: x86-64-v3 Translation ---------------- */

static void test_v3_translation(void)
{
    /* 3a. MOVBE + LZCNT + BMI1/BMI2 */
    {
        uint64_t pc = 0x10300;
        uint64_t *mem = (uint64_t *)(uintptr_t)0x20300;
        memcpy((void *)(uintptr_t)pc, code_v3_int, sizeof(code_v3_int));
        mem[0] = 0x0102030405060708ULL;
        mem[1] = 0;

        reset_guest_env(0x18000);
        env.regs[R_EDI] = (uint64_t)(uintptr_t)mem;
        env.regs[R_ESI] = 0x0010000000000000ULL;
        env.regs[R_EDX] = 0x0000000000001080ULL;
        env.regs[R_R13] = 4;
        check(run_snippet_until_hlt(pc, 16) > 0, "v3: movbe/lzcnt/bmi ran");
        check(env.regs[R_EAX] == 0x0807060504030201ULL && mem[1] == 0x0102030405060708ULL,
              "v3: MOVBE load+store exact");
        check(env.regs[R_EBX] == 11 && (env.regs[R_R8] & 1u) == 0,
              "v3: LZCNT exact with CF=0");
        check(env.regs[R_ECX] == 7, "v3: TZCNT exact");
        check(env.regs[R_R9] == 0x0010000000000000ULL, "v3: ANDN exact");
        check(env.regs[R_R10] == 0x1000ULL, "v3: BLSR exact");
        check(env.regs[R_R11] == 0x80ULL, "v3: BLSI exact");
        check(env.regs[R_R12] == 0x0000001000000000ULL, "v3: RORX exact");
        check(env.regs[R_R14] == 0x10800ULL, "v3: SHLX exact");
        check(env.regs[R_R15] == 0x0001000000000000ULL, "v3: SHRX exact");
    }

    /* 3b. AVX2 256-bit YMM + VZEROUPPER + FMA */
    {
        uint64_t pc = 0x10400;
        uint8_t *buf = (uint8_t *)(uintptr_t)0x20400;
        uint32_t a[8] = { 10,20,30,40, 50,60,70,80 };
        uint32_t b[8] = { 1,2,3,4, 5,6,7,8 };
        uint32_t exp_add[8] = { 11,22,33,44, 55,66,77,88 };
        uint32_t exp_xor[8];
        uint32_t exp_zu[8] = { 11,22,33,44, 0,0,0,0 };
        uint64_t fma_a[2] = { 0x4004000000000000ULL, 0x4008000000000000ULL }; /* 2.5, 3.0 */
        uint64_t fma_b[2] = { 0x4010000000000000ULL, 0x4014000000000000ULL }; /* 4.0, 5.0 */
        uint64_t fma_c[2] = { 0x3ff8000000000000ULL, 0x4000000000000000ULL }; /* 1.5, 2.0 */
        uint64_t fma_exp[2] = { 0x4027000000000000ULL, 0x4031000000000000ULL }; /* 11.5, 17.0 */

        for (int i = 0; i < 8; i++) exp_xor[i] = exp_add[i] ^ b[i];
        memcpy((void *)(uintptr_t)pc, code_v3_avx2_fma, sizeof(code_v3_avx2_fma));
        memset(buf, 0, 256);
        memcpy(buf + 0, a, 32);
        memcpy(buf + 32, b, 32);
        memcpy(buf + 160, fma_a, 16);
        memcpy(buf + 176, fma_b, 16);
        memcpy(buf + 192, fma_c, 16);

        reset_guest_env(0x18000);
        env.regs[R_EDI] = (uint64_t)(uintptr_t)buf;
        check(run_snippet_until_hlt(pc, 16) > 0, "v3: avx2/vzeroupper/fma ran");
        check(memcmp(buf + 64, exp_add, 32) == 0, "v3: VPADDD ymm 256-bit exact");
        check(memcmp(buf + 96, exp_xor, 32) == 0, "v3: VPXOR ymm 256-bit exact");
        check(memcmp(buf + 128, exp_zu, 32) == 0, "v3: VZEROUPPER zeroed high 128-bit");
        check(memcmp(buf + 208, fma_exp, 16) == 0, "v3: VFMADD231PD exact");
    }

    /* 3c. XSETBV / XGETBV / XSAVE64 / VZEROALL / XRSTOR64 */
    {
        uint64_t pc = 0x10500;
        uint64_t *ybuf = (uint64_t *)(uintptr_t)0x20600;
        uint8_t *xsave_area = (uint8_t *)(uintptr_t)0x21000; /* 64-byte aligned */

        memcpy((void *)(uintptr_t)pc, code_v3_xsave, sizeof(code_v3_xsave));
        memset(xsave_area, 0, 1024);
        ybuf[0] = 0x1111222233334444ULL;
        ybuf[1] = 0x5555666677778888ULL;
        ybuf[2] = 0x9999aaaabbbbccccULL;
        ybuf[3] = 0xddddeeeeffff0001ULL;
        ybuf[4] = ybuf[5] = ybuf[6] = ybuf[7] = 0;

        reset_guest_env(0x18000);
        env.regs[R_EDI] = (uint64_t)(uintptr_t)ybuf;
        env.regs[R_ESI] = (uint64_t)(uintptr_t)xsave_area;
        check(run_snippet_until_hlt(pc, 16) > 0, "v3: xsetbv/xgetbv/xsave/xrstor ran");
        check(env.regs[R_R8] == 7, "v3: XGETBV returned XCR0=7");
        check(ybuf[4] == ybuf[0] && ybuf[5] == ybuf[1] &&
              ybuf[6] == ybuf[2] && ybuf[7] == ybuf[3],
              "v3: XSAVE64 -> VZEROALL -> XRSTOR64 preserved 256-bit YMM0");
    }
}

/* ---------------- Suite 4: Multi-Core (4-vCPU) Isolation, Atomics & IPI ---------------- */

static void test_smp_isolation_and_atomics(uint32_t lvl)
{
    const uint64_t iters = 64;
    uint64_t pc_v2 = 0x11000;
    uint64_t *shared = (uint64_t *)(uintptr_t)0x22000;
    uint64_t *percpu_base = (uint64_t *)(uintptr_t)0x22100; /* 160 bytes per vCPU */

    memcpy((void *)(uintptr_t)pc_v2, code_smp_worker_v2, sizeof(code_smp_worker_v2));
    memset(shared, 0, 64);
    memset(percpu_base, 0, 4 * 256);

    int done[MAX_VCPUS] = { 0, 0, 0, 0 };
    for (uint32_t c = 0; c < MAX_VCPUS; c++) {
        uint64_t *buf = (uint64_t *)((uint8_t *)percpu_base + c * 256);
        buf[0] = 0xa000000000000001ULL | ((uint64_t)c << 48);
        buf[1] = 0xb000000000000002ULL | ((uint64_t)c << 48);
        buf[2] = 0xc000000000000003ULL | ((uint64_t)c << 48);
        buf[3] = 0xd000000000000004ULL | ((uint64_t)c << 48);

        reset_guest_env(0x30000 + c * 0x1000);
        env.eip = pc_v2;
        env.regs[R_EDI] = (uint64_t)(uintptr_t)shared;
        env.regs[R_ESI] = c;
        env.regs[R_R8] = (uint64_t)(uintptr_t)buf;
        env.regs[R_R11] = iters;
        g_vcpu_save[c] = env;
    }

    g_cur_vcpu = 0;
    env = g_vcpu_save[0];
    g_vcpu_switches = 0;
    g_timer_ticks = 0;
    g_vcpu_preempt_needed = 0;
    g_plv3_timer_preempt = 1;
    timer_ack();
    timer_start(2000); /* 20 us aggressive preemption */

    int total_steps = 0;
    while (total_steps < 50000) {
        int all_done = 1;
        for (uint32_t c = 0; c < MAX_VCPUS; c++) {
            if (!done[c]) { all_done = 0; break; }
        }
        if (all_done) break;

        if (done[g_cur_vcpu]) {
            for (uint32_t s = 1; s < MAX_VCPUS; s++) {
                uint32_t n = (g_cur_vcpu + s) % MAX_VCPUS;
                if (!done[n]) { switch_vcpu(n); break; }
            }
            continue;
        }

        uint64_t pc = env.eip;
        TbCacheEntry *e = tbc_lookup(&tb_cache, pc, 0, 0, 0);
        if (!e) {
            e = tbc_insert(&tb_cache, pc, 0, 0, 0);
            if (!e) break;
            uint32_t slot = (uint32_t)(e - tb_entries);
            if (slot + 1 > g_tb_highwater) g_tb_highwater = slot + 1;
            uint8_t *dst = code_cache + code_used;
            int rc = tb_harness_translate_at_with_map(pc, dst, CODE_CACHE_SIZE - code_used,
                                                      &tb_insn_maps[slot]);
            if (rc <= 0) break;
            e->code = (uint64_t)(uintptr_t)dst;
            e->code_size = (uint32_t)rc;
            code_used += (((uint64_t)rc + 255) & ~UINT64_C(255));
            __asm__ volatile("ibar 0" ::: "memory");
        }

        regress_run_plv3(pro_buf, (void *)(uintptr_t)e->code, &env);
        total_steps++;
        if (g_plv3_exit.reason != 1) break;

        if (*(volatile uint8_t *)(uintptr_t)env.eip == 0xf4u) {
            done[g_cur_vcpu] = 1;
        }
        if (g_vcpu_preempt_needed || (total_steps & 3) == 0 || done[g_cur_vcpu]) {
            g_vcpu_preempt_needed = 0;
            for (uint32_t s = 1; s < MAX_VCPUS; s++) {
                uint32_t n = (g_cur_vcpu + s) % MAX_VCPUS;
                if (!done[n]) { switch_vcpu(n); break; }
            }
        }
    }

    timer_stop();
    timer_ack();
    g_plv3_timer_preempt = 0;

    int all_ok = 1;
    for (uint32_t c = 0; c < MAX_VCPUS; c++) {
        uint64_t *buf = (uint64_t *)((uint8_t *)percpu_base + c * 256);
        if (!done[c] || buf[8] != 1) all_ok = 0;
    }
    check(all_ok, "smp: 4-vCPU XMM0/XMM1 + GPR isolation under timer preemption");
    check(shared[0] == MAX_VCPUS * iters, "smp: 4-vCPU LOCK XADD exact total");
    check(shared[1] == MAX_VCPUS * iters, "smp: 4-vCPU LOCK CMPXCHG exact total");
    check(shared[2] == MAX_VCPUS * iters && shared[3] == MAX_VCPUS * iters * 3,
          "smp: 4-vCPU LOCK CMPXCHG16B exact 128-bit total");
    check(g_vcpu_switches >= 16, "smp: preemptive vCPU switches occurred");

    /* On x86-64-v3, also verify full 256-bit YMM0/YMM1 isolation across 4 vCPUs! */
    if (lvl >= 3) {
        uint64_t pc_v3 = 0x11200;
        memcpy((void *)(uintptr_t)pc_v3, code_smp_worker_v3_ymm, sizeof(code_smp_worker_v3_ymm));
        memset(shared, 0, 64);
        memset(percpu_base, 0, 4 * 256);
        for (uint32_t c = 0; c < MAX_VCPUS; c++) {
            uint64_t *buf = (uint64_t *)((uint8_t *)percpu_base + c * 256);
            for (int k = 0; k < 8; k++) {
                buf[k] = 0xe000000000000000ULL | ((uint64_t)c << 48) | ((uint64_t)k << 8) | 0x5au;
            }
            reset_guest_env(0x30000 + c * 0x1000);
            env.eip = pc_v3;
            env.regs[R_EDI] = (uint64_t)(uintptr_t)shared;
            env.regs[R_ESI] = c;
            env.regs[R_R8] = (uint64_t)(uintptr_t)buf;
            env.regs[R_R11] = iters;
            g_vcpu_save[c] = env;
            done[c] = 0;
        }
        g_cur_vcpu = 0;
        env = g_vcpu_save[0];
        g_plv3_timer_preempt = 1;
        timer_ack();
        timer_start(2000);

        total_steps = 0;
        while (total_steps < 50000) {
            int all_done = 1;
            for (uint32_t c = 0; c < MAX_VCPUS; c++) {
                if (!done[c]) { all_done = 0; break; }
            }
            if (all_done) break;
            if (done[g_cur_vcpu]) {
                for (uint32_t s = 1; s < MAX_VCPUS; s++) {
                    uint32_t n = (g_cur_vcpu + s) % MAX_VCPUS;
                    if (!done[n]) { switch_vcpu(n); break; }
                }
                continue;
            }
            uint64_t pc = env.eip;
            TbCacheEntry *e = tbc_lookup(&tb_cache, pc, 0, 0, 0);
            if (!e) {
                e = tbc_insert(&tb_cache, pc, 0, 0, 0);
                if (!e) break;
                uint32_t slot = (uint32_t)(e - tb_entries);
                if (slot + 1 > g_tb_highwater) g_tb_highwater = slot + 1;
                uint8_t *dst = code_cache + code_used;
                int rc = tb_harness_translate_at_with_map(pc, dst, CODE_CACHE_SIZE - code_used,
                                                          &tb_insn_maps[slot]);
                if (rc <= 0) break;
                e->code = (uint64_t)(uintptr_t)dst;
                e->code_size = (uint32_t)rc;
                code_used += (((uint64_t)rc + 255) & ~UINT64_C(255));
                __asm__ volatile("ibar 0" ::: "memory");
            }
            regress_run_plv3(pro_buf, (void *)(uintptr_t)e->code, &env);
            total_steps++;
            if (g_plv3_exit.reason != 1) break;
            if (*(volatile uint8_t *)(uintptr_t)env.eip == 0xf4u) {
                done[g_cur_vcpu] = 1;
            }
            if (g_vcpu_preempt_needed || (total_steps & 3) == 0 || done[g_cur_vcpu]) {
                g_vcpu_preempt_needed = 0;
                for (uint32_t s = 1; s < MAX_VCPUS; s++) {
                    uint32_t n = (g_cur_vcpu + s) % MAX_VCPUS;
                    if (!done[n]) { switch_vcpu(n); break; }
                }
            }
        }
        timer_stop();
        timer_ack();
        g_plv3_timer_preempt = 0;

        int ymm_ok = 1;
        for (uint32_t c = 0; c < MAX_VCPUS; c++) {
            uint64_t *buf = (uint64_t *)((uint8_t *)percpu_base + c * 256);
            if (!done[c] || buf[16] != 1) ymm_ok = 0;
        }
        check(ymm_ok && shared[0] == MAX_VCPUS * iters,
              "smp: 4-vCPU 256-bit YMM0/YMM1 isolation under timer preemption");
    }

    /* 4c. 3-AP HLT + Fixed IPI wake-up + IRETQ */
    {
        uint64_t pc_wait = 0x11400;
        uint64_t pc_isr = 0x11500;
        uint64_t *post_hlt = (uint64_t *)(uintptr_t)0x23000;
        uint64_t *ipi_cnt = (uint64_t *)(uintptr_t)0x23080;

        memcpy((void *)(uintptr_t)pc_wait, code_ipi_waiter, sizeof(code_ipi_waiter));
        memcpy((void *)(uintptr_t)pc_isr, code_ipi_handler, sizeof(code_ipi_handler));
        memset(post_hlt, 0, 64);
        memset(ipi_cnt, 0, 64);

        int ipi_ok = 1;
        for (uint32_t c = 1; c < MAX_VCPUS; c++) {
            reset_guest_env(0x34000 + c * 0x1000);
            env.segs[R_CS].selector = 0x08;
            env.segs[R_SS].selector = 0x10;
            env.regs[R_EDI] = (uint64_t)(uintptr_t)&post_hlt[c];
            env.regs[R_ESI] = (uint64_t)(uintptr_t)&ipi_cnt[c];
            /* Run until first HLT (offset +1) */
            if (run_snippet_until_hlt(pc_wait, 8) <= 0 || env.eip != pc_wait + 1) {
                ipi_ok = 0;
                continue;
            }
            /* Inject Fixed IPI frame pushing return RIP = pc_wait + 2 (past HLT) */
            uint64_t rsp = env.regs[R_ESP];
            uint64_t *frame = (uint64_t *)(uintptr_t)(rsp - 40);
            frame[0] = pc_wait + 2;
            frame[1] = 0x08;
            frame[2] = env.eflags;
            frame[3] = rsp;
            frame[4] = 0x10;
            env.regs[R_ESP] = (uint64_t)(uintptr_t)frame;
            env.eflags &= ~IF_MASK;
            /* Execute ISR -> IRETQ -> movq $0x55aa, (%rdi) -> final HLT */
            if (run_snippet_until_hlt(pc_isr, 16) <= 0 ||
                ipi_cnt[c] != 1 || post_hlt[c] != 0x55aaULL) {
                ipi_ok = 0;
            }
        }
        check(ipi_ok, "smp: 3-AP HLT + IPI delivery + IRETQ post-HLT execution");
    }
}

void kmain(void)
{
    uint64_t euen, crmd;
    uint32_t cfg2 = 0;

    latx_bare_uart_puts("regress: start\n");

    __asm__ volatile("cpucfg %0, %1" : "=r"(cfg2) : "r"(2));
    euen = csrrd(0x2);
    euen |= 0x1 | 0x2 | 0x40 | 0x80;
    if ((cfg2 & (1u << 7)) != 0) {
        euen |= 0x4; /* ASXE: enable LASX */
    }
    csrwr(0x2, euen);
    crmd = csrrd(0x0);
    crmd &= ~(uint64_t)0x4;
    csrwr(0x0, crmd);
    if ((cfg2 & (1u << 18)) == 0) {
        g_use_sw_lbt = 1;
    }

    memset(&env, 0, sizeof(env));
    memset(&tr, 0, sizeof(tr));
    memset(&lsenv_storage, 0, sizeof(lsenv_storage));
    memset(&tcg_storage, 0, sizeof(tcg_storage));
    memset(fast_jmp_cache, 0, sizeof(fast_jmp_cache));
    memset(code_cache, 0, sizeof(code_cache));
    memset(epi_buf, 0, sizeof(epi_buf));
    memset(pro_buf, 0, sizeof(pro_buf));

    env.sys.codemode = 1;
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

    uint32_t lvl = (uint32_t)latx_cpu_x86_level();
    latx_bare_uart_puts("regress: x86_level=");
    puthex(lvl);
    latx_bare_uart_puts(" lbt_hw=");
    puthex((uint64_t)!g_use_sw_lbt);
    latx_bare_uart_puts("\n");

    option_guest_paging = 0;
    option_save_xmm = 0xff;
    option_lbt_jiscr = !g_use_sw_lbt;

    latx_gen_epilogue(epi_buf);
    latx_gen_prologue(pro_buf);
    qemu_loglevel |= CPU_LOG_TB_NOCHAIN;
    check(tbc_init(&tb_cache, tb_entries, tb_buckets, TB_TABLE_CAP) == 1,
          "init: tb cache");

    lapg_pool_init(&g_pool, (uint64_t)(uintptr_t)g_pool_mem, sizeof(g_pool_mem));
    check(lapg_pool_alloc(&g_pool, LAPG_PAGE_SIZE, &g_root) == 1, "init: root pt");
    lapg_zero_page((void *)(uintptr_t)g_root);
    map_host_range((uint64_t)(uintptr_t)code_cache, CODE_CACHE_SIZE,
                   LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_EXEC | LAPG_PERM_USER);
    map_host_range((uint64_t)(uintptr_t)pro_buf, sizeof(pro_buf),
                   LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_EXEC | LAPG_PERM_USER);
    map_host_range((uint64_t)(uintptr_t)epi_buf, sizeof(epi_buf),
                   LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_EXEC | LAPG_PERM_USER);
    map_host_range((uint64_t)(uintptr_t)&env, sizeof(env),
                   LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_USER);
    map_host_range((uint64_t)(uintptr_t)fast_jmp_cache, sizeof(fast_jmp_cache),
                   LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_USER);
    map_host_range((uint64_t)(uintptr_t)kstack, sizeof(kstack),
                   LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_USER);
    map_host_range((uint64_t)(uintptr_t)pf_table, sizeof(pf_table),
                   LAPG_PERM_READ | LAPG_PERM_USER);

    csrwr(0x1c, lapg_pwcl_value());
    csrwr(0x1d, lapg_pwch_value());
    csrwr(0x1e, lapg_stlbps_value());
    csrwr(0x4, csrrd(0x4) & ~(UINT64_C(0x7) << 16));
    csrwr(0xc, (uint64_t)(uintptr_t)regress_trap);
    csrwr(0x88, (uint64_t)(uintptr_t)regress_trap);
    csrwr(0x180, (UINT64_C(1) << 4) | 1u);
    {
        uint64_t c = csrrd(0x0);
        c &= ~UINT64_C(0x1f);
        c |= UINT64_C(1) << 4;
        csrwr(0x0, c);
    }
    __asm__ volatile("invtlb 0x0, $zero, $zero" ::: "memory");

    test_cpuid_abi(lvl);
    test_v2_translation();
    if (lvl >= 3) {
        test_v3_translation();
    }
    test_smp_isolation_and_atomics(lvl);

    latx_bare_uart_puts("regress: checks=");
    puthex((uint64_t)checks);
    latx_bare_uart_puts(" fails=");
    puthex((uint64_t)fails);
    latx_bare_uart_puts(fails ? " BAD\n" : " ok\n");
    latx_bare_uart_puts("regress: done\n");
    for (;;) {
    }
}
