/*
 * Bare-metal driver for the LATR SMP guest scenario.
 *
 * The machine under test is the real one (latr_machine): two harts, CLINT,
 * PLIC, DTB, and generated LoongArch code executed through latr_enter_tb. The
 * guest is baremetal/bare/latr_smp_guest.S, a two-hart RISC-V M-mode program
 * built into a flat image and embedded as a blob. Running it on a LoongArch
 * host (QEMU or silicon) is the only place guest-visible SMP semantics are
 * executed for real; the host harness tests the scheduler around the same
 * machine but cannot jump into generated code.
 *
 * What the guest proves, and the checks below mirror it:
 *   - AMO write-back and LR/SC forward progress in generated code;
 *   - a software interrupt delivered through the CLINT to a hart parked in
 *     WFI, with mret returning to the interrupted loop;
 *   - FENCE.I coherence across harts: hart 1 translated the old code first,
 *     hart 0 rewrote it and fenced, and hart 1's post-IPI call must see the
 *     new instruction bytes.
 */
#include <stdint.h>
#include "latr.h"
#include "latr-machine.h"
#include "loongemu-perf.h"

extern const uint8_t latr_smp_idle[];
extern const uint8_t latr_smp_idle_end[];

extern void latr_enter_tb(void *env, void *code);
extern uint8_t latr_smp_guest[];
extern uint8_t latr_smp_guest_end[];

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

#define UART0_THR ((volatile uint8_t *)0x1fe001e0)
static void uart_putc(int c)
{
    *UART0_THR = (uint8_t)c;
}
static void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}
static void uart_dec(uint64_t v)
{
    char b[21];
    int i = 20;

    b[i] = 0;
    do {
        b[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0 && i > 0);
    uart_puts(&b[i]);
}
static void uart_hex(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";

    for (int shift = 60; shift >= 0; shift -= 4) {
        uart_putc(digits[(v >> shift) & 15u]);
    }
}

void kmain(void);
__asm__(".text\n.globl _start\n_start:\n  pcalau12i $sp, %pc_hi20(latr_smp_stack)\n  addi.d $sp, $sp, %pc_lo12(latr_smp_stack)\n"
        "  lu12i.w $t0, 128\n  add.d $sp, $sp, $t0\n  bl kmain\n1: b 1b\n");
/* 1 MiB: the machine + translator path runs several KiB deep per frame, and a
 * 256 KiB stack silently corrupted a callee-saved pointer mid-translation
 * (an address error inside run_hart, not a diagnostics message). */
static uint8_t latr_smp_stack[1u << 20] __attribute__((aligned(16), used));

static uint8_t ram[16u << 20] __attribute__((aligned(4096)));
static uint8_t machine_work[(2u << 20) * 4u] __attribute__((aligned(4096)));
static LatrMachine machine;

#define DATA_PAGE 0x80001000u
#define D_COUNTER (DATA_PAGE + 0x08u)
#define D_LRSC (DATA_PAGE + 0x10u)
#define D_EARLY (DATA_PAGE + 0x18u)
#define D_IPI (DATA_PAGE + 0x20u)
#define D_H0 (DATA_PAGE + 0x28u)
#define D_H1 (DATA_PAGE + 0x30u)
#define D_EXIT (DATA_PAGE + 0x38u)
#define D_XPAGE_BEFORE (DATA_PAGE + 0x40u)
#define D_XPAGE_AFTER (DATA_PAGE + 0x48u)

static uint64_t read64(uint64_t pa)
{
    uint64_t *p = (uint64_t *)(void *)(ram + (pa - LATR_SYSTEM_RAM_BASE));

    return *p;
}

static int checks;
static int failures;

static void check_u64(const char *what, uint64_t got, uint64_t want)
{
    checks++;
    if (got != want) {
        failures++;
        uart_puts("latr-smp: FAIL ");
        uart_puts(what);
        uart_puts(" got=");
        uart_dec(got);
        uart_puts(" want=");
        uart_dec(want);
        uart_puts("\n");
    }
}

static void check_true(const char *what, int cond)
{
    checks++;
    if (!cond) {
        failures++;
        uart_puts("latr-smp: FAIL ");
        uart_puts(what);
        uart_puts("\n");
    }
}

/*
 * Scripted interleaving (design 9.2/9.3): the driver, not the guest, decides
 * which hart runs and when. Each scenario puts hart 1 in a known place, applies
 * one machine event from the driver's thread, and asserts the invariant the
 * threaded host test asserts - so the protocol is covered in the CI image too,
 * where pthreads do not exist.
 *
 *   1. a shootdown applied while hart 1 is between TBs invalidates its cache;
 *   2. the hart must dispatch again after a wake (re-translation, not a stale
 *      entry) - a hart parked in WFI cannot sample anything by itself;
 *   3. an exit request is honoured after the block boundary it was raised at,
 *      not in the middle of one, and does not leak into the next run.
 */
static void scripted_scenarios(void)
{
    static LatrMachine m;
    static uint8_t work[1u << 20];
    uint64_t generation;
    uint32_t entries;
    uint64_t tbs;
    uint64_t wake;
    int rc;

    uart_puts("latr-smp-script: begin\n");
    if (!latr_machine_init(&m, ram, sizeof(ram), 2, work, sizeof(work))) {
        uart_puts("latr-smp: FAIL scripted machine init\n");
        check_true("scripted machine init", 0);
        return;
    }
    latr_machine_set_enter_tb(&m, latr_enter_tb);
    if (!latr_machine_load_firmware(&m, latr_smp_idle,
                                    (uint64_t)(latr_smp_idle_end -
                                               latr_smp_idle)) ||
        !latr_machine_build_dtb(&m) || !latr_machine_boot(&m)) {
        uart_puts("latr-smp: FAIL scripted boot\n");
        check_true("scripted firmware boots", 0);
        return;
    }

    /* Hart 1's first TB is wfi. The machine fast-forwards the idle advance
     * instead of parking (a parked hart is the embedder's job, and the host
     * harness covers it), so what this image can pin is that wfi neither
     * wedges the hart nor runs hart 0 behind the driver's back. */
    rc = latr_machine_run_hart(&m, 1, 1);
    if (rc == 0) {
        uart_puts("latr-smp-script: hart 1 failed trapped=");
        uart_dec((uint64_t)m.trapped);
        uart_puts(" pc=");
        uart_hex(m.harts[1].env.pc);
        uart_puts("\n");
    }
    check_u64("hart 1 dispatches its wfi block", (uint64_t)rc, 1);
    check_u64("hart 0 stayed out of the scripted run", m.harts[0].tbs, 0);
    entries = m.harts[1].tb_cache.count;
    check_true("hart 1 has a translated block before the fence", entries >= 1);

    /* Scenario 1: a shootdown applied while hart 1 is between TBs invalidates
     * its cache, and the hart must get back to dispatching. The one-shot yield
     * that a shootdown raises is deliberately not pinned to a call index: it is
     * consumed by whichever dispatch comes next, so the invariant asserted here
     * is progress plus re-translation, not a counter value. */
    generation = m.harts[1].tb_cache.generation;
    (void)latr_tb_fence_begin(0, 1);
    check_true("fence.i bumps hart 1's cache generation",
               m.harts[1].tb_cache.generation > generation);
    check_u64("fence.i leaves no valid entry behind",
              m.harts[1].tb_cache.count, 0);
    {
        int dispatched = 0;

        for (int i = 0; i < 4; i++) {
            if (latr_machine_run_hart(&m, 1, 1) == 1) {
                dispatched++;
            }
        }
        check_true("the hart dispatches again after the shootdown",
                   dispatched >= 1);
        check_true("the block was re-translated after the fence",
                   m.harts[1].tb_cache.count >= 1);
    }

    /* Scenario 2: a wake is counted, which is what lets an embedder parked in
     * WFI (the host harness case) come back. */
    wake = latr_machine_wake_events(&m);
    latr_machine_wake_hart(&m, 1);
    check_true("the wake is counted", latr_machine_wake_events(&m) == wake + 1);

    /* Scenario 3: an exit request raised between TBs runs at most one more
     * block, clears itself, and does not wedge the hart. */
    latr_hart_request_exit(&m.harts[1].park);
    tbs = m.harts[1].tbs;
    (void)latr_machine_run_hart(&m, 1, 8);
    check_true("at most one block runs after the exit request",
               m.harts[1].tbs - tbs <= 1);
    check_u64("the exit request is cleared", m.harts[1].park.exit_request, 0);
    check_u64("the hart keeps running after the exit request",
              (uint64_t)latr_machine_run_hart(&m, 1, 1), 1);
    check_u64("hart 0 never entered the scripted run", m.harts[0].tbs, 0);
}

void kmain(void)
{
    int rc;

    uart_puts("latr-smp: begin\n");
    if (!latr_machine_init(&machine, ram, sizeof(ram), 2, machine_work,
                           sizeof(machine_work))) {
        uart_puts("latr-smp: FAIL machine init\nlatr-smp: done\n");
        return;
    }
    uart_puts("latr-smp: init ok\n");
    latr_machine_set_enter_tb(&machine, latr_enter_tb);
    if (!latr_machine_load_firmware(
            &machine, latr_smp_guest,
            (uint64_t)(latr_smp_guest_end - latr_smp_guest))) {
        uart_puts("latr-smp: FAIL firmware load\nlatr-smp: done\n");
        return;
    }
    uart_puts("latr-smp: firmware loaded\n");
    if (!latr_machine_build_dtb(&machine) || !latr_machine_boot(&machine)) {
        uart_puts("latr-smp: FAIL boot\nlatr-smp: done\n");
        return;
    }

    uart_puts("latr-smp: booted\n");
    rc = latr_machine_run(&machine, 5000);
    uart_puts("latr-smp: run rc=");
    uart_dec((uint64_t)rc);
    uart_puts(" trapped=");
    uart_dec((uint64_t)machine.trapped);
    uart_puts(" pc0=");
    uart_hex(machine.harts[0].env.pc);
    uart_puts(" pc1=");
    uart_hex(machine.harts[1].env.pc);
    uart_puts(" tbs0=");
    uart_dec(machine.harts[0].tbs);
    uart_puts(" tbs1=");
    uart_dec(machine.harts[1].tbs);
    uart_puts(" wake=");
    uart_dec(latr_machine_wake_events(&machine));
    uart_puts("\n");

    check_true("guest powered off through the exit device",
               rc == 1 && machine.sys.exit_request == 0x5555);
    check_true("both harts dispatched blocks",
               machine.harts[0].tbs > 0 && machine.harts[1].tbs > 0);
    check_u64("AMO counter is 32", read64(D_COUNTER), 32);
    check_u64("LR/SC incremented 16 times", read64(D_LRSC), 16);
    check_u64("hart 1 called the old target", read64(D_EARLY), 1);
    check_u64("one software interrupt delivered", read64(D_IPI), 1);
    check_u64("hart 0 sees the rewritten target", read64(D_H0), 2);
    check_u64("hart 1 sees the rewritten target", read64(D_H1), 2);
    check_u64("exit marker carries hart 1's result", read64(D_EXIT), 2);
    check_u64("page-straddling target ran before the rewrite",
              read64(D_XPAGE_BEFORE), 1);
    check_u64("page-straddling target ran after the rewrite",
              read64(D_XPAGE_AFTER), 3);
    /* The straddling instruction's bytes, as the guest left them: the rewrite
     * is two halfword stores, so this also pins that a page boundary does not
     * split a store path. */
    {
        const uint8_t *p = ram + 0xFFEu;

        check_u64("straddling word rewritten in place",
                  (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
                      ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24),
                  0x00300513u);
        check_u64("instruction after the boundary rewritten in place",
                  (uint64_t)p[4] | ((uint64_t)p[5] << 8),
                  0x8067u);
    }

    /* Chain-control baseline (design: chain-control-ras.md P0): the epoch moves
     * at each real break point and `links` must stay 0 while nothing chains. */
    for (uint32_t i = 0; i < 2; i++) {
        const LatrChainControl *c = latr_machine_chain(&machine, i);

        uart_puts("latr-chain: h");
        uart_dec(i);
        uart_puts(" epoch=");
        uart_dec(latr_chain_epoch(c));
        uart_puts(" gen=");
        uart_dec(latr_chain_tb_generation(c));
        uart_puts(" links=");
        uart_dec(latr_chain_links(c));
        uart_puts(" cand-hits=");
        uart_dec(latr_chain_candidate_hits(c));
        for (unsigned r = 0; r < LATR_CHAIN_BREAK_COUNT; r++) {
            uart_puts(" ");
            uart_puts(latr_chain_break_name((LatrChainBreak)r));
            uart_puts("=");
            uart_dec(latr_chain_breaks(c, (LatrChainBreak)r));
        }
        uart_puts("\n");
        /* Phase A: a chained dispatch is one that reused a chain candidate
         * instead of hashing the key and scanning the set. */
        check_true("chained dispatches happened", latr_chain_links(c) > 0);
        check_u64("every link came through a candidate",
                  latr_chain_links(c), latr_chain_candidate_hits(c));
        check_u64("chain epoch equals the sum of its breaks",
                  latr_chain_epoch(c), latr_chain_total_breaks(c));
    }

#ifdef LOONGEMU_PERF
    /* Dispatch cost vs execution cost, per block: this is the number that says
     * whether chaining (5.2 Phase A/B) can pay for itself at all. */
    {
        char hexbuf[19];
        unsigned long long vals[2] = {
            (unsigned long long)loongemu_perf_read(
                LOONGEMU_PERF_TB_COMPILE_TICKS),
            (unsigned long long)loongemu_perf_read(LOONGEMU_PERF_TB_EXEC_TICKS),
        };

        uart_puts("latr-smp-perf: compile=");
        for (int k = 15; k >= 0; k--) {
            unsigned d = (unsigned)((vals[0] >> (k * 4)) & 15u);

            uart_putc("0123456789abcdef"[d]);
        }
        uart_puts(" exec=");
        for (int k = 15; k >= 0; k--) {
            unsigned d = (unsigned)((vals[1] >> (k * 4)) & 15u);

            uart_putc("0123456789abcdef"[d]);
        }
        uart_puts("\n");
        (void)hexbuf;
    }
#endif
    {
        /* Translation-time invariant: every borrowed temporary is returned
         * before the next guest instruction. A leak here would let a later
         * translation reuse a register that still holds a guest value. */
        LatrRegCacheStats rc = latr_regcache_stats();

        uart_puts("latr-smp-borrow: leaks=");
        uart_dec(rc.borrow_leaks);
        uart_puts(" max=");
        uart_dec(rc.borrow_outstanding_max);
        uart_puts(" first-opcode=");
        uart_dec(rc.borrow_leak_opcode);
        uart_puts("\n");
        check_u64("no translator borrow leaks in this run",
                  rc.borrow_leaks, 0);
        check_u64("no outstanding borrows after any instruction",
                  rc.borrow_outstanding_max, 0);
    }
    scripted_scenarios();
    uart_puts(failures == 0 ? "latr-smp: PASS checks=" : "latr-smp: FAIL checks=");
    uart_dec((uint64_t)checks);
    uart_puts(" fails=");
    uart_dec((uint64_t)failures);
    uart_puts("\nlatr-smp: done\n");
}
