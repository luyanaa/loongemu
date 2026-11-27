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
#include "loongemu-perf.h"

/* Supplied by latr_linux_blob.S / latr_initrd_blob.S. */
extern const uint8_t latr_linux_image[];
extern const uint8_t latr_linux_image_end[];
extern const uint8_t latr_initrd_image[];
extern const uint8_t latr_initrd_image_end[];

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
#ifndef GUEST_RAM_MIB
#define GUEST_RAM_MIB 768
#endif
#define GUEST_RAM_SIZE ((uint32_t)GUEST_RAM_MIB << 20)
/* Overridable so a smoke run can reach the harness exit path (and therefore the
 * milestone report) without booting the kernel for an hour. */
#ifndef STEP_BUDGET
#define STEP_BUDGET 20000u
#endif
/* How many translated blocks between progress lines: enough to be cheap, small
 * enough that a stall is reported at the PC where it happened. */
#define PROGRESS_EVERY 20000u

static uint8_t ram[GUEST_RAM_SIZE] __attribute__((aligned(4096),
                                                 section(".guest_ram")));
static uint8_t machine_work[8u << 20] __attribute__((aligned(4096)));
static LatrMachine machine;
static uint64_t last_reported;
static uint32_t console_mirrored;
static uint32_t console_last_len;
static uint32_t console_stall_reports;
#define CONSOLE_STALL_REPORTS 8u
static uint64_t last_translated;
/* Written from the enter_tb trampoline (inline asm), so it must not be static:
 * the asm names it with a relocation the linker resolves by symbol. */
uint64_t last_entered;

/* Ring of the guest PCs that were translated, so a wedge names the last few
 * blocks instead of only the one that never returned. */
#define PC_RING 48
static uint64_t pc_ring[PC_RING];
static uint32_t pc_ring_at;

/* Host stable counter. Only used for *ratios* here: what matters is whether a
 * phase runs at the same blocks-per-tick as the one before it, which is
 * frequency-free and comparable across runs. */
static uint64_t now_ticks(void)
{
    uint64_t v;

    __asm__ volatile("rdtime.d %0, $zero" : "=r"(v));
    return v;
}

static uint64_t last_ticks;
static uint64_t last_tbs;
static uint64_t last_traps;

static void dump_opcode_cost(void);
static void dump_ratio_hist(void);
static void dump_pc_hash(void);
static void dump_probe(void);
/*
 * Where the host time goes, when built with PERF=1.
 *
 * TB_COMPILE_TICKS is translate+assemble, TB_EXEC_TICKS is time spent inside
 * generated code, and the memory counters say how often a guest load/store
 * needed the software TLB, a walk, or an MMIO dispatch. Without this split,
 * "three orders of magnitude slower than qemu-system-riscv64" has no owner:
 * a slow translator and slow generated code need opposite fixes.
 */
/*
 * Boot milestones, recognised in the kernel's own console output.
 *
 * The alternative -- reading the raw dump and deciding by eye how far it got --
 * is what made "did it reach SLUB or not?" a matter of opinion across sessions,
 * and it cannot be asserted on. Each entry is a literal the kernel prints, in
 * boot order; the harness reports the highest one reached, once, as
 * `latr-linux: milestone <name>`, plus the full list at exit.
 */
static const struct {
    const char *marker;
    const char *name;
} milestones[] = {
    { "Booting Linux",                "start_kernel" },
    { "SBI specification",            "sbi" },
    { "earlycon: sbi0",               "earlycon" },
    { "Initmem setup node",           "initmem" },
    { "mem auto-init",                "mem_auto_init" },
    { "SLUB:",                        "slub" },
    { "ftrace:",                      "ftrace" },
    { "rcu: Preemptible",             "rcu" },
    { "Memory:",                      "mem_print_info" },
    /* Inserted after mem_print_info so the levels already used by the gate and
     * quoted in MACHINE.md (1-9) do not move. */
    { "Trying to unpack rootfs image as initramfs", "initramfs_unpack" },
    { "Freeing unused kernel memory", "free_initmem" },
    { "Run /init as init process",    "run_init" },
    { "Kernel panic",                 "panic" },
};
#define NMILESTONES ((int)(sizeof(milestones) / sizeof(milestones[0])))

static bool milestone_hit[NMILESTONES];
static int milestone_highest = -1;

static bool log_contains(const char *log, uint32_t len, const char *needle)
{
    uint32_t n = 0;

    while (needle[n] != '\0') {
        n++;
    }
    if (n == 0 || len < n) {
        return false;
    }
    for (uint32_t i = 0; i + n <= len; i++) {
        uint32_t j = 0;

        while (j < n && log[i + j] == needle[j]) {
            j++;
        }
        if (j == n) {
            return true;
        }
    }
    return false;
}

/* A marker never straddles more than a few dozen bytes, and the log is small,
 * so rescanning the whole buffer is cheaper than tracking a partial match. */
static void milestone_scan(void)
{
    const char *log = latr_system_uart_log(&machine.sys);
    uint32_t len = machine.sys.uart_log_len;

    for (int i = 0; i < NMILESTONES; i++) {
        if (milestone_hit[i]) {
            continue;
        }
        if (log_contains(log, len, milestones[i].marker)) {
            milestone_hit[i] = true;
            /* A panic is an event, not progress: a kernel that panicked during
             * early init reached level 1, not level 12. It is reported by name
             * and left out of the ordering. */
            if (i != NMILESTONES - 1 && i > milestone_highest) {
                milestone_highest = i;
            }
            latx_bare_uart_puts("latr-linux: milestone ");
            latx_bare_uart_puts(milestones[i].name);
            /* The level is repeated here, not only at exit, because a boot this
             * slow is normally killed by the runner's watchdog: a gate that
             * reads a value printed only on the exit path fails every long run
             * for the wrong reason. */
            latx_bare_uart_puts("\nlatr-linux: milestone-level=");
            put_dec(i + 1);
            latx_bare_uart_puts("\n");
        }
    }
}

static void milestone_report(void)
{
    latx_bare_uart_puts("latr-linux: milestones reached:");
    for (int i = 0; i < NMILESTONES; i++) {
        if (milestone_hit[i]) {
            latx_bare_uart_puts(" ");
            latx_bare_uart_puts(milestones[i].name);
        }
    }
    latx_bare_uart_puts("\nlatr-linux: milestone-highest=");
    if (milestone_highest < 0) {
        latx_bare_uart_puts("none");
    } else {
        latx_bare_uart_puts(milestones[milestone_highest].name);
    }
    /* 1-based so 0 unambiguously means "nothing reached"; the Makefile compares
     * this against LATR_LINUX_MIN_LEVEL instead of parsing names. */
    latx_bare_uart_puts("\nlatr-linux: milestone-level=");
    put_dec(milestone_highest + 1);
    latx_bare_uart_puts("\n");
}

static void dump_perf(void)
{
#ifdef LOONGEMU_PERF
    latx_bare_uart_puts("latr-linux: perf tb=");
    put_dec(loongemu_perf_tb_count());
    latx_bare_uart_puts(" host-insns=");
    put_dec(loongemu_perf_host_insns());
    latx_bare_uart_puts(" guest-insns=");
    put_dec(loongemu_perf_guest_insns());
    latx_bare_uart_puts(" compile/exec=");
    put_dec(loongemu_perf_read(LOONGEMU_PERF_TB_COMPILE_TICKS));
    latx_bare_uart_puts("/");
    put_dec(loongemu_perf_read(LOONGEMU_PERF_TB_EXEC_TICKS));
    latx_bare_uart_puts(" helper=");
    put_dec(loongemu_perf_read(LOONGEMU_PERF_HELPER_CALLS));
    latx_bare_uart_puts(" tlb-hit/miss=");
    put_dec(loongemu_perf_read(LOONGEMU_PERF_TLB_HIT));
    latx_bare_uart_puts("/");
    put_dec(loongemu_perf_read(LOONGEMU_PERF_TLB_MISS));
    latx_bare_uart_puts(" fast/plan=");
    put_dec(loongemu_perf_read(LOONGEMU_PERF_FAST_HIT));
    latx_bare_uart_puts("/");
    put_dec(loongemu_perf_read(LOONGEMU_PERF_PLAN_HIT));
    latx_bare_uart_puts(" mmio=");
    put_dec(loongemu_perf_read(LOONGEMU_PERF_MMIO_ACCESS));
    latx_bare_uart_puts("\n");
#else
    /* Everything reads zero without PERF=1, and a line of zeros in every log
     * reads as a measurement instead of as an absent one. */
#endif
}

static void dump_pc_ring(void);


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
    dump_pc_ring();
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
static void dump_pc_ring(void)
{
    latx_bare_uart_puts("latr-linux: last pcs (oldest first):");
    for (uint32_t i = 0; i < PC_RING; i++) {
        latx_bare_uart_puts(" ");
        puthex64(pc_ring[(pc_ring_at + i) % PC_RING]);
    }
    latx_bare_uart_puts("\n");
}

/*
 * Guest instructions per translated block, logarithmic buckets:
 * 1, 2, 3-4, 5-8, 9-16, 17-32, 33-64, 65+.
 *
 * This -- not the host/guest ratio -- is what decides throughput here: every
 * block costs one dispatch (cache lookup, key compare, chain handoff), so a
 * kernel that ends a block every one or two instructions pays that cost per
 * instruction. The host/guest ratio was measured first and is useless as an
 * alarm: one guest instruction legitimately expands to 2-6 host instructions,
 * so 87606 of 87606 blocks landed in the top bucket.
 */
/*
 * Zero means "arm automatically on the most frequent block of the first silent
 * window". A compile-time `PROBE_PC=n` still pins a specific block.
 */
#ifndef PROBE_PC
#define PROBE_PC 0
#endif
static uint64_t probe_pc = PROBE_PC;
static uint64_t probe_gpr[2][32];
static uint64_t probe_insns[2];
static uint32_t probe_hits;

static uint32_t icount_hist[8];
static uint32_t icount_worst;
static uint64_t icount_worst_pc;

/*
 * Entry-frequency sketch. A wedged kernel re-enters the same handful of blocks
 * forever, and that is visible without any symbol table: the top buckets are
 * the loop body. Index by the low bits of the guest PC, count entries, and
 * print the leaders so the loop can be disassembled from the image directly.
 */
#define PC_HASH 256
static uint32_t pc_hash_count[PC_HASH];
static uint64_t pc_hash_pc[PC_HASH];

static void pc_hash_record(uint64_t pc)
{
    uint32_t h = (uint32_t)((pc >> 2) & (PC_HASH - 1));

    if (pc_hash_count[h] == 0) {
        pc_hash_pc[h] = pc;
    }
    pc_hash_count[h]++;
}

static void dump_pc_hash(void)
{
    uint32_t top = 0;

    for (uint32_t i = 1; i < PC_HASH; i++) {
        if (pc_hash_count[i] > pc_hash_count[top]) {
            top = i;
        }
    }
    /*
     * Arm the register probe on whatever block is actually repeating, instead
     * of on a hand-picked PC. Picking one by hand burned several long runs on
     * blocks that are entered once (a function's prologue) rather than on the
     * loop head that comes back every iteration. Two gates matter: before the
     * console has printed anything every window looks silent, and arming there
     * pins whatever early-boot helper is hot (vsnprintf, at 1704 entries) rather
     * than the loop we are chasing, so arming waits for uart_log_len > 0; and
     * the count threshold keeps it off the kernel's entry block, which is
     * entered exactly once. The next silent window prints how the chosen
     * block's registers moved.
     */
    if (pc_hash_count[top] >= 200 && machine.sys.uart_log_len > 0) {
        probe_pc = pc_hash_pc[top];
        probe_hits = 0;
        latx_bare_uart_puts("latr-linux: probe armed at ");
        puthex64(probe_pc);
        latx_bare_uart_puts("\n");
    }
    latx_bare_uart_puts("latr-linux: top block entries:");
    for (uint32_t n = 0; n < 8; n++) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < PC_HASH; i++) {
            if (pc_hash_count[i] > pc_hash_count[best]) {
                best = i;
            }
        }
        if (pc_hash_count[best] == 0) {
            break;
        }
        latx_bare_uart_puts(" ");
        puthex64(pc_hash_pc[best]);
        latx_bare_uart_puts("x");
        put_dec(pc_hash_count[best]);
        pc_hash_count[best] = 0;
    }
    for (uint32_t i = 0; i < PC_HASH; i++) {
        pc_hash_count[i] = 0;
    }
    latx_bare_uart_puts("\n");
}

/*
 * Which guest opcodes emit the most host instructions. This is the measurement
 * that "84 host instructions per guest instruction" needs in order to be
 * actionable: without it every candidate (CSR masking, recovery bindings,
 * memory access) has a plausible story.
 */
static void dump_opcode_cost(void)
{
    /*
     * Non-destructive: an earlier version cleared each printed entry, so the
     * second dump of a long boot showed nothing and it was impossible to tell a
     * zeroed counter from a counter that never accumulated.
     */
    uint32_t used[10];
    uint32_t used_n = 0;
    uint64_t total_ir2 = 0;
    uint64_t total_insns = 0;

    for (uint32_t op = 1; op < (uint32_t)LATR_RV_OPCODE_COUNT; op++) {
        total_ir2 += latr_opcode_ir2[op];
        total_insns += latr_opcode_insns[op];
    }
    latx_bare_uart_puts("latr-linux: blocks=");
    put_dec(latr_block_count);
    latx_bare_uart_puts(" ir2=");
    put_dec(latr_block_ir2_total);
    latx_bare_uart_puts(" words=");
    put_dec(latr_block_words_total);
    latx_bare_uart_puts(" flush-ir2=");
    put_dec(latr_flush_ir2);
    latx_bare_uart_puts(" flush-calls=");
    put_dec(latr_flush_emit_calls);
    latx_bare_uart_puts(" entry=");
    put_dec(latr_block_entry_ir2);
    latx_bare_uart_puts(" exit=");
    put_dec(latr_block_exit_ir2);
    latx_bare_uart_puts(" ir2/word=");
    put_dec(latr_block_words_total != 0
                ? latr_block_ir2_total / latr_block_words_total : 0);
    latx_bare_uart_puts("\nlatr-linux: worst insn pc=");
    puthex64(latr_insn_worst_pc);
    latx_bare_uart_puts(" op=");
    latx_bare_uart_puts(latr_opcode_name(latr_insn_worst_opcode));
    latx_bare_uart_puts(" ir2=");
    put_dec(latr_insn_worst_ir2);
    latx_bare_uart_puts("\nlatr-linux: worst block pc=");
    puthex64(latr_block_worst_pc);
    latx_bare_uart_puts(" insns=");
    put_dec(latr_block_worst_icount);
    latx_bare_uart_puts(" ir2=");
    put_dec(latr_block_worst_ir2);
    latx_bare_uart_puts(" words=");
    put_dec(latr_block_worst_words);
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("latr-linux: opcode cost total ir2=");
    put_dec(total_ir2);
    latx_bare_uart_puts(" insns=");
    put_dec(total_insns);
    latx_bare_uart_puts("\n");

    for (uint32_t n = 0; n < 10 && used_n < 10; n++) {
        uint32_t best = 0;
        uint64_t best_cost = 0;

        for (uint32_t op = 1; op < (uint32_t)LATR_RV_OPCODE_COUNT; op++) {
            bool seen = false;

            for (uint32_t u = 0; u < used_n; u++) {
                if (used[u] == op) {
                    seen = true;
                }
            }
            if (seen || latr_opcode_insns[op] == 0) {
                continue;
            }
            if (latr_opcode_ir2[op] > best_cost) {
                best_cost = latr_opcode_ir2[op];
                best = op;
            }
        }
        if (best == 0) {
            break;
        }
        used[used_n++] = best;
        latx_bare_uart_puts("  ");
        latx_bare_uart_puts(latr_opcode_name(best));
        latx_bare_uart_puts(" insns=");
        put_dec(latr_opcode_insns[best]);
        latx_bare_uart_puts(" ir2=");
        put_dec(latr_opcode_ir2[best]);
        latx_bare_uart_puts(" per-insn=");
        put_dec(latr_opcode_ir2[best] / latr_opcode_insns[best]);
        latx_bare_uart_puts("\n");
    }
}

static void dump_ratio_hist(void)
{
    latx_bare_uart_puts("latr-linux: guest-insns/block for 1,2,3-4,5-8,9-16,"
                        "17-32,33-64,65+:");
    for (uint32_t i = 0; i < 8; i++) {
        latx_bare_uart_puts(" ");
        put_dec(icount_hist[i]);
    }
    latx_bare_uart_puts(" worst=");
    put_dec(icount_worst);
    latx_bare_uart_puts(" at ");
    puthex64(icount_worst_pc);
    latx_bare_uart_puts("\n");
}

static void ratio_record(uint64_t pc, uint32_t words, uint32_t guest_insns)
{
    int b;

    (void)words;
    if (guest_insns == 0) {
        return;
    }
    b = guest_insns <= 1   ? 0
      : guest_insns == 2   ? 1
      : guest_insns <= 4   ? 2
      : guest_insns <= 8   ? 3
      : guest_insns <= 16  ? 4
      : guest_insns <= 32  ? 5
      : guest_insns <= 64  ? 6
                           : 7;
    icount_hist[b]++;
    if (guest_insns > icount_worst) {
        icount_worst = guest_insns;
        icount_worst_pc = pc;
    }
}

/*
 * One-shot register snapshot at a chosen block, 1000 entries apart.
 *
 * A loop that never terminates and a loop that is merely long look identical in
 * PC samples. The loop *counter* tells them apart: if it keeps growing past the
 * bound the kernel compares it against, the branch is mis-translated; if it
 * stays inside a small window, the loop is progressing and the cost is real.
 */

static void probe_maybe(void *env, uint64_t pc, uint64_t insns)
{
    CPUArchState *e = (CPUArchState *)env;

    if (probe_pc == 0 || pc != probe_pc) {
        return;
    }
    probe_hits++;
    if (probe_hits == 1) {
        for (int i = 0; i < 32; i++) {
            probe_gpr[0][i] = e->gpr[i];
        }
        probe_insns[0] = insns;
    } else if (probe_hits == 1001) {
        for (int i = 0; i < 32; i++) {
            probe_gpr[1][i] = e->gpr[i];
        }
        probe_insns[1] = insns;
    }
}

static void dump_probe(void)
{
    if (probe_hits < 1001) {
        return;
    }
    latx_bare_uart_puts("latr-linux: probe at ");
    puthex64(probe_pc);
    latx_bare_uart_puts(" hits=");
    put_dec(probe_hits);
    latx_bare_uart_puts("\n");
    for (int i = 0; i < 32; i++) {
        if (probe_gpr[0][i] == probe_gpr[1][i]) {
            continue;
        }
        latx_bare_uart_puts("  x");
        put_dec((uint32_t)i);
        latx_bare_uart_puts(" ");
        puthex64(probe_gpr[0][i]);
        latx_bare_uart_puts(" -> ");
        puthex64(probe_gpr[1][i]);
        latx_bare_uart_puts("\n");
    }
}

static void hook_translated(void *env, uint64_t pc, uint32_t host_words,
                            uint32_t guest_insns)
{
    (void)env;
    ratio_record(pc, host_words, guest_insns);
    pc_hash_record(pc);
    probe_maybe(env, pc, machine.harts[0].tbs);
    last_translated = pc;
    /* Record *entries*, not only translations. Once a block is cached it is
     * never translated again, so a translation-only ring is frozen and says
     * nothing about where a wedged kernel is spinning. */
    pc_ring[pc_ring_at++ % PC_RING] = pc;
}

static void hook_translate(void *env, uint64_t pc)
{
    (void)env;
    /* No ring entry here: a translation always ends in an entry, so recording
     * both made every block appear twice and halved the ring's useful window. */
    last_translated = pc;
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
    latx_bare_uart_puts(" traps=");
    put_dec(machine.trap_count);
    /*
     * fence.i / sfence.vma sequence number. A guest that patches its own text
     * issues fence.i per patch site, and LATR treats each one as a whole-cache
     * invalidation, so this number climbing in step with tbs is the signature of
     * a boot whose time goes into re-translating code that was just thrown away.
     */
    latx_bare_uart_puts(" fences=");
    put_dec(latr_tb_fence_seq());
    latx_bare_uart_puts(" vfy=");
    put_dec(machine.verify_checks);
    latx_bare_uart_puts("/");
    put_dec(machine.verify_drops);
    {
        /*
         * Ticks per 1000 blocks and traps per 1000 blocks for the window just
         * finished. A phase that suddenly costs 20x more ticks per block, or
         * that starts trapping thousands of times per window, is the difference
         * between "a slow kernel loop" and "the translator took a slow path".
         */
        uint64_t t = now_ticks();
        uint64_t dt = t - last_ticks;
        uint64_t db = h->tbs - last_tbs;
        uint64_t dr = machine.trap_count - last_traps;

        latx_bare_uart_puts(" ktick/kblk=");
        put_dec(db != 0 ? dt * 1000u / db : 0);
        latx_bare_uart_puts(" trap/kblk=");
        put_dec(db != 0 ? dr * 1000u / db : 0);
        last_ticks = t;
        last_tbs = h->tbs;
        last_traps = machine.trap_count;
    }
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
    /*
     * Every report, not only the silent ones: a console that is producing
     * output is exactly when a new milestone appears, and putting this inside
     * the silent-window branch meant no milestone was ever recorded on a boot
     * that was printing.
     */
    milestone_scan();
    /*
     * A console that has emitted nothing for CONSOLE_STALL_REPORTS consecutive
     * reports is either inside a long kernel loop or wedged, and the PC alone
     * cannot tell them apart. Dump the entry ring once per silent window so the
     * log shows the spin set (a handful of addresses) or a walk (a moving
     * window) without waiting for the runner's watchdog.
     */
    if (machine.sys.uart_log_len == console_last_len) {
        console_stall_reports++;
        if (console_stall_reports == CONSOLE_STALL_REPORTS) {
            dump_pc_ring();
            dump_opcode_cost();
            dump_probe();
            dump_perf();
            dump_pc_hash();
            dump_ratio_hist();
            console_stall_reports = 0;
        }
    } else {
        console_last_len = machine.sys.uart_log_len;
        console_stall_reports = 0;
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
    /*
     * Load the initramfs before the DTB is built: the chosen node carries
     * linux,initrd-start/end, so the properties have to exist when the tree is
     * written. Without an initrd the kernel has no root= to mount and panics in
     * mount_root_generic, which is exactly where the reference boot stops too.
     */
    {
        uint64_t initrd_len =
            (uint64_t)(uintptr_t)(latr_initrd_image_end - latr_initrd_image);

        if (!latr_machine_load_initrd(&machine, latr_initrd_image, initrd_len)) {
            latx_bare_uart_puts("latr-linux: FAIL initrd\n");
            return;
        }
        latx_bare_uart_puts("latr-linux: initrd=");
        puthex64(machine.initrd_start);
        latx_bare_uart_puts("-");
        puthex64(machine.initrd_end);
        latx_bare_uart_puts("\n");
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
            dump_pc_ring();
            break;
        }
    }
    progress();
    dump_ratio_hist();
    milestone_report();
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
