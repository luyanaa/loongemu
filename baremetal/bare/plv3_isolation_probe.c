/*
 * Standalone P0 PLV3 isolation probe harness.
 *
 * Designed for non-LVZ LoongArch64 compatibility (2K3000 / 3B6000M / QEMU
 * -cpu la464 and -cpu max): uses standard VALEN=48 4-level 4 KiB page tables
 * (CSR.PWCL/PWCH/STLBPS), separate PLV0/PLV3 page-table roots (PGDH/PGDL),
 * split 4 KiB-aligned TLBRENTRY and EENTRY vectors, atomic CSR.SAVE0 trap
 * stack switching, and Execute-Only PLV3 code pages.
 *
 * Host state (VMM .text/.rodata/.data/.bss, g_host_kstack, g_host_trap_stack,
 * g_vcpu_env, g_plv0_secret, g_pool_mem) is NEVER mapped in g_plv3_root and
 * NEVER accessed directly at PLV3. All prologue/epilogue guest register spills
 * execute strictly at PLV0.
 */

#include <stdint.h>

#include "loongemu/loongarch_paging.h"

#define UART0_THR ((volatile unsigned char *)0x1fe001e0)
#define UART0_LSR ((volatile unsigned char *)0x1fe001e5)

static void uart_putc(char c)
{
    for (unsigned spin = 0; spin < 100000u; spin++) {
        if (*UART0_LSR & 0x20u) {
            break;
        }
    }
    *UART0_THR = (unsigned char)c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}

static void puthex(uint64_t v)
{
    static const char d[] = "0123456789abcdef";

    for (int i = 60; i >= 0; i -= 4) {
        uart_putc(d[(v >> i) & 0xf]);
    }
}

static int g_checks;
static int g_fails;

static void check(int cond, const char *what)
{
    g_checks++;
    if (!cond) {
        g_fails++;
        uart_puts("plv3-iso: FAIL ");
        uart_puts(what);
        uart_puts("\n");
    }
}

/* LoongArch ESTAT.Ecode values. */
#define EXC_PIL  0x01u
#define EXC_PIS  0x02u
#define EXC_PIF  0x03u
#define EXC_PME  0x04u
#define EXC_PNR  0x05u
#define EXC_PNX  0x06u
#define EXC_PPI  0x07u
#define EXC_ADE  0x08u
#define EXC_ALE  0x09u
#define EXC_SYS  0x0bu
#define EXC_BRK  0x0cu
#define EXC_IPE  0x0du
#define EXC_GSPR 0x0eu

/* LoongArch GPR indices in saved trap frame. */
#define LA_RA  1
#define LA_TP  2
#define LA_SP  3
#define LA_A0  4
#define LA_A1  5
#define LA_A2  6
#define LA_A3  7
#define LA_A4  8
#define LA_A5  9
#define LA_A6  10
#define LA_A7  11
#define LA_T0  12
#define LA_FP  22
#define LA_S0  23
#define LA_S1  24
#define LA_S2  25
#define LA_S3  26
#define LA_S4  27
#define LA_S5  28
#define LA_S6  29
#define LA_S7  30
#define LA_S8  31
#define LA_T5  17
#define LA_T6  18
#define LA_T7  19
#define LA_T8  20

#define GUEST_CODE_VA  UINT64_C(0x01000000)
#define GUEST_XO_VA    UINT64_C(0x01004000)
#define GUEST_DATA_VA  UINT64_C(0x01100000)
#define GUEST_RO_VA    UINT64_C(0x01102000)
#define GUEST_STACK_VA UINT64_C(0x01200000)

#define KSTACK_CANARY     UINT64_C(0x55aa112233447788)
#define ENV_CANARY_LO     UINT64_C(0xfeedfacecafebeef)
#define ENV_CANARY_HI     UINT64_C(0x0123456789abcdef)
#define SECRET_MAGIC      UINT64_C(0xdeadbeefcafebabe)
#define RO_MAGIC          UINT64_C(0x1122334455667788)

typedef struct ProbeVcpuEnv {
    uint64_t canary_lo;
    uint64_t gpr[16];
    uint64_t canary_hi;
} ProbeVcpuEnv;

typedef enum ProbeExitReason {
    PROBE_EXIT_NONE    = 0,
    PROBE_EXIT_SYSCALL = 1,
    PROBE_EXIT_FAULT   = 2,
} ProbeExitReason;

typedef struct ProbeExitInfo {
    uint64_t reason;
    uint64_t ecode;
    uint64_t is_refill;
    uint64_t era;
    uint64_t badv;
    uint64_t guest_sp;
} ProbeExitInfo;

/* PLV0-only host structures (all live >= 0x8000000, never mapped in g_plv3_root). */
static uint64_t g_pool_mem[128 * (LAPG_PAGE_SIZE / 8)]
    __attribute__((aligned(LAPG_PAGE_SIZE)));
static unsigned char g_host_kstack[65536]
    __attribute__((aligned(LAPG_PAGE_SIZE), used));
static unsigned char g_host_trap_stack[32768]
    __attribute__((aligned(LAPG_PAGE_SIZE), used));
static ProbeVcpuEnv g_vcpu_env __attribute__((aligned(LAPG_PAGE_SIZE)));
static uint64_t g_plv0_secret[LAPG_PAGE_SIZE / 8]
    __attribute__((aligned(LAPG_PAGE_SIZE)));

/* Separate guest backing buffers (mapped only in g_plv3_root at low guest VAs). */
static unsigned char g_guest_code_page[LAPG_PAGE_SIZE]
    __attribute__((aligned(LAPG_PAGE_SIZE)));
static uint32_t g_guest_xo_page[LAPG_PAGE_SIZE / 4]
    __attribute__((aligned(LAPG_PAGE_SIZE)));
static uint64_t g_guest_data_page[LAPG_PAGE_SIZE / 8]
    __attribute__((aligned(LAPG_PAGE_SIZE)));
static uint64_t g_guest_ro_page[LAPG_PAGE_SIZE / 8]
    __attribute__((aligned(LAPG_PAGE_SIZE)));
static uint64_t g_guest_stack_page[LAPG_PAGE_SIZE / 8]
    __attribute__((aligned(LAPG_PAGE_SIZE)));

static LapgPagePool g_pool;
static uint64_t g_plv0_root;
static uint64_t g_plv3_root;

static volatile uint64_t g_tlb_fills;
static volatile uint64_t g_fault_count;
static volatile ProbeExitInfo g_probe_exit;

uint64_t g_saved_plv0_sp __attribute__((used));
ProbeVcpuEnv *g_active_vcpu_env __attribute__((used));

extern char _end[];
void _start(void);
void kmain(void);
void probe_tlbr_entry(void);
void probe_eentry(void);
void probe_run_plv3(uint64_t entry_va, uint64_t arg0, ProbeVcpuEnv *vcpu_env);
extern char probe_epilogue_plv0[];

extern char guest_stub_positive_start[];
extern char guest_stub_positive_end[];
extern char guest_stub_load_start[];
extern char guest_stub_load_end[];
extern char guest_stub_store_start[];
extern char guest_stub_store_end[];
extern char guest_stub_exec_start[];
extern char guest_stub_exec_end[];
extern char guest_stub_priv_start[];
extern char guest_stub_priv_end[];

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
    R(0x0) R(0x1) R(0x4) R(0x5) R(0x6) R(0x7) R(0xc)
    R(0x10) R(0x11) R(0x12) R(0x13) R(0x18) R(0x19) R(0x1a)
    R(0x1c) R(0x1d) R(0x1e) R(0x30)
    R(0x88) R(0x89) R(0x8a) R(0x8c) R(0x8d) R(0x8e) R(0x8f)
    R(0x180) R(0x181) R(0x182) R(0x183)
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
    W(0x0) W(0x1) W(0x4) W(0x6) W(0xc)
    W(0x10) W(0x11) W(0x12) W(0x13) W(0x18) W(0x19) W(0x1a)
    W(0x1c) W(0x1d) W(0x1e) W(0x30)
    W(0x88) W(0x8a) W(0x8c) W(0x8d) W(0x8e) W(0x8f)
    W(0x180) W(0x181) W(0x182) W(0x183)
#undef W
    default:
        break;
    }
}

static void zero_page_phys(uint64_t phys)
{
    lapg_zero_page((void *)(uintptr_t)phys);
}

static void copy_bytes(void *dst, const void *src, uint64_t len)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    for (uint64_t i = 0; i < len; i++) {
        d[i] = s[i];
    }
}

static int map_range_mat(uint64_t root, uint64_t va_start, uint64_t pa_start,
                         uint64_t size, uint32_t perm, uint32_t mat)
{
    uint64_t off = va_start & ~LAPG_PAGE_MASK;
    uint64_t va = va_start & LAPG_PAGE_MASK;
    uint64_t pa = pa_start & LAPG_PAGE_MASK;
    uint64_t end = (va_start + size + LAPG_PAGE_SIZE - 1u) & LAPG_PAGE_MASK;

    (void)off;
    for (; va < end; va += LAPG_PAGE_SIZE, pa += LAPG_PAGE_SIZE) {
        if (lapg_map_mat(&g_memory, &g_pool, root, va, pa, perm, mat) != LAPG_OK) {
            return 0;
        }
    }
    return 1;
}

static uint32_t classify_fault_access(uint64_t fault_pc, uint64_t badv,
                                      uint64_t ecode, int is_refill)
{
    if (fault_pc == badv || (!is_refill && (ecode == EXC_PIF || ecode == EXC_PNX))) {
        return LAPG_PERM_EXEC | LAPG_PERM_USER;
    }
    if (!is_refill && (ecode == EXC_PIS || ecode == EXC_PME)) {
        return LAPG_PERM_WRITE | LAPG_PERM_USER;
    }
    if (!is_refill && (ecode == EXC_PIL || ecode == EXC_PNR)) {
        return LAPG_PERM_READ | LAPG_PERM_USER;
    }

    /* Refill / PPI on data load or store: translate fault_pc via g_plv3_root. */
    {
        LapgTranslation pc_tr;
        uint32_t insn;
        uint32_t major;
        int is_store = 0;

        if (lapg_walk(&g_memory, g_plv3_root, fault_pc,
                      LAPG_PERM_EXEC | LAPG_PERM_USER, &pc_tr) != LAPG_OK) {
            return LAPG_PERM_READ | LAPG_PERM_USER;
        }
        insn = *(volatile uint32_t *)(uintptr_t)pc_tr.phys;
        major = (insn >> 26) & 0x3fu;
        if (major == 0x0au) {
            uint32_t op8 = (insn >> 24) & 0xffu;
            if (op8 == 0x29u) {
                is_store = 1;
            } else if (op8 == 0x2bu) {
                is_store = ((insn >> 22) & 1u) != 0;
            }
        } else if (major == 0x08u || major == 0x09u) {
            is_store = ((insn >> 24) & 1u) != 0;
        } else if (major == 0x0bu || major == 0x0cu) {
            is_store = ((insn >> 24) & 1u) != 0;
        } else if (major == 0x0eu) {
            uint32_t op22 = (insn >> 15) & 0x7ffu;
            if (op22 == 0x020u || op22 == 0x028u || op22 == 0x030u ||
                op22 == 0x038u || op22 == 0x080u || op22 == 0x088u ||
                op22 == 0x090u || op22 == 0x098u ||
                (op22 >= 0x0c0u && op22 <= 0x0ffu)) {
                is_store = 1;
            }
        }
        return (is_store ? LAPG_PERM_WRITE : LAPG_PERM_READ) | LAPG_PERM_USER;
    }
}

static void save_guest_gprs_from_trap(const uint64_t *regs)
{
    static const int la_map[16] = {
        LA_S0, LA_S1, LA_S2, LA_S3, LA_S4, LA_S5, LA_S6, LA_S7,
        LA_A2, LA_A3, LA_A4, LA_A5, LA_T5, LA_T6, LA_T7, LA_T8,
    };

    if (g_active_vcpu_env == 0) {
        return;
    }
    for (int i = 0; i < 16; i++) {
        g_active_vcpu_env->gpr[i] = regs[la_map[i]];
    }
}

static void abort_to_plv0_epilogue(int is_refill)
{
    if (is_refill) {
        csrwr(0x8f, 0);
        csrwr(0x8a, ((uint64_t)(uintptr_t)probe_epilogue_plv0 & ~UINT64_C(3)) | 1u);
    } else {
        uint64_t prmd = csrrd(0x1) & ~UINT64_C(0x7);
        csrwr(0x1, prmd);
        csrwr(0x6, (uint64_t)(uintptr_t)probe_epilogue_plv0);
    }
}

void probe_handle_trap(uint64_t *regs, int is_refill)
{
    uint64_t tlbrera = csrrd(0x8a);
    uint64_t estat = csrrd(0x5);
    uint64_t ecode = (estat >> 16) & 0x3fu;
    uint64_t era = is_refill ? ((tlbrera >> 2) << 2) : csrrd(0x6);
    uint64_t badv = is_refill ? csrrd(0x89) : csrrd(0x7);

    g_fault_count++;

    if (!is_refill && ecode == EXC_SYS) {
        save_guest_gprs_from_trap(regs);
        g_probe_exit.reason = PROBE_EXIT_SYSCALL;
        g_probe_exit.ecode = ecode;
        g_probe_exit.is_refill = 0;
        g_probe_exit.era = era;
        g_probe_exit.badv = badv;
        g_probe_exit.guest_sp = regs[LA_SP];
        abort_to_plv0_epilogue(0);
        return;
    }

    if (is_refill) {
        uint32_t access = classify_fault_access(era, badv, ecode, 1);
        uint64_t vppn = badv & UINT64_C(0x0000ffffffffe000);
        LapgTranslation tr_fault, tr0, tr1;

        if (lapg_walk(&g_memory, g_plv3_root, badv, access, &tr_fault) == LAPG_OK) {
            uint64_t pte0 = 0;
            uint64_t pte1 = 0;

            if (lapg_walk(&g_memory, g_plv3_root, vppn, LAPG_PERM_USER, &tr0) == LAPG_OK) {
                pte0 = tr0.pte & ~LAPG_PTE_G;
            }
            if (lapg_walk(&g_memory, g_plv3_root, vppn + LAPG_PAGE_SIZE,
                          LAPG_PERM_USER, &tr1) == LAPG_OK) {
                pte1 = tr1.pte & ~LAPG_PTE_G;
            }
            csrwr(0x8e, vppn | (uint64_t)LAPG_PAGE_BITS);
            csrwr(0x8c, pte0);
            csrwr(0x8d, pte1);
            __asm__ volatile("tlbfill" ::: "memory");
            g_tlb_fills++;
            return;
        }
    }

    g_probe_exit.reason = PROBE_EXIT_FAULT;
    g_probe_exit.ecode = ecode;
    g_probe_exit.is_refill = (uint64_t)(is_refill ? 1 : 0);
    g_probe_exit.era = era;
    g_probe_exit.badv = badv;
    g_probe_exit.guest_sp = regs[LA_SP];
    abort_to_plv0_epilogue(is_refill);
}

/*
 * Boot entry, split 4 KiB-aligned trap vectors with atomic CSR.SAVE0 trap-stack
 * switching, PLV0 prologue/epilogue, and standalone PIC guest stubs.
 */
__asm__(
".text\n"
".globl _start\n"
"_start:\n"
"  la   $sp, g_host_kstack\n"
"  lu12i.w $t0, 16\n"        /* +65536 */
"  add.d $sp, $sp, $t0\n"
"  bl   kmain\n"
"1: b   1b\n"

/* Common trap save/restore macro; trap_arg is 1 for TLBR, 0 for EENTRY. */
".macro PROBE_TRAP_STUB name, trap_arg\n"
".balign 4096\n"
".globl \\name\n"
"\\name:\n"
"  csrwr  $sp, 0x30\n"       /* atomically swap PLV3 $sp with host trap stack */
"  addi.d $sp, $sp, -256\n"
"  st.d   $zero, $sp, 0\n"
"  st.d   $ra,   $sp, 8\n"
"  st.d   $tp,   $sp, 16\n"
"  st.d   $a0,   $sp, 32\n"
"  st.d   $a1,   $sp, 40\n"
"  st.d   $a2,   $sp, 48\n"
"  st.d   $a3,   $sp, 56\n"
"  st.d   $a4,   $sp, 64\n"
"  st.d   $a5,   $sp, 72\n"
"  st.d   $a6,   $sp, 80\n"
"  st.d   $a7,   $sp, 88\n"
"  st.d   $t0,   $sp, 96\n"
"  st.d   $t1,   $sp, 104\n"
"  st.d   $t2,   $sp, 112\n"
"  st.d   $t3,   $sp, 120\n"
"  st.d   $t4,   $sp, 128\n"
"  st.d   $t5,   $sp, 136\n"
"  st.d   $t6,   $sp, 144\n"
"  st.d   $t7,   $sp, 152\n"
"  st.d   $t8,   $sp, 160\n"
"  st.d   $r21,  $sp, 168\n"
"  st.d   $fp,   $sp, 176\n"
"  st.d   $s0,   $sp, 184\n"
"  st.d   $s1,   $sp, 192\n"
"  st.d   $s2,   $sp, 200\n"
"  st.d   $s3,   $sp, 208\n"
"  st.d   $s4,   $sp, 216\n"
"  st.d   $s5,   $sp, 224\n"
"  st.d   $s6,   $sp, 232\n"
"  st.d   $s7,   $sp, 240\n"
"  st.d   $s8,   $sp, 248\n"
"  csrrd  $t0,   0x30\n"
"  st.d   $t0,   $sp, 24\n"  /* save interrupted $sp into regs[3] */
"  addi.d $t0,   $sp, 256\n"
"  csrwr  $t0,   0x30\n"     /* restore trap stack top in CSR.SAVE0 */
"  move   $a0,   $sp\n"
"  ori    $a1,   $zero, \\trap_arg\n"
"  bl     probe_handle_trap\n"
"  ld.d   $ra,   $sp, 8\n"
"  ld.d   $tp,   $sp, 16\n"
"  ld.d   $a0,   $sp, 32\n"
"  ld.d   $a1,   $sp, 40\n"
"  ld.d   $a2,   $sp, 48\n"
"  ld.d   $a3,   $sp, 56\n"
"  ld.d   $a4,   $sp, 64\n"
"  ld.d   $a5,   $sp, 72\n"
"  ld.d   $a6,   $sp, 80\n"
"  ld.d   $a7,   $sp, 88\n"
"  ld.d   $t1,   $sp, 104\n"
"  ld.d   $t2,   $sp, 112\n"
"  ld.d   $t3,   $sp, 120\n"
"  ld.d   $t4,   $sp, 128\n"
"  ld.d   $t5,   $sp, 136\n"
"  ld.d   $t6,   $sp, 144\n"
"  ld.d   $t7,   $sp, 152\n"
"  ld.d   $t8,   $sp, 160\n"
"  ld.d   $r21,  $sp, 168\n"
"  ld.d   $fp,   $sp, 176\n"
"  ld.d   $s0,   $sp, 184\n"
"  ld.d   $s1,   $sp, 192\n"
"  ld.d   $s2,   $sp, 200\n"
"  ld.d   $s3,   $sp, 208\n"
"  ld.d   $s4,   $sp, 216\n"
"  ld.d   $s5,   $sp, 224\n"
"  ld.d   $s6,   $sp, 232\n"
"  ld.d   $s7,   $sp, 240\n"
"  ld.d   $s8,   $sp, 248\n"
"  ld.d   $t0,   $sp, 24\n"
"  csrwr  $t0,   0x30\n"     /* stage interrupted $sp in CSR.SAVE0 */
"  ld.d   $t0,   $sp, 96\n"
"  addi.d $sp,   $sp, 256\n"
"  csrwr  $sp,   0x30\n"     /* atomically swap back $sp and trap_stack_top */
"  ertn\n"
".endm\n"

"PROBE_TRAP_STUB probe_tlbr_entry, 1\n"
"PROBE_TRAP_STUB probe_eentry, 0\n"

/*
 * PLV0 prologue -> ERTN -> PLV3 -> trap -> PLV0 epilogue.
 * void probe_run_plv3(uint64_t entry_va, uint64_t arg0, ProbeVcpuEnv *vcpu_env)
 */
".text\n"
".globl probe_run_plv3\n"
"probe_run_plv3:\n"
"  addi.d $sp, $sp, -112\n"
"  st.d   $ra, $sp, 0\n"
"  st.d   $fp, $sp, 8\n"
"  st.d   $s0, $sp, 16\n"
"  st.d   $s1, $sp, 24\n"
"  st.d   $s2, $sp, 32\n"
"  st.d   $s3, $sp, 40\n"
"  st.d   $s4, $sp, 48\n"
"  st.d   $s5, $sp, 56\n"
"  st.d   $s6, $sp, 64\n"
"  st.d   $s7, $sp, 72\n"
"  st.d   $s8, $sp, 80\n"
"  la     $t0, g_saved_plv0_sp\n"
"  st.d   $sp, $t0, 0\n"
"  la     $t0, g_active_vcpu_env\n"
"  st.d   $a2, $t0, 0\n"
"  la     $t0, g_host_trap_stack\n"
"  lu12i.w $t1, 8\n"         /* +32768 */
"  add.d  $t0, $t0, $t1\n"
"  csrwr  $t0, 0x30\n"       /* CSR.SAVE0 := trap stack top */
/* Load 16 guest GPRs from ProbeVcpuEnv in PLV0 (vcpu_env never touched at PLV3). */
"  ld.d   $s0, $a2, 8\n"
"  ld.d   $s1, $a2, 16\n"
"  ld.d   $s2, $a2, 24\n"
"  ld.d   $s3, $a2, 32\n"
"  ld.d   $s4, $a2, 40\n"
"  ld.d   $s5, $a2, 48\n"
"  ld.d   $s6, $a2, 56\n"
"  ld.d   $s7, $a2, 64\n"
"  ld.d   $t5, $a2, 104\n"
"  ld.d   $t6, $a2, 112\n"
"  ld.d   $t7, $a2, 120\n"
"  ld.d   $t8, $a2, 128\n"
"  ld.d   $a3, $a2, 80\n"
"  ld.d   $a4, $a2, 88\n"
"  ld.d   $a5, $a2, 96\n"
"  ld.d   $a2, $a2, 72\n"
/* Program PRMD := PPLV=3, PIE=0 and ERA := entry_va. */
"  csrrd  $t0, 0x1\n"
"  srli.d $t0, $t0, 3\n"
"  slli.d $t0, $t0, 3\n"
"  ori    $t0, $t0, 0x3\n"
"  csrwr  $t0, 0x1\n"
"  csrwr  $a0, 0x6\n"
/* Prepare clean PLV3 register state: $a0=arg0, $sp=GUEST_STACK_VA+0x800, clear host ptrs. */
"  move   $a0, $a1\n"
"  lu12i.w $sp, 0x1200\n"
"  ori    $sp, $sp, 0x800\n"
"  move   $ra,  $zero\n"
"  move   $tp,  $zero\n"
"  move   $a1,  $zero\n"
"  move   $a6,  $zero\n"
"  move   $a7,  $zero\n"
"  move   $t0,  $zero\n"
"  move   $t1,  $zero\n"
"  move   $t2,  $zero\n"
"  move   $t3,  $zero\n"
"  move   $t4,  $zero\n"
"  move   $r21, $zero\n"
"  move   $fp,  $zero\n"
"  move   $s8,  $zero\n"
"  ertn\n"

".globl probe_epilogue_plv0\n"
"probe_epilogue_plv0:\n"
"  la     $t0, g_saved_plv0_sp\n"
"  ld.d   $sp, $t0, 0\n"
"  ld.d   $ra, $sp, 0\n"
"  ld.d   $fp, $sp, 8\n"
"  ld.d   $s0, $sp, 16\n"
"  ld.d   $s1, $sp, 24\n"
"  ld.d   $s2, $sp, 32\n"
"  ld.d   $s3, $sp, 40\n"
"  ld.d   $s4, $sp, 48\n"
"  ld.d   $s5, $sp, 56\n"
"  ld.d   $s6, $sp, 64\n"
"  ld.d   $s7, $sp, 72\n"
"  ld.d   $s8, $sp, 80\n"
"  addi.d $sp, $sp, 112\n"
"  jirl   $zero, $ra, 0\n"

/* Position-independent PLV3 guest stubs copied to GUEST_CODE_VA. */
".globl guest_stub_positive_start\n"
"guest_stub_positive_start:\n"
"  lu12i.w $t1, 0x1102\n"    /* GUEST_RO_VA = 0x01102000 */
"  ld.d    $t0, $t1, 0\n"
"  add.d   $s0, $s0, $t0\n"
"  addi.d  $t5, $t5, 0x42\n"
"  addi.d  $sp, $sp, -16\n"
"  st.d    $s1, $sp, 0\n"
"  ld.d    $s2, $sp, 0\n"
"  addi.d  $sp, $sp, 16\n"
"  st.d    $s0, $a0, 0\n"    /* store into GUEST_DATA_VA */
"  lu12i.w $sp, 0xdead\n"    /* corrupt PLV3 $sp to prove atomic CSR.SAVE0 trap stack switch */
"  syscall 0\n"
".globl guest_stub_positive_end\n"
"guest_stub_positive_end:\n"

".globl guest_stub_load_start\n"
"guest_stub_load_start:\n"
"  ld.d    $s0, $a0, 0\n"
"  syscall 0\n"
".globl guest_stub_load_end\n"
"guest_stub_load_end:\n"

".globl guest_stub_store_start\n"
"guest_stub_store_start:\n"
"  lu12i.w $t0, 0x1337\n"
"  st.d    $t0, $a0, 0\n"
"  syscall 0\n"
".globl guest_stub_store_end\n"
"guest_stub_store_end:\n"

".globl guest_stub_exec_start\n"
"guest_stub_exec_start:\n"
"  jirl    $zero, $a0, 0\n"
"  syscall 0\n"
".globl guest_stub_exec_end\n"
"guest_stub_exec_end:\n"

".globl guest_stub_priv_start\n"
"guest_stub_priv_start:\n"
"  csrrd   $t0, 0x0\n"
"  syscall 0\n"
".globl guest_stub_priv_end\n"
"guest_stub_priv_end:\n"
);

static void reset_vcpu_env(ProbeVcpuEnv *e)
{
    e->canary_lo = ENV_CANARY_LO;
    for (int i = 0; i < 16; i++) {
        e->gpr[i] = UINT64_C(0x1000) + (uint64_t)i;
    }
    e->canary_hi = ENV_CANARY_HI;
}

static void verify_host_canaries(void)
{
    uint64_t *ks = (uint64_t *)(uintptr_t)g_host_kstack;
    uint64_t *ts = (uint64_t *)(uintptr_t)g_host_trap_stack;

    check(ks[0] == KSTACK_CANARY, "host kstack bottom canary intact");
    check(ts[0] == KSTACK_CANARY, "host trap stack bottom canary intact");
    check(g_vcpu_env.canary_lo == ENV_CANARY_LO &&
          g_vcpu_env.canary_hi == ENV_CANARY_HI,
          "g_vcpu_env canaries intact");
    check(g_plv0_secret[0] == SECRET_MAGIC, "g_plv0_secret unmodified");
}

static void run_negative_va_test(const char *label, uint64_t stub_va,
                                 uint64_t target_va)
{
    uint64_t fault_before = g_fault_count;

    g_probe_exit = (ProbeExitInfo){0};
    probe_run_plv3(stub_va, target_va, &g_vcpu_env);
    check(g_probe_exit.reason == PROBE_EXIT_FAULT &&
          g_probe_exit.badv == target_va &&
          g_fault_count > fault_before,
          label);
    verify_host_canaries();
}

void kmain(void)
{
    uint64_t img_start = (uint64_t)(uintptr_t)_start & LAPG_PAGE_MASK;
    uint64_t img_end = ((uint64_t)(uintptr_t)_end + LAPG_PAGE_SIZE - 1u) & LAPG_PAGE_MASK;
    uint64_t off_pos = 0, off_load, off_store, off_exec, off_priv;

    uart_puts("plv3-iso: start\n");

    /* Seed host canaries & guest fixtures. */
    *(uint64_t *)(uintptr_t)g_host_kstack = KSTACK_CANARY;
    *(uint64_t *)(uintptr_t)g_host_trap_stack = KSTACK_CANARY;
    reset_vcpu_env(&g_vcpu_env);
    g_plv0_secret[0] = SECRET_MAGIC;
    g_guest_ro_page[0] = RO_MAGIC;
    g_guest_data_page[0] = 0;

    /* Copy PIC guest stubs into g_guest_code_page (mapped Execute-Only at GUEST_CODE_VA). */
    {
        uint64_t cur = 0;
        uint64_t sz;

        sz = (uint64_t)(guest_stub_positive_end - guest_stub_positive_start);
        copy_bytes(g_guest_code_page + cur, guest_stub_positive_start, sz);
        cur += sz;

        off_load = cur;
        sz = (uint64_t)(guest_stub_load_end - guest_stub_load_start);
        copy_bytes(g_guest_code_page + cur, guest_stub_load_start, sz);
        cur += sz;

        off_store = cur;
        sz = (uint64_t)(guest_stub_store_end - guest_stub_store_start);
        copy_bytes(g_guest_code_page + cur, guest_stub_store_start, sz);
        cur += sz;

        off_exec = cur;
        sz = (uint64_t)(guest_stub_exec_end - guest_stub_exec_start);
        copy_bytes(g_guest_code_page + cur, guest_stub_exec_start, sz);
        cur += sz;

        off_priv = cur;
        sz = (uint64_t)(guest_stub_priv_end - guest_stub_priv_start);
        copy_bytes(g_guest_code_page + cur, guest_stub_priv_start, sz);
        cur += sz;

        check(cur <= LAPG_PAGE_SIZE, "guest probe stubs fit within 4 KiB");
        /* Put executable instructions in GUEST_XO_VA (Execute-Only) and GUEST_DATA_VA+8 (NX). */
        g_guest_xo_page[0] = 0x002b0000u; /* syscall 0 */
        *(volatile uint32_t *)((uintptr_t)g_guest_data_page + 8) = 0x002b0000u; /* syscall 0 */
    }

    /* 1. Build explicit PLV0-only root (g_plv0_root) and PLV3-only root (g_plv3_root). */
    lapg_pool_init(&g_pool, (uint64_t)(uintptr_t)g_pool_mem, sizeof(g_pool_mem));
    check(lapg_pool_alloc(&g_pool, LAPG_PAGE_SIZE, &g_plv0_root) == 1, "alloc g_plv0_root");
    check(lapg_pool_alloc(&g_pool, LAPG_PAGE_SIZE, &g_plv3_root) == 1, "alloc g_plv3_root");
    zero_page_phys(g_plv0_root);
    zero_page_phys(g_plv3_root);

    /* g_plv0_root maps VMM image + UART with PLV0-only (no LAPG_PERM_USER). */
    check(map_range_mat(g_plv0_root, img_start, img_start, img_end - img_start,
                        LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_EXEC,
                        LAPG_MAT_CC),
          "g_plv0_root maps VMM image PLV0-only");
    check(map_range_mat(g_plv0_root, UINT64_C(0x1fe00000), UINT64_C(0x1fe00000),
                        LAPG_PAGE_SIZE, LAPG_PERM_READ | LAPG_PERM_WRITE,
                        LAPG_MAT_SUC),
          "g_plv0_root maps UART PLV0-only SUC");

    /* g_plv3_root maps ONLY the dedicated guest pages. */
    check(map_range_mat(g_plv3_root, GUEST_CODE_VA,
                        (uint64_t)(uintptr_t)g_guest_code_page, LAPG_PAGE_SIZE,
                        LAPG_PERM_EXEC | LAPG_PERM_USER, LAPG_MAT_CC),
          "g_plv3_root maps GUEST_CODE_VA Execute-Only");
    check(map_range_mat(g_plv3_root, GUEST_XO_VA,
                        (uint64_t)(uintptr_t)g_guest_xo_page, LAPG_PAGE_SIZE,
                        LAPG_PERM_EXEC | LAPG_PERM_USER, LAPG_MAT_CC),
          "g_plv3_root maps GUEST_XO_VA Execute-Only");
    check(map_range_mat(g_plv3_root, GUEST_DATA_VA,
                        (uint64_t)(uintptr_t)g_guest_data_page, LAPG_PAGE_SIZE,
                        LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_USER,
                        LAPG_MAT_CC),
          "g_plv3_root maps GUEST_DATA_VA RW+NX");
    check(map_range_mat(g_plv3_root, GUEST_RO_VA,
                        (uint64_t)(uintptr_t)g_guest_ro_page, LAPG_PAGE_SIZE,
                        LAPG_PERM_READ | LAPG_PERM_USER, LAPG_MAT_CC),
          "g_plv3_root maps GUEST_RO_VA RO+NX");
    check(map_range_mat(g_plv3_root, GUEST_STACK_VA,
                        (uint64_t)(uintptr_t)g_guest_stack_page, LAPG_PAGE_SIZE,
                        LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_USER,
                        LAPG_MAT_CC),
          "g_plv3_root maps GUEST_STACK_VA RW+NX");

    /* Table-level isolation checks before entering PLV3. */
    {
        LapgTranslation tr;
        uint64_t secret_va = (uint64_t)(uintptr_t)g_plv0_secret;

        check(lapg_walk(&g_memory, g_plv0_root, secret_va, LAPG_PERM_READ, &tr) == LAPG_OK,
              "g_plv0_root permits PLV0 read of g_plv0_secret");
        check(lapg_walk(&g_memory, g_plv0_root, secret_va,
                        LAPG_PERM_READ | LAPG_PERM_USER, &tr) == LAPG_FAULT_PROTECTION,
              "g_plv0_root denies PLV3 read of g_plv0_secret");
        check(lapg_walk(&g_memory, g_plv3_root, (uint64_t)(uintptr_t)&g_vcpu_env,
                        LAPG_PERM_READ | LAPG_PERM_USER, &tr) == LAPG_FAULT_UNMAPPED,
              "g_plv3_root excludes g_vcpu_env");
        check(lapg_walk(&g_memory, g_plv3_root, (uint64_t)(uintptr_t)g_host_kstack,
                        LAPG_PERM_READ | LAPG_PERM_USER, &tr) == LAPG_FAULT_UNMAPPED,
              "g_plv3_root excludes g_host_kstack");
        check(lapg_walk(&g_memory, g_plv3_root, (uint64_t)(uintptr_t)g_host_trap_stack,
                        LAPG_PERM_READ | LAPG_PERM_USER, &tr) == LAPG_FAULT_UNMAPPED,
              "g_plv3_root excludes g_host_trap_stack");
        check(lapg_walk(&g_memory, g_plv3_root, secret_va,
                        LAPG_PERM_READ | LAPG_PERM_USER, &tr) == LAPG_FAULT_UNMAPPED,
              "g_plv3_root excludes g_plv0_secret");
        check(lapg_walk(&g_memory, g_plv3_root, (uint64_t)(uintptr_t)kmain,
                        LAPG_PERM_READ | LAPG_PERM_USER, &tr) == LAPG_FAULT_UNMAPPED,
              "g_plv3_root excludes VMM .text");
    }

    /* 2. Configure MMU CSRs, split trap vectors, DMW0 (PLV0-only), and enable paging. */
    csrwr(0x19, g_plv3_root);               /* CSR.PGDL */
    csrwr(0x1a, g_plv0_root);               /* CSR.PGDH */
    csrwr(0x1c, lapg_pwcl_value());         /* CSR.PWCL */
    csrwr(0x1d, lapg_pwch_value());         /* CSR.PWCH */
    csrwr(0x1e, lapg_stlbps_value());       /* CSR.STLBPS = 12 */
    csrwr(0x18, 1u);                        /* CSR.ASID = 1 */
    csrwr(0x4, csrrd(0x4) & ~(UINT64_C(0x7) << 16)); /* ECFG.VS = 0 */
    csrwr(0xc, (uint64_t)(uintptr_t)probe_eentry);
    csrwr(0x88, (uint64_t)(uintptr_t)probe_tlbr_entry);
    check((csrrd(0x88) & 0xfffULL) == 0u && (csrrd(0xc) & 0xfffULL) == 0u,
          "TLBRENTRY and EENTRY are 4 KiB aligned");

    csrwr(0x180, ((uint64_t)0 << 60) | (UINT64_C(1) << 4) | 1u); /* DMW0: PLV0 only */
    csrwr(0x181, 0);
    csrwr(0x182, 0);
    csrwr(0x183, 0);

    {
        uint64_t crmd = csrrd(0x0);
        crmd &= ~UINT64_C(0x1f);            /* clear PLV, IE, DA, PG */
        crmd |= UINT64_C(1) << 4;           /* PG = 1, DA = 0, PLV = 0 */
        csrwr(0x0, crmd);
    }
    __asm__ volatile("invtlb 0x0, $zero, $zero\n dbar 0\n ibar 0" ::: "memory");

    /* 3. Positive test: PLV0 prologue -> PLV3 TB (with corrupted $sp) -> PLV0 trap/epilogue. */
    reset_vcpu_env(&g_vcpu_env);
    g_probe_exit = (ProbeExitInfo){0};
    probe_run_plv3(GUEST_CODE_VA + off_pos, GUEST_DATA_VA, &g_vcpu_env);
    check(g_probe_exit.reason == PROBE_EXIT_SYSCALL &&
          g_probe_exit.guest_sp == UINT64_C(0x0dead000),
          "positive: PLV3 TB exited via syscall after corrupting $sp");
    check(g_tlb_fills >= 4u,
          "positive: PLV3 refills serviced for code, ro, data, and stack");
    check(g_vcpu_env.gpr[0] == UINT64_C(0x1000) + RO_MAGIC,
          "positive: guest $s0 received RO_MAGIC addition in PLV0 epilogue");
    check(g_vcpu_env.gpr[2] == UINT64_C(0x1001),
          "positive: guest stack push/pop updated $s2 from $s1");
    check(g_vcpu_env.gpr[12] == UINT64_C(0x100c) + 0x42u,
          "positive: guest $t5 updated and saved by PLV0 epilogue");
    check(g_guest_data_page[0] == UINT64_C(0x1000) + RO_MAGIC,
          "positive: PLV3 store landed in guest data page");
    verify_host_canaries();

    /* 4. Negative tests 1-5: Guest VA -> Host VMM state must fault & leave state intact. */
    __asm__ volatile("invtlb 0x3, $zero, $zero" ::: "memory");
    run_negative_va_test("neg1: PLV3 read of g_vcpu_env faults",
                         GUEST_CODE_VA + off_load,
                         (uint64_t)(uintptr_t)&g_vcpu_env);
    run_negative_va_test("neg2: PLV3 write of g_vcpu_env faults",
                         GUEST_CODE_VA + off_store,
                         (uint64_t)(uintptr_t)&g_vcpu_env.gpr[0]);
    run_negative_va_test("neg3: PLV3 read of g_host_kstack faults",
                         GUEST_CODE_VA + off_load,
                         (uint64_t)(uintptr_t)&g_host_kstack[32768]);
    run_negative_va_test("neg4: PLV3 write of g_host_trap_stack faults",
                         GUEST_CODE_VA + off_store,
                         (uint64_t)(uintptr_t)&g_host_trap_stack[16384]);
    run_negative_va_test("neg5a: PLV3 read of VMM .text faults",
                         GUEST_CODE_VA + off_load,
                         (uint64_t)(uintptr_t)kmain & ~UINT64_C(3));
    run_negative_va_test("neg5b: PLV3 read of g_plv0_secret faults",
                         GUEST_CODE_VA + off_load,
                         (uint64_t)(uintptr_t)&g_plv0_secret[0]);

    /* 5. Negative test 6: Warm PLV0-only TLB entry raises EXC_PPI (0x7) at PLV3. */
    {
        uint64_t secret_va = (uint64_t)(uintptr_t)&g_plv0_secret[0];
        uint64_t vppn = secret_va & UINT64_C(0x0000ffffffffe000);
        int is_odd = (int)((secret_va >> LAPG_PAGE_BITS) & 1u);
        LapgTranslation tr_sec;

        check(lapg_walk(&g_memory, g_plv0_root, secret_va, LAPG_PERM_READ,
                        &tr_sec) == LAPG_OK,
              "neg6: walk g_plv0_root for warm PLV0-only TLB fill");
        __asm__ volatile("invtlb 0x5, %0, %1" :: "r"(UINT64_C(1)), "r"(vppn) : "memory");
        csrwr(0x10, (uint64_t)LAPG_PAGE_BITS << 24);
        csrwr(0x11, vppn);
        csrwr(0x12, is_odd ? 0ULL : (tr_sec.pte & ~LAPG_PTE_G));
        csrwr(0x13, is_odd ? (tr_sec.pte & ~LAPG_PTE_G) : 0ULL);
        __asm__ volatile("tlbfill\n tlbsrch" ::: "memory");
        check((csrrd(0x10) & (UINT64_C(1) << 31)) == 0,
              "neg6: PLV0-only entry is warm in hardware TLB");

        g_probe_exit = (ProbeExitInfo){0};
        probe_run_plv3(GUEST_CODE_VA + off_load, secret_va, &g_vcpu_env);
        check(g_probe_exit.reason == PROBE_EXIT_FAULT &&
              g_probe_exit.is_refill == 0 &&
              g_probe_exit.ecode == EXC_PPI &&
              g_probe_exit.badv == secret_va,
              "neg6: warm PLV0-only TLB entry faults with EXC_PPI at PLV3");
        verify_host_canaries();
    }

    /*
     * 6. Negative test 7: Execute-Only (NR=1, NX=0) enforcement & EXC_PNR (0x5).
     *
     * Note on QEMU TCG versions lacking "target/loongarch: Clear PAGE_READ
     * when PTE.NR is set": when MMU_INST_FETCH caches an Execute-Only page in
     * QEMU's SoftMMU TLB first, QEMU mistakenly sets PAGE_READ in that SoftMMU
     * entry, masking a data load to that same page.  Using GUEST_XO_VA (warmed
     * directly in hardware TLB without prior fetch) proves that hardware TLB
     * NR=1 denies loads with EXC_PNR and permits execution on the same warm
     * TLB entry, while neg7-tlb also verifies GUEST_CODE_VA holds NR=1, NX=0.
     */
    {
        uint64_t xo_vppn = GUEST_XO_VA & UINT64_C(0x0000ffffffffe000);
        LapgTranslation tr_xo;

        csrwr(0x11, GUEST_CODE_VA & UINT64_C(0x0000ffffffffe000));
        __asm__ volatile("tlbsrch\n tlbrd" ::: "memory");
        check((csrrd(0x10) & (UINT64_C(1) << 31)) == 0 &&
              (csrrd(0x12) & (LAPG_PTE_V | LAPG_PTE_D | LAPG_PTE_PLV_MASK |
                              LAPG_PTE_NR | LAPG_PTE_NX)) ==
              (LAPG_PTE_V | LAPG_PTE_PLV3 | LAPG_PTE_NR),
              "neg7-tlb: warm GUEST_CODE_VA TLB entry has V=1 PLV3 NR=1 NX=0");

        check(lapg_walk(&g_memory, g_plv3_root, GUEST_XO_VA,
                        LAPG_PERM_EXEC | LAPG_PERM_USER, &tr_xo) == LAPG_OK,
              "neg7-prep: walk g_plv3_root for GUEST_XO_VA Execute-Only");
        __asm__ volatile("invtlb 0x5, %0, %1" :: "r"(UINT64_C(1)), "r"(xo_vppn) : "memory");
        csrwr(0x10, (uint64_t)LAPG_PAGE_BITS << 24);
        csrwr(0x11, xo_vppn);
        csrwr(0x12, tr_xo.pte & ~LAPG_PTE_G);
        csrwr(0x13, 0ULL);
        __asm__ volatile("tlbfill\n tlbsrch\n tlbrd" ::: "memory");
        check((csrrd(0x10) & (UINT64_C(1) << 31)) == 0 &&
              (csrrd(0x12) & (LAPG_PTE_V | LAPG_PTE_D | LAPG_PTE_PLV_MASK |
                              LAPG_PTE_NR | LAPG_PTE_NX)) ==
              (LAPG_PTE_V | LAPG_PTE_PLV3 | LAPG_PTE_NR),
              "neg7-xo-tlb: warm GUEST_XO_VA TLB entry has NR=1 NX=0");

        g_probe_exit = (ProbeExitInfo){0};
        probe_run_plv3(GUEST_CODE_VA + off_load, GUEST_XO_VA, &g_vcpu_env);
        check(g_probe_exit.reason == PROBE_EXIT_FAULT &&
              g_probe_exit.is_refill == 0 &&
              g_probe_exit.ecode == EXC_PNR &&
              g_probe_exit.badv == GUEST_XO_VA,
              "neg7a: warm Execute-Only page denies PLV3 load with EXC_PNR");

        g_probe_exit = (ProbeExitInfo){0};
        probe_run_plv3(GUEST_CODE_VA + off_exec, GUEST_XO_VA, &g_vcpu_env);
        check(g_probe_exit.reason == PROBE_EXIT_SYSCALL &&
              g_probe_exit.era == GUEST_XO_VA,
              "neg7b: same warm Execute-Only TLB entry executes at PLV3");
        verify_host_canaries();
    }

    /* 7. Negative test 8: Warm RO guest page denies PLV3 write with EXC_PME (0x4). */
    {
        g_probe_exit = (ProbeExitInfo){0};
        probe_run_plv3(GUEST_CODE_VA + off_load, GUEST_RO_VA, &g_vcpu_env);
        check(g_probe_exit.reason == PROBE_EXIT_SYSCALL &&
              g_vcpu_env.gpr[0] == RO_MAGIC,
              "neg8-prep: PLV3 load warms RO guest page");

        g_probe_exit = (ProbeExitInfo){0};
        probe_run_plv3(GUEST_CODE_VA + off_store, GUEST_RO_VA, &g_vcpu_env);
        check(g_probe_exit.reason == PROBE_EXIT_FAULT &&
              g_probe_exit.is_refill == 0 &&
              g_probe_exit.ecode == EXC_PME &&
              g_probe_exit.badv == GUEST_RO_VA &&
              g_guest_ro_page[0] == RO_MAGIC,
              "neg8: warm RO guest page denies PLV3 store with EXC_PME");
    }

    /* 8. Negative test 9: Warm NX guest data page denies exec (EXC_PNX) + priv CSR denies (EXC_IPE). */
    {
        g_probe_exit = (ProbeExitInfo){0};
        probe_run_plv3(GUEST_CODE_VA + off_load, GUEST_DATA_VA, &g_vcpu_env);
        check(g_probe_exit.reason == PROBE_EXIT_SYSCALL,
              "neg9a-prep: PLV3 load warms NX guest data page");

        g_probe_exit = (ProbeExitInfo){0};
        probe_run_plv3(GUEST_CODE_VA + off_exec, GUEST_DATA_VA + 8u, &g_vcpu_env);
        check(g_probe_exit.reason == PROBE_EXIT_FAULT &&
              g_probe_exit.is_refill == 0 &&
              g_probe_exit.ecode == EXC_PNX &&
              g_probe_exit.badv == GUEST_DATA_VA + 8u,
              "neg9a: warm NX guest data page denies PLV3 fetch with EXC_PNX");

        g_probe_exit = (ProbeExitInfo){0};
        probe_run_plv3(GUEST_CODE_VA + off_priv, 0, &g_vcpu_env);
        check(g_probe_exit.reason == PROBE_EXIT_FAULT &&
              g_probe_exit.is_refill == 0 &&
              (g_probe_exit.ecode == EXC_IPE || g_probe_exit.ecode == EXC_GSPR),
              "neg9b: PLV3 privileged CSR read traps with EXC_IPE/GSPR");
    }

    uart_puts("plv3-iso: checks=");
    puthex((uint64_t)g_checks);
    uart_puts(" fails=");
    puthex((uint64_t)g_fails);
    uart_puts(g_fails ? " BAD\n" : " ok\n");
    uart_puts("plv3-iso: done\n");
    for (;;) {
    }
}
