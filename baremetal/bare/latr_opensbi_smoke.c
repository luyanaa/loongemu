/* Optional real-OpenSBI smoke: embeds a prebuilt RISC-V fw_dynamic image and
 * runs it through the LATR virt machine. Kept separate from latr-boot so the
 * default test set has no external firmware dependency. */
#include <string.h>
#include "latr.h"
#include "latr-system.h"
#include "latr-machine.h"
#include "translate.h"

extern void latr_enter_tb(void *env, void *code);
extern const unsigned char latr_opensbi_image[];
extern const unsigned char latr_opensbi_end[];

#define UART0_THR ((volatile uint8_t *)0x1fe001e0)
#define UART0_LSR ((volatile uint8_t *)0x1fe001e5)

static uint32_t uart_stall_spins;

static void putc_raw(int c)
{
    /* Wait for THR empty. Without this the 16550 model drops bytes whenever a
     * diagnostic burst (a 200-word code dump, a PC trace) outruns the chardev,
     * which silently truncates exactly the output being debugged.
     *
     * The wait has to be bounded, though. A diagnostic harness that can deadlock
     * on its own trace is worse than one that prints garbage: the trace stops
     * mid-sentence and the run looks like a wedged guest, which is precisely the
     * thing being investigated. On timeout the byte is written anyway and the
     * stall is counted, so a truncated line still leaves evidence behind. */
    uint32_t spins = 0;

    while ((*UART0_LSR & 0x20) == 0) {
        if (++spins > 4000000u) {
            uart_stall_spins++;
            break;
        }
    }
    *UART0_THR = (uint8_t)c;
}

static void puts_raw(const char *s)
{
    while (*s != '\0') {
        putc_raw(*s++);
    }
}

static void put_raw_crlf(void)
{
    putc_raw('\n');
}

static void puthex8(uint32_t v)
{
    static const char digits[] = "0123456789abcdef";

    for (int i = 28; i >= 0; i -= 4) {
        putc_raw(digits[(v >> i) & 15]);
    }
}

static void puthex(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";

    for (int i = 60; i >= 0; i -= 4) {
        putc_raw(digits[(v >> i) & 15]);
    }
}

/* Counts how often generated code handed control back to the trampoline. A
 * block that runs off into the weeds shows up here as a count that stops
 * advancing while the dispatch entry count keeps climbing. */
volatile uint64_t latr_ret_count;
__asm__(
".text\n"
".globl latr_enter_tb\n"
"latr_enter_tb:\n"
"  addi.d $sp, $sp, -16\n"
"  st.d   $ra, $sp, 0\n"
"  st.d   $s8, $sp, 8\n"
"  move   $s8, $a0\n"
"  jirl   $ra, $a1, 0\n"
"  la.local $t6, latr_ret_count\n"
"  ld.d   $t7, $t6, 0\n"
"  addi.d $t7, $t7, 1\n"
"  st.d   $t7, $t6, 0\n"
"  ld.d   $ra, $sp, 0\n"
"  ld.d   $s8, $sp, 8\n"
"  addi.d $sp, $sp, 16\n"
"  jirl   $zero, $ra, 0\n");

void kmain(void);
__asm__(".text\n.globl _start\n_start:\n  pcalau12i $sp, %pc_hi20(opensbi_stack)\n  addi.d $sp, $sp, %pc_lo12(opensbi_stack)\n"
        "  lu12i.w $t0, 256\n  add.d $sp, $sp, $t0\n  bl kmain\n1: b 1b\n");
static uint8_t opensbi_stack[1u << 20] __attribute__((aligned(16), used));

static uint8_t ram[16u << 20] __attribute__((aligned(4096)));
static uint8_t work[16u << 20] __attribute__((aligned(4096)));
static LatrMachine machine;

/*
 * Host-CPU exception handler.
 *
 * The harness emulates a RISC-V guest with machine code it generates itself, but
 * it is also an ordinary LoongArch program whose *own* faults used to go
 * nowhere: EENTRY was left at 0, so an illegal instruction inside generated
 * code trapped to address 0, the "handler" there was more garbage, and the
 * machine settled into an eternal exception loop that produced no output and no
 * diagnostic. From the outside that is indistinguishable from a wedged guest -
 * the log simply stops.
 *
 * Report ERA/ESTAT/BADV plus the offending instruction word and stop.
 * ESTAT.Ecode 13 is INE (instruction non-existent), which for this harness means
 * the hart executed words that are not valid LoongArch: either the arena copy of
 * a block is stale/truncated, or control left the block.
 */
volatile uint64_t host_fault_era;
volatile uint64_t host_fault_estat;
volatile uint64_t host_fault_badv;

/* Tentative definition; the dispatcher below increments the same object. */
static uint64_t tb_total;

void latr_host_fault_report(void)
{
    uint64_t era = host_fault_era;
    uint64_t estat = host_fault_estat;
    const uint32_t *code = NULL;

    if (era >= (uint64_t)(uintptr_t)work &&
        era + 32 <= (uint64_t)(uintptr_t)work + sizeof(work)) {
        code = (const uint32_t *)(uintptr_t)era;
    } else if (era >= (uint64_t)(uintptr_t)ram &&
               era + 32 <= (uint64_t)(uintptr_t)ram + sizeof(ram)) {
        code = (const uint32_t *)(uintptr_t)era;
    }

    puts_raw("\nlatr-opensbi: HOSTFAULT ecode=");
    puthex((estat >> 16) & 0x3fu);
    puts_raw(" era=");
    puthex(era);
    puts_raw(" badv=");
    puthex(host_fault_badv);
    puts_raw(" tb=");
    puthex(tb_total);
    puts_raw(" guestpc=");
    puthex(machine.harts[0].env.pc);
    puts_raw(" in=");
    if (code != NULL) {
        puts_raw("work");
    } else {
        puts_raw("other");
    }
    if (code != NULL) {
        for (int i = -2; i < 6; i++) {
            putc_raw(' ');
            puthex8(code[i]);
        }
    }
    puts_raw("\nlatr-opensbi: FAIL host-fault\n");
    puts_raw("latr-opensbi: done\n");
    for (;;) {
    }
}

__asm__(
".text\n"
".globl latr_host_eentry\n"
"latr_host_eentry:\n"
/*
 * No SAVE registers here on purpose. SAVE0..N live at CSR 0x30+N, not at
 * 0x1a..0x1d - in that numbering 0x1a/0x1b are PGDH/PGD, so "saving" a
 * register there rewrites the page-table base and the handler raises a second
 * exception before it can report the first one. Nothing has to be preserved
 * anyway: this handler reports and halts.
 */
"  csrrd  $t0, 0x6\n"         /* ERA: the faulting instruction */
"  la.local $t2, host_fault_era\n"
"  st.d   $t0, $t2, 0\n"
"  csrrd  $t0, 0x5\n"         /* ESTAT */
"  la.local $t2, host_fault_estat\n"
"  st.d   $t0, $t2, 0\n"
"  csrrd  $t0, 0x7\n"         /* BADV */
"  la.local $t2, host_fault_badv\n"
"  st.d   $t0, $t2, 0\n"
"  bl     latr_host_fault_report\n"
"1: b 1b\n");

extern void latr_host_eentry(void);

static void install_host_fault_handler(void)
{
    uint64_t eentry = (uint64_t)(uintptr_t)&latr_host_eentry;

    __asm__ volatile("csrwr %0, 0xc" : "+r"(eentry) :: "memory");
}

/*
 * Generated blocks keep the guest's floating point / vector registers in the
 * host's f0..f31 and vector registers, so the hart must have those units
 * enabled before the first block runs. On LoongArch every FP or LSX/LASX
 * instruction is gated by EUEN: with EUEN.FPE clear the hart raises FPD
 * (ESTAT.Ecode 15) instead of executing it, and the failing instruction is a
 * perfectly correct `fst.d` inside generated code.
 *
 * Leaving this out is not a performance detail - it is a silent functional
 * failure, because the raised exception lands in the VMM's own exception
 * vector rather than the guest's. That is why install_host_fault_handler() is
 * called first: with both in place an FPD shows up immediately as a host fault
 * at an arena address instead of an unattributed hang.
 */
static void enable_host_units(void)
{
    uint64_t euen, crmd;

    __asm__ volatile("csrrd %0, 0x2" : "=r"(euen));
    euen |= UINT64_C(0x1) |   /* FPE  : basic FP        */
            UINT64_C(0x2) |   /* SXE  : 128-bit vector  */
            UINT64_C(0x40) |  /* ASXE : 256-bit vector  */
            UINT64_C(0x80);   /* BTE  : binary translation ext */
    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");

    /* Keep host interrupts off: the CLINT model drives guest timers by polling,
     * not by delivering host interrupts. */
    __asm__ volatile("csrrd %0, 0x0" : "=r"(crmd));
    crmd &= ~UINT64_C(0x4);
    __asm__ volatile("csrwr %0, 0x0" :: "r"(crmd) : "memory");
}


/* S-mode next stage. This used to be a hand-written seven-instruction stub that
 * poked the 16550 and the exit device directly: enough to prove that *some* code
 * ran in S-mode, but it never touched OpenSBI again. The real payload below
 * comes from baremetal/bare/latr_smode_payload.S; it prints through the SBI
 * legacy console (ecall from S), so a byte in the log proves the full round
 * trip S-mode -> M-mode trap -> OpenSBI ecall handler -> UART -> M-mode return
 * -> S-mode, which is the mechanism the whole translator has to get right. */
extern const uint8_t latr_smode_payload[];
extern const uint8_t latr_smode_payload_end[];

/* LATR_OPEN_SBI_TRACE turns the guest-PC ring into a full, unfiltered trace of
 * every dispatched TB. That makes the record directly comparable, one entry per
 * TB, with `qemu-system-riscv64 -d exec` on the same firmware + DTB: the first
 * misaligned entry is the first guest instruction whose translation is wrong.
 * Keep it off by default, the dump is ~17 bytes per TB over the UART. */
#ifndef LATR_OPEN_SBI_TRACE
#define LATR_OPEN_SBI_TRACE 0
#endif
#ifndef LATR_OPEN_SBI_TBS
#define LATR_OPEN_SBI_TBS 4000
#endif
/* Consecutive rounds (1000 TBs each) without a console byte before the run * gives up. 0 disables the guard, which is what a long investigation wants:
 * OpenSBI is silent for tens of thousands of TBs while it walks the hardware
 * description, so a short guard reports a livelock that is really just boot. */
/* When non-zero, every dispatch past this TB number prints its guest PC before
 * entering the block. A wedge *inside* one translated block never returns to
 * the dispatch loop, so the last line printed is the culprit block -- the only
 * way to see it from the guest side. */
#ifndef LATR_OPEN_SBI_PCLOG_FROM
#define LATR_OPEN_SBI_PCLOG_FROM 0
#endif

#ifndef LATR_OPEN_SBI_CODEWORDS
#define LATR_OPEN_SBI_CODEWORDS 200
#endif

#ifndef LATR_OPEN_SBI_STALL_ROUNDS
#define LATR_OPEN_SBI_STALL_ROUNDS 4
#endif

/* Non-zero stops the run once the S-mode payload has reported completion. The
 * payload normally terminates the machine itself through SBI SRST (whose reset
 * lands on the exit device), so this is only a backstop for a platform where
 * that path is unimplemented -- and it keeps a passing run from burning its
 * whole TB budget spinning in the payload's final loop.
 *
 * Note what this must NOT key on: stopping at "OpenSBI" (the banner) proves
 * only that firmware decoded and printed, not that it ever reached S-mode, and
 * an earlier version of this harness stopped there and called it a pass. */
#ifndef LATR_OPEN_SBI_STOP
#define LATR_OPEN_SBI_STOP 1
#endif

/* Per-dispatch instrumentation (two rdtime reads, a 4K-entry histogram and a
 * chain of PC comparisons). It is what makes a wedge diagnosable from inside
 * the guest, but it dominates the runtime of a healthy run: a bare-metal VMM
 * lives or dies by per-TB dispatch cost. Off by default, enable only while
 * investigating. */
#ifndef LATR_OPEN_SBI_PROBES
#define LATR_OPEN_SBI_PROBES 0
#endif

#define TRACE_CAP (LATR_OPEN_SBI_TRACE ? 32768u : 256u)

static uint64_t trace_pc[TRACE_CAP];
static uint32_t trace_pos;
static uint8_t trace_frozen;
static uint64_t path_tb[32];
static uint64_t path_arg[32];
static uint32_t path_count;
static uint64_t tb_total;
static uint64_t scan_tb, scan_s1, scan_a1, scan_a2;
static uint32_t hist[4096];
/* Wall-clock attribution: rdtime.d is the host stable counter, so the delta
 * between two dispatches is the cost of the TB that ran in between (translation
 * included, because the miss path happens inside the previous dispatch). */
static uint64_t tsum[4096];
static uint64_t tslow, tmax, tmax_pc;
static uint64_t prev_pc, prev_ts;
static uint8_t code_dumped;

static uint64_t host_cycles(void)
{
    uint64_t now;

    __asm__ volatile("rdtime.d %0, $zero" : "=r"(now) :: "memory");
    return now;
}

/* Focused probe for the OpenSBI "walks a string list forever" livelock: the
 * helper at 0x1a53c takes (list, len, pattern) and the loop head at 0x1a580 is
 * entered once per list entry. Recording the arguments plus the entry count
 * says whether the list is genuinely that long or the terminator is never hit. */
static uint64_t list_calls, list_iters;
/* Translate-probe counter: every entry means a TB-cache miss. Comparing it with
 * tb_total gives the hit rate, which is what decides whether this run is
 * "slow because the guest is busy" or "slow because nothing is ever cached". */
static uint64_t n_translate;
/* Incremented after latr_enter_tb() comes back. Together with latr_ret_count
 * this separates the three ways a run can stop:
 *   entered == tb_total     -> the block returned; the harness itself stalled
 *   entered == tb_total - 1 -> the last block never came back
 *   ret     <  entered      -> the block returned past the dispatcher's back */
static uint64_t n_entered;
static uint64_t list_a0, list_a1, list_a2, list_ra;
static uint64_t tag_ra[16];
static uint32_t tag_ra_count[16];
static uint32_t tag_ra_n;

static void machine_enter_tb(void *env, void *code)
{
    uint64_t pc = ((CPUArchState *)env)->pc;

    tb_total++;
#if LATR_OPEN_SBI_PROBES
    hist[(pc >> 8) & 4095u]++;
    {
        uint64_t now = host_cycles();

        if (prev_pc != 0) {
            uint64_t d = now - prev_ts;

            tsum[(prev_pc >> 8) & 4095u] += d;
            if (d > 1000000u) {
                tslow++;
            }
            if (d > tmax) {
                tmax = d;
                tmax_pc = prev_pc;
            }
        }
        prev_pc = pc;
        prev_ts = now;
    }
    /* Heartbeat from inside the dispatch loop: a checkpoint line is only
     * printed between 1000-TB batches, so a run that wedges inside one batch
     * (or inside one TB) is otherwise indistinguishable from a slow one. */
    if (pc == 0x8001a53cu) {
        CPUArchState *e = (CPUArchState *)env;

        list_calls++;
        list_a0 = e->gpr[10];
        list_a1 = e->gpr[11];
        list_a2 = e->gpr[12];
        list_ra = e->gpr[1];
    }
    if (pc == 0x8001a580u) {
        list_iters++;
    }
    if (pc == 0x80019560u) {
        uint64_t ra = ((CPUArchState *)env)->gpr[1];
        uint32_t i;

        for (i = 0; i < tag_ra_n; i++) {
            if (tag_ra[i] == ra) {
                tag_ra_count[i]++;
                break;
            }
        }
        if (i == tag_ra_n && tag_ra_n < 16u) {
            tag_ra[tag_ra_n] = ra;
            tag_ra_count[tag_ra_n] = 1;
            tag_ra_n++;
        }
    }
    if (pc == 0x800195d6u) {
        scan_tb = tb_total;
        scan_s1 = ((CPUArchState *)env)->gpr[9];
        scan_a1 = ((CPUArchState *)env)->gpr[11];
        scan_a2 = ((CPUArchState *)env)->gpr[12];
    }
    if (pc == 0x80019f56u && path_count < 32u) {
        path_tb[path_count] = tb_total;
        path_arg[path_count] = ((CPUArchState *)env)->gpr[10];
        path_count++;
    }

    if (!trace_frozen) {
#if LATR_OPEN_SBI_TRACE
        if (pc == 0x80005a9eu) {
            trace_frozen = 1;
        } else {
            trace_pc[trace_pos++ & (TRACE_CAP - 1u)] = pc;
        }
#else
        if (pc == 0x80005a9eu) {
            trace_frozen = 1;
        } else if (pc < 0x80019400u || pc >= 0x80019860u) {
            trace_pc[trace_pos++ & (TRACE_CAP - 1u)] = pc;
        }
#endif
    }
#endif /* LATR_OPEN_SBI_PROBES */

    /* Kept outside the probe block: this one is cheap (one AND, one compare)
     * and it is the only way to tell "still running" from "wedged" without
     * turning the expensive per-TB instrumentation back on. */
    if ((tb_total & 0xfffu) == 0u) {
        puts_raw("\nlatr-opensbi: hb=");
        puthex(tb_total);
        puts_raw(" pc=");
        puthex(pc);
        puts_raw(" miss=");
        puthex(n_translate);
        puts_raw(" ret=");
        puthex(latr_ret_count);
        puts_raw("\n");
    }
#if LATR_OPEN_SBI_PCLOG_FROM > 0
    if (tb_total >= (uint64_t)LATR_OPEN_SBI_PCLOG_FROM) {
        puthex8((uint32_t)pc);
        putc_raw('\n');
    }
#endif
#if LATR_OPEN_SBI_CODEWORDS > 0
    /* One-shot dump of the *runtime* generated code for the block that wedges;
     * the translator's own dump would have to guess the CSR state. Kept out of
     * the PCLOG block: capturing the code must not require turning on a PC
     * trace for every dispatch. */
    if (pc == 0x80002162u && !code_dumped) {
        const uint32_t *w = (const uint32_t *)code;
        const CPUArchState *e = (const CPUArchState *)env;

        code_dumped = 1;
        puts_raw("CODE ms=");
        puthex(e->csr[0x300]);
        puts_raw(" satp=");
        puthex(e->csr[0x180]);
        puts_raw(" priv=");
        puthex(e->privilege);
        puts_raw(" t0=");
        puthex(e->gpr[5]);
        puts_raw(" a0=");
        puthex(e->gpr[10]);
        puts_raw(" ff=");
        puthex(e->fetch_faulted);
        puts_raw(" ffc=");
        puthex(e->fetch_fault_cause);
        puts_raw(" ffva=");
        puthex(e->fetch_fault_va);
        puts_raw(" map=");
        puthex((uint64_t)(uintptr_t)e->mem_map);
        puts_raw(" ");
        for (int i = 0; i < LATR_OPEN_SBI_CODEWORDS; i++) {
            puthex8(w[i]);
            putc_raw(' ');
        }
        puts_raw("\n");
    }
#endif
    latr_enter_tb(env, code);
    n_entered++;
#if LATR_OPEN_SBI_PCLOG_FROM > 0
    if (tb_total >= (uint64_t)LATR_OPEN_SBI_PCLOG_FROM) {
        putc_raw('>');
        puthex8((uint32_t)((CPUArchState *)env)->pc);
        putc_raw('\n');
    }
#endif
}

/* Debug-only: translate one guest block and dump the generated LoongArch words,
 * so the host can disassemble them with llvm-mc. Used for blocks whose executed
 * code wedges (the dispatcher never regains control, so this is the only way to
 * see what was emitted). */
static uint8_t exec_buf[65536] __attribute__((aligned(64)));

static void dump_block(LatrMachine *m, uint64_t pc)
{
    static TranslationBlock dtb;
    uint8_t *buf = exec_buf;
    LatrMachineHart *h = &m->harts[0];
    uint64_t saved_pc = h->env.pc;
    int limit = 8;
    int words;

    for (size_t i = 0; i < sizeof(dtb); i++) {
        ((uint8_t *)&dtb)[i] = 0;
    }
    h->cpu.env_ptr = &h->env;
    /* OpenSBI enables FP before the register-restore table, so translate the
     * block the way it will really be translated (FS != 0); with FS=0 the
     * translator emits the illegal-instruction path instead, which says nothing
     * about the block that actually wedges. */
    h->env.csr[0x300] |= UINT64_C(1) << 13;
    h->env.pc = pc;
    dtb.pc = pc;
    dtb.tc.ptr = buf;
    dtb.cflags = 0;
    target_disasm(&dtb, &limit, &h->cpu);
    if (dtb.icount == 0) {
        puts_raw("DUMP ");
        puthex8((uint32_t)pc);
        puts_raw(" no-decode\n");
        h->env.pc = saved_pc;
        return;
    }
    puts_raw("XF ");
    puthex8((uint32_t)pc);
    puts_raw(" ic=");
    puthex8((uint32_t)dtb.icount);
    puts_raw(" ff=");
    puthex(h->env.fetch_faulted);
    puts_raw(" ffc=");
    puthex(h->env.fetch_fault_cause);
    puts_raw(" ffva=");
    puthex(h->env.fetch_fault_va);
    puts_raw(" satp=");
    puthex(h->env.csr[0x180]);
    puts_raw("\n");
    tr_init(&dtb);
    if (!tr_ir2_generate(&dtb)) {
        tr_fini();
        puts_raw("DUMP ");
        puthex8((uint32_t)pc);
        puts_raw(" illegal\n");
        h->env.pc = saved_pc;
        return;
    }
    words = tr_ir2_assemble(buf);
    tr_fini();
    puts_raw("DUMP ");
    puthex8((uint32_t)pc);
    puts_raw(" n=");
    puthex8((uint32_t)dtb.icount);
    puts_raw(" w=");
    puthex8((uint32_t)words);
    puts_raw(" ");
    for (int i = 0; i < words; i++) {
        uint32_t w;

        for (size_t b = 0; b < 4; b++) {
            ((uint8_t *)&w)[b] = buf[i * 4 + b];
        }
        puthex8(w);
        putc_raw(' ');
    }
    puts_raw("\n");
    h->env.pc = saved_pc;
}

/* Translation-time probe: fires before a block is translated, so a translator
 * hang shows up as a 'T' line with no following enter line. */
static void machine_translate_tb(void *env, uint64_t pc)
{
    (void)env;
    n_translate++;
#if LATR_OPEN_SBI_PCLOG_FROM > 0
    if (tb_total >= (uint64_t)LATR_OPEN_SBI_PCLOG_FROM) {
        putc_raw('T');
        puthex8((uint32_t)pc);
        putc_raw('\n');
    }
#endif
}

/* Fires once the block is assembled and about to run. A 'T' line with no 'D'
 * means the translator itself is spinning; 'D' with no '>' means the generated
 * code never comes back. */
static void machine_translated_tb(void *env, uint64_t pc)
{
    (void)env;
#if LATR_OPEN_SBI_PCLOG_FROM > 0
    if (tb_total >= (uint64_t)LATR_OPEN_SBI_PCLOG_FROM) {
        putc_raw('D');
        puthex8((uint32_t)pc);
        putc_raw('\n');
    }
#endif
}

static int contains(const char *hay, const char *needle){
    size_t n = strlen(needle);

    for (; *hay != '\0'; hay++) {
        if (strncmp(hay, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

void kmain(void)
{
    uint64_t firmware_len = (uint64_t)(latr_opensbi_end - latr_opensbi_image);
    uint64_t t_kmain = host_cycles();
    uint64_t t_loop_end = 0;
    int rc;

    puts_raw("latr-opensbi: begin fw=");
    puthex(firmware_len);
    puts_raw("\n");

    install_host_fault_handler();
    enable_host_units();

    /* OpenSBI's BSS is not part of the raw image; make sure the firmware
     * region is zero before loading. */
    memset(ram, 0, 4u << 20);
    if (!latr_machine_init(&machine, ram, sizeof(ram), 1, work, sizeof(work))) {
        puts_raw("latr-opensbi: FAIL init\n");
        return;
    }
    latr_machine_set_enter_tb(&machine, machine_enter_tb);
    latr_machine_set_translate_tb(&machine, machine_translate_tb);
    latr_machine_set_translated_tb(&machine, machine_translated_tb);
    if (!latr_machine_set_bootargs(&machine,
                                   "console=ttyS0 earlycon=sbi keep_bootcon")) {
        puts_raw("latr-opensbi: FAIL bootargs\n");
        return;
    }
    if (!latr_machine_load_firmware(&machine, latr_opensbi_image,
                                    firmware_len)) {
        puts_raw("latr-opensbi: FAIL firmware\n");
        return;
    }
    if (!latr_machine_load_kernel(&machine, latr_smode_payload,
                                  (uint64_t)(latr_smode_payload_end -
                                             latr_smode_payload))) {
        puts_raw("latr-opensbi: FAIL kernel\n");
        return;
    }
    if (!latr_machine_build_dtb(&machine) || !latr_machine_boot(&machine)) {
        puts_raw("latr-opensbi: FAIL boot\n");
        return;
    }

    {
        static const char hexdig[] = "0123456789abcdef";
        const uint8_t *dtb = ram + (machine.dtb_addr - 0x80000000u);

        puts_raw("latr-opensbi: dtb=");
        for (uint32_t i = 0; i < machine.dtb_size; i++) {
            putc_raw(hexdig[dtb[i] >> 4]);
            putc_raw(hexdig[dtb[i] & 15]);
        }
        puts_raw("\n");
    }
#if LATR_OPEN_SBI_PCLOG_FROM > 0
    {
        static const uint64_t dump_pcs[] = {
            0x80002162u, 0x8000210au, 0x80003774u, 0x80003782u, 0x80003786u,
        };

        for (unsigned i = 0; i < sizeof(dump_pcs) / sizeof(dump_pcs[0]); i++) {
            dump_block(&machine, dump_pcs[i]);
        }
    }
#endif
    rc = 2;
    {
        uint32_t budget = LATR_OPEN_SBI_TBS;
        uint32_t stall = 0;
        uint32_t last_uart = 0;
        uint32_t round = 0;

        uint64_t batch_ticks = 0;

        while (budget > 0) {
            uint32_t step = budget < 1000u ? budget : 1000u;
            uint64_t t_batch = host_cycles();
            uint64_t dt;

            rc = latr_machine_run(&machine, step);
            dt = host_cycles() - t_batch;
            batch_ticks = batch_ticks == 0 ? dt : (batch_ticks * 7u + dt) / 8u;
            budget -= step;
            round++;
            /* Self-calibrating livelock guard: a batch that costs orders of
             * magnitude more than the running average means the guest wedged.
             * Break out *without* returning, so the diagnostics (and the PC
             * trace ring) still get dumped. */
            if (batch_ticks != 0 && dt > batch_ticks * 64u && dt > 1000000u) {
                puts_raw("latr-opensbi: slow-batch dt=");
                puthex(dt);
                puts_raw(" avg=");
                puthex(batch_ticks);
                puts_raw(" pc=");
                puthex(machine.last_pc);
                puts_raw("\n");
                break;
            }
            if ((LATR_OPEN_SBI_STOP &&
                 contains(machine.sys.uart_log, "smode ok")) ||
                machine.sys.exit_request != 0 || rc != 2) {
                break;
            }
            /* A traced run must not stop early: the whole point is to see how
             * far the guest really got. The compact run keeps the livelock
             * guard so a broken translation fails in seconds, not minutes. */
#if !LATR_OPEN_SBI_TRACE && LATR_OPEN_SBI_STALL_ROUNDS > 0
            if (machine.sys.uart_log_len == last_uart) {
                if (++stall >= (uint32_t)LATR_OPEN_SBI_STALL_ROUNDS) {
                    puts_raw("latr-opensbi: stall\n");
                    break;
                }
            } else {
                last_uart = machine.sys.uart_log_len;
                stall = 0;
            }
#endif
            if ((round & 7u) == 0u) {
                puts_raw("latr-opensbi: ckpt=");
                puthex(round);
                puts_raw(" pc=");
                puthex(machine.last_pc);
                puts_raw(" uart=");
                puthex(machine.sys.uart_log_len);
                puts_raw("\n");
            }
        }
    }
    t_loop_end = host_cycles();
    /* Did the firmware ever touch the console? OpenSBI programs LCR/FCR and
     * the divisor latch before it prints anything, so a pristine register file
     * means we never reached console init at all. */
    puts_raw("latr-opensbi: uartregs lcr=");
    puthex(machine.sys.uart.lcr);
    puts_raw(" fcr=");
    puthex(machine.sys.uart.fcr);
    puts_raw(" ier=");
    puthex(machine.sys.uart.ier);
    puts_raw(" dll=");
    puthex(machine.sys.uart.dll);
    puts_raw(" dlm=");
    puthex(machine.sys.uart.dlm);
    puts_raw(" fifo=");
    puthex((uint32_t)machine.sys.uart.fifo_enabled);
    puts_raw("\n");
    puts_raw("latr-opensbi: rc=");
    puthex((uint32_t)rc);
    puts_raw(" lastpc=");
    puthex(machine.last_pc);
    puts_raw(" trap=");
    puthex(machine.first_trap_cause);
    puts_raw(" trappc=");
    puthex(machine.first_trap_pc);
    puts_raw(" bad=");
    puthex(machine.first_trap_bad);
    puts_raw(" fetch=");
    puthex(machine.first_trap_fetch);
    puts_raw(" mem=");
    puthex(machine.first_trap_mem);
    puts_raw(" exit=");
    puthex(machine.sys.exit_request);
    puts_raw(" tb=");
    puthex(tb_total);
    puts_raw(" entered=");
    puthex(n_entered);
    puts_raw(" ret=");
    puthex(latr_ret_count);
    puts_raw(" uartspin=");
    puthex(uart_stall_spins);
    puts_raw(" ra=");
    puthex(machine.harts[0].env.gpr[1]);
    puts_raw(" a0=");
    puthex(machine.harts[0].env.gpr[10]);
    puts_raw(" priv=");
    puthex(machine.harts[0].env.privilege);
    puts_raw(" mepc=");
    puthex(machine.harts[0].env.csr[0x341]);
    puts_raw(" mcause=");
    puthex(machine.harts[0].env.csr[0x342]);
    puts_raw(" mtval=");
    puthex(machine.harts[0].env.csr[0x343]);
    {
        const uint8_t *base = ram;
        uint64_t s1 = machine.harts[0].env.gpr[9];
        uint64_t s2 = machine.harts[0].env.gpr[18];
        uint64_t sp = machine.harts[0].env.gpr[2];
        uint64_t ops = 0, op0 = 0, op2 = 0;

        if (s2 >= 0x80000000u && s2 + 0x68 <= 0x80000000u + sizeof(ram)) {
            ops = *(const uint64_t *)(const void *)(base + (s2 - 0x80000000u) + 0x60);
        }
        if (ops >= 0x80000000u && ops < 0x80000000u + sizeof(ram)) {
            op0 = *(const uint64_t *)(const void *)(base + (ops - 0x80000000u));
            op2 = *(const uint64_t *)(const void *)(base + (ops - 0x80000000u) + 0x10);
        }
        puts_raw(" s1=");
        puthex(s1);
        puts_raw(" s2=");
        puthex(s2);
        puts_raw(" ops=");
        puthex(ops);
        puts_raw(" op0=");
        puthex(op0);
        puts_raw(" op2=");
        puthex(op2);
        puts_raw(" sp=");
        puthex(sp);
    }
    {
        uint64_t s1 = machine.harts[0].env.gpr[9];

        puts_raw("\nlatr-opensbi: scratch=");
        if (s1 >= 0x80000000u && s1 + 0x80 <= 0x80000000u + sizeof(ram)) {
            for (int i = 0; i < 16; i++) {
                uint64_t v = *(const uint64_t *)(const void *)
                    (ram + (s1 - 0x80000000u) + (uint64_t)i * 8u);

                puthex(v);
                putc_raw(' ');
            }
        }
    }
    puts_raw("\nlatr-opensbi: mtimecmp=");
    puthex(machine.sys.clint.mtimecmp[0]);
    puts_raw(" mtime=");
    puthex(machine.sys.clint.mtime);
    puts_raw(" mip=");
    puthex(machine.harts[0].env.csr[0x344]);
    puts_raw(" mie=");
    puthex(machine.harts[0].env.csr[0x304]);
    puts_raw(" mscratch=");
    puthex(machine.harts[0].env.csr[0x340]);
    puts_raw(" mtvec=");
    puthex(machine.harts[0].env.csr[0x305]);
    puts_raw("\nlatr-opensbi: tbs=");
    puthex(machine.harts[0].tbs);
    puts_raw(" entries=");
    puthex(machine.harts[0].tb_cache.count);
    puts_raw(" dispatch=");
    puthex(machine.dispatch_count);
    puts_raw("\nlatr-opensbi: tagra=");
    for (uint32_t i = 0; i < tag_ra_n; i++) {
        puthex(tag_ra[i]);
        putc_raw(':');
        puthex(tag_ra_count[i]);
        putc_raw(' ');
    }
    puts_raw("\nlatr-opensbi: top=");
    for (int rank = 0; rank < 8; rank++) {
        uint32_t best = 0, besti = 0;

        for (uint32_t i = 0; i < 4096u; i++) {
            if (hist[i] > best) {
                best = hist[i];
                besti = i;
            }
        }
        if (best == 0) {
            break;
        }
        puthex(0x80000000u + (besti << 8));
        putc_raw(':');
        puthex(best);
        putc_raw(' ');
        hist[besti] = 0;
    }
    puts_raw("\nlatr-opensbi: slow=");
    puthex(tslow);
    puts_raw(" tmax=");
    puthex(tmax);
    puts_raw(" tmaxpc=");
    puthex(tmax_pc);
    puts_raw("\nlatr-opensbi: toptime=");
    for (int rank = 0; rank < 8; rank++) {
        uint32_t best = 0, besti = 0;

        for (uint32_t i = 0; i < 4096u; i++) {
            uint32_t mb = (uint32_t)(tsum[i] >> 20);

            if (mb > best) {
                best = mb;
                besti = i;
            }
        }
        if (best == 0) {
            break;
        }
        puthex(0x80000000u + (besti << 8));
        putc_raw(':');
        puthex(best);
        putc_raw('m');
        putc_raw(' ');
        tsum[besti] = 0;
    }
    puts_raw("\nlatr-opensbi: list=calls:");
    puthex(list_calls);
    puts_raw(" iters:");
    puthex(list_iters);
    puts_raw(" a0:");
    puthex(list_a0);
    puts_raw(" a1:");
    puthex(list_a1);
    puts_raw(" a2:");
    puthex(list_a2);
    puts_raw(" ra:");
    puthex(list_ra);
    puts_raw(" off:");
    puthex(list_a0 >= machine.dtb_addr ? list_a0 - machine.dtb_addr : 0);
    puts_raw("\nlatr-opensbi: listmem=");
    if (list_a0 >= 0x80000000u && list_a0 + 96u <= 0x80000000u + sizeof(ram)) {
        const uint8_t *q = ram + (list_a0 - 0x80000000u);

        for (int i = 0; i < 96; i++) {
            puthex(q[i]);
            putc_raw(' ');
        }
    }
    puts_raw("\nlatr-opensbi: scan=");
    puthex(scan_tb);
    putc_raw('@');
    puthex(scan_s1);
    putc_raw(':');
    puthex(scan_a1);
    putc_raw(':');
    puthex(scan_a2);
    puts_raw("\nlatr-opensbi: paths=");
    for (uint32_t i = 0; i < path_count; i++) {
        puthex(path_tb[i]);
        putc_raw('@');
        puthex(path_arg[i]);
        putc_raw(' ');
    }
    puts_raw("\nlatr-opensbi: trace=");
    {
#if LATR_OPEN_SBI_TRACE
        uint32_t count = trace_pos < TRACE_CAP ? trace_pos : TRACE_CAP;
        uint32_t start = trace_pos < TRACE_CAP ? 0u : trace_pos - TRACE_CAP;
#else
        uint32_t count = trace_pos > TRACE_CAP ? TRACE_CAP : trace_pos;
        uint32_t start = trace_pos - count;
#endif

        for (uint32_t i = start; i < trace_pos; i++) {
            puthex(trace_pc[i & (TRACE_CAP - 1u)]);
            putc_raw((i == trace_pos - 1u) ? ';' : ' ');
        }
        puts_raw("trace_n=");
        puthex(count);
        puts_raw(" trace_pos=");
        puthex(trace_pos);
    }
    puts_raw("\n uart=");
    /* Dump the whole capture, not a fixed prefix: the interesting byte is the
     * last one, and truncating to 200 characters once made a fully successful
     * run look like it stopped in the middle of the OpenSBI logo. */
    for (uint32_t i = 0; i < machine.sys.uart_log_len; i++) {
        char c = machine.sys.uart_log[i];

        putc_raw((c >= 0x20 && c < 0x7f) || c == '\n' || c == '\r' ? c : '.');
    }
    puts_raw("\n uart_len=");
    puthex(machine.sys.uart_log_len);
    puts_raw("\n");

    puts_raw("\nlatr-opensbi: wall_loop=");
    puthex(t_loop_end - t_kmain);
    puts_raw(" wall_dump=");
    puthex(host_cycles() - t_loop_end);
    puts_raw("\n");

    if (contains(machine.sys.uart_log, "OpenSBI")) {
        puts_raw("latr-opensbi: PASS opensbi=1");
        /* Both markers come from the S-mode payload, and both are printed with
         * SBI ecall, so seeing them means the handoff happened *and* the
         * S->M->S trap round trip works. Checking for a bare "K" (as an earlier
         * version did) matched the letter inside the OpenSBI banner and reported
         * kernel=1 for a machine whose next stage had never run at all. */
        if (contains(machine.sys.uart_log, "smode up hart=")
            && contains(machine.sys.uart_log, "smode ok")) {
            puts_raw(" smode=1");
        }
        puts_raw("\n");
        puts_raw("latr-opensbi: done\n");
    } else {
        puts_raw("latr-opensbi: FAIL no-banner\n");
        /* The watchdog in the run script keys on this token; without it a
         * failed run sits in QEMU until the timeout, which reads as "slow"
         * even though the machine finished in under a second. */
        puts_raw("latr-opensbi: done\n");
    }
}
